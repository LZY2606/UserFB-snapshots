/*
 * Forward-mode automatic differentiation for SPIRV-VM.
 *
 * The primal (numeric) computation is performed by the ordinary opcode
 * executor.  This module observes the same instruction stream and maintains a
 * side table of analytic directional derivatives; it never perturbs inputs and
 * never re-runs the shader.
 *
 * Key design points:
 *
 *  - Tangents are keyed by the identity of the storage leaf (the address of
 *    the spvm_member inside a result's member tree), not by result id.  An
 *    OpAccessChain rebinds a result's members to point into another result's
 *    storage, so two chains that reach the same member share the same tangent
 *    slots; aliasing therefore falls out naturally.
 *
 *  - Only the current member trees are ever consulted, so a value copied out
 *    of storage is an independent derivative object afterwards: later writes
 *    through the original pointer do not affect it.
 *
 *  - Function parameters are bound by the executor to the argument's member
 *    addresses.  SPIR-V values are immutable SSA values, so this matches
 *    pass-by-value for values and is exactly right for pointer parameters.
 *
 *  - Function-local storage is allocated once by the VM setup pass and starts
 *    with zero tangents, so a tangent seeded on a global never leaks into a
 *    fresh function local.
 *
 *  - Every instruction that writes a value overwrites the destination
 *    tangents (including writing explicit zeros), so loop iterations do not
 *    accumulate stale derivatives.
 */

#include <spvm/forward.h>
#include <spvm/value.h>
#include <spvm/spirv.h>
#include <spvm/state.h>
#include <spvm/result.h>

#include "forward_observe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* GLSL.std.450 instruction numbers we differentiate. */
#define SPVM_GLSL450_FAbs 4
#define SPVM_GLSL450_Sin 13
#define SPVM_GLSL450_Cos 14
#define SPVM_GLSL450_Sqrt 31
#define SPVM_GLSL450_Length 66
#define SPVM_GLSL450_Normalize 69

/* ------------------------------------------------------------------ */
/* Context storage                                                     */
/* ------------------------------------------------------------------ */

typedef struct spvm_fwd_entry {
	spvm_member_t leaf;
	float tangent[SPVM_FORWARD_MAX_DIRECTIONS];
	struct spvm_fwd_entry* next;
} spvm_fwd_entry;

struct spvm_forward {
	spvm_state_t state;
	spvm_word directions;
	spvm_byte active;

	int error;
	spvm_word error_instruction;
	spvm_word error_line;

	spvm_fwd_entry* entries;
	spvm_word entry_count;
};

/* One binding per state, in a module-private registry (not state->user_data,
 * not state->analyzer, not the host extension table). */
typedef struct spvm_fwd_binding {
	spvm_state_t state;
	spvm_forward_t fwd;
	struct spvm_fwd_binding* next;
} spvm_fwd_binding;

static spvm_fwd_binding* spvm_fwd_bindings = NULL;

spvm_forward_t spvm_forward_get_context(spvm_state_t state)
{
	for (spvm_fwd_binding* b = spvm_fwd_bindings; b; b = b->next)
		if (b->state == state)
			return b->fwd;
	return NULL;
}

static int spvm_fwd_bind(spvm_state_t state, spvm_forward_t fwd)
{
	if (spvm_forward_get_context(state))
		return 0;
	spvm_fwd_binding* b = (spvm_fwd_binding*)malloc(sizeof(spvm_fwd_binding));
	if (!b)
		return 0;
	b->state = state;
	b->fwd = fwd;
	b->next = spvm_fwd_bindings;
	spvm_fwd_bindings = b;
	return 1;
}

static void spvm_fwd_unbind(spvm_state_t state, spvm_forward_t fwd)
{
	spvm_fwd_binding** link = &spvm_fwd_bindings;
	while (*link) {
		if ((*link)->state == state && (*link)->fwd == fwd) {
			spvm_fwd_binding* dead = *link;
			*link = dead->next;
			free(dead);
			return;
		}
		link = &(*link)->next;
	}
}

/* ------------------------------------------------------------------ */
/* Error handling                                                      */
/* ------------------------------------------------------------------ */

static void spvm_fwd_error(spvm_forward_t fwd, spvm_state_t state)
{
	if (!fwd || fwd->error)
		return;
	fwd->error = 1;
	fwd->error_instruction = state ? state->instruction_count : 0;
	fwd->error_line = state ? state->current_line : -1;
}

/* ------------------------------------------------------------------ */
/* Tangent entry map (keyed by leaf address)                           */
/* ------------------------------------------------------------------ */

static spvm_fwd_entry* spvm_fwd_find(spvm_forward_t fwd, spvm_member_t leaf, int create)
{
	for (spvm_fwd_entry* e = fwd->entries; e; e = e->next)
		if (e->leaf == leaf)
			return e;
	if (!create)
		return NULL;

	spvm_fwd_entry* e = (spvm_fwd_entry*)calloc(1, sizeof(spvm_fwd_entry));
	if (!e) {
		spvm_fwd_error(fwd, fwd->state);
		return NULL;
	}
	e->leaf = leaf;
	e->next = fwd->entries;
	fwd->entries = e;
	fwd->entry_count++;
	return e;
}

