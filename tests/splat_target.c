/*
 * Independent verification for tglRenderSplats().
 *
 * The reference below re-derives every projection step in plain double
 * precision: its Jacobian comes from finite differences of the divide-by-w
 * viewport map, it never includes or calls the implementation in src/splat.c
 * (only the public header), and its rasteriser walks every viewport pixel
 * directly instead of using tiles. The tile statistics are then counted
 * separately with the same 16x16 bucket rules.
 */

#include "task_support/check.h"
#include "GL/gl.h"
#include "GL/splat.h"
#include "zbuffer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef TILE
#define TILE 16
#endif
#define LOWPASS 0.3
#define QMAX 9.0

/* ------------------------------------------------------------------ */
/* small matrix helpers, all column major 4x4                          */
/* ------------------------------------------------------------------ */

static void mat_identity(double m[16]) {
	int i;
	for (i = 0; i < 16; i++)
		m[i] = 0.0;
	m[0] = m[5] = m[10] = m[15] = 1.0;
}

static void mat_vec(const double m[16], const double v[4], double o[4]) {
	int i, j;
	for (i = 0; i < 4; i++) {
		double s = 0.0;
		for (j = 0; j < 4; j++)
			s += m[4 * j + i] * v[j];
		o[i] = s;
	}
}

/* glFrustum style finite perspective projection, NDC z in [-1,1]. */
static void mat_perspective(double l, double r, double b, double t,
                            double n, double f, double m[16]) {
	memset(m, 0, 16 * sizeof(double));
	m[0] = (2.0 * n) / (r - l);
	m[5] = (2.0 * n) / (t - b);
	m[8] = (r + l) / (r - l);
	m[9] = (t + b) / (t - b);
	m[10] = -(f + n) / (f - n);
	m[11] = -1.0;
	m[14] = -(2.0 * f * n) / (f - n);
}

static void mat_ortho(double l, double r, double b, double t,
                      double n, double f, double m[16]) {
	memset(m, 0, 16 * sizeof(double));
	m[0] = 2.0 / (r - l);
	m[5] = 2.0 / (t - b);
	m[10] = -2.0 / (f - n);
	m[15] = 1.0;
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[14] = -(f + n) / (f - n);
}

/* Rigid world-to-camera view from rotation R (row major 3x3) and t. */
static void mat_view(const double R[3][3], const double t[3], double m[16]) {
	int i, j;
	memset(m, 0, 16 * sizeof(double));
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++)
			m[4 * j + i] = R[i][j];
		m[12 + i] = t[i];
	}
	m[15] = 1.0;
}

static double wcov(const double c[6], int i, int j) {
	static const unsigned char idx[3][3] = {
	    {0, 1, 2}, {1, 3, 4}, {2, 4, 5}};
	if (i > j) {
		int h = i;
		i = j;
		j = h;
	}
	return c[idx[i][j]];
}

static TGLSplat make_splat(double mx, double my, double mz,
                           double sxx, double sxy, double sxz,
                           double syy, double syz, double szz,
                           double r, double g, double bl, double op) {
	TGLSplat s;
	s.mean[0] = mx;
	s.mean[1] = my;
	s.mean[2] = mz;
	s.covariance[0] = sxx;
	s.covariance[1] = sxy;
	s.covariance[2] = sxz;
	s.covariance[3] = syy;
	s.covariance[4] = syz;
	s.covariance[5] = szz;
	s.rgb[0] = r;
	s.rgb[1] = g;
	s.rgb[2] = bl;
	s.opacity = op;
	return s;
}

/* Invert the camera placement so a desired camera-space centre is easy
 * to express as a world mean: world = R^T (mc - t). */
static void world_from_camera(const double R[3][3], const double t[3],
                              const double mc[3], double world[3]) {
	int i, j;
	for (i = 0; i < 3; i++) {
		double s = -t[i];
		for (j = 0; j < 3; j++)
			s += R[j][i] * mc[j]; /* R is orthonormal, R^T[i][j]=R[j][i] */
		world[i] = s;
	}
}

/* ------------------------------------------------------------------ */
/* independent projection reference                                    */
/* ------------------------------------------------------------------ */

typedef struct RefSplat {
	double cx, cy;
	double C[2][2];
	double inv[2][2];
	double dist;
	double r, g, b, op;
	unsigned short z;
	int bx0, bx1, by0, by1;
	int index;
} RefSplat;

/* TinyGL fixed point depth, copied from its viewport formulas. */
static unsigned short ref_depth(double ndcz) {
	const double zsize = (double)(1L << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
	double scale_z = -((zsize - 0.5) * 0.5);
	double trans_z = (zsize - 0.5) * 0.5
	               + (double)(1 << ZB_POINT_Z_FRAC_BITS) * 0.5;
	double full = ndcz * scale_z + trans_z;
	return (unsigned short)((long long)full >> ZB_POINT_Z_FRAC_BITS);
}

/* Returns 1 if the point passes the centre rule, 0 otherwise. */
static int ref_project(const TGLSplat *s, const TGLSplatView *vw,
                       RefSplat *o) {
	const double *V = vw->view;
	const double *P = vw->projection;
	double R[3][3], t[3], mc[3], sc[3][3];
	double clip[4], w, ndc[3], X, Y, a, bb;
	double J[2][3];
	int i, j, k, l;

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++)
			R[i][j] = V[4 * j + i];
		t[i] = V[12 + i];
	}
	for (i = 0; i < 3; i++) {
		mc[i] = t[i];
		for (j = 0; j < 3; j++)
			mc[i] += R[i][j] * s->mean[j];
	}
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {
			double sum = 0.0;
			for (k = 0; k < 3; k++)
				for (l = 0; l < 3; l++)
					sum += R[i][k] * wcov(s->covariance, k, l) * R[j][l];
			sc[i][j] = sum;
		}
	{
		double v4[4] = {mc[0], mc[1], mc[2], 1.0};
		mat_vec(P, v4, clip);
	}
	w = clip[3];
	if (!(w > 0.0))
		return 0;
	ndc[0] = clip[0] / w;
	ndc[1] = clip[1] / w;
	ndc[2] = clip[2] / w;
	if (!(ndc[2] > -1.0 && ndc[2] < 1.0))
		return 0;

	a = ((double)vw->width - 0.5) * 0.5;
	bb = ((double)vw->height - 0.5) * 0.5;
	X = (double)vw->x + a * (ndc[0] + 1.0);
	Y = (double)vw->y + bb * (1.0 - ndc[1]);

	/* Finite difference Jacobian of the screen map w.r.t. camera pos. */
	for (k = 0; k < 3; k++) {
		double eps = 1e-7 * (1.0 + fabs(mc[k]));
		double mc2[3], v4[4], clip2[4], w2;
		mc2[0] = mc[0];
		mc2[1] = mc[1];
		mc2[2] = mc[2];
		mc2[k] += eps;
		v4[0] = mc2[0];
		v4[1] = mc2[1];
		v4[2] = mc2[2];
		v4[3] = 1.0;
		mat_vec(P, v4, clip2);
		w2 = clip2[3];
		J[0][k] = (((double)vw->x + a * (clip2[0] / w2 + 1.0)) - X) / eps;
		J[1][k] = (((double)vw->y + bb * (1.0 - clip2[1] / w2)) - Y) / eps;
	}

	o->C[0][0] = o->C[0][1] = o->C[1][1] = 0.0;
	for (k = 0; k < 3; k++)
		for (l = 0; l < 3; l++) {
			o->C[0][0] += J[0][k] * sc[k][l] * J[0][l];
			o->C[0][1] += J[0][k] * sc[k][l] * J[1][l];
			o->C[1][1] += J[1][k] * sc[k][l] * J[1][l];
		}
	o->C[1][0] = o->C[0][1];
	o->C[0][0] += LOWPASS;
	o->C[1][1] += LOWPASS;
	{
		double det = o->C[0][0] * o->C[1][1] - o->C[0][1] * o->C[1][0];
		o->inv[0][0] = o->C[1][1] / det;
		o->inv[0][1] = -o->C[0][1] / det;
		o->inv[1][0] = o->inv[0][1];
		o->inv[1][1] = o->C[0][0] / det;
	}

	o->cx = X;
	o->cy = Y;
	o->dist = -mc[2];
	o->r = s->rgb[0];
	o->g = s->rgb[1];
	o->b = s->rgb[2];
	o->op = s->opacity;
	o->z = ref_depth(ndc[2]);

	{
		double radx = 3.0 * sqrt(o->C[0][0]);
		double rady = 3.0 * sqrt(o->C[1][1]);
		double x0 = ceil(X - radx - 0.5);
		double x1 = floor(X + radx - 0.5);
		double y0 = ceil(Y - rady - 0.5);
		double y1 = floor(Y + rady - 0.5);
		int vx1 = vw->x + vw->width - 1;
		int vy1 = vw->y + vw->height - 1;
		if (x0 < vw->x)
			x0 = vw->x;
		if (x1 > vx1)
			x1 = vx1;
		if (y0 < vw->y)
			y0 = vw->y;
		if (y1 > vy1)
			y1 = vy1;
		o->bx0 = (int)x0;
		o->bx1 = (int)x1;
		o->by0 = (int)y0;
		o->by1 = (int)y1;
	}
	return 1;
}

