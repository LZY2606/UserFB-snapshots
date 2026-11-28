//
//  instance.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 23.6.2020.
//  Copyright © 2020-2025 Valtteri Koskivuori. All rights reserved.
//

#include "../../includes.h"
#include <common/vector.h>
#include <accelerators/bvh.h>
#include <renderer/pathtrace.h>
#include "instance.h"
#include <datatypes/bbox.h>
#include <datatypes/mesh.h>
#include <datatypes/sphere.h>
#include <datatypes/scene.h>

#include <float.h>
#include <math.h>

struct sphereVolume {
	struct sphere *sphere;
	float density;
};

struct meshVolume {
	struct mesh *mesh;
	float density;
};

static inline struct vector normalize_or(struct vector v, struct vector fallback) {
	const double len = sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z);
	if (!(len > 0.0) || !isfinite(len))
		return fallback;
	return (struct vector){
		(float)((double)v.x / len),
		(float)((double)v.y / len),
		(float)((double)v.z / len),
	};
}

// Determinant of the linear (upper-left 3x3) part of the affine transform.
static inline double linear_determinant(const struct matrix4x4 m) {
	return (double)m.mtx[0][0] * ((double)m.mtx[1][1] * m.mtx[2][2] - (double)m.mtx[1][2] * m.mtx[2][1])
		 - (double)m.mtx[0][1] * ((double)m.mtx[1][0] * m.mtx[2][2] - (double)m.mtx[1][2] * m.mtx[2][0])
		 + (double)m.mtx[0][2] * ((double)m.mtx[1][0] * m.mtx[2][1] - (double)m.mtx[1][1] * m.mtx[2][0]);
}

// Transform an object-space hit record into world space.
//
// * The hit point is transformed by the instance's affine transform.
// * The conservative position error is transformed by the absolute value of
//   that transform, so non-uniform scaling and shear expand it correctly.
// * The geometric normal is transformed by the inverse-transpose. For
//   triangles we additionally flip it by the sign of the determinant so the
//   result is the transformed oriented edge cross product; for spheres the
//   inverse-transpose already yields the outward implicit-surface normal.
// * The shading (surface) normal is transformed the same way, preserving the
//   object-space "faces the incident ray" convention.
static void transform_hit_to_world(
	const struct instance *instance,
	const struct lightRay *world_ray,
	struct hitRecord *isect,
	bool flip_geometric_by_det)
{
	const struct matrix4x4 A = instance->composite.A;
	const struct matrix4x4 Ainv = instance->composite.Ainv;
	const double eps = (double)FLT_EPSILON;

	const struct vector op = isect->hitPoint;
	const struct vector oe = isect->pointError;
	const double t = isect->distance;

	const double ox = op.x, oy = op.y, oz = op.z;
	const double wx = (double)A.mtx[0][0] * ox + (double)A.mtx[0][1] * oy + (double)A.mtx[0][2] * oz + (double)A.mtx[0][3];
	const double wy = (double)A.mtx[1][0] * ox + (double)A.mtx[1][1] * oy + (double)A.mtx[1][2] * oz + (double)A.mtx[1][3];
	const double wz = (double)A.mtx[2][0] * ox + (double)A.mtx[2][1] * oy + (double)A.mtx[2][2] * oz + (double)A.mtx[2][3];

	const struct vector wp = { (float)wx, (float)wy, (float)wz };

	const double rows[3] = { wx, wy, wz };
	const double ray_start[3] = { world_ray->start.x, world_ray->start.y, world_ray->start.z };
	const double ray_dir_t[3] = {
		t * (double)world_ray->direction.x,
		t * (double)world_ray->direction.y,
		t * (double)world_ray->direction.z,
	};
	const double obj_err[3] = { fabs(oe.x), fabs(oe.y), fabs(oe.z) };

	struct vector werr;
	float *werr_components[3] = { &werr.x, &werr.y, &werr.z };
	for (int i = 0; i < 3; ++i) {
		const double row0 = A.mtx[i][0], row1 = A.mtx[i][1], row2 = A.mtx[i][2];
		// Error mapped through the absolute transform.
		double err = fabs(row0) * obj_err[0] + fabs(row1) * obj_err[1] + fabs(row2) * obj_err[2];
		// Rounding of the world-space matrix multiply itself.
		err += 16.0 * eps * (fabs(row0 * ox) + fabs(row1 * oy) + fabs(row2 * oz) + fabs((double)A.mtx[i][3]));
		// Storing the resulting world point as float.
		err += 16.0 * eps * fabs(rows[i]);
		// Consistency with start + t * direction on the original ray.
		err += 8.0 * eps * (fabs(ray_start[i]) + fabs(ray_dir_t[i]));
		*werr_components[i] = (float)err;
	}

	struct vector geometric = isect->geometricNormal;
	tform_vector_transpose(&geometric, Ainv);
	if (flip_geometric_by_det && linear_determinant(A) < 0.0)
		geometric = vec_negate(geometric);
	geometric = normalize_or(geometric, isect->geometricNormal);

	struct vector shading = isect->surfaceNormal;
	tform_vector_transpose(&shading, Ainv);
	shading = normalize_or(shading, geometric);

	isect->hitPoint = wp;
	isect->pointError = werr;
	isect->geometricNormal = geometric;
	isect->surfaceNormal = shading;
}

