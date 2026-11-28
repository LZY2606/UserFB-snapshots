//
//  ray_spawn.c
//  c-ray
//
//  Copyright © 2026 Valtteri Koskivuori. All rights reserved.
//

#include "../../includes.h"
#include "ray_spawn.h"

#include <float.h>
#include <math.h>
#include <datatypes/hitrecord.h>

// Next representable float above `x` (away from -infinity for negative x).
static inline double next_float_up(float x) {
	return (double)nextafterf(x, INFINITY);
}

// The amount by which a float coordinate must move so that its stored
// representation can no longer round back onto the original value. This is
// the local floating point spacing, not a scene-sized epsilon.
static inline double float_spacing(float x) {
	const double up = next_float_up(x);
	return fabs(up - (double)x);
}

struct lightRay spawn_surface_ray(const struct hitRecord *hit, struct vector direction, enum ray_type type) {
	// Fall back to the recorded point if there is no usable surface frame.
	if (!hit) {
		return (struct lightRay){ .start = vec_zero(), .direction = direction, .type = type };
	}

	struct vector normal = hit->geometricNormal;
	double nlen = sqrt((double)normal.x * normal.x +
					   (double)normal.y * normal.y +
					   (double)normal.z * normal.z);

	// A valid geometric normal must be a finite, non-zero vector. If it is
	// not, there is no meaningful side to leave through; leave the start
	// untouched rather than inventing an offset.
	if (!isfinite(nlen) || nlen <= 0.0) {
		return (struct lightRay){ .start = hit->hitPoint, .direction = direction, .type = type };
	}

	normal.x = (float)((double)normal.x / nlen);
	normal.y = (float)((double)normal.y / nlen);
	normal.z = (float)((double)normal.z / nlen);

	// Choose the side of the geometric surface that the outgoing ray leaves
	// through. Only the geometric normal is consulted here: a smoothed
	// shading normal must never decide the offset side. A direction exactly
	// tangent to the surface is treated as leaving through the outward side.
	const double dn = (double)direction.x * normal.x +
					  (double)direction.y * normal.y +
					  (double)direction.z * normal.z;
	const double side = dn < 0.0 ? -1.0 : 1.0;

	// Project the conservative position error box onto the geometric normal.
	// This bounds how far the exact surface point can be from the recorded
	// hit point along the direction we are leaving in.
	const double ex = fabs((double)hit->pointError.x);
	const double ey = fabs((double)hit->pointError.y);
	const double ez = fabs((double)hit->pointError.z);
	const double projected_error =
		fabs((double)normal.x) * ex +
		fabs((double)normal.y) * ey +
		fabs((double)normal.z) * ez;

	// Account for the fact that the resulting start is itself stored in
	// floats. One spacing per axis, projected on the normal, guarantees that
	// rounding cannot drag the start back inside the error interval.
	const double spacing =
		fabs((double)normal.x) * float_spacing(hit->hitPoint.x) +
		fabs((double)normal.y) * float_spacing(hit->hitPoint.y) +
		fabs((double)normal.z) * float_spacing(hit->hitPoint.z);

	// A small multiple of the spacing is added on top of the error bound so
	// the rounded start lies strictly outside it, while still staying local
	// to this surface event.
	double offset = projected_error + 2.0 * spacing;

	if (!isfinite(offset) || offset < 0.0) {
		return (struct lightRay){ .start = hit->hitPoint, .direction = direction, .type = type };
	}

	struct vector start = {
		(float)((double)hit->hitPoint.x + side * offset * (double)normal.x),
		(float)((double)hit->hitPoint.y + side * offset * (double)normal.y),
		(float)((double)hit->hitPoint.z + side * offset * (double)normal.z),
	};

	return (struct lightRay){ .start = start, .direction = direction, .type = type };
}
