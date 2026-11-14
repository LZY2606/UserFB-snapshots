//
//  ray_spawn.c
//  c-ray
//
//  Regression tests for the surface ray overhaul:
//    parameter  - intersection parameters are true ray t values, hit points and
//                 conservative point errors are checked against a double reference
//    spawn      - spawn_surface_ray() side selection and finite error-based origins
//    transport  - real path_trace() reflection/transmission paths
//
//  Copyright © 2025 Valtteri Koskivuori. All rights reserved.
//

#include <v.h>

#include <math.h>
#include <float.h>
#include <stdlib.h>

#include <common/vector.h>
#include <common/transforms.h>
#include <common/hashtable.h>
#include <common/mempool.h>
#include <common/color.h>
#include <datatypes/hitrecord.h>
#include <datatypes/lightray.h>
#include <datatypes/sphere.h>
#include <datatypes/mesh.h>
#include <datatypes/poly.h>
#include <datatypes/scene.h>
#include <renderer/instance.h>
#include <renderer/ray_spawn.h>
#include <renderer/pathtrace.h>
#include <accelerators/bvh.h>
#include <renderer/samplers/sampler.h>
#include <nodes/bsdfnode.h>

#include "../task_support/check.h"

// ---------------------------------------------------------------- double reference

struct dvec { double x, y, z; };

