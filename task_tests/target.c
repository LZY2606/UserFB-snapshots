#include "checks.h"
#include "renderer/core/api.h"
#include "renderer/core/decal.h"
#include "renderer/shaders/pbr_shader.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int feq(float a, float b) {
    return fabsf(a - b) < 1e-4f;
}

static int v3eq(vec3_t a, vec3_t b) {
    return feq(a.x, b.x) && feq(a.y, b.y) && feq(a.z, b.z);
}

static texture_t *solid_texture(int width, int height, vec4_t texel) {
    texture_t *texture = texture_create(width, height);
    int i;
    for (i = 0; i < width * height; i++) {
        texture->buffer[i] = texel;
    }
    return texture;
}

/* applies the collection to a probe point and reports the painted color */
static vec3_t painted_color(decal_collection_t *collection, vec3_t point,
                            vec3_t base_normal) {
    vec3_t basecolor = vec3_new(0.2f, 0.3f, 0.4f);
    vec3_t normal = vec3_new(0, 0, 1);
    float roughness = 0.2f;
    float metalness = 0.3f;
    decal_collection_apply(collection, point, base_normal, &basecolor,
                           &roughness, &metalness, &normal);
    return basecolor;
}

static int is_painted(decal_collection_t *collection, vec3_t point,
                      vec3_t base_normal) {
    vec3_t color = painted_color(collection, point, base_normal);
    return v3eq(color, vec3_new(1, 0, 0));
}