static inline struct coord getTexMapSphere(const struct hitRecord *isect) {
	struct vector ud = isect->surfaceNormal;
	//To polar from cartesian
	float phi = atan2f(ud.z, ud.x);
	float theta = asinf(ud.y);
	float v = (theta + PI / 2.0f) / PI;
	float u = 1.0f - (phi + PI) / (PI * 2.0f);
	u = wrap_min_max(u, 0.0f, 1.0f);
	v = wrap_min_max(v, 0.0f, 1.0f);
	return (struct coord){ u, v };
}

static bool intersectSphere(const struct instance *instance, const struct lightRay *ray, struct hitRecord *isect, sampler *sampler) {
	(void)sampler;
	struct lightRay copy = *ray;
	tform_ray(&copy, instance->composite.Ainv);
	struct sphere *sphere = &(*((struct sphere **)instance->object_arr))[instance->object_idx];
	if (rayIntersectsWithSphere(&copy, sphere, isect)) {
		isect->uv = getTexMapSphere(isect);
		isect->polygon = NULL;
		isect->bsdf = instance->bbuf->bsdfs[0];
		transform_hit_to_world(instance, ray, isect, false);
		return true;
	}
	return false;
}

static bool intersectSphereVolume(const struct instance *instance, const struct lightRay *ray, struct hitRecord *isect, sampler *sampler) {
	(void)instance;
	(void)ray;
	(void)isect;
	(void)sampler;
	return false;
}

static void getSphereBBoxAndCenter(const struct instance *instance, struct boundingBox *bbox, struct vector *center) {
	struct sphere *sphere = &(*((struct sphere **)instance->object_arr))[instance->object_idx];
	bbox->min = (struct vector){ -sphere->radius, -sphere->radius, -sphere->radius };
	bbox->max = (struct vector){  sphere->radius,  sphere->radius,  sphere->radius };
	*center = vec_zero();
	tform_bbox(bbox, instance->composite.A);
	*center = bboxCenter(bbox);
}

static void getSphereVolumeBBoxAndCenter(const struct instance *instance, struct boundingBox *bbox, struct vector *center) {
	(void)instance;
	(void)bbox;
	(void)center;
}

