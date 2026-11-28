#ifndef POINT_SHADOW_H
#define POINT_SHADOW_H

#include "graphics.h"
#include "maths.h"

/*
 * omnidirectional point light shadow.
 *
 * face order and orientation follow the cubemap convention used by the
 * renderer: positive x, negative x, positive y, negative y, positive z,
 * negative z.
 *
 * for a face texel center (x, y) with u = (x + 0.5) / size and
 * v = (y + 0.5) / size, let s = 2 * u - 1 and t = 2 * v - 1. the
 * directions reconstructed on the six faces are:
 *
 *     px: ( 1, -t, -s)
 *     nx: (-1, -t,  s)
 *     py: ( s,  1,  t)
 *     ny: ( s, -1, -t)
 *     pz: ( s, -t,  1)
 *     nz: (-s, -t, -1)
 *
 * "near" and "far" are positive distances along each face's view axis.
 * each face stores the rasterizer's [0, 1] perspective depth.
 */

typedef struct point_shadow_t {
    vec3_t position;
    float near;
    float far;
    float bias;
    int size;
    framebuffer_t *faces[6];
    /* capture matrices, paired with the face layout described above */
    mat4_t proj_matrix;
    mat4_t view_matrices[6];
    mat4_t view_proj_matrices[6];
} point_shadow_t;

/* point shadow creating/releasing */
point_shadow_t *point_shadow_create(vec3_t position, float near, float far,
                                    int size, float bias);
void point_shadow_release(point_shadow_t *point_shadow);

/*
 * returns the fraction of the nine depth taps around world_position that
 * are visible from the point light; 1 means fully lit.
 */
float point_shadow_visibility(const point_shadow_t *point_shadow,
                              vec3_t world_position);

#endif