static int ref_cmp(const void *pa, const void *pb) {
	const RefSplat *a = (const RefSplat *)pa;
	const RefSplat *b = (const RefSplat *)pb;
	if (a->dist < b->dist)
		return -1;
	if (a->dist > b->dist)
		return 1;
	return (a->index < b->index) ? -1 : (a->index > b->index);
}

typedef struct RefStats {
	size_t projected, refs, tests;
} RefStats;

static unsigned char quantize(double v) {
	if (v < 0.0)
		v = 0.0;
	if (v > 1.0)
		v = 1.0;
	return (unsigned char)floor(255.0 * v + 0.5);
}

/* Full independent render: returns a malloc'd copy of the whole buffer
 * with the splats composited over the supplied destination colours. */
static PIXEL *ref_render(const ZBuffer *dst, const TGLSplat *pts,
                         size_t count, const TGLSplatView *vw,
                         RefStats *st) {
	size_t npix = (size_t)dst->xsize * (size_t)dst->ysize;
	PIXEL *out = (PIXEL *)malloc(npix * sizeof(PIXEL));
	RefSplat *rs = (RefSplat *)malloc(count * sizeof(RefSplat) + 1);
	size_t n = 0, i;
	int tiles_x, tiles_y, tx, ty, px, py;

	if (out == NULL || rs == NULL) {
		free(out);
		free(rs);
		return NULL;
	}
	memcpy(out, dst->pbuf, npix * sizeof(PIXEL));

	for (i = 0; i < count; i++)
		if (ref_project(&pts[i], vw, &rs[n])) {
			rs[n].index = (int)i;
			n++;
		}
	qsort(rs, n, sizeof(*rs), ref_cmp);
	st->projected = n;

	tiles_x = (vw->width + TILE - 1) / TILE;
	tiles_y = (vw->height + TILE - 1) / TILE;
	st->refs = 0;
	st->tests = 0;

	/* Independent tile bookkeeping, identical 16x16 bucket rules. */
	for (i = 0; i < n; i++) {
		int tx0, tx1, ty0, ty1;
		if (rs[i].bx0 > rs[i].bx1 || rs[i].by0 > rs[i].by1)
			continue;
		tx0 = (rs[i].bx0 - vw->x) >> 4;
		tx1 = (rs[i].bx1 - vw->x) >> 4;
		ty0 = (rs[i].by0 - vw->y) >> 4;
		ty1 = (rs[i].by1 - vw->y) >> 4;
		st->refs += (size_t)(tx1 - tx0 + 1) * (size_t)(ty1 - ty0 + 1);
		for (ty = ty0; ty <= ty1; ty++)
			for (tx = tx0; tx <= tx1; tx++) {
				int px0 = vw->x + tx * TILE;
				int py0 = vw->y + ty * TILE;
				int px1 = px0 + TILE - 1;
				int py1 = py0 + TILE - 1;
				int cx0 = rs[i].bx0 > px0 ? rs[i].bx0 : px0;
				int cx1 = rs[i].bx1 < px1 ? rs[i].bx1 : px1;
				int cy0 = rs[i].by0 > py0 ? rs[i].by0 : py0;
				int cy1 = rs[i].by1 < py1 ? rs[i].by1 : py1;
				if (cx1 >= vw->x + vw->width)
					cx1 = vw->x + vw->width - 1;
				if (cy1 >= vw->y + vw->height)
					cy1 = vw->y + vw->height - 1;
				st->tests += (size_t)(cx1 - cx0 + 1)
				           * (size_t)(cy1 - cy0 + 1);
			}
	}

	/* Brute force per-pixel composite in back-to-front order. */
	for (py = vw->y; py < vw->y + vw->height; py++) {
		for (px = vw->x; px < vw->x + vw->width; px++) {
			double sum[3] = {0.0, 0.0, 0.0};
			double T = 1.0;
			size_t pix = (size_t)py * (size_t)dst->xsize + px;
			for (i = 0; i < n; i++) {
				double dx, dy, q, alpha;
				if (px < rs[i].bx0 || px > rs[i].bx1 ||
				    py < rs[i].by0 || py > rs[i].by1)
					continue;
				dx = (px + 0.5) - rs[i].cx;
				dy = (py + 0.5) - rs[i].cy;
				q = dx * (rs[i].inv[0][0] * dx + rs[i].inv[0][1] * dy)
				  + dy * (rs[i].inv[1][0] * dx + rs[i].inv[1][1] * dy);
				if (!(q <= QMAX))
					continue;
				alpha = rs[i].op * exp(-0.5 * q);
				if (alpha > 0.99)
					alpha = 0.99;
				if (!(alpha > 0.0))
					continue;
				if (dst->depth_test && rs[i].z < dst->zbuf[pix])
					continue;
				sum[0] += T * alpha * rs[i].r;
				sum[1] += T * alpha * rs[i].g;
				sum[2] += T * alpha * rs[i].b;
				T *= 1.0 - alpha;
			}
			if (T < 1.0) {
				PIXEL old = out[pix];
				double orig[3] = {
				    (double)GET_RED(old) / 255.0,
				    (double)GET_GREEN(old) / 255.0,
				    (double)GET_BLUE(old) / 255.0};
				out[pix] = (old & 0xFF000000u)
				         | ((PIXEL)quantize(sum[0] + T * orig[0]) << 16)
				         | ((PIXEL)quantize(sum[1] + T * orig[1]) << 8)
				         | (PIXEL)quantize(sum[2] + T * orig[2]);
			}
		}
	}
	free(rs);
	return out;
}

