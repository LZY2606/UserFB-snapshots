#include <math.h>
#include <stdlib.h>
#include "macro.h"
#include "point_shadow.h"

/*
 * cubemap face conventions, shared with texture.c:
 *
 * face | direction | right (u) | up (v)
 * -----+-----------+-----------+--------
 *  px  |   +x      |    -z     |   -y
 *  nx  |   -x      |    +z     |   -y
 *  py  |   +y      |    +x     |   +z
 *  ny  |   -y      |    +x     |   -z
 *  pz  |   +z      |    +x     |   -y
 *  nz  |   -z      |    -x     |   -y
 */

static const vec3_t FACE_DIRECTIONS[6] = {
    { 1,  0,  0},
    {-1,  0,  0},
    { 0,  1,  0},
    { 0, -1,  0},
    { 0,  0,  1},
    { 0,  0, -1},
};

static const vec3_t FACE_UPS[6] = {
    { 0, -1,  0},
    { 0, -1,  0},
    { 0,  0,  1},
    { 0,  0, -1},
    { 0, -1,  0},
    { 0, -1,  0},
};

point_shadow_t *point_shadow_create(vec3_t position, float near, float far,
                                    int size, float bias) {
    point_shadow_t *point_shadow;
    int i;

    point_shadow = (point_shadow_t*)malloc(sizeof(point_shadow_t));
    if (point_shadow == NULL) {
        return NULL;
    }
    point_shadow->position = position;
    point_shadow->near = near;
    point_shadow->far = far;
    point_shadow->bias = bias;
    for (i = 0; i < 6; i++) {
        point_shadow->faces[i] = framebuffer_create(size, size);
        if (point_shadow->faces[i] == NULL) {
            int j;
            for (j = 0; j < i; j++) {
                framebuffer_release(point_shadow->faces[j]);
            }
            free(point_shadow);
            return NULL;
        }
        framebuffer_clear_depth(point_shadow->faces[i], 1);
    }
    return point_shadow;
}

void point_shadow_release(point_shadow_t *point_shadow) {
    int i;
    if (point_shadow != NULL) {
        for (i = 0; i < 6; i++) {
            framebuffer_release(point_shadow->faces[i]);
        }
        free(point_shadow);
    }
}

mat4_t point_shadow_view_matrix(const point_shadow_t *point_shadow,
                                int face_index) {
    vec3_t eye = point_shadow->position;
    vec3_t target = vec3_add(eye, FACE_DIRECTIONS[face_index]);
    vec3_t up = FACE_UPS[face_index];
    return mat4_lookat(eye, target, up);
}

mat4_t point_shadow_proj_matrix(const point_shadow_t *point_shadow) {
    return mat4_perspective(PI / 2, 1, point_shadow->near, point_shadow->far);
}

/*
 * same axis priority and tie breaking as select_cubemap_face in texture.c:
 * x wins over y and z, y wins over z when the components tie
 */
static int select_face(vec3_t direction, vec2_t *texcoord) {
    float abs_x = (float)fabs(direction.x);
    float abs_y = (float)fabs(direction.y);
    float abs_z = (float)fabs(direction.z);
    float ma, sc, tc;
    int face_index;

    if (abs_x > abs_y && abs_x > abs_z) {
        ma = abs_x;
        if (direction.x > 0) {
            face_index = 0;
            sc = -direction.z;
            tc = -direction.y;
        } else {
            face_index = 1;
            sc = +direction.z;
            tc = -direction.y;
        }
    } else if (abs_y > abs_z) {
        ma = abs_y;
        if (direction.y > 0) {
            face_index = 2;
            sc = +direction.x;
            tc = +direction.z;
        } else {
            face_index = 3;
            sc = +direction.x;
            tc = -direction.z;
        }
    } else {
        ma = abs_z;
        if (direction.z > 0) {
            face_index = 4;
            sc = +direction.x;
            tc = -direction.y;
        } else {
            face_index = 5;
            sc = -direction.x;
            tc = -direction.y;
        }
    }

    texcoord->x = (sc / ma + 1) / 2;
    texcoord->y = (tc / ma + 1) / 2;
    return face_index;
}

