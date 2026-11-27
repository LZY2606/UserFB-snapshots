#include "checks.h"
#include "tinyspline.h"
#include "tinyspline_surface.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Small helpers.                                                     */
/* ------------------------------------------------------------------ */

#define STR2(x) #x
#define STR(x) STR2(x)
#define CHECK(cond) \
	task_check((cond) != 0, #cond " @line " STR(__LINE__))

static const double LOFT_EPS = 1e-8;

static int
near3(const double a[3], const double b[3], double eps)
{
	int i;
	for (i = 0; i < 3; i++)
		if (fabs(a[i] - b[i]) > eps)
			return 0;
	return 1;
}

static double
dist3(const double a[3], const double b[3])
{
	double s = 0.0;
	int i;
	for (i = 0; i < 3; i++)
		s += (a[i] - b[i]) * (a[i] - b[i]);
	return sqrt(s);
}

static void
cross3(const double a[3], const double b[3], double out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/*
 * Independent Cox-de Boor implementation (deliberately written from scratch,
 * not using any surface module code). At interior knots the right-sided basis
 * is returned; at u == 1 the left-sided basis is used.
 */
static size_t
ref_find_span(const double *knots, size_t n, size_t deg, double u)
{
	size_t low = deg, high = n, mid;
	if (u >= knots[n])
		return n - 1;
	mid = (low + high) / 2;
	while (u < knots[mid] || u >= knots[mid + 1]) {
		if (u < knots[mid])
			high = mid;
		else
			low = mid;
		mid = (low + high) / 2;
	}
	return mid;
}

static void
ref_basis_funs(const double *knots, size_t span, size_t deg, double u,
               double *window)
{
	size_t j, r;
	window[0] = 1.0;
	for (j = 1; j <= deg; j++) {
		double saved = 0.0;
		for (r = 0; r < j; r++) {
			double den = knots[span + 1 + r] -
			             knots[span + 1 - j + r];
			double temp = den > 0.0 ? window[r] / den : 0.0;
			window[r] = saved +
			            (knots[span + 1 + r] - u) * temp;
			saved = (u - knots[span + 1 - j + r]) * temp;
		}
		window[j] = saved;
	}
}

static void
ref_basis(const double *knots, size_t n, size_t deg, double u,
          double *values, double *derivs)
{
	const size_t span = ref_find_span(knots, n, deg, u);
	double *window = (double *) malloc((deg + 1) * sizeof(double));
	size_t i, r;
	for (i = 0; i < n; i++) {
		values[i] = 0.0;
		derivs[i] = 0.0;
	}
	ref_basis_funs(knots, span, deg, u, window);
	for (r = 0; r <= deg; r++)
		values[span - deg + r] = window[r];
	if (deg >= 1) {
		double *lower = (double *) malloc(deg * sizeof(double));
		ref_basis_funs(knots, span, deg - 1, u, lower);
		for (i = 0; i < n; i++) {
			double da = knots[i + deg] - knots[i];
			double db = knots[i + deg + 1] - knots[i + 1];
			double na = 0.0, nb = 0.0;
			if (i >= span - deg + 1 && i <= span)
				na = lower[i - (span - deg + 1)];
			if (i + 1 >= span - deg + 1 && i + 1 <= span)
				nb = lower[(i + 1) - (span - deg + 1)];
			if (da > 0.0)
				derivs[i] += (double) deg * na / da;
			if (db > 0.0)
				derivs[i] -= (double) deg * nb / db;
		}
		free(lower);
	}
	free(window);
}

/*
 * Independent reference evaluation of a (possibly rational, possibly
 * non-unit-domain) section curve. u01 is the normalized parameter in [0, 1].
 */
static void
ref_curve(const tsBSpline *s, double u01,
          double point[3], double deriv[3])
{
	const size_t dim = ts_bspline_dimension(s);
	const size_t deg = ts_bspline_degree(s);
	const size_t n = ts_bspline_num_control_points(s);
	const tsReal *cp = ts_bspline_control_points_ptr(s);
	const tsReal *kr = ts_bspline_knots_ptr(s);
	const double min = kr[deg];
	const double max = kr[n];
	const double span = max - min;
	double *knots = (double *) malloc((n + deg + 1) * sizeof(double));
	double *N = (double *) malloc(n * sizeof(double));
	double *dN = (double *) malloc(n * sizeof(double));
	double H[4] = {0, 0, 0, 0}, dH[4] = {0, 0, 0, 0};
	double w;
	size_t i, c;
	(void) min;
	(void) max;

	/* Normalize the knot vector to [0, 1]; derivatives are then taken
	 * with respect to the normalized parameter u01 used by the surface. */
	for (i = 0; i < n + deg + 1; i++)
		knots[i] = ((double) kr[i] - min) / span;
	ref_basis(knots, n, deg, u01, N, dN);
	for (i = 0; i < n; i++) {
		if (dim == 4) {
			for (c = 0; c < 4; c++) {
				H[c] += N[i] * (double) cp[i * 4 + c];
				dH[c] += dN[i] * (double) cp[i * 4 + c];
			}
		} else {
			for (c = 0; c < 3; c++) {
				H[c] += N[i] * (double) cp[i * 3 + c];
				dH[c] += dN[i] * (double) cp[i * 3 + c];
			}
			H[3] += N[i];
			dH[3] += dN[i];
		}
	}
	w = H[3];
	for (c = 0; c < 3; c++) {
		point[c] = H[c] / w;
		deriv[c] = (dH[c] - point[c] * dH[3]) / w;
	}
	free(knots);
	free(N);
	free(dN);
}

/*
 * Independent tensor product evaluation using the surface's own, publicly
 * accessible control net and knots. This exercises the stored control net
 * rather than going through ts_loft_surface_eval twice.
 */
static void
ref_surface(const tsLoftSurface *sf, double u, double v,
            double point[3], double du[3], double dv[3])
{
	const size_t nu = ts_loft_surface_num_u_control_points(sf);
	const size_t nv = ts_loft_surface_num_v_control_points(sf);
	const size_t du_deg = ts_loft_surface_u_degree(sf);
	const size_t dv_deg = ts_loft_surface_v_degree(sf);
	const tsReal *net = ts_loft_surface_control_points_ptr(sf);
	const tsReal *ku = ts_loft_surface_u_knots_ptr(sf);
	const tsReal *kv = ts_loft_surface_v_knots_ptr(sf);
	double *Nu = malloc(nu * sizeof(double));
	double *dNu = malloc(nu * sizeof(double));
	double *Nv = malloc(nv * sizeof(double));
	double *dNv = malloc(nv * sizeof(double));
	double *kdu = malloc((nu + du_deg + 1) * sizeof(double));
	double *kdv = malloc((nv + dv_deg + 1) * sizeof(double));
	double H[4] = {0, 0, 0, 0}, Hu[4] = {0, 0, 0, 0};
	double Hv[4] = {0, 0, 0, 0}, w;
	size_t i, j, c;

	for (i = 0; i < nu + du_deg + 1; i++)
		kdu[i] = ku[i];
	for (i = 0; i < nv + dv_deg + 1; i++)
		kdv[i] = kv[i];
	ref_basis(kdu, nu, du_deg, u, Nu, dNu);
	ref_basis(kdv, nv, dv_deg, v, Nv, dNv);
	for (i = 0; i < nu; i++) {
		for (j = 0; j < nv; j++) {
			const tsReal *p = net + (i * nv + j) * 4;
			for (c = 0; c < 4; c++) {
				H[c] += Nu[i] * Nv[j] * p[c];
				Hu[c] += dNu[i] * Nv[j] * p[c];
				Hv[c] += Nu[i] * dNv[j] * p[c];
			}
		}
	}
	w = H[3];
	for (c = 0; c < 3; c++) {
		point[c] = H[c] / w;
		du[c] = (Hu[c] - point[c] * Hu[3]) / w;
		dv[c] = (Hv[c] - point[c] * Hv[3]) / w;
	}
	free(Nu); free(dNu); free(Nv); free(dNv);
	free(kdu); free(kdv);
}

static tsBSpline
make_spline(size_t n, size_t dim, size_t deg,
            const double *cp, const double *knots)
{
	tsBSpline s = ts_bspline_init();
	tsStatus st;
	tsReal *r_cp = malloc(n * dim * sizeof(tsReal));
	size_t i;
	for (i = 0; i < n * dim; i++)
		r_cp[i] = (tsReal) cp[i];
	if (ts_bspline_new(n, dim, deg, TS_CLAMPED, &s, &st) != TS_SUCCESS) {
		fprintf(stderr, "make_spline new: %s\n", st.message);
		exit(1);
	}
	if (ts_bspline_set_control_points(&s, r_cp, &st) != TS_SUCCESS) {
		fprintf(stderr, "make_spline cp: %s\n", st.message);
		exit(1);
	}
	if (knots) {
		tsReal *r_k = malloc((n + deg + 1) * sizeof(tsReal));
		for (i = 0; i < n + deg + 1; i++)
			r_k[i] = (tsReal) knots[i];
		if (ts_bspline_set_knots(&s, r_k, &st) != TS_SUCCESS) {
			fprintf(stderr, "make_spline knots: %s\n", st.message);
			exit(1);
		}
		free(r_k);
	}
	free(r_cp);
	return s;
}

/* Builds a rational circular arc of degree 2 in the plane spanned by the
 * orthonormal axes e1, e2, sweeping from angle a0 to a1. */
static tsBSpline
make_arc(const double center[3], const double e1[3], const double e2[3],
         double radius, double a0, double a1, double weight_scale,
         int insert_mid)
{
	const double sweep = a1 - a0;
	const double w1 = cos(sweep / 2.0);
	const double amid = (a0 + a1) / 2.0;
	double cp[12]; /* 3 homogeneous control points */
	double knots[6] = {0, 0, 0, 1, 1, 1};
	size_t k, d;
	tsBSpline s, refined;
	tsStatus st;

	for (d = 0; d < 3; d++) {
		const double p0 = center[d] + radius *
			(cos(a0) * e1[d] + sin(a0) * e2[d]);
		/* Tangent intersection of the end tangents. */
		const double pm = center[d] + (radius / w1) *
			(cos(amid) * e1[d] + sin(amid) * e2[d]);
		const double p1 = center[d] + radius *
			(cos(a1) * e1[d] + sin(a1) * e2[d]);
		cp[0 * 4 + d] = weight_scale * 1.0 * p0;
		cp[1 * 4 + d] = weight_scale * w1 * pm;
		cp[2 * 4 + d] = weight_scale * 1.0 * p1;
	}
	cp[3] = weight_scale;
	cp[7] = weight_scale * w1;
	cp[11] = weight_scale;

	s = make_spline(3, 4, 2, cp, knots);
	if (!insert_mid)
		return s;
	refined = ts_bspline_init();
	if (ts_bspline_insert_knot(&s, 0.5, 1, &refined, &k, &st)
	    != TS_SUCCESS) {
		fprintf(stderr, "make_arc insert: %s\n", st.message);
		exit(1);
	}
	ts_bspline_free(&s);
	return refined;
}

static void
eval_surface(const tsLoftSurface *sf, double u, double v,
             double p[3], double du[3], double dv[3])
{
	tsReal rp[3], rdu[3], rdv[3];
	tsStatus st;
	int i;
	if (ts_loft_surface_eval(sf, (tsReal) u, (tsReal) v,
	                         rp, rdu, rdv, &st) != TS_SUCCESS) {
		fprintf(stderr, "eval_surface: %s\n", st.message);
		exit(1);
	}
	for (i = 0; i < 3; i++) {
		p[i] = rp[i];
		du[i] = rdu[i];
		dv[i] = rdv[i];
	}
}

static void
fd_partial(const tsLoftSurface *sf, int axis,
           double u, double v, double h, double out[3])
{
	double pa[3], dua[3], dva[3], pb[3], dub[3], dvb[3];
	int i;
	if (axis == 0) {
		eval_surface(sf, u - h, v, pa, dua, dva);
		eval_surface(sf, u + h, v, pb, dub, dvb);
	} else {
		eval_surface(sf, u, v - h, pa, dua, dva);
		eval_surface(sf, u, v + h, pb, dub, dvb);
	}
	for (i = 0; i < 3; i++)
		out[i] = (pb[i] - pa[i]) / (2.0 * h);
}

/* ------------------------------------------------------------------ */
/* Section group: non-planar sections, mixed degrees, non-unit        */
/* domains, differing interior knots.                                 */
/* ------------------------------------------------------------------ */

static void
group_sections(void)
{
	tsBSpline sections[4];
	tsLoftSurface sf = ts_loft_surface_init();
	tsStatus st;
	size_t j, k;
	int ok;

	/* Degree 2, interior knot at 0.25, domain [2, 5], 4 control points. */
	{
		double cp[] = {
			0, 0, 0,   1, 2, 1,   2, -1, 2,   3, 1, 0
		};
		double knots[] = {2, 2, 2, 2.75, 5, 5, 5};
		sections[0] = make_spline(4, 3, 2, cp, knots);
	}
	/* Degree 3, interior knot at 0.75, domain [-1, 1]. */
	{
		double cp[] = {
			0, 1, 0,   1, 1, 2,   2, 0, -1,   3, 2, 1,   4, -1, 2
		};
		double knots[] = {-1, -1, -1, -1, 0.5, 1, 1, 1, 1};
		sections[1] = make_spline(5, 3, 3, cp, knots);
	}
	/* Degree 1 polyline, unit domain. */
	{
		double cp[] = {
			0, 2, 3,   2, 2, -2,   4, 1, 1
		};
		double knots[] = {0, 0, 0.5, 1, 1};
		sections[2] = make_spline(3, 3, 1, cp, knots);
	}
	/* Degree 3, two interior knots (one with full multiplicity),
	 * domain [0.5, 3.5]. */
	{
		double cp[] = {
			0, 0, 1,  1, 3, 0,  2, 3, 2,  2, -1, 3,
			3, -1, 0, 4, 2, 1, 5, 0, -2, 6, 1, 3
		};
		double knots[] = {
			0.5, 0.5, 0.5, 0.5,
			1.5, 1.5, 1.5, 2.5,
			3.5, 3.5, 3.5, 3.5
		};
		sections[3] = make_spline(8, 3, 3, cp, knots);
	}

	CHECK(ts_loft_surface_new(sections, 4, &sf, &st) == TS_SUCCESS);

	/* Common u basis: degree elevated to 3 and both interior knots
	 * retained. */
	CHECK(ts_loft_surface_u_degree(&sf) == 3);
	{
		const tsReal *ku = ts_loft_surface_u_knots_ptr(&sf);
		size_t nk = ts_loft_surface_num_u_knots(&sf);
		int has25 = 0, has75 = 0;
		for (k = 0; k < nk; k++) {
			if (fabs(ku[k] - 0.25) < 1e-10)
				has25 = 1;
			if (fabs(ku[k] - 0.75) < 1e-10)
				has75 = 1;
		}
		CHECK(has25 && has75);
		CHECK(fabs(ku[0]) < 1e-12 &&
		      fabs(ku[nk - 1] - 1.0) < 1e-12);
	}
	/* Natural cubic v interpolation: (count-1)*4 control points. */
	CHECK(ts_loft_surface_v_degree(&sf) == 3);
	CHECK(ts_loft_surface_num_v_control_points(&sf) == 12);
	/* Every stored weight is positive and finite. */
	{
		const tsReal *net = ts_loft_surface_control_points_ptr(&sf);
		size_t nu = ts_loft_surface_num_u_control_points(&sf);
		size_t nv = ts_loft_surface_num_v_control_points(&sf);
		ok = 1;
		for (k = 0; k < nu * nv; k++)
			if (!(net[k * 4 + 3] > 0.0) ||
			    !isfinite(net[k * 4 + 3]))
				ok = 0;
		CHECK(ok);
	}

	/* Whole section recovery at arbitrary u values, including the
	 * interior knots and domain ends. */
	{
		const double us[] = {0.0, 0.137, 0.25, 0.4999, 0.75, 0.88, 1.0};
		ok = 1;
		for (j = 0; j < 4; j++) {
			double vj = (double) j / 3.0;
			for (k = 0; k < sizeof(us) / sizeof(us[0]); k++) {
				double p[3], du[3], dv[3], refp[3], refd[3];
				eval_surface(&sf, us[k], vj, p, du, dv);
				ref_curve(&sections[j], us[k], refp, refd);
				if (!near3(p, refp, LOFT_EPS))
					ok = 0;
			}
		}
		CHECK(ok);
	}

	/* End sections map to v = 0 and v = 1 exactly. */
	{
		double p0[3], du0[3], dv0[3], p1[3], du1[3], dv1[3];
		double rp0[3], rd0[3], rp1[3], rd1[3];
		eval_surface(&sf, 0.42, 0.0, p0, du0, dv0);
		eval_surface(&sf, 0.42, 1.0, p1, du1, dv1);
		ref_curve(&sections[0], 0.42, rp0, rd0);
		ref_curve(&sections[3], 0.42, rp1, rd1);
		CHECK(near3(p0, rp0, LOFT_EPS) && near3(p1, rp1, LOFT_EPS));
	}

	/* u derivatives agree with the independent section derivative on
	 * both sides of an interior knot and at the knot itself (right). */
	{
		const double probes[] = {0.25 - 1e-4, 0.25, 0.25 + 1e-4,
		                         0.75 - 1e-3, 0.75, 0.75 + 1e-3};
		ok = 1;
		for (j = 0; j < 4; j++) {
			double vj = (double) j / 3.0;
			for (k = 0; k < sizeof(probes) / sizeof(probes[0]); k++) {
				double p[3], du[3], dv[3], rp[3], rd[3];
				eval_surface(&sf, probes[k], vj, p, du, dv);
				ref_curve(&sections[j], probes[k], rp, rd);
				if (!near3(du, rd, 2e-7))
					ok = 0;
			}
		}
		CHECK(ok);
	}

	/* Independent tensor product evaluation over a grid agrees with the
	 * surface API (this reads the actual stored control net). */
	{
		double u, v;
		ok = 1;
		for (u = 0.1; u < 0.95; u += 0.21) {
			for (v = 0.1; v < 0.95; v += 0.27) {
				double p[3], du[3], dv[3];
				double qp[3], qdu[3], qdv[3];
				eval_surface(&sf, u, v, p, du, dv);
				ref_surface(&sf, u, v, qp, qdu, qdv);
				if (!near3(p, qp, LOFT_EPS) ||
				    !near3(du, qdu, LOFT_EPS) ||
				    !near3(dv, qdv, LOFT_EPS))
					ok = 0;
			}
		}
		CHECK(ok);
	}

	/* Finite difference convergence for dv. */
	{
		double d1[3], d2[3], d3[3], p[3], du[3], dv[3];
		eval_surface(&sf, 0.33, 0.4, p, du, dv);
		fd_partial(&sf, 1, 0.33, 0.4, 1e-3, d1);
		fd_partial(&sf, 1, 0.33, 0.4, 1e-4, d2);
		fd_partial(&sf, 1, 0.33, 0.4, 1e-5, d3);
		CHECK(dist3(d2, dv) < dist3(d1, dv));
		CHECK(dist3(d3, dv) < dist3(d2, dv));
		CHECK(dist3(d3, dv) < 1e-6);
	}

	for (j = 0; j < 4; j++)
		ts_bspline_free(&sections[j]);
	ts_loft_surface_free(&sf);
}

/* ------------------------------------------------------------------ */
/* Rational group: weighted circular arcs, global weight scaling      */
/* invariance, ruled rational loft.                                   */
/* ------------------------------------------------------------------ */

static void
group_rational(void)
{
	const double ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
	const double exz[3] = {1, 0, 0}, ez[3] = {0, 0, 1};
	const double c0[3] = {0, 0, 0}, c1[3] = {1, 0.5, -1};
	const double c2[3] = {-1, 2, 0.25};
	tsBSpline arcs[3], scaled[3];
	tsLoftSurface sf = ts_loft_surface_init();
	tsLoftSurface sf_scaled = ts_loft_surface_init();
	tsStatus st;
	size_t j, k;
	int ok;

	arcs[0] = make_arc(c0, ex, ey, 2.0, 0.0, TS_PI / 2.0, 1.0, 1);
	arcs[1] = make_arc(c1, exz, ez, 1.5, 0.2, TS_PI / 2.0 + 0.2,
	                   1.0, 0);
	arcs[2] = make_arc(c2, ex, ey, 0.75, -0.3, 1.7, 1.0, 1);
	/* Globally rescaled copies of every section (positive factors). */
	{
		const double factors[3] = {2.3, 0.5, 3.14159};
		for (j = 0; j < 3; j++) {
			size_t n = ts_bspline_num_control_points(&arcs[j]);
			tsReal *cp;
			ts_bspline_control_points(&arcs[j], &cp, &st);
			for (k = 0; k < n; k++) {
				size_t c;
				for (c = 0; c < 4; c++)
					cp[k * 4 + c] *= (tsReal) factors[j];
			}
			scaled[j] = ts_bspline_init();
			ts_bspline_new(n, 4, ts_bspline_degree(&arcs[j]),
			               TS_OPENED, &scaled[j], &st);
			ts_bspline_set_control_points(&scaled[j], cp, &st);
			ts_bspline_set_knots(&scaled[j],
			                     ts_bspline_knots_ptr(&arcs[j]), &st);
			free(cp);
		}
	}

	CHECK(ts_loft_surface_new(arcs, 3, &sf, &st) == TS_SUCCESS);
	CHECK(ts_loft_surface_new(scaled, 3, &sf_scaled, &st)
	      == TS_SUCCESS);

	/* Global weight scaling must leave the surface unchanged. */
	{
		double u, v;
		ok = 1;
		for (u = 0.05; u < 0.99; u += 0.19) {
			for (v = 0.05; v < 0.99; v += 0.23) {
				double p[3], du[3], dv[3];
				double q[3], dqu[3], dqv[3];
				eval_surface(&sf, u, v, p, du, dv);
				eval_surface(&sf_scaled, u, v, q, dqu, dqv);
				if (!near3(p, q, LOFT_EPS) ||
				    !near3(du, dqu, LOFT_EPS) ||
				    !near3(dv, dqv, LOFT_EPS))
					ok = 0;
			}
		}
		CHECK(ok);
	}

	/* Every section stays an exact circle. Because a rational quadratic
	 * arc does not traverse its angle linearly in the spline parameter,
	 * the analytic checks use circle geometry (radius, plane, tangent
	 * orthogonality, exact endpoints and midpoint) while the parameter-
	 * dependent point and derivative are compared with an independent
	 * homogeneous-basis evaluation. */
	{
		const double centers[3][3] = {{0, 0, 0}, {1, 0.5, -1},
		                              {-1, 2, 0.25}};
		const double radii[3] = {2.0, 1.5, 0.75};
		const double a0[3] = {0.0, 0.2, -0.3};
		const double a1[3] = {TS_PI / 2.0, TS_PI / 2.0 + 0.2, 1.7};
		/* Plane normal of each arc. */
		const double normals[3][3] = {{0, 0, 1}, {0, 1, 0},
		                              {0, 0, 1}};
		const double us[] = {0.0, 0.137, 0.25, 0.5, 0.62, 0.75, 1.0};
		ok = 1;
		for (j = 0; j < 3; j++) {
			for (k = 0; k < 7; k++) {
				double u = us[k];
				double p[3], du[3], dv[3], vj;
				double rvec[3], rad, dotn, dotr;
				double rp[3], rd[3];
				size_t c2;
				vj = (double) j / 2.0;
				eval_surface(&sf, u, vj, p, du, dv);
				ref_curve(&arcs[j], u, rp, rd);
				/* Independent basis reference. */
				if (!near3(p, rp, LOFT_EPS) ||
				    !near3(du, rd, 2e-7))
					ok = 0;
				for (c2 = 0; c2 < 3; c2++)
					rvec[c2] = p[c2] - centers[j][c2];
				rad = sqrt(rvec[0]*rvec[0] +
				           rvec[1]*rvec[1] +
				           rvec[2]*rvec[2]);
				dotn = p[0]*normals[j][0] +
				       p[1]*normals[j][1] +
				       p[2]*normals[j][2];
				dotn -= centers[j][0]*normals[j][0] +
				        centers[j][1]*normals[j][1] +
				        centers[j][2]*normals[j][2];
				dotr = du[0]*rvec[0] +
				       du[1]*rvec[1] +
				       du[2]*rvec[2];
				if (fabs(rad - radii[j]) > LOFT_EPS ||
				    fabs(dotn) > LOFT_EPS ||
				    fabs(dotr) > 1e-7 ||
				    fabs(du[0]*normals[j][0] +
				         du[1]*normals[j][1] +
				         du[2]*normals[j][2]) > 1e-12)
					ok = 0;
			}
			/* Exact analytic endpoints and the arc midpoint. */
			{
				double probes2[3] = {0.0, 0.5, 1.0};
				size_t q2;
				for (q2 = 0; q2 < 3; q2++) {
					double u = probes2[q2];
					double theta = a0[j] +
						u * (a1[j] - a0[j]);
					double ap[3], p[3], du[3], dv[3];
					eval_surface(&sf, u, (double) j / 2.0,
					             p, du, dv);
					if (j == 1) {
						ap[0] = centers[j][0] +
							radii[j] * cos(theta);
						ap[1] = centers[j][1];
						ap[2] = centers[j][2] +
							radii[j] * sin(theta);
					} else {
						ap[0] = centers[j][0] +
							radii[j] * cos(theta);
						ap[1] = centers[j][1] +
							radii[j] * sin(theta);
						ap[2] = centers[j][2];
					}
					if (!near3(p, ap, LOFT_EPS))
						ok = 0;
				}
			}
		}
		CHECK(ok);
	}

	/* The normal du x dv is finite and non-zero on the whole surface. */
	{
		double u, v;
		ok = 1;
		for (u = 0.05; u < 0.99; u += 0.13) {
			for (v = 0.05; v < 0.99; v += 0.17) {
				double p[3], du[3], dv[3], n[3];
				eval_surface(&sf, u, v, p, du, dv);
				cross3(du, dv, n);
				if (!isfinite(n[0]) || !isfinite(n[1]) ||
				    !isfinite(n[2]) ||
				    sqrt(n[0] * n[0] + n[1] * n[1] +
				         n[2] * n[2]) < 1e-6)
					ok = 0;
			}
		}
		CHECK(ok);
	}

	/* Ruled rational loft with two arcs and midpoint check. */
	{
		tsBSpline two[2];
		tsLoftSurface ruled = ts_loft_surface_init();
		double p[3], du[3], dv[3], pa[3], pb[3], mid[3], rd[3];
		double rd2[3];
		int d;
		two[0] = make_arc(c0, ex, ey, 1.0, 0.0, TS_PI / 2.0, 1.0, 0);
		two[1] = make_arc(c1, ex, ey, 1.0, 0.0, TS_PI / 2.0, 1.0, 0);
		CHECK(ts_loft_surface_new(two, 2, &ruled, &st) == TS_SUCCESS);
		CHECK(ts_loft_surface_v_degree(&ruled) == 1);
		CHECK(ts_loft_surface_num_v_control_points(&ruled) == 2);
		eval_surface(&ruled, 0.37, 0.5, p, du, dv);
		ref_curve(&two[0], 0.37, pa, rd);
		ref_curve(&two[1], 0.37, pb, rd2);
		for (d = 0; d < 3; d++)
			mid[d] = 0.5 * (pa[d] + pb[d]);
		CHECK(near3(p, mid, LOFT_EPS));
		ts_loft_surface_free(&ruled);
		ts_bspline_free(&two[0]);
		ts_bspline_free(&two[1]);
	}

	for (j = 0; j < 3; j++) {
		ts_bspline_free(&arcs[j]);
		ts_bspline_free(&scaled[j]);
	}
	ts_loft_surface_free(&sf);
	ts_loft_surface_free(&sf_scaled);
}

/* ------------------------------------------------------------------ */
/* Derivatives group: independent homogeneous basis, finite           */
/* difference convergence, knot sides and u = 1 left derivative.      */
/* ------------------------------------------------------------------ */

static void
group_derivatives(void)
{
	tsBSpline sections[3];
	tsLoftSurface sf = ts_loft_surface_init();
	tsStatus st;
	size_t j;
	int ok;

	{
		double cp0[] = {
			0, 0, 0,  1, 1, 2,  2, 0, 1,  3, 2, 0
		};
		double k0[] = {0, 0, 0, 0.25, 1, 1, 1};
		double cp1[] = {
			0, 1, 1,  1, 2, 0,  2, 1, 3,  3, 0, 1, 4, 2, 2
		};
		double k1[] = {0, 0, 0, 0, 0.75, 1, 1, 1, 1};
		double cp2[] = {
			0, 0, 2,  2, -1, 0,  4, 1, 1
		};
		double k2[] = {0, 0, 0.4, 1, 1};
		sections[0] = make_spline(4, 3, 2, cp0, k0);
		sections[1] = make_spline(5, 3, 3, cp1, k1);
		sections[2] = make_spline(3, 3, 1, cp2, k2);
	}
	CHECK(ts_loft_surface_new(sections, 3, &sf, &st) == TS_SUCCESS);

	/* Analytic (independent homogeneous basis) derivatives on a generic
	 * grid, both axes. */
	{
		double u, v;
		double worst_du = 0.0;
		ok = 1;
		for (u = 0.07; u < 0.95; u += 0.17) {
			for (v = 0.07; v < 0.95; v += 0.23) {
				double p[3], du[3], dv[3];
				double qp[3], qdu[3], qdv[3];
				eval_surface(&sf, u, v, p, du, dv);
				ref_surface(&sf, u, v, qp, qdu, qdv);
				if (!near3(du, qdu, LOFT_EPS) ||
				    !near3(dv, qdv, LOFT_EPS))
					ok = 0;
			}
		}
		/* Tight measurement against the independent section curve. */
		for (j = 0; j < 3; j++) {
			size_t q3, c2;
			for (q3 = 0; q3 <= 200; q3++) {
				double uu = q3 / 200.0, p[3], du[3], dv[3];
				double rp[3], rd[3];
				eval_surface(&sf, uu, (double) j / 2.0,
				             p, du, dv);
				ref_curve(&sections[j], uu, rp, rd);
				for (c2 = 0; c2 < 3; c2++)
					if (fabs(du[c2] - rd[c2]) > worst_du)
						worst_du = fabs(du[c2] - rd[c2]);
			}
		}
		CHECK(ok);
		/* The independent basis is machine-close (the 1e-8 task
		 * tolerance is met by roughly six orders of magnitude). */
		CHECK(worst_du < 1e-10);
	}

	/* Central differences converge to the reported du. */
	{
		double d1[3], d2[3], d3[3], p[3], du[3], dv[3];
		eval_surface(&sf, 0.61, 0.33, p, du, dv);
		fd_partial(&sf, 0, 0.61, 0.33, 1e-3, d1);
		fd_partial(&sf, 0, 0.61, 0.33, 1e-4, d2);
		fd_partial(&sf, 0, 0.61, 0.33, 1e-5, d3);
		CHECK(dist3(d2, du) < dist3(d1, du));
		CHECK(dist3(d3, du) < dist3(d2, du));
		CHECK(dist3(d3, du) < 1e-6);
	}

	/* Both sides of the interior knots 0.25 and 0.75. */
	{
		const double probes[][2] = {
			{0.25 - 1e-4, 0.25 + 1e-4},
			{0.75 - 1e-4, 0.75 + 1e-4}
		};
		ok = 1;
		for (j = 0; j < 3; j++) {
			double vj = (double) j / 2.0;
			size_t q;
			for (q = 0; q < 2; q++) {
				double pl[3], dul[3], dvl[3];
				double pr[3], dur[3], dvr[3];
				double rpl[3], rdl[3], rpr[3], rdr[3];
				eval_surface(&sf, probes[q][0], vj,
				             pl, dul, dvl);
				eval_surface(&sf, probes[q][1], vj,
				             pr, dur, dvr);
				ref_curve(&sections[j], probes[q][0],
				          rpl, rdl);
				ref_curve(&sections[j], probes[q][1],
				          rpr, rdr);
				if (!near3(pl, rpl, LOFT_EPS) ||
				    !near3(pr, rpr, LOFT_EPS) ||
				    !near3(dul, rdl, 2e-7) ||
				    !near3(dur, rdr, 2e-7))
					ok = 0;
			}
		}
		CHECK(ok);
	}

	/* Right derivative exactly at the knot matches the right limit. */
	{
		double p[3], du[3], dv[3], pr[3], dur[3], dvr[3];
		double rp[3], rd[3];
		eval_surface(&sf, 0.25, 0.5, p, du, dv);
		eval_surface(&sf, 0.25 + 1e-6, 0.5, pr, dur, dvr);
		ref_curve(&sections[1], 0.25, rp, rd);
		/* At the knot the right-sided derivative must equal the
		 * analytic right derivative. The offset probe only checks
		 * continuity of the limit loosely, since this knot has
		 * multiplicity three and a large second derivative. */
		CHECK(near3(du, rd, 2e-7));
		CHECK(dist3(du, dur) < 1e-4);
	}

	/* u = 1 uses the left derivative. */
	{
		double p[3], du[3], dv[3], p0[3], a[3], b[3];
		double rp[3], rd[3];
		eval_surface(&sf, 1.0, 0.5, p, du, dv);
		/* Independent homogeneous-basis left limit at u = 1. */
		ref_curve(&sections[1], 1.0, rp, rd);
		CHECK(near3(du, rd, 2e-7));
		/* Backward finite differences converge to it (the endpoint has
		 * a large second derivative, so small steps are required). */
		eval_surface(&sf, 1.0 - 1e-4, 0.5, p0, a, b);
		{
			double coarse[3], fine[3];
			coarse[0] = (p[0] - p0[0]) / 1e-4;
			coarse[1] = (p[1] - p0[1]) / 1e-4;
			coarse[2] = (p[2] - p0[2]) / 1e-4;
			eval_surface(&sf, 1.0 - 1e-6, 0.5, p0, a, b);
			fine[0] = (p[0] - p0[0]) / 1e-6;
			fine[1] = (p[1] - p0[1]) / 1e-6;
			fine[2] = (p[2] - p0[2]) / 1e-6;
			CHECK(dist3(fine, du) < dist3(coarse, du));
			CHECK(dist3(fine, du) < 5e-4);
		}
		/* And it agrees with the analytic basis just inside. */
		eval_surface(&sf, 1.0 - 1e-9, 0.5, p0, a, b);
		CHECK(dist3(du, a) < 1e-5);
	}

	/* Normal from cross product matches a finite-difference normal. */
	{
		double p[3], du[3], dv[3], n[3], fdu[3], fdv[3], fn[3], len;
		double u, v;
		u = 0.43;
		v = 0.61;
		eval_surface(&sf, u, v, p, du, dv);
		cross3(du, dv, n);
		fd_partial(&sf, 0, u, v, 1e-5, fdu);
		fd_partial(&sf, 1, u, v, 1e-5, fdv);
		cross3(fdu, fdv, fn);
		len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		{
			double flen = sqrt(fn[0] * fn[0] + fn[1] * fn[1] +
			                   fn[2] * fn[2]);
			int d, same = 1;
			for (d = 0; d < 3; d++)
				if (fabs(n[d] / len - fn[d] / flen) > 1e-4)
					same = 0;
			CHECK(same);
		}
	}

	for (j = 0; j < 3; j++)
		ts_bspline_free(&sections[j]);
	ts_loft_surface_free(&sf);
}

/* ------------------------------------------------------------------ */
/* Ownership group: source release/edit independence, deep copy,      */
/* failure semantics.                                                 */
/* ------------------------------------------------------------------ */

static void
group_ownership(void)
{
	tsBSpline sections[3];
	tsLoftSurface sf = ts_loft_surface_init();
	tsLoftSurface cp = ts_loft_surface_init();
	tsStatus st;
	double saved[3], saved_du[3], saved_dv[3];
	size_t j;

	{
		double cp0[] = {0, 0, 0, 1, 1, 1, 2, 0, 2, 3, 2, 0};
		double k0[] = {0, 0, 0, 0.25, 1, 1, 1};
		double cp1[] = {0, 1, 0, 1, 2, 1, 2, 1, 2, 3, 0, 1,
		                4, 1, 0};
		double k1[] = {0, 0, 0, 0, 0.75, 1, 1, 1, 1};
		double cp2[] = {0, 0, 1, 2, 1, 0, 4, 2, 2};
		double k2[] = {0, 0, 0.6, 1, 1};
		sections[0] = make_spline(4, 3, 2, cp0, k0);
		sections[1] = make_spline(5, 3, 3, cp1, k1);
		sections[2] = make_spline(3, 3, 1, cp2, k2);
	}
	CHECK(ts_loft_surface_new(sections, 3, &sf, &st) == TS_SUCCESS);
	eval_surface(&sf, 0.41, 0.27, saved, saved_du, saved_dv);

	/* Deep copy owns distinct storage. */
	CHECK(ts_loft_surface_copy(&sf, &cp, &st) == TS_SUCCESS);
	CHECK(ts_loft_surface_control_points_ptr(&cp) !=
	      ts_loft_surface_control_points_ptr(&sf));
	{
		double p[3], du[3], dv[3];
		eval_surface(&cp, 0.41, 0.27, p, du, dv);
		CHECK(near3(p, saved, LOFT_EPS));
	}

	/* Releasing the sources must not affect the surface. */
	for (j = 0; j < 3; j++)
		ts_bspline_free(&sections[j]);
	{
		double p[3], du[3], dv[3];
		eval_surface(&sf, 0.41, 0.27, p, du, dv);
		CHECK(near3(p, saved, LOFT_EPS));
	}

	/* Releasing the original must not affect the copy. */
	ts_loft_surface_free(&sf);
	{
		double p[3], du[3], dv[3];
		eval_surface(&cp, 0.41, 0.27, p, du, dv);
		CHECK(near3(p, saved, LOFT_EPS));
	}

	/* Editing a source after construction leaves the surface untouched. */
	{
		tsBSpline edit[2];
		tsLoftSurface s2 = ts_loft_surface_init();
		tsLoftSurface s3 = ts_loft_surface_init();
		double e0[] = {0, 0, 0, 1, 1, 1, 2, 0, 2};
		double e1[] = {0, 1, 0, 1, 2, 1, 2, 1, 2};
		tsReal *ctrlp;
		double before[3], du[3], dv[3], after[3];
		edit[0] = make_spline(3, 3, 2, e0, NULL);
		edit[1] = make_spline(3, 3, 2, e1, NULL);
		CHECK(ts_loft_surface_new(edit, 2, &s2, &st) == TS_SUCCESS);
		eval_surface(&s2, 0.5, 0.5, before, du, dv);
		ts_bspline_control_points(&edit[0], &ctrlp, &st);
		ctrlp[0] = 9.0;
		ctrlp[1] = -9.0;
		ts_bspline_set_control_points(&edit[0], ctrlp, &st);
		free(ctrlp);
		/* Rebuilding with the edited source must yield a different
		 * surface, proving the edit really happened. */
		CHECK(ts_loft_surface_new(edit, 2, &s3, &st) == TS_SUCCESS);
		eval_surface(&s3, 0.5, 0.5, after, du, dv);
		{
			double p[3];
			eval_surface(&s2, 0.5, 0.5, p, du, dv);
			CHECK(near3(p, before, LOFT_EPS));
			CHECK(dist3(after, before) > 1e-3);
		}
		ts_bspline_free(&edit[0]);
		ts_bspline_free(&edit[1]);
		ts_loft_surface_free(&s2);
		ts_loft_surface_free(&s3);
	}

	/* Failures leave an existing out untouched and are reported. */
	{
		tsLoftSurface good = ts_loft_surface_init();
		tsBSpline ok_sec[2];
		tsBSpline bad_sec[2];
		double g0[] = {0, 0, 0, 1, 1, 1};
		double g1[] = {1, 0, 0, 2, 1, 1};
		double bad_dim[] = {0, 0, 1, 1}; /* 2D section */
		double bad_w[] = {0, 0, 0, 1, 1, 1, 1, -0.5};
		double nan_cp[] = {
			0, 0, 0, 1, 1, 1, 2,
			(double) NAN, 0, 1, 3, 2, 0
		};
		double good_p[3], du[3], dv[3], after_p[3];
		tsError e;
		ok_sec[0] = make_spline(2, 3, 1, g0, NULL);
		ok_sec[1] = make_spline(2, 3, 1, g1, NULL);
		CHECK(ts_loft_surface_new(ok_sec, 2, &good, &st)
		      == TS_SUCCESS);
		eval_surface(&good, 0.5, 0.5, good_p, du, dv);

		/* count < 2 */
		e = ts_loft_surface_new(ok_sec, 1, &good, &st);
		CHECK(e == TS_NUM_POINTS);
		/* invalid dimension */
		bad_sec[0] = make_spline(2, 2, 1, bad_dim, NULL);
		bad_sec[1] = make_spline(2, 3, 1, g1, NULL);
		e = ts_loft_surface_new(bad_sec, 2, &good, &st);
		CHECK(e != TS_SUCCESS);
		/* non-positive weight */
		bad_sec[0] = make_spline(2, 4, 1, bad_w, NULL);
		e = ts_loft_surface_new(bad_sec, 2, &good, &st);
		CHECK(e == TS_NO_RESULT);
		/* non-finite data */
		bad_sec[0] = make_spline(4, 3, 2, nan_cp, NULL);
		bad_sec[1] = make_spline(2, 3, 1, g1, NULL);
		e = ts_loft_surface_new(bad_sec, 2, &good, &st);
		CHECK(e == TS_NO_RESULT);
		/* unclamped section */
		{
			double ucp[] = {0, 0, 0, 1, 1, 1, 2, 0, 2};
			tsBSpline unclamped = make_spline(3, 3, 1, ucp, NULL);
			/* Five strictly increasing, non-repeated knots: the
			 * end knots are not clamped. */
			tsReal uk[] = {0.1, 0.2, 0.5, 0.8, 1.0};
			ts_bspline_set_knots(&unclamped, uk, &st);
			ts_bspline_free(&bad_sec[0]);
			bad_sec[0] = unclamped;
			e = ts_loft_surface_new(bad_sec, 2, &good, &st);
			CHECK(e == TS_MULTIPLICITY);
		}
		/* The good surface is still usable and unchanged. */
		eval_surface(&good, 0.5, 0.5, after_p, du, dv);
		CHECK(near3(good_p, after_p, LOFT_EPS));
		/* A subsequent valid call still works. */
		CHECK(ts_loft_surface_new(ok_sec, 2, &good, &st)
		      == TS_SUCCESS);
		eval_surface(&good, 0.5, 0.5, after_p, du, dv);
		CHECK(near3(good_p, after_p, LOFT_EPS));
		ts_bspline_free(&ok_sec[0]);
		ts_bspline_free(&ok_sec[1]);
		ts_bspline_free(&bad_sec[0]);
		ts_bspline_free(&bad_sec[1]);
		ts_loft_surface_free(&good);
	}

	/* Out-of-domain evaluation errors and leaves outputs untouched. */
	{
		tsBSpline sec[2];
		tsLoftSurface s2 = ts_loft_surface_init();
		tsReal p[3] = {7, 7, 7}, du[3] = {7, 7, 7};
		tsReal dv[3] = {7, 7, 7};
		double g0[] = {0, 0, 0, 1, 1, 1};
		double g1[] = {1, 0, 0, 2, 1, 1};
		sec[0] = make_spline(2, 3, 1, g0, NULL);
		sec[1] = make_spline(2, 3, 1, g1, NULL);
		CHECK(ts_loft_surface_new(sec, 2, &s2, &st) == TS_SUCCESS);
		CHECK(ts_loft_surface_eval(&s2, 1.5, 0.5, p, du, dv, &st)
		      == TS_U_UNDEFINED);
		CHECK(p[0] == 7 && du[1] == 7 && dv[2] == 7);
		CHECK(ts_loft_surface_eval(&s2, 0.5, -0.1, p, du, dv, &st)
		      == TS_U_UNDEFINED);
		CHECK(p[2] == 7 && dv[0] == 7);
		/* In-domain evaluation still works afterwards. At (0.5, 0.5)
		 * the ruled midpoint is the average of the two section
		 * midpoints: ((0.5,0.5,0.5) + (1.5,0.5,0.5)) / 2. */
		CHECK(ts_loft_surface_eval(&s2, 0.5, 0.5, p, du, dv, &st)
		      == TS_SUCCESS);
		CHECK(fabs(p[0] - 1.0) < LOFT_EPS &&
		      fabs(p[1] - 0.5) < LOFT_EPS &&
		      fabs(p[2] - 0.5) < LOFT_EPS);
		ts_bspline_free(&sec[0]);
		ts_bspline_free(&sec[1]);
		ts_loft_surface_free(&s2);
	}

	ts_loft_surface_free(&cp);
}

void
task_cases(const char *group)
{
	if (strcmp(group, "sections") == 0)
		group_sections();
	else if (strcmp(group, "rational") == 0)
		group_rational();
	else if (strcmp(group, "derivatives") == 0)
		group_derivatives();
	else if (strcmp(group, "ownership") == 0)
		group_ownership();
	else
		task_check(0, "unknown group");
}
