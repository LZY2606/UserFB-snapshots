/*
 * skin_bvh_test - target test cases for examples/cgltf_skin_bvh.
 *
 * Scenes are generated in C (no files, no downloads). The reference is an
 * independent per-vertex morph+skin evaluation followed by an exhaustive
 * triangle scan; it is used to verify closest hits, barycentric weights and
 * original triangle identities across many poses. Registered through
 * task_support/cases.h; no main() here.
 */
#include "../cgltf.h"
#include "../examples/cgltf_skin_bvh.h"
#include "../task_support/cases.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* scene builder: fills a cgltf_data with manually owned storage       */

#define TB_MAX_VIEWS 64
#define TB_MAX_ACCS 64
#define TB_MAX_NODES 40
#define TB_MAX_MESHES 10
#define TB_MAX_PRIMS 10
#define TB_MAX_ATTRS 80
#define TB_MAX_TARGETS 20
#define TB_MAX_SKINS 10
#define TB_MAX_PTRS 80
#define TB_MAX_WEIGHTS 32

typedef struct
{
	cgltf_data data;
	cgltf_buffer buffer;
	unsigned char* bytes;
	cgltf_size bytes_size, bytes_cap;
	cgltf_buffer_view views[TB_MAX_VIEWS]; cgltf_size views_count;
	cgltf_accessor accs[TB_MAX_ACCS]; cgltf_size accs_count;
	cgltf_mesh meshes[TB_MAX_MESHES]; cgltf_size meshes_count;
	cgltf_primitive prims[TB_MAX_PRIMS]; cgltf_size prims_count;
	cgltf_attribute attrs[TB_MAX_ATTRS]; cgltf_size attrs_count;
	cgltf_morph_target targets[TB_MAX_TARGETS]; cgltf_size targets_count;
	cgltf_node nodes[TB_MAX_NODES]; cgltf_size nodes_count;
	cgltf_skin skins[TB_MAX_SKINS]; cgltf_size skins_count;
	cgltf_node* ptrs[TB_MAX_PTRS]; cgltf_size ptrs_count;
	cgltf_scene scenes[1];
	float weights_pool[TB_MAX_WEIGHTS]; cgltf_size weights_count;
} tb;

static void* tb_grow(tb* b, cgltf_size need)
{
	if (b->bytes_size + need > b->bytes_cap)
	{
		cgltf_size cap = (b->bytes_cap ? b->bytes_cap * 2 : 4096);
		while (cap < b->bytes_size + need) cap *= 2;
		unsigned char* nb = (unsigned char*)realloc(b->bytes, cap);
		TARGET_CHECK(nb != NULL);
		b->bytes = nb;
		b->bytes_cap = cap;
	}
	return b->bytes + b->bytes_size;
}

static void tb_init(tb* b)
{
	memset(b, 0, sizeof(*b));
	b->data.buffers = &b->buffer;
	b->data.buffers_count = 1;
	b->data.buffer_views = b->views;
	b->data.accessors = b->accs;
	b->data.meshes = b->meshes;
	b->data.nodes = b->nodes;
	b->data.skins = b->skins;
	b->data.scenes = b->scenes;
	b->data.scenes_count = 1;
	b->data.scene = &b->scenes[0];
}

static void tb_finish(tb* b)
{
	b->buffer.data = b->bytes;
	b->buffer.size = b->bytes_size;
	b->data.buffer_views_count = b->views_count;
	b->data.accessors_count = b->accs_count;
	b->data.meshes_count = b->meshes_count;
	b->data.nodes_count = b->nodes_count;
	b->data.skins_count = b->skins_count;
}

static void tb_free(tb* b)
{
	free(b->bytes);
	b->bytes = NULL;
}

static cgltf_buffer_view* tb_view(tb* b, const void* data, cgltf_size size, cgltf_size stride)
{
	TARGET_CHECK(b->views_count < TB_MAX_VIEWS);
	cgltf_size pad = (4 - (b->bytes_size & 3)) & 3;
	b->bytes_size += pad;
	unsigned char* at = tb_grow(b, size ? size : 1);
	if (data && size)
		memcpy(at, data, size);
	else
		memset(at, 0, size ? size : 1);
	cgltf_buffer_view* v = &b->views[b->views_count++];
	memset(v, 0, sizeof(*v));
	v->buffer = &b->buffer;
	v->offset = b->bytes_size;
	v->size = size;
	v->stride = stride;
	b->bytes_size += size;
	return v;
}

static cgltf_accessor* tb_acc(tb* b, cgltf_buffer_view* view, cgltf_size offset,
	cgltf_component_type comp, cgltf_bool norm, cgltf_type type, cgltf_size count, cgltf_size stride)
{
	TARGET_CHECK(b->accs_count < TB_MAX_ACCS);
	cgltf_accessor* a = &b->accs[b->accs_count++];
	memset(a, 0, sizeof(*a));
	a->buffer_view = view;
	a->offset = offset;
	a->component_type = comp;
	a->normalized = norm;
	a->type = type;
	a->count = count;
	a->stride = stride;
	return a;
}

/* tightly packed accessor over a copy of src */
static cgltf_accessor* tb_pack(tb* b, cgltf_component_type comp, cgltf_bool norm, cgltf_type type,
	cgltf_size count, const void* src, cgltf_size elem_size)
{
	cgltf_buffer_view* v = tb_view(b, src, count * elem_size, elem_size);
	return tb_acc(b, v, 0, comp, norm, type, count, elem_size);
}

static cgltf_mesh* tb_mesh(tb* b)
{
	TARGET_CHECK(b->meshes_count < TB_MAX_MESHES);
	cgltf_mesh* m = &b->meshes[b->meshes_count++];
	memset(m, 0, sizeof(*m));
	return m;
}

static cgltf_primitive* tb_prim(tb* b, cgltf_mesh* m)
{
	TARGET_CHECK(b->prims_count < TB_MAX_PRIMS);
	cgltf_primitive* p = &b->prims[b->prims_count++];
	memset(p, 0, sizeof(*p));
	p->type = cgltf_primitive_type_triangles;
	p->attributes = &b->attrs[b->attrs_count];
	if (!m->primitives)
		m->primitives = p;
	m->primitives_count++;
	return p;
}

static void tb_attr(tb* b, cgltf_primitive* p, cgltf_attribute_type type, cgltf_int index, cgltf_accessor* acc)
{
	TARGET_CHECK(b->attrs_count < TB_MAX_ATTRS);
	cgltf_attribute* a = &b->attrs[b->attrs_count++];
	memset(a, 0, sizeof(*a));
	a->type = type;
	a->index = index;
	a->data = acc;
	p->attributes_count++;
}

static cgltf_morph_target* tb_target(tb* b, cgltf_primitive* p)
{
	TARGET_CHECK(b->targets_count < TB_MAX_TARGETS);
	cgltf_morph_target* t = &b->targets[b->targets_count++];
	memset(t, 0, sizeof(*t));
	t->attributes = &b->attrs[b->attrs_count];
	if (!p->targets)
		p->targets = t;
	p->targets_count++;
	return t;
}

static void tb_target_attr(tb* b, cgltf_morph_target* t, cgltf_attribute_type type, cgltf_accessor* acc)
{
	TARGET_CHECK(b->attrs_count < TB_MAX_ATTRS);
	cgltf_attribute* a = &b->attrs[b->attrs_count++];
	memset(a, 0, sizeof(*a));
	a->type = type;
	a->index = 0;
	a->data = acc;
	t->attributes_count++;
}

static cgltf_node* tb_node(tb* b)
{
	TARGET_CHECK(b->nodes_count < TB_MAX_NODES);
	cgltf_node* n = &b->nodes[b->nodes_count++];
	memset(n, 0, sizeof(*n));
	n->rotation[3] = 1.0f; /* cgltf_parse defaults: identity rotation, unit scale */
	n->scale[0] = n->scale[1] = n->scale[2] = 1.0f;
	return n;
}