/*
 * inverse of the face projection above. given s = 2u - 1, t = 2v - 1, the
 * inverse directions of the six faces are (in order):
 *     (1, -t, -s), (-1, -t, s), (s, 1, t), (s, -1, -t),
 *     (s, -t, 1), (-s, -t, -1)
 */
static vec3_t face_direction(int face_index, float s, float t) {
    switch (face_index) {
        case 0:
            return vec3_new( 1, -t, -s);
        case 1:
            return vec3_new(-1, -t,  s);
        case 2:
            return vec3_new( s,  1,  t);
        case 3:
            return vec3_new( s, -1, -t);
        case 4:
            return vec3_new( s, -t,  1);
        default:
            return vec3_new(-s, -t, -1);
    }
}

static int is_in_range(float u, float v) {
    return u >= 0 && u < 1 && v >= 0 && v < 1;
}

static int clamp_index(int index, int size) {
    if (index < 0) {
        return 0;
    }
    if (index > size - 1) {
        return size - 1;
    }
    return index;
}

/*
 * one depth tap. out-of-range taps are turned back into a direction on the
 * original face and re-projected onto the neighboring face; indices are
 * clamped only once the final face is known.
 */
static int tap_is_visible(const point_shadow_t *point_shadow,
                          int origin_face, float u, float v,
                          float receiver_distance) {
    int face_index = origin_face;
    int size = point_shadow->faces[0]->width;
    int column, row, index;
    float depth, axis_distance, radial_distance;
    vec3_t direction;

    if (!is_in_range(u, v)) {
        float s = 2 * u - 1;
        float t = 2 * v - 1;
        vec2_t remapped;
        direction = face_direction(origin_face, s, t);
        face_index = select_face(direction, &remapped);
        u = remapped.x;
        v = remapped.y;
    }

    column = clamp_index((int)floor(u * size), size);
    row = clamp_index((int)floor(v * size), size);
    index = row * size + column;
    depth = point_shadow->faces[face_index]->depth_buffer[index];

    /* the clear value means nothing was drawn there */
    if (!(depth < 1)) {
        return 1;
    }

    /* [0, 1] perspective depth -> positive distance along the viewing axis */
    axis_distance = point_shadow->near * point_shadow->far /
                    (point_shadow->far -
                     depth * (point_shadow->far - point_shadow->near));

    /* the texel center looks along this direction, so scale to radial space */
    {
        float center_u = (column + 0.5f) / size;
        float center_v = (row + 0.5f) / size;
        float center_s = 2 * center_u - 1;
        float center_t = 2 * center_v - 1;
        direction = face_direction(face_index, center_s, center_t);
        radial_distance = axis_distance * vec3_length(direction);
    }

    return !(receiver_distance > radial_distance + point_shadow->bias);
}

float point_shadow_visibility(const point_shadow_t *point_shadow,
                              vec3_t world_position) {
    vec3_t delta = vec3_sub(world_position, point_shadow->position);
    float receiver_distance = vec3_length(delta);
    vec2_t texcoord;
    int origin_face;
    float major_axis, u, v, texel_size;
    int visible_taps = 0;
    int ox, oy;

    /* sitting on the light itself */
    if (receiver_distance == 0) {
        return 1;
    }

    origin_face = select_face(delta, &texcoord);
    major_axis = (float)fabs(origin_face <= 1 ? delta.x :
                             (origin_face <= 3 ? delta.y : delta.z));
    if (major_axis < point_shadow->near ||
        major_axis > point_shadow->far) {
        return 1;
    }

    u = texcoord.x;
    v = texcoord.y;
    texel_size = 1.0f / point_shadow->faces[0]->width;
    for (oy = -1; oy <= 1; oy++) {
        for (ox = -1; ox <= 1; ox++) {
            float tap_u = u + ox * texel_size;
            float tap_v = v + oy * texel_size;
            visible_taps += tap_is_visible(point_shadow, origin_face,
                                           tap_u, tap_v, receiver_distance);
        }
    }
    return visible_taps / 9.0f;
}
