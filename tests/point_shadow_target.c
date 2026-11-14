#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define change_directory(path) _chdir(path)
#else
#include <unistd.h>
#define change_directory(path) chdir(path)
#endif
#include "task_support/check.h"
#include "renderer/core/api.h"
#include "renderer/core/point_shadow.h"
#include "renderer/shaders/blinn_shader.h"
#include "renderer/shaders/cache_helper.h"
#include "renderer/shaders/pbr_shader.h"
#include "renderer/tests/test_helper.h"

/* ========================================================================== */
/* helpers                                                                    */
/* ========================================================================== */

/* volatile arithmetic produces NaN/Inf even under -ffast-math */
static float make_nan(void) {
    volatile float zero = 0;
    return zero / zero;
}

static float make_inf(void) {
    volatile float one = 1;
    volatile float zero = 0;
    return one / zero;
}

static perframe_t make_perframe(void) {
    perframe_t p;
    memset(&p, 0, sizeof(p));
    p.camera_view_matrix = mat4_identity();
    p.camera_proj_matrix = mat4_perspective(1.57079632679f, 1, 1, 9);
    p.light_view_matrix = mat4_identity();
    p.light_proj_matrix = mat4_identity();
    p.light_dir = vec3_new(0, 0, -1);
    p.camera_pos = vec3_new(0, 0, 0);
    p.punctual_intensity = 1;
    p.layer_view = -1;
    return p;
}

static int count_written(const framebuffer_t *framebuffer) {
    int num_pixels = framebuffer->width * framebuffer->height;
    int count = 0;
    int i;
    for (i = 0; i < num_pixels; i++) {
        if (framebuffer->depth_buffer[i] < 1) {
            count += 1;
        }
    }
    return count;
}

static int face_all_clear(const point_shadow_t *ps, int face) {
    return count_written(ps->faces[face]) == 0;
}

static int all_faces_clear(const point_shadow_t *ps) {
    int face;
    for (face = 0; face < 6; face++) {
        if (!face_all_clear(ps, face)) {
            return 0;
        }
    }
    return 1;
}

static int written_off_center(const point_shadow_t *ps, int face) {
    int size = ps->faces[0]->width;
    int center = size / 2;
    int x, y;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            if (x == center && y == center) {
                continue;
            }
            if (ps->faces[face]->depth_buffer[y * size + x] < 1) {
                return 1;
            }
        }
    }
    return 0;
}

static float projected_depth(mat4_t view, mat4_t proj, vec3_t vertex) {
    vec4_t world = vec4_from_vec3(vertex, 1);
    vec4_t clip = mat4_mul_vec4(mat4_mul_mat4(proj, view), world);
    return (clip.z / clip.w + 1) * 0.5f;
}

static int face_depth_constant(const point_shadow_t *ps, int face,
                                float expected) {
    int size = ps->faces[0]->width;
    int x, y;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float depth = ps->faces[face]->depth_buffer[y * size + x];
            if (depth < 1 && fabsf(depth - expected) > 1e-5f) {
                return 0;
            }
        }
    }
    return 1;
}

static mat4_t place_triangle(vec3_t translation, vec3_t axis_rotation) {
    mat4_t t = mat4_translate(translation.x, translation.y, translation.z);
    mat4_t r;
    mat4_t s = mat4_scale(0.4f, 0.4f, 0.4f);
    if (axis_rotation.z != 0) {
        r = mat4_rotate_z(axis_rotation.z);
    } else if (axis_rotation.y != 0) {
        r = mat4_rotate_y(axis_rotation.y);
    } else {
        r = mat4_rotate_x(axis_rotation.x);
    }
    return mat4_mul_mat4(t, mat4_mul_mat4(r, s));
}

static scene_t *blinn_triangle_scene(mat4_t transform,
                                     const blinn_material_t *material,
                                     float ambient, float punctual) {
    model_t *model = blinn_create_model("checks/triangle.obj", transform,
                                        NULL, -1, (blinn_material_t*)material);
    model_t **models = NULL;
    darray_push(models, model);
    return scene_create(vec3_new(0, 0, 0), NULL, models,
                        ambient, punctual, 0, 0);
}

static scene_t *pbr_triangle_scene(int metalness, mat4_t transform,
                                   const char *env_name,
                                   float ambient, float punctual) {
    model_t *model;
    model_t **models = NULL;
    if (metalness) {
        pbrm_material_t m;
        memset(&m, 0, sizeof(m));
        m.basecolor_factor = vec4_new(1, 1, 1, 1);
        m.metalness_factor = 0.5f;
        m.roughness_factor = 0.7f;
        m.double_sided = 1;
        model = pbrm_create_model("checks/triangle.obj", transform,
                                  NULL, -1, &m, env_name);
    } else {
        pbrs_material_t m;
        memset(&m, 0, sizeof(m));
        m.diffuse_factor = vec4_new(1, 1, 1, 1);
        m.specular_factor = vec3_new(0.2f, 0.2f, 0.2f);
        m.glossiness_factor = 0.3f;
        m.double_sided = 1;
        model = pbrs_create_model("checks/triangle.obj", transform,
                                  NULL, -1, &m, env_name);
    }
    darray_push(models, model);
    return scene_create(vec3_new(0, 0, 0), NULL, models,
                        ambient, punctual, 0, 0);
}

static void write_rgba_tga(const char *path, int width, int height,
                           unsigned char rgb, unsigned char alpha_low,
                           unsigned char alpha_high, int split_v) {
    image_t *image = image_create(width, height, 4, FORMAT_LDR);
    int x, y;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int index = (y * width + x) * 4;
            float v = (float)y / (float)(height - 1);
            unsigned char alpha = (!split_v || v <= 0.5f) ? alpha_high
                                                          : alpha_low;
            image->ldr_buffer[index + 0] = rgb;
            image->ldr_buffer[index + 1] = rgb;
            image->ldr_buffer[index + 2] = rgb;
            image->ldr_buffer[index + 3] = alpha;
        }
    }
    image_save(image, path);
    image_release(image);
}

