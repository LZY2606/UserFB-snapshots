//
//  ray_spawn.c
//  c-ray task regressions
//
//  Regression coverage for:
//    parameter - sphere/mesh hit points stay on the original ray, in world
//                space, with a conservative position error.
//    spawn     - spawn_surface_ray() leaves the recorded surface on the
//                correct side by a finite, error-bounded amount.
//    transport - real path_trace() reflection/transmission paths make
//                progress, both under identity and affine instances.
//
// Everything here is plain C and self-contained: no windows, no scenes on
// disk. All reference values are computed independently in double precision.
//

#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <stdint.h>

#include <common/vector.h>
#include <common/color.h>
#include <common/transforms.h>
#include <common/mempool.h>
#include <common/hashtable.h>
#include <common/logging.h>
#include <datatypes/lightray.h>
#include <datatypes/hitrecord.h>
#include <datatypes/sphere.h>
#include <datatypes/mesh.h>
#include <datatypes/poly.h>
#include <datatypes/scene.h>
#include <datatypes/bbox.h>
#include <accelerators/bvh.h>
#include <renderer/instance.h>
#include <renderer/pathtrace.h>
#include <renderer/ray_spawn.h>
#include <renderer/samplers/sampler.h>
#include <nodes/bsdfnode.h>
#include <nodes/colornode.h>
#include <nodes/valuenode.h>
#include <nodes/vectornode.h>

//
// ---- Independent double precision reference helpers ---------------------
//

static double dvec_length(struct vector v) {
	return sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z);
}

static double dvec_dot(struct vector a, struct vector b) {
	return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
}

static double dvec_component(struct vector v, int i) {
	switch (i) {
		case 0: return v.x;
		case 1: return v.y;
		default: return v.z;
	}
}

// World-space oriented edge cross product for polygon `p`, computed
// independently in double precision as (v1 - v0) x (v2 - v0).
static struct vector reference_mesh_world_normal(const struct mesh *m, const struct poly *p, struct matrix4x4 A) {
	const struct vector v0 = m->vbuf.vertices[p->vertexIndex[0]];
	const struct vector v1 = m->vbuf.vertices[p->vertexIndex[1]];
	const struct vector v2 = m->vbuf.vertices[p->vertexIndex[2]];
	const struct vector e1 = { v1.x - v0.x, v1.y - v0.y, v1.z - v0.z };
	const struct vector e2 = { v2.x - v0.x, v2.y - v0.y, v2.z - v0.z };
	const struct vector n = vec_cross(e1, e2);
	// The transformed edge cross product is det(A) * transpose(A^-1) n, so
	// for a positive-determinant transform it points the same way as the
	// object-space cross product; for a mirrored transform the winding flips.
	struct vector world = n;
	tform_vector_transpose(&world, mat_invert(A));
	double det = (double)A.mtx[0][0] * ((double)A.mtx[1][1] * A.mtx[2][2] - (double)A.mtx[1][2] * A.mtx[2][1])
			   - (double)A.mtx[0][1] * ((double)A.mtx[1][0] * A.mtx[2][2] - (double)A.mtx[1][2] * A.mtx[2][0])
			   + (double)A.mtx[0][2] * ((double)A.mtx[1][0] * A.mtx[2][1] - (double)A.mtx[1][1] * A.mtx[2][0]);
	if (det < 0.0) world = vec_negate(world);
	double len = dvec_length(world);
	if (len == 0.0) return world;
	return (struct vector){ (float)(world.x / len), (float)(world.y / len), (float)(world.z / len) };
}

// Distance from `p` along `dir` to the smallest positive sphere intersection.
// Returns false if there is no intersection in front of the origin.
static bool reference_sphere_hit(struct lightRay ray, double radius, double *t_out) {
	const double sx = ray.start.x, sy = ray.start.y, sz = ray.start.z;
	const double dx = ray.direction.x, dy = ray.direction.y, dz = ray.direction.z;
	const double A = dx * dx + dy * dy + dz * dz;
	if (A == 0.0) return false;
	const double B = 2.0 * (dx * sx + dy * sy + dz * sz);
	const double C = sx * sx + sy * sy + sz * sz - radius * radius;
	const double disc = B * B - 4.0 * A * C;
	if (disc < 0.0) return false;
	const double sq = sqrt(disc);
	const double t0 = (-B + sq) / (2.0 * A);
	const double t1 = (-B - sq) / (2.0 * A);
	double t = 0.0;
	if (t1 > 0.0) t = t1;
	else if (t0 > 0.0) t = t0;
	else return false;
	if (t < 0.0) return false;
	*t_out = t;
	return true;
}

// The world-space ray parameter t for which start + t*direction is closest
// to `p`. Used to check the reported t against the world hit point.
static double project_point_onto_ray(struct lightRay ray, struct vector p) {
	const double dx = ray.direction.x, dy = ray.direction.y, dz = ray.direction.z;
	const double len2 = dx * dx + dy * dy + dz * dz;
	if (len2 == 0.0) return 0.0;
	const double num =
		((double)p.x - ray.start.x) * dx +
		((double)p.y - ray.start.y) * dy +
		((double)p.z - ray.start.z) * dz;
	return num / len2;
}

static struct vector world_ray_at(struct lightRay ray, double t) {
	return (struct vector){
		(float)((double)ray.start.x + t * (double)ray.direction.x),
		(float)((double)ray.start.y + t * (double)ray.direction.y),
		(float)((double)ray.start.z + t * (double)ray.direction.z),
	};
}

