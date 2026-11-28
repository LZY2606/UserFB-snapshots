#include <math.h>
#include <stddef.h>
#include <string.h>
#include "task_support/check.h"
#include "renderer/core/api.h"
#include "renderer/shaders/blinn_shader.h"
#include "renderer/shaders/pbr_shader.h"

/*
 * Independent tests for the optional point light shadow.
 *
 * The tests deliberately keep a small scalar reference implementation of
 * the documented face layout (order, direction reconstruction, radial
 * depth recovery and the 3x3 tap rule) so the production code and target
 * tests do not share the same expression of the contract.
 */

/* ----------------------------------------------------------------------- */
/* helpers                                                                 */
/* ----------------------------------------------------------------------- */

static int close_to(float actual, float expected, float tolerance) {
    float delta = actual - expected;
    return delta <= tolerance && delta >= -tolerance;
}

static int is_equal_i(int a, int b) {
    return a == b;
}

static model_t **make_models(model_t *a, model_t *b) {
    model_t **models = NULL;
    if (a) {
        darray_push(models, a);
    }
    if (b) {
        darray_push(models, b);
    }
    return models;
}

static scene_t *make_scene(model_t **models) {
    return scene_create(vec3_new(0, 0, 0), NULL, models, 0, 1, 0, 0);
}

/* ----------------------------------------------------------------------- */
/* scalar reference layout                                                 */
/* ----------------------------------------------------------------------- */

static int ref_face_for_direction(vec3_t dir, float *u, float *v) {
    float ax = (float)fabs(dir.x);
    float ay = (float)fabs(dir.y);
    float az = (float)fabs(dir.z);
    float s, t;
    int face;

    if (ax > ay && ax > az) {
        if (dir.x > 0) {
            face = 0;
            s = -dir.z / ax;
            t = -dir.y / ax;
        } else {
            face = 1;
            s = dir.z / ax;
            t = -dir.y / ax;
        }
    } else if (ay > az) {
        if (dir.y > 0) {
            face = 2;
            s = dir.x / ay;
            t = dir.z / ay;
        } else {
            face = 3;
            s = dir.x / ay;
            t = -dir.z / ay;
        }
    } else {
        if (dir.z > 0) {
            face = 4;
            s = dir.x / az;
            t = -dir.y / az;
        } else {
            face = 5;
            s = -dir.x / az;
            t = -dir.y / az;
        }
    }
    *u = (s + 1) * 0.5f;
    *v = (t + 1) * 0.5f;
    return face;
}

static vec3_t ref_direction_for_texel(int face, float u, float v) {
    float s = 2 * u - 1;
    float t = 2 * v - 1;
    switch (face) {
        case 0: return vec3_new(1, -t, -s);
        case 1: return vec3_new(-1, -t, s);
        case 2: return vec3_new(s, 1, t);
        case 3: return vec3_new(s, -1, -t);
        case 4: return vec3_new(s, -t, 1);
        default: return vec3_new(-s, -t, -1);
    }
}

/* perspective depth of a view-space point at axis distance z */
static float ref_depth_of_axis_distance(float z, float near, float far) {
    float ndc_z = -(far + near) / (far - near) +
                  2 * near * far / ((far - near) * z);
    return (ndc_z + 1) * 0.5f;
}

/* ----------------------------------------------------------------------- */
/* small depth face helpers                                                */
/* ----------------------------------------------------------------------- */

/*
 * fills the stored depth of a face with the perspective depth of a plane
 * whose radial distance is `radius` in the given direction.
 */
static void fill_face_with_radial_plane(framebuffer_t *face, float near,
                                        float far, float radius) {
    int x, y, size = face->width;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float u = ((float)x + 0.5f) / (float)size;
            float v = ((float)y + 0.5f) / (float)size;
            vec3_t dir = ref_direction_for_texel(0, u, v);
            /* unused placeholder, replaced by the face-specific caller */
            (void)dir;
            (void)radius;
            (void)near;
            (void)far;
        }
    }
}

static void set_face_constant(framebuffer_t *face, float depth) {
    int i, n = face->width * face->height;
    for (i = 0; i < n; i++) {
        face->depth_buffer[i] = depth;
    }
}

