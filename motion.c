#include "motion.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small polynomial helpers.  Coefficients are stored low degree first. */
/* ------------------------------------------------------------------ */

static double mp_eval(const double *a, int deg, double t) {
	double v = 0.0;
	int i;
	for (i = deg; i >= 0; --i) v = v * t + a[i];
	return v;
}

static int mp_trim(const double *a, int deg) {
	while (deg > 0 && a[deg] == 0.0) --deg;
	return deg;
}

static void mp_derivative(const double *a, int deg, double *d, int *ddeg) {
	int i;
	if (deg <= 0) {
		d[0] = 0.0;
		*ddeg = 0;
		return;
	}
	for (i = 1; i <= deg; ++i) d[i - 1] = (double)i * a[i];
	*ddeg = mp_trim(d, deg - 1);
}

static int mp_quad_roots(const double q[3], double roots[2]) {
	double a = q[2], b = q[1], c = q[0];
	double disc, s, qq, scale, tol;
	int n = 0;

	if (a == 0.0) {
		if (b == 0.0) return 0;
		roots[n++] = -c / b;
		return n;
	}

	disc = b * b - 4.0 * a * c;
	scale = fabs(b * b);
	if (fabs(4.0 * a * c) > scale) scale = fabs(4.0 * a * c);
	tol = 1e-12 * scale;
	if (disc < 0.0 && disc >= -tol) disc = 0.0;
	if (disc < 0.0) return 0;

	s = sqrt(disc);
	qq = -0.5 * (b + copysign(s, b));
	if (qq != 0.0) {
		roots[n++] = qq / a;
		roots[n++] = c / qq;
	} else {
		roots[n++] = (-b + s) / (2.0 * a);
		roots[n++] = (-b - s) / (2.0 * a);
	}
	return n;
}

static void mp_bisect(const double *a, int deg, double lo, double hi,
	double flo, double *root) {
	int i;
	double mid, fmid;
	int positive = flo > 0.0;

	for (i = 0; i < 80; ++i) {
		mid = lo + (hi - lo) * 0.5;
		if (mid == lo || mid == hi) break;
		fmid = mp_eval(a, deg, mid);
		if (fmid == 0.0) {
			lo = hi = mid;
			break;
		}
		if ((fmid > 0.0) == positive) {
			lo = mid;
			flo = fmid;
			positive = flo > 0.0;
		} else {
			hi = mid;
		}
	}
	*root = lo + (hi - lo) * 0.5;
}

/* Sign-changing roots in [0,1].  Degree is at most five in this file. */
static int mp_cross_roots(const double *a, int deg, double *roots, int cap) {
	double aa[6], da[5], dr[8], bounds[10];
	int ddeg, nd, nb, i, j, n = 0;
	double prev;

	for (i = 0; i <= deg; ++i) aa[i] = a[i];
	deg = mp_trim(aa, deg);
	if (deg <= 0) return 0;

	if (deg <= 2) {
		double q[3] = { aa[0], aa[1], deg == 2 ? aa[2] : 0.0 };
		double qr[2];
		int qn = mp_quad_roots(q, qr);
		for (i = 0; i < qn && n < cap; ++i) {
			if (qr[i] > 0.0 && qr[i] < 1.0) roots[n++] = qr[i];
		}
		return n;
	}

	if (deg == 1) {
		double r = -aa[0] / aa[1];
		if (r > 0.0 && r < 1.0 && n < cap) roots[n++] = r;
		return n;
	}

	mp_derivative(aa, deg, da, &ddeg);
	nd = mp_cross_roots(da, ddeg, dr, (int)(sizeof(dr) / sizeof(dr[0])));
	for (i = 0; i < nd; ++i) {
		for (j = i + 1; j < nd; ++j) {
			if (dr[j] < dr[i]) {
				double tmp = dr[i];
				dr[i] = dr[j];
				dr[j] = tmp;
			}
		}
	}

	nb = 0;
	bounds[nb++] = 0.0;
	for (i = 0; i < nd; ++i) {
		if (dr[i] > 0.0 && dr[i] < 1.0) bounds[nb++] = dr[i];
	}
	bounds[nb++] = 1.0;

	prev = bounds[0];
	for (i = 1; i < nb; ++i) {
		double next = bounds[i];
		double fl = mp_eval(aa, deg, prev);
		double fr = mp_eval(aa, deg, next);
		if ((fl < 0.0 && fr > 0.0) || (fl > 0.0 && fr < 0.0)) {
			double r;
			mp_bisect(aa, deg, prev, next, fl, &r);
			if (r > 0.0 && r < 1.0 && n < cap) roots[n++] = r;
		}
		prev = next;
	}
	return n;
}