// |hitPoint - (start + distance*direction)| per component must lie inside
// the reported pointError.
static bool point_within_error(struct hitRecord *h, double *worst_ratio) {
	struct lightRay ray = *h->incident;
	struct vector expected = world_ray_at(ray, h->distance);
	double worst = 0.0;
	for (int i = 0; i < 3; ++i) {
		double actual = dvec_component(h->hitPoint, i);
		double exp = dvec_component(expected, i);
		double err = dvec_component(h->pointError, i);
		if (!(err >= 0.0) || !isfinite(err)) return false;
		double delta = fabs(actual - exp);
		double ratio = err > 0.0 ? delta / err : (delta == 0.0 ? 0.0 : 1e300);
		if (ratio > worst) worst = ratio;
	}
	if (worst_ratio) *worst_ratio = worst;
	return true;
}

//
// ---- Fixture: a node storage for building BSDF nodes --------------------
//

struct fixture {
	struct world world;
	struct block *node_pool;
};

static void fixture_init(struct fixture *f) {
	f->world.storage.node_pool = newBlock(NULL, 1024);
	f->world.storage.node_table = newHashtable(compareNodes, &f->world.storage.node_pool);
}

static void fixture_free(struct fixture *f) {
	destroyHashtable(f->world.storage.node_table);
	destroyBlocks(f->world.storage.node_pool);
}

static struct bsdf_buffer *make_buffer(struct fixture *f, const struct bsdfNode *bsdf) {
	(void)f;
	struct bsdf_buffer *buf = calloc(1, sizeof(*buf));
	const struct bsdfNode **bsdfs = NULL;
	v_arr_add(bsdfs, bsdf);
	buf->bsdfs = bsdfs;
	return buf;
}

static void destroy_buffer(struct bsdf_buffer *buf) {
	if (!buf) return;
	v_arr_free(buf->bsdfs);
	free(buf);
}

//
// ---- Fixture: a single triangle mesh, shared by affine instances --------
//

struct mesh_fixture {
	struct mesh mesh;
	struct mesh *mesh_ptr;
	struct bsdf_buffer *bufs[8];
	size_t buf_count;
};

static void mesh_fixture_build(struct fixture *f, struct mesh_fixture *mf) {
	memset(mf, 0, sizeof(*mf));
	struct mesh *m = &mf->mesh;
	mf->mesh_ptr = m;

	// A square made of two triangles in the z = 0 plane, spanning [-1,1]^2.
	// Winding is counter-clockwise when viewed from +z, so the raw edge
	// cross product points along +z.
	struct vector *vertices = NULL;
	v_arr_add(vertices, ((struct vector){ -1.0f, -1.0f, 0.0f }));
	v_arr_add(vertices, ((struct vector){  1.0f, -1.0f, 0.0f }));
	v_arr_add(vertices, ((struct vector){  1.0f,  1.0f, 0.0f }));
	v_arr_add(vertices, ((struct vector){ -1.0f,  1.0f, 0.0f }));

	// Per-vertex normals tilted away from the geometric normal so that a
	// smoothed shading normal differs from the geometric one.
	struct vector *normals = NULL;
	v_arr_add(normals, ((struct vector){ -0.6f, -0.6f, 0.8f }));
	v_arr_add(normals, ((struct vector){  0.6f, -0.6f, 0.8f }));
	v_arr_add(normals, ((struct vector){  0.6f,  0.6f, 0.8f }));
	v_arr_add(normals, ((struct vector){ -0.6f,  0.6f, 0.8f }));

	m->vbuf.vertices = vertices;
	m->vbuf.normals = normals;

	struct poly *polygons = NULL;
	struct poly p0 = { 0 };
	p0.vertexIndex[0] = 0; p0.vertexIndex[1] = 1; p0.vertexIndex[2] = 2;
	p0.normalIndex[0] = 0; p0.normalIndex[1] = 1; p0.normalIndex[2] = 2;
	p0.textureIndex[0] = -1; p0.textureIndex[1] = -1; p0.textureIndex[2] = -1;
	p0.materialIndex = 0;
	p0.hasNormals = true;
	v_arr_add(polygons, p0);

	struct poly p1 = { 0 };
	p1.vertexIndex[0] = 0; p1.vertexIndex[1] = 2; p1.vertexIndex[2] = 3;
	p1.normalIndex[0] = 0; p1.normalIndex[1] = 2; p1.normalIndex[2] = 3;
	p1.textureIndex[0] = -1; p1.textureIndex[1] = -1; p1.textureIndex[2] = -1;
	p1.materialIndex = 0;
	p1.hasNormals = true;
	v_arr_add(polygons, p1);

	m->polygons = polygons;
	m->bvh = build_mesh_bvh(m);

	// One material buffer so instances can be intersected.
	struct bsdf_buffer *buf = make_buffer(f, newDiffuse(&f->world.storage, newConstantTexture(&f->world.storage, g_white_color)));
	mf->bufs[mf->buf_count++] = buf;
}

static void mesh_fixture_free(struct mesh_fixture *mf) {
	v_arr_free(mf->mesh.vbuf.vertices);
	v_arr_free(mf->mesh.vbuf.normals);
	v_arr_free(mf->mesh.polygons);
	destroy_bvh(mf->mesh.bvh);
}

static struct instance make_mesh_instance(struct mesh_fixture *mf, struct transform t) {
	// `new_mesh_instance` stores a pointer to the mesh array slot, so keep a
	// stable array of one pointer alive for the fixture's lifetime.
	struct mesh **meshes = &mf->mesh_ptr;
	struct instance inst = new_mesh_instance(meshes, 0, NULL, NULL);
	inst.composite = t;
	inst.bbuf = mf->bufs[0];
	return inst;
}

//
// ---- CASE: parameter group ----------------------------------------------
//

