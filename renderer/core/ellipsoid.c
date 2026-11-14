#include <math.h>
#include <stddef.h>
#include "ellipsoid.h"
#include "graphics_internal.h"
#include "maths.h"

/*
 * analytic ellipsoid rendering
 *
 * each pixel center is unprojected to a world-space segment running from
 * the near plane to the far plane.  the segment is transformed into local
 * (unit sphere) space, where the intersections with x^2+y^2+z^2=1 are
 * found by solving a quadratic.  all ray computations are done in double
 * precision so that strongly non-uniform transforms do not flip the root
 * selection; only the final hit record is converted back to float.
 */

typedef struct {double x, y, z;} dvec3_t;
typedef struct {double x, y, z, w;} dvec4_t;

static dvec3_t dvec3_new(double x, double y, double z) {
    dvec3_t v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static dvec3_t dvec3_add(dvec3_t a, dvec3_t b) {
    return dvec3_new(a.x + b.x, a.y + b.y, a.z + b.z);
}

static dvec3_t dvec3_sub(dvec3_t a, dvec3_t b) {
    return dvec3_new(a.x - b.x, a.y - b.y, a.z - b.z);
}

static dvec3_t dvec3_mul(dvec3_t v, double factor) {
    return dvec3_new(v.x * factor, v.y * factor, v.z * factor);
}

static double dvec3_dot(dvec3_t a, dvec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double dvec3_length(dvec3_t v) {
    return sqrt(dvec3_dot(v, v));
}

static dvec3_t dvec3_normalize(dvec3_t v) {
    return dvec3_mul(v, 1 / dvec3_length(v));
}

static dvec4_t dvec4_new(double x, double y, double z, double w) {
    dvec4_t v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    return v;
}

static dvec4_t dmat4_mul_dvec4(mat4_t m, dvec4_t v) {
    return dvec4_new(
        m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3] * v.w,
        m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3] * v.w,
        m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3] * v.w,
        m.m[3][0] * v.x + m.m[3][1] * v.y + m.m[3][2] * v.z + m.m[3][3] * v.w);
}

static dvec3_t dmat4_mul_point(mat4_t m, dvec3_t p) {
    dvec4_t h = dmat4_mul_dvec4(m, dvec4_new(p.x, p.y, p.z, 1));
    return dvec3_new(h.x / h.w, h.y / h.w, h.z / h.w);
}

static dvec3_t dmat3_mul_dvec3(mat3_t m, dvec3_t v) {
    return dvec3_new(
        m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
        m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
        m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z);
}

/* input validation */

static int mat4_is_finite(mat4_t m) {
    int r, c;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            if (!isfinite((double)m.m[r][c])) {
                return 0;
            }
        }
    }
    return 1;
}

static int mat4_is_affine(mat4_t m) {
    return fabsf(m.m[3][0]) <= 1e-6f
        && fabsf(m.m[3][1]) <= 1e-6f
        && fabsf(m.m[3][2]) <= 1e-6f
        && fabsf(m.m[3][3] - 1) <= 1e-6f;
}

/*
 * conservative screen-space bounding box
 *
 * the ellipsoid is contained in the world-space box centered at the
 * translation column of object_to_world with half extents equal to the
 * row lengths of its linear part.  when every box corner is in front of
 * the camera (positive clip w), the projected ellipsoid lies inside the
 * projected corner hull; otherwise the whole screen is scanned.
 */
static void find_ellipsoid_bbox(const ellipsoid_draw_t *draw,
                                int width, int height,
                                int *min_x, int *min_y, int *max_x, int *max_y) {
    double center_x = draw->object_to_world.m[0][3];
    double center_y = draw->object_to_world.m[1][3];
    double center_z = draw->object_to_world.m[2][3];
    double extent[3];
    double lo_x = 0, lo_y = 0, hi_x = 0, hi_y = 0;
    int behind = 0;
    int i;

    for (i = 0; i < 3; i++) {
        double e0 = draw->object_to_world.m[i][0];
        double e1 = draw->object_to_world.m[i][1];
        double e2 = draw->object_to_world.m[i][2];
        extent[i] = sqrt(e0 * e0 + e1 * e1 + e2 * e2);
    }

    for (i = 0; i < 8; i++) {
        dvec3_t corner = dvec3_new(
            center_x + ((i & 1) ? extent[0] : -extent[0]),
            center_y + ((i & 2) ? extent[1] : -extent[1]),
            center_z + ((i & 4) ? extent[2] : -extent[2]));
        dvec4_t clip = dmat4_mul_dvec4(draw->world_to_clip,
                                       dvec4_new(corner.x, corner.y, corner.z, 1));
        double window_x, window_y;
        if (!(clip.w > 0)) {
            behind = 1;
            break;
        }
        window_x = (clip.x / clip.w + 1) * 0.5 * width;
        window_y = (clip.y / clip.w + 1) * 0.5 * height;
        if (i == 0) {
            lo_x = hi_x = window_x;
            lo_y = hi_y = window_y;
        } else {
            lo_x = lo_x < window_x ? lo_x : window_x;
            lo_y = lo_y < window_y ? lo_y : window_y;
            hi_x = hi_x > window_x ? hi_x : window_x;
            hi_y = hi_y > window_y ? hi_y : window_y;
        }
    }

    if (behind) {
        *min_x = 0;
        *min_y = 0;
        *max_x = width - 1;
        *max_y = height - 1;
    } else {
        *min_x = (int)floor(lo_x);
        *min_y = (int)floor(lo_y);
        *max_x = (int)ceil(hi_x);
        *max_y = (int)ceil(hi_y);
        if (*min_x < 0) {
            *min_x = 0;
        }
        if (*min_y < 0) {
            *min_y = 0;
        }
        if (*max_x > width - 1) {
            *max_x = width - 1;
        }
        if (*max_y > height - 1) {
            *max_y = height - 1;
        }
    }
}