static void make_texture_paths(char *alpha_path, char *emission_path) {
    const char *tmp = getenv("TMPDIR");
    char directory[PATH_SIZE];
    size_t length;
    if (tmp == NULL) {
        tmp = "/tmp/";
    }
    length = strlen(tmp);
    if (length > 0 && tmp[length - 1] != '/') {
        snprintf(directory, sizeof(directory), "%s/", tmp);
    } else {
        snprintf(directory, sizeof(directory), "%s", tmp);
    }
    snprintf(alpha_path, PATH_SIZE, "%sps_alpha.tga", directory);
    snprintf(emission_path, PATH_SIZE, "%sps_emission.tga", directory);
}

/* ========================================================================== */
/* independent scalar reference for the sampling specification                */
/* ========================================================================== */

static void ref_select(vec3_t d, int *face, float *u, float *v) {
    float ax = fabsf(d.x), ay = fabsf(d.y), az = fabsf(d.z);
    float ma, s, t;
    if (ax > ay && ax > az) {
        ma = ax;
        if (d.x > 0) {
            *face = 0;
            s = -d.z;
            t = -d.y;
        } else {
            *face = 1;
            s = d.z;
            t = -d.y;
        }
    } else if (ay > az) {
        ma = ay;
        if (d.y > 0) {
            *face = 2;
            s = d.x;
            t = d.z;
        } else {
            *face = 3;
            s = d.x;
            t = -d.z;
        }
    } else {
        ma = az;
        if (d.z > 0) {
            *face = 4;
            s = d.x;
            t = -d.y;
        } else {
            *face = 5;
            s = -d.x;
            t = -d.y;
        }
    }
    *u = (s / ma + 1) * 0.5f;
    *v = (t / ma + 1) * 0.5f;
}

static vec3_t ref_inverse(int face, float s, float t) {
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

static int ref_clamp(int index, int size) {
    return index < 0 ? 0 : (index > size - 1 ? size - 1 : index);
}

static int ref_tap(const point_shadow_t *ps, int origin_face,
                   float u, float v, float receiver, int *final_face) {
    int face = origin_face;
    int size = ps->faces[0]->width;
    int column, row;
    float depth, axis, radial, center_u, center_v;
    vec3_t direction;

    if (u < 0 || u >= 1 || v < 0 || v >= 1) {
        vec2_t remapped;
        direction = ref_inverse(origin_face, 2 * u - 1, 2 * v - 1);
        ref_select(direction, &face, &remapped.x, &remapped.y);
        u = remapped.x;
        v = remapped.y;
    }
    if (final_face != NULL) {
        *final_face = face;
    }

    column = ref_clamp((int)floorf(u * size), size);
    row = ref_clamp((int)floorf(v * size), size);
    depth = ps->faces[face]->depth_buffer[row * size + column];
    if (!(depth < 1)) {
        return 1;
    }
    axis = ps->near * ps->far /
           (ps->far - depth * (ps->far - ps->near));
    center_u = (column + 0.5f) / size;
    center_v = (row + 0.5f) / size;
    direction = ref_inverse(face, 2 * center_u - 1, 2 * center_v - 1);
    radial = axis * vec3_length(direction);
    return !(receiver > radial + ps->bias);
}

static float ref_visibility(const point_shadow_t *ps, vec3_t world_position) {
    vec3_t delta = vec3_sub(world_position, ps->position);
    float receiver = vec3_length(delta);
    float major;
    vec2_t texcoord;
    int face, visible = 0, ox, oy;

    if (receiver == 0) {
        return 1;
    }
    ref_select(delta, &face, &texcoord.x, &texcoord.y);
    major = fabsf(face <= 1 ? delta.x : (face <= 3 ? delta.y : delta.z));
    if (major < ps->near || major > ps->far) {
        return 1;
    }
    for (oy = -1; oy <= 1; oy++) {
        for (ox = -1; ox <= 1; ox++) {
            float u = texcoord.x + ox / (float)ps->faces[0]->width;
            float v = texcoord.y + oy / (float)ps->faces[0]->width;
            visible += ref_tap(ps, face, u, v, receiver, NULL);
        }
    }
    return visible / 9.0f;
}

static float depth_from_axis(float near, float far, float axis) {
    return far * (1 - near / axis) / (far - near);
}

/* ========================================================================== */
/* sampling group                                                             */
/* ========================================================================== */

static int sampling_matches_reference(void) {
    point_shadow_t *ps = point_shadow_create(vec3_new(0, 0, 0),
                                             1, 10, 8, 0.05f);
    float wall_depth = depth_from_axis(1, 10, 3);
    int column, row, i;

    /* a square wall on the px face at axis distance 3 */
    for (row = 2; row <= 5; row++) {
        for (column = 2; column <= 5; column++) {
            ps->faces[0]->depth_buffer[row * 8 + column] = wall_depth;
        }
    }
    /* one occluder texel on the pz face for seam coverage */
    ps->faces[4]->depth_buffer[0] = wall_depth;

    /* clear value means no occlusion */
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(5, 0, 0)) == 0);
    TASK_CHECK(ref_visibility(ps, vec3_new(5, 0, 0)) == 0);

    /* sitting on the light, or outside the axis range, is fully visible */
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(0, 0, 0)) == 1);
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(12, 0, 0)) == 1);
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(0.5f, 0, 0)) == 1);
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(-12, 7, 7)) == 1);

    /* broad sweep against the independent scalar reference */
    srand(1234);
    for (i = 0; i < 600; i++) {
        float sx = ((float)(rand() % 301) / 100) - 1.5f;
        float sy = ((float)(rand() % 301) / 100) - 1.5f;
        float sz = ((float)(rand() % 301) / 100) - 1.5f;
        float radii[4] = {1.5f, 3, 5, 9};
        float radius = radii[rand() % 4];
        vec3_t direction = vec3_normalize(vec3_new(sx, sy, sz));
        vec3_t point = vec3_mul(direction, radius);
        float expected = ref_visibility(ps, point);
        float actual = point_shadow_visibility(ps, point);
        TASK_CHECK(fabsf(expected - actual) < 1e-6f);
    }

    point_shadow_release(ps);
    return 0;
}