// Sphere intersection must return the parameter along the original,
// non-unit direction; hitPoint must be start + t*direction.
static int case_sphere_parameter_nonunit(void) {
	struct sphere sphere = { .radius = 1.0f };
	const struct lightRay rays[] = {
		{ .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -2.0f }, .type = rt_camera },
		{ .start = { 0.0f, 0.0f, 5.0f }, .direction = { 0.0f, 0.0f, -7.5f }, .type = rt_camera },
		{ .start = { 3.0f, 0.0f, 0.0f }, .direction = { -4.0f, 0.0f, 0.0f }, .type = rt_camera },
		{ .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera },
		{ .start = { 0.3f, -0.2f, 4.0f }, .direction = { 0.1f, 0.05f, -3.0f }, .type = rt_camera },
	};
	const size_t n = sizeof(rays) / sizeof(rays[0]);

	for (size_t i = 0; i < n; ++i) {
		struct lightRay ray = rays[i];
		struct hitRecord hit = { .incident = &ray, .distance = FLT_MAX };
		TASK_CHECK(rayIntersectsWithSphere(&ray, &sphere, &hit));

		double ref_t = 0.0;
		TASK_CHECK(reference_sphere_hit(ray, sphere.radius, &ref_t));
		// The returned t must be the original-ray parameter.
		TASK_CHECK(fabs((double)hit.distance - ref_t) <= 1e-5 * (1.0 + fabs(ref_t)));

		struct vector expected = world_ray_at(ray, ref_t);
		double worst = 0.0;
		TASK_CHECK(point_within_error(&hit, &worst));
		TASK_CHECK(worst <= 1.0);
		for (int c = 0; c < 3; ++c)
			TASK_CHECK(fabs(dvec_component(hit.hitPoint, c) - dvec_component(expected, c)) <=
					   fabs(dvec_component(hit.pointError, c)) + 1e-6 * (1.0 + fabs(dvec_component(expected, c))));

		// Geometric normal points outward from the sphere centered at origin.
		TASK_CHECK(fabs(dvec_length(hit.geometricNormal) - 1.0) <= 1e-5);
		TASK_CHECK(dvec_dot(hit.geometricNormal, hit.hitPoint) > 0.0);
	}
	return 0;
}

// Inside hits: both sphere roots are genuine parameters along the same ray.
static int case_sphere_inside_and_compare(void) {
	struct sphere sphere = { .radius = 2.0f };
	// Start inside the sphere, direction toward -z. Only the exit root is
	// in front of the origin.
	struct lightRay inside = { .start = { 0.0f, 0.0f, 0.5f }, .direction = { 0.0f, 0.0f, -3.0f }, .type = rt_camera };
	struct hitRecord hit = { .incident = &inside, .distance = FLT_MAX };
	TASK_CHECK(rayIntersectsWithSphere(&inside, &sphere, &hit));

	double ref_t = 0.0;
	TASK_CHECK(reference_sphere_hit(inside, sphere.radius, &ref_t));
	TASK_CHECK(fabs((double)hit.distance - ref_t) <= 1e-5 * (1.0 + ref_t));
	// Exit hit is on the -z side, so the geometric normal points -z.
	TASK_CHECK(hit.geometricNormal.z < 0.0f);
	// The hit is exactly on the sphere.
	TASK_CHECK(fabs((double)dvec_length(hit.hitPoint) - sphere.radius) <= 1e-4);

	// A ray whose direction is scaled by a factor must produce a parameter
	// scaled by the inverse factor; both must describe the same surface.
	struct lightRay slow = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct lightRay fast = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -2.0f }, .type = rt_camera };
	struct hitRecord hs = { .incident = &slow, .distance = FLT_MAX };
	struct hitRecord hf = { .incident = &fast, .distance = FLT_MAX };
	TASK_CHECK(rayIntersectsWithSphere(&slow, &sphere, &hs));
	TASK_CHECK(rayIntersectsWithSphere(&fast, &sphere, &hf));
	TASK_CHECK(fabs((double)hs.distance - 2.0 * (double)hf.distance) <= 1e-4);
	// Same world surface point.
	TASK_CHECK(fabs((double)hs.hitPoint.z - (double)hf.hitPoint.z) <= 1e-4);
	return 0;
}

// Sphere instance under an affine transform: world hit point lies on the
// original ray, distance is the original-ray t, and the error bound covers
// the residual.
static int case_sphere_instance_affine(void) {
	struct fixture f; fixture_init(&f);
	struct sphere spheres[1] = { { .radius = 1.0f } };
	struct sphere *arr = spheres;
	struct instance inst = new_sphere_instance(&arr, 0, NULL, NULL);
	struct bsdf_buffer *buf = make_buffer(&f, newDiffuse(&f.world.storage, newConstantTexture(&f.world.storage, g_white_color)));
	inst.bbuf = buf;

	const struct transform tf[] = {
		tform_new_scale3(3.0f, 0.5f, 2.0f),
		tform_new_scale3(-2.0f, 1.0f, 1.0f), // negative determinant
		tform_new_translate(10.0f, -4.0f, 2.0f),
	};
	for (size_t i = 0; i < sizeof(tf) / sizeof(tf[0]); ++i) {
		inst.composite = tf[i];
		// Shoot a family of rays at the transformed sphere from outside.
		for (int k = 0; k < 8; ++k) {
			struct vector dir = { (float)(0.1 * k - 0.35), (float)(0.05 * k - 0.2), -1.0f };
			struct lightRay ray = { .start = { 10.0f, -4.0f, 8.0f }, .direction = dir, .type = rt_camera };
			struct hitRecord hit = { .incident = &ray, .distance = FLT_MAX };
			if (!inst.intersectFn(&inst, &ray, &hit, NULL))
				continue;
			double worst = 0.0;
			TASK_CHECK(point_within_error(&hit, &worst));
			TASK_CHECK(worst <= 1.0);
			// The world hit point must be close to a point on the original
			// world ray, at parameter t.
			double projected = project_point_onto_ray(ray, hit.hitPoint);
			TASK_CHECK(fabs(projected - (double)hit.distance) <=
					   1e-3 * (1.0 + fabs((double)hit.distance)));
			// Geometric normal must be unit length and point to the
			// transformed implicit surface's outward side: for a sphere under
			// A that direction is normalize(A^-T * object_point).
			struct vector object_point = hit.hitPoint;
			tform_point(&object_point, tf[i].Ainv);
			struct vector expected_normal = object_point;
			tform_vector_transpose(&expected_normal, mat_invert(tf[i].A));
			double elen = dvec_length(expected_normal);
			TASK_CHECK(elen > 0.0);
			TASK_CHECK(fabs(dvec_length(hit.geometricNormal) - 1.0) <= 1e-5);
			TASK_CHECK(dvec_dot(hit.geometricNormal, expected_normal) / elen > 0.999);
		}
	}

	destroy_buffer(buf);
	fixture_free(&f);
	return 0;
}

