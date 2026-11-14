//=====================================================================
//
// motion.c - continuous-shutter rendering for moving triangles
//
// The exposure t in [0,1] is split at every event that can change
// what a pixel sees:
//
//   1. an edge line crosses the pixel centre (quadratic in t);
//   2. the projected triangle changes orientation / degenerates
//      (signed area A(t) == 0, quadratic in t);
//   3. two covering triangles exchange the window-z interpolated at
//      this pixel centre.  Barycentric depth is a cubic-over-quadratic
//      rational function, so equality of two faces is a polynomial of
//      degree up to 5; every real root in (0,1) is isolated with a
//      Sturm sequence, never by dense sampling.
//
// Inside each resulting interval the set of covering faces and the
// front face are constant, so the visible colour (barycentric vertex
// colour interpolation, not a face average) is integrated with an
// error-controlled adaptive Simpson rule.
//
//=====================================================================
#include "motion.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Quadratic polynomial a*t^2 + b*t + c in double precision.
typedef struct { double a, b, c; } dpoly2;

typedef struct { double x, y, z; } dvec3;

// Generic real polynomial c[0] + c[1]*t + ... + c[n]*t^n.
typedef struct { int n; double c[8]; } dpoly;

// One triangle projected for the whole exposure: screen x/y and
// window-z of each vertex are linear in t and stored as quadratics.
typedef struct {
	dpoly2 px[3], py[3], pz[3];
	double cr[3], cg[3], cb[3];      // vertex colours, constant in t
} mt_face;

#define MT_AREA_EPS     1e-11        // whole-exposure zero area threshold
#define MT_ROOT_EPS     1e-12        // event roots kept strictly inside (0,1)
#define MT_MERGE_EPS    1e-10        // simultaneous-event merging tolerance
#define MT_BARY_EPS     1e-12        // pixel centre sitting on an edge
#define MT_EDGE_EPS     1e-12        // horizontal-edge test
#define MT_Z_EPS        1e-12        // exact depth tie tolerance
#define MT_INT_EPS      1e-10        // per-interval integration tolerance
#define MT_POLY_EPS     1e-12        // polynomial coefficient / zero test

static double dp2_eval(const dpoly2 *p, double t) {
	return (p->a * t + p->b) * t + p->c;
}

static dpoly2 dp2_sub(dpoly2 p, const dpoly2 *q) {
	p.a -= q->a;
	p.b -= q->b;
	p.c -= q->c;
	return p;
}

// 2-D cross product of two vectors whose components are linear in t:
// u = u0 + u1*t, v = v0 + v1*t with the constant/linear parts held
// in .c / .b of each quadratic.
static dpoly2 cross2_poly(const dpoly2 *ux, const dpoly2 *uy,
	const dpoly2 *vx, const dpoly2 *vy) {
	dpoly2 r;
	r.a = ux->b * vy->b - uy->b * vx->b;
	r.b = ux->c * vy->b + ux->b * vy->c
	    - uy->c * vx->b - uy->b * vx->c;
	r.c = ux->c * vy->c - uy->c * vx->c;
	return r;
}

// Real roots of a quadratic; returns 0..2 roots in ascending order.
// Tangential double roots are returned once.
static int dp2_roots(const dpoly2 *p, double out[2]) {
	double a = p->a, b = p->b, c = p->c;
	double scale = fmax(fmax(fabs(a), fabs(b)), fabs(c));
	if (scale == 0.0) return 0;
	a /= scale;
	b /= scale;
	c /= scale;
	if (fabs(a) <= 1e-14) {
		if (fabs(b) <= 1e-14) return 0;
		out[0] = -c / b;
		return 1;
	}
	double disc = b * b - 4.0 * a * c;
	if (disc < 0.0) {
		if (disc > -1e-12 * (1.0 + b * b)) disc = 0.0;
		else return 0;
	}
	if (disc == 0.0) {
		out[0] = -0.5 * b / a;
		return 1;
	}
	double s = sqrt(disc);
	double q = -0.5 * (b + (b >= 0.0 ? s : -s));
	double t1 = q / a;
	double t2 = (fabs(q) > 1e-300) ? c / q : -0.5 * b / a;
	if (t1 > t2) { double h = t1; t1 = t2; t2 = h; }
	out[0] = t1;
	out[1] = t2;
	return 2;
}