static void mp_mul(const double *a, int da, const double *b, int db,
	double *out, int *dout) {
	int i, j;
	for (i = 0; i <= da + db; ++i) out[i] = 0.0;
	for (i = 0; i <= da; ++i)
		for (j = 0; j <= db; ++j)
			out[i + j] += a[i] * b[j];
	*dout = mp_trim(out, da + db);
}

static int cmp_double(const void *pa, const void *pb) {
	double a = *(const double *)pa, b = *(const double *)pb;
	return (a > b) - (a < b);
}

/* ------------------------------------------------------------------ */
/* Per-triangle projected motion                                        */
/* ------------------------------------------------------------------ */

typedef struct {
	double x[3][2], y[3][2], z[3][2];
	double color[3][3];
} screen_tri;

typedef struct {
	double x0[3], dx[3];
	double y0[3], dy[3];
	double z0[3], dz[3];
	double area[3];
	double numerator[3][3]; /* [barycentric index][power] */
	int zero_n[3];
	int flat;
} pixel_tri;

typedef struct {
	double *v;
	size_t n, cap;
} event_list;

static int events_init(event_list *e) {
	e->cap = 128;
	e->n = 0;
	e->v = (double *)malloc(e->cap * sizeof(double));
	return e->v != NULL;
}

static int events_reserve(event_list *e, size_t need) {
	double *p;
	size_t cap;
	if (need <= e->cap) return 1;
	if (need > (size_t)-1 / sizeof(double)) return 0;
	cap = e->cap ? e->cap : 128;
	while (cap < need) {
		if (cap > ((size_t)-1 / sizeof(double)) / 2U) {
			cap = need;
			break;
		}
		cap *= 2;
	}
	if (cap < need) cap = need;
	p = (double *)realloc(e->v, cap * sizeof(double));
	if (!p) return 0;
	e->v = p;
	e->cap = cap;
	return 1;
}

static int events_add(event_list *e, double t) {
	if (!isfinite(t)) return 1; /* a non-finite root cannot be an event */
	if (t <= 0.0 || t >= 1.0) return 1;
	if (!events_reserve(e, e->n + 1)) return 0;
	e->v[e->n++] = t;
	return 1;
}

static int events_add_quad(event_list *e, const double q[3]) {
	double r[2];
	int n = mp_quad_roots(q, r), i;
	for (i = 0; i < n; ++i)
		if (!events_add(e, r[i])) return 0;
	return 1;
}

static int events_add_poly(event_list *e, const double *a, int deg) {
	double roots[8];
	int n = mp_cross_roots(a, deg, roots, 8), i;
	for (i = 0; i < n; ++i)
		if (!events_add(e, roots[i])) return 0;
	return 1;
}

static void cross_quad(
	double ax0, double ax1, double ay0, double ay1,
	double bx0, double bx1, double by0, double by1,
	double q[3]) {
	q[0] = ax0 * by0 - ay0 * bx0;
	q[1] = ax1 * by0 + ax0 * by1 - ay1 * bx0 - ay0 * bx1;
	q[2] = ax1 * by1 - ay1 * bx1;
}

static void project_endpoint(const device_t *device, const vertex_t *v,
	double out[3]) {
	vector_t clip, screen;
	transform_apply(&device->transform, &clip, &v->pos);
	transform_homogenize(&device->transform, &screen, &clip);
	out[0] = screen.x;
	out[1] = screen.y;
	out[2] = screen.z;
}

