/*
 * CPU 3D Gaussian splat renderer for TinyGL.
 *
 * The splats are transformed and projected entirely on the CPU in double
 * precision, bucketed into 16x16 pixel tiles and alpha composited on top of
 * the existing opaque frame buffer. The depth buffer is read only.
 *
 * The math (EWA projection, centre rule, tile bounds and compositing) is
 * documented in doc/SPLAT.md.
 */

#include "GL/splat.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if TGL_FEATURE_RENDER_BITS != 32
#error "tglRenderSplats requires the 32 bit RGBA build"
#endif

#define TILE_SHIFT 4
#define TILE_SIZE (1 << TILE_SHIFT) /* 16 pixels per tile side */
#define LOWPASS_VAR 0.3            /* pixels^2 added to both diagonal entries */
#define Q_MAX 9.0                  /* Mahalanobis radius, 3 sigma ellipse */
#define ALPHA_MAX 0.99

/* World covariance accessor for the packed upper triangle
 * {xx, xy, xz, yy, yz, zz}. */
static double wcov(const double c[6], int i, int j) {
	if (i > j) {
		int t = i;
		i = j;
		j = t;
	}
	/* (0,0)=0 (0,1)=1 (0,2)=2 (1,1)=3 (1,2)=4 (2,2)=5 */
	static const unsigned char idx[3][3] = {
	    {0, 1, 2}, {1, 3, 4}, {2, 4, 5}};
	return c[idx[i][j]];
}

/* Strict positive definiteness through the leading principal minors.
 * NaN fails every comparison, so non finite data is rejected as well. */
static int cov_positive_definite(const double c[6]) {
	double m1 = c[0];
	double m2 = c[0] * c[3] - c[1] * c[1];
	double det = c[0] * (c[3] * c[5] - c[4] * c[4])
	           - c[1] * (c[1] * c[5] - c[4] * c[2])
	           + c[2] * (c[1] * c[4] - c[3] * c[2]);
	return (m1 > 0.0) && (m2 > 0.0) && (det > 0.0);
}

static int validate_inputs(ZBuffer *dst, const TGLSplat *points, size_t count,
                           const TGLSplatView *view) {
	size_t k;
	int i;
	long long right, top;

	if (dst == NULL || view == NULL)
		return -1;
	if (dst->pbuf == NULL || dst->zbuf == NULL)
		return -1;
	if (dst->xsize <= 0 || dst->ysize <= 0)
		return -1;
	if (count > 0 && points == NULL)
		return -1;
	if (view->width <= 0 || view->height <= 0)
		return -1;
	if (view->x < 0 || view->y < 0)
		return -1;
	right = (long long)view->x + (long long)view->width;
	top = (long long)view->y + (long long)view->height;
	if (right > (long long)dst->xsize || top > (long long)dst->ysize)
		return -1;

	for (i = 0; i < 16; i++) {
		if (!isfinite(view->view[i]) || !isfinite(view->projection[i]))
			return -1;
	}

	for (k = 0; k < count; k++) {
		const TGLSplat *s = &points[k];
		for (i = 0; i < 3; i++) {
			if (!isfinite(s->mean[i]) || !isfinite(s->rgb[i]))
				return -1;
			if (s->rgb[i] < 0.0 || s->rgb[i] > 1.0)
				return -1;
		}
		for (i = 0; i < 6; i++) {
			if (!isfinite(s->covariance[i]))
				return -1;
		}
		if (!isfinite(s->opacity) || s->opacity < 0.0 || s->opacity > 1.0)
			return -1;
		if (!cov_positive_definite(s->covariance))
			return -1;
	}
	return 0;
}

typedef struct ProjectedSplat {
	double cx, cy;             /* screen space centre, pixels */
	double inv00, inv01, inv11;/* inverse of the 2x2 screen covariance */
	double r, g, b;
	double opacity;
	double dist;               /* camera distance, -mean_camera.z */
	unsigned short z;          /* TinyGL fixed point depth for the centre */
	int bx0, bx1, by0, by1;    /* pixel bounds clipped to the viewport */
	int index;                 /* original input index, tie breaker */
} ProjectedSplat;