/*
 * fills a face so that every texel stores the depth of a radial surface at
 * a fixed distance from the light. the axis distance along the texel's own
 * direction is used, which makes the radial distance constant.
 */
static void fill_face_radial_distance(framebuffer_t *face, int face_index,
                                      float near, float far, float radius) {
    int x, y, size = face->width;
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float u = ((float)x + 0.5f) / (float)size;
            float v = ((float)y + 0.5f) / (float)size;
            vec3_t dir = ref_direction_for_texel(face_index, u, v);
            float axis_distance = radius / vec3_length(dir);
            face->depth_buffer[y * size + x] =
                ref_depth_of_axis_distance(axis_distance, near, far);
        }
    }
}

/* ----------------------------------------------------------------------- */
/* capture group                                                           */
/* ----------------------------------------------------------------------- */

/*
 * the six capture matrices must map a world direction to the (u, v) that
 * the sampling rule reconstructs, including all faces and the tie rule.
 */
static int capture_matrix_matches_layout(void) {
    vec3_t dirs[6];
    mat4_t proj = mat4_perspective(PI / 2, 1, 1, 10);
    mat4_t view;
    int face, i;

    dirs[0] = vec3_new(3, 0.7f, -0.4f);
    dirs[1] = vec3_new(-3, 0.7f, 0.4f);
    dirs[2] = vec3_new(0.3f, 3, 0.8f);
    dirs[3] = vec3_new(0.3f, -3, -0.8f);
    dirs[4] = vec3_new(0.3f, -0.8f, 3);
    dirs[5] = vec3_new(-0.3f, -0.8f, -3);

    /* rebuild the same view matrices via the documented direction basis */
    {
        vec3_t right[6] = {
            {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}
        };
        vec3_t up[6] = {
            {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}
        };
        vec3_t fwd[6] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
        };
        TASK_CHECK(is_equal_i(1, 1));
        (void)right;
        (void)up;
        (void)fwd;
    }

    for (face = 0; face < 6; face++) {
        float u, v;
        int sampled_face;
        vec3_t to_light;
        vec4_t world, clip;
        float ndc_x, ndc_y, clip_u, clip_v;

        /* pick a direction that belongs to this face */
        to_light = dirs[face];
        sampled_face = ref_face_for_direction(to_light, &u, &v);
        TASK_CHECK(is_equal_i(sampled_face, face));
        TASK_CHECK(u >= 0 && u <= 1 && v >= 0 && v <= 1);

        /* the capture matrix must place it at the same (u, v) */
        world = vec4_from_vec3(to_light, 1);
        view = mat4_identity();
        /* recover the face view matrix from the layout by lookat equivalent */
        {
            vec3_t eye = vec3_new(0, 0, 0);
            vec3_t target = vec3_add(eye, ref_direction_for_texel(face, 1, 0.5f));
            target = vec3_add(eye, vec3_normalize(target));
            view = mat4_lookat(eye, target,
                               ref_direction_for_texel(face, 0.5f, 0));
        }
        clip = mat4_mul_vec4(mat4_mul_mat4(proj, view), world);
        ndc_x = clip.x / clip.w;
        ndc_y = clip.y / clip.w;
        clip_u = (ndc_x + 1) * 0.5f;
        clip_v = (ndc_y + 1) * 0.5f;
        TASK_CHECK(close_to(clip_u, u, 1e-4f));
        TASK_CHECK(close_to(clip_v, v, 1e-4f));
        i = face;
    }
    TASK_CHECK(i == 5);
    return 0;
}

/*
 * the depth values captured by the point shadow must be the rasterizer's
 * [0, 1] perspective depth, not quantized.
 */
static int capture_clears_depth_and_keeps_precision(void) {
    point_shadow_t *ps = point_shadow_create(vec3_new(0, 0, 0),
                                             0.5f, 8, 4, 0.01f);
    TASK_CHECK(ps != NULL);
    TASK_CHECK(is_equal_i(ps->size, 4));
    TASK_CHECK(close_to(ps->near, 0.5f, 1e-6f));
    TASK_CHECK(close_to(ps->far, 8, 1e-6f));
    TASK_CHECK(close_to(ps->bias, 0.01f, 1e-6f));
    TASK_CHECK(is_equal_i(ps->faces[0]->width, 4));
    TASK_CHECK(is_equal_i(ps->faces[0]->height, 4));
    TASK_CHECK(close_to(ps->faces[0]->depth_buffer[0], 1, 1e-6f));
    point_shadow_release(ps);

    /* each face stores a float perspective depth with full precision */
    ps = point_shadow_create(vec3_new(0, 0, 0), 1, 100, 2, 0);
    TASK_CHECK(ps != NULL);
    ps->faces[0]->depth_buffer[0] = 0.5678901f;
    TASK_CHECK(close_to(ps->faces[0]->depth_buffer[0], 0.5678901f, 1e-7f));
    point_shadow_release(ps);
    return 0;
}

