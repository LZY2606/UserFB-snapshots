//
//  poly.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 28/02/2015.
//  Copyright © 2015-2025 Valtteri Koskivuori. All rights reserved.
//

#include "../../includes.h"
#include "poly.h"

#include "lightray.h"
#include <common/vector.h>
#include <renderer/pathtrace.h>
#include <datatypes/mesh.h>
#include <float.h>

// Conservative object-space per-axis error of a point produced by the
// Möller–Trumbore edge cross products and the ray equation r(t) = start + t*dir.
// Every bound is a sum of absolute operand magnitudes scaled by a small integer
// number of ulps; no scene-size-dependent epsilon is involved.
static struct vector polygon_local_error(const struct mesh *mesh, const struct poly *poly,
										 const struct vector edges[2],
										 const struct lightRay *ray, float t) {
	const struct vector v0 = mesh->vbuf.vertices[poly->vertexIndex[0]];
	const float edgeMagnitude = vec_length(edges[0]) + vec_length(edges[1]);
	const float vertexMagnitude = fabsf(v0.x) + fabsf(v0.y) + fabsf(v0.z);
	const float rayMagnitude =
		fabsf(ray->start.x) + fabsf(ray->start.y) + fabsf(ray->start.z) +
		fabsf(t * ray->direction.x) + fabsf(t * ray->direction.y) + fabsf(t * ray->direction.z);
	const float e = 6.0f * FLT_EPSILON * (rayMagnitude + vertexMagnitude + edgeMagnitude) + FLT_MIN;
	return (struct vector){ e, e, e };
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
			// Unit geometric normal from the oriented edge cross product.  It is never
			// smoothed or flipped toward the incident ray; the instance transform
			// (including negative determinants) is applied by the instance code.
			isect->geometricNormal = vec_normalize(n);
			if (likely(poly->hasNormals)) {
				struct vector upcomp = vec_scale(mesh->vbuf.normals[poly->normalIndex[1]], u);
				struct vector vpcomp = vec_scale(mesh->vbuf.normals[poly->normalIndex[2]], v);
				struct vector wpcomp = vec_scale(mesh->vbuf.normals[poly->normalIndex[0]], w);

				isect->surfaceNormal = vec_add(vec_add(upcomp, vpcomp), wpcomp);
			} else {
				isect->surfaceNormal = n;
			}
			// Support two-sided materials by flipping the normal if needed
			if (vec_dot(ray->direction, isect->surfaceNormal) >= 0.0f) isect->surfaceNormal = vec_negate(isect->surfaceNormal);
			isect->hitPoint = alongRay(ray, t);
			isect->pointError = polygon_local_error(mesh, poly, (const struct vector[2]){ e1, e2 }, ray, t);
			return true;
		}
	}
	return false;
}