static float spvm_fwd_read(spvm_forward_t fwd, spvm_member_t leaf, spvm_word dir)
{
	spvm_fwd_entry* e = spvm_fwd_find(fwd, leaf, 0);
	return e ? e->tangent[dir] : 0.0f;
}

/* Overwrite the tangent of one leaf in one direction (explicit zero included). */
static void spvm_fwd_write(spvm_forward_t fwd, spvm_member_t leaf, spvm_word dir, float value)
{
	spvm_fwd_entry* e = spvm_fwd_find(fwd, leaf, 1);
	if (e)
		e->tangent[dir] = value;
}

/* Read all directions of a leaf. */
static void spvm_fwd_read_all(spvm_forward_t fwd, spvm_member_t leaf, float* out)
{
	spvm_fwd_entry* e = spvm_fwd_find(fwd, leaf, 0);
	for (spvm_word k = 0; k < fwd->directions; k++)
		out[k] = e ? e->tangent[k] : 0.0f;
}

static void spvm_fwd_write_all(spvm_forward_t fwd, spvm_member_t leaf, const float* in)
{
	spvm_fwd_entry* e = spvm_fwd_find(fwd, leaf, 1);
	if (!e)
		return;
	for (spvm_word k = 0; k < fwd->directions; k++)
		e->tangent[k] = in[k];
}

static void spvm_fwd_entries_free(spvm_forward_t fwd)
{
	spvm_fwd_entry* e = fwd->entries;
	while (e) {
		spvm_fwd_entry* next = e->next;
		free(e);
		e = next;
	}
	fwd->entries = NULL;
	fwd->entry_count = 0;
}

/* ------------------------------------------------------------------ */
/* Type / leaf resolution                                              */
/* ------------------------------------------------------------------ */

static spvm_result_t spvm_fwd_type_info(spvm_state_t state, spvm_word type)
{
	if (type <= 0 || type >= state->owner->bound)
		return NULL;
	return spvm_state_get_type_info(state->results, &state->results[type]);
}

static int spvm_fwd_is_float32(spvm_result_t type_info)
{
	return type_info && type_info->value_type == spvm_value_type_float &&
		type_info->value_bitcount == 32;
}

static int spvm_fwd_type_has_float(spvm_state_t state, spvm_word type)
{
	spvm_result_t ti = spvm_fwd_type_info(state, type);
	if (!ti)
		return 0;
	switch (ti->value_type) {
	case spvm_value_type_float:
		return ti->value_bitcount == 32;
	case spvm_value_type_vector:
	case spvm_value_type_matrix:
	case spvm_value_type_array:
	case spvm_value_type_runtime_array:
		return spvm_fwd_type_has_float(state, ti->pointer);
	case spvm_value_type_struct:
		for (spvm_word i = 0; i < ti->member_count; i++)
			if (spvm_fwd_type_has_float(state, ti->params[i]))
				return 1;
		return 0;
	default:
		return 0;
	}
}

/* Resolve id + member-index path to a float32 scalar leaf.
 *
 * The value at `id` is described by its result type (res->pointer) and its
 * member tree.  A path of length N walks N members.  Because the VM's
 * spvm_member tree stores composite members as members-with-children, we track
 * both the "container" (whose .members array we index) and the type of the
 * container. */
static spvm_member_t spvm_fwd_resolve(spvm_state_t state, spvm_word id,
	const spvm_word* path, spvm_word path_len)
{
	if (id <= 0 || id >= state->owner->bound)
		return NULL;

	spvm_result_t res = &state->results[id];
	if (!res->members || res->member_count <= 0 || res->pointer <= 0)
		return NULL;

	spvm_word cur_type = res->pointer;

	if (path_len == 0) {
		/* Scalar value: exactly one leaf member. */
		spvm_result_t ti = spvm_fwd_type_info(state, cur_type);
		if (!spvm_fwd_is_float32(ti) || res->member_count != 1 ||
			res->members[0].member_count != 0)
			return NULL;
		return &res->members[0];
	}

	spvm_member_t container = res->members;

	/* The first index selects from res->members directly; subsequent indices
	 * select from the chosen composite's own .members. */
	spvm_word container_count = res->member_count;

	for (spvm_word i = 0; i < path_len; i++) {
		spvm_result_t ti = spvm_fwd_type_info(state, cur_type);
		spvm_word idx = path[i];
		if (!ti || idx < 0 || idx >= container_count)
			return NULL;

		switch (ti->value_type) {
		case spvm_value_type_vector:
		case spvm_value_type_matrix:
		case spvm_value_type_array:
			cur_type = ti->pointer;
			break;
		case spvm_value_type_struct:
			if (idx >= ti->member_count)
				return NULL;
			cur_type = ti->params[idx];
			break;
		default:
			return NULL;
		}

		spvm_member_t chosen = &container[idx];

		if (i + 1 == path_len) {
			spvm_result_t lt = spvm_fwd_type_info(state, cur_type);
			if (!spvm_fwd_is_float32(lt) || chosen->member_count != 0)
				return NULL;
			return chosen;
		}

		/* Descend: the next index indexes the chosen composite's children. */
		if (chosen->member_count == 0)
			return NULL;
		container = chosen->members;
		container_count = chosen->member_count;
	}

	return NULL;
}

