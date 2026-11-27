#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checks.h"
#include "renderer/core/api.h"
#include "renderer/core/decal.h"
#include "renderer/shaders/pbr_shader.h"

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static texture_t *make_color_texture(int width, int height, vec4_t color) {
    texture_t *texture = texture_create(width, height);
    int num_pixels = width * height;
    int i;
    for (i = 0; i < num_pixels; i++) {
        texture->buffer[i] = color;
    }
    return texture;
}

/* left half keeps alpha 0 (a punched hole), right half is solid paint */
static texture_t *make_hole_texture(int width, int height, vec4_t color) {
    texture_t *texture = texture_create(width, height);
    int r, c;
    for (r = 0; r < height; r++) {
        for (c = 0; c < width; c++) {
            vec4_t texel = color;
            if (c * 2 < width) {
                texel.w = 0.0f;
            }
            texture->buffer[r * width + c] = texel;
        }
    }
    return texture;
}

static texture_t *make_normal_texture(vec3_t tangent_normal) {
    texture_t *texture = texture_create(2, 2);
    vec4_t encoded = vec4_new(tangent_normal.x * 0.5f + 0.5f,
                              tangent_normal.y * 0.5f + 0.5f,
                              tangent_normal.z * 0.5f + 0.5f, 1.0f);
    int i;
    for (i = 0; i < 4; i++) {
        texture->buffer[i] = encoded;
    }
    return texture;
}

static int nearly(float a, float b) {
    return fabsf(a - b) < 1e-4f;
}

static int vec3_nearly(vec3_t a, vec3_t b) {
    return nearly(a.x, b.x) && nearly(a.y, b.y) && nearly(a.z, b.z);
}

static decal_surface_t fresh_surface(void) {
    decal_surface_t surface;
    surface.basecolor = vec3_new(0.5f, 0.5f, 0.5f);
    surface.normal = vec3_new(0, 0, 1);
    surface.roughness = 0.5f;
    surface.metalness = 0.0f;
    return surface;
}

/* ------------------------------------------------------------------ */
/* test_projection: boxes, frames, gating and transform validation    */
/* ------------------------------------------------------------------ */

