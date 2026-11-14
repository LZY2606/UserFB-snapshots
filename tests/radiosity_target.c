//=====================================================================
//
// radiosity_target.c - task tests for the hemicube radiosity module
//
// Three groups are registered:
//   visibility  - independent per-pixel ray/triangle hemicube reference
//   transport   - analytic areas plus an independent F driven recursion
//   integration - split steps, reset, dual scenes, draw pipeline
//
// This translation unit provides task_cases/task_case_count and never
// defines main().
//
//=====================================================================
#include "task_support/check.h"
#include "radiosity.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#define RT_PI 3.14159265358979323846

//---------------------------------------------------------------------
// local test geometry helpers (independent from the engine internals)
//---------------------------------------------------------------------
static void rt_vset(point_t *p, float x, float y, float z) {
	p->x = x; p->y = y; p->z = z; p->w = 1.0f;
}

static void rt_cset(color_t *c, float r, float g, float b) {
	c->r = r; c->g = g; c->b = b;
}

static void rt_sub(vector_t *z, const vector_t *a, const vector_t *b) {
	z->x = a->x - b->x; z->y = a->y - b->y; z->z = a->z - b->z; z->w = 1.0f;
}

static void rt_cross(vector_t *z, const vector_t *a, const vector_t *b) {
	z->x = a->y * b->z - a->z * b->y;
	z->y = a->z * b->x - a->x * b->z;
	z->z = a->x * b->y - a->y * b->x;
	z->w = 1.0f;
}

static float rt_dot(const vector_t *a, const vector_t *b) {
	return a->x * b->x + a->y * b->y + a->z * b->z;
}

static float rt_len(const vector_t *a) {
	return sqrtf(rt_dot(a, a));
}

static void rt_norm(vector_t *a) {
	float l = rt_len(a);
	a->x /= l; a->y /= l; a->z /= l; a->w = 1.0f;
}

static float rt_tri_area(const radiosity_patch *p) {
	vector_t e1, e2, cr;
	rt_sub(&e1, &p->v[1], &p->v[0]);
	rt_sub(&e2, &p->v[2], &p->v[0]);
	rt_cross(&cr, &e1, &e2);
	return 0.5f * rt_len(&cr);
}

static void rt_normal(const radiosity_patch *p, vector_t *n) {
	vector_t e1, e2;
	rt_sub(&e1, &p->v[1], &p->v[0]);
	rt_sub(&e2, &p->v[2], &p->v[0]);
	rt_cross(n, &e1, &e2);
	rt_norm(n);
}

//---------------------------------------------------------------------
// independent two sided ray/triangle intersection (Moller-Trumbore)
//---------------------------------------------------------------------
static int rt_ray_tri(const point_t *ro, const vector_t *d,
	const radiosity_patch *p, float *out_t) {
	vector_t e1, e2, h, s, q;
	float a, f, u, v, t;
	rt_sub(&e1, &p->v[1], &p->v[0]);
	rt_sub(&e2, &p->v[2], &p->v[0]);
	rt_cross(&h, d, &e2);
	a = rt_dot(&e1, &h);
	if (fabsf(a) < 1e-9f) return 0;
	f = 1.0f / a;
	rt_sub(&s, ro, &p->v[0]);
	u = f * rt_dot(&s, &h);
	if (u < 0.0f || u > 1.0f) return 0;
	rt_cross(&q, &s, &e1);
	v = f * rt_dot(d, &q);
	if (v < 0.0f || u + v > 1.0f) return 0;
	t = f * rt_dot(&e2, &q);
	if (t <= 1e-7f) return 0;
	*out_t = t;
	return 1;
}

//---------------------------------------------------------------------
// reference hemicube: same published cell formulas and one shared
// discrete normalization, but visibility resolved by exact rays
//---------------------------------------------------------------------
typedef struct {
	size_t count;
	int resolution;
	float offset;
	const radiosity_patch *patches;
} rt_ref_scene;
static void rt_basis(const vector_t *n, vector_t *t, vector_t *b) {
	vector_t helper;
	if (fabsf(n->y) < 0.9f)
		rt_vset(&helper, 0.0f, 1.0f, 0.0f);
	else
		rt_vset(&helper, 1.0f, 0.0f, 0.0f);
	rt_cross(t, &helper, n);
	rt_norm(t);
	rt_cross(b, n, t);
}

static float rt_raw_top(float cell, float x, float y) {
	float d = 1.0f + x * x + y * y;
	return cell * cell / (RT_PI * d * d);
}

static float rt_raw_side(float cell, float x, float z) {
	float d = 1.0f + x * x + z * z;
	return cell * cell * z / (RT_PI * d * d);
}