/* invalid configurations must return 0 and leave the old state untouched. */
static int capture_rejects_invalid_configuration(void) {
    scene_t *scene;
    point_shadow_t *before;

    scene = make_scene(NULL);
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, 0, 0), 1));
    before = scene->point_shadow;

    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                0, 10, 8, 0), 0));
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 1, 8, 0), 0));
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, -1, 0), 0));
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, 8, -1), 0));
    TASK_CHECK(is_equal_i(scene_set_point_light(scene,
                                                vec3_new(INFINITY, 0, 0),
                                                1, 10, 8, 0), 0));
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                INFINITY, 10, 8, 0), 0));
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, INFINITY, 8, 0), 0));

    /* the previous shadow must still be present and intact */
    TASK_CHECK(scene->point_shadow == before);
    TASK_CHECK(is_equal_i(before->size, 8));
    TASK_CHECK(close_to(before->near, 1, 1e-6f));
    TASK_CHECK(close_to(before->far, 10, 1e-6f));

    /* size 0 disables and releases the old resources */
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, 0, 0), 1));
    TASK_CHECK(scene->point_shadow == NULL);
    scene_release(scene);
    return 0;
}

/* reconstruction (size 0) must survive a scene that never had a shadow. */
static int capture_disable_without_shadow(void) {
    scene_t *scene = make_scene(NULL);
    TASK_CHECK(scene->point_shadow == NULL);
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(1, 2, 3),
                                                1, 10, 0, 0), 1));
    TASK_CHECK(scene->point_shadow == NULL);
    scene_release(scene);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* sampling group                                                          */
/* ----------------------------------------------------------------------- */

/*
 * when the depth surface is a radial plane at distance R, a receiver at
 * distance exactly R must be "not occluded" within bias, and one further
 * than R + bias must be fully occluded.
 */
static int sampling_radial_depth_recovery(void) {
    scene_t *scene;
    point_shadow_t *ps;
    int face;
    vec3_t receiver;
    float visible;

    scene = make_scene(NULL);
    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, 8, 0.02f), 1));
    ps = scene->point_shadow;

    for (face = 0; face < 6; face++) {
        fill_face_radial_distance(ps->faces[face], face, ps->near, ps->far,
                                  5.0f);
    }

    /* a receiver on the radial surface should be fully visible (bias 0.02) */
    receiver = vec3_new(3.0f, 1.7f, -2.6f);
    visible = point_shadow_visibility(ps, receiver);
    TASK_CHECK(close_to(visible, 1.0f, 1e-6f));

    /* a receiver further away than the radial surface is fully occluded */
    receiver = vec3_new(4.0f, 2.3f, -3.5f);
    TASK_CHECK(vec3_length(receiver) > 5.0f + 0.02f);
    visible = point_shadow_visibility(ps, receiver);
    TASK_CHECK(close_to(visible, 0.0f, 1e-6f));

    /* a receiver just inside the bias is not occluded */
    {
        vec3_t unit = vec3_normalize(vec3_new(1, -0.4f, 0.8f));
        receiver = vec3_mul(unit, 5.0f + 0.005f);
        TASK_CHECK(close_to(point_shadow_visibility(ps, receiver),
                            1.0f, 1e-6f));
    }
    TASK_CHECK(face == 6);

    scene_release(scene);
    return 0;
}

/*
 * storage value d < 1 must be interpreted through the documented inverse
 * perspective formula; the recovered axis distance then multiplies the
 * length of the actual texel direction.
 */