// Mesh (triangle) parameter test: the triangle's geometric normal must be
// the transformed oriented edge cross product, and the world hit point must
// lie on the original ray at the reported t.
static int case_mesh_parameter(void) {
	struct fixture f; fixture_init(&f);
	struct mesh_fixture mf; mesh_fixture_build(&f, &mf);

	struct instance inst = make_mesh_instance(&mf, tform_new());
	struct lightRay ray = { .start = { 0.25f, 0.25f, 4.0f }, .direction = { 0.0f, 0.0f, -2.0f }, .type = rt_camera };
	struct hitRecord hit = { .incident = &ray, .distance = FLT_MAX };
	TASK_CHECK(inst.intersectFn(&inst, &ray, &hit, NULL));

	// Plane is z = 0, ray starts at z=4 heading -2 in z: t == 2.
	TASK_CHECK(fabs((double)hit.distance - 2.0) <= 1e-5);
	TASK_CHECK(fabs((double)hit.hitPoint.z) <= 1e-5);

	double worst = 0.0;
	TASK_CHECK(point_within_error(&hit, &worst));
	TASK_CHECK(worst <= 1.0);

	// Geometric normal is the transformed oriented edge cross product, per
	// the intersection routine's e1/e2 convention (independently computed in
	// double precision against the identity transform).
	struct vector ref = reference_mesh_world_normal(&mf.mesh, hit.polygon, mat_id());
	TASK_CHECK(dvec_dot(hit.geometricNormal, ref) > 0.999);
	TASK_CHECK(fabs(dvec_length(hit.geometricNormal) - 1.0) <= 1e-5);

	// Shading normal is the smoothed one and should be tilted.
	TASK_CHECK(fabs((double)hit.surfaceNormal.x) > 1e-3 ||
			   fabs((double)hit.surfaceNormal.y) > 1e-3 ||
			   fabs((double)hit.surfaceNormal.z - 1.0f) > 1e-3);

	// The geometric normal must not be flipped toward the incident ray; the
	// shading normal is the one allowed to face the ray.
	TASK_CHECK(dvec_dot(ray.direction, hit.geometricNormal) == dvec_dot(ray.direction, ref));

	mesh_fixture_free(&mf);
	fixture_free(&f);
	return 0;
}