/* ------------------------------------------------------------------ */
/* buffer helpers                                                      */
/* ------------------------------------------------------------------ */

#define MARKER 0xA5u

static ZBuffer *new_buffer(int w, int h) {
	ZBuffer *z = ZB_open(w, h, ZB_MODE_RGBA, NULL);
	if (z)
		z->depth_test = 0;
	return z;
}

static void fill_background(ZBuffer *z, unsigned char r, unsigned char g,
                            unsigned char b) {
	int i;
	for (i = 0; i < z->xsize * z->ysize; i++)
		z->pbuf[i] = ((PIXEL)MARKER << 24) | ((PIXEL)r << 16)
		           | ((PIXEL)g << 8) | (PIXEL)b;
}

static PIXEL *snapshot(ZBuffer *z) {
	size_t n = (size_t)z->xsize * (size_t)z->ysize;
	PIXEL *p = (PIXEL *)malloc(n * sizeof(PIXEL));
	if (p)
		memcpy(p, z->pbuf, n * sizeof(PIXEL));
	return p;
}

static int buffers_match(const PIXEL *a, const PIXEL *b, int n,
                         int *exact, int *worst) {
	int i;
	*exact = 0;
	*worst = 0;
	for (i = 0; i < n; i++) {
		int dr = abs((int)GET_RED(a[i]) - (int)GET_RED(b[i]));
		int dg = abs((int)GET_GREEN(a[i]) - (int)GET_GREEN(b[i]));
		int db = abs((int)GET_BLUE(a[i]) - (int)GET_BLUE(b[i]));
		int d = dr;
		if (dg > d)
			d = dg;
		if (db > d)
			d = db;
		if (d > *worst)
			*worst = d;
		if (d == 0 && (a[i] & 0xFF000000u) == (b[i] & 0xFF000000u))
			(*exact)++;
		if (d > 1)
			return 0;
		if ((a[i] & 0xFF000000u) != (b[i] & 0xFF000000u))
			return 0;
	}
	return 1;
}

static int marker_intact(const ZBuffer *z) {
	int i;
	for (i = 0; i < z->xsize * z->ysize; i++)
		if (((z->pbuf[i] >> 24) & 0xff) != MARKER)
			return 0;
	return 1;
}

/* Compare the implementation against the brute force reference and the
 * reported statistics against the independently counted ones. */
static int check_render(ZBuffer *z, const TGLSplat *pts, size_t count,
                        const TGLSplatView *vw) {
	RefStats rs;
	TGLSplatStats st;
	PIXEL *expect;
	int exact, worst, npix = z->xsize * z->ysize;
	int rc;
	memset(&st, 0, sizeof(st));
	expect = ref_render(z, pts, count, vw, &rs);
	TASK_CHECK(expect != NULL);
	rc = tglRenderSplats(z, pts, count, vw, &st);
	TASK_CHECK(rc == 0);
	TASK_CHECK(buffers_match(expect, z->pbuf, npix, &exact, &worst));
	TASK_CHECK(exact * 10 >= npix * 9); /* at least 90% byte exact */
	TASK_CHECK(st.projected_count == rs.projected);
	TASK_CHECK(st.tile_refs == rs.refs);
	TASK_CHECK(st.pixel_tests == rs.tests);
	TASK_CHECK(marker_intact(z));
	free(expect);
	return 0;
}

/* ================================================================== */
/* projection group                                                    */
/* ================================================================== */

/* Hand computed orthographic anchor for the finite difference reference,
 * plus the analytic/FD agreement through a full render. */
static int case_proj_ortho_hand_anchor(void) {
	ZBuffer *z = new_buffer(64, 64);
	TGLSplatView vw;
	TGLSplat s;
	RefSplat o;
	RefStats rs;
	PIXEL *expect;
	TGLSplatStats st;
	double a = (64.0 - 0.5) * 0.5;
	TASK_CHECK(z != NULL);
	fill_background(z, 100, 100, 100);
	mat_identity(vw.view);
	mat_ortho(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 64;
	vw.height = 64;
	s = make_splat(0.1, -0.2, -6.0, 0.04, 0.0, 0.0, 0.09, 0.0, 0.05,
	               1.0, 0.0, 0.0, 1.0);
	TASK_CHECK(ref_project(&s, &vw, &o) == 1);

	/* ndc == eye under this ortho matrix; X = a*(x+1). */
	TASK_CHECK(fabs(o.cx - a * 1.1) < 1e-6);
	TASK_CHECK(fabs(o.cy - a * (1.0 - (-0.2))) < 1e-6);
	/* J columns: dX/dx = a, dY/dy = -a, z column must vanish. */
	TASK_CHECK(fabs(o.C[0][0] - (a * a * 0.04 + LOWPASS)) < 1e-5);
	TASK_CHECK(fabs(o.C[1][1] - (a * a * 0.09 + LOWPASS)) < 1e-5);
	TASK_CHECK(fabs(o.C[0][1]) < 1e-6);
	{
		/* Re-derive the Jacobian z column explicitly through FD. */
		double mc2[3] = {0.1, -0.2, -6.0 + 1e-6};
		double v4[4] = {mc2[0], mc2[1], mc2[2], 1.0};
		double clip2[4];
		mat_vec(vw.projection, v4, clip2);
		TASK_CHECK(fabs(clip2[0] - 0.1) < 1e-12);
		TASK_CHECK(fabs(clip2[1] - (-0.2)) < 1e-12);
	}
	TASK_CHECK(o.dist == 6.0);

	memset(&st, 0, sizeof(st));
	expect = ref_render(z, &s, 1, &vw, &rs);
	TASK_CHECK(expect != NULL);
	TASK_CHECK(tglRenderSplats(z, &s, 1, &vw, &st) == 0);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect, z->pbuf, 64 * 64, &exact, &worst));
		TASK_CHECK(worst <= 1);
	}
	TASK_CHECK(st.projected_count == 1);
	TASK_CHECK(marker_intact(z));
	free(expect);
	ZB_close(z);
	return 0;
}

/* Rotated anisotropic Gaussian under perspective: the screen covariance
 * must keep a nonzero off-diagonal and unequal axes, and every pixel must
 * match the finite-difference reference, including its touched mask. */
