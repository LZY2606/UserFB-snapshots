#include "checks.h"
#include "tinyspline.h"
#include "tinyspline_surface.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int
eqv(double x, double y, double tol)
{
	return fabs(x - y) <= tol;
}

static int
eq3(const tsReal *a, const tsReal *b, double tol)
{
	return eqv(a[0], b[0], tol) &&
	       eqv(a[1], b[1], tol) &&
	       eqv(a[2], b[2], tol);
}

static tsBSpline
make_curve(size_t num_ctrlp,
           size_t dim,
           size_t degree,
           const tsReal *ctrlp,
           const tsReal *knots)
{
	tsBSpline s = ts_bspline_init();
	tsStatus status;
	if (ts_bspline_new(num_ctrlp, dim, degree, TS_OPENED,
	                   &s, &status) != TS_SUCCESS) {
		fprintf(stderr, "make_curve new failed: %s\n", status.message);
		exit(1);
	}
	if (ts_bspline_set_knots(&s, knots, &status) != TS_SUCCESS) {
		fprintf(stderr, "make_curve knots failed: %s\n", status.message);
		exit(1);
	}
	if (ts_bspline_set_control_points(&s, ctrlp, &status) != TS_SUCCESS) {
		fprintf(stderr, "make_curve ctrlp failed: %s\n",
		        status.message);
		exit(1);
	}
	return s;
}

/* ------------------------------------------------------------------ */
/* Independent reference implementations (Cox-de Boor, written from   */
/* scratch in this file and not shared with the surface module)       */
/* ------------------------------------------------------------------ */

typedef struct
{
	size_t deg;
	size_t dim;
	size_t n;
	tsReal knots[96];
	tsReal ctrlp[384]; /* up to 96 components */
} RefCurve;

