/*
 * headless regression checks for the tiled detail normal layer of the pbr
 * shader. built and driven by regress/check_detail_normals.sh, which supplies
 * a scratch working directory as the only command line argument and creates
 * the <work>/detail directory ahead of this program. every mesh, texture and
 * scene file used here is generated into that directory, so the test never
 * opens a window and never touches the repository's large model assets.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "renderer/core/api.h"
#include "renderer/core/private.h"
#include "renderer/scenes/scene_helper.h"
#include "renderer/shaders/cache_helper.h"
#include "renderer/shaders/pbr_shader.h"

#define FB_WIDTH 64
#define FB_HEIGHT 64
#define TOL_TIGHT 1e-4f
#define TOL_BYTE 2e-2f

typedef unsigned char (*pixel_func_t)(int c, int r, int channel);

static int g_failures = 0;

static int vec3_near(vec3_t got, vec3_t expected, float tol) {
    return fabs(got.x - expected.x) <= tol &&
           fabs(got.y - expected.y) <= tol &&
           fabs(got.z - expected.z) <= tol;
}

static int vec3_all_finite(vec3_t v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static void fail(const char *label, const char *extra) {
    printf("FAIL %s%s\n", label, extra);
    g_failures += 1;
}

static void check_vec3(const char *label, vec3_t got, vec3_t expected,
                       float tol) {
    char extra[160];
    if (!vec3_all_finite(got) || !vec3_near(got, expected, tol)) {
        sprintf(extra, ": got (%.4f %.4f %.4f), expected (%.4f %.4f %.4f)",
                got.x, got.y, got.z, expected.x, expected.y, expected.z);
        fail(label, extra);
    }
}

static void check_int(const char *label, int got, int expected) {
    char extra[64];
    if (got != expected) {
        sprintf(extra, ": got %d, expected %d", got, expected);
        fail(label, extra);
    }
}

static vec3_t preview_rgb(vec3_t normal) {
    return vec3_new(normal.x * 0.5f + 0.5f,
                    normal.y * 0.5f + 0.5f,
                    normal.z * 0.5f + 0.5f);
}

/* ------------------------------------------------------------------ */
/* tiny in-memory textures (linear data, no color space conversion)   */
/* ------------------------------------------------------------------ */

static void put_texel(texture_t *texture, int c, int r, vec4_t texel) {
    texture->buffer[r * texture->width + c] = texel;
}

static vec4_t normal_texel(vec3_t normal) {
    return vec4_new(normal.x * 0.5f + 0.5f,
                    normal.y * 0.5f + 0.5f,
                    normal.z * 0.5f + 0.5f, 1);
}

/* ------------------------------------------------------------------ */
/* direct fragment shader calls                                       */
/* ------------------------------------------------------------------ */

static pbr_varyings_t identity_varyings(void) {
    pbr_varyings_t v;
    memset(&v, 0, sizeof(v));
    v.clip_position = vec4_new(0, 0, 0, 1);
    v.world_tangent = vec3_new(1, 0, 0);
    v.world_bitangent = vec3_new(0, 1, 0);
    v.world_normal = vec3_new(0, 0, 1);
    v.texcoord = vec2_new(0, 0);
    return v;
}

static pbr_uniforms_t base_uniforms(void) {
    pbr_uniforms_t u;
    memset(&u, 0, sizeof(u));
    u.workflow = METALNESS_WORKFLOW;
    u.basecolor_factor = vec4_new(1, 1, 1, 1);
    u.roughness_factor = 0.5f;
    u.layer_view = 5;
    u.detail_normal_strength = 1;
    u.detail_normal_tiling = vec2_new(1, 1);
    u.detail_normal_offset = vec2_new(0, 0);
    return u;
}

static vec4_t run_fragment(pbr_uniforms_t *u, pbr_varyings_t *v,
                           int backface) {
    int discard = 0;
    return pbr_fragment_shader(v, u, &discard, backface);
}