// ---- generic polynomials -------------------------------------------------

static double dp_eval(const dpoly *p, double t) {
	double r = 0.0;
	int i;
	for (i = p->n; i >= 0; i--) r = r * t + p->c[i];
	return r;
}

static void dp_trim(dpoly *p) {
	double mx = 0.0;
	int i;
	for (i = 0; i <= p->n; i++)
		if (fabs(p->c[i]) > mx) mx = fabs(p->c[i]);
	if (mx == 0.0) { p->n = 0; return; }
	while (p->n > 0 && fabs(p->c[p->n]) <= MT_POLY_EPS * mx)
		p->n--;
}

static int dp_is_zero(const dpoly *p) {
	double mx = 0.0;
	int i;
	for (i = 0; i <= p->n; i++)
		if (fabs(p->c[i]) > mx) mx = fabs(p->c[i]);
	return mx == 0.0;
}

static dpoly dp_derivative(const dpoly *p) {
	dpoly r;
	int i;
	r.n = p->n - 1;
	if (r.n < 0) r.n = 0;
	for (i = 1; i <= p->n; i++) r.c[i - 1] = i * p->c[i];
	for (; i < 8; i++) r.c[i] = 0.0;
	dp_trim(&r);
	return r;
}

// -(a mod b): the next Sturm sequence member.  A remainder whose
// coefficients are below 1e-13 of the operands' scale is treated as
// exact zero so the chain terminates instead of carrying round-off.
static dpoly dp_neg_remainder(const dpoly *a, const dpoly *b) {
	dpoly r;
	int i, j;
	double scale = 0.0, rmax = 0.0;
	for (i = 0; i < 8; i++) r.c[i] = 0.0;
	r.n = a->n;
	for (i = 0; i <= a->n; i++) {
		r.c[i] = a->c[i];
		if (fabs(a->c[i]) > scale) scale = fabs(a->c[i]);
	}
	for (i = 0; i <= b->n; i++)
		if (fabs(b->c[i]) > scale) scale = fabs(b->c[i]);
	int db = b->n;
	for (i = a->n - db; i >= 0; i--) {
		double q = r.c[i + db] / b->c[db];
		for (j = 0; j <= db; j++) r.c[i + j] -= q * b->c[j];
	}
	for (i = 0; i < db; i++)
		if (fabs(r.c[i]) > rmax) rmax = fabs(r.c[i]);
	if (scale > 0.0 && rmax <= 1e-13 * scale) {
		for (i = 0; i < 8; i++) r.c[i] = 0.0;
		r.n = 0;
		return r;
	}
	r.n = db - 1;
	if (r.n < 0) r.n = 0;
	for (i = 0; i <= r.n; i++) r.c[i] = -r.c[i];
	dp_trim(&r);
	return r;
}

static dpoly dp_multiply(const dpoly *a, const dpoly *b) {
	dpoly r;
	int i, j;
	for (i = 0; i < 8; i++) r.c[i] = 0.0;
	r.n = a->n + b->n;
	for (i = 0; i <= a->n; i++)
		for (j = 0; j <= b->n; j++)
			r.c[i + j] += a->c[i] * b->c[j];
	dp_trim(&r);
	return r;
}

static dpoly dp_sub_generic(dpoly a, const dpoly *b) {
	int i;
	for (i = 0; i <= b->n; i++) a.c[i] -= b->c[i];
	if (b->n > a.n) a.n = b->n;
	dp_trim(&a);
	return a;
}

// Sturm sign-variation count at t (zero values are skipped).
static int sturm_variations(const dpoly *seq, int len, double t) {
	int count = 0, i;
	int prev = 0;
	for (i = 0; i < len; i++) {
		double v = dp_eval(&seq[i], t);
		int s;
		if (v == 0.0) continue;
		s = (v > 0.0) ? 1 : -1;
		if (prev != 0 && s != prev) count++;
		prev = s;
	}
	return count;
}

static void sturm_isolate(const dpoly *seq, int len, double lo, double hi,
	int nroots, double *roots, int *nroots_out);