static size_t
ref_find_span(const tsReal *knots, size_t degree, size_t n_knots, tsReal u)
{
	const size_t n = n_knots - degree - 2;
	size_t low, high, mid;
	if (u >= knots[n + 1])
		return n;
	if (u <= knots[degree])
		return degree;
	low = degree;
	high = n + 1;
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

static double
ref_basis(const tsReal *knots, size_t i, size_t deg, double u, int at_end,
          size_t last_index)
{
	if (deg == 0) {
		if (u >= knots[i] && u < knots[i + 1])
			return 1.0;
		if (at_end && i == last_index)
			return 1.0;
		return 0.0;
	} else {
		double left = 0.0, right = 0.0;
		double dl = knots[i + deg] - knots[i];
		double dr = knots[i + deg + 1] - knots[i + 1];
		if (dl > 0.0)
			left = (u - knots[i]) / dl *
			       ref_basis(knots, i, deg - 1, u, at_end,
			                 last_index);
		if (dr > 0.0)
			right = (knots[i + deg + 1] - u) / dr *
			        ref_basis(knots, i + 1, deg - 1, u, at_end,
			                  last_index);
		return left + right;
	}
}

static double
ref_basis_deriv(const tsReal *knots, size_t i, size_t deg, double u,
                int at_end, size_t last_index)
{
	double dl = knots[i + deg] - knots[i];
	double dr = knots[i + deg + 1] - knots[i + 1];
	double r = 0.0;
	if (dl > 0.0)
		r += (double) deg / dl *
		     ref_basis(knots, i, deg - 1, u, at_end, last_index);
	if (dr > 0.0)
		r -= (double) deg / dr *
		     ref_basis(knots, i + 1, deg - 1, u, at_end, last_index);
	return r;
}

static void
ref_curve_from(const tsBSpline *s, RefCurve *r)
{
	size_t i;
	r->deg = ts_bspline_degree(s);
	r->dim = ts_bspline_dimension(s);
	r->n = ts_bspline_num_control_points(s);
	memcpy(r->knots, ts_bspline_knots_ptr(s),
	       ts_bspline_num_knots(s) * sizeof(tsReal));
	memcpy(r->ctrlp, ts_bspline_control_points_ptr(s),
	       r->n * r->dim * sizeof(tsReal));
	for (i = r->n * r->dim; i < sizeof(r->ctrlp) / sizeof(tsReal); i++)
		r->ctrlp[i] = 0.0;
}

/* Evaluates the original section at normalized parameter t in [0, 1],
 * returning the Euclidean point and the derivative w.r.t. t. */
static void
ref_curve_eval(const RefCurve *r, double t, double point[3], double deriv[3])
{
	const size_t n_knots = r->n + r->deg + 1;
	const double lo = r->knots[r->deg];
	const double hi = r->knots[n_knots - r->deg - 1];
	const double u = lo + t * (hi - lo);
	const int at_end = (t >= 1.0);
	size_t span, i0, i, c;
	double H[4] = {0, 0, 0, 0};
	double Hd[4] = {0, 0, 0, 0};
	double w, dw;

	span = ref_find_span(r->knots, r->deg, n_knots, u);
	i0 = span - r->deg;
	for (i = 0; i <= r->deg; i++) {
		const size_t idx = i0 + i;
		const size_t last = n_knots - r->deg - 2;
		const double N = ref_basis(r->knots, idx, r->deg, u,
		                           at_end, last);
		const double Nd = ref_basis_deriv(r->knots, idx, r->deg,
		                                  u, at_end, last);
		double p[4];
		if (r->dim == 4) {
			for (c = 0; c < 4; c++)
				p[c] = r->ctrlp[idx * 4 + c];
		} else {
			p[0] = r->ctrlp[idx * 3 + 0];
			p[1] = r->ctrlp[idx * 3 + 1];
			p[2] = r->ctrlp[idx * 3 + 2];
			p[3] = 1.0;
		}
		for (c = 0; c < 4; c++) {
			H[c] += N * p[c];
			Hd[c] += Nd * p[c];
		}
	}
	w = H[3];
	dw = Hd[3];
	for (c = 0; c < 3; c++) {
		point[c] = H[c] / w;
		/* chain rule: d/dt = (hi - lo) * d/du */
		deriv[c] = (hi - lo) * (Hd[c] * w - H[c] * dw) / (w * w);
	}
}

/* Independent tensor product evaluation driven entirely by the public,
 * read-only control net and knot accessors of the loft surface. */
static void
ref_surface_eval(const tsLoftSurface *s, double u, double v,
                 double point[3], double du[3], double dv[3])
{
	const size_t deg_u = ts_loft_surface_degree_u(s);
	const size_t deg_v = ts_loft_surface_degree_v(s);
	const size_t n_u = ts_loft_surface_num_control_points_u(s);
	const size_t n_v = ts_loft_surface_num_control_points_v(s);
	const size_t nku = ts_loft_surface_num_knots_u(s);
	const size_t nkv = ts_loft_surface_num_knots_v(s);
	const tsReal *ku = ts_loft_surface_knots_u_ptr(s);
	const tsReal *kv = ts_loft_surface_knots_v_ptr(s);
	const tsReal *net = ts_loft_surface_control_points_ptr(s);
	const int end_u = (u >= 1.0);
	const int end_v = (v >= 1.0);
	size_t su, sv, i0, q0, i, q, c;
	double H[4] = {0, 0, 0, 0};
	double Hu[4] = {0, 0, 0, 0};
	double Hv[4] = {0, 0, 0, 0};

	su = ref_find_span(ku, deg_u, nku, u);
	sv = ref_find_span(kv, deg_v, nkv, v);
	i0 = su - deg_u;
	q0 = sv - deg_v;
	(void) n_u;
	for (i = 0; i <= deg_u; i++) {
		const size_t ui = i0 + i;
		const size_t last_u = nku - deg_u - 2;
		const double Nu = ref_basis(ku, ui, deg_u, u, end_u,
		                            last_u);
		const double Ndu = ref_basis_deriv(ku, ui, deg_u, u, end_u,
		                                   last_u);
		for (q = 0; q <= deg_v; q++) {
			const size_t vq = q0 + q;
			const size_t last_v = nkv - deg_v - 2;
			const double Mv = ref_basis(kv, vq, deg_v, v, end_v,
			                            last_v);
			const double Mdv = ref_basis_deriv(kv, vq, deg_v,
			                                   v, end_v, last_v);
			const tsReal *p = net + (ui * n_v + vq) * 4;
			for (c = 0; c < 4; c++) {
				H[c] += Nu * Mv * p[c];
				Hu[c] += Ndu * Mv * p[c];
				Hv[c] += Nu * Mdv * p[c];
			}
		}
	}
	for (c = 0; c < 3; c++) {
		point[c] = H[c] / H[3];
		du[c] = (Hu[c] * H[3] - H[c] * Hu[3]) / (H[3] * H[3]);
		dv[c] = (Hv[c] * H[3] - H[c] * Hv[3]) / (H[3] * H[3]);
	}
}

/* ------------------------------------------------------------------ */
/* Section builders                                                    */
/* ------------------------------------------------------------------ */

/* Non-planar sections with mixed degrees, custom interior knots and
 * non-unit parameter domains. */
static size_t
build_mixed_sections(tsBSpline sections[4])
{
	/* degree 2, interior knot at .25, wavy 3D curve */
	const tsReal c0[12] = {
		0, 0, 0,
		1, 0.5, 1,
		1.5, 2, -1,
		2, 1, 0.5
	};
	const tsReal k0[7] = {0, 0, 0, .25, 1, 1, 1};

	/* degree 3, domain [2, 5], interior knot at 4.25 (== normalized .75) */
	const tsReal c1[15] = {
		0, 0, 1,
		0.7, 1, 2,
		1.4, 0.4, 0,
		2.1, 1.6, 1.5,
		2.8, 0.8, 0.5
	};
	const tsReal k1[9] = {2, 2, 2, 2, 4.25, 5, 5, 5, 5};

	/* degree 1 line, domain [0, 4] */
	const tsReal c2[6] = {
		0, 0.2, 2,
		2.8, 1.0, -1
	};
	const tsReal k2[4] = {0, 0, 4, 4};

	/* degree 3, piecewise Bezier: interior knot .5 with multiplicity 3 */
	const tsReal c3[21] = {
		0, 0, 0,
		0.6, 1.2, 0.8,
		1.2, -0.6, 1.4,
		1.8, 0.9, 0.3,
		2.1, 1.8, -0.7,
		2.5, 0.4, 0.9,
		2.8, 1.0, 0.5
	};
	const tsReal k3[11] = {0, 0, 0, 0, .5, .5, .5, 1, 1, 1, 1};

	sections[0] = make_curve(4, 3, 2, c0, k0);
	sections[1] = make_curve(5, 3, 3, c1, k1);
	sections[2] = make_curve(2, 3, 1, c2, k2);
	sections[3] = make_curve(7, 3, 3, c3, k3);
	return 4;
}

/* Quarter circle NURBS arcs of radius r at height z. Homogeneous control
 * points: (wx, wy, wz, w). */
static tsBSpline
make_quarter_circle(double r, double z, double weight_scale)
{
	const double w = sqrt(0.5) * weight_scale;
	tsReal cp[12];
	const tsReal knots[6] = {0, 0, 0, 1, 1, 1};
	cp[0] = (tsReal) (r * weight_scale);
	cp[1] = 0;
	cp[2] = (tsReal) (z * weight_scale);
	cp[3] = (tsReal) weight_scale;
	cp[4] = (tsReal) (r * w);
	cp[5] = (tsReal) (r * w);
	cp[6] = (tsReal) (z * w);
	cp[7] = (tsReal) w;
	cp[8] = 0;
	cp[9] = (tsReal) (r * weight_scale);
	cp[10] = (tsReal) (z * weight_scale);
	cp[11] = (tsReal) weight_scale;
	return make_curve(3, 4, 2, cp, knots);
}

static size_t
build_circle_sections(tsBSpline sections[4], const double radii[4],
                      const double heights[4])
{
	size_t j;
	for (j = 0; j < 4; j++)
		sections[j] = make_quarter_circle(radii[j], heights[j], 1.0);
	return 4;
}

/* ------------------------------------------------------------------ */
/* Group: sections                                                     */
/* ------------------------------------------------------------------ */

static void
group_sections(void)
{
	tsBSpline sections[4];
	tsBSpline refs[4];
	RefCurve ref[4];
	const size_t count = build_mixed_sections(sections);
	size_t j;
	tsLoftSurface s = ts_loft_surface_init();
	tsStatus status;
	const double us[] = {0, .13, .25, .37, .5, .63, .75, .9, 1};
	const size_t n_us = sizeof(us) / sizeof(us[0]);

	for (j = 0; j < count; j++) {
		ref_curve_from(&sections[j], &ref[j]);
		refs[j] = ts_bspline_init();
		if (ts_bspline_copy(&sections[j], &refs[j], NULL) != TS_SUCCESS)
			exit(1);
	}

	task_check(ts_loft_surface_new(sections, count, &s, &status) ==
	           TS_SUCCESS, "loft of mixed sections succeeds");

	task_check(ts_loft_surface_degree_u(&s) == 3,
	           "common u degree is the maximum section degree");
	task_check(ts_loft_surface_degree_v(&s) == 3,
	           "loft axis is cubic");
	task_check(ts_loft_surface_num_control_points_v(&s) == 12,
	           "natural cubic loft of four sections has 12 v controls");
	task_check(ts_loft_surface_num_knots_u(&s) ==
	           ts_loft_surface_num_control_points_u(&s) +
	           ts_loft_surface_degree_u(&s) + 1,
	           "u knot vector length matches degree and control count");

	/* The common knot vector must carry the union of interior knots:
	 * .25 (section 0), .75 (section 1) and .5 with multiplicity three
	 * (piecewise Bezier section 3). Degree elevation may represent the
	 * unified knots as piecewise Bezier knots, which is explicitly
	 * allowed. */
	{
		const tsReal *ku = ts_loft_surface_knots_u_ptr(&s);
		const size_t nku = ts_loft_surface_num_knots_u(&s);
		size_t k, m25 = 0, m50 = 0, m75 = 0;
		for (k = 0; k < nku; k++) {
			if (fabs(ku[k] - .25) < 1e-12) m25++;
			if (fabs(ku[k] - .5) < 1e-12) m50++;
			if (fabs(ku[k] - .75) < 1e-12) m75++;
		}
		task_check(m25 >= 1 && m75 >= 1,
		           "distinct interior knots .25 and .75 are unified");
		task_check(m50 == 3,
		           "piecewise Bezier knot keeps multiplicity three");
		task_check(m25 >= 1 && m75 >= 1 &&
		           ku[0] == 0 && ku[nku - 1] == 1,
		           "unified knot vector stays clamped on [0, 1]");
	}

	/* Every section must be reproduced exactly at its v station, for
	 * arbitrary u -- not only at control points or uniform samples. */
	{
		int all_ok = 1;
		for (j = 0; j < count; j++) {
			const double vstation = (double) j / (double) (count - 1);
			size_t a;
			for (a = 0; a < n_us; a++) {
				tsReal p[3], du[3], dv[3];
				double expect[3], expect_deriv[3];
				ref_curve_eval(&ref[j], us[a], expect,
				               expect_deriv);
				if (ts_loft_surface_eval(&s, us[a], vstation,
				                         p, du, dv, NULL) != TS_SUCCESS) {
					all_ok = 0;
					break;
				}
				if (!eq3(p, expect, 1e-8))
					all_ok = 0;
			}
		}
		task_check(all_ok,
		           "surface reproduces every normalized section curve");
	}

	/* The stored control net itself (read through the public accessors)
	 * must describe the same surface: evaluate it with the independent
	 * tensor product reference. */
	{
		int all_ok = 1;
		const double probes[][2] = {
			{.07, .1}, {.33, .42}, {.5, .5}, {.81, .77},
			{.99, .93}, {.25, .25}, {.75, .5}
		};
		size_t a;
		for (a = 0; a < sizeof(probes) / sizeof(probes[0]); a++) {
			tsReal p[3], du[3], dv[3];
			double rp[3], rdu[3], rdv[3];
			ts_loft_surface_eval(&s, probes[a][0], probes[a][1],
			                     p, du, dv, NULL);
			ref_surface_eval(&s, probes[a][0], probes[a][1],
			                 rp, rdu, rdv);
			if (!eq3(p, rp, 1e-9))
				all_ok = 0;
		}
		task_check(all_ok,
		           "stored tensor product net matches public evaluation");
	}

	/* The net is homogeneous with positive weights and the four corners
	 * agree with the section end points. */
	{
		const tsReal *net = ts_loft_surface_control_points_ptr(&s);
		const size_t nu = ts_loft_surface_num_control_points_u(&s);
		const size_t nv = ts_loft_surface_num_control_points_v(&s);
		size_t i, ok = 1;
		tsReal p[3], du[3], dv[3];
		for (i = 0; i < nu * nv; i++)
			if (!(net[i * 4 + 3] > 0.0) || !isfinite(net[i * 4 + 3]))
				ok = 0;
		task_check(ok, "control net weights are all positive");
		ts_loft_surface_eval(&s, 0, 0, p, du, dv, NULL);
		task_check(eqv(p[0], 0, 1e-12) && eqv(p[1], 0, 1e-12) &&
		           eqv(p[2], 0, 1e-12),
		           "surface corner matches first section start point");
		ts_loft_surface_eval(&s, 1, 1, p, du, dv, NULL);
		task_check(eqv(p[0], 2.8, 1e-12) && eqv(p[2], .5, 1e-12),
		           "surface corner matches last section end point");
	}

	/* Interior knot: values from both sides coincide and the reported
	 * derivative at the knot is the right hand derivative. */
	{
		tsReal pl[3], pr[3], pk[3], dul[3], duk[3], dvl[3], dvk[3];
		double rpr[3], rdur[3], rdvr[3];
		ts_loft_surface_eval(&s, .25 - 1e-7, .3, pl, dul, dvl, NULL);
		ts_loft_surface_eval(&s, .25 + 1e-7, .3, pr, dul, dvl, NULL);
		ts_loft_surface_eval(&s, .25, .3, pk, duk, dvk, NULL);
		task_check(eq3(pl, pr, 1e-6),
		           "surface is continuous across interior knot .25");
		ref_surface_eval(&s, .25 + 1e-9, .3, rpr, rdur, rdvr);
		task_check(eq3(duk, rdur, 1e-5),
		           "interior knot returns the right hand u derivative");
	}

	for (j = 0; j < count; j++) {
		ts_bspline_free(&sections[j]);
		ts_bspline_free(&refs[j]);
	}
	ts_loft_surface_free(&s);
}

/* ------------------------------------------------------------------ */
/* Group: rational sections                                            */
/* ------------------------------------------------------------------ */

static void
group_rational(void)
{
	const double radii[4] = {1.0, 1.5, .8, 1.2};
	const double heights[4] = {0, .4, .8, 1.2};
	tsBSpline sections[4];
	const size_t count = build_circle_sections(sections, radii, heights);
	tsLoftSurface s = ts_loft_surface_init();
	tsStatus status;
	size_t j, a;

	task_check(ts_loft_surface_new(sections, count, &s, &status) ==
	           TS_SUCCESS, "loft of rational circle sections succeeds");

	/* Analytic reference for the standard rational quadratic quarter
	 * circle, evaluated directly from its homogeneous Bezier control
	 * points and the Bernstein basis. This is independent of both the
	 * surface module and tinyspline's de Boor code. */
	{
		int ok_point = 1, ok_tangent = 1, ok_radius = 1;
		const double us[] = {0, .1, .25, .5, .75, .9, 1};
		for (j = 0; j < count; j++) {
			const double vstation = (double) j / (count - 1);
			const double r = radii[j];
			const double z = heights[j];
			const double w = sqrt(0.5);
			/* homogeneous control points */
			const double h0[4] = {r, 0, z, 1};
			const double h1[4] = {r * w, r * w, z * w, w};
			const double h2[4] = {0, r, z, 1};
			for (a = 0; a < sizeof(us) / sizeof(us[0]); a++) {
				const double t = us[a];
				const double b0 = (1 - t) * (1 - t);
				const double b1 = 2 * t * (1 - t);
				const double b2 = t * t;
				/* Bernstein derivatives */
				const double db0 = -2 * (1 - t);
				const double db1 = 2 * (1 - 2 * t);
				const double db2 = 2 * t;
				double H[4], Hd[4];
				double expect[3], expect_du[3];
				tsReal p[3], du[3], dv[3];
				size_t c;
				for (c = 0; c < 4; c++) {
					H[c] = b0 * h0[c] + b1 * h1[c] +
					       b2 * h2[c];
					Hd[c] = db0 * h0[c] + db1 * h1[c] +
					        db2 * h2[c];
				}
				for (c = 0; c < 3; c++) {
					expect[c] = H[c] / H[3];
					expect_du[c] =
					        (Hd[c] * H[3] - H[c] * Hd[3]) /
					        (H[3] * H[3]);
				}
				ts_loft_surface_eval(&s, us[a], vstation,
				                     p, du, dv, NULL);
				if (!eq3(p, expect, 1e-8))
					ok_point = 0;
				if (!eq3(du, expect_du, 1e-7))
					ok_tangent = 0;
				/* the geometry must stay on the circle of
				 * the section's radius at its height */
				if (fabs(sqrt(p[0] * p[0] + p[1] * p[1]) -
				         r) > 1e-8)
					ok_radius = 0;
				if (fabs(p[2] - z) > 1e-8)
					ok_radius = 0;
			}
		}
		task_check(ok_point,
		           "rational sections match the analytic rational "
		           "Bezier circle points");
		task_check(ok_tangent,
		           "u derivatives match the analytic rational circle "
		           "derivatives");
		task_check(ok_radius,
		           "lofted rational sections keep their radius and "
		           "height");
	}

	/* Scaling every homogeneous control point of one section by a
	 * positive scalar must leave the whole surface unchanged. */
	{
		tsBSpline scaled[4];
		tsLoftSurface s2 = ts_loft_surface_init();
		int ok = 1;
		size_t i;
		for (j = 0; j < count; j++) {
			tsReal *cp;
			if (ts_bspline_copy(&sections[j], &scaled[j], NULL) !=
			    TS_SUCCESS)
				exit(1);
			if (ts_bspline_control_points(&scaled[j], &cp, NULL) !=
			    TS_SUCCESS)
				exit(1);
			if (j == 2)
				for (i = 0; i < ts_bspline_len_control_points(
				             &scaled[j]); i++)
					cp[i] *= (tsReal) 3.7;
			ts_bspline_set_control_points(&scaled[j], cp, NULL);
			free(cp);
		}
		task_check(ts_loft_surface_new(scaled, count, &s2, NULL) ==
		           TS_SUCCESS, "scaled weight sections loft successfully");
		for (a = 0; a < 11; a++) {
			const double u = (double) a / 10.0;
			const double v = (a % 4) / 3.0;
			tsReal p1[3], du1[3], dv1[3], p2[3], du2[3], dv2[3];
			ts_loft_surface_eval(&s, u, v, p1, du1, dv1, NULL);
			ts_loft_surface_eval(&s2, u, v, p2, du2, dv2, NULL);
			if (!eq3(p1, p2, 1e-8) || !eq3(du1, du2, 1e-7) ||
			    !eq3(dv1, dv2, 1e-7))
				ok = 0;
		}
		task_check(ok, "global positive weight scaling leaves surface "
		           "and derivatives invariant");
		for (j = 0; j < count; j++)
			ts_bspline_free(&scaled[j]);
		ts_loft_surface_free(&s2);
	}

	/* Bad weights and non-finite data are rejected and the existing
	 * surface stays usable. */
	{
		tsBSpline bad[4];
		tsReal *cp;
		tsLoftSurface untouched = ts_loft_surface_init();
		tsReal before[3], du[3], dv[3], after[3];
		for (j = 0; j < count; j++)
			if (ts_bspline_copy(&sections[j], &bad[j], NULL) !=
			    TS_SUCCESS)
				exit(1);
		ts_bspline_control_points(&bad[1], &cp, NULL);
		cp[7] = (tsReal) -0.5; /* negative weight */
		ts_bspline_set_control_points(&bad[1], cp, NULL);
		free(cp);
		ts_loft_surface_eval(&s, .4, .4, before, du, dv, NULL);
		task_check(ts_loft_surface_new(bad, count, &untouched, &status) ==
		           TS_NO_RESULT, "negative weight is rejected");
		ts_loft_surface_eval(&s, .4, .4, after, du, dv, NULL);
		task_check(eq3(before, after, 0.0),
		           "failed loft leaves the existing surface untouched");

		ts_bspline_control_points(&bad[1], &cp, NULL);
		cp[7] = (tsReal) 0.0; /* zero weight */
		ts_bspline_set_control_points(&bad[1], cp, NULL);
		free(cp);
		task_check(ts_loft_surface_new(bad, count, &untouched, NULL) ==
		           TS_NO_RESULT, "zero weight is rejected");

		ts_bspline_control_points(&bad[1], &cp, NULL);
		cp[3] = (tsReal) NAN;
		ts_bspline_set_control_points(&bad[1], cp, NULL);
		free(cp);
		task_check(ts_loft_surface_new(bad, count, &untouched, NULL) !=
		           TS_SUCCESS, "non-finite control data is rejected");
		for (j = 0; j < count; j++)
			ts_bspline_free(&bad[j]);
		ts_loft_surface_free(&untouched);
	}

	/* Illegal shapes are reported with the documented error codes. */
	{
		tsBSpline dim2 = ts_bspline_init();
		tsBSpline single[1];
		tsLoftSurface dummy = ts_loft_surface_init();
		const tsReal cp2[4] = {0, 0, 1, 1};
		const tsReal k2[4] = {0, 0, 1, 1};
		dim2 = make_curve(2, 2, 1, cp2, k2);
		task_check(ts_loft_surface_new(&dim2, 1, &dummy, NULL) ==
		           TS_NUM_POINTS, "a single section is rejected");
		single[0] = dim2;
		task_check(ts_loft_surface_new(single, 1, &dummy, &status) ==
		           TS_NUM_POINTS, "count below two is TS_NUM_POINTS");
		task_check(ts_loft_surface_new(sections, count, &dummy, NULL) ==
		           TS_SUCCESS, "dummy surface can still be created after "
		           "validation failures");
		ts_bspline_free(&dim2);
		ts_loft_surface_free(&dummy);
	}

	/* A mix of non-rational (dim 3) and rational (dim 4) sections can be
	 * lofted together, and each section is still reproduced. */
	{
		tsBSpline mixed_dim[3];
		tsLoftSurface ms = ts_loft_surface_init();
		RefCurve rref[3];
		const tsReal p3[9] = {0, 0, 0, 1, 1, 1, 2, 0, 1};
		const tsReal k3[6] = {0, 0, 0, 1, 1, 1};
		const tsReal p4[12] = {0, 0, 0, 1, 1.4, 1.4, 0, .7071,
		                       2, 0, 1, 1};
		const tsReal k4[6] = {0, 0, 0, 1, 1, 1};
		int ok = 1;
		size_t aa;
		mixed_dim[0] = make_curve(3, 3, 2, p3, k3);
		mixed_dim[1] = make_curve(3, 4, 2, p4, k4);
		mixed_dim[2] = make_curve(3, 3, 2, p3, k3);
		for (j = 0; j < 3; j++)
			ref_curve_from(&mixed_dim[j], &rref[j]);
		task_check(ts_loft_surface_new(mixed_dim, 3, &ms, NULL) ==
		           TS_SUCCESS, "mixed dim 3 and dim 4 sections loft");
		for (j = 0; j < 3; j++) {
			const double vv = j / 2.0;
			for (aa = 0; aa <= 10; aa++) {
				double expect[3], ed[3];
				tsReal pp[3], du[3], dv[3];
				ref_curve_eval(&rref[j], aa / 10.0, expect, ed);
				ts_loft_surface_eval(&ms, aa / 10.0, vv,
				                     pp, du, dv, NULL);
				if (!eq3(pp, expect, 1e-8))
					ok = 0;
			}
		}
		task_check(ok, "mixed dimensionality sections are reproduced");
		for (j = 0; j < 3; j++)
			ts_bspline_free(&mixed_dim[j]);
		ts_loft_surface_free(&ms);
	}

	/* Illegal dimensionality is reported with TS_DIM_ZERO. */
	{
		tsBSpline dim2_pair[2];
		tsLoftSurface dummy = ts_loft_surface_init();
		const tsReal cp2[4] = {0, 0, 1, 1};
		const tsReal k2[4] = {0, 0, 1, 1};
		dim2_pair[0] = make_curve(2, 2, 1, cp2, k2);
		dim2_pair[1] = make_curve(2, 2, 1, cp2, k2);
		task_check(ts_loft_surface_new(dim2_pair, 2, &dummy, &status) ==
		           TS_DIM_ZERO,
		           "two dimensional sections report TS_DIM_ZERO");
		ts_bspline_free(&dim2_pair[0]);
		ts_bspline_free(&dim2_pair[1]);
		ts_loft_surface_free(&dummy);
	}

	/* A true break (interior knot multiplicity == order) is rejected
	 * with TS_MULTIPLICITY. */
	{
		tsBSpline brk[2];
		tsLoftSurface dummy = ts_loft_surface_init();
		const tsReal cp[12] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3};
		const tsReal kb[6] = {0, 0, .5, .5, 1, 1};
		brk[0] = make_curve(4, 3, 1, cp, kb);
		brk[1] = make_curve(4, 3, 1, cp, kb);
		task_check(ts_loft_surface_new(brk, 2, &dummy, &status) ==
		           TS_MULTIPLICITY,
		           "a broken section reports TS_MULTIPLICITY");
		ts_bspline_free(&brk[0]);
		ts_bspline_free(&brk[1]);
		ts_loft_surface_free(&dummy);
	}

	/* Non-clamped section is rejected. */
	{
		tsBSpline open_s[2];
		tsLoftSurface dummy = ts_loft_surface_init();
		const tsReal cp[6] = {0, 0, 0, 1, 1, 1};
		const tsReal k_open[4] = {0, .2, .8, 1};
		open_s[0] = make_curve(2, 3, 1, cp, k_open);
		open_s[1] = make_curve(2, 3, 1, cp, k_open);
		task_check(ts_loft_surface_new(open_s, 2, &dummy, NULL) !=
		           TS_SUCCESS, "non-clamped sections are rejected");
		ts_bspline_free(&open_s[0]);
		ts_bspline_free(&open_s[1]);
		ts_loft_surface_free(&dummy);
	}

	for (j = 0; j < count; j++)
		ts_bspline_free(&sections[j]);
	ts_loft_surface_free(&s);
}

