#include <float.h>
#include <math.h>
#include <stddef.h>
#include "ellipsoid.h"
#include "graphics.h"
#include "macro.h"
#include "maths.h"

/*
 * analytic ray/ellipsoid intersection
 *
 * the ellipsoid is the image of the unit sphere under object_to_world:
 *
 *     O(p) = object_to_world * (p, 1),   |p| = 1
 *
 * a pixel (x, y) is tested with the same half-pixel center triangles use,
 * (x + 0.5, y + 0.5). that screen point is lifted to the near (z_ndc = -1) and
 * far (z_ndc = +1) planes of the existing [-1, 1] NDC depth convention and
 * mapped through the inverse of world_to_clip, which yields a world space
 * segment [w_near, w_far] after the perspective division.
 *
 * the segment is then pulled back into object space with the inverse of the
 * affine object_to_world. affine maps preserve lines, so intersecting the
 * segment with the unit sphere is exactly intersecting the ray with the
 * transformed ellipsoid, and the accepted root identifies a real surface
 * point:
 *
 *   - both roots on the segment: take the smaller t, the point nearer the
 *     camera along the near->far direction (front surface);
 *   - the near end already inside the sphere: the camera is inside or the
 *     near plane cuts into the shape, so the visible surface is the exit
 *     point, the larger root. no flat cap is added at the near plane;
 *   - a double root counts as a hit (grazing tangency);
 *   - a ray that misses the sphere shades nothing, even when the screen space
 *     bounding rectangle covers the pixel. the rectangle only limits the
 *     pixel loop and is never used as a surface or as a depth.
 *
 * hits are shaded only after the shared "depth <= depth_buffer[index]" test,
 * and the commit goes through graphics_commit_fragment(), the very same path
 * triangles use, so ellipsoid and triangle blending cannot diverge.
 */

/* ------------------------------------------------------------------------- */
/* small numeric helpers                                                     */
/* ------------------------------------------------------------------------- */

static int is_finite_float(float f) {
    return f == f && f >= -FLT_MAX && f <= FLT_MAX;
}

static int is_finite_double(double d) {
    return d == d && d >= -DBL_MAX && d <= DBL_MAX;
}

static int is_finite_mat4(mat4_t m) {
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (!is_finite_float(m.m[i][j])) {
                return 0;
            }
        }
    }
    return 1;
}

static int min_integer(int a, int b) {
    return a < b ? a : b;
}

static int max_integer(int a, int b) {
    return a > b ? a : b;
}

typedef struct {
    int min_x, min_y, max_x, max_y;
} ellipsoid_bbox_t;

/*
 * determinant of the upper-left 3x3 block of an affine matrix, evaluated in
 * double so the invertibility test does not depend on the float rounding of
 * the caller's matrix.
 */
static double linear_determinant(mat4_t m) {
    double a00 = m.m[0][0], a01 = m.m[0][1], a02 = m.m[0][2];
    double a10 = m.m[1][0], a11 = m.m[1][1], a12 = m.m[1][2];
    double a20 = m.m[2][0], a21 = m.m[2][1], a22 = m.m[2][2];
    return a00 * (a11 * a22 - a12 * a21) -
           a01 * (a10 * a22 - a12 * a20) +
           a02 * (a10 * a21 - a11 * a20);
}

static double column_norm_squared(mat4_t m, int column) {
    double n = 0;
    int i;
    for (i = 0; i < 3; i++) {
        double v = m.m[i][column];
        n += v * v;
    }
    return n;
}

/*
 * object_to_world must be a finite, invertible, affine transform: the bottom
 * row is (0, 0, 0, 1) and the linear block is far enough from singular that
 * its inverse is numerically meaningful.
 */