void test_projection(Check *c) {
    texture_t *red = solid_texture(2, 2, vec4_new(1, 0, 0, 1));
    quat_t rot90y = quat_new(0, 0.70710678f, 0, 0.70710678f);
    decal_collection_t *collection;
    mat4_t transform;
    int rc;

    /* identity box: the closed box [-.5,.5]^3 around the origin */
    collection = decal_collection_create();
    rc = decal_collection_add(collection, mat4_identity(), red, NULL,
                              0.8f, 0.1f, -1.0f);
    check(c, rc == 0, "identity transform must be accepted");
    check(c, collection->num_decals == 1, "one decal after one add");
    check(c, is_painted(collection, vec3_new(0, 0, 0), vec3_new(0, 0, 1)),
          "box center is painted");
    check(c, is_painted(collection, vec3_new(0.5f, -0.5f, 0.5f),
                        vec3_new(0, 0, 1)),
          "box boundary belongs to the closed box");
    check(c, !is_painted(collection, vec3_new(1.5f, 0, 0), vec3_new(0, 0, 1)),
          "repeat sampling must not wrap paint outside the box");
    check(c, !is_painted(collection, vec3_new(0, 0, -0.6f),
                         vec3_new(0, 0, 1)),
          "points behind the box stay unpainted");
    check(c, !is_painted(collection, vec3_new(0, 0.51f, 0),
                         vec3_new(0, 0, 1)),
          "points above the box stay unpainted");
    decal_collection_release(collection);

    /* rotation by 90 degrees around y plus positive non-uniform scale */
    collection = decal_collection_create();
    transform = mat4_from_trs(vec3_new(1, 2, 3), rot90y, vec3_new(2, 4, 1));
    rc = decal_collection_add(collection, transform, red, NULL,
                              0.8f, 0.1f, -1.0f);
    check(c, rc == 0, "rotated and scaled transform must be accepted");
    check(c, v3eq(collection->decals[0].axis_x, vec3_new(0, 0, -1)),
          "frame x axis follows the rotation only");
    check(c, v3eq(collection->decals[0].axis_y, vec3_new(0, 1, 0)),
          "frame y axis follows the rotation only");
    check(c, v3eq(collection->decals[0].axis_z, vec3_new(1, 0, 0)),
          "frame z axis follows the rotation only");
    check(c, is_painted(collection, vec3_new(1, 2, 3), vec3_new(0, 0, 1)),
          "rotated box center is painted");
    check(c, is_painted(collection, vec3_new(1, 3.9f, 3), vec3_new(0, 0, 1)),
          "y scale 4 reaches 1.9 world units");
    check(c, !is_painted(collection, vec3_new(1, 4.1f, 3), vec3_new(0, 0, 1)),
          "y scale 4 does not reach 2.1 world units");
    check(c, is_painted(collection, vec3_new(1.4f, 2, 3), vec3_new(0, 0, 1)),
          "z scale 1 reaches 0.4 world units along world x");
    check(c, !is_painted(collection, vec3_new(1.6f, 2, 3),
                         vec3_new(0, 0, 1)),
          "z scale 1 does not reach 0.6 world units along world x");
    check(c, is_painted(collection, vec3_new(1, 2, 2.1f), vec3_new(0, 0, 1)),
          "x scale 2 reaches 0.9 world units along world -z");
    check(c, !is_painted(collection, vec3_new(1, 2, 1.8f),
                         vec3_new(0, 0, 1)),
          "x scale 2 does not reach 1.2 world units along world -z");
    decal_collection_release(collection);

    /* UV = (x+.5, y+.5) under the same rotated and scaled box */
    {
        texture_t *grid = texture_create(4, 4);
        int i;
        for (i = 0; i < 16; i++) {
            grid->buffer[i] = vec4_new(1, 0, 0, 1);
        }
        grid->buffer[5] = vec4_new(0, 0, 1, 1);   /* u=.5,  v=.5  */
        grid->buffer[10] = vec4_new(0, 1, 0, 1);  /* u=.75, v=.75 */
        grid->buffer[8] = vec4_new(1, 1, 0, 1);   /* u=.25, v=.75 */
        grid->buffer[2] = vec4_new(0, 1, 1, 1);   /* u=.75, v=.25 */
        collection = decal_collection_create();
        rc = decal_collection_add(collection, transform, grid, NULL,
                                  0.8f, 0.1f, -1.0f);
        check(c, rc == 0, "grid decal accepted");
        check(c, v3eq(painted_color(collection, vec3_new(1, 2, 3),
                                    vec3_new(0, 0, 1)),
                      vec3_new(0, 0, 1)),
              "local (0,0) maps to UV (.5,.5)");
        check(c, v3eq(painted_color(collection, vec3_new(1, 3, 2.5f),
                                    vec3_new(0, 0, 1)),
                      vec3_new(0, 1, 0)),
              "local (.25,.25) maps to UV (.75,.75)");
        check(c, v3eq(painted_color(collection, vec3_new(1, 3, 3.5f),
                                    vec3_new(0, 0, 1)),
                      vec3_new(1, 1, 0)),
              "local (-.25,.25) maps to UV (.25,.75)");
        check(c, v3eq(painted_color(collection, vec3_new(1, 1, 2.5f),
                                    vec3_new(0, 0, 1)),
                      vec3_new(0, 1, 1)),
              "local (.25,-.25) maps to UV (.75,.25)");
        decal_collection_release(collection);
        texture_release(grid);
    }

    /* illegal transforms are reported and add nothing */
    {
        decal_collection_t *invalid = decal_collection_create();
        mat4_t shear = mat4_identity();
        mat4_t flat = mat4_identity();
        mat4_t mirror = mat4_scale(-1, 1, 1);
        mat4_t projective = mat4_identity();
        shear.m[0][1] = 0.5f;
        flat.m[2][2] = 0;
        projective.m[3][0] = 0.5f;
        check(c, decal_collection_add(invalid, shear, red, NULL,
                                      0, 0, -1) != 0,
              "shear must be reported");
        check(c, decal_collection_add(invalid, flat, red, NULL,
                                      0, 0, -1) != 0,
              "singular scale must be reported");
        check(c, decal_collection_add(invalid, mirror, red, NULL,
                                      0, 0, -1) != 0,
              "reflection must be reported");
        check(c, decal_collection_add(invalid, projective, red, NULL,
                                      0, 0, -1) != 0,
              "non-affine transform must be reported");
        check(c, decal_collection_add(invalid, mat4_identity(), NULL, NULL,
                                      0, 0, -1) != 0,
              "missing color texture must be reported");
        check(c, invalid->num_decals == 0, "failed adds keep no decals");
        decal_collection_release(invalid);
    }

    /* the angle filter looks at the base normal */
    {
        decal_collection_t *angled = decal_collection_create();
        rc = decal_collection_add(angled, mat4_identity(), red, NULL,
                                  0.8f, 0.1f, 0.5f);
        check(c, rc == 0, "angled decal accepted");
        check(c, is_painted(angled, vec3_new(0, 0, 0), vec3_new(0, 0, 1)),
              "surface facing the projection receives the decal");
        check(c, is_painted(angled, vec3_new(0, 0, 0),
                            vec3_new(0.8660254f, 0, 0.5f)),
              "surface exactly at the angle limit receives the decal");
        check(c, !is_painted(angled, vec3_new(0, 0, 0),
                             vec3_new(0.8717f, 0, 0.49f)),
              "surface tilted past the angle limit is rejected");
        check(c, !is_painted(angled, vec3_new(0, 0, 0), vec3_new(1, 0, 0)),
              "surface perpendicular to the projection is rejected");
        check(c, !is_painted(angled, vec3_new(0, 0, 0), vec3_new(0, 0, -1)),
              "surface facing away from the projection is rejected");
        decal_collection_release(angled);
    }

    texture_release(red);
}