static void tb_node_translation(cgltf_node* n, float x, float y, float z)
{
	n->has_translation = 1;
	n->translation[0] = x; n->translation[1] = y; n->translation[2] = z;
}

static void tb_node_rotation(cgltf_node* n, float x, float y, float z, float w)
{
	n->has_rotation = 1;
	n->rotation[0] = x; n->rotation[1] = y; n->rotation[2] = z; n->rotation[3] = w;
}

static void tb_node_matrix(cgltf_node* n, const float* m16)
{
	n->has_matrix = 1;
	memcpy(n->matrix, m16, 16 * sizeof(float));
}

static void tb_root(tb* b, cgltf_node* n)
{
	TARGET_CHECK(b->ptrs_count < TB_MAX_PTRS);
	if (!b->scenes[0].nodes)
		b->scenes[0].nodes = &b->ptrs[b->ptrs_count];
	b->ptrs[b->ptrs_count++] = n;
	b->scenes[0].nodes_count++;
}

static cgltf_skin* tb_skin(tb* b, cgltf_size joints_count, cgltf_accessor* ibm)
{
	TARGET_CHECK(b->skins_count < TB_MAX_SKINS);
	TARGET_CHECK(b->ptrs_count + joints_count <= TB_MAX_PTRS);
	cgltf_skin* s = &b->skins[b->skins_count++];
	memset(s, 0, sizeof(*s));
	s->joints = &b->ptrs[b->ptrs_count];
	b->ptrs_count += joints_count;
	s->joints_count = joints_count;
	s->inverse_bind_matrices = ibm;
	return s;
}

static void tb_node_weights(tb* b, cgltf_node* n, const float* w, cgltf_size count)
{
	TARGET_CHECK(b->weights_count + count <= TB_MAX_WEIGHTS);
	n->weights = &b->weights_pool[b->weights_count];
	if (count)
		memcpy(n->weights, w, count * sizeof(float));
	b->weights_count += count;
	n->weights_count = count;
}

static void tb_mesh_weights(tb* b, cgltf_mesh* m, const float* w, cgltf_size count)
{
	TARGET_CHECK(b->weights_count + count <= TB_MAX_WEIGHTS);
	m->weights = &b->weights_pool[b->weights_count];
	if (count)
		memcpy(m->weights, w, count * sizeof(float));
	b->weights_count += count;
	m->weights_count = count;
}

static cgltf_accessor* tb_ibm(tb* b, const float* mats, cgltf_size count)
{
	return tb_pack(b, cgltf_component_type_r_32f, 0, cgltf_type_mat4, count, mats, 64);
}

static void tb_fill_world(tb* b, float* out)
{
	for (cgltf_size i = 0; i < b->nodes_count; ++i)
		cgltf_node_transform_world(&b->nodes[i], out + i * 16);
}

/* ------------------------------------------------------------------ */
/* reference: independent per-vertex morph+skin and exhaustive scan    */

typedef struct
{
	const cgltf_data* data;
	const float* world;        /* 16 floats per node */
	const float* const* morph; /* per-node overrides or NULL */
} ref_pose;

static void ref_mat_apply(const float* m, float x, float y, float z, float* out)
{
	out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
	out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
	out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

static void ref_mat_mul(const float* a, const float* b, float* out)
{
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

static void ref_morph_weights(const ref_pose* r, const cgltf_node* node, cgltf_size node_index,
	cgltf_size target_count, float* out)
{
	const float* src = NULL;
	if (r->morph && r->morph[node_index])
		src = r->morph[node_index];
	else if (node->weights_count > 0)
		src = node->weights;
	else if (node->mesh && node->mesh->weights_count > 0)
		src = node->mesh->weights;
	for (cgltf_size k = 0; k < target_count; ++k)
		out[k] = src ? src[k] : 0.0f;
}

static void ref_deform(const ref_pose* r, const cgltf_node* node, cgltf_size node_index,
	const cgltf_primitive* prim, cgltf_size v, float* out)
{
	const cgltf_accessor* pos = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0);
	float p[3] = {0, 0, 0};
	TARGET_CHECK(cgltf_accessor_read_float(pos, v, p, 3) != 0);
	float w[16];
	TARGET_CHECK(prim->targets_count <= 16);
	ref_morph_weights(r, node, node_index, prim->targets_count, w);
	for (cgltf_size k = 0; k < prim->targets_count; ++k)
	{
		if (w[k] == 0.0f)
			continue;
		const cgltf_morph_target* target = &prim->targets[k];
		const cgltf_accessor* dacc = NULL;
		for (cgltf_size a = 0; a < target->attributes_count; ++a)
			if (target->attributes[a].type == cgltf_attribute_type_position)
				dacc = target->attributes[a].data;
		if (!dacc)
			continue;
		float d[3] = {0, 0, 0};
		TARGET_CHECK(cgltf_accessor_read_float(dacc, v, d, 3) != 0);
		p[0] += w[k] * d[0];
		p[1] += w[k] * d[1];
		p[2] += w[k] * d[2];
	}
	if (!node->skin)
	{
		ref_mat_apply(r->world + node_index * 16, p[0], p[1], p[2], out);
		return;
	}
	const cgltf_skin* skin = node->skin;
	const cgltf_accessor* joints[2], *weights[2];
	joints[0] = cgltf_find_accessor(prim, cgltf_attribute_type_joints, 0);
	joints[1] = cgltf_find_accessor(prim, cgltf_attribute_type_joints, 1);
	weights[0] = cgltf_find_accessor(prim, cgltf_attribute_type_weights, 0);
	weights[1] = cgltf_find_accessor(prim, cgltf_attribute_type_weights, 1);
	cgltf_uint jm[8];
	float wm[8];
	cgltf_size count = 0;
	float total = 0.0f;
	for (int set = 0; set < 2; ++set)
	{
		if (!joints[set])
			continue;
		cgltf_uint jraw[4] = {0, 0, 0, 0};
		float wraw[4] = {0, 0, 0, 0};
		TARGET_CHECK(cgltf_accessor_read_uint(joints[set], v, jraw, 4) != 0);
		TARGET_CHECK(cgltf_accessor_read_float(weights[set], v, wraw, 4) != 0);
		for (int c = 0; c < 4; ++c)
		{
			float wgt = wraw[c];
			total += wgt;
			if (wgt == 0.0f)
				continue;
			cgltf_size k = 0;
			while (k < count && jm[k] != jraw[c])
				++k;
			if (k < count)
				wm[k] += wgt;
			else
			{
				jm[count] = jraw[c];
				wm[count] = wgt;
				++count;
			}
		}
	}
	TARGET_CHECK(count > 0 && total > 0.0f);
	float acc[3] = {0, 0, 0};
	for (cgltf_size k = 0; k < count; ++k)
	{
		cgltf_size jn = cgltf_node_index(r->data, skin->joints[jm[k]]);
		float ibm[16];
		if (skin->inverse_bind_matrices)
			TARGET_CHECK(cgltf_accessor_read_float(skin->inverse_bind_matrices, jm[k], ibm, 16) != 0);
		else
		{
			memset(ibm, 0, sizeof(ibm));
			ibm[0] = ibm[5] = ibm[10] = ibm[15] = 1.0f;
		}
		float m[16], t[3];
		ref_mat_mul(r->world + jn * 16, ibm, m);
		ref_mat_apply(m, p[0], p[1], p[2], t);
		float wn = wm[k] / total;
		acc[0] += wn * t[0];
		acc[1] += wn * t[1];
		acc[2] += wn * t[2];
	}
	out[0] = acc[0];
	out[1] = acc[1];
	out[2] = acc[2];
}

typedef struct
{
	int found;
	cgltf_size node, prim, tri;
	float t, bary[3], pos[3], nrm[3];
} ref_hit;

static int ref_ray_tri(const float o[3], const float d[3], const float v0[3], const float v1[3], const float v2[3],
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

static int ref_better(const ref_hit* best, float t, cgltf_size node, cgltf_size prim, cgltf_size tri)
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

static void ref_ray_node(const ref_pose* r, const cgltf_node* node, const float o[3], const float d[3],
	float tmin, ref_hit* best)
{
	if (node->mesh)
	{
		cgltf_size ni = cgltf_node_index(r->data, node);
		for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi)
		{
			const cgltf_primitive* prim = &node->mesh->primitives[pi];
			if (prim->type != cgltf_primitive_type_triangles)
				continue;
			const cgltf_accessor* pos = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0);
			cgltf_size tri_count = prim->indices ? prim->indices->count / 3 : pos->count / 3;
			for (cgltf_size ti = 0; ti < tri_count; ++ti)
			{
				cgltf_size iv[3];
				for (int c = 0; c < 3; ++c)
					iv[c] = prim->indices ? cgltf_accessor_read_index(prim->indices, ti * 3 + (cgltf_size)c) : ti * 3 + (cgltf_size)c;
				float v0[3], v1[3], v2[3];
				ref_deform(r, node, ni, prim, iv[0], v0);
				ref_deform(r, node, ni, prim, iv[1], v1);
				ref_deform(r, node, ni, prim, iv[2], v2);
				float t, u, v;
				if (!ref_ray_tri(o, d, v0, v1, v2, tmin, best->t, &t, &u, &v))
					continue;
				if (!ref_better(best, t, ni, pi, ti))
					continue;
				best->found = 1;
				best->node = ni;
				best->prim = pi;
				best->tri = ti;
				best->t = t;
				best->bary[0] = 1.0f - u - v;
				best->bary[1] = u;
				best->bary[2] = v;
				best->pos[0] = o[0] + t * d[0];
				best->pos[1] = o[1] + t * d[1];
				best->pos[2] = o[2] + t * d[2];
				float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
				float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
				float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
				float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				best->nrm[0] = n[0] / len;
				best->nrm[1] = n[1] / len;
				best->nrm[2] = n[2] / len;
			}
		}
	}
	for (cgltf_size c = 0; c < node->children_count; ++c)
		ref_ray_node(r, node->children[c], o, d, tmin, best);
}

