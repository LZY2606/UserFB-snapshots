#include <spvm/checkpoint.h>
#include <spvm/result.h>
#include <spvm/value.h>
#include <spvm/spirv.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Checkpoint binary format, version 1. Every multi-byte integer is stored
 * little-endian and is emitted field by field: the blob never contains a
 * host struct image, pointer, padding byte or uninitialized stack slot.
 * The authoritative layout documentation lives in doc/checkpoint-format.md.
 */

#define CP_MAGIC   0x4350564Du /* "CPVM" */
#define CP_VERSION 1u

#define CP_TAG_MODULE  0x31444f4du /* "MOD1" */
#define CP_TAG_EXEC    0x31434558u /* "XEC1" */
#define CP_TAG_VALUES  0x314c4156u /* "VAL1" */
#define CP_TAG_END     0x31444e45u /* "END1" */

#define CP_NONE 0xFFFFFFFFu

/* tree node kinds */
#define CP_KIND_SCALAR 1u
#define CP_KIND_VECTOR 2u
#define CP_KIND_MATRIX 3u
#define CP_KIND_ARRAY  4u
#define CP_KIND_STRUCT 5u

/* scalar leaf widths, in bytes */
#define CP_WBOOL 1u
#define CP_W32   4u
#define CP_W64   8u

/* execution flags */
#define CP_FLAG_TERMINAL 1u

typedef struct cp_buf {
	spvm_byte* p;
	size_t len;
	size_t cap;
	int ok;
} cp_buf;

