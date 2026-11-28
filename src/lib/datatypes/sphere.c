//
//  sphere.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 28/02/2015.
//  Copyright © 2015-2025 Valtteri Koskivuori. All rights reserved.
//

#include "../../includes.h"
#include "sphere.h"

#include <float.h>
#include <renderer/pathtrace.h>
#include "lightray.h"

// A conservative, per-coordinate position error for an object-space hit
// point. The hit point is produced by evaluating start + t * direction, so
// the error in each component is bounded by the magnitude of the terms that
// contributed to it. The factor is deliberately small: it only covers
// floating point rounding, not any world-space epsilon or bounding box size.
static inline struct vector object_space_point_error(
	struct vector point, struct vector start, double t, struct vector direction)
{
	const double term[] = {
		fabs(point.x) + fabs(start.x) + fabs(t) * fabs(direction.x),
		fabs(point.y) + fabs(start.y) + fabs(t) * fabs(direction.y),
		fabs(point.z) + fabs(start.z) + fabs(t) * fabs(direction.z),
	};
	const double factor = 8.0 * (double)FLT_EPSILON;
	return (struct vector){
		(float)(factor * term[0]),
		(float)(factor * term[1]),
		(float)(factor * term[2]),
	};
}

//Calculates intersection with a sphere and a light ray.
//The returned t is the parameter along the *original* ray (which need not be
//normalized): the hit point is always start + t * direction.
bool intersect(const struct lightRay *ray, const struct sphere *sphere, float *t) {
	// Work in double precision internally so that the reported parameter is
	// not needlessly degraded by the quadratic solve. A must not be assumed
	// to be 1: callers may pass any (non-zero) direction vector.
	const double dx = ray->direction.x;
	const double dy = ray->direction.y;
	const double dz = ray->direction.z;
	const double sx = ray->start.x;
	const double sy = ray->start.y;
	const double sz = ray->start.z;

	const double A = dx * dx + dy * dy + dz * dz;

	// A degenerate (zero length) direction can never hit anything.
	if (A == 0.0)
		return false;

	// Center of the sphere is at the object-space origin.
	const double B = 2.0 * (dx * sx + dy * sy + dz * sz);
	const double C = (sx * sx + sy * sy + sz * sz) -
		((double)sphere->radius * (double)sphere->radius);

	const double trigDiscriminant = B * B - 4.0 * A * C;

	//If discriminant is negative, no real roots and the ray has missed the sphere
	if (trigDiscriminant < 0.0)
		return false;

	const double sqrtOfDiscriminant = sqrt(trigDiscriminant);
	const double inv2A = 1.0 / (2.0 * A);
	double t0 = (-B + sqrtOfDiscriminant) * inv2A;
	double t1 = (-B - sqrtOfDiscriminant) * inv2A;

	// Pick the nearest intersection in front of the ray origin. Both roots
	// are real parameters along the same (possibly non-unit) direction, so
	// they can be compared directly without normalizing anything.
	double hit = 0.0;
	if (t0 < t1) {
		const double tmp = t0;
		t0 = t1;
		t1 = tmp;
	}
	// Now t1 <= t0.
	if (t1 > 0.0) {
		// Ray starts outside the sphere: nearest front-facing root.
		hit = t1;
	} else if (t0 > 0.0) {
		// Ray starts inside the sphere: the only root in front of us.
		hit = t0;
	} else {
		// Both intersections are behind the ray origin.
		return false;
	}

	//Verify intersection is less than the original distance
	if (hit > (double)*t)
		return false;

	*t = (float)hit;
	return true;
}

bool rayIntersectsWithSphere(const struct lightRay *ray, const struct sphere *sphere, struct hitRecord *isect) {
	if (intersect(ray, sphere, &isect->distance)) {
		//Compute normal and store it to isect
		const double t = isect->distance;
		const struct vector point = {
			(float)((double)ray->start.x + t * (double)ray->direction.x),
			(float)((double)ray->start.y + t * (double)ray->direction.y),
			(float)((double)ray->start.z + t * (double)ray->direction.z),
		};
		isect->hitPoint = point;
		// The sphere is centered at the object-space origin, so the outward
		// geometric normal is simply the normalized position vector. It is
		// never flipped towards the incident ray.
		isect->geometricNormal = vec_normalize(point);
		isect->surfaceNormal = isect->geometricNormal;
		isect->pointError = object_space_point_error(point, ray->start, t, ray->direction);
		isect->polygon = NULL;
		return true;
	}
	return false;
}