static int sampling_radial_vs_axis(void) {
    point_shadow_t *ps = point_shadow_create(vec3_new(0, 0, 0),
                                             1, 10, 8, 0);
    vec3_t texel_direction = vec3_new(1, 0.625f, -0.625f);
    float length = vec3_length(texel_direction);
    vec3_t unit = vec3_div(texel_direction, length);
    vec3_t receiver = vec3_mul(unit, 2.5f);
    float axis_depth = depth_from_axis(1, 10, 2);

    /* px face texel (column 6, row 1), whose center is exactly this ray */
    ps->faces[0]->depth_buffer[1 * 8 + 6] = axis_depth;

    /*
     * axis distance of the occluder is 2 but its radial distance is
     * 2 * 1.3346..., so a receiver 2.5 away along that ray stays visible
     */
    TASK_CHECK(2 * length > 2.5f);
    TASK_CHECK(point_shadow_visibility(ps, receiver) == 1);
    TASK_CHECK(ref_visibility(ps, receiver) == 1);

    /* the same receiver compared against a naive axis test would be shadowed */
    TASK_CHECK(2.5f > 2);

    point_shadow_release(ps);
    return 0;
}

static int sampling_seams_and_corners(void) {
    point_shadow_t *ps = point_shadow_create(vec3_new(0, 0, 0),
                                             1, 10, 8, 0);
    float wall_depth = depth_from_axis(1, 10, 2);
    int final_face = -1;
    int visible;
    vec3_t seam_point, corner_point;

    /*
     * wall only on the pz face, next to its px-facing edge. a px receiver
     * crossing that edge remaps onto pz column 7; clamping on px would miss
     * the occluder entirely. axis distance 2 keeps its radial distance on
     * the diagonal texel centers (~2.67) below the receiver distance of 3.
     */
    ps->faces[4]->depth_buffer[3 * 8 + 6] = wall_depth;
    ps->faces[4]->depth_buffer[3 * 8 + 7] = wall_depth;
    ps->faces[4]->depth_buffer[4 * 8 + 6] = wall_depth;
    ps->faces[4]->depth_buffer[4 * 8 + 7] = wall_depth;

    /* px receiver whose leftmost taps cross onto the pz neighbor face */
    seam_point = vec3_mul(vec3_normalize(vec3_new(1, 0, 0.95f)), 3);
    visible = ref_tap(ps, 0,
                      0.0128f - 1.0f / 8, 0.5f,
                      vec3_length(seam_point), &final_face);
    TASK_CHECK(final_face == 4);
    TASK_CHECK(visible == 0);
    TASK_CHECK(fabsf(point_shadow_visibility(ps, seam_point) -
                     ref_visibility(ps, seam_point)) < 1e-6f);
    TASK_CHECK(point_shadow_visibility(ps, seam_point) > 0);
    TASK_CHECK(point_shadow_visibility(ps, seam_point) < 1);

    /* a corner tap remaps through both out-of-range coordinates onto py */
    corner_point = vec3_mul(vec3_normalize(vec3_new(1, 1.15f, 1.05f)), 3);
    visible = ref_tap(ps, 0,
                      -0.025f, -0.075f,
                      vec3_length(corner_point), &final_face);
    TASK_CHECK(final_face == 2);
    TASK_CHECK(visible == 1);
    TASK_CHECK(fabsf(point_shadow_visibility(ps, corner_point) -
                     ref_visibility(ps, corner_point)) < 1e-6f);

    point_shadow_release(ps);
    return 0;
}

static int sampling_pcf_fraction(void) {
    point_shadow_t *ps = point_shadow_create(vec3_new(0, 0, 0),
                                             1, 10, 8, 0);
    float occluder_depth = depth_from_axis(1, 10, 2);

    /* receiver on the px face center at radial distance 5 */
    ps->faces[0]->depth_buffer[3 * 8 + 3] = occluder_depth;
    TASK_CHECK(fabsf(point_shadow_visibility(ps, vec3_new(5, 0, 0)) -
                     8.0f / 9) < 1e-6f);

    ps->faces[0]->depth_buffer[3 * 8 + 4] = occluder_depth;
    ps->faces[0]->depth_buffer[4 * 8 + 3] = occluder_depth;
    ps->faces[0]->depth_buffer[4 * 8 + 4] = occluder_depth;
    TASK_CHECK(fabsf(point_shadow_visibility(ps, vec3_new(5, 0, 0)) -
                     5.0f / 9) < 1e-6f);

    /* everything occluded */
    {
        int column, row;
        for (row = 2; row <= 5; row++) {
            for (column = 2; column <= 5; column++) {
                ps->faces[0]->depth_buffer[row * 8 + column] = occluder_depth;
            }
        }
    }
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(5, 0, 0)) == 0);
    TASK_CHECK(ref_visibility(ps, vec3_new(5, 0, 0)) == 0);

    point_shadow_release(ps);
    return 0;
}

/* ========================================================================== */
/* capture group                                                              */
/* ========================================================================== */