/* ------------------------------------------------------------------ */
/* Tangent tree operations (positional, over current member addresses) */
/* ------------------------------------------------------------------ */
/*
 * A result's value is stored as an *array* of `member_count` spvm_member
 * nodes (res->members).  Each node is either a leaf (member_count == 0) or a
 * composite whose children are again an array.  The helpers below therefore
 * always carry an explicit (array, count) pair.
 */

static void spvm_fwd_zero_array(spvm_forward_t fwd, spvm_member_t arr, spvm_word count)
{
	for (spvm_word i = 0; i < count; i++) {
		spvm_member_t m = &arr[i];
		if (m->member_count == 0) {
			spvm_fwd_entry* e = spvm_fwd_find(fwd, m, 0);
			if (e)
				for (spvm_word k = 0; k < fwd->directions; k++)
					e->tangent[k] = 0.0f;
		} else {
			spvm_fwd_zero_array(fwd, m->members, m->member_count);
		}
	}
}

/* dst = src positionally.  Destination leaves without a source become zero. */
static void spvm_fwd_copy_array(spvm_forward_t fwd,
	spvm_member_t dst, spvm_word dst_count, spvm_member_t src, spvm_word src_count)
{
	spvm_word n = dst_count < src_count ? dst_count : src_count;
	for (spvm_word i = 0; i < n; i++) {
		spvm_member_t d = &dst[i];
		spvm_member_t s = &src[i];
		if (d->member_count == 0 && s->member_count == 0) {
			spvm_fwd_entry* se = spvm_fwd_find(fwd, s, 0);
			if (se) {
				spvm_fwd_entry* de = spvm_fwd_find(fwd, d, 1);
				if (de)
					for (spvm_word k = 0; k < fwd->directions; k++)
						de->tangent[k] = se->tangent[k];
			} else {
				spvm_fwd_entry* de = spvm_fwd_find(fwd, d, 0);
				if (de)
					for (spvm_word k = 0; k < fwd->directions; k++)
						de->tangent[k] = 0.0f;
			}
		} else if (d->member_count != 0 && s->member_count != 0) {
			spvm_fwd_copy_array(fwd, d->members, d->member_count, s->members, s->member_count);
		}
	}
	for (spvm_word i = n; i < dst_count; i++)
		spvm_fwd_zero_array(fwd, &dst[i], 1);
}

/* dst += scale * src positionally. */
static void spvm_fwd_axpy_array(spvm_forward_t fwd,
	spvm_member_t dst, spvm_word dst_count, spvm_member_t src, spvm_word src_count, float scale)
{
	spvm_word n = dst_count < src_count ? dst_count : src_count;
	for (spvm_word i = 0; i < n; i++) {
		spvm_member_t d = &dst[i];
		spvm_member_t s = &src[i];
		if (d->member_count == 0 && s->member_count == 0) {
			spvm_fwd_entry* se = spvm_fwd_find(fwd, s, 0);
			if (!se)
				continue;
			spvm_fwd_entry* de = spvm_fwd_find(fwd, d, 1);
			if (!de)
				continue;
			for (spvm_word k = 0; k < fwd->directions; k++)
				de->tangent[k] += scale * se->tangent[k];
		} else if (d->member_count != 0 && s->member_count != 0) {
			spvm_fwd_axpy_array(fwd, d->members, d->member_count, s->members, s->member_count, scale);
		}
	}
}

/* Single-node convenience wrapper (a node is a 1-element tree). */
static void spvm_fwd_copy_node(spvm_forward_t fwd, spvm_member_t dst, spvm_member_t src)
{
	spvm_fwd_copy_array(fwd, dst, 1, src, 1);
}

/* Result-slot convenience: copy/zero the whole member array of a result. */
static void spvm_fwd_copy_result(spvm_forward_t fwd, spvm_result_t dst, spvm_result_t src)
{
	spvm_fwd_copy_array(fwd, dst->members, dst->member_count, src->members, src->member_count);
}
static void spvm_fwd_zero_result(spvm_forward_t fwd, spvm_result_t res)
{
	spvm_fwd_zero_array(fwd, res->members, res->member_count);
}

/* ------------------------------------------------------------------ */
/* Pre-execution: unsupported floating-point computation              */
/* ------------------------------------------------------------------ */

/* Returns the result type id for instructions that have one, else 0. */
static int spvm_fwd_has_result_type(SpvOp opcode)
{
	switch (opcode) {
	case SpvOpLoad:
	case SpvOpAccessChain:
	case SpvOpFunctionCall:
	case SpvOpVectorExtractDynamic:
	case SpvOpVectorInsertDynamic:
	case SpvOpVectorShuffle:
	case SpvOpCompositeConstruct:
	case SpvOpCompositeExtract:
	case SpvOpCompositeInsert:
	case SpvOpCopyObject:
	case SpvOpFNegate:
	case SpvOpFAdd:
	case SpvOpFSub:
	case SpvOpFMul:
	case SpvOpFDiv:
	case SpvOpFMod:
	case SpvOpFRem:
	case SpvOpVectorTimesScalar:
	case SpvOpMatrixTimesScalar:
	case SpvOpVectorTimesMatrix:
	case SpvOpMatrixTimesVector:
	case SpvOpMatrixTimesMatrix:
	case SpvOpOuterProduct:
	case SpvOpTranspose:
	case SpvOpDot:
	case SpvOpFConvert:
	case SpvOpQuantizeToF16:
	case SpvOpExtInst:
	case SpvOpImageSampleImplicitLod:
	case SpvOpImageSampleExplicitLod:
	case SpvOpImageFetch:
	case SpvOpImageGather:
	case SpvOpImageDrefGather:
	case SpvOpImageRead:
	case SpvOpImage:
	case SpvOpImageQueryLod:
		return 1;
	default:
		return 0;
	}
}