/* ------------------------------------------------------------------ */
/* Group: derivatives                                                  */
/* ------------------------------------------------------------------ */

static void
group_derivatives(void)
{
	tsBSpline mixed[4];
	tsBSpline circles[4];
	const double radii[4] = {1.0, 1.3, .9, 1.1};
	const double heights[4] = {0, .3, .6, .9};
	const size_t n_mixed = build_mixed_sections(mixed);
	const size_t n_circ = build_circle_sections(circles, radii, heights);
	tsLoftSurface sm = ts_loft_surface_init();
	tsLoftSurface sc = ts_loft_surface_init();
	size_t a;

	task_check(ts_loft_surface_new(mixed, n_mixed, &sm, NULL) == TS_SUCCESS,
	           "mixed surface for derivative checks is built");
	task_check(ts_loft_surface_new(circles, n_circ, &sc, NULL) ==
	           TS_SUCCESS, "circle surface for derivative checks is built");

	/* Both partial derivatives against the independent homogeneous
	 * tensor product reference, including interior knot locations. */
	{
		int ok = 1;
		const double probes[][2] = {
			{.05, .2}, {.25, .333}, {.4999, .5}, {.5001, .7},
			{.75, .25}, {.9, .9}, {.123, .678}, {1, 1}
		};
		for (a = 0; a < sizeof(probes) / sizeof(probes[0]); a++) {
			tsReal p[3], du[3], dv[3];
			double rp[3], rdu[3], rdv[3];
			ts_loft_surface_eval(&sm, probes[a][0], probes[a][1],
			                     p, du, dv, NULL);
			ref_surface_eval(&sm, probes[a][0], probes[a][1],
			                 rp, rdu, rdv);
			if (!eq3(du, rdu, 1e-7) || !eq3(dv, rdv, 1e-7))
				ok = 0;
		}
		task_check(ok, "analytic partials match independent homogeneous "
		           "basis reference");
	}

	/* Centered finite differences must converge toward the analytic
	 * derivatives as the step shrinks. */
	{
		int ok = 1;
		const double u = .4321, v = .31415;
		const double hs[3] = {1e-3, 1e-4, 1e-5};
		double err_du[3], err_dv[3];
		tsReal p[3], du[3], dv[3];
		tsReal pp[3], d1[3], d2[3], pm[3], qm[3], qp[3];
		for (a = 0; a < 3; a++) {
			const double h = hs[a];
			size_t c;
			ts_loft_surface_eval(&sm, u, v, p, du, dv, NULL);
			ts_loft_surface_eval(&sm, u + h, v, pp, d1, d2, NULL);
			ts_loft_surface_eval(&sm, u - h, v, pm, d1, d2, NULL);
			ts_loft_surface_eval(&sm, u, v + h, qp, d1, d2, NULL);
			ts_loft_surface_eval(&sm, u, v - h, qm, d1, d2, NULL);
			err_du[a] = 0;
			err_dv[a] = 0;
			for (c = 0; c < 3; c++) {
				const double fdu = (pp[c] - pm[c]) / (2 * h);
				const double fdv = (qp[c] - qm[c]) / (2 * h);
				err_du[a] += fabs(fdu - du[c]);
				err_dv[a] += fabs(fdv - dv[c]);
			}
		}
		if (!(err_du[1] < 1e-6 && err_dv[1] < 1e-6))
			ok = 0;
		if (!(err_du[2] < err_du[0] && err_dv[2] < err_dv[0]))
			ok = 0;
		task_check(ok, "finite difference derivatives converge to the "
		           "analytic ones");
	}

	/* Right hand derivative at an interior knot and left hand
	 * derivative at u = 1. */
	{
		tsReal pk[3], duk[3], dvk[3];
		double rp[3], rdu[3], rdv[3];
		ts_loft_surface_eval(&sm, .5, .4, pk, duk, dvk, NULL);
		ref_surface_eval(&sm, .5 + 1e-9, .4, rp, rdu, rdv);
		task_check(eq3(duk, rdu, 1e-5),
		           "right hand derivative at u = .5 is used");
		ts_loft_surface_eval(&sm, 1, .4, pk, duk, dvk, NULL);
		ref_surface_eval(&sm, 1, .4, rp, rdu, rdv);
		task_check(eq3(duk, rdu, 1e-8),
		           "u = 1 returns the left hand derivative");
	}

	/* Normal from du x dv: finite, non-zero and aligned with the
	 * finite difference normal. */
	{
		const double u = .37, v = .62, h = 1e-5;
		tsReal p[3], du[3], dv[3];
		tsReal pu[3], mu[3], pv[3], mv[3], junk[3];
		tsReal n[3], nfd[3], dot, len, lenfd;
		size_t c;
		ts_loft_surface_eval(&sc, u, v, p, du, dv, NULL);
		ts_loft_surface_eval(&sc, u + h, v, pu, junk, junk, NULL);
		ts_loft_surface_eval(&sc, u - h, v, mu, junk, junk, NULL);
		ts_loft_surface_eval(&sc, u, v + h, pv, junk, junk, NULL);
		ts_loft_surface_eval(&sc, u, v - h, mv, junk, junk, NULL);
		n[0] = du[1] * dv[2] - du[2] * dv[1];
		n[1] = du[2] * dv[0] - du[0] * dv[2];
		n[2] = du[0] * dv[1] - du[1] * dv[0];
		{
			tsReal tu[3], tv[3];
			for (c = 0; c < 3; c++) {
				tu[c] = (pu[c] - mu[c]) / (2 * h);
				tv[c] = (pv[c] - mv[c]) / (2 * h);
			}
			nfd[0] = tu[1] * tv[2] - tu[2] * tv[1];
			nfd[1] = tu[2] * tv[0] - tu[0] * tv[2];
			nfd[2] = tu[0] * tv[1] - tu[1] * tv[0];
		}
		len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		lenfd = sqrt(nfd[0] * nfd[0] + nfd[1] * nfd[1] +
		             nfd[2] * nfd[2]);
		dot = 0;
		for (c = 0; c < 3; c++)
			dot += n[c] * nfd[c] / (len * lenfd);
		task_check(isfinite(len) && len > 0,
		           "cross product normal is finite and non-zero");
		task_check(fabs(fabs(dot) - 1.0) < 1e-4,
		           "analytic normal aligns with finite difference normal");
	}

	/* Ruled case (count == 2): the mid section is the straight
	 * interpolation of two non-rational sections. */
	{
		tsBSpline two[2];
		tsLoftSurface ruled = ts_loft_surface_init();
		RefCurve r0, r1;
		const tsReal cp_a[6] = {0, 0, 0, 2, 0, 0};
		const tsReal cp_b[6] = {0, 2, 1, 2, 2, 1};
		const tsReal k[4] = {0, 0, 1, 1};
		int ok = 1;
		two[0] = make_curve(2, 3, 1, cp_a, k);
		two[1] = make_curve(2, 3, 1, cp_b, k);
		ref_curve_from(&two[0], &r0);
		ref_curve_from(&two[1], &r1);
		task_check(ts_loft_surface_new(two, 2, &ruled, NULL) == TS_SUCCESS,
		           "two section ruled loft succeeds");
		for (a = 0; a < 11; a++) {
			const double u = (double) a / 10.0;
			double p0[3], d0[3], p1[3], d1[3], mid[3];
			tsReal p[3], du[3], dv[3];
			ref_curve_eval(&r0, u, p0, d0);
			ref_curve_eval(&r1, u, p1, d1);
			ts_loft_surface_eval(&ruled, u, .5, p, du, dv, NULL);
			for (size_t c = 0; c < 3; c++)
				mid[c] = .5 * (p0[c] + p1[c]);
			if (!eq3(p, mid, 1e-9))
				ok = 0;
		}		task_check(ok, "ruled loft mid section is the linear blend");
		ts_bspline_free(&two[0]);
		ts_bspline_free(&two[1]);
		ts_loft_surface_free(&ruled);
	}

	for (a = 0; a < n_mixed; a++)
		ts_bspline_free(&mixed[a]);
	for (a = 0; a < n_circ; a++)
		ts_bspline_free(&circles[a]);
	ts_loft_surface_free(&sm);
	ts_loft_surface_free(&sc);
}

