/*
 * projected_decals: renders two quads facing different directions and
 * projects a paint mark onto both of them with a world-space decal box,
 * then writes the result as a binary PPM to the path in argv[1].
 *
 * Ownership: this program creates the decal collection and both decal
 * textures, and keeps owning them for its whole lifetime. The collection
 * only borrows the textures (decal_collection_release never frees them),
 * and the shader program only borrows the collection while it is bound
 * through uniforms->decals (the high-level pbr_set_decals(model, ...)
 * entry binds the same way and never releases the collection either).
 * The collection must stay alive and read-only whenever it is bound;
 * here everything is released at the end, textures last.
 */
#include "renderer/core/api.h"
#include "renderer/core/decal.h"
#include "renderer/shaders/pbr_shader.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 400
#define HEIGHT 400

static texture_t *create_paint_texture(void) {
    /* an orange paint stripe with fully transparent surroundings */
    texture_t *texture = texture_create(64, 64);
    int x, y;
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            float u = (float)x / 63.0f;
            float v = (float)y / 63.0f;
            float du = fabsf(u - 0.5f);
            float dv = fabsf(v - 0.5f);
            vec4_t texel = vec4_new(0, 0, 0, 0);
            if (dv < 0.16f && du < 0.42f) {
                texel = vec4_new(1.0f, 0.35f, 0.05f, 1.0f);
            } else if (du < 0.10f && dv < 0.42f) {
                texel = vec4_new(0.9f, 0.9f, 0.95f, 0.85f);
            }
            texture->buffer[y * 64 + x] = texel;
        }
    }
    return texture;
}

static texture_t *create_normal_texture(void) {
    /* a gentle dome bump inside the stripe, flat elsewhere */
    texture_t *texture = texture_create(64, 64);
    int x, y;
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            float u = (float)x / 63.0f - 0.5f;
            float v = (float)y / 63.0f - 0.5f;
            vec3_t n = vec3_normalize(vec3_new(-u * 1.5f, -v * 1.5f, 1));
            texture->buffer[y * 64 + x] = vec4_new(n.x * 0.5f + 0.5f,
                                                   n.y * 0.5f + 0.5f,
                                                   n.z * 0.5f + 0.5f, 1);
        }
    }
    return texture;
}

static void draw_quad(framebuffer_t *framebuffer, program_t *program,
                      vec3_t center, vec3_t axis_u, vec3_t axis_v,
                      vec3_t normal) {
    vec3_t corners[4];
    int indices[6] = {0, 1, 2, 0, 2, 3};
    int i, j;
    corners[0] = vec3_sub(vec3_sub(center, axis_u), axis_v);
    corners[1] = vec3_sub(vec3_add(center, axis_u), axis_v);
    corners[2] = vec3_add(vec3_add(center, axis_u), axis_v);
    corners[3] = vec3_add(vec3_sub(center, axis_u), axis_v);
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 3; j++) {
            vec3_t position = corners[indices[i * 3 + j]];
            pbr_attribs_t *attribs;
            attribs = (pbr_attribs_t*)program_get_attribs(program, j);
            memset(attribs, 0, sizeof(pbr_attribs_t));
            attribs->position = position;
            attribs->normal = normal;
        }
        graphics_draw_triangle(framebuffer, program);
    }
}

static int write_ppm(const char *path, framebuffer_t *framebuffer) {
    FILE *file = fopen(path, "wb");
    int x, y;
    if (file == NULL) {
        return -1;
    }
    fprintf(file, "P6\n%d %d\n255\n", framebuffer->width,
            framebuffer->height);
    for (y = framebuffer->height - 1; y >= 0; y--) {
        for (x = 0; x < framebuffer->width; x++) {
            unsigned char *pixel;
            pixel = &framebuffer->color_buffer[(y * framebuffer->width + x)
                                               * 4];
            fwrite(pixel, 1, 3, file);
        }
    }
    fclose(file);
    return 0;
}

int main(int argc, char **argv) {
    const char *output = argc > 1 ? argv[1] : "projected_decals.ppm";
    framebuffer_t *framebuffer = framebuffer_create(WIDTH, HEIGHT);
    program_t *program = program_create(pbr_vertex_shader,
                                        pbr_fragment_shader,
                                        sizeof(pbr_attribs_t),
                                        sizeof(pbr_varyings_t),
                                        sizeof(pbr_uniforms_t), 1, 0);
    pbr_uniforms_t *uniforms;
    mat4_t view, proj;
    quat_t box_rotation;
    float tilt = TO_RADIANS(50);

    /* the decal collection and its textures belong to this program */
    texture_t *paint = create_paint_texture();
    texture_t *bump = create_normal_texture();
    decal_collection_t *decals = decal_collection_create();
    box_rotation = quat_new(0, (float)sin(TO_RADIANS(25) / 2), 0,
                            (float)cos(TO_RADIANS(25) / 2));
    if (decal_collection_add(decals,
                             mat4_from_trs(vec3_new(0.05f, 0, 0.6f),
                                           box_rotation,
                                           vec3_new(2.6f, 1.4f, 3.0f)),
                             paint, bump, 0.85f, 0.0f, 0.3f) != 0) {
        fprintf(stderr, "failed to configure the decal\n");
        return 1;
    }

    uniforms = (pbr_uniforms_t*)program_get_uniforms(program);
    memset(uniforms, 0, sizeof(pbr_uniforms_t));
    uniforms->workflow = METALNESS_WORKFLOW;
    uniforms->basecolor_factor = vec4_new(0.75f, 0.75f, 0.78f, 1);
    uniforms->metalness_factor = 0.05f;
    uniforms->roughness_factor = 0.5f;
    uniforms->model_matrix = mat4_identity();
    uniforms->normal_matrix = mat3_identity();
    view = mat4_lookat(vec3_new(0, 0.4f, 4.2f), vec3_new(0, 0, 0),
                       vec3_new(0, 1, 0));
    proj = mat4_perspective(TO_RADIANS(40), 1, 1, 10);
    uniforms->camera_vp_matrix = mat4_mul_mat4(proj, view);
    uniforms->camera_pos = vec3_new(0, 0.4f, 4.2f);
    uniforms->light_dir = vec3_normalize(vec3_new(0.3f, 0.5f, -1));
    uniforms->punctual_intensity = 1.5f;
    uniforms->layer_view = -1;
    /* the program borrows the collection while it is bound */
    uniforms->decals = decals;

    framebuffer_clear_color(framebuffer, vec4_new(0.1f, 0.1f, 0.12f, 1));
    framebuffer_clear_depth(framebuffer, 1);

    /* a quad facing the camera */
    draw_quad(framebuffer, program, vec3_new(-0.75f, 0, 0),
              vec3_new(0.85f, 0, 0), vec3_new(0, 0.85f, 0),
              vec3_new(0, 0, 1));
    /* a second quad tilted 50 degrees away, further back */
    draw_quad(framebuffer, program, vec3_new(0.85f, 0, -0.4f),
              vec3_mul(vec3_new((float)cos(tilt), 0,
                                -(float)sin(tilt)), 0.85f),
              vec3_new(0, 0.85f, 0),
              vec3_new((float)sin(tilt), 0, (float)cos(tilt)));

    if (write_ppm(output, framebuffer) != 0) {
        fprintf(stderr, "cannot write %s\n", output);
        return 1;
    }
    printf("wrote %s\n", output);

    /* release order: program first, then what the program borrowed */
    program_release(program);
    framebuffer_release(framebuffer);
    decal_collection_release(decals);
    texture_release(paint);
    texture_release(bump);
    return 0;
}
