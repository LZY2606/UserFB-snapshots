/*
 * CPU 3D Gaussian splat compositing for TinyGL.
 *
 * The splats are projected from their world space mean and full 3x3
 * covariance, bucketed into a 16x16 pixel tile grid and alpha composited on
 * top of the existing 32 bit RGBA frame buffer.  The depth buffer is only
 * read for the opaque occlusion test and is never written.
 *
 * Everything here is plain C99 plus the C standard library.  See
 * doc/SPLAT.md for the data layout and the approximation limits.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "../include/GL/splat.h"

#define TGL_SPLAT_TILE_SHIFT 4
#define TGL_SPLAT_TILE_SIZE  (1 << TGL_SPLAT_TILE_SHIFT)
/* Low pass variance added to the two diagonal screen covariances, in px^2. */
#define TGL_SPLAT_LOWPASS    0.3
/* Squared Mahalanobis radius that is still drawn. */
#define TGL_SPLAT_Q_MAX      9.0
#define TGL_SPLAT_ALPHA_MAX  0.99

/* TinyGL stores z in 16 bits with 14 fractional sub bits. */
#define TGL_SPLAT_Z_BITS     ZB_Z_BITS
#define TGL_SPLAT_Z_FRAC     ZB_POINT_Z_FRAC_BITS

typedef struct {
	double dist;          /* camera distance, -mean_cam.z */
	double cx, cy;        /* screen space center, pixels */
	double cxx, cxy, cyy; /* screen covariance, pixels^2, with low pass */
	double icxx, icxy, icyy; /* inverse of the screen covariance */
	double rgb[3];
	double opacity;
	GLushort zfixed;      /* TinyGL depth buffer value for the center */
	int x0, y0, x1, y1;   /* inclusive, already clamped to the viewport */
	size_t input_index;
} ProjectedSplat;

typedef struct {
	size_t splat; /* index into the ProjectedSplat array */
	size_t order; /* original input index, used for equal distances */
	double dist;
	int tile;
} SplatRef;

static int finite_n(const double *p, size_t n) {
	size_t i;
	for (i = 0; i < n; ++i) {
		if (!isfinite(p[i])) {
			return 0;
		}
	}
	return 1;
}

/* LDL^T style positive definiteness test for a symmetric 3x3 matrix. */
static int pd3(double a00, double a01, double a02, double a11,
               double a12, double a22) {
	double d0, l10, l20, d1, l21, d2;

	d0 = a00;
	if (!(d0 > 0.0) || !isfinite(d0)) {
		return 0;
	}
	l10 = a01 / d0;
	l20 = a02 / d0;
	d1 = a11 - l10 * l10 * d0;
	if (!(d1 > 0.0) || !isfinite(d1) || !isfinite(l10) || !isfinite(l20)) {
		return 0;
	}
	l21 = (a12 - l20 * l10 * d0) / d1;
	d2 = a22 - (l20 * l20 * d0 + l21 * l21 * d1);
	if (!(d2 > 0.0) || !isfinite(d2) || !isfinite(l21)) {
		return 0;
	}
	return 1;
}

/* Inverse of a symmetric positive definite 2x2 matrix. */
static int inv2(double a, double b, double d, double inv[3]) {
	double det = a * d - b * b;
	if (!(det > 0.0) || !isfinite(det)) {
		return 0;
	}
	det = 1.0 / det;
	inv[0] = d * det;
	inv[1] = -b * det;
	inv[2] = a * det;
	return finite_n(inv, 3);
}

static int cmp_ref(const void *pa, const void *pb) {
	const SplatRef *a = (const SplatRef *)pa;
	const SplatRef *b = (const SplatRef *)pb;

	if (a->tile < b->tile) {
		return -1;
	}
	if (a->tile > b->tile) {
		return 1;
	}
	if (a->dist < b->dist) {
		return -1;
	}
	if (a->dist > b->dist) {
		return 1;
	}
	if (a->order < b->order) {
		return -1;
	}
	if (a->order > b->order) {
		return 1;
	}
	return 0;
}

static int quantize8(double v) {
	int q;
	if (v < 0.0) {
		v = 0.0;
	} else if (v > 1.0) {
		v = 1.0;
	}
	q = (int)floor(v * 255.0 + 0.5);
	if (q < 0) {
		q = 0;
	} else if (q > 255) {
		q = 255;
	}
	return q;
}