// All distinct real roots of p strictly inside (0,1).  Even-multiplicity
// roots are found: Sturm counts distinct roots regardless of multiplicity.
static int dp_roots_unit(const dpoly *p, double *roots) {
	dpoly seq[8];
	dpoly q;
	int len = 0, i;
	double scale = 0.0;
	for (i = 0; i <= p->n; i++)
		if (fabs(p->c[i]) > scale) scale = fabs(p->c[i]);
	if (scale == 0.0) return 0;
	seq[len++] = *p;
	dp_trim(&seq[0]);
	if (seq[0].n == 0) return 0;
	seq[len++] = dp_derivative(&seq[0]);
	while (seq[len - 1].n > 0 && len < 7) {
		q = dp_neg_remainder(&seq[len - 2], &seq[len - 1]);
		if (dp_is_zero(&q)) break;
		seq[len++] = q;
	}
	int total = sturm_variations(seq, len, 0.0)
	          - sturm_variations(seq, len, 1.0);
	// Sturm counts roots in (a,b]; drop a root sitting exactly at 1.
	if (fabs(dp_eval(p, 1.0)) <= MT_POLY_EPS * scale) total--;
	if (total <= 0) return 0;
	int nout = 0;
	sturm_isolate(seq, len, 0.0, 1.0, total, roots, &nout);
	return nout;
}

static void sturm_isolate(const dpoly *seq, int len, double lo, double hi,
	int nroots, double *roots, int *nroots_out) {
	if (nroots == 0) return;
	if (hi - lo <= 1e-13) {
		roots[(*nroots_out)++] = 0.5 * (lo + hi);
		return;
	}
	if (nroots == 1) {
		// Pure sign-variation bisection: it converges for roots of
		// any multiplicity, where the polynomial itself never
		// changes sign.
		double a = lo, b = hi;
		while (b - a > 1e-13) {
			double m = 0.5 * (a + b);
			int left = sturm_variations(seq, len, a)
			         - sturm_variations(seq, len, m);
			if (left >= 1) b = m;
			else a = m;
		}
		roots[(*nroots_out)++] = 0.5 * (a + b);
		return;
	}
	double m = 0.5 * (lo + hi);
	// V(lo)-V(m) counts roots in (lo,m]; a root sitting exactly at m
	// is attributed to the left half, whose own bisection converges
	// to m from below, so no explicit zero test is needed.
	int left = sturm_variations(seq, len, lo)
	         - sturm_variations(seq, len, m);
	sturm_isolate(seq, len, lo, m, left, roots, nroots_out);
	sturm_isolate(seq, len, m, hi, nroots - left, roots, nroots_out);
}

// ---- projection ----------------------------------------------------------

// Project one world vertex through the composed transform, reusing
// the transform_apply / transform_homogenize conventions but carried
// out in double precision.
static void project_vertex(const device_t *device, const vertex_t *v,
	dvec3 *out) {
	const float *m = &device->transform.transform.m[0][0];
	double X = v->pos.x, Y = v->pos.y, Z = v->pos.z, W = v->pos.w;
	double cx = X * m[0] + Y * m[4] + Z * m[8]  + W * m[12];
	double cy = X * m[1] + Y * m[5] + Z * m[9]  + W * m[13];
	double cz = X * m[2] + Y * m[6] + Z * m[10] + W * m[14];
	double cw = X * m[3] + Y * m[7] + Z * m[11] + W * m[15];
	double rhw = 1.0 / cw;
	out->x = (cx * rhw + 1.0) * (double)device->transform.w * 0.5;
	out->y = (1.0 - cy * rhw) * (double)device->transform.h * 0.5;
	out->z = cz * rhw;
}

static dpoly2 linear_poly(double v0, double v1) {
	dpoly2 r;
	r.a = 0.0;
	r.b = v1 - v0;
	r.c = v0;
	return r;
}

// Signed projected area polynomial A(t) of face f.
static dpoly2 face_area_poly(const mt_face *f) {
	dpoly2 ex = dp2_sub(f->px[1], &f->px[0]);
	dpoly2 ey = dp2_sub(f->py[1], &f->py[0]);
	dpoly2 fx = dp2_sub(f->px[2], &f->px[0]);
	dpoly2 fy = dp2_sub(f->py[2], &f->py[0]);
	return cross2_poly(&ex, &ey, &fx, &fy);
}

