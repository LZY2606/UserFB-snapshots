#include <math.h>
#include <stdlib.h>
#include "decal.h"
#include "macro.h"

/* the box axes must be orthogonal to this relative tolerance */
#define SHEAR_TOLERANCE 1e-4f

decal_collection_t *decal_collection_create(void) {
    decal_collection_t *collection;

    collection = (decal_collection_t*)malloc(sizeof(decal_collection_t));
    collection->decals = NULL;
    collection->num_decals = 0;

    return collection;
}

static vec3_t get_basis_column(mat4_t m, int column) {
    return vec3_new(m.m[0][column], m.m[1][column], m.m[2][column]);
}

static int is_valid_transform(mat4_t m, vec3_t *axis_x, vec3_t *axis_y,
                              vec3_t *axis_z) {
    vec3_t column_x = get_basis_column(m, 0);
    vec3_t column_y = get_basis_column(m, 1);
    vec3_t column_z = get_basis_column(m, 2);
    float length_x = vec3_length(column_x);
    float length_y = vec3_length(column_y);
    float length_z = vec3_length(column_z);
    float tolerance;

    /* must be an affine transform */
    if (m.m[3][0] != 0 || m.m[3][1] != 0 || m.m[3][2] != 0 ||
        m.m[3][3] != 1) {
        return 0;
    }
    /* positive scales only: no singular or mirrored axes */
    if (length_x < EPSILON || length_y < EPSILON || length_z < EPSILON) {
        return 0;
    }
    /* no shear: the basis vectors must be mutually orthogonal */
    tolerance = SHEAR_TOLERANCE * length_x * length_y;
    if ((float)fabs(vec3_dot(column_x, column_y)) > tolerance) {
        return 0;
    }
    tolerance = SHEAR_TOLERANCE * length_y * length_z;
    if ((float)fabs(vec3_dot(column_y, column_z)) > tolerance) {
        return 0;
    }
    tolerance = SHEAR_TOLERANCE * length_z * length_x;
    if ((float)fabs(vec3_dot(column_z, column_x)) > tolerance) {
        return 0;
    }
    /* no reflection: the basis must stay right-handed */
    if (vec3_dot(vec3_cross(column_x, column_y), column_z) <= 0) {
        return 0;
    }

    *axis_x = vec3_div(column_x, length_x);
    *axis_y = vec3_div(column_y, length_y);
    *axis_z = vec3_div(column_z, length_z);
    return 1;
}

int decal_collection_add(decal_collection_t *collection,
                         mat4_t local_to_world,
                         texture_t *color_map, texture_t *normal_map,
                         float roughness, float metalness, float angle_cos) {
    decal_t decal;
    decal_t *decals;

    if (color_map == NULL) {
        return 1;
    }
    if (!is_valid_transform(local_to_world,
                            &decal.axis_x, &decal.axis_y, &decal.axis_z)) {
        return 1;
    }

    decal.local_to_world = local_to_world;
    decal.world_to_local = mat4_inverse(local_to_world);
    decal.color_map = color_map;
    decal.normal_map = normal_map;
    decal.roughness = roughness;
    decal.metalness = metalness;
    decal.angle_cos = angle_cos;

    decals = (decal_t*)realloc(collection->decals,
                               sizeof(decal_t) * (collection->num_decals + 1));
    if (decals == NULL) {
        return 1;
    }
    collection->decals = decals;
    collection->decals[collection->num_decals] = decal;
    collection->num_decals += 1;

    return 0;
}

void decal_collection_release(decal_collection_t *collection) {
    free(collection->decals);
    free(collection);
}

static void apply_decal(decal_t *decal, vec3_t world_position,
                        vec3_t base_normal, vec3_t *basecolor,
                        float *roughness, float *metalness, vec3_t *normal) {
    vec4_t world_point = vec4_from_vec3(world_position, 1);
    vec3_t local = vec3_from_vec4(mat4_mul_vec4(decal->world_to_local,
                                                world_point));
    vec2_t texcoord;
    vec4_t color;
    float weight;

    /* closed box test: outside layers never contribute, and since the
     * texcoord is known to be inside [0,1] here, the repeat sampling of
     * texture_t can never wrap around and paint surfaces outside the box */
    if ((float)fabs(local.x) > 0.5f || (float)fabs(local.y) > 0.5f ||
        (float)fabs(local.z) > 0.5f) {
        return;
    }
    /* the base normal decides whether the surface faces the projection */
    if (vec3_dot(base_normal, decal->axis_z) < decal->angle_cos) {
        return;
    }

    texcoord = vec2_new(local.x + 0.5f, local.y + 0.5f);
    color = texture_sample(decal->color_map, texcoord);
    weight = color.w;
    if (weight <= 0) {
        return;
    }

    *basecolor = vec3_lerp(*basecolor, vec3_from_vec4(color), weight);
    *roughness = float_lerp(*roughness, decal->roughness, weight);
    *metalness = float_lerp(*metalness, decal->metalness, weight);

    if (decal->normal_map) {
        vec4_t sample = texture_sample(decal->normal_map, texcoord);
        vec3_t tangent_normal = vec3_new(sample.x * 2 - 1,
                                         sample.y * 2 - 1,
                                         sample.z * 2 - 1);
        /* the orthonormal frame carries the normal to world space, so a
         * non-uniformly scaled box never skews the decal normals */
        mat3_t frame = mat3_from_cols(decal->axis_x, decal->axis_y,
                                      decal->axis_z);
        vec3_t world_normal = mat3_mul_vec3(frame,
                                            vec3_normalize(tangent_normal));
        *normal = vec3_normalize(vec3_lerp(*normal, world_normal, weight));
    }
}

void decal_collection_apply(decal_collection_t *collection,
                            vec3_t world_position, vec3_t base_normal,
                            vec3_t *basecolor, float *roughness,
                            float *metalness, vec3_t *normal) {
    int i;
    for (i = 0; i < collection->num_decals; i++) {
        apply_decal(&collection->decals[i], world_position, base_normal,
                    basecolor, roughness, metalness, normal);
    }
}