int graphics_draw_ellipsoid(framebuffer_t *framebuffer,
                            const ellipsoid_draw_t *draw) {
    mat4_t world_to_object;
    mat4_t clip_to_world;
    mat3_t normal_matrix;
    int width, height;
    int min_x, min_y, max_x, max_y;
    int x, y;

    /* reject invalid input before touching the buffers or the shader */
    if (framebuffer == NULL || draw == NULL || draw->shader == NULL) {
        return 1;
    }
    if (!mat4_is_finite(draw->object_to_world)
            || !mat4_is_finite(draw->world_to_clip)) {
        return 1;
    }
    if (!mat4_is_affine(draw->object_to_world)) {
        return 1;
    }
    world_to_object = mat4_inverse(draw->object_to_world);
    clip_to_world = mat4_inverse(draw->world_to_clip);
    if (!mat4_is_finite(world_to_object) || !mat4_is_finite(clip_to_world)) {
        return 1;  /* singular transform or non-invertible projection */
    }
    normal_matrix = mat3_inverse_transpose(mat3_from_mat4(draw->object_to_world));

    width = framebuffer->width;
    height = framebuffer->height;
    find_ellipsoid_bbox(draw, width, height, &min_x, &min_y, &max_x, &max_y);

    for (y = min_y; y <= max_y; y++) {
        for (x = min_x; x <= max_x; x++) {
            double ndc_x = ((x + 0.5) / width) * 2 - 1;
            double ndc_y = ((y + 0.5) / height) * 2 - 1;
            dvec3_t world_near = dmat4_mul_point(clip_to_world,
                                                 dvec3_new(ndc_x, ndc_y, -1));
            dvec3_t world_far = dmat4_mul_point(clip_to_world,
                                                dvec3_new(ndc_x, ndc_y, 1));
            dvec3_t local_near = dmat4_mul_point(world_to_object, world_near);
            dvec3_t local_far = dmat4_mul_point(world_to_object, world_far);
            dvec3_t origin = local_near;
            dvec3_t direction = dvec3_sub(local_far, local_near);
            double a = dvec3_dot(direction, direction);
            double b = 2 * dvec3_dot(origin, direction);
            double c = dvec3_dot(origin, origin) - 1;
            double discriminant, root, q, t0, t1, t, swap;
            dvec3_t local_pos, world_pos, world_normal, view_dir;
            dvec4_t clip;
            ellipsoid_hit_t hit;
            vec4_t color;
            float depth;
            int index, backface, discard;

            /* solve |origin + t * direction|^2 = 1 on the segment [0, 1] */
            if (!(a > 0)) {
                continue;
            }
            discriminant = b * b - 4 * a * c;
            if (!(discriminant >= 0)) {
                continue;
            }
            root = sqrt(discriminant);
            q = -0.5 * (b + (b >= 0 ? root : -root));
            if (q != 0) {
                t0 = q / a;
                t1 = c / q;
            } else {
                t0 = t1 = 0;  /* tangent at the near plane */
            }
            if (t0 > t1) {
                swap = t0;
                t0 = t1;
                t1 = swap;
            }
            if (t0 >= 0 && t0 <= 1) {
                t = t0;  /* nearest intersection on the segment */
            } else if (t1 >= 0 && t1 <= 1) {
                t = t1;  /* near plane inside the ellipsoid: take the exit */
            } else {
                continue;
            }

            /* surface point, outward normal, and true depth */
            local_pos = dvec3_add(origin, dvec3_mul(direction, t));
            world_pos = dmat4_mul_point(draw->object_to_world, local_pos);
            world_normal = dvec3_normalize(dmat3_mul_dvec3(normal_matrix,
                                                           local_pos));
            view_dir = dvec3_sub(world_far, world_near);
            backface = dvec3_dot(world_normal, view_dir) > 0 ? 1 : 0;
            clip = dmat4_mul_dvec4(draw->world_to_clip,
                                   dvec4_new(world_pos.x, world_pos.y,
                                             world_pos.z, 1));
            depth = (float)((clip.z / clip.w + 1) * 0.5);

            /* shared visibility and color commit rules */
            index = y * width + x;
            if (graphics_test_depth(framebuffer, index, depth)) {
                hit.local_position = vec3_new((float)local_pos.x,
                                              (float)local_pos.y,
                                              (float)local_pos.z);
                hit.world_position = vec3_new((float)world_pos.x,
                                              (float)world_pos.y,
                                              (float)world_pos.z);
                hit.world_normal = vec3_new((float)world_normal.x,
                                            (float)world_normal.y,
                                            (float)world_normal.z);
                hit.depth = depth;
                hit.backface = backface;
                discard = 0;
                color = draw->shader(&hit, draw->uniforms, &discard);
                if (!discard) {
                    graphics_commit_fragment(framebuffer, index, depth, color,
                                             draw->enable_blend);
                }
            }
        }
    }

    return 0;
}
