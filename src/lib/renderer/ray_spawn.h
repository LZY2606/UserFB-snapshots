//
//  ray_spawn.h
//  c-ray
//
//  Surface secondary ray generation.
//
//  Copyright © 2025 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <datatypes/hitrecord.h>
#include <datatypes/lightray.h>

// Builds a secondary ray leaving a surface hit.
//
// The side of the surface the new ray starts on is chosen from the sign of
// dot(direction, geometricNormal), so a transmission direction continues through
// the surface while a reflection direction stays on the incident side.  The origin
// is displaced only as far as the conservative world-space point error of the hit
// (plus one floating-point representability step) requires, which:
//   - prevents immediate re-intersection with the same surface,
//   - still allows hitting another surface that lies beyond the error bound,
//   - never relies on a scene-wide epsilon or a bounding-box size.
//
// The supplied direction and ray type are preserved verbatim.  Callers must not
// offset the origin themselves; hits without a surface event (volumes, background)
// must not be routed through this entry point.
struct lightRay spawn_surface_ray(const struct hitRecord *hit, struct vector direction, enum ray_type type);
