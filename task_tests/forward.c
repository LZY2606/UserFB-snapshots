/*
 * Tests for the forward-mode differentiation API (<spvm/forward.h>).
 *
 * Every fixture below is a readable C word array that assembles a real SPIR-V
 * module.  The shader is executed by the ordinary VM; derivative values are
 * compared against analytic expressions written out independently here.
 * Nothing calls a host autodiff routine and nothing perturbs inputs to
 * finite-difference them.
 *
 * Registered groups: arithmetic, aliases, control.
 */

#include "../task_support/check.h"

#include <spvm/context.h>
#include <spvm/state.h>
#include <spvm/forward.h>
#include <spvm/ext/GLSL450.h>
#include <spvm/spirv.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GLSL.std.450 instructions used by the fixtures. */
#define GLSL_FAbs 4
#define GLSL_Sin 13
#define GLSL_Cos 14
#define GLSL_Sqrt 31
#define GLSL_Length 66
#define GLSL_Normalize 69

/* ------------------------------------------------------------------ */
/* Word-buffer assembler                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	spvm_word v[24000];
	int n;
} spv_buf;

static void bw(spv_buf* b, spvm_word w)
{
	if (b->n < (int)(sizeof(b->v) / sizeof(b->v[0])))
		b->v[b->n++] = w;
}

static void ins(spv_buf* b, SpvOp op, int count, const spvm_word* ops)
{
	bw(b, (spvm_word)(((count + 1) << 16) | (spvm_word) op));
	for (int i = 0; i < count; i++)
		bw(b, ops[i]);
}

static void i0(spv_buf* b, SpvOp op) { ins(b, op, 0, NULL); }
static void i1(spv_buf* b, SpvOp op, spvm_word a) { spvm_word o[1] = { a }; ins(b, op, 1, o); }
static void i2(spv_buf* b, SpvOp op, spvm_word a, spvm_word c) { spvm_word o[2] = { a, c }; ins(b, op, 2, o); }
static void i3(spv_buf* b, SpvOp op, spvm_word a, spvm_word c, spvm_word d) { spvm_word o[3] = { a, c, d }; ins(b, op, 3, o); }
static void i4(spv_buf* b, SpvOp op, spvm_word a, spvm_word c, spvm_word d, spvm_word e) { spvm_word o[4] = { a, c, d, e }; ins(b, op, 4, o); }
static void i5(spv_buf* b, SpvOp op, spvm_word a, spvm_word c, spvm_word d, spvm_word e, spvm_word f) { spvm_word o[5] = { a, c, d, e, f }; ins(b, op, 5, o); }
static void i6(spv_buf* b, SpvOp op, spvm_word a, spvm_word c, spvm_word d, spvm_word e, spvm_word f, spvm_word g) { spvm_word o[6] = { a, c, d, e, f, g }; ins(b, op, 6, o); }
/* TypeStruct takes a variable number of member types. */
static void b_struct(spv_buf* b, spvm_word id, int count, const spvm_word* members)
{
	bw(b, (spvm_word)(((count + 2) << 16) | SpvOpTypeStruct));
	bw(b, id);
	for (int i = 0; i < count; i++)
		bw(b, members[i]);
}

static void b_string(spv_buf* b, const char* s)
{
	size_t len = strlen(s) + 1;
	size_t words = (len + 3) / 4;
	for (size_t i = 0; i < words; i++) {
		unsigned w = 0;
		for (size_t j = 0; j < 4; j++) {
			size_t k = i * 4 + j;
			unsigned char c = (k < strlen(s)) ? (unsigned char) s[k] : 0;
			w |= (unsigned) c << (8 * j);
		}
		bw(b, (spvm_word) w);
	}
}

static void b_name(spv_buf* b, spvm_word id, const char* name)
{
	spvm_word words = (spvm_word)((strlen(name) + 4) / 4);
	bw(b, (spvm_word)(((2 + words) << 16) | SpvOpName));
	bw(b, id);
	b_string(b, name);
}

static void b_member_name(spv_buf* b, spvm_word type, spvm_word index, const char* name)
{
	spvm_word words = (spvm_word)((strlen(name) + 4) / 4);
	bw(b, (spvm_word)(((3 + words) << 16) | SpvOpMemberName));
	bw(b, type);
	bw(b, index);
	b_string(b, name);
}

static void b_ext_import(spv_buf* b, spvm_word id, const char* name)
{
	spvm_word words = (spvm_word)((strlen(name) + 4) / 4);
	bw(b, (spvm_word)(((2 + words) << 16) | SpvOpExtInstImport));
	bw(b, id);
	b_string(b, name);
}

static void b_float(spv_buf* b, spvm_word type, spvm_word id, float value)
{
	unsigned bits;
	memcpy(&bits, &value, sizeof(bits));
	i3(b, SpvOpConstant, type, id, (spvm_word) bits);
}

static void b_int(spv_buf* b, spvm_word type, spvm_word id, int value)
{
	i3(b, SpvOpConstant, type, id, (spvm_word) value);
}

static void b_module_begin(spv_buf* b, unsigned bound)
{
	bw(b, (spvm_word) SpvMagicNumber);
	bw(b, 0x00010000);
	bw(b, 0);
	bw(b, (spvm_word) bound);
	bw(b, 0);
}

/* ------------------------------------------------------------------ */
/* Fixture lifecycle                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
	spvm_context_t ctx;
	spvm_program_t prog;
	spvm_state_t state;
	spvm_forward_t fwd;
	spvm_ext_opcode_func* ext;
} fixture;

static int fixture_build(fixture* f, const spv_buf* b, int with_forward, int directions)
{
	memset(f, 0, sizeof(*f));
	f->ctx = spvm_context_initialize();
	if (!f->ctx) return 0;
	f->prog = spvm_program_create(f->ctx, b->v, (size_t) b->n);
	if (!f->prog) return 0;
	f->state = spvm_state_create(f->prog);
	if (!f->state) return 0;
	f->ext = spvm_build_glsl450_ext();
	spvm_state_set_extension(f->state, "GLSL.std.450", f->ext);
	if (with_forward) {
		f->fwd = spvm_forward_create(f->state, directions);
		if (!f->fwd) return 0;
	}
	return 1;
}

static void fixture_destroy(fixture* f)
{
	if (f->fwd) spvm_forward_delete(f->fwd);
	if (f->state) spvm_state_delete(f->state);
	if (f->prog) spvm_program_delete(f->prog);
	if (f->ext) free(f->ext);
	if (f->ctx) spvm_context_deinitialize(f->ctx);
	memset(f, 0, sizeof(*f));
}

static spvm_word id_of(spvm_state_t state, const char* name)
{
	return spvm_state_get_result_location(state, (spvm_string) name);
}

static void run_main(spvm_state_t state)
{
	spvm_state_prepare(state, id_of(state, "main"));
	spvm_state_call_function(state);
}

static void set_vec(spvm_state_t state, spvm_word id, const float* data)
{
	spvm_result_t r = &state->results[id];
	spvm_member_set_value_f(r->members, (size_t) r->member_count, (float*) data);
}

static void set_scalar(spvm_state_t state, spvm_word id, float v)
{
	set_vec(state, id, &v);
}

static float scalar_at(spvm_state_t state, spvm_word id)
{
	return state->results[id].members[0].value.f;
}

static int nearf(float a, float b, float eps)
{
	return fabsf(a - b) <= eps * (1.0f + fabsf(b));
}

static int seed_comp(spvm_forward_t f, spvm_word id, spvm_word idx, spvm_word dir, float v)
{
	return spvm_forward_seed(f, id, &idx, 1, dir, v);
}
static int get_comp(spvm_forward_t f, spvm_word id, spvm_word idx, spvm_word dir, float* out)
{
	return spvm_forward_get(f, id, &idx, 1, dir, out);
}

/* ================================================================== */
/* Group: arithmetic                                                  */
/* ================================================================== */

/* ---- d = dot(normalize(a), b) ---- */