static struct dvec dadd(struct dvec a, struct dvec b) { return (struct dvec){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static struct dvec dsub(struct dvec a, struct dvec b) { return (struct dvec){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static struct dvec dscale(struct dvec a, double s) { return (struct dvec){ a.x * s, a.y * s, a.z * s }; }
static double ddot(struct dvec a, struct dvec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double dlen(struct dvec a) { return sqrt(ddot(a, a)); }
static struct dvec dnorm(struct dvec a) { return dscale(a, 1.0 / dlen(a)); }

static struct dvec dpoint(const struct vector v) { return (struct dvec){ v.x, v.y, v.z }; }

static struct dvec dmat_point(const struct matrix4x4 m, struct dvec p) {
	return (struct dvec){
		m.mtx[0][0] * p.x + m.mtx[0][1] * p.y + m.mtx[0][2] * p.z + m.mtx[0][3],
		m.mtx[1][0] * p.x + m.mtx[1][1] * p.y + m.mtx[1][2] * p.z + m.mtx[1][3],
		m.mtx[2][0] * p.x + m.mtx[2][1] * p.y + m.mtx[2][2] * p.z + m.mtx[2][3],
	};
}

static struct dvec dmat_vec_transpose_inv(const struct matrix4x4 m, struct dvec v) {
	// Inverse-transpose linear part: (M^-1)^T v, entries taken directly from Ainv.
	return (struct dvec){
		m.mtx[0][0] * v.x + m.mtx[1][0] * v.y + m.mtx[2][0] * v.z,
		m.mtx[0][1] * v.x + m.mtx[1][1] * v.y + m.mtx[2][1] * v.z,
		m.mtx[0][2] * v.x + m.mtx[1][2] * v.y + m.mtx[2][2] * v.z,
	};
}

// Nearest positive root of |o + t d|^2 = r^2 computed entirely in double.
// t_out[1] holds the far root when the origin is inside.
static bool dsphere_roots(struct dvec o, struct dvec d, double r, double t_out[2]) {
	const double A = ddot(d, d);
	const double B = 2.0 * ddot(d, o);
	const double C = ddot(o, o) - r * r;
	const double disc = B * B - 4.0 * A * C;
	if (disc < 0.0) return false;
	const double s = sqrt(disc);
	double t0 = (-B - s) / (2.0 * A);
	double t1 = (-B + s) / (2.0 * A);
	if (t0 > t1) { const double tmp = t0; t0 = t1; t1 = tmp; }
	t_out[0] = t0;
	t_out[1] = t1;
	return true;
}

static bool float_in_error(float got, double reference, float error) {
	const float ref = (float)reference;
	const float slack = error + 2.0f * FLT_EPSILON * fabsf(ref) + FLT_MIN;
	return fabsf(got - ref) <= slack;
}

static bool error_is_sound(const struct vector e) {
	return isfinite(e.x) && isfinite(e.y) && isfinite(e.z) && e.x >= 0.0f && e.y >= 0.0f && e.z >= 0.0f;
}

static bool normal_is_unit(const struct vector n) {
	return isfinite(vec_length(n)) && fabsf(vec_length(n) - 1.0f) < 1e-6f;
}

// ------------------------------------------------------------- scene construction

static struct matrix4x4 matrix_mirror_z(void) {
	struct matrix4x4 m = mat_id();
	m.mtx[2][2] = -1.0f;
	return m;
}

static struct matrix4x4 matrix_shear_xy(float s) {
	struct matrix4x4 m = mat_id();
	m.mtx[0][1] = s;
	return m;
}

// A stored-but-never-sampled BSDF pointer keeps instance intersection happy in
// parameter/spawn tests that never run path_trace().
static struct bsdfNode dummy_bsdf = { 0 };

static struct instance make_sphere_inst(struct sphere **spheres, size_t idx, const struct bsdfNode **bsdfs) {
	struct instance inst = new_sphere_instance(spheres, idx, NULL, NULL);
	struct bsdf_buffer *buf = calloc(1, sizeof(*buf));
	static const struct bsdfNode *fallback[1] = { &dummy_bsdf };
	buf->bsdfs = bsdfs ? (const struct bsdfNode **)bsdfs : fallback;
	inst.bbuf = buf;
	return inst;
}

// Appends a fully built mesh to a contiguous v_arr, matching how production scenes
// store meshes: instance intersection indexes *arr by object_idx, so the backing
// storage must be contiguous struct mesh values, not an array of pointers.
static struct mesh *append_triangle_mesh(struct mesh **arr, bool smooth, struct vector vertices[3], struct vector normals[3]) {
	struct mesh m = { 0 };
	for (int i = 0; i < 3; ++i) v_arr_add(m.vbuf.vertices, vertices[i]);
	if (smooth)
		for (int i = 0; i < 3; ++i) v_arr_add(m.vbuf.normals, normals[i]);
	struct poly p = { 0 };
	p.vertexIndex[0] = 0; p.vertexIndex[1] = 1; p.vertexIndex[2] = 2;
	p.normalIndex[0] = 0; p.normalIndex[1] = 1; p.normalIndex[2] = 2;
	p.hasNormals = smooth;
	p.materialIndex = 0;
	v_arr_add(m.polygons, p);
	m.bvh = build_mesh_bvh(&m);
	const size_t idx = v_arr_len(*arr);
	v_arr_add(*arr, m);
	return &(*arr)[idx];
}

static struct instance make_mesh_inst(struct mesh **meshes, size_t idx, const struct bsdfNode **bsdfs, struct matrix4x4 A) {
	struct instance inst = new_mesh_instance(meshes, idx, NULL, NULL);
	struct bsdf_buffer *buf = calloc(1, sizeof(*buf));
	static const struct bsdfNode *fallback[1] = { &dummy_bsdf };
	buf->bsdfs = bsdfs ? (const struct bsdfNode **)bsdfs : fallback;
	inst.bbuf = buf;
	inst.composite = (struct transform){ .A = A, .Ainv = mat_invert(A) };
	return inst;
}

// =================================================================== parameter 1

static int parameter_sphere_nonunit_and_roots(void) {
	struct sphere spheres[1] = {{ .radius = 1.0f }};
	// Direction is deliberately not unit length.
	struct lightRay ray = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -2.0f }, .type = rt_camera };
	struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };

	TASK_CHECK(rayIntersectsWithSphere(&ray, spheres, &rec));

	double roots[2];
	TASK_CHECK(dsphere_roots((struct dvec){0,0,3}, (struct dvec){0,0,-2}, 1.0, roots));
	const double tRef = roots[0];
	TASK_CHECK(tRef > 0.0 && roots[1] > tRef);
	// distance is the parameter of r(t) = start + t*direction, not a normalized length
	TASK_CHECK(fabsf(rec.distance - (float)tRef) <= 2.0f * FLT_EPSILON * fabsf((float)tRef) + FLT_MIN);
	TASK_CHECK(fabsf(rec.distance - 1.0f) < 1e-6f);