void test_projection(Check *c) {
    decal_collection_t *collection = decal_collection_create();
    texture_t *paint = make_color_texture(
        4, 4, vec4_new(0.1f, 0.8f, 0.2f, 1.0f));
    texture_t *hole = make_hole_texture(
        4, 4, vec4_new(0.1f, 0.8f, 0.2f, 1.0f));
    mat4_t identity = mat4_identity();
    decal_surface_t surface;
    int index;

    /* identity box at the origin, projection along local -Z */
    index = decal_collection_add(collection, identity, paint, NULL,
                                 0.2f, 0.7f, 0.0f);
    check(c, index == 0, "first decal index should be 0");
    check(c, decal_collection_size(collection) == 1, "collection holds one");

    /* point inside the closed box, facing +Z: fully painted */
    surface = fresh_surface();
    decal_collection_apply(collection, vec3_new(0.2f, 0.1f, 0.3f),
                           vec3_new(0, 0, 1), &surface);
    check(c, vec3_nearly(surface.basecolor, vec3_new(0.1f, 0.8f, 0.2f)),
          "inside point should take the paint color");
    check(c, nearly(surface.roughness, 0.2f), "roughness should be painted");
    check(c, nearly(surface.metalness, 0.7f), "metalness should be painted");

    /* point outside the box: repeat sampling must not wrap onto it */
    surface = fresh_surface();
    decal_collection_apply(collection, vec3_new(0.6f, 0.1f, 0.3f),
                           vec3_new(0, 0, 1), &surface);
    check(c, vec3_nearly(surface.basecolor, vec3_new(0.5f, 0.5f, 0.5f)),
          "outside-box point must stay unpainted");
    check(c, nearly(surface.roughness, 0.5f), "outside roughness untouched");

    /* box boundary is closed: local x = 0.5 gives u = 1 and still paints */
    surface = fresh_surface();
    decal_collection_apply(collection, vec3_new(0.5f, 0.0f, 0.0f),
                           vec3_new(0, 0, 1), &surface);
    check(c, vec3_nearly(surface.basecolor, vec3_new(0.1f, 0.8f, 0.2f)),
          "closed-box boundary should be painted");

    /* back-facing surface relative to the world +Z frame is rejected */
    surface = fresh_surface();
    decal_collection_apply(collection, vec3_new(0.0f, 0.0f, 0.0f),
                           vec3_new(0, 0, -1), &surface);
    check(c, vec3_nearly(surface.basecolor, vec3_new(0.5f, 0.5f, 0.5f)),
          "back-facing fragment must be rejected");

    /* angled surface: exactly at the angle_cos threshold is accepted */
    surface = fresh_surface();
    decal_collection_apply(collection, vec3_new(0.0f, 0.0f, 0.0f),
                           vec3_normalize(vec3_new(0, 1, 1)), &surface);
    check(c, vec3_nearly(surface.basecolor, vec3_new(0.1f, 0.8f, 0.2f)),
          "45 degree face passes angle_cos = 0");

    /* stricter angle gate rejects the same sloped face */
    decal_collection_release(collection);
    collection = decal_collection_create();
    decal_collection_add(collection, identity, paint, NULL,
                         0.2f, 0.7f, 0.9f);
    surface = fresh_surface();
    decal_collection_apply(collection, vec3_new(0.0f, 0.0f, 0.0f),
                           vec3_normalize(vec3_new(0, 1, 1)), &surface);
    check(c, vec3_nearly(surface.basecolor, vec3_new(0.5f, 0.5f, 0.5f)),
          "oblique face must fail a strict angle_cos");

    /* rotation plus non-uniform, positive scale */
    {
        quat_t rotation = quat_normalize(quat_new(0.0f,
            sinf(TO_RADIANS(45) * 0.5f), 0.0f,
            cosf(TO_RADIANS(45) * 0.5f)));
        vec3_t translation = vec3_new(1.0f, 0.0f, 0.0f);
        vec3_t scale = vec3_new(2.0f, 0.5f, 4.0f);
        mat4_t trs = mat4_from_trs(translation, rotation, scale);
        vec3_t axis_z;
        vec3_t local_point, inside_world, outside_world;
        mat4_t inv;

        decal_collection_release(collection);
        collection = decal_collection_create();
        index = decal_collection_add(collection, trs, paint, NULL,
                                     0.2f, 0.7f, 0.0f);
        check(c, index == 0, "rotated non-uniform decal should be accepted");
        axis_z = decal_collection_get(collection, 0)->axis_z;
        check(c, nearly(axis_z.x, sinf(TO_RADIANS(45))) &&
                  nearly(axis_z.z, cosf(TO_RADIANS(45))),
              "direction frame should follow the rotation only");

        /* a local corner maps through T*R*S and must still be detected */
        local_point = vec3_new(0.25f, -0.25f, 0.5f);
        inside_world = vec3_from_vec4(mat4_mul_vec4(
            trs, vec4_from_vec3(local_point, 1)));
        inv = decal_collection_get(collection, 0)->world_to_local;
        check(c, vec3_nearly(
                  vec3_from_vec4(mat4_mul_vec4(
                      inv, vec4_from_vec3(inside_world, 1))),
                  local_point),
              "inverse transform should recover the local point");
        surface = fresh_surface();
        decal_collection_apply(collection, inside_world, axis_z, &surface);
        check(c, vec3_nearly(surface.basecolor, vec3_new(0.1f, 0.8f, 0.2f)),
              "rotated scaled box should paint its interior");

        outside_world = vec3_from_vec4(mat4_mul_vec4(
            trs, vec4_new(0.51f, 0.0f, 0.0f, 1.0f)));
        surface = fresh_surface();
        decal_collection_apply(collection, outside_world, axis_z, &surface);
        check(c, vec3_nearly(surface.basecolor, vec3_new(0.5f, 0.5f, 0.5f)),
              "non-uniform box edge must not leak paint outside");
    }

    /* transparent texel hole: alpha 0 changes nothing */
    decal_collection_release(collection);
    collection = decal_collection_create();
    decal_collection_add(collection, identity, hole, NULL,
                         0.2f, 0.7f, 0.0f);
    surface = fresh_surface();
    decal_collection_apply(collection, vec3_new(-0.25f, 0.0f, 0.0f),
                           vec3_new(0, 0, 1), &surface);
    check(c, vec3_nearly(surface.basecolor, vec3_new(0.5f, 0.5f, 0.5f)),
          "transparent texel hole should leave the surface untouched");
    surface = fresh_surface();
    decal_collection_apply(collection, vec3_new(0.25f, 0.0f, 0.0f),
                           vec3_new(0, 0, 1), &surface);
    check(c, vec3_nearly(surface.basecolor, vec3_new(0.1f, 0.8f, 0.2f)),
          "solid texels next to the hole still paint");

    /* illegal transforms are reported and rejected */
    check(c, decal_collection_add(collection, identity, NULL, NULL,
                                  0.2f, 0.7f, 0.0f) == -1,
          "missing color texture must be rejected");
    {
        mat4_t singular = mat4_identity();
        mat4_t shear = mat4_identity();
        singular.m[1][1] = 0.0f;  /* zero scale column, no mat4_scale assert */
        check(c, decal_collection_add(collection, singular, paint, NULL,
                                      0.2f, 0.7f, 0.0f) == -1,
              "singular scale must be rejected");
        check(c, decal_collection_add(
                  collection, mat4_scale(-1.0f, 1.0f, 1.0f), paint, NULL,
                  0.2f, 0.7f, 0.0f) == -1,
              "reflected scale must be rejected");
        shear.m[0][1] = 1.0f;
        check(c, decal_collection_add(collection, shear, paint, NULL,
                                      0.2f, 0.7f, 0.0f) == -1,
              "shear transform must be rejected");
    }
    check(c, decal_collection_size(collection) == 1,
          "rejected decals must not be stored");

    texture_release(hole);
    texture_release(paint);
    decal_collection_release(collection);
}