static int capture_six_faces(void) {
    static const vec3_t positions[6] = {
        { 2,  0,  0}, {-2,  0,  0},
        { 0,  2,  0}, { 0, -2,  0},
        { 0,  0,  2}, { 0,  0, -2},
    };
    static const vec3_t rotations[6] = {
        {0, -1.5707963f, 0}, {0, 1.5707963f, 0},
        {1.5707963f, 0, 0}, {-1.5707963f, 0, 0},
        {0, 0, 0}, {0, 3.1415927f, 0},
    };
    blinn_material_t material;
    int face, other;

    memset(&material, 0, sizeof(material));
    material.basecolor = vec4_new(1, 1, 1, 1);
    material.shininess = 8;
    material.double_sided = 1;

    /* one scene per face proves the faces never share matrices or depths */
    for (face = 0; face < 6; face++) {
        model_t **models = NULL;
        scene_t *scene;
        framebuffer_t *framebuffer = framebuffer_create(32, 32);
        perframe_t perframe = make_perframe();
        point_shadow_t *ps;
        mat4_t view, proj;
        float expected;
        model_t *model;

        model = blinn_create_model(
            "checks/triangle.obj",
            place_triangle(positions[face], rotations[face]),
            NULL, -1, &material);
        darray_push(models, model);
        scene = scene_create(vec3_new(0, 0, 0), NULL, models, 0, 1, 0, 0);
        TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                         0.5f, 10, 32, 0) == 1);
        ps = scene->point_shadow;
        perframe.point_shadow = ps;
        test_draw_scene(scene, framebuffer, &perframe);

        view = point_shadow_view_matrix(ps, face);
        proj = point_shadow_proj_matrix(ps);
        expected = projected_depth(view, proj, positions[face]);

        TASK_CHECK(count_written(ps->faces[face]) > 0);
        TASK_CHECK(written_off_center(ps, face));
        TASK_CHECK(face_depth_constant(ps, face, expected));
        for (other = 0; other < 6; other++) {
            if (other != face) {
                TASK_CHECK(face_all_clear(ps, other));
            }
        }

        framebuffer_release(framebuffer);
        scene_release(scene);
    }
    cache_cleanup();
    return 0;
}

static int capture_offscreen_caster(void) {
    blinn_material_t material;
    scene_t *scene;
    model_t **models = NULL;
    model_t *receiver, *caster;
    framebuffer_t *framebuffer = framebuffer_create(32, 32);
    perframe_t perframe = make_perframe();
    point_shadow_t *ps;

    memset(&material, 0, sizeof(material));
    material.basecolor = vec4_new(1, 1, 1, 1);
    material.shininess = 8;
    material.double_sided = 1;

    receiver = blinn_create_model("checks/triangle.obj",
                                  mat4_translate(0, 0, -2), NULL, -1,
                                  &material);
    caster = blinn_create_model("checks/triangle.obj",
                                place_triangle(vec3_new(0, 0, 2),
                                               vec3_new(0, 3.1415927f, 0)),
                                NULL, -1, &material);
    darray_push(models, receiver);
    darray_push(models, caster);
    scene = scene_create(vec3_new(0, 0, 0), NULL, models, 0, 1, 0, 0);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 32, 0) == 1);
    ps = scene->point_shadow;
    perframe.point_shadow = ps;
    test_draw_scene(scene, framebuffer, &perframe);

    /* the caster sits behind the camera yet is captured on the pz face */
    TASK_CHECK(count_written(ps->faces[4]) > 0);
    TASK_CHECK(count_written(ps->faces[5]) > 0);
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(0, 0, 3)) < 1);
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(0, 0, 3)) ==
               ref_visibility(ps, vec3_new(0, 0, 3)));

    /* main camera only sees the receiver triangle */
    TASK_CHECK(count_written(framebuffer) == 128);

    framebuffer_release(framebuffer);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

static int capture_alpha_hole(void) {
    char alpha_path[PATH_SIZE];
    char emission_path[PATH_SIZE];
    blinn_material_t cut_material, full_material;
    scene_t *cut_scene, *full_scene;
    framebuffer_t *framebuffer = framebuffer_create(32, 32);
    perframe_t cut_perframe, full_perframe;
    point_shadow_t *cut_ps, *full_ps;
    int cut_count, full_count, i, found_hole = 0;

    make_texture_paths(alpha_path, emission_path);
    write_rgba_tga(alpha_path, 8, 8, 255, 0, 255, 1);

    memset(&cut_material, 0, sizeof(cut_material));
    cut_material.basecolor = vec4_new(1, 1, 1, 1);
    cut_material.shininess = 8;
    cut_material.double_sided = 1;
    cut_material.alpha_cutoff = 0.5f;
    cut_material.diffuse_map = alpha_path;

    memset(&full_material, 0, sizeof(full_material));
    full_material.basecolor = vec4_new(1, 1, 1, 1);
    full_material.shininess = 8;
    full_material.double_sided = 1;
    full_material.diffuse_map = alpha_path;

    cut_scene = blinn_triangle_scene(mat4_translate(0, 0, -2),
                                     &cut_material, 0, 1);
    full_scene = blinn_triangle_scene(mat4_translate(0, 0, -2),
                                      &full_material, 0, 1);
    TASK_CHECK(scene_set_point_light(cut_scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 32, 0) == 1);
    TASK_CHECK(scene_set_point_light(full_scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 32, 0) == 1);
    cut_ps = cut_scene->point_shadow;
    full_ps = full_scene->point_shadow;
    cut_perframe = make_perframe();
    full_perframe = make_perframe();
    cut_perframe.point_shadow = cut_ps;
    full_perframe.point_shadow = full_ps;

    test_draw_scene(cut_scene, framebuffer, &cut_perframe);
    test_draw_scene(full_scene, framebuffer, &full_perframe);

    cut_count = count_written(cut_ps->faces[5]);
    full_count = count_written(full_ps->faces[5]);
    TASK_CHECK(cut_count > 0);
    TASK_CHECK(cut_count < full_count);
    for (i = 0; i < 32 * 32; i++) {
        if (cut_ps->faces[5]->depth_buffer[i] == 1 &&
            full_ps->faces[5]->depth_buffer[i] < 1) {
            found_hole = 1;
        }
    }
    TASK_CHECK(found_hole);

    framebuffer_release(framebuffer);
    scene_release(cut_scene);
    scene_release(full_scene);
    cache_cleanup();
    remove(alpha_path);
    return 0;
}