static void ref_raycast(const ref_pose* r, const cgltf_scene* scene, const float o[3], const float d[3],
	float tmin, float tmax, ref_hit* hit)
{
	memset(hit, 0, sizeof(*hit));
	hit->t = tmax;
	for (cgltf_size i = 0; i < scene->nodes_count; ++i)
		ref_ray_node(r, scene->nodes[i], o, d, tmin, hit);
}

/* ------------------------------------------------------------------ */
/* comparison helpers                                                  */

static int closef(float a, float b)
{
	return fabsf(a - b) <= 1.0e-5f * (1.0f + fabsf(a) + fabsf(b));
}

static void check_ray(cgltf_skin_bvh* bvh, const ref_pose* r, const cgltf_scene* scene,
	const float o[3], const float d[3], float tmin, float tmax)
{
	ref_hit rh;
	ref_raycast(r, scene, o, d, tmin, tmax, &rh);
	cgltf_skin_bvh_hit hit;
	cgltf_skin_bvh_result res = cgltf_skin_bvh_raycast(bvh, o, d, tmin, tmax, &hit, NULL);
	TARGET_CHECK((res == cgltf_skin_bvh_result_success) == (rh.found != 0));
	if (!rh.found)
	{
		TARGET_CHECK(res == cgltf_skin_bvh_result_no_hit);
		return;
	}
	TARGET_CHECK(hit.node_index == rh.node);
	TARGET_CHECK(hit.primitive_index == rh.prim);
	TARGET_CHECK(hit.triangle_index == rh.tri);
	TARGET_CHECK(closef(hit.t, rh.t));
	TARGET_CHECK(closef(hit.barycentric[0], rh.bary[0]));
	TARGET_CHECK(closef(hit.barycentric[1], rh.bary[1]));
	TARGET_CHECK(closef(hit.barycentric[2], rh.bary[2]));
	TARGET_CHECK(closef(hit.position[0], rh.pos[0]));
	TARGET_CHECK(closef(hit.position[1], rh.pos[1]));
	TARGET_CHECK(closef(hit.position[2], rh.pos[2]));
	TARGET_CHECK(closef(hit.normal[0], rh.nrm[0]));
	TARGET_CHECK(closef(hit.normal[1], rh.nrm[1]));
	TARGET_CHECK(closef(hit.normal[2], rh.nrm[2]));
}

/* casts a ray through the reference-deformed centroid of every triangle */
static void check_pose(tb* b, cgltf_skin_bvh* bvh, const float* world, const float* const* morph)
{
	ref_pose r;
	r.data = &b->data;
	r.world = world;
	r.morph = morph;
	for (cgltf_size ni = 0; ni < b->nodes_count; ++ni)
	{
		const cgltf_node* node = &b->nodes[ni];
		if (!node->mesh)
			continue;
		for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi)
		{
			const cgltf_primitive* prim = &node->mesh->primitives[pi];
			const cgltf_accessor* pos = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0);
			cgltf_size tri_count = prim->indices ? prim->indices->count / 3 : pos->count / 3;
			for (cgltf_size ti = 0; ti < tri_count; ++ti)
			{
				float c[3] = {0, 0, 0};
				for (int k = 0; k < 3; ++k)
				{
					cgltf_size iv = prim->indices ? cgltf_accessor_read_index(prim->indices, ti * 3 + (cgltf_size)k) : ti * 3 + (cgltf_size)k;
					float v[3];
					ref_deform(&r, node, ni, prim, iv, v);
					c[0] += v[0] / 3.0f;
					c[1] += v[1] / 3.0f;
					c[2] += v[2] / 3.0f;
				}
				static const float offs[3][3] = {{0.31f, -0.27f, 1.0f}, {-0.53f, 0.11f, -0.9f}, {0.07f, 0.61f, 0.8f}};
				const float* off = offs[ti % 3];
				float len = sqrtf(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
				float d[3] = {-off[0] / len, -off[1] / len, -off[2] / len};
				float o[3] = {c[0] - 5.0f * d[0], c[1] - 5.0f * d[1], c[2] - 5.0f * d[2]};
				check_ray(bvh, &r, &b->scenes[0], o, d, 0.0f, 50.0f);
			}
		}
	}
	/* a ray that must miss everything */
	{
		float o[3] = {1.0e4f, 1.0e4f, 1.0e4f};
		float d[3] = {0.0f, 1.0f, 0.0f};
		check_ray(bvh, &r, &b->scenes[0], o, d, 0.0f, 10.0f);
	}
}

static unsigned long long rng_state = 0x123456789abcdefULL;

static float frand(void)
{
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (float)((rng_state >> 33) & 0xFFFFFF) / 16777216.0f;
}

/* ------------------------------------------------------------------ */
/* deform group                                                        */

/* one mesh instantiated by two nodes with different skins and morphs,
 * non-identity inverse-bind matrices, strided positions, u16 indices */
