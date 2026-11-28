#include <float.h>
#include <math.h>
#include <stdlib.h>
#include "graphics.h"
#include "macro.h"
#include "maths.h"
#include "point_shadow.h"

/* C89 safe finiteness test */
static int is_finite_float(float value) {
    return value == value && value <= FLT_MAX && value >= -FLT_MAX;
}

/*
 * face order matches the renderer's cubemap convention:
 * px, nx, py, ny, pz, nz
 *
 * a face's view space is defined by right/up/forward, where forward is the
 * positive axis of the face. the direction reconstructed for a texel is
 * s * right + t * up + forward with s = 2u - 1 and t = 2v - 1.
 */
static void get_face_axes(int face, vec3_t *right, vec3_t *up,
                          vec3_t *forward) {
    switch (face) {
        case 0:
            *right = vec3_new(0, 0, -1);
            *up = vec3_new(0, -1, 0);
            *forward = vec3_new(1, 0, 0);
            break;
        case 1:
            *right = vec3_new(0, 0, 1);
            *up = vec3_new(0, -1, 0);
            *forward = vec3_new(-1, 0, 0);
            break;
        case 2:
            *right = vec3_new(1, 0, 0);
            *up = vec3_new(0, 0, 1);
            *forward = vec3_new(0, 1, 0);
            break;
        case 3:
            *right = vec3_new(1, 0, 0);
            *up = vec3_new(0, 0, -1);
            *forward = vec3_new(0, -1, 0);
            break;
        case 4:
            *right = vec3_new(1, 0, 0);
            *up = vec3_new(0, -1, 0);
            *forward = vec3_new(0, 0, 1);
            break;
        default:
            *right = vec3_new(-1, 0, 0);
            *up = vec3_new(0, -1, 0);
            *forward = vec3_new(0, 0, -1);
            break;
    }
}

/*
 * builds a view matrix whose rotation rows are right, up and -forward.
 * the translation part is -dot(axis, eye) for each row, which places the
 * light position at the origin of the face view space.
 */
static mat4_t get_face_view_matrix(vec3_t eye, int face) {
    vec3_t right, up, forward, back;
    mat4_t m = mat4_identity();

    get_face_axes(face, &right, &up, &forward);
    back = vec3_negate(forward);

    m.m[0][0] = right.x;
    m.m[0][1] = right.y;
    m.m[0][2] = right.z;
    m.m[0][3] = -vec3_dot(right, eye);

    m.m[1][0] = up.x;
    m.m[1][1] = up.y;
    m.m[1][2] = up.z;
    m.m[1][3] = -vec3_dot(up, eye);

    m.m[2][0] = back.x;
    m.m[2][1] = back.y;
    m.m[2][2] = back.z;
    m.m[2][3] = -vec3_dot(back, eye);

    return m;
}

point_shadow_t *point_shadow_create(vec3_t position, float near, float far,
                                    int size, float bias) {
    point_shadow_t *point_shadow;
    mat4_t proj_matrix;
    int i;

    if (!(size > 0)) {
        return NULL;
    }
    if (!(near > 0) || !(far > near) || !(bias >= 0)) {
        return NULL;
    }
    if (!is_finite_float(position.x) || !is_finite_float(position.y) ||
        !is_finite_float(position.z) || !is_finite_float(near) ||
        !is_finite_float(far) || !is_finite_float(bias)) {
        return NULL;
    }
    /*
     * the six faces must fit in memory; reject sizes that would overflow
     * the pixel index computations.
     */
    if (size > 16384) {
        return NULL;
    }

    point_shadow = (point_shadow_t*)malloc(sizeof(point_shadow_t));
    point_shadow->position = position;
    point_shadow->near = near;
    point_shadow->far = far;
    point_shadow->bias = bias;
    point_shadow->size = size;

    proj_matrix = mat4_perspective(PI / 2, 1, near, far);
    point_shadow->proj_matrix = proj_matrix;
    for (i = 0; i < 6; i++) {
        point_shadow->faces[i] = framebuffer_create(size, size);
        point_shadow->view_matrices[i] = get_face_view_matrix(position, i);
        point_shadow->view_proj_matrices[i] =
            mat4_mul_mat4(proj_matrix, point_shadow->view_matrices[i]);
    }

    return point_shadow;
}

void point_shadow_release(point_shadow_t *point_shadow) {
    int i;
    if (point_shadow) {
        for (i = 0; i < 6; i++) {
            if (point_shadow->faces[i]) {
                framebuffer_release(point_shadow->faces[i]);
            }
        }
        free(point_shadow);
    }
}

/*
 * maps a direction to a face and normalized coordinates (u, v), matching
 * the face layout documented in the header. ties between equally sized
 * major axes resolve like the existing cubemap code: x needs to be
 * strictly greatest, otherwise y wins over z.
 */
