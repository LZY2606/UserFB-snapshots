#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "renderer/core/api.h"
#include "renderer/core/decal.h"
#include "renderer/shaders/pbr_shader.h"

/*
 * projected decals demo (headless, no window).
 *
 * two single-triangle faces are drawn through the real PBR pipeline:
 * face A faces +Z, face B is rotated 45 degrees about Y. one decal
 * collection holds two paint decals, one oriented box per face, so the
 * paint follows world-space projection instead of mesh UVs.
 *
 * ownership:
 *   main() owns every texture_t and the decal_collection_t. the models
 *   only borrow them through pbr_set_decals(); the collection and its
 *   textures must stay alive until all draws finish, then main releases
 *   textures, collection and models in that order. no global state is
 *   involved.
 *
 * usage: projected_decals output.ppm
 */

#define IMAGE_SIZE 96

typedef struct {
    vec3_t positions[3];
    vec3_t normal;
    mat4_t model_matrix;
} face_t;

static texture_t *make_paint_texture(vec3_t color) {
    /* radial marker: opaque center, soft alpha falloff toward the edge */
    int size = 32;
    texture_t *texture = texture_create(size, size);
    int r, c;
    for (r = 0; r < size; r++) {
        for (c = 0; c < size; c++) {
            float dx = (c + 0.5f) / size * 2.0f - 1.0f;
            float dy = (r + 0.5f) / size * 2.0f - 1.0f;
            float radius = sqrtf(dx * dx + dy * dy);
            float alpha = float_saturate(1.0f - radius);
            texture->buffer[r * size + c] =
                vec4_new(color.x, color.y, color.z, alpha);
        }
    }
    return texture;
}

static program_t *make_pbr_program(void) {
    return program_create(pbr_vertex_shader, pbr_fragment_shader,
                          sizeof(pbr_attribs_t), sizeof(pbr_varyings_t),
                          sizeof(pbr_uniforms_t), 1, 0);
}

static void draw_face(framebuffer_t *framebuffer, program_t *program,
                      const face_t *face) {
    pbr_uniforms_t *uniforms =
        (pbr_uniforms_t*)program_get_uniforms(program);
    pbr_attribs_t *attribs;
    vec2_t texcoords[3] = {
        vec2_new(0, 0), vec2_new(1, 0), vec2_new(1, 1),
    };
    int j;

    uniforms->model_matrix = face->model_matrix;
    uniforms->normal_matrix =
        mat3_inverse_transpose(mat3_from_mat4(face->model_matrix));
    for (j = 0; j < 3; j++) {
        attribs = (pbr_attribs_t*)program_get_attribs(program, j);
        attribs->position = face->positions[j];
        attribs->texcoord = texcoords[j];
        attribs->normal = face->normal;
        attribs->tangent = vec4_new(1, 0, 0, 1);
    }
    graphics_draw_triangle(framebuffer, program);
}