// Whole-exposure zero projected area: the face must be ignored.
static int face_always_degenerate(const mt_face *f) {
	dpoly2 A = face_area_poly(f);
	double m0 = fabs(dp2_eval(&A, 0.0));
	double m1 = fabs(dp2_eval(&A, 1.0));
	double mx = fmax(m0, m1);
	if (fabs(A.a) > 0.0) {
		double tv = -0.5 * A.b / A.a;
		if (tv > 0.0 && tv < 1.0)
			mx = fmax(mx, fabs(dp2_eval(&A, tv)));
	}
	return mx <= MT_AREA_EPS;
}

// Barycentric edge numerators L_k(t) = cross2(p_{k+2}-p_{k+1}, C-p_{k+1}),
// with l_k(t) = L_k(t) / A(t).
static void face_bary_polys(const mt_face *f, const dvec3 *C,
	dpoly2 L[3]) {
	int k;
	for (k = 0; k < 3; k++) {
		int a = (k + 1) % 3;
		int b = (k + 2) % 3;
		dpoly2 ex = dp2_sub(f->px[b], &f->px[a]);
		dpoly2 ey = dp2_sub(f->py[b], &f->py[a]);
		dpoly2 cx = { 0.0, 0.0, C->x };
		dpoly2 cy = { 0.0, 0.0, C->y };
		cx = dp2_sub(cx, &f->px[a]);
		cy = dp2_sub(cy, &f->py[a]);
		L[k] = cross2_poly(&ex, &ey, &cx, &cy);
	}
}

// Barycentric coordinates of pixel centre C at time t.
static double face_bary(const mt_face *f, double t, const dvec3 *C,
	double l[3]) {
	double px[3], py[3];
	int k;
	for (k = 0; k < 3; k++) {
		px[k] = dp2_eval(&f->px[k], t);
		py[k] = dp2_eval(&f->py[k], t);
	}
	double denom = (px[1] - px[0]) * (py[2] - py[0])
	             - (py[1] - py[0]) * (px[2] - px[0]);
	if (denom == 0.0) {
		l[0] = l[1] = l[2] = 0.0;
		return 0.0;
	}
	l[0] = ((px[2] - px[1]) * (C->y - py[1])
	      - (py[2] - py[1]) * (C->x - px[1])) / denom;
	l[1] = ((px[0] - px[2]) * (C->y - py[2])
	      - (py[0] - py[2]) * (C->x - px[2])) / denom;
	l[2] = 1.0 - l[0] - l[1];
	return denom;
}

// Top-left convention (screen y grows downward): an edge owns a
// centre lying exactly on it when the edge descends, or is horizontal
// and runs to the right.  Adjacent triangles traverse a shared edge
// in opposite directions, so exactly one of them owns it.
static int edge_is_top_left(const mt_face *f, int opposite, double t) {
	int a = (opposite + 1) % 3;
	int b = (opposite + 2) % 3;
	double dx = dp2_eval(&f->px[b], t) - dp2_eval(&f->px[a], t);
	double dy = dp2_eval(&f->py[b], t) - dp2_eval(&f->py[a], t);
	if (fabs(dy) <= MT_EDGE_EPS) return dx > 0.0;
	return dy > 0.0;
}

// Half-open coverage test at time t.  Barycentric coordinates are
// non-negative inside the triangle for either winding (a flip changes
// the sign of both numerator and denominator), so the interior test
// is simply l_k >= 0; a centre landing on an edge is owned only when
// that edge is top-left.
static int face_covers(const mt_face *f, double t, const dvec3 *C) {
	double l[3];
	double denom = face_bary(f, t, C, l);
	if (fabs(denom) <= MT_AREA_EPS) return 0;
	int k;
	for (k = 0; k < 3; k++) {
		if (fabs(l[k]) <= MT_BARY_EPS) {
			if (!edge_is_top_left(f, k, t)) return 0;
		} else if (l[k] < 0.0) {
			return 0;
		}
	}
	return 1;
}

// Window-z at the pixel centre: screen barycentric interpolation.
static double face_depth_at(const mt_face *f, double t, const dvec3 *C) {
	double l[3];
	double denom = face_bary(f, t, C, l);
	if (fabs(denom) <= MT_AREA_EPS) return 0.0;
	return l[0] * dp2_eval(&f->pz[0], t)
	     + l[1] * dp2_eval(&f->pz[1], t)
	     + l[2] * dp2_eval(&f->pz[2], t);
}