static int build_pixel_tri(const screen_tri *s, double px, double py,
	pixel_tri *p) {
	int k;
	double ux0, ux1, uy0, uy1;
	double vx0, vx1, vy0, vy1;
	double dpx0, dpx1, dpy0, dpy1;

	for (k = 0; k < 3; ++k) {
		p->x0[k] = s->x[k][0];
		p->dx[k] = s->x[k][1] - s->x[k][0];
		p->y0[k] = s->y[k][0];
		p->dy[k] = s->y[k][1] - s->y[k][0];
		p->z0[k] = s->z[k][0];
		p->dz[k] = s->z[k][1] - s->z[k][0];
	}

	ux0 = p->x0[1] - p->x0[0];
	ux1 = p->dx[1] - p->dx[0];
	uy0 = p->y0[1] - p->y0[0];
	uy1 = p->dy[1] - p->dy[0];
	vx0 = p->x0[2] - p->x0[0];
	vx1 = p->dx[2] - p->dx[0];
	vy0 = p->y0[2] - p->y0[0];
	vy1 = p->dy[2] - p->dy[0];
	cross_quad(ux0, ux1, uy0, uy1, vx0, vx1, vy0, vy1, p->area);

	dpx0 = px - p->x0[0];
	dpx1 = -p->dx[0];
	dpy0 = py - p->y0[0];
	dpy1 = -p->dy[0];

	/* lambda1 * area = cross(p-q0, q2-q0) */
	cross_quad(dpx0, dpx1, dpy0, dpy1,
		vx0, vx1, vy0, vy1, p->numerator[1]);
	/* lambda2 * area = cross(q1-q0, p-q0) */
	cross_quad(ux0, ux1, uy0, uy1,
		dpx0, dpx1, dpy0, dpy1, p->numerator[2]);

	/* lambda0 * area = cross(q2-q1, p-q1), computed directly so that a
	   persistent opposite edge is an exact zero polynomial. */
	{
		double ex0 = p->x0[2] - p->x0[1];
		double ex1 = p->dx[2] - p->dx[1];
		double ey0e = p->y0[2] - p->y0[1];
		double ey1e = p->dy[2] - p->dy[1];
		double qx0 = px - p->x0[1];
		double qx1 = -p->dx[1];
		double qy0 = py - p->y0[1];
		double qy1 = -p->dy[1];
		cross_quad(ex0, ex1, ey0e, ey1e,
			qx0, qx1, qy0, qy1, p->numerator[0]);
	}

	for (k = 0; k < 3; ++k) {
		p->zero_n[k] = (p->numerator[k][0] == 0.0 &&
			p->numerator[k][1] == 0.0 &&
			p->numerator[k][2] == 0.0);
	}

	p->flat = (p->area[0] == 0.0 && p->area[1] == 0.0 &&
		p->area[2] == 0.0);
	return !p->flat;
}

static double pixel_depth(const pixel_tri *p, double t) {
	double a = mp_eval(p->area, 2, t);
	double n[3], zn = 0.0;
	int k;
	if (a == 0.0) return 0.0;
	for (k = 0; k < 3; ++k) n[k] = mp_eval(p->numerator[k], 2, t);
	for (k = 0; k < 3; ++k) {
		double z = p->z0[k] + p->dz[k] * t;
		zn += z * n[k];
	}
	return zn / a;
}

/*
 * Direct3D-style top-left rule, expressed after normalizing the triangle
 * to positive signed area.  For a normalized cyclic edge, upward edges
 * and horizontal rightward edges are the inclusive ones.
 */
static int edge_inclusive(const pixel_tri *p, int k, double t, double a) {
	int ia = (k + 1) % 3;
	int ib = (k + 2) % 3;
	double ex = (p->x0[ib] + p->dx[ib] * t) -
		(p->x0[ia] + p->dx[ia] * t);
	double ey = (p->y0[ib] + p->dy[ib] * t) -
		(p->y0[ia] + p->dy[ia] * t);
	double s = (a > 0.0) ? 1.0 : -1.0;
	double nx = s * ex, ny = s * ey;
	return ny < 0.0 || (ny == 0.0 && nx > 0.0);
}