static void check_fragment(const char *label, pbr_uniforms_t *u,
                           pbr_varyings_t *v, int backface,
                           vec3_t expected_normal, float tol) {
    vec4_t color = run_fragment(u, v, backface);
    vec3_t rgb = vec3_from_vec4(color);
    check_vec3(label, rgb, preview_rgb(expected_normal), tol);
}

/* the reference normals straight from the task description */
static const vec3_t BASE_NORMAL = {0.6f, 0.0f, 0.8f};
static const vec3_t DETAIL_NORMAL = {0.0f, 0.8f, 0.6f};
static const vec3_t COMPOSITE_NORMAL = {0.36f, 0.8f, 0.48f};
static const vec3_t FLAT_NORMAL = {0.0f, 0.0f, 1.0f};

static void test_constant_composite(void) {
    pbr_uniforms_t u = base_uniforms();
    pbr_varyings_t v = identity_varyings();

    u.normal_map = texture_create(1, 1);
    put_texel(u.normal_map, 0, 0, normal_texel(BASE_NORMAL));
    u.detail_normal_map = texture_create(1, 1);
    put_texel(u.detail_normal_map, 0, 0, normal_texel(DETAIL_NORMAL));

    check_fragment("constant composite normal", &u, &v, 0,
                   COMPOSITE_NORMAL, TOL_TIGHT);
    check_vec3("constant preview rgb",
               vec3_from_vec4(run_fragment(&u, &v, 0)),
               vec3_new(0.68f, 0.9f, 0.74f), TOL_TIGHT);

    texture_release(u.normal_map);
    texture_release(u.detail_normal_map);
}

static void test_zero_strength(void) {
    pbr_uniforms_t u = base_uniforms();
    pbr_varyings_t v = identity_varyings();

    u.normal_map = texture_create(1, 1);
    put_texel(u.normal_map, 0, 0, normal_texel(BASE_NORMAL));
    u.detail_normal_map = texture_create(1, 1);
    put_texel(u.detail_normal_map, 0, 0, normal_texel(DETAIL_NORMAL));
    u.detail_normal_strength = 0;

    /* strength zero makes the detail layer flat, leaving the base normal */
    check_fragment("zero strength", &u, &v, 0, BASE_NORMAL, TOL_TIGHT);

    texture_release(u.normal_map);
    texture_release(u.detail_normal_map);
}

static void test_detail_only_and_tangent_frame(void) {
    pbr_uniforms_t u = base_uniforms();
    pbr_varyings_t v = identity_varyings();
    pbr_attribs_t attribs;
    pbr_varyings_t generated;
    vec4_t clip;

    u.detail_normal_map = texture_create(1, 1);
    put_texel(u.detail_normal_map, 0, 0, normal_texel(DETAIL_NORMAL));

    /* with no base map the detail normal alone must survive */
    check_fragment("detail only", &u, &v, 0, DETAIL_NORMAL, TOL_TIGHT);

    /*
     * the vertex shader still has to build the tangent frame when only a
     * detail normal map is configured
     */
    u.model_matrix = mat4_identity();
    u.normal_matrix = mat3_identity();
    u.camera_vp_matrix = mat4_identity();
    memset(&attribs, 0, sizeof(attribs));
    attribs.position = vec3_new(0, 0, 0);
    attribs.normal = vec3_new(0, 0, 1);
    attribs.tangent = vec4_new(1, 0, 0, 1);
    memset(&generated, 0, sizeof(generated));
    clip = pbr_vertex_shader(&attribs, &generated, &u);
    check_vec3("detail-only tangent", generated.world_tangent,
               vec3_new(1, 0, 0), TOL_TIGHT);
    check_vec3("detail-only bitangent", generated.world_bitangent,
               vec3_new(0, 1, 0), TOL_TIGHT);
    check_vec3("detail-only normal", generated.world_normal,
               vec3_new(0, 0, 1), TOL_TIGHT);
    check_vec3("detail-only clip position", vec3_from_vec4(clip),
               vec3_new(0, 0, 0), TOL_TIGHT);
    if (fabs(clip.w - 1) > TOL_TIGHT) {
        fail("detail-only clip w", "");
    }

    texture_release(u.detail_normal_map);
}