// Cubic numerator N(t) = sum_k L_k(t) * z_k(t) of the barycentric
// depth rational N(t)/A(t).
static dpoly face_depth_numerator(const mt_face *f, const dpoly2 L[3]) {
	dpoly r;
	int k, j;
	for (j = 0; j < 8; j++) r.c[j] = 0.0;
	r.n = 3;
	for (k = 0; k < 3; k++) {
		double zc[2] = { f->pz[k].c, f->pz[k].b };  // z_k linear in t
		r.c[0] += L[k].c * zc[0];
		r.c[1] += L[k].b * zc[0] + L[k].c * zc[1];
		r.c[2] += L[k].a * zc[0] + L[k].b * zc[1];
		r.c[3] += L[k].a * zc[1];
	}
	dp_trim(&r);
	return r;
}

// Colour numerator quadratic sum_k c_k * L_k(t).
static dpoly2 color_numerator(const double col[3], const dpoly2 L[3]) {
	dpoly2 r;
	int k;
	r.a = r.b = r.c = 0.0;
	for (k = 0; k < 3; k++) {
		r.a += col[k] * L[k].a;
		r.b += col[k] * L[k].b;
		r.c += col[k] * L[k].c;
	}
	return r;
}

// ---- adaptive Simpson integration of num(t)/den(t) ----------------------

typedef struct { const dpoly2 *num; const dpoly2 *den; } rat_t;

static double rat_value(const rat_t *r, double t, double inward) {
	double d = dp2_eval(r->den, t);
	double n = dp2_eval(r->num, t);
	double eps = 1e-12 * (1.0 + fabs(d));
	if (fabs(d) <= eps) {
		// The ratio has a finite limit at a cut endpoint (the face
		// only degenerates at that single instant): evaluate just
		// inside the interval instead of inventing a zero.
		double tt = t + inward;
		d = dp2_eval(r->den, tt);
		n = dp2_eval(r->num, tt);
		if (fabs(d) <= eps) return 0.0;
	}
	return n / d;
}

static double simpson(double a, double b,
	double fa, double fm, double fb) {
	return (b - a) * (fa + 4.0 * fm + fb) / 6.0;
}

static double adaptive_simpson_rec(const rat_t *r, double a, double b,
	double fa, double fm, double fb, double whole, double eps, int depth) {
	double m = 0.5 * (a + b);
	double lm = 0.5 * (a + m), rm = 0.5 * (m + b);
	double flm = rat_value(r, lm, 0.0), frm = rat_value(r, rm, 0.0);
	double left = simpson(a, m, fa, flm, fm);
	double right = simpson(m, b, fm, frm, fb);
	double refined = left + right;
	if (depth <= 0 || fabs(refined - whole) <= 15.0 * eps)
		return refined + (refined - whole) / 15.0;
	double half_eps = eps * 0.5;
	return adaptive_simpson_rec(r, a, m, fa, flm, fm, left, half_eps,
			depth - 1)
	     + adaptive_simpson_rec(r, m, b, fm, frm, fb, right, half_eps,
			depth - 1);
}

static double integrate_color(const dpoly2 *num, const dpoly2 *den,
	double a, double b) {
	rat_t r = { num, den };
	double fa = rat_value(&r, a, 1e-12);
	double fb = rat_value(&r, b, -1e-12);
	double fm = rat_value(&r, 0.5 * (a + b), 0.0);
	double whole = simpson(a, b, fa, fm, fb);
	double eps = MT_INT_EPS * (b - a) + 1e-13;
	return adaptive_simpson_rec(&r, a, b, fa, fm, fb, whole, eps, 30);
}

static int add_event(double *events, int *count, int cap, double t) {
	if (!(t > MT_ROOT_EPS && t < 1.0 - MT_ROOT_EPS)) return 1;
	if (*count >= cap) return 0;
	events[(*count)++] = t;
	return 1;
}

static int dbl_cmp(const void *pa, const void *pb) {
	double a = *(const double *)pa, b = *(const double *)pb;
	return (a > b) - (a < b);
}

