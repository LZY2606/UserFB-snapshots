#ifndef ELLIPSOID_H
#define ELLIPSOID_H

#include "graphics.h"
#include "maths.h"

/*
 * analytic ellipsoid primitive for offline scientific visualization
 *
 * instead of tessellating every particle into a triangle mesh, each
 * ellipsoid is ray-cast analytically per pixel and shares the framebuffer
 * (color/depth), the <= depth comparison, color saturation, and RGB alpha
 * blending rules with the triangle pipeline.
 *
 * coordinate conventions
 * ----------------------
 * - local geometry is the unit sphere x^2 + y^2 + z^2 = 1 centered at the
 *   local origin.
 * - object_to_world maps local coordinates to world coordinates.  it must
 *   be a finite, invertible, affine matrix (last row 0 0 0 1).  non-uniform
 *   scaling, shearing, and reflection are all allowed.
 * - world_to_clip maps world coordinates to clip coordinates.  it is the
 *   product of a finite perspective or orthographic projection and the
 *   view transform, using the renderer's usual NDC convention: x, y, z in
 *   [-1, 1] after perspective division, with window-space depth [0, 1]
 *   (0 is the near plane, 1 is the far plane).
 * - pixels are sampled at half-pixel centers, exactly like triangles.
 *   each pixel center is unprojected to a world-space segment running
 *   from the near plane to the far plane; only intersections on that
 *   segment are considered, and the nearest one wins.  when the near
 *   plane already cuts into the ellipsoid, the exit intersection is used
 *   instead (no near-plane cap is synthesized).  a tangent ray counts as
 *   a hit, and parts of the ellipsoid outside the view frustum are not
 *   drawn.
 *
 * per-fragment data
 * -----------------
 * - local_position: the hit point in local space, on the unit sphere.
 * - world_position: the hit point in world space, on the true ellipsoid
 *   surface (not on its bounding box or any proxy geometry).
 * - world_normal: the outward unit normal in world space, computed with
 *   the inverse-transpose of the linear part of object_to_world.
 * - depth: the window-space depth of the true hit point, reprojected to
 *   [0, 1].
 * - backface: non-zero when the outward normal points in the same
 *   direction as the near-to-far view ray (e.g. the exit surface seen
 *   from inside the ellipsoid).
 *
 * the shader callback is only invoked for fragments that lie on the true
 * surface and pass the shared depth comparison; pixels covered by the
 * bounding region but missing the surface never trigger it.  setting
 * *discard suppresses both the color and the depth write.  the hit struct
 * is only valid for the duration of the callback.
 *
 * usage example
 * -------------
 *     framebuffer_t *fb = framebuffer_create(320, 240);
 *     ellipsoid_draw_t draw;
 *     draw.object_to_world = mat4_mul_mat4(mat4_translate(0, 0, -3),
 *                                          mat4_scale(1.5f, 0.5f, 1.0f));
 *     draw.world_to_clip = mat4_ortho(-2, 2, -2, 2, 1, 5);
 *     draw.shader = my_ellipsoid_shader;
 *     draw.uniforms = &my_uniforms;   // may be NULL
 *     draw.enable_blend = 0;
 *     if (graphics_draw_ellipsoid(fb, &draw) != 0) {
 *         // invalid configuration: nothing was drawn
 *     }
 *
 * graphics_draw_ellipsoid returns 0 on success.  it returns non-zero -
 * before invoking the shader or modifying any buffer - when the shader is
 * NULL, when either matrix is non-finite, when object_to_world is singular
 * or non-affine, or when world_to_clip is not invertible.  the draw struct
 * and the matrices it contains are never modified.
 */

typedef struct {
    vec3_t local_position;  /* hit point on the local unit sphere */
    vec3_t world_position;  /* hit point on the world-space ellipsoid */
    vec3_t world_normal;    /* outward unit normal in world space */
    float depth;            /* window-space depth of the hit, [0, 1] */
    int backface;           /* non-zero if the normal faces away from viewer */
} ellipsoid_hit_t;

typedef vec4_t ellipsoid_shader_t(const ellipsoid_hit_t *hit, void *uniforms,
                                  int *discard);

typedef struct {
    mat4_t object_to_world;       /* finite, invertible, affine */
    mat4_t world_to_clip;         /* finite projection * view */
    ellipsoid_shader_t *shader;   /* fragment callback, must not be NULL */
    void *uniforms;               /* passed to the shader, may be NULL */
    int enable_blend;             /* non-zero enables RGB alpha blending */
} ellipsoid_draw_t;

int graphics_draw_ellipsoid(framebuffer_t *framebuffer,
                            const ellipsoid_draw_t *draw);

#endif