	struct dvec hitRef = dadd(dpoint(ray.start), dscale(dpoint(ray.direction), tRef));
	TASK_CHECK(float_in_error(rec.hitPoint.x, hitRef.x, rec.pointError.x));
	TASK_CHECK(float_in_error(rec.hitPoint.y, hitRef.y, rec.pointError.y));
	TASK_CHECK(float_in_error(rec.hitPoint.z, hitRef.z, rec.pointError.z));
	TASK_CHECK(error_is_sound(rec.pointError));
	TASK_CHECK(normal_is_unit(rec.geometricNormal));
	// Front of the unit sphere at z = +1: outward normal points toward +z.
	TASK_CHECK(fabsf(rec.geometricNormal.z - 1.0f) < 1e-6f);
	return 0;
}

// ------------------------------------------------------------------- parameter 2

static int parameter_sphere_inner_root(void) {
	struct sphere spheres[1] = {{ .radius = 1.0f }};
	// Ray origin is inside the sphere: nearest root is behind us, the exit root
	// (needed for glass/transmission continuation) must be reported instead.
	struct lightRay ray = { .start = { 0.0f, 0.0f, 0.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_transmission };
	struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };

	TASK_CHECK(rayIntersectsWithSphere(&ray, spheres, &rec));
	double roots[2];
	TASK_CHECK(dsphere_roots((struct dvec){0,0,0}, (struct dvec){0,0,-1}, 1.0, roots));
	TASK_CHECK(roots[0] <= 0.0);
	TASK_CHECK(fabsf(rec.distance - (float)roots[1]) <= 2.0f * FLT_EPSILON + FLT_MIN);
	TASK_CHECK(rec.distance > 0.0f);
	TASK_CHECK(fabsf(rec.hitPoint.z + 1.0f) < 1e-6f);
	TASK_CHECK(fabsf(rec.geometricNormal.z + 1.0f) < 1e-6f);
	return 0;
}

// ------------------------------------------------------------------- parameter 3

static int parameter_triangle_geometric_normal(void) {
	// Winding chosen so the oriented edge cross product points along +z.
	struct vector verts[3] = {
		{ -1.0f, 0.0f, 0.0f },
		{  0.0f, 1.0f, 0.0f },
		{  1.0f, 0.0f, 0.0f },
	};
	// Tilted vertex normals: smoothing must not leak into geometricNormal.
	struct vector norms[3] = {
		vec_normalize((struct vector){ 0.0f, 0.5f, 1.0f }),
		vec_normalize((struct vector){ 0.0f, 0.5f, 1.0f }),
		vec_normalize((struct vector){ 0.0f, 0.5f, 1.0f }),
	};
	struct mesh *meshes = NULL;
	struct mesh *mesh = append_triangle_mesh(&meshes, true, verts, norms);

	struct lightRay ray = { .start = { 0.0f, 0.25f, 2.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(traverse_bottom_level_bvh(mesh, &ray, &rec, NULL));

	TASK_CHECK(fabsf(rec.distance - 2.0f) < 1e-6f);
	TASK_CHECK(normal_is_unit(rec.geometricNormal));
	TASK_CHECK(fabsf(rec.geometricNormal.z - 1.0f) < 1e-6f);
	TASK_CHECK(fabsf(rec.geometricNormal.x) < 1e-6f && fabsf(rec.geometricNormal.y) < 1e-6f);
	// The smoothed normal tilts toward +y and must remain distinct from the geometric one.
	TASK_CHECK(fabsf(rec.surfaceNormal.y) > 0.1f);
	TASK_CHECK(fabsf(rec.surfaceNormal.y - rec.geometricNormal.y) > 0.1f);
	TASK_CHECK(error_is_sound(rec.pointError));
	struct dvec hitRef = dadd(dpoint(ray.start), dscale(dpoint(ray.direction), 2.0));
	TASK_CHECK(float_in_error(rec.hitPoint.z, hitRef.z, rec.pointError.z));
	return 0;
}

// ------------------------------------------------------------------- parameter 4

static int parameter_shared_mesh_affine_instances(void) {
	struct vector verts[3] = {
		{ -1.0f, 0.0f, 0.0f },
		{  0.0f, 1.0f, 0.0f },
		{  1.0f, 0.0f, 0.0f },
	};
	struct mesh *meshes = NULL;
	append_triangle_mesh(&meshes, false, verts, NULL);

	struct matrix4x4 matrices[4] = {
		tform_new_scale3(2.0f, 2.0f, 2.0f).A,
		tform_new_scale3(2.0f, 0.5f, 1.0f).A,
		matrix_shear_xy(0.75f),
		matrix_mirror_z(),
	};

	for (int k = 0; k < 4; ++k) {
		struct instance inst = make_mesh_inst(&meshes, 0, NULL, matrices[k]);
		struct lightRay ray = { .start = { 0.0f, 0.25f, 2.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
		struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };
		TASK_CHECK(inst.intersectFn(&inst, &ray, &rec, NULL));

		// Double reference: transform the ray, solve the same planar hit, transform back.
		struct matrix4x4 inv = inst.composite.Ainv;
		struct dvec lo = dmat_point(inv, dpoint(ray.start));
		struct dvec ld = dsub(dmat_point(inv, dadd(dpoint(ray.start), dpoint(ray.direction))), lo);
		// Triangle lies in z=0: t = -lo.z / ld.z.
		const double tRef = -lo.z / ld.z;
		TASK_CHECK(tRef > 0.0);
		TASK_CHECK(fabsf(rec.distance - (float)tRef) <= 4.0f * FLT_EPSILON * fabsf((float)tRef) + FLT_MIN);

		struct dvec localHit = dadd(lo, dscale(ld, tRef));
		struct dvec worldHit = dmat_point(matrices[k], localHit);
		TASK_CHECK(float_in_error(rec.hitPoint.x, worldHit.x, rec.pointError.x));
		TASK_CHECK(float_in_error(rec.hitPoint.y, worldHit.y, rec.pointError.y));
		TASK_CHECK(float_in_error(rec.hitPoint.z, worldHit.z, rec.pointError.z));
		TASK_CHECK(error_is_sound(rec.pointError));
		TASK_CHECK(normal_is_unit(rec.geometricNormal));

		struct dvec nRef = dnorm(dmat_vec_transpose_inv(inv, (struct dvec){ 0.0, 0.0, 1.0 }));
		TASK_CHECK(fabsf(rec.geometricNormal.x - (float)nRef.x) < 1e-5f);
		TASK_CHECK(fabsf(rec.geometricNormal.y - (float)nRef.y) < 1e-5f);
		TASK_CHECK(fabsf(rec.geometricNormal.z - (float)nRef.z) < 1e-5f);
	}
	return 0;
}

// ------------------------------------------------------------------- parameter 5

static int parameter_nearest_across_instances(void) {
	struct sphere *spheres = NULL;
	v_arr_add(spheres, ((struct sphere){ .radius = 1.0f }));
	v_arr_add(spheres, ((struct sphere){ .radius = 1.0f }));

	struct instance *instances = NULL;
	struct instance a = make_sphere_inst(&spheres, 0, NULL);
	struct instance b = make_sphere_inst(&spheres, 1, NULL);
	a.composite = (struct transform){ .A = tform_new_translate(0.0f, 0.0f, -2.0f).A, .Ainv = tform_new_translate(0.0f, 0.0f, -2.0f).Ainv };
	b.composite = (struct transform){ .A = tform_new_translate(0.0f, 0.0f, -5.0f).A, .Ainv = tform_new_translate(0.0f, 0.0f, -5.0f).Ainv };
	v_arr_add(instances, a);
	v_arr_add(instances, b);
	struct bvh *top = build_top_level_bvh(instances);

	// Non-unit direction again: both candidates must be compared on the same t scale.
	struct lightRay ray = { .start = { 0.0f, 0.0f, 0.0f }, .direction = { 0.0f, 0.0f, -3.0f }, .type = rt_camera };
	struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(traverse_top_level_bvh(instances, top, &ray, &rec, NULL));
	TASK_CHECK(rec.instIndex == 0);
	// Sphere center at z=-2, radius 1: world hit at z=-1, t such that -3t = -1 -> t = 1/3.
	TASK_CHECK(fabsf(rec.distance - (1.0f / 3.0f)) < 1e-6f);
	struct dvec ref = dadd(dpoint(ray.start), dscale(dpoint(ray.direction), 1.0 / 3.0));
	TASK_CHECK(float_in_error(rec.hitPoint.z, ref.z, rec.pointError.z));
	destroy_bvh(top);
	return 0;
}

// ----------------------------------------------------------------------- spawn 1

static int spawn_reflection_stays_outside(void) {
	struct sphere *spheres = NULL;
	v_arr_add(spheres, ((struct sphere){ .radius = 1.0f }));
	struct instance inst = make_sphere_inst(&spheres, 0, NULL);

	struct lightRay ray = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(inst.intersectFn(&inst, &ray, &rec, NULL));
	TASK_CHECK(rec.geometricNormal.z > 0.99f);

	struct vector outDir = { 0.0f, 0.0f, 1.0f }; // reflection: dot(geomN, dir) > 0
	struct lightRay spawned = spawn_surface_ray(&rec, outDir, rt_reflection);

	const struct vector delta = vec_sub(spawned.start, rec.hitPoint);
	TASK_CHECK(vec_dot(delta, rec.geometricNormal) > 0.0f);
	// The whole error box is cleared along the normal.
	TASK_CHECK(vec_dot(delta, rec.geometricNormal) >=
		fabsf(rec.geometricNormal.x) * rec.pointError.x +
		fabsf(rec.geometricNormal.y) * rec.pointError.y +
		fabsf(rec.geometricNormal.z) * rec.pointError.z);
	TASK_CHECK(isfinite(spawned.start.x) && isfinite(spawned.start.y) && isfinite(spawned.start.z));
	TASK_CHECK(vec_equals(spawned.direction, outDir));
	TASK_CHECK(spawned.type == rt_reflection);

	// The spawned ray must not immediately re-hit the same sphere.
	struct hitRecord again = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(!inst.intersectFn(&inst, &spawned, &again, NULL));
	return 0;
}

// ----------------------------------------------------------------------- spawn 2

static int spawn_transmission_continues_inside(void) {
	struct sphere *spheres = NULL;
	v_arr_add(spheres, ((struct sphere){ .radius = 1.0f }));
	struct instance inst = make_sphere_inst(&spheres, 0, NULL);

	struct lightRay ray = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(inst.intersectFn(&inst, &ray, &rec, NULL));

	struct vector inDir = { 0.0f, 0.0f, -1.0f }; // transmission: dot(geomN, dir) < 0
	struct lightRay spawned = spawn_surface_ray(&rec, inDir, rt_transmission);
	TASK_CHECK(spawned.type == rt_transmission);

	const struct vector delta = vec_sub(spawned.start, rec.hitPoint);
	// Origin is on the interior side of the geometric surface.
	TASK_CHECK(vec_dot(delta, rec.geometricNormal) < 0.0f);

	// Continuation hits the *far* sphere wall, roughly a diameter away, never a
	// spurious t ~ 0 re-intersection with the entry surface.
	struct hitRecord exitRec = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(inst.intersectFn(&inst, &spawned, &exitRec, NULL));
	TASK_CHECK(exitRec.distance > 1.5f);
	TASK_CHECK(exitRec.geometricNormal.z < -0.99f);
	return 0;
}

// ----------------------------------------------------------------------- spawn 3

static int spawn_uses_geometric_not_smooth_normal(void) {
	struct vector verts[3] = {
		{ -1.0f, 0.0f, 0.0f },
		{  0.0f, 1.0f, 0.0f },
		{  1.0f, 0.0f, 0.0f },
	};
	struct vector norms[3] = {
		vec_normalize((struct vector){ 0.0f, 0.9f, 0.4f }),
		vec_normalize((struct vector){ 0.0f, 0.9f, 0.4f }),
		vec_normalize((struct vector){ 0.0f, 0.9f, 0.4f }),
	};
	struct mesh *meshes = NULL;
	append_triangle_mesh(&meshes, true, verts, norms);
	struct instance inst = make_mesh_inst(&meshes, 0, NULL, mat_id());

	struct lightRay ray = { .start = { 0.0f, 0.25f, 2.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(inst.intersectFn(&inst, &ray, &rec, NULL));
	TASK_CHECK(fabsf(rec.geometricNormal.z - 1.0f) < 1e-6f);
	TASK_CHECK(fabsf(rec.surfaceNormal.z - 1.0f) > 0.05f); // genuinely smoothed

	// Outgoing direction that is back-facing relative to the geometric normal...
	struct vector backDir = { 0.0f, 0.0f, -1.0f };
	struct lightRay through = spawn_surface_ray(&rec, backDir, rt_transmission);
	struct vector d = vec_sub(through.start, rec.hitPoint);
	TASK_CHECK(vec_dot(d, rec.geometricNormal) < 0.0f);
	// ...even though the smoothed shading normal would suggest a different tilt.
	TASK_CHECK(isfinite(through.start.x));
	return 0;
}

// ----------------------------------------------------------------------- spawn 4

static int spawn_direction_type_and_finite(void) {
	struct hitRecord hit = {
		.hitPoint = { 1000.0f, -2000.0f, 3000.0f },
		.geometricNormal = vec_normalize((struct vector){ 1.0f, 2.0f, 3.0f }),
		.surfaceNormal = hit.geometricNormal,
		.pointError = { 1e-3f, 2e-3f, 3e-3f },
		.distance = 7.0f,
		.instIndex = 0,
	};
	struct vector dir = vec_normalize((struct vector){ -1.0f, 1.0f, 0.5f });
	struct lightRay out = spawn_surface_ray(&hit, dir, rt_glossy);
	TASK_CHECK(vec_equals(out.direction, dir));
	TASK_CHECK(out.type == rt_glossy);
	TASK_CHECK(isfinite(out.start.x) && isfinite(out.start.y) && isfinite(out.start.z));
	TASK_CHECK(!vec_equals(out.start, hit.hitPoint));
	// Offset stays tight: a handful of error-box diagonals at most, never a bbox-sized move.
	const float move = vec_length(vec_sub(out.start, hit.hitPoint));
	TASK_CHECK(move < 10.0f * vec_length(hit.pointError));
	return 0;
}

// ----------------------------------------------------------------------- spawn 5

static int spawn_keeps_nearby_surface_and_large_coords(void) {
	// Two parallel triangles separated by a gap much smaller than any bbox diagonal.
	struct vector lowerVerts[3] = {
		{ -1.0f, 0.0f, -0.001f },
		{  0.0f, 1.0f, -0.001f },
		{  1.0f, 0.0f, -0.001f },
	};
	// Contiguous v_arr storage, exactly like a production scene: both instances index
	// the same mesh array by object_idx.
	struct mesh *meshes = NULL;
	append_triangle_mesh(&meshes, false, (struct vector[3]){
		{ -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }
	}, NULL);
	append_triangle_mesh(&meshes, false, lowerVerts, NULL);
	struct instance frontInst = make_mesh_inst(&meshes, 0, NULL, mat_id());
	struct instance backInst = make_mesh_inst(&meshes, 1, NULL, mat_id());

	struct lightRay ray = { .start = { 0.0f, 0.25f, 2.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord rec = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(frontInst.intersectFn(&frontInst, &ray, &rec, NULL));

	struct lightRay through = spawn_surface_ray(&rec, ray.direction, rt_transmission);
	// The error-based offset is tiny compared with the 1e-3 gap...
	const float move = vec_length(vec_sub(through.start, rec.hitPoint));
	TASK_CHECK(move > 0.0f && move < 0.00025f);
	// ...so the tightly-fitting neighboring surface is still hit.
	struct hitRecord next = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(backInst.intersectFn(&backInst, &through, &next, NULL));
	TASK_CHECK(next.distance > 0.0f && next.distance < 0.001f);

	// Same geometry at a 1e4 coordinate span: error grows with the coordinates but
	// remains proportional to ulps, and reflection still cannot re-hit the surface.
	struct mesh *farArr = NULL;
	append_triangle_mesh(&farArr, false, (struct vector[3]){
		{ -1.0f, 0.0f, 10000.0f }, { 0.0f, 1.0f, 10000.0f }, { 1.0f, 0.0f, 10000.0f }
	}, NULL);
	struct instance farInst = make_mesh_inst(&farArr, 0, NULL, mat_id());
	struct lightRay farRay = { .start = { 0.0f, 0.25f, 10002.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord farRec = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(farInst.intersectFn(&farInst, &farRay, &farRec, NULL));
	TASK_CHECK(error_is_sound(farRec.pointError));
	// The bound is a small, bounded number of ulps of the point coordinate rather
	// than a scene-wide epsilon: it stays large enough to clear representability
	// (the old fixed bbox offset, ~2e-4, is below one ulp at this magnitude and
	// would round away) while remaining tiny against the 1e4 coordinate span.
	const float ulpAtPoint = nextafterf(farRec.hitPoint.z, FLT_MAX) - farRec.hitPoint.z;
	TASK_CHECK(farRec.pointError.z >= ulpAtPoint);
	TASK_CHECK(farRec.pointError.z < 64.0f * ulpAtPoint);
	TASK_CHECK(farRec.pointError.z / 10000.0f < 1e-4f);
	struct lightRay farOut = spawn_surface_ray(&farRec, (struct vector){ 0.0f, 0.0f, 1.0f }, rt_reflection);
	struct hitRecord farAgain = { .distance = FLT_MAX, .instIndex = -1 };
	TASK_CHECK(!farInst.intersectFn(&farInst, &farOut, &farAgain, NULL));
	return 0;
}

// ------------------------------------------------------------- transport fixtures

struct test_bsdf {
	struct bsdfNode node;
	struct color tint;
	int mode; // 0 = mirror reflection, 1 = straight transmission
	int *hits;
};

static struct bsdfSample test_bsdf_sample(const struct bsdfNode *bsdf, sampler *s, const struct hitRecord *record) {
	(void)s;
	const struct test_bsdf *b = (const struct test_bsdf *)bsdf;
	if (b->hits) (*b->hits)++;
	struct vector dir;
	enum ray_type type;
	if (b->mode == 0) {
		dir = vec_reflect(vec_normalize(record->incident->direction), record->surfaceNormal);
		type = rt_reflection;
	} else {
		dir = record->incident->direction;
		type = rt_transmission;
	}
	return (struct bsdfSample){
		.out = { .start = record->hitPoint, .direction = dir, .type = type },
		.weight = b->tint,
	};
}

struct test_background {
	struct bsdfNode node;
	struct color radiance;
};

static struct bsdfSample background_sample(const struct bsdfNode *bsdf, sampler *s, const struct hitRecord *record) {
	(void)s; (void)record;
	const struct test_background *b = (const struct test_background *)bsdf;
	return (struct bsdfSample){ .out = { 0 }, .weight = b->radiance };
}

struct transport_scene {
	struct world *world;
	struct instance *instances;
	struct test_background background;
	struct test_bsdf bsdfs[4];
	struct bsdf_buffer buffers[4];
	const struct bsdfNode *nodeSets[4][2];
};

static struct transport_scene *make_transport_scene(void) {
	struct transport_scene *ts = calloc(1, sizeof(*ts));
	ts->world = calloc(1, sizeof(struct world));
	ts->background.node.sample = background_sample;
	ts->background.radiance = (struct color){ 0.25f, 0.5f, 0.75f, 1.0f };
	ts->world->background = &ts->background.node;
	return ts;
}

static void finalize_transport_scene(struct transport_scene *ts) {
	for (size_t i = 0; i < v_arr_len(ts->instances); ++i)
		v_arr_add(ts->world->instances, ts->instances[i]);
	ts->world->topLevel = build_top_level_bvh(ts->world->instances);
}

static struct sampler *fresh_sampler(uint32_t pixel) {
	struct sampler *s = sampler_new();
	sampler_init(s, Halton, 0, 16, pixel);
	return s;
}

// ------------------------------------------------------------------- transport 1

static int transport_mirror_no_immediate_self_hit(void) {
	struct transport_scene *ts = make_transport_scene();
	int hits = 0;
	ts->bsdfs[0] = (struct test_bsdf){
		.node = { .sample = test_bsdf_sample }, .tint = g_white_color, .mode = 0, .hits = &hits,
	};
	ts->nodeSets[0][0] = &ts->bsdfs[0].node;

	struct mesh *meshes = NULL;
	append_triangle_mesh(&meshes, false, (struct vector[3]){
		{ -2.0f, -2.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, { 2.0f, -2.0f, 0.0f }
	}, NULL);
	struct instance inst = make_mesh_inst(&meshes, 0, ts->nodeSets[0], mat_id());
	v_arr_add(ts->instances, inst);
	finalize_transport_scene(ts);

	// Straight-on reflection from in front of the mirror: without a correct spawn
	// offset the spawned ray re-intersects the same triangle at t ~ 0 every bounce.
	struct lightRay ray = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct sampler *s = fresh_sampler(7);
	struct color result = path_trace(ray, ts->world, 4, s);

	TASK_CHECK(hits == 1); // exactly one surface event, then background
	TASK_CHECK(fabsf(result.red - 0.25f) < 1e-6f);
	TASK_CHECK(fabsf(result.green - 0.5f) < 1e-6f);
	TASK_CHECK(fabsf(result.blue - 0.75f) < 1e-6f);
	sampler_destroy(s);
	return 0;
}

// ------------------------------------------------------------------- transport 2

static int transport_transmission_enters_and_exits(void) {
	struct transport_scene *ts = make_transport_scene();
	int hits = 0;
	ts->bsdfs[0] = (struct test_bsdf){
		.node = { .sample = test_bsdf_sample }, .tint = g_white_color, .mode = 1, .hits = &hits,
	};
	ts->nodeSets[0][0] = &ts->bsdfs[0].node;

	struct sphere *spheres = NULL;
	v_arr_add(spheres, ((struct sphere){ .radius = 1.0f }));
	struct instance inst = make_sphere_inst(&spheres, 0, ts->nodeSets[0]);
	v_arr_add(ts->instances, inst);
	finalize_transport_scene(ts);

	struct lightRay ray = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct sampler *s = fresh_sampler(11);
	struct color result = path_trace(ray, ts->world, 6, s);

	// Entry and exit surface events, each spawned on the correct side.
	TASK_CHECK(hits == 2);
	// White transmission tint, so the path ends at the plain background radiance.
	TASK_CHECK(fabsf(result.red - 0.25f) < 1e-6f);
	TASK_CHECK(fabsf(result.green - 0.5f) < 1e-6f);
	TASK_CHECK(fabsf(result.blue - 0.75f) < 1e-6f);
	sampler_destroy(s);
	return 0;
}

// ------------------------------------------------------------------- transport 3

static int transport_production_glass_sphere(void) {
	// End-to-end run with the real glass BSDF: pick a Halton pixel index whose first
	// dimension lands on the transmission branch at near-normal incidence.
	struct node_storage storage = { 0 };
	storage.node_pool = newBlock(NULL, 1024);
	storage.node_table = newHashtable(compareNodes, &storage.node_pool);
	const struct bsdfNode *glass = newGlass(
		&storage,
		newConstantTexture(&storage, g_white_color),
		newConstantValue(&storage, 0.0f),
		newConstantValue(&storage, 1.01f));

	struct transport_scene *ts = make_transport_scene();
	ts->nodeSets[0][0] = glass;

	struct sphere *spheres = NULL;
	v_arr_add(spheres, ((struct sphere){ .radius = 1.0f }));
	struct instance inst = make_sphere_inst(&spheres, 0, ts->nodeSets[0]);
	v_arr_add(ts->instances, inst);
	finalize_transport_scene(ts);

	struct lightRay ray = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	bool transmitted = false;
	struct color result = { 0 };
	for (uint32_t pixel = 1; pixel < 64 && !transmitted; ++pixel) {
		struct sampler *probe = fresh_sampler(pixel);
		const float first = sampler_dimension(probe);
		sampler_destroy(probe);
		if (first <= 0.5f) continue; // first glass sample would reflect
		struct sampler *s = fresh_sampler(pixel);
		result = path_trace(ray, ts->world, 6, s);
		sampler_destroy(s);
		transmitted = true;
	}
	TASK_CHECK(transmitted);
	TASK_CHECK(isfinite(result.red) && isfinite(result.green) && isfinite(result.blue));
	// The ray travels through the sphere and reaches background: radiance stays close
	// to the background color rather than collapsing to black (lost ray / self-hit loop).
	TASK_CHECK(result.green > 0.3f);
	destroyHashtable(storage.node_table);
	destroyBlocks(storage.node_pool);
	return 0;
}

// --------------------------------------------------------------------- registration

const struct task_case task_cases[] = {
	{ "parameter", "sphere_nonunit_parameter_and_two_roots", parameter_sphere_nonunit_and_roots },
	{ "parameter", "sphere_inner_exit_root",                 parameter_sphere_inner_root },
	{ "parameter", "triangle_geometric_normal_and_error",    parameter_triangle_geometric_normal },
	{ "parameter", "shared_mesh_affine_instances",           parameter_shared_mesh_affine_instances },
	{ "parameter", "nearest_hit_across_instances",           parameter_nearest_across_instances },
	{ "spawn",     "reflection_origin_outside",              spawn_reflection_stays_outside },
	{ "spawn",     "transmission_origin_inside",             spawn_transmission_continues_inside },
	{ "spawn",     "side_uses_geometric_normal",             spawn_uses_geometric_not_smooth_normal },
	{ "spawn",     "direction_type_preserved_finite",        spawn_direction_type_and_finite },
	{ "spawn",     "nearby_surface_and_large_coordinates",   spawn_keeps_nearby_surface_and_large_coords },
	{ "transport", "mirror_path_no_immediate_self_hit",      transport_mirror_no_immediate_self_hit },
	{ "transport", "transmission_path_enters_and_exits",     transport_transmission_enters_and_exits },
	{ "transport", "production_glass_sphere_path",           transport_production_glass_sphere },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