static void test_uv_density(void) {
    pbr_uniforms_t u = base_uniforms();
    pbr_varyings_t v = identity_varyings();
    texture_t *detail;
    vec3_t side_normal = vec3_new(-0.6f, 0, 0.8f);

    u.normal_map = texture_create(1, 1);
    put_texel(u.normal_map, 0, 0, normal_texel(FLAT_NORMAL));

    detail = texture_create(4, 4);
    put_texel(detail, 0, 0, normal_texel(FLAT_NORMAL));
    put_texel(detail, 1, 1, normal_texel(DETAIL_NORMAL));
    put_texel(detail, 2, 0, normal_texel(side_normal));
    u.detail_normal_map = detail;

    /* tiling 2, offset 0: uv (0, 0) lands on texel (0, 0) -> flat */
    u.detail_normal_tiling = vec2_new(2, 2);
    u.detail_normal_offset = vec2_new(0, 0);
    v.texcoord = vec2_new(0, 0);
    check_fragment("uv tiling 2 offset 0", &u, &v, 0,
                   FLAT_NORMAL, TOL_TIGHT);

    /* offset (0.5, 0.5) shifts the sample onto texel (1, 1) */
    u.detail_normal_offset = vec2_new(0.5f, 0.5f);
    check_fragment("uv tiling 2 offset 0.5", &u, &v, 0,
                   DETAIL_NORMAL, TOL_TIGHT);

    /*
     * tiling (4, 1) at uv (0.7, 0): the base layer still samples its own 1x1
     * map while the detail layer lands on texel (2, 0)
     */
    u.detail_normal_tiling = vec2_new(4, 1);
    u.detail_normal_offset = vec2_new(0, 0);
    v.texcoord = vec2_new(0.7f, 0);
    check_fragment("uv tiling 4", &u, &v, 0, side_normal, TOL_TIGHT);

    /* repeat wrapping matches texture_sample for coordinates past one */
    u.detail_normal_tiling = vec2_new(1, 1);
    u.detail_normal_offset = vec2_new(1.5f, 2.5f);
    v.texcoord = vec2_new(0, 0);
    check_fragment("uv repeat wrap", &u, &v, 0,
                   DETAIL_NORMAL, TOL_TIGHT);

    texture_release(u.normal_map);
    texture_release(detail);
}

static void test_mirrored_handedness(void) {
    pbr_uniforms_t u = base_uniforms();
    pbr_varyings_t v = identity_varyings();
    vec3_t expected;

    u.normal_map = texture_create(1, 1);
    put_texel(u.normal_map, 0, 0, normal_texel(BASE_NORMAL));
    u.detail_normal_map = texture_create(1, 1);
    put_texel(u.detail_normal_map, 0, 0, normal_texel(DETAIL_NORMAL));

    /* mirror the bitangent, as a negative tangent.w mesh would produce */
    v.world_bitangent = vec3_new(0, -1, 0);
    expected = vec3_new(COMPOSITE_NORMAL.x, -COMPOSITE_NORMAL.y,
                        COMPOSITE_NORMAL.z);
    check_fragment("mirrored tangent handedness", &u, &v, 0,
                   expected, TOL_TIGHT);

    texture_release(u.normal_map);
    texture_release(u.detail_normal_map);
}

static void test_backface(void) {
    pbr_uniforms_t u = base_uniforms();
    pbr_varyings_t v = identity_varyings();
    vec3_t expected = vec3_negate(COMPOSITE_NORMAL);

    u.normal_map = texture_create(1, 1);
    put_texel(u.normal_map, 0, 0, normal_texel(BASE_NORMAL));
    u.detail_normal_map = texture_create(1, 1);
    put_texel(u.detail_normal_map, 0, 0, normal_texel(DETAIL_NORMAL));

    /* the whole composited normal flips, not just the base layer */
    check_fragment("backface composite", &u, &v, 1, expected, TOL_TIGHT);

    texture_release(u.normal_map);
    texture_release(u.detail_normal_map);
}

