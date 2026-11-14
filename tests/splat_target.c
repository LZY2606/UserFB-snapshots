/*
 * Independent tests for the CPU Gaussian splat entry point.
 *
 * The reference renderer in this file is self contained: it re-derives every
 * projection with its own double precision matrices and a finite difference
 * Jacobian, builds its own tile buckets and does its own float compositing.
 * It never calls tglRenderSplats() or any projection helper from src/splat.c.
 * The production entry point is only driven through its public API and then
 * compared against this reference.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"
#include "GL/gl.h"
#include "GL/splat.h"

#define TILE 16
#define LOWPASS 0.3
#define QMAX 9.0

/* ------------------------------------------------------------------ matrices */

static void mat_identity(double m[16]) {
	int i;
	for (i = 0; i < 16; ++i) {
		m[i] = 0.0;
	}
	m[0] = m[5] = m[10] = m[15] = 1.0;
}

/* column major glFrustum, NDC z in [-1,1] */
static void mat_frustum(double m[16], double l, double r, double b, double t,
                        double n, double f) {
	mat_identity(m);
	m[0] = (2.0 * n) / (r - l);
	m[5] = (2.0 * n) / (t - b);
	m[8] = (r + l) / (r - l);   /* M20 */
	m[9] = (t + b) / (t - b);   /* M21 */
	m[10] = -(f + n) / (f - n); /* M22 = C */
	m[11] = -1.0;               /* M32 */
	m[14] = -(2.0 * f * n) / (f - n); /* M23 = D */
	m[15] = 0.0;
}

/* column major glOrtho */
static void mat_ortho(double m[16], double l, double r, double b, double t,
                      double n, double f) {
	mat_identity(m);
	m[0] = 2.0 / (r - l);
	m[5] = 2.0 / (t - b);
	m[10] = -2.0 / (f - n);
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[14] = -(f + n) / (f - n);
	m[15] = 1.0;
}

/* axis angle rotation, row major 3x3 */
static void rot_axis_angle(double R[9], double ax, double ay, double az,
                           double ang) {
	double c = cos(ang), s = sin(ang), C = 1.0 - c;
	double n = sqrt(ax * ax + ay * ay + az * az);
	ax /= n;
	ay /= n;
	az /= n;
	R[0] = c + ax * ax * C;
	R[1] = ax * ay * C - az * s;
	R[2] = ax * az * C + ay * s;
	R[3] = ay * ax * C + az * s;
	R[4] = c + ay * ay * C;
	R[5] = ay * az * C - ax * s;
	R[6] = az * ax * C - ay * s;
	R[7] = az * ay * C + ax * s;
	R[8] = c + az * az * C;
}

/* rigid world-to-camera view from a row major rotation and translation */
static void view_from(double v[16], const double R[9], const double t[3]) {
	int row, col;
	mat_identity(v);
	for (col = 0; col < 3; ++col) {
		for (row = 0; row < 3; ++row) {
			v[col * 4 + row] = R[row * 3 + col];
		}
	}
	v[12] = t[0];
	v[13] = t[1];
	v[14] = t[2];
}

static void make_view(TGLSplatView *view, const double v[16],
                      const double p[16], int x, int y, int w, int h) {
	memcpy(view->view, v, sizeof(view->view));
	memcpy(view->projection, p, sizeof(view->projection));
	view->x = x;
	view->y = y;
	view->width = w;
	view->height = h;
}

static void make_splat(TGLSplat *s, double mx, double my, double mz,
                       double c00, double c01, double c02, double c11,
                       double c12, double c22, double r, double g, double bl,
                       double op) {
	s->mean[0] = mx;
	s->mean[1] = my;
	s->mean[2] = mz;
	s->covariance[0] = c00;
	s->covariance[1] = c01;
	s->covariance[2] = c02;
	s->covariance[3] = c11;
	s->covariance[4] = c12;
	s->covariance[5] = c22;
	s->rgb[0] = r;
	s->rgb[1] = g;
	s->rgb[2] = bl;
	s->opacity = op;
}

/* ------------------------------------------------------------- reference math */

typedef struct {
	int valid;
	double cx, cy;
	double cxx, cxy, cyy;
	double icxx, icxy, icyy;
	double jfd[6];   /* finite difference Jacobian */
	double jan[6];   /* closed form Jacobian, cross checked against jfd */
	double dist;
	unsigned short zfixed;
	int x0, y0, x1, y1;
	size_t order;
	double rgb[3];
	double opacity;
} RefProj;

static void screen_point(const double P[16], const double mc[3], double vx,
                         double vy, double a, double b, double out[2]) {
	double clip[4], w;
	int k;
	for (k = 0; k < 4; ++k) {
		clip[k] = P[0 * 4 + k] * mc[0] + P[1 * 4 + k] * mc[1] +
		          P[2 * 4 + k] * mc[2] + P[3 * 4 + k];
	}
	w = clip[3];
	out[0] = vx + a * (clip[0] / w + 1.0);
	out[1] = vy + b * (1.0 - clip[1] / w);
}

/* finite difference 2x3 Jacobian of the divide-by-w screen mapping */
static void jacobian_fd(const double P[16], const double mc[3], double vx,
                        double vy, double a, double b, double J[6]) {
	double pp[3], fp[2], fm[2];
	int k, row;
	for (k = 0; k < 3; ++k) {
		double h = 1e-6 * (fabs(mc[k]) > 1.0 ? fabs(mc[k]) : 1.0);
		for (row = 0; row < 3; ++row) {
			pp[row] = mc[row];
		}
		pp[k] += h;
		screen_point(P, pp, vx, vy, a, b, fp);
		pp[k] -= 2.0 * h;
		screen_point(P, pp, vx, vy, a, b, fm);
		J[0 * 3 + k] = (fp[0] - fm[0]) / (2.0 * h);
		J[1 * 3 + k] = (fp[1] - fm[1]) / (2.0 * h);
	}
}

/* closed form Jacobian, the same formula family as the production code */
static void jacobian_analytic(const double P[16], const double mc[3], double a,
                              double b, double J[6]) {
	double clip[4], w, w2;
	int k;
	for (k = 0; k < 4; ++k) {
		clip[k] = P[0 * 4 + k] * mc[0] + P[1 * 4 + k] * mc[1] +
		          P[2 * 4 + k] * mc[2] + P[3 * 4 + k];
	}
	w = clip[3];
	w2 = w * w;
	for (k = 0; k < 3; ++k) {
		double px = P[k * 4 + 0];
		double py = P[k * 4 + 1];
		double pw = P[k * 4 + 3];
		J[0 * 3 + k] = a * (px * w - clip[0] * pw) / w2;
		J[1 * 3 + k] = -b * (py * w - clip[1] * pw) / w2;
	}
}