// returns 1 when patch j is front facing the hemicube origin
static int rt_front(const rt_ref_scene *rs, size_t j, const point_t *eye) {
	vector_t n, to;
	rt_normal(&rs->patches[j], &n);
	rt_sub(&to, eye, &rs->patches[j].v[0]);
	return rt_dot(&n, &to) > 0.0f;
}

// cast one ray and return nearest hit index or SIZE_MAX; back faces
// occlude exactly like front faces. Hits closer than the hemicube near
// plane (offset / 4) are discarded, matching the engine clipping
static size_t rt_cast(const rt_ref_scene *rs, size_t source,
	const point_t *eye, const vector_t *dir) {
	size_t j, best = SIZE_MAX;
	float best_t = FLT_MAX;
	float near_plane = rs->offset * 0.25f;
	for (j = 0; j < rs->count; j++) {
		float t;
		if (j == source) continue;
		if (rt_ray_tri(eye, dir, &rs->patches[j], &t)) {
			if (t < near_plane) continue;
			if (t < best_t) { best_t = t; best = j; }
		}
	}
	return best;
}

static void rt_reference_row(const rt_ref_scene *rs, size_t source,
	float *ref, int face_hits[5]) {
	int res = rs->resolution, half = res / 2;
	float cell = 2.0f / (float)res;
	float total = 0.0f, inv;
	vector_t n, t, b;
	point_t eye;
	int px, py;

	rt_normal(&rs->patches[source], &n);
	rt_basis(&n, &t, &b);
	rt_vset(&eye,
		(rs->patches[source].v[0].x + rs->patches[source].v[1].x +
		 rs->patches[source].v[2].x) / 3.0f + n.x * rs->offset,
		(rs->patches[source].v[0].y + rs->patches[source].v[1].y +
		 rs->patches[source].v[2].y) / 3.0f + n.y * rs->offset,
		(rs->patches[source].v[0].z + rs->patches[source].v[1].z +
		 rs->patches[source].v[2].z) / 3.0f + n.z * rs->offset);

	for (py = 0; py < res; py++)
		for (px = 0; px < res; px++) {
			float x = -1.0f + ((float)px + 0.5f) * cell;
			float y = 1.0f - ((float)py + 0.5f) * cell;
			total += rt_raw_top(cell, x, y);
		}
	for (py = 0; py < half; py++)
		for (px = 0; px < res; px++) {
			float x = -1.0f + ((float)px + 0.5f) * cell;
			float z = 1.0f - ((float)py + 0.5f) * cell;
			total += 4.0f * rt_raw_side(cell, x, z);
		}
	inv = 1.0f / total;
	memset(ref, 0, sizeof(float) * rs->count);
	memset(face_hits, 0, sizeof(int) * 5);

	// top face: direction n + x*t + y*b
	for (py = 0; py < res; py++)
		for (px = 0; px < res; px++) {
			float x = -1.0f + ((float)px + 0.5f) * cell;
			float y = 1.0f - ((float)py + 0.5f) * cell;
			vector_t dir;
			size_t id;
			dir.x = n.x + x * t.x + y * b.x;
			dir.y = n.y + x * t.y + y * b.y;
			dir.z = n.z + x * t.z + y * b.z;
			dir.w = 1.0f;
			id = rt_cast(rs, source, &eye, &dir);
			if (id != SIZE_MAX) {
				face_hits[0]++;
				if (rt_front(rs, id, &eye))
					ref[id] += rt_raw_top(cell, x, y) * inv;
			}
		}

	// four side faces: facing f, right cross(n, f), up n; the upper
	// half uses z in (0, 1]
	{
		vector_t face[4];
		int fi;
		face[0] = t;
		face[1].x = -t.x; face[1].y = -t.y; face[1].z = -t.z; face[1].w = 1.0f;
		face[2] = b;
		face[3].x = -b.x; face[3].y = -b.y; face[3].z = -b.z; face[3].w = 1.0f;
		for (fi = 0; fi < 4; fi++) {
			vector_t right;
			rt_cross(&right, &n, &face[fi]);
			for (py = 0; py < half; py++)
				for (px = 0; px < res; px++) {
					float x = -1.0f + ((float)px + 0.5f) * cell;
					float z = 1.0f - ((float)py + 0.5f) * cell;
					vector_t dir;
					size_t id;
					dir.x = face[fi].x + x * right.x + z * n.x;
					dir.y = face[fi].y + x * right.y + z * n.y;
					dir.z = face[fi].z + x * right.z + z * n.z;
					dir.w = 1.0f;
					id = rt_cast(rs, source, &eye, &dir);
					if (id != SIZE_MAX) {
						face_hits[fi + 1]++;
						if (rt_front(rs, id, &eye))
							ref[id] += rt_raw_side(cell, x, z) * inv;
					}
				}
		}
	}
}