static int validate_object_to_world(mat4_t m) {
    const double tol = 1e-6;
    double determinant, scale;

    if (!is_finite_mat4(m)) {
        return 0;
    }
    if (fabs((double)m.m[3][0]) > tol || fabs((double)m.m[3][1]) > tol ||
        fabs((double)m.m[3][2]) > tol ||
        fabs((double)m.m[3][3] - 1.0) > tol) {
        return 0;
    }

    determinant = linear_determinant(m);
    if (!is_finite_double(determinant)) {
        return 0;
    }
    /*
     * |det| is the volume scale of the linear map. compare it with the product
     * of the column lengths, which is the largest |det| an equally sized
     * orthogonal map could have, so the residual is a scale-free measure of
     * how singular the matrix is.
     */
    scale = sqrt(column_norm_squared(m, 0)) *
            sqrt(column_norm_squared(m, 1)) *
            sqrt(column_norm_squared(m, 2));
    if (!(scale > 0) || !is_finite_double(scale)) {
        return 0;
    }
    return fabs(determinant) > 1e-9 * scale;
}

/*
 * world_to_clip is a view/projection product. a view is affine, so its bottom
 * row is (0, 0, 0, 1); multiplying from the left by an orthographic projection
 * (bottom row 0,0,0,1) keeps that row, while a perspective projection
 * (bottom row 0,0,-1,0) turns the view's z row into the clip w row, which is
 * an affine function of world position with a non-zero z slope. a usable
 * matrix is therefore finite, has an invertible linear block (near and far
 * points must be recoverable) and a non-degenerate w row. the exact w row
 * constant is not checked: after a view it carries the eye's z offset.
 */
static int validate_world_to_clip(mat4_t m) {
    double determinant, scale, w_norm;

    if (!is_finite_mat4(m)) {
        return 0;
    }

    determinant = linear_determinant(m);
    if (!is_finite_double(determinant)) {
        return 0;
    }
    scale = sqrt(column_norm_squared(m, 0)) *
            sqrt(column_norm_squared(m, 1)) *
            sqrt(column_norm_squared(m, 2));
    if (!(scale > 0) || !is_finite_double(scale)) {
        return 0;
    }
    if (!(fabs(determinant) > 1e-9 * scale)) {
        return 0;
    }

    w_norm = sqrt((double)m.m[3][0] * m.m[3][0] +
                  (double)m.m[3][1] * m.m[3][1] +
                  (double)m.m[3][2] * m.m[3][2] +
                  (double)m.m[3][3] * m.m[3][3]);
    if (!(w_norm > 1e-9)) {
        return 0;
    }
    return 1;
}

/*
 * inverse of a 4x4 matrix by Gauss-Jordan elimination with partial pivoting,
 * in double precision. returns 0 when the matrix is numerically singular.
 */
static int invert_4x4(const double in[4][4], double out[4][4]) {
    double a[4][4];
    double b[4][4];
    int i, j, r, c;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            a[i][j] = in[i][j];
            b[i][j] = (i == j) ? 1.0 : 0.0;
        }
    }
    for (c = 0; c < 4; c++) {
        int pivot = c;
        double divisor;
        for (r = c + 1; r < 4; r++) {
            if (fabs(a[r][c]) > fabs(a[pivot][c])) {
                pivot = r;
            }
        }
        if (!(fabs(a[pivot][c]) > DBL_MIN)) {
            return 0;
        }
        if (pivot != c) {
            for (j = 0; j < 4; j++) {
                double tmp = a[c][j];
                a[c][j] = a[pivot][j];
                a[pivot][j] = tmp;
                tmp = b[c][j];
                b[c][j] = b[pivot][j];
                b[pivot][j] = tmp;
            }
        }
        divisor = a[c][c];
        for (j = 0; j < 4; j++) {
            a[c][j] /= divisor;
            b[c][j] /= divisor;
        }
        for (r = 0; r < 4; r++) {
            double factor;
            if (r == c) {
                continue;
            }
            factor = a[r][c];
            for (j = 0; j < 4; j++) {
                a[r][j] -= factor * a[c][j];
                b[r][j] -= factor * b[c][j];
            }
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (!is_finite_double(b[i][j])) {
                return 0;
            }
            out[i][j] = b[i][j];
        }
    }
    return 1;
}