/* ------------------------------------------------------------------ */
/* test_material: independent numeric blending of color and normals   */
/* ------------------------------------------------------------------ */

void test_material(Check *c) {
    vec3_t base_color = vec3_new(0.8f, 0.2f, 0.1f);
    vec3_t paint_color = vec3_new(0.1f, 0.3f, 0.9f);
    float alpha = 0.25f;
    decal_surface_t surface, reference;
    texture_t *paint;
    decal_collection_t *collection;
    int i;

    /* single-layer color/roughness/metalness against hand-computed math */
    paint = make_color_texture(2, 2,
                               vec4_new(paint_color.x, paint_color.y,
                                        paint_color.z, alpha));
    collection = decal_collection_create();
    decal_collection_add(collection, mat4_identity(), paint, NULL,
                         0.1f, 0.9f, 0.0f);
    surface = fresh_surface();
    surface.basecolor = base_color;
    decal_collection_apply(collection, vec3_new(0, 0, 0),
                           vec3_new(0, 0, 1), &surface);
    for (i = 0; i < 3; i++) {
        float base = i == 0 ? base_color.x : (i == 1 ? base_color.y
                                                      : base_color.z);
        float target = i == 0 ? paint_color.x
                              : (i == 1 ? paint_color.y : paint_color.z);
        float expected = base * (1 - alpha) + target * alpha;
        float actual = i == 0 ? surface.basecolor.x
                              : (i == 1 ? surface.basecolor.y
                                        : surface.basecolor.z);
        check(c, nearly(actual, expected), "linear color blend numeric");
    }
    check(c, nearly(surface.roughness,
                    0.5f * (1 - alpha) + 0.1f * alpha),
          "roughness blend numeric");
    check(c, nearly(surface.metalness,
                    0.0f * (1 - alpha) + 0.9f * alpha),
          "metalness blend numeric");
    reference = surface;  /* same single-layer collection, later frame */

    /* two overlapping layers in collection order, checked independently */
    {
        texture_t *paint2 = make_color_texture(
            2, 2, vec4_new(0.9f, 0.9f, 0.2f, 0.5f));
        decal_collection_t *forward = decal_collection_create();
        decal_collection_t *reversed = decal_collection_create();
        vec3_t expected_order, expected_reverse;
        decal_surface_t other;

        decal_collection_add(forward, mat4_identity(), paint, NULL,
                             0.1f, 0.9f, 0.0f);
        decal_collection_add(forward, mat4_identity(), paint2, NULL,
                             0.8f, 0.2f, 0.0f);
        decal_collection_add(reversed, mat4_identity(), paint2, NULL,
                             0.8f, 0.2f, 0.0f);
        decal_collection_add(reversed, mat4_identity(), paint, NULL,
                             0.1f, 0.9f, 0.0f);

        surface = fresh_surface();
        surface.basecolor = base_color;
        decal_collection_apply(forward, vec3_new(0, 0, 0),
                               vec3_new(0, 0, 1), &surface);
        other = fresh_surface();
        other.basecolor = base_color;
        decal_collection_apply(reversed, vec3_new(0, 0, 0),
                               vec3_new(0, 0, 1), &other);

        /* hand computation: layer 1 then layer 2 */
        expected_order = vec3_lerp(base_color, paint_color, alpha);
        expected_order = vec3_lerp(expected_order,
                                   vec3_new(0.9f, 0.9f, 0.2f), 0.5f);
        expected_reverse = vec3_lerp(base_color,
                                     vec3_new(0.9f, 0.9f, 0.2f), 0.5f);
        expected_reverse = vec3_lerp(expected_reverse, paint_color, alpha);
        check(c, vec3_nearly(surface.basecolor, expected_order),
              "overlap order should match forward hand computation");
        check(c, vec3_nearly(other.basecolor, expected_reverse),
              "reversed layers should match reversed hand computation");
        check(c, !vec3_nearly(surface.basecolor, other.basecolor),
              "reversing layer order must change the result");
        decal_collection_release(forward);
        decal_collection_release(reversed);
        texture_release(paint2);
    }

    /* repainting a fresh surface next frame gives identical material */
    {
        decal_surface_t again = fresh_surface();
        again.basecolor = base_color;
        decal_collection_apply(collection, vec3_new(0, 0, 0),
                               vec3_new(0, 0, 1), &again);
        check(c, vec3_nearly(again.basecolor, reference.basecolor) &&
                  nearly(again.roughness, reference.roughness) &&
                  nearly(again.metalness, reference.metalness),
              "decals hold no state between frames");
    }

    decal_collection_release(collection);
    texture_release(paint);

    /* normal blending, independent numeric computation */
    {
        texture_t *normal_tex = make_normal_texture(vec3_new(0, 0, 1));
        texture_t *color_a = make_color_texture(
            1, 1, vec4_new(1, 1, 1, 0.5f));
        texture_t *color_b;
        quat_t rotation = quat_normalize(quat_new(
            0.0f, sinf(TO_RADIANS(90) * 0.5f), 0.0f,
            cosf(TO_RADIANS(90) * 0.5f)));
        mat4_t rotated = mat4_from_trs(vec3_new(0, 0, 0), rotation,
                                       vec3_new(1, 1, 1));
        mat4_t stretched = mat4_from_trs(vec3_new(0, 0, 0), rotation,
                                         vec3_new(3.0f, 0.5f, 7.0f));
        decal_collection_t *normals = decal_collection_create();
        decal_collection_t *stretched_normals = decal_collection_create();
        vec3_t n0 = vec3_normalize(vec3_new(0, 1, 1));
        vec3_t expected;
        decal_surface_t with_normal, stretched_surface, no_normal_map;

        decal_collection_add(normals, rotated, color_a, normal_tex,
                             0.3f, 0.0f, 0.0f);
        decal_collection_add(stretched_normals, stretched, color_a,
                             normal_tex, 0.3f, 0.0f, 0.0f);

        with_normal = fresh_surface();
        with_normal.normal = n0;
        decal_collection_apply(normals, vec3_new(0, 0, 0), n0,
                               &with_normal);
        /* tangent (0,0,1) maps onto the frame +Z = world +X here */
        expected = vec3_normalize(vec3_lerp(n0, vec3_new(1, 0, 0), 0.5f));
        check(c, vec3_nearly(with_normal.normal, expected),
              "decaled normal should match hand-computed blend");

        stretched_surface = fresh_surface();
        stretched_surface.normal = n0;
        decal_collection_apply(stretched_normals, vec3_new(0, 0, 0), n0,
                               &stretched_surface);
        check(c, vec3_nearly(stretched_surface.normal, expected),
              "non-uniform scale must not skew the decaled normal");

        /* without a normal texture only the other quantities change */
        no_normal_map = fresh_surface();
        no_normal_map.normal = n0;
        decal_collection_release(normals);
        normals = decal_collection_create();
        color_b = make_color_texture(1, 1,
                                     vec4_new(0.2f, 0.4f, 0.6f, 1.0f));
        decal_collection_add(normals, mat4_identity(), color_b, NULL,
                             0.1f, 0.0f, 0.0f);
        decal_collection_apply(normals, vec3_new(0, 0, 0), n0,
                               &no_normal_map);
        check(c, vec3_nearly(no_normal_map.normal, n0),
              "missing normal texture must keep the shading normal");
        check(c, vec3_nearly(no_normal_map.basecolor,
                             vec3_new(0.2f, 0.4f, 0.6f)),
              "missing normal texture still paints basecolor");

        /* collections borrow textures, so the test releases both itself */
        decal_collection_release(stretched_normals);
        decal_collection_release(normals);
        texture_release(color_a);
        texture_release(color_b);
        texture_release(normal_tex);
    }
}