static void case_shared_mesh_two_skins(void)
{
	tb b;
	tb_init(&b);

	/* strided POSITION (16-byte stride, 4 bytes of padding) */
	float pos_raw[4][4] = {{0, 0, 0, 99}, {1, 0, 0, 99}, {1, 1, 0, 99}, {0, 1, 0, 99}};
	cgltf_accessor* pos = tb_acc(&b, tb_view(&b, pos_raw, sizeof(pos_raw), 16), 0,
		cgltf_component_type_r_32f, 0, cgltf_type_vec3, 4, 16);
	cgltf_uint idx[6] = {0, 1, 2, 0, 2, 3};
	cgltf_accessor* indices = tb_pack(&b, cgltf_component_type_r_16u, 0, cgltf_type_scalar, 6, idx, 2);
	uint8_t joints[4][4] = {{0, 1, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}, {1, 0, 0, 0}};
	cgltf_accessor* joints0 = tb_pack(&b, cgltf_component_type_r_8u, 0, cgltf_type_vec4, 4, joints, 4);
	uint8_t weights[4][4] = {{128, 127, 0, 0}, {0, 255, 0, 0}, {200, 55, 0, 0}, {100, 155, 0, 0}};
	cgltf_accessor* weights0 = tb_pack(&b, cgltf_component_type_r_8u, 1, cgltf_type_vec4, 4, weights, 4);
	float delta[4][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 0, 0}};
	cgltf_accessor* morph = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 4, delta, 12);

	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos);
	tb_attr(&b, prim, cgltf_attribute_type_joints, 0, joints0);
	tb_attr(&b, prim, cgltf_attribute_type_weights, 0, weights0);
	prim->indices = indices;
	cgltf_morph_target* target = tb_target(&b, prim);
	tb_target_attr(&b, target, cgltf_attribute_type_position, morph);
	float mesh_w[1] = {-0.25f};
	tb_mesh_weights(&b, mesh, mesh_w, 1);

	cgltf_node* j0 = tb_node(&b); tb_node_translation(j0, 1, 2, 0);
	cgltf_node* j1 = tb_node(&b); tb_node_translation(j1, 0, 0, 3);
	cgltf_node* j2 = tb_node(&b); tb_node_translation(j2, -2, 1, 1);
	cgltf_node* n3 = tb_node(&b);
	cgltf_node* n4 = tb_node(&b);
	n3->mesh = mesh;
	n4->mesh = mesh;
	float node_w[1] = {0.5f};
	tb_node_weights(&b, n3, node_w, 1);

	float ibm0[32];
	memset(ibm0, 0, sizeof(ibm0));
	for (int j = 0; j < 2; ++j)
		for (int i = 0; i < 4; ++i)
			ibm0[j * 16 + i * 5] = 1.0f;
	ibm0[12] = -2; ibm0[13] = -2;            /* joint0: T(-2,-2,0) */
	ibm0[16 + 12] = 1; ibm0[16 + 14] = -3;   /* joint1: T(1,0,-3) */
	cgltf_skin* skin0 = tb_skin(&b, 2, tb_ibm(&b, ibm0, 2));
	skin0->joints[0] = j0;
	skin0->joints[1] = j1;
	n3->skin = skin0;

	float ibm1[32];
	memset(ibm1, 0, sizeof(ibm1));
	for (int j = 0; j < 2; ++j)
		for (int i = 0; i < 4; ++i)
			ibm1[j * 16 + i * 5] = 1.0f;
	ibm1[13] = 1; ibm1[14] = -3;             /* joint1: T(0,1,-3) */
	ibm1[16 + 12] = 2; ibm1[16 + 13] = -1; ibm1[16 + 14] = -1; /* joint2: T(2,-1,-1) */
	cgltf_skin* skin1 = tb_skin(&b, 2, tb_ibm(&b, ibm1, 2));
	skin1->joints[0] = j1;
	skin1->joints[1] = j2;
	n4->skin = skin1;

	tb_root(&b, j0);
	tb_root(&b, j1);
	tb_root(&b, j2);
	tb_root(&b, n3);
	tb_root(&b, n4);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);
	TARGET_CHECK(bvh != NULL);

	/* pose 1: file pose */
	float world[40 * 16];
	tb_fill_world(&b, world);
	TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, NULL, NULL, NULL) == cgltf_skin_bvh_result_success);
	check_pose(&b, bvh, world, NULL);

	/* pose 2: joints moved, morph override on node 3 only */
	world[0 * 16 + 12] = 3.0f; world[0 * 16 + 13] = -1.0f;
	world[1 * 16 + 14] = 5.0f;
	world[2 * 16 + 12] = -4.0f; world[2 * 16 + 13] = 2.0f;
	float ovr3[1] = {-1.5f};
	const float* ovr[40];
	cgltf_size ovr_count[40];
	memset(ovr, 0, sizeof(ovr));
	memset(ovr_count, 0, sizeof(ovr_count));
	ovr[3] = ovr3;
	ovr_count[3] = 1;
	TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, ovr, ovr_count, NULL) == cgltf_skin_bvh_result_success);
	check_pose(&b, bvh, world, ovr);

	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
}

/* JOINTS_0/1 + WEIGHTS_0/1: eight influences, duplicate joints, zero weights
 * with out-of-range joint indices, unnormalized weights, identity IBMs,
 * non-indexed primitive */
static void case_eight_influences(void)
{
	tb b;
	tb_init(&b);

	float pos[3][3] = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
	cgltf_accessor* pos_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, pos, 12);
	uint16_t j0[3][4] = {{0, 1, 1, 2}, {0, 0, 0, 0}, {3, 0, 1, 2}};
	uint16_t j1[3][4] = {{3, 0, 2, 200}, {0, 0, 0, 0}, {200, 0, 0, 0}};
	float w0[3][4] = {{0.2f, 0.3f, 0.1f, 0.1f}, {2.0f, 0, 0, 0}, {0.5f, 0.1f, 0.2f, 0.1f}};
	float w1[3][4] = {{0.1f, 0.1f, 0.1f, 0.0f}, {0, 0, 0, 0}, {0.0f, 0.1f, 0, 0}};
	cgltf_accessor* joints0 = tb_pack(&b, cgltf_component_type_r_16u, 0, cgltf_type_vec4, 3, j0, 8);
	cgltf_accessor* joints1 = tb_pack(&b, cgltf_component_type_r_16u, 0, cgltf_type_vec4, 3, j1, 8);
	cgltf_accessor* weights0 = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec4, 3, w0, 16);
	cgltf_accessor* weights1 = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec4, 3, w1, 16);

	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos_acc);
	tb_attr(&b, prim, cgltf_attribute_type_joints, 0, joints0);
	tb_attr(&b, prim, cgltf_attribute_type_weights, 0, weights0);
	tb_attr(&b, prim, cgltf_attribute_type_joints, 1, joints1);
	tb_attr(&b, prim, cgltf_attribute_type_weights, 1, weights1);

	cgltf_node* j[4];
	float t[4][3] = {{10, 0, 0}, {0, 10, 0}, {0, 0, 10}, {-5, -5, 0}};
	for (int i = 0; i < 4; ++i)
	{
		j[i] = tb_node(&b);
		tb_node_translation(j[i], t[i][0], t[i][1], t[i][2]);
	}
	cgltf_node* mn = tb_node(&b);
	mn->mesh = mesh;
	cgltf_skin* skin = tb_skin(&b, 4, NULL); /* no inverseBindMatrices: identity */
	for (int i = 0; i < 4; ++i)
		skin->joints[i] = j[i];
	mn->skin = skin;
	for (int i = 0; i < 4; ++i)
		tb_root(&b, j[i]);
	tb_root(&b, mn);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);

	float world[40 * 16];
	tb_fill_world(&b, world);
	check_pose(&b, bvh, world, NULL);

	/* move two joints and re-check */
	world[1 * 16 + 12] = -3.0f;
	world[2 * 16 + 13] = 7.0f;
	TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, NULL, NULL, NULL) == cgltf_skin_bvh_result_success);
	check_pose(&b, bvh, world, NULL);

	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
}

/* negative morph weights (node, mesh and explicit override), sparse morph
 * accessor with zero base, joint translation, static instance with shear */