static void build_normdot(spv_buf* b)
{
	enum {
		t_void = 1, t_float, t_v2, p_in_v2, p_out_f,
		v_a, v_b, v_out, t_fn, e_glsl, k_one,
		fn_main, lbl, v_la, v_norm, v_lb, v_d, bound
	};

	b_module_begin(b, bound);
	i1(b, SpvOpCapability, SpvCapabilityShader);
	i2(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	i1(b, SpvOpTypeVoid, t_void);
	i2(b, SpvOpTypeFloat, t_float, 32);
	i3(b, SpvOpTypeVector, t_v2, t_float, 2);
	i3(b, SpvOpTypePointer, p_in_v2, SpvStorageClassInput, t_v2);
	i3(b, SpvOpTypePointer, p_out_f, SpvStorageClassOutput, t_float);
	b_float(b, t_float, k_one, 1.0f);       /* a real OpConstant, for rejection */
	b_name(b, v_a, "a");
	b_name(b, v_b, "b");
	b_name(b, v_out, "out");
	i3(b, SpvOpVariable, p_in_v2, v_a, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_in_v2, v_b, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_out_f, v_out, SpvStorageClassOutput);
	i2(b, SpvOpTypeFunction, t_fn, t_void);
	b_ext_import(b, e_glsl, "GLSL.std.450");
	b_name(b, fn_main, "main");

	i4(b, SpvOpFunction, t_void, fn_main, 0, t_fn);
	i1(b, SpvOpLabel, lbl);
	i3(b, SpvOpLoad, t_v2, v_la, v_a);
	i5(b, SpvOpExtInst, t_v2, v_norm, e_glsl, GLSL_Normalize, v_la);
	i3(b, SpvOpLoad, t_v2, v_lb, v_b);
	i4(b, SpvOpDot, t_float, v_d, v_norm, v_lb);
	i2(b, SpvOpStore, v_out, v_d);
	i0(b, SpvOpReturn);
	i0(b, SpvOpFunctionEnd);
}

/* Analytic Jacobian of dot(normalize(a), b) wrt a is b^T (I - y y^T)/|a|. */
static void normdot_expect(const float* a, const float* b, float* j0, float* j1)
{
	float len = sqrtf(a[0] * a[0] + a[1] * a[1]);
	float y0 = a[0] / len, y1 = a[1] / len;
	*j0 = b[0] * (1.0f - y0 * y0) / len + b[1] * (-y1 * y0) / len;
	*j1 = b[0] * (-y0 * y1) / len + b[1] * (1.0f - y1 * y1) / len;
}

static int case_arith_normdot(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_normdot(&b);

	const float a_in[2] = { 3.0f, 4.0f };
	const float b_in[2] = { 1.0f, 0.0f };

	fixture plain;
	TASK_CHECK(fixture_build(&plain, &b, 0, 0));
	set_vec(plain.state, id_of(plain.state, "a"), a_in);
	set_vec(plain.state, id_of(plain.state, "b"), b_in);
	run_main(plain.state);
	float primal_plain = scalar_at(plain.state, id_of(plain.state, "out"));
	TASK_CHECK(nearf(primal_plain, 0.6f, 1e-6f));
	fixture_destroy(&plain);

	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 1, 2));
	TASK_CHECK(spvm_forward_direction_count(f.fwd) == 2);
	spvm_word va = id_of(f.state, "a");
	spvm_word vb = id_of(f.state, "b");
	spvm_word vout = id_of(f.state, "out");
	set_vec(f.state, va, a_in);
	set_vec(f.state, vb, b_in);
	spvm_state_prepare(f.state, id_of(f.state, "main"));

	TASK_CHECK(seed_comp(f.fwd, va, 0, 0, 1.0f) == 0);
	TASK_CHECK(seed_comp(f.fwd, va, 1, 1, 1.0f) == 0);

	spvm_state_call_function(f.state);

	float d0 = -1, d1 = -1;
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 0, &d0) == 0);
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 1, &d1) == 0);

	float e0, e1;
	normdot_expect(a_in, b_in, &e0, &e1);
	TASK_CHECK(nearf(d0, e0, 1e-5f));
	TASK_CHECK(nearf(d1, e1, 1e-5f));
	TASK_CHECK(nearf(d0, 0.128f, 1e-5f));
	TASK_CHECK(nearf(d1, -0.096f, 1e-5f));
	TASK_CHECK(nearf(scalar_at(f.state, vout), primal_plain, 1e-6f));

	fixture_destroy(&f);
	return 0;
}

/* ---- composite / vector / copy instructions ---- */

/*
 * Inputs: vec2 a, vec2 b, float k.  Output float.
 *   c  = shuffle(a, b, [0,3])        = (a.x, b.y)
 *   c2 = insert_dynamic(c, a.x, 1)   = (a.x, a.x)
 *   c3 = extract_dynamic(c2, 0), c4 = extract_dynamic(c2, 1)
 *   s  = c3 + c4                     = 2 a.x
 *   vs = c2 * k
 *   d  = dot(vs, c2)                 = 2 a.x^2 k
 *   t  = s + d
 *   comp = construct(t, s)
 *   ins  = composite_insert(comp, ex, 0) with ex = extract(comp,1) = s
 *   co = copy_object(t)
 *   var1 = t ; copy_memory(var2, var1) ; loaded = var2
 *   out = ex + co + loaded + extract(ins,0)
 * With a=(2,3), b=(3,1), k=4: t=36, s=4, out = 4+36+36+4 = 80.
 * d(out)/d(a.x) = 2 + 34 + 34 + 2 = 72; d(out)/d(k) = 0 + 8 + 8 + 0 = 16.
 */
static void build_composite_ops(spv_buf* b)
{
	enum {
		t_void = 1, t_float, t_int, t_v2,
		p_in_v2, p_in_f, p_out_f, p_fn_f,
		v_a, v_b, v_k, v_out, var1, var2, t_fn,
		fn_main, lbl,
		idx0, idx1,
		la, lb, lk, a0, c, c2, c3, c4, s, vs, d, t,
		comp, ex, ins, ex2, co, loaded, sum1, sum2, bound
	};

	b_module_begin(b, bound);
	i1(b, SpvOpCapability, SpvCapabilityShader);
	i2(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	i1(b, SpvOpTypeVoid, t_void);
	i2(b, SpvOpTypeFloat, t_float, 32);
	i3(b, SpvOpTypeInt, t_int, 32, 1);
	i3(b, SpvOpTypeVector, t_v2, t_float, 2);
	i3(b, SpvOpTypePointer, p_in_v2, SpvStorageClassInput, t_v2);
	i3(b, SpvOpTypePointer, p_in_f, SpvStorageClassInput, t_float);
	i3(b, SpvOpTypePointer, p_out_f, SpvStorageClassOutput, t_float);
	i3(b, SpvOpTypePointer, p_fn_f, SpvStorageClassFunction, t_float);
	b_int(b, t_int, idx0, 0);
	b_int(b, t_int, idx1, 1);
	b_name(b, v_a, "a");
	b_name(b, v_b, "b");
	b_name(b, v_k, "k");
	b_name(b, v_out, "out");
	i3(b, SpvOpVariable, p_in_v2, v_a, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_in_v2, v_b, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_in_f, v_k, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_out_f, v_out, SpvStorageClassOutput);
	i2(b, SpvOpTypeFunction, t_fn, t_void);
	b_name(b, fn_main, "main");

	i4(b, SpvOpFunction, t_void, fn_main, 0, t_fn);
	i1(b, SpvOpLabel, lbl);
	i3(b, SpvOpVariable, p_fn_f, var1, SpvStorageClassFunction);
	i3(b, SpvOpVariable, p_fn_f, var2, SpvStorageClassFunction);

	i3(b, SpvOpLoad, t_v2, la, v_a);
	i3(b, SpvOpLoad, t_v2, lb, v_b);
	i3(b, SpvOpLoad, t_float, lk, v_k);

	i4(b, SpvOpCompositeExtract, t_float, a0, la, 0);
	i6(b, SpvOpVectorShuffle, t_v2, c, la, lb, 0, 3);
	i5(b, SpvOpVectorInsertDynamic, t_v2, c2, c, a0, idx1);
	i4(b, SpvOpVectorExtractDynamic, t_float, c3, c2, idx0);
	i4(b, SpvOpVectorExtractDynamic, t_float, c4, c2, idx1);
	i4(b, SpvOpFAdd, t_float, s, c3, c4);
	i4(b, SpvOpVectorTimesScalar, t_v2, vs, c2, lk);
	i4(b, SpvOpDot, t_float, d, vs, c2);
	i4(b, SpvOpFAdd, t_float, t, s, d);

	i4(b, SpvOpCompositeConstruct, t_v2, comp, t, s);
	i4(b, SpvOpCompositeExtract, t_float, ex, comp, 1);
	i5(b, SpvOpCompositeInsert, t_v2, ins, ex, comp, 0);
	i4(b, SpvOpCompositeExtract, t_float, ex2, ins, 0);
	i3(b, SpvOpCopyObject, t_float, co, t);

	i2(b, SpvOpStore, var1, t);
	i2(b, SpvOpCopyMemory, var2, var1);
	i3(b, SpvOpLoad, t_float, loaded, var2);

	i4(b, SpvOpFAdd, t_float, sum1, ex, co);
	i4(b, SpvOpFAdd, t_float, sum1, sum1, loaded);
	i4(b, SpvOpFAdd, t_float, sum2, sum1, ex2);
	i2(b, SpvOpStore, v_out, sum2);
	i0(b, SpvOpReturn);
	i0(b, SpvOpFunctionEnd);
}

static int case_arith_composite_ops(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_composite_ops(&b);

	const float a_in[2] = { 2.0f, 3.0f };
	const float b_in[2] = { 3.0f, 1.0f };
	const float k_in = 4.0f;

	fixture plain;
	TASK_CHECK(fixture_build(&plain, &b, 0, 0));
	set_vec(plain.state, id_of(plain.state, "a"), a_in);
	set_vec(plain.state, id_of(plain.state, "b"), b_in);
	set_scalar(plain.state, id_of(plain.state, "k"), k_in);
	run_main(plain.state);
	float primal_plain = scalar_at(plain.state, id_of(plain.state, "out"));
	TASK_CHECK(nearf(primal_plain, 80.0f, 1e-5f));
	fixture_destroy(&plain);

	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 1, 2));
	spvm_word va = id_of(f.state, "a");
	spvm_word vb = id_of(f.state, "b");
	spvm_word vk = id_of(f.state, "k");
	spvm_word vout = id_of(f.state, "out");
	set_vec(f.state, va, a_in);
	set_vec(f.state, vb, b_in);
	set_scalar(f.state, vk, k_in);
	spvm_state_prepare(f.state, id_of(f.state, "main"));

	TASK_CHECK(seed_comp(f.fwd, va, 0, 0, 1.0f) == 0);
	TASK_CHECK(spvm_forward_seed(f.fwd, vk, NULL, 0, 1, 1.0f) == 0);

	spvm_state_call_function(f.state);
	TASK_CHECK(nearf(scalar_at(f.state, vout), primal_plain, 1e-5f));

	float d0 = 0, d1 = 0;
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 0, &d0) == 0);
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 1, &d1) == 0);
	TASK_CHECK(nearf(d0, 72.0f, 1e-4f));
	TASK_CHECK(nearf(d1, 16.0f, 1e-4f));

	fixture_destroy(&f);
	return 0;
}