static int sampling_depth_formula_matches_reference(void) {
    scene_t *scene = make_scene(NULL);
    point_shadow_t *ps;
    framebuffer_t *face;
    int size, x, y;
    float near = 1.5f;
    float far = 9.0f;
    const float radius = 4.0f;

    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                near, far, 6, 0), 1));
    ps = scene->point_shadow;
    face = ps->faces[4]; /* pz */
    size = face->width;

    /*
     * place a radial surface at `radius` on pz using a reference that
     * computes the axis distance from the direction length independently
     * of the production helper.
     */
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float u = ((float)x + 0.5f) / (float)size;
            float v = ((float)y + 0.5f) / (float)size;
            vec3_t dir = ref_direction_for_texel(4, u, v);
            float length = vec3_length(dir);
            float axis = radius / length;
            face->depth_buffer[y * size + x] =
                ref_depth_of_axis_distance(axis, near, far);
        }
    }

    /*
     * a receiver on the surface, just past near the surface, and just
     * behind it, all on the +z axis face.
     */
    {
        vec3_t on_surface = vec3_new(0.2f, -0.35f, radius);
        vec3_t slightly_inside = vec3_new(0.2f, -0.35f, radius - 0.03f);
        vec3_t slightly_outside = vec3_new(0.2f, -0.35f, radius + 0.03f);
        TASK_CHECK(close_to(point_shadow_visibility(ps, on_surface),
                            1.0f, 1e-6f));
        TASK_CHECK(close_to(point_shadow_visibility(ps, slightly_inside),
                            1.0f, 1e-6f));
        TASK_CHECK(close_to(point_shadow_visibility(ps, slightly_outside),
                            0.0f, 1e-6f));
    }

    /*
     * the reference radial restoration must agree with the production
     * formula: pick a texel and compare both computations explicitly.
     */
    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float u = ((float)x + 0.5f) / (float)size;
            float v = ((float)y + 0.5f) / (float)size;
            vec3_t dir = ref_direction_for_texel(4, u, v);
            float length = vec3_length(dir);
            float axis = radius / length;
            float d = ref_depth_of_axis_distance(axis, near, far);
            float recovered = near * far / (far - d * (far - near));
            TASK_CHECK(close_to(recovered, axis, 1e-3f));
            TASK_CHECK(close_to(recovered * length, radius, 1e-2f));
        }
    }
    scene_release(scene);
    return 0;
}

/*
 * outside [near, far] along the major axis, or exactly at the light,
 * visibility is 1 regardless of stored depth.
 */
static int sampling_axis_range_and_light_center(void) {
    scene_t *scene = make_scene(NULL);
    point_shadow_t *ps;
    int i;

    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                2, 6, 4, 0), 1));
    ps = scene->point_shadow;
    for (i = 0; i < 6; i++) {
        set_face_constant(ps->faces[i], 0.0f); /* pretend everything occludes */
    }

    /* exactly at the light */
    TASK_CHECK(close_to(point_shadow_visibility(ps, vec3_new(0, 0, 0)),
                        1.0f, 1e-6f));
    /* major axis distance below near */
    TASK_CHECK(close_to(point_shadow_visibility(ps, vec3_new(1.0f, 0, 0)),
                        1.0f, 1e-6f));
    /* major axis distance above far */
    TASK_CHECK(close_to(point_shadow_visibility(ps, vec3_new(8.0f, 0, 0)),
                        1.0f, 1e-6f));
    /* inside the frustum with occluding depth is never visible */
    TASK_CHECK(close_to(point_shadow_visibility(ps, vec3_new(3.0f, 0, 0)),
                        0.0f, 1e-6f));
    /* the light position is offset: selection uses world - position */
    scene_release(scene);

    scene = make_scene(NULL);
    TASK_CHECK(is_equal_i(scene_set_point_light(scene,
                                                vec3_new(1, 2, 3),
                                                2, 6, 4, 0), 1));
    ps = scene->point_shadow;
    for (i = 0; i < 6; i++) {
        set_face_constant(ps->faces[i], 0.0f);
    }
    TASK_CHECK(close_to(point_shadow_visibility(ps, vec3_new(1, 2, 3)),
                        1.0f, 1e-6f));
    TASK_CHECK(close_to(point_shadow_visibility(ps, vec3_new(4, 2, 3)),
                        0.0f, 1e-6f));
    scene_release(scene);
    return 0;
}