static int case_proj_persp_aniso_rotated(void) {
	ZBuffer *z = new_buffer(64, 64);
	TGLSplatView vw;
	TGLSplat s;
	RefSplat o;
	double R[3][3], t[3] = {0.1, -0.2, 0.3};
	double mc[3] = {0.0, 0.0, -6.0};
	double world[3];
	double theta = 0.6;
	int px, py, touched_ref = 0, touched_impl = 0, mismatches = 0;
	RefStats rs;
	PIXEL *expect, *before;
	TGLSplatStats st;

	TASK_CHECK(z != NULL);
	fill_background(z, 90, 120, 70);
	R[0][0] = cos(theta);
	R[0][1] = 0.0;
	R[0][2] = sin(theta);
	R[1][0] = 0.0;
	R[1][1] = 1.0;
	R[1][2] = 0.0;
	R[2][0] = -sin(theta);
	R[2][1] = 0.0;
	R[2][2] = cos(theta);
	mat_view(R, t, vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 64;
	vw.height = 64;

	world_from_camera(R, t, mc, world);
	s = make_splat(world[0], world[1], world[2],
	               0.18, 0.05, 0.02, 0.10, 0.03, 0.12,
	               0.2, 0.9, 0.4, 0.9);
	TASK_CHECK(ref_project(&s, &vw, &o) == 1);

	/* Anisotropy and correlation survive the projection. At z=-6 the
	 * perspective scale a/w ~ 5.3 turns a ~0.18 world variance into a
	 * few pixels of screen standard deviation. */
	TASK_CHECK(fabs(o.C[0][1]) > 1.0);
	TASK_CHECK(fabs(o.C[0][0] - o.C[1][1]) > 1.0);
	TASK_CHECK(sqrt(o.C[0][0]) > 1.5);

	before = snapshot(z);
	memset(&st, 0, sizeof(st));
	expect = ref_render(z, &s, 1, &vw, &rs);
	TASK_CHECK(expect != NULL);
	TASK_CHECK(tglRenderSplats(z, &s, 1, &vw, &st) == 0);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect, z->pbuf, 64 * 64, &exact, &worst));
		TASK_CHECK(worst <= 1);
		TASK_CHECK(exact > 64 * 64 - 8);
	}
	TASK_CHECK(st.tile_refs == rs.refs);
	TASK_CHECK(st.pixel_tests == rs.tests);

	/* Touched mask must agree pixel for pixel (diagonal ellipse shape). */
	for (py = 0; py < 64; py++)
		for (px = 0; px < 64; px++) {
			int ch_ref = memcmp(&expect[py * 64 + px],
			                    &before[py * 64 + px],
			                    sizeof(PIXEL)) != 0;
			int ch_impl = memcmp(&z->pbuf[py * 64 + px],
			                     &before[py * 64 + px],
			                     sizeof(PIXEL)) != 0;
			if (ch_ref)
				touched_ref++;
			if (ch_impl)
				touched_impl++;
			if (ch_ref != ch_impl)
				mismatches++;
		}
	TASK_CHECK(mismatches == 0);
	TASK_CHECK(touched_ref > 20);
	TASK_CHECK(touched_ref == touched_impl);

	/* The nonzero correlation makes the two diagonals asymmetric. At the
	 * 3 sigma boundary of the stretched diagonal the mirrored diagonal is
	 * already outside the ellipse. */
	{
		double a_plus = o.inv[0][0] + 2.0 * o.inv[0][1] + o.inv[1][1];
		double a_minus = o.inv[0][0] - 2.0 * o.inv[0][1] + o.inv[1][1];
		double a_small = a_plus < a_minus ? a_plus : a_minus;
		double a_large = a_plus < a_minus ? a_minus : a_plus;
		double d, qp, qm, id;
		TASK_CHECK(a_small > 0.0 && a_large > a_small);
		d = 3.0 / sqrt(a_small);
		qp = d * d * a_small;
		qm = d * d * a_large;
		TASK_CHECK(qp <= QMAX + 1e-9);
		TASK_CHECK(qm > QMAX);
		/* The same asymmetry appears at an integer pixel radius. */
		id = floor(d);
		{
			double iqp = id * id * a_plus;
			double iqm = id * id * a_minus;
			TASK_CHECK(fabs(iqp - iqm) > 1e-6);
			TASK_CHECK((iqp < iqm ? iqp : iqm) <= QMAX);
		}
	}
	free(expect);
	free(before);
	ZB_close(z);
	return 0;
}

/* Centre off the side of the viewport, but a wide tail still paints. */
static int case_proj_offscreen_tail(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView vw;
	TGLSplat s;
	RefSplat o;
	RefStats rs;
	PIXEL *expect, *before;
	TGLSplatStats st;
	int px, painted = 0;

	TASK_CHECK(z != NULL);
	fill_background(z, 40, 40, 40);
	mat_identity(vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;
	/* At z=-3 the half width of the view is 3 world units, so an eye x
	 * of -3.5 puts the centre off the left edge (negative screen x). A
	 * large x variance keeps its 3 sigma tail inside the viewport. */
	s = make_splat(-3.5, 0.0, -3.0, 0.35, 0.0, 0.0, 0.08, 0.0, 0.05,
	               0.1, 0.7, 1.0, 1.0);
	TASK_CHECK(ref_project(&s, &vw, &o) == 1);
	TASK_CHECK(o.cx < 0.0);
	TASK_CHECK(o.bx0 == 0);            /* clipped, not dropped on centre xy */
	TASK_CHECK(sqrt(o.C[0][0]) * 3.0 > -o.cx);

	before = snapshot(z);
	expect = ref_render(z, &s, 1, &vw, &rs);
	TASK_CHECK(expect != NULL);
	memset(&st, 0, sizeof(st));
	TASK_CHECK(tglRenderSplats(z, &s, 1, &vw, &st) == 0);
	TASK_CHECK(st.projected_count == 1);
	for (px = 0; px < 48; px++)
		if (memcmp(&z->pbuf[24 * 48 + px], &before[24 * 48 + px],
		           sizeof(PIXEL)) != 0)
			painted++;
	TASK_CHECK(painted > 0);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect, z->pbuf, 48 * 48, &exact, &worst));
		TASK_CHECK(worst <= 1);
	}
	free(expect);
	free(before);
	ZB_close(z);
	return 0;
}

/* A nearly degenerate covariance still yields the 0.3 px^2 footprint. */
static int case_proj_lowpass_footprint(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView vw;
	TGLSplat s;
	RefSplat o;
	TGLSplatStats st;
	PIXEL *expect, *before;
	RefStats rs;
	int px, py, changed = 0;

	TASK_CHECK(z != NULL);
	fill_background(z, 200, 200, 200);
	mat_identity(vw.view);
	mat_ortho(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;
	s = make_splat(0.0, 0.0, -5.0,
	               1e-9, 0.0, 0.0, 1e-9, 0.0, 1e-9,
	               0.0, 0.0, 0.0, 1.0);
	TASK_CHECK(ref_project(&s, &vw, &o) == 1);
	TASK_CHECK(o.C[0][0] >= LOWPASS - 1e-12);
	TASK_CHECK(o.C[1][1] >= LOWPASS - 1e-12);
	TASK_CHECK(fabs(o.C[0][1]) < 1e-9);

	before = snapshot(z);
	expect = ref_render(z, &s, 1, &vw, &rs);
	memset(&st, 0, sizeof(st));
	TASK_CHECK(tglRenderSplats(z, &s, 1, &vw, &st) == 0);
	for (py = 0; py < 48; py++)
		for (px = 0; px < 48; px++)
			if (memcmp(&z->pbuf[py * 48 + px], &before[py * 48 + px],
			           sizeof(PIXEL)) != 0)
				changed++;
	TASK_CHECK(changed >= 1);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect, z->pbuf, 48 * 48, &exact, &worst));
		TASK_CHECK(worst <= 1);
	}
	free(expect);
	free(before);
	ZB_close(z);
	return 0;
}