static unsigned short depth_fixed(double ndcz) {
	double zsize = (double)(1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
	double scale_z = -(zsize - 0.5) / 2.0;
	double trans_z = (zsize - 0.5) / 2.0 +
	                 (double)(1 << ZB_POINT_Z_FRAC_BITS) / 2.0;
	double zfull = ndcz * scale_z + trans_z;
	double zval = floor(zfull / (double)(1 << ZB_POINT_Z_FRAC_BITS));
	return (unsigned short)zval;
}

/*
 * Project every splat with the independent reference.  When use_lowpass is 0
 * the 0.3 px^2 anti-aliasing term is omitted, which lets the low pass test
 * prove the production code really adds it.
 */
static RefProj *ref_project(const TGLSplat *points, size_t count,
                            const TGLSplatView *view, int use_lowpass,
                            size_t *out_nproj) {
	const double *V = view->view;
	const double *P = view->projection;
	double R[9], t[3], a, b;
	RefProj *res;
	size_t i, np = 0;
	int r, c;

	for (c = 0; c < 3; ++c) {
		for (r = 0; r < 3; ++r) {
			R[r * 3 + c] = V[c * 4 + r];
		}
		t[c] = V[12 + c];
	}
	a = ((double)view->width - 0.5) / 2.0;
	b = ((double)view->height - 0.5) / 2.0;

	res = (RefProj *)calloc(count ? count : 1, sizeof(RefProj));
	if (!res) {
		return NULL;
	}

	for (i = 0; i < count; ++i) {
		const TGLSplat *in = &points[i];
		RefProj *o;
		double mc[3], sc[6], clip[4], w, ndc[3];
		double J[6], t00, t01, t02, t10, t11, t12;
		double cxx, cxy, cyy, det, fx0, fx1, fy0, fy1;
		int k;
		static const int sym[3][3] = {{0, 1, 2}, {1, 3, 4}, {2, 4, 5}};

		for (k = 0; k < 3; ++k) {
			mc[k] = R[k * 3 + 0] * in->mean[0] +
			        R[k * 3 + 1] * in->mean[1] +
			        R[k * 3 + 2] * in->mean[2] + t[k];
		}
		{
			double rs[9];
			const double s00 = in->covariance[0], s01 = in->covariance[1],
			             s02 = in->covariance[2], s11 = in->covariance[3],
			             s12 = in->covariance[4], s22 = in->covariance[5];
			for (r = 0; r < 3; ++r) {
				double r0 = R[r * 3 + 0], r1 = R[r * 3 + 1],
				       r2 = R[r * 3 + 2];
				rs[r * 3 + 0] = r0 * s00 + r1 * s01 + r2 * s02;
				rs[r * 3 + 1] = r0 * s01 + r1 * s11 + r2 * s12;
				rs[r * 3 + 2] = r0 * s02 + r1 * s12 + r2 * s22;
			}
			for (r = 0; r < 3; ++r) {
				for (c = r; c < 3; ++c) {
					double v = 0.0;
					int q;
					for (q = 0; q < 3; ++q) {
						v += rs[r * 3 + q] * R[c * 3 + q];
					}
					sc[sym[r][c]] = v;
				}
			}
		}

		for (k = 0; k < 4; ++k) {
			clip[k] = P[0 * 4 + k] * mc[0] + P[1 * 4 + k] * mc[1] +
			          P[2 * 4 + k] * mc[2] + P[3 * 4 + k];
		}
		w = clip[3];
		if (!(w > 0.0)) {
			continue;
		}
		ndc[0] = clip[0] / w;
		ndc[1] = clip[1] / w;
		ndc[2] = clip[2] / w;
		if (!(ndc[2] > -1.0 && ndc[2] < 1.0)) {
			continue;
		}

		jacobian_fd(P, mc, (double)view->x, (double)view->y, a, b, J);
		o = &res[np];
		memcpy(o->jfd, J, sizeof(o->jfd));
		jacobian_analytic(P, mc, a, b, o->jan);

		t00 = J[0] * sc[0] + J[1] * sc[1] + J[2] * sc[2];
		t01 = J[0] * sc[1] + J[1] * sc[3] + J[2] * sc[4];
		t02 = J[0] * sc[2] + J[1] * sc[4] + J[2] * sc[5];
		t10 = J[3] * sc[0] + J[4] * sc[1] + J[5] * sc[2];
		t11 = J[3] * sc[1] + J[4] * sc[3] + J[5] * sc[4];
		t12 = J[3] * sc[2] + J[4] * sc[4] + J[5] * sc[5];
		cxx = t00 * J[0] + t01 * J[1] + t02 * J[2];
		cxy = t00 * J[3] + t01 * J[4] + t02 * J[5];
		cyy = t10 * J[3] + t11 * J[4] + t12 * J[5];
		if (use_lowpass) {
			cxx += LOWPASS;
			cyy += LOWPASS;
		}
		det = cxx * cyy - cxy * cxy;
		if (!(det > 0.0)) {
			continue;
		}
		o->cxx = cxx;
		o->cxy = cxy;
		o->cyy = cyy;
		o->icxx = cyy / det;
		o->icxy = -cxy / det;
		o->icyy = cxx / det;

		o->cx = (double)view->x + a * (ndc[0] + 1.0);
		o->cy = (double)view->y + b * (1.0 - ndc[1]);
		o->dist = -mc[2];
		o->zfixed = depth_fixed(ndc[2]);
		o->order = i;
		for (k = 0; k < 3; ++k) {
			o->rgb[k] = in->rgb[k];
		}
		o->opacity = in->opacity;

		fx0 = ceil(o->cx - 3.0 * sqrt(cxx) - 0.5);
		fx1 = floor(o->cx + 3.0 * sqrt(cxx) - 0.5);
		fy0 = ceil(o->cy - 3.0 * sqrt(cyy) - 0.5);
		fy1 = floor(o->cy + 3.0 * sqrt(cyy) - 0.5);
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
		o->x0 = (int)fx0;
		o->x1 = (int)fx1;
		o->y0 = (int)fy0;
		o->y1 = (int)fy1;
		o->valid = 1;
		++np;
	}

	*out_nproj = np;
	return res;
}

typedef struct {
	size_t splat;
	size_t order;
	double dist;
	int tile;
} RefRef;

static int ref_cmp(const void *pa, const void *pb) {
	const RefRef *a = (const RefRef *)pa;
	const RefRef *b = (const RefRef *)pb;
	if (a->tile != b->tile) {
		return a->tile < b->tile ? -1 : 1;
	}
	if (a->dist != b->dist) {
		return a->dist < b->dist ? -1 : 1;
	}
	if (a->order != b->order) {
		return a->order < b->order ? -1 : 1;
	}
	return 0;
}

static int quant8(double v) {
	if (v < 0.0) {
		v = 0.0;
	} else if (v > 1.0) {
		v = 1.0;
	}
	return (int)floor(v * 255.0 + 0.5);
}

/* Independent full renderer into out (a copy of the current color buffer). */
static int ref_render(ZBuffer *dst, const TGLSplat *points, size_t count,
                      const TGLSplatView *view, PIXEL *out,
                      TGLSplatStats *st) {
	RefProj *rp;
	size_t nproj, nrefs = 0, ri, k, npix, tests = 0;
	int ntx, nty, ntiles, tile;
	RefRef *refs;
	double *ar, *ag, *ab, *tr;

	rp = ref_project(points, count, view, 1, &nproj);
	if (!rp) {
		return 1;
	}
	ntx = (view->width + TILE - 1) / TILE;
	nty = (view->height + TILE - 1) / TILE;
	ntiles = ntx * nty;

	for (k = 0; k < nproj; ++k) {
		int i0, i1, j0, j1, i, j;
		if (rp[k].x0 > rp[k].x1 || rp[k].y0 > rp[k].y1) {
			continue;
		}
		i0 = (rp[k].x0 - view->x) >> 4;
		i1 = (rp[k].x1 - view->x) >> 4;
		j0 = (rp[k].y0 - view->y) >> 4;
		j1 = (rp[k].y1 - view->y) >> 4;
		for (j = j0; j <= j1; ++j) {
			for (i = i0; i <= i1; ++i) {
				++nrefs;
			}
		}
	}
	refs = (RefRef *)malloc((nrefs ? nrefs : 1) * sizeof(RefRef));
	npix = (size_t)view->width * (size_t)view->height;
	ar = (double *)calloc(npix, sizeof(double));
	ag = (double *)calloc(npix, sizeof(double));
	ab = (double *)calloc(npix, sizeof(double));
	tr = (double *)malloc(npix * sizeof(double));
	if (!refs || !ar || !ag || !ab || !tr) {
		free(rp);
		free(refs);
		free(ar);
		free(ag);
		free(ab);
		free(tr);
		return 1;
	}
	for (k = 0; k < npix; ++k) {
		tr[k] = 1.0;
	}

	ri = 0;
	for (k = 0; k < nproj; ++k) {
		int i0, i1, j0, j1, i, j;
		if (rp[k].x0 > rp[k].x1 || rp[k].y0 > rp[k].y1) {
			continue;
		}
		i0 = (rp[k].x0 - view->x) >> 4;
		i1 = (rp[k].x1 - view->x) >> 4;
		j0 = (rp[k].y0 - view->y) >> 4;
		j1 = (rp[k].y1 - view->y) >> 4;
		for (j = j0; j <= j1; ++j) {
			for (i = i0; i <= i1; ++i) {
				refs[ri].splat = k;
				refs[ri].order = rp[k].order;
				refs[ri].dist = rp[k].dist;
				refs[ri].tile = j * ntx + i;
				++ri;
			}
		}
	}
	qsort(refs, nrefs, sizeof(RefRef), ref_cmp);

	ri = 0;
	for (tile = 0; tile < ntiles; ++tile) {
		size_t rs = ri;
		int ti = tile % ntx, tj = tile / ntx;
		int lx = view->x + ti * TILE, ly = view->y + tj * TILE;
		int rx = lx + TILE - 1, ry = ly + TILE - 1;
		size_t rr;
		if (rx > view->x + view->width - 1) {
			rx = view->x + view->width - 1;
		}
		if (ry > view->y + view->height - 1) {
			ry = view->y + view->height - 1;
		}
		while (ri < nrefs && refs[ri].tile == tile) {
			++ri;
		}
		for (rr = rs; rr < ri; ++rr) {
			const RefProj *s = &rp[refs[rr].splat];
			int px0 = s->x0 > lx ? s->x0 : lx;
			int px1 = s->x1 < rx ? s->x1 : rx;
			int py0 = s->y0 > ly ? s->y0 : ly;
			int py1 = s->y1 < ry ? s->y1 : ry;
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
					++tests;
					if (q > QMAX) {
						continue;
					}
					if (s->zfixed <
					    dst->zbuf[(size_t)py * (size_t)dst->xsize +
					              (size_t)px]) {
						continue;
					}
					{
						double alpha = s->opacity * exp(-0.5 * q);
						double tt;
						if (alpha > 0.99) {
							alpha = 0.99;
						}
						if (!(alpha > 0.0)) {
							continue;
						}
						tt = tr[local];
						ar[local] += tt * alpha * s->rgb[0];
						ag[local] += tt * alpha * s->rgb[1];
						ab[local] += tt * alpha * s->rgb[2];
						tr[local] = tt * (1.0 - alpha);
					}
				}
			}
		}
	}

	{
		int px, py;
		for (py = 0; py < view->height; ++py) {
			for (px = 0; px < view->width; ++px) {
				size_t local = (size_t)py * (size_t)view->width +
				               (size_t)px;
				size_t bi = (size_t)(view->y + py) * (size_t)dst->xsize +
				            (size_t)(view->x + px);
				PIXEL old = out[bi];
				double r = ar[local] + tr[local] * (GET_RED(old) / 255.0);
				double g = ag[local] + tr[local] * (GET_GREEN(old) / 255.0);
				double bl = ab[local] + tr[local] * (GET_BLUE(old) / 255.0);
				PIXEL packed = (PIXEL)((quant8(r) << 16) | (quant8(g) << 8) |
				                       quant8(bl));
				out[bi] = (old & (PIXEL)0xff000000u) | packed;
			}
		}
	}

	if (st) {
		st->projected_count = nproj;
		st->tile_refs = nrefs;
		st->pixel_tests = tests;
	}
	free(rp);
	free(refs);
	free(ar);
	free(ag);
	free(ab);
	free(tr);
	return 0;
}

