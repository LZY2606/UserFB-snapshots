#ifndef POINT_SHADOW_H
#define POINT_SHADOW_H

#include "graphics.h"
#include "maths.h"

/*
 * omnidirectional point-light shadow
 *
 * six square framebuffers store the [0, 1] perspective depths rendered from
 * the light position, one per cubemap face. the face order and coordinates
 * follow the existing cubemap convention: px, nx, py, ny, pz, nz.
 */
typedef struct {
    vec3_t position;
    float near;
    float far;
    float bias;
    framebuffer_t *faces[6];
} point_shadow_t;

/* resource management */
point_shadow_t *point_shadow_create(vec3_t position, float near, float far,
                                    int size, float bias);
void point_shadow_release(point_shadow_t *point_shadow);

/*
 * fraction (in [0, 1]) of the nine depth taps that consider the given world
 * position visible from the point light
 */
float point_shadow_visibility(const point_shadow_t *point_shadow,
                              vec3_t world_position);

/*
 * view and projection matrices of one cubemap face. the projection is a
 * 90 degree perspective frustum whose near and far are positive distances
 * along the viewing axis, not a spherical range.
 */
mat4_t point_shadow_view_matrix(const point_shadow_t *point_shadow,
                                int face_index);
mat4_t point_shadow_proj_matrix(const point_shadow_t *point_shadow);

#endif