/* ------------------------------------------------------------------ */
/* Group: ownership and error handling                                 */
/* ------------------------------------------------------------------ */

static void
group_ownership(void)
{
	tsBSpline sections[4];
	const size_t count = build_mixed_sections(sections);
	tsLoftSurface s = ts_loft_surface_init();
	tsLoftSurface cp = ts_loft_surface_init();
	tsReal snapshot[11][3];
	tsReal p[3], du[3], dv[3];
	size_t a, j;

	task_check(ts_loft_surface_new(sections, count, &s, NULL) == TS_SUCCESS,
	           "surface built for ownership checks");

	for (a = 0; a < 11; a++)
		ts_loft_surface_eval(&s, (double) a / 10.0, .5,
		                     snapshot[a], du, dv, NULL);

	/* Releasing the sources must not affect the surface. */
	for (j = 0; j < count; j++)
		ts_bspline_free(&sections[j]);
	{
		int ok = 1;
		for (a = 0; a < 11; a++) {
			ts_loft_surface_eval(&s, (double) a / 10.0, .5,
			                     p, du, dv, NULL);
			if (!eq3(p, snapshot[a], 0.0))
				ok = 0;
		}
		task_check(ok, "surface survives releasing the source sections");
	}

	/* Rebuild sources, mutate them, the existing surface is independent. */
	{
		tsBSpline again[4];
		tsReal *ctrlp;
		build_mixed_sections(again);
		ts_bspline_control_points(&again[0], &ctrlp, NULL);
		ctrlp[0] = (tsReal) 1234.0;
		ctrlp[1] = (tsReal) -5678.0;
		ts_bspline_set_control_points(&again[0], ctrlp, NULL);
		free(ctrlp);
		ts_loft_surface_eval(&s, .3, .5, p, du, dv, NULL);
		task_check(eqv(p[0], snapshot[3][0], 0.0) &&
		           eqv(p[1], snapshot[3][1], 0.0),
		           "editing a source curve leaves the surface unchanged");
		for (j = 0; j < 4; j++)
			ts_bspline_free(&again[j]);
	}

	/* Deep copy: distinct storage, identical values, survives free. */
	task_check(ts_loft_surface_copy(&s, &cp, NULL) == TS_SUCCESS,
	           "deep copy succeeds");
	task_check(ts_loft_surface_control_points_ptr(&cp) !=
	           ts_loft_surface_control_points_ptr(&s),
	           "deep copy owns separate control net storage");
	{
		int ok = 1;
		for (a = 0; a < 11; a++) {
			tsReal pc[3], duc[3], dvc[3];
			ts_loft_surface_eval(&cp, (double) a / 10.0, .5,
			                     pc, duc, dvc, NULL);
			if (!eq3(pc, snapshot[a], 1e-12))
				ok = 0;
		}
		task_check(ok, "deep copy evaluates identically");
	}
	ts_loft_surface_free(&s);
	{
		int ok = 1;
		for (a = 0; a < 11; a++) {
			tsReal pc[3], duc[3], dvc[3];
			ts_loft_surface_eval(&cp, (double) a / 10.0, .5,
			                     pc, duc, dvc, NULL);
			if (!eq3(pc, snapshot[a], 1e-12))
				ok = 0;
		}
		task_check(ok, "copied surface survives freeing the original");
	}

	/* Failed construction leaves the output surface intact. */
	{
		tsBSpline bad[2];
		tsLoftSurface target = ts_loft_surface_init();
		tsReal before[3], after[3];
		const tsReal goodcp[6] = {0, 0, 0, 1, 1, 1};
		const tsReal goodk[4] = {0, 0, 1, 1};
		const tsReal bank[12] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3};
		const tsReal bk[5] = {0, 0, .5, 1, 1};
		bad[0] = make_curve(2, 3, 1, goodcp, goodk);
		bad[1] = make_curve(3, 4, 1, bank, bk);
		task_check(ts_loft_surface_new(bad, 2, &target, NULL) == TS_SUCCESS,
		           "reference surface for failure handling is built");
		ts_loft_surface_eval(&target, .6, .6, before, du, dv, NULL);
		ts_bspline_free(&bad[1]);
		bad[1] = make_curve(2, 3, 1, goodcp, goodk);
		/* count == 1 must not touch target */
		task_check(ts_loft_surface_new(bad, 1, &target, NULL) ==
		           TS_NUM_POINTS, "construction failure is reported");
		ts_loft_surface_eval(&target, .6, .6, after, du, dv, NULL);
		task_check(eq3(before, after, 0.0),
		           "failed construction leaves out unchanged");
		ts_bspline_free(&bad[0]);
		ts_bspline_free(&bad[1]);
		ts_loft_surface_free(&target);
	}

	/* Out of domain evaluation: correct status and untouched outputs,
	 * followed by continued successful use. */
	{
		tsReal guard_p[3] = {7, 7, 7};
		tsReal guard_du[3] = {8, 8, 8};
		tsReal guard_dv[3] = {9, 9, 9};
		tsStatus st;
		task_check(ts_loft_surface_eval(&cp, 1.5, .5, guard_p, guard_du,
		                                guard_dv, &st) == TS_U_UNDEFINED,
		           "u outside [0,1] reports TS_U_UNDEFINED");
		task_check(eqv(guard_p[0], 7, 0) && eqv(guard_du[1], 8, 0) &&
		           eqv(guard_dv[2], 9, 0),
		           "failed evaluation leaves all output arrays untouched");
		task_check(ts_loft_surface_eval(&cp, .5, -0.1, guard_p, guard_du,
		                                guard_dv, NULL) == TS_U_UNDEFINED,
		           "v outside [0,1] reports TS_U_UNDEFINED");
		task_check(ts_loft_surface_eval(&cp, NAN, .5, guard_p, guard_du,
		                                guard_dv, NULL) == TS_U_UNDEFINED,
		           "non-finite u reports TS_U_UNDEFINED");
		ts_loft_surface_eval(&cp, .5, .5, p, du, dv, &st);
		task_check(st.code == TS_SUCCESS && isfinite(p[0]) &&
		           isfinite(du[0]) && isfinite(dv[0]),
		           "surface keeps working after repeated failures");
	}

	ts_loft_surface_free(&cp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

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
	else {
		fprintf(stderr, "unknown group: %s\n", group);
		exit(2);
	}
}