// Integrate one pixel over the exposure.
static int render_pixel(const mt_face *faces, size_t fcount, int px, int py,
	double *events, int event_cap, const double bg[3], double out[3]) {
	dvec3 C = { (double)px + 0.5, (double)py + 0.5, 0.0 };
	size_t i, j;
	int k, n = 0;

	// Per-face barycentric polynomials at this pixel centre.
	dpoly2 (*Lall)[3] = (dpoly2 (*)[3])malloc(
		(fcount ? fcount : 1) * sizeof(dpoly2[3]));
	if (Lall == NULL) return 0;
	for (i = 0; i < fcount; i++)
		face_bary_polys(&faces[i], &C, Lall[i]);

	// Events 1 and 2: edge crossings and orientation flips per face.
	for (i = 0; i < fcount; i++) {
		const mt_face *f = &faces[i];
		dpoly2 A = face_area_poly(f);
		double roots[2];
		int nr, e;
		nr = dp2_roots(&A, roots);
		for (e = 0; e < nr; e++)
			if (!add_event(events, &n, event_cap, roots[e])) goto fail;
		for (k = 0; k < 3; k++) {
			nr = dp2_roots(&Lall[i][k], roots);
			for (e = 0; e < nr; e++)
				if (!add_event(events, &n, event_cap, roots[e])) goto fail;
		}
	}

	// Event 3: depth exchanges at this pixel (polynomial of degree
	// up to 5, all roots isolated by Sturm's theorem).
	for (i = 0; i < fcount; i++) {
		dpoly Ni = face_depth_numerator(&faces[i], Lall[i]);
		dpoly2 Ai = face_area_poly(&faces[i]);
		dpoly Aig;
		Aig.n = 2;
		Aig.c[0] = Ai.c; Aig.c[1] = Ai.b; Aig.c[2] = Ai.a;
		Aig.c[3] = Aig.c[4] = Aig.c[5] = Aig.c[6] = Aig.c[7] = 0.0;
		for (j = i + 1; j < fcount; j++) {
			dpoly Nj = face_depth_numerator(&faces[j], Lall[j]);
			dpoly2 Aj = face_area_poly(&faces[j]);
			dpoly Ajg;
			Ajg.n = 2;
			Ajg.c[0] = Aj.c; Ajg.c[1] = Aj.b; Ajg.c[2] = Aj.a;
			Ajg.c[3] = Ajg.c[4] = Ajg.c[5] = Ajg.c[6] = Ajg.c[7] = 0.0;
			dpoly p1 = dp_multiply(&Ni, &Ajg);
			dpoly p2 = dp_multiply(&Nj, &Aig);
			dpoly diff = dp_sub_generic(p1, &p2);
			double roots[8];
			int nr, e;
			if (dp_is_zero(&diff)) continue;
			nr = dp_roots_unit(&diff, roots);
			for (e = 0; e < nr; e++)
				if (!add_event(events, &n, event_cap, roots[e])) goto fail;
		}
	}

	qsort(events, (size_t)n, sizeof(double), dbl_cmp);

	double *cuts = (double *)malloc((size_t)(n + 2) * sizeof(double));
	if (cuts == NULL) goto fail;
	int cuts_n = 0, ci;
	cuts[cuts_n++] = 0.0;
	for (k = 0; k < n; k++) {
		if (events[k] <= MT_ROOT_EPS || events[k] >= 1.0 - MT_ROOT_EPS)
			continue;
		if (cuts_n > 1 && events[k] - cuts[cuts_n - 1] <= MT_MERGE_EPS)
			continue;
		cuts[cuts_n++] = events[k];
	}
	cuts[cuts_n++] = 1.0;

	out[0] = out[1] = out[2] = 0.0;

	for (ci = 0; ci + 1 < cuts_n; ci++) {
		double u = cuts[ci], v = cuts[ci + 1];
		double tm = 0.5 * (u + v);
		double dur = v - u;
		if (dur <= 0.0) continue;

		// Winner at the interval midpoint; the cut list guarantees it
		// is constant on the whole open interval.
		int winner = -1;
		double best_z = 0.0;
		for (i = 0; i < fcount; i++) {
			if (!face_covers(&faces[i], tm, &C)) continue;
			double z = face_depth_at(&faces[i], tm, &C);
			if (winner < 0 || z < best_z - MT_Z_EPS ||
				(fabs(z - best_z) <= MT_Z_EPS && (int)i < winner)) {
				winner = (int)i;
				best_z = z;
			}
		}

		if (winner < 0) {
			out[0] += dur * bg[0];
			out[1] += dur * bg[1];
			out[2] += dur * bg[2];
			continue;
		}

		const mt_face *f = &faces[winner];
		dpoly2 den = face_area_poly(f);
		dpoly2 nr = color_numerator(f->cr, Lall[winner]);
		dpoly2 ng = color_numerator(f->cg, Lall[winner]);
		dpoly2 nb = color_numerator(f->cb, Lall[winner]);
		out[0] += integrate_color(&nr, &den, u, v);
		out[1] += integrate_color(&ng, &den, u, v);
		out[2] += integrate_color(&nb, &den, u, v);
	}
	free(cuts);
	free(Lall);
	return 1;

fail:
	free(Lall);
	return 0;
}