static int triangle_covers(const pixel_tri *p, double t) {
	double a = mp_eval(p->area, 2, t);
	int k;
	if (a == 0.0 || !isfinite(a)) return 0;
	for (k = 0; k < 3; ++k) {
		double n = p->zero_n[k] ? 0.0 : mp_eval(p->numerator[k], 2, t);
		if (n == 0.0) {
			if (!edge_inclusive(p, k, t, a)) return 0;
		} else if ((n > 0.0) != (a > 0.0)) {
			return 0; /* lambda_k is negative */
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Error-controlled color integration on one winner interval.           */
/* ------------------------------------------------------------------ */

typedef struct {
	double a[3];
	double n0[3], n1[3], n2[3];
	const screen_tri *st;
	int channel;
} seg_color;

static void local_quad(const double *g, double l, double h, double q[3]) {
	q[0] = mp_eval(g, 2, l);
	q[1] = h * (g[1] + 2.0 * g[2] * l);
	q[2] = h * h * g[2];
}

static seg_color make_seg_color(const pixel_tri *p, const screen_tri *s,
	int tri, int channel, double l, double r) {
	seg_color f;
	double h = r - l;
	f.st = &s[tri];
	f.channel = channel;
	local_quad(p->area, l, h, f.a);
	local_quad(p->numerator[0], l, h, f.n0);
	local_quad(p->numerator[1], l, h, f.n1);
	local_quad(p->numerator[2], l, h, f.n2);
	return f;
}

static double seg_eval(void *ctx, double u) {
	const seg_color *f = (const seg_color *)ctx;
	double A = f->a[0] + u * (f->a[1] + u * f->a[2]);
	double N[3];
	const screen_tri *st = f->st;
	int ch = f->channel;
	double v;
	int k;
	if (A == 0.0 || !isfinite(A)) return 0.0;
	N[0] = f->n0[0] + u * (f->n0[1] + u * f->n0[2]);
	N[1] = f->n1[0] + u * (f->n1[1] + u * f->n1[2]);
	N[2] = f->n2[0] + u * (f->n2[1] + u * f->n2[2]);
	v = 0.0;
	for (k = 0; k < 3; ++k)
		v += st->color[k][ch] * N[k];
	v /= A;
	if (!isfinite(v)) return 0.0;
	if (v < 0.0) v = 0.0;
	if (v > 1.0) v = 1.0;
	return v;
}

static double endpoint_color(const seg_color *f, int upper) {
	double A0, A1d, A2d;
	double B0, B1d, B2d, denom, v;
	const double *nq[3];
	const screen_tri *st = f->st;
	int ch = f->channel, k;
	double scale;

	nq[0] = f->n0;
	nq[1] = f->n1;
	nq[2] = f->n2;

	A0 = upper ? f->a[0] + f->a[1] + f->a[2] : f->a[0];
	A1d = upper ? f->a[1] + 2.0 * f->a[2] : f->a[1];
	A2d = 2.0 * f->a[2];

	B0 = B1d = B2d = 0.0;
	for (k = 0; k < 3; ++k) {
		const double *q = nq[k];
		double b0 = upper ? q[0] + q[1] + q[2] : q[0];
		double b1 = upper ? q[1] + 2.0 * q[2] : q[1];
		double b2 = 2.0 * q[2];
		B0 += st->color[k][ch] * b0;
		B1d += st->color[k][ch] * b1;
		B2d += st->color[k][ch] * b2;
	}

	scale = fabs(f->a[0]) + fabs(f->a[1]) + fabs(f->a[2]);
	if (fabs(A0) <= 1e-11 * scale) {
		if (fabs(A1d) > 1e-14 * scale) {
			v = B1d / A1d;
		} else {
			denom = A2d;
			v = (denom != 0.0) ? B2d / denom : 0.0;
		}
	} else {
		v = B0 / A0;
	}
	if (!isfinite(v)) v = 0.0;
	if (v < 0.0) v = 0.0;
	if (v > 1.0) v = 1.0;
	return v;
}

static double simpson4(double a, double b, double fa, double fm, double fb) {
	return (b - a) * (fa + 4.0 * fm + fb) / 6.0;
}

static int adaptive_simpson(void *ctx,
	double (*fn)(void *, double),
	double l, double r, double fl, double fm, double fr,
	double whole, double eps, int depth, double *result) {
	double m = l + (r - l) * 0.5;
	double lm = l + (m - l) * 0.5;
	double rm = m + (r - m) * 0.5;
	double flm = fn(ctx, lm);
	double frm = fn(ctx, rm);
	double left = simpson4(l, m, fl, flm, fm);
	double right = simpson4(m, r, fm, frm, fr);
	double delta = left + right - whole;
	if (depth <= 0 || fabs(delta) <= 15.0 * eps) {
		*result = left + right + delta / 15.0;
		return 1;
	}
	{
		double a = 0.0, b = 0.0;
		int ok1 = adaptive_simpson(ctx, fn, l, m, fl, flm, fm,
			left, eps * 0.5, depth - 1, &a);
		int ok2 = adaptive_simpson(ctx, fn, m, r, fm, frm, fr,
			right, eps * 0.5, depth - 1, &b);
		*result = a + b;
		return ok1 && ok2;
	}
}

static double integrate_channel(const pixel_tri *pt, const screen_tri *st,
	int tri, int ch, double l, double r, double time_eps) {
	double h = r - l;
	seg_color f;
	double fl, fm, fr, whole, integral = 0.0, eps_u;

	if (h <= 0.0) return 0.0;
	/* Color is a convex vertex-color combination, hence lies in [0,1]. */
	if (h <= time_eps) {
		f = make_seg_color(pt, st, tri, ch, l, r);
		return h * seg_eval(&f, 0.5);
	}

	f = make_seg_color(pt, st, tri, ch, l, r);
	fl = endpoint_color(&f, 0);
	fr = endpoint_color(&f, 1);
	fm = seg_eval(&f, 0.5);
	whole = simpson4(0.0, 1.0, fl, fm, fr);
	eps_u = 0.1 * time_eps / h;
	if (eps_u > 0.25) eps_u = 0.25;
	adaptive_simpson(&f, seg_eval, 0.0, 1.0, fl, fm, fr, whole,
		eps_u, 28, &integral);
	return h * integral;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

int motion_render(device_t *device, const motion_triangle *triangles,
	size_t count, double *rgb) {
	int width, height;
	size_t pixels, total3, pi;
	screen_tri *screens = NULL;
	pixel_tri *tris = NULL;
	double *linear = NULL;
	event_list events = { NULL, 0, 0 };
	double bg[3];
	int ok = 0;

	if (!device || !rgb) return 0;
	if (count != 0 && !triangles) return 0;
	width = device->width;
	height = device->height;
	if (width <= 0 || height <= 0 || !device->framebuffer) return 0;
	if ((size_t)width > (size_t)-1 / (size_t)height) return 0;
	pixels = (size_t)width * (size_t)height;
	if (pixels > (size_t)-1 / 3U) return 0;
	total3 = pixels * 3U;
	if (total3 > (size_t)-1 / sizeof(double)) return 0;

	linear = (double *)calloc(total3, sizeof(double));
	if (!linear) goto cleanup;
	if (count != 0) {
		if (count > (size_t)-1 / sizeof(screen_tri) ||
			count > (size_t)-1 / sizeof(pixel_tri))
			goto cleanup;
		screens = (screen_tri *)calloc(count, sizeof(screen_tri));
		tris = (pixel_tri *)calloc(count, sizeof(pixel_tri));
		if (!screens || !tris) goto cleanup;
	}
	if (!events_init(&events)) goto cleanup;

	bg[0] = (double)((device->background >> 16) & 255U) / 255.0;
	bg[1] = (double)((device->background >> 8) & 255U) / 255.0;
	bg[2] = (double)(device->background & 255U) / 255.0;

	{
		size_t i, k;
		for (i = 0; i < count; ++i) {
			for (k = 0; k < 3; ++k) {
				double q[3];
				project_endpoint(device, &triangles[i].open[k], q);
				screens[i].x[k][0] = q[0];
				screens[i].y[k][0] = q[1];
				screens[i].z[k][0] = q[2];
				project_endpoint(device, &triangles[i].close[k], q);
				screens[i].x[k][1] = q[0];
				screens[i].y[k][1] = q[1];
				screens[i].z[k][1] = q[2];
				screens[i].color[k][0] = (double)triangles[i].open[k].color.r;
				screens[i].color[k][1] = (double)triangles[i].open[k].color.g;
				screens[i].color[k][2] = (double)triangles[i].open[k].color.b;
			}
		}
	}

	for (pi = 0; pi < pixels; ++pi) {
		int py_i = (int)(pi / (size_t)width);
		int px_i = (int)(pi % (size_t)width);
		double px = (double)px_i + 0.5;
		double py = (double)py_i + 0.5;
		double acc[3] = { 0.0, 0.0, 0.0 };
		size_t i, j, e;
		size_t interval_count;
		double time_eps;

		events.n = 2;
		events.v[0] = 0.0;
		events.v[1] = 1.0;

		for (i = 0; i < count; ++i) {
			int k;
			build_pixel_tri(&screens[i], px, py, &tris[i]);
			if (tris[i].flat) continue;
			if (!events_add_quad(&events, tris[i].area)) goto cleanup;
			for (k = 0; k < 3; ++k) {
				if (!events_add_quad(&events, tris[i].numerator[k]))
					goto cleanup;
				if (tris[i].zero_n[k]) {
					int ia = (k + 1) % 3, ib = (k + 2) % 3;
					double ey0 = tris[i].y0[ib] - tris[i].y0[ia];
					double ey1 = tris[i].dy[ib] - tris[i].dy[ia];
					if (ey1 != 0.0) {
						double r = -ey0 / ey1;
						if (!events_add(&events, r)) goto cleanup;
					}
				}
			}
		}

		/* Depth exchanges: (z_i-z_j) * A_i*A_j is degree <= 5. */
		for (i = 0; i < count; ++i) {
			for (j = i + 1; j < count; ++j) {
				double zi[4] = { 0,0,0,0 }, zj[4] = { 0,0,0,0 };
				double pi2[6], pj2[6], h[6];
				int di, dj, dh, k, d;
				if (tris[i].flat || tris[j].flat) continue;
				for (k = 0; k < 3; ++k) {
					double z0i = tris[i].z0[k], dzi = tris[i].dz[k];
					double z0j = tris[j].z0[k], dzj = tris[j].dz[k];
					const double *ni = tris[i].numerator[k];
					const double *nj = tris[j].numerator[k];
					for (d = 0; d <= 2; ++d) {
						zi[d] += z0i * ni[d];
						zj[d] += z0j * nj[d];
					}
					for (d = 0; d <= 2; ++d)
						zi[d + 1] += dzi * ni[d];
					for (d = 0; d <= 2; ++d)
						zj[d + 1] += dzj * nj[d];
				}
				mp_mul(zi, 3, tris[j].area, 2, pi2, &di);
				mp_mul(zj, 3, tris[i].area, 2, pj2, &dj);
				for (d = 0; d <= 5; ++d)
					h[d] = (d <= di ? pi2[d] : 0.0) -
						(d <= dj ? pj2[d] : 0.0);
				dh = mp_trim(h, 5);
				if (!events_add_poly(&events, h, dh)) goto cleanup;
			}
		}

		qsort(events.v, events.n, sizeof(double), cmp_double);
		interval_count = events.n > 0 ? events.n - 1 : 1;
		time_eps = 2e-10 / (double)(interval_count + 1);

		for (e = 0; e + 1 < events.n; ++e) {
			double l = events.v[e], r = events.v[e + 1];
			double h, mid;
			int winner = -1;
			double best_z = 0.0;

			if (r <= l) continue;
			h = r - l;
			mid = l + h * 0.5;
			if (mid <= l || mid >= r) {
				/* One-ulp interval: its total possible error is < 2e-16. */
				continue;
			}

			for (i = 0; i < count; ++i) {
				double z;
				if (tris[i].flat || !triangle_covers(&tris[i], mid))
					continue;
				z = pixel_depth(&tris[i], mid);
				if (winner < 0) {
					winner = (int)i;
					best_z = z;
				} else if (z < best_z - 1e-12) {
					winner = (int)i;
					best_z = z;
				}
				/* Near/equal depth keeps the smaller input index. */
			}

			if (winner < 0) {
				acc[0] += bg[0] * h;
				acc[1] += bg[1] * h;
				acc[2] += bg[2] * h;
			} else {
				int ch;
				for (ch = 0; ch < 3; ++ch) {
					double v = integrate_channel(&tris[winner],
						screens, winner, ch, l, r, time_eps);
					acc[ch] += v;
				}
			}
		}

		linear[pi * 3 + 0] = acc[0];
		linear[pi * 3 + 1] = acc[1];
		linear[pi * 3 + 2] = acc[2];
	}

	/* Only now publish: linear integral output, then one quantized frame. */
	memcpy(rgb, linear, total3 * sizeof(double));
	{
		size_t p;
		for (p = 0; p < pixels; ++p) {
			int y = (int)(p / (size_t)width);
			int x = (int)(p % (size_t)width);
			int q[3], ch;
			IUINT32 color;
			for (ch = 0; ch < 3; ++ch) {
				double c = rgb[p * 3 + ch];
				if (c < 0.0) c = 0.0;
				if (c > 1.0) c = 1.0;
				rgb[p * 3 + ch] = c;
				q[ch] = (int)floor(255.0 * c + 0.5);
				if (q[ch] < 0) q[ch] = 0;
				if (q[ch] > 255) q[ch] = 255;
			}
			color = (IUINT32)((q[0] << 16) | (q[1] << 8) | q[2]);
			device->framebuffer[y][x] = color;
		}
	}

	ok = 1;

cleanup:
	free(events.v);
	free(tris);
	free(screens);
	free(linear);
	return ok;
}