/*
 * map a pixel center on the given NDC z plane to a world position. the point
 * is (x_f, y_f, z, 1) in clip space; multiplying by the inverse of
 * world_to_clip gives a homogeneous world point whose perspective division is
 * the world position.
 */
static int unproject_world(const double inverse_clip[4][4],
                           int x, int y, int width, int height,
                           double z_ndc, double world[3]) {
    double x_f = 2.0 * ((double)x + 0.5) / (double)width - 1.0;
    double y_f = 2.0 * ((double)y + 0.5) / (double)height - 1.0;
    double h[4];
    int i;

    for (i = 0; i < 4; i++) {
        h[i] = inverse_clip[i][0] * x_f +
               inverse_clip[i][1] * y_f +
               inverse_clip[i][2] * z_ndc +
               inverse_clip[i][3];
    }
    if (!(fabs(h[3]) > 1e-12)) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        world[i] = h[i] / h[3];
        if (!is_finite_double(world[i])) {
            return 0;
        }
    }
    return 1;
}

/*
 * screen space bounding rectangle of the transformed ellipsoid. this is only
 * a work limit; every pixel inside it still runs the exact ray test.
 */
static ellipsoid_bbox_t find_screen_bbox(mat4_t object_to_world,
                                         mat4_t world_to_clip,
                                         int width, int height) {
    double extent[3];
    vec3_t center = vec3_new(object_to_world.m[0][3],
                             object_to_world.m[1][3],
                             object_to_world.m[2][3]);
    double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;
    int i, j, any_behind = 0, any_projected = 0;
    ellipsoid_bbox_t bbox;

    for (i = 0; i < 3; i++) {
        double row_squared = 0.0;
        for (j = 0; j < 3; j++) {
            double v = object_to_world.m[i][j];
            row_squared += v * v;
        }
        /* support function of the unit ball in coordinate i: row 2-norm */
        extent[i] = sqrt(row_squared);
    }
    /* the world AABB of the ellipsoid contains the unit-sphere image */
    for (i = 0; i < 8; i++) {
        vec3_t corner = vec3_new(
            (float)((double)center.x + ((i & 1) ? extent[0] : -extent[0])),
            (float)((double)center.y + ((i & 2) ? extent[1] : -extent[1])),
            (float)((double)center.z + ((i & 4) ? extent[2] : -extent[2])));
        vec4_t clip = mat4_mul_vec4(world_to_clip, vec4_from_vec3(corner, 1));
        double ndc_x, ndc_y;
        if (!is_finite_float(clip.w) || !(clip.w > EPSILON)) {
            any_behind = 1;
            continue;
        }
        ndc_x = (double)clip.x / clip.w;
        ndc_y = (double)clip.y / clip.w;
        if (!is_finite_double(ndc_x) || !is_finite_double(ndc_y)) {
            any_behind = 1;
            continue;
        }
        if (!any_projected) {
            min_x = max_x = ndc_x;
            min_y = max_y = ndc_y;
            any_projected = 1;
        } else {
            if (ndc_x < min_x) min_x = ndc_x;
            if (ndc_x > max_x) max_x = ndc_x;
            if (ndc_y < min_y) min_y = ndc_y;
            if (ndc_y > max_y) max_y = ndc_y;
        }
    }
    if (!any_projected || any_behind) {
        /*
         * a corner lies at or behind the eye: the AABB straddles the camera
         * plane, so the safe superset is the whole framebuffer. (also covers
         * the camera being inside the ellipsoid)
         */
        bbox.min_x = 0;
        bbox.min_y = 0;
        bbox.max_x = width - 1;
        bbox.max_y = height - 1;
        return bbox;
    }
    /* same viewport mapping and floor/ceil rules as triangles */
    bbox.min_x = max_integer((int)floor((min_x + 1.0) * 0.5 * width), 0);
    bbox.min_y = max_integer((int)floor((min_y + 1.0) * 0.5 * height), 0);
    bbox.max_x = min_integer((int)ceil((max_x + 1.0) * 0.5 * width), width - 1);
    bbox.max_y = min_integer((int)ceil((max_y + 1.0) * 0.5 * height),
                             height - 1);
    return bbox;
}