/* ------------------------------------------------------------------ */
/* test_pipeline: real rasterizer, shader, layers, models, shadow     */
/* ------------------------------------------------------------------ */

static void setup_uniforms(pbr_uniforms_t *u) {
    memset(u, 0, sizeof(pbr_uniforms_t));
    u->workflow = METALNESS_WORKFLOW;
    u->basecolor_factor = vec4_new(0.7f, 0.7f, 0.7f, 1);
    u->roughness_factor = 0.6f;
    u->camera_pos = vec3_new(0, 0, 3);
    u->camera_vp_matrix = mat4_identity();
    u->light_vp_matrix = mat4_identity();
    u->model_matrix = mat4_identity();
    u->normal_matrix = mat3_identity();
    u->layer_view = -1;
    /* without IBL a punctual light is the only non-black contribution */
    u->punctual_intensity = 1.0f;
    u->light_dir = vec3_new(0, 0, -1);
}

static void draw_triangle(program_t *program, framebuffer_t *framebuffer,
                          vec3_t normal) {
    pbr_attribs_t *attribs;
    vec3_t positions[3] = {
        vec3_new(-0.8f, -0.8f, 0.0f),
        vec3_new(0.8f, -0.8f, 0.0f),
        vec3_new(0.0f, 0.8f, 0.0f),
    };
    vec2_t texcoords[3] = {
        vec2_new(0, 0), vec2_new(1, 0), vec2_new(1, 1),
    };
    int j;
    for (j = 0; j < 3; j++) {
        attribs = (pbr_attribs_t*)program_get_attribs(program, j);
        attribs->position = positions[j];
        attribs->texcoord = texcoords[j];
        attribs->normal = normal;
        attribs->tangent = vec4_new(1, 0, 0, 1);
    }
    graphics_draw_triangle(framebuffer, program);
}