/* ---- API contract ---- */

static int case_arith_api_contract(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_normdot(&b);

	const float a_in[2] = { 3.0f, 4.0f };
	const float b_in[2] = { 1.0f, 0.0f };

	/* Direction count must be 1..4. */
	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 0, 0));
	TASK_CHECK(spvm_forward_create(f.state, 0) == NULL);
	TASK_CHECK(spvm_forward_create(f.state, 5) == NULL);
	TASK_CHECK(spvm_forward_create(f.state, -1) == NULL);

	spvm_forward_t fwd = spvm_forward_create(f.state, 2);
	TASK_CHECK(fwd != NULL);
	/* Only one forward context per state. */
	TASK_CHECK(spvm_forward_create(f.state, 1) == NULL);
	TASK_CHECK(spvm_forward_get_context(f.state) == fwd);

	spvm_word va = id_of(f.state, "a");
	spvm_word vb = id_of(f.state, "b");
	spvm_word vout = id_of(f.state, "out");
	set_vec(f.state, va, a_in);
	set_vec(f.state, vb, b_in);

	/* Add a named constant id in the module so constant seeding can be
	 * rejected.  build_normdot does not have one, so a type result stands in:
	 * seeding a non-variable result must fail. */
	spvm_word type_id = f.state->results[va].pointer; /* pointer type: not a variable */

	/* Out-of-range direction. */
	spvm_word idx0 = 0;
	TASK_CHECK(spvm_forward_seed(fwd, va, &idx0, 1, 2, 1.0f) != 0);
	TASK_CHECK(spvm_forward_seed(fwd, va, &idx0, 1, -1, 1.0f) != 0);

	/* A valid seed, then failing writes must not disturb it. */
	TASK_CHECK(spvm_forward_seed(fwd, va, &idx0, 1, 0, 5.0f) == 0);
	TASK_CHECK(spvm_forward_seed(fwd, va, &idx0, 1, 0, 9.0f) == 0); /* overwrite allowed */
	float chk = 0;
	TASK_CHECK(spvm_forward_get(fwd, va, &idx0, 1, 0, &chk) == 0);
	TASK_CHECK(nearf(chk, 9.0f, 0.0f));

	/* A real OpConstant (id 10 in build_normdot) must be rejected as a seed. */
	TASK_CHECK(spvm_forward_seed(fwd, 10, NULL, 0, 0, 1.0f) != 0);

	/* Invalid path (index out of range) and seeding a non-variable. */
	spvm_word bad = 7;
	TASK_CHECK(spvm_forward_seed(fwd, va, &bad, 1, 0, 1.0f) != 0);
	TASK_CHECK(spvm_forward_seed(fwd, type_id, NULL, 0, 0, 1.0f) != 0);
	TASK_CHECK(spvm_forward_get(fwd, va, &bad, 1, 0, &chk) != 0);
	TASK_CHECK(spvm_forward_get(fwd, vout, NULL, 0, 5, &chk) != 0);
	/* Failed writes left the previous tangent intact. */
	TASK_CHECK(spvm_forward_get(fwd, va, &idx0, 1, 0, &chk) == 0);
	TASK_CHECK(nearf(chk, 9.0f, 0.0f));

	/* Run once. */
	spvm_state_prepare(f.state, id_of(f.state, "main"));
	TASK_CHECK(spvm_forward_seed(fwd, va, &idx0, 1, 0, 1.0f) == 0);
	spvm_state_call_function(f.state);
	float d0 = 0;
	TASK_CHECK(spvm_forward_get(fwd, vout, NULL, 0, 0, &d0) == 0);
	TASK_CHECK(nearf(d0, 0.128f, 1e-5f));

	/* clear() removes derivatives but not numeric state / PC / results. */
	float numeric_before = scalar_at(f.state, vout);
	spvm_source pc_before = f.state->code_current;
	spvm_word instr_before = f.state->instruction_count;
	spvm_forward_clear(fwd);
	TASK_CHECK(f.state->code_current == pc_before);
	TASK_CHECK(f.state->instruction_count == instr_before);
	TASK_CHECK(nearf(scalar_at(f.state, vout), numeric_before, 0.0f));
	TASK_CHECK(spvm_forward_get(fwd, vout, NULL, 0, 0, &chk) == 0);
	TASK_CHECK(chk == 0.0f);

	/* prepare() clears all tangents; the previous seed must not survive. */
	spvm_state_prepare(f.state, id_of(f.state, "main"));
	float after_prepare = 123;
	TASK_CHECK(spvm_forward_get(fwd, va, &idx0, 1, 0, &after_prepare) == 0);
	TASK_CHECK(after_prepare == 0.0f);
	TASK_CHECK(spvm_forward_error(fwd, NULL) == 0);

	/* Re-seed and re-run: same derivative as before. */
	TASK_CHECK(spvm_forward_seed(fwd, va, &idx0, 1, 0, 1.0f) == 0);
	spvm_state_call_function(f.state);
	float d2 = 0;
	TASK_CHECK(spvm_forward_get(fwd, vout, NULL, 0, 0, &d2) == 0);
	TASK_CHECK(nearf(d2, 0.128f, 1e-5f));
	TASK_CHECK(nearf(scalar_at(f.state, vout), numeric_before, 1e-6f));

	/* Deleting the forward context must not invalidate the state. */
	spvm_forward_delete(fwd);
	TASK_CHECK(spvm_forward_get_context(f.state) == NULL);
	run_main(f.state);
	TASK_CHECK(nearf(scalar_at(f.state, vout), numeric_before, 1e-6f));

	fixture_destroy(&f);
	return 0;
}

/* ---- the shipped example shader: time -> sin -> color ---- */

static spvm_word* load_spv(const char* path, size_t* words)
{
	FILE* fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0 || size % 4 != 0) {
		fclose(fp);
		return NULL;
	}
	spvm_word* data = (spvm_word*) malloc((size_t) size);
	if (!data) {
		fclose(fp);
		return NULL;
	}
	if (fread(data, 1, (size_t) size, fp) != (size_t) size) {
		free(data);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	*words = (size_t) size / sizeof(spvm_word);
	return data;
}

static int case_arith_example_shader(void)
{
	size_t word_count = 0;
	spvm_word* code = load_spv("example/shader.spv", &word_count);
	TASK_CHECK(code != NULL);

	spvm_context_t ctx = spvm_context_initialize();
	TASK_CHECK(ctx != NULL);
	spvm_program_t prog = spvm_program_create(ctx, code, word_count);
	TASK_CHECK(prog != NULL);
	spvm_state_t state = spvm_state_create(prog);
	TASK_CHECK(state != NULL);
	spvm_ext_opcode_func* ext = spvm_build_glsl450_ext();
	spvm_state_set_extension(state, "GLSL.std.450", ext);

	spvm_word ublock = id_of(state, "uBlock");
	TASK_CHECK(ublock != 0);
	/* uBlock.time = 0.5 */
	spvm_result_t u = &state->results[ublock];
	u->members[1].value.f = 0.5f;
	u->members[0].members[0].value.f = 0.2f;
	u->members[0].members[1].value.f = 0.8f;

	spvm_forward_t fwd = spvm_forward_create(state, 1);
	TASK_CHECK(fwd != NULL);
	spvm_state_prepare(state, id_of(state, "main"));

	spvm_word path_time = 1;
	TASK_CHECK(spvm_forward_seed(fwd, ublock, &path_time, 1, 0, 1.0f) == 0);
	spvm_state_call_function(state);

	float d = -1;
	spvm_word zc = 2;
	TASK_CHECK(get_comp(fwd, id_of(state, "outColor"), zc, 0, &d) == 0);
	/* outColor.z = |sin(time)|, sin(0.5) > 0 so the tangent is cos(0.5). */
	TASK_CHECK(nearf(d, cosf(0.5f), 1e-5f));
	TASK_CHECK(fabsf(d - cosf(0.5f)) < 1e-5f);

	/* The adaptive info lanes are independent of time. */
	spvm_word idx0 = 0, idx1 = 1;
	TASK_CHECK(get_comp(fwd, id_of(state, "outColor"), idx0, 0, &d) == 0);
	TASK_CHECK(d == 0.0f);
	TASK_CHECK(get_comp(fwd, id_of(state, "outColor"), idx1, 0, &d) == 0);
	TASK_CHECK(d == 0.0f);

	spvm_forward_delete(fwd);
	spvm_state_delete(state);
	spvm_program_delete(prog);
	spvm_context_deinitialize(ctx);
	free(ext);
	free(code);
	return 0;
}

/* ================================================================== */
/* Group: aliases                                                     */
/* ================================================================== */