int tglRenderSplats(ZBuffer *dst, const TGLSplat *points, size_t count,
                    const TGLSplatView *view, TGLSplatStats *stats) {
	const double *V;
	const double *P;
	double vr[9];      /* view rotation, row major */
	double vt[3];      /* view translation */
	double a, b;       /* TinyGL floating point viewport scale */
	int ntx, nty, ntiles;
	long long vp_right, vp_top;
	size_t nproj = 0;
	size_t total_refs = 0;
	ProjectedSplat *proj = NULL;
	SplatRef *refs = NULL;
	double *acc_r = NULL, *acc_g = NULL, *acc_b = NULL, *transmit = NULL;
	size_t npix;
	size_t i;
	int rc = 1;

	/* ---- input validation, before any allocation that can touch pixels ---- */
	if (dst == NULL || view == NULL) {
		return 1;
	}
	if (dst->pbuf == NULL || dst->zbuf == NULL) {
		return 1;
	}
	if (count == 0) {
		if (stats != NULL) {
			stats->projected_count = 0;
			stats->tile_refs = 0;
			stats->pixel_tests = 0;
		}
		return 0;
	}
	if (points == NULL) {
		return 1;
	}
	if (view->width < 1 || view->height < 1 || view->x < 0 || view->y < 0) {
		return 1;
	}
	vp_right = (long long)view->x + (long long)view->width;
	vp_top = (long long)view->y + (long long)view->height;
	if (vp_right > (long long)dst->xsize || vp_top > (long long)dst->ysize) {
		return 1;
	}
	if (!finite_n(view->view, 16) || !finite_n(view->projection, 16)) {
		return 1;
	}

	for (i = 0; i < count; ++i) {
		const TGLSplat *s = &points[i];
		int k;
		if (!finite_n(s->mean, 3) || !finite_n(s->covariance, 6) ||
		    !finite_n(s->rgb, 3) || !isfinite(s->opacity)) {
			return 1;
		}
		for (k = 0; k < 3; ++k) {
			if (s->rgb[k] < 0.0 || s->rgb[k] > 1.0) {
				return 1;
			}
		}
		if (s->opacity < 0.0 || s->opacity > 1.0) {
			return 1;
		}
		if (!pd3(s->covariance[0], s->covariance[1], s->covariance[2],
		         s->covariance[3], s->covariance[4], s->covariance[5])) {
			return 1;
		}
	}

	/* ---- temporary storage, fully prepared before colors are modified ---- */
	if (count > SIZE_MAX / sizeof(*proj)) {
		return 1;
	}
	proj = (ProjectedSplat *)malloc(count * sizeof(*proj));
	if (proj == NULL) {
		return 1;
	}

	npix = (size_t)view->width * (size_t)view->height;
	/* calloc carries an internal overflow check for the element product */
	acc_r = (double *)calloc(npix, sizeof(double));
	acc_g = (double *)calloc(npix, sizeof(double));
	acc_b = (double *)calloc(npix, sizeof(double));
	transmit = (double *)calloc(npix, sizeof(double));
	if (acc_r == NULL || acc_g == NULL || acc_b == NULL || transmit == NULL) {
		goto fail;
	}
	for (i = 0; i < npix; ++i) {
		transmit[i] = 1.0;
	}

	V = view->view;
	P = view->projection;

	/* Column major rigid view: rotation in the upper 3x3, translation in the
	   last column. */
	{
		int r, c;
		for (c = 0; c < 3; ++c) {
			for (r = 0; r < 3; ++r) {
				vr[r * 3 + c] = V[c * 4 + r];
			}
			vt[c] = V[3 * 4 + c];
		}
	}

	a = ((double)view->width - 0.5) / 2.0;
	b = ((double)view->height - 0.5) / 2.0;

	for (i = 0; i < count; ++i) {
		const TGLSplat *in = &points[i];
		ProjectedSplat *out;
		double mc[3];
		double sc[6]; /* camera covariance: xx,xy,xz,yy,yz,zz */
		double clip[4], w, ndcx, ndcy, ndcz;
		double J[6];   /* 2x3, rows x then y */
		double t00, t01, t02, t10, t11, t12;
		double cxx, cxy, cyy, inv[3];
		double rx, ry, fx0, fy0, fx1, fy1;
		int x0, y0, x1, y1;
		int k;

		/* mean to camera space */
		for (k = 0; k < 3; ++k) {
			mc[k] = vr[k * 3 + 0] * in->mean[0] +
			        vr[k * 3 + 1] * in->mean[1] +
			        vr[k * 3 + 2] * in->mean[2] + vt[k];
		}
		if (!finite_n(mc, 3)) {
			goto fail;
		}

		/* full covariance to camera space: Sigma_c = R Sigma R^T */
		{
			/* symmetric index map: entry (r,c) -> packed xx,xy,xz,... */
			static const int sym[3][3] = {
				{0, 1, 2}, {1, 3, 4}, {2, 4, 5}};
			double rs[9]; /* R * Sigma, row major */
			int r, c, q;
			const double sx00 = in->covariance[0], sx01 = in->covariance[1],
			             sx02 = in->covariance[2], sx11 = in->covariance[3],
			             sx12 = in->covariance[4], sx22 = in->covariance[5];
			for (r = 0; r < 3; ++r) {
				double r0 = vr[r * 3 + 0];
				double r1 = vr[r * 3 + 1];
				double r2 = vr[r * 3 + 2];
				rs[r * 3 + 0] = r0 * sx00 + r1 * sx01 + r2 * sx02;
				rs[r * 3 + 1] = r0 * sx01 + r1 * sx11 + r2 * sx12;
				rs[r * 3 + 2] = r0 * sx02 + r1 * sx12 + r2 * sx22;
			}
			for (r = 0; r < 3; ++r) {
				for (c = r; c < 3; ++c) {
					double v = 0.0;
					for (q = 0; q < 3; ++q) {
						v += rs[r * 3 + q] * vr[c * 3 + q];
					}
					sc[sym[r][c]] = v;
				}
			}
		}
		if (!finite_n(sc, 6)) {
			goto fail;
		}

		/* project the camera space mean with the projection matrix */
		for (k = 0; k < 4; ++k) {
			clip[k] = P[0 * 4 + k] * mc[0] + P[1 * 4 + k] * mc[1] +
			          P[2 * 4 + k] * mc[2] + P[3 * 4 + k];
		}
		w = clip[3];
		if (!(w > 0.0) || !isfinite(w) || !finite_n(clip, 3)) {
			continue; /* center rule: positive projection w */
		}
		ndcx = clip[0] / w;
		ndcy = clip[1] / w;
		ndcz = clip[2] / w;
		if (!isfinite(ndcx) || !isfinite(ndcy) || !isfinite(ndcz)) {
			goto fail;
		}
		if (!(ndcz > -1.0 && ndcz < 1.0)) {
			continue; /* center rule: strict NDC z range */
		}

		/* Jacobian of the divide-by-w screen mapping wrt camera position */
		{
			double w2 = w * w;
			for (k = 0; k < 3; ++k) {
				double px = P[k * 4 + 0];
				double py = P[k * 4 + 1];
				double pw = P[k * 4 + 3];
				J[0 * 3 + k] = a * (px * w - clip[0] * pw) / w2;
				J[1 * 3 + k] = -b * (py * w - clip[1] * pw) / w2;
			}
		}
		if (!finite_n(J, 6)) {
			goto fail;
		}

		/* T = J Sigma_c ; C = T J^T */
		t00 = J[0] * sc[0] + J[1] * sc[1] + J[2] * sc[2];
		t01 = J[0] * sc[1] + J[1] * sc[3] + J[2] * sc[4];
		t02 = J[0] * sc[2] + J[1] * sc[4] + J[2] * sc[5];
		t10 = J[3] * sc[0] + J[4] * sc[1] + J[5] * sc[2];
		t11 = J[3] * sc[1] + J[4] * sc[3] + J[5] * sc[4];
		t12 = J[3] * sc[2] + J[4] * sc[4] + J[5] * sc[5];
		cxx = t00 * J[0] + t01 * J[1] + t02 * J[2];
		cxy = t00 * J[3] + t01 * J[4] + t02 * J[5];
		cyy = t10 * J[3] + t11 * J[4] + t12 * J[5];
		/* low pass anti-aliasing variance on the diagonal only */
		cxx += TGL_SPLAT_LOWPASS;
		cyy += TGL_SPLAT_LOWPASS;
		if (!isfinite(cxx) || !isfinite(cxy) || !isfinite(cyy)) {
			goto fail;
		}
		if (!inv2(cxx, cxy, cyy, inv)) {
			goto fail;
		}

		out = &proj[nproj];
		out->dist = -mc[2];
		out->cx = (double)view->x + a * (ndcx + 1.0);
		out->cy = (double)view->y + b * (1.0 - ndcy);
		out->cxx = cxx;
		out->cxy = cxy;
		out->cyy = cyy;
		out->icxx = inv[0];
		out->icxy = inv[1];
		out->icyy = inv[2];
		for (k = 0; k < 3; ++k) {
			out->rgb[k] = in->rgb[k];
		}
		out->opacity = in->opacity;
		out->input_index = i;

		/* TinyGL fixed point depth, same mapping as the triangle pipeline */
		{
			double zsize = (double)(1 << (TGL_SPLAT_Z_BITS + TGL_SPLAT_Z_FRAC));
			double scale_z = -(zsize - 0.5) / 2.0;
			double trans_z = (zsize - 0.5) / 2.0 +
			                 (double)(1 << TGL_SPLAT_Z_FRAC) / 2.0;
			double zfull = ndcz * scale_z + trans_z;
			double zval = floor(zfull / (double)(1 << TGL_SPLAT_Z_FRAC));
			if (!isfinite(zval) || zval < 0.0 || zval > 65535.0) {
				goto fail;
			}
			out->zfixed = (GLushort)zval;
		}

		/* axis aligned 3-sigma pixel bounds, pixel centers at (x+.5,y+.5) */
		rx = 3.0 * sqrt(cxx);
		ry = 3.0 * sqrt(cyy);
		fx0 = ceil(out->cx - rx - 0.5);
		fx1 = floor(out->cx + rx - 0.5);
		fy0 = ceil(out->cy - ry - 0.5);
		fy1 = floor(out->cy + ry - 0.5);
		if (!isfinite(fx0) || !isfinite(fx1) || !isfinite(fy0) ||
		    !isfinite(fy1)) {
			goto fail;
		}
		if (fx0 < (double)view->x) {
			fx0 = (double)view->x;
		}
		if (fx1 > (double)(view->x + view->width - 1)) {
			fx1 = (double)(view->x + view->width - 1);
		}
		if (fy0 < (double)view->y) {
			fy0 = (double)view->y;
		}
		if (fy1 > (double)(view->y + view->height - 1)) {
			fy1 = (double)(view->y + view->height - 1);
		}
		x0 = (int)fx0;
		x1 = (int)fx1;
		y0 = (int)fy0;
		y1 = (int)fy1;
		out->x0 = x0;
		out->x1 = x1;
		out->y0 = y0;
		out->y1 = y1;

		++nproj;
	}

	/* ---- build the tile references ---------------------------------------- */
	ntx = (view->width + TGL_SPLAT_TILE_SIZE - 1) / TGL_SPLAT_TILE_SIZE;
	nty = (view->height + TGL_SPLAT_TILE_SIZE - 1) / TGL_SPLAT_TILE_SIZE;
	ntiles = ntx * nty;

	for (i = 0; i < nproj; ++i) {
		const ProjectedSplat *s = &proj[i];
		int ti0, ti1, tj0, tj1, ti, tj;
		if (s->x0 > s->x1 || s->y0 > s->y1) {
			continue; /* tail does not reach the viewport at all */
		}
		ti0 = (s->x0 - view->x) >> TGL_SPLAT_TILE_SHIFT;
		ti1 = (s->x1 - view->x) >> TGL_SPLAT_TILE_SHIFT;
		tj0 = (s->y0 - view->y) >> TGL_SPLAT_TILE_SHIFT;
		tj1 = (s->y1 - view->y) >> TGL_SPLAT_TILE_SHIFT;
		for (tj = tj0; tj <= tj1; ++tj) {
			for (ti = ti0; ti <= ti1; ++ti) {
				++total_refs;
			}
		}
	}

	if (total_refs > 0) {
		if (total_refs > SIZE_MAX / sizeof(*refs)) {
			goto fail;
		}
		refs = (SplatRef *)malloc(total_refs * sizeof(*refs));
		if (refs == NULL) {
			goto fail;
		}
		{
			size_t ri = 0;
			for (i = 0; i < nproj; ++i) {
				const ProjectedSplat *s = &proj[i];
				int ti0, ti1, tj0, tj1, ti, tj;
				if (s->x0 > s->x1 || s->y0 > s->y1) {
					continue;
				}
				ti0 = (s->x0 - view->x) >> TGL_SPLAT_TILE_SHIFT;
				ti1 = (s->x1 - view->x) >> TGL_SPLAT_TILE_SHIFT;
				tj0 = (s->y0 - view->y) >> TGL_SPLAT_TILE_SHIFT;
				tj1 = (s->y1 - view->y) >> TGL_SPLAT_TILE_SHIFT;
				for (tj = tj0; tj <= tj1; ++tj) {
					for (ti = ti0; ti <= ti1; ++ti) {
						refs[ri].splat = i;
						refs[ri].order = s->input_index;
						refs[ri].dist = s->dist;
						refs[ri].tile = tj * ntx + ti;
						++ri;
					}
				}
			}
		}
		qsort(refs, total_refs, sizeof(*refs), cmp_ref);
	}

	/* ---- composite, near to far, only touching covered pixels ------------- */
	{
		size_t pixel_tests = 0;
		size_t ri = 0;
		int tile;
		for (tile = 0; tile < ntiles; ++tile) {
			size_t rs = ri;
			int ti = tile % ntx;
			int tj = tile / ntx;
			int txl = view->x + ti * TGL_SPLAT_TILE_SIZE;
			int tyl = view->y + tj * TGL_SPLAT_TILE_SIZE;
			int txr = txl + TGL_SPLAT_TILE_SIZE - 1;
			int tyr = tyl + TGL_SPLAT_TILE_SIZE - 1;
			size_t rr;
			if (txr > view->x + view->width - 1) {
				txr = view->x + view->width - 1;
			}
			if (tyr > view->y + view->height - 1) {
				tyr = view->y + view->height - 1;
			}
			while (ri < total_refs && refs[ri].tile == tile) {
				++ri;
			}
			for (rr = rs; rr < ri; ++rr) {
				const ProjectedSplat *s = &proj[refs[rr].splat];
				int px0 = s->x0 > txl ? s->x0 : txl;
				int px1 = s->x1 < txr ? s->x1 : txr;
				int py0 = s->y0 > tyl ? s->y0 : tyl;
				int py1 = s->y1 < tyr ? s->y1 : tyr;
				int px, py;
				if (px0 > px1 || py0 > py1) {
					continue;
				}
				for (py = py0; py <= py1; ++py) {
					for (px = px0; px <= px1; ++px) {
						double dx = (px + 0.5) - s->cx;
						double dy = (py + 0.5) - s->cy;
						double q = s->icxx * dx * dx +
						           2.0 * s->icxy * dx * dy +
						           s->icyy * dy * dy;
						size_t local = (size_t)(py - view->y) *
						                   (size_t)view->width +
						               (size_t)(px - view->x);
						++pixel_tests;
						if (q > TGL_SPLAT_Q_MAX) {
							continue;
						}
						/* center-depth occlusion against opaque geometry */
						if (s->zfixed <
						    dst->zbuf[(size_t)py * (size_t)dst->xsize +
						              (size_t)px]) {
							continue;
						}
						{
							double alpha = s->opacity * exp(-0.5 * q);
							double t;
							if (alpha > TGL_SPLAT_ALPHA_MAX) {
								alpha = TGL_SPLAT_ALPHA_MAX;
							}
							if (!(alpha > 0.0)) {
								continue;
							}
							t = transmit[local];
							acc_r[local] += t * alpha * s->rgb[0];
							acc_g[local] += t * alpha * s->rgb[1];
							acc_b[local] += t * alpha * s->rgb[2];
							transmit[local] = t * (1.0 - alpha);
						}
					}
				}
			}
		}

		/* ---- finalize once per pixel; layers never quantized -------------- */
		{
			int py, px;
			for (py = 0; py < view->height; ++py) {
				for (px = 0; px < view->width; ++px) {
					size_t local = (size_t)py * (size_t)view->width +
					               (size_t)px;
					int fx = view->x + px;
					int fy = view->y + py;
					size_t bi = (size_t)fy * (size_t)dst->xsize +
					            (size_t)fx;
					PIXEL old = dst->pbuf[bi];
					double t = transmit[local];
					double rr = acc_r[local] + t * ((GET_RED(old)) / 255.0);
					double gg = acc_g[local] + t * ((GET_GREEN(old)) / 255.0);
					double bb = acc_b[local] + t * ((GET_BLUE(old)) / 255.0);
					PIXEL packed = (PIXEL)(((quantize8(rr) << 16) |
					                        (quantize8(gg) << 8) |
					                        quantize8(bb)) &
					                       COLOR_MASK);
					dst->pbuf[bi] = (old & (PIXEL)0xff000000u) | packed;
				}
			}
		}

		if (stats != NULL) {
			stats->projected_count = nproj;
			stats->tile_refs = total_refs;
			stats->pixel_tests = pixel_tests;
		}
	}

	rc = 0;
fail:
	free(transmit);
	free(acc_b);
	free(acc_g);
	free(acc_r);
	free(refs);
	free(proj);
	return rc;
}