//---------------------------------------------------------------------
// visibility scene: ceiling, four walls, a back face occluder hiding
// a front patch, two stacked front layers, and a tilted triangle
// crossing the near plane; oversized parts cross the side boundaries
//---------------------------------------------------------------------
#define V_COUNT 10
#define V_RES 16
#define V_OFFSET 0.5f

static void vt_patch(radiosity_patch *p,
	float x0, float y0, float z0,
	float x1, float y1, float z1,
	float x2, float y2, float z2,
	float rr, float gg, float bb) {
	rt_vset(&p->v[0], x0, y0, z0);
	rt_vset(&p->v[1], x1, y1, z1);
	rt_vset(&p->v[2], x2, y2, z2);
	rt_cset(&p->reflectance, rr, gg, bb);
	rt_cset(&p->emission, 0.0f, 0.0f, 0.0f);
}

static radiosity_scene *vt_build(radiosity_patch *p) {
	memset(p, 0, sizeof(radiosity_patch) * V_COUNT);
	// 0: source floor patch, normal +Y
	vt_patch(&p[0], -1, 0, 1, 1, 0, 1, 1, 0, -1, 0.5f, 0.5f, 0.5f);
	rt_cset(&p[0].emission, 1.0f, 1.0f, 1.0f);
	// 1: oversized ceiling facing down (-Y), crosses top side bounds
	vt_patch(&p[1], -8, 3, -8, 8, 3, -8, 8, 3, 8, 0.6f, 0.6f, 0.6f);
	// 2..5: four inward walls far outside the 45 degree side frusta
	vt_patch(&p[2], 4, -1, -4, 4, -1, 4, 4, 5, 4, 0.4f, 0.4f, 0.4f);   // +X, normal -X
	vt_patch(&p[3], -4, -1, 4, -4, -1, -4, -4, 5, -4, 0.4f, 0.4f, 0.4f); // -X, normal +X
	vt_patch(&p[4], -4, -1, 4, 4, 5, 4, 4, -1, 4, 0.4f, 0.4f, 0.4f);   // +Z, normal -Z
	vt_patch(&p[5], 4, -1, -4, -4, 5, -4, -4, -1, -4, 0.4f, 0.4f, 0.4f); // -Z, normal +Z
	// 6: back facing patch (normal +Y) above the eye, absorbs only
	vt_patch(&p[6], -0.8f, 1.5f, -0.8f, 0.8f, 1.5f, 0.8f, 0.8f, 1.5f, -0.8f,
		0.3f, 0.3f, 0.3f);
	// 7: front patch behind 6, part of it is hidden by the back face
	vt_patch(&p[7], -0.8f, 2.5f, 0.8f, 0.8f, 2.5f, -0.8f, 0.8f, 2.5f, 0.8f,
		0.3f, 0.3f, 0.3f);
	// 8: tilted front patch with a vertex inside the near plane
	vt_patch(&p[8], 0.35f, 0.52f, 0.35f, 1.8f, 0.9f, 0.4f, 0.4f, 0.9f, 1.8f,
		0.35f, 0.35f, 0.35f);
	// 9: second front layer between eye and ceiling along some rays
	vt_patch(&p[9], -2.5f, 2.0f, -2.5f, -1.2f, 2.0f, -2.5f, -1.2f, 2.0f, -1.2f,
		0.35f, 0.35f, 0.35f);
	return radiosity_create(p, V_COUNT, V_RES, V_OFFSET);
}

static int visibility_reference(void) {
	radiosity_patch p[V_COUNT];
	radiosity_scene *rs = vt_build(p);
	rt_ref_scene ref_scene;
	size_t sources[2] = { 0, 9 };
	size_t si, j;
	TASK_CHECK(rs != NULL);
	ref_scene.count = V_COUNT;
	ref_scene.resolution = V_RES;
	ref_scene.offset = V_OFFSET;
	ref_scene.patches = p;
	for (si = 0; si < 2; si++) {
		size_t source = sources[si];
		float *ref = (float *)calloc(V_COUNT, sizeof(float));
		int face_hits[5];
		float engine_sum = 0.0f, ref_sum = 0.0f;
		rt_reference_row(&ref_scene, source, ref, face_hits);
		for (j = 0; j < 5; j++)
			TASK_CHECK(face_hits[j] > 0);              // every face sees geometry
		for (j = 0; j < V_COUNT; j++) {
			float f = radiosity_factor(rs, source, j);
			TASK_CHECK(f >= 0.0f && f <= 1.0f);
			TASK_CHECK(fabsf(f - ref[j]) <= 0.025f ||
				(ref[j] > 0.05f && fabsf(f - ref[j]) <= 0.15f * ref[j]));
			engine_sum += f;
			ref_sum += ref[j];
		}
		TASK_CHECK(fabsf(engine_sum - ref_sum) <= 0.02f);
		TASK_CHECK(engine_sum <= 1.0f + 1e-6f);
		TASK_CHECK(radiosity_factor(rs, source, source) == 0.0f);
		free(ref);
	}
	radiosity_destroy(rs);
	return 0;
}

