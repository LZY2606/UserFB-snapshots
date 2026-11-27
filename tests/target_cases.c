//
//  target_cases.c
//  c-ray
//
//  Tests for the cr_cn_triplanar color node: world-space triplanar
//  projection with an affine world_to_texture transform.
//

#include <c-ray/c-ray.h>
#include "../support/test_api.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <common/color.h>
#include <common/node_parse.h>
#include <nodes/bsdfnode.h>
#include <nodes/colornode.h>
#include <datatypes/hitrecord.h>
#include <datatypes/scene.h>
#include <renderer/renderer.h>
#include <renderer/samplers/sampler.h>
#include <protocol/protocol.h>

// ---- Descriptor helpers -------------------------------------------------

static struct cr_value_node *mk_const_value(double v) {
	struct cr_value_node *n = calloc(1, sizeof(*n));
	n->type = cr_vn_constant;
	n->arg.constant = v;
	return n;
}

static struct cr_value_node *mk_uv_component(enum cr_vec_to_value_component comp) {
	struct cr_vector_node *uv = calloc(1, sizeof(*uv));
	uv->type = cr_vec_uv;
	struct cr_value_node *n = calloc(1, sizeof(*n));
	n->type = cr_vn_vec_to_value;
	n->arg.vec_to_value.comp = comp;
	n->arg.vec_to_value.vec = uv;
	return n;
}

// Asymmetric procedural color: (u, v, 0.5, 1), composed through the UV node.
static struct cr_color_node *mk_uv_color(void) {
	struct cr_color_node *c = calloc(1, sizeof(*c));
	c->type = cr_cn_rgb;
	c->arg.rgb.red = mk_uv_component(U);
	c->arg.rgb.green = mk_uv_component(V);
	c->arg.rgb.blue = mk_const_value(0.5);
	return c;
}

static struct cr_color_node *mk_const_color(float r, float g, float b, float a) {
	struct cr_color_node *c = calloc(1, sizeof(*c));
	c->type = cr_cn_constant;
	c->arg.constant = (struct cr_color){ r, g, b, a };
	return c;
}

static struct cr_color_node *mk_checker(float scale) {
	struct cr_color_node *c = calloc(1, sizeof(*c));
	c->type = cr_cn_checkerboard;
	c->arg.checkerboard.a = mk_const_color(1.0f, 0.0f, 0.0f, 1.0f);
	c->arg.checkerboard.b = mk_const_color(0.0f, 0.0f, 1.0f, 1.0f);
	c->arg.checkerboard.scale = mk_const_value(scale);
	return c;
}

// mtx == NULL leaves the all-zero matrix, i.e. the identity default.
static struct cr_color_node *mk_triplanar(struct cr_color_node *source, const float mtx[4][4], float sharpness) {
	struct cr_color_node *c = calloc(1, sizeof(*c));
	c->type = cr_cn_triplanar;
	c->arg.triplanar.source = source;
	if (mtx) memcpy(c->arg.triplanar.world_to_texture, mtx, sizeof(c->arg.triplanar.world_to_texture));
	c->arg.triplanar.sharpness = sharpness;
	return c;
}

// ---- Reference implementation (from the spec, not from the library) -----

static float wrap01f(float x) {
	return x - floorf(x);
}

static const float g_identity[4][4] = {
	{ 1.0f, 0.0f, 0.0f, 0.0f },
	{ 0.0f, 1.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 1.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0f, 1.0f },
};

static const float g_shear[4][4] = {
	{ 2.0f, 0.5f, 0.0f,  0.25f },
	{ 0.0f, 1.0f, 0.0f, -0.50f },
	{ 0.0f, 0.0f, 0.5f,  0.0f  },
	{ 0.0f, 0.0f, 0.0f,  1.0f  },
};

static const float g_mirror[4][4] = {
	{ -1.0f, 0.0f, 0.0f, 0.75f },
	{  0.0f, 1.0f, 0.0f, 0.0f  },
	{  0.0f, 0.0f, 1.0f, 0.0f  },
	{  0.0f, 0.0f, 0.0f, 1.0f  },
};

struct ref_projection {
	float w[3];
	float uv[3][2];
};

// Inverse-transpose of the 3x3 linear part, so the normal stays a covector.
static void ref_normal_mtx(const float m[4][4], float nrm[3][3]) {
	const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
	const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
	const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
	const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
	const float inv[3][3] = {
		{ c00 / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det },
		{ c01 / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det },
		{ c02 / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det },
	};
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			nrm[r][c] = inv[c][r];
}