/* --------------------------------------------------------------- comparison */

static int diff8(int a, int b) {
	int d = a - b;
	return d < 0 ? -d : d;
}

static int buffers_equal_view(ZBuffer *z, const PIXEL *ref, const TGLSplatView *vw,
                              int tol, int *bad) {
	int px, py;
	*bad = 0;
	for (py = 0; py < vw->height; ++py) {
		for (px = 0; px < vw->width; ++px) {
			size_t bi = (size_t)(vw->y + py) * (size_t)z->xsize +
			            (size_t)(vw->x + px);
			PIXEL a = z->pbuf[bi], b = ref[bi];
			if (diff8(GET_RED(a), GET_RED(b)) > tol ||
			    diff8(GET_GREEN(a), GET_GREEN(b)) > tol ||
			    diff8(GET_BLUE(a), GET_BLUE(b)) > tol) {
				++*bad;
			}
		}
	}
	return *bad == 0;
}

static int outside_unchanged(ZBuffer *z, const PIXEL *before,
                             const TGLSplatView *vw) {
	int x, y;
	for (y = 0; y < z->ysize; ++y) {
		for (x = 0; x < z->xsize; ++x) {
			int inside = x >= vw->x && x < vw->x + vw->width &&
			             y >= vw->y && y < vw->y + vw->height;
			size_t bi = (size_t)y * (size_t)z->xsize + (size_t)x;
			if (!inside && z->pbuf[bi] != before[bi]) {
				return 0;
			}
		}
	}
	return 1;
}