static void cp_reserve(cp_buf* b, size_t extra)
{
	if (!b->ok)
		return;
	if (extra > SIZE_MAX - b->len) {
		b->ok = 0;
		return;
	}
	size_t need = b->len + extra;
	if (need <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 256;
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			cap = need;
			break;
		}
		cap *= 2;
	}
	spvm_byte* p = (spvm_byte*)realloc(b->p, cap);
	if (p == NULL) {
		b->ok = 0;
		return;
	}
	b->p = p;
	b->cap = cap;
}
static void put_u8(cp_buf* b, spvm_byte v)
{
	cp_reserve(b, 1);
	if (b->ok)
		b->p[b->len++] = v;
}
static void put_u32(cp_buf* b, uint32_t v)
{
	cp_reserve(b, 4);
	if (!b->ok)
		return;
	b->p[b->len++] = (spvm_byte)(v & 0xFF);
	b->p[b->len++] = (spvm_byte)((v >> 8) & 0xFF);
	b->p[b->len++] = (spvm_byte)((v >> 16) & 0xFF);
	b->p[b->len++] = (spvm_byte)((v >> 24) & 0xFF);
}
static void put_u64(cp_buf* b, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		put_u8(b, (spvm_byte)((v >> (8 * i)) & 0xFF));
}
static void put_u32_at(cp_buf* b, size_t at, uint32_t v)
{
	if (at + 4 > b->len) {
		b->ok = 0;
		return;
	}
	b->p[at] = (spvm_byte)(v & 0xFF);
	b->p[at + 1] = (spvm_byte)((v >> 8) & 0xFF);
	b->p[at + 2] = (spvm_byte)((v >> 16) & 0xFF);
	b->p[at + 3] = (spvm_byte)((v >> 24) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* reader                                                              */
/* ------------------------------------------------------------------ */

typedef struct cp_rd {
	const spvm_byte* p;
	size_t len;
	size_t cur;
	int ok;
} cp_rd;

static uint32_t get_u8(cp_rd* r)
{
	if (r->cur >= r->len) {
		r->ok = 0;
		return 0;
	}
	return r->p[r->cur++];
}
static uint32_t get_u32(cp_rd* r)
{
	if (r->cur + 4 > r->len) {
		r->ok = 0;
		return 0;
	}
	uint32_t v = (uint32_t)r->p[r->cur]
		| ((uint32_t)r->p[r->cur + 1] << 8)
		| ((uint32_t)r->p[r->cur + 2] << 16)
		| ((uint32_t)r->p[r->cur + 3] << 24);
	r->cur += 4;
	return v;
}
static uint64_t get_u64(cp_rd* r)
{
	if (r->cur + 8 > r->len) {
		r->ok = 0;
		return 0;
	}
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v |= (uint64_t)r->p[r->cur + i] << (8 * i);
	r->cur += 8;
	return v;
}
static int expect_tag(cp_rd* r, uint32_t tag)
{
	uint32_t t = get_u32(r);
	if (!r->ok || t != tag) {
		r->ok = 0;
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* module identity: two independent 64-bit checksums over every word   */
/* of the complete SPIR-V instruction stream                           */
/* ------------------------------------------------------------------ */

#define FNV_OFFSET_A 14695981039346656037ULL
#define FNV_OFFSET_B 7239295142933142347ULL
#define FNV_PRIME    1099511628211ULL

static void fnv1a_byte(uint64_t* h, spvm_byte b)
{
	*h ^= (uint64_t)b;
	*h *= FNV_PRIME;
}
static void fnv1a_word(uint64_t* h, uint32_t w)
{
	for (int i = 0; i < 4; i++)
		fnv1a_byte(h, (spvm_byte)((w >> (8 * i)) & 0xFF));
}
static void module_hashes(spvm_program_t prog, uint64_t* ha, uint64_t* hb)
{
	uint64_t a = FNV_OFFSET_A;
	uint64_t b = FNV_OFFSET_B;

	/* fold structural identity so equal-prefix streams cannot collide
	 * through truncation */
	fnv1a_word(&a, (uint32_t)prog->bound);
	fnv1a_word(&a, (uint32_t)prog->code_length);
	fnv1a_word(&b, (uint32_t)prog->code_length);
	fnv1a_word(&b, (uint32_t)prog->bound);

	for (size_t i = 0; i < prog->code_length; i++) {
		uint32_t w = (uint32_t)prog->code[i];
		fnv1a_word(&a, w);
		/* the second checksum consumes words in reverse byte order so a
		 * byte-reordering corruption cannot defeat both at once */
		for (int k = 3; k >= 0; k--)
			fnv1a_byte(&b, (spvm_byte)((w >> (8 * k)) & 0xFF));
	}
	*ha = a;
	*hb = b;
}

/* ------------------------------------------------------------------ */
/* type shape helpers                                                  */
/* ------------------------------------------------------------------ */

static spvm_result_t cp_type_info(spvm_result* results, spvm_word type)
{
	return spvm_state_get_type_info(results, &results[type]);
}
static int cp_is_scalar_type(spvm_result_t t)
{
	return t->value_type == spvm_value_type_bool ||
		t->value_type == spvm_value_type_int ||
		t->value_type == spvm_value_type_float;
}
static spvm_byte cp_leaf_width(spvm_result_t t)
{
	if (t->value_type == spvm_value_type_bool)
		return CP_WBOOL;
	if (t->value_bitcount > 32)
		return CP_W64;
	return CP_W32;
}
static int cp_supported_type(spvm_result_t t)
{
	switch (t->value_type) {
	case spvm_value_type_void:
	case spvm_value_type_bool:
	case spvm_value_type_int:
	case spvm_value_type_float:
	case spvm_value_type_vector:
	case spvm_value_type_matrix:
	case spvm_value_type_array:
	case spvm_value_type_struct:
		return 1;
	default:
		return 0;
	}
}
static spvm_byte cp_composite_kind(spvm_result_t t)
{
	switch (t->value_type) {
	case spvm_value_type_vector: return CP_KIND_VECTOR;
	case spvm_value_type_matrix: return CP_KIND_MATRIX;
	case spvm_value_type_array:  return CP_KIND_ARRAY;
	default:                     return CP_KIND_STRUCT;
	}
}

/* ------------------------------------------------------------------ */
/* support gate: numeric Logical/GLSL shaders only                     */
/* ------------------------------------------------------------------ */

static int cp_storage_allowed(SpvStorageClass sc)
{
	return sc == SpvStorageClassFunction ||
		sc == SpvStorageClassPrivate ||
		sc == SpvStorageClassInput ||
		sc == SpvStorageClassOutput ||
		sc == SpvStorageClassUniform;
}

/* opcode-level rejection: the checkpoint only covers plain numeric
 * execution, so any instruction family backed by external resources or
 * unsupported storage makes the whole module ineligible */
static int cp_opcode_supported(uint32_t op)
{
	if (op >= SpvOpSampledImage && op <= SpvOpImageQuerySamples) /* 86..107 */
		return 0;
	if (op >= SpvOpEmitVertex && op <= SpvOpEndStreamPrimitive) /* 218..221 */
		return 0;
	if (op == SpvOpControlBarrier || op == SpvOpMemoryBarrier)
		return 0;
	if (op >= SpvOpAtomicLoad && op <= SpvOpAtomicXor) /* 227..242 */
		return 0;
	if (op == SpvOpAtomicFlagTestAndSet || op == SpvOpAtomicFlagClear)
		return 0;
	if (op >= SpvOpImageSparseSampleImplicitLod && op <= SpvOpImageSparseTexelsResident)
		return 0;
	if (op == SpvOpImageSparseRead)
		return 0;
	if (op == SpvOpNamedBarrierInitialize || op == SpvOpMemoryNamedBarrier)
		return 0;
	return 1;
}
static int cp_type_opcode_supported(uint32_t op)
{
	return op != SpvOpTypeImage &&
		op != SpvOpTypeSampler &&
		op != SpvOpTypeSampledImage &&
		op != SpvOpTypeRuntimeArray;
}
static int cp_module_supported(spvm_program_t prog)
{
	spvm_source code = prog->code;
	size_t i = 0;
	while (i < prog->code_length) {
		spvm_word head = code[i];
		spvm_word words = head >> SpvWordCountShift;
		uint32_t op = (uint32_t)(head & SpvOpCodeMask);
		if (words < 1 || (size_t)words > prog->code_length - i)
			return 0;
		if (!cp_opcode_supported(op) || !cp_type_opcode_supported(op))
			return 0;
		i += (size_t)words;
	}
	return i == prog->code_length;
}
static int cp_state_supported(spvm_state_t state)
{
	spvm_program_t prog = state->owner;

	if (prog->addressing != SpvAddressingModelLogical)
		return 0;
	if (!cp_module_supported(prog))
		return 0;
	if (state->derivative_used || state->derivative_group_x ||
		state->derivative_group_y || state->derivative_group_d ||
		state->_derivative_is_group_member)
		return 0;
	if (state->emit_vertex || state->end_primitive || state->control_barrier)
		return 0;
	if (prog->allocate_workgroup_memory || prog->write_workgroup_memory ||
		prog->atomic_operation)
		return 0;

	for (spvm_word id = 1; id < (spvm_word)prog->bound; id++) {
		spvm_result_t r = &state->results[id];

		if (r->type == spvm_result_type_variable &&
			!cp_storage_allowed(r->storage_class))
			return 0;

		if (r->type == spvm_result_type_type &&
			r->value_type != spvm_value_type_pointer &&
			!cp_supported_type(r))
			return 0;

		if (r->pointer) {
			spvm_result_t t = cp_type_info(state->results, r->pointer);
			if (t && !cp_supported_type(t))
				return 0;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* value tree encoding                                                 */
/*                                                                     */
/* A member *array* physically holds `count` slots of static type T.   */
/* Result-level variable/constant trees are such arrays; a composite   */
/* slot reaches its elements through its own .members array.           */
/* ------------------------------------------------------------------ */

static int cp_write_array(cp_buf* b, spvm_result* results,
	spvm_member_t node, spvm_word count, spvm_word type_id);

/* A composite slot reaches its elements through .members; a scalar slot
 * carries the value itself. */
static int cp_write_slot(cp_buf* b, spvm_result* results,
	spvm_member_t slot, spvm_word type_id)
{
	spvm_result_t t = cp_type_info(results, type_id);
	if (t == NULL || !cp_supported_type(t))
		return 0;
	if (cp_is_scalar_type(t)) {
		if (slot->member_count != 0)
			return 0;
		put_u8(b, CP_KIND_SCALAR);
		put_u8(b, cp_leaf_width(t));
		put_u64(b, slot->value.u64);
		return b->ok;
	}
	if (slot->member_count == 0 || slot->members == NULL)
		return 0;
	return cp_write_array(b, results, slot->members, slot->member_count, type_id);
}

static int cp_write_array(cp_buf* b, spvm_result* results,
	spvm_member_t node, spvm_word count, spvm_word type_id)
{
	if (type_id == 0 || type_id == (spvm_word)-1 || node == NULL || count == 0)
		return 0;
	spvm_result_t t = cp_type_info(results, type_id);
	if (t == NULL || !cp_supported_type(t))
		return 0;

	if (cp_is_scalar_type(t)) {
		if (count != 1)
			return 0;
		put_u8(b, CP_KIND_SCALAR);
		put_u8(b, cp_leaf_width(t));
		put_u64(b, node[0].value.u64);
		return b->ok;
	}

	put_u8(b, cp_composite_kind(t));
	put_u32(b, (uint32_t)count);
	if (!b->ok)
		return 0;
	if (t->value_type == spvm_value_type_struct) {
		if (count != t->member_count)
			return 0;
		for (spvm_word i = 0; i < count; i++)
			if (!cp_write_slot(b, results, &node[i], t->params[i]))
				return 0;
	}
	else {
		for (spvm_word i = 0; i < count; i++)
			if (!cp_write_slot(b, results, &node[i], t->pointer))
				return 0;
	}
	return b->ok;
}

/* ------------------------------------------------------------------ */
/* alias resolution: find a member slot inside an owning value tree    */
/* and record the index path from the root's member array              */
/* ------------------------------------------------------------------ */

/* Find `target` inside the tree rooted at the `count` sibling slots in
 * `node`. A pointer alias names either a slot itself (scalar case, the
 * alias .members points at the slot) or a slot's child array (composite
 * case, the alias .members equals the slot's .members); both resolve to
 * the index path of that slot. A target equal to the root's own member
 * array resolves to an empty path. */
static int cp_resolve_path(spvm_member_t node, spvm_word count,
	spvm_member_t target, spvm_word* path, spvm_word* depth, spvm_word cap)
{
	for (spvm_word i = 0; i < count; i++) {
		if (&node[i] == target) {
			if (*depth >= cap)
				return 0;
			path[(*depth)++] = i;
			return 1;
		}
	}
	for (spvm_word i = 0; i < count; i++) {
		if (node[i].member_count && node[i].members) {
			if (node[i].members == target) {
				if (*depth >= cap)
					return 0;
				path[(*depth)++] = i;
				return 1;
			}
			spvm_word before = *depth;
			if (*depth >= cap)
				return 0;
			path[(*depth)++] = i;
			if (cp_resolve_path(node[i].members, node[i].member_count,
				target, path, depth, cap))
				return 1;
			*depth = before;
		}
	}
	return 0;
}

static int cp_is_root(spvm_result_t r)
{
	return (r->type == spvm_result_type_variable ||
		r->type == spvm_result_type_constant) &&
		r->members != NULL && r->member_count > 0;
}
static int cp_is_alias(spvm_result_t r)
{
	return (r->type == spvm_result_type_access_chain ||
		r->type == spvm_result_type_function_parameter) &&
		r->members != NULL;
}

/* ------------------------------------------------------------------ */
/* save: execution section                                             */
/* ------------------------------------------------------------------ */

static uint32_t cp_find_file(spvm_state_t s)
{
	if (s->current_file == NULL)
		return CP_NONE;
	for (spvm_word id = 1; id < (spvm_word)s->owner->bound; id++) {
		if (s->results[id].type == spvm_result_type_string &&
			s->results[id].name == s->current_file)
			return (uint32_t)id;
	}
	return CP_NONE;
}

static int cp_write_execution(cp_buf* b, spvm_state_t s)
{
	put_u32(b, CP_TAG_EXEC);
	size_t size_at = b->len;
	put_u32(b, 0);
	size_t payload_at = b->len;

	uint32_t flags = s->code_current == NULL ? CP_FLAG_TERMINAL : 0u;
	put_u32(b, flags);

	uint32_t line = s->current_line < 0 ? CP_NONE : (uint32_t)s->current_line;
	uint32_t column = s->current_column < 0 ? CP_NONE : (uint32_t)s->current_column;
	uint32_t ret = s->return_id < 0 ? CP_NONE : (uint32_t)s->return_id;
	uint32_t file = cp_find_file(s);
	if (file == CP_NONE && s->current_file != NULL)
		return 0;

	if (flags & CP_FLAG_TERMINAL) {
		put_u32(b, s->instruction_count);
		put_u32(b, ret);
		put_u32(b, line);
		put_u32(b, column);
		put_u32(b, file);
		put_u8(b, s->did_jump);
		put_u8(b, s->discarded);
		if (!b->ok)
			return 0;
		put_u32_at(b, size_at, (uint32_t)(b->len - payload_at));
		return 1;
	}

	if (s->function_stack == NULL)
		return 0;
	spvm_word depth = s->function_stack_current + 1;
	if (depth < 1 || (uint32_t)depth > (uint32_t)s->owner->code_length)
		return 0;

	ptrdiff_t pc = s->code_current - s->owner->code;
	if (pc < 0 || (uint32_t)pc >= (uint32_t)s->owner->code_length)
		return 0;
	ptrdiff_t fn = s->current_function - s->results;
	if (fn <= 0 || fn >= s->owner->bound)
		return 0;

	put_u32(b, (uint32_t)pc);
	put_u32(b, s->instruction_count);
	put_u32(b, (uint32_t)fn);
	put_u32(b, ret);
	put_u32(b, s->current_parameter);
	put_u32(b, line);
	put_u32(b, column);
	put_u32(b, file);
	put_u8(b, s->did_jump);
	put_u8(b, s->discarded);
	put_u32(b, (uint32_t)depth);

	for (spvm_word i = 0; i < depth; i++) {
		ptrdiff_t fpc = s->function_stack[i] - s->owner->code;
		ptrdiff_t fid = s->function_stack_info[i] - s->results;
		if (fpc < 0 || (uint32_t)fpc >= (uint32_t)s->owner->code_length)
			return 0;
		if (fid <= 0 || fid >= s->owner->bound || s->function_stack_info[i] == NULL)
			return 0;
		put_u32(b, (uint32_t)fpc);
		put_u32(b, (uint32_t)fid);
		put_u32(b, i == 0 ? CP_NONE : (uint32_t)s->function_stack_returns[i]);
		put_u32(b, s->function_stack_cfg[i]);
		put_u32(b, s->function_stack_cfg_parent[i]);
	}

	if (!b->ok)
		return 0;
	put_u32_at(b, size_at, (uint32_t)(b->len - payload_at));
	return 1;
}

/* ------------------------------------------------------------------ */
/* save: values section                                                */
/* ------------------------------------------------------------------ */

static int cp_write_values(cp_buf* b, spvm_state_t s)
{
	spvm_word bound = s->owner->bound;

	put_u32(b, CP_TAG_VALUES);	size_t size_at = b->len;
	put_u32(b, 0);
	size_t payload_at = b->len;

	spvm_word root_count = 0;
	for (spvm_word id = 1; id < bound; id++)
		if (cp_is_root(&s->results[id]))
			root_count++;
	put_u32(b, (uint32_t)root_count);

	for (spvm_word id = 1; id < bound; id++) {
		spvm_result_t r = &s->results[id];
		if (!cp_is_root(r))
			continue;
		if (r->pointer == 0)
			return 0;
		spvm_result_t t = cp_type_info(s->results, r->pointer);
		if (t == NULL || !cp_supported_type(t))
			return 0;
		put_u32(b, (uint32_t)id);
		if (!cp_write_array(b, s->results, r->members, r->member_count, r->pointer))
			return 0;
	}

	spvm_word alias_count = 0;
	for (spvm_word id = 1; id < bound; id++)
		if (cp_is_alias(&s->results[id]))
			alias_count++;
	put_u32(b, (uint32_t)alias_count);

	spvm_word* path = (spvm_word*)malloc((size_t)bound * sizeof(spvm_word));
	if (path == NULL)
		return 0;

	for (spvm_word id = 1; id < bound; id++) {
		spvm_result_t r = &s->results[id];
		if (!cp_is_alias(r))
			continue;

		spvm_word root = 0;
		spvm_word depth = 0;
		for (spvm_word cand = 1; cand < bound; cand++) {
			spvm_result_t rr = &s->results[cand];
			if (!cp_is_root(rr))
				continue;
			depth = 0;
			if (cp_resolve_path(rr->members, rr->member_count,
				r->members, path, &depth, bound)) {
				root = cand;
				break;
			}
		}
		if (root == 0 || depth == 0) {
			free(path);
			return 0;
		}

		put_u32(b, (uint32_t)id);
		put_u32(b, (uint32_t)root);
		put_u32(b, (uint32_t)depth);
		for (spvm_word i = 0; i < depth; i++)
			put_u32(b, (uint32_t)path[i]);
		if (!b->ok) {
			free(path);
			return 0;
		}
	}
	free(path);

	if (!b->ok)
		return 0;
	put_u32_at(b, size_at, (uint32_t)(b->len - payload_at));
	return 1;
}

int spvm_checkpoint_save(spvm_state_t state, spvm_byte** data, size_t* size)
{
	if (data)
		*data = NULL;
	if (size)
		*size = 0;
	if (state == NULL || data == NULL || size == NULL)
		return 0;
	if (!cp_state_supported(state))
		return 0;

	cp_buf b;
	b.p = NULL;
	b.len = 0;
	b.cap = 0;
	b.ok = 1;

	put_u32(&b, CP_MAGIC);
	put_u32(&b, CP_VERSION);

	put_u32(&b, CP_TAG_MODULE);
	size_t mod_size_at = b.len;
	put_u32(&b, 0);
	size_t mod_payload_at = b.len;
	put_u32(&b, (uint32_t)state->owner->bound);
	put_u32(&b, (uint32_t)state->owner->code_length);
	uint64_t ha, hb;
	module_hashes(state->owner, &ha, &hb);
	put_u64(&b, ha);
	put_u64(&b, hb);
	if (!b.ok)
		goto fail;
	put_u32_at(&b, mod_size_at, (uint32_t)(b.len - mod_payload_at));

	if (!cp_write_execution(&b, state))
		goto fail;
	if (!cp_write_values(&b, state))
		goto fail;
	put_u32(&b, CP_TAG_END);
	if (!b.ok)
		goto fail;

	*data = b.p;
	*size = b.len;
	return 1;

fail:
	free(b.p);
	return 0;
}

/* ------------------------------------------------------------------ */
/* restore                                                             */
/* ------------------------------------------------------------------ */

typedef struct cp_frame {
	uint32_t pc;
	uint32_t func;
	uint32_t ret;
	uint32_t cfg;
	uint32_t parent;
} cp_frame;

typedef struct cp_root_rec {
	uint32_t id;
	size_t off;
} cp_root_rec;

typedef struct cp_alias_rec {
	uint32_t id;
	uint32_t root;
	size_t path_off;
	uint32_t path_len;
} cp_alias_rec;

typedef struct cp_image {
	cp_rd rd;
	spvm_state_t state;
	spvm_program_t prog;

	uint32_t bound;
	uint32_t code_words;

	uint32_t flags;
	int terminal;

	uint32_t pc;
	uint32_t inst;
	uint32_t current_function;
	uint32_t return_id;
	uint32_t current_parameter;
	uint32_t line;
	uint32_t column;
	uint32_t file;
	spvm_byte did_jump;
	spvm_byte discarded;
	uint32_t depth;
	cp_frame* frames;

	uint32_t root_count;
	uint32_t alias_count;
	cp_root_rec* roots;
	cp_alias_rec* aliases;

	spvm_byte* boundary; /* instruction-boundary map, one byte per word */
	uint32_t* prev_boundary; /* previous instruction start for each word */
	spvm_byte* aliasable; /* result ids produced by OpAccessChain/OpFunctionParameter */
} cp_image;

static int cp_mark_boundaries(cp_image* im)
{
	im->boundary = (spvm_byte*)calloc(im->code_words, 1);
	im->prev_boundary = (uint32_t*)malloc((size_t)im->code_words * sizeof(uint32_t));
	im->aliasable = (spvm_byte*)calloc(im->bound, 1);
	if (im->boundary == NULL || im->prev_boundary == NULL || im->aliasable == NULL) {
		free(im->boundary);
		free(im->prev_boundary);
		free(im->aliasable);
		im->boundary = NULL;
		im->prev_boundary = NULL;
		im->aliasable = NULL;
		return 0;
	}

	spvm_source code = im->prog->code;
	size_t i = 0;
	uint32_t last = CP_NONE;
	while (i < im->code_words) {
		spvm_word head = code[i];
		spvm_word words = head >> SpvWordCountShift;
		if (words < 1 || (size_t)words > im->code_words - i) {
			free(im->boundary);
			free(im->prev_boundary);
			free(im->aliasable);
			im->boundary = NULL;
			im->prev_boundary = NULL;
			im->aliasable = NULL;
			return 0;
		}
		im->boundary[i] = 1;
		im->prev_boundary[i] = last;
		last = (uint32_t)i;

		uint32_t op = (uint32_t)(head & SpvOpCodeMask);
		/* OpAccessChain: head, result type, result id, base, ...
		 * OpFunctionParameter: head, type, result id */
		if ((op == SpvOpAccessChain || op == SpvOpInBoundsAccessChain ||
			op == SpvOpPtrAccessChain || op == SpvOpInBoundsPtrAccessChain) &&
			words >= 3) {
			uint32_t id = (uint32_t)code[i + 2];
			if (id < im->bound)
				im->aliasable[id] = 1;
		}
		else if (op == SpvOpFunctionParameter && words >= 3) {
			uint32_t id = (uint32_t)code[i + 2];
			if (id < im->bound)
				im->aliasable[id] = 1;
		}

		i += (size_t)words;
	}
	return i == im->code_words;
}

static int cp_valid_id(cp_image* im, uint32_t id)
{
	return id >= 1 && id < im->bound;
}
static int cp_at_boundary(cp_image* im, uint32_t off)
{
	return off < im->code_words && im->boundary[off];
}
static uint32_t cp_opcode_at(cp_image* im, uint32_t off)
{
	return (uint32_t)(im->prog->code[off] & SpvOpCodeMask);
}

static int cp_check_module(cp_image* im)
{
	cp_rd* r = &im->rd;
	if (!expect_tag(r, CP_TAG_MODULE))
		return 0;
	uint32_t payload = get_u32(r);
	/* bound + code_length + two 64-bit checksums */
	if (!r->ok || payload != 24 || r->cur + payload > r->len)
		return 0;
	size_t end = r->cur + payload;

	uint32_t bound = get_u32(r);
	uint32_t code_words = get_u32(r);
	uint64_t ha = get_u64(r);
	uint64_t hb = get_u64(r);
	if (!r->ok || r->cur != end)
		return 0;

	if (bound != (uint32_t)im->prog->bound ||
		code_words != (uint32_t)im->prog->code_length)
		return 0;

	uint64_t wa, wb;
	module_hashes(im->prog, &wa, &wb);
	if (ha != wa || hb != wb)
		return 0;

	im->bound = bound;
	im->code_words = code_words;
	return 1;
}

static int cp_check_execution(cp_image* im)
{
	cp_rd* r = &im->rd;
	if (!expect_tag(r, CP_TAG_EXEC))
		return 0;
	uint32_t payload = get_u32(r);
	if (!r->ok || payload < 6 || r->cur + payload > r->len)
		return 0;
	size_t end = r->cur + payload;

	im->flags = get_u32(r);
	if (im->flags & ~(uint32_t)CP_FLAG_TERMINAL)
		return 0;
	im->terminal = (im->flags & CP_FLAG_TERMINAL) != 0;

	if (im->terminal) {
		im->inst = get_u32(r);
		im->return_id = get_u32(r);
		im->line = get_u32(r);
		im->column = get_u32(r);
		im->file = get_u32(r);
		im->did_jump = (spvm_byte)get_u8(r);
		im->discarded = (spvm_byte)get_u8(r);
		if (!r->ok || r->cur != end)
			return 0;
	}
	else {
		im->pc = get_u32(r);
		im->inst = get_u32(r);
		im->current_function = get_u32(r);
		im->return_id = get_u32(r);
		im->current_parameter = get_u32(r);
		im->line = get_u32(r);
		im->column = get_u32(r);
		im->file = get_u32(r);
		im->did_jump = (spvm_byte)get_u8(r);
		im->discarded = (spvm_byte)get_u8(r);
		im->depth = get_u32(r);
		if (!r->ok)
			return 0;

		if (im->depth < 1 || im->depth > im->code_words)
			return 0;
		size_t frame_bytes = (size_t)im->depth * 5u * 4u;
		if (r->cur + frame_bytes != end)
			return 0;

		im->frames = (cp_frame*)calloc(im->depth, sizeof(cp_frame));
		if (im->frames == NULL)
			return 0;

		for (uint32_t i = 0; i < im->depth; i++) {
			im->frames[i].pc = get_u32(r);
			im->frames[i].func = get_u32(r);
			im->frames[i].ret = get_u32(r);
			im->frames[i].cfg = get_u32(r);
			im->frames[i].parent = get_u32(r);
			if (!r->ok)
				return 0;
		}
		if (r->cur != end)
			return 0;
	}

	if (im->return_id != CP_NONE && !cp_valid_id(im, im->return_id))
		return 0;
	if (im->file != CP_NONE) {
		if (!cp_valid_id(im, im->file) ||
			im->state->results[im->file].type != spvm_result_type_string)
			return 0;
	}
	if (im->line == CP_NONE && im->column != CP_NONE)
		return 0;
	if (im->terminal)
		return 1;

	if (!cp_at_boundary(im, im->pc))
		return 0;
	if (!cp_valid_id(im, im->current_function) ||
		im->state->results[im->current_function].type != spvm_result_type_function)
		return 0;

	/* current PC must lie inside the body of the current function,
	 * from its OpFunction entry up to (but excluding) OpFunctionEnd */
	spvm_result_t cur_fn = &im->state->results[im->current_function];
	ptrdiff_t cur_entry = cur_fn->source_location - im->prog->code;
	if (cur_entry < 0 || (uint32_t)cur_entry > im->pc)
		return 0;
	uint32_t body_end = im->code_words;
	{
		size_t j = (size_t)cur_entry;
		while (j < im->code_words) {
			spvm_word h = im->prog->code[j];
			spvm_word wc = h >> SpvWordCountShift;
			if (wc < 1)
				return 0;
			if ((h & SpvOpCodeMask) == SpvOpFunctionEnd) {
				body_end = (uint32_t)j;
				break;
			}
			j += (size_t)wc;
		}
	}
	if (im->pc >= body_end)
		return 0;

	for (uint32_t i = 0; i < im->depth; i++) {
		cp_frame* f = &im->frames[i];
		if (!cp_at_boundary(im, f->pc))
			return 0;
		if (!cp_valid_id(im, f->func) ||
			im->state->results[f->func].type != spvm_result_type_function)
			return 0;
		/* only the deepest frame stores its function entry; the frames
		 * above it store resume points, validated through the call that
		 * opened the frame below them */
		if (i == im->depth - 1) {
			spvm_result_t fn_res = &im->state->results[f->func];
			ptrdiff_t entry = fn_res->source_location - im->prog->code;
			if (entry < 0 || (uint32_t)entry >= im->code_words ||
				f->pc != (uint32_t)entry)
				return 0;
		}
		if (f->cfg != 0 &&
			(!cp_valid_id(im, f->cfg) ||
			im->state->results[f->cfg].type != spvm_result_type_label))
			return 0;
		if (f->parent != 0 &&
			(!cp_valid_id(im, f->parent) ||
			im->state->results[f->parent].type != spvm_result_type_label))
			return 0;
		if (i == 0) {
			if (f->ret != CP_NONE)
				return 0;
		}
		else {
			cp_frame* caller = &im->frames[i - 1];
			if (!cp_valid_id(im, f->ret))
				return 0;
			/* the executor parks the caller frame on the instruction
			 * following the OpFunctionCall that created this frame;
			 * validate that preceding call and its result/callee ids */
			uint32_t call_pc = im->prev_boundary[caller->pc];
			if (call_pc == CP_NONE || cp_opcode_at(im, call_pc) != SpvOpFunctionCall)
				return 0;
			spvm_word head = im->prog->code[call_pc];
			spvm_word words = head >> SpvWordCountShift;
			if (words < 4)
				return 0;
			if ((uint32_t)im->prog->code[call_pc + 2] != f->ret ||
				(uint32_t)im->prog->code[call_pc + 3] != f->func)
				return 0;
		}
	}

	cp_frame* top = &im->frames[im->depth - 1];
	if (top->func != im->current_function)
		return 0;
	return 1;
}

/* validate an encoded tree against the target's existing member shape;
 * the reader is left positioned just past the tree. No value is written. */
static int cp_check_array(cp_image* im, spvm_member_t node,
	spvm_word count, spvm_word type_id);

static int cp_check_slot(cp_image* im, spvm_member_t slot, spvm_word type_id)
{
	spvm_result_t t = cp_type_info(im->state->results, type_id);
	if (t == NULL || !cp_supported_type(t))
		return 0;
	if (cp_is_scalar_type(t)) {
		if (slot->member_count != 0)
			return 0;
		cp_rd* r = &im->rd;
		if (get_u8(r) != CP_KIND_SCALAR || !r->ok)
			return 0;
		if (get_u8(r) != cp_leaf_width(t))
			return 0;
		(void)get_u64(r);
		return r->ok;
	}
	if (slot->member_count == 0 || slot->members == NULL)
		return 0;
	return cp_check_array(im, slot->members, slot->member_count, type_id);
}

static int cp_check_array(cp_image* im, spvm_member_t node,
	spvm_word count, spvm_word type_id)
{
	cp_rd* r = &im->rd;
	if (!cp_valid_id(im, type_id) || node == NULL || count == 0)
		return 0;
	spvm_result_t t = cp_type_info(im->state->results, type_id);
	if (t == NULL || !cp_supported_type(t))
		return 0;

	uint32_t kind = get_u8(r);
	if (!r->ok)
		return 0;

	if (cp_is_scalar_type(t)) {
		if (kind != CP_KIND_SCALAR || count != 1)
			return 0;
		uint32_t width = get_u8(r);
		if (width != cp_leaf_width(t))
			return 0;
		(void)get_u64(r);
		return r->ok;
	}

	if (kind != cp_composite_kind(t))
		return 0;
	uint32_t encoded_count = get_u32(r);
	if (!r->ok || encoded_count != (uint32_t)count)
		return 0;
	if (t->value_type == spvm_value_type_struct && count != t->member_count)
		return 0;

	for (spvm_word i = 0; i < count; i++) {
		spvm_word child_type = t->value_type == spvm_value_type_struct
			? t->params[i] : t->pointer;
		if (!cp_check_slot(im, &node[i], child_type))
			return 0;
	}
	return r->ok;
}

/* walk a member-index path inside a root tree, checking every index
 * against the physical bounds of the target's own allocation */
static int cp_check_path(cp_image* im, spvm_result_t root_res, uint32_t path_len)
{
	cp_rd* r = &im->rd;
	spvm_member_t cur = root_res->members;
	spvm_word cur_count = root_res->member_count;

	for (uint32_t k = 0; k < path_len; k++) {
		uint32_t idx = get_u32(r);
		if (!r->ok || idx >= (uint32_t)cur_count)
			return 0;
		if (k + 1 < path_len) {
			if (cur[idx].member_count == 0 || cur[idx].members == NULL)
				return 0;
			spvm_word next_count = cur[idx].member_count;
			cur = cur[idx].members;
			cur_count = next_count;
		}
	}
	return 1;
}

static int cp_check_values(cp_image* im)
{
	cp_rd* r = &im->rd;
	if (!expect_tag(r, CP_TAG_VALUES))
		return 0;
	uint32_t payload = get_u32(r);
	if (!r->ok || r->cur + payload > r->len)
		return 0;
	size_t end = r->cur + payload;

	uint64_t remaining = (uint64_t)(end - r->cur);
	uint32_t root_count = get_u32(r);
	/* smallest possible root record: one id plus a scalar node */
	if (!r->ok || root_count >= im->bound || root_count * 14ull > remaining)
		return 0;

	im->root_count = root_count;
	im->roots = (cp_root_rec*)calloc(root_count ? root_count : 1, sizeof(cp_root_rec));
	if (im->roots == NULL)
		return 0;

	spvm_byte* seen = (spvm_byte*)calloc(im->bound, 1);
	if (seen == NULL)
		goto fail;

	for (uint32_t i = 0; i < root_count; i++) {
		uint32_t id = get_u32(r);
		if (!r->ok || !cp_valid_id(im, id) || seen[id])
			goto fail;
		spvm_result_t res = &im->state->results[id];
		if (!cp_is_root(res) || res->pointer == 0)
			goto fail;
		seen[id] = 1;
		im->roots[i].id = id;
		im->roots[i].off = r->cur;
		if (!cp_check_array(im, res->members, res->member_count, res->pointer))
			goto fail;
		if (r->cur > end)
			goto fail;
	}

	/* every owning tree of the target must be covered, otherwise an
	 * uncaptured result would keep stale target values */
	for (uint32_t id = 1; id < im->bound; id++) {
		if (cp_is_root(&im->state->results[id]) && !seen[id])
			goto fail;
	}

	uint32_t alias_count = get_u32(r);
	/* each alias record has at least a 12-byte header (id/root/length) */
	if (!r->ok || alias_count >= im->bound ||
		(uint64_t)alias_count * 12ull > (uint64_t)(end - r->cur))
		goto fail;

	im->alias_count = alias_count;
	im->aliases = (cp_alias_rec*)calloc(alias_count ? alias_count : 1, sizeof(cp_alias_rec));
	if (im->aliases == NULL)
		goto fail;

	for (uint32_t i = 0; i < alias_count; i++) {
		size_t rec_at = r->cur;
		uint32_t id = get_u32(r);
		uint32_t root = get_u32(r);
		uint32_t path_len = get_u32(r);
		if (!r->ok || !cp_valid_id(im, id) || seen[id] ||
			!cp_valid_id(im, root) || path_len < 1 ||
			(uint64_t)path_len * 4ull > (uint64_t)(end - r->cur))
			goto fail;
		spvm_result_t root_res = &im->state->results[root];
		if (!cp_is_root(root_res) || !seen[root])
			goto fail;
		if (!im->aliasable[id])
			goto fail;
		if (!cp_check_path(im, root_res, path_len))
			goto fail;

		seen[id] = 1;
		im->aliases[i].id = id;
		im->aliases[i].root = root;
		im->aliases[i].path_off = rec_at + 12;
		im->aliases[i].path_len = path_len;
	}

	if (r->cur != end)
		goto fail;
	free(seen);
	return 1;

fail:
	free(seen);
	return 0;
}

/* ------------------------------------------------------------------ */
/* apply phase: runs only after every section has validated            */
/* ------------------------------------------------------------------ */

static void cp_apply_array(cp_image* im, spvm_member_t node,
	spvm_word count, spvm_word type_id);

static void cp_apply_slot(cp_image* im, spvm_member_t slot, spvm_word type_id)
{
	cp_rd* r = &im->rd;
	spvm_result_t t = cp_type_info(im->state->results, type_id);
	if (cp_is_scalar_type(t)) {
		(void)get_u8(r); /* kind */
		(void)get_u8(r); /* width */
		slot->value.u64 = get_u64(r);
		return;
	}
	cp_apply_array(im, slot->members, slot->member_count, type_id);
}

static void cp_apply_array(cp_image* im, spvm_member_t node,
	spvm_word count, spvm_word type_id)
{
	cp_rd* r = &im->rd;
	spvm_result_t t = cp_type_info(im->state->results, type_id);
	(void)get_u8(r); /* kind */

	if (cp_is_scalar_type(t)) {
		(void)get_u8(r); /* width */
		node[0].value.u64 = get_u64(r);
		return;
	}
	(void)get_u32(r); /* count */
	for (spvm_word i = 0; i < count; i++) {
		spvm_word child_type = t->value_type == spvm_value_type_struct
			? t->params[i] : t->pointer;
		cp_apply_slot(im, &node[i], child_type);
	}
}

static void cp_apply_alias(cp_image* im, cp_alias_rec* a)
{
	cp_rd* r = &im->rd;
	r->cur = a->path_off;

	spvm_result_t root_res = &im->state->results[a->root];
	spvm_result_t alias_res = &im->state->results[a->id];
	spvm_member_t cur = root_res->members;
	spvm_word cur_count = root_res->member_count;
	spvm_member_t target = NULL;

	for (uint32_t k = 0; k < a->path_len; k++) {
		uint32_t idx = get_u32(r);
		if (idx >= (uint32_t)cur_count)
			return; /* validated earlier */
		if (k + 1 == a->path_len) {
			target = &cur[idx];
			break;
		}
		spvm_word next_count = cur[idx].member_count;
		cur = cur[idx].members;
		cur_count = next_count;
	}
	if (target == NULL)
		return;

	/* reproduce spvm_execute_OpAccessChain's exact pointer rule so that
	 * aliases land on the target state's own numeric objects */
	if (target->member_count != 0) {
		alias_res->member_count = target->member_count;
		alias_res->members = target->members;
	}
	else {
		alias_res->member_count = 1;
		alias_res->members = target;
	}
}

static int cp_apply(cp_image* im)
{
	spvm_state_t s = im->state;
	cp_rd* r = &im->rd;

	/* allocate replacement stack storage before touching anything, so an
	 * allocation failure cannot leave the target half-modified */
	spvm_source* new_stack = NULL;
	spvm_result_t* new_info = NULL;
	spvm_word* new_returns = NULL;
	spvm_word* new_cfg = NULL;
	spvm_word* new_parent = NULL;
	spvm_word new_cap = im->terminal ? 10 : im->depth;

	new_stack = (spvm_source*)calloc(new_cap, sizeof(spvm_source));
	new_info = (spvm_result_t*)calloc(new_cap, sizeof(spvm_result_t));
	new_returns = (spvm_word*)calloc(new_cap, sizeof(spvm_word));
	new_cfg = (spvm_word*)calloc(new_cap, sizeof(spvm_word));
	new_parent = (spvm_word*)calloc(new_cap, sizeof(spvm_word));
	if (!new_stack || !new_info || !new_returns || !new_cfg || !new_parent) {
		free(new_stack);
		free(new_info);
		free(new_returns);
		free(new_cfg);
		free(new_parent);
		return 0;
	}

	/* stale aliases of a previously run target are dropped; every alias
	 * named by the blob is rebuilt on the target's own trees below */
	for (uint32_t id = 1; id < im->bound; id++) {
		spvm_result_t q = &s->results[id];
		if (q->type == spvm_result_type_access_chain ||
			q->type == spvm_result_type_function_parameter)
			q->members = NULL;
	}

	for (uint32_t i = 0; i < im->root_count; i++) {
		cp_root_rec* rr = &im->roots[i];
		r->cur = rr->off;
		spvm_result_t res = &s->results[rr->id];
		cp_apply_array(im, res->members, res->member_count, res->pointer);
	}
	for (uint32_t i = 0; i < im->alias_count; i++)
		cp_apply_alias(im, &im->aliases[i]);
	if (!r->ok)
		return 0;

	/* scalar execution scene */
	s->did_jump = im->did_jump;
	s->discarded = im->discarded;
	s->instruction_count = im->inst;
	s->current_line = im->line == CP_NONE ? -1 : (spvm_word)im->line;
	s->current_column = im->column == CP_NONE ? -1 : (spvm_word)im->column;
	s->current_file = im->file == CP_NONE ? NULL : s->results[im->file].name;
	s->return_id = im->return_id == CP_NONE ? -1 : (spvm_word)im->return_id;

	free(s->function_stack);
	free(s->function_stack_info);
	free(s->function_stack_returns);
	free(s->function_stack_cfg);
	free(s->function_stack_cfg_parent);
	s->function_stack = new_stack;
	s->function_stack_info = new_info;
	s->function_stack_returns = new_returns;
	s->function_stack_cfg = new_cfg;
	s->function_stack_cfg_parent = new_parent;
	s->function_stack_count = new_cap;

	if (im->terminal) {
		s->function_stack_current = (spvm_word)-1;
		s->code_current = NULL;
		s->current_function = NULL;
		return 1;
	}

	s->current_parameter = im->current_parameter;
	s->function_stack_current = (spvm_word)(im->depth - 1);

	for (uint32_t i = 0; i < im->depth; i++) {
		cp_frame* f = &im->frames[i];
		s->function_stack[i] = im->prog->code + f->pc;
		s->function_stack_info[i] = &s->results[f->func];
		s->function_stack_returns[i] = f->ret == CP_NONE ? (spvm_word)-1 : (spvm_word)f->ret;
		s->function_stack_cfg[i] = f->cfg;
		s->function_stack_cfg_parent[i] = f->parent;
	}

	s->code_current = im->prog->code + im->pc;
	s->current_function = &s->results[im->current_function];
	return 1;
}

int spvm_checkpoint_restore(spvm_state_t state, const spvm_byte* data, size_t size)
{
	if (state == NULL || data == NULL || size < 16)
		return 0;
	if (!cp_state_supported(state))
		return 0;

	cp_image im;
	memset(&im, 0, sizeof(im));
	im.rd.p = data;
	im.rd.len = size;
	im.rd.cur = 0;
	im.rd.ok = 1;
	im.state = state;
	im.prog = state->owner;

	cp_rd* r = &im.rd;
	int ok = 0;

	if (get_u32(r) != CP_MAGIC || get_u32(r) != CP_VERSION || !r->ok)
		goto out;
	if (!cp_check_module(&im))
		goto out;
	if (!cp_mark_boundaries(&im))
		goto out;
	if (!cp_check_execution(&im))
		goto out;
	if (!cp_check_values(&im))
		goto out;
	if (!expect_tag(r, CP_TAG_END) || r->cur != r->len)
		goto out;

	/* full validation succeeded; only now may the target change */
	if (!cp_apply(&im))
		goto out;
	ok = 1;

out:
	free(im.frames);
	free(im.roots);
	free(im.aliases);
	free(im.boundary);
	free(im.prev_boundary);
	free(im.aliasable);
	return ok;
}
