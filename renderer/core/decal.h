#ifndef DECAL_H
#define DECAL_H

#include "maths.h"
#include "texture.h"

/*
 * Projected decals: each decal is a closed box [-.5,.5]^3 in its local
 * space, projected along local -Z with UV = (x + .5, y + .5). The box is
 * placed in the world with local_to_world, which may combine translation,
 * rotation, and positive non-uniform scale; shear and reflection are
 * rejected when the decal is configured.
 *
 * A decal collection is created, configured, and released explicitly.
 * The collection stores copies of the decal parameters, but it does NOT
 * take ownership of the textures: the caller must keep color_map and
 * normal_map alive for as long as the collection may be evaluated, and
 * releases them separately. A collection is read-only while it is bound
 * for drawing.
 */

typedef struct {
    /* configuration, see decal_collection_add */
    mat4_t local_to_world;
    texture_t *color_map;       /* RGBA, alpha is the blend weight */
    texture_t *normal_map;      /* optional tangent-space normal map */
    float roughness;
    float metalness;
    float angle_cos;
    /* derived at configuration time */
    mat4_t world_to_local;      /* inverse of local_to_world */
    vec3_t axis_x;              /* orthonormal world frame of the box, */
    vec3_t axis_y;              /* unaffected by the scale magnitudes */
    vec3_t axis_z;
} decal_t;

typedef struct {
    decal_t *decals;
    int num_decals;
} decal_collection_t;

/* creates an empty collection */
decal_collection_t *decal_collection_create(void);

/*
 * appends a decal; returns 0 on success and a nonzero value (without
 * adding anything) if the transform is singular, sheared, reflected,
 * or not affine, or if color_map is NULL
 */
int decal_collection_add(decal_collection_t *collection,
                         mat4_t local_to_world,
                         texture_t *color_map, texture_t *normal_map,
                         float roughness, float metalness, float angle_cos);

/* releases the collection itself; the textures stay with the caller */
void decal_collection_release(decal_collection_t *collection);

/*
 * evaluates every decal of the collection in collection order against a
 * surface point; basecolor/roughness/metalness are blended in linear
 * space and normal (the current world shading normal) is blended and
 * renormalized per layer. base_normal is the normalized interpolated
 * world normal before any normal mapping or backface flip; a layer is
 * skipped when the point is outside the box or when
 * dot(base_normal, axis_z) < angle_cos.
 */
void decal_collection_apply(decal_collection_t *collection,
                            vec3_t world_position, vec3_t base_normal,
                            vec3_t *basecolor, float *roughness,
                            float *metalness, vec3_t *normal);

#endif