static ZBuffer *new_buffer(int w, int h, unsigned char r, unsigned char g,
                           unsigned char b) {
	ZBuffer *z = ZB_open(w, h, ZB_MODE_RGBA, 0);
	if (!z) {
		return NULL;
	}
	/*
	 * ZB_clear expects color already scaled into TinyGL's pixel domain, the
	 * same transform glopClearColor applies: byte v becomes v * 0xff0000/255,
	 * i.e. v shifted into the high byte.
	 */
	ZB_clear(z, 1, 0, 1, (int)r << 16, (int)g << 16, (int)b << 16);
	return z;
}

static PIXEL *snapshot(ZBuffer *z) {
	size_t n = (size_t)z->xsize * (size_t)z->ysize;
	PIXEL *p = (PIXEL *)malloc(n * sizeof(PIXEL));
	if (p) {
		memcpy(p, z->pbuf, n * sizeof(PIXEL));
	}
	return p;
}

static PIXEL *ref_copy(ZBuffer *z) {
	size_t n = (size_t)z->xsize * (size_t)z->ysize;
	PIXEL *p = (PIXEL *)malloc(n * sizeof(PIXEL));
	if (p) {
		memcpy(p, z->pbuf, n * sizeof(PIXEL));
	}
	return p;
}

/* Run production and reference on identical inputs and compare everything. */
static int cross_check(ZBuffer *z, const TGLSplat *pts, size_t count,
                       const TGLSplatView *vw, TGLSplatStats *got,
                       TGLSplatStats *expect) {
	PIXEL *before = snapshot(z);
	PIXEL *ref = ref_copy(z);
	int bad = 0;
	int ok;
	if (!before || !ref) {
		free(before);
		free(ref);
		return 0;
	}
	if (ref_render(z, pts, count, vw, ref, expect) != 0) {
		free(before);
		free(ref);
		return 0;
	}
	if (tglRenderSplats(z, pts, count, vw, got) != 0) {
		free(before);
		free(ref);
		return 0;
	}
	ok = buffers_equal_view(z, ref, vw, 1, &bad);
	free(before);
	free(ref);
	return ok;
}

/* ---------------------------------------------------------------- projection */