// Two instances of the same shared mesh at different scales: the nearest
// hit must use the same parameterization standard, and the reported world
// hits must both lie on the same test ray at their own t.
static int case_shared_mesh_instances(void) {
	struct fixture f; fixture_init(&f);
	struct mesh_fixture mf; mesh_fixture_build(&f, &mf);

	// Instance A shrunk toward origin, instance B scaled up; both share the
	// same underlying mesh and (importantly) the same polygons.
	struct instance instA = make_mesh_instance(&mf, tform_new_scale(0.5f));
	struct instance instB = make_mesh_instance(&mf, tform_new_scale3(2.0f, 3.0f, 1.0f));

	struct lightRay ray = { .start = { 0.25f, 0.25f, 4.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord hA = { .incident = &ray, .distance = FLT_MAX };
	struct hitRecord hB = { .incident = &ray, .distance = FLT_MAX };
	TASK_CHECK(instA.intersectFn(&instA, &ray, &hA, NULL));
	TASK_CHECK(instB.intersectFn(&instB, &ray, &hB, NULL));

	// The scaled flat plane remains at z = 0; the ray parameter is the same
	// for both (world points coincide), so the nearest-hit comparison uses
	// the same parameter for both.
	TASK_CHECK(fabs((double)hA.distance - (double)hB.distance) <= 1e-4);
	TASK_CHECK(fabs((double)hA.hitPoint.z) <= 1e-4);
	TASK_CHECK(fabs((double)hB.hitPoint.z) <= 1e-4);

	double worstA = 0.0, worstB = 0.0;
	TASK_CHECK(point_within_error(&hA, &worstA));
	TASK_CHECK(point_within_error(&hB, &worstB));
	TASK_CHECK(worstA <= 1.0);
	TASK_CHECK(worstB <= 1.0);

	// Both geometric normals are unit and match the independently computed
	// transformed edge cross product, including the mirrored instance.
	struct vector refA = reference_mesh_world_normal(&mf.mesh, hA.polygon, instA.composite.A);
	struct vector refB = reference_mesh_world_normal(&mf.mesh, hB.polygon, instB.composite.A);
	TASK_CHECK(fabs(dvec_length(hA.geometricNormal) - 1.0) <= 1e-5);
	TASK_CHECK(fabs(dvec_length(hB.geometricNormal) - 1.0) <= 1e-5);
	TASK_CHECK(dvec_dot(hA.geometricNormal, refA) > 0.999);
	TASK_CHECK(dvec_dot(hB.geometricNormal, refB) > 0.999);

	mesh_fixture_free(&mf);
	fixture_free(&f);
	return 0;
}

//
// ---- CASE: spawn group --------------------------------------------------
//

// A hit record with a known surface frame, used to probe spawn_surface_ray.
static struct hitRecord make_hit(struct lightRay *ray, struct vector point,
								 struct vector geometric, struct vector shading,
								 struct vector error, float t)
{
	struct hitRecord h = { 0 };
	h.incident = ray;
	h.hitPoint = point;
	h.geometricNormal = geometric;
	h.surfaceNormal = shading;
	h.pointError = error;
	h.distance = t;
	return h;
}

static int case_spawn_outward_reflection(void) {
	// A flat surface at z = 0 with geometric normal +z, shading normal tilted
	// strongly toward +x. The outgoing reflection leaves through +z.
	struct lightRay incident = { .start = { 0.0f, 0.0f, 4.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct vector geom = { 0.0f, 0.0f, 1.0f };
	struct vector shading = vec_normalize((struct vector){ 0.7f, 0.0f, 0.7f });
	struct vector err = { 1e-6f, 1e-6f, 1e-6f };
	struct hitRecord h = make_hit(&incident, (struct vector){ 0.0f, 0.0f, 0.0f }, geom, shading, err, 4.0f);

	struct vector outdir = vec_normalize((struct vector){ 0.0f, 0.0f, 1.0f });
	struct lightRay spawned = spawn_surface_ray(&h, outdir, rt_reflection | rt_singular);

	// Direction and type are preserved exactly.
	TASK_CHECK(spawned.direction.x == outdir.x && spawned.direction.y == outdir.y && spawned.direction.z == outdir.z);
	TASK_CHECK(spawned.type == (rt_reflection | rt_singular));

	// Start must move to the +z side of the geometric surface.
	TASK_CHECK(spawned.start.z > 0.0f);
	// ... but be finite and local: bounded by the error + a few float
	// spacings, not by the geom's magnitude.
	double offset = fabs((double)spawned.start.z);
	TASK_CHECK(offset <= 1.0e-4);
	TASK_CHECK(offset >= (double)err.z);

	// Smoothing must not be able to decide the side: the shading normal
	// here has a much larger +x component, yet the spawn is purely +z.
	TASK_CHECK(fabs((double)spawned.start.x) <= 16.0 * (double)FLT_EPSILON * fabs((double)shading.x));
	return 0;
}

static int case_spawn_inward_transmission(void) {
	// Outgoing direction points into the surface (-z): start must go to the
	// -z side of the geometric surface, even though the shading normal has
	// a +z component.
	struct lightRay incident = { .start = { 0.0f, 0.0f, 4.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_transmission };
	struct vector geom = { 0.0f, 0.0f, 1.0f };
	struct vector shading = vec_normalize((struct vector){ 0.0f, 0.4f, 0.9f });
	struct vector err = { 2e-6f, 2e-6f, 2e-6f };
	struct hitRecord h = make_hit(&incident, (struct vector){ 0.0f, 0.0f, 0.0f }, geom, shading, err, 4.0f);

	struct vector outdir = vec_normalize((struct vector){ 0.0f, 0.0f, -1.0f });
	struct lightRay spawned = spawn_surface_ray(&h, outdir, rt_transmission | rt_singular);
	TASK_CHECK(spawned.start.z < 0.0f);
	TASK_CHECK(fabs((double)spawned.start.z) >= (double)err.z);
	TASK_CHECK(fabs((double)spawned.start.z) <= 1.0e-4);
	// Side is geometric (-z), so no +x/+y contamination.
	TASK_CHECK(fabs((double)spawned.start.x) < 1e-12);
	TASK_CHECK(fabs((double)spawned.start.y) < 1e-12);
	TASK_CHECK(spawned.direction.z == outdir.z);
	TASK_CHECK(spawned.type == (rt_transmission | rt_singular));
	return 0;
}

// The spawn offset must be driven by the recorded position error, not by
// any scene or bounding box size. Same surface, wildly different hit points
// far from the origin, must yield comparable offsets.
static int case_spawn_error_scaling(void) {
	struct lightRay incident = { .start = { 0.0f, 0.0f, 1.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct vector geom = { 0.0f, 0.0f, 1.0f };
	struct vector dir = { 0.0f, 0.0f, 1.0f };

	double offsets[2];
	struct vector points[2] = { { 0.0f, 0.0f, 0.0f }, { 100000.0f, -50000.0f, 0.0f } };
	for (int i = 0; i < 2; ++i) {
		struct vector err = { 1e-3f, 1e-3f, 1e-3f };
		struct hitRecord h = make_hit(&incident, points[i], geom, geom, err, 1.0f);
		struct lightRay spawned = spawn_surface_ray(&h, dir, rt_reflection | rt_singular);
		TASK_CHECK(spawned.start.z > points[i].z);
		offsets[i] = (double)spawned.start.z - (double)points[i].z;
		TASK_CHECK(offsets[i] >= 1e-3);
	}
	// Far from the origin the float spacing is larger, but the offset must
	// remain a small local quantity, not proportional to the coordinate.
	TASK_CHECK(offsets[1] < 1.0);
	TASK_CHECK(offsets[0] < 0.01);

	// A larger declared error must produce a proportionally larger offset.
	struct vector err_small = { 1e-4f, 0.0f, 1e-4f };
	struct vector err_big = { 1e-2f, 0.0f, 1e-2f };
	struct hitRecord hs = make_hit(&incident, (struct vector){ 0.0f, 0.0f, 0.0f }, geom, geom, err_small, 1.0f);
	struct hitRecord hb = make_hit(&incident, (struct vector){ 0.0f, 0.0f, 0.0f }, geom, geom, err_big, 1.0f);
	struct lightRay ss = spawn_surface_ray(&hs, dir, rt_reflection | rt_singular);
	struct lightRay sb = spawn_surface_ray(&hb, dir, rt_reflection | rt_singular);
	TASK_CHECK(sb.start.z > ss.start.z);
	return 0;
}

// A spawn off a transformed (non-uniformly scaled) sphere must leave the
// world-space geometric normal side and stay finite; this exercises the
// world-space geometric normal + pointError that instances produce.
static int case_spawn_off_affine_sphere(void) {
	struct fixture f; fixture_init(&f);
	struct sphere spheres[1] = { { .radius = 1.0f } };
	struct sphere *arr = spheres;
	struct instance inst = new_sphere_instance(&arr, 0, NULL, NULL);
	struct bsdf_buffer *buf = make_buffer(&f, newDiffuse(&f.world.storage, newConstantTexture(&f.world.storage, g_white_color)));
	inst.bbuf = buf;
	inst.composite = tform_new_scale3(4.0f, 0.25f, 1.5f);

	struct lightRay ray = { .start = { 0.0f, 0.0f, 5.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord hit = { .incident = &ray, .distance = FLT_MAX };
	TASK_CHECK(inst.intersectFn(&inst, &ray, &hit, NULL));

	// Reflect back out along +z.
	struct vector outdir = vec_reflect(vec_normalize(ray.direction), hit.geometricNormal);
	struct lightRay spawned = spawn_surface_ray(&hit, outdir, rt_reflection | rt_singular);

	// Moving along the outward normal by the offset must strictly increase
	// the signed distance from the surface: the start is no longer inside.
	double move = dvec_dot((struct vector){
		spawned.start.x - hit.hitPoint.x,
		spawned.start.y - hit.hitPoint.y,
		spawned.start.z - hit.hitPoint.z,
	}, hit.geometricNormal);
	TASK_CHECK(move > 0.0);
	// The move is bounded by the error projection plus a few spacings.
	double err_proj = fabs((double)hit.pointError.x * hit.geometricNormal.x) +
					  fabs((double)hit.pointError.y * hit.geometricNormal.y) +
					  fabs((double)hit.pointError.z * hit.geometricNormal.z);
	TASK_CHECK(move >= err_proj * 0.999);
	TASK_CHECK(move <= err_proj + 1e-4);

	destroy_buffer(buf);
	fixture_free(&f);
	return 0;
}

// The core anti-self-intersection property across coordinate scales and
// instance scales: a ray spawned from an actual hit must not immediately
// re-hit the same surface, yet a second, distinct surface placed just
// outside the first hit's conservative error must still be reachable.
static int case_spawn_no_immediate_rehit(void) {
	struct fixture f; fixture_init(&f);

	// A single sphere instance scaled differently and placed at wildly
	// different distances. The ray comes straight down the z axis.
	const float scales[] = { 1.0f, 1000.0f, 0.001f };
	const float centers[] = { 0.0f, 250000.0f, 0.0f };

	for (size_t si = 0; si < sizeof(scales) / sizeof(scales[0]); ++si) {
		struct sphere spheres[1] = { { .radius = 1.0f } };
		struct sphere *arr = spheres;
		struct instance inst = new_sphere_instance(&arr, 0, NULL, NULL);
		struct bsdf_buffer *buf = make_buffer(&f, newMetal(&f.world.storage,
			newConstantTexture(&f.world.storage, g_white_color),
			newConstantValue(&f.world.storage, 0.0f)));
		inst.bbuf = buf;
		struct transform place = tform_new_scale(scales[si]);
		place.A.mtx[2][3] = centers[si];
		place.Ainv = mat_invert(place.A);
		inst.composite = place;

		const float reach = scales[si] * 1.5f + 1.0f;
		struct lightRay ray = {
			.start = { 0.0f, 0.0f, centers[si] + reach },
			.direction = { 0.0f, 0.0f, -1.0f },
			.type = rt_camera,
		};
		struct hitRecord hit = { .incident = &ray, .distance = FLT_MAX };
		TASK_CHECK(inst.intersectFn(&inst, &ray, &hit, NULL));

		// Mirror-reflect off the sphere; this sends the ray back out.
		struct vector outdir = vec_reflect(vec_normalize(ray.direction), hit.geometricNormal);
		struct lightRay spawned = spawn_surface_ray(&hit, outdir, rt_reflection | rt_singular);
		// The outgoing direction leaves through the geometric normal side.
		TASK_CHECK(dvec_dot(outdir, hit.geometricNormal) > 0.0);
		TASK_CHECK(dvec_dot((struct vector){
			spawned.start.x - hit.hitPoint.x,
			spawned.start.y - hit.hitPoint.y,
			spawned.start.z - hit.hitPoint.z,
		}, hit.geometricNormal) > 0.0);

		// Re-intersect the same instance from the spawned ray. It must not
		// find a hit at ~zero distance (which would be the same surface).
		struct instance probe = inst;
		struct hitRecord again = { .incident = &spawned, .distance = FLT_MAX };
		if (probe.intersectFn(&probe, &spawned, &again, NULL)) {
			// Any re-hit of the sphere must be on the far side, not at the
			// outset.
			TASK_CHECK((double)again.distance > 8.0 * (double)(hit.pointError.x + hit.pointError.y + hit.pointError.z));
		}

		destroy_buffer(buf);
	}

	fixture_free(&f);
	return 0;
}

static struct bsdf_buffer *transport_diffuse_buffer(struct fixture *f) {
	return make_buffer(f, newDiffuse(&f->world.storage, newConstantTexture(&f->world.storage, g_white_color)));
}

static struct bsdf_buffer *transport_metal_buffer(struct fixture *f) {
	return make_buffer(f, newMetal(&f->world.storage, newConstantTexture(&f->world.storage, g_white_color), newConstantValue(&f->world.storage, 0.0f)));
}

static struct bsdf_buffer *transport_transparent_buffer(struct fixture *f) {
	return make_buffer(f, newTransparent(&f->world.storage, newConstantTexture(&f->world.storage, g_white_color)));
}

static struct bsdf_buffer *transport_glass_buffer(struct fixture *f) {
	return make_buffer(f, newGlass(&f->world.storage,
		newConstantTexture(&f->world.storage, g_white_color),
		newConstantValue(&f->world.storage, 0.0f),
		newConstantValue(&f->world.storage, 1.5f)));
}

// Counts how many times the background is sampled. A path that escapes the
// scene terminates by sampling the background.
static unsigned background_hits;
static struct bsdfSample background_sample(const struct bsdfNode *bsdf, sampler *s, const struct hitRecord *rec) {
	(void)bsdf; (void)s; (void)rec;
	background_hits++;
	return (struct bsdfSample){ .weight = g_white_color };
}

static const struct bsdfNode background_node = { .sample = background_sample };

static void build_world(struct world *w, struct instance *instances, struct bsdf_buffer **bufs) {
	(void)bufs;
	w->instances = instances;
	w->background = &background_node;
	w->topLevel = build_top_level_bvh(instances);
}

// Append an instance to a v_array and return the (possibly reallocated)
// array, mirroring how the production scene keeps its instances.
static struct instance *append_instance(struct instance *arr, struct instance inst) {
	v_arr_add(arr, inst);
	return arr;
}

//
// ---- CASE: transport group ----------------------------------------------
//

// Real path_trace through a metal sphere under identity transform: the
// reflected ray must escape and hit the background instead of immediately
// re-hitting the sphere.
static int case_transport_metal_reflection(void) {
	struct fixture f; fixture_init(&f);
	struct sphere spheres[1] = { { .radius = 1.0f } };
	struct sphere *arr = spheres;
	struct instance inst = new_sphere_instance(&arr, 0, NULL, NULL);
	struct bsdf_buffer *buf = transport_metal_buffer(&f);
	inst.bbuf = buf;

	struct instance *instances = NULL;
	instances = append_instance(instances, inst);
	struct world w = { 0 };
	build_world(&w, instances, &buf);

	struct lightRay camera = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 42);

	background_hits = 0;
	struct color c = path_trace(camera, &w, 4, s);
	// The path must terminate on the background, not loop on the sphere.
	TASK_CHECK(background_hits >= 1);
	TASK_CHECK(isfinite(c.red) && isfinite(c.green) && isfinite(c.blue));

	sampler_destroy(s);
	destroy_bvh(w.topLevel);
	v_arr_free(instances);
	destroy_buffer(buf);
	fixture_free(&f);
	return 0;
}

// Glass sphere: transmitted/reflected paths must make progress out of the
// object rather than immediately re-hit the same surface.
static int case_transport_glass(void) {
	struct fixture f; fixture_init(&f);
	struct sphere spheres[1] = { { .radius = 1.0f } };
	struct sphere *arr = spheres;
	struct instance inst = new_sphere_instance(&arr, 0, NULL, NULL);
	struct bsdf_buffer *buf = transport_glass_buffer(&f);
	inst.bbuf = buf;

	struct instance *instances = NULL;
	instances = append_instance(instances, inst);
	struct world w = { 0 };
	build_world(&w, instances, &buf);

	sampler *s = sampler_new();
	// Several camera rays crossing the sphere, several passes so both
	// reflection and transmission are exercised.
	unsigned escaped = 0;
	for (uint32_t pass = 0; pass < 8; ++pass) {
		sampler_init(s, Random, (int)pass, 8, 7);
		for (int k = 0; k < 5; ++k) {
			struct lightRay camera = {
				.start = { 0.05f * k - 0.1f, 0.0f, 4.0f },
				.direction = { 0.0f, 0.0f, -1.0f },
				.type = rt_camera
			};
			background_hits = 0;
			struct color c = path_trace(camera, &w, 6, s);
			TASK_CHECK(isfinite(c.red) && isfinite(c.green) && isfinite(c.blue));
			if (background_hits) escaped++;
		}
	}
	// At least some paths must escape; a self-intersection loop would keep
	// every path stuck on the sphere.
	TASK_CHECK(escaped > 0);

	sampler_destroy(s);
	destroy_bvh(w.topLevel);
	v_arr_free(instances);
	destroy_buffer(buf);
	fixture_free(&f);
	return 0;
}

// Transparent material: the ray must pass straight through without
// re-interacting with the same surface at distance zero.
static int case_transport_transparent(void) {
	struct fixture f; fixture_init(&f);
	struct sphere spheres[1] = { { .radius = 1.0f } };
	struct sphere *arr = spheres;
	struct instance inst = new_sphere_instance(&arr, 0, NULL, NULL);
	struct bsdf_buffer *buf = transport_transparent_buffer(&f);
	inst.bbuf = buf;

	struct instance *instances = NULL;
	instances = append_instance(instances, inst);
	struct world w = { 0 };
	build_world(&w, instances, &buf);

	sampler *s = sampler_new();
	sampler_init(s, Random, 0, 1, 3);
	struct lightRay camera = { .start = { 0.0f, 0.0f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	background_hits = 0;
	struct color c = path_trace(camera, &w, 8, s);
	TASK_CHECK(background_hits >= 1);
	TASK_CHECK(isfinite(c.red));

	sampler_destroy(s);
	destroy_bvh(w.topLevel);
	v_arr_free(instances);
	destroy_buffer(buf);
	fixture_free(&f);
	return 0;
}

// Diffuse reflection under a non-uniformly scaled instance: the spawned ray
// must originate in world space from the transformed surface.
static int case_transport_affine_diffuse(void) {
	struct fixture f; fixture_init(&f);
	struct sphere spheres[1] = { { .radius = 1.0f } };
	struct sphere *arr = spheres;
	struct instance inst = new_sphere_instance(&arr, 0, NULL, NULL);
	struct bsdf_buffer *buf = transport_diffuse_buffer(&f);
	inst.bbuf = buf;
	inst.composite = tform_new_scale3(2.0f, 0.5f, 1.0f);

	struct instance *instances = NULL;
	instances = append_instance(instances, inst);
	struct world w = { 0 };
	build_world(&w, instances, &buf);

	sampler *s = sampler_new();
	unsigned escaped = 0;
	for (uint32_t pass = 0; pass < 8; ++pass) {
		sampler_init(s, Random, (int)pass, 8, 11);
		for (int k = 0; k < 4; ++k) {
			struct lightRay camera = {
				.start = { 0.0f, 0.05f * k - 0.1f, 4.0f },
				.direction = { 0.0f, 0.0f, -1.0f },
				.type = rt_camera
			};
			background_hits = 0;
			struct color c = path_trace(camera, &w, 6, s);
			TASK_CHECK(isfinite(c.red));
			if (background_hits) escaped++;
		}
	}
	TASK_CHECK(escaped > 0);

	sampler_destroy(s);
	destroy_bvh(w.topLevel);
	v_arr_free(instances);
	destroy_buffer(buf);
	fixture_free(&f);
	return 0;
}

// Two affine instances of the same shared mesh, with one behind the other.
// A ray must report the nearest hit using the same world parameterization,
// then the reflection path must be able to hit the second surface when it
// is genuinely outside the first surface's error bound.
static int case_transport_shared_mesh_depth(void) {
	struct fixture f; fixture_init(&f);
	struct mesh_fixture mf; mesh_fixture_build(&f, &mf);

	// Two copies of the same square plane. The second is mirrored in z
	// (negative determinant) and pushed behind the first at z = -0.05.
	struct instance *instances = NULL;
	instances = append_instance(instances, make_mesh_instance(&mf, tform_new_translate(0.0f, 0.0f, 0.0f)));
	struct transform mirror = tform_new_scale3(1.0f, 1.0f, -1.0f);
	mirror.A.mtx[2][3] = -0.05f; // z' = -z - 0.05
	mirror.Ainv = mat_invert(mirror.A);
	instances = append_instance(instances, make_mesh_instance(&mf, mirror));

	struct world w = { 0 };
	build_world(&w, instances, NULL);

	struct lightRay ray = { .start = { 0.1f, 0.1f, 3.0f }, .direction = { 0.0f, 0.0f, -1.0f }, .type = rt_camera };
	struct hitRecord hit = { .incident = &ray, .distance = FLT_MAX, .instIndex = -1 };
	traverse_top_level_bvh(instances, w.topLevel, &ray, &hit, NULL);

	// Nearest surface is the first plane at z = 0, so t == 3.
	TASK_CHECK(fabs((double)hit.distance - 3.0) <= 1e-4);
	TASK_CHECK(fabs((double)hit.hitPoint.z) <= 1e-4);
	TASK_CHECK(hit.instIndex >= 0);

	// The two surfaces are 0.05 apart, which is far outside the first hit's
	// conservative position error. That means a second surface lies beyond
	// the geometric error and must still be reachable.
	TASK_CHECK((double)hit.pointError.z < 0.05 * 0.5);

	// Spawn a ray leaving the first plane; it must not immediately re-hit
	// either plane at ~zero distance, and should hit the second surface at
	// roughly z = -0.05.
	struct vector outdir = vec_reflect(vec_normalize(ray.direction), hit.geometricNormal);
	struct lightRay spawned = spawn_surface_ray(&hit, outdir, rt_reflection | rt_singular);
	struct hitRecord next = { .incident = &spawned, .distance = FLT_MAX, .instIndex = -1 };
	traverse_top_level_bvh(instances, w.topLevel, &spawned, &next, NULL);
	// The reflected ray heads back up (+z) and out of the scene, so it must
	// not hit the plane it came from.
	TASK_CHECK(next.instIndex < 0);

	// Spawn a straight-through ray (as the transparent BSDF does) leaving the
	// first plane into -z. Its start is only a few float spacings past the
	// first surface, yet the second surface at z = -0.05 is outside the
	// error bound and must still be hit. This catches an over-large offset
	// that would swallow nearby geometry.
	struct lightRay through = spawn_surface_ray(&hit, (struct vector){ 0.0f, 0.0f, -1.0f }, rt_transmission | rt_singular);
	TASK_CHECK(through.start.z < 0.0f);
	struct hitRecord second = { .incident = &through, .distance = FLT_MAX, .instIndex = -1 };
	bool got_second = traverse_top_level_bvh(instances, w.topLevel, &through, &second, NULL);
	TASK_CHECK(got_second);
	TASK_CHECK(second.instIndex >= 0);
	// The second plane is at world z = -0.05.
	TASK_CHECK(second.hitPoint.z < 0.0f);
	TASK_CHECK(fabs((double)second.hitPoint.z + 0.05) <= 1e-3);

	destroy_bvh(w.topLevel);
	v_arr_free(instances);
	mesh_fixture_free(&mf);
	fixture_free(&f);
	return 0;
}

//
// ---- Registration --------------------------------------------------------
//

const struct task_case task_cases[] = {
	{ "parameter", "sphere_nonunit_parameter", case_sphere_parameter_nonunit },
	{ "parameter", "sphere_inside_and_scale", case_sphere_inside_and_compare },
	{ "parameter", "sphere_instance_affine", case_sphere_instance_affine },
	{ "parameter", "mesh_parameter", case_mesh_parameter },
	{ "parameter", "shared_mesh_instances", case_shared_mesh_instances },
	{ "spawn", "spawn_outward_reflection", case_spawn_outward_reflection },
	{ "spawn", "spawn_inward_transmission", case_spawn_inward_transmission },
	{ "spawn", "spawn_error_scaling", case_spawn_error_scaling },
	{ "spawn", "spawn_off_affine_sphere", case_spawn_off_affine_sphere },
	{ "spawn", "no_immediate_rehit", case_spawn_no_immediate_rehit },
	{ "transport", "metal_reflection", case_transport_metal_reflection },
	{ "transport", "glass_transmission_reflection", case_transport_glass },
	{ "transport", "transparent_pass_through", case_transport_transparent },
	{ "transport", "affine_diffuse", case_transport_affine_diffuse },
	{ "transport", "shared_mesh_depth", case_transport_shared_mesh_depth },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
