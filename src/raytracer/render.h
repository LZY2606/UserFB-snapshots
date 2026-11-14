/*
 * Copyright (c) 2021-2022 Wojciech Graj
 *
 * Licensed under the MIT license: https://opensource.org/licenses/MIT
 * Permission is granted to use, copy, modify, and redistribute the work.
 * Full license information available in the project LICENSE file.
 *
 * DESCRIPTION:
 *   Image rendering
 **/

#ifndef __RENDER_H__
#define __RENDER_H__

#include <stddef.h>

#include "lighttree.h"
#include "type.h"

void render_init(void);
void render(void);

/*
 * Install a snapshot of point lights for the deterministic hierarchical
 * diffuse direct-light channel.  Replaces any previously installed tree; the
 * caller's array is not retained.  offset is independent of the legacy light
 * attenuation offset.  Call render_clear_point_lights (or install an empty
 * set) to disable the channel, restoring the legacy output and random stream.
 */
void render_set_point_lights(const struct PointLight *lights, size_t count, double offset, double abs_tol, double rel_tol);

void render_clear_point_lights(void);

extern v3 global_ambient_light_intensity;

#endif /* __RENDER_H__ */