static void write_ppm(const char *filename, framebuffer_t *framebuffer) {
    FILE *file = fopen(filename, "wb");
    int i;
    if (file == NULL) {
        fprintf(stderr, "cannot open %s\n", filename);
        exit(1);
    }
    fprintf(file, "P6\n%d %d\n255\n", framebuffer->width, framebuffer->height);
    for (i = 0; i < framebuffer->width * framebuffer->height; i++) {
        unsigned char rgb[3];
        rgb[0] = framebuffer->color_buffer[i * 4 + 0];
        rgb[1] = framebuffer->color_buffer[i * 4 + 1];
        rgb[2] = framebuffer->color_buffer[i * 4 + 2];
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

int main(int argc, char *argv[]) {
    framebuffer_t *framebuffer;
    program_t *program;
    pbr_uniforms_t *uniforms;
    decal_collection_t *collection;
    texture_t *red_paint;
    texture_t *blue_paint;
    face_t face_a, face_b;
    mat4_t view_matrix, proj_matrix;
    quat_t tilt;
    const char *output;

    if (argc < 2) {
        fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
        return 1;
    }
    output = argv[1];

    framebuffer = framebuffer_create(IMAGE_SIZE, IMAGE_SIZE);
    framebuffer_clear_color(framebuffer, vec4_new(0.05f, 0.05f, 0.05f, 1));
    framebuffer_clear_depth(framebuffer, 1);

    program = make_pbr_program();
    uniforms = (pbr_uniforms_t*)program_get_uniforms(program);
    memset(uniforms, 0, sizeof(pbr_uniforms_t));
    uniforms->workflow = METALNESS_WORKFLOW;
    uniforms->basecolor_factor = vec4_new(0.75f, 0.75f, 0.75f, 1);
    uniforms->roughness_factor = 0.6f;
    uniforms->metalness_factor = 0.0f;
    uniforms->punctual_intensity = 1.0f;
    uniforms->light_dir = vec3_new(0, 0, -1);  /* light shines along +Z */
    uniforms->camera_pos = vec3_new(0, 0, 4);
    view_matrix = mat4_lookat(vec3_new(0, 0, 4), vec3_new(0, 0, 0),
                              vec3_new(0, 1, 0));
    proj_matrix = mat4_ortho(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 10.0f);
    uniforms->camera_vp_matrix = mat4_mul_mat4(proj_matrix, view_matrix);
    uniforms->light_vp_matrix = uniforms->camera_vp_matrix;
    uniforms->layer_view = -1;

    /* face A: upright, facing +Z */
    face_a.positions[0] = vec3_new(-0.7f, -0.7f, 0);
    face_a.positions[1] = vec3_new(0.7f, -0.7f, 0);
    face_a.positions[2] = vec3_new(0.0f, 0.7f, 0);
    face_a.normal = vec3_new(0, 0, 1);
    face_a.model_matrix = mat4_translate(-1.05f, 0, 0);

    /* face B: same triangle rotated 45 degrees about Y */
    face_b.positions[0] = vec3_new(-0.7f, -0.7f, 0);
    face_b.positions[1] = vec3_new(0.7f, -0.7f, 0);
    face_b.positions[2] = vec3_new(0.0f, 0.7f, 0);
    face_b.normal = vec3_new(0, 0, 1);
    tilt = quat_normalize(quat_new(
        0.0f, sinf(TO_RADIANS(45) * 0.5f), 0.0f,
        cosf(TO_RADIANS(45) * 0.5f)));
    face_b.model_matrix = mat4_mul_mat4(
        mat4_translate(1.05f, 0, 0),
        mat4_from_quat(tilt));

    /*
     * the collection and its textures are owned here; the shader only
     * holds a borrowed pointer for the duration of the draws.
     */
    red_paint = make_paint_texture(vec3_new(0.9f, 0.1f, 0.1f));
    blue_paint = make_paint_texture(vec3_new(0.1f, 0.2f, 0.9f));
    collection = decal_collection_create();
    decal_collection_add(
        collection,
        mat4_from_trs(vec3_new(-1.05f, 0, 0), quat_new(0, 0, 0, 1),
                      vec3_new(1.7f, 1.7f, 0.4f)),
        red_paint, NULL, 0.05f, 0.9f, 0.0f);
    decal_collection_add(
        collection,
        mat4_from_trs(vec3_new(1.05f, 0, 0), tilt,
                      vec3_new(1.7f, 1.7f, 0.4f)),
        blue_paint, NULL, 0.9f, 0.1f, 0.0f);
    uniforms->decals = collection;

    draw_face(framebuffer, program, &face_a);
    draw_face(framebuffer, program, &face_b);

    write_ppm(output, framebuffer);
    printf("wrote %s: two %d-degree-offset faces with projected decals\n",
           output, 45);

    /* release in reverse ownership order: bindings first, owners last */
    uniforms->decals = NULL;
    decal_collection_release(collection);
    texture_release(red_paint);
    texture_release(blue_paint);
    program_release(program);
    framebuffer_release(framebuffer);
    return 0;
}