static struct ref_projection ref_project(const float m[4][4], float sharp, struct vector p, struct vector n) {
	float q[3];
	for (int i = 0; i < 3; ++i)
		q[i] = m[i][0] * p.x + m[i][1] * p.y + m[i][2] * p.z + m[i][3];
	float nrm[3][3];
	ref_normal_mtx(m, nrm);
	float tn[3];
	for (int i = 0; i < 3; ++i)
		tn[i] = nrm[i][0] * n.x + nrm[i][1] * n.y + nrm[i][2] * n.z;
	const float len = sqrtf(tn[0] * tn[0] + tn[1] * tn[1] + tn[2] * tn[2]);
	for (int i = 0; i < 3; ++i) tn[i] /= len;
	struct ref_projection out;
	float sum = 0.0f;
	for (int i = 0; i < 3; ++i) {
		out.w[i] = powf(fabsf(tn[i]), sharp);
		sum += out.w[i];
	}
	for (int i = 0; i < 3; ++i) out.w[i] /= sum;
	const float sx = tn[0] < 0.0f ? -1.0f : 1.0f;
	const float sy = tn[1] < 0.0f ? -1.0f : 1.0f;
	const float sz = tn[2] < 0.0f ? -1.0f : 1.0f;
	out.uv[0][0] = wrap01f(-sx * q[2]); out.uv[0][1] = wrap01f(q[1]);
	out.uv[1][0] = wrap01f(q[0]);       out.uv[1][1] = wrap01f(-sy * q[2]);
	out.uv[2][0] = wrap01f(sz * q[0]);  out.uv[2][1] = wrap01f(q[1]);
	return out;
}

// Expected result for the (u, v, 0.5) procedural source color.
static struct color ref_uv_color(const float m[4][4], float sharp, struct vector p, struct vector n) {
	struct ref_projection proj = ref_project(m, sharp, p, n);
	struct color out = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int i = 0; i < 3; ++i) {
		out.red   += proj.w[i] * proj.uv[i][0];
		out.green += proj.w[i] * proj.uv[i][1];
		out.blue  += proj.w[i] * 0.5f;
		out.alpha += proj.w[i] * 1.0f;
	}
	return out;
}

// ---- Test scaffolding ---------------------------------------------------

static struct color eval_desc(struct cr_scene *scene, sampler *sam, const struct cr_color_node *desc, const struct hitRecord *rec) {
	const struct colorNode *node = build_color_node(scene, desc);
	return node->eval(node, sam, rec);
}

static struct hitRecord mk_record(struct lightRay *ray, struct vector p, struct vector n) {
	return (struct hitRecord){
		.incident = ray,
		.hitPoint = p,
		.surfaceNormal = n,
		.uv = { 0.13f, 0.87f },
	};
}

static void require_color(struct suite *s, struct color got, struct color want, float tol, const char *why) {
	char buf[160];
	snprintf(buf, sizeof(buf), "%s (got %.6f %.6f %.6f, want %.6f %.6f %.6f)",
		why, got.red, got.green, got.blue, want.red, want.green, want.blue);
	require(s,
		fabsf(got.red - want.red) < tol &&
		fabsf(got.green - want.green) < tol &&
		fabsf(got.blue - want.blue) < tol,
		buf);
}

// ---- Cases --------------------------------------------------------------

static void case_axis_projections(struct suite *st) {
	case_begin(st, "axis projections");
	struct cr_renderer *r = cr_new_renderer();
	struct cr_scene *scene = cr_renderer_scene_get(r);
	sampler *sam = sampler_new();
	sampler_init(sam, Halton, 0, 2, 1);
	struct lightRay ray = { .direction = { 0.0f, 0.0f, -1.0f } };
	const struct vector p = { 0.2f, 0.4f, 0.7f };
	const struct vector normals[6] = {
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
	};
	struct cr_color_node *desc = mk_triplanar(mk_uv_color(), g_identity, 1.0f);
	for (int i = 0; i < 6; ++i) {
		struct hitRecord rec = mk_record(&ray, p, normals[i]);
		struct color got = eval_desc(scene, sam, desc, &rec);
		struct color want = ref_uv_color(g_identity, 1.0f, p, normals[i]);
		require_color(st, got, want, 1e-5f, "axis projection mismatch");
	}
	// All-zero matrix must behave like the identity default.
	struct cr_color_node *def_mtx = mk_triplanar(mk_uv_color(), NULL, 1.0f);
	struct hitRecord rec = mk_record(&ray, p, normals[4]);
	require_color(st, eval_desc(scene, sam, def_mtx, &rec),
		eval_desc(scene, sam, desc, &rec), 1e-6f, "zero matrix != identity");
	// Sharpness 0 selects the default of 1.
	struct cr_color_node *def_sharp = mk_triplanar(mk_uv_color(), g_identity, 0.0f);
	require_color(st, eval_desc(scene, sam, def_sharp, &rec),
		eval_desc(scene, sam, desc, &rec), 1e-6f, "sharpness 0 != default 1");
	sampler_destroy(sam);
	cr_destroy_renderer(r);
}