static int visibility_backface(void) {
	radiosity_patch p[V_COUNT];
	radiosity_scene *rs = vt_build(p);
	point_t eye;
	vector_t n;
	TASK_CHECK(rs != NULL);
	rt_normal(&p[0], &n);
	rt_vset(&eye, 1.0f / 3.0f, V_OFFSET, 1.0f / 3.0f);
	// patch 6 is back facing the hemicube origin and may receive no
	// weight even though it occludes patch 7
	TASK_CHECK(!rt_front(&(rt_ref_scene){ V_COUNT, V_RES, V_OFFSET, p }, 6, &eye));
	TASK_CHECK(radiosity_factor(rs, 0, 6) == 0.0f);
	// the occluded front patch keeps a positive but reduced view: the
	// ceiling alone would give every upper cell a destination
	TASK_CHECK(radiosity_factor(rs, 0, 1) > 0.05f);
	TASK_CHECK(radiosity_factor(rs, 0, 7) >= 0.0f);
	// a real front facing patch does collect energy
	TASK_CHECK(radiosity_factor(rs, 0, 8) > 0.0f);
	radiosity_destroy(rs);
	return 0;
}

//---------------------------------------------------------------------
// transport scene: analytic areas, distinct per-channel reflectance,
// patches facing the emitter along different directions
//---------------------------------------------------------------------
#define T_COUNT 4
#define T_RES 16
#define T_OFFSET 0.25f

static radiosity_scene *tt_build(radiosity_patch *p) {
	memset(p, 0, sizeof(radiosity_patch) * T_COUNT);
	// emitter at z=0, normal +Z, area 2
	vt_patch(&p[0], -1, -1, 0, 1, -1, 0, 1, 1, 0, 0.0f, 0.0f, 0.0f);
	rt_cset(&p[0].emission, 1.0f, 2.0f, 3.0f);
	// small receiver area 0.5 at z=2, normal -Z
	vt_patch(&p[1], 0, 0, 2, 1, 1, 2, 1, 0, 2, 0.7f, 0.2f, 0.1f);
	// receiver area 2 at z=4, normal -Z
	vt_patch(&p[2], -1, -1, 4, -1, 1, 4, 1, 1, 4, 0.1f, 0.7f, 0.2f);
	// side receiver area 2 at x=3, normal -X
	vt_patch(&p[3], 3, -1, 0, 3, 1, 2, 3, 1, 0, 0.2f, 0.1f, 0.7f);
	return radiosity_create(p, T_COUNT, T_RES, T_OFFSET);
}

static int transport_areas(void) {
	radiosity_patch p[T_COUNT];
	radiosity_scene *rs = tt_build(p);
	float expect_area[T_COUNT] = { 2.0f, 0.5f, 2.0f, 2.0f };
	size_t i, j;
	TASK_CHECK(rs != NULL);
	for (i = 0; i < T_COUNT; i++) {
		float area;
		color_t B, U;
		TASK_CHECK(radiosity_get(rs, i, &B, &U, &area) == 1);
		TASK_CHECK(fabsf(area - expect_area[i]) < 1e-5f);
		TASK_CHECK(fabsf(area - rt_tri_area(&p[i])) < 1e-5f);
		TASK_CHECK(fabsf(B.r - p[i].emission.r) < 1e-7f);
		TASK_CHECK(fabsf(U.g - p[i].emission.g) < 1e-7f);
	}
	for (i = 0; i < T_COUNT; i++) {
		float sum = 0.0f;
		for (j = 0; j < T_COUNT; j++) {
			float f = radiosity_factor(rs, i, j);
			TASK_CHECK(f >= 0.0f);
			sum += f;
		}
		TASK_CHECK(sum <= 1.0f + 1e-6f);
		TASK_CHECK(radiosity_factor(rs, i, i) == 0.0f);
	}
	TASK_CHECK(radiosity_factor(rs, 0, T_COUNT) == 0.0f);
	radiosity_destroy(rs);
	return 0;
}

