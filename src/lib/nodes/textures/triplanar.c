//
//  triplanar.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 14/11/2026.
//  Copyright © 2026 Valtteri Koskivuori. All rights reserved.
//

#include <stdio.h>
#include <string.h>
#include <common/color.h>
#include <common/vector.h>
#include <common/mempool.h>
#include <common/hashtable.h>
#include <datatypes/hitrecord.h>
#include <datatypes/scene.h>
#include "../colornode.h"

#include "triplanar.h"
#include "constant.h"

struct triplanarTexture {
	struct colorNode node;
	const struct colorNode *source;
	float world_to_texture[4][4]; // Row-major affine, maps world position to q
	float sharpness;
};

// Wrap any finite coordinate into [0,1), negatives included.
static inline float wrap_unit(float x) {
	return x - floorf(x);
}

// Affine point transform, row-major.
static inline struct vector transform_point(const float m[4][4], const struct vector p) {
	return (struct vector){
		m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
		m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
		m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3],
	};
}

// Transform a surface normal through the inverse transpose of the upper 3x3,
// so non-uniform scale, shear and mirror keep it perpendicular to the same
// tangent plane. The 1/det factor is kept, since a mirrored matrix has a
// negative determinant and dropping it would flip every component's sign.
static inline struct vector transform_normal(const float m[4][4], const struct vector n) {
	const float m00 = m[0][0], m01 = m[0][1], m02 = m[0][2];
	const float m10 = m[1][0], m11 = m[1][1], m12 = m[1][2];
	const float m20 = m[2][0], m21 = m[2][1], m22 = m[2][2];

	const float c00 = m11 * m22 - m12 * m21;
	const float c01 = -(m10 * m22 - m12 * m20);
	const float c02 = m10 * m21 - m11 * m20;
	const float c10 = -(m01 * m22 - m02 * m21);
	const float c11 = m00 * m22 - m02 * m20;
	const float c12 = -(m00 * m21 - m01 * m20);
	const float c20 = m01 * m12 - m02 * m11;
	const float c21 = -(m00 * m12 - m02 * m10);
	const float c22 = m00 * m11 - m01 * m10;

	const float det = m00 * c00 + m01 * c01 + m02 * c02;
	if (det == 0.0f) {
		// Callers promise a finite, invertible matrix; stay defensive anyway.
		return n;
	}
	return vec_normalize((struct vector){
		(c00 * n.x + c01 * n.y + c02 * n.z) / det,
		(c10 * n.x + c11 * n.y + c12 * n.z) / det,
		(c20 * n.x + c21 * n.y + c22 * n.z) / det,
	});
}

static inline float sign_or_positive(float x) {
	return x < 0.0f ? -1.0f : 1.0f;
}

static bool compare(const void *A, const void *B) {
	const struct triplanarTexture *this = A;
	const struct triplanarTexture *other = B;
	return this->source == other->source &&
			this->sharpness == other->sharpness &&
			memcmp(this->world_to_texture, other->world_to_texture, sizeof(this->world_to_texture)) == 0;
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
	const struct triplanarTexture *self = (struct triplanarTexture *)node;
	char source[DUMPBUF_SIZE / 2] = "";
	if (self->source->base.dump) self->source->base.dump(self->source, source, sizeof(source));
	snprintf(dumpbuf, len, "triplanarTexture { source: %s, sharpness: %.2f }", source, (double)self->sharpness);
}

static struct color eval(const struct colorNode *node, sampler *sampler, const struct hitRecord *record) {
	const struct triplanarTexture *this = (const struct triplanarTexture *)node;

	const struct vector q = transform_point(this->world_to_texture, record->hitPoint);
	const struct vector tn = transform_normal(this->world_to_texture, record->surfaceNormal);

	const float ax = powf(fabsf(tn.x), this->sharpness);
	const float ay = powf(fabsf(tn.y), this->sharpness);
	const float az = powf(fabsf(tn.z), this->sharpness);
	const float sum = ax + ay + az;
	const float wx = ax / sum;
	const float wy = ay / sum;
	const float wz = az / sum;

	const float sx = sign_or_positive(tn.x);
	const float sy = sign_or_positive(tn.y);
	const float sz = sign_or_positive(tn.z);

	// Only the UV seen by the child evaluation is overridden; the hit record
	// itself (position, normal, incident ray, outer UV) is left untouched.
	struct hitRecord local = *record;

	local.uv = (struct coord){ wrap_unit(-sx * q.z), wrap_unit(q.y) };
	const struct color cx = this->source->eval(this->source, sampler, &local);

	local.uv = (struct coord){ wrap_unit(q.x), wrap_unit(-sy * q.z) };
	const struct color cy = this->source->eval(this->source, sampler, &local);

	local.uv = (struct coord){ wrap_unit(sz * q.x), wrap_unit(q.y) };
	const struct color cz = this->source->eval(this->source, sampler, &local);

	return (struct color){
		wx * cx.red   + wy * cy.red   + wz * cz.red,
		wx * cx.green + wy * cy.green + wz * cz.green,
		wx * cx.blue  + wy * cy.blue  + wz * cz.blue,
		wx * cx.alpha + wy * cy.alpha + wz * cz.alpha,
	};
}

const struct colorNode *newTriplanar(const struct node_storage *s,
									 const struct colorNode *source,
									 const float world_to_texture[4][4],
									 float sharpness) {
	// HASH_CONS takes a literal initializer, which can't carry the matrix copy,
	// so replicate its lookup/insert logic here.
	struct triplanarTexture candidate = {
		.source = source ? source : newConstantTexture(s, g_black_color),
		.sharpness = sharpness,
		.node = {
			.eval = eval,
			.base = { .compare = compare, .dump = dump }
		}
	};
	memcpy(candidate.world_to_texture, world_to_texture, sizeof(candidate.world_to_texture));

	char dumpbuf[DUMPBUF_SIZE] = "";
	if (candidate.node.base.dump) candidate.node.base.dump(&candidate, dumpbuf, sizeof(dumpbuf));
	const uint32_t h = hash(&candidate);
	const struct triplanarTexture *existing = findInHashtable(s->node_table, &candidate, h);
	if (existing) return (const struct colorNode *)existing;
	insertInHashtable(s->node_table, &candidate, sizeof(candidate), h);
	return (const struct colorNode *)findInHashtable(s->node_table, &candidate, h);
}