/* True if the instruction's result type is a float-carrying type.  Writes and
 * other result-less instructions that appear in the reject list are float
 * operations by definition, so they are reported as float-producing. */
static int spvm_fwd_float_producing(spvm_state_t state, SpvOp opcode, spvm_source operands)
{
	if (!spvm_fwd_has_result_type(opcode))
		return 1;
	return spvm_fwd_type_has_float(state, operands[0]);
}

static int spvm_fwd_unsupported(spvm_state_t state, SpvOp opcode, spvm_source operands)
{
	switch (opcode) {
	case SpvOpFConvert:
	case SpvOpQuantizeToF16:
	case SpvOpFMod:
	case SpvOpFRem:
	case SpvOpMatrixTimesScalar:
	case SpvOpVectorTimesMatrix:
	case SpvOpMatrixTimesVector:
	case SpvOpMatrixTimesMatrix:
	case SpvOpOuterProduct:
	case SpvOpTranspose:
	case SpvOpDPdx:
	case SpvOpDPdy:
	case SpvOpDPdxFine:
	case SpvOpDPdyFine:
	case SpvOpDPdxCoarse:
	case SpvOpDPdyCoarse:
	case SpvOpFwidth:
	case SpvOpFwidthFine:
	case SpvOpFwidthCoarse:
	case SpvOpImageSampleImplicitLod:
	case SpvOpImageSampleExplicitLod:
	case SpvOpImageFetch:
	case SpvOpImageGather:
	case SpvOpImageDrefGather:
	case SpvOpImageRead:
	case SpvOpImageWrite:
	case SpvOpImage:
	case SpvOpImageQueryLod:
		return 1;
	case SpvOpCopyMemorySized:
		return 1;
	default:
		break;
	}

	if (opcode == SpvOpExtInst) {
		spvm_source p = operands;
		SPVM_SKIP_WORD(p);              /* result type */
		SPVM_SKIP_WORD(p);              /* result id */
		spvm_word set = SPVM_READ_WORD(p);
		spvm_word inst = SPVM_READ_WORD(p);
		if (set <= 0 || set >= state->owner->bound)
			return 1;
		spvm_result_t ext = &state->results[set];
		if (!ext->name || strcmp(ext->name, "GLSL.std.450") != 0)
			return 0;               /* host-provided; out of scope */
		if (!spvm_fwd_type_has_float(state, SPVM_READ_WORD(operands)))
			return 0;               /* integer result: tangent is zero */
		switch (inst) {
		case SPVM_GLSL450_Sin:
		case SPVM_GLSL450_Cos:
		case SPVM_GLSL450_Sqrt:
		case SPVM_GLSL450_Length:
		case SPVM_GLSL450_Normalize:
		case SPVM_GLSL450_FAbs:
			return 0;
		default:
			return 1;
		}
	}
	(void)spvm_fwd_float_producing;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Post-execution propagation                                          */
/* ------------------------------------------------------------------ */

static void spvm_fwd_op_load(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word ptr_id = o[2];
	spvm_fwd_copy_result(fwd, &state->results[id], &state->results[ptr_id]);
}

static void spvm_fwd_op_store(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word ptr_id = o[0];
	spvm_word val_id = o[1];
	spvm_fwd_copy_result(fwd, &state->results[ptr_id], &state->results[val_id]);
}

static void spvm_fwd_op_copymemory(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_fwd_copy_result(fwd, &state->results[o[0]], &state->results[o[1]]);
}

static void spvm_fwd_op_composite_construct(spvm_forward_t fwd, spvm_state_t state, spvm_word word_count, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word arg_count = word_count - 2;
	spvm_member_t result = state->results[id].members;
	spvm_source p = o + 2;

	for (spvm_word a = 0; a < arg_count; a++) {
		spvm_word index = SPVM_READ_WORD(p);
		spvm_result_t arg = &state->results[index];

		if (state->results[id].type == spvm_value_type_vector && arg->type == spvm_value_type_vector) {
			for (spvm_word j = 0; j < arg->member_count; j++) {
				spvm_fwd_copy_array(fwd, result, 1, &arg->members[j], 1);
				result++;
			}
		} else {
			/* Mirror the executor: scalar destinations take the argument's
			 * member 0, composite destinations copy all members. */
			if (result->member_count == 0)
				spvm_fwd_copy_array(fwd, result, 1, &arg->members[0], 1);
			else
				spvm_fwd_copy_array(fwd, result->members, result->member_count,
					arg->members, arg->member_count);
			result++;
		}
	}
}

static void spvm_fwd_op_composite_extract(spvm_forward_t fwd, spvm_state_t state, spvm_word word_count, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word value_id = o[2];
	spvm_word index_count = word_count - 4;
	spvm_source p = o + 3;

	spvm_word index = SPVM_READ_WORD(p);
	spvm_member_t result = state->results[value_id].members + index;
	while (index_count) {
		index = SPVM_READ_WORD(p);
		result = result->members + index;
		index_count--;
	}
	spvm_result_t dst = &state->results[id];
	if (result->member_count)
		spvm_fwd_copy_array(fwd, dst->members, dst->member_count,
			result->members, result->member_count);
	else
		spvm_fwd_copy_array(fwd, dst->members, dst->member_count, result, 1);
}
static void spvm_fwd_op_composite_insert(spvm_forward_t fwd, spvm_state_t state, spvm_word word_count, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word object_id = o[2];
	spvm_word value_id = o[3];
	spvm_word index_count = word_count - 5;
	spvm_source p = o + 4;

	spvm_fwd_copy_result(fwd, &state->results[id], &state->results[value_id]);

	spvm_word index = SPVM_READ_WORD(p);
	spvm_member_t result = state->results[id].members + index;
	while (index_count) {
		index = SPVM_READ_WORD(p);
		result = result->members + index;
		index_count--;
	}
	spvm_fwd_copy_node(fwd, result, state->results[object_id].members);
}

static void spvm_fwd_op_vector_shuffle(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word v1_id = o[2];
	spvm_word v2_id = o[3];
	spvm_source p = o + 4;

	spvm_result_t v1 = &state->results[v1_id];
	spvm_result_t v2 = &state->results[v2_id];
	for (spvm_word i = 0; i < state->results[id].member_count; i++) {
		spvm_word index = SPVM_READ_WORD(p);
		spvm_member_t src = index >= v1->member_count
			? &v2->members[index - v1->member_count]
			: &v1->members[index];
		spvm_fwd_copy_node(fwd, &state->results[id].members[i], src);
	}
}

static void spvm_fwd_op_vector_extract_dynamic(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word vector = o[2];
	spvm_word index_id = o[3];
	spvm_word index = (spvm_word)state->results[index_id].members[0].value.u64;
	spvm_result_t dst = &state->results[id];
	if (index >= 0 && index < state->results[vector].member_count)
		spvm_fwd_copy_array(fwd, dst->members, dst->member_count,
			&state->results[vector].members[index], 1);
	else
		spvm_fwd_zero_result(fwd, dst);
}

static void spvm_fwd_op_vector_insert_dynamic(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word vector = o[2];
	spvm_word comp = o[3];
	spvm_word index_id = o[4];
	spvm_word index = (spvm_word)state->results[index_id].members[0].value.u64;

	spvm_fwd_copy_result(fwd, &state->results[id], &state->results[vector]);
	if (index >= 0 && index < state->results[id].member_count)
		spvm_fwd_copy_node(fwd, &state->results[id].members[index], state->results[comp].members);
}

static void spvm_fwd_op_copy_object(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_fwd_copy_result(fwd, &state->results[o[1]], &state->results[o[2]]);
}

static void spvm_fwd_op_fnegate(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word op = o[2];
	spvm_result_t dst = &state->results[id];
	spvm_result_t src = &state->results[op];
	spvm_fwd_zero_result(fwd, dst);
	spvm_fwd_axpy_array(fwd, dst->members, dst->member_count,
		src->members, src->member_count, -1.0f);
}

enum spvm_fwd_binop { SPVM_FWD_ADD, SPVM_FWD_SUB, SPVM_FWD_MUL, SPVM_FWD_DIV };

static void spvm_fwd_binary_leaf(spvm_forward_t fwd, spvm_member_t dst,
	spvm_member_t a, spvm_member_t b, enum spvm_fwd_binop op)
{
	float av = a->value.f, bv = b->value.f;
	float ta[SPVM_FORWARD_MAX_DIRECTIONS], tb[SPVM_FORWARD_MAX_DIRECTIONS];
	spvm_fwd_read_all(fwd, a, ta);
	spvm_fwd_read_all(fwd, b, tb);

	float out[SPVM_FORWARD_MAX_DIRECTIONS];
	for (spvm_word k = 0; k < fwd->directions; k++) {
		switch (op) {
		case SPVM_FWD_ADD: out[k] = ta[k] + tb[k]; break;
		case SPVM_FWD_SUB: out[k] = ta[k] - tb[k]; break;
		case SPVM_FWD_MUL: out[k] = ta[k] * bv + av * tb[k]; break;
		case SPVM_FWD_DIV:
			if (bv == 0.0f) {
				spvm_fwd_error(fwd, fwd->state);
				out[k] = 0.0f;
			} else
				out[k] = (ta[k] * bv - av * tb[k]) / (bv * bv);
			break;
		default: out[k] = 0.0f; break;
		}
	}
	spvm_fwd_write_all(fwd, dst, out);
}

static void spvm_fwd_binary(spvm_forward_t fwd, spvm_state_t state, enum spvm_fwd_binop op, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word op1 = o[2];
	spvm_word op2 = o[3];

	spvm_word n = state->results[id].member_count;
	for (spvm_word i = 0; i < n; i++)
		spvm_fwd_binary_leaf(fwd, &state->results[id].members[i],
			&state->results[op1].members[i], &state->results[op2].members[i], op);
}

static void spvm_fwd_op_vector_times_scalar(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word vec_id = o[2];
	spvm_word scalar_id = o[3];

	spvm_member_t scalar = &state->results[scalar_id].members[0];
	float sv = scalar->value.f;
	float ds[SPVM_FORWARD_MAX_DIRECTIONS];
	spvm_fwd_read_all(fwd, scalar, ds);

	for (spvm_word i = 0; i < state->results[id].member_count; i++) {
		spvm_member_t v = &state->results[vec_id].members[i];
		spvm_member_t d = &state->results[id].members[i];
		float vv = v->value.f;
		float dv[SPVM_FORWARD_MAX_DIRECTIONS];
		spvm_fwd_read_all(fwd, v, dv);
		float out[SPVM_FORWARD_MAX_DIRECTIONS];
		for (spvm_word k = 0; k < fwd->directions; k++)
			out[k] = dv[k] * sv + vv * ds[k];
		spvm_fwd_write_all(fwd, d, out);
	}
}

static void spvm_fwd_op_dot(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word v1_id = o[2];
	spvm_word v2_id = o[3];

	spvm_member_t dst = &state->results[id].members[0];
	spvm_word n = state->results[v1_id].member_count < state->results[v2_id].member_count
		? state->results[v1_id].member_count : state->results[v2_id].member_count;

	float out[SPVM_FORWARD_MAX_DIRECTIONS];
	for (spvm_word k = 0; k < fwd->directions; k++) {
		float t = 0.0f;
		for (spvm_word i = 0; i < n; i++) {
			spvm_member_t a = &state->results[v1_id].members[i];
			spvm_member_t b = &state->results[v2_id].members[i];
			t += spvm_fwd_read(fwd, a, k) * b->value.f;
			t += a->value.f * spvm_fwd_read(fwd, b, k);
		}
		out[k] = t;
	}
	spvm_fwd_write_all(fwd, dst, out);
}

static void spvm_fwd_op_glsl_unary(spvm_forward_t fwd, spvm_state_t state, spvm_word inst, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word x_id = o[4];

	spvm_word n = state->results[id].member_count;
	for (spvm_word i = 0; i < n; i++) {
		spvm_member_t x = &state->results[x_id].members[i];
		spvm_member_t d = &state->results[id].members[i];
		float xv = x->value.f;
		float factor;
		switch (inst) {
		case SPVM_GLSL450_Sin: factor = cosf(xv); break;
		case SPVM_GLSL450_Cos: factor = -sinf(xv); break;
		case SPVM_GLSL450_Sqrt:
			if (xv > 0.0f)
				factor = 0.5f / sqrtf(xv);
			else {
				spvm_fwd_error(fwd, state);
				return;
			}
			break;
		case SPVM_GLSL450_FAbs:
			/* Convention: the tangent of |x| at x == 0 is zero. */
			factor = xv > 0.0f ? 1.0f : (xv < 0.0f ? -1.0f : 0.0f);
			break;
		default:
			factor = 0.0f;
			break;
		}
		float dx[SPVM_FORWARD_MAX_DIRECTIONS], out[SPVM_FORWARD_MAX_DIRECTIONS];
		spvm_fwd_read_all(fwd, x, dx);
		for (spvm_word k = 0; k < fwd->directions; k++)
			out[k] = dx[k] * factor;
		spvm_fwd_write_all(fwd, d, out);
	}
}

static void spvm_fwd_op_glsl_length(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word x_id = o[4];

	spvm_member_t dst = &state->results[id].members[0];
	spvm_word n = state->results[x_id].member_count;

	float len2 = 0.0f;
	for (spvm_word i = 0; i < n; i++) {
		float v = state->results[x_id].members[i].value.f;
		len2 += v * v;
	}
	if (len2 <= 0.0f) {
		spvm_fwd_error(fwd, state);
		return;
	}
	float len = sqrtf(len2);
	float out[SPVM_FORWARD_MAX_DIRECTIONS];
	for (spvm_word k = 0; k < fwd->directions; k++) {
		float t = 0.0f;
		for (spvm_word i = 0; i < n; i++)
			t += state->results[x_id].members[i].value.f *
				spvm_fwd_read(fwd, &state->results[x_id].members[i], k);
		out[k] = t / len;
	}
	spvm_fwd_write_all(fwd, dst, out);
}

static void spvm_fwd_op_glsl_normalize(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word x_id = o[4];

	spvm_word n = state->results[id].member_count;
	if (n <= 0 || n > 64) {
		spvm_fwd_error(fwd, state);
		return;
	}
	float len2 = 0.0f;
	for (spvm_word i = 0; i < n; i++) {
		float v = state->results[x_id].members[i].value.f;
		len2 += v * v;
	}
	if (len2 <= 0.0f) {
		spvm_fwd_error(fwd, state);
		return;
	}
	float len = sqrtf(len2);
	float inv = 1.0f / (len * len * len);

	float tangents[SPVM_FORWARD_MAX_DIRECTIONS][64];
	for (spvm_word k = 0; k < fwd->directions; k++) {
		float dot = 0.0f;
		for (spvm_word i = 0; i < n; i++)
			dot += state->results[x_id].members[i].value.f *
				spvm_fwd_read(fwd, &state->results[x_id].members[i], k);
		for (spvm_word i = 0; i < n; i++) {
			float dx = spvm_fwd_read(fwd, &state->results[x_id].members[i], k);
			tangents[k][i] = dx / len -
				state->results[x_id].members[i].value.f * dot * inv;
		}
	}
	for (spvm_word i = 0; i < n; i++) {
		spvm_member_t d = &state->results[id].members[i];
		float out[SPVM_FORWARD_MAX_DIRECTIONS];
		for (spvm_word k = 0; k < fwd->directions; k++)
			out[k] = tangents[k][i];
		spvm_fwd_write_all(fwd, d, out);
	}
}

static void spvm_fwd_op_select(spvm_forward_t fwd, spvm_state_t state, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word cond = o[2];
	spvm_word obj1 = o[3];
	spvm_word obj2 = o[4];

	spvm_byte c = state->results[cond].members[0].value.b;
	spvm_word obj = c ? obj1 : obj2;
	spvm_fwd_copy_result(fwd, &state->results[id], &state->results[obj]);
}

static void spvm_fwd_op_phi(spvm_forward_t fwd, spvm_state_t state, spvm_word word_count, spvm_source o)
{
	spvm_word id = o[1];
	spvm_word pairs = (word_count - 2) / 2;
	spvm_word parent = state->function_stack_cfg_parent[state->function_stack_current];
	spvm_source p = o + 2;

	for (spvm_word i = 0; i < pairs; i++) {
		spvm_word variable = SPVM_READ_WORD(p);
		spvm_word parent_lbl = SPVM_READ_WORD(p);
		if (parent_lbl == parent) {
			spvm_fwd_copy_result(fwd, &state->results[id], &state->results[variable]);
			return;
		}
	}
	spvm_fwd_zero_result(fwd, &state->results[id]);
}

static void spvm_fwd_op_function_call(spvm_forward_t fwd, spvm_state_t state, spvm_word word_count, spvm_source o)
{
	spvm_word func = o[2];
	spvm_word argc = word_count - 3;
	spvm_result_t next_func = &state->results[func];
	spvm_source p = o + 3;

	/* The result slot is assigned by spvm_state_pop_function_stack; only the
	 * argument -> parameter binding is handled here. */
	for (spvm_word i = 0; i < argc; i++) {
		spvm_word arg_id = SPVM_READ_WORD(p);
		spvm_word param_id = next_func->params[i];
		spvm_fwd_copy_result(fwd, &state->results[param_id], &state->results[arg_id]);
	}
}

/* ------------------------------------------------------------------ */
/* Observer entry points used by the interpreter                        */
/* ------------------------------------------------------------------ */

void spvm_forward_observe_pre(spvm_state_t state, SpvOp opcode, spvm_word word_count, spvm_source operands)
{
	(void)word_count;
	spvm_forward_t fwd = spvm_forward_get_context(state);
	if (!fwd)
		return;
	if (spvm_fwd_unsupported(state, opcode, operands) &&
		spvm_fwd_float_producing(state, opcode, operands))
		spvm_fwd_error(fwd, state);
}

void spvm_forward_observe_post(spvm_state_t state, SpvOp opcode, spvm_word word_count, spvm_source operands)
{
	spvm_forward_t fwd = spvm_forward_get_context(state);
	if (!fwd || fwd->error)
		return;


	switch (opcode) {
	case SpvOpLoad: spvm_fwd_op_load(fwd, state, operands); break;
	case SpvOpStore: spvm_fwd_op_store(fwd, state, operands); break;
	case SpvOpCopyMemory: spvm_fwd_op_copymemory(fwd, state, operands); break;
	case SpvOpVectorExtractDynamic: spvm_fwd_op_vector_extract_dynamic(fwd, state, operands); break;
	case SpvOpVectorInsertDynamic: spvm_fwd_op_vector_insert_dynamic(fwd, state, operands); break;
	case SpvOpVectorShuffle: spvm_fwd_op_vector_shuffle(fwd, state, operands); break;
	case SpvOpCompositeConstruct: spvm_fwd_op_composite_construct(fwd, state, word_count, operands); break;
	case SpvOpCompositeExtract: spvm_fwd_op_composite_extract(fwd, state, word_count, operands); break;
	case SpvOpCompositeInsert: spvm_fwd_op_composite_insert(fwd, state, word_count, operands); break;
	case SpvOpCopyObject: spvm_fwd_op_copy_object(fwd, state, operands); break;
	case SpvOpFNegate: spvm_fwd_op_fnegate(fwd, state, operands); break;
	case SpvOpFAdd: spvm_fwd_binary(fwd, state, SPVM_FWD_ADD, operands); break;
	case SpvOpFSub: spvm_fwd_binary(fwd, state, SPVM_FWD_SUB, operands); break;
	case SpvOpFMul: spvm_fwd_binary(fwd, state, SPVM_FWD_MUL, operands); break;
	case SpvOpFDiv: spvm_fwd_binary(fwd, state, SPVM_FWD_DIV, operands); break;
	case SpvOpVectorTimesScalar: spvm_fwd_op_vector_times_scalar(fwd, state, operands); break;
	case SpvOpDot: spvm_fwd_op_dot(fwd, state, operands); break;
	case SpvOpSelect: spvm_fwd_op_select(fwd, state, operands); break;
	case SpvOpPhi: spvm_fwd_op_phi(fwd, state, word_count, operands); break;
	case SpvOpFunctionCall: spvm_fwd_op_function_call(fwd, state, word_count, operands); break;
	case SpvOpExtInst: {
		spvm_word inst = operands[3];
		switch (inst) {
		case SPVM_GLSL450_Sin:
		case SPVM_GLSL450_Cos:
		case SPVM_GLSL450_Sqrt:
		case SPVM_GLSL450_FAbs:
			spvm_fwd_op_glsl_unary(fwd, state, inst, operands);
			break;
		case SPVM_GLSL450_Length:
			spvm_fwd_op_glsl_length(fwd, state, operands);
			break;
		case SPVM_GLSL450_Normalize:
			spvm_fwd_op_glsl_normalize(fwd, state, operands);
			break;
		default:
			break;
		}
		break;
	}
	default:
		break;
	}
}

void spvm_forward_observe_return(spvm_state_t state, spvm_word store_id, spvm_word value_id)
{
	spvm_forward_t fwd = spvm_forward_get_context(state);
	if (!fwd || fwd->error)
		return;
	if (store_id <= 0 || store_id >= state->owner->bound ||
		value_id <= 0 || value_id >= state->owner->bound)
		return;
	spvm_fwd_copy_result(fwd, &state->results[store_id], &state->results[value_id]);
}

void spvm_forward_observe_prepare(spvm_state_t state)
{
	spvm_forward_t fwd = spvm_forward_get_context(state);
	if (fwd)
		spvm_forward_clear(fwd);
}

void spvm_forward_observe_state_deleted(spvm_state_t state)
{
	spvm_forward_t fwd = spvm_forward_get_context(state);
	if (!fwd)
		return;
	spvm_fwd_unbind(state, fwd);
	fwd->state = NULL;
	fwd->active = 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

spvm_forward_t spvm_forward_create(spvm_state_t state, spvm_word direction_count)
{
	if (!state || direction_count < 1 || direction_count > SPVM_FORWARD_MAX_DIRECTIONS)
		return NULL;
	if (spvm_forward_get_context(state))
		return NULL;

	spvm_forward_t fwd = (spvm_forward_t)calloc(1, sizeof(struct spvm_forward));
	if (!fwd)
		return NULL;

	fwd->state = state;
	fwd->directions = direction_count;
	fwd->active = 1;

	if (!spvm_fwd_bind(state, fwd)) {
		free(fwd);
		return NULL;
	}
	return fwd;
}

void spvm_forward_delete(spvm_forward_t fwd)
{
	if (!fwd)
		return;
	spvm_fwd_unbind(fwd->state, fwd);
	spvm_fwd_entries_free(fwd);
	free(fwd);
}

void spvm_forward_clear(spvm_forward_t fwd)
{
	if (!fwd)
		return;
	spvm_fwd_entries_free(fwd);
	fwd->error = 0;
	fwd->error_instruction = 0;
	fwd->error_line = -1;
}

int spvm_forward_seed(spvm_forward_t fwd, spvm_word result_id,
	const spvm_word* path, spvm_word path_len, spvm_word direction, float value)
{
	if (!fwd || !fwd->active || !fwd->state)
		return 1;
	if (direction < 0 || direction >= fwd->directions)
		return 1;
	if (path_len < 0 || (path_len > 0 && !path))
		return 1;

	spvm_state_t state = fwd->state;
	if (result_id <= 0 || result_id >= state->owner->bound)
		return 1;

	spvm_result_t res = &state->results[result_id];

	/* Only actual storage can be seeded.  SPIR-V result types used as constants
	 * and intermediate values are rejected. */
	if (res->type != spvm_result_type_variable)
		return 1;
	switch (res->storage_class) {
	case SpvStorageClassInput:
	case SpvStorageClassUniform:
	case SpvStorageClassPrivate:
	case SpvStorageClassFunction:
	case SpvStorageClassOutput:
		break;
	default:
		return 1;
	}

	spvm_member_t leaf = spvm_fwd_resolve(state, result_id, path, path_len);
	if (!leaf && path_len == 1 && path[0] == 0) {
		/* A scalar is addressed by its implicit member 0 as well. */
		spvm_result_t ti = spvm_fwd_type_info(state, res->pointer);
		if (spvm_fwd_is_float32(ti) && res->members[0].member_count == 0)
			leaf = &res->members[0];
	}
	if (!leaf)
		return 1;

	spvm_fwd_write(fwd, leaf, direction, value);
	return 0;
}

int spvm_forward_get(spvm_forward_t fwd, spvm_word result_id,
	const spvm_word* path, spvm_word path_len, spvm_word direction, float* out)
{
	if (!fwd || !fwd->active || !fwd->state || !out)
		return 1;
	if (direction < 0 || direction >= fwd->directions)
		return 1;
	if (fwd->error)
		return 1;
	if (path_len < 0 || (path_len > 0 && !path))
		return 1;

	spvm_state_t state = fwd->state;
	if (result_id <= 0 || result_id >= state->owner->bound)
		return 1;

	spvm_member_t leaf = spvm_fwd_resolve(state, result_id, path, path_len);
	if (!leaf)
		return 1;

	*out = spvm_fwd_read(fwd, leaf, direction);
	return 0;
}

int spvm_forward_error(spvm_forward_t fwd, spvm_word* instruction)
{
	if (!fwd)
		return 0;
	if (instruction)
		*instruction = fwd->error_instruction;
	return fwd->error;
}

spvm_word spvm_forward_direction_count(spvm_forward_t fwd)
{
	return fwd ? fwd->directions : 0;
}

spvm_state_t spvm_forward_state(spvm_forward_t fwd)
{
	return fwd ? fwd->state : NULL;
}
