#ifndef ELLIPSOID_H
#define ELLIPSOID_H

#include "graphics.h"
#include "maths.h"

/*
 * analytic ellipsoid primitive
 *
 * the local geometry is the unit sphere |local_position| = 1, mapped to world
 * space by the affine transform object_to_world. a pixel is shaded only when
 * the eye ray that passes through its half-pixel center actually hits that
 * sphere; the bounding rectangle on screen is merely used to skip pixels, it
 * is never treated as a surface. this means the primitive needs no triangle
 * mesh and no fixed tessellation, and is exact for non-uniform scale, shear,
 * reflection and a camera placed inside the ellipsoid.
 */

typedef struct {
    /* local_position: point on the unit sphere, in object space */
    vec3_t local_position;
    /* world_position: the same point mapped through object_to_world */
    vec3_t world_position;
    /* world_normal: outward unit normal in world space (inverse transpose) */
    vec3_t world_normal;
    /* depth: the hit reprojected to the [0, 1] framebuffer depth range */
    float depth;
    /*
     * backface: 1 when the outward normal points the same way as the eye ray
     * travelling from the near plane towards the far plane (that is, the
     * camera is inside the ellipsoid and this is the exit point), 0 otherwise
     */
    int backface;
} ellipsoid_hit_t;

typedef vec4_t ellipsoid_shader_t(const ellipsoid_hit_t *hit, void *uniforms,
                                  int *discard);

typedef struct {
    /* local unit sphere -> world space; finite, invertible, affine */
    mat4_t object_to_world;
    /* world space -> clip space; finite, homogeneous (perspective or ortho) */
    mat4_t world_to_clip;
    /* fragment shader; required, must not be NULL */
    ellipsoid_shader_t *shader;
    /* opaque pointer handed to the shader, may be NULL */
    void *uniforms;
    /* same alpha blending as a program_t created with enable_blend */
    int enable_blend;
} ellipsoid_draw_t;

/*
 * draws one ellipsoid into the framebuffer, sharing color and depth buffers
 * with graphics_draw_triangle:
 *
 *   - the fragment center is (x + 0.5, y + 0.5) exactly as for triangles
 *   - a fragment is shaded only if it passed "depth <= depth_buffer[index]"
 *   - the shader may clear *discard to skip the color and depth writes
 *   - colors are saturated and, with enable_blend, RGB is blended while the
 *     destination alpha byte keeps its previous value
 *
 * returns 0 on success, non-zero when the arguments are unusable (NULL
 * framebuffer/draw/shader, non-finite matrix entries, a singular or non-affine
 * object_to_world, or a degenerate / non-homogeneous world_to_clip). all
 * validation happens before any fragment is shaded or any buffer is written.
 * the matrices and configuration are read only, and no pointer to a hit is
 * retained after the call.
 */
int graphics_draw_ellipsoid(framebuffer_t *framebuffer,
                            const ellipsoid_draw_t *draw);

#endif
