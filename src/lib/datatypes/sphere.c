//
//  sphere.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 28/02/2015.
//  Copyright © 2015-2025 Valtteri Koskivuori. All rights reserved.
//

#include "../../includes.h"
#include "sphere.h"

#include <renderer/pathtrace.h>
#include "lightray.h"
#include <float.h>
#include <math.h>

// Intersects the implicit sphere |x|^2 = r^2 with the ray r(t) = start + t*direction
// without normalizing direction.  Both positive roots are reported as parameters of
// the *original* ray equation, so callers must never compare them against a
// length measured on a normalized direction.
static bool intersect(const struct lightRay *ray, const struct sphere *sphere, float tMax, float roots[2]) {
	// Vector dot product of the direction
	const float A = vec_dot(ray->direction, ray->direction);
	if (!(A > 0.0f) || !isfinite(A))
		return false;

	// Distance between start of a lightRay and the sphere position
	const float B = 2.0f * vec_dot(ray->direction, ray->start);

	const float C = vec_dot(ray->start, ray->start) - (sphere->radius * sphere->radius);

	const float trigDiscriminant = B * B - 4.0f * A * C;

	// If discriminant is negative, no real roots and the ray has missed the sphere
	if (!(trigDiscriminant >= 0.0f))
		return false;

	const float sqrtOfDiscriminant = sqrtf(trigDiscriminant);
	float t0 = (-B - sqrtOfDiscriminant) / (2.0f * A);
	float t1 = (-B + sqrtOfDiscriminant) / (2.0f * A);
	if (t0 > t1) {
		const float tmp = t0;
		t0 = t1;
		t1 = tmp;
	}

	if (t1 <= 0.0f || t0 >= tMax)
		return false;

	roots[0] = t0;
	roots[1] = t1;
	return true;
}

// Conservative per-axis position error (in object space) of a point on the sphere
// surface: a few ulps of the operands of the ray equation plus a small term for the
// deviation of the rounded point from the implicit surface itself.
static struct vector sphere_local_error(const struct vector *point, const struct lightRay *ray, float t) {
	const float term = 5.0f * FLT_EPSILON *
		(fabsf(ray->start.x) + fabsf(t * ray->direction.x) +
		 fabsf(ray->start.y) + fabsf(t * ray->direction.y) +
		 fabsf(ray->start.z) + fabsf(t * ray->direction.z));
	const float surface = FLT_EPSILON * (vec_length(*point) + FLT_MIN);
	const float e = term + surface + FLT_MIN;
	return (struct vector){ e, e, e };
}

bool rayIntersectsWithSphere(const struct lightRay *ray, const struct sphere *sphere, struct hitRecord *isect) {
	float roots[2];
	if (!intersect(ray, sphere, isect->distance, roots))
		return false;

	// Pick the nearest root that is strictly in front of the ray origin.  A ray
	// starting inside the volume (t0 <= 0, e.g. after refraction) keeps the exit
	// root instead, so the two roots are never mixed up.
	float t = roots[0] > 0.0f ? roots[0] : roots[1];
	if (!(t > 0.0f) || t >= isect->distance)
		return false;

	isect->distance = t;
	isect->hitPoint = alongRay(ray, t);
	// Outward unit normal of the implicit surface |x|^2 = r^2.  Computed from the
	// local hit point rather than normalized after a transpose transform, so a
	// non-uniformly scaled ellipsoid gets a correct geometric normal as well.
	isect->geometricNormal = vec_normalize(isect->hitPoint);
	isect->surfaceNormal = isect->geometricNormal;
	isect->pointError = sphere_local_error(&isect->hitPoint, ray, t);
	isect->polygon = NULL;
	return true;
}