static void case_negative_coords_checker(struct suite *st) {
	case_begin(st, "negative coords and checkerboard");
	struct cr_renderer *r = cr_new_renderer();
	struct cr_scene *scene = cr_renderer_scene_get(r);
	sampler *sam = sampler_new();
	sampler_init(sam, Halton, 0, 2, 1);
	struct lightRay ray = { .direction = { 0.0f, 0.0f, -1.0f } };

	// Wrapped UVs must land in [0,1) even for negative texture coords.
	const struct vector p_neg = { -3.7f, 2.3f, -1.9f };
	const struct vector n_slant = { 0.3f, -0.5f, 0.8f };
	struct cr_color_node *uvc = mk_triplanar(mk_uv_color(), g_identity, 1.0f);
	struct hitRecord rec = mk_record(&ray, p_neg, n_slant);
	struct color got = eval_desc(scene, sam, uvc, &rec);
	require(st, got.red >= 0.0f && got.red < 1.0f && got.green >= 0.0f && got.green < 1.0f,
		"wrapped UV left [0,1)");
	require_color(st, got, ref_uv_color(g_identity, 1.0f, p_neg, n_slant), 1e-5f,
		"negative coordinate projection mismatch");

	// The wrapped UVs must not trip the checkerboard's no-UV 3D fallback:
	// mapped checker at (0.7, 0.9) is red, the 3D fallback here would be blue.
	struct cr_color_node *checker = mk_triplanar(mk_checker(2.0f), g_identity, 1.0f);
	const struct vector p_chk = { -0.3f, -0.1f, 0.6f };
	rec = mk_record(&ray, p_chk, (struct vector){ 0.0f, 0.0f, 1.0f });
	got = eval_desc(scene, sam, checker, &rec);
	require(st, got.red > 0.9f && got.blue < 0.1f, "Z projection took checkerboard fallback");
	rec = mk_record(&ray, p_chk, (struct vector){ 1.0f, 0.0f, 0.0f });
	got = eval_desc(scene, sam, checker, &rec);
	require(st, got.blue > 0.9f && got.red < 0.1f, "X projection checkerboard mismatch");
	sampler_destroy(sam);
	cr_destroy_renderer(r);
}

static void case_shear_and_mirror(struct suite *st) {
	case_begin(st, "shear, scale and mirror transforms");
	struct cr_renderer *r = cr_new_renderer();
	struct cr_scene *scene = cr_renderer_scene_get(r);
	sampler *sam = sampler_new();
	sampler_init(sam, Halton, 0, 2, 1);
	struct lightRay ray = { .direction = { 0.0f, 0.0f, -1.0f } };
	const struct vector p = { 0.35f, -0.2f, 0.9f };
	const struct vector n = { 0.3f, -0.5f, 0.8f };

	struct cr_color_node *shear1 = mk_triplanar(mk_uv_color(), g_shear, 1.0f);
	struct hitRecord rec = mk_record(&ray, p, n);
	require_color(st, eval_desc(scene, sam, shear1, &rec),
		ref_uv_color(g_shear, 1.0f, p, n), 1e-5f, "shear+scale sharpness 1 mismatch");

	struct cr_color_node *shear32 = mk_triplanar(mk_uv_color(), g_shear, 32.0f);
	require_color(st, eval_desc(scene, sam, shear32, &rec),
		ref_uv_color(g_shear, 32.0f, p, n), 1e-5f, "shear+scale sharpness 32 mismatch");

	struct cr_color_node *mirror = mk_triplanar(mk_uv_color(), g_mirror, 2.0f);
	require_color(st, eval_desc(scene, sam, mirror, &rec),
		ref_uv_color(g_mirror, 2.0f, p, n), 1e-5f, "mirror transform mismatch");

	// Mirroring flips the X sign of the texture-space normal, which must
	// show up in the projected UVs.
	const struct vector nx = { 1.0f, 0.0f, 0.0f };
	rec = mk_record(&ray, p, nx);
	require_color(st, eval_desc(scene, sam, mirror, &rec),
		ref_uv_color(g_mirror, 2.0f, p, nx), 1e-5f, "mirror sign flip mismatch");
	sampler_destroy(sam);
	cr_destroy_renderer(r);
}