void test_material(Check *c) {
    quat_t rot90y = quat_new(0, 0.70710678f, 0, 0.70710678f);
    texture_t *half_red = solid_texture(2, 2, vec4_new(1, 0, 0, 0.5f));
    texture_t *red = solid_texture(2, 2, vec4_new(1, 0, 0, 1));
    texture_t *green = solid_texture(2, 2, vec4_new(0, 1, 0, 1));
    texture_t *half_blue = solid_texture(2, 2, vec4_new(0, 0, 1, 0.5f));
    vec3_t basecolor, normal;
    float roughness, metalness;

    /* color, roughness, and metalness blend with the color alpha */
    {
        decal_collection_t *collection = decal_collection_create();
        decal_collection_add(collection, mat4_identity(), half_red, NULL,
                             0.8f, 0.0f, -1.0f);
        basecolor = vec3_new(0.2f, 0.4f, 0.6f);
        roughness = 0.2f;
        metalness = 0.9f;
        normal = vec3_new(0, 0, 1);
        decal_collection_apply(collection, vec3_new(0, 0, 0),
                               vec3_new(0, 0, 1), &basecolor, &roughness,
                               &metalness, &normal);
        check(c, v3eq(basecolor, vec3_new(0.6f, 0.2f, 0.3f)),
              "basecolor lerps toward the decal color by alpha");
        check(c, feq(roughness, 0.5f), "roughness lerps by the same alpha");
        check(c, feq(metalness, 0.45f), "metalness lerps by the same alpha");
        check(c, v3eq(normal, vec3_new(0, 0, 1)),
              "missing normal texture leaves the normal untouched");
        decal_collection_release(collection);
    }

    /* tangent normals go through the orthonormal frame, scale-independent */
    {
        texture_t *bump = solid_texture(2, 2, vec4_new(0.85355339f, 0.5f,
                                                       0.85355339f, 1));
        decal_collection_t *scaled = decal_collection_create();
        decal_collection_t *uniform = decal_collection_create();
        vec3_t normal_scaled, normal_uniform;
        decal_collection_add(scaled,
                             mat4_from_trs(vec3_new(0, 0, 0), rot90y,
                                           vec3_new(3, 1, 5)),
                             half_red, bump, 0.8f, 0.0f, -1.0f);
        decal_collection_add(uniform,
                             mat4_from_trs(vec3_new(0, 0, 0), rot90y,
                                           vec3_new(1, 1, 1)),
                             half_red, bump, 0.8f, 0.0f, -1.0f);
        basecolor = vec3_new(0.2f, 0.4f, 0.6f);
        roughness = 0.2f;
        metalness = 0.9f;
        normal_scaled = vec3_new(0, 0, 1);
        decal_collection_apply(scaled, vec3_new(0, 0, 0), vec3_new(0, 0, 1),
                               &basecolor, &roughness, &metalness,
                               &normal_scaled);
        /* tangent normal (0.7071,0,0.7071) becomes (0.7071,0,-0.7071) in
         * world space; blended 50/50 with (0,0,1) and renormalized */
        check(c, v3eq(normal_scaled, vec3_new(0.92387953f, 0, 0.38268343f)),
              "decal normal blends and renormalizes through the frame");
        normal_uniform = vec3_new(0, 0, 1);
        basecolor = vec3_new(0.2f, 0.4f, 0.6f);
        roughness = 0.2f;
        metalness = 0.9f;
        decal_collection_apply(uniform, vec3_new(0, 0, 0), vec3_new(0, 0, 1),
                               &basecolor, &roughness, &metalness,
                               &normal_uniform);
        check(c, v3eq(normal_scaled, normal_uniform),
              "non-uniform scale must not skew decal normals");
        decal_collection_release(scaled);
        decal_collection_release(uniform);
        texture_release(bump);
    }

    /* layers are applied in collection order and order changes results */
    {
        decal_collection_t *red_green = decal_collection_create();
        decal_collection_t *green_red = decal_collection_create();
        decal_collection_t *red_blue = decal_collection_create();
        decal_collection_t *blue_red = decal_collection_create();
        vec3_t color_rg, color_gr, color_rb, color_br;
        decal_collection_add(red_green, mat4_identity(), red, NULL,
                             0.8f, 0.0f, -1.0f);
        decal_collection_add(red_green, mat4_identity(), green, NULL,
                             0.8f, 0.0f, -1.0f);
        decal_collection_add(green_red, mat4_identity(), green, NULL,
                             0.8f, 0.0f, -1.0f);
        decal_collection_add(green_red, mat4_identity(), red, NULL,
                             0.8f, 0.0f, -1.0f);
        decal_collection_add(red_blue, mat4_identity(), half_red, NULL,
                             0.8f, 0.0f, -1.0f);
        decal_collection_add(red_blue, mat4_identity(), half_blue, NULL,
                             0.8f, 0.0f, -1.0f);
        decal_collection_add(blue_red, mat4_identity(), half_blue, NULL,
                             0.8f, 0.0f, -1.0f);
        decal_collection_add(blue_red, mat4_identity(), half_red, NULL,
                             0.8f, 0.0f, -1.0f);
        color_rg = painted_color(red_green, vec3_new(0, 0, 0),
                                 vec3_new(0, 0, 1));
        color_gr = painted_color(green_red, vec3_new(0, 0, 0),
                                 vec3_new(0, 0, 1));
        color_rb = painted_color(red_blue, vec3_new(0, 0, 0),
                                 vec3_new(0, 0, 1));
        color_br = painted_color(blue_red, vec3_new(0, 0, 0),
                                 vec3_new(0, 0, 1));
        check(c, v3eq(color_rg, vec3_new(0, 1, 0)),
              "later opaque layer wins");
        check(c, v3eq(color_gr, vec3_new(1, 0, 0)),
              "reversed order gives the reversed result");
        check(c, v3eq(color_rb, vec3_new(0.3f, 0.075f, 0.6f)),
              "half alpha red then blue blends over the base color");
        check(c, v3eq(color_br, vec3_new(0.55f, 0.075f, 0.35f)),
              "half alpha blue then red blends over the base color");
        check(c, !v3eq(color_rb, color_br),
              "layer order changes the blended result");

        /* repeated evaluation starts fresh: nothing accumulates */
        check(c, v3eq(painted_color(red_blue, vec3_new(0, 0, 0),
                                    vec3_new(0, 0, 1)), color_rb),
              "repeated evaluation does not accumulate");
        decal_collection_release(red_green);
        decal_collection_release(green_red);
        decal_collection_release(red_blue);
        decal_collection_release(blue_red);
    }

    texture_release(half_red);
    texture_release(red);
    texture_release(green);
    texture_release(half_blue);
}