/* Centre rule: behind camera and at/outside the NDC z bounds are dropped
 * without touching the buffer. */
static int case_proj_center_rule_reject(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView vw;
	TGLSplat pts[4];
	TGLSplatStats st;
	PIXEL *before;
	int rc;

	TASK_CHECK(z != NULL);
	fill_background(z, 10, 20, 30);
	mat_identity(vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;

	pts[0] = make_splat(0.0, 0.0, 2.0, 0.05, 0, 0, 0.05, 0, 0.05,
	                    1, 0, 0, 1.0);                 /* w < 0 */
	pts[1] = make_splat(0.0, 0.0, -101.0, 0.05, 0, 0, 0.05, 0, 0.05,
	                    0, 1, 0, 1.0);               /* past far */
	pts[2] = make_splat(0.0, 0.0, -150.0, 0.05, 0, 0, 0.05, 0, 0.05,
	                    0, 0, 1, 1.0);               /* well past far */
	pts[3] = make_splat(0.0, 0.0, -1.0, 0.05, 0, 0, 0.05, 0, 0.05,
	                    1, 1, 0, 1.0);               /* ndc z == -1 */

	before = snapshot(z);
	memset(&st, 0xFF, sizeof(st));
	rc = tglRenderSplats(z, pts, 4, &vw, &st);
	TASK_CHECK(rc == 0);
	TASK_CHECK(st.projected_count == 0);
	TASK_CHECK(st.tile_refs == 0);
	TASK_CHECK(st.pixel_tests == 0);
	TASK_CHECK(memcmp(before, z->pbuf,
	                  48 * 48 * sizeof(PIXEL)) == 0);
	free(before);
	ZB_close(z);
	return 0;
}

/* ================================================================== */
/* compositing group                                                   */
/* ================================================================== */

/* Overlapping Gaussians: floating point accumulation, one quantisation. */
static int case_comp_overlap_float(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView vw;
	TGLSplat pts[3];
	TGLSplatStats st;
	RefStats rs;
	PIXEL *expect;
	int exact, worst;

	TASK_CHECK(z != NULL);
	fill_background(z, 100, 100, 100);
	mat_identity(vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;

	pts[0] = make_splat(-0.05, 0.0, -6.0, 0.06, 0.01, 0, 0.06, 0, 0.04,
	                    1.0, 0.0, 0.0, 0.7);
	pts[1] = make_splat(0.05, 0.0, -5.0, 0.07, 0.0, 0.01, 0.05, 0.01, 0.04,
	                    0.0, 1.0, 0.0, 0.6);
	pts[2] = make_splat(0.0, 0.05, -4.0, 0.05, 0.0, 0, 0.05, 0, 0.04,
	                    0.0, 0.0, 1.0, 0.5);

	expect = ref_render(z, pts, 3, &vw, &rs);
	TASK_CHECK(expect != NULL);
	memset(&st, 0, sizeof(st));
	TASK_CHECK(tglRenderSplats(z, pts, 3, &vw, &st) == 0);
	TASK_CHECK(buffers_match(expect, z->pbuf, 48 * 48, &exact, &worst));
	TASK_CHECK(worst <= 1);
	TASK_CHECK(st.projected_count == 3);

	/* Hand value at the centre pixel using the reference's own q. */
	{
		RefSplat o[3];
		size_t k;
		double sum[3] = {0, 0, 0}, T = 1.0;
		int cpx = 24, cpy = 24;
		unsigned char rr, gg, bb;
		PIXEL got;
		for (k = 0; k < 3; k++)
			TASK_CHECK(ref_project(&pts[k], &vw, &o[k]) == 1);
		/* nearest first: index 2, 1, 0 */
		for (k = 0; k < 3; k++) {
			int order[3] = {2, 1, 0};
			const RefSplat *p = &o[order[k]];
			double dx = (cpx + 0.5) - p->cx;
			double dy = (cpy + 0.5) - p->cy;
			double q = dx * (p->inv[0][0] * dx + p->inv[0][1] * dy)
			         + dy * (p->inv[1][0] * dx + p->inv[1][1] * dy);
			double alpha = p->op * exp(-0.5 * q);
			if (alpha > 0.99)
				alpha = 0.99;
			sum[0] += T * alpha * p->r;
			sum[1] += T * alpha * p->g;
			sum[2] += T * alpha * p->b;
			T *= 1.0 - alpha;
		}
		rr = quantize(sum[0] + T * (100.0 / 255.0));
		gg = quantize(sum[1] + T * (100.0 / 255.0));
		bb = quantize(sum[2] + T * (100.0 / 255.0));
		got = z->pbuf[cpy * 48 + cpx];
		TASK_CHECK(abs((int)GET_RED(got) - (int)rr) <= 1);
		TASK_CHECK(abs((int)GET_GREEN(got) - (int)gg) <= 1);
		TASK_CHECK(abs((int)GET_BLUE(got) - (int)bb) <= 1);
	}
	free(expect);
	ZB_close(z);
	return 0;
}

/* Equal camera distance: input index decides the paint order. */
static int case_comp_equal_distance_order(void) {
	ZBuffer *za = new_buffer(48, 48);
	ZBuffer *zb = new_buffer(48, 48);
	TGLSplatView vw;
	TGLSplat ab[2], ba[2];
	TGLSplat red, blue;
	int center;

	TASK_CHECK(za != NULL && zb != NULL);
	fill_background(za, 80, 80, 80);
	fill_background(zb, 80, 80, 80);
	mat_identity(vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;

	red = make_splat(0.0, 0.0, -6.0, 0.06, 0, 0, 0.06, 0, 0.04,
	                 1.0, 0.0, 0.0, 0.8);
	blue = make_splat(0.0, 0.0, -6.0, 0.06, 0, 0, 0.06, 0, 0.04,
	                  0.0, 0.0, 1.0, 0.8);
	ab[0] = red;
	ab[1] = blue;
	ba[0] = blue;
	ba[1] = red;

	TASK_CHECK(tglRenderSplats(za, ab, 2, &vw, NULL) == 0);
	TASK_CHECK(tglRenderSplats(zb, ba, 2, &vw, NULL) == 0);
	center = 24 * 48 + 24;
	/* Front to back accumulation (T *= 1-alpha) lets the first splat
	 * dominate, so the lower index wins: red in ab, blue in ba. */
	TASK_CHECK(GET_RED(za->pbuf[center]) > GET_BLUE(za->pbuf[center]));
	TASK_CHECK(GET_BLUE(zb->pbuf[center]) > GET_RED(zb->pbuf[center]));
	TASK_CHECK(za->pbuf[center] != zb->pbuf[center]);

	/* Each ordering still matches the independently sorted reference. */
	{
		RefStats rs;
		PIXEL *ea = ref_render(za, ab, 2, &vw, &rs);
		PIXEL *eb = ref_render(zb, ba, 2, &vw, &rs);
		int exact, worst;
		TASK_CHECK(ea != NULL && eb != NULL);
		/* Reference renders over current colours; rebuild from the same
		 * grey background instead so the comparison is self contained. */
		fill_background(za, 80, 80, 80);
		fill_background(zb, 80, 80, 80);
		free(ea);
		free(eb);
		ea = ref_render(za, ab, 2, &vw, &rs);
		eb = ref_render(zb, ba, 2, &vw, &rs);
		TASK_CHECK(tglRenderSplats(za, ab, 2, &vw, NULL) == 0);
		TASK_CHECK(tglRenderSplats(zb, ba, 2, &vw, NULL) == 0);
		TASK_CHECK(buffers_match(ea, za->pbuf, 48 * 48, &exact, &worst));
		TASK_CHECK(worst <= 1);
		TASK_CHECK(buffers_match(eb, zb->pbuf, 48 * 48, &exact, &worst));
		TASK_CHECK(worst <= 1);
		free(ea);
		free(eb);
	}
	ZB_close(za);
	ZB_close(zb);
	return 0;
}

/* Real triangle occlusion and an untouched depth buffer. */
static int case_comp_triangle_occlusion_depth(void) {
	ZBuffer *z = ZB_open(64, 64, ZB_MODE_RGBA, NULL);
	TGLSplatView vw;
	TGLSplat behind, front, side;
	TGLSplatStats st;
	GLushort *zbefore;
	PIXEL color_before;
	int center = 32 * 64 + 32; /* screen centre, inside the big triangle */
	int sidepix = 32 * 64 + 4; /* far left, outside the triangle */
	int i;

	TASK_CHECK(z != NULL);
	glInit(z);
	glViewport(0, 0, 64, 64);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	/* Same frustum the splat view uses below (each in its own storage
	 * convention). */
	glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_LIGHTING);
	glDisable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 1, 0);
	glBegin(GL_TRIANGLES);
	glVertex3f(-0.9f, -0.9f, -4.0f);
	glVertex3f(0.9f, -0.9f, -4.0f);
	glVertex3f(0.0f, 0.9f, -4.0f);
	glEnd();
	TASK_CHECK(GET_GREEN(z->pbuf[center]) > 200);
	TASK_CHECK(GET_GREEN(z->pbuf[sidepix]) == 0);

	/* Marker byte must survive the splat pass. */
	for (i = 0; i < 64 * 64; i++)
		z->pbuf[i] |= (PIXEL)MARKER << 24;
	color_before = z->pbuf[center];
	zbefore = (GLushort *)malloc(64 * 64 * sizeof(GLushort));
	TASK_CHECK(zbefore != NULL);
	memcpy(zbefore, z->zbuf, 64 * 64 * sizeof(GLushort));

	mat_identity(vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 64;
	vw.height = 64;

	/* Triangle at eye z=-4. A splat at -6 is farther (occluded at the
	 * centre), one at -2 is nearer (paints over it). */
	behind = make_splat(0.0, 0.0, -6.0, 0.02, 0, 0, 0.02, 0, 0.02,
	                    1.0, 0.0, 0.0, 1.0);
	front = make_splat(0.0, 0.0, -2.0, 0.02, 0, 0, 0.02, 0, 0.02,
	                   0.0, 0.0, 1.0, 1.0);
	/* Far left, outside the triangle silhouette but inside the frustum. */
	side = make_splat(-2.6, 0.0, -3.0, 0.05, 0, 0, 0.02, 0, 0.02,
	                  1.0, 1.0, 1.0, 1.0);
	{
		TGLSplat pts[3] = {behind, front, side};
		memset(&st, 0, sizeof(st));
		TASK_CHECK(tglRenderSplats(z, pts, 3, &vw, &st) == 0);
		TASK_CHECK(st.projected_count == 3);
	}

	/* The behind splat is hidden at the centre, but the front one paints;
	 * the net centre colour therefore differs from the plain triangle. */
	TASK_CHECK(z->pbuf[center] != color_before);
	TASK_CHECK(GET_BLUE(z->pbuf[center]) > 0);
	/* Side splat lands outside the triangle and composites over black. */
	TASK_CHECK(GET_RED(z->pbuf[sidepix]) > 0 ||
	           GET_GREEN(z->pbuf[sidepix]) > 0 ||
	           GET_BLUE(z->pbuf[sidepix]) > 0);
	/* Depth storage is byte-for-byte unchanged and centre not written. */
	TASK_CHECK(memcmp(zbefore, z->zbuf,
	                  64 * 64 * sizeof(GLushort)) == 0);
	TASK_CHECK(marker_intact(z));

	/* A render with only the behind splat leaves the centre untouched. */
	{
		PIXEL saved = z->pbuf[center];
		TGLSplat one[1] = {behind};
		TASK_CHECK(tglRenderSplats(z, one, 1, &vw, NULL) == 0);
		TASK_CHECK(z->pbuf[center] == saved);
	}
	free(zbefore);
	glClose();
	ZB_close(z);
	return 0;
}

/* ================================================================== */
/* tiles group                                                         */
/* ================================================================== */

/* Partial edge tiles and a nonzero viewport origin; outside pixels stay
 * untouched, inside pixels match the tile-free reference. */
static int case_tile_partial_viewport_origin(void) {
	ZBuffer *z = new_buffer(64, 64);
	TGLSplatView vw;
	TGLSplat pts[4];
	TGLSplatStats st;
	RefStats rs;
	PIXEL *expect;
	int px, py, outside_ok = 1;

	TASK_CHECK(z != NULL);
	fill_background(z, 110, 90, 60);
	mat_identity(vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 5;
	vw.y = 7;
	vw.width = 33;   /* 3 full tiles plus a 1 px partial column */
	vw.height = 29;  /* 1 full tile plus a 13 px partial row */

	pts[0] = make_splat(-0.6, 0.5, -6.0, 0.12, 0.02, 0, 0.10, 0, 0.05,
	                    1, 0, 0, 0.9);
	pts[1] = make_splat(0.6, -0.5, -5.5, 0.10, 0, 0.01, 0.12, 0.02, 0.05,
	                    0, 1, 0, 0.8);
	pts[2] = make_splat(0.0, 0.0, -4.5, 0.08, 0.03, 0, 0.08, 0, 0.04,
	                    0, 0, 1, 0.85);
	pts[3] = make_splat(0.2, 0.2, -7.0, 0.05, 0, 0, 0.05, 0, 0.04,
	                    1, 1, 0, 0.5);

	expect = ref_render(z, pts, 4, &vw, &rs);
	TASK_CHECK(expect != NULL);
	memset(&st, 0, sizeof(st));
	TASK_CHECK(tglRenderSplats(z, pts, 4, &vw, &st) == 0);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect, z->pbuf, 64 * 64, &exact, &worst));
		TASK_CHECK(worst <= 1);
	}
	TASK_CHECK(st.projected_count == rs.projected);
	TASK_CHECK(st.tile_refs == rs.refs);
	TASK_CHECK(st.pixel_tests == rs.tests);

	for (py = 0; py < 64; py++)
		for (px = 0; px < 64; px++) {
			int inside = px >= vw.x && px < vw.x + vw.width
			          && py >= vw.y && py < vw.y + vw.height;
			if (!inside && z->pbuf[py * 64 + px] != expect[py * 64 + px])
				outside_ok = 0;
		}
	TASK_CHECK(outside_ok);
	TASK_CHECK(marker_intact(z));
	free(expect);
	ZB_close(z);
	return 0;
}