static void case_negative_morph_sparse(void)
{
	tb b;
	tb_init(&b);

	/* skinned mesh: one triangle, u32 indices */
	float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	cgltf_accessor* pos_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, pos, 12);
	cgltf_uint idx[3] = {0, 1, 2};
	cgltf_accessor* indices = tb_pack(&b, cgltf_component_type_r_32u, 0, cgltf_type_scalar, 3, idx, 4);
	uint8_t j0[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
	uint16_t w0[3][4] = {{65535, 0, 0, 0}, {65535, 0, 0, 0}, {65535, 0, 0, 0}};
	cgltf_accessor* joints0 = tb_pack(&b, cgltf_component_type_r_8u, 0, cgltf_type_vec4, 3, j0, 4);
	cgltf_accessor* weights0 = tb_pack(&b, cgltf_component_type_r_16u, 1, cgltf_type_vec4, 3, w0, 8);
	/* sparse morph target: zero base, vertex 1 overridden by (0, 0, 2) */
	cgltf_accessor* morph = tb_acc(&b, NULL, 0, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, 12);
	uint8_t sidx[1] = {1};
	float sval[3] = {0, 0, 2};
	morph->is_sparse = 1;
	morph->sparse.count = 1;
	morph->sparse.indices_buffer_view = tb_view(&b, sidx, 1, 0);
	morph->sparse.indices_component_type = cgltf_component_type_r_8u;
	morph->sparse.indices_byte_offset = 0;
	morph->sparse.values_buffer_view = tb_view(&b, sval, sizeof(sval), 0);
	morph->sparse.values_byte_offset = 0;

	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos_acc);
	tb_attr(&b, prim, cgltf_attribute_type_joints, 0, joints0);
	tb_attr(&b, prim, cgltf_attribute_type_weights, 0, weights0);
	prim->indices = indices;
	cgltf_morph_target* target = tb_target(&b, prim);
	tb_target_attr(&b, target, cgltf_attribute_type_position, morph);

	cgltf_node* joint = tb_node(&b);
	tb_node_translation(joint, 0, 0, 0);
	cgltf_node* mn = tb_node(&b);
	mn->mesh = mesh;
	cgltf_skin* skin = tb_skin(&b, 1, NULL);
	skin->joints[0] = joint;
	mn->skin = skin;
	float node_w[1] = {-1.5f};
	tb_node_weights(&b, mn, node_w, 1);

	/* static instance of the same mesh with a shear+scale matrix */
	cgltf_node* sn = tb_node(&b);
	sn->mesh = mesh;
	float shear[16] = {2, 0, 0, 0,  0.5f, 1, 0, 0,  0, 0, 1, 0,  5, -3, 2, 1};
	tb_node_matrix(sn, shear);

	tb_root(&b, joint);
	tb_root(&b, mn);
	tb_root(&b, sn);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);

	float world[40 * 16];
	tb_fill_world(&b, world);
	check_pose(&b, bvh, world, NULL);

	/* joint translated far, explicit morph overrides (explicit zero for the
	 * skinned node, negative for the static one) */
	world[0 * 16 + 12] = 30.0f;
	world[0 * 16 + 13] = -7.0f;
	float ovr_mn[1] = {0.0f};
	float ovr_sn[1] = {-2.0f};
	const float* ovr[40];
	cgltf_size ovr_count[40];
	memset(ovr, 0, sizeof(ovr));
	memset(ovr_count, 0, sizeof(ovr_count));
	ovr[1] = ovr_mn;
	ovr_count[1] = 1;
	ovr[2] = ovr_sn;
	ovr_count[2] = 1;
	TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, ovr, ovr_count, NULL) == cgltf_skin_bvh_result_success);
	check_pose(&b, bvh, world, ovr);

	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
}

/* minimal one-triangle scene used by the rejection tests */
typedef enum
{
	REJ_OK, REJ_POINTS, REJ_DRACO, REJ_MESHOPT, REJ_INSTANCING,
	REJ_UNPAIRED, REJ_ZERO_SUM, REJ_BAD_JOINT, REJ_NEGATIVE_W
} rej_kind;

static cgltf_skin_bvh_result rej_create(rej_kind kind)
{
	tb b;
	tb_init(&b);
	float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	cgltf_accessor* pos_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, pos, 12);
	uint8_t j0[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
	float w0[3][4] = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
	if (kind == REJ_ZERO_SUM)
		memset(w0, 0, sizeof(w0));
	if (kind == REJ_BAD_JOINT)
		j0[0][0] = 5;
	if (kind == REJ_NEGATIVE_W)
		w0[1][0] = -0.5f;
	cgltf_accessor* joints0 = tb_pack(&b, cgltf_component_type_r_8u, 0, cgltf_type_vec4, 3, j0, 4);
	cgltf_accessor* weights0 = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec4, 3, w0, 16);
	if (kind == REJ_MESHOPT)
		pos_acc->buffer_view->has_meshopt_compression = 1;

	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	if (kind == REJ_POINTS)
		prim->type = cgltf_primitive_type_points;
	if (kind == REJ_DRACO)
		prim->has_draco_mesh_compression = 1;
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos_acc);
	tb_attr(&b, prim, cgltf_attribute_type_joints, 0, joints0);
	tb_attr(&b, prim, cgltf_attribute_type_weights, 0, weights0);
	if (kind == REJ_UNPAIRED)
	{
		uint8_t j1[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
		cgltf_accessor* joints1 = tb_pack(&b, cgltf_component_type_r_8u, 0, cgltf_type_vec4, 3, j1, 4);
		tb_attr(&b, prim, cgltf_attribute_type_joints, 1, joints1); /* no WEIGHTS_1 */
	}

	cgltf_node* joint = tb_node(&b);
	cgltf_node* mn = tb_node(&b);
	mn->mesh = mesh;
	cgltf_skin* skin = tb_skin(&b, 1, NULL);
	skin->joints[0] = joint;
	mn->skin = skin;
	if (kind == REJ_INSTANCING)
		mn->has_mesh_gpu_instancing = 1;
	tb_root(&b, joint);
	tb_root(&b, mn);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	cgltf_skin_bvh_result res = cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]);
	if (res == cgltf_skin_bvh_result_success)
		cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
	return res;
}

static void case_create_rejections(void)
{
	TARGET_CHECK(rej_create(REJ_OK) == cgltf_skin_bvh_result_success);
	TARGET_CHECK(rej_create(REJ_POINTS) == cgltf_skin_bvh_result_unsupported);
	TARGET_CHECK(rej_create(REJ_DRACO) == cgltf_skin_bvh_result_unsupported);
	TARGET_CHECK(rej_create(REJ_MESHOPT) == cgltf_skin_bvh_result_unsupported);
	TARGET_CHECK(rej_create(REJ_INSTANCING) == cgltf_skin_bvh_result_unsupported);
	TARGET_CHECK(rej_create(REJ_UNPAIRED) == cgltf_skin_bvh_result_invalid_data);
	TARGET_CHECK(rej_create(REJ_ZERO_SUM) == cgltf_skin_bvh_result_invalid_data);
	TARGET_CHECK(rej_create(REJ_BAD_JOINT) == cgltf_skin_bvh_result_invalid_data);
	TARGET_CHECK(rej_create(REJ_NEGATIVE_W) == cgltf_skin_bvh_result_invalid_data);
}

/* ------------------------------------------------------------------ */
/* bounds group                                                        */