static int cmp_projected(const void *pa, const void *pb) {
	const ProjectedSplat *a = (const ProjectedSplat *)pa;
	const ProjectedSplat *b = (const ProjectedSplat *)pb;
	if (a->dist < b->dist)
		return -1;
	if (a->dist > b->dist)
		return 1;
	if (a->index < b->index)
		return -1;
	if (a->index > b->index)
		return 1;
	return 0;
}

/* Reference inside one tile: rank is the global back to front position. */
typedef struct TileRef {
	int rank;
	int p;
} TileRef;

static int cmp_tile_ref(const void *pa, const void *pb) {
	const TileRef *a = (const TileRef *)pa;
	const TileRef *b = (const TileRef *)pb;
	if (a->rank < b->rank)
		return -1;
	if (a->rank > b->rank)
		return 1;
	return 0;
}

/*
 * Transform one splat into camera space, project it and fill *out.
 * Returns 1 when the splat passes the centre rule, 0 when it is culled,
 * and -1 on a non finite intermediate (a hard preparation failure).
 */
static int project_splat(const TGLSplat *s, const TGLSplatView *view,
                         ProjectedSplat *out) {
	const double *V = view->view;
	const double *P = view->projection;
	double mc[3];
	double sc[3][3];
	double R[3][3];
	double clip[4];
	double w, ndcx, ndcy, ndcz;
	double a, b, X, Y;
	double J[2][3];
	double C00, C01, C11, det;
	double scale_z, trans_z, z_full;
	int i, j, k, l;

	/* Rotation part of the rigid world-to-camera transform. */
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			R[i][j] = V[4 * j + i];

	/* Camera space mean. */
	for (i = 0; i < 3; i++) {
		mc[i] = V[4 * 3 + i]; /* translation column */
		for (j = 0; j < 3; j++)
			mc[i] += R[i][j] * s->mean[j];
	}

	/* Camera space covariance Sc = R S R^T. */
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			double sum = 0.0;
			for (k = 0; k < 3; k++)
				for (l = 0; l < 3; l++)
					sum += R[i][k] * wcov(s->covariance, k, l) * R[j][l];
			sc[i][j] = sum;
		}
	}

	/* Clip space: clip = P * (mc, 1). P is column major. */
	for (i = 0; i < 4; i++) {
		clip[i] = P[4 * 3 + i];
		for (j = 0; j < 3; j++)
			clip[i] += P[4 * j + i] * mc[j];
	}
	w = clip[3];
	if (!(w > 0.0))
		return 0;
	ndcx = clip[0] / w;
	ndcy = clip[1] / w;
	ndcz = clip[2] / w;
	if (!isfinite(ndcx) || !isfinite(ndcy) || !isfinite(ndcz))
		return -1;
	/* Centre rule: NDC z strictly inside (-1, 1). */
	if (!(ndcz > -1.0 && ndcz < 1.0))
		return 0;

	/* TinyGL floating point viewport mapping. */
	a = ((double)view->width - 0.5) * 0.5;
	b = ((double)view->height - 0.5) * 0.5;
	X = (double)view->x + a * (ndcx + 1.0);
	Y = (double)view->y + b * (1.0 - ndcy);

	/*
	 * Jacobian of the divide-by-w viewport map with respect to the
	 * camera position. fx = clip0/w, fy = clip1/w,
	 * d(fx)/d mc[k] = (p0k*w - clip0*p3k) / w^2.
	 */
	{
		double w2 = w * w;
		for (k = 0; k < 3; k++) {
			double dfx = (P[4 * k + 0] * w - clip[0] * P[4 * k + 3]) / w2;
			double dfy = (P[4 * k + 1] * w - clip[1] * P[4 * k + 3]) / w2;
			J[0][k] = a * dfx;
			J[1][k] = -b * dfy;
		}
	}

	/* Screen covariance C = J Sc J^T. */
	C00 = C01 = C11 = 0.0;
	for (k = 0; k < 3; k++) {
		for (l = 0; l < 3; l++) {
			C00 += J[0][k] * sc[k][l] * J[0][l];
			C01 += J[0][k] * sc[k][l] * J[1][l];
			C11 += J[1][k] * sc[k][l] * J[1][l];
		}
	}
	/* Low pass floor, keeps the diagonal strictly positive. */
	C00 += LOWPASS_VAR;
	C11 += LOWPASS_VAR;
	det = C00 * C11 - C01 * C01;
	if (!isfinite(C00) || !isfinite(C01) || !isfinite(C11) ||
	    !isfinite(X) || !isfinite(Y) || !(det > 0.0))
		return -1;

	out->cx = X;
	out->cy = Y;
	out->inv00 = C11 / det;
	out->inv01 = -C01 / det;
	out->inv11 = C00 / det;
	if (!isfinite(out->inv00) || !isfinite(out->inv01) ||
	    !isfinite(out->inv11))
		return -1;

	out->r = s->rgb[0];
	out->g = s->rgb[1];
	out->b = s->rgb[2];
	out->opacity = s->opacity;
	out->dist = -mc[2];
	if (!isfinite(out->dist))
		return -1;

	/*
	 * TinyGL viewport depth: zsize = 2^(16+14), the stored value is the
	 * 30 bit fixed point depth shifted right by ZB_POINT_Z_FRAC_BITS.
	 * Centre NDC z in (-1, 1) keeps this value positive.
	 */
	{
		const double zsize = (double)(1L << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
		scale_z = -((zsize - 0.5) * 0.5);
		trans_z = (zsize - 0.5) * 0.5
		        + (double)(1 << ZB_POINT_Z_FRAC_BITS) * 0.5;
		z_full = ndcz * scale_z + trans_z;
		if (!isfinite(z_full) || z_full < 0.0)
			return -1;
		out->z = (unsigned short)((long long)z_full >> ZB_POINT_Z_FRAC_BITS);
	}

	/* Axis aligned 3 sigma pixel bounds, clipped to the viewport. A
	 * pixel px has its centre at px + 0.5, hence the -0.5 offsets. */
	{
		double radx = 3.0 * sqrt(C00);
		double rady = 3.0 * sqrt(C11);
		double x0d = ceil(X - radx - 0.5);
		double x1d = floor(X + radx - 0.5);
		double y0d = ceil(Y - rady - 0.5);
		double y1d = floor(Y + rady - 0.5);
		int vx1 = view->x + view->width - 1;
		int vy1 = view->y + view->height - 1;

		if (x0d < view->x)
			x0d = view->x;
		if (x1d > vx1)
			x1d = vx1;
		if (y0d < view->y)
			y0d = view->y;
		if (y1d > vy1)
			y1d = vy1;
		out->bx0 = (int)x0d;
		out->bx1 = (int)x1d;
		out->by0 = (int)y0d;
		out->by1 = (int)y1d;
	}
	return 1;
}

