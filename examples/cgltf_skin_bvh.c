/*
 * cgltf_skin_bvh implementation. See cgltf_skin_bvh.h for the API contract.
 *
 * The context keeps, per mesh instance, a triangle-leaf BVH whose topology is
 * built once from the base (unmorphed) geometry, plus per-leaf conservative
 * bound data: the base AABB of the leaf, the per-target morph delta radii and
 * the set of skin joints influencing the leaf. A pose submit only evaluates
 * these coefficients (joint matrix = joint world * inverse-bind, morph
 * expansion = sum |w_k| * radius_k) to produce world-space leaf boxes, then
 * refits both BVH levels bottom-up. Exact vertex deformation happens lazily
 * inside raycast, cached per instance and pose generation.
 */
#include "cgltf_skin_bvh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SBVH_MAX_INFLUENCES 8
#define SBVH_LEAF_TRIANGLES 4
#define SBVH_TOP_LEAF 0xFFFFFFFFu

typedef struct
{
	float bmin[3], bmax[3];
	cgltf_uint left, right;  /* child node indices for internal nodes */
	cgltf_uint first, count; /* leaf: range into tri_order (count > 0 marks a leaf) */
	cgltf_uint leaf_id;      /* leaf: index into the primitive's leaf array */
} sbvh_node;

typedef struct
{
	float lmin[3], lmax[3];         /* base local-space AABB of the leaf */
	cgltf_uint joint_offset;        /* into the primitive's leaf_joints array */
	cgltf_uint joint_count;         /* 0 for static primitives */
	cgltf_uint node_index;          /* BVH node that owns this leaf */
} sbvh_leaf;

typedef struct
{
	cgltf_size mesh_index;
	cgltf_size primitive_index;
	cgltf_size vertex_count;
	cgltf_size triangle_count;
	cgltf_size target_count;
	cgltf_uint* indices;        /* 3 * triangle_count, original vertex order */
	float* positions;           /* 3 * vertex_count, base positions */
	float* morph_deltas;        /* target_count * 3 * vertex_count, NULL when no targets */
	float* morph_radius;        /* target_count max delta lengths */
	cgltf_uint* infl_joints;    /* vertex_count * SBVH_MAX_INFLUENCES, skinned only */
	float* infl_weights;        /* vertex_count * SBVH_MAX_INFLUENCES, skinned only */
	unsigned char* infl_counts; /* vertex_count, skinned only */
	cgltf_uint* leaf_joints;    /* concatenated per-leaf joint lists, skinned only */
	sbvh_leaf* leaves;
	cgltf_size leaf_count;
	sbvh_node* nodes;
	cgltf_size node_count;
	cgltf_uint* tri_order;      /* BVH order -> original triangle index */
	float* cache_pos;           /* 3 * vertex_count deformed vertex cache */
	cgltf_uint* cache_gen;      /* vertex_count cache generation tags */
	cgltf_size leaf_base;       /* base index into the context leaf scratch */
} sbvh_prim;

typedef struct
{
	const cgltf_node* node;
	const cgltf_mesh* mesh;
	cgltf_size node_index;
	cgltf_size mesh_index;
	int skinned;
	cgltf_size joints_count;
	cgltf_size* joint_nodes;    /* joints_count node indices into data->nodes */
	float* ibms;                /* 16 * joints_count inverse-bind matrices */
	float* joint_mats;          /* 16 * joints_count committed pose joint matrices */
	float* pending_joints;      /* 16 * joints_count scratch for pose submit */
	cgltf_size target_count;
	float* morph_weights;       /* target_count committed resolved weights */
	float* pending_weights;     /* target_count scratch for pose submit */
	sbvh_prim* prims;
	cgltf_size prims_count;
	float base_bmin[3], base_bmax[3]; /* static local bounds (BVH topology only) */
	float bmin[3], bmax[3];           /* current world bounds */
	int empty;                        /* no triangles in any primitive */
} sbvh_instance;

typedef struct
{
	float bmin[3], bmax[3];
	cgltf_uint left, right; /* child node indices; left == SBVH_TOP_LEAF marks a leaf */
	cgltf_uint instance;    /* leaf payload */
} sbvh_tnode;

struct cgltf_skin_bvh
{
	const cgltf_data* data;
	cgltf_size nodes_count;
	cgltf_size* mesh_target_counts; /* data->meshes_count */
	sbvh_instance* instances;
	cgltf_size instances_count;
	sbvh_tnode* top_nodes;
	cgltf_size top_node_count;
	float* pose_mats;        /* 16 * nodes_count committed world matrices */
	cgltf_uint generation;   /* bumped by every accepted pose */
	float* leaf_scratch;     /* 6 * total_leaves pending leaf bounds */
	cgltf_uint* top_stack;   /* top_node_count traversal stack */
	cgltf_uint* prim_stack;  /* max primitive node_count traversal stack */
};

/* ------------------------------------------------------------------ */
/* small math helpers (column-major affine matrices)                   */

static void sbvh_mat_mul(const float* a, const float* b, float* out)
{
	/* out = a * b; both affine, so the last row is exactly 0,0,0,1 */
	for (int c = 0; c < 4; ++c)
	{
		float b0 = b[c * 4 + 0], b1 = b[c * 4 + 1], b2 = b[c * 4 + 2];
		out[c * 4 + 0] = a[0] * b0 + a[4] * b1 + a[8] * b2 + (c == 3 ? a[12] : 0.0f);
		out[c * 4 + 1] = a[1] * b0 + a[5] * b1 + a[9] * b2 + (c == 3 ? a[13] : 0.0f);
		out[c * 4 + 2] = a[2] * b0 + a[6] * b1 + a[10] * b2 + (c == 3 ? a[14] : 0.0f);
		out[c * 4 + 3] = 0.0f;
	}
	out[15] = 1.0f;
}