static program_t *make_program(void) {
    return program_create(pbr_vertex_shader, pbr_fragment_shader,
                          sizeof(pbr_attribs_t), sizeof(pbr_varyings_t),
                          sizeof(pbr_uniforms_t), 1, 0);
}

static unsigned char pixel_at(framebuffer_t *framebuffer, int x, int y,
                              int channel) {
    return framebuffer->color_buffer[(y * framebuffer->width + x) * 4 +
                                     channel];
}

void test_pipeline(Check *c) {
    framebuffer_t *reference = framebuffer_create(16, 16);
    framebuffer_t *decaled = framebuffer_create(16, 16);
    program_t *program = make_program();
    pbr_uniforms_t *u =
        (pbr_uniforms_t*)program_get_uniforms(program);
    texture_t *paint = make_color_texture(
        4, 4, vec4_new(0.9f, 0.1f, 0.1f, 1.0f));
    decal_collection_t *collection = decal_collection_create();
    vec4_t background = vec4_new(0, 0, 0, 1);
    int i;

    setup_uniforms(u);
    decal_collection_add(collection, mat4_identity(), paint, NULL,
                         0.05f, 0.95f, 0.0f);

    /* no decals: baseline output is preserved byte for byte */
    framebuffer_clear_color(reference, background);
    framebuffer_clear_depth(reference, 1);
    draw_triangle(program, reference, vec3_new(0, 0, 1));

    framebuffer_clear_color(decaled, background);
    framebuffer_clear_depth(decaled, 1);
    draw_triangle(program, decaled, vec3_new(0, 0, 1));
    for (i = 0; i < 16 * 16 * 4; i++) {
        check(c, reference->color_buffer[i] == decaled->color_buffer[i],
              "without decals the framebuffer must be identical");
    }

    /* same draw with a bound collection changes covered pixels only */
    u->decals = collection;
    framebuffer_clear_color(decaled, background);
    framebuffer_clear_depth(decaled, 1);
    draw_triangle(program, decaled, vec3_new(0, 0, 1));
    check(c, pixel_at(decaled, 8, 8, 0) != pixel_at(reference, 8, 8, 0),
          "covered center pixel should change color");
    check(c, pixel_at(decaled, 8, 8, 0) > pixel_at(decaled, 8, 8, 1),
          "painted center should be reddish");
    check(c, nearly(decaled->depth_buffer[8 * 16 + 8],
                    reference->depth_buffer[8 * 16 + 8]),
          "decals must not change depth");

    /* mesh UV seams cannot split a world-space projected decal */
    {
        framebuffer_t *seam_a = framebuffer_create(16, 16);
        framebuffer_t *seam_b = framebuffer_create(16, 16);
        texture_t *opaque = make_color_texture(
            4, 4, vec4_new(0.2f, 0.7f, 0.3f, 1.0f));
        pbr_attribs_t *attribs;
        int channel;

        u->basecolor_map = opaque;  /* makes mesh UVs matter to the base */
        framebuffer_clear_color(seam_a, background);
        framebuffer_clear_depth(seam_a, 1);
        draw_triangle(program, seam_a, vec3_new(0, 0, 1));

        /* same geometry, UVs shifted across an atlas seam */
        framebuffer_clear_color(seam_b, background);
        framebuffer_clear_depth(seam_b, 1);
        draw_triangle(program, seam_b, vec3_new(0, 0, 1));
        attribs = (pbr_attribs_t*)program_get_attribs(program, 0);
        attribs->texcoord = vec2_new(5.0f, 3.0f);
        attribs = (pbr_attribs_t*)program_get_attribs(program, 1);
        attribs->texcoord = vec2_new(6.0f, 3.0f);
        attribs = (pbr_attribs_t*)program_get_attribs(program, 2);
        attribs->texcoord = vec2_new(6.0f, 4.0f);
        draw_triangle(program, seam_b, vec3_new(0, 0, 1));
        for (channel = 0; channel < 3; channel++) {
            check(c, pixel_at(seam_a, 8, 8, channel) ==
                      pixel_at(seam_b, 8, 8, channel),
                  "fully covered decal must ignore mesh UV seams");
        }
        u->basecolor_map = NULL;
        framebuffer_release(seam_a);
        framebuffer_release(seam_b);
        texture_release(opaque);
    }

    /* a fragment alpha-cutoff discard cannot be resurrected by a decal */
    u->alpha_cutoff = 0.5f;
    u->basecolor_factor = vec4_new(0.7f, 0.7f, 0.7f, 0.0f);
    framebuffer_clear_color(decaled, background);
    framebuffer_clear_depth(decaled, 1);
    draw_triangle(program, decaled, vec3_new(0, 0, 1));
    check(c, pixel_at(decaled, 8, 8, 0) == 0 &&
              pixel_at(decaled, 8, 8, 1) == 0 &&
              pixel_at(decaled, 8, 8, 2) == 0,
          "alpha-cutout holes must stay discarded under decals");
    u->alpha_cutoff = 0.0f;
    u->basecolor_factor = vec4_new(0.7f, 0.7f, 0.7f, 1);

    /* the roughness layer preview sees the same decaled material */
    framebuffer_clear_color(decaled, background);
    framebuffer_clear_depth(decaled, 1);
    u->layer_view = 3;
    draw_triangle(program, decaled, vec3_new(0, 0, 1));
    framebuffer_clear_color(reference, background);
    framebuffer_clear_depth(reference, 1);
    u->layer_view = 3;
    u->decals = NULL;
    draw_triangle(program, reference, vec3_new(0, 0, 1));
    check(c, pixel_at(decaled, 8, 8, 0) != pixel_at(reference, 8, 8, 0),
          "roughness layer preview should show the decal roughness");
    u->layer_view = -1;
    u->decals = collection;

    /* specular workflow keeps its original output even with decals set */
    {
        framebuffer_t *pbrs_ref = framebuffer_create(16, 16);
        framebuffer_t *pbrs_dec = framebuffer_create(16, 16);
        u->workflow = SPECULAR_WORKFLOW;
        u->diffuse_factor = vec4_new(0.6f, 0.4f, 0.2f, 1);
        u->specular_factor = vec3_new(0.5f, 0.5f, 0.5f);
        u->glossiness_factor = 0.3f;
        framebuffer_clear_color(pbrs_ref, background);
        framebuffer_clear_depth(pbrs_ref, 1);
        u->decals = NULL;
        draw_triangle(program, pbrs_ref, vec3_new(0, 0, 1));
        framebuffer_clear_color(pbrs_dec, background);
        framebuffer_clear_depth(pbrs_dec, 1);
        u->decals = collection;
        draw_triangle(program, pbrs_dec, vec3_new(0, 0, 1));
        for (i = 0; i < 16 * 16 * 4; i++) {
            check(c, pbrs_ref->color_buffer[i] == pbrs_dec->color_buffer[i],
                  "specular workflow must ignore decals");
        }
        framebuffer_release(pbrs_ref);
        framebuffer_release(pbrs_dec);
        u->workflow = METALNESS_WORKFLOW;
        u->decals = collection;
    }

    /* shadow pass writes depth but never paints decals */
    {
        framebuffer_t *shadow_ref = framebuffer_create(16, 16);
        framebuffer_t *shadow_dec = framebuffer_create(16, 16);
        u->shadow_pass = 1;
        u->alpha_cutoff = 0.0f;
        framebuffer_clear_color(shadow_ref, background);
        framebuffer_clear_depth(shadow_ref, 1);
        u->decals = NULL;
        draw_triangle(program, shadow_ref, vec3_new(0, 0, 1));
        framebuffer_clear_color(shadow_dec, background);
        framebuffer_clear_depth(shadow_dec, 1);
        u->decals = collection;
        draw_triangle(program, shadow_dec, vec3_new(0, 0, 1));
        check(c, nearly(shadow_ref->depth_buffer[8 * 16 + 8],
                        shadow_dec->depth_buffer[8 * 16 + 8]),
              "shadow depth must be unchanged by decals");
        check(c, pixel_at(shadow_dec, 8, 8, 0) == 0,
              "shadow pass must not paint decal color");
        framebuffer_release(shadow_ref);
        framebuffer_release(shadow_dec);
        u->shadow_pass = 0;
    }

    program_release(program);
    framebuffer_release(reference);
    framebuffer_release(decaled);

    /* high-level entry: two models bound to two different collections */
    {
        pbrm_material_t material;
        model_t *model_a;
        model_t *model_b;
        decal_collection_t *coll_a = decal_collection_create();
        decal_collection_t *coll_b = decal_collection_create();
        texture_t *paint_b = make_color_texture(
            4, 4, vec4_new(0.1f, 0.2f, 0.9f, 1.0f));
        perframe_t perframe;
        framebuffer_t *fb_a = framebuffer_create(16, 16);
        framebuffer_t *fb_b = framebuffer_create(16, 16);
        pbr_uniforms_t *ua;
        pbr_uniforms_t *ub;

        memset(&material, 0, sizeof(material));
        material.basecolor_factor = vec4_new(0.7f, 0.7f, 0.7f, 1);
        material.roughness_factor = 0.6f;
        model_a = pbrm_create_model("assets/common/box.obj",
                                    mat4_identity(), NULL, -1, &material,
                                    NULL);
        model_b = pbrm_create_model("assets/common/box.obj",
                                    mat4_identity(), NULL, -1, &material,
                                    NULL);
        decal_collection_add(coll_a, mat4_identity(), paint, NULL,
                             0.05f, 0.95f, 0.0f);
        decal_collection_add(coll_b, mat4_identity(), paint_b, NULL,
                             0.95f, 0.05f, 0.0f);
        pbr_set_decals(model_a, coll_a);
        pbr_set_decals(model_b, coll_b);

        memset(&perframe, 0, sizeof(perframe));
        perframe.camera_pos = vec3_new(0, 0, 3);
        perframe.camera_view_matrix = mat4_identity();
        perframe.camera_proj_matrix = mat4_identity();
        perframe.light_view_matrix = mat4_identity();
        perframe.light_proj_matrix = mat4_identity();
        perframe.light_dir = vec3_new(0, 0, -1);
        model_a->update(model_a, &perframe);
        model_b->update(model_b, &perframe);

        ua = (pbr_uniforms_t*)program_get_uniforms(model_a->program);
        ub = (pbr_uniforms_t*)program_get_uniforms(model_b->program);
        check(c, ua->decals == coll_a && ub->decals == coll_b,
              "update must not clear the decal binding");

        framebuffer_clear_color(fb_a, background);
        framebuffer_clear_depth(fb_a, 1);
        framebuffer_clear_color(fb_b, background);
        framebuffer_clear_depth(fb_b, 1);
        model_a->draw(model_a, fb_a, 0);
        model_b->draw(model_b, fb_b, 0);
        check(c, pixel_at(fb_a, 8, 8, 0) > pixel_at(fb_a, 8, 8, 2),
              "model A should carry its own red collection");
        check(c, pixel_at(fb_b, 8, 8, 2) > pixel_at(fb_b, 8, 8, 0),
              "model B should carry its own blue collection");

        /* unbinding via NULL restores the plain material output */
        pbr_set_decals(model_a, NULL);
        check(c, ((pbr_uniforms_t*)program_get_uniforms(
                      model_a->program))->decals == NULL,
              "NULL should unbind the collection");

        framebuffer_release(fb_a);
        framebuffer_release(fb_b);
        model_a->release(model_a);
        model_b->release(model_b);
        texture_release(paint_b);
        decal_collection_release(coll_a);
        decal_collection_release(coll_b);
    }

    texture_release(paint);
    decal_collection_release(collection);
}