static void test_invalid_texels(void) {
    pbr_uniforms_t u = base_uniforms();
    pbr_varyings_t v = identity_varyings();

    u.normal_map = texture_create(1, 1);
    put_texel(u.normal_map, 0, 0, normal_texel(BASE_NORMAL));
    u.detail_normal_map = texture_create(1, 1);

    /* a zero-length decoded normal falls back to +Z */
    put_texel(u.detail_normal_map, 0, 0, vec4_new(0.5f, 0.5f, 0.5f, 1));
    check_fragment("zero-length detail falls back to flat", &u, &v, 0,
                   BASE_NORMAL, TOL_TIGHT);

    /* a negative-z decoded normal is rejected as well */
    put_texel(u.detail_normal_map, 0, 0, vec4_new(0.5f, 0.5f, 0, 1));
    check_fragment("negative-z detail falls back to flat", &u, &v, 0,
                   BASE_NORMAL, TOL_TIGHT);

    texture_release(u.normal_map);
    texture_release(u.detail_normal_map);
}

/* ------------------------------------------------------------------ */
/* generated on-disk assets for the real scene entry point            */
/* ------------------------------------------------------------------ */

static void write_tga(const char *path, int width, int height,
                      pixel_func_t pixel_func) {
    image_t *image = image_create(width, height, 3, FORMAT_LDR);
    int r, c, k;
    for (r = 0; r < height; r++) {
        for (c = 0; c < width; c++) {
            int index = (r * width + c) * 3;
            for (k = 0; k < 3; k++) {
                image->ldr_buffer[index + k] = pixel_func(c, r, k);
            }
        }
    }
    image_save(image, path);
    image_release(image);
}

/* 4x4 detail texture: only texel (1, 1) carries the detail normal */
static unsigned char density_pixel(int c, int r, int channel) {
    if (c == 1 && r == 1) {
        if (channel == 0) {
            return 128;                          /* x ~= 0 */
        }
        return channel == 1 ? 229 : 204;        /* y ~= .8, z ~= .6 */
    }
    return channel == 2 ? 255 : 128;            /* flat (0, 0, 1) */
}

static unsigned char flat_pixel(int c, int r, int channel) {
    (void)c;
    (void)r;
    return channel == 2 ? 255 : 128;
}

static void write_plane_obj(const char *path) {
    FILE *file = fopen(path, "wb");
    int i;
    assert(file != NULL);
    fputs("v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n", file);
    fputs("vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n", file);
    fputs("vn 0 0 1\n", file);
    for (i = 0; i < 4; i++) {
        fputs("# ext.tangent 1 0 0 1\n", file);
    }
    fputs("f 1/1/1 2/2/1 3/3/1\n", file);
    fputs("f 1/1/1 3/3/1 4/4/1\n", file);
    fclose(file);
}

static void write_lighting(FILE *file, const char *type) {
    fprintf(file, "type: %s\n\n", type);
    fputs("lighting:\n", file);
    fputs("    background: 0 0 0\n", file);
    fputs("    environment: null\n", file);
    fputs("    skybox: off\n", file);
    fputs("    shadow: off\n", file);
    fputs("    ambient: 0\n", file);
    fputs("    punctual: 0\n\n", file);
}

static void write_identity_transform(FILE *file, int index,
                                     float tx, float ty) {
    fprintf(file, "    transform %d:\n", index);
    fprintf(file, "        0.5 0 0 %g\n", tx);
    fprintf(file, "        0 0.5 0 %g\n", ty);
    fputs("        0 0 1 0\n", file);
    fputs("        0 0 0 1\n", file);
}

static void write_model_line(FILE *file, int index) {
    fprintf(file, "    model %d:\n", index);
    fputs("        mesh: detail/plane.obj\n", file);
    fputs("        skeleton: null\n", file);
    fputs("        attached: -1\n", file);
    fprintf(file, "        material: %d\n", index);
    fprintf(file, "        transform: %d\n", index);
}

