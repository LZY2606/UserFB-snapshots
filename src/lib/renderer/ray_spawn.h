//
//  ray_spawn.h
//  c-ray
//
//  Copyright © 2026 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <datatypes/lightray.h>

struct hitRecord;

// Spawn a ray that leaves a surface event at `hit`.
//
// The starting point is offset from the recorded world-space hit point along
// the side of the geometric surface that `direction` leaves through. The
// offset is derived from the hit record's conservative position error plus the
// local floating point spacing; no scene-, object- or bounding box-sized
// epsilon is used. `direction` and `type` are preserved verbatim.
struct lightRay spawn_surface_ray(const struct hitRecord *hit, struct vector direction, enum ray_type type);