static int select_face(vec3_t dir, float *u, float *v) {
    float abs_x = (float)fabs(dir.x);
    float abs_y = (float)fabs(dir.y);
    float abs_z = (float)fabs(dir.z);
    float s, t;
    int face;

    if (abs_x > abs_y && abs_x > abs_z) {
        if (dir.x > 0) {
            face = 0;
            s = -dir.z / abs_x;
            t = -dir.y / abs_x;
        } else {
            face = 1;
            s = dir.z / abs_x;
            t = -dir.y / abs_x;
        }
    } else if (abs_y > abs_z) {
        if (dir.y > 0) {
            face = 2;
            s = dir.x / abs_y;
            t = dir.z / abs_y;
        } else {
            face = 3;
            s = dir.x / abs_y;
            t = -dir.z / abs_y;
        }
    } else {
        if (dir.z > 0) {
            face = 4;
            s = dir.x / abs_z;
            t = -dir.y / abs_z;
        } else {
            face = 5;
            s = -dir.x / abs_z;
            t = -dir.y / abs_z;
        }
    }

    *u = (s + 1) * 0.5f;
    *v = (t + 1) * 0.5f;
    return face;
}

/*
 * reconstructs the unnormalized direction of a face texel center. this is
 * the inverse of select_face() and is used to re-project taps that fall
 * outside the selected face onto a neighboring face.
 */
static vec3_t get_face_direction(int face, float u, float v) {
    float s = 2 * u - 1;
    float t = 2 * v - 1;
    switch (face) {
        case 0:
            return vec3_new(1, -t, -s);
        case 1:
            return vec3_new(-1, -t, s);
        case 2:
            return vec3_new(s, 1, t);
        case 3:
            return vec3_new(s, -1, -t);
        case 4:
            return vec3_new(s, -t, 1);
        default:
            return vec3_new(-s, -t, -1);
    }
}

/*
 * returns 1 if the tap is occluded. if the tap leaves the selected face,
 * its inverse-mapped direction selects a neighbor face first; only the
 * final face's texel index is floored and clamped.
 */
static int is_tap_occluded(const point_shadow_t *point_shadow, int face,
                           float u, float v, float receiver_distance) {
    framebuffer_t *framebuffer;
    float cu = u;
    float cv = v;
    float depth, occluder_distance;
    int cx, cy;

    if (cu < 0 || cu > 1 || cv < 0 || cv > 1) {
        vec3_t direction = get_face_direction(face, cu, cv);
        face = select_face(direction, &cu, &cv);
    }

    cx = (int)floor(cu * (float)point_shadow->size);
    cy = (int)floor(cv * (float)point_shadow->size);
    if (cx < 0) {
        cx = 0;
    }
    if (cx > point_shadow->size - 1) {
        cx = point_shadow->size - 1;
    }
    if (cy < 0) {
        cy = 0;
    }
    if (cy > point_shadow->size - 1) {
        cy = point_shadow->size - 1;
    }

    framebuffer = point_shadow->faces[face];
    depth = framebuffer->depth_buffer[cy * point_shadow->size + cx];
    if (!(depth < 1)) {
        return 0;
    }

    /*
     * recover the axis distance from the stored perspective depth, then
     * scale by the length of the actual texel center direction that was
     * read to obtain the radial occluder distance.
     */
    {
        float tu = ((float)cx + 0.5f) / (float)point_shadow->size;
        float tv = ((float)cy + 0.5f) / (float)point_shadow->size;
        vec3_t direction = get_face_direction(face, tu, tv);
        float axis_distance =
            point_shadow->near * point_shadow->far /
            (point_shadow->far -
             depth * (point_shadow->far - point_shadow->near));
        direction = vec3_mul(direction, axis_distance);
        occluder_distance = vec3_length(direction);
    }

    return receiver_distance > occluder_distance + point_shadow->bias;
}

float point_shadow_visibility(const point_shadow_t *point_shadow,
                              vec3_t world_position) {
    vec3_t to_light;
    float abs_x, abs_y, abs_z, axis_distance, u, v, receiver_distance;
    float offsets[3];
    int face, i, j, visible;

    if (point_shadow == NULL) {
        return 1;
    }

    to_light = vec3_sub(world_position, point_shadow->position);
    receiver_distance = vec3_length(to_light);
    if (receiver_distance == 0) {
        return 1;
    }

    /* the major axis distance must be along the face's view axis */
    abs_x = (float)fabs(to_light.x);
    abs_y = (float)fabs(to_light.y);
    abs_z = (float)fabs(to_light.z);
    if (abs_x > abs_y && abs_x > abs_z) {
        axis_distance = abs_x;
    } else if (abs_y > abs_z) {
        axis_distance = abs_y;
    } else {
        axis_distance = abs_z;
    }
    if (axis_distance < point_shadow->near ||
        axis_distance > point_shadow->far) {
        return 1;
    }

    face = select_face(to_light, &u, &v);

    offsets[0] = -1;
    offsets[1] = 0;
    offsets[2] = 1;
    visible = 0;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            float tu = u + offsets[i] / (float)point_shadow->size;
            float tv = v + offsets[j] / (float)point_shadow->size;
            if (!is_tap_occluded(point_shadow, face, tu, tv,
                                 receiver_distance)) {
                visible += 1;
            }
        }
    }
    return (float)visible / 9.0f;
}