static void write_pbrm_material(FILE *file, int index, int with_block,
                                int null_path, float tiling) {
    fprintf(file, "    material %d:\n", index);
    fputs("        basecolor_factor: 1 1 1 1\n", file);
    fputs("        metalness_factor: 0\n", file);
    fputs("        roughness_factor: 0.5\n", file);
    fputs("        basecolor_map: null\n", file);
    fputs("        metalness_map: null\n", file);
    fputs("        roughness_map: null\n", file);
    fputs("        normal_map: null\n", file);
    if (with_block) {
        fputs("        detail_normal_map: ", file);
        fputs(null_path ? "null\n" : "detail/detail.tga\n", file);
        fputs("        detail_normal_strength: 1\n", file);
        fprintf(file, "        detail_normal_tiling: %g %g\n",
                tiling, tiling);
        fputs("        detail_normal_offset: 0 0\n", file);
    }
    fputs("        occlusion_map: null\n", file);
    fputs("        emission_map: null\n", file);
    fputs("        double_sided: off\n", file);
    fputs("        enable_blend: off\n", file);
    fputs("        alpha_cutoff: 0\n", file);
}

static void write_pbrs_material(FILE *file, int index) {
    fprintf(file, "    material %d:\n", index);
    fputs("        diffuse_factor: 0.8 0.8 0.8 1\n", file);
    fputs("        specular_factor: 0.04 0.04 0.04\n", file);
    fputs("        glossiness_factor: 0.5\n", file);
    fputs("        diffuse_map: null\n", file);
    fputs("        specular_map: null\n", file);
    fputs("        glossiness_map: null\n", file);
    fputs("        normal_map: null\n", file);
    fputs("        detail_normal_map: null\n", file);
    fputs("        detail_normal_strength: 1\n", file);
    fputs("        detail_normal_tiling: 1 1\n", file);
    fputs("        detail_normal_offset: 0 0\n", file);
    fputs("        occlusion_map: null\n", file);
    fputs("        emission_map: null\n", file);
    fputs("        double_sided: off\n", file);
    fputs("        enable_blend: off\n", file);
    fputs("        alpha_cutoff: 0\n", file);
}

static const float QUADRANT_X[4] = {-1, 1, -1, 1};
static const float QUADRANT_Y[4] = {-1, -1, 1, 1};
static const float TILINGS[4] = {1, 2, 3, 4};
static const int QUADRANT_PIXEL_X[4] = {16, 48, 16, 48};
static const int QUADRANT_PIXEL_Y[4] = {16, 16, 48, 48};

static void write_density_scene(const char *path) {
    FILE *file = fopen(path, "wb");
    int i;
    assert(file != NULL);
    write_lighting(file, "pbrm");
    fputs("materials 4:\n", file);
    for (i = 0; i < 4; i++) {
        write_pbrm_material(file, i, 1, 0, TILINGS[i]);
    }
    fputs("\ntransforms 4:\n", file);
    for (i = 0; i < 4; i++) {
        write_identity_transform(file, i, QUADRANT_X[i], QUADRANT_Y[i]);
    }
    fputs("\nmodels 4:\n", file);
    for (i = 0; i < 4; i++) {
        write_model_line(file, i);
    }
    fclose(file);
}

static void write_single_material_scene(const char *path, const char *type,
                                        int with_block, int null_path) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    write_lighting(file, type);
    fputs("materials 1:\n", file);
    if (strcmp(type, "pbrs") == 0) {
        write_pbrs_material(file, 0);
    } else {
        write_pbrm_material(file, 0, with_block, null_path, 1);
    }
    fputs("\ntransforms 1:\n", file);
    write_identity_transform(file, 0, 0, 0);
    fputs("\nmodels 1:\n", file);
    write_model_line(file, 0);
    fclose(file);
}