// independent double precision recursion driven purely by the public
// factor matrix and analytic areas
static void tt_simulate(radiosity_patch *p, size_t steps,
	double *Bo, double *Uo, size_t *shooters) {
	double area[T_COUNT], B[T_COUNT][3], U[T_COUNT][3];
	double F[T_COUNT][T_COUNT];
	size_t s, i, j, k;
	radiosity_scene *probe = tt_build(p);
	for (i = 0; i < T_COUNT; i++) {
		float a;
		radiosity_get(probe, i, NULL, NULL, &a);
		area[i] = a;
		for (k = 0; k < 3; k++) {
			B[i][k] = U[i][k] = (k == 0) ? p[i].emission.r :
				(k == 1 ? p[i].emission.g : p[i].emission.b);
		}
	}
	for (i = 0; i < T_COUNT; i++)
		for (j = 0; j < T_COUNT; j++)
			F[i][j] = radiosity_factor(probe, i, j);
	radiosity_destroy(probe);
	for (s = 0; s < steps; s++) {
		size_t shooter = SIZE_MAX;
		double best = 0.0;
		for (i = 0; i < T_COUNT; i++) {
			double e = area[i] * (U[i][0] + U[i][1] + U[i][2]);
			if (e > best) { best = e; shooter = i; }
		}
		shooters[s] = shooter;
		if (shooter == SIZE_MAX) break;
		for (j = 0; j < T_COUNT; j++) {
			double ratio = area[shooter] / area[j] * F[shooter][j];
			double rho[3] = { p[j].reflectance.r, p[j].reflectance.g,
				p[j].reflectance.b };
			for (k = 0; k < 3; k++) {
				double add = rho[k] * U[shooter][k] * ratio;
				B[j][k] += add;
				U[j][k] += add;
			}
		}
		for (k = 0; k < 3; k++) U[shooter][k] = 0.0;
	}
	for (i = 0; i < T_COUNT; i++) {
		Bo[i * 3 + 0] = B[i][0]; Bo[i * 3 + 1] = B[i][1]; Bo[i * 3 + 2] = B[i][2];
		Uo[i * 3 + 0] = U[i][0]; Uo[i * 3 + 1] = U[i][1]; Uo[i * 3 + 2] = U[i][2];
	}
}

static int transport_recursion(void) {
	radiosity_patch p[T_COUNT];
	radiosity_scene *rs = tt_build(p);
	double refB[T_COUNT * 3], refU[T_COUNT * 3];
	size_t shooters[8];
	size_t i, k, steps = 6;
	TASK_CHECK(rs != NULL);
	TASK_CHECK(radiosity_last_shooter(rs) == SIZE_MAX);
	TASK_CHECK(radiosity_step(rs, steps) == steps);
	tt_simulate(p, steps, refB, refU, shooters);
	TASK_CHECK(radiosity_last_shooter(rs) == shooters[steps - 1]);
	for (i = 0; i < T_COUNT; i++) {
		color_t B, U;
		radiosity_get(rs, i, &B, &U, NULL);
		for (k = 0; k < 3; k++) {
			float bv = (k == 0) ? B.r : (k == 1 ? B.g : B.b);
			float uv = (k == 0) ? U.r : (k == 1 ? U.g : U.b);
			TASK_CHECK(fabs(bv - (float)refB[i * 3 + k]) <=
				1e-3f + 1e-4f * fabsf(bv));
			TASK_CHECK(fabs(uv - refU[i * 3 + k]) <=
				1e-3f + 1e-4f * fabs(uv));
		}
	}
	radiosity_destroy(rs);
	return 0;
}

static int transport_color_bleed(void) {
	radiosity_patch p[T_COUNT];
	radiosity_scene *rs = tt_build(p);
	color_t B1;
	float f01, area0, area1;
	float expect[3], got[3];
	TASK_CHECK(rs != NULL);
	TASK_CHECK(radiosity_step(rs, 1) == 1);
	TASK_CHECK(radiosity_last_shooter(rs) == 0);
	radiosity_get(rs, 0, NULL, NULL, &area0);
	radiosity_get(rs, 1, &B1, NULL, &area1);
	f01 = radiosity_factor(rs, 0, 1);
	TASK_CHECK(f01 > 0.0f);
	// receiver started black: B now equals one reflected bounce only
	expect[0] = p[1].reflectance.r * p[0].emission.r * (area0 / area1) * f01;
	expect[1] = p[1].reflectance.g * p[0].emission.g * (area0 / area1) * f01;
	expect[2] = p[1].reflectance.b * p[0].emission.b * (area0 / area1) * f01;
	got[0] = B1.r; got[1] = B1.g; got[2] = B1.b;
	TASK_CHECK(fabsf(got[0] - expect[0]) < 1e-6f);
	TASK_CHECK(fabsf(got[1] - expect[1]) < 1e-6f);
	TASK_CHECK(fabsf(got[2] - expect[2]) < 1e-6f);
	// per channel reflectance ordering gives the expected color bleed
	TASK_CHECK(got[0] > got[1] && got[1] > got[2]);
	// shooter was fully drained
	{
		color_t U0;
		radiosity_get(rs, 0, NULL, &U0, NULL);
		TASK_CHECK(U0.r == 0.0f && U0.g == 0.0f && U0.b == 0.0f);
	}
	radiosity_destroy(rs);
	return 0;
}

