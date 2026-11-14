#include <math.h>
#include <stdlib.h>
#include "darray.h"
#include "graphics.h"
#include "maths.h"
#include "mesh.h"
#include "scene.h"
#include "skeleton.h"
#include "texture.h"

scene_t *scene_create(vec3_t background, model_t *skybox, model_t **models,
                      float ambient_intensity, float punctual_intensity,
                      int shadow_width, int shadow_height) {
    scene_t *scene = (scene_t*)malloc(sizeof(scene_t));
    scene->background = vec4_from_vec3(background, 1);
    scene->skybox = skybox;
    scene->models = models;
    scene->ambient_intensity = ambient_intensity;
    scene->punctual_intensity = punctual_intensity;
    if (shadow_width > 0 && shadow_height > 0) {
        scene->shadow_buffer = framebuffer_create(shadow_width, shadow_height);
        scene->shadow_map = texture_create(shadow_width, shadow_height);
    } else {
        scene->shadow_buffer = NULL;
        scene->shadow_map = NULL;
    }
    scene->point_shadow = NULL;
    return scene;
}

int scene_set_point_light(scene_t *scene, vec3_t position, float near,
                          float far, int size, float bias) {
    point_shadow_t *point_shadow;

    if (size == 0) {
        if (scene->point_shadow) {
            point_shadow_release(scene->point_shadow);
            scene->point_shadow = NULL;
        }
        return 1;
    }

    if (size < 0 || near <= 0 || far <= near || bias < 0 ||
        !isfinite(position.x) || !isfinite(position.y) ||
        !isfinite(position.z) || !isfinite(near) || !isfinite(far) ||
        !isfinite(bias)) {
        return 0;
    }

    point_shadow = point_shadow_create(position, near, far, size, bias);
    if (point_shadow == NULL) {
        return 0;
    }
    if (scene->point_shadow) {
        point_shadow_release(scene->point_shadow);
    }
    scene->point_shadow = point_shadow;
    return 1;
}

void scene_release(scene_t *scene) {
    int num_models = darray_size(scene->models);
    int i;
    if (scene->skybox) {
        model_t *skybox = scene->skybox;
        skybox->release(skybox);
    }
    for (i = 0; i < num_models; i++) {
        model_t *model = scene->models[i];
        model->release(model);
    }
    darray_free(scene->models);
    if (scene->shadow_buffer) {
        framebuffer_release(scene->shadow_buffer);
    }
    if (scene->shadow_map) {
        texture_release(scene->shadow_map);
    }
    if (scene->point_shadow) {
        point_shadow_release(scene->point_shadow);
    }
    free(scene);
}