/* ------------------------------------------------------------------ */
/* scene-level rendering and cache checks                             */
/* ------------------------------------------------------------------ */

static vec3_t read_pixel(framebuffer_t *framebuffer, int x, int y) {
    int index = (y * framebuffer->width + x) * 4;
    return vec3_new(float_from_uchar(framebuffer->color_buffer[index + 0]),
                    float_from_uchar(framebuffer->color_buffer[index + 1]),
                    float_from_uchar(framebuffer->color_buffer[index + 2]));
}

static perframe_t make_perframe(camera_t *camera, int layer_view) {
    perframe_t perframe;
    memset(&perframe, 0, sizeof(perframe));
    perframe.light_dir = vec3_new(0, 0, -1);
    perframe.camera_pos = camera_get_position(camera);
    perframe.light_view_matrix = mat4_identity();
    perframe.light_proj_matrix = mat4_identity();
    perframe.camera_view_matrix = camera_get_view_matrix(camera);
    perframe.camera_proj_matrix = camera_get_proj_matrix(camera);
    perframe.ambient_intensity = 0;
    perframe.punctual_intensity = 0;
    perframe.shadow_map = NULL;
    perframe.layer_view = layer_view;
    return perframe;
}

static framebuffer_t *render_models(model_t **models, int num_models,
                                    int layer_view) {
    framebuffer_t *framebuffer = framebuffer_create(FB_WIDTH, FB_HEIGHT);
    camera_t *camera = camera_create(vec3_new(0, 0, 3), vec3_new(0, 0, 0), 1);
    perframe_t perframe = make_perframe(camera, layer_view);
    int i;

    framebuffer_clear_color(framebuffer, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(framebuffer, 1);
    for (i = 0; i < num_models; i++) {
        models[i]->update(models[i], &perframe);
        models[i]->draw(models[i], framebuffer, 0);
    }
    camera_release(camera);
    return framebuffer;
}

static framebuffer_t *render_scene(scene_t *scene, int layer_view) {
    return render_models(scene->models, darray_size(scene->models),
                         layer_view);
}

static framebuffer_t *render_model(model_t *model, int layer_view) {
    model_t *models[1];
    models[0] = model;
    return render_models(models, 1, layer_view);
}

static void save_framebuffer(framebuffer_t *framebuffer, const char *path) {
    image_t *image = image_create(FB_WIDTH, FB_HEIGHT, 4, FORMAT_LDR);
    private_blit_rgb(framebuffer, image);
    image_save(image, path);
    image_release(image);
}

static void test_scene_density(const char *work_dir) {
    char path[256];
    char label[256];
    scene_t *scene;
    framebuffer_t *framebuffer;
    vec3_t flat_rgb = preview_rgb(FLAT_NORMAL);
    vec3_t detail_rgb = preview_rgb(DETAIL_NORMAL);
    /*
     * the exact texel math for tiling/offset/repeat is covered against known
     * texels by test_uv_density; through the full scene path we instead prove
     * that changing the tiling density changes which detail normal is sampled:
     * every quadrant center must be one of the two authored normal colors, and
     * the four densities must exercise both of them
     */
    int saw_flat = 0, saw_detail = 0, i;

    sprintf(path, "%s/detail/density.scn", work_dir);
    scene = scene_from_file(path, mat4_identity());
    framebuffer = render_scene(scene, 5);

    for (i = 0; i < 4; i++) {
        vec3_t got = read_pixel(framebuffer,
                                QUADRANT_PIXEL_X[i], QUADRANT_PIXEL_Y[i]);
        int is_flat = vec3_near(got, flat_rgb, TOL_BYTE);
        int is_detail = vec3_near(got, detail_rgb, TOL_BYTE);

        sprintf(label, "density tiling %g valid normal (material %d, "
                "pixel %d,%d)", TILINGS[i], i,
                QUADRANT_PIXEL_X[i], QUADRANT_PIXEL_Y[i]);
        check_int(label, is_flat || is_detail, 1);
        if (is_flat) {
            saw_flat = 1;
        }
        if (is_detail) {
            saw_detail = 1;
        }
    }

    check_int("density tilings produce both flat and detail normals",
              saw_flat && saw_detail, 1);

    sprintf(path, "%s/detail_density.tga", work_dir);
    save_framebuffer(framebuffer, path);
    printf("density comparison image: %s\n", path);

    framebuffer_release(framebuffer);
    scene_release(scene);
}

static void test_optional_blocks(void) {
    scene_t *scene;
    framebuffer_t *framebuffer;
    vec3_t center;
    vec3_t flat_rgb = preview_rgb(FLAT_NORMAL);

    /* pbrs workflow with a complete block whose path is null */
    scene = scene_from_file("detail/pbrs_null.scn", mat4_identity());
    framebuffer = render_scene(scene, 5);
    center = read_pixel(framebuffer, 32, 32);
    check_vec3("pbrs null detail block (pixel 32,32)", center,
               flat_rgb, TOL_BYTE);
    framebuffer_release(framebuffer);
    scene_release(scene);

    /* legacy pbrm material without any detail block keeps working */
    scene = scene_from_file("detail/pbrm_legacy.scn", mat4_identity());
    framebuffer = render_scene(scene, 5);
    center = read_pixel(framebuffer, 32, 32);
    check_vec3("legacy pbrm without block (pixel 32,32)", center,
               flat_rgb, TOL_BYTE);
    framebuffer_release(framebuffer);
    scene_release(scene);
}

static pbrm_material_t plain_pbrm_material(void) {
    pbrm_material_t material;
    memset(&material, 0, sizeof(material));
    material.basecolor_factor = vec4_new(1, 1, 1, 1);
    material.roughness_factor = 0.5f;
    return material;
}

static pbrs_material_t plain_pbrs_material(void) {
    pbrs_material_t material;
    memset(&material, 0, sizeof(material));
    material.diffuse_factor = vec4_new(0.8f, 0.8f, 0.8f, 1);
    material.specular_factor = vec3_new(0.04f, 0.04f, 0.04f);
    material.glossiness_factor = 0.5f;
    return material;
}

static void test_high_level_api_and_cache(void) {
    pbrm_material_t pbrm = plain_pbrm_material();
    pbrs_material_t pbrs = plain_pbrs_material();
    model_t *model_a, *model_b, *model_c;
    framebuffer_t *framebuffer;
    vec3_t center, detail_rgb = preview_rgb(DETAIL_NORMAL);
    vec3_t flat_rgb = preview_rgb(FLAT_NORMAL);
    int accepted;
    float nan_value = 0.0f / 0.0f;
    float infinity_value = 1.0f / 0.0f;

    model_a = pbrm_create_model("detail/plane.obj", mat4_identity(),
                                NULL, -1, &pbrm, NULL);
    model_b = pbrm_create_model("detail/plane.obj", mat4_identity(),
                                NULL, -1, &pbrm, NULL);
    model_c = pbrs_create_model("detail/plane.obj", mat4_identity(),
                                NULL, -1, &pbrs, NULL);

    /* both workflows accept the same call, and the texture is shared */
    accepted = pbr_set_detail_normal(model_a, "detail/detail.tga", 1,
                                     vec2_new(1, 1), vec2_new(0, 0));
    check_int("pbrm set detail accepted", accepted, 1);
    accepted = pbr_set_detail_normal(model_b, "detail/detail.tga", 1,
                                     vec2_new(1, 1), vec2_new(0, 0));
    check_int("shared texture set accepted", accepted, 1);
    accepted = pbr_set_detail_normal(model_c, "detail/detail.tga", 1,
                                     vec2_new(1, 1), vec2_new(0, 0));
    check_int("pbrs set detail accepted", accepted, 1);

    /* invalid configurations are rejected at configuration time */
    check_int("negative strength rejected",
              pbr_set_detail_normal(model_a, "detail/detail.tga", -1,
                                    vec2_new(1, 1), vec2_new(0, 0)), 0);
    check_int("infinite strength rejected",
              pbr_set_detail_normal(model_a, "detail/detail.tga",
                                    infinity_value,
                                    vec2_new(1, 1), vec2_new(0, 0)), 0);
    check_int("nan tiling rejected",
              pbr_set_detail_normal(model_a, "detail/detail.tga", 1,
                                    vec2_new(nan_value, 1), vec2_new(0, 0)),
              0);
    check_int("infinite offset rejected",
              pbr_set_detail_normal(model_a, "detail/detail.tga", 1,
                                    vec2_new(1, 1),
                                    vec2_new(infinity_value, 0)), 0);

    /* the rejected calls left the old configuration drawable */
    framebuffer = render_model(model_a, 5);
    center = read_pixel(framebuffer, 32, 32);
    check_vec3("old config still draws after rejection (pixel 32,32)",
               center, detail_rgb, TOL_BYTE);
    framebuffer_release(framebuffer);

    /* swapping the texture releases only this model's reference */
    accepted = pbr_set_detail_normal(model_a, "detail/detail2.tga", 1,
                                     vec2_new(1, 1), vec2_new(0, 0));
    check_int("texture replacement accepted", accepted, 1);
    framebuffer = render_model(model_b, 5);
    center = read_pixel(framebuffer, 32, 32);
    check_vec3("shared texture survives replacement (pixel 32,32)",
               center, detail_rgb, TOL_BYTE);
    framebuffer_release(framebuffer);

    /* an empty path disables the layer */
    accepted = pbr_set_detail_normal(model_a, "", 1,
                                     vec2_new(1, 1), vec2_new(0, 0));
    check_int("empty path disables detail", accepted, 1);
    framebuffer = render_model(model_a, 5);
    center = read_pixel(framebuffer, 32, 32);
    check_vec3("disabled detail renders flat (pixel 32,32)", center,
               flat_rgb, TOL_BYTE);
    framebuffer_release(framebuffer);

    /* the other two models still hold the shared texture */
    framebuffer = render_model(model_c, 5);
    center = read_pixel(framebuffer, 32, 32);
    check_vec3("pbrs shared texture still draws (pixel 32,32)", center,
               detail_rgb, TOL_BYTE);
    framebuffer_release(framebuffer);

    /* balanced release: cache_cleanup below asserts every ref hit zero */
    model_a->release(model_a);
    model_b->release(model_b);
    model_c->release(model_c);
}

int main(int argc, char **argv) {
    const char *work_dir;
    char path[256];

    if (argc != 2) {
        printf("usage: %s <working-directory>\n", argv[0]);
        return 2;
    }
    work_dir = argv[1];

    /* direct shader math */
    test_constant_composite();
    test_zero_strength();
    test_detail_only_and_tangent_frame();
    test_uv_density();
    test_mirrored_handedness();
    test_backface();
    test_invalid_texels();

    /* generated assets exercised through the real scene/model entry points */
    sprintf(path, "%s/detail/plane.obj", work_dir);
    write_plane_obj(path);

    sprintf(path, "%s/detail/detail.tga", work_dir);
    write_tga(path, 4, 4, density_pixel);
    sprintf(path, "%s/detail/detail2.tga", work_dir);
    write_tga(path, 4, 4, flat_pixel);

    sprintf(path, "%s/detail/density.scn", work_dir);
    write_density_scene(path);
    sprintf(path, "%s/detail/pbrs_null.scn", work_dir);
    write_single_material_scene(path, "pbrs", 1, 1);
    sprintf(path, "%s/detail/pbrm_legacy.scn", work_dir);
    write_single_material_scene(path, "pbrm", 0, 0);

    test_scene_density(work_dir);
    test_optional_blocks();
    test_high_level_api_and_cache();

    cache_cleanup();

    if (g_failures != 0) {
        printf("%d detail normal check(s) failed\n", g_failures);
        return 1;
    }
    printf("all detail normal checks passed\n");
    return 0;
}