/* Explicit, hand counted tile and pixel statistics. */
static int case_tile_stats_counts(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView vw;
	TGLSplat pts[3];
	TGLSplatStats st;
	RefSplat o;
	size_t expect_refs = 0, expect_tests = 0;
	int i;

	TASK_CHECK(z != NULL);
	fill_background(z, 0, 0, 0);
	mat_identity(vw.view);
	mat_ortho(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;

	/* Three compact splats in distinct places, one rejected. */
	pts[0] = make_splat(-0.5, 0.0, -4.0, 0.002, 0, 0, 0.002, 0, 0.002,
	                    1, 0, 0, 1);
	pts[1] = make_splat(0.5, 0.0, -4.0, 0.002, 0, 0, 0.002, 0, 0.002,
	                    0, 1, 0, 1);
	pts[2] = make_splat(0.0, 0.0, 2.0, 0.002, 0, 0, 0.002, 0, 0.002,
	                    0, 0, 1, 1); /* behind camera */

	memset(&st, 0, sizeof(st));
	TASK_CHECK(tglRenderSplats(z, pts, 3, &vw, &st) == 0);
	TASK_CHECK(st.projected_count == 2);

	for (i = 0; i < 2; i++) {
		int tx0, tx1, ty0, ty1, tx, ty;
		TASK_CHECK(ref_project(&pts[i], &vw, &o) == 1);
		tx0 = o.bx0 >> 4;
		tx1 = o.bx1 >> 4;
		ty0 = o.by0 >> 4;
		ty1 = o.by1 >> 4;
		expect_refs += (size_t)(tx1 - tx0 + 1) * (ty1 - ty0 + 1);
		for (ty = ty0; ty <= ty1; ty++)
			for (tx = tx0; tx <= tx1; tx++) {
				int px0 = tx * 16, py0 = ty * 16;
				int cx0 = o.bx0 > px0 ? o.bx0 : px0;
				int cx1 = o.bx1 < px0 + 15 ? o.bx1 : px0 + 15;
				int cy0 = o.by0 > py0 ? o.by0 : py0;
				int cy1 = o.by1 < py0 + 15 ? o.by1 : py0 + 15;
				expect_tests += (size_t)(cx1 - cx0 + 1)
				              * (cy1 - cy0 + 1);
			}
	}
	TASK_CHECK(st.tile_refs == expect_refs);
	TASK_CHECK(st.pixel_tests == expect_tests);
	TASK_CHECK(st.tile_refs >= 2);
	ZB_close(z);
	return 0;
}

/* A splat straddling four tiles renders identically to one fully inside a
 * single tile, with no seam along the tile boundaries. */
static int case_tile_cross_tile_seam(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView vw;
	TGLSplat big, small;
	RefStats rs;
	PIXEL *expect;
	TGLSplatStats st;
	int px, py, seam_mismatch = 0;

	TASK_CHECK(z != NULL);
	fill_background(z, 128, 128, 128);
	mat_identity(vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;

	/* Put the centre on the tile corner at screen (16,16): with a=23.75
	 * and w=8, eye x = w*(16/a-1), eye y = w*(1-16/a). */
	{
		double ex = 8.0 * (16.0 / 23.75 - 1.0);
		double ey = 8.0 * (1.0 - 16.0 / 23.75);
		big = make_splat(ex, ey, -8.0, 0.30, 0.05, 0.02, 0.30, 0.0, 0.10,
		                 0.9, 0.3, 0.6, 0.95);
	}
	small = make_splat(0.0, 0.0, -8.0, 0.001, 0, 0, 0.001, 0, 0.001,
	                   0.9, 0.3, 0.6, 0.95);

	expect = ref_render(z, &big, 1, &vw, &rs);
	TASK_CHECK(expect != NULL);
	TASK_CHECK(rs.refs >= 4); /* crosses at least four tiles */
	memset(&st, 0, sizeof(st));
	TASK_CHECK(tglRenderSplats(z, &big, 1, &vw, &st) == 0);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect, z->pbuf, 48 * 48, &exact, &worst));
		TASK_CHECK(worst <= 1);
	}
	for (py = 14; py <= 18; py++)
		for (px = 14; px <= 18; px++) {
			int d = abs((int)GET_RED(z->pbuf[py * 48 + px])
			            - (int)GET_RED(expect[py * 48 + px]));
			if (d > 1)
				seam_mismatch++;
		}
	TASK_CHECK(seam_mismatch == 0);
	free(expect);

	/* The tiny splat occupies one tile and still renders. */
	fill_background(z, 128, 128, 128);
	expect = ref_render(z, &small, 1, &vw, &rs);
	memset(&st, 0, sizeof(st));
	TASK_CHECK(tglRenderSplats(z, &small, 1, &vw, &st) == 0);
	TASK_CHECK(st.tile_refs == 1);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect, z->pbuf, 48 * 48, &exact, &worst));
		TASK_CHECK(worst <= 1);
	}
	free(expect);
	ZB_close(z);
	return 0;
}