/* builds a single skinned triangle bound to one joint */
static void make_one_joint_scene(tb* b)
{
	tb_init(b);
	float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	cgltf_accessor* pos_acc = tb_pack(b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, pos, 12);
	uint8_t j0[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
	float w0[3][4] = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
	cgltf_accessor* joints0 = tb_pack(b, cgltf_component_type_r_8u, 0, cgltf_type_vec4, 3, j0, 4);
	cgltf_accessor* weights0 = tb_pack(b, cgltf_component_type_r_32f, 0, cgltf_type_vec4, 3, w0, 16);
	cgltf_mesh* mesh = tb_mesh(b);
	cgltf_primitive* prim = tb_prim(b, mesh);
	tb_attr(b, prim, cgltf_attribute_type_position, 0, pos_acc);
	tb_attr(b, prim, cgltf_attribute_type_joints, 0, joints0);
	tb_attr(b, prim, cgltf_attribute_type_weights, 0, weights0);
	cgltf_node* joint = tb_node(b);
	cgltf_node* mn = tb_node(b);
	mn->mesh = mesh;
	cgltf_skin* skin = tb_skin(b, 1, NULL);
	skin->joints[0] = joint;
	mn->skin = skin;
	tb_root(b, joint);
	tb_root(b, mn);
	tb_finish(b);
}

/* two consecutive pose jumps that both leave the previous bounds behind;
 * also verifies submit does not deform vertices and queries cache them */
static void case_refit_consecutive_moves(void)
{
	tb b;
	make_one_joint_scene(&b);
	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);

	float world[2 * 16];
	cgltf_skin_bvh_stats st;
	cgltf_skin_bvh_hit hit;
	ref_pose r;
	r.data = &b.data;
	r.morph = NULL;

	for (int move = 0; move < 3; ++move)
	{
		tb_fill_world(&b, world);
		world[0 * 16 + 12] = 100.0f * (float)move; /* joint jumps +100 in x */
		TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, NULL, NULL, &st) == cgltf_skin_bvh_result_success);
		TARGET_CHECK(st.deformed_vertices == 0);   /* no per-vertex work on submit */
		TARGET_CHECK(st.bound_evaluations > 0);
		r.world = world;

		float o[3] = {100.0f * (float)move + 0.25f, 0.25f, -5.0f};
		float d[3] = {0, 0, 1};
		TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, d, 0.0f, 100.0f, &hit, &st) == cgltf_skin_bvh_result_success);
		TARGET_CHECK(st.deformed_vertices == 3);   /* first query deforms the triangle */
		TARGET_CHECK(hit.node_index == 1 && hit.triangle_index == 0);
		TARGET_CHECK(closef(hit.t, 5.0f));
		TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, d, 0.0f, 100.0f, &hit, &st) == cgltf_skin_bvh_result_success);
		TARGET_CHECK(st.deformed_vertices == 0);   /* cached by the second query */

		/* the old location must be a miss now: bounds moved with the pose */
		float o_old[3] = {100.0f * (float)move - 100.0f + 0.25f, 0.25f, -5.0f};
		TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o_old, d, 0.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_no_hit);

		check_pose(&b, bvh, world, NULL);
	}
	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
}

/* failed pose submissions keep the previous pose queryable */
static void case_failed_pose_retained(void)
{
	tb b;
	tb_init(&b);
	float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	cgltf_accessor* pos_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, pos, 12);
	float delta[3][3] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
	cgltf_accessor* morph = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, delta, 12);
	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos_acc);
	cgltf_morph_target* target = tb_target(&b, prim);
	tb_target_attr(&b, target, cgltf_attribute_type_position, morph);
	cgltf_node* mn = tb_node(&b);
	mn->mesh = mesh;
	tb_root(&b, mn);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);

	float world[16];
	tb_fill_world(&b, world);
	cgltf_skin_bvh_hit hit;
	const float o[3] = {0.25f, 0.25f, -5.0f};
	const float d[3] = {0, 0, 1};
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, d, 0.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_success);
	TARGET_CHECK(closef(hit.t, 5.0f));

	/* non-finite matrix */
	{
		float bad[16];
		memcpy(bad, world, sizeof(bad));
		bad[0] = NAN;
		TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, bad, NULL, NULL, NULL) == cgltf_skin_bvh_result_invalid_data);
	}
	/* non-affine matrix */
	{
		float bad[16];
		memcpy(bad, world, sizeof(bad));
		bad[15] = 2.0f;
		TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, bad, NULL, NULL, NULL) == cgltf_skin_bvh_result_invalid_data);
	}
	/* wrong morph weight count */
	{
		float one[1] = {1.0f};
		const float* ovr[1] = {one};
		cgltf_size cnt[1] = {2}; /* mesh has 1 target */
		TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, ovr, cnt, NULL) == cgltf_skin_bvh_result_invalid_data);
	}
	/* non-finite morph weight */
	{
		float one[1] = {INFINITY};
		const float* ovr[1] = {one};
		cgltf_size cnt[1] = {1};
		TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, ovr, cnt, NULL) == cgltf_skin_bvh_result_invalid_data);
	}
	/* overflowing bounds: finite but huge transform */
	{
		float bad[16];
		memcpy(bad, world, sizeof(bad));
		bad[0] = bad[5] = bad[10] = 2.0e38f;
		bad[12] = 2.0e38f;
		TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, bad, NULL, NULL, NULL) == cgltf_skin_bvh_result_invalid_data);
	}
	/* argument errors */
	TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, NULL, NULL, NULL, NULL) == cgltf_skin_bvh_result_invalid_argument);
	{
		float one[1] = {0.0f};
		const float* ovr[1] = {one};
		TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, ovr, NULL, NULL) == cgltf_skin_bvh_result_invalid_argument);
	}

	/* the previous pose is still queryable */
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, d, 0.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_success);
	TARGET_CHECK(closef(hit.t, 5.0f));
	TARGET_CHECK(hit.node_index == 0 && hit.triangle_index == 0);
	check_pose(&b, bvh, world, NULL);

	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
}

/* random poses over a skinned+morphed mesh, verified against the reference:
 * any under-sized or stale bound would drop or misplace hits */
static void case_random_poses_bounds(void)
{
	tb b;
	tb_init(&b);

	enum { NV = 12, NT = 6 };
	float pos[NV][3];
	uint8_t j0[NV][4];
	uint16_t w0[NV][4];
	for (int i = 0; i < NV; ++i)
	{
		pos[i][0] = (float)(i % 3) * 2.0f;
		pos[i][1] = (float)(i / 3) * 2.0f;
		pos[i][2] = 0.25f * (float)(i % 2);
		j0[i][0] = (uint8_t)(i % 3);
		j0[i][1] = (uint8_t)((i + 1) % 3);
		j0[i][2] = 0;
		j0[i][3] = 0;
		w0[i][0] = (uint16_t)(20000 + 10000 * (i % 3));
		w0[i][1] = 35535;
		w0[i][2] = 0;
		w0[i][3] = 0;
	}
	cgltf_uint idx[NT * 3] = {0, 1, 4, 0, 4, 3, 1, 2, 5, 3, 4, 7, 6, 7, 10, 9, 10, 11};
	cgltf_accessor* pos_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, NV, pos, 12);
	cgltf_accessor* indices = tb_pack(&b, cgltf_component_type_r_32u, 0, cgltf_type_scalar, NT * 3, idx, 4);
	cgltf_accessor* joints0 = tb_pack(&b, cgltf_component_type_r_8u, 0, cgltf_type_vec4, NV, j0, 4);
	cgltf_accessor* weights0 = tb_pack(&b, cgltf_component_type_r_16u, 1, cgltf_type_vec4, NV, w0, 8);
	float d0[NV][3], d1[NV][3];
	for (int i = 0; i < NV; ++i)
	{
		d0[i][0] = 0.1f * (float)i; d0[i][1] = -0.2f * (float)i; d0[i][2] = 0.05f * (float)i;
		d1[i][0] = -0.15f * (float)i; d1[i][1] = 0.1f * (float)i; d1[i][2] = -0.3f;
	}
	cgltf_accessor* m0 = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, NV, d0, 12);
	cgltf_accessor* m1 = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, NV, d1, 12);

	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos_acc);
	tb_attr(&b, prim, cgltf_attribute_type_joints, 0, joints0);
	tb_attr(&b, prim, cgltf_attribute_type_weights, 0, weights0);
	prim->indices = indices;
	cgltf_morph_target* t0 = tb_target(&b, prim);
	tb_target_attr(&b, t0, cgltf_attribute_type_position, m0);
	cgltf_morph_target* t1 = tb_target(&b, prim);
	tb_target_attr(&b, t1, cgltf_attribute_type_position, m1);

	cgltf_node* joints[3];
	for (int i = 0; i < 3; ++i)
		joints[i] = tb_node(&b);
	cgltf_node* mn = tb_node(&b);
	mn->mesh = mesh;
	float ibm[48];
	memset(ibm, 0, sizeof(ibm));
	for (int jj = 0; jj < 3; ++jj)
		for (int i = 0; i < 4; ++i)
			ibm[jj * 16 + i * 5] = 1.0f;
	ibm[12] = -1; ibm[16 + 13] = 2; ibm[32 + 14] = -3;
	cgltf_skin* skin = tb_skin(&b, 3, tb_ibm(&b, ibm, 3));
	for (int i = 0; i < 3; ++i)
		skin->joints[i] = joints[i];
	mn->skin = skin;
	for (int i = 0; i < 3; ++i)
		tb_root(&b, joints[i]);
	tb_root(&b, mn);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);

	rng_state = 0xdeadbeefcafeULL;
	for (int pose = 0; pose < 30; ++pose)
	{
		/* random joint transforms */
		for (int i = 0; i < 3; ++i)
		{
			tb_node_translation(joints[i], 8.0f * (frand() - 0.5f), 8.0f * (frand() - 0.5f), 8.0f * (frand() - 0.5f));
			float ax = frand() - 0.5f, ay = frand() - 0.5f, az = frand() - 0.5f;
			float al = sqrtf(ax * ax + ay * ay + az * az) + 1.0e-3f;
			float ang = 6.0f * frand();
			float s = sinf(ang * 0.5f) / al;
			tb_node_rotation(joints[i], ax * s, ay * s, az * s, cosf(ang * 0.5f));
		}
		float world[40 * 16];
		tb_fill_world(&b, world);
		float ovr_w[2] = {2.0f * (frand() - 0.5f), -2.0f * frand()};
		const float* ovr[40];
		cgltf_size ovr_count[40];
		memset(ovr, 0, sizeof(ovr));
		memset(ovr_count, 0, sizeof(ovr_count));
		ovr[3] = ovr_w;
		ovr_count[3] = 2;
		TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, ovr, ovr_count, NULL) == cgltf_skin_bvh_result_success);

		ref_pose r;
		r.data = &b.data;
		r.world = world;
		r.morph = ovr;
		for (int ray = 0; ray < 40; ++ray)
		{
			float o[3] = {20.0f * (frand() - 0.5f), 20.0f * (frand() - 0.5f), 20.0f * (frand() - 0.5f)};
			float d[3] = {2.0f * (frand() - 0.5f), 2.0f * (frand() - 0.5f), 2.0f * (frand() - 0.5f)};
			if (d[0] == 0 && d[1] == 0 && d[2] == 0)
				d[2] = 1.0f;
			check_ray(bvh, &r, &b.scenes[0], o, d, 0.0f, 100.0f);
		}
	}
	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
}