int tglRenderSplats(ZBuffer *dst, const TGLSplat *points, size_t count,
                    const TGLSplatView *view, TGLSplatStats *stats) {
	ProjectedSplat *proj = NULL;
	size_t proj_count = 0;
	size_t ntiles, nrefs = 0;
	size_t *tile_count = NULL, *tile_offset = NULL, *tile_cursor = NULL;
	TileRef *refs = NULL;
	int tiles_x, tiles_y;
	size_t i, t;
	int fail = 0;

	if (stats != NULL) {
		stats->projected_count = 0;
		stats->tile_refs = 0;
		stats->pixel_tests = 0;
	}

	if (validate_inputs(dst, points, count, view) != 0)
		return -1;

	if (count == 0)
		return 0;

	if (count > SIZE_MAX / sizeof(*proj))
		return -1;
	if (count > (size_t)INT_MAX)
		return -1;
	proj = (ProjectedSplat *)malloc(count * sizeof(*proj));
	if (proj == NULL)
		return -1;

	/* ---- Preparation: validate every splat and project it. No pixel is
	 * touched in this phase, so a failure leaves the target untouched. ---- */
	for (i = 0; i < count; i++) {
		int rc = project_splat(&points[i], view, &proj[proj_count]);
		if (rc < 0) {
			fail = 1;
			break;
		}
		if (rc == 0)
			continue;
		proj[proj_count].index = (int)i;
		proj_count++;
	}
	if (fail)
		goto cleanup;

	/* Back to front: nearest first, input index breaks distance ties. */
	qsort(proj, proj_count, sizeof(*proj), cmp_projected);
	for (i = 0; i < proj_count; i++) {
		if (!isfinite(proj[i].dist) || !isfinite(proj[i].cx) ||
		    !isfinite(proj[i].cy)) {
			fail = 1;
			break;
		}
	}
	if (fail)
		goto cleanup;

	tiles_x = (view->width + TILE_SIZE - 1) / TILE_SIZE;
	tiles_y = (view->height + TILE_SIZE - 1) / TILE_SIZE;
	if (tiles_x <= 0 || tiles_y <= 0 ||
	    (size_t)tiles_x > SIZE_MAX / (size_t)tiles_y) {
		fail = 1;
		goto cleanup;
	}
	ntiles = (size_t)tiles_x * (size_t)tiles_y;
	if (ntiles > SIZE_MAX / sizeof(*tile_count)) {
		fail = 1;
		goto cleanup;
	}
	tile_count = (size_t *)calloc(ntiles, sizeof(*tile_count));
	tile_offset = (size_t *)malloc(ntiles * sizeof(*tile_offset));
	tile_cursor = (size_t *)malloc(ntiles * sizeof(*tile_cursor));
	if (tile_count == NULL || tile_offset == NULL || tile_cursor == NULL) {
		fail = 1;
		goto cleanup;
	}

	/* First pass: count the (point, tile) references. */
	for (i = 0; i < proj_count; i++) {
		ProjectedSplat *p = &proj[i];
		int tx0, tx1, ty0, ty1, tx, ty;
		if (p->bx0 > p->bx1 || p->by0 > p->by1)
			continue; /* ellipse does not reach the viewport */
		tx0 = (p->bx0 - view->x) >> TILE_SHIFT;
		tx1 = (p->bx1 - view->x) >> TILE_SHIFT;
		ty0 = (p->by0 - view->y) >> TILE_SHIFT;
		ty1 = (p->by1 - view->y) >> TILE_SHIFT;
		for (ty = ty0; ty <= ty1; ty++) {
			for (tx = tx0; tx <= tx1; tx++)
				tile_count[(size_t)ty * tiles_x + tx]++;
		}
	}
	for (t = 0; t < ntiles; t++) {
		tile_offset[t] = nrefs;
		nrefs += tile_count[t];
		if (nrefs < tile_offset[t]) { /* size_t overflow */
			fail = 1;
			goto cleanup;
		}
	}
	if (nrefs > SIZE_MAX / sizeof(*refs)) {
		fail = 1;
		goto cleanup;
	}
	if (nrefs > 0) {
		refs = (TileRef *)malloc(nrefs * sizeof(*refs));
		if (refs == NULL) {
			fail = 1;
			goto cleanup;
		}
	}
	memcpy(tile_cursor, tile_offset, ntiles * sizeof(*tile_cursor));

	/* Second pass: fill references in global back-to-front order. */
	for (i = 0; i < proj_count; i++) {
		ProjectedSplat *p = &proj[i];
		int tx0, tx1, ty0, ty1, tx, ty;
		if (p->bx0 > p->bx1 || p->by0 > p->by1)
			continue;
		tx0 = (p->bx0 - view->x) >> TILE_SHIFT;
		tx1 = (p->bx1 - view->x) >> TILE_SHIFT;
		ty0 = (p->by0 - view->y) >> TILE_SHIFT;
		ty1 = (p->by1 - view->y) >> TILE_SHIFT;
		for (ty = ty0; ty <= ty1; ty++) {
			for (tx = tx0; tx <= tx1; tx++) {
				size_t cell = (size_t)ty * tiles_x + tx;
				refs[tile_cursor[cell]].rank = (int)i;
				refs[tile_cursor[cell]].p = (int)i;
				tile_cursor[cell]++;
			}
		}
	}

	/* ---- All preparation succeeded; from here on only pbuf is written,
	 * zbuf is never modified. Per tile 16x16 accumulation keeps the
	 * back-to-front order and the floating point math exact per pixel. ---- */
	{
		double sum[TILE_SIZE * TILE_SIZE][3];
		double trans[TILE_SIZE * TILE_SIZE];
		size_t pixel_tests = 0;
		int tx, ty;

		for (ty = 0; ty < tiles_y; ty++) {
			for (tx = 0; tx < tiles_x; tx++) {
				size_t cell = (size_t)ty * tiles_x + tx;
				size_t start = tile_offset[cell];
				size_t end = start + tile_count[cell];
				int px0 = view->x + tx * TILE_SIZE;
				int py0 = view->y + ty * TILE_SIZE;
				int px1 = px0 + TILE_SIZE - 1;
				int py1 = py0 + TILE_SIZE - 1;
				size_t r;
				int lx, ly;

				if (px1 > view->x + view->width - 1)
					px1 = view->x + view->width - 1;
				if (py1 > view->y + view->height - 1)
					py1 = view->y + view->height - 1;

				for (ly = 0; ly < TILE_SIZE; ly++)
					for (lx = 0; lx < TILE_SIZE; lx++) {
						sum[ly * TILE_SIZE + lx][0] = 0.0;
						sum[ly * TILE_SIZE + lx][1] = 0.0;
						sum[ly * TILE_SIZE + lx][2] = 0.0;
						trans[ly * TILE_SIZE + lx] = 1.0;
					}

				qsort(&refs[start], end - start, sizeof(*refs), cmp_tile_ref);

				for (r = start; r < end; r++) {
					const ProjectedSplat *p = &proj[refs[r].p];
					int cx0 = p->bx0 > px0 ? p->bx0 : px0;
					int cx1 = p->bx1 < px1 ? p->bx1 : px1;
					int cy0 = p->by0 > py0 ? p->by0 : py0;
					int cy1 = p->by1 < py1 ? p->by1 : py1;
					int px, py;

					for (py = cy0; py <= cy1; py++) {
						for (px = cx0; px <= cx1; px++) {
							double dx = (px + 0.5) - p->cx;
							double dy = (py + 0.5) - p->cy;
							double q = dx * (p->inv00 * dx + p->inv01 * dy)
							         + dy * (p->inv01 * dx + p->inv11 * dy);
							double alpha;
							int lx = px - px0;
							int ly = py - py0;
							int slot = ly * TILE_SIZE + lx;
							size_t pix;

							pixel_tests++;
							if (!(q <= Q_MAX))
								continue;
							alpha = p->opacity * exp(-0.5 * q);
							if (alpha > ALPHA_MAX)
								alpha = ALPHA_MAX;
							if (!(alpha > 0.0))
								continue;

							pix = (size_t)py * (size_t)dst->xsize + px;
							/* Same >= convention as the triangle rasteriser:
							 * when depth_test is off every splat passes. */
							if (dst->depth_test && p->z < dst->zbuf[pix])
								continue;

							sum[slot][0] += trans[slot] * alpha * p->r;
							sum[slot][1] += trans[slot] * alpha * p->g;
							sum[slot][2] += trans[slot] * alpha * p->b;
							trans[slot] *= 1.0 - alpha;
						}
					}
				}

				/* One final composite per touched pixel; high 8 bits kept. */
				{
				int px, py;
				for (py = py0; py <= py1; py++) {
					for (px = px0; px <= px1; px++) {
						int lx = px - px0;
						int ly = py - py0;
						int slot = ly * TILE_SIZE + lx;
						double T = trans[slot];
						PIXEL old, out;
						size_t pix;
						int ch;
						double finals[3];
						if (T >= 1.0)
							continue;
						pix = (size_t)py * (size_t)dst->xsize + px;
						old = dst->pbuf[pix];
						{
							double orig[3];
							orig[0] = (double)GET_RED(old) / 255.0;
							orig[1] = (double)GET_GREEN(old) / 255.0;
							orig[2] = (double)GET_BLUE(old) / 255.0;
							for (ch = 0; ch < 3; ch++) {
								double v = sum[slot][ch] + T * orig[ch];
								if (v < 0.0)
									v = 0.0;
								else if (v > 1.0)
									v = 1.0;
								finals[ch] = floor(255.0 * v + 0.5);
							}
						}
						out = (old & 0xFF000000u)
						    | ((PIXEL)(unsigned)finals[0] << 16)
						    | ((PIXEL)(unsigned)finals[1] << 8)
						    | ((PIXEL)(unsigned)finals[2]);
						dst->pbuf[pix] = out;
					}
				}
				}
			}
		}

		if (stats != NULL) {
			stats->projected_count = proj_count;
			stats->tile_refs = nrefs;
			stats->pixel_tests = pixel_tests;
		}
	}

cleanup:
	free(refs);
	free(tile_cursor);
	free(tile_offset);
	free(tile_count);
	free(proj);
	return fail ? -1 : 0;
}