static void case_sharing_and_nesting(struct suite *st) {
	case_begin(st, "hash-consing, sharing and nesting");
	struct cr_renderer *r = cr_new_renderer();
	struct cr_scene *scene = cr_renderer_scene_get(r);
	sampler *sam = sampler_new();
	sampler_init(sam, Halton, 0, 2, 1);
	struct lightRay ray = { .direction = { 0.0f, 0.0f, -1.0f } };
	const struct vector p = { 0.35f, -0.2f, 0.9f };
	const struct vector n = { 0.3f, -0.5f, 0.8f };

	struct cr_color_node *a = mk_triplanar(mk_uv_color(), NULL, 2.0f);
	const struct colorNode *na1 = build_color_node(scene, a);
	const struct colorNode *na2 = build_color_node(scene, a);
	require(st, na1 == na2, "identical triplanar descriptors not hash-consed");

	struct cr_color_node *b = mk_triplanar(mk_uv_color(), NULL, 3.0f);
	require(st, build_color_node(scene, b) != na1, "sharpness-only difference merged");

	struct cr_color_node *c = mk_triplanar(mk_uv_color(), g_shear, 2.0f);
	require(st, build_color_node(scene, c) != na1, "matrix-only difference merged");

	// Nested projection: the inner node re-projects from the same hit, so
	// the outer node's UV override is fully replaced by the inner one.
	struct cr_color_node *inner = mk_triplanar(mk_uv_color(), g_shear, 4.0f);
	struct cr_color_node *outer = mk_triplanar(inner, g_mirror, 16.0f);
	struct hitRecord rec = mk_record(&ray, p, n);
	struct color got_outer = eval_desc(scene, sam, outer, &rec);
	require_color(st, got_outer, ref_uv_color(g_shear, 4.0f, p, n), 1e-5f,
		"nested projection mismatch");

	// The same source also works as a plain UV color, and the record's
	// own UV survives every projection above untouched.
	struct cr_color_node *plain = mk_uv_color();
	struct color direct = eval_desc(scene, sam, plain, &rec);
	require_color(st, direct, (struct color){ 0.13f, 0.87f, 0.5f, 1.0f }, 1e-6f,
		"plain UV evaluation mismatch after nesting");
	require(st, rec.uv.x == 0.13f && rec.uv.y == 0.87f, "hit record UV clobbered");
	sampler_destroy(sam);
	cr_destroy_renderer(r);
}

