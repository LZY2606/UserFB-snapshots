//
//  ray_spawn.c
//  c-ray
//
//  Surface secondary ray generation.
//
//  Copyright © 2025 Valtteri Koskivuori. All rights reserved.
//

#include "../../includes.h"
#include "ray_spawn.h"

#include <math.h>
#include <float.h>

// Snap x one floating-point representability step in the direction of s (+inf or -inf).
// A zero component means the origin does not need to move on that axis.  If x is
// already at the end of the finite range it is returned unchanged, so the resulting
// ray origin always stays finite.
static float step_away(float x, float s) {
	if (s == 0.0f)
		return x;
	float y = s > 0.0f ? nextafterf(x, FLT_MAX) : nextafterf(x, -FLT_MAX);
	return isfinite(y) ? y : x;
}

struct lightRay spawn_surface_ray(const struct hitRecord *hit, struct vector direction, enum ray_type type) {
	struct lightRay out = {
		.direction = direction,
		.type = type,
	};

	const struct vector p = hit->hitPoint;
	struct vector n = hit->geometricNormal;
	// Defensive: every hit path stores a unit geometric normal, but a hand-built
	// record must not be able to produce a degenerate offset.
	const float nLength = vec_length(n);
	if (!(nLength > 0.0f))
		n = hit->surfaceNormal;
	if (vec_length(n) > 0.0f)
		n = vec_normalize(n);

	struct vector e = hit->pointError;
	e.x = max(0.0f, e.x);
	e.y = max(0.0f, e.y);
	e.z = max(0.0f, e.z);

	// Choose the side of the surface the outgoing direction actually travels to.
	// A transmitted (dot < 0) ray continues into the surface, a reflected one
	// (dot >= 0) stays on the incident side.  The geometric normal decides this,
	// never the smoothed shading normal.
	const float side = vec_dot(direction, n) >= 0.0f ? 1.0f : -1.0f;
	const struct vector nSide = vec_scale(n, side);

	// Distance along the normal that clears the whole per-axis error box:
	// for any q with |q-p| <= e, (p + nSide*d - q).nSide >= 0.  This follows the
	// OffsetRayOrigin construction (see e.g. "Physically Based Rendering", 3rd ed.),
	// using only the hit's own conservative error rather than a global epsilon.
	const float normalError = fabsf(nSide.x) * e.x + fabsf(nSide.y) * e.y + fabsf(nSide.z) * e.z;
	struct vector origin = vec_add(p, vec_scale(nSide, normalError));

	// Round each coordinate one representable interval away from the surface, so the
	// origin is strictly (not merely approximately) on the chosen side even when the
	// normal error itself rounds to zero.
	origin.x = step_away(origin.x, nSide.x);
	origin.y = step_away(origin.y, nSide.y);
	origin.z = step_away(origin.z, nSide.z);

	out.start = origin;
	return out;
}
