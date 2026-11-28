#ifndef SCENE_H
#define SCENE_H

#include "graphics.h"
#include "maths.h"
#include "mesh.h"
#include "skeleton.h"
#include "texture.h"

struct point_shadow_t;

typedef struct {
    float frame_time;
    float delta_time;
    vec3_t light_dir;
    vec3_t camera_pos;
    mat4_t light_view_matrix;
    mat4_t light_proj_matrix;
    mat4_t camera_view_matrix;
    mat4_t camera_proj_matrix;
    float ambient_intensity;
    float punctual_intensity;
    texture_t *shadow_map;
    struct point_shadow_t *point_shadow;
    int layer_view;
} perframe_t;

typedef struct model {
    mesh_t *mesh;
    program_t *program;
    mat4_t transform;
    /* for animation */
    skeleton_t *skeleton;
    int attached;
    /* for sorting */
    int opaque;
    float distance;
    /* polymorphism */
    void (*update)(struct model *model, perframe_t *perframe);
    void (*draw)(struct model *model, framebuffer_t *framebuffer,
                 int shadow_pass);
    void (*release)(struct model *model);
} model_t;

typedef struct {
    vec4_t background;
    model_t *skybox;
    model_t **models;
    /* light intensity */
    float ambient_intensity;
    float punctual_intensity;
    /* shadow mapping */
    framebuffer_t *shadow_buffer;
    texture_t *shadow_map;
    /* point light shadow */
    struct point_shadow_t *point_shadow;
} scene_t;

scene_t *scene_create(vec3_t background, model_t *skybox, model_t **models,
                      float ambient_intensity, float punctual_intensity,
                      int shadow_width, int shadow_height);
void scene_release(scene_t *scene);

/*
 * configures (or disables when size is 0) the optional point light shadow.
 * size < 0 is always invalid. for size > 0, near must be > 0, far > near,
 * bias >= 0 and all floating point arguments finite.
 * returns 1 on success, leaving the previous configuration untouched and
 * returning 0 on invalid arguments.
 */
int scene_set_point_light(scene_t *scene, vec3_t position, float near,
                          float far, int size, float bias);

#endif