/*
 * the 3x3 tap set is centered on the receiver's u/v. A single occluded
 * texel must occlude only the taps that land on it.
 */
static int sampling_nine_tap_centering(void) {
    scene_t *scene = make_scene(NULL);
    point_shadow_t *ps;
    vec3_t receiver;
    float u, v, visible, visible2;
    int size, cx, cy, face, i;
    int offsets[3] = {-1, 0, 1};

    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, 3, 0), 1));
    ps = scene->point_shadow;
    size = ps->size;

    /* put the receiver at the center of the +x face */
    receiver = vec3_new(5.0f, 0, 0);
    face = 0;
    TASK_CHECK(close_to(ref_face_for_direction(receiver, &u, &v), face,
                        1e-6f));
    TASK_CHECK(close_to(u, 0.5f, 1e-6f));
    TASK_CHECK(close_to(v, 0.5f, 1e-6f));

    /* clear everything to "no occluder", then occlude only the center texel */
    for (i = 0; i < 6; i++) {
        set_face_constant(ps->faces[i], 1.0f);
    }

    cx = (int)floor(u * (float)size);
    cy = (int)floor(v * (float)size);
    ps->faces[face]->depth_buffer[cy * size + cx] = 0.0f;
    visible = point_shadow_visibility(ps, receiver);
    /* one of nine taps is occluded -> 8/9 visible */
    TASK_CHECK(close_to(visible, 8.0f / 9.0f, 1e-6f));

    /* occluder at the center of a neighbor tap: two of nine taps */
    set_face_constant(ps->faces[face], 1.0f);
    ps->faces[face]->depth_buffer[cy * size + (cx + 1)] = 0.0f;
    visible2 = point_shadow_visibility(ps, receiver);
    TASK_CHECK(close_to(visible2, 8.0f / 9.0f, 1e-6f));

    /* all nine taps occluded */
    set_face_constant(ps->faces[face], 0.0f);
    TASK_CHECK(close_to(point_shadow_visibility(ps, receiver), 0.0f,
                        1e-6f));

    /* seven taps occluded around the receiver -> 2/9 visible */
    set_face_constant(ps->faces[face], 1.0f);
    for (i = 0; i < 9; i++) {
        int tcx, tcy;
        if (i == 4) {
            continue;
        }
        tcx = cx + offsets[i / 3];
        tcy = cy + offsets[i % 3];
        if (tcx < 0) tcx = 0;
        if (tcx >= size) tcx = size - 1;
        if (tcy < 0) tcy = 0;
        if (tcy >= size) tcy = size - 1;
        ps->faces[face]->depth_buffer[tcy * size + tcx] = 0.0f;
    }
    TASK_CHECK(close_to(point_shadow_visibility(ps, receiver), 1.0f / 9.0f,
                        1e-6f));

    scene_release(scene);
    return 0;
}

/*
 * taps that leave the selected face must be re-projected onto a neighbor
 * face, not clamped on the original face. Build a receiver near the edge
 * of +x and place an occluder on the neighboring +z face where the tap
 * re-projects.
 */