static int transport_shooter_pick(void) {
	radiosity_patch p[T_COUNT];
	radiosity_scene *rs = tt_build(p);
	double refB[1], refU[1];
	size_t shooters[8];
	size_t s;
	TASK_CHECK(rs != NULL);
	tt_simulate(p, 5, refB, refU, shooters);
	for (s = 0; s < 5; s++) {
		TASK_CHECK(radiosity_step(rs, 1) == 1);
		TASK_CHECK(radiosity_last_shooter(rs) == shooters[s]);
	}
	radiosity_destroy(rs);
	return 0;
}

//---------------------------------------------------------------------
// integration scene: emitter, two receivers and an occluder
//---------------------------------------------------------------------
#define I_COUNT 5
#define I_RES 12
#define I_OFFSET 0.3f

static radiosity_scene *it_build(radiosity_patch *p) {
	memset(p, 0, sizeof(radiosity_patch) * I_COUNT);
	vt_patch(&p[0], -1, 2, -1, 1, 2, -1, 1, 2, 1, 0.0f, 0.0f, 0.0f); // emitter -Y
	rt_cset(&p[0].emission, 2.0f, 1.0f, 0.5f);
	vt_patch(&p[1], -2, 0, 2, 2, 0, 2, 2, 0, -2, 0.6f, 0.3f, 0.2f); // floor +Y
	vt_patch(&p[2], 3, 0, -2, 3, 0, 2, 3, 3, 2, 0.2f, 0.6f, 0.3f);   // wall -X
	vt_patch(&p[3], -0.5f, 1.0f, -0.5f, 0.5f, 1.0f, -0.5f, 0.5f, 1.0f, 0.5f,
		0.3f, 0.3f, 0.3f);                                            // occluder +Y
	vt_patch(&p[4], -2, 1, 2.5f, 2, 1, 2.5f, 2, 2.5f, 2.5f, 0.25f, 0.25f, 0.5f);
	return radiosity_create(p, I_COUNT, I_RES, I_OFFSET);
}

static int integration_split(void) {
	radiosity_patch pa[I_COUNT], pb[I_COUNT];
	radiosity_scene *a = it_build(pa);
	radiosity_scene *b = it_build(pb);
	size_t i;
	TASK_CHECK(a != NULL && b != NULL);
	TASK_CHECK(radiosity_step(a, 5) == 5);
	TASK_CHECK(radiosity_step(b, 2) == 2);
	TASK_CHECK(radiosity_step(b, 3) == 3);
	for (i = 0; i < I_COUNT; i++) {
		color_t Ba, Ua, Bb, Ub;
		radiosity_get(a, i, &Ba, &Ua, NULL);
		radiosity_get(b, i, &Bb, &Ub, NULL);
		TASK_CHECK(memcmp(&Ba, &Bb, sizeof(color_t)) == 0);
		TASK_CHECK(memcmp(&Ua, &Ub, sizeof(color_t)) == 0);
	}
	TASK_CHECK(radiosity_last_shooter(a) == radiosity_last_shooter(b));
	radiosity_destroy(a);
	radiosity_destroy(b);
	return 0;
}

static int integration_reset(void) {
	radiosity_patch p[I_COUNT];
	radiosity_scene *rs = it_build(p);
	float f_before, f_after;
	size_t i;
	TASK_CHECK(rs != NULL);
	TASK_CHECK(radiosity_step(rs, 4) == 4);
	f_before = radiosity_factor(rs, 1, 2);
	radiosity_reset(rs);
	TASK_CHECK(radiosity_last_shooter(rs) == SIZE_MAX);
	f_after = radiosity_factor(rs, 1, 2);
	TASK_CHECK(f_after == f_before);               // matrix preserved
	for (i = 0; i < I_COUNT; i++) {
		color_t B, U;
		radiosity_get(rs, i, &B, &U, NULL);
		TASK_CHECK(memcmp(&B, &p[i].emission, sizeof(color_t)) == 0);
		TASK_CHECK(memcmp(&U, &p[i].emission, sizeof(color_t)) == 0);
	}
	// solution reproduces after restarting
	{
		radiosity_patch p2[I_COUNT];
		radiosity_scene *fresh = it_build(p2);
		TASK_CHECK(radiosity_step(fresh, 4) == 4);
		TASK_CHECK(radiosity_step(rs, 4) == 4);
		for (i = 0; i < I_COUNT; i++) {
			color_t B1, B2;
			radiosity_get(rs, i, &B1, NULL, NULL);
			radiosity_get(fresh, i, &B2, NULL, NULL);
			TASK_CHECK(memcmp(&B1, &B2, sizeof(color_t)) == 0);
		}
		radiosity_destroy(fresh);
	}
	radiosity_destroy(rs);
	return 0;
}