static void case_serialize_roundtrip(struct suite *st) {
	case_begin(st, "serialize round trip");
	struct cr_renderer *r1 = cr_new_renderer();
	cr_renderer_set_num_pref(r1, cr_renderer_threads, 1);
	cr_renderer_set_num_pref(r1, cr_renderer_samples, 1);
	cr_renderer_set_num_pref(r1, cr_renderer_bounces, 1);
	struct cr_scene *s1 = cr_renderer_scene_get(r1);
	cr_camera cam = cr_camera_new(s1);
	cr_camera_set_num_pref(s1, cam, cr_camera_res_x, 8);
	cr_camera_set_num_pref(s1, cam, cr_camera_res_y, 8);
	cr_camera_set_num_pref(s1, cam, cr_camera_pose_z, -4.0);
	cr_camera_update(s1, cam);

	struct cr_shader_node *shader = calloc(1, sizeof(*shader));
	shader->type = cr_bsdf_diffuse;
	shader->arg.diffuse.color = mk_triplanar(mk_uv_color(), g_shear, 8.0f);
	cr_material_set set = cr_scene_new_material_set(s1);
	cr_material_set_add(s1, set, shader);
	cr_sphere sphere = cr_scene_add_sphere(s1, 1.0f);
	cr_instance inst = cr_instance_new(s1, sphere, cr_object_sphere);
	cr_instance_bind_material_set(s1, inst, set);
	struct cr_color_node white = { .type = cr_cn_constant, .arg.constant = { 1, 1, 1, 1 } };
	struct cr_shader_node bg = { .type = cr_bsdf_background, .arg.background.color = &white };
	cr_scene_set_background(s1, &bg);
	// The bound material must not depend on the original descriptor anymore.
	cr_shader_node_free(shader);

	char *json = serialize_renderer((struct renderer *)r1);
	require(st, json != NULL, "serialize_renderer returned NULL");
	require(st, strstr(json, "\"triplanar\"") != NULL, "triplanar missing from serialized json");

	struct renderer *r2 = deserialize_renderer(json);
	require(st, r2 != NULL, "deserialize_renderer returned NULL");
	struct cr_shader_node *desc2 = r2->scene->shader_buffers[0].descriptions[0];
	require(st, desc2->type == cr_bsdf_diffuse &&
		desc2->arg.diffuse.color &&
		desc2->arg.diffuse.color->type == cr_cn_triplanar,
		"deserialized node is not a diffuse triplanar");
	const struct cr_triplanar_params *tp = &desc2->arg.diffuse.color->arg.triplanar;
	bool mtx_ok = tp->sharpness == 8.0f;
	for (int r_ = 0; r_ < 4; ++r_)
		for (int c_ = 0; c_ < 4; ++c_)
			mtx_ok = mtx_ok && tp->world_to_texture[r_][c_] == g_shear[r_][c_];
	require(st, mtx_ok, "matrix or sharpness changed in round trip");

	// Colors must match the reference after the round trip, in both scenes.
	sampler *sam = sampler_new();
	sampler_init(sam, Halton, 0, 2, 1);
	struct lightRay ray = { .direction = { 0.0f, 0.0f, -1.0f } };
	const struct vector p = { 0.35f, -0.2f, 0.9f };
	const struct vector n = { 0.3f, -0.5f, 0.8f };
	struct hitRecord rec = mk_record(&ray, p, n);
	struct color want = ref_uv_color(g_shear, 8.0f, p, n);
	struct cr_shader_node *desc1 = ((struct renderer *)r1)->scene->shader_buffers[0].descriptions[0];
	require_color(st, eval_desc(s1, sam, desc1->arg.diffuse.color, &rec), want, 1e-5f,
		"original scene color mismatch");
	require_color(st, eval_desc((struct cr_scene *)r2->scene, sam, desc2->arg.diffuse.color, &rec), want, 1e-5f,
		"deserialized scene color mismatch");
	sampler_destroy(sam);

	// Rendering with the original descriptor long gone must still work.
	cr_renderer_render(r1);
	struct cr_bitmap *b = cr_renderer_get_result(r1);
	require(st, b && b->precision == cr_bm_float && b->width == 8 && b->height == 8,
		"render after freeing descriptor failed");
	bool finite = true;
	for (size_t i = 0; i < b->width * b->height * 4; ++i)
		finite = finite && isfinite(b->data.float_ptr[i]);
	require(st, finite, "non-finite pixel after freeing descriptor");

	free(json);
	cr_destroy_renderer(r1);
	cr_destroy_renderer((struct cr_renderer *)r2);
}

