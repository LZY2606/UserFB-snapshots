//
//  poly.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 28/02/2015.
//  Copyright © 2015-2025 Valtteri Koskivuori. All rights reserved.
//

#include "../../includes.h"
#include "poly.h"

#include <float.h>
#include "lightray.h"
#include <common/vector.h>
#include <renderer/pathtrace.h>
#include <datatypes/mesh.h>

//Conservative per-coordinate position error for `start + t * direction`.
static inline struct vector ray_point_error(struct vector point, struct vector start, float t, struct vector direction) {
	const double term[] = {
		fabs(point.x) + fabs(start.x) + fabs((double)t * direction.x),
		fabs(point.y) + fabs(start.y) + fabs((double)t * direction.y),
		fabs(point.z) + fabs(start.z) + fabs((double)t * direction.z),
	};
	const double factor = 8.0 * (double)FLT_EPSILON;
	return (struct vector){
		(float)(factor * term[0]),
		(float)(factor * term[1]),
		(float)(factor * term[2]),
	};
}

bool rayIntersectsWithPolygon(const struct mesh *mesh, const struct lightRay *ray, const struct poly *poly, struct hitRecord *isect) {
	// Möller-Trumbore ray-triangle intersection routine
	// (see "Fast, Minimum Storage Ray-Triangle Intersection", by T. Möller and B. Trumbore)
	struct vector e1 = vec_sub(mesh->vbuf.vertices[poly->vertexIndex[0]], mesh->vbuf.vertices[poly->vertexIndex[1]]);
	struct vector e2 = vec_sub(mesh->vbuf.vertices[poly->vertexIndex[2]], mesh->vbuf.vertices[poly->vertexIndex[0]]);
	struct vector n = vec_cross(e1, e2);

	struct vector c = vec_sub(mesh->vbuf.vertices[poly->vertexIndex[0]], ray->start);
	struct vector r = vec_cross(ray->direction, c);
	float invDet = 1.0f / vec_dot(n, ray->direction);

	float u = vec_dot(r, e2) * invDet;
	float v = vec_dot(r, e1) * invDet;
	float w = 1.0f - u - v;

	// This order of comparisons guarantees that none of u, v, or t, are NaNs:
	// IEEE-754 mandates that they compare to false if the left hand side is a NaN.
	if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f) {
		float t = vec_dot(n, c) * invDet;
		if (t >= 0.0f && t < isect->distance) {
			isect->uv = (struct coord) { u, v };
			isect->distance = t;

			// The geometric normal is the unit direction of the triangle's
			// oriented edge cross product, (v1 - v0) x (v2 - v0). It is never
			// smoothed and never flipped towards the incident ray; the
			// instance transform maps it to world space.
			struct vector g1 = vec_sub(mesh->vbuf.vertices[poly->vertexIndex[1]], mesh->vbuf.vertices[poly->vertexIndex[0]]);
			struct vector g2 = vec_sub(mesh->vbuf.vertices[poly->vertexIndex[2]], mesh->vbuf.vertices[poly->vertexIndex[0]]);
			isect->geometricNormal = vec_normalize(vec_cross(g1, g2));

			if (likely(poly->hasNormals)) {
				struct vector upcomp = vec_scale(mesh->vbuf.normals[poly->normalIndex[1]], u);
				struct vector vpcomp = vec_scale(mesh->vbuf.normals[poly->normalIndex[2]], v);
				struct vector wpcomp = vec_scale(mesh->vbuf.normals[poly->normalIndex[0]], w);

				isect->surfaceNormal = vec_add(vec_add(upcomp, vpcomp), wpcomp);
			} else {
				isect->surfaceNormal = n;
			}
			// Support two-sided materials by flipping the shading normal if needed
			if (vec_dot(ray->direction, isect->surfaceNormal) >= 0.0f) isect->surfaceNormal = vec_negate(isect->surfaceNormal);
			isect->hitPoint = alongRay(ray, t);
			isect->pointError = ray_point_error(isect->hitPoint, ray->start, t, ray->direction);
			return true;
		}
	}
	return false;
}