/*
 * Private struct S { vec2 v; float s; } g;
 * Output float out.
 *
 *   inner(float* p, float* q):
 *       a = load p;  store q, a*2;  b = load q;  return b*3
 *   half(float x): return x*0.5
 *   outer(p, q):   return half(inner(p, q))
 *
 * main:
 *   v_old = load g.v;  x_old = extract v_old[1]    (copy taken before writes)
 *   p = &g.v[1]; q = &g.v[1]                        (two pointers, same member)
 *   r = outer(p, q)
 *   v_new = load g.v;  y_new = extract v_new[1]     (fresh copy after writes)
 *   out = x_old + y_new + r
 *
 * With g.v = (0.5, 0.25), seeding d/d g.v[1]:
 *   inner: a'=1, store q'=2, b'=2, return 3b -> 6
 *   half : 3
 *   x_old: 1   (value copy, NOT affected by the aliased store)
 *   y_new: 2   (fresh load sees the store through q)
 *   out' : 1 + 2 + 3 = 6
 * Primal: inner = 1.5, half = 0.75, x_old = .25, y_new = .5, out = 1.5.
 */
static void build_alias_module(spv_buf* b)
{
	enum {
		t_void = 1, t_float, t_int, t_v2, t_S,
		p_S, p_v2, p_f, p_out_f,
		g, v_out,
		t_fn_void, t_fn_pp, t_fn_f,
		fn_main, fn_inner, fn_outer, fn_half,
		lbl_main, lbl_inner, lbl_outer, lbl_half,
		ip, iq, o1, o2, hx,
		i_zero, i_one, f_two, f_three, f_half,
		vg, v_old, x_old, vg2, v_new, y_new,
		p_ip, p_iq, r_outer, r_in, r_half,
		in_a, in_m, in_b, in_r,
		sum1, sum2, bound
	};

	b_module_begin(b, bound);
	i1(b, SpvOpCapability, SpvCapabilityShader);
	i2(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	i1(b, SpvOpTypeVoid, t_void);
	i2(b, SpvOpTypeFloat, t_float, 32);
	i3(b, SpvOpTypeInt, t_int, 32, 1);
	i3(b, SpvOpTypeVector, t_v2, t_float, 2);
	{
		spvm_word members[2] = { t_v2, t_float };
		b_struct(b, t_S, 2, members);
	}
	i3(b, SpvOpTypePointer, p_S, SpvStorageClassPrivate, t_S);
	i3(b, SpvOpTypePointer, p_v2, SpvStorageClassPrivate, t_v2);
	i3(b, SpvOpTypePointer, p_f, SpvStorageClassPrivate, t_float);
	i3(b, SpvOpTypePointer, p_out_f, SpvStorageClassOutput, t_float);
	b_member_name(b, t_S, 0, "v");
	b_member_name(b, t_S, 1, "s");

	b_name(b, g, "g");
	b_name(b, v_out, "out");
	i3(b, SpvOpVariable, p_S, g, SpvStorageClassPrivate);
	i3(b, SpvOpVariable, p_out_f, v_out, SpvStorageClassOutput);

	i2(b, SpvOpTypeFunction, t_fn_void, t_void);
	i4(b, SpvOpTypeFunction, t_fn_pp, t_float, p_f, p_f);
	i3(b, SpvOpTypeFunction, t_fn_f, t_float, t_float);

	b_int(b, t_int, i_zero, 0);
	b_int(b, t_int, i_one, 1);
	b_float(b, t_float, f_two, 2.0f);
	b_float(b, t_float, f_three, 3.0f);
	b_float(b, t_float, f_half, 0.5f);

	b_name(b, fn_main, "main");

	/* ---- inner(float* p, float* q) ---- */
	i4(b, SpvOpFunction, t_float, fn_inner, 0, t_fn_pp);
	i2(b, SpvOpFunctionParameter, p_f, ip);
	i2(b, SpvOpFunctionParameter, p_f, iq);
	i1(b, SpvOpLabel, lbl_inner);
	i3(b, SpvOpLoad, t_float, in_a, ip);
	i4(b, SpvOpFMul, t_float, in_m, in_a, f_two);
	i2(b, SpvOpStore, iq, in_m);
	/* Read back through the *other* pointer: p and q alias the same member. */
	i3(b, SpvOpLoad, t_float, in_b, ip);
	i4(b, SpvOpFMul, t_float, in_r, in_b, f_three);
	i1(b, SpvOpReturnValue, in_r);
	i0(b, SpvOpFunctionEnd);

	/* ---- half(float) ---- */
	i4(b, SpvOpFunction, t_float, fn_half, 0, t_fn_f);
	i2(b, SpvOpFunctionParameter, t_float, hx);
	i1(b, SpvOpLabel, lbl_half);
	i4(b, SpvOpFMul, t_float, r_half, hx, f_half);
	i1(b, SpvOpReturnValue, r_half);
	i0(b, SpvOpFunctionEnd);

	/* ---- outer(float*, float*) : half(inner(p, q)) ---- */
	i4(b, SpvOpFunction, t_float, fn_outer, 0, t_fn_pp);
	i2(b, SpvOpFunctionParameter, p_f, o1);
	i2(b, SpvOpFunctionParameter, p_f, o2);
	i1(b, SpvOpLabel, lbl_outer);
	i5(b, SpvOpFunctionCall, t_float, r_in, fn_inner, o1, o2);
	i4(b, SpvOpFunctionCall, t_float, r_outer, fn_half, r_in);
	i1(b, SpvOpReturnValue, r_outer);
	i0(b, SpvOpFunctionEnd);

	/* ---- main ---- */
	i4(b, SpvOpFunction, t_void, fn_main, 0, t_fn_void);
	i1(b, SpvOpLabel, lbl_main);
	i4(b, SpvOpAccessChain, p_v2, vg, g, i_zero);          /* &g.v */
	i3(b, SpvOpLoad, t_v2, v_old, vg);                     /* copy before writes */
	i4(b, SpvOpCompositeExtract, t_float, x_old, v_old, 1);
	i5(b, SpvOpAccessChain, p_f, p_ip, g, i_zero, i_one);  /* &g.v[1] */
	i5(b, SpvOpAccessChain, p_f, p_iq, g, i_zero, i_one);  /* &g.v[1] again */
	i5(b, SpvOpFunctionCall, t_float, r_outer, fn_outer, p_ip, p_iq);
	i4(b, SpvOpAccessChain, p_v2, vg2, g, i_zero);
	i3(b, SpvOpLoad, t_v2, v_new, vg2);
	i4(b, SpvOpCompositeExtract, t_float, y_new, v_new, 1);
	i4(b, SpvOpFAdd, t_float, sum1, x_old, y_new);
	i4(b, SpvOpFAdd, t_float, sum2, sum1, r_outer);
	i2(b, SpvOpStore, v_out, sum2);
	i0(b, SpvOpReturn);
	i0(b, SpvOpFunctionEnd);
}

static int case_alias_helpers(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_alias_module(&b);

	const float gv[2] = { 0.5f, 0.25f };

	fixture plain;
	TASK_CHECK(fixture_build(&plain, &b, 0, 0));
	spvm_word pg = id_of(plain.state, "g");
	/* g.v */
	spvm_member_set_value_f(plain.state->results[pg].members[0].members, 2, (float*) gv);
	run_main(plain.state);
	float primal_plain = scalar_at(plain.state, id_of(plain.state, "out"));
	TASK_CHECK(nearf(primal_plain, 1.5f, 1e-5f));
	fixture_destroy(&plain);

	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 1, 2));
	spvm_word g = id_of(f.state, "g");
	spvm_member_set_value_f(f.state->results[g].members[0].members, 2, (float*) gv);
	spvm_state_prepare(f.state, id_of(f.state, "main"));

	/* Seed d/d g.v[1] in direction 0, d/d g.v[0] in direction 1. */
	spvm_word vpath0[2] = { 0, 0 };
	spvm_word vpath1[2] = { 0, 1 };
	TASK_CHECK(spvm_forward_seed(f.fwd, g, vpath1, 2, 0, 1.0f) == 0);
	TASK_CHECK(spvm_forward_seed(f.fwd, g, vpath0, 2, 1, 1.0f) == 0);

	spvm_state_call_function(f.state);

	float out_plain = scalar_at(f.state, id_of(f.state, "out"));
	TASK_CHECK(nearf(out_plain, primal_plain, 1e-5f));

	float d0 = 0, d1 = 0;
	TASK_CHECK(spvm_forward_get(f.fwd, id_of(f.state, "out"), NULL, 0, 0, &d0) == 0);
	TASK_CHECK(spvm_forward_get(f.fwd, id_of(f.state, "out"), NULL, 0, 1, &d1) == 0);
	/* y is untouched by the helper; only g.v[1] participates. */
	TASK_CHECK(nearf(d0, 6.0f, 1e-5f));
	TASK_CHECK(d1 == 0.0f);

	fixture_destroy(&f);
	return 0;
}

/* ================================================================== */
/* Group: control                                                     */
/* ================================================================== */

/*
 * Finite loop with a loop-carried Phi.
 *
 *   Input float b;  Output float out.
 *   sum = 0; i = 0;
 *   while (i < 3) { sum = sum + b; i = i + 1; }
 *   out = sqrt(sum);
 *
 * With b = 2: sum = 6, out = sqrt(6).
 * d(out)/db = 3 / (2*sqrt(6)).
 *
 * The branch condition depends only on the integer loop counter, so the
 * numeric control flow is identical for all tangent directions.
 */