/* ------------------------------------------------------------------ */
/* query group                                                         */

/* non-unit ray directions, signed-zero components, reflection pose */
static void case_nonunit_ray_reflection(void)
{
	tb b;
	tb_init(&b);
	float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	cgltf_accessor* pos_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, pos, 12);
	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos_acc);
	cgltf_node* mn = tb_node(&b);
	mn->mesh = mesh;
	tb_root(&b, mn);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);

	/* reflection + translation pose: x' = -x, then shifted */
	float world[16] = {-1, 0, 0, 0,  0, 2, 0, 0,  0, 0, 1, 0,  3, 1, 0, 1};
	TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, NULL, NULL, NULL) == cgltf_skin_bvh_result_success);

	ref_pose r;
	r.data = &b.data;
	r.world = world;
	r.morph = NULL;

	/* reflected+scaled triangle: (3,1,0), (2,1,0), (3,3,0); winding flipped */
	cgltf_skin_bvh_hit hit;
	float o[3] = {2.75f, 1.5f, -10.0f};
	float d[3] = {0.0f, 0.0f, 2.5f}; /* non-unit direction: t scales by 1/2.5 */
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, d, 0.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_success);
	TARGET_CHECK(closef(hit.t, 4.0f)); /* distance 10 at |D| = 2.5 */
	TARGET_CHECK(closef(hit.normal[2], -1.0f)); /* geometric normal follows the flipped winding */
	check_ray(bvh, &r, &b.scenes[0], o, d, 0.0f, 100.0f);

	/* signed zero direction components, ray parallel to box axes */
	float o2[3] = {2.75f, 1.5f, -10.0f};
	float d2[3] = {-0.0f, 0.0f, 1.0f};
	check_ray(bvh, &r, &b.scenes[0], o2, d2, 0.0f, 100.0f);

	/* origin exactly on the bounding-box side plane */
	float o3[3] = {3.0f, 1.5f, -10.0f};
	check_ray(bvh, &r, &b.scenes[0], o3, d2, 0.0f, 100.0f);

	/* reversed ray (negative direction) from the other side */
	float o4[3] = {2.75f, 1.5f, 10.0f};
	float d4[3] = {0.0f, -0.0f, -3.0f};
	check_ray(bvh, &r, &b.scenes[0], o4, d4, 0.0f, 100.0f);

	check_pose(&b, bvh, world, NULL);
	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
}

/* 4096 spatially separated triangles under one joint; rays through each
 * triangle must cost far less than a full scan, and pose submit must not
 * deform any vertices */
static void case_bvh_performance(void)
{
	tb b;
	tb_init(&b);

	enum { GRID = 64, NTRI = GRID * GRID, NV = NTRI * 3 };
	float (*pos)[3] = (float(*)[3])malloc(NV * 3 * sizeof(float));
	uint8_t (*j0)[4] = (uint8_t(*)[4])malloc(NV * 4);
	float (*w0)[4] = (float(*)[4])malloc(NV * 4 * sizeof(float));
	cgltf_uint* idx = (cgltf_uint*)malloc(NTRI * 3 * sizeof(cgltf_uint));
	TARGET_CHECK(pos && j0 && w0 && idx);
	for (int t = 0; t < NTRI; ++t)
	{
		float x = (float)(t % GRID) * 4.0f;
		float y = (float)(t / GRID) * 4.0f;
		for (int k = 0; k < 3; ++k)
		{
			int v = t * 3 + k;
			pos[v][0] = x + (k == 1 ? 1.0f : 0.0f);
			pos[v][1] = y + (k == 2 ? 1.0f : 0.0f);
			pos[v][2] = 0.0f;
			j0[v][0] = 0; j0[v][1] = 0; j0[v][2] = 0; j0[v][3] = 0;
			w0[v][0] = 1.0f; w0[v][1] = 0; w0[v][2] = 0; w0[v][3] = 0;
			idx[t * 3 + k] = (cgltf_uint)v;
		}
	}
	cgltf_accessor* pos_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, NV, pos, 12);
	cgltf_accessor* joints0 = tb_pack(&b, cgltf_component_type_r_8u, 0, cgltf_type_vec4, NV, j0, 4);
	cgltf_accessor* weights0 = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec4, NV, w0, 16);
	cgltf_accessor* indices = tb_pack(&b, cgltf_component_type_r_32u, 0, cgltf_type_scalar, NTRI * 3, idx, 4);
	free(pos);
	free(j0);
	free(w0);
	free(idx);

	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos_acc);
	tb_attr(&b, prim, cgltf_attribute_type_joints, 0, joints0);
	tb_attr(&b, prim, cgltf_attribute_type_weights, 0, weights0);
	prim->indices = indices;

	cgltf_node* joint = tb_node(&b);
	tb_node_translation(joint, 10, 5, 3);
	float hz = 0.2617993877991494f; /* 15 degrees */
	tb_node_rotation(joint, 0, 0, sinf(hz), cosf(hz));
	cgltf_node* mn = tb_node(&b);
	mn->mesh = mesh;
	cgltf_skin* skin = tb_skin(&b, 1, NULL);
	skin->joints[0] = joint;
	mn->skin = skin;
	tb_root(&b, joint);
	tb_root(&b, mn);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);

	float world[2 * 16];
	tb_fill_world(&b, world);
	cgltf_skin_bvh_stats st;
	TARGET_CHECK(cgltf_skin_bvh_submit_pose(bvh, world, NULL, NULL, &st) == cgltf_skin_bvh_result_success);
	TARGET_CHECK(st.deformed_vertices == 0); /* submit never deforms vertices */
	TARGET_CHECK(st.bound_evaluations > 0);

	/* rays through every triangle interior in the posed (non-identity) state */
	float dlen = sqrtf(0.3f * 0.3f + 0.2f * 0.2f + 1.0f);
	float d[3] = {0.3f / dlen, 0.2f / dlen, 1.0f / dlen};
	unsigned long long total_tests = 0;
	cgltf_skin_bvh_hit hit;
	for (int t = 0; t < NTRI; ++t)
	{
		float base[3] = {
			(float)(t % GRID) * 4.0f + 1.0f / 3.0f,
			(float)(t / GRID) * 4.0f + 1.0f / 3.0f,
			0.0f};
		float c[3];
		ref_mat_apply(world, base[0], base[1], base[2], c); /* joint world matrix */
		float o[3] = {c[0] - 8.0f * d[0], c[1] - 8.0f * d[1], c[2] - 8.0f * d[2]};
		TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, d, 0.0f, 100.0f, &hit, &st) == cgltf_skin_bvh_result_success);
		TARGET_CHECK(hit.node_index == 1);
		TARGET_CHECK(hit.triangle_index == (cgltf_size)t);
		TARGET_CHECK(closef(hit.t, 8.0f));
		TARGET_CHECK(closef(hit.position[0], c[0]));
		TARGET_CHECK(closef(hit.position[1], c[1]));
		TARGET_CHECK(closef(hit.position[2], c[2]));
		total_tests += st.triangle_tests;
	}
	/* full scan would be NTRI tests per ray; require less than 1/16 of that */
	TARGET_CHECK(total_tests < (unsigned long long)NTRI * NTRI / 16);

	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);
}

