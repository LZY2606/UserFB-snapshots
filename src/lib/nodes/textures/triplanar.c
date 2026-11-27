//
//  triplanar.c
//  c-ray
//
//  Copyright © 2020-2025 Valtteri Koskivuori. All rights reserved.
//

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <common/color.h>
#include <common/vector.h>
#include <common/mempool.h>
#include <common/hashtable.h>
#include <common/logging.h>
#include <datatypes/poly.h>
#include <datatypes/hitrecord.h>
#include <datatypes/scene.h>
#include "../colornode.h"

#include "triplanar.h"

struct triplanarTexture {
	struct colorNode node;
	const struct colorNode *source;
	float world_to_texture[4][4];  // Row-major affine, world -> texture space
	float normal_to_texture[3][3]; // Inverse-transpose of the linear part
	float sharpness;
};

static const float g_identity_mtx[4][4] = {
	{ 1.0f, 0.0f, 0.0f, 0.0f },
	{ 0.0f, 1.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 1.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0f, 1.0f },
};

static bool mtx_all_zero(const float m[4][4]) {
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			if (m[r][c] != 0.0f) return false;
	return true;
}

// Invert the 3x3 linear part of `m` into `normal_mtx`, transposed, so that
// normals (covectors) keep representing the same tangent plane under
// non-uniform scale, shear and mirroring. Returns false if the matrix is
// non-finite or singular.
static bool compute_normal_matrix(const float m[4][4], float normal_mtx[3][3]) {
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			if (!isfinite(m[r][c])) return false;
	const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
	const float c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
	const float c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
	const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
	if (!isfinite(det) || fabsf(det) < 1e-20f) return false;
	const float inv_det = 1.0f / det;
	const float inv[3][3] = {
		{ c00 * inv_det,
		  (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv_det,
		  (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv_det },
		{ c01 * inv_det,
		  (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv_det,
		  (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv_det },
		{ c02 * inv_det,
		  (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv_det,
		  (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv_det },
	};
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			normal_mtx[r][c] = inv[c][r];
	return true;
}

static float wrap01(float x) {
	return x - floorf(x);
}

static bool compare(const void *A, const void *B) {
	const struct triplanarTexture *this = A;
	const struct triplanarTexture *other = B;
	if (this->source != other->source) return false;
	if (this->sharpness != other->sharpness) return false;
	return memcmp(this->world_to_texture, other->world_to_texture, sizeof(this->world_to_texture)) == 0;
}

static uint32_t hash(const void *p) {
	const struct triplanarTexture *this = p;
	uint32_t h = hashInit();
	h = hashBytes(h, &this->source, sizeof(this->source));
	h = hashBytes(h, this->world_to_texture, sizeof(this->world_to_texture));
	h = hashBytes(h, &this->sharpness, sizeof(this->sharpness));
	return h;
}

static void dump(const void *node, char *dumpbuf, int len) {
	struct triplanarTexture *self = (struct triplanarTexture *)node;
	snprintf(dumpbuf, len, "triplanarTexture { sharpness: %.3f, row0: { %.3f, %.3f, %.3f, %.3f } }",
		self->sharpness,
		self->world_to_texture[0][0],
		self->world_to_texture[0][1],
		self->world_to_texture[0][2],
		self->world_to_texture[0][3]);
}

static struct color eval(const struct colorNode *node, sampler *sampler, const struct hitRecord *record) {
	const struct triplanarTexture *this = (const struct triplanarTexture *)node;
	const float (*m)[4] = this->world_to_texture;
	const struct vector p = record->hitPoint;
	const struct vector q = {
		m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
		m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
		m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3],
	};

	// Texture-space unit normal, transformed as a covector.
	const float (*t)[3] = this->normal_to_texture;
	const struct vector n = record->surfaceNormal;
	struct vector tn = {
		t[0][0] * n.x + t[0][1] * n.y + t[0][2] * n.z,
		t[1][0] * n.x + t[1][1] * n.y + t[1][2] * n.z,
		t[2][0] * n.x + t[2][1] * n.y + t[2][2] * n.z,
	};
	const float len = vec_length(tn);
	tn = len > 0.0f ? vec_scale(tn, 1.0f / len) : (struct vector){ 0.0f, 0.0f, 1.0f };

	float wx = powf(fabsf(tn.x), this->sharpness);
	float wy = powf(fabsf(tn.y), this->sharpness);
	float wz = powf(fabsf(tn.z), this->sharpness);
	const float sum = wx + wy + wz;
	if (sum > 0.0f) {
		wx /= sum;
		wy /= sum;
		wz /= sum;
	} else {
		wx = wy = wz = 1.0f / 3.0f;
	}

	const float sx = tn.x < 0.0f ? -1.0f : 1.0f;
	const float sy = tn.y < 0.0f ? -1.0f : 1.0f;
	const float sz = tn.z < 0.0f ? -1.0f : 1.0f;

	// Only the UV seen by this one source evaluation is replaced; the
	// incoming record is left untouched, so nothing per-pixel ends up
	// in the shared, hash-consed node.
	struct hitRecord projected = *record;
	struct color out = { 0.0f, 0.0f, 0.0f, 0.0f };
	projected.uv = (struct coord){ wrap01(-sx * q.z), wrap01(q.y) };
	out = colorAdd(out, colorCoef(wx, this->source->eval(this->source, sampler, &projected)));
	projected.uv = (struct coord){ wrap01(q.x), wrap01(-sy * q.z) };
	out = colorAdd(out, colorCoef(wy, this->source->eval(this->source, sampler, &projected)));
	projected.uv = (struct coord){ wrap01(sz * q.x), wrap01(q.y) };
	out = colorAdd(out, colorCoef(wz, this->source->eval(this->source, sampler, &projected)));
	return out;
}

const struct colorNode *newTriplanarTexture(const struct node_storage *s, const struct colorNode *source, const float world_to_texture[4][4], float sharpness) {
	float mtx[4][4];
	memcpy(mtx, g_identity_mtx, sizeof(mtx));
	if (world_to_texture && !mtx_all_zero(world_to_texture))
		memcpy(mtx, world_to_texture, sizeof(mtx));
	float normal_mtx[3][3];
	if (!compute_normal_matrix(mtx, normal_mtx)) {
		logr(warning, "Invalid world_to_texture matrix for triplanar node, falling back to identity.\n");
		memcpy(mtx, g_identity_mtx, sizeof(mtx));
		compute_normal_matrix(mtx, normal_mtx);
	}
	float sharp = sharpness;
	if (!isfinite(sharp)) sharp = 1.0f;
	sharp = clamp(sharp, 1.0f, 64.0f);
	struct triplanarTexture to_insert = {
		.source = source ? source : newConstantTexture(s, g_black_color),
		.sharpness = sharp,
		.node = {
			.eval = eval,
			.base = { .compare = compare, .dump = dump }
		}
	};
	memcpy(to_insert.world_to_texture, mtx, sizeof(mtx));
	memcpy(to_insert.normal_to_texture, normal_mtx, sizeof(normal_mtx));
	HASH_CONS(s->node_table, hash, struct triplanarTexture, to_insert);
}