/* vertex shader passing world positions straight through the pipeline */
typedef struct {
    vec4_t position;
    vec2_t texcoord;
} test_attribs_t;

static vec4_t test_vertex_shader(void *attribs_, void *varyings_,
                                 void *uniforms_) {
    test_attribs_t *attribs = (test_attribs_t*)attribs_;
    pbr_varyings_t *varyings = (pbr_varyings_t*)varyings_;
    UNUSED_VAR(uniforms_);
    memset(varyings, 0, sizeof(pbr_varyings_t));
    varyings->world_position = vec3_from_vec4(attribs->position);
    varyings->world_normal = vec3_new(0, 0, 1);
    varyings->clip_position = attribs->position;
    varyings->texcoord = attribs->texcoord;
    return attribs->position;
}

static void draw_fullscreen_triangle(framebuffer_t *framebuffer,
                                     program_t *program) {
    test_attribs_t attribs[3] = {
        {vec4_new(-1, -1, 0, 1), vec2_new(0, 0)},
        {vec4_new(3, -1, 0, 1), vec2_new(2, 0)},
        {vec4_new(-1, 3, 0, 1), vec2_new(0, 2)},
    };
    int i;
    for (i = 0; i < 3; i++) {
        memcpy(program_get_attribs(program, i), &attribs[i],
               sizeof(test_attribs_t));
    }
    graphics_draw_triangle(framebuffer, program);
}

