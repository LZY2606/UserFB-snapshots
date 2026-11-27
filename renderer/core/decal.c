#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "darray.h"
#include "decal.h"
#include "macro.h"
#include "maths.h"

struct decal_collection {
    decal_t *decals;
};

decal_collection_t *decal_collection_create(void) {
    decal_collection_t *collection =
        (decal_collection_t*)malloc(sizeof(decal_collection_t));
    collection->decals = NULL;
    return collection;
}

void decal_collection_release(decal_collection_t *collection) {
    if (collection != NULL) {
        /* textures are borrowed, only the stored entries are owned here */
        darray_free(collection->decals);
        free(collection);
    }
}

int decal_collection_size(decal_collection_t *collection) {
    return darray_size(collection->decals);
}

const decal_t *decal_collection_get(decal_collection_t *collection,
                                    int index) {
    return &collection->decals[index];
}

static float mat3_determinant_of(mat4_t m) {
    float a = m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]);
    float b = m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]);
    float c = m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
    return a - b + c;
}

/*
 * a legal decal transform is T * R * S with strictly positive, possibly
 * non-uniform, scales. its upper 3x3 therefore has three mutually
 * orthogonal columns whose normalized versions keep a right-handed,
 * orthonormal frame.
 */
static int validate_frame(mat4_t local_to_world, vec3_t axes[3]) {
    vec3_t c0 = vec3_new(local_to_world.m[0][0],
                         local_to_world.m[1][0],
                         local_to_world.m[2][0]);
    vec3_t c1 = vec3_new(local_to_world.m[0][1],
                         local_to_world.m[1][1],
                         local_to_world.m[2][1]);
    vec3_t c2 = vec3_new(local_to_world.m[0][2],
                         local_to_world.m[1][2],
                         local_to_world.m[2][2]);
    float l0 = vec3_length(c0);
    float l1 = vec3_length(c1);
    float l2 = vec3_length(c2);
    vec3_t r0, r1, r2, handed;
    float tol;

    if (l0 < EPSILON || l1 < EPSILON || l2 < EPSILON) {
        return 0;  /* singular */
    }
    tol = EPSILON * float_max(float_max(l0 * l1, l0 * l2), l1 * l2);
    if (fabsf(vec3_dot(c0, c1)) > tol ||
        fabsf(vec3_dot(c0, c2)) > tol ||
        fabsf(vec3_dot(c1, c2)) > tol) {
        return 0;  /* sheared */
    }
    r0 = vec3_div(c0, l0);
    r1 = vec3_div(c1, l1);
    r2 = vec3_div(c2, l2);
    handed = vec3_cross(r0, r1);
    if (fabsf(handed.x - r2.x) > EPSILON ||
        fabsf(handed.y - r2.y) > EPSILON ||
        fabsf(handed.z - r2.z) > EPSILON) {
        return 0;  /* reflected */
    }
    if (fabsf(local_to_world.m[3][0]) > EPSILON ||
        fabsf(local_to_world.m[3][1]) > EPSILON ||
        fabsf(local_to_world.m[3][2]) > EPSILON ||
        fabsf(local_to_world.m[3][3] - 1.0f) > EPSILON) {
        return 0;  /* not an affine rigid-frame transform */
    }
    axes[0] = r0;
    axes[1] = r1;
    axes[2] = r2;
    return 1;
}

