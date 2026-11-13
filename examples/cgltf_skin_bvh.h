/*
 * cgltf_skin_bvh - pose-aware ray casting for skinned and morph-target glTF scenes.
 *
 * Overview
 * --------
 * This module builds a two-level BVH (triangle leaves per mesh instance, one
 * top-level tree over instances) over the mesh instances found under one scene
 * of an already parsed cgltf_data. Poses are submitted as plain world matrices
 * plus optional per-node morph weight overrides; each accepted pose only
 * re-evaluates conservative leaf bounds from precomputed bound coefficients
 * (joint influences, inverse-bind matrices, morph delta radii) and refits both
 * BVH levels bottom-up. No vertex streams are touched while a pose is
 * submitted, and no heap allocation happens in either submit or raycast.
 * Deformed vertices are computed lazily, only for triangles that become ray
 * candidates, and are cached per instance until the next accepted pose.
 *
 * Usage
 * -----
 * 1. Parse a file with cgltf_parse / cgltf_parse_file, then cgltf_load_buffers
 *    and cgltf_validate. The module only reads the data; the caller must keep
 *    the cgltf_data (and its buffers) alive and unmodified until destroy.
 * 2. Create a context with cgltf_skin_bvh_create(&bvh, data, scene). Only mesh
 *    instances reachable from the scene's root nodes are included; skin joints
 *    may live outside the scene. The initial pose is the static pose described
 *    by the file (node TRS/matrix transforms, node/mesh morph weights).
 * 3. Optionally submit new poses with cgltf_skin_bvh_submit_pose:
 *      - world_matrices: data->nodes_count column-major affine 4x4 matrices in
 *        data->nodes order (16 floats each, last row exactly 0,0,0,1). No
 *        parent/child composition or animation sampling is performed.
 *      - morph_weights / morph_weight_counts: optional per-node override
 *        arrays (also in data->nodes order). A NULL entry falls back to the
 *        node weights, then the mesh weights, then zeros. A non-NULL entry is
 *        an explicit override (explicit zeros included) whose length must
 *        equal the morph target count of that node's mesh (0 for nodes
 *        without morph targets).
 *    On failure the previously accepted pose stays queryable.
 * 4. Query with cgltf_skin_bvh_raycast: ray O + t*D (direction need not be
 *     normalized) over the closed interval [tmin, tmax]. Both triangle sides
 *     are hit; zero-area triangles never hit. On success the hit carries the
 *     node/mesh/primitive indices into data->nodes/data->meshes and the
 *     mesh's primitive array, the primitive's original triangle index, the
 *     ray parameter t, the three barycentric weights in the primitive's
 *     original vertex order, the world-space position and the unit geometric
 *     normal derived from the deformed winding. Ties on t are broken by the
 *     lexicographically smallest (node, primitive, triangle).
 * 5. Destroy with cgltf_skin_bvh_destroy.
 *
 * Every call may optionally fill a cgltf_skin_bvh_stats with the actual work
 * performed: bound coefficient evaluations (pose submit), deformed vertex
 * computations (raycast only; always zero for submit), BVH node visits and
 * triangle intersection tests. Counters are reset per call.
 *
 * Supported geometry: TRIANGLES primitives, indexed (U8/U16/U32) or not, with
 * float VEC3 POSITION and float VEC3 POSITION morph targets (accessor offset,
 * stride, sparse and all-zero base data are honored). Skinning uses
 * JOINTS_0/WEIGHTS_0 and the paired JOINTS_1/WEIGHTS_1 (U8/U16 joints, float
 * or normalized U8/U16 weights, at most eight influences per vertex; zero
 * weights never dereference their joint, duplicate joints are merged and the
 * result is normalized). Missing inverseBindMatrices default to identity.
 * Compressed geometry (Draco / meshopt buffer views), GPU instancing and
 * non-triangle primitives are rejected explicitly. An empty scene is valid
 * and simply never hits anything.
 *
 * Contexts are independent: two contexts may be used interleaved, but a single
 * context is not safe for concurrent queries.
 *
 * Only C99, the standard library and libm are required.
 */
