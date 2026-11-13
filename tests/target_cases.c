//
//  target_cases.c
//  c-ray
//
//  Target tests for the cr_cn_triplanar triplanar color projection node.
//

#include <c-ray/c-ray.h>

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include <nodes/bsdfnode.h>
#include <nodes/colornode.h>
#include <datatypes/hitrecord.h>
#include <datatypes/scene.h>
#include <renderer/samplers/sampler.h>
#include <protocol/protocol.h>
#include <node_parse.h>
#include <texture.h>

#include "../support/test_api.h"

// Asymmetric procedural color: every (u,v) maps to a distinct, hand-checkable
// linear RGBA value. Built out of UV vector nodes, vec_to_value and rgb nodes.
static struct cr_color_node *procedural_desc(void) {
	// u and v need their own UV descriptors, since each subtree frees its own.
	struct cr_vector_node *uv_u = calloc(1, sizeof(*uv_u));
	uv_u->type = cr_vec_uv;
	struct cr_vector_node *uv_v = calloc(1, sizeof(*uv_v));
	uv_v->type = cr_vec_uv;

	struct cr_value_node *u = calloc(1, sizeof(*u));
	u->type = cr_vn_vec_to_value;
	u->arg.vec_to_value.vec = uv_u;
	u->arg.vec_to_value.comp = U;

	struct cr_value_node *v = calloc(1, sizeof(*v));
	v->type = cr_vn_vec_to_value;
	v->arg.vec_to_value.vec = uv_v;
	v->arg.vec_to_value.comp = V;

	struct cr_value_node *k = calloc(1, sizeof(*k));
	k->type = cr_vn_constant;
	k->arg.constant = 0.2;

	struct cr_color_node *out = calloc(1, sizeof(*out));
	out->type = cr_cn_rgb;
	out->arg.rgb.red = u;
	out->arg.rgb.green = v;
	out->arg.rgb.blue = k;
	return out;
}

// Expected procedural color for a given wrapped UV.
static struct color expected_proc(float u, float v) {
	return (struct color){ u, v, 0.2f, 1.0f };
}