static int case_proj_aniso_rotated(void) {
	ZBuffer *z = new_buffer(40, 28, 20, 20, 20);
	double V[16], P[16], R[9], t[3];
	TGLSplatView vw;
	TGLSplat pts[2];
	TGLSplatStats got, expect;
	RefProj *rp;
	size_t np;
	int k;
	if (!z) {
		return 1;
	}
	rot_axis_angle(R, 0.0, 1.0, 0.0, 0.6);
	t[0] = 0.3;
	t[1] = -0.2;
	t[2] = 0.0;
	view_from(V, R, t);
	mat_frustum(P, -1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
	make_view(&vw, V, P, 0, 0, 40, 28);

	/* anisotropic world covariance, distinct axes, mild off diagonal */
	make_splat(&pts[0], 0.2, -0.1, -3.0, 0.09, 0.02, 0.01, 0.04, 0.005, 0.02,
	           0.9, 0.2, 0.1, 0.8);
	make_splat(&pts[1], -0.5, 0.4, -4.5, 0.05, 0.0, 0.0, 0.12, 0.0, 0.03,
	           0.1, 0.7, 0.9, 0.6);

	TASK_CHECK(cross_check(z, pts, 2, &vw, &got, &expect));
	TASK_CHECK(got.projected_count == expect.projected_count);
	TASK_CHECK(got.tile_refs == expect.tile_refs);
	TASK_CHECK(got.pixel_tests == expect.pixel_tests);

	rp = ref_project(pts, 2, &vw, 1, &np);
	TASK_CHECK(rp != NULL && np >= 1);
	for (k = 0; k < (int)np; ++k) {
		int j;
		/* finite difference and closed form Jacobians must agree */
		for (j = 0; j < 6; ++j) {
			double d = fabs(rp[k].jfd[j] - rp[k].jan[j]);
			TASK_CHECK(d < 1e-5);
		}
		/* the rotated anisotropic covariance stays anisotropic on screen */
		TASK_CHECK(fabs(rp[k].cxx - rp[k].cyy) > 1e-6);
	}
	/* rotated projection must generate a non zero off diagonal term */
	TASK_CHECK(fabs(rp[0].cxy) > 1e-9);
	free(rp);
	ZB_close(z);
	return 0;
}

static int case_proj_perspective_vs_ortho(void) {
	ZBuffer *z = new_buffer(40, 28, 0, 0, 0);
	double V[16], Pp[16], Po[16], R[9], t[3];
	TGLSplatView vp, vo;
	TGLSplat pts[1];
	TGLSplatStats gp, ep, go, eo;
	RefProj *rpp, *rpo;
	size_t np1, np2;
	if (!z) {
		return 1;
	}
	rot_axis_angle(R, 0.0, 0.0, 1.0, 0.35);
	t[0] = 0.0;
	t[1] = 0.0;
	t[2] = 0.0;
	view_from(V, R, t);
	mat_frustum(Pp, -1.0, 1.0, -1.0, 1.0, 1.0, 20.0);
	mat_ortho(Po, -1.0, 1.0, -1.0, 1.0, 0.1, 20.0);
	make_view(&vp, V, Pp, 0, 0, 40, 28);
	make_view(&vo, V, Po, 0, 0, 40, 28);
	make_splat(&pts[0], 0.4, 0.25, -5.0, 0.06, 0.01, 0.0, 0.06, 0.0, 0.03,
	           0.8, 0.8, 0.2, 0.7);

	TASK_CHECK(cross_check(z, pts, 1, &vp, &gp, &ep));
	TASK_CHECK(cross_check(z, pts, 1, &vo, &go, &eo));

	rpp = ref_project(pts, 1, &vp, 1, &np1);
	rpo = ref_project(pts, 1, &vo, 1, &np2);
	TASK_CHECK(rpp && rpo && np1 == 1 && np2 == 1);
	/* perspective and orthographic projections must land differently */
	TASK_CHECK(fabs(rpp[0].cx - rpo[0].cx) > 1e-6 ||
	           fabs(rpp[0].cy - rpo[0].cy) > 1e-6);
	/* perspective Jacobian has camera-position dependent entries */
	TASK_CHECK(fabs(rpp[0].jfd[0] - rpo[0].jfd[0]) > 1e-6);
	free(rpp);
	free(rpo);
	ZB_close(z);
	return 0;
}

static int case_proj_offscreen_center(void) {
	ZBuffer *z = new_buffer(40, 28, 10, 30, 50);
	double V[16], P[16];
	TGLSplatView vw;
	TGLSplat pts[2];
	TGLSplatStats got, expect;
	RefProj *rp;
	size_t np;
	int changed = 0, x, y;
	PIXEL *before;
	if (!z) {
		return 1;
	}
	mat_identity(V);
	mat_ortho(P, -2.0, 2.0, -2.0, 2.0, 0.1, 20.0);
	/* small viewport sitting inside the buffer with a non zero origin */
	make_view(&vw, V, P, 8, 6, 20, 16);

	/* center just outside the viewport on the left, its tail reaches in */
	make_splat(&pts[0], -2.05, 0.0, -3.0, 0.08, 0.0, 0.0, 0.08, 0.0, 0.02,
	           1.0, 1.0, 1.0, 0.9);
	/* fully beyond the top right corner, no overlap with the viewport */
	make_splat(&pts[1], 2.5, 2.5, -3.0, 0.002, 0.0, 0.0, 0.002, 0.0,
	           0.002, 1.0, 0.0, 0.0, 0.9);

	before = snapshot(z);
	TASK_CHECK(before != NULL);
	TASK_CHECK(cross_check(z, pts, 2, &vw, &got, &expect));
	TASK_CHECK(got.projected_count == 2);
	TASK_CHECK(got.tile_refs == expect.tile_refs);
	TASK_CHECK(got.pixel_tests == expect.pixel_tests);
	for (y = vw.y; y < vw.y + vw.height; ++y) {
		for (x = vw.x; x < vw.x + vw.width; ++x) {
			size_t bi = (size_t)y * (size_t)z->xsize + (size_t)x;
			if (z->pbuf[bi] != before[bi]) {
				changed = 1;
			}
		}
	}
	TASK_CHECK(changed); /* the off-center tail really painted something */
	TASK_CHECK(outside_unchanged(z, before, &vw));

	rp = ref_project(pts, 2, &vw, 1, &np);
	TASK_CHECK(rp != NULL && np == 2);
	/* the second splat has no tile references: its tail never reaches */
	TASK_CHECK(rp[1].x0 > rp[1].x1 || rp[1].y0 > rp[1].y1);
	free(rp);
	free(before);
	ZB_close(z);
	return 0;
}

static int case_proj_lowpass(void) {
	ZBuffer *z = new_buffer(40, 28, 0, 0, 0);
	double V[16], P[16];
	TGLSplatView vw;
	TGLSplat pts[1];
	TGLSplatStats got, with;
	RefProj *rpon, *rpoff;
	size_t n1, n2;
	if (!z) {
		return 1;
	}
	mat_identity(V);
	mat_frustum(P, -1.0, 1.0, -1.0, 1.0, 1.0, 20.0);
	make_view(&vw, V, P, 0, 0, 40, 28);
	/* very tight covariance: coverage is dominated by the low pass term */
	make_splat(&pts[0], 0.0, 0.0, -4.0, 0.0004, 0.0, 0.0, 0.0004, 0.0,
	           0.0004, 0.7, 0.7, 0.7, 0.9);

	TASK_CHECK(cross_check(z, pts, 1, &vw, &got, &with));
	rpon = ref_project(pts, 1, &vw, 1, &n1);
	rpoff = ref_project(pts, 1, &vw, 0, &n2);
	TASK_CHECK(rpon && rpoff && n1 == 1 && n2 == 1);
	/* diagonal screen covariance carries exactly the 0.3 px^2 addition */
	TASK_CHECK(fabs((rpon[0].cxx - rpoff[0].cxx) - LOWPASS) < 1e-9);
	TASK_CHECK(fabs((rpon[0].cyy - rpoff[0].cyy) - LOWPASS) < 1e-9);
	TASK_CHECK(fabs(rpon[0].cxy - rpoff[0].cxy) < 1e-12);
	/* the low pass term strictly grows the covered footprint */
	TASK_CHECK(got.pixel_tests > 0);
	TASK_CHECK((rpon[0].x1 - rpon[0].x0) >= (rpoff[0].x1 - rpoff[0].x0));
	TASK_CHECK((rpon[0].y1 - rpon[0].y0) >= (rpoff[0].y1 - rpoff[0].y0));
	TASK_CHECK((rpon[0].x1 - rpon[0].x0) > (rpoff[0].x1 - rpoff[0].x0) ||
	           (rpon[0].y1 - rpon[0].y0) > (rpoff[0].y1 - rpoff[0].y0));
	free(rpon);
	free(rpoff);
	ZB_close(z);
	return 0;
}

/* ------------------------------------------------------------- compositing */

static int case_comp_overlap_float(void) {
	ZBuffer *z = new_buffer(32, 32, 40, 40, 40);
	double V[16], P[16];
	TGLSplatView vw;
	TGLSplat pts[2];
	TGLSplatStats got, expect;
	int px, py;
	if (!z) {
		return 1;
	}
	mat_identity(V);
	mat_ortho(P, -1.0, 1.0, -1.0, 1.0, 0.1, 20.0);
	make_view(&vw, V, P, 0, 0, 32, 32);
	make_splat(&pts[0], -0.05, 0.0, -3.0, 0.02, 0.0, 0.0, 0.02, 0.0, 0.01,
	           0.9, 0.1, 0.1, 0.6);
	make_splat(&pts[1], 0.05, 0.0, -4.0, 0.02, 0.0, 0.0, 0.02, 0.0, 0.01,
	           0.1, 0.2, 0.9, 0.5);

	TASK_CHECK(cross_check(z, pts, 2, &vw, &got, &expect));
	TASK_CHECK(got.pixel_tests == expect.pixel_tests);

	/* independent single-pixel float check against a hand driven reference */
	{
		RefProj *rp;
		size_t np_ref;
		rp = ref_project(pts, 2, &vw, 1, &np_ref);
		TASK_CHECK(rp != NULL && np_ref >= 1);
		for (py = 0; py < 32; ++py) {
			for (px = 0; px < 32; ++px) {
				size_t bi = (size_t)py * 32u + (size_t)px;
				size_t k;
				double T = 1.0, sr = 0.0, sg = 0.0, sb = 0.0;
				for (k = 0; k < np_ref; ++k) {
					double dx = (px + 0.5) - rp[k].cx;
					double dy = (py + 0.5) - rp[k].cy;
					double q = rp[k].icxx * dx * dx +
					           2.0 * rp[k].icxy * dx * dy + rp[k].icyy * dy * dy;
					double alpha;
					if (q > QMAX) {
						continue;
					}
					alpha = rp[k].opacity * exp(-0.5 * q);
					if (alpha > 0.99) {
						alpha = 0.99;
					}
					sr += T * alpha * rp[k].rgb[0];
					sg += T * alpha * rp[k].rgb[1];
					sb += T * alpha * rp[k].rgb[2];
					T *= 1.0 - alpha;
				}
				{
					double r = sr + T * (40.0 / 255.0);
					double g = sg + T * (40.0 / 255.0);
					double b = sb + T * (40.0 / 255.0);
					/*
					 * Independently re-derived float blend.  The reference uses
					 * the finite difference Jacobian whose q differs from the
					 * analytic build by a rounding scale, so one quantization
					 * step is allowed.
					 */
					TASK_CHECK(diff8(GET_RED(z->pbuf[bi]), quant8(r)) <= 1);
					TASK_CHECK(diff8(GET_GREEN(z->pbuf[bi]), quant8(g)) <= 1);
					TASK_CHECK(diff8(GET_BLUE(z->pbuf[bi]), quant8(b)) <= 1);
				}
			}
		}
		free(rp);
	}
	ZB_close(z);
	return 0;
}

static int case_comp_equal_distance(void) {
	ZBuffer *z1 = new_buffer(24, 24, 0, 0, 0);
	ZBuffer *z2 = new_buffer(24, 24, 0, 0, 0);
	double V[16], P[16];
	TGLSplatView vw;
	TGLSplat ab[2], ba[2];
	TGLSplatStats s1, e1, s2, e2;
	PIXEL *r1, *r2;
	int differ = 0, x, y;
	if (!z1 || !z2) {
		if (z1) {
			ZB_close(z1);
		}
		if (z2) {
			ZB_close(z2);
		}
		return 1;
	}
	mat_identity(V);
	mat_ortho(P, -1.0, 1.0, -1.0, 1.0, 0.1, 20.0);
	make_view(&vw, V, P, 0, 0, 24, 24);
	/* identical means and therefore identical camera distance */
	make_splat(&ab[0], 0.0, 0.0, -3.0, 0.03, 0.0, 0.0, 0.03, 0.0, 0.02,
	           0.9, 0.1, 0.1, 0.7);
	make_splat(&ab[1], 0.0, 0.0, -3.0, 0.03, 0.0, 0.0, 0.03, 0.0, 0.02,
	           0.1, 0.1, 0.9, 0.7);
	ba[0] = ab[1];
	ba[1] = ab[0];

	r1 = ref_copy(z1);
	r2 = ref_copy(z2);
	TASK_CHECK(r1 && r2);
	TASK_CHECK(ref_render(z1, ab, 2, &vw, r1, &e1) == 0);
	TASK_CHECK(ref_render(z2, ba, 2, &vw, r2, &e2) == 0);
	TASK_CHECK(tglRenderSplats(z1, ab, 2, &vw, &s1) == 0);
	TASK_CHECK(tglRenderSplats(z2, ba, 2, &vw, &s2) == 0);
	TASK_CHECK(s1.pixel_tests == e1.pixel_tests);
	for (y = 0; y < 24; ++y) {
		for (x = 0; x < 24; ++x) {
			size_t bi = (size_t)y * 24u + (size_t)x;
			int bad = 0;
			TASK_CHECK(buffers_equal_view(z1, r1, &vw, 0, &bad));
			TASK_CHECK(buffers_equal_view(z2, r2, &vw, 0, &bad));
			if (z1->pbuf[bi] != z2->pbuf[bi]) {
				differ = 1;
			}
		}
	}
	TASK_CHECK(differ); /* input index, not distance, breaks the tie */
	free(r1);
	free(r2);
	ZB_close(z1);
	ZB_close(z2);
	return 0;
}

static int case_comp_triangle_occlusion(void) {
	const int W = 48, H = 32;
	ZBuffer *z = ZB_open(W, H, ZB_MODE_RGBA, 0);
	double V[16], P[16];
	TGLSplatView vw;
	TGLSplat pts[2];
	TGLSplatStats got, expect;
	PIXEL *color_before, *ref;
	GLushort *z_before;
	size_t zn;
	int x, y, front_changed = 0, behind_visible = 0;
	if (!z) {
		return 1;
	}
	glInit(z);
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	/* same finite frustum is handed to the splats in double precision below */
	glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_LIGHTING);
	glDisable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* opaque gray triangle at eye depth -5, covering the central band */
	glColor3f(0.3f, 0.3f, 0.3f);
	glBegin(GL_TRIANGLES);
	glVertex3f(-2.0f, -1.0f, -5.0f);
	glVertex3f(2.0f, -1.0f, -5.0f);
	glVertex3f(0.0f, 1.0f, -5.0f);
	glEnd();

	mat_identity(V);
	mat_frustum(P, -1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
	make_view(&vw, V, P, 0, 0, W, H);
	/* pure red splat in front of the triangle (eye z -3, distance 3) */
	make_splat(&pts[0], 0.0, 0.0, -3.0, 0.05, 0.0, 0.0, 0.05, 0.0, 0.02,
	           0.9, 0.0, 0.0, 0.9);
	/* pure green, broad splat behind the triangle (eye z -7, distance 7) */
	make_splat(&pts[1], 0.0, 0.0, -7.0, 6.0, 0.0, 0.0, 6.0, 0.0, 0.5,
	           0.0, 0.9, 0.0, 0.9);

	color_before = snapshot(z);
	zn = (size_t)W * (size_t)H;
	z_before = (GLushort *)malloc(zn * sizeof(GLushort));
	ref = ref_copy(z);
	TASK_CHECK(color_before && z_before && ref);
	memcpy(z_before, z->zbuf, zn * sizeof(GLushort));

	TASK_CHECK(ref_render(z, pts, 2, &vw, ref, &expect) == 0);
	TASK_CHECK(tglRenderSplats(z, pts, 2, &vw, &got) == 0);
	TASK_CHECK(got.pixel_tests == expect.pixel_tests);
	TASK_CHECK(got.projected_count == expect.projected_count);

	for (y = 0; y < H; ++y) {
		for (x = 0; x < W; ++x) {
			size_t bi = (size_t)y * (size_t)W + (size_t)x;
			/* depth buffer is read only for splats ... */
			TASK_CHECK(z->zbuf[bi] == z_before[bi]);
			/* ... and the production image matches the independent renderer */
			TASK_CHECK(diff8(GET_RED(z->pbuf[bi]), GET_RED(ref[bi])) <= 1);
			TASK_CHECK(diff8(GET_GREEN(z->pbuf[bi]), GET_GREEN(ref[bi])) <= 1);
			TASK_CHECK(diff8(GET_BLUE(z->pbuf[bi]), GET_BLUE(ref[bi])) <= 1);
		}
	}

	/* geometry level assertions in the center column */
	for (y = 0; y < H; ++y) {
		size_t bi = (size_t)y * (size_t)W + (size_t)(W / 2);
		PIXEL now = z->pbuf[bi], was = color_before[bi];
		if (z_before[bi] != 0) {
			/*
			 * Opaque triangle pixel.  The green splat behind it fails the >=
			 * depth test and the red front splat carries no green, so the
			 * green channel can never grow here.
			 */
			TASK_CHECK(GET_GREEN(now) <= GET_GREEN(was) + 1);
		}
		if (y >= 14 && y <= 18) {
			/* the front red splat visibly tints even the triangle region */
			if (GET_RED(now) > GET_RED(was)) {
				front_changed = 1;
			}
		}
	}
	/*
	 * Near the top edge there is no triangle, so the broad green splat must
	 * still composite onto the background there.
	 */
	{
		size_t top = (size_t)2 * (size_t)W + (size_t)(W / 2);
		TASK_CHECK(z_before[top] == 0);
		if (GET_GREEN(z->pbuf[top]) > GET_GREEN(color_before[top])) {
			behind_visible = 1;
		}
	}
	TASK_CHECK(front_changed);
	TASK_CHECK(behind_visible);
	free(color_before);
	free(z_before);
	free(ref);
	ZB_close(z);
	return 0;
}

/* -------------------------------------------------------------------- tiles */

static int case_tiles_partial_viewport(void) {
	ZBuffer *z = new_buffer(40, 28, 30, 30, 30);
	double V[16], P[16];
	TGLSplatView vw;
	TGLSplat pts[3];
	TGLSplatStats got, expect;
	PIXEL *before;
	if (!z) {
		return 1;
	}
	mat_identity(V);
	mat_ortho(P, -2.0, 2.0, -2.0, 2.0, 0.1, 20.0);
	/* non zero origin, width and height are not multiples of 16 */
	make_view(&vw, V, P, 3, 5, 33, 21);
	make_splat(&pts[0], -0.9, 0.7, -3.0, 0.08, 0.01, 0.0, 0.08, 0.0, 0.03,
	           0.9, 0.2, 0.2, 0.8);
	make_splat(&pts[1], 0.6, -0.6, -4.0, 0.1, 0.0, 0.0, 0.06, 0.0, 0.03,
	           0.2, 0.9, 0.3, 0.7);
	make_splat(&pts[2], 1.1, 1.1, -5.0, 0.05, 0.0, 0.0, 0.05, 0.0, 0.02,
	           0.3, 0.3, 0.9, 0.6);

	before = snapshot(z);
	TASK_CHECK(before != NULL);
	TASK_CHECK(cross_check(z, pts, 3, &vw, &got, &expect));
	TASK_CHECK(got.projected_count == expect.projected_count);
	TASK_CHECK(got.tile_refs == expect.tile_refs);
	TASK_CHECK(got.pixel_tests == expect.pixel_tests);
	TASK_CHECK(got.tile_refs > 0);
	TASK_CHECK(outside_unchanged(z, before, &vw));
	free(before);
	ZB_close(z);
	return 0;
}

static int case_tiles_counts_and_seams(void) {
	ZBuffer *z = new_buffer(48, 32, 0, 0, 0);
	double V[16], P[16];
	TGLSplatView vw;
	TGLSplat pts[4];
	TGLSplatStats got, expect;
	RefProj *rp;
	size_t np, k;
	int manual_refs = 0, ntx, nty, seam_ok = 1;
	if (!z) {
		return 1;
	}
	mat_identity(V);
	mat_ortho(P, -2.0, 2.0, -2.0, 2.0, 0.1, 20.0);
	make_view(&vw, V, P, 0, 0, 48, 32);
	/* splats deliberately straddling the x=16 and x=32 tile borders */
	make_splat(&pts[0], -0.35, 0.0, -3.0, 0.05, 0.0, 0.0, 0.05, 0.0, 0.02,
	           0.9, 0.9, 0.2, 0.8);
	make_splat(&pts[1], 0.35, 0.0, -3.5, 0.05, 0.0, 0.0, 0.05, 0.0, 0.02,
	           0.2, 0.9, 0.9, 0.8);
	make_splat(&pts[2], 0.0, 0.9, -4.5, 0.07, 0.0, 0.0, 0.04, 0.0, 0.02,
	           0.9, 0.3, 0.9, 0.7);
	make_splat(&pts[3], 0.0, -0.9, -5.5, 0.07, 0.0, 0.0, 0.04, 0.0, 0.02,
	           0.5, 0.9, 0.5, 0.7);

	TASK_CHECK(cross_check(z, pts, 4, &vw, &got, &expect));
	TASK_CHECK(got.pixel_tests == expect.pixel_tests);
	TASK_CHECK(got.tile_refs == expect.tile_refs);

	rp = ref_project(pts, 4, &vw, 1, &np);
	TASK_CHECK(rp != NULL && np == 4);
	ntx = (vw.width + 15) / 16;
	nty = (vw.height + 15) / 16;
	for (k = 0; k < np; ++k) {
		int i0 = (rp[k].x0 - vw.x) >> 4;
		int i1 = (rp[k].x1 - vw.x) >> 4;
		int j0 = (rp[k].y0 - vw.y) >> 4;
		int j1 = (rp[k].y1 - vw.y) >> 4;
		int i, j;
		for (j = j0; j <= j1; ++j) {
			for (i = i0; i <= i1; ++i) {
				++manual_refs;
			}
		}
	}
	TASK_CHECK(manual_refs == (int)got.tile_refs);
	TASK_CHECK(ntx == 3 && nty == 2);
	/* at least one splat spans multiple tiles, and it stays seamless */
	for (k = 0; k < np; ++k) {
		if ((rp[k].x1 >> 4) > (rp[k].x0 >> 4)) {
			seam_ok = 1;
		}
	}
	TASK_CHECK(seam_ok);
	free(rp);
	ZB_close(z);
	return 0;
}

static int case_tiles_invalid_preserves(void) {
	ZBuffer *z = new_buffer(32, 32, 12, 24, 48);
	double V[16], P[16];
	TGLSplatView good, bad_vp;
	TGLSplat ok[1], bad_cov[1], nan_pt[1];
	PIXEL *before;
	GLushort *zbefore;
	size_t n = 32u * 32u;
	TGLSplatStats st;
	if (!z) {
		return 1;
	}
	mat_identity(V);
	mat_ortho(P, -1.0, 1.0, -1.0, 1.0, 0.1, 20.0);
	make_view(&good, V, P, 0, 0, 32, 32);
	make_splat(&ok[0], 0.0, 0.0, -3.0, 0.02, 0.0, 0.0, 0.02, 0.0, 0.02,
	           0.5, 0.5, 0.5, 0.5);

	bad_cov[0] = ok[0];
	bad_cov[0].covariance[0] = -0.02; /* not positive definite */
	nan_pt[0] = ok[0];
	nan_pt[0].mean[1] = strtod("nan", NULL);

	bad_vp = good;
	bad_vp.width = 60; /* extends past the 32 wide target */

	before = snapshot(z);
	zbefore = (GLushort *)malloc(n * sizeof(GLushort));
	TASK_CHECK(before && zbefore);
	memcpy(zbefore, z->zbuf, n * sizeof(GLushort));

	/* NULL destination / view */
	TASK_CHECK(tglRenderSplats(NULL, ok, 1, &good, &st) != 0);
	TASK_CHECK(tglRenderSplats(z, ok, 1, NULL, &st) != 0);
	/* non empty batch with NULL points */
	TASK_CHECK(tglRenderSplats(z, NULL, 1, &good, &st) != 0);
	/* viewport outside the target */
	TASK_CHECK(tglRenderSplats(z, ok, 1, &bad_vp, &st) != 0);
	/* bad geometry */
	TASK_CHECK(tglRenderSplats(z, bad_cov, 1, &good, &st) != 0);
	TASK_CHECK(tglRenderSplats(z, nan_pt, 1, &good, &st) != 0);
	/* out of range color */
	{
		TGLSplat bad_rgb = ok[0];
		bad_rgb.rgb[2] = 1.5;
		TASK_CHECK(tglRenderSplats(z, &bad_rgb, 1, &good, &st) != 0);
	}
	/* zero/negative viewport size */
	{
		TGLSplatView zv = good;
		zv.width = 0;
		TASK_CHECK(tglRenderSplats(z, ok, 1, &zv, &st) != 0);
	}

	/* every rejection must leave color and depth untouched */
	TASK_CHECK(memcmp(before, z->pbuf, n * sizeof(PIXEL)) == 0);
	TASK_CHECK(memcmp(zbefore, z->zbuf, n * sizeof(GLushort)) == 0);

	/* count 0 is legal even with NULL points and changes nothing */
	TASK_CHECK(tglRenderSplats(z, NULL, 0, &good, &st) == 0);
	TASK_CHECK(st.projected_count == 0 && st.tile_refs == 0 &&
	           st.pixel_tests == 0);
	TASK_CHECK(memcmp(before, z->pbuf, n * sizeof(PIXEL)) == 0);
	/* NULL stats must be accepted */
	TASK_CHECK(tglRenderSplats(z, ok, 1, &good, NULL) == 0);

	free(before);
	free(zbefore);
	ZB_close(z);
	return 0;
}

static int case_tiles_two_views(void) {
	const int W = 40, H = 28;
	ZBuffer *za = new_buffer(W, H, 25, 25, 25);
	ZBuffer *zb = new_buffer(W, H, 25, 25, 25);
	double Va[16], Vb[16], P[16], Rb[9], t[3];
	TGLSplatView wa, wb;
	TGLSplat pts[3];
	TGLSplatStats s;
	PIXEL *ref;
	int bad = 0;
	if (!za || !zb) {
		if (za) {
			ZB_close(za);
		}
		if (zb) {
			ZB_close(zb);
		}
		return 1;
	}
	mat_identity(Va);
	rot_axis_angle(Rb, 0.0, 1.0, 0.0, 0.5);
	t[0] = 0.2;
	t[1] = 0.0;
	t[2] = 0.0;
	view_from(Vb, Rb, t);
	mat_frustum(P, -1.0, 1.0, -1.0, 1.0, 1.0, 20.0);
	make_view(&wa, Va, P, 2, 2, 32, 22);
	make_view(&wb, Vb, P, 2, 2, 32, 22);
	make_splat(&pts[0], 0.3, 0.2, -4.0, 0.05, 0.0, 0.0, 0.05, 0.0, 0.02,
	           0.9, 0.2, 0.3, 0.8);
	make_splat(&pts[1], -0.4, -0.3, -5.0, 0.06, 0.0, 0.0, 0.05, 0.0, 0.02,
	           0.2, 0.8, 0.4, 0.7);
	make_splat(&pts[2], 0.0, 0.6, -6.0, 0.04, 0.0, 0.0, 0.04, 0.0, 0.02,
	           0.4, 0.4, 0.9, 0.6);

	/* production: two successive calls with different views, no caching */
	TASK_CHECK(tglRenderSplats(za, pts, 3, &wa, &s) == 0);
	TASK_CHECK(tglRenderSplats(za, pts, 3, &wb, &s) == 0);

	/* reference: apply the same two passes to the current buffer colors */
	ref = ref_copy(zb);
	TASK_CHECK(ref != NULL);
	TASK_CHECK(ref_render(zb, pts, 3, &wa, ref, NULL) == 0);
	TASK_CHECK(ref_render(zb, pts, 3, &wb, ref, NULL) == 0);

	TASK_CHECK(buffers_equal_view(za, ref, &wb, 1, &bad));
	free(ref);
	ZB_close(za);
	ZB_close(zb);
	return 0;
}

/* --------------------------------------------------------------------- cases */

const struct task_case task_cases[] = {
	{"projection", "anisotropic_rotation", case_proj_aniso_rotated},
	{"projection", "perspective_vs_ortho", case_proj_perspective_vs_ortho},
	{"projection", "offscreen_center", case_proj_offscreen_center},
	{"projection", "lowpass_term", case_proj_lowpass},

	{"compositing", "overlap_float", case_comp_overlap_float},
	{"compositing", "equal_distance_order", case_comp_equal_distance},
	{"compositing", "triangle_occlusion", case_comp_triangle_occlusion},

	{"tiles", "partial_viewport", case_tiles_partial_viewport},
	{"tiles", "counts_and_seams", case_tiles_counts_and_seams},
	{"tiles", "invalid_preserves", case_tiles_invalid_preserves},
	{"tiles", "two_changed_views", case_tiles_two_views},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