static void build_loop_module(spv_buf* b)
{
	enum {
		t_void = 1, t_float, t_int, t_bool,
		p_in_f, p_out_f,
		v_b, v_out, t_fn, e_glsl,
		fn_main, lbl_entry, lbl_head, lbl_body, lbl_exit,
		i_zero, i_one, i_three,
		b_val, phi_sum, phi_i, next_sum, next_i,
		cond, f_sum, f_sqrt,
		bound
	};

	b_module_begin(b, bound);
	i1(b, SpvOpCapability, SpvCapabilityShader);
	i2(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	i1(b, SpvOpTypeVoid, t_void);
	i2(b, SpvOpTypeFloat, t_float, 32);
	i3(b, SpvOpTypeInt, t_int, 32, 1);
	i1(b, SpvOpTypeBool, t_bool);
	i3(b, SpvOpTypePointer, p_in_f, SpvStorageClassInput, t_float);
	i3(b, SpvOpTypePointer, p_out_f, SpvStorageClassOutput, t_float);
	b_name(b, v_b, "b");
	b_name(b, v_out, "out");
	i3(b, SpvOpVariable, p_in_f, v_b, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_out_f, v_out, SpvStorageClassOutput);
	i2(b, SpvOpTypeFunction, t_fn, t_void);
	b_ext_import(b, e_glsl, "GLSL.std.450");
	b_name(b, fn_main, "main");

	b_int(b, t_int, i_zero, 0);
	b_int(b, t_int, i_one, 1);
	b_int(b, t_int, i_three, 3);
	b_float(b, t_float, f_sum, 0.0f);

	i4(b, SpvOpFunction, t_void, fn_main, 0, t_fn);
	i1(b, SpvOpLabel, lbl_entry);
	i3(b, SpvOpLoad, t_float, b_val, v_b);
	/* entry -> head */
	{
		spvm_word ops[1] = { lbl_head };
		ins(b, SpvOpBranch, 1, ops);
	}

	i1(b, SpvOpLabel, lbl_head);
	{
		spvm_word ops[6] = { t_float, phi_sum, f_sum, lbl_entry, next_sum, lbl_body };
		ins(b, SpvOpPhi, 6, ops);
	}
	{
		spvm_word ops[6] = { t_int, phi_i, i_zero, lbl_entry, next_i, lbl_body };
		ins(b, SpvOpPhi, 6, ops);
	}
	i4(b, SpvOpSLessThan, t_bool, cond, phi_i, i_three);
	{
		spvm_word ops[3] = { cond, lbl_body, lbl_exit };
		ins(b, SpvOpBranchConditional, 3, ops);
	}

	i1(b, SpvOpLabel, lbl_body);
	i4(b, SpvOpFAdd, t_float, next_sum, phi_sum, b_val);
	i4(b, SpvOpIAdd, t_int, next_i, phi_i, i_one);
	{
		spvm_word ops[1] = { lbl_head };
		ins(b, SpvOpBranch, 1, ops);
	}

	i1(b, SpvOpLabel, lbl_exit);
	i5(b, SpvOpExtInst, t_float, f_sqrt, e_glsl, GLSL_Sqrt, phi_sum);
	i2(b, SpvOpStore, v_out, f_sqrt);
	i0(b, SpvOpReturn);
	i0(b, SpvOpFunctionEnd);
}

static int case_control_loop_phi(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_loop_module(&b);

	const float b_in = 2.0f;

	fixture plain;
	TASK_CHECK(fixture_build(&plain, &b, 0, 0));
	set_scalar(plain.state, id_of(plain.state, "b"), b_in);
	run_main(plain.state);
	float primal_plain = scalar_at(plain.state, id_of(plain.state, "out"));
	TASK_CHECK(nearf(primal_plain, sqrtf(6.0f), 1e-5f));
	fixture_destroy(&plain);

	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 1, 1));
	spvm_word vb = id_of(f.state, "b");
	spvm_word vout = id_of(f.state, "out");
	set_scalar(f.state, vb, b_in);
	spvm_state_prepare(f.state, id_of(f.state, "main"));
	TASK_CHECK(spvm_forward_seed(f.fwd, vb, NULL, 0, 0, 1.0f) == 0);

	spvm_state_call_function(f.state);
	TASK_CHECK(nearf(scalar_at(f.state, vout), primal_plain, 1e-5f));

	float d = 0;
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 0, &d) == 0);
	/* sum = 3*b; out = sqrt(sum); dout/db = 3/(2 sqrt(6)). */
	float expect = 3.0f / (2.0f * sqrtf(6.0f));
	TASK_CHECK(nearf(d, expect, 1e-5f));

	fixture_destroy(&f);
	return 0;
}

/* ================================================================== */
/* Group: control                                                     */
/* ================================================================== */

static int case_control_two_states(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_normdot(&b);

	spvm_context_t ctx = spvm_context_initialize();
	spvm_program_t prog = spvm_program_create(ctx, b.v, (size_t) b.n);
	TASK_CHECK(ctx && prog);

	spvm_state_t s1 = spvm_state_create(prog);
	spvm_state_t s2 = spvm_state_create(prog);
	TASK_CHECK(s1 && s2);
	spvm_ext_opcode_func* ext1 = spvm_build_glsl450_ext();
	spvm_ext_opcode_func* ext2 = spvm_build_glsl450_ext();
	spvm_state_set_extension(s1, "GLSL.std.450", ext1);
	spvm_state_set_extension(s2, "GLSL.std.450", ext2);

	spvm_forward_t f1 = spvm_forward_create(s1, 1);
	spvm_forward_t f2 = spvm_forward_create(s2, 1);
	TASK_CHECK(f1 && f2);

	const float a1[2] = { 3.0f, 4.0f }, b1[2] = { 1.0f, 0.0f };
	const float a2[2] = { 1.0f, 0.0f }, b2[2] = { 0.0f, 2.0f };
	set_vec(s1, id_of(s1, "a"), a1);
	set_vec(s1, id_of(s1, "b"), b1);
	set_vec(s2, id_of(s2, "a"), a2);
	set_vec(s2, id_of(s2, "b"), b2);

	spvm_state_prepare(s1, id_of(s1, "main"));
	spvm_state_prepare(s2, id_of(s2, "main"));

	spvm_word ia1 = id_of(s1, "a");
	spvm_word ia2 = id_of(s2, "a");
	TASK_CHECK(seed_comp(f1, ia1, 0, 0, 1.0f) == 0);
	TASK_CHECK(seed_comp(f2, ia2, 0, 0, 1.0f) == 0);

	int guard = 0;
	while ((s1->code_current || s2->code_current) && guard++ < 100000) {
		if (s1->code_current) spvm_state_step_opcode(s1);
		if (s2->code_current) spvm_state_step_opcode(s2);
	}
	TASK_CHECK(guard < 100000);

	float p1 = scalar_at(s1, id_of(s1, "out"));
	float p2 = scalar_at(s2, id_of(s2, "out"));
	TASK_CHECK(nearf(p1, 0.6f, 1e-5f));
	TASK_CHECK(nearf(p2, 0.0f, 1e-5f));

	float d1 = 0, d2 = 0;
	TASK_CHECK(spvm_forward_get(f1, id_of(s1, "out"), NULL, 0, 0, &d1) == 0);
	TASK_CHECK(spvm_forward_get(f2, id_of(s2, "out"), NULL, 0, 0, &d2) == 0);

	float e0, e1;
	normdot_expect(a1, b1, &e0, &e1);
	TASK_CHECK(nearf(d1, e0, 1e-5f));
	TASK_CHECK(d2 == 0.0f);

	spvm_forward_delete(f1);
	spvm_forward_delete(f2);
	spvm_state_delete(s1);
	spvm_state_delete(s2);
	spvm_program_delete(prog);
	free(ext1);
	free(ext2);
	spvm_context_deinitialize(ctx);
	return 0;
}

/* ================================================================== */
/* Group: arithmetic (unary/scalar coverage)                          */
/* ================================================================== */

/*
 * Exercises FNegate, FSub, FMul, FDiv, Sqrt, FAbs, Length and a vector
 * built by CompositeConstruct, all in one chain:
 *
 *   Input vec2 a; Input float k; Output float out.
 *   v = (a.x*k - a.y, |a.x|)            // FSub, FMul, FAbs, CompositeConstruct
 *   w = -v / len(v)                     // FNegate, FDiv (componentwise), Length
 *   out = sqrt(v.x*v.x + v.y*v.y)       // FMul, FAdd, Sqrt
 *
 * With a = (3, 4), k = 2:
 *   v = (2, 3), |v| = sqrt(13)
 *   out = sqrt(13)
 *   d out/d a.x = (2*2 + 3*1)/sqrt(13) ... = (v.x*2 + v.y*1)/sqrt13 = 7/sqrt13
 *   d out/d a.y = (v.x*(-1) + v.y*0)/sqrt13 = -2/sqrt13
 *   d out/d k   = (v.x*3 + v.y*0)/sqrt13 = 6/sqrt13
 * FAbs at a.x > 0 contributes +1 to v.y.
 *
 * A second, separate evaluation seeds a.x such that |a.x| is at zero, checking
 * the FAbs zero-tangent convention: with a.x = 0, v.y = 0 and its tangent is 0.
 */