/* ------------------------------------------------------------------------- */
/* public entry point                                                        */
/* ------------------------------------------------------------------------- */

int graphics_draw_ellipsoid(framebuffer_t *framebuffer,
                            const ellipsoid_draw_t *draw) {
    mat4_t object_to_world;
    mat4_t world_to_clip;
    double inverse_linear[3][3];
    double inverse_transpose_linear[3][3];
    double offset[3];
    double inverse_clip[4][4];
    double clip_matrix[4][4];
    ellipsoid_bbox_t bbox;
    int width, height, x, y, i, j;

    /* ---- validation: nothing below may shade or touch the buffers -------- */
    if (framebuffer == NULL || draw == NULL || draw->shader == NULL) {
        return 1;
    }
    if (framebuffer->width <= 0 || framebuffer->height <= 0 ||
        framebuffer->color_buffer == NULL ||
        framebuffer->depth_buffer == NULL) {
        return 1;
    }
    if (!validate_object_to_world(draw->object_to_world)) {
        return 1;
    }
    if (!validate_world_to_clip(draw->world_to_clip)) {
        return 1;
    }

    object_to_world = draw->object_to_world;
    world_to_clip = draw->world_to_clip;
    width = framebuffer->width;
    height = framebuffer->height;

    /* inverse of the linear part, and the inverse-transpose for normals */
    {
        double det = linear_determinant(object_to_world);
        double inv_det;
        double a00 = object_to_world.m[0][0];
        double a01 = object_to_world.m[0][1];
        double a02 = object_to_world.m[0][2];
        double a10 = object_to_world.m[1][0];
        double a11 = object_to_world.m[1][1];
        double a12 = object_to_world.m[1][2];
        double a20 = object_to_world.m[2][0];
        double a21 = object_to_world.m[2][1];
        double a22 = object_to_world.m[2][2];
        if (!is_finite_double(det) || det == 0.0) {
            return 1;
        }
        inv_det = 1.0 / det;
        inverse_linear[0][0] = (a11 * a22 - a12 * a21) * inv_det;
        inverse_linear[0][1] = (a02 * a21 - a01 * a22) * inv_det;
        inverse_linear[0][2] = (a01 * a12 - a02 * a11) * inv_det;
        inverse_linear[1][0] = (a12 * a20 - a10 * a22) * inv_det;
        inverse_linear[1][1] = (a00 * a22 - a02 * a20) * inv_det;
        inverse_linear[1][2] = (a02 * a10 - a00 * a12) * inv_det;
        inverse_linear[2][0] = (a10 * a21 - a11 * a20) * inv_det;
        inverse_linear[2][1] = (a01 * a20 - a00 * a21) * inv_det;
        inverse_linear[2][2] = (a00 * a11 - a01 * a10) * inv_det;
        /* inverse transpose = transpose of inverse */
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                inverse_transpose_linear[i][j] = inverse_linear[j][i];
                if (!is_finite_double(inverse_linear[i][j]) ||
                    !is_finite_double(inverse_transpose_linear[i][j])) {
                    return 1;
                }
            }
        }
    }

    /* translation of the affine inverse: -A^-1 * t */
    for (i = 0; i < 3; i++) {
        offset[i] = inverse_linear[i][0] * object_to_world.m[0][3] +
                    inverse_linear[i][1] * object_to_world.m[1][3] +
                    inverse_linear[i][2] * object_to_world.m[2][3];
        if (!is_finite_double(offset[i])) {
            return 1;
        }
    }

    /* inverse of world_to_clip, in double */
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            clip_matrix[i][j] = world_to_clip.m[i][j];
        }
    }
    if (!invert_4x4(clip_matrix, inverse_clip)) {
        return 1;
    }

    bbox = find_screen_bbox(object_to_world, world_to_clip, width, height);

    /* ---- fragment loop --------------------------------------------------- */
    for (y = bbox.min_y; y <= bbox.max_y; y++) {
        for (x = bbox.min_x; x <= bbox.max_x; x++) {
            double near_world[3], far_world[3];
            double near_local[3], far_local[3];
            double direction[3];
            double quad_a, quad_b, quad_c;
            double discriminant;
            double t_near, t_far, t;
            int inside_near, backface;
            vec3_t local, world, normal;
            vec4_t clip;
            float depth;
            int index;
            ellipsoid_hit_t hit;
            vec4_t color;
            int discard;

            if (!unproject_world(inverse_clip, x, y, width, height, -1.0,
                                 near_world) ||
                !unproject_world(inverse_clip, x, y, width, height, +1.0,
                                 far_world)) {
                continue;
            }

            /* pull both endpoints into object space with the affine inverse */
            for (i = 0; i < 3; i++) {
                near_local[i] = inverse_linear[i][0] * near_world[0] +
                                inverse_linear[i][1] * near_world[1] +
                                inverse_linear[i][2] * near_world[2] - offset[i];
                far_local[i] = inverse_linear[i][0] * far_world[0] +
                               inverse_linear[i][1] * far_world[1] +
                               inverse_linear[i][2] * far_world[2] - offset[i];
                direction[i] = far_local[i] - near_local[i];
            }

            /*
             * solve |near + t * direction|^2 = 1 for t in [0, 1]:
             *     a t^2 + b t + c = 0
             */
            quad_a = direction[0] * direction[0] +
                     direction[1] * direction[1] +
                     direction[2] * direction[2];
            if (!(quad_a > 0.0)) {
                continue;
            }
            quad_b = 2.0 * (near_local[0] * direction[0] +
                            near_local[1] * direction[1] +
                            near_local[2] * direction[2]);
            quad_c = near_local[0] * near_local[0] +
                     near_local[1] * near_local[1] +
                     near_local[2] * near_local[2] - 1.0;
            discriminant = quad_b * quad_b - 4.0 * quad_a * quad_c;
            if (!(discriminant >= 0.0)) {
                continue;  /* no real surface at this pixel */
            }
            /* snap a barely-negative / cancellation-dominated result to a
             * double root so tangency is not lost to rounding */
            if (discriminant <= 1e-12 * (quad_b * quad_b +
                                         4.0 * quad_a * fabs(quad_c) +
                                         1e-300)) {
                discriminant = 0.0;
            }
            {
                double root = sqrt(discriminant);
                t_near = (-quad_b - root) / (2.0 * quad_a);
                t_far = (-quad_b + root) / (2.0 * quad_a);
            }
            inside_near = quad_c <= 0.0;

            if (inside_near) {
                /*
                 * the near end is already inside the sphere (camera inside,
                 * or the near plane cuts into the shape): the visible surface
                 * is the real exit point, the larger root. if that root is
                 * behind the eye or beyond the far plane there is no surface
                 * point on the segment, and clipping would fabricate a cap,
                 * so nothing is drawn. the near plane itself is never a cap.
                 */
                if (!(t_far >= 0.0 && t_far <= 1.0)) {
                    continue;
                }
                t = t_far;
                backface = 1;
            } else {
                /* front surface: the entry root must be on the segment */
                if (t_near < 0.0) {
                    if (!(t_near >= -1e-5)) {
                        continue;
                    }
                    t_near = 0.0;
                }
                if (t_near > 1.0) {
                    continue;
                }
                t = t_near;
                backface = 0;
            }
            /* a double root may be a hair outside [0, 1] and is still the
             * grazing point; snap it back before forming the position */
            if (t < 0.0) {
                t = (t >= -1e-5) ? 0.0 : t;
            }
            if (t > 1.0) {
                t = (t <= 1.0 + 1e-5) ? 1.0 : t;
            }
            if (t < 0.0 || t > 1.0) {
                continue;
            }

            /* the accepted root is a genuine sphere point */
            local = vec3_new((float)(near_local[0] + t * direction[0]),
                             (float)(near_local[1] + t * direction[1]),
                             (float)(near_local[2] + t * direction[2]));
            {
                double length = sqrt((double)local.x * local.x +
                                     (double)local.y * local.y +
                                     (double)local.z * local.z);
                if (!(length > 0) || !is_finite_double(length)) {
                    continue;
                }
                local = vec3_div(local, (float)length);
            }

            /* world point from the real local point, never from the bbox */
            world = vec3_add(
                vec3_new(object_to_world.m[0][3], object_to_world.m[1][3],
                         object_to_world.m[2][3]),
                mat3_mul_vec3(mat3_from_mat4(object_to_world), local));

            /* depth from the real hit, projected to [0, 1] */
            clip = mat4_mul_vec4(world_to_clip, vec4_from_vec3(world, 1));
            if (!is_finite_float(clip.w) || !(fabs((double)clip.w) > 1e-12)) {
                continue;
            }
            {
                double ndc_x = (double)clip.x / clip.w;
                double ndc_y = (double)clip.y / clip.w;
                double ndc_z = (double)clip.z / clip.w;
                if (!is_finite_double(ndc_x) || !is_finite_double(ndc_y) ||
                    !is_finite_double(ndc_z)) {
                    continue;
                }
                /* a hit projected outside the NDC rectangle is not drawn */
                if (!(fabs(ndc_x) <= 1.0 + 1e-5) ||
                    !(fabs(ndc_y) <= 1.0 + 1e-5)) {
                    continue;
                }
                /* depth from the real intersection point */
                depth = (float)((ndc_z + 1.0) * 0.5);
                if (!is_finite_float(depth)) {
                    continue;
                }
                /* only geometry inside the NDC depth range is drawn */
                if (!(ndc_z >= -1.0 - 1e-5 && ndc_z <= 1.0 + 1e-5)) {
                    continue;
                }
            }


            /* outward world normal through the inverse transpose */
            normal = vec3_normalize(vec3_new(
                (float)(inverse_transpose_linear[0][0] * local.x +
                        inverse_transpose_linear[0][1] * local.y +
                        inverse_transpose_linear[0][2] * local.z),
                (float)(inverse_transpose_linear[1][0] * local.x +
                        inverse_transpose_linear[1][1] * local.y +
                        inverse_transpose_linear[1][2] * local.z),
                (float)(inverse_transpose_linear[2][0] * local.x +
                        inverse_transpose_linear[2][1] * local.y +
                        inverse_transpose_linear[2][2] * local.z)));
            if (!is_finite_float(normal.x) || !is_finite_float(normal.y) ||
                !is_finite_float(normal.z)) {
                continue;
            }

            index = y * width + x;
            /* existing depth comparison, exactly as triangles use it */
            if (!(depth <= framebuffer->depth_buffer[index])) {
                continue;
            }

            /*
             * the callback runs only for a real surface hit that passed the
             * depth test. hit lives on the stack and is not retained.
             */
            hit.local_position = local;
            hit.world_position = world;
            hit.world_normal = normal;
            hit.depth = depth;
            hit.backface = backface;

            discard = 0;
            color = draw->shader(&hit, draw->uniforms, &discard);
            graphics_commit_fragment(framebuffer, index, depth, color,
                                     &discard, draw->enable_blend);
        }
    }

    return 0;
}