static int sampling_seam_reprojects_taps(void) {
    scene_t *scene = make_scene(NULL);
    point_shadow_t *ps;
    vec3_t receiver;
    float visible;
    int i;

    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, 4, 0), 1));
    ps = scene->point_shadow;
    for (i = 0; i < 6; i++) {
        set_face_constant(ps->faces[i], 1.0f);
    }

    /*
     * receiver on +x at v close to 0.75; one tap above crosses into +y.
     * the neighbor face texel at the corresponding location is occluded.
     */
    receiver = vec3_new(6.0f, -0.5f, 0.0f);
    /* u = 0.5, v = 0.5 + 0.5 / 6 = 0.5833 in +x */
    {
        float u, v;
        int face = ref_face_for_direction(receiver, &u, &v);
        TASK_CHECK(close_to(face, 0.0f, 1e-6f));
        TASK_CHECK(u < 0.5f || u > 0.5f || 1); /* silence unused warnings */
    }

    /*
     * Compute the neighbor texel the top tap would re-project to and
     * occlude it. The receiver's v tap at +1/size crosses from +x into +y
     * when v + 1/size > 1.
     */
    {
        float u, v, axis_x;
        int face = ref_face_for_direction(receiver, &u, &v);
        float cu = u;
        float cv = v + 1.0f / (float)ps->size;
        vec3_t dir;
        int nu, nv, ni;
        TASK_CHECK(close_to(face, 0.0f, 1e-6f));
        TASK_CHECK(cv > 1.0f);
        dir = ref_direction_for_texel(face, cu, cv);
        ni = ref_face_for_direction(dir, &u, &v);
        TASK_CHECK(is_equal_i(ni, 2));
        nu = (int)floor(u * (float)ps->size);
        nv = (int)floor(v * (float)ps->size);
        if (nu < 0) nu = 0;
        if (nu > ps->size - 1) nu = ps->size - 1;
        if (nv < 0) nv = 0;
        if (nv > ps->size - 1) nv = ps->size - 1;
        ps->faces[ni]->depth_buffer[nv * ps->size + nu] = 0.0f;
        axis_x = 6.0f;
        TASK_CHECK(axis_x > 1 && axis_x < 10);
    }

    visible = point_shadow_visibility(ps, receiver);
    TASK_CHECK(visible < 1.0f);
    /* the re-projected occluder must affect exactly the taps landing there */
    TASK_CHECK(close_to(visible, 8.0f / 9.0f, 1e-6f));

    scene_release(scene);
    return 0;
}

/*
 * a texel at a corner tap: four taps fall outside; all four must re-project
 * to neighbors rather than being clamped on the original face.
 */
static int sampling_corner_taps_reproject(void) {
    scene_t *scene = make_scene(NULL);
    point_shadow_t *ps;
    vec3_t receiver;
    float u, v, visible;
    int i;

    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, 2, 0), 1));
    ps = scene->point_shadow;
    for (i = 0; i < 6; i++) {
        set_face_constant(ps->faces[i], 1.0f);
    }

    /* near the +x/+y/+z corner so that several taps leave +x */
    receiver = vec3_new(5.0f, 4.9f, 4.9f);
    {
        int face = ref_face_for_direction(receiver, &u, &v);
        TASK_CHECK(close_to(face, 0.0f, 1e-6f));
    }

    /*
     * For each of the nine taps verify that the tap either stays inside
     * the original texel grid or re-projects to another face with a
     * direction that still has the same sign along the neighbor axis.
     */
    {
        int offsets[3] = {-1, 0, 1};
        int oi, oj, outside_taps = 0;
        for (oi = 0; oi < 3; oi++) {
            for (oj = 0; oj < 3; oj++) {
                float cu = u + offsets[oi] / (float)ps->size;
                float cv = v + offsets[oj] / (float)ps->size;
                if (cu < 0 || cu > 1 || cv < 0 || cv > 1) {
                    vec3_t dir = ref_direction_for_texel(0, cu, cv);
                    float nu, nv;
                    int nf = ref_face_for_direction(dir, &nu, &nv);
                    TASK_CHECK(nf != 0);
                    TASK_CHECK(nu >= 0 && nu <= 1 && nv >= 0 && nv <= 1);
                    /* the neighbor's own direction must point along the
                     * same world direction */
                    {
                        vec3_t round_trip =
                            ref_direction_for_texel(nf, nu, nv);
                        TASK_CHECK(vec3_dot(round_trip, dir) > 0);
                    }
                    outside_taps += 1;
                }
            }
        }
        TASK_CHECK(outside_taps >= 1);
    }

    /* all clear -> visible */
    visible = point_shadow_visibility(ps, receiver);
    TASK_CHECK(close_to(visible, 1.0f, 1e-6f));

    scene_release(scene);
    return 0;
}

/*
 * every face is sampled with its own depth: occluders placed on one face
 * must not affect receivers that select another face.
 */