static void build_unary_chain(spv_buf* b)
{
	enum {
		t_void = 1, t_float, t_v2,
		p_in_v2, p_in_f, p_out_f,
		v_a, v_k, v_out, t_fn, e_glsl,
		fn_main, lbl,
		la, lk, ax, ay, t1, vx, vy, v, lenv, nlen, nego, w,
		sq1, sq2, sumsq, res, bound
	};

	b_module_begin(b, bound);
	i1(b, SpvOpCapability, SpvCapabilityShader);
	i2(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	i1(b, SpvOpTypeVoid, t_void);
	i2(b, SpvOpTypeFloat, t_float, 32);
	i3(b, SpvOpTypeVector, t_v2, t_float, 2);
	i3(b, SpvOpTypePointer, p_in_v2, SpvStorageClassInput, t_v2);
	i3(b, SpvOpTypePointer, p_in_f, SpvStorageClassInput, t_float);
	i3(b, SpvOpTypePointer, p_out_f, SpvStorageClassOutput, t_float);
	b_name(b, v_a, "a");
	b_name(b, v_k, "k");
	b_name(b, v_out, "out");
	i3(b, SpvOpVariable, p_in_v2, v_a, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_in_f, v_k, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_out_f, v_out, SpvStorageClassOutput);
	i2(b, SpvOpTypeFunction, t_fn, t_void);
	b_ext_import(b, e_glsl, "GLSL.std.450");
	b_name(b, fn_main, "main");

	i4(b, SpvOpFunction, t_void, fn_main, 0, t_fn);
	i1(b, SpvOpLabel, lbl);
	i3(b, SpvOpLoad, t_v2, la, v_a);
	i3(b, SpvOpLoad, t_float, lk, v_k);
	i4(b, SpvOpCompositeExtract, t_float, ax, la, 0);
	i4(b, SpvOpCompositeExtract, t_float, ay, la, 1);
	i4(b, SpvOpFMul, t_float, t1, ax, lk);     /* a.x * k */
	i4(b, SpvOpFSub, t_float, vx, t1, ay);     /* - a.y */
	i5(b, SpvOpExtInst, t_float, vy, e_glsl, GLSL_FAbs, ax);
	i4(b, SpvOpCompositeConstruct, t_v2, v, vx, vy);
	i5(b, SpvOpExtInst, t_float, lenv, e_glsl, GLSL_Length, v);
	i4(b, SpvOpFDiv, t_float, nlen, vy, lenv); /* unused shaping, exercises FDiv */
	/* w is computed but folded into out below so it cannot be optimised out
	 * conceptually; the VM runs every instruction regardless. */
	i5(b, SpvOpExtInst, t_v2, w, e_glsl, GLSL_Normalize, v);
	i3(b, SpvOpFNegate, t_v2, nego, w);
	(void) nlen;
	i4(b, SpvOpFMul, t_float, sq1, vx, vx);
	i4(b, SpvOpFMul, t_float, sq2, vy, vy);
	i4(b, SpvOpFAdd, t_float, sumsq, sq1, sq2);
	i5(b, SpvOpExtInst, t_float, res, e_glsl, GLSL_Sqrt, sumsq);
	i4(b, SpvOpCompositeExtract, t_float, t1, nego, 0); /* keep nego live */
	i4(b, SpvOpFAdd, t_float, res, res, t1);
	i4(b, SpvOpFSub, t_float, res, res, t1);            /* cancels; tangent too */
	i2(b, SpvOpStore, v_out, res);
	i0(b, SpvOpReturn);
	i0(b, SpvOpFunctionEnd);
}

static int case_arith_unary_chain(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_unary_chain(&b);

	const float a_in[2] = { 3.0f, 4.0f };
	const float k_in = 2.0f;

	fixture plain;
	TASK_CHECK(fixture_build(&plain, &b, 0, 0));
	set_vec(plain.state, id_of(plain.state, "a"), a_in);
	set_scalar(plain.state, id_of(plain.state, "k"), k_in);
	run_main(plain.state);
	float primal = scalar_at(plain.state, id_of(plain.state, "out"));
	TASK_CHECK(nearf(primal, sqrtf(13.0f), 1e-5f));
	fixture_destroy(&plain);

	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 1, 3));
	spvm_word va = id_of(f.state, "a");
	spvm_word vk = id_of(f.state, "k");
	spvm_word vout = id_of(f.state, "out");
	set_vec(f.state, va, a_in);
	set_scalar(f.state, vk, k_in);
	spvm_state_prepare(f.state, id_of(f.state, "main"));

	TASK_CHECK(seed_comp(f.fwd, va, 0, 0, 1.0f) == 0);
	TASK_CHECK(seed_comp(f.fwd, va, 1, 1, 1.0f) == 0);
	TASK_CHECK(spvm_forward_seed(f.fwd, vk, NULL, 0, 2, 1.0f) == 0);

	spvm_state_call_function(f.state);
	TASK_CHECK(nearf(scalar_at(f.state, vout), primal, 1e-5f));

	float d0 = 0, d1 = 0, d2 = 0;
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 0, &d0) == 0);
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 1, &d1) == 0);
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 2, &d2) == 0);

	const float L = sqrtf(13.0f);
	float vx_da0 = k_in, vx_da1 = -1.0f, vx_dk = a_in[0];
	float vy_da0 = 1.0f, vy_da1 = 0.0f;
	float e0 = (2.0f * vx_da0 + 3.0f * vy_da0) / L;
	float e1 = (2.0f * vx_da1 + 3.0f * vy_da1) / L;
	float e2 = (2.0f * vx_dk) / L;
	TASK_CHECK(nearf(d0, e0, 1e-5f));
	TASK_CHECK(nearf(d1, e1, 1e-5f));
	TASK_CHECK(nearf(d2, e2, 1e-5f));

	fixture_destroy(&f);

	/* FAbs at zero: with a.x = 0, the |a.x| lane has tangent 0. */
	const float a_zero[2] = { 0.0f, 4.0f };
	fixture fz;
	TASK_CHECK(fixture_build(&fz, &b, 1, 1));
	spvm_word vz = id_of(fz.state, "a");
	spvm_word kz = id_of(fz.state, "k");
	spvm_word oz = id_of(fz.state, "out");
	set_vec(fz.state, vz, a_zero);
	set_scalar(fz.state, kz, k_in);
	spvm_state_prepare(fz.state, id_of(fz.state, "main"));
	TASK_CHECK(seed_comp(fz.fwd, vz, 0, 0, 1.0f) == 0);
	spvm_state_call_function(fz.state);
	float dz = 0;
	TASK_CHECK(spvm_forward_get(fz.fwd, oz, NULL, 0, 0, &dz) == 0);
	/* v = (0*k - 4, |0|) = (-4, 0); dvx = k = 2, dvy = 0.
	 * out = sqrt(vx^2 + vy^2) -> d = (-4*2)/4 = -2. */
	TASK_CHECK(nearf(dz, -2.0f, 1e-5f));
	fixture_destroy(&fz);

	return 0;
}

/* ================================================================== */
/* Group: aliases (local arrays and coverage order)                   */
/* ================================================================== */

/*
 * Local fixed-size array, written member-by-member through AccessChain, then
 * read back.  Also proves a fresh Function-local starts with zero tangents
 * (a private global's seed must not leak into it).
 *
 *   Private float gv;                 // seeded: tangent 1
 *   Function float arr[2];
 *   arr[0] = gv;                      // first store
 *   arr[0] = gv * 3;                  // later store to the SAME member wins
 *   arr[1] = gv * 2;
 *   out = arr[0] + arr[1] + gv;       // = 3+2+1 = 6, tangent 3+2+1 = 6
 *   Also Function float fresh; fresh is never stored; out2 = fresh (0).
 *
 * If a fresh local inherited the global's tangent, out would be wrong.
 */