static int write_ppm(const char *path, const struct cr_bitmap *b) {
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	fprintf(f, "P6\n%zu %zu\n255\n", b->width, b->height);
	for (size_t i = 0; i < b->width * b->height; ++i) {
		const float *px = &b->data.float_ptr[i * 4];
		unsigned char rgb[3];
		for (int c = 0; c < 3; ++c) {
			float v = px[c];
			v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
			rgb[c] = (unsigned char)(linearToSRGB(v) * 255.0f + 0.5f);
		}
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	return 0;
}

static void case_render_ppm(struct suite *st) {
	case_begin(st, "render corner surface ppm");
	struct cr_renderer *r = cr_new_renderer();
	cr_renderer_set_num_pref(r, cr_renderer_threads, 1);
	cr_renderer_set_num_pref(r, cr_renderer_samples, 2);
	cr_renderer_set_num_pref(r, cr_renderer_bounces, 2);
	struct cr_scene *scene = cr_renderer_scene_get(r);
	cr_camera cam = cr_camera_new(scene);
	cr_camera_set_num_pref(scene, cam, cr_camera_res_x, 64);
	cr_camera_set_num_pref(scene, cam, cr_camera_res_y, 48);
	cr_camera_set_num_pref(scene, cam, cr_camera_pose_z, -3.2);
	cr_camera_update(scene, cam);

	// A scanned-style cube: no UVs unwrapped at all.
	struct cr_vector verts[8] = {
		{ -0.8f, -0.8f, -0.8f }, { 0.8f, -0.8f, -0.8f },
		{  0.8f,  0.8f, -0.8f }, { -0.8f, 0.8f, -0.8f },
		{ -0.8f, -0.8f,  0.8f }, { 0.8f, -0.8f,  0.8f },
		{  0.8f,  0.8f,  0.8f }, { -0.8f, 0.8f,  0.8f },
	};
	struct cr_face faces[12] = { 0 };
	const int tris[12][3] = {
		{ 4, 5, 6 }, { 4, 6, 7 }, { 1, 0, 3 }, { 1, 3, 2 },
		{ 5, 1, 2 }, { 5, 2, 6 }, { 0, 4, 7 }, { 0, 7, 3 },
		{ 7, 6, 2 }, { 7, 2, 3 }, { 0, 1, 5 }, { 0, 5, 4 },
	};
	for (int i = 0; i < 12; ++i) {
		for (int k = 0; k < 3; ++k) {
			faces[i].vertex_idx[k] = tris[i][k];
			faces[i].texture_idx[k] = -1;
		}
		faces[i].has_normals = false;
	}
	cr_mesh cube = cr_scene_mesh_new(scene, "cube");
	cr_mesh_bind_vertex_buf(scene, cube, (struct cr_vertex_buf_param){
		.vertices = verts, .vertex_count = 8,
	});
	cr_mesh_bind_faces(scene, cube, faces, 12);
	cr_mesh_finalize(scene, cube);

	// Emissive triplanar checker so the projection shows up directly.
	struct cr_shader_node *shader = calloc(1, sizeof(*shader));
	shader->type = cr_bsdf_emissive;
	shader->arg.emissive.color = mk_triplanar(mk_checker(3.0f), NULL, 8.0f);
	shader->arg.emissive.strength = mk_const_value(1.0);
	cr_material_set set = cr_scene_new_material_set(scene);
	cr_material_set_add(scene, set, shader);
	cr_shader_node_free(shader);

	cr_instance inst = cr_instance_new(scene, cube, cr_object_mesh);
	// Tip the cube so a corner faces the camera.
	const float ay = 0.6f, ax = 0.45f;
	const float cy = cosf(ay), sy = sinf(ay), cx = cosf(ax), sx = sinf(ax);
	float rot[4][4] = {
		{ cy,  sy * sx,  sy * cx, 0.0f },
		{ 0.0f,     cx,      -sx, 0.0f },
		{ -sy, cy * sx,  cy * cx, 0.0f },
		{ 0.0f,   0.0f,    0.0f,  1.0f },
	};
	cr_instance_set_transform(scene, inst, rot);
	cr_instance_bind_material_set(scene, inst, set);

	struct cr_color_node gray = { .type = cr_cn_constant, .arg.constant = { 0.05f, 0.05f, 0.05f, 1.0f } };
	struct cr_shader_node bg = { .type = cr_bsdf_background, .arg.background.color = &gray };
	cr_scene_set_background(scene, &bg);

	cr_renderer_render(r);
	struct cr_bitmap *b = cr_renderer_get_result(r);
	require(st, b && b->precision == cr_bm_float && b->width == 64 && b->height == 48,
		"unexpected render result bitmap");
	bool finite = true;
	float min_r = 1e9f, max_r = -1e9f, min_b = 1e9f, max_b = -1e9f;
	for (size_t i = 0; i < b->width * b->height; ++i) {
		const float *px = &b->data.float_ptr[i * 4];
		for (int c = 0; c < 4; ++c) finite = finite && isfinite(px[c]);
		if (px[0] < min_r) min_r = px[0];
		if (px[0] > max_r) max_r = px[0];
		if (px[2] < min_b) min_b = px[2];
		if (px[2] > max_b) max_b = px[2];
	}
	require(st, finite, "non-finite pixel in corner render");
	// The projected checkerboard must produce visible red/blue variation.
	require(st, max_r - min_r > 0.3f && max_b - min_b > 0.3f,
		"triplanar projection shows no color variation");
	require(st, write_ppm("output/triplanar_corner.ppm", b) == 0, "failed to write ppm");
	cr_destroy_renderer(r);
}

void target_cases(struct suite *s) {
	case_axis_projections(s);
	case_negative_coords_checker(s);
	case_shear_and_mirror(s);
	case_sharing_and_nesting(s);
	case_serialize_roundtrip(s);
	case_render_ppm(s);
}