static unsigned char red_byte_at(framebuffer_t *framebuffer, int x, int y) {
    return framebuffer->color_buffer[(y * framebuffer->width + x) * 4];
}

void test_pipeline(Check *c) {
    texture_t *red = solid_texture(2, 2, vec4_new(1, 0, 0, 1));
    decal_collection_t *collection = decal_collection_create();
    framebuffer_t *framebuffer;
    program_t *program;
    pbr_uniforms_t *uniforms;
    unsigned char painted_roughness, base_roughness;

    decal_collection_add(collection, mat4_identity(), red, NULL,
                         0.9f, 0.0f, -1.0f);

    /* real draw through graphics_draw_triangle, low-level binding */
    framebuffer = framebuffer_create(16, 16);
    program = program_create(test_vertex_shader, pbr_fragment_shader,
                             sizeof(test_attribs_t), sizeof(pbr_varyings_t),
                             sizeof(pbr_uniforms_t), 1, 0);
    uniforms = (pbr_uniforms_t*)program_get_uniforms(program);
    memset(uniforms, 0, sizeof(pbr_uniforms_t));
    uniforms->workflow = METALNESS_WORKFLOW;
    uniforms->basecolor_factor = vec4_new(0.5f, 0.5f, 0.5f, 1);
    uniforms->roughness_factor = 0.4f;
    uniforms->camera_pos = vec3_new(0, 0, 3);
    uniforms->layer_view = 3;
    uniforms->decals = collection;

    painted_roughness = float_to_uchar(0.9f);
    base_roughness = float_to_uchar(0.4f);

    draw_fullscreen_triangle(framebuffer, program);
    check(c, red_byte_at(framebuffer, 8, 8) == painted_roughness,
          "decal roughness visible in the layer preview at the box");
    check(c, red_byte_at(framebuffer, 1, 1) == base_roughness,
          "base roughness kept outside the box");

    /* the same material shows up in the diffuse layer preview */
    uniforms->layer_view = 1;
    framebuffer_clear_color(framebuffer, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(framebuffer, 1);
    draw_fullscreen_triangle(framebuffer, program);
    check(c, red_byte_at(framebuffer, 8, 8) ==
          float_to_uchar(float_linear2srgb(float_aces(0.96f))),
          "decal diffuse visible in the layer preview");
    check(c, red_byte_at(framebuffer, 1, 1) ==
          float_to_uchar(float_linear2srgb(float_aces(0.48f))),
          "base diffuse kept outside the box");

    /* and in the fully shaded output */
    uniforms->layer_view = -1;
    uniforms->punctual_intensity = 1;
    uniforms->light_dir = vec3_new(0, 0, -1);
    uniforms->decals = NULL;
    framebuffer_clear_color(framebuffer, vec4_new(0, 0, 0, 1));
    framebuffer_clear_depth(framebuffer, 1);
    draw_fullscreen_triangle(framebuffer, program);
    {
        unsigned char base_center = red_byte_at(framebuffer, 8, 8);
        unsigned char base_corner = red_byte_at(framebuffer, 1, 1);
        unsigned char with_decal, corner_with_decal, redrawn;

        uniforms->decals = collection;
        framebuffer_clear_color(framebuffer, vec4_new(0, 0, 0, 1));
        framebuffer_clear_depth(framebuffer, 1);
        draw_fullscreen_triangle(framebuffer, program);
        with_decal = red_byte_at(framebuffer, 8, 8);
        corner_with_decal = red_byte_at(framebuffer, 1, 1);
        check(c, with_decal != base_center,
              "decal changes the shaded output inside the box");
        check(c, corner_with_decal == base_corner,
              "shaded output outside the box is untouched");

        /* drawing the next frame again does not accumulate decals */
        framebuffer_clear_color(framebuffer, vec4_new(0, 0, 0, 1));
        framebuffer_clear_depth(framebuffer, 1);
        draw_fullscreen_triangle(framebuffer, program);
        redrawn = red_byte_at(framebuffer, 8, 8);
        check(c, redrawn == with_decal,
              "repeated frames do not accumulate decal material");

        /* unbinding restores the previous output exactly */
        uniforms->decals = NULL;
        framebuffer_clear_color(framebuffer, vec4_new(0, 0, 0, 1));
        framebuffer_clear_depth(framebuffer, 1);
        draw_fullscreen_triangle(framebuffer, program);
        check(c, red_byte_at(framebuffer, 8, 8) == base_center,
              "unbinding restores the base material");
    }

    /* the shadow pass ignores decals */
    uniforms->shadow_pass = 1;
    uniforms->alpha_cutoff = 0;
    {
        pbr_varyings_t varyings;
        vec4_t color;
        int discard = 0;
        memset(&varyings, 0, sizeof(varyings));
        varyings.world_position = vec3_new(0, 0, 0);
        varyings.world_normal = vec3_new(0, 0, 1);
        color = pbr_fragment_shader(&varyings, uniforms, &discard, 0);
        check(c, !discard && color.x == 0 && color.w == 0,
              "shadow pass stays depth-only with decals bound");
    }
    uniforms->shadow_pass = 0;

    program_release(program);
    framebuffer_release(framebuffer);

    /* mesh UV seams do not affect world-space projection */
    {
        pbr_uniforms_t fu;
        pbr_varyings_t v1, v2;
        vec4_t out1, out2, out_base;
        int discard = 0;
        texture_t *grey = solid_texture(2, 2, vec4_new(0.5f, 0.5f, 0.5f, 1));
        float expected_r;
        memset(&fu, 0, sizeof(fu));
        fu.workflow = METALNESS_WORKFLOW;
        fu.basecolor_factor = vec4_new(1, 1, 1, 1);
        fu.basecolor_map = grey;
        fu.roughness_factor = 0.4f;
        fu.layer_view = 1;
        fu.decals = collection;
        memset(&v1, 0, sizeof(v1));
        v1.world_position = vec3_new(0.1f, 0.1f, 0);
        v1.world_normal = vec3_new(0, 0, 1);
        v1.clip_position = vec4_new(0, 0, 0, 1);
        v1.texcoord = vec2_new(0.99f, 0.5f);
        v2 = v1;
        v2.texcoord = vec2_new(0.01f, 0.5f);
        out1 = pbr_fragment_shader(&v1, &fu, &discard, 0);
        check(c, !discard, "fragment before the seam is kept");
        out2 = pbr_fragment_shader(&v2, &fu, &discard, 0);
        check(c, !discard, "fragment after the seam is kept");
        check(c, v3eq(vec3_from_vec4(out1), vec3_from_vec4(out2)),
              "the decal crosses the mesh UV seam");
        expected_r = float_linear2srgb(float_aces(0.96f));
        check(c, feq(out1.x, expected_r) && feq(out1.y, 0),
              "both sides of the seam get the decal color");
        fu.decals = NULL;
        out_base = pbr_fragment_shader(&v1, &fu, &discard, 0);
        check(c, !feq(out_base.x, out1.x),
              "seam fragments differ from the unpainted base");
        texture_release(grey);
    }

    /* the base normal map changes neither coverage nor cutout holes */
    {
        pbr_uniforms_t fu;
        pbr_varyings_t varyings;
        vec4_t out;
        int discard = 0;
        texture_t *tilt = solid_texture(2, 2, vec4_new(0.14644661f, 0.5f,
                                                       0.85355339f, 1));
        decal_collection_t *steep = decal_collection_create();
        decal_collection_add(steep, mat4_identity(), red, NULL,
                             0.9f, 0.0f, 0.9f);
        memset(&fu, 0, sizeof(fu));
        fu.workflow = METALNESS_WORKFLOW;
        fu.basecolor_factor = vec4_new(0.5f, 0.5f, 0.5f, 1);
        fu.roughness_factor = 0.4f;
        fu.normal_map = tilt;
        fu.layer_view = 1;
        fu.decals = steep;
        memset(&varyings, 0, sizeof(varyings));
        varyings.world_position = vec3_new(0, 0, 0);
        varyings.world_normal = vec3_new(0, 0, 1);
        varyings.world_tangent = vec3_new(1, 0, 0);
        varyings.world_bitangent = vec3_new(0, 1, 0);
        varyings.clip_position = vec4_new(0, 0, 0, 1);
        out = pbr_fragment_shader(&varyings, &fu, &discard, 0);
        check(c, !discard, "fragment kept with base normal map");
        check(c, feq(out.x, float_linear2srgb(float_aces(0.96f))),
              "base normal map does not change decal coverage");
        /* the pre-flip base normal decides, even on backfaces */
        out = pbr_fragment_shader(&varyings, &fu, &discard, 1);
        check(c, feq(out.x, float_linear2srgb(float_aces(0.96f))),
              "backface flip does not change decal coverage");
        decal_collection_release(steep);
        texture_release(tilt);
    }

    /* alpha_cutoff holes stay holes inside the decal box */
    {
        pbr_uniforms_t fu;
        pbr_varyings_t varyings;
        vec4_t out;
        int discard = 0;
        texture_t *cutout = solid_texture(4, 4, vec4_new(0.5f, 0.5f, 0.5f, 1));
        cutout->buffer[0] = vec4_new(0.5f, 0.5f, 0.5f, 0);
        memset(&fu, 0, sizeof(fu));
        fu.workflow = METALNESS_WORKFLOW;
        fu.basecolor_factor = vec4_new(1, 1, 1, 0.8f);
        fu.basecolor_map = cutout;
        fu.roughness_factor = 0.4f;
        fu.alpha_cutoff = 0.5f;
        fu.layer_view = 1;
        fu.decals = collection;
        memset(&varyings, 0, sizeof(varyings));
        varyings.world_position = vec3_new(0, 0, 0);
        varyings.world_normal = vec3_new(0, 0, 1);
        varyings.clip_position = vec4_new(0, 0, 0, 1);
        varyings.texcoord = vec2_new(0.25f, 0.25f);
        pbr_fragment_shader(&varyings, &fu, &discard, 0);
        check(c, discard, "cutout hole inside the box stays discarded");
        varyings.texcoord = vec2_new(0.75f, 0.75f);
        discard = 0;
        out = pbr_fragment_shader(&varyings, &fu, &discard, 0);
        check(c, !discard, "opaque texel inside the box is kept");
        check(c, feq(out.x, float_linear2srgb(float_aces(0.96f))),
              "kept fragment next to the hole gets the decal color");
        check(c, feq(out.w, 0.8f),
              "the decal never raises the base alpha");
        texture_release(cutout);
    }

    /* the specular workflow is not rerouted through decals */
    {
        pbr_uniforms_t fu;
        pbr_varyings_t varyings;
        vec4_t with_decals, without_decals;
        int discard = 0;
        memset(&fu, 0, sizeof(fu));
        fu.workflow = SPECULAR_WORKFLOW;
        fu.diffuse_factor = vec4_new(0.5f, 0.4f, 0.3f, 1);
        fu.specular_factor = vec3_new(0.04f, 0.04f, 0.04f);
        fu.glossiness_factor = 0.6f;
        fu.layer_view = 1;
        fu.decals = collection;
        memset(&varyings, 0, sizeof(varyings));
        varyings.world_position = vec3_new(0, 0, 0);
        varyings.world_normal = vec3_new(0, 0, 1);
        varyings.clip_position = vec4_new(0, 0, 0, 1);
        with_decals = pbr_fragment_shader(&varyings, &fu, &discard, 0);
        fu.decals = NULL;
        without_decals = pbr_fragment_shader(&varyings, &fu, &discard, 0);
        check(c, feq(with_decals.x, without_decals.x) &&
                 feq(with_decals.y, without_decals.y) &&
                 feq(with_decals.z, without_decals.z),
              "specular workflow output is unchanged by decals");
    }

    /* high-level binding: two models hold two different collections */
    {
        pbrm_material_t material;
        model_t *model_a, *model_b;
        decal_collection_t *collection_b = decal_collection_create();
        perframe_t perframe;
        pbr_uniforms_t *uniforms_a, *uniforms_b;
        decal_collection_add(collection_b, mat4_identity(), red, NULL,
                             0.1f, 1.0f, -1.0f);
        memset(&material, 0, sizeof(material));
        material.basecolor_factor = vec4_new(0.5f, 0.5f, 0.5f, 1);
        material.roughness_factor = 0.4f;
        model_a = pbrm_create_model(NULL, mat4_identity(), NULL, -1,
                                    &material, NULL);
        model_b = pbrm_create_model(NULL, mat4_identity(), NULL, -1,
                                    &material, NULL);
        pbr_set_decals(model_a, collection);
        pbr_set_decals(model_b, collection_b);
        uniforms_a = (pbr_uniforms_t*)program_get_uniforms(model_a->program);
        uniforms_b = (pbr_uniforms_t*)program_get_uniforms(model_b->program);
        check(c, uniforms_a->decals == collection,
              "first model keeps its own collection");
        check(c, uniforms_b->decals == collection_b,
              "second model keeps a different collection");
        memset(&perframe, 0, sizeof(perframe));
        model_a->update(model_a, &perframe);
        check(c, uniforms_a->decals == collection,
              "model update does not clear the binding");
        pbr_set_decals(model_a, NULL);
        check(c, uniforms_a->decals == NULL, "NULL unbinds the collection");
        check(c, uniforms_b->decals == collection_b,
              "unbinding one model leaves the other bound");
        model_a->release(model_a);
        model_b->release(model_b);
        decal_collection_release(collection_b);
    }

    decal_collection_release(collection);
    texture_release(red);
}