static void build_local_array(spv_buf* b)
{
	enum {
		t_void = 1, t_float, t_int,
		p_priv_f, p_out_f, p_out2_f, p_fn_arr, p_fn_f,
		gv, v_out, v_out2,
		ci0, ci1, ci2, f_two, f_three,
		t_arr, t_fn, fn_main, lbl,
		lk, arr, fresh, p0, p1, m1, a0, a1, s1, s2, fr,
		bound
	};

	b_module_begin(b, bound);
	i1(b, SpvOpCapability, SpvCapabilityShader);
	i2(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	i1(b, SpvOpTypeVoid, t_void);
	i2(b, SpvOpTypeFloat, t_float, 32);
	i3(b, SpvOpTypeInt, t_int, 32, 1);
	/* This VM reads the array length while setting up OpTypeArray, so the
	 * length constant is emitted before the type (standard SPIR-V puts
	 * constants after types, but the executor accepts this order). */
	b_int(b, t_int, ci0, 0);
	b_int(b, t_int, ci1, 1);
	b_int(b, t_int, ci2, 2);
	i3(b, SpvOpTypeArray, t_arr, t_float, ci2);
	i3(b, SpvOpTypePointer, p_priv_f, SpvStorageClassPrivate, t_float);
	i3(b, SpvOpTypePointer, p_out_f, SpvStorageClassOutput, t_float);
	i3(b, SpvOpTypePointer, p_out2_f, SpvStorageClassOutput, t_float);
	i3(b, SpvOpTypePointer, p_fn_arr, SpvStorageClassFunction, t_arr);
	i3(b, SpvOpTypePointer, p_fn_f, SpvStorageClassFunction, t_float);
	b_float(b, t_float, f_two, 2.0f);
	b_float(b, t_float, f_three, 3.0f);
	b_name(b, gv, "gv");
	b_name(b, v_out, "out");
	b_name(b, v_out2, "out2");
	i3(b, SpvOpVariable, p_priv_f, gv, SpvStorageClassPrivate);
	i3(b, SpvOpVariable, p_out_f, v_out, SpvStorageClassOutput);
	i3(b, SpvOpVariable, p_out2_f, v_out2, SpvStorageClassOutput);
	i2(b, SpvOpTypeFunction, t_fn, t_void);
	b_name(b, fn_main, "main");

	i4(b, SpvOpFunction, t_void, fn_main, 0, t_fn);
	i1(b, SpvOpLabel, lbl);
	i3(b, SpvOpVariable, p_fn_arr, arr, SpvStorageClassFunction);
	i3(b, SpvOpVariable, p_fn_f, fresh, SpvStorageClassFunction);
	i3(b, SpvOpLoad, t_float, lk, gv);
	/* arr[0] = gv, again arr[0] = gv * 3 (later store wins), arr[1] = gv * 2 */
	i4(b, SpvOpAccessChain, p_fn_f, p0, arr, ci0);
	i2(b, SpvOpStore, p0, lk);
	i4(b, SpvOpFMul, t_float, m1, lk, f_three);
	i2(b, SpvOpStore, p0, m1);
	i4(b, SpvOpAccessChain, p_fn_f, p1, arr, ci1);
	i4(b, SpvOpFMul, t_float, m1, lk, f_two);
	i2(b, SpvOpStore, p1, m1);
	/* out = arr[0] + arr[1] + gv */
	i3(b, SpvOpLoad, t_float, a0, p0);
	i3(b, SpvOpLoad, t_float, a1, p1);
	i4(b, SpvOpFAdd, t_float, s1, a0, a1);
	i4(b, SpvOpFAdd, t_float, s2, s1, lk);
	i2(b, SpvOpStore, v_out, s2);
	/* out2 = fresh (never written: must have a zero tangent) */
	i3(b, SpvOpLoad, t_float, fr, fresh);
	i2(b, SpvOpStore, v_out2, fr);
	i0(b, SpvOpReturn);
	i0(b, SpvOpFunctionEnd);
}

static int case_alias_local_array(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_local_array(&b);

	fixture plain;
	TASK_CHECK(fixture_build(&plain, &b, 0, 0));
	set_scalar(plain.state, id_of(plain.state, "gv"), 1.0f);
	run_main(plain.state);
	TASK_CHECK(nearf(scalar_at(plain.state, id_of(plain.state, "out")), 6.0f, 1e-6f));
	TASK_CHECK(scalar_at(plain.state, id_of(plain.state, "out2")) == 0.0f);
	fixture_destroy(&plain);

	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 1, 1));
	spvm_word gv = id_of(f.state, "gv");
	set_scalar(f.state, gv, 1.0f);
	spvm_state_prepare(f.state, id_of(f.state, "main"));
	TASK_CHECK(spvm_forward_seed(f.fwd, gv, NULL, 0, 0, 1.0f) == 0);
	spvm_state_call_function(f.state);

	float d = 0, d2 = -1;
	TASK_CHECK(spvm_forward_get(f.fwd, id_of(f.state, "out"), NULL, 0, 0, &d) == 0);
	TASK_CHECK(nearf(d, 6.0f, 1e-6f));
	/* Fresh local has zero tangent: the global seed did not leak. */
	TASK_CHECK(spvm_forward_get(f.fwd, id_of(f.state, "out2"), NULL, 0, 0, &d2) == 0);
	TASK_CHECK(d2 == 0.0f);

	fixture_destroy(&f);
	return 0;
}

/* ================================================================== */
/* Group: arithmetic (error handling)                                 */
/* ================================================================== */

/*
 * Unsupported floating-point computation.  The shader computes
 * out = FMod(a, b) (OpFMod is outside the supported set).  The numeric result
 * must still be produced by the ordinary executor; reading derivatives must
 * fail and report the instruction position; the error clears with clear().
 */
static void build_unsupported_module(spv_buf* b)
{
	enum {
		t_void = 1, t_float, p_in_f, p_out_f,
		v_a, v_b, v_out, t_fn, fn_main, lbl,
		la, lb, r, bound
	};

	b_module_begin(b, bound);
	i1(b, SpvOpCapability, SpvCapabilityShader);
	i2(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	i1(b, SpvOpTypeVoid, t_void);
	i2(b, SpvOpTypeFloat, t_float, 32);
	i3(b, SpvOpTypePointer, p_in_f, SpvStorageClassInput, t_float);
	i3(b, SpvOpTypePointer, p_out_f, SpvStorageClassOutput, t_float);
	b_name(b, v_a, "a");
	b_name(b, v_b, "b");
	b_name(b, v_out, "out");
	i3(b, SpvOpVariable, p_in_f, v_a, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_in_f, v_b, SpvStorageClassInput);
	i3(b, SpvOpVariable, p_out_f, v_out, SpvStorageClassOutput);
	i2(b, SpvOpTypeFunction, t_fn, t_void);
	b_name(b, fn_main, "main");

	i4(b, SpvOpFunction, t_void, fn_main, 0, t_fn);
	i1(b, SpvOpLabel, lbl);
	i3(b, SpvOpLoad, t_float, la, v_a);
	i3(b, SpvOpLoad, t_float, lb, v_b);
	i4(b, SpvOpFMod, t_float, r, la, lb);
	i2(b, SpvOpStore, v_out, r);
	i0(b, SpvOpReturn);
	i0(b, SpvOpFunctionEnd);
}

static int case_arith_unsupported(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_unsupported_module(&b);

	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 1, 1));
	spvm_word va = id_of(f.state, "a");
	spvm_word vb = id_of(f.state, "b");
	spvm_word vout = id_of(f.state, "out");
	set_scalar(f.state, va, 5.5f);
	set_scalar(f.state, vb, 2.0f);
	spvm_state_prepare(f.state, id_of(f.state, "main"));
	TASK_CHECK(spvm_forward_seed(f.fwd, va, NULL, 0, 0, 1.0f) == 0);

	spvm_state_call_function(f.state);

	/* The ordinary numeric path must still have produced the primal value. */
	TASK_CHECK(nearf(scalar_at(f.state, vout), 1.5f, 1e-6f));

	/* Derivatives are refused, with a recorded instruction position. */
	spvm_word at = -1;
	TASK_CHECK(spvm_forward_error(f.fwd, &at) != 0);
	TASK_CHECK(at >= 0);
	float d = 123.0f;
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 0, &d) != 0);
	TASK_CHECK(d == 123.0f);

	/* clear() resets the differential error but not the numeric state. */
	float numeric_before = scalar_at(f.state, vout);
	spvm_forward_clear(f.fwd);
	TASK_CHECK(spvm_forward_error(f.fwd, NULL) == 0);
	TASK_CHECK(nearf(scalar_at(f.state, vout), numeric_before, 0.0f));
	float zero = 99.0f;
	TASK_CHECK(spvm_forward_get(f.fwd, vout, NULL, 0, 0, &zero) == 0);
	TASK_CHECK(zero == 0.0f);

	fixture_destroy(&f);
	return 0;
}

/* ================================================================== */
/* Group: aliases (continued)                                         */
/* ================================================================== */

/*
 * Value parameters.  Two helper functions:
 *   f(float x, vec2 v, float* out) -> float   (value args, plus pointer out)
 *       *out = x + v.y;  return x + v.x + v.y;
 *   main:
 *       side = 0
 *       r = f(g.v.x, g.v, &side)      // value copy of g.v into `v`
 *       out = r * 2 + side + g.v.y
 *
 * With g.v = (2, 3):
 *   f: side = x + v.y = 5, r = x + v.x + v.y = 7;  out = 14 + 5 + 3 = 22
 *   d/d g.v.x : r' = 1 (x) + 1 (v.x) = 2 -> r*2 = 4; side' = 1 -> +1; total 5
 *   d/d g.v.y : r' = 1 (v.y) = 1 -> r*2 = 2; side' = 1 -> +1; plus 1 -> 4
 *
 * Also: the callee writes *out through a pointer, exercising pointer params
 * alongside value params in the same call.
 */