int decal_collection_add(decal_collection_t *collection,
                         mat4_t local_to_world,
                         texture_t *color_map, texture_t *normal_map,
                         float roughness, float metalness, float angle_cos) {
    decal_t decal;
    vec3_t axes[3];
    float determinant;

    if (color_map == NULL) {
        fprintf(stderr, "decal: color texture is required\n");
        return -1;
    }
    determinant = mat3_determinant_of(local_to_world);
    if (fabsf(determinant) < EPSILON) {
        fprintf(stderr, "decal: singular local_to_world transform\n");
        return -1;
    }
    if (!validate_frame(local_to_world, axes)) {
        fprintf(stderr,
                "decal: local_to_world must be T*R*S with positive scale, "
                "no shear or reflection\n");
        return -1;
    }

    memset(&decal, 0, sizeof(decal_t));
    decal.local_to_world = local_to_world;
    decal.world_to_local = mat4_inverse(local_to_world);
    decal.axis_x = axes[0];
    decal.axis_y = axes[1];
    decal.axis_z = axes[2];
    decal.color_map = color_map;
    decal.normal_map = normal_map;
    decal.roughness = float_clamp(roughness, 0, 1);
    decal.metalness = float_clamp(metalness, 0, 1);
    decal.angle_cos = float_clamp(angle_cos, -1, 1);

    darray_push(collection->decals, decal);
    return decal_collection_size(collection) - 1;
}

static int inside_closed_box(vec3_t local) {
    return fabsf(local.x) <= 0.5f &&
           fabsf(local.y) <= 0.5f &&
           fabsf(local.z) <= 0.5f;
}

static int inside_uv_range(vec2_t uv) {
    return uv.x >= 0.0f && uv.x <= 1.0f &&
           uv.y >= 0.0f && uv.y <= 1.0f;
}

void decal_collection_apply(decal_collection_t *collection,
                            vec3_t world_position, vec3_t base_normal,
                            decal_surface_t *surface) {
    int num_decals = decal_collection_size(collection);
    vec3_t normal_dir = vec3_normalize(base_normal);
    int i;

    for (i = 0; i < num_decals; i++) {
        const decal_t *decal = &collection->decals[i];
        vec4_t world_h = vec4_from_vec3(world_position, 1);
        vec4_t local_h = mat4_mul_vec4(decal->world_to_local, world_h);
        vec3_t local = vec3_from_vec4(local_h);
        vec2_t uv;
        vec4_t color_sample;
        float weight;

        if (!inside_closed_box(local)) {
            continue;
        }
        uv = vec2_new(local.x + 0.5f, local.y + 0.5f);
        if (!inside_uv_range(uv)) {
            continue;  /* never let texture repeat paint outside the box */
        }
        if (vec3_dot(normal_dir, decal->axis_z) + EPSILON <
            decal->angle_cos) {
            continue;  /* back-facing or too oblique for this decal */
        }

        /* range was checked above, clamp sampling is belt-and-braces */
        color_sample = texture_clamp_sample(decal->color_map, uv);
        weight = float_saturate(color_sample.w);
        if (weight <= 0.0f) {
            continue;
        }

        /*
         * basecolor/roughness/metalness blend in linear space; the
         * diffuse/specular split happens afterwards in the PBR shader.
         */
        surface->basecolor = vec3_lerp(surface->basecolor,
                                       vec3_from_vec4(color_sample),
                                       weight);
        surface->roughness = float_lerp(surface->roughness,
                                        decal->roughness, weight);
        surface->metalness = float_lerp(surface->metalness,
                                        decal->metalness, weight);
        surface->roughness = float_clamp(surface->roughness, 0, 1);
        surface->metalness = float_clamp(surface->metalness, 0, 1);

        if (decal->normal_map) {
            vec4_t normal_sample = texture_clamp_sample(decal->normal_map,
                                                        uv);
            vec3_t tangent_normal = vec3_new(normal_sample.x * 2 - 1,
                                             normal_sample.y * 2 - 1,
                                             normal_sample.z * 2 - 1);
            vec3_t world_normal;

            tangent_normal = vec3_normalize(tangent_normal);
            /*
             * the decal frame is orthonormal, so non-uniform box scales
             * cannot skew the mapped normal
             */
            world_normal = vec3_add(
                vec3_add(vec3_mul(decal->axis_x, tangent_normal.x),
                         vec3_mul(decal->axis_y, tangent_normal.y)),
                vec3_mul(decal->axis_z, tangent_normal.z));
            world_normal = vec3_normalize(world_normal);
            surface->normal = vec3_normalize(
                vec3_lerp(surface->normal, world_normal, weight));
        }
    }
}