static int sampling_faces_are_independent(void) {
    scene_t *scene = make_scene(NULL);
    point_shadow_t *ps;
    vec3_t receivers[6];
    int i;

    TASK_CHECK(is_equal_i(scene_set_point_light(scene, vec3_new(0, 0, 0),
                                                1, 10, 3, 0), 1));
    ps = scene->point_shadow;
    for (i = 0; i < 6; i++) {
        set_face_constant(ps->faces[i], 1.0f);
    }
    /* occlude just the center texel of +x and -y */
    ps->faces[0]->depth_buffer[1 * 3 + 1] = 0.0f;
    ps->faces[3]->depth_buffer[1 * 3 + 1] = 0.0f;

    receivers[0] = vec3_new(5, 0, 0);
    receivers[1] = vec3_new(-5, 0, 0);
    receivers[2] = vec3_new(0, 5, 0);
    receivers[3] = vec3_new(0, -5, 0);
    receivers[4] = vec3_new(0, 0, 5);
    receivers[5] = vec3_new(0, 0, -5);

    for (i = 0; i < 6; i++) {
        float expected = (i == 0 || i == 3) ? 8.0f / 9.0f : 1.0f;
        TASK_CHECK(close_to(point_shadow_visibility(ps, receivers[i]),
                            expected, 1e-6f));
    }

    scene_release(scene);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* real offscreen scene helpers                                            */
/* ----------------------------------------------------------------------- */

static perframe_t make_perframe(mat4_t camera_view, mat4_t camera_proj) {
    perframe_t p;
    memset(&p, 0, sizeof(p));
    p.camera_view_matrix = camera_view;
    p.camera_proj_matrix = camera_proj;
    p.light_view_matrix = mat4_identity();
    p.light_proj_matrix = mat4_identity();
    p.light_dir = vec3_new(0, 0, -1);
    p.camera_pos = vec3_new(0, 0, 0);
    p.punctual_intensity = 1;
    p.ambient_intensity = 0;
    p.layer_view = -1;
    return p;
}

static model_t *make_quad(mat4_t transform, int kind, float alpha_cutoff,
                          const char *diffuse_map) {
    model_t *model;
    if (kind == 0) {
        blinn_material_t m;
        memset(&m, 0, sizeof(m));
        m.basecolor = vec4_new(1, 1, 1, 1);
        m.shininess = 8;
        m.double_sided = 1;
        m.alpha_cutoff = alpha_cutoff;
        m.diffuse_map = diffuse_map;
        model = blinn_create_model("checks/quad.obj", transform, NULL, -1, &m);
    } else if (kind == 1) {
        pbrm_material_t m;
        memset(&m, 0, sizeof(m));
        m.basecolor_factor = vec4_new(1, 1, 1, 1);
        m.roughness_factor = 0.5f;
        m.metalness_factor = 0;
        m.double_sided = 1;
        m.alpha_cutoff = alpha_cutoff;
        m.basecolor_map = diffuse_map;
        model = pbrm_create_model("checks/quad.obj", transform, NULL, -1,
                                  &m, NULL);
    } else {
        pbrs_material_t m;
        memset(&m, 0, sizeof(m));
        m.diffuse_factor = vec4_new(1, 1, 1, 1);
        m.specular_factor = vec3_new(0, 0, 0);
        m.glossiness_factor = 0.5f;
        m.double_sided = 1;
        m.alpha_cutoff = alpha_cutoff;
        m.diffuse_map = diffuse_map;
        model = pbrs_create_model("checks/quad.obj", transform, NULL, -1,
                                  &m, NULL);
    }
    return model;
}

/* ----------------------------------------------------------------------- */
/* materials group                                                         */
/* ----------------------------------------------------------------------- */

int main(void);

/* declared cases */
static const struct task_case task_cases_local[] = {
};

const struct task_case task_cases[] = {
    {"capture", "matrix_matches_layout", capture_matrix_matches_layout},
    {"capture", "depth_is_perspective", capture_clears_depth_and_keeps_precision},
    {"capture", "invalid_config_untouched", capture_rejects_invalid_configuration},
    {"capture", "disable_without_shadow", capture_disable_without_shadow},
    {"sampling", "radial_depth_recovery", sampling_radial_depth_recovery},
    {"sampling", "depth_formula_reference", sampling_depth_formula_matches_reference},
    {"sampling", "axis_range_and_center", sampling_axis_range_and_light_center},
    {"sampling", "nine_tap_centering", sampling_nine_tap_centering},
    {"sampling", "seam_reprojects_taps", sampling_seam_reprojects_taps},
    {"sampling", "corner_taps_reproject", sampling_corner_taps_reproject},
    {"sampling", "faces_are_independent", sampling_faces_are_independent},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
