/*
MATHC skinning helper: CPU vertex skinning without any graphics context.

This software is provided 'as-is', without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use of
this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to
the following restrictions:

  1. The origin of this software must not be misrepresented; you must not claim
     that you wrote the original software. If you use this software in a
     product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "mathc_skin.h"

/* Degeneracy thresholds, scaled by the working precision. */
#define SKIN_EPS (MFLOAT_C(64.0) * MFLT_EPSILON)

static mfloat_t skin_dot3(const mfloat_t a[3], const mfloat_t b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static mfloat_t skin_length3(const mfloat_t v[3])
{
	return MSQRT(skin_dot3(v, v));
}

static void skin_cross3(mfloat_t result[3], const mfloat_t a[3],
	const mfloat_t b[3])
{
	result[0] = a[1] * b[2] - a[2] * b[1];
	result[1] = a[2] * b[0] - a[0] * b[2];
	result[2] = a[0] * b[1] - a[1] * b[0];
}

bool mathc_skin_vertex(mfloat_t out_position[3], mfloat_t out_normal[3],
	mfloat_t out_tangent[4], const mfloat_t position[3],
	const mfloat_t normal[3], const mfloat_t tangent[4],
	const unsigned joints[4], const mfloat_t weights[4],
	const mfloat_t *palette, size_t palette_count)
{
	mfloat_t blend[MAT4_SIZE];
	mfloat_t a0[3], a1[3], a2[3]; /* columns of the blended linear part */
	mfloat_t r0[3], r1[3], r2[3]; /* columns of its cofactor matrix */
	mfloat_t weight_sum, det, s, d;
	mfloat_t p[3], n[3], tg[4];
	mfloat_t nl, tl, ol;
	int i, k;

	if (out_position == NULL || out_normal == NULL || out_tangent == NULL ||
		position == NULL || normal == NULL || tangent == NULL ||
		joints == NULL || weights == NULL || palette == NULL) {
		return false;
	}
	/* Validate the vertex inputs before touching the palette. */
	for (i = 0; i < 3; i++) {
		if (!isfinite(position[i]) || !isfinite(normal[i]) ||
			!isfinite(tangent[i])) {
			return false;
		}
	}
	if (!isfinite(tangent[3])) {
		return false;
	}
	if (tangent[3] != MFLOAT_C(1.0) && tangent[3] != -MFLOAT_C(1.0)) {
		return false;
	}
	if (skin_length3(normal) == MFLOAT_C(0.0) ||
		skin_length3(tangent) == MFLOAT_C(0.0)) {
		return false;
	}
	skin_cross3(r0, normal, tangent);
	if (skin_length3(r0) == MFLOAT_C(0.0)) {
		/* Normal and tangent must not be parallel. */
		return false;
	}
	weight_sum = MFLOAT_C(0.0);
	for (i = 0; i < 4; i++) {
		mfloat_t w = weights[i];
		if (!isfinite(w) || w < MFLOAT_C(0.0)) {
			return false;
		}
		weight_sum += w;
	}
	if (weight_sum == MFLOAT_C(0.0)) {
		return false;
	}
	/* Blend the joint matrices element-wise with normalized weights. */
	for (k = 0; k < MAT4_SIZE; k++) {
		blend[k] = MFLOAT_C(0.0);
	}
	for (i = 0; i < 4; i++) {
		const mfloat_t *pm;
		mfloat_t wn;
		if (weights[i] == MFLOAT_C(0.0)) {
			/* Zero-weight joints are never accessed. */
			continue;
		}
		if ((size_t)joints[i] >= palette_count) {
			return false;
		}
		pm = palette + (size_t)joints[i] * MAT4_SIZE;
		for (k = 0; k < MAT4_SIZE; k++) {
			if (!isfinite(pm[k])) {
				return false;
			}
		}
		if (pm[3] != MFLOAT_C(0.0) || pm[7] != MFLOAT_C(0.0) ||
			pm[11] != MFLOAT_C(0.0) || pm[15] != MFLOAT_C(1.0)) {
			/* Only affine matrices (last row 0 0 0 1) are accepted. */
			return false;
		}
		wn = weights[i] / weight_sum;
		for (k = 0; k < MAT4_SIZE; k++) {
			blend[k] += wn * pm[k];
		}
	}
	/* Blended 3x3 linear part, column-major. */
	a0[0] = blend[0]; a0[1] = blend[1]; a0[2] = blend[2];
	a1[0] = blend[4]; a1[1] = blend[5]; a1[2] = blend[6];
	a2[0] = blend[8]; a2[1] = blend[9]; a2[2] = blend[10];
	s = MFLOAT_C(0.0);
	for (k = 0; k < 3; k++) {
		s = MFMAX(s, MFABS(a0[k]));
		s = MFMAX(s, MFABS(a1[k]));
		s = MFMAX(s, MFABS(a2[k]));
	}
	if (s == MFLOAT_C(0.0)) {
		return false;
	}
	/*
	For a matrix with columns a0, a1, a2 the inverse-transpose is the
	cofactor matrix divided by the determinant, and the cofactor columns
	are the cross products below.
	*/
	skin_cross3(r0, a1, a2);
	skin_cross3(r1, a2, a0);
	skin_cross3(r2, a0, a1);
	det = skin_dot3(a0, r0);
	if (MFABS(det) <= SKIN_EPS * s * s * s) {
		return false;
	}
	/* Position: full affine transform of the blended matrix. */
	p[0] = a0[0] * position[0] + a1[0] * position[1] +
		a2[0] * position[2] + blend[12];
	p[1] = a0[1] * position[0] + a1[1] * position[1] +
		a2[1] * position[2] + blend[13];
	p[2] = a0[2] * position[0] + a1[2] * position[1] +
		a2[2] * position[2] + blend[14];
	/* Normal: inverse-transpose of the linear part, then normalize. */
	n[0] = (normal[0] * r0[0] + normal[1] * r1[0] + normal[2] * r2[0]) / det;
	n[1] = (normal[0] * r0[1] + normal[1] * r1[1] + normal[2] * r2[1]) / det;
	n[2] = (normal[0] * r0[2] + normal[1] * r1[2] + normal[2] * r2[2]) / det;
	nl = skin_length3(n);
	if (nl == MFLOAT_C(0.0)) {
		return false;
	}
	n[0] /= nl;
	n[1] /= nl;
	n[2] /= nl;
	/* Tangent: forward linear transform (translation never applies). */
	tg[0] = a0[0] * tangent[0] + a1[0] * tangent[1] + a2[0] * tangent[2];
	tg[1] = a0[1] * tangent[0] + a1[1] * tangent[1] + a2[1] * tangent[2];
	tg[2] = a0[2] * tangent[0] + a1[2] * tangent[1] + a2[2] * tangent[2];
	tl = skin_length3(tg);
	if (tl == MFLOAT_C(0.0)) {
		return false;
	}
	/* Gram-Schmidt against the output normal. */
	d = skin_dot3(n, tg);
	tg[0] -= d * n[0];
	tg[1] -= d * n[1];
	tg[2] -= d * n[2];
	ol = skin_length3(tg);
	if (ol <= SKIN_EPS * tl) {
		return false;
	}
	tg[0] /= ol;
	tg[1] /= ol;
	tg[2] /= ol;
	tg[3] = tangent[3];
	if (det < MFLOAT_C(0.0)) {
		tg[3] = -tg[3];
	}
	/* Everything validated: publish the results. */
	for (k = 0; k < 3; k++) {
		out_position[k] = p[k];
		out_normal[k] = n[k];
		out_tangent[k] = tg[k];
	}
	out_tangent[3] = tg[3];
	return true;
}