#ifndef CGLTF_SKIN_BVH_H
#define CGLTF_SKIN_BVH_H

#include "../cgltf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cgltf_skin_bvh cgltf_skin_bvh;

typedef enum cgltf_skin_bvh_result
{
	cgltf_skin_bvh_result_success = 0,      /* call completed; for raycast a hit was found */
	cgltf_skin_bvh_result_no_hit,           /* raycast completed; nothing was hit in [tmin, tmax] */
	cgltf_skin_bvh_result_invalid_argument, /* NULL arguments, non-finite/zero ray, reversed interval */
	cgltf_skin_bvh_result_invalid_data,     /* malformed geometry, weights or pose (non-finite, non-affine, overflow) */
	cgltf_skin_bvh_result_unsupported,      /* compressed geometry, GPU instancing, non-triangle primitives */
	cgltf_skin_bvh_result_out_of_memory
} cgltf_skin_bvh_result;

typedef struct cgltf_skin_bvh_stats
{
	unsigned long long bound_evaluations;  /* conservative bound coefficient applications (pose submit) */
	unsigned long long deformed_vertices;  /* exact vertex deformations (raycast only; always 0 for submit) */
	unsigned long long bvh_nodes_visited;  /* BVH node visits (both levels, raycast only) */
	unsigned long long triangle_tests;     /* ray/triangle intersection tests (raycast only) */
} cgltf_skin_bvh_stats;

typedef struct cgltf_skin_bvh_hit
{
	cgltf_size node_index;       /* index into data->nodes */
	cgltf_size mesh_index;       /* index into data->meshes */
	cgltf_size primitive_index;  /* index into mesh->primitives */
	cgltf_size triangle_index;   /* original triangle order of the primitive */
	float t;                     /* ray parameter of the hit, within [tmin, tmax] */
	float barycentric[3];        /* weights for the triangle's original vertex order */
	float position[3];           /* world-space hit position */
	float normal[3];             /* unit geometric normal from the deformed winding */
} cgltf_skin_bvh_hit;

/* Creates a BVH context over the mesh instances of one scene. The data must
 * have loaded buffers and must stay alive and unmodified until destroy. The
 * initial pose is the static pose stored in the file. */
cgltf_skin_bvh_result cgltf_skin_bvh_create(cgltf_skin_bvh** out_bvh, const cgltf_data* data, const cgltf_scene* scene);

void cgltf_skin_bvh_destroy(cgltf_skin_bvh* bvh);

/* Submits a full pose: world_matrices holds data->nodes_count column-major
 * affine matrices in data->nodes order. morph_weights/morph_weight_counts are
 * optional per-node override arrays in the same order (both NULL for no
 * overrides); a non-NULL entry must match the morph target count of that
 * node's mesh. On any failure the previous pose remains active. */
cgltf_skin_bvh_result cgltf_skin_bvh_submit_pose(cgltf_skin_bvh* bvh,
	const float* world_matrices, const float* const* morph_weights,
	const cgltf_size* morph_weight_counts, cgltf_skin_bvh_stats* stats);

/* Finds the closest triangle hit of O + t*D for t in [tmin, tmax]. The
 * direction need not be normalized. Returns cgltf_skin_bvh_result_no_hit
 * (distinct from any error) when nothing is hit. */
cgltf_skin_bvh_result cgltf_skin_bvh_raycast(cgltf_skin_bvh* bvh,
	const float origin[3], const float direction[3], float tmin, float tmax,
	cgltf_skin_bvh_hit* out_hit, cgltf_skin_bvh_stats* stats);

#ifdef __cplusplus
}
#endif

#endif /* CGLTF_SKIN_BVH_H */