int motion_render(device_t *device, const motion_triangle *triangles,
	size_t count, double *rgb) {
	if (device == NULL || rgb == NULL) return 0;
	if (count != 0 && triangles == NULL) return 0;
	if (device->width <= 0 || device->height <= 0) return 0;

	size_t i, k;
	mt_face *faces = (mt_face *)malloc((count ? count : 1) * sizeof(mt_face));
	if (faces == NULL) return 0;

	size_t kept = 0;
	for (i = 0; i < count; i++) {
		mt_face f;
		dvec3 po[3], pc[3];
		for (k = 0; k < 3; k++) {
			project_vertex(device, &triangles[i].open[k], &po[k]);
			project_vertex(device, &triangles[i].close[k], &pc[k]);
			f.px[k] = linear_poly(po[k].x, pc[k].x);
			f.py[k] = linear_poly(po[k].y, pc[k].y);
			f.pz[k] = linear_poly(po[k].z, pc[k].z);
			// Vertex RGB is constant during the exposure and taken
			// from the open endpoint.
			f.cr[k] = (double)triangles[i].open[k].color.r;
			f.cg[k] = (double)triangles[i].open[k].color.g;
			f.cb[k] = (double)triangles[i].open[k].color.b;
		}
		if (face_always_degenerate(&f)) continue;
		faces[kept++] = f;
	}

	// Upper bound on roots per pixel: 8 per face (3 edges * 2 +
	// area * 2) and 5 per face pair.
	size_t pairs = kept * (kept - (kept > 0 ? 1 : 0)) / 2;
	int event_cap = (int)(8 * kept + 5 * pairs + 8);
	double *events = (double *)malloc((size_t)event_cap * sizeof(double));
	if (events == NULL) {
		free(faces);
		return 0;
	}

	double bg[3];
	bg[0] = (double)((device->background >> 16) & 255) / 255.0;
	bg[1] = (double)((device->background >> 8) & 255) / 255.0;
	bg[2] = (double)(device->background & 255) / 255.0;

	int W = device->width, H = device->height;
	int ok = 1;
	for (int y = 0; y < H && ok; y++) {
		for (int x = 0; x < W; x++) {
			double acc[3];
			if (!render_pixel(faces, kept, x, y, events, event_cap,
				bg, acc)) {
				ok = 0;
				break;
			}
			size_t base = ((size_t)y * W + x) * 3;
			rgb[base + 0] = acc[0];
			rgb[base + 1] = acc[1];
			rgb[base + 2] = acc[2];
		}
	}

	free(events);
	free(faces);
	if (!ok) return 0;

	// Clamp each channel and quantise exactly once into the frame
	// buffer.  The zbuffer is deliberately never read or written.
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			size_t base = ((size_t)y * W + x) * 3;
			int q[3];
			for (k = 0; k < 3; k++) {
				double c = rgb[base + k];
				if (c < 0.0) c = 0.0;
				if (c > 1.0) c = 1.0;
				rgb[base + k] = c;
				q[k] = (int)floor(255.0 * c + 0.5);
				if (q[k] > 255) q[k] = 255;
			}
			device->framebuffer[y][x] =
				(IUINT32)((q[0] << 16) | (q[1] << 8) | q[2]);
		}
	}
	return 1;
}