static void build_value_params(spv_buf* b)
{
	enum {
		t_void = 1, t_float, t_int, t_v2, t_S,
		p_priv_S, p_priv_v2, p_priv_f, p_out_f, p_fn_f,
		g, v_out, side, t_fn_main, t_fn_f,
		fn_main, fn_f, lbl_main, lbl_f,
		ip_x, ip_v, ip_out, i_zero, f_two,
		vg, v_old, x_old, side_val, r_call, r_mul, sum1, sum2,
		f_side, f_r, f_vx, f_vy, m_vy, bound
	};

	b_module_begin(b, bound);
	i1(b, SpvOpCapability, SpvCapabilityShader);
	i2(b, SpvOpMemoryModel, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
	i1(b, SpvOpTypeVoid, t_void);
	i2(b, SpvOpTypeFloat, t_float, 32);
	i3(b, SpvOpTypeInt, t_int, 32, 1);
	i3(b, SpvOpTypeVector, t_v2, t_float, 2);
	{
		spvm_word members[2] = { t_v2, t_float };
		b_struct(b, t_S, 2, members);
	}
	i3(b, SpvOpTypePointer, p_priv_S, SpvStorageClassPrivate, t_S);
	i3(b, SpvOpTypePointer, p_priv_v2, SpvStorageClassPrivate, t_v2);
	i3(b, SpvOpTypePointer, p_priv_f, SpvStorageClassPrivate, t_float);
	i3(b, SpvOpTypePointer, p_out_f, SpvStorageClassOutput, t_float);
	i3(b, SpvOpTypePointer, p_fn_f, SpvStorageClassFunction, t_float);
	b_member_name(b, t_S, 0, "v");
	b_name(b, g, "g");
	b_name(b, v_out, "out");
	b_name(b, side, "side");
	i3(b, SpvOpVariable, p_priv_S, g, SpvStorageClassPrivate);
	i3(b, SpvOpVariable, p_out_f, v_out, SpvStorageClassOutput);
	i3(b, SpvOpVariable, p_fn_f, side, SpvStorageClassFunction);
	i2(b, SpvOpTypeFunction, t_fn_main, t_void);
	{
		spvm_word ops[5] = { t_fn_f, t_float, t_float, t_v2, p_fn_f };
		ins(b, SpvOpTypeFunction, 5, ops);
	}
	b_int(b, t_int, i_zero, 0);
	b_float(b, t_float, f_two, 2.0f);

	b_name(b, fn_main, "main");

	/* f(float x, vec2 v, float* out) */
	i4(b, SpvOpFunction, t_float, fn_f, 0, t_fn_f);
	i2(b, SpvOpFunctionParameter, t_float, ip_x);
	i2(b, SpvOpFunctionParameter, t_v2, ip_v);
	i2(b, SpvOpFunctionParameter, p_fn_f, ip_out);
	i1(b, SpvOpLabel, lbl_f);
	i4(b, SpvOpCompositeExtract, t_float, f_vx, ip_v, 0);  /* v.x */
	i4(b, SpvOpCompositeExtract, t_float, f_vy, ip_v, 1);  /* v.y */
	i4(b, SpvOpFAdd, t_float, f_side, ip_x, f_vy);         /* side = x + v.y */
	i2(b, SpvOpStore, ip_out, f_side);
	i4(b, SpvOpFAdd, t_float, f_r, ip_x, f_vx);            /* x + v.x */
	i4(b, SpvOpFAdd, t_float, f_r, f_r, f_vy);             /* + v.y */
	i1(b, SpvOpReturnValue, f_r);
	i0(b, SpvOpFunctionEnd);

	/* main */
	i4(b, SpvOpFunction, t_void, fn_main, 0, t_fn_main);
	i1(b, SpvOpLabel, lbl_main);
	i4(b, SpvOpAccessChain, p_priv_v2, vg, g, i_zero);
	i3(b, SpvOpLoad, t_v2, v_old, vg);
	i4(b, SpvOpCompositeExtract, t_float, x_old, v_old, 0);
	i6(b, SpvOpFunctionCall, t_float, r_call, fn_f, x_old, v_old, side);
	i4(b, SpvOpFMul, t_float, r_mul, r_call, f_two);
	i3(b, SpvOpLoad, t_float, side_val, side);
	i4(b, SpvOpFAdd, t_float, sum1, r_mul, side_val);
	i4(b, SpvOpCompositeExtract, t_float, m_vy, v_old, 1);
	i4(b, SpvOpFAdd, t_float, sum2, sum1, m_vy);
	i2(b, SpvOpStore, v_out, sum2);
	i0(b, SpvOpReturn);
	i0(b, SpvOpFunctionEnd);

	(void) f_side;
}

static int case_alias_value_params(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_value_params(&b);

	const float gv[2] = { 2.0f, 3.0f };

	fixture plain;
	TASK_CHECK(fixture_build(&plain, &b, 0, 0));
	spvm_word pg = id_of(plain.state, "g");
	spvm_member_set_value_f(plain.state->results[pg].members[0].members, 2, (float*) gv);
	run_main(plain.state);
	float primal_plain = scalar_at(plain.state, id_of(plain.state, "out"));
	TASK_CHECK(nearf(primal_plain, 22.0f, 1e-5f));
	fixture_destroy(&plain);

	fixture f;
	TASK_CHECK(fixture_build(&f, &b, 1, 2));
	spvm_word g = id_of(f.state, "g");
	spvm_member_set_value_f(f.state->results[g].members[0].members, 2, (float*) gv);
	spvm_state_prepare(f.state, id_of(f.state, "main"));

	spvm_word p0[2] = { 0, 0 };
	spvm_word p1[2] = { 0, 1 };
	TASK_CHECK(spvm_forward_seed(f.fwd, g, p0, 2, 0, 1.0f) == 0);
	TASK_CHECK(spvm_forward_seed(f.fwd, g, p1, 2, 1, 1.0f) == 0);

	spvm_state_call_function(f.state);
	TASK_CHECK(nearf(scalar_at(f.state, id_of(f.state, "out")), primal_plain, 1e-5f));

	float d0 = 0, d1 = 0;
	TASK_CHECK(spvm_forward_get(f.fwd, id_of(f.state, "out"), NULL, 0, 0, &d0) == 0);
	TASK_CHECK(spvm_forward_get(f.fwd, id_of(f.state, "out"), NULL, 0, 1, &d1) == 0);
	TASK_CHECK(nearf(d0, 5.0f, 1e-5f));
	TASK_CHECK(nearf(d1, 4.0f, 1e-5f));

	fixture_destroy(&f);
	return 0;
}

/* ================================================================== */
/* Group: control (continued)                                         */
/* ================================================================== */

static int case_control_step_equiv(void)
{
	spv_buf b;
	memset(&b, 0, sizeof(b));
	build_normdot(&b);

	const float a_in[2] = { 3.0f, 4.0f };
	const float b_in[2] = { 1.0f, 0.0f };

	/* Run 1: call_function. */
	fixture f1;
	TASK_CHECK(fixture_build(&f1, &b, 1, 1));
	spvm_word a1 = id_of(f1.state, "a");
	set_vec(f1.state, a1, a_in);
	set_vec(f1.state, id_of(f1.state, "b"), b_in);
	spvm_state_prepare(f1.state, id_of(f1.state, "main"));
	TASK_CHECK(spvm_forward_seed(f1.fwd, a1, NULL, 0, 0, 1.0f) != 0); /* vec: scalar seed invalid */
	spvm_word idx0 = 0;
	TASK_CHECK(spvm_forward_seed(f1.fwd, a1, &idx0, 1, 0, 1.0f) == 0);
	spvm_state_call_function(f1.state);
	float call_out = scalar_at(f1.state, id_of(f1.state, "out"));
	float call_d = 0;
	TASK_CHECK(spvm_forward_get(f1.fwd, id_of(f1.state, "out"), NULL, 0, 0, &call_d) == 0);

	/* Run 2: step_opcode one instruction at a time. */
	fixture f2;
	TASK_CHECK(fixture_build(&f2, &b, 1, 1));
	spvm_word a2 = id_of(f2.state, "a");
	set_vec(f2.state, a2, a_in);
	set_vec(f2.state, id_of(f2.state, "b"), b_in);
	spvm_state_prepare(f2.state, id_of(f2.state, "main"));
	TASK_CHECK(spvm_forward_seed(f2.fwd, a2, &idx0, 1, 0, 1.0f) == 0);

	int guard = 0;
	while (f2.state->code_current && guard++ < 100000)
		spvm_state_step_opcode(f2.state);
	TASK_CHECK(guard < 100000);
	float step_out = scalar_at(f2.state, id_of(f2.state, "out"));
	float step_d = 0;
	TASK_CHECK(spvm_forward_get(f2.fwd, id_of(f2.state, "out"), NULL, 0, 0, &step_d) == 0);

	TASK_CHECK(nearf(step_out, call_out, 1e-6f));
	TASK_CHECK(nearf(step_d, call_d, 1e-6f));
	TASK_CHECK(nearf(step_d, 0.128f, 1e-5f));

	fixture_destroy(&f1);
	fixture_destroy(&f2);
	return 0;
}

/* ================================================================== */
/* Registration                                                       */
/* ================================================================== */

const struct task_case task_cases[] = {
	{ "arithmetic", "normalize_dot_two_directions", case_arith_normdot },
	{ "arithmetic", "composite_and_vector_ops", case_arith_composite_ops },
	{ "arithmetic", "api_contract", case_arith_api_contract },
	{ "arithmetic", "example_shader_time", case_arith_example_shader },
	{ "arithmetic", "unary_chain_and_fabs_zero", case_arith_unary_chain },
	{ "arithmetic", "unsupported_reports_error", case_arith_unsupported },
	{ "aliases", "helpers_alias_and_copy", case_alias_helpers },
	{ "aliases", "function_value_params", case_alias_value_params },
	{ "aliases", "local_array_store_order", case_alias_local_array },
	{ "control", "loop_with_phi", case_control_loop_phi },
	{ "control", "two_states_interleaved", case_control_two_states },
	{ "control", "step_vs_call_equivalent", case_control_step_equiv },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
