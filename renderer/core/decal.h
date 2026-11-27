#ifndef DECAL_H
#define DECAL_H

#include "maths.h"
#include "texture.h"

/*
 * projected decals
 *
 * each decal owns a local box [-0.5, 0.5]^3 that projects along its
 * local -Z axis. the decal UV is (local_x + 0.5, local_y + 0.5), so a
 * fragment is painted from its world position only: mesh UV seams never
 * split a decal and no texture has to be rebaked per model.
 *
 * local_to_world must be a translation * rotation * positive scale
 * composition (see mat4_from_trs). shear, reflection and singular
 * transforms are rejected. non-uniform scale only resizes the box, the
 * orthonormal direction frame keeps normal mapping undistorted.
 *
 * textures are borrowed: the collection never takes ownership of the
 * color/normal textures. the caller has to keep both the collection and
 * every texture it references alive for as long as a draw uses them.
 */

typedef struct decal {
    mat4_t local_to_world;
    mat4_t world_to_local;
    /* orthonormal world directions of the local axes (columns of R) */
    vec3_t axis_x;
    vec3_t axis_y;
    vec3_t axis_z;
    texture_t *color_map;   /* RGBA, alpha is the per-texel blend weight */
    texture_t *normal_map;  /* optional tangent-space normal */
    float roughness;
    float metalness;
    float angle_cos;
} decal_t;

typedef struct decal_collection decal_collection_t;

/* collection management */
decal_collection_t *decal_collection_create(void);
void decal_collection_release(decal_collection_t *collection);
int decal_collection_size(decal_collection_t *collection);
const decal_t *decal_collection_get(decal_collection_t *collection, int index);

/*
 * appends a decal in collection order and returns its index.
 * returns -1 (after reporting on stderr) when color_map is NULL or the
 * transform is singular, reflected or sheared.
 */
int decal_collection_add(decal_collection_t *collection,
                         mat4_t local_to_world,
                         texture_t *color_map, texture_t *normal_map,
                         float roughness, float metalness, float angle_cos);

/* linear-space surface quantities modified by a collection */
typedef struct {
    vec3_t basecolor;
    vec3_t normal;
    float roughness;
    float metalness;
} decal_surface_t;

/*
 * evaluates every decal of the collection against one fragment in
 * collection order. base_normal is the interpolated geometric normal
 * before normal-mapping and before back-face flipping; only fragments
 * inside a box, inside its UV range and facing its world +Z axis well
 * enough (dot >= angle_cos) are modified.
 */
void decal_collection_apply(decal_collection_t *collection,
                            vec3_t world_position, vec3_t base_normal,
                            decal_surface_t *surface);

#endif