static int capture_double_sided(void) {
    blinn_material_t material;
    scene_t *away_scene, *toward_scene;
    framebuffer_t *framebuffer = framebuffer_create(32, 32);
    perframe_t perframe = make_perframe();
    point_shadow_t *away_ps, *toward_ps;

    memset(&material, 0, sizeof(material));
    material.basecolor = vec4_new(1, 1, 1, 1);
    material.shininess = 8;
    material.double_sided = 0;   /* single sided: back faces are culled */

    away_scene = blinn_triangle_scene(
        place_triangle(vec3_new(0, 0, -2), vec3_new(0, 3.1415927f, 0)),
        &material, 0, 1);
    toward_scene = blinn_triangle_scene(mat4_translate(0, 0, -2),
                                        &material, 0, 1);
    TASK_CHECK(scene_set_point_light(away_scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 32, 0) == 1);
    TASK_CHECK(scene_set_point_light(toward_scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 32, 0) == 1);
    away_ps = away_scene->point_shadow;
    toward_ps = toward_scene->point_shadow;
    perframe.point_shadow = away_ps;
    test_draw_scene(away_scene, framebuffer, &perframe);
    perframe.point_shadow = toward_ps;
    test_draw_scene(toward_scene, framebuffer, &perframe);

    TASK_CHECK(face_all_clear(away_ps, 5));
    TASK_CHECK(count_written(toward_ps->faces[5]) > 0);

    framebuffer_release(framebuffer);
    scene_release(away_scene);
    scene_release(toward_scene);
    cache_cleanup();
    return 0;
}

static int capture_reconfigure_and_clear(void) {
    blinn_material_t material;
    scene_t *scene;
    framebuffer_t *framebuffer = framebuffer_create(32, 32);
    perframe_t perframe = make_perframe();
    point_shadow_t *ps;
    int face;

    memset(&material, 0, sizeof(material));
    material.basecolor = vec4_new(1, 1, 1, 1);
    material.shininess = 8;
    material.double_sided = 1;
    scene = blinn_triangle_scene(
        place_triangle(vec3_new(2, 0, 0), vec3_new(0, -1.5707963f, 0)),
        &material, 0, 1);

    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 16, 0) == 1);
    ps = scene->point_shadow;
    TASK_CHECK(ps->faces[0]->width == 16);
    perframe.point_shadow = ps;
    test_draw_scene(scene, framebuffer, &perframe);
    TASK_CHECK(count_written(ps->faces[0]) > 0);

    /* reconfiguration hands out freshly cleared faces of the new size */
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 32, 0.1f) == 1);
    ps = scene->point_shadow;
    TASK_CHECK(ps->faces[0]->width == 32);
    TASK_CHECK(fabsf(ps->bias - 0.1f) < 1e-6f);
    TASK_CHECK(all_faces_clear(ps));

    /* drawing again repopulates the expected face */
    perframe.point_shadow = ps;
    test_draw_scene(scene, framebuffer, &perframe);
    for (face = 0; face < 6; face++) {
        if (face != 0) {
            TASK_CHECK(face_all_clear(ps, face));
        }
    }
    TASK_CHECK(count_written(ps->faces[0]) > 0);

    /* size 0 releases the configuration */
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 0, 0) == 1);
    TASK_CHECK(scene->point_shadow == NULL);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 0, 0) == 1);

    framebuffer_release(framebuffer);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

static int capture_config_validation(void) {
    blinn_material_t material;
    scene_t *scene;
    point_shadow_t *ps;

    memset(&material, 0, sizeof(material));
    material.basecolor = vec4_new(1, 1, 1, 1);
    material.shininess = 8;
    material.double_sided = 1;
    scene = blinn_triangle_scene(mat4_identity(), &material, 0, 1);

    TASK_CHECK(scene != NULL);
    TASK_CHECK(scene->point_shadow == NULL);

    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     0, 10, 16, 0) == 0);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     -1, 10, 16, 0) == 0);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     5, 5, 16, 0) == 0);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     5, 4, 16, 0) == 0);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     1, 10, -1, 0) == 0);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     1, 10, 16, -0.1f) == 0);
    TASK_CHECK(scene_set_point_light(scene,
                                     vec3_new(make_nan(), 0, 0),
                                     1, 10, 16, 0) == 0);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     make_inf(), 10, 16, 0) == 0);
    TASK_CHECK(scene->point_shadow == NULL);

    TASK_CHECK(scene_set_point_light(scene, vec3_new(1, 2, 3),
                                     1, 10, 16, 0.2f) == 1);
    ps = scene->point_shadow;
    TASK_CHECK(ps != NULL);
    TASK_CHECK(ps->position.x == 1 && ps->position.y == 2 &&
               ps->position.z == 3);

    /* an invalid reconfiguration leaves the old configuration untouched */
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     10, 5, 8, 0) == 0);
    TASK_CHECK(scene->point_shadow == ps);
    TASK_CHECK(ps->faces[0]->width == 16);

    scene_release(scene);
    cache_cleanup();
    return 0;
}

