/*
Checks for mathc_skin_vertex. Built and run by tests/check_skin.sh for both
the default (single precision) and the MATHC_USE_DOUBLE_FLOATING_POINT
configurations. Every case carries independent numeric expectations; frame
cases additionally assert orthogonality and unit lengths.
*/
#include <stdio.h>

#include "mathc.h"
#include "mathc_skin.h"

static int g_failures = 0;

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	g_failures += 1;
}

static mfloat_t dot3(const mfloat_t a[3], const mfloat_t b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static mfloat_t len3(const mfloat_t v[3])
{
	return MSQRT(dot3(v, v));
}

static void expect_close(const char *what, mfloat_t got, mfloat_t want)
{
	mfloat_t tol = MFLT_EPSILON * MFLOAT_C(256.0) *
		(MFLOAT_C(1.0) + MFABS(want));
	if (MFABS(got - want) > tol) {
		printf("FAIL: %s: got %.9g, expected %.9g\n", what, (double)got,
			(double)want);
		g_failures += 1;
	}
}

static void expect_vec3(const char *what, const mfloat_t got[3], mfloat_t x,
	mfloat_t y, mfloat_t z)
{
	char name[128];
	snprintf(name, sizeof(name), "%s.x", what);
	expect_close(name, got[0], x);
	snprintf(name, sizeof(name), "%s.y", what);
	expect_close(name, got[1], y);
	snprintf(name, sizeof(name), "%s.z", what);
	expect_close(name, got[2], z);
}

static void expect_vec4(const char *what, const mfloat_t got[4], mfloat_t x,
	mfloat_t y, mfloat_t z, mfloat_t w)
{
	char name[128];
	snprintf(name, sizeof(name), "%s.w", what);
	expect_vec3(what, got, x, y, z);
	expect_close(name, got[3], w);
}

/* Orthonormal frame properties: unit lengths, orthogonality, handedness. */
static void expect_frame(const char *what, const mfloat_t n[3],
	const mfloat_t t[4])
{
	mfloat_t b[3];
	char name[128];
	snprintf(name, sizeof(name), "%s: |normal|", what);
	expect_close(name, len3(n), MFLOAT_C(1.0));
	snprintf(name, sizeof(name), "%s: |tangent|", what);
	expect_close(name, len3(t), MFLOAT_C(1.0));
	snprintf(name, sizeof(name), "%s: normal.tangent", what);
	expect_close(name, dot3(n, t), MFLOAT_C(0.0));
	if (t[3] != MFLOAT_C(1.0) && t[3] != -MFLOAT_C(1.0)) {
		snprintf(name, sizeof(name), "%s: tangent.w not +/-1", what);
		fail(name);
		return;
	}
	/* bitangent = cross(normal, tangent.xyz) * tangent.w must be unit */
	b[0] = (n[1] * t[2] - n[2] * t[1]) * t[3];
	b[1] = (n[2] * t[0] - n[0] * t[2]) * t[3];
	b[2] = (n[0] * t[1] - n[1] * t[0]) * t[3];
	snprintf(name, sizeof(name), "%s: |bitangent|", what);
	expect_close(name, len3(b), MFLOAT_C(1.0));
}

static void lin_diag(mfloat_t a[9], mfloat_t x, mfloat_t y, mfloat_t z)
{
	a[0] = x; a[1] = 0; a[2] = 0;
	a[3] = 0; a[4] = y; a[5] = 0;
	a[6] = 0; a[7] = 0; a[8] = z;
}

static void mat4_affine(mfloat_t m[16], const mfloat_t a[9], mfloat_t tx,
	mfloat_t ty, mfloat_t tz)
{
	m[0] = a[0]; m[1] = a[1]; m[2] = a[2];  m[3] = 0;
	m[4] = a[3]; m[5] = a[4]; m[6] = a[5];  m[7] = 0;
	m[8] = a[6]; m[9] = a[7]; m[10] = a[8]; m[11] = 0;
	m[12] = tx;  m[13] = ty;  m[14] = tz;   m[15] = 1;
}

/* Two translation-free bones: scale (2,1,1) and scale (1,3,1). */
static void palette_scales(mfloat_t *palette)
{
	mfloat_t a[9];
	lin_diag(a, 2, 1, 1);
	mat4_affine(palette + 0, a, 0, 0, 0);
	lin_diag(a, 1, 3, 1);
	mat4_affine(palette + 16, a, 0, 0, 0);
}

static mfloat_t make_nan(void)
{
	volatile mfloat_t zero = MFLOAT_C(0.0);
	return zero / zero;
}

static mfloat_t make_inf(void)
{
	volatile mfloat_t one = MFLOAT_C(1.0);
	volatile mfloat_t zero = MFLOAT_C(0.0);
	return one / zero;
}

/*
The reference case from the module contract: bones scale (2,1,1) and
(1,3,1), weights (2,2,0,0), position (1,1,0), normal (1,1,0), tangent
(1,-1,0,+1). Blending unnormalized weights, blending per-bone normals, or
transforming the normal forward all give different (wrong) results.
*/
static void test_scale_blend(void)
{
	mfloat_t palette[32];
	mfloat_t pos[3], nrm[3], tan[4];
	unsigned joints[4] = {0, 1, 0, 0};
	mfloat_t weights[4] = {2, 2, 0, 0};
	mfloat_t position[3] = {1, 1, 0};
	mfloat_t normal[3] = {1, 1, 0};
	mfloat_t tangent[4] = {1, -1, 0, 1};
	palette_scales(palette);
	if (!mathc_skin_vertex(pos, nrm, tan, position, normal, tangent,
		joints, weights, palette, 2)) {
		fail("scale_blend: rejected valid input");
		return;
	}
	expect_vec3("scale_blend position", pos, MFLOAT_C(1.5), 2, 0);
	expect_vec3("scale_blend normal", nrm, MFLOAT_C(0.8), MFLOAT_C(0.6), 0);
	expect_vec4("scale_blend tangent", tan, MFLOAT_C(0.6), MFLOAT_C(-0.8),
		0, 1);
	expect_frame("scale_blend", nrm, tan);
}

/* Single bone: rotation of 90 degrees around z plus a translation. */
static void test_rotation_translation(void)
{
	mfloat_t palette[16];
	mfloat_t pos[3], nrm[3], tan[4];
	mfloat_t a[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
	unsigned joints[4] = {0, 0, 0, 0};
	mfloat_t weights[4] = {1, 0, 0, 0};
	mfloat_t position[3] = {1, 0, 0};
	mfloat_t normal[3] = {1, 0, 0};
	mfloat_t tangent[4] = {0, 1, 0, -1};
	mat4_affine(palette, a, 10, 20, 30);
	if (!mathc_skin_vertex(pos, nrm, tan, position, normal, tangent,
		joints, weights, palette, 1)) {
		fail("rotation_translation: rejected valid input");
		return;
	}
	expect_vec3("rotation_translation position", pos, 10, 21, 30);
	expect_vec3("rotation_translation normal", nrm, 0, 1, 0);
	expect_vec4("rotation_translation tangent", tan, -1, 0, 0, -1);
	expect_frame("rotation_translation", nrm, tan);
}

/* Adding the same translation to every bone must only move the position. */
static void test_uniform_translation(void)
{
	mfloat_t palette[32];
	mfloat_t pos[3], nrm[3], tan[4];
	unsigned joints[4] = {0, 1, 0, 0};
	mfloat_t weights[4] = {2, 2, 0, 0};
	mfloat_t position[3] = {1, 1, 0};
	mfloat_t normal[3] = {1, 1, 0};
	mfloat_t tangent[4] = {1, -1, 0, 1};
	palette_scales(palette);
	palette[12] = 5;  palette[13] = -3; palette[14] = 7;
	palette[28] = 5;  palette[29] = -3; palette[30] = 7;
	if (!mathc_skin_vertex(pos, nrm, tan, position, normal, tangent,
		joints, weights, palette, 2)) {
		fail("uniform_translation: rejected valid input");
		return;
	}
	expect_vec3("uniform_translation position", pos, MFLOAT_C(6.5), -1, 7);
	expect_vec3("uniform_translation normal", nrm, MFLOAT_C(0.8),
		MFLOAT_C(0.6), 0);
	expect_vec4("uniform_translation tangent", tan, MFLOAT_C(0.6),
		MFLOAT_C(-0.8), 0, 1);
}

/* Repeated joint references must accumulate like a single merged weight. */
static void test_repeated_joints(void)
{
	mfloat_t palette[32];
	mfloat_t pos[3], nrm[3], tan[4];
	unsigned joints[4] = {0, 0, 1, 1};
	mfloat_t weights[4] = {1, 3, 2, 2};
	mfloat_t position[3] = {1, 1, 0};
	mfloat_t normal[3] = {1, 1, 0};
	mfloat_t tangent[4] = {1, -1, 0, 1};
	palette_scales(palette);
	if (!mathc_skin_vertex(pos, nrm, tan, position, normal, tangent,
		joints, weights, palette, 2)) {
		fail("repeated_joints: rejected valid input");
		return;
	}
	expect_vec3("repeated_joints position", pos, MFLOAT_C(1.5), 2, 0);
	expect_vec3("repeated_joints normal", nrm, MFLOAT_C(0.8), MFLOAT_C(0.6),
		0);
	expect_vec4("repeated_joints tangent", tan, MFLOAT_C(0.6),
		MFLOAT_C(-0.8), 0, 1);
}

/* Zero-weight joints are never accessed: wild indices and even a NaN
palette entry behind a zero weight must be harmless. */
static void test_zero_weight_joints(void)
{
	mfloat_t palette[48];
	mfloat_t pos[3], nrm[3], tan[4];
	unsigned joints[4] = {0, 1, 99, 12345};
	mfloat_t weights[4] = {2, 2, 0, 0};
	mfloat_t position[3] = {1, 1, 0};
	mfloat_t normal[3] = {1, 1, 0};
	mfloat_t tangent[4] = {1, -1, 0, 1};
	int k;
	palette_scales(palette);
	for (k = 32; k < 48; k++) {
		palette[k] = make_nan();
	}
	if (!mathc_skin_vertex(pos, nrm, tan, position, normal, tangent,
		joints, weights, palette, 2)) {
		fail("zero_weight_joints: rejected valid input");
		return;
	}
	expect_vec3("zero_weight_joints position", pos, MFLOAT_C(1.5), 2, 0);
	expect_vec3("zero_weight_joints normal", nrm, MFLOAT_C(0.8),
		MFLOAT_C(0.6), 0);
	expect_vec4("zero_weight_joints tangent", tan, MFLOAT_C(0.6),
		MFLOAT_C(-0.8), 0, 1);
}

/* Four distinct joints with distinct weights and translations. */
static void test_four_joints(void)
{
	mfloat_t palette[64];
	mfloat_t pos[3], nrm[3], tan[4];
	mfloat_t a[9];
	unsigned joints[4] = {0, 1, 2, 3};
	mfloat_t weights[4] = {1, 1, 2, 4};
	mfloat_t position[3] = {1, 1, 1};
	mfloat_t normal[3] = {1, 1, 0};
	mfloat_t tangent[4] = {0, 0, 1, 1};
	mfloat_t inv;
	lin_diag(a, 2, 1, 1);
	mat4_affine(palette + 0, a, 0, 0, 0);
	lin_diag(a, 1, 3, 1);
	mat4_affine(palette + 16, a, 1, 0, 0);
	lin_diag(a, 1, 1, 4);
	mat4_affine(palette + 32, a, 0, 1, 0);
	lin_diag(a, 1, 1, 1);
	mat4_affine(palette + 48, a, 0, 0, 1);
	if (!mathc_skin_vertex(pos, nrm, tan, position, normal, tangent,
		joints, weights, palette, 4)) {
		fail("four_joints: rejected valid input");
		return;
	}
	/* blended linear part diag(9/8, 10/8, 14/8), translation (1,2,4)/8 */
	expect_vec3("four_joints position", pos, MFLOAT_C(1.25), MFLOAT_C(1.5),
		MFLOAT_C(2.25));
	/* normal (1,1,0) -> inverse-transpose gives (8/9, 4/5, 0), which
	normalizes to (10, 9, 0) / sqrt(181) */
	inv = 1 / MSQRT(MFLOAT_C(181.0));
	expect_vec3("four_joints normal", nrm, 10 * inv, 9 * inv, 0);
	expect_vec4("four_joints tangent", tan, 0, 0, 1, 1);
	expect_frame("four_joints", nrm, tan);
}

/* Rotation+scale mix with independent analytic expectations.
bone0 = uniform scale 2 after a 90 degree z rotation, translation (1,2,3);
bone1 = identity, translation (4,5,6); weights (1,1,0,0).
Blended linear part has columns (0.5,1,0), (-1,0.5,0), (0,0,1.5). */
static void test_rotation_scale_mix(void)
{
	mfloat_t palette[32];
	mfloat_t pos[3], nrm[3], tan[4];
	mfloat_t a[9] = {0, 2, 0, -2, 0, 0, 0, 0, 2};
	mfloat_t b[9];
	unsigned joints[4] = {0, 1, 0, 0};
	mfloat_t weights[4] = {1, 1, 0, 0};
	mfloat_t position[3] = {1, 1, 1};
	mfloat_t normal[3] = {1, 0, 0};
	mfloat_t tangent[4] = {0, 1, 0, 1};
	mfloat_t inv5;
	mat4_affine(palette + 0, a, 1, 2, 3);
	lin_diag(b, 1, 1, 1);
	mat4_affine(palette + 16, b, 4, 5, 6);
	if (!mathc_skin_vertex(pos, nrm, tan, position, normal, tangent,
		joints, weights, palette, 2)) {
		fail("rotation_scale_mix: rejected valid input");
		return;
	}
	inv5 = 1 / MSQRT(MFLOAT_C(5.0));
	expect_vec3("rotation_scale_mix position", pos, 2, 5, 6);
	expect_vec3("rotation_scale_mix normal", nrm, inv5, 2 * inv5, 0);
	expect_vec4("rotation_scale_mix tangent", tan, -2 * inv5, inv5, 0, 1);
	expect_frame("rotation_scale_mix", nrm, tan);
}

/* A single reflecting bone flips the position and tangent x and the
tangent w sign, while a z-aligned normal survives. */
static void test_reflection(void)
{
	mfloat_t palette[16];
	mfloat_t pos[3], nrm[3], tan[4];
	mfloat_t a[9];
	unsigned joints[4] = {0, 0, 0, 0};
	mfloat_t weights[4] = {1, 0, 0, 0};
	mfloat_t position[3] = {1, 2, 3};
	mfloat_t normal[3] = {0, 0, 1};
	mfloat_t tangent[4] = {1, 0, 0, 1};
	lin_diag(a, -1, 1, 1);
	mat4_affine(palette, a, 0, 0, 0);
	if (!mathc_skin_vertex(pos, nrm, tan, position, normal, tangent,
		joints, weights, palette, 1)) {
		fail("reflection: rejected valid input");
		return;
	}
	expect_vec3("reflection position", pos, -1, 2, 3);
	expect_vec3("reflection normal", nrm, 0, 0, 1);
	expect_vec4("reflection tangent", tan, -1, 0, 0, -1);
	expect_frame("reflection", nrm, tan);
}

/* Each output may exactly alias its corresponding input. */
static void test_in_place(void)
{
	mfloat_t palette[32];
	mfloat_t pos[3] = {1, 1, 0};
	mfloat_t nrm[3] = {1, 1, 0};
	mfloat_t tan[4] = {1, -1, 0, 1};
	unsigned joints[4] = {0, 1, 0, 0};
	mfloat_t weights[4] = {2, 2, 0, 0};
	palette_scales(palette);
	if (!mathc_skin_vertex(pos, nrm, tan, pos, nrm, tan, joints, weights,
		palette, 2)) {
		fail("in_place: rejected valid input");
		return;
	}
	expect_vec3("in_place position", pos, MFLOAT_C(1.5), 2, 0);
	expect_vec3("in_place normal", nrm, MFLOAT_C(0.8), MFLOAT_C(0.6), 0);
	expect_vec4("in_place tangent", tan, MFLOAT_C(0.6), MFLOAT_C(-0.8), 0,
		1);
}

/* A rejected call must return false and leave every output untouched. */
static void expect_rejected(const char *what, const mfloat_t position[3],
	const mfloat_t normal[3], const mfloat_t tangent[4],
	const unsigned joints[4], const mfloat_t weights[4],
	const mfloat_t *palette, size_t palette_count)
{
	mfloat_t pos[3] = {-11, -12, -13};
	mfloat_t nrm[3] = {-21, -22, -23};
	mfloat_t tan[4] = {-31, -32, -33, -34};
	if (mathc_skin_vertex(pos, nrm, tan, position, normal, tangent, joints,
		weights, palette, palette_count)) {
		fail(what);
		return;
	}
	if (pos[0] != -11 || pos[1] != -12 || pos[2] != -13 ||
		nrm[0] != -21 || nrm[1] != -22 || nrm[2] != -23 ||
		tan[0] != -31 || tan[1] != -32 || tan[2] != -33 ||
		tan[3] != -34) {
		fail("outputs modified on failure");
	}
}

static void test_invalid_inputs(void)
{
	mfloat_t palette[32];
	unsigned joints[4] = {0, 1, 0, 0};
	mfloat_t weights[4] = {2, 2, 0, 0};
	mfloat_t position[3] = {1, 1, 0};
	mfloat_t normal[3] = {1, 1, 0};
	mfloat_t tangent[4] = {1, -1, 0, 1};
	mfloat_t v3[3];
	mfloat_t v4[4];
	mfloat_t w[4];
	unsigned j[4];
	palette_scales(palette);

	w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 0;
	expect_rejected("zero weight sum", position, normal, tangent, joints,
		w, palette, 2);
	w[0] = -1; w[1] = 2; w[2] = 0; w[3] = 0;
	expect_rejected("negative weight", position, normal, tangent, joints,
		w, palette, 2);
	w[0] = make_nan(); w[1] = 2; w[2] = 0; w[3] = 0;
	expect_rejected("NaN weight", position, normal, tangent, joints, w,
		palette, 2);
	w[0] = make_inf(); w[1] = 2; w[2] = 0; w[3] = 0;
	expect_rejected("infinite weight", position, normal, tangent, joints,
		w, palette, 2);

	j[0] = 7; j[1] = 1; j[2] = 0; j[3] = 0;
	expect_rejected("positive-weight joint out of range", position, normal,
		tangent, j, weights, palette, 2);

	v3[0] = 0; v3[1] = 0; v3[2] = 0;
	expect_rejected("zero normal", position, v3, tangent, joints, weights,
		palette, 2);
	v4[0] = 0; v4[1] = 0; v4[2] = 0; v4[3] = 1;
	expect_rejected("zero tangent", position, normal, v4, joints, weights,
		palette, 2);
	v3[0] = 1; v3[1] = 0; v3[2] = 0;
	v4[0] = 2; v4[1] = 0; v4[2] = 0; v4[3] = 1;
	expect_rejected("parallel normal and tangent", position, v3, v4, joints,
		weights, palette, 2);
	v4[0] = 1; v4[1] = -1; v4[2] = 0; v4[3] = MFLOAT_C(0.5);
	expect_rejected("tangent.w not +/-1", position, normal, v4, joints,
		weights, palette, 2);
	v4[0] = 1; v4[1] = -1; v4[2] = 0; v4[3] = 0;
	expect_rejected("tangent.w zero", position, normal, v4, joints, weights,
		palette, 2);

	v3[0] = make_nan(); v3[1] = 1; v3[2] = 0;
	expect_rejected("NaN position", v3, normal, tangent, joints, weights,
		palette, 2);
	v3[0] = 1; v3[1] = make_inf(); v3[2] = 0;
	expect_rejected("infinite normal", position, v3, tangent, joints,
		weights, palette, 2);
	v4[0] = 1; v4[1] = -1; v4[2] = 0; v4[3] = make_nan();
	expect_rejected("NaN tangent.w", position, normal, v4, joints, weights,
		palette, 2);

	palette[15] = 2;
	expect_rejected("non-affine palette (m[15])", position, normal, tangent,
		joints, weights, palette, 2);
	palette[15] = 1;
	palette[3] = 1;
	expect_rejected("non-affine palette (m[3])", position, normal, tangent,
		joints, weights, palette, 2);
	palette[3] = 0;
	palette[5] = make_nan();
	expect_rejected("NaN palette entry", position, normal, tangent, joints,
		weights, palette, 2);
	palette[5] = 1;
}

static void test_degenerate_blend(void)
{
	mfloat_t palette[32];
	mfloat_t a[9];
	unsigned joints[4] = {0, 1, 0, 0};
	mfloat_t weights[4] = {1, 1, 0, 0};
	mfloat_t position[3] = {1, 1, 0};
	mfloat_t normal[3] = {1, 1, 0};
	mfloat_t tangent[4] = {1, -1, 0, 1};
	unsigned j1[4] = {0, 0, 0, 0};
	mfloat_t w1[4] = {1, 0, 0, 0};

	/* identity blended 50/50 with a reflection: singular linear part */
	lin_diag(a, 1, 1, 1);
	mat4_affine(palette + 0, a, 0, 0, 0);
	lin_diag(a, -1, 1, 1);
	mat4_affine(palette + 16, a, 0, 0, 0);
	expect_rejected("singular blended linear part", position, normal,
		tangent, joints, weights, palette, 2);

	/* zero linear part: s == 0 */
	lin_diag(a, 0, 0, 0);
	mat4_affine(palette + 0, a, 0, 0, 0);
	expect_rejected("zero blended linear part", position, normal, tangent,
		j1, w1, palette, 1);

	/* extreme anisotropy: |det| <= 64 * MFLT_EPSILON * s^3 */
	lin_diag(a, 1, MFLOAT_C(1.0e8), 1);
	mat4_affine(palette + 0, a, 0, 0, 0);
	expect_rejected("near-degenerate blended linear part", position, normal,
		tangent, j1, w1, palette, 1);
}

int main(void)
{
	test_scale_blend();
	test_rotation_translation();
	test_uniform_translation();
	test_repeated_joints();
	test_zero_weight_joints();
	test_four_joints();
	test_rotation_scale_mix();
	test_reflection();
	test_in_place();
	test_invalid_inputs();
	test_degenerate_blend();
	if (g_failures != 0) {
		printf("check_skin: %d failure(s)\n", g_failures);
		return 1;
	}
#if defined(MATHC_USE_DOUBLE_FLOATING_POINT)
	printf("check_skin: all tests passed (double)\n");
#else
	printf("check_skin: all tests passed (float)\n");
#endif
	return 0;
}