static void identity_matrix(float m[4][4]) {
	memset(m, 0, sizeof(float) * 16);
	m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static struct hitRecord make_record(struct vector p, struct vector n, struct coord outer_uv) {
	struct hitRecord rec = {0};
	rec.hitPoint = p;
	rec.surfaceNormal = n;
	rec.uv = outer_uv;
	return rec;
}

static bool closef(float a, float b) {
	return fabsf(a - b) < 1e-5f;
}

static bool close_color(struct color a, struct color b) {
	return closef(a.red, b.red) && closef(a.green, b.green) &&
			closef(a.blue, b.blue) && closef(a.alpha, b.alpha);
}

static struct color eval_desc(struct cr_scene *s, struct cr_color_node *desc,
							  sampler *sam, struct hitRecord *rec) {
	const struct colorNode *node = build_color_node(s, desc);
	return node->eval(node, sam, rec);
}

void target_cases(struct suite *s) {
	cr_log_level_set(Silent);

	struct cr_renderer *r = cr_new_renderer();
	struct cr_scene *scene = cr_renderer_scene_get(r);
	struct sampler *sam = sampler_new();
	sampler_init(sam, Halton, 0, 2, 1);

	const struct coord outer_uv = { 0.7f, 0.9f };
	const struct vector P = { 0.2f, -0.3f, 0.4f };

	// -- Case 1: identity matrix, axis-aligned normals, negative UV wrapping --
	case_begin(s, "axis projections wrap negative UVs into [0,1)");
	{
		float m[4][4];
		identity_matrix(m);
		struct cr_color_node *proc = procedural_desc();
		struct cr_color_node tp = {
			.type = cr_cn_triplanar,
			.arg.triplanar = { .source = proc, .sharpness = 1.0f }
		};
		memcpy(tp.arg.triplanar.world_to_texture, m, sizeof(m));

		// +X normal: X projection UV = (-q.z, q.y) = (-0.4, -0.3) -> (0.6, 0.7)
		struct hitRecord rec = make_record(P, (struct vector){ 1, 0, 0 }, outer_uv);
		struct color got = eval_desc(scene, &tp, sam, &rec);
		require(s, close_color(got, expected_proc(0.6f, 0.7f)), "+X projection uses (-q.z,q.y) wrapped");

		// -Y normal: sy = -1, Y projection UV = (q.x, +q.z) = (0.2, 0.4)
		rec = make_record(P, (struct vector){ 0, -1, 0 }, outer_uv);
		got = eval_desc(scene, &tp, sam, &rec);
		require(s, close_color(got, expected_proc(0.2f, 0.4f)), "-Y projection flips q.z sign");

		// -Z normal: sz = -1, Z projection UV = (-q.x, q.y) = (-0.2, -0.3) -> (0.8, 0.7)
		rec = make_record(P, (struct vector){ 0, 0, -1 }, outer_uv);
		got = eval_desc(scene, &tp, sam, &rec);
		require(s, close_color(got, expected_proc(0.8f, 0.7f)), "-Z projection flips q.x sign and wraps");

		// A checkerboard source must stay on its UV-mapped path: the raw UVs
		// are negative here, and wrapping makes uv.x >= 0 instead of falling
		// back to the axis-aligned, no-UV checkerboard variant.
		struct cr_color_node red = { .type = cr_cn_constant, .arg.constant = { 1, 0, 0, 1 } };
		struct cr_color_node blue = { .type = cr_cn_constant, .arg.constant = { 0, 0, 1, 1 } };
		struct cr_value_node unit = { .type = cr_vn_constant, .arg.constant = 1.0 };
		struct cr_color_node checker = {
			.type = cr_cn_checkerboard,
			.arg.checkerboard = { .a = &red, .b = &blue, .scale = &unit }
		};
		struct cr_color_node tp_checker = {
			.type = cr_cn_triplanar,
			.arg.triplanar = { .source = &checker, .sharpness = 1.0f }
		};
		memcpy(tp_checker.arg.triplanar.world_to_texture, m, sizeof(m));
		rec = make_record(P, (struct vector){ 1, 0, 0 }, outer_uv);
		got = eval_desc(scene, &tp_checker, sam, &rec);
		require(s, close_color(got, (struct color){ 1, 0, 0, 1 }),
				"wrapped negative UV keeps checkerboard on its mapped path");
		cr_color_node_free(proc);
	}

	// -- Case 2: diagonal normal blending with sharpness --------------------
	case_begin(s, "diagonal normal blends three projections by sharpness weights");
	{
		float m[4][4];
		identity_matrix(m);
		struct cr_color_node *proc = procedural_desc();
		struct cr_color_node tp = {
			.type = cr_cn_triplanar,
			.arg.triplanar = { .source = proc, .sharpness = 2.0f }
		};
		memcpy(tp.arg.triplanar.world_to_texture, m, sizeof(m));

		const float inv = 1.0f / sqrtf(2.0f);
		struct vector n = { inv, inv, 0.0f };
		struct hitRecord rec = make_record(P, n, outer_uv);
		// |nx|==|ny| => equal 0.5/0.5 weights regardless of sharpness.
		// X color: (0.6,0.7,0.2); Y color: (q.x,-q.z)=(0.2,-0.4) -> (0.2,0.6,0.2)
		struct color got = eval_desc(scene, &tp, sam, &rec);
		require(s, closef(got.red, 0.4f), "diagonal red is the 50/50 blend");
		require(s, closef(got.green, 0.65f), "diagonal green is the 50/50 blend");
		require(s, closef(got.blue, 0.2f), "blue channel blends linearly too");
		cr_color_node_free(proc);
	}

	// -- Case 3: non-uniform shear and a mirrored matrix --------------------
	case_begin(s, "inverse-transpose normal survives shear, scale and mirroring");
	{
		// Non-uniform scale + shear, det = 1/3.
		float shear[4][4] = {
			{ 0.5f, 0.0f, 0.0f, 0.0f },
			{ 0.25f, 2.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f / 3.0f, 0.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
		};
		struct cr_color_node *proc = procedural_desc();
		struct cr_color_node tp = {
			.type = cr_cn_triplanar,
			.arg.triplanar = { .source = proc, .sharpness = 1.0f }
		};
		memcpy(tp.arg.triplanar.world_to_texture, shear, sizeof(shear));

		struct vector p = { 0.25f, -0.5f, 1.0f };
		struct hitRecord rec = make_record(p, (struct vector){ 0, 1, 0 }, outer_uv);
		// q = M*p = (0.125, -0.9375, 0.3333); inverse-transpose normal ~
		// (-0.4472, 0.8944, 0), weights wx = 1/3, wy = 2/3.
		struct color got = eval_desc(scene, &tp, sam, &rec);
		// X UV: sx=-1 -> (-sx*q.z,q.y) = (0.3333,-0.9375) -> (0.3333,0.0625)
		// Y UV: sy=+1 -> (q.x,-q.z) = (0.125,-0.3333) -> (0.125,0.6667)
		const float exp_red = (1.0f / 3.0f) * (1.0f / 3.0f) + (2.0f / 3.0f) * 0.125f;
		const float exp_green = (1.0f / 3.0f) * 0.0625f + (2.0f / 3.0f) * (2.0f / 3.0f);
		require(s, closef(got.red, exp_red), "shear weights come from inverse-transpose normal");
		require(s, closef(got.green, exp_green), "shear blend green matches hand calculation");

		// Mirror on X: det negative, the inverse-transpose normal flips to -X.
		// Absolute weights and the sx sign flip then keep representing the
		// same tangent plane.
		float mirror[4][4];
		identity_matrix(mirror);
		mirror[0][0] = -1.0f;
		struct cr_color_node tp_m = {
			.type = cr_cn_triplanar,
			.arg.triplanar = { .source = proc, .sharpness = 1.0f }
		};
		memcpy(tp_m.arg.triplanar.world_to_texture, mirror, sizeof(mirror));
		struct vector mp = { 0.3f, 0.0f, 0.2f };
		rec = make_record(mp, (struct vector){ 1, 0, 0 }, outer_uv);
		// q = (-0.3, 0, 0.2); tn = -X, sx=-1, UV u = -sx*q.z = 0.2.
		got = eval_desc(scene, &tp_m, sam, &rec);
		require(s, closef(got.red, 0.2f), "mirrored matrix keeps the same shaded tangent plane");
		cr_color_node_free(proc);
	}

	// -- Case 4: shared child, nesting, hit record preservation -------------
	case_begin(s, "shared and nested projections leave the hit record untouched");
	{
		float m_id[4][4], m_scale[4][4];
		identity_matrix(m_id);
		identity_matrix(m_scale);
		for (int i = 0; i < 3; ++i) m_scale[i][i] = 2.0f;

		struct cr_color_node *proc = procedural_desc();
		const struct colorNode *shared_source = build_color_node(scene, proc);

		// Two projection nodes differing only in sharpness must not merge.
		const struct colorNode *tp4 = newTriplanar(&((struct world *)scene)->storage, shared_source, m_id, 4.0f);
		const struct colorNode *tp8 = newTriplanar(&((struct world *)scene)->storage, shared_source, m_id, 8.0f);
		require(s, tp4 != tp8, "different sharpness produces distinct hash-consed nodes");

		// Neither may two nodes differing only in the affine matrix merge.
		const struct colorNode *tp_scale_node = newTriplanar(&((struct world *)scene)->storage, shared_source, m_scale, 4.0f);
		require(s, tp_scale_node != tp4, "different matrices produce distinct hash-consed nodes");

		const float inv5 = 1.0f / sqrtf(5.0f);
		struct vector n = { inv5, 2.0f * inv5, 0.0f };
		struct vector p = { 0.1f, 0.2f, 0.3f };
		struct hitRecord rec = make_record(p, n, outer_uv);
		struct color c4 = tp4->eval(tp4, sam, &rec);
		struct color c8 = tp8->eval(tp8, sam, &rec);
		require(s, !close_color(c4, c8), "sharpness 4 and 8 weight the axes differently");

		// Nested projection: outer scale(2) feeds wrapped UVs to an inner
		// identity projection, which must re-derive UVs from position/normal.
		struct cr_color_node *inner_desc = calloc(1, sizeof(*inner_desc));
		inner_desc->type = cr_cn_triplanar;
		inner_desc->arg.triplanar.source = proc; // same descriptor, rebuilt node
		inner_desc->arg.triplanar.sharpness = 4.0f;
		memcpy(inner_desc->arg.triplanar.world_to_texture, m_id, sizeof(m_id));

		struct cr_color_node outer_desc = {
			.type = cr_cn_triplanar,
			.arg.triplanar = { .source = inner_desc, .sharpness = 1.0f }
		};
		memcpy(outer_desc.arg.triplanar.world_to_texture, m_scale, sizeof(m_scale));

		struct vector zn = { 0, 0, 1 };
		rec = make_record(p, zn, outer_uv);
		struct color nested = eval_desc(scene, &outer_desc, sam, &rec);
		// Inner identity node sees Z UV = (p.x,p.y) = (0.1,0.2), not the (0.2,0.4)
		// UV the outer projection temporarily installed.
		require(s, close_color(nested, expected_proc(0.1f, 0.2f)), "nested projection overrides the outer UV");

		// Ordinary UV material sharing the very same child still uses outer UV.
		struct color plain = shared_source->eval(shared_source, sam, &rec);
		require(s, close_color(plain, expected_proc(0.7f, 0.9f)), "shared child still honors ordinary UV evaluation");

		// An image texture is also a valid projection source. Build a small
		// in-memory texture rather than touching the async file decoder.
		struct texture *img = tex_new(float_p, 2, 2, 4);
		tex_set_px(img, (struct color){ 0.1f, 0.2f, 0.3f, 1.0f }, 0, 0);
		tex_set_px(img, (struct color){ 0.4f, 0.5f, 0.6f, 1.0f }, 1, 0);
		tex_set_px(img, (struct color){ 0.7f, 0.8f, 0.9f, 1.0f }, 0, 1);
		tex_set_px(img, (struct color){ 0.15f, 0.25f, 0.35f, 1.0f }, 1, 1);
		const struct colorNode *img_node = newImageTexture(&((struct world *)scene)->storage, img, NO_BILINEAR);
		const struct colorNode *tp_img = newTriplanar(&((struct world *)scene)->storage, img_node, m_id, 1.0f);
		// +X normal, P=(0.25,0.25,0.25): X UV = (-0.25,0.25) -> (0.75,0.25),
		// which samples pixel (1,0) of the 2x2 texture without filtering.
		struct hitRecord img_rec = make_record((struct vector){ 0.25f, 0.25f, 0.25f },
												(struct vector){ 1, 0, 0 }, outer_uv);
		struct color img_got = tp_img->eval(tp_img, sam, &img_rec);
		require(s, close_color(img_got, (struct color){ 0.4f, 0.5f, 0.6f, 1.0f }),
				"triplanar drives an image texture with wrapped UVs");
		tex_destroy(img);

		// Hit record fields survive all of the above.
		require(s, rec.uv.x == 0.7f && rec.uv.y == 0.9f, "outer UV preserved after projection");
		require(s, rec.hitPoint.x == p.x && rec.hitPoint.y == p.y && rec.hitPoint.z == p.z, "hit point preserved");
		require(s, rec.surfaceNormal.z == 1.0f && rec.surfaceNormal.x == 0.0f, "surface normal preserved");
		// Freeing inner_desc also frees the shared proc descriptor tree.
		cr_color_node_free(inner_desc);
	}

	// -- Case 5: deepcopy, descriptor free, serialize/deserialize roundtrip --
	case_begin(s, "deepcopy and renderer serialization roundtrip preserve parameters");
	{
		float m[4][4] = {
			{ 1.0f, 0.0f, 0.0f, 0.1f },
			{ 0.25f, 2.0f, 0.0f, 0.2f },
			{ 0.0f, 0.0f, 0.5f, 0.5f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
		};
		struct cr_color_node *proc = procedural_desc();
		struct cr_color_node *orig = calloc(1, sizeof(*orig));
		orig->type = cr_cn_triplanar;
		orig->arg.triplanar.source = proc;
		orig->arg.triplanar.sharpness = 7.0f;
		memcpy(orig->arg.triplanar.world_to_texture, m, sizeof(m));

		struct cr_renderer *r2 = cr_new_renderer();
		struct cr_scene *s2 = cr_renderer_scene_get(r2);
		const struct colorNode *before = build_color_node(s2, orig);
		struct hitRecord rec = make_record((struct vector){ 0.3f, -0.7f, 0.9f },
										   (struct vector){ 0, 1, 0 }, outer_uv);
		struct color want = before->eval(before, sam, &rec);

		// Bind a material, then free the original descriptor: the bound material
		// (and its deep-copied description) must keep rendering.
		struct cr_shader_node mat = {
			.type = cr_bsdf_diffuse,
			.arg.diffuse.color = orig
		};
		cr_material_set set = cr_scene_new_material_set(s2);
		cr_material_set_add(s2, set, &mat);
		cr_color_node_free(orig);

		struct world *w2 = (struct world *)s2;
		const struct bsdfNode *bound = w2->shader_buffers[set].bsdfs[0];
		struct bsdfSample bs = bound->sample(bound, sam, &rec);
		require(s, close_color(bs.weight, want), "bound material survives descriptor free");
		struct cr_shader_node *kept = w2->shader_buffers[set].descriptions[0];
		require(s, kept->arg.diffuse.color->arg.triplanar.sharpness == 7.0f, "deepcopy keeps sharpness");
		require(s, kept->arg.diffuse.color->arg.triplanar.world_to_texture[1][0] == 0.25f &&
				   kept->arg.diffuse.color->arg.triplanar.world_to_texture[2][3] == 0.5f,
				"deepcopy keeps the full affine matrix");

		cr_camera cam = cr_camera_new(s2);
		cr_camera_set_num_pref(s2, cam, cr_camera_res_x, 8);
		cr_camera_set_num_pref(s2, cam, cr_camera_res_y, 8);
		cr_camera_set_num_pref(s2, cam, cr_camera_pose_z, -2);
		cr_camera_update(s2, cam);

		char *data = serialize_renderer((struct renderer *)r2);
		require(s, data && strstr(data, "\"triplanar\"") && strstr(data, "world_to_texture"),
				"serializer emits the triplanar JSON encoding");

		struct renderer *back = deserialize_renderer(data);
		require(s, back != NULL, "deserializer accepts the serialized renderer");
		struct world *wb = (struct world *)cr_renderer_scene_get((struct cr_renderer *)back);
		struct cr_shader_node *back_desc = wb->shader_buffers[0].descriptions[0];
		require(s, back_desc->arg.diffuse.color->type == cr_cn_triplanar &&
				   back_desc->arg.diffuse.color->arg.triplanar.sharpness == 7.0f,
				"roundtrip restores type and sharpness");
		const struct bsdfNode *back_bsdf = wb->shader_buffers[0].bsdfs[0];
		struct bsdfSample bs2 = back_bsdf->sample(back_bsdf, sam, &rec);
		require(s, close_color(bs2.weight, want), "roundtrip renders the identical color");
		free(data);
		cr_destroy_renderer((struct cr_renderer *)back);
		cr_destroy_renderer(r2);
	}

	// -- Case 6: real render of an un-UV'd corner mesh, written out as PPM ----
	case_begin(s, "cr_renderer_render paints a triplanar corner surface to PPM");
	{
		struct cr_renderer *r3 = cr_new_renderer();
		struct cr_scene *s3 = cr_renderer_scene_get(r3);
		cr_renderer_set_num_pref(r3, cr_renderer_threads, 1);
		cr_renderer_set_num_pref(r3, cr_renderer_samples, 1);
		cr_renderer_set_num_pref(r3, cr_renderer_bounces, 1);

		// An L-shaped corner: an X-facing square and a Z-facing square meeting
		// along one edge. No texture coordinates are bound, so the mesh has no
		// unfolded UVs.
		struct cr_vector verts[6] = {
			{ -0.5f, -0.5f,  0.5f }, // A, shared edge
			{ -0.5f,  0.5f,  0.5f }, // B, shared edge
			{ -0.5f,  0.5f, -0.5f }, // C
			{ -0.5f, -0.5f, -0.5f }, // D
			{  0.5f, -0.5f,  0.5f }, // E
			{  0.5f,  0.5f,  0.5f }, // F
		};
		struct cr_vector norms[8] = {
			{ -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 }, { -1, 0, 0 },
			{ 0, 0, 1 },  { 0, 0, 1 },  { 0, 0, 1 },  { 0, 0, 1 },
		};
		cr_mesh mesh = cr_scene_mesh_new(s3, "corner");
		cr_mesh_bind_vertex_buf(s3, mesh, (struct cr_vertex_buf_param){
			.vertices = verts, .vertex_count = 6,
			.normals = norms, .normal_count = 8,
		});
		struct cr_face faces[4];
		memset(faces, 0, sizeof(faces));
		// X-facing square: A,B,C and A,C,D
		faces[0].vertex_idx[0] = 0; faces[0].vertex_idx[1] = 1; faces[0].vertex_idx[2] = 2;
		faces[0].normal_idx[0] = 0; faces[0].normal_idx[1] = 1; faces[0].normal_idx[2] = 2;
		faces[0].has_normals = true;
		faces[1].vertex_idx[0] = 0; faces[1].vertex_idx[1] = 2; faces[1].vertex_idx[2] = 3;
		faces[1].normal_idx[0] = 0; faces[1].normal_idx[1] = 2; faces[1].normal_idx[2] = 3;
		faces[1].has_normals = true;
		// Z-facing square: A,E,F and A,F,B
		faces[2].vertex_idx[0] = 0; faces[2].vertex_idx[1] = 4; faces[2].vertex_idx[2] = 5;
		faces[2].normal_idx[0] = 4; faces[2].normal_idx[1] = 5; faces[2].normal_idx[2] = 6;
		faces[2].has_normals = true;
		faces[3].vertex_idx[0] = 0; faces[3].vertex_idx[1] = 5; faces[3].vertex_idx[2] = 1;
		faces[3].normal_idx[0] = 4; faces[3].normal_idx[1] = 6; faces[3].normal_idx[2] = 7;
		faces[3].has_normals = true;
		cr_mesh_bind_faces(s3, mesh, faces, 4);
		cr_mesh_finalize(s3, mesh);
		cr_instance inst = cr_instance_new(s3, mesh, cr_object_mesh);
		// Turn the corner 45 degrees about Y so a camera looking down +Z sees
		// both perpendicular faces at once.
		const float r2 = sqrtf(0.5f);
		float rot[4][4] = {
			{  r2, 0.0f, r2, 0.0f },
			{ 0.0f, 1.0f, 0.0f, 0.0f },
			{ -r2, 0.0f, r2, 0.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
		};
		cr_instance_set_transform(s3, inst, rot);

		// Translate texture space by 0.25 on Z so opposite faces sample
		// different parts of the procedural pattern.
		float m[4][4];
		identity_matrix(m);
		m[2][3] = 0.25f;
		struct cr_color_node *proc = procedural_desc();
		struct cr_color_node tp = {
			.type = cr_cn_triplanar,
			.arg.triplanar = { .source = proc, .sharpness = 1.0f }
		};
		memcpy(tp.arg.triplanar.world_to_texture, m, sizeof(m));
		struct cr_value_node strength = { .type = cr_vn_constant, .arg.constant = 1.0 };
		struct cr_shader_node emit = {
			.type = cr_bsdf_emissive,
			.arg.emissive = { .color = &tp, .strength = &strength }
		};
		cr_material_set set = cr_scene_new_material_set(s3);
		cr_material_set_add(s3, set, &emit);
		cr_instance_bind_material_set(s3, inst, set);

		struct cr_color_node black = { .type = cr_cn_constant, .arg.constant = { 0, 0, 0, 1 } };
		struct cr_shader_node bg = { .type = cr_bsdf_background, .arg.background.color = &black };
		cr_scene_set_background(s3, &bg);

		cr_camera cam = cr_camera_new(s3);
		cr_camera_set_num_pref(s3, cam, cr_camera_res_x, 32);
		cr_camera_set_num_pref(s3, cam, cr_camera_res_y, 32);
		cr_camera_set_num_pref(s3, cam, cr_camera_fov, 50.0);
		// Default orientation looks down +Z; the rotated corner faces it.
		cr_camera_set_num_pref(s3, cam, cr_camera_pose_y, 0.4f);
		cr_camera_set_num_pref(s3, cam, cr_camera_pose_z, -3.5f);
		cr_camera_update(s3, cam);

		cr_renderer_render(r3);
		struct cr_bitmap *bm = cr_renderer_get_result(r3);
		require(s, bm && bm->precision == cr_bm_float && bm->width == 32 && bm->height == 32,
				"render produces a 32x32 float bitmap");

		int finite_pixels = 1;
		size_t mesh_pixels = 0;
		float red_min = 1.0f, red_max = 0.0f;
		for (size_t k = 0; k < bm->width * bm->height; ++k) {
			const float *px = &bm->data.float_ptr[k * 4];
			if (!isfinite(px[0]) || !isfinite(px[1]) || !isfinite(px[2])) finite_pixels = 0;
			// The procedural pattern pins blue to 0.2 on every projected face.
			if (px[2] > 0.15f && px[2] < 0.25f) {
				mesh_pixels++;
				red_min = fminf(red_min, px[0]);
				red_max = fmaxf(red_max, px[0]);
			}
		}
		require(s, finite_pixels, "all rendered pixels are finite");
		require(s, mesh_pixels >= 64, "the corner covers a substantial part of the frame");
		require(s, red_max - red_min > 0.2f, "the two corner faces project distinct colors");
		// Image corner misses the mesh entirely and stays background black.
		const float *miss = &bm->data.float_ptr[0];
		require(s, miss[0] == 0.0f && miss[1] == 0.0f && miss[2] == 0.0f,
				"rays missing the corner keep the background color");

		// Persist the rendered corner as a binary PPM so it can be inspected
		// outside the test run. Prefer the repository's gitignored output dir,
		// fall back to the sandbox-provided TMPDIR.
		char path[512];
		snprintf(path, sizeof(path), "output/triplanar_corner.ppm");
		FILE *f = fopen(path, "wb");
		if (!f) {
			const char *tmpdir = getenv("TMPDIR");
			if (!tmpdir) tmpdir = "/tmp";
			snprintf(path, sizeof(path), "%s/triplanar_corner.ppm", tmpdir);
			f = fopen(path, "wb");
		}
		require(s, f != NULL, "PPM output file opens");
		fprintf(f, "P6\n%zu %zu\n255\n", bm->width, bm->height);
		for (size_t y = 0; y < bm->height; ++y) {
			for (size_t x = 0; x < bm->width; ++x) {
				const float *px = &bm->data.float_ptr[(y * bm->width + x) * 4];
				for (int c = 0; c < 3; ++c) {
					float v = px[c] < 0 ? 0 : (px[c] > 1 ? 1 : px[c]);
					unsigned char byte = (unsigned char)(v * 255.0f + 0.5f);
					fwrite(&byte, 1, 1, f);
				}
			}
		}
		fclose(f);
		printf("PPM %s\n", path);
		f = fopen(path, "rb");
		require(s, f && fgetc(f) == 'P' && fgetc(f) == '6', "PPM file has a valid P6 header");
		if (f) fclose(f);

		cr_color_node_free(proc);
		cr_destroy_renderer(r3);
	}

	sampler_destroy(sam);
	cr_destroy_renderer(r);
}