static int capture_scene_interleave(void) {
    blinn_material_t material;
    scene_t *scene_a, *scene_b;
    framebuffer_t *framebuffer = framebuffer_create(32, 32);
    perframe_t perframe_a = make_perframe();
    perframe_t perframe_b = make_perframe();
    point_shadow_t *ps_a, *ps_b;

    memset(&material, 0, sizeof(material));
    material.basecolor = vec4_new(1, 1, 1, 1);
    material.shininess = 8;
    material.double_sided = 1;

    scene_a = blinn_triangle_scene(
        place_triangle(vec3_new(2, 0, 0), vec3_new(0, -1.5707963f, 0)),
        &material, 0, 1);
    scene_b = blinn_triangle_scene(
        place_triangle(vec3_new(-2, 0, 0), vec3_new(0, 1.5707963f, 0)),
        &material, 0, 1);
    TASK_CHECK(scene_set_point_light(scene_a, vec3_new(0, 0, 0),
                                     0.5f, 10, 16, 0) == 1);
    TASK_CHECK(scene_set_point_light(scene_b, vec3_new(0, 0, 0),
                                     0.5f, 10, 16, 0) == 1);
    ps_a = scene_a->point_shadow;
    ps_b = scene_b->point_shadow;
    TASK_CHECK(ps_a->faces[0] != ps_b->faces[0]);
    perframe_a.point_shadow = ps_a;
    perframe_b.point_shadow = ps_b;

    /* interleave the two scenes; each set of six faces stays independent */
    test_draw_scene(scene_a, framebuffer, &perframe_a);
    test_draw_scene(scene_b, framebuffer, &perframe_b);
    test_draw_scene(scene_a, framebuffer, &perframe_a);
    TASK_CHECK(count_written(ps_a->faces[0]) > 0);
    TASK_CHECK(face_all_clear(ps_a, 1));
    TASK_CHECK(face_all_clear(ps_b, 0));
    TASK_CHECK(count_written(ps_b->faces[1]) > 0);

    framebuffer_release(framebuffer);
    scene_release(scene_a);
    scene_release(scene_b);
    cache_cleanup();
    return 0;
}

/* ========================================================================== */
/* materials group                                                            */
/* ========================================================================== */

static unsigned char center_red(const framebuffer_t *framebuffer) {
    int center = 16 * framebuffer->width + 16;
    return framebuffer->color_buffer[center * 4];
}

static int matrices_equal(const mat4_t *a, const mat4_t *b) {
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (fabsf(a->m[i][j] - b->m[i][j]) > 1e-6f) {
                return 0;
            }
        }
    }
    return 1;
}

static int materials_blinn_scene(void) {
    blinn_material_t material;
    scene_t *scene;
    framebuffer_t *framebuffer = framebuffer_create(32, 32);
    perframe_t perframe = make_perframe();
    point_shadow_t *ps;
    unsigned char front, side, far_away, behind, emission_byte;
    char alpha_path[PATH_SIZE];
    char emission_path[PATH_SIZE];

    memset(&material, 0, sizeof(material));
    material.basecolor = vec4_new(1, 1, 1, 1);
    material.shininess = 8;
    material.double_sided = 1;
    scene = blinn_triangle_scene(mat4_translate(0, 0, -2), &material,
                                 0, 1);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, -1),
                                     0.1f, 20, 32, 0.01f) == 1);
    ps = scene->point_shadow;
    perframe.point_shadow = ps;

    /* r ~= 1, normal faces the light, no caster: near full strength */
    TASK_CHECK(point_shadow_visibility(ps, vec3_new(0, 0, -2)) == 1);
    test_draw_scene(scene, framebuffer, &perframe);
    front = center_red(framebuffer);
    TASK_CHECK(front >= 248);

    /* r = 2: punctual intensity / r^2 = 0.25 */
    ps->position = vec3_new(0, 0, 0);
    test_draw_scene(scene, framebuffer, &perframe);
    side = center_red(framebuffer);
    TASK_CHECK(side >= 61 && side <= 65);

    /* r = 4: punctual intensity / r^2 = 1/16 */
    ps->position = vec3_new(0, 0, 2);
    test_draw_scene(scene, framebuffer, &perframe);
    far_away = center_red(framebuffer);
    TASK_CHECK(far_away >= 13 && far_away <= 17);
    TASK_CHECK((int)side > (int)far_away);

    /* light behind the surface: n_dot_l < 0, no direct light */
    ps->position = vec3_new(0, 0, -3);
    test_draw_scene(scene, framebuffer, &perframe);
    behind = center_red(framebuffer);
    TASK_CHECK(behind == 0);

    /* emission survives even with the point light behind the surface */
    make_texture_paths(alpha_path, emission_path);
    write_rgba_tga(emission_path, 4, 4, 128, 128, 128, 0);
    {
        blinn_material_t emissive;
        scene_t *emissive_scene;
        framebuffer_t *emissive_buffer = framebuffer_create(32, 32);
        perframe_t emissive_perframe = make_perframe();
        memset(&emissive, 0, sizeof(emissive));
        emissive.basecolor = vec4_new(1, 1, 1, 1);
        emissive.shininess = 8;
        emissive.double_sided = 1;
        emissive.emission_map = emission_path;
        emissive_scene = blinn_triangle_scene(mat4_translate(0, 0, -2),
                                              &emissive, 0, 1);
        TASK_CHECK(scene_set_point_light(emissive_scene,
                                         vec3_new(0, 0, -3),
                                         0.1f, 20, 32, 0.01f) == 1);
        emissive_perframe.point_shadow = emissive_scene->point_shadow;
        test_draw_scene(emissive_scene, emissive_buffer, &emissive_perframe);
        emission_byte = center_red(emissive_buffer);
        TASK_CHECK(emission_byte == 128);
        framebuffer_release(emissive_buffer);
        scene_release(emissive_scene);
    }

    framebuffer_release(framebuffer);
    scene_release(scene);
    cache_cleanup();
    remove(emission_path);
    return 0;
}