struct instance new_sphere_instance(struct sphere **spheres, size_t idx, float *density, struct block **pool) {
	if (density && pool) {
		struct sphereVolume *volume = allocBlock(pool, sizeof(*volume));
		volume->sphere = NULL;//sphere;
		volume->density = *density;
		return (struct instance) {
			.object_arr = NULL,
			.object_idx = 0,
			.composite = tform_new(),
			.intersectFn = intersectSphereVolume,
			.getBBoxAndCenterFn = getSphereVolumeBBoxAndCenter
		};
	} else {
		return (struct instance) {
			.object_arr = spheres,
			.object_idx = idx,
			.composite = tform_new(),
			.intersectFn = intersectSphere,
			.getBBoxAndCenterFn = getSphereBBoxAndCenter
		};
	}
}

static void mesh_uv(const struct mesh *mesh, struct hitRecord *isect) {
	if (!v_arr_len(mesh->vbuf.texture_coords))
		return;
	struct poly *p = isect->polygon;
	if (!p || p->textureIndex[0] == -1)
		return;

	// Barycentric coordinates for this polygon
	const float b_u = isect->uv.x;
	const float b_v = isect->uv.y;
	const float b_w = 1.0f - b_u - b_v;

	// Weighted texture coordinates
	const struct coord u = coord_scale(b_u, mesh->vbuf.texture_coords[p->textureIndex[1]]);
	const struct coord v = coord_scale(b_v, mesh->vbuf.texture_coords[p->textureIndex[2]]);
	const struct coord w = coord_scale(b_w, mesh->vbuf.texture_coords[p->textureIndex[0]]);

	isect->uv = coord_add(coord_add(u, v), w);
}

static bool intersectMesh(const struct instance *instance, const struct lightRay *ray, struct hitRecord *isect, sampler *sampler) {
	struct lightRay copy = *ray;
	tform_ray(&copy, instance->composite.Ainv);
	struct mesh *mesh = &(*((struct mesh **)instance->object_arr))[instance->object_idx];
	if (traverse_bottom_level_bvh(mesh, &copy, isect, sampler)) {
		isect->bsdf = instance->bbuf->bsdfs[isect->polygon->materialIndex];
		transform_hit_to_world(instance, ray, isect, true);
		// Repopulate uv with actual texture mapping
		mesh_uv(mesh, isect);
		return true;
	}
	return false;
}

static bool intersectMeshVolume(const struct instance *instance, const struct lightRay *ray, struct hitRecord *isect, sampler *sampler) {
	(void)instance;
	(void)ray;
	(void)isect;
	(void)sampler;
	return false;
}

enum cr_instance_type instance_type(const struct instance *i) {
	if (i->intersectFn == intersectMesh) return CR_I_MESH;
	if (i->intersectFn == intersectSphere) return CR_I_SPHERE;
	return CR_I_UNKNOWN;
}

static void getMeshBBoxAndCenter(const struct instance *instance, struct boundingBox *bbox, struct vector *center) {
	struct mesh *mesh = &(*((struct mesh **)instance->object_arr))[instance->object_idx];
	*bbox = get_transformed_root_bbox(mesh->bvh, &instance->composite.A);
	*center = bboxCenter(bbox);
}

static void getMeshVolumeBBoxAndCenter(const struct instance *instance, struct boundingBox *bbox, struct vector *center) {
	(void)instance;
	(void)bbox;
	(void)center;
}

struct instance new_mesh_instance(struct mesh **meshes, size_t idx, float *density, struct block **pool) {
	if (density && pool) {
		struct meshVolume *volume = allocBlock(pool, sizeof(*volume));
		volume->mesh = NULL;//mesh;
		volume->density = *density;
		return (struct instance) {
			//FIXME
			.object_arr = NULL,
			.object_idx = 0,
			.composite = tform_new(),
			.intersectFn = intersectMeshVolume,
			.getBBoxAndCenterFn = getMeshVolumeBBoxAndCenter
		};
	} else {
		return (struct instance) {
			.object_arr = meshes,
			.object_idx = idx,
			.composite = tform_new(),
			.intersectFn = intersectMesh,
			.getBBoxAndCenterFn = getMeshBBoxAndCenter
		};
	}
}