static int integration_dual(void) {
	radiosity_patch pa[I_COUNT], pb[I_COUNT];
	radiosity_scene *a = it_build(pa);
	radiosity_scene *b = it_build(pb);
	size_t i;
	TASK_CHECK(a != NULL && b != NULL);
	radiosity_step(a, 1);
	radiosity_step(b, 3);
	radiosity_step(a, 2);
	radiosity_step(b, 1);
	TASK_CHECK(radiosity_last_shooter(a) != radiosity_last_shooter(b) ||
		radiosity_last_shooter(a) != SIZE_MAX);
	for (i = 0; i < I_COUNT; i++) {
		color_t Ba, Bb;
		radiosity_get(a, i, &Ba, NULL, NULL);
		radiosity_get(b, i, &Bb, NULL, NULL);
		// a has run 3 steps, b 4 steps; states must be allowed to differ
		TASK_CHECK(isfinite(Ba.r) && isfinite(Bb.r));
	}
	// advancing one scene never perturbs the other's trajectory:
	// interleaved steps match contiguous steps on both scenes
	{
		radiosity_patch pc[I_COUNT], pd[I_COUNT];
		radiosity_scene *c = it_build(pc);
		radiosity_scene *d = it_build(pd);
		radiosity_step(c, 4);
		radiosity_step(d, 3);
		for (i = 0; i < I_COUNT; i++) {
			color_t Ba, Bb, Bc, Bd;
			radiosity_get(a, i, &Ba, NULL, NULL);
			radiosity_get(b, i, &Bb, NULL, NULL);
			radiosity_get(c, i, &Bc, NULL, NULL);
			radiosity_get(d, i, &Bd, NULL, NULL);
			TASK_CHECK(memcmp(&Bb, &Bc, sizeof(color_t)) == 0);
			TASK_CHECK(memcmp(&Ba, &Bd, sizeof(color_t)) == 0);
		}
		TASK_CHECK(radiosity_last_shooter(b) == radiosity_last_shooter(c));
		TASK_CHECK(radiosity_last_shooter(a) == radiosity_last_shooter(d));
		radiosity_destroy(c);
		radiosity_destroy(d);
	}
	radiosity_destroy(a);
	radiosity_destroy(b);
	return 0;
}

static void it_camera(device_t *d) {
	point_t eye = { 0.0f, -6.0f, 2.5f, 1.0f };
	point_t at = { 0.0f, 1.0f, 0.5f, 1.0f };
	point_t up = { 0.0f, 0.0f, 1.0f, 1.0f };
	matrix_set_lookat(&d->transform.view, &eye, &at, &up);
	transform_update(&d->transform);
}

static int integration_draw(void) {
	radiosity_patch p[I_COUNT];
	radiosity_scene *rs = it_build(p);
	device_t a, b, c;
	int x, y;
	matrix_t saved_world, saved_view, saved_proj;
	int saved_state;
	TASK_CHECK(rs != NULL);
	radiosity_step(rs, 3);

	device_init(&a, 48, 48, NULL);
	device_init(&b, 48, 48, NULL);
	device_init(&c, 48, 48, NULL);
	device_clear(&a, 0);
	device_clear(&b, 0);
	it_camera(&a);
	it_camera(&b);
	it_camera(&c);

	saved_state = RENDER_STATE_TEXTURE;
	a.render_state = RENDER_STATE_TEXTURE;
	matrix_set_rotate(&a.transform.world, 1, 0, 0, 0.7f);
	transform_update(&a.transform);
	// capture the exact caller state radiosity_draw must restore
	saved_world = a.transform.world;
	saved_view = a.transform.view;
	saved_proj = a.transform.projection;

	radiosity_draw(rs, &a, 0.5f);

	// transform and render state restored, caller colors not cleared
	TASK_CHECK(a.render_state == saved_state);
	TASK_CHECK(memcmp(&a.transform.world, &saved_world, sizeof(matrix_t)) == 0);
	TASK_CHECK(memcmp(&a.transform.view, &saved_view, sizeof(matrix_t)) == 0);
	TASK_CHECK(memcmp(&a.transform.projection, &saved_proj, sizeof(matrix_t)) == 0);

	// independent reference drawing through the same public pipeline
	matrix_set_identity(&b.transform.world);
	transform_update(&b.transform);
	b.render_state = RENDER_STATE_COLOR;
	for (x = 0; x < (int)I_COUNT; x++) {
		vertex_t v[3];
		color_t B, col;
		int k;
		radiosity_get(rs, (size_t)x, &B, NULL, NULL);
		col.r = B.r * 0.5f; col.g = B.g * 0.5f; col.b = B.b * 0.5f;
		for (k = 0; k < 3; k++) {
			memset(&v[k], 0, sizeof(vertex_t));
			v[k].pos = p[x].v[k];
			v[k].color = col;
		}
		device_draw_primitive(&b, &v[0], &v[1], &v[2]);
	}
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++)
			TASK_CHECK(a.framebuffer[y][x] == b.framebuffer[y][x]);

	// untouched framebuffer and depth cells keep their caller values
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++) {
			c.framebuffer[y][x] = 0x123456u;
			c.zbuffer[y][x] = 0.0f;
		}
	c.zbuffer[0][0] = 0.75f;
	radiosity_draw(rs, &c, 0.5f);
	TASK_CHECK(c.zbuffer[0][0] == 0.75f);
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++) {
			if (b.framebuffer[y][x] == b.background)
				TASK_CHECK(c.framebuffer[y][x] == 0x123456u);
		}

	device_destroy(&a);
	device_destroy(&b);
	device_destroy(&c);
	radiosity_destroy(rs);
	return 0;
}