static int materials_pbr_scene(int metalness) {
    scene_t *scene;
    framebuffer_t *framebuffer = framebuffer_create(32, 32);
    perframe_t perframe = make_perframe();
    point_shadow_t *ps;
    unsigned char lit, dim, dark;

    scene = pbr_triangle_scene(metalness, mat4_translate(0, 0, -2), NULL,
                               0, 1);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, -1),
                                     0.1f, 20, 32, 0.01f) == 1);
    ps = scene->point_shadow;
    perframe.point_shadow = ps;
    test_draw_scene(scene, framebuffer, &perframe);
    lit = center_red(framebuffer);
    TASK_CHECK(lit > 0);

    /* square falloff: moving the light away darkens the same pixel */
    ps->position = vec3_new(0, 0, 0);
    test_draw_scene(scene, framebuffer, &perframe);
    dim = center_red(framebuffer);
    TASK_CHECK(dim > 0 && dim < lit);

    /* light behind the surface: n_dot_l < 0 gives no direct light at all */
    ps->position = vec3_new(0, 0, -3);
    test_draw_scene(scene, framebuffer, &perframe);
    dark = center_red(framebuffer);
    TASK_CHECK(dark == 0);

    framebuffer_release(framebuffer);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

static int materials_pbrm_scene(void) {
    return materials_pbr_scene(1);
}

static int materials_pbrs_scene(void) {
    return materials_pbr_scene(0);
}

static int materials_fragment_entries(void) {
    blinn_material_t blinn_mat;
    pbrm_material_t pbrm_mat;
    pbrs_material_t pbrs_mat;
    model_t *blinn_model, *pbrm_model, *pbrs_model;
    blinn_uniforms_t *blinn_u;
    pbr_uniforms_t *pbrm_u, *pbrs_u;
    blinn_varyings_t blinn_v;
    pbr_varyings_t pbr_v;
    point_shadow_t *ps = point_shadow_create(vec3_new(0, 0, -2),
                                             0.1f, 20, 8, 0);
    int discard;
    vec4_t color;

    memset(&blinn_mat, 0, sizeof(blinn_mat));
    blinn_mat.basecolor = vec4_new(1, 1, 1, 1);
    blinn_mat.shininess = 8;
    blinn_mat.double_sided = 1;
    memset(&pbrm_mat, 0, sizeof(pbrm_mat));
    pbrm_mat.basecolor_factor = vec4_new(1, 1, 1, 1);
    pbrm_mat.roughness_factor = 0.7f;
    pbrm_mat.double_sided = 1;
    memset(&pbrs_mat, 0, sizeof(pbrs_mat));
    pbrs_mat.diffuse_factor = vec4_new(1, 1, 1, 1);
    pbrs_mat.specular_factor = vec3_new(0.2f, 0.2f, 0.2f);
    pbrs_mat.glossiness_factor = 0.3f;
    pbrs_mat.double_sided = 1;

    blinn_model = blinn_create_model("checks/triangle.obj", mat4_identity(),
                                     NULL, -1, &blinn_mat);
    pbrm_model = pbrm_create_model("checks/triangle.obj", mat4_identity(),
                                   NULL, -1, &pbrm_mat, NULL);
    pbrs_model = pbrs_create_model("checks/triangle.obj", mat4_identity(),
                                   NULL, -1, &pbrs_mat, NULL);
    blinn_u = program_get_uniforms(blinn_model->program);
    pbrm_u = program_get_uniforms(pbrm_model->program);
    pbrs_u = program_get_uniforms(pbrs_model->program);
    blinn_u->camera_pos = vec3_new(0, 0, 0);
    blinn_u->punctual_intensity = 1;
    pbrm_u->camera_pos = vec3_new(0, 0, 0);
    pbrm_u->punctual_intensity = 1;
    pbrs_u->camera_pos = vec3_new(0, 0, 0);
    pbrs_u->punctual_intensity = 1;

    memset(&blinn_v, 0, sizeof(blinn_v));
    blinn_v.normal = vec3_new(0, 0, 1);
    blinn_v.texcoord = vec2_new(0, 0);
    memset(&pbr_v, 0, sizeof(pbr_v));
    pbr_v.world_normal = vec3_new(0, 0, 1);
    pbr_v.texcoord = vec2_new(0, 0);

    /* r = 0: all three fragment entries output no direct light */
    blinn_u->point_shadow = ps;
    blinn_v.world_position = vec3_new(0, 0, -2);
    discard = 0;
    color = blinn_fragment_shader(&blinn_v, blinn_u, &discard, 0);
    TASK_CHECK(color.x == 0 && color.y == 0 && color.z == 0);

    pbrm_u->point_shadow = ps;
    pbr_v.world_position = vec3_new(0, 0, -2);
    discard = 0;
    color = pbr_fragment_shader(&pbr_v, pbrm_u, &discard, 0);
    TASK_CHECK(color.x == 0 && color.y == 0 && color.z == 0);

    pbrs_u->point_shadow = ps;
    discard = 0;
    color = pbr_fragment_shader(&pbr_v, pbrs_u, &discard, 0);
    TASK_CHECK(color.x == 0 && color.y == 0 && color.z == 0);

    /* r = 1 with the normal pointing at the light: direct light appears */
    blinn_v.world_position = vec3_new(0, 0, -3);
    color = blinn_fragment_shader(&blinn_v, blinn_u, &discard, 0);
    TASK_CHECK(color.x > 0);
    pbr_v.world_position = vec3_new(0, 0, -3);
    color = pbr_fragment_shader(&pbr_v, pbrm_u, &discard, 0);
    TASK_CHECK(color.x > 0);
    color = pbr_fragment_shader(&pbr_v, pbrs_u, &discard, 0);
    TASK_CHECK(color.x > 0);

    /* ambient/IBL and the layer preview ignore point-light visibility */
    {
        ibldata_t *ibl;
        vec4_t ibl_on, ibl_off, layer_on, layer_off;
        /* the image cache resolves environment paths relative to assets/ */
        TASK_CHECK(change_directory("assets") == 0);
        ibl = cache_acquire_ibldata("spruit");
        TASK_CHECK(change_directory("..") == 0);
        pbrm_u->ibldata = ibl;
        pbrm_u->ambient_intensity = 1;
        pbrm_u->point_shadow = ps;
        /* the fragment sits on the light: r = 0, so no direct light either way */
        pbr_v.world_position = vec3_new(0, 0, -2);
        ibl_on = pbr_fragment_shader(&pbr_v, pbrm_u, &discard, 0);
        pbrm_u->punctual_intensity = 0;
        ibl_off = pbr_fragment_shader(&pbr_v, pbrm_u, &discard, 0);
        TASK_CHECK(ibl_on.x > 0);
        TASK_CHECK(ibl_on.x == ibl_off.x && ibl_on.y == ibl_off.y &&
                   ibl_on.z == ibl_off.z);
        pbrm_u->punctual_intensity = 1;

        pbrm_u->layer_view = 1;
        layer_on = pbr_fragment_shader(&pbr_v, pbrm_u, &discard, 0);
        pbrm_u->point_shadow = NULL;
        layer_off = pbr_fragment_shader(&pbr_v, pbrm_u, &discard, 0);
        TASK_CHECK(layer_on.x == layer_off.x && layer_on.y == layer_off.y &&
                   layer_on.z == layer_off.z);
        /* the owning pbrm model releases this ibldata on teardown */
    }

    point_shadow_release(ps);
    blinn_model->release(blinn_model);
    pbrm_model->release(pbrm_model);
    pbrs_model->release(pbrs_model);
    cache_cleanup();
    return 0;
}