/* Every validation failure returns non zero and leaves both buffers
 * byte-for-byte intact. */
static int case_tile_input_failures(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView vw, badview;
	TGLSplat good, bad;
	TGLSplat pts[2];
	PIXEL *pbefore;
	GLushort *zbefore;
	int rc;

	TASK_CHECK(z != NULL);
	fill_background(z, 50, 60, 70);
	mat_identity(vw.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;
	good = make_splat(0, 0, -5, 0.05, 0, 0, 0.05, 0, 0.05, 1, 1, 1, 0.8);

	pbefore = snapshot(z);
	zbefore = (GLushort *)malloc(48 * 48 * sizeof(GLushort));
	TASK_CHECK(pbefore != NULL && zbefore != NULL);
	memcpy(zbefore, z->zbuf, 48 * 48 * sizeof(GLushort));

	rc = tglRenderSplats(NULL, &good, 1, &vw, NULL);
	TASK_CHECK(rc != 0);
	rc = tglRenderSplats(z, &good, 1, NULL, NULL);
	TASK_CHECK(rc != 0);
	rc = tglRenderSplats(z, NULL, 1, &vw, NULL);
	TASK_CHECK(rc != 0);

	badview = vw;
	badview.x = -1;
	rc = tglRenderSplats(z, &good, 1, &badview, NULL);
	TASK_CHECK(rc != 0);
	badview = vw;
	badview.width = 49; /* extends past the 48 px buffer */
	rc = tglRenderSplats(z, &good, 1, &badview, NULL);
	TASK_CHECK(rc != 0);
	badview = vw;
	badview.view[0] = (double)NAN;
	rc = tglRenderSplats(z, &good, 1, &badview, NULL);
	TASK_CHECK(rc != 0);

	bad = good;
	bad.mean[1] = (double)NAN;
	rc = tglRenderSplats(z, &bad, 1, &vw, NULL);
	TASK_CHECK(rc != 0);
	bad = good;
	bad.rgb[0] = 1.5;
	rc = tglRenderSplats(z, &bad, 1, &vw, NULL);
	TASK_CHECK(rc != 0);
	bad = good;
	bad.opacity = -0.1;
	rc = tglRenderSplats(z, &bad, 1, &vw, NULL);
	TASK_CHECK(rc != 0);
	bad = good;
	bad.covariance[0] = -0.05; /* negative diagonal */
	rc = tglRenderSplats(z, &bad, 1, &vw, NULL);
	TASK_CHECK(rc != 0);
	bad = make_splat(0, 0, -5, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1);
	rc = tglRenderSplats(z, &bad, 1, &vw, NULL); /* semi-definite */
	TASK_CHECK(rc != 0);
	bad = good;
	bad.covariance[5] = (double)INFINITY;
	rc = tglRenderSplats(z, &bad, 1, &vw, NULL);
	TASK_CHECK(rc != 0);

	/* One bad apple in a batch aborts the whole call before painting. */
	pts[0] = good;
	pts[1] = bad;
	rc = tglRenderSplats(z, pts, 2, &vw, NULL);
	TASK_CHECK(rc != 0);

	TASK_CHECK(memcmp(pbefore, z->pbuf,
	                  48 * 48 * sizeof(PIXEL)) == 0);
	TASK_CHECK(memcmp(zbefore, z->zbuf,
	                  48 * 48 * sizeof(GLushort)) == 0);
	free(pbefore);
	free(zbefore);
	ZB_close(z);
	return 0;
}

/* count == 0 is legal and stats are zeroed with the buffer untouched. */
static int case_tile_count_zero(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView vw;
	TGLSplatStats st;
	PIXEL *before;

	TASK_CHECK(z != NULL);
	fill_background(z, 1, 2, 3);
	mat_identity(vw.view);
	mat_identity(vw.projection);
	vw.x = 0;
	vw.y = 0;
	vw.width = 48;
	vw.height = 48;
	before = snapshot(z);
	memset(&st, 0xFF, sizeof(st));
	TASK_CHECK(tglRenderSplats(z, NULL, 0, &vw, &st) == 0);
	TASK_CHECK(st.projected_count == 0);
	TASK_CHECK(st.tile_refs == 0);
	TASK_CHECK(st.pixel_tests == 0);
	TASK_CHECK(memcmp(before, z->pbuf,
	                  48 * 48 * sizeof(PIXEL)) == 0);
	TASK_CHECK(tglRenderSplats(z, NULL, 0, NULL, NULL) != 0);
	free(before);
	ZB_close(z);
	return 0;
}

/* Two calls with changed views: the second pass recomposites over the
 * current colours and never keeps stale per-view state. */
static int case_tile_two_view_changes(void) {
	ZBuffer *z = new_buffer(48, 48);
	TGLSplatView va, vb;
	TGLSplat pts[2];
	TGLSplatStats sa, sb;
	RefStats rs;
	PIXEL *expect_a, *expect_b;
	int px, py, stale = 0;

	TASK_CHECK(z != NULL);
	fill_background(z, 70, 70, 90);
	mat_identity(va.view);
	mat_perspective(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0, va.projection);
	va.x = 0;
	va.y = 0;
	va.width = 48;
	va.height = 48;

	pts[0] = make_splat(0.0, 0.0, -6.0, 0.06, 0.02, 0, 0.06, 0, 0.04,
	                    1, 0, 0, 0.9);
	pts[1] = make_splat(0.3, 0.2, -5.0, 0.05, 0, 0, 0.05, 0, 0.04,
	                    0, 1, 1, 0.8);

	expect_a = ref_render(z, pts, 2, &va, &rs);
	TASK_CHECK(expect_a != NULL);
	memset(&sa, 0, sizeof(sa));
	TASK_CHECK(tglRenderSplats(z, pts, 2, &va, &sa) == 0);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect_a, z->pbuf, 48 * 48, &exact, &worst));
		TASK_CHECK(worst <= 1);
	}

	/* Shift the camera sideways so every splat moves on screen. */
	vb = va;
	vb.view[12] = 0.45; /* camera translation column, x component */
	expect_b = ref_render(z, pts, 2, &vb, &rs);
	TASK_CHECK(expect_b != NULL);
	memset(&sb, 0, sizeof(sb));
	TASK_CHECK(tglRenderSplats(z, pts, 2, &vb, &sb) == 0);
	{
		int exact, worst;
		TASK_CHECK(buffers_match(expect_b, z->pbuf, 48 * 48, &exact, &worst));
		TASK_CHECK(worst <= 1);
	}
	TASK_CHECK(sa.projected_count == sb.projected_count);

	/* Pixels only view A covered must keep their view A result: pass B
	 * must not erase or repaint where its splats do not reach. */
	for (py = 0; py < 48; py++)
		for (px = 0; px < 48; px++) {
			int a_changed = expect_a[py * 48 + px]
			              != ((PIXEL)MARKER << 24
			                  | ((PIXEL)70 << 16) | ((PIXEL)70 << 8)
			                  | (PIXEL)90);
			int b_changed = expect_b[py * 48 + px] != expect_a[py * 48 + px];
			if (a_changed && !b_changed &&
			    z->pbuf[py * 48 + px] != expect_a[py * 48 + px])
				stale++;
		}
	TASK_CHECK(stale == 0);
	TASK_CHECK(marker_intact(z));
	free(expect_a);
	free(expect_b);
	ZB_close(z);
	return 0;
}

/* ------------------------------------------------------------------ */
/* case table                                                          */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"projection", "ortho_hand_anchor", case_proj_ortho_hand_anchor},
    {"projection", "persp_aniso_rotated", case_proj_persp_aniso_rotated},
    {"projection", "offscreen_tail", case_proj_offscreen_tail},
    {"projection", "lowpass_footprint", case_proj_lowpass_footprint},
    {"projection", "center_rule_reject", case_proj_center_rule_reject},

    {"compositing", "overlap_float", case_comp_overlap_float},
    {"compositing", "equal_distance_order", case_comp_equal_distance_order},
    {"compositing", "triangle_occlusion_depth",
     case_comp_triangle_occlusion_depth},

    {"tiles", "partial_viewport_origin", case_tile_partial_viewport_origin},
    {"tiles", "stats_counts", case_tile_stats_counts},
    {"tiles", "cross_tile_seam", case_tile_cross_tile_seam},
    {"tiles", "input_failures", case_tile_input_failures},
    {"tiles", "count_zero", case_tile_count_zero},
    {"tiles", "two_view_changes", case_tile_two_view_changes},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