/* ray argument validation, zero-area triangles, exact-tie identity,
 * empty scene */
static void case_ray_validation_degenerate(void)
{
	tb b;
	tb_init(&b);
	float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	cgltf_accessor* pos_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, pos, 12);
	float degen[3][3] = {{5, 5, 5}, {5, 5, 5}, {5, 5, 5}};
	cgltf_accessor* degen_acc = tb_pack(&b, cgltf_component_type_r_32f, 0, cgltf_type_vec3, 3, degen, 12);

	cgltf_mesh* mesh = tb_mesh(&b);
	cgltf_primitive* prim = tb_prim(&b, mesh);
	tb_attr(&b, prim, cgltf_attribute_type_position, 0, pos_acc);
	cgltf_mesh* mesh2 = tb_mesh(&b);
	cgltf_primitive* prim2 = tb_prim(&b, mesh2);
	tb_attr(&b, prim2, cgltf_attribute_type_position, 0, degen_acc);

	cgltf_node* n0 = tb_node(&b); /* regular triangle, identity */
	n0->mesh = mesh;
	cgltf_node* n1 = tb_node(&b); /* zero-area triangle */
	n1->mesh = mesh2;
	cgltf_node* n2 = tb_node(&b); /* exact duplicate of n0 for the tie test */
	n2->mesh = mesh;
	/* roots listed out of node order on purpose */
	tb_root(&b, n2);
	tb_root(&b, n1);
	tb_root(&b, n0);
	tb_finish(&b);

	cgltf_skin_bvh* bvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&bvh, &b.data, &b.scenes[0]) == cgltf_skin_bvh_result_success);

	cgltf_skin_bvh_hit hit;
	/* zero-area triangle never hits */
	float od[3] = {5, 5, -5};
	float dd[3] = {0, 0, 1};
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, od, dd, 0.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_no_hit);

	/* exact tie between node 0 and node 2: smallest (node, primitive, triangle) wins */
	float o[3] = {0.25f, 0.25f, -5.0f};
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dd, 0.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_success);
	TARGET_CHECK(hit.node_index == 0);
	TARGET_CHECK(hit.primitive_index == 0 && hit.triangle_index == 0);
	TARGET_CHECK(closef(hit.t, 5.0f));
	TARGET_CHECK(closef(hit.barycentric[0], 0.5f));
	TARGET_CHECK(closef(hit.barycentric[1], 0.25f));
	TARGET_CHECK(closef(hit.barycentric[2], 0.25f));
	TARGET_CHECK(closef(hit.normal[2], 1.0f));

	/* interval clipping: the triangle is outside [tmin, tmax] */
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dd, 0.0f, 4.0f, &hit, NULL) == cgltf_skin_bvh_result_no_hit);
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dd, 6.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_no_hit);
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dd, 5.0f, 5.0f, &hit, NULL) == cgltf_skin_bvh_result_success);

	/* invalid rays are errors, distinct from a legal miss */
	float onan[3] = {NAN, 0, 0};
	float dinf[3] = {0, 0, INFINITY};
	float dz[3] = {0, 0, 0};
	float dzn[3] = {-0.0f, -0.0f, -0.0f};
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, onan, dd, 0.0f, 1.0f, &hit, NULL) == cgltf_skin_bvh_result_invalid_argument);
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dinf, 0.0f, 1.0f, &hit, NULL) == cgltf_skin_bvh_result_invalid_argument);
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dz, 0.0f, 1.0f, &hit, NULL) == cgltf_skin_bvh_result_invalid_argument);
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dzn, 0.0f, 1.0f, &hit, NULL) == cgltf_skin_bvh_result_invalid_argument);
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dd, 2.0f, 1.0f, &hit, NULL) == cgltf_skin_bvh_result_invalid_argument);
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dd, 0.0f, INFINITY, &hit, NULL) == cgltf_skin_bvh_result_invalid_argument);
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, o, dd, 0.0f, 1.0f, NULL, NULL) == cgltf_skin_bvh_result_invalid_argument);
	/* a legal miss is not an error */
	float om[3] = {50, 50, -5};
	TARGET_CHECK(cgltf_skin_bvh_raycast(bvh, om, dd, 0.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_no_hit);

	cgltf_skin_bvh_destroy(bvh);
	tb_free(&b);

	/* empty scene is valid */
	tb e;
	tb_init(&e);
	tb_finish(&e);
	cgltf_skin_bvh* ebvh = NULL;
	TARGET_CHECK(cgltf_skin_bvh_create(&ebvh, &e.data, &e.scenes[0]) == cgltf_skin_bvh_result_success);
	TARGET_CHECK(cgltf_skin_bvh_raycast(ebvh, o, dd, 0.0f, 100.0f, &hit, NULL) == cgltf_skin_bvh_result_no_hit);
	float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	TARGET_CHECK(cgltf_skin_bvh_submit_pose(ebvh, world, NULL, NULL, NULL) == cgltf_skin_bvh_result_success);
	cgltf_skin_bvh_destroy(ebvh);
	tb_free(&e);
}

/* ------------------------------------------------------------------ */

const TargetCase target_cases[] = {
	{"deform", "shared_mesh_two_skins", case_shared_mesh_two_skins},
	{"deform", "eight_influences_zero_weights", case_eight_influences},
	{"deform", "negative_morph_sparse", case_negative_morph_sparse},
	{"deform", "create_rejections", case_create_rejections},
	{"bounds", "refit_consecutive_moves", case_refit_consecutive_moves},
	{"bounds", "failed_pose_retained", case_failed_pose_retained},
	{"bounds", "random_poses_bounds", case_random_poses_bounds},
	{"query", "nonunit_ray_reflection", case_nonunit_ray_reflection},
	{"query", "bvh_performance", case_bvh_performance},
	{"query", "ray_validation_degenerate", case_ray_validation_degenerate},
};
const size_t target_count = sizeof(target_cases) / sizeof(target_cases[0]);