static int materials_matrix_restore(void) {
    blinn_material_t material;
    scene_t *scene;
    framebuffer_t *framebuffer = framebuffer_create(32, 32);
    perframe_t perframe = make_perframe();
    model_t *model;
    blinn_uniforms_t *uniforms;
    mat4_t expected_light_vp, expected_camera_vp, face_vp;
    point_shadow_t *ps;

    memset(&material, 0, sizeof(material));
    material.basecolor = vec4_new(1, 1, 1, 1);
    material.shininess = 8;
    material.double_sided = 1;
    scene = blinn_triangle_scene(mat4_translate(0, 0, -2), &material,
                                 0, 1);
    TASK_CHECK(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                     0.5f, 10, 16, 0) == 1);
    ps = scene->point_shadow;

    /* distinctive caller matrices that no face matrix could reproduce */
    perframe.light_view_matrix = mat4_translate(3, 5, 7);
    perframe.light_proj_matrix = mat4_perspective(1.2f, 1, 0.5f, 8);
    perframe.point_shadow = ps;
    expected_light_vp = mat4_mul_mat4(perframe.light_proj_matrix,
                                      perframe.light_view_matrix);
    expected_camera_vp = mat4_mul_mat4(perframe.camera_proj_matrix,
                                       perframe.camera_view_matrix);
    face_vp = mat4_mul_mat4(point_shadow_proj_matrix(ps),
                            point_shadow_view_matrix(ps, 0));

    test_draw_scene(scene, framebuffer, &perframe);

    model = scene->models[0];
    uniforms = program_get_uniforms(model->program);
    TASK_CHECK(matrices_equal(&uniforms->light_vp_matrix,
                              &expected_light_vp));
    TASK_CHECK(matrices_equal(&uniforms->camera_vp_matrix,
                              &expected_camera_vp));
    TASK_CHECK(!matrices_equal(&uniforms->light_vp_matrix, &face_vp));

    /* the caller's perframe light matrices are restored as well */
    {
        mat4_t caller_view = mat4_translate(3, 5, 7);
        TASK_CHECK(matrices_equal(&perframe.light_view_matrix, &caller_view));
    }

    framebuffer_release(framebuffer);
    scene_release(scene);
    cache_cleanup();
    return 0;
}

/* ========================================================================== */
/* registration                                                               */
/* ========================================================================== */

const struct task_case task_cases[] = {
    {"capture", "six_faces", capture_six_faces},
    {"capture", "offscreen_caster", capture_offscreen_caster},
    {"capture", "alpha_hole", capture_alpha_hole},
    {"capture", "double_sided", capture_double_sided},
    {"capture", "reconfigure_and_clear", capture_reconfigure_and_clear},
    {"capture", "config_validation", capture_config_validation},
    {"capture", "scene_interleave", capture_scene_interleave},

    {"sampling", "matches_reference", sampling_matches_reference},
    {"sampling", "radial_vs_axis", sampling_radial_vs_axis},
    {"sampling", "seams_and_corners", sampling_seams_and_corners},
    {"sampling", "pcf_fraction", sampling_pcf_fraction},

    {"materials", "blinn_scene", materials_blinn_scene},
    {"materials", "pbrm_scene", materials_pbrm_scene},
    {"materials", "pbrs_scene", materials_pbrs_scene},
    {"materials", "fragment_entries", materials_fragment_entries},
    {"materials", "matrix_restore", materials_matrix_restore},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