static void sbvh_mat_apply(const float* m, float x, float y, float z, float* out)
{
	out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
	out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
	out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

static void sbvh_box_transform_merge(const float* m, const float bmin[3], const float bmax[3], float* omin, float* omax)
{
	for (int c = 0; c < 8; ++c)
	{
		float p[3];
		sbvh_mat_apply(m, (c & 1) ? bmax[0] : bmin[0], (c & 2) ? bmax[1] : bmin[1], (c & 4) ? bmax[2] : bmin[2], p);
		for (int a = 0; a < 3; ++a)
		{
			if (p[a] < omin[a]) omin[a] = p[a];
			if (p[a] > omax[a]) omax[a] = p[a];
		}
	}
}

static int sbvh_finite3(const float* v)
{
	return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/* slab test robust to zero (including signed zero) direction components */
static int sbvh_ray_box(const float o[3], const float d[3], const float bmin[3], const float bmax[3],
	float tmin, float tmax, float* out_tnear)
{
	float t0 = tmin, t1 = tmax;
	for (int a = 0; a < 3; ++a)
	{
		if (d[a] == 0.0f)
		{
			if (o[a] < bmin[a] || o[a] > bmax[a])
				return 0;
		}
		else
		{
			float inv = 1.0f / d[a];
			float ta = (bmin[a] - o[a]) * inv;
			float tb = (bmax[a] - o[a]) * inv;
			if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
			if (ta > t0) t0 = ta;
			if (tb < t1) t1 = tb;
			if (t0 > t1) return 0;
		}
	}
	*out_tnear = t0;
	return 1;
}

/* two-sided Moller-Trumbore; zero-area triangles give det == 0 and miss */
static int sbvh_ray_tri(const float o[3], const float d[3], const float v0[3], const float v1[3], const float v2[3],
	float tmin, float tmax, float* out_t, float* out_u, float* out_v)
{
	float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
	float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
	float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
	float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
	if (det == 0.0f)
		return 0;
	float inv = 1.0f / det;
	float s[3] = {o[0] - v0[0], o[1] - v0[1], o[2] - v0[2]};
	float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
	if (u < 0.0f || u > 1.0f)
		return 0;
	float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
	float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
	if (v < 0.0f || u + v > 1.0f)
		return 0;
	float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
	if (t < tmin || t > tmax)
		return 0;
	*out_t = t;
	*out_u = u;
	*out_v = v;
	return 1;
}

/* ------------------------------------------------------------------ */
/* destruction                                                         */

static void sbvh_free_prim(sbvh_prim* prim)
{
	free(prim->indices);
	free(prim->positions);
	free(prim->morph_deltas);
	free(prim->morph_radius);
	free(prim->infl_joints);
	free(prim->infl_weights);
	free(prim->infl_counts);
	free(prim->leaf_joints);
	free(prim->leaves);
	free(prim->nodes);
	free(prim->tri_order);
	free(prim->cache_pos);
	free(prim->cache_gen);
}

void cgltf_skin_bvh_destroy(cgltf_skin_bvh* bvh)
{
	if (!bvh)
		return;
	for (cgltf_size i = 0; i < bvh->instances_count; ++i)
	{
		sbvh_instance* inst = &bvh->instances[i];
		for (cgltf_size p = 0; p < inst->prims_count; ++p)
			sbvh_free_prim(&inst->prims[p]);
		free(inst->prims);
		free(inst->joint_nodes);
		free(inst->ibms);
		free(inst->joint_mats);
		free(inst->pending_joints);
		free(inst->morph_weights);
		free(inst->pending_weights);
	}
	free(bvh->instances);
	free(bvh->mesh_target_counts);
	free(bvh->top_nodes);
	free(bvh->pose_mats);
	free(bvh->leaf_scratch);
	free(bvh->top_stack);
	free(bvh->prim_stack);
	free(bvh);
}

/* ------------------------------------------------------------------ */
/* create-time geometry validation and reading                         */

static int sbvh_accessor_compressed(const cgltf_accessor* acc)
{
	if (!acc)
		return 0;
	if (acc->buffer_view && acc->buffer_view->has_meshopt_compression)
		return 1;
	if (acc->is_sparse)
	{
		if (acc->sparse.indices_buffer_view && acc->sparse.indices_buffer_view->has_meshopt_compression)
			return 1;
		if (acc->sparse.values_buffer_view && acc->sparse.values_buffer_view->has_meshopt_compression)
			return 1;
	}
	return 0;
}

static cgltf_skin_bvh_result sbvh_check_positions(const cgltf_accessor* acc)
{
	if (!acc)
		return cgltf_skin_bvh_result_invalid_data;
	if (acc->type != cgltf_type_vec3 || acc->component_type != cgltf_component_type_r_32f)
		return cgltf_skin_bvh_result_invalid_data;
	if (sbvh_accessor_compressed(acc))
		return cgltf_skin_bvh_result_unsupported;
	return cgltf_skin_bvh_result_success;
}

static cgltf_skin_bvh_result sbvh_read_positions(const cgltf_accessor* acc, float* out, cgltf_size count)
{
	if (cgltf_accessor_unpack_floats(acc, out, count * 3) != count * 3)
		return cgltf_skin_bvh_result_invalid_data;
	for (cgltf_size i = 0; i < count * 3; ++i)
		if (!isfinite(out[i]))
			return cgltf_skin_bvh_result_invalid_data;
	return cgltf_skin_bvh_result_success;
}

static cgltf_skin_bvh_result sbvh_check_joints_accessor(const cgltf_accessor* acc, cgltf_size vertex_count)
{
	if (acc->type != cgltf_type_vec4)
		return cgltf_skin_bvh_result_invalid_data;
	if (acc->component_type != cgltf_component_type_r_8u && acc->component_type != cgltf_component_type_r_16u)
		return cgltf_skin_bvh_result_invalid_data;
	if (acc->count != vertex_count)
		return cgltf_skin_bvh_result_invalid_data;
	if (sbvh_accessor_compressed(acc))
		return cgltf_skin_bvh_result_unsupported;
	return cgltf_skin_bvh_result_success;
}

static cgltf_skin_bvh_result sbvh_check_weights_accessor(const cgltf_accessor* acc, cgltf_size vertex_count)
{
	if (acc->type != cgltf_type_vec4)
		return cgltf_skin_bvh_result_invalid_data;
	if (acc->component_type == cgltf_component_type_r_32f)
	{
		/* plain float weights */
	}
	else if ((acc->component_type == cgltf_component_type_r_8u || acc->component_type == cgltf_component_type_r_16u) && acc->normalized)
	{
		/* normalized integer weights */
	}
	else
	{
		return cgltf_skin_bvh_result_invalid_data;
	}
	if (acc->count != vertex_count)
		return cgltf_skin_bvh_result_invalid_data;
	if (sbvh_accessor_compressed(acc))
		return cgltf_skin_bvh_result_unsupported;
	return cgltf_skin_bvh_result_success;
}

/* Reads, validates, merges and normalizes the influences of one vertex. */
static cgltf_skin_bvh_result sbvh_read_influences(const cgltf_accessor* joints0, const cgltf_accessor* joints1,
	const float* weights0, const float* weights1, cgltf_size vertex, cgltf_size joints_count,
	cgltf_uint* out_joints, float* out_weights, cgltf_size* out_count)
{
	cgltf_uint joints[SBVH_MAX_INFLUENCES];
	float weights[SBVH_MAX_INFLUENCES];
	cgltf_size count = 0;
	float total = 0.0f;
	for (int set = 0; set < 2; ++set)
	{
		const cgltf_accessor* jacc = set == 0 ? joints0 : joints1;
		const float* wdata = set == 0 ? weights0 : weights1;
		if (!jacc)
			continue;
		cgltf_uint jraw[4] = {0, 0, 0, 0};
		if (!cgltf_accessor_read_uint(jacc, vertex, jraw, 4))
			return cgltf_skin_bvh_result_invalid_data;
		for (int c = 0; c < 4; ++c)
		{
			float w = wdata[vertex * 4 + (cgltf_size)c];
			if (!isfinite(w) || w < 0.0f)
				return cgltf_skin_bvh_result_invalid_data;
			total += w;
			if (!isfinite(total))
				return cgltf_skin_bvh_result_invalid_data;
			if (w == 0.0f)
				continue; /* zero weights never dereference their joint */
			cgltf_uint j = jraw[c];
			if ((cgltf_size)j >= joints_count)
				return cgltf_skin_bvh_result_invalid_data;
			cgltf_size k = 0;
			while (k < count && joints[k] != j)
				++k;
			if (k < count)
			{
				weights[k] += w; /* merge duplicate joints */
			}
			else
			{
				joints[count] = j;
				weights[count] = w;
				++count;
			}
		}
	}
	if (total == 0.0f)
		return cgltf_skin_bvh_result_invalid_data;
	for (cgltf_size k = 0; k < count; ++k)
	{
		out_joints[k] = joints[k];
		out_weights[k] = weights[k] / total;
	}
	*out_count = count;
	return cgltf_skin_bvh_result_success;
}

/* ------------------------------------------------------------------ */
/* triangle BVH construction (create-time, heap allowed)               */

typedef struct
{
	const float* positions;
	const cgltf_uint* indices;
	sbvh_node* nodes;
	cgltf_uint node_count;
	sbvh_leaf* leaves;
	cgltf_uint leaf_count;
	cgltf_uint* order;
	cgltf_uint order_count;
} sbvh_tri_builder;

static float sbvh_tri_centroid(const sbvh_tri_builder* b, cgltf_uint tid, int axis)
{
	const cgltf_uint* tri = b->indices + (cgltf_size)tid * 3;
	return b->positions[tri[0] * 3 + axis] + b->positions[tri[1] * 3 + axis] + b->positions[tri[2] * 3 + axis];
}

static void sbvh_sort_tris(sbvh_tri_builder* b, cgltf_uint* ids, cgltf_size n, int axis)
{
	if (n < 16)
	{
		for (cgltf_size i = 1; i < n; ++i)
		{
			cgltf_uint key = ids[i];
			float kv = sbvh_tri_centroid(b, key, axis);
			cgltf_size j = i;
			while (j > 0 && sbvh_tri_centroid(b, ids[j - 1], axis) > kv)
			{
				ids[j] = ids[j - 1];
				--j;
			}
			ids[j] = key;
		}
		return;
	}
	float pivot = sbvh_tri_centroid(b, ids[n / 2], axis);
	cgltf_size lo = 0, hi = n - 1;
	while (lo <= hi)
	{
		while (sbvh_tri_centroid(b, ids[lo], axis) < pivot) ++lo;
		while (sbvh_tri_centroid(b, ids[hi], axis) > pivot) --hi;
		if (lo <= hi)
		{
			cgltf_uint tmp = ids[lo]; ids[lo] = ids[hi]; ids[hi] = tmp;
			++lo;
			if (hi > 0) --hi; else break;
		}
	}
	if (hi > 0)
		sbvh_sort_tris(b, ids, hi + 1, axis);
	if (lo < n)
		sbvh_sort_tris(b, ids + lo, n - lo, axis);
}

static cgltf_uint sbvh_build_tris(sbvh_tri_builder* b, cgltf_uint* ids, cgltf_uint n)
{
	cgltf_uint ni = b->node_count++;
	sbvh_node* node = &b->nodes[ni];
	float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
	float cmin[3] = {0, 0, 0}, cmax[3] = {0, 0, 0};
	for (cgltf_uint i = 0; i < n; ++i)
	{
		const cgltf_uint* tri = b->indices + (cgltf_size)ids[i] * 3;
		for (int v = 0; v < 3; ++v)
		{
			const float* p = b->positions + (cgltf_size)tri[v] * 3;
			for (int a = 0; a < 3; ++a)
			{
				if (i == 0 && v == 0) { bmin[a] = bmax[a] = p[a]; cmin[a] = cmax[a] = p[a]; }
				if (p[a] < bmin[a]) bmin[a] = p[a];
				if (p[a] > bmax[a]) bmax[a] = p[a];
				if (p[a] < cmin[a]) cmin[a] = p[a];
				if (p[a] > cmax[a]) cmax[a] = p[a];
			}
		}
	}
	memcpy(node->bmin, bmin, sizeof(bmin));
	memcpy(node->bmax, bmax, sizeof(bmax));
	node->leaf_id = 0;
	if (n <= SBVH_LEAF_TRIANGLES)
	{
		node->first = b->order_count;
		node->count = n;
		node->left = node->right = 0;
		node->leaf_id = b->leaf_count++;
		sbvh_leaf* leaf = &b->leaves[node->leaf_id];
		memcpy(leaf->lmin, bmin, sizeof(bmin));
		memcpy(leaf->lmax, bmax, sizeof(bmax));
		leaf->joint_offset = 0;
		leaf->joint_count = 0;
		leaf->node_index = ni;
		memcpy(b->order + b->order_count, ids, n * sizeof(cgltf_uint));
		b->order_count += n;
		return ni;
	}
	int axis = 0;
	float extent = cmax[0] - cmin[0];
	for (int a = 1; a < 3; ++a)
		if (cmax[a] - cmin[a] > extent) { extent = cmax[a] - cmin[a]; axis = a; }
	if (extent > 0.0f)
		sbvh_sort_tris(b, ids, n, axis);
	cgltf_uint mid = n / 2;
	node->count = 0;
	node->first = 0;
	node->left = sbvh_build_tris(b, ids, mid);
	node->right = sbvh_build_tris(b, ids + mid, n - mid);
	return ni;
}

/* ------------------------------------------------------------------ */
/* per-primitive create                                                */

static cgltf_skin_bvh_result sbvh_build_prim(cgltf_skin_bvh* bvh, sbvh_instance* inst, sbvh_prim* prim,
	const cgltf_primitive* src, cgltf_size* leaf_running_total, cgltf_size* max_prim_nodes)
{
	cgltf_skin_bvh_result res;
	memset(prim, 0, sizeof(*prim));
	prim->mesh_index = inst->mesh_index;
	prim->primitive_index = (cgltf_size)(src - inst->mesh->primitives);
	prim->target_count = inst->target_count;

	if (src->type != cgltf_primitive_type_triangles)
		return cgltf_skin_bvh_result_unsupported;
	if (src->has_draco_mesh_compression)
		return cgltf_skin_bvh_result_unsupported;

	const cgltf_accessor* pos_acc = cgltf_find_accessor(src, cgltf_attribute_type_position, 0);
	res = sbvh_check_positions(pos_acc);
	if (res != cgltf_skin_bvh_result_success)
		return res;

	const cgltf_accessor* joints0 = cgltf_find_accessor(src, cgltf_attribute_type_joints, 0);
	const cgltf_accessor* joints1 = cgltf_find_accessor(src, cgltf_attribute_type_joints, 1);
	const cgltf_accessor* weights0 = cgltf_find_accessor(src, cgltf_attribute_type_weights, 0);
	const cgltf_accessor* weights1 = cgltf_find_accessor(src, cgltf_attribute_type_weights, 1);
	if ((joints0 != NULL) != (weights0 != NULL))
		return cgltf_skin_bvh_result_invalid_data;
	if ((joints1 != NULL) != (weights1 != NULL))
		return cgltf_skin_bvh_result_invalid_data;
	if (joints1 && !joints0)
		return cgltf_skin_bvh_result_invalid_data;
	if (inst->skinned && !joints0)
		return cgltf_skin_bvh_result_invalid_data;

	cgltf_size vc = pos_acc->count;
	prim->vertex_count = vc;

	if (joints0)
	{
		res = sbvh_check_joints_accessor(joints0, vc);
		if (res != cgltf_skin_bvh_result_success) return res;
		res = sbvh_check_weights_accessor(weights0, vc);
		if (res != cgltf_skin_bvh_result_success) return res;
	}
	if (joints1)
	{
		res = sbvh_check_joints_accessor(joints1, vc);
		if (res != cgltf_skin_bvh_result_success) return res;
		res = sbvh_check_weights_accessor(weights1, vc);
		if (res != cgltf_skin_bvh_result_success) return res;
	}

	/* indices */
	cgltf_size triangle_count = 0;
	cgltf_uint* indices = NULL;
	if (src->indices)
	{
		const cgltf_accessor* iacc = src->indices;
		if (iacc->type != cgltf_type_scalar)
			return cgltf_skin_bvh_result_invalid_data;
		if (iacc->component_type != cgltf_component_type_r_8u &&
			iacc->component_type != cgltf_component_type_r_16u &&
			iacc->component_type != cgltf_component_type_r_32u)
			return cgltf_skin_bvh_result_invalid_data;
		if (sbvh_accessor_compressed(iacc))
			return cgltf_skin_bvh_result_unsupported;
		if (iacc->count % 3 != 0)
			return cgltf_skin_bvh_result_invalid_data;
		triangle_count = iacc->count / 3;
		indices = (cgltf_uint*)malloc(iacc->count * sizeof(cgltf_uint));
		if (!indices && iacc->count)
			return cgltf_skin_bvh_result_out_of_memory;
		for (cgltf_size i = 0; i < iacc->count; ++i)
		{
			cgltf_size idx = cgltf_accessor_read_index(iacc, i);
			if (idx >= vc)
			{
				free(indices);
				return cgltf_skin_bvh_result_invalid_data;
			}
			indices[i] = (cgltf_uint)idx;
		}
	}
	else
	{
		if (vc % 3 != 0)
			return cgltf_skin_bvh_result_invalid_data;
		triangle_count = vc / 3;
		indices = (cgltf_uint*)malloc(vc * sizeof(cgltf_uint));
		if (!indices && vc)
			return cgltf_skin_bvh_result_out_of_memory;
		for (cgltf_size i = 0; i < vc; ++i)
			indices[i] = (cgltf_uint)i;
	}
	prim->indices = indices;
	prim->triangle_count = triangle_count;

	/* base positions */
	if (vc)
	{
		prim->positions = (float*)malloc(vc * 3 * sizeof(float));
		if (!prim->positions)
			return cgltf_skin_bvh_result_out_of_memory;
		res = sbvh_read_positions(pos_acc, prim->positions, vc);
		if (res != cgltf_skin_bvh_result_success)
			return res;
	}

	/* morph targets (POSITION deltas only) */
	if (prim->target_count)
	{
		prim->morph_deltas = (float*)calloc(prim->target_count * (vc ? vc : 1) * 3, sizeof(float));
		prim->morph_radius = (float*)calloc(prim->target_count, sizeof(float));
		if (!prim->morph_deltas || !prim->morph_radius)
			return cgltf_skin_bvh_result_out_of_memory;
		for (cgltf_size k = 0; k < prim->target_count; ++k)
		{
			const cgltf_morph_target* target = &src->targets[k];
			const cgltf_accessor* dacc = NULL;
			for (cgltf_size a = 0; a < target->attributes_count; ++a)
				if (target->attributes[a].type == cgltf_attribute_type_position)
					dacc = target->attributes[a].data;
			if (!dacc)
				continue; /* targets without POSITION contribute nothing */
			res = sbvh_check_positions(dacc);
			if (res != cgltf_skin_bvh_result_success)
				return res;
			if (dacc->count != vc)
				return cgltf_skin_bvh_result_invalid_data;
			float* deltas = prim->morph_deltas + k * vc * 3;
			res = sbvh_read_positions(dacc, deltas, vc);
			if (res != cgltf_skin_bvh_result_success)
				return res;
			float radius = 0.0f;
			for (cgltf_size v = 0; v < vc; ++v)
			{
				float dx = deltas[v * 3 + 0], dy = deltas[v * 3 + 1], dz = deltas[v * 3 + 2];
				float len = sqrtf(dx * dx + dy * dy + dz * dz);
				if (!isfinite(len))
					return cgltf_skin_bvh_result_invalid_data;
				if (len > radius) radius = len;
			}
			prim->morph_radius[k] = radius;
		}
	}

	/* skin influences */
	if (inst->skinned)
	{
		prim->infl_joints = (cgltf_uint*)calloc(vc ? vc : 1, SBVH_MAX_INFLUENCES * sizeof(cgltf_uint));
		prim->infl_weights = (float*)calloc(vc ? vc : 1, SBVH_MAX_INFLUENCES * sizeof(float));
		prim->infl_counts = (unsigned char*)calloc(vc ? vc : 1, 1);
		if (!prim->infl_joints || !prim->infl_weights || !prim->infl_counts)
			return cgltf_skin_bvh_result_out_of_memory;
		float* w0 = (float*)malloc((vc ? vc : 1) * 4 * sizeof(float));
		float* w1 = joints1 ? (float*)malloc(vc * 4 * sizeof(float)) : NULL;
		if (!w0 || (joints1 && !w1))
		{
			free(w0);
			free(w1);
			return cgltf_skin_bvh_result_out_of_memory;
		}
		cgltf_size got0 = cgltf_accessor_unpack_floats(weights0, w0, vc * 4);
		cgltf_size got1 = joints1 ? cgltf_accessor_unpack_floats(weights1, w1, vc * 4) : vc * 4;
		if (got0 != vc * 4 || got1 != vc * 4)
		{
			free(w0);
			free(w1);
			return cgltf_skin_bvh_result_invalid_data;
		}
		for (cgltf_size v = 0; v < vc; ++v)
		{
			cgltf_size count = 0;
			res = sbvh_read_influences(joints0, joints1, w0, w1, v, inst->joints_count,
				prim->infl_joints + v * SBVH_MAX_INFLUENCES, prim->infl_weights + v * SBVH_MAX_INFLUENCES, &count);
			if (res != cgltf_skin_bvh_result_success)
			{
				free(w0);
				free(w1);
				return res;
			}
			prim->infl_counts[v] = (unsigned char)count;
		}
		free(w0);
		free(w1);
	}

	/* triangle BVH */
	if (triangle_count)
	{
		prim->nodes = (sbvh_node*)calloc(2 * triangle_count, sizeof(sbvh_node));
		prim->leaves = (sbvh_leaf*)calloc(triangle_count, sizeof(sbvh_leaf));
		prim->tri_order = (cgltf_uint*)malloc(triangle_count * sizeof(cgltf_uint));
		if (!prim->nodes || !prim->leaves || !prim->tri_order)
			return cgltf_skin_bvh_result_out_of_memory;
		cgltf_uint* ids = (cgltf_uint*)malloc(triangle_count * sizeof(cgltf_uint));
		if (!ids)
			return cgltf_skin_bvh_result_out_of_memory;
		for (cgltf_size i = 0; i < triangle_count; ++i)
			ids[i] = (cgltf_uint)i;
		sbvh_tri_builder builder;
		builder.positions = prim->positions;
		builder.indices = prim->indices;
		builder.nodes = prim->nodes;
		builder.node_count = 0;
		builder.leaves = prim->leaves;
		builder.leaf_count = 0;
		builder.order = prim->tri_order;
		builder.order_count = 0;
		sbvh_build_tris(&builder, ids, (cgltf_uint)triangle_count);
		free(ids);
		prim->node_count = builder.node_count;
		prim->leaf_count = builder.leaf_count;
		if (prim->node_count > *max_prim_nodes)
			*max_prim_nodes = prim->node_count;
	}

	/* per-leaf joint sets (skinned) */
	if (inst->skinned && prim->leaf_count)
	{
		cgltf_size joints_count = inst->joints_count;
		unsigned char* bitmap = (unsigned char*)calloc(joints_count, 1);
		if (!bitmap)
			return cgltf_skin_bvh_result_out_of_memory;
		cgltf_size total = 0;
		for (cgltf_size li = 0; li < prim->leaf_count; ++li)
		{
			sbvh_leaf* leaf = &prim->leaves[li];
			const sbvh_node* node = &prim->nodes[leaf->node_index];
			memset(bitmap, 0, joints_count);
			cgltf_uint count = 0;
			for (cgltf_uint t = node->first; t < node->first + node->count; ++t)
			{
				cgltf_uint tid = prim->tri_order[t];
				for (int vi = 0; vi < 3; ++vi)
				{
					cgltf_uint v = prim->indices[tid * 3 + vi];
					for (cgltf_size ii = 0; ii < prim->infl_counts[v]; ++ii)
					{
						cgltf_uint j = prim->infl_joints[v * SBVH_MAX_INFLUENCES + ii];
						if (!bitmap[j]) { bitmap[j] = 1; ++count; }
					}
				}
			}
			leaf->joint_offset = (cgltf_uint)total;
			leaf->joint_count = count;
			total += count;
		}
		prim->leaf_joints = (cgltf_uint*)malloc((total ? total : 1) * sizeof(cgltf_uint));
		if (!prim->leaf_joints)
		{
			free(bitmap);
			return cgltf_skin_bvh_result_out_of_memory;
		}
		for (cgltf_size li = 0; li < prim->leaf_count; ++li)
		{
			const sbvh_leaf* leaf = &prim->leaves[li];
			const sbvh_node* node = &prim->nodes[leaf->node_index];
			cgltf_uint* out = prim->leaf_joints + leaf->joint_offset;
			cgltf_uint count = 0;
			memset(bitmap, 0, joints_count);
			for (cgltf_uint t = node->first; t < node->first + node->count; ++t)
			{
				cgltf_uint tid = prim->tri_order[t];
				for (int vi = 0; vi < 3; ++vi)
				{
					cgltf_uint v = prim->indices[tid * 3 + vi];
					for (cgltf_size ii = 0; ii < prim->infl_counts[v]; ++ii)
					{
						cgltf_uint j = prim->infl_joints[v * SBVH_MAX_INFLUENCES + ii];
						if (!bitmap[j]) { bitmap[j] = 1; out[count++] = j; }
					}
				}
			}
		}
		free(bitmap);
	}

	/* deformed vertex cache */
	if (vc)
	{
		prim->cache_pos = (float*)malloc(vc * 3 * sizeof(float));
		prim->cache_gen = (cgltf_uint*)calloc(vc, sizeof(cgltf_uint));
		if (!prim->cache_pos || !prim->cache_gen)
			return cgltf_skin_bvh_result_out_of_memory;
	}

	prim->leaf_base = *leaf_running_total;
	*leaf_running_total += prim->leaf_count;
	(void)bvh;
	return cgltf_skin_bvh_result_success;
}

/* ------------------------------------------------------------------ */
/* instance collection and top-level BVH                               */

static cgltf_size sbvh_count_mesh_nodes(cgltf_node* node)
{
	cgltf_size n = node->mesh ? 1 : 0;
	for (cgltf_size i = 0; i < node->children_count; ++i)
		n += sbvh_count_mesh_nodes(node->children[i]);
	return n;
}

static void sbvh_collect_mesh_nodes(cgltf_node* node, const cgltf_node** out, cgltf_size* at)
{
	if (node->mesh)
		out[(*at)++] = node;
	for (cgltf_size i = 0; i < node->children_count; ++i)
		sbvh_collect_mesh_nodes(node->children[i], out, at);
}

typedef struct
{
	sbvh_tnode* nodes;
	cgltf_uint count;
	sbvh_instance* instances;
	const cgltf_uint* items;
} sbvh_top_builder;

static float sbvh_instance_centroid(sbvh_top_builder* b, cgltf_uint item, int axis)
{
	const sbvh_instance* inst = &b->instances[item];
	return (inst->base_bmin[axis] + inst->base_bmax[axis]) * 0.5f;
}

static void sbvh_sort_instances(sbvh_top_builder* b, cgltf_uint* items, cgltf_size n, int axis)
{
	if (n < 16)
	{
		for (cgltf_size i = 1; i < n; ++i)
		{
			cgltf_uint key = items[i];
			float kv = sbvh_instance_centroid(b, key, axis);
			cgltf_size j = i;
			while (j > 0 && sbvh_instance_centroid(b, items[j - 1], axis) > kv)
			{
				items[j] = items[j - 1];
				--j;
			}
			items[j] = key;
		}
		return;
	}
	float pivot = sbvh_instance_centroid(b, items[n / 2], axis);
	cgltf_size lo = 0, hi = n - 1;
	while (lo <= hi)
	{
		while (sbvh_instance_centroid(b, items[lo], axis) < pivot) ++lo;
		while (sbvh_instance_centroid(b, items[hi], axis) > pivot) --hi;
		if (lo <= hi)
		{
			cgltf_uint tmp = items[lo]; items[lo] = items[hi]; items[hi] = tmp;
			++lo;
			if (hi > 0) --hi; else break;
		}
	}
	if (hi > 0)
		sbvh_sort_instances(b, items, hi + 1, axis);
	if (lo < n)
		sbvh_sort_instances(b, items + lo, n - lo, axis);
}

static cgltf_uint sbvh_build_top(sbvh_top_builder* b, cgltf_uint* items, cgltf_uint n)
{
	cgltf_uint ni = b->count++;
	sbvh_tnode* node = &b->nodes[ni];
	node->instance = 0;
	if (n == 1)
	{
		const sbvh_instance* inst = &b->instances[items[0]];
		node->left = SBVH_TOP_LEAF;
		node->right = 0;
		node->instance = items[0];
		memcpy(node->bmin, inst->base_bmin, sizeof(node->bmin));
		memcpy(node->bmax, inst->base_bmax, sizeof(node->bmax));
		return ni;
	}
	float cmin[3] = {0, 0, 0}, cmax[3] = {0, 0, 0};
	for (cgltf_uint i = 0; i < n; ++i)
	{
		const sbvh_instance* inst = &b->instances[items[i]];
		for (int a = 0; a < 3; ++a)
		{
			float c = (inst->base_bmin[a] + inst->base_bmax[a]) * 0.5f;
			if (i == 0) { cmin[a] = cmax[a] = c; }
			if (c < cmin[a]) cmin[a] = c;
			if (c > cmax[a]) cmax[a] = c;
		}
	}
	int axis = 0;
	float extent = cmax[0] - cmin[0];
	for (int a = 1; a < 3; ++a)
		if (cmax[a] - cmin[a] > extent) { extent = cmax[a] - cmin[a]; axis = a; }
	if (extent > 0.0f)
		sbvh_sort_instances(b, items, n, axis);
	cgltf_uint mid = n / 2;
	node->left = sbvh_build_top(b, items, mid);
	node->right = sbvh_build_top(b, items + mid, n - mid);
	for (int a = 0; a < 3; ++a)
	{
		float l0 = b->nodes[node->left].bmin[a], l1 = b->nodes[node->left].bmax[a];
		float r0 = b->nodes[node->right].bmin[a], r1 = b->nodes[node->right].bmax[a];
		node->bmin[a] = l0 < r0 ? l0 : r0;
		node->bmax[a] = l1 > r1 ? l1 : r1;
	}
	return ni;
}

/* ------------------------------------------------------------------ */
/* create                                                              */

cgltf_skin_bvh_result cgltf_skin_bvh_create(cgltf_skin_bvh** out_bvh, const cgltf_data* data, const cgltf_scene* scene)
{
	if (!out_bvh || !data || !scene)
		return cgltf_skin_bvh_result_invalid_argument;
	*out_bvh = NULL;

	cgltf_skin_bvh* bvh = (cgltf_skin_bvh*)calloc(1, sizeof(cgltf_skin_bvh));
	if (!bvh)
		return cgltf_skin_bvh_result_out_of_memory;
	bvh->data = data;
	bvh->nodes_count = data->nodes_count;
	bvh->generation = 1;

	cgltf_skin_bvh_result res = cgltf_skin_bvh_result_success;
	cgltf_size leaf_total = 0;
	cgltf_size max_prim_nodes = 0;

	/* morph target counts per mesh (first primitive defines it) */
	if (data->meshes_count)
	{
		bvh->mesh_target_counts = (cgltf_size*)calloc(data->meshes_count, sizeof(cgltf_size));
		if (!bvh->mesh_target_counts)
		{
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
		for (cgltf_size m = 0; m < data->meshes_count; ++m)
		{
			const cgltf_mesh* mesh = &data->meshes[m];
			bvh->mesh_target_counts[m] = mesh->primitives_count ? mesh->primitives[0].targets_count : 0;
		}
	}

	/* collect mesh instances reachable from the scene roots */
	cgltf_size instance_count = 0;
	for (cgltf_size i = 0; i < scene->nodes_count; ++i)
		instance_count += sbvh_count_mesh_nodes(scene->nodes[i]);
	if (instance_count)
	{
		const cgltf_node** mesh_nodes = (const cgltf_node**)malloc(instance_count * sizeof(cgltf_node*));
		if (!mesh_nodes)
		{
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
		cgltf_size at = 0;
		for (cgltf_size i = 0; i < scene->nodes_count; ++i)
			sbvh_collect_mesh_nodes(scene->nodes[i], mesh_nodes, &at);
		bvh->instances = (sbvh_instance*)calloc(instance_count, sizeof(sbvh_instance));
		if (!bvh->instances)
		{
			free(mesh_nodes);
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
		bvh->instances_count = instance_count;

		for (cgltf_size ii = 0; ii < instance_count; ++ii)
		{
			sbvh_instance* inst = &bvh->instances[ii];
			const cgltf_node* node = mesh_nodes[ii];
			const cgltf_mesh* mesh = node->mesh;
			inst->node = node;
			inst->mesh = mesh;
			inst->node_index = cgltf_node_index(data, node);
			inst->mesh_index = cgltf_mesh_index(data, mesh);
			inst->skinned = node->skin != NULL;
			inst->target_count = bvh->mesh_target_counts[inst->mesh_index];
			inst->empty = 1;

			if (node->has_mesh_gpu_instancing)
			{
				free(mesh_nodes);
				res = cgltf_skin_bvh_result_unsupported;
				goto fail;
			}
			if (node->weights_count > 0 && node->weights_count != inst->target_count)
			{
				free(mesh_nodes);
				res = cgltf_skin_bvh_result_invalid_data;
				goto fail;
			}
			if (mesh->weights_count > 0 && mesh->weights_count != inst->target_count)
			{
				free(mesh_nodes);
				res = cgltf_skin_bvh_result_invalid_data;
				goto fail;
			}
			for (cgltf_size p = 0; p < mesh->primitives_count; ++p)
				if (mesh->primitives[p].targets_count != inst->target_count)
				{
					free(mesh_nodes);
					res = cgltf_skin_bvh_result_invalid_data;
					goto fail;
				}

			/* skin data */
			if (inst->skinned)
			{
				const cgltf_skin* skin = node->skin;
				inst->joints_count = skin->joints_count;
				if (inst->joints_count == 0)
				{
					free(mesh_nodes);
					res = cgltf_skin_bvh_result_invalid_data;
					goto fail;
				}
				inst->joint_nodes = (cgltf_size*)malloc(inst->joints_count * sizeof(cgltf_size));
				inst->ibms = (float*)malloc(inst->joints_count * 16 * sizeof(float));
				inst->joint_mats = (float*)malloc(inst->joints_count * 16 * sizeof(float));
				inst->pending_joints = (float*)malloc(inst->joints_count * 16 * sizeof(float));
				if (!inst->joint_nodes || !inst->ibms || !inst->joint_mats || !inst->pending_joints)
				{
					free(mesh_nodes);
					res = cgltf_skin_bvh_result_out_of_memory;
					goto fail;
				}
				for (cgltf_size j = 0; j < inst->joints_count; ++j)
				{
					if (!skin->joints[j])
					{
						free(mesh_nodes);
						res = cgltf_skin_bvh_result_invalid_data;
						goto fail;
					}
					inst->joint_nodes[j] = cgltf_node_index(data, skin->joints[j]);
				}
				if (skin->inverse_bind_matrices)
				{
					const cgltf_accessor* acc = skin->inverse_bind_matrices;
					if (acc->type != cgltf_type_mat4 || acc->component_type != cgltf_component_type_r_32f || acc->count < inst->joints_count)
					{
						free(mesh_nodes);
						res = cgltf_skin_bvh_result_invalid_data;
						goto fail;
					}
					if (sbvh_accessor_compressed(acc))
					{
						free(mesh_nodes);
						res = cgltf_skin_bvh_result_unsupported;
						goto fail;
					}
					for (cgltf_size j = 0; j < inst->joints_count; ++j)
					{
						float* m = inst->ibms + j * 16;
						if (!cgltf_accessor_read_float(acc, j, m, 16))
						{
							free(mesh_nodes);
							res = cgltf_skin_bvh_result_invalid_data;
							goto fail;
						}
						for (int k = 0; k < 16; ++k)
							if (!isfinite(m[k]))
							{
								free(mesh_nodes);
								res = cgltf_skin_bvh_result_invalid_data;
								goto fail;
							}
					}
				}
				else
				{
					for (cgltf_size j = 0; j < inst->joints_count; ++j)
					{
						float* m = inst->ibms + j * 16;
						memset(m, 0, 16 * sizeof(float));
						m[0] = m[5] = m[10] = m[15] = 1.0f;
					}
				}
			}

			/* morph weight slots */
			if (inst->target_count)
			{
				inst->morph_weights = (float*)calloc(inst->target_count, sizeof(float));
				inst->pending_weights = (float*)calloc(inst->target_count, sizeof(float));
				if (!inst->morph_weights || !inst->pending_weights)
				{
					free(mesh_nodes);
					res = cgltf_skin_bvh_result_out_of_memory;
					goto fail;
				}
			}

			/* primitives */
			inst->prims_count = mesh->primitives_count;
			if (inst->prims_count)
			{
				inst->prims = (sbvh_prim*)calloc(inst->prims_count, sizeof(sbvh_prim));
				if (!inst->prims)
				{
					free(mesh_nodes);
					res = cgltf_skin_bvh_result_out_of_memory;
					goto fail;
				}
				for (cgltf_size p = 0; p < inst->prims_count; ++p)
				{
					res = sbvh_build_prim(bvh, inst, &inst->prims[p], &mesh->primitives[p], &leaf_total, &max_prim_nodes);
					if (res != cgltf_skin_bvh_result_success)
					{
						free(mesh_nodes);
						goto fail;
					}
					if (inst->prims[p].triangle_count)
						inst->empty = 0;
				}
			}

			/* static local bounds (topology of the top-level BVH) */
			for (int a = 0; a < 3; ++a)
			{
				inst->base_bmin[a] = 0.0f;
				inst->base_bmax[a] = 0.0f;
			}
			int first_prim = 1;
			for (cgltf_size p = 0; p < inst->prims_count; ++p)
			{
				sbvh_prim* prim = &inst->prims[p];
				if (!prim->node_count)
					continue;
				for (int a = 0; a < 3; ++a)
				{
					float lo = prim->nodes[0].bmin[a], hi = prim->nodes[0].bmax[a];
					if (first_prim) { inst->base_bmin[a] = lo; inst->base_bmax[a] = hi; }
					if (lo < inst->base_bmin[a]) inst->base_bmin[a] = lo;
					if (hi > inst->base_bmax[a]) inst->base_bmax[a] = hi;
				}
				first_prim = 0;
			}
		}
		free(mesh_nodes);
	}

	/* top-level BVH over non-empty instances */
	cgltf_size nonempty = 0;
	for (cgltf_size i = 0; i < bvh->instances_count; ++i)
		if (!bvh->instances[i].empty)
			++nonempty;
	if (nonempty)
	{
		cgltf_uint* items = (cgltf_uint*)malloc(nonempty * sizeof(cgltf_uint));
		if (!items)
		{
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
		cgltf_size at = 0;
		for (cgltf_size i = 0; i < bvh->instances_count; ++i)
			if (!bvh->instances[i].empty)
				items[at++] = (cgltf_uint)i;
		bvh->top_nodes = (sbvh_tnode*)calloc(2 * nonempty, sizeof(sbvh_tnode));
		if (!bvh->top_nodes)
		{
			free(items);
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
		sbvh_top_builder builder;
		builder.nodes = bvh->top_nodes;
		builder.count = 0;
		builder.instances = bvh->instances;
		builder.items = items;
		sbvh_build_top(&builder, items, (cgltf_uint)nonempty);
		bvh->top_node_count = builder.count;
		free(items);
	}

	/* pose and traversal workspaces (all heap allocation happens here) */
	bvh->pose_mats = (float*)malloc((bvh->nodes_count ? bvh->nodes_count : 1) * 16 * sizeof(float));
	if (!bvh->pose_mats)
	{
		res = cgltf_skin_bvh_result_out_of_memory;
		goto fail;
	}
	if (leaf_total)
	{
		bvh->leaf_scratch = (float*)malloc(leaf_total * 6 * sizeof(float));
		if (!bvh->leaf_scratch)
		{
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
	}
	if (bvh->top_node_count)
	{
		bvh->top_stack = (cgltf_uint*)malloc(bvh->top_node_count * sizeof(cgltf_uint));
		if (!bvh->top_stack)
		{
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
	}
	if (max_prim_nodes)
	{
		bvh->prim_stack = (cgltf_uint*)malloc(max_prim_nodes * sizeof(cgltf_uint));
		if (!bvh->prim_stack)
		{
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
	}

	/* the initial pose is the static pose stored in the file */
	{
		float* initial = (float*)malloc((bvh->nodes_count ? bvh->nodes_count : 1) * 16 * sizeof(float));
		if (!initial)
		{
			res = cgltf_skin_bvh_result_out_of_memory;
			goto fail;
		}
		for (cgltf_size i = 0; i < bvh->nodes_count; ++i)
			cgltf_node_transform_world(&data->nodes[i], initial + i * 16);
		res = cgltf_skin_bvh_submit_pose(bvh, initial, NULL, NULL, NULL);
		free(initial);
		if (res != cgltf_skin_bvh_result_success)
			goto fail;
	}

	*out_bvh = bvh;
	return cgltf_skin_bvh_result_success;

fail:
	cgltf_skin_bvh_destroy(bvh);
	return res;
}

/* ------------------------------------------------------------------ */
/* pose submit                                                         */

static cgltf_skin_bvh_result sbvh_resolve_weights(cgltf_skin_bvh* bvh, sbvh_instance* inst,
	const float* const* overrides, float* out)
{
	cgltf_size count = inst->target_count;
	const float* src = NULL;
	if (count == 0)
		return cgltf_skin_bvh_result_success;
	if (overrides && overrides[inst->node_index])
		src = overrides[inst->node_index];
	else if (inst->node->weights_count > 0)
		src = inst->node->weights;
	else if (inst->mesh->weights_count > 0)
		src = inst->mesh->weights;
	if (src)
	{
		memcpy(out, src, count * sizeof(float));
		for (cgltf_size k = 0; k < count; ++k)
			if (!isfinite(out[k]))
				return cgltf_skin_bvh_result_invalid_data;
	}
	else
	{
		memset(out, 0, count * sizeof(float));
	}
	(void)bvh;
	return cgltf_skin_bvh_result_success;
}

cgltf_skin_bvh_result cgltf_skin_bvh_submit_pose(cgltf_skin_bvh* bvh,
	const float* world_matrices, const float* const* morph_weights,
	const cgltf_size* morph_weight_counts, cgltf_skin_bvh_stats* stats)
{
	if (stats)
		memset(stats, 0, sizeof(*stats));
	if (!bvh || !world_matrices)
		return cgltf_skin_bvh_result_invalid_argument;
	if ((morph_weights != NULL) != (morph_weight_counts != NULL))
		return cgltf_skin_bvh_result_invalid_argument;

	/* all matrices must be finite and exactly affine */
	for (cgltf_size i = 0; i < bvh->nodes_count; ++i)
	{
		const float* m = world_matrices + i * 16;
		for (int k = 0; k < 16; ++k)
			if (!isfinite(m[k]))
				return cgltf_skin_bvh_result_invalid_data;
		if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f)
			return cgltf_skin_bvh_result_invalid_data;
	}

	/* override lengths must match the target counts of each node's mesh */
	if (morph_weights)
	{
		for (cgltf_size i = 0; i < bvh->nodes_count; ++i)
		{
			if (!morph_weights[i])
				continue;
			const cgltf_node* node = &bvh->data->nodes[i];
			cgltf_size expected = 0;
			if (node->mesh)
				expected = bvh->mesh_target_counts[cgltf_mesh_index(bvh->data, node->mesh)];
			if (morph_weight_counts[i] != expected)
				return cgltf_skin_bvh_result_invalid_data;
			for (cgltf_size k = 0; k < expected; ++k)
				if (!isfinite(morph_weights[i][k]))
					return cgltf_skin_bvh_result_invalid_data;
		}
	}

	/* evaluate conservative bounds for every leaf into scratch space */
	for (cgltf_size ii = 0; ii < bvh->instances_count; ++ii)
	{
		sbvh_instance* inst = &bvh->instances[ii];
		if (inst->empty)
			continue;
		cgltf_skin_bvh_result res = sbvh_resolve_weights(bvh, inst, morph_weights, inst->pending_weights);
		if (res != cgltf_skin_bvh_result_success)
			return res;
		if (inst->skinned)
		{
			for (cgltf_size j = 0; j < inst->joints_count; ++j)
			{
				float* mj = inst->pending_joints + j * 16;
				sbvh_mat_mul(world_matrices + inst->joint_nodes[j] * 16, inst->ibms + j * 16, mj);
				for (int k = 0; k < 12; ++k)
					if (!isfinite(mj[k]))
						return cgltf_skin_bvh_result_invalid_data;
			}
		}
		const float* node_mat = world_matrices + inst->node_index * 16;
		for (cgltf_size p = 0; p < inst->prims_count; ++p)
		{
			sbvh_prim* prim = &inst->prims[p];
			if (!prim->leaf_count)
				continue;
			float expand = 0.0f;
			for (cgltf_size k = 0; k < prim->target_count; ++k)
				expand += fabsf(inst->pending_weights[k]) * prim->morph_radius[k];
			if (!isfinite(expand))
				return cgltf_skin_bvh_result_invalid_data;
			for (cgltf_size li = 0; li < prim->leaf_count; ++li)
			{
				const sbvh_leaf* leaf = &prim->leaves[li];
				float lmin[3], lmax[3];
				for (int a = 0; a < 3; ++a)
				{
					lmin[a] = leaf->lmin[a] - expand;
					lmax[a] = leaf->lmax[a] + expand;
				}
				float* out = bvh->leaf_scratch + (prim->leaf_base + li) * 6;
				out[0] = out[1] = out[2] = INFINITY;
				out[3] = out[4] = out[5] = -INFINITY;
				if (!inst->skinned)
				{
					sbvh_box_transform_merge(node_mat, lmin, lmax, out + 0, out + 3);
					if (stats)
						stats->bound_evaluations += 1;
				}
				else
				{
					for (cgltf_uint ji = 0; ji < leaf->joint_count; ++ji)
					{
						cgltf_uint j = prim->leaf_joints[leaf->joint_offset + ji];
						sbvh_box_transform_merge(inst->pending_joints + (cgltf_size)j * 16, lmin, lmax, out + 0, out + 3);
					}
					if (stats)
						stats->bound_evaluations += leaf->joint_count;
				}
				if (!sbvh_finite3(out + 0) || !sbvh_finite3(out + 3))
					return cgltf_skin_bvh_result_invalid_data;
			}
		}
	}

	/* everything validated: commit the pose */
	memcpy(bvh->pose_mats, world_matrices, bvh->nodes_count * 16 * sizeof(float));
	for (cgltf_size ii = 0; ii < bvh->instances_count; ++ii)
	{
		sbvh_instance* inst = &bvh->instances[ii];
		if (inst->empty)
			continue;
		float* swap;
		swap = inst->joint_mats; inst->joint_mats = inst->pending_joints; inst->pending_joints = swap;
		swap = inst->morph_weights; inst->morph_weights = inst->pending_weights; inst->pending_weights = swap;
		for (cgltf_size p = 0; p < inst->prims_count; ++p)
		{
			sbvh_prim* prim = &inst->prims[p];
			if (!prim->node_count)
				continue;
			for (cgltf_size li = 0; li < prim->leaf_count; ++li)
			{
				const sbvh_leaf* leaf = &prim->leaves[li];
				const float* box = bvh->leaf_scratch + (prim->leaf_base + li) * 6;
				sbvh_node* node = &prim->nodes[leaf->node_index];
				memcpy(node->bmin, box + 0, 3 * sizeof(float));
				memcpy(node->bmax, box + 3, 3 * sizeof(float));
			}
			for (cgltf_size ni = prim->node_count; ni-- > 0;)
			{
				sbvh_node* node = &prim->nodes[ni];
				if (node->count)
					continue;
				const sbvh_node* l = &prim->nodes[node->left];
				const sbvh_node* r = &prim->nodes[node->right];
				for (int a = 0; a < 3; ++a)
				{
					node->bmin[a] = l->bmin[a] < r->bmin[a] ? l->bmin[a] : r->bmin[a];
					node->bmax[a] = l->bmax[a] > r->bmax[a] ? l->bmax[a] : r->bmax[a];
				}
			}
		}
		int first_prim = 1;
		for (cgltf_size p = 0; p < inst->prims_count; ++p)
		{
			sbvh_prim* prim = &inst->prims[p];
			if (!prim->node_count)
				continue;
			for (int a = 0; a < 3; ++a)
			{
				float lo = prim->nodes[0].bmin[a], hi = prim->nodes[0].bmax[a];
				if (first_prim) { inst->bmin[a] = lo; inst->bmax[a] = hi; }
				if (lo < inst->bmin[a]) inst->bmin[a] = lo;
				if (hi > inst->bmax[a]) inst->bmax[a] = hi;
			}
			first_prim = 0;
		}
	}
	for (cgltf_size ni = bvh->top_node_count; ni-- > 0;)
	{
		sbvh_tnode* node = &bvh->top_nodes[ni];
		if (node->left == SBVH_TOP_LEAF)
		{
			const sbvh_instance* inst = &bvh->instances[node->instance];
			memcpy(node->bmin, inst->bmin, sizeof(node->bmin));
			memcpy(node->bmax, inst->bmax, sizeof(node->bmax));
		}
		else
		{
			const sbvh_tnode* l = &bvh->top_nodes[node->left];
			const sbvh_tnode* r = &bvh->top_nodes[node->right];
			for (int a = 0; a < 3; ++a)
			{
				node->bmin[a] = l->bmin[a] < r->bmin[a] ? l->bmin[a] : r->bmin[a];
				node->bmax[a] = l->bmax[a] > r->bmax[a] ? l->bmax[a] : r->bmax[a];
			}
		}
	}
	bvh->generation += 1;
	return cgltf_skin_bvh_result_success;
}

/* ------------------------------------------------------------------ */
/* raycast                                                             */

typedef struct
{
	int found;
	float t;
	cgltf_size node;
	cgltf_size mesh;
	cgltf_size prim;
	cgltf_size tri;
	float bary[3];
	float normal[3];
} sbvh_best;

static int sbvh_better(const sbvh_best* best, float t, cgltf_size node, cgltf_size prim, cgltf_size tri)
{
	if (!best->found || t < best->t)
		return 1;
	if (t > best->t)
		return 0;
	if (node != best->node)
		return node < best->node;
	if (prim != best->prim)
		return prim < best->prim;
	return tri < best->tri;
}

static const float* sbvh_deform_vertex(cgltf_skin_bvh* bvh, const sbvh_instance* inst, sbvh_prim* prim,
	cgltf_size v, cgltf_skin_bvh_stats* stats)
{
	float* cache = prim->cache_pos + v * 3;
	if (prim->cache_gen[v] == bvh->generation)
		return cache;
	const float* p = prim->positions + v * 3;
	float qx = p[0], qy = p[1], qz = p[2];
	for (cgltf_size k = 0; k < prim->target_count; ++k)
	{
		float w = inst->morph_weights[k];
		if (w != 0.0f)
		{
			const float* d = prim->morph_deltas + (k * prim->vertex_count + v) * 3;
			qx += w * d[0];
			qy += w * d[1];
			qz += w * d[2];
		}
	}
	if (!inst->skinned)
	{
		sbvh_mat_apply(bvh->pose_mats + inst->node_index * 16, qx, qy, qz, cache);
	}
	else
	{
		float rx = 0.0f, ry = 0.0f, rz = 0.0f;
		cgltf_size base = v * SBVH_MAX_INFLUENCES;
		for (cgltf_size i = 0; i < prim->infl_counts[v]; ++i)
		{
			cgltf_uint j = prim->infl_joints[base + i];
			float w = prim->infl_weights[base + i];
			float r[3];
			sbvh_mat_apply(inst->joint_mats + (cgltf_size)j * 16, qx, qy, qz, r);
			rx += w * r[0];
			ry += w * r[1];
			rz += w * r[2];
		}
		cache[0] = rx;
		cache[1] = ry;
		cache[2] = rz;
	}
	prim->cache_gen[v] = bvh->generation;
	if (stats)
		stats->deformed_vertices += 1;
	return cache;
}

static void sbvh_raycast_prim(cgltf_skin_bvh* bvh, sbvh_instance* inst, sbvh_prim* prim,
	const float o[3], const float d[3], float tmin, sbvh_best* best, cgltf_skin_bvh_stats* stats)
{
	if (!prim->node_count)
		return;
	cgltf_uint* stack = bvh->prim_stack;
	cgltf_size sp = 0;
	stack[sp++] = 0;
	while (sp)
	{
		cgltf_uint ni = stack[--sp];
		sbvh_node* node = &prim->nodes[ni];
		if (stats)
			stats->bvh_nodes_visited += 1;
		if (node->count)
		{
			for (cgltf_uint t = node->first; t < node->first + node->count; ++t)
			{
				cgltf_uint tid = prim->tri_order[t];
				cgltf_uint i0 = prim->indices[tid * 3 + 0];
				cgltf_uint i1 = prim->indices[tid * 3 + 1];
				cgltf_uint i2 = prim->indices[tid * 3 + 2];
				const float* v0 = sbvh_deform_vertex(bvh, inst, prim, i0, stats);
				const float* v1 = sbvh_deform_vertex(bvh, inst, prim, i1, stats);
				const float* v2 = sbvh_deform_vertex(bvh, inst, prim, i2, stats);
				if (stats)
					stats->triangle_tests += 1;
				float hit_t, hit_u, hit_v;
				if (!sbvh_ray_tri(o, d, v0, v1, v2, tmin, best->t, &hit_t, &hit_u, &hit_v))
					continue;
				if (!sbvh_better(best, hit_t, inst->node_index, prim->primitive_index, tid))
					continue;
				best->found = 1;
				best->t = hit_t;
				best->node = inst->node_index;
				best->mesh = prim->mesh_index;
				best->prim = prim->primitive_index;
				best->tri = tid;
				best->bary[0] = 1.0f - hit_u - hit_v;
				best->bary[1] = hit_u;
				best->bary[2] = hit_v;
				float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
				float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
				float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
				float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (len > 0.0f)
				{
					best->normal[0] = n[0] / len;
					best->normal[1] = n[1] / len;
					best->normal[2] = n[2] / len;
				}
				else
				{
					best->normal[0] = best->normal[1] = best->normal[2] = 0.0f;
				}
			}
		}
		else
		{
			float tn_l = 0.0f, tn_r = 0.0f;
			const sbvh_node* l = &prim->nodes[node->left];
			const sbvh_node* r = &prim->nodes[node->right];
			int hl = sbvh_ray_box(o, d, l->bmin, l->bmax, tmin, best->t, &tn_l);
			int hr = sbvh_ray_box(o, d, r->bmin, r->bmax, tmin, best->t, &tn_r);
			if (hl && hr)
			{
				if (tn_l <= tn_r) { stack[sp++] = node->right; stack[sp++] = node->left; }
				else { stack[sp++] = node->left; stack[sp++] = node->right; }
			}
			else if (hl) stack[sp++] = node->left;
			else if (hr) stack[sp++] = node->right;
		}
	}
}

cgltf_skin_bvh_result cgltf_skin_bvh_raycast(cgltf_skin_bvh* bvh,
	const float origin[3], const float direction[3], float tmin, float tmax,
	cgltf_skin_bvh_hit* out_hit, cgltf_skin_bvh_stats* stats)
{
	if (stats)
		memset(stats, 0, sizeof(*stats));
	if (!bvh || !origin || !direction || !out_hit)
		return cgltf_skin_bvh_result_invalid_argument;
	for (int a = 0; a < 3; ++a)
		if (!isfinite(origin[a]) || !isfinite(direction[a]))
			return cgltf_skin_bvh_result_invalid_argument;
	if (direction[0] == 0.0f && direction[1] == 0.0f && direction[2] == 0.0f)
		return cgltf_skin_bvh_result_invalid_argument;
	if (!isfinite(tmin) || !isfinite(tmax) || tmin > tmax)
		return cgltf_skin_bvh_result_invalid_argument;
	if (!bvh->top_node_count)
		return cgltf_skin_bvh_result_no_hit;

	sbvh_best best;
	memset(&best, 0, sizeof(best));
	best.t = tmax;

	cgltf_uint* stack = bvh->top_stack;
	cgltf_size sp = 0;
	stack[sp++] = 0;
	while (sp)
	{
		cgltf_uint ni = stack[--sp];
		const sbvh_tnode* node = &bvh->top_nodes[ni];
		if (stats)
			stats->bvh_nodes_visited += 1;
		if (node->left == SBVH_TOP_LEAF)
		{
			sbvh_instance* inst = &bvh->instances[node->instance];
			for (cgltf_size p = 0; p < inst->prims_count; ++p)
				sbvh_raycast_prim(bvh, inst, &inst->prims[p], origin, direction, tmin, &best, stats);
		}
		else
		{
			float tn_l = 0.0f, tn_r = 0.0f;
			const sbvh_tnode* l = &bvh->top_nodes[node->left];
			const sbvh_tnode* r = &bvh->top_nodes[node->right];
			int hl = sbvh_ray_box(origin, direction, l->bmin, l->bmax, tmin, best.t, &tn_l);
			int hr = sbvh_ray_box(origin, direction, r->bmin, r->bmax, tmin, best.t, &tn_r);
			if (hl && hr)
			{
				if (tn_l <= tn_r) { stack[sp++] = node->right; stack[sp++] = node->left; }
				else { stack[sp++] = node->left; stack[sp++] = node->right; }
			}
			else if (hl) stack[sp++] = node->left;
			else if (hr) stack[sp++] = node->right;
		}
	}

	if (!best.found)
		return cgltf_skin_bvh_result_no_hit;
	out_hit->node_index = best.node;
	out_hit->mesh_index = best.mesh;
	out_hit->primitive_index = best.prim;
	out_hit->triangle_index = best.tri;
	out_hit->t = best.t;
	out_hit->barycentric[0] = best.bary[0];
	out_hit->barycentric[1] = best.bary[1];
	out_hit->barycentric[2] = best.bary[2];
	out_hit->position[0] = origin[0] + best.t * direction[0];
	out_hit->position[1] = origin[1] + best.t * direction[1];
	out_hit->position[2] = origin[2] + best.t * direction[2];
	out_hit->normal[0] = best.normal[0];
	out_hit->normal[1] = best.normal[1];
	out_hit->normal[2] = best.normal[2];
	return cgltf_skin_bvh_result_success;
}