static int integration_validation(void) {
	radiosity_patch good[2], bad[2];
	radiosity_scene *rs;
	color_t B;
	TASK_CHECK(radiosity_create(NULL, 1, I_RES, I_OFFSET) == NULL);
	TASK_CHECK(radiosity_create(good, 0, I_RES, I_OFFSET) == NULL);
	TASK_CHECK(radiosity_create(good, 1, 3, I_OFFSET) == NULL);
	TASK_CHECK(radiosity_create(good, 1, 5, I_OFFSET) == NULL);
	TASK_CHECK(radiosity_create(good, 1, I_RES, 0.0f) == NULL);
	TASK_CHECK(radiosity_create(good, 1, I_RES, -1.0f) == NULL);

	memset(good, 0, sizeof(good));
	vt_patch(&good[0], 0, 0, 0, 1, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0.5f);
	vt_patch(&good[1], 0, 0, 2, 1, 0, 2, 0, 1, 2, 0.5f, 0.5f, 0.5f);
	rs = radiosity_create(good, 2, I_RES, I_OFFSET);
	TASK_CHECK(rs != NULL);
	TASK_CHECK(radiosity_get(rs, 2, &B, NULL, NULL) == 0);
	TASK_CHECK(radiosity_get(rs, 0, NULL, NULL, NULL) == 1);
	radiosity_destroy(rs);

	memcpy(bad, good, sizeof(good));
	bad[0].v[1].w = 0.5f;
	TASK_CHECK(radiosity_create(bad, 2, I_RES, I_OFFSET) == NULL);

	memcpy(bad, good, sizeof(good));
	bad[0].v[1].x = NAN;
	TASK_CHECK(radiosity_create(bad, 2, I_RES, I_OFFSET) == NULL);

	memcpy(bad, good, sizeof(good));
	rt_vset(&bad[0].v[1], 0, 0, 0);       // degenerate
	TASK_CHECK(radiosity_create(bad, 2, I_RES, I_OFFSET) == NULL);

	memcpy(bad, good, sizeof(good));
	bad[1].reflectance.r = 1.0f;          // must be strictly below 1
	TASK_CHECK(radiosity_create(bad, 2, I_RES, I_OFFSET) == NULL);

	memcpy(bad, good, sizeof(good));
	bad[1].emission.b = -0.1f;
	TASK_CHECK(radiosity_create(bad, 2, I_RES, I_OFFSET) == NULL);

	TASK_CHECK(radiosity_factor(NULL, 0, 0) == 0.0f);
	radiosity_destroy(NULL);
	radiosity_reset(NULL);
	return 0;
}

//---------------------------------------------------------------------
// registration
//---------------------------------------------------------------------
const struct task_case task_cases[] = {
	{ "visibility", "hemicube_ray_reference", visibility_reference },
	{ "visibility", "backface_absorbs", visibility_backface },
	{ "transport", "analytic_areas_and_rows", transport_areas },
	{ "transport", "independent_recursion", transport_recursion },
	{ "transport", "color_bleed_one_bounce", transport_color_bleed },
	{ "transport", "shooter_selection", transport_shooter_pick },
	{ "integration", "split_step_equivalence", integration_split },
	{ "integration", "reset_keeps_matrix", integration_reset },
	{ "integration", "dual_independent_scenes", integration_dual },
	{ "integration", "draw_uses_original_pipeline", integration_draw },
	{ "integration", "input_validation", integration_validation },
};
const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
