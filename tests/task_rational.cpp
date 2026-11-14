/*
 * Tests for rational (NURBS) support in TinySpline.
 *
 * The tests are registered with the task driver: `task_cases' is an array of
 * cases grouped into `rational_eval', `rational_edit', and `rational_sweep'.
 * Each case returns 0 on success (all checks passed) and non-zero otherwise.
 */

#include "../task_support/check.h"

#include "tinysplinecxx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

const double EPS = 1e-12;

bool
close_to(double a, double b, double eps = EPS)
{
	return std::fabs(a - b) <= eps;
}

bool
close_vec(const std::vector<double> &a,
          const std::vector<double> &b,
          double eps = EPS)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (!close_to(a[i], b[i], eps))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Exact quarter circle: rational quadratic Bezier with weights        */
/* (1, sqrt(2)/2, 1) and control points (1,0), (1,1), (0,1).           */
/* ------------------------------------------------------------------ */

const double W_MID = 0.70710678118654752440084436210485; /* sqrt(2)/2 */

void
quarter_circle_weights(std::vector<double> &w)
{
	w.resize(3);
	w[0] = 1.0;
	w[1] = W_MID;
	w[2] = 1.0;
}

void
quarter_circle_weights(double *w)
{
	w[0] = 1.0;
	w[1] = W_MID;
	w[2] = 1.0;
}

void
quarter_circle_points(double *p)
{
	p[0] = 1.0; p[1] = 0.0;
	p[2] = 1.0; p[3] = 1.0;
	p[4] = 0.0; p[5] = 1.0;
}

/* Reference evaluation of the rational quadratic Bezier in 3D. `dim' is
 * the number of components of each point; points and weights have 3 entries. */
void
rational_bezier(double u,
                const double *points,
                const double *weights,
                size_t dim,
                std::vector<double> &value)
{
	const double b0 = (1.0 - u) * (1.0 - u);
	const double b1 = 2.0 * u * (1.0 - u);
	const double b2 = u * u;
	double wsum = b0 * weights[0] + b1 * weights[1] + b2 * weights[2];
	value.assign(dim, 0.0);
	for (size_t d = 0; d < dim; ++d) {
		value[d] = (b0 * weights[0] * points[d]
		            + b1 * weights[1] * points[dim + d]
		            + b2 * weights[2] * points[2*dim + d]) / wsum;
	}
}

/* Reference derivatives (orders 0, 1, 2) of the rational quadratic Bezier. */
void
rational_bezier_derivatives(double u,
                            const double *points,
                            const double *weights,
                            size_t dim,
                            std::vector<double> &values)
{
	const double b[3]  = {(1.0 - u) * (1.0 - u),
	                      2.0 * u * (1.0 - u),
	                      u * u};
	const double b1[3] = {-2.0 * (1.0 - u),
	                      2.0 * (1.0 - 2.0 * u),
	                      2.0 * u};
	const double b2[3] = {2.0, -4.0, 2.0};

	double W = 0.0, W1 = 0.0, W2 = 0.0;
	std::vector<double> N(dim, 0.0), N1(dim, 0.0), N2(dim, 0.0);
	for (size_t i = 0; i < 3; ++i) {
		W  += weights[i] * b[i];
		W1 += weights[i] * b1[i];
		W2 += weights[i] * b2[i];
		for (size_t d = 0; d < dim; ++d) {
			const double p = points[i*dim + d];
			N[d]  += weights[i] * b[i]  * p;
			N1[d] += weights[i] * b1[i] * p;
			N2[d] += weights[i] * b2[i] * p;
		}
	}

	values.assign(3 * dim, 0.0);
	std::vector<double> C(dim), C1(dim);
	for (size_t d = 0; d < dim; ++d) {
		C[d]  = N[d] / W;
		C1[d] = (N1[d] - W1 * C[d]) / W;
	}
	for (size_t d = 0; d < dim; ++d) {
		values[d]           = C[d];
		values[dim + d]     = C1[d];
		values[2*dim + d]   = (N2[d] - 2.0 * W1 * C1[d] - W2 * C[d]) / W;
	}
}

int
make_quarter_circle(tinyspline::BSpline &spline)
{
	double ctrlp[6];
	quarter_circle_points(ctrlp);
	spline = tinyspline::BSpline(3, 2, 2, tinyspline::BSpline::Beziers);
	spline.setControlPoints(std::vector<double>(ctrlp, ctrlp + 6));
	std::vector<double> w;
	quarter_circle_weights(w);
	spline.setWeights(w);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Non-planar rational rail (quadratic, non-constant weights).         */
/* ------------------------------------------------------------------ */

void
rail_points(double *p)
{
	p[0] = 1.0;  p[1] = 0.0;  p[2] = 0.0;
	p[3] = 1.0;  p[4] = 1.0;  p[5] = 0.5;
	p[6] = 0.0;  p[7] = 1.0;  p[8] = 1.0;
}

void
rail_weights(std::vector<double> &w)
{
	w.resize(3);
	w[0] = 1.0;
	w[1] = 0.6;
	w[2] = 1.3;
}

void
rail_weights(double *w)
{
	w[0] = 1.0;
	w[1] = 0.6;
	w[2] = 1.3;
}

int
make_rail(tinyspline::BSpline &spline)
{
	double ctrlp[9];
	rail_points(ctrlp);
	spline = tinyspline::BSpline(3, 3, 2, tinyspline::BSpline::Beziers);
	spline.setControlPoints(std::vector<double>(ctrlp, ctrlp + 9));
	std::vector<double> w;
	rail_weights(w);
	spline.setWeights(w);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rational_eval: coordinates, derivatives, weight semantics.          */
/* ------------------------------------------------------------------ */

int
eval_quarter_circle_coordinates(void)
{
	tinyspline::BSpline spline;
	make_quarter_circle(spline);

	TASK_CHECK(close_vec(spline.weights(),
	                     std::vector<double>{1.0, W_MID, 1.0}));

	for (int i = 0; i <= 20; ++i) {
		const double u = i / 20.0;
		const std::vector<double> result = spline.eval(u).result();
		std::vector<double> expected;
		rational_bezier(u, spline.controlPoints().data(),
		                spline.weights().data(), 2, expected);
		TASK_CHECK(close_vec(result, expected, 1e-14));
		TASK_CHECK(close_to(std::hypot(result[0], result[1]), 1.0));

		/* Public DeBoorNet semantics stay euclidean. */
		tinyspline::DeBoorNet net = spline.eval(u);
		TASK_CHECK(net.dimension() == 2);
		TASK_CHECK(net.points().size() == net.points().size() / 2 * 2);
		TASK_CHECK(close_vec(net.result(),
		                     std::vector<double>(result.begin(),
		                                         result.end())));
	}

	/* eval_all and sample return rational euclidean points. */
	const std::vector<double> all =
		spline.evalAll(std::vector<double>{0.0, 0.25, 0.5, 0.75, 1.0});
	TASK_CHECK(all.size() == 10);
	for (size_t i = 0; i < 5; ++i) {
		std::vector<double> expected;
		rational_bezier(i * 0.25, spline.controlPoints().data(),
		                spline.weights().data(), 2, expected);
		TASK_CHECK(close_to(all[2*i], expected[0], 1e-14));
		TASK_CHECK(close_to(all[2*i + 1], expected[1], 1e-14));
	}
	const std::vector<double> sampled = spline.sample(17);
	TASK_CHECK(sampled.size() == 34);
	for (size_t i = 0; i < 17; ++i)
		TASK_CHECK(close_to(std::hypot(sampled[2*i], sampled[2*i + 1]),
		                    1.0));

	/* Endpoints are exactly the control points. */
	TASK_CHECK(close_to(spline.eval(0.0).result()[0], 1.0));
	TASK_CHECK(close_to(spline.eval(0.0).result()[1], 0.0));
	TASK_CHECK(close_to(spline.eval(1.0).result()[0], 0.0));
	TASK_CHECK(close_to(spline.eval(1.0).result()[1], 1.0));

	return 0;
}

int
eval_quarter_circle_derivatives(void)
{
	tinyspline::BSpline spline;
	make_quarter_circle(spline);
	const std::vector<double> ctrlp = spline.controlPoints();
	const std::vector<double> w = spline.weights();

	for (int i = 0; i <= 20; ++i) {
		const double u = i / 20.0;
		const std::vector<double> got = spline.evalDerivatives(u, 2);
		TASK_CHECK(got.size() == 6);

		std::vector<double> expected;
		rational_bezier_derivatives(u, ctrlp.data(), w.data(), 2,
		                            expected);
		TASK_CHECK(close_vec(got, expected, 1e-12));

		/* Circle invariants: C . C' == 0 and C . C'' == -|C'|^2. */
		const double d0 = got[0] * got[2] + got[1] * got[3];
		const double d1 = got[0] * got[4] + got[1] * got[5]
		                + got[2] * got[2] + got[3] * got[3];
		TASK_CHECK(close_to(d0, 0.0, 1e-11));
		TASK_CHECK(close_to(d1, 0.0, 1e-11));
	}

	/* A partial result (order 0 and 1) must be the prefix. */
	const std::vector<double> one = spline.evalDerivatives(0.37, 1);
	const std::vector<double> two = spline.evalDerivatives(0.37, 2);
	TASK_CHECK(one.size() == 4);
	TASK_CHECK(close_vec(one, std::vector<double>(two.begin(),
	                                             two.begin() + 4)));

	/* Order > 2 is not supported. */
	{
		bool threw = false;
		try {
			spline.evalDerivatives(0.5, 3);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}

	/* Insert an interior knot of multiplicity 1: degree 2 => C^1 there,
	 * hence order 2 must be rejected while orders 0 and 1 still work. */
	tinyspline::BSpline refined = spline.insertKnot(0.5, 1);
	{
		bool threw = false;
		try {
			refined.evalDerivatives(0.5, 2);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}
	TASK_CHECK(refined.evalDerivatives(0.5, 1).size() == 4);

	/* Lower-degree rational splines: a rational line has a non-constant
	 * first derivative and a zero second derivative. C(u) =
	 * (P1*u*w1) / ((1-u)*w0 + u*w1) in each coordinate. */
	{
		tinyspline::BSpline line(2, 2, 1, tinyspline::BSpline::Clamped);
		line.setControlPoints(std::vector<double>{0, 0, 10, 10});
		line.setWeights(std::vector<double>{1.0, 3.0});
		for (int i = 0; i <= 4; ++i) {
			const double u = i / 4.0;
			const std::vector<double> d = line.evalDerivatives(u, 1);
			const double den = (1.0 - u) + 3.0 * u;
			const double expected = 30.0 / (den * den);
			TASK_CHECK(close_to(d[0], 10.0 * 3.0 * u / den));
			TASK_CHECK(close_to(d[1], 10.0 * 3.0 * u / den));
			TASK_CHECK(close_to(d[2], expected));
			TASK_CHECK(close_to(d[3], expected));
		}
		/* Degree 1 has no second derivative anywhere. */
		bool threw = false;
		try {
			line.evalDerivatives(0.5, 2);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}

	/* Degree 0 (a point): only order 0 is available. */
	{
		tinyspline::BSpline point(1, 2, 0, tinyspline::BSpline::Clamped);
		point.setControlPoints(std::vector<double>{3, 4});
		point.setWeights(std::vector<double>{2.0});
		const std::vector<double> d = point.evalDerivatives(0.5, 0);
		TASK_CHECK(close_to(d[0], 3.0));
		TASK_CHECK(close_to(d[1], 4.0));
		bool threw = false;
		try {
			point.evalDerivatives(0.5, 1);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}

	return 0;
}

int
eval_c_api(void)
{
	tsStatus status;
	tsBSpline spline = ts_bspline_init();
	tsBSpline copy = ts_bspline_init();
	double ctrlp[6];
	double w[3];
	quarter_circle_points(ctrlp);
	quarter_circle_weights(w);

	TASK_CHECK(ts_bspline_new(3, 2, 2, TS_BEZIERS, &spline, &status)
	           == TS_SUCCESS);
	TASK_CHECK(ts_bspline_set_control_points(&spline, ctrlp, &status)
	           == TS_SUCCESS);

	/* Default weights are all 1. */
	TASK_CHECK(ts_bspline_num_weights(&spline) == 3);
	for (size_t i = 0; i < 3; ++i)
		TASK_CHECK(close_to(ts_bspline_weights_ptr(&spline)[i], 1.0));

	TASK_CHECK(ts_bspline_set_weights(&spline, w, 3, &status)
	           == TS_SUCCESS);
	TASK_CHECK(close_to(ts_bspline_weights_ptr(&spline)[1], W_MID));

	/* Euclidean eval keeps the public dimension. */
	tsDeBoorNet net = ts_deboornet_init();
	TASK_CHECK(ts_bspline_eval(&spline, 0.5, &net, &status) == TS_SUCCESS);
	TASK_CHECK(ts_deboornet_dimension(&net) == 2);
	TASK_CHECK(ts_deboornet_len_points(&net)
	           == ts_deboornet_num_points(&net) * 2);
	TASK_CHECK(close_to(ts_deboornet_result_ptr(&net)[0], std::sqrt(0.5)));
	TASK_CHECK(close_to(ts_deboornet_result_ptr(&net)[1], std::sqrt(0.5)));
	ts_deboornet_free(&net);

	/* Derivatives through the C API. */
	tsReal *values = NULL;
	TASK_CHECK(ts_bspline_eval_derivatives(&spline, 0.5, 2,
	                                       &values, &status)
	           == TS_SUCCESS);
	TASK_CHECK(values != NULL);
	TASK_CHECK(close_to(values[0], std::sqrt(0.5)));
	TASK_CHECK(close_to(values[1], std::sqrt(0.5)));
	/* d/du of the quarter circle at u=0.5 is (-a, a), where
	 * a = 1 / (0.25 + 0.5 * sqrt(2)/2 + 0.25) = 2 / (1 + sqrt(2)/2). */
	const double a = 2.0 / (1.0 + W_MID);
	TASK_CHECK(close_to(values[2], -a));
	TASK_CHECK(close_to(values[3], a));
	free(values);
	values = NULL;

	/* Invalid requests leave the output empty. */
	tsStatus fail;
	TASK_CHECK(ts_bspline_eval_derivatives(&spline, 0.5, 3,
	                                       &values, &fail)
	           == TS_NO_RESULT);
	TASK_CHECK(values == NULL);

	/* Scaling all weights by a positive constant does not change the
	 * geometry, and the weights are not normalized behind the scenes. */
	tsBSpline scaled = ts_bspline_init();
	TASK_CHECK(ts_bspline_copy(&spline, &scaled, &status) == TS_SUCCESS);
	double scaled_w[3] = {7.0 * w[0], 7.0 * w[1], 7.0 * w[2]};
	TASK_CHECK(ts_bspline_set_weights(&scaled, scaled_w, 3, &status)
	           == TS_SUCCESS);
	for (int i = 0; i <= 10; ++i) {
		const double u = i / 10.0;
		tsDeBoorNet a = ts_deboornet_init();
		tsDeBoorNet b = ts_deboornet_init();
		TASK_CHECK(ts_bspline_eval(&spline, u, &a, &status) == TS_SUCCESS);
		TASK_CHECK(ts_bspline_eval(&scaled, u, &b, &status) == TS_SUCCESS);
		TASK_CHECK(ts_distance(ts_deboornet_result_ptr(&a),
		                       ts_deboornet_result_ptr(&b), 2) < EPS);
		ts_deboornet_free(&a);
		ts_deboornet_free(&b);
	}
	TASK_CHECK(close_to(ts_bspline_weights_ptr(&scaled)[1],
	                    7.0 * W_MID));
	TASK_CHECK(close_to(ts_bspline_weights_ptr(&spline)[1], W_MID));

	/* Invalid weights: wrong count, NaN, inf, zero, negative. Every
	 * failed call must leave the object untouched. */
	{
		double bad[3] = {1.0, 1.0, 1.0};
		bad[1] = std::nan("");
		TASK_CHECK(ts_bspline_set_weights(&spline, bad, 3, &fail)
		           == TS_NO_RESULT);
		TASK_CHECK(close_to(ts_bspline_weights_ptr(&spline)[1], W_MID));

		bad[1] = std::numeric_limits<double>::infinity();
		TASK_CHECK(ts_bspline_set_weights(&spline, bad, 3, &fail)
		           == TS_NO_RESULT);
		TASK_CHECK(close_to(ts_bspline_weights_ptr(&spline)[1], W_MID));

		bad[1] = 0.0;
		TASK_CHECK(ts_bspline_set_weights(&spline, bad, 3, &fail)
		           == TS_NO_RESULT);
		TASK_CHECK(close_to(ts_bspline_weights_ptr(&spline)[1], W_MID));

		bad[1] = -2.0;
		TASK_CHECK(ts_bspline_set_weights(&spline, bad, 3, &fail)
		           == TS_NO_RESULT);
		TASK_CHECK(close_to(ts_bspline_weights_ptr(&spline)[1], W_MID));

		TASK_CHECK(ts_bspline_set_weights(&spline, bad, 2, &fail)
		           == TS_NO_RESULT);
		TASK_CHECK(close_to(ts_bspline_weights_ptr(&spline)[1], W_MID));
	}

	/* Copy keeps weights, including self-assignment and JSON. */
	TASK_CHECK(ts_bspline_copy(&spline, &copy, &status) == TS_SUCCESS);
	TASK_CHECK(close_to(ts_bspline_weights_ptr(&copy)[1], W_MID));
	TASK_CHECK(ts_bspline_copy(&copy, &copy, &status) == TS_SUCCESS);
	TASK_CHECK(close_to(ts_bspline_weights_ptr(&copy)[1], W_MID));

	ts_bspline_free(&scaled);
	ts_bspline_free(&copy);
	ts_bspline_free(&spline);
	return 0;
}

int
eval_json_and_legacy(void)
{
	/* Rational JSON round trip. */
	tinyspline::BSpline spline;
	make_quarter_circle(spline);
	const std::string json = spline.toJson();
	TASK_CHECK(json.find("\"weights\"") != std::string::npos);

	tinyspline::BSpline restored =
		tinyspline::BSpline::parseJson(json);
	TASK_CHECK(close_vec(restored.weights(), spline.weights()));

	for (int i = 0; i <= 10; ++i) {
		const double u = i / 10.0;
		TASK_CHECK(close_vec(restored.eval(u).result(),
		                     spline.eval(u).result()));
	}

	/* Legacy JSON without a weights field is read as all 1. */
	const char *legacy =
		"{"
		"\"degree\": 2,"
		"\"dimension\": 2,"
		"\"control_points\": [1.0, 0.0, 1.0, 1.0, 0.0, 1.0],"
		"\"knots\": [0.0, 0.0, 0.0, 1.0, 1.0, 1.0]"
		"}";
	tinyspline::BSpline poly = tinyspline::BSpline::parseJson(legacy);
	TASK_CHECK(poly.weights().size() == 3);
	for (size_t i = 0; i < 3; ++i)
		TASK_CHECK(close_to(poly.weights()[i], 1.0));

	/* The legacy polynomial spline is the ordinary quadratic Bezier:
	 * (1-u)^2*P0 + 2u(1-u)*P1 + u^2*P2 at u=0.25. */
	const std::vector<double> plain = poly.eval(0.25).result();
	TASK_CHECK(close_to(plain[0], 0.9375));
	TASK_CHECK(close_to(plain[1], 0.4375));

	/* C API JSON round trip of an edited rational spline. */
	tsStatus status;
	tsBSpline edited = ts_bspline_init();
	tsBSpline parsed = ts_bspline_init();
	TASK_CHECK(ts_bspline_parse_json(json.c_str(), &edited, &status)
	           == TS_SUCCESS);
	char *out = NULL;
	TASK_CHECK(ts_bspline_to_json(&edited, &out, &status) == TS_SUCCESS);
	TASK_CHECK(ts_bspline_parse_json(out, &parsed, &status) == TS_SUCCESS);
	TASK_CHECK(close_to(ts_bspline_weights_ptr(&parsed)[1], W_MID));
	free(out);
	ts_bspline_free(&edited);
	ts_bspline_free(&parsed);

	return 0;
}

/* Evaluates `spline' at `u' and returns the first result point. At a
 * discontinuity (full-multiplicity knot), DeBoorNet::result() contains two
 * points; only the first one is compared here. */
std::vector<double>
first_point(const tinyspline::BSpline &spline, double u)
{
	const std::vector<double> result = spline.eval(u).result();
	return std::vector<double>(result.begin(),
	                           result.begin()
	                           + spline.dimension());
}

/* Asserts that `a' and `b' evaluate to the same points at `num' uniformly
 * distributed knots (using only the first point in case of a
 * discontinuity). Returns false on the first mismatch. */
bool
same_shape(const tinyspline::BSpline &a,
           const tinyspline::BSpline &b,
           size_t num = 41,
           double eps = EPS)
{
	const double lo = std::max(a.domain().min(), b.domain().min());
	const double hi = std::min(a.domain().max(), b.domain().max());
	if (!(hi > lo))
		return false;
	for (size_t i = 0; i < num; ++i) {
		const double u = lo + (hi - lo) * i / (num - 1);
		if (!close_vec(first_point(a, u), first_point(b, u), eps))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* rational_edit: homogeneous-exact editing and weight preservation.   */
/* ------------------------------------------------------------------ */

int
edit_insert_knot_exact(void)
{
	tinyspline::BSpline spline;
	make_quarter_circle(spline);

	/* Repeated insertion, including the maximum multiplicity. */
	tinyspline::BSpline refined = spline.insertKnot(0.5, 1);
	TASK_CHECK(refined.numControlPoints() == 4);
	TASK_CHECK(refined.weights().size() == 4);
	for (int i = 0; i <= 20; ++i) {
		const double u = i / 20.0;
		TASK_CHECK(close_vec(first_point(refined, u),
		                     first_point(spline, u)));
	}
	/* Insertion near the ends and repeatedly at the same knot. */
	refined = refined.insertKnot(0.5, 1);
	TASK_CHECK(refined.numControlPoints() == 5);
	refined = refined.insertKnot(0.125, 1);
	refined = refined.insertKnot(0.875, 1);
	for (int i = 0; i <= 40; ++i) {
		const double u = i / 40.0;
		TASK_CHECK(close_vec(first_point(refined, u),
		                     first_point(spline, u)));
	}

	/* Every output weight is positive and usable by further edits. */
	for (size_t i = 0; i < refined.weights().size(); ++i)
		TASK_CHECK(refined.weights()[i] > 0.0);

	/* Continue editing the refined spline. */
	tinyspline::BSpline again = refined.insertKnot(0.3, 1);
	again = again.insertKnot(0.7, 1);
	for (int i = 0; i <= 40; ++i) {
		const double u = i / 40.0;
		TASK_CHECK(close_vec(first_point(again, u),
		                     first_point(spline, u)));
	}

	/* Split preserves the geometry on both sides. */
	tinyspline::BSpline split = spline.split(0.5);
	for (int i = 0; i <= 20; ++i) {
		const double u = i / 40.0;
		TASK_CHECK(close_vec(first_point(split, u),
		                     first_point(spline, u)));
	}
	for (int i = 0; i <= 20; ++i) {
		const double u = 0.5 + i / 40.0;
		TASK_CHECK(close_vec(first_point(split, u),
		                     first_point(spline, u)));
	}

	/* C API: in-place insertion must be exact too. */
	tsStatus status;
	tsBSpline c = ts_bspline_init();
	double ctrlp[6];
	double w[3];
	size_t k;
	quarter_circle_points(ctrlp);
	quarter_circle_weights(w);
	TASK_CHECK(ts_bspline_new(3, 2, 2, TS_BEZIERS, &c, &status)
	           == TS_SUCCESS);
	TASK_CHECK(ts_bspline_set_control_points(&c, ctrlp, &status)
	           == TS_SUCCESS);
	TASK_CHECK(ts_bspline_set_weights(&c, w, 3, &status) == TS_SUCCESS);
	TASK_CHECK(ts_bspline_insert_knot(&c, 0.5, 1, &c, &k, &status)
	           == TS_SUCCESS);
	for (int i = 0; i <= 20; ++i) {
		const double u = i / 20.0;
		std::vector<double> expected;
		rational_bezier(u, ctrlp, w, 2, expected);
		tsReal *values = NULL;
		TASK_CHECK(ts_bspline_eval_derivatives(&c, u, 0, &values, &status)
		           == TS_SUCCESS);
		TASK_CHECK(close_to(values[0], expected[0]));
		TASK_CHECK(close_to(values[1], expected[1]));
		free(values);
	}
	ts_bspline_free(&c);

	return 0;
}

int
edit_to_beziers_exact(void)
{
	/* Build a rational spline with an interior knot by refining the
	 * quarter circle, then decompose it. */
	tinyspline::BSpline circle;
	make_quarter_circle(circle);
	tinyspline::BSpline refined = circle.insertKnot(0.5, 1);

	tinyspline::BSpline beziers = refined.toBeziers();
	TASK_CHECK(beziers.numControlPoints() % beziers.order() == 0);
	TASK_CHECK(beziers.weights().size()
	           == beziers.numControlPoints());
	for (int i = 0; i <= 40; ++i) {
		const double u = i / 40.0;
		TASK_CHECK(close_vec(first_point(beziers, u),
		                     first_point(circle, u)));
	}

	/* The output is directly editable. */
	tinyspline::BSpline edited = beziers.insertKnot(0.25, 1);
	edited.setWeights(edited.weights());
	edited = edited.split(0.75);
	for (int i = 0; i <= 40; ++i) {
		const double u = i / 40.0;
		TASK_CHECK(close_vec(first_point(edited, u),
		                     first_point(circle, u)));
	}

	/* A non-planar rational rail with an internal knot. */
	tinyspline::BSpline rail;
	make_rail(rail);
	tinyspline::BSpline rail_refined = rail.insertKnot(0.5, 1);
	tinyspline::BSpline rail_beziers = rail_refined.toBeziers();
	for (int i = 0; i <= 40; ++i) {
		const double u = i / 40.0;
		TASK_CHECK(close_vec(first_point(rail_beziers, u),
		                     first_point(rail, u), 1e-11));
	}

	/* The weights survive JSON and copy. */
	tinyspline::BSpline parsed =
		tinyspline::BSpline::parseJson(rail_beziers.toJson());
	TASK_CHECK(close_vec(parsed.weights(), rail_beziers.weights()));
	TASK_CHECK(close_vec(parsed.controlPoints(),
	                     rail_beziers.controlPoints()));
	tinyspline::BSpline copy(parsed);
	tinyspline::BSpline moved(std::move(copy));
	TASK_CHECK(close_vec(moved.weights(), rail_beziers.weights()));
	copy = rail_beziers;
	TASK_CHECK(close_vec(copy.weights(), rail_beziers.weights()));

	/* Self-assignment (copy and move) preserves the weights. */
	tinyspline::BSpline &self = copy;
	copy = self;
	TASK_CHECK(close_vec(copy.weights(), rail_beziers.weights()));
	copy = std::move(self);
	TASK_CHECK(close_vec(copy.weights(), rail_beziers.weights()));
	TASK_CHECK(close_vec(copy.controlPoints(),
	                     rail_beziers.controlPoints()));

	/* Editing keeps the weights attached to their control points: the
	 * weights of an inserted spline are exactly the convex combinations
	 * produced by homogeneous insertion, i.e. every weight stays within
	 * the range of the original weights. */
	double lo_w = 1e300, hi_w = -1e300;
	for (size_t i = 0; i < rail_beziers.weights().size(); ++i) {
		lo_w = std::min(lo_w, rail_beziers.weights()[i]);
		hi_w = std::max(hi_w, rail_beziers.weights()[i]);
	}
	for (size_t i = 0; i < copy.weights().size(); ++i) {
		TASK_CHECK(copy.weights()[i] >= lo_w - EPS);
		TASK_CHECK(copy.weights()[i] <= hi_w + EPS);
	}

	return 0;
}

int
edit_full_multiplicity_knot(void)
{
	tinyspline::BSpline circle;
	make_quarter_circle(circle);

	/* Split inserts until multiplicity == order (degree 2 => 3). */
	tinyspline::BSpline split = circle.split(0.5);
	TASK_CHECK(split.numControlPoints() == 6);
	TASK_CHECK(split.weights().size() == 6);

	/* Both one-sided limits are on the circle and equal in value. */
	const std::vector<double> left = split.eval(0.5).result();
	TASK_CHECK(close_to(std::hypot(left[0], left[1]), 1.0));

	/* Splitting again at the full-multiplicity knot is a no-op in terms
	 * of geometry (the split operation copies the spline). */
	tinyspline::BSpline split2 = split.split(0.5);
	for (int i = 0; i <= 20; ++i) {
		const double u = i / 20.0;
		TASK_CHECK(close_vec(first_point(split2, u),
		                     first_point(split, u)));
	}

	/* At a full-multiplicity internal knot only the position (order 0)
	 * is available. */
	{
		bool threw = false;
		try {
			split.evalDerivatives(0.5, 1);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}
	TASK_CHECK(split.evalDerivatives(0.5, 0).size() == 2);

	/* Endpoints allow one-sided derivatives up to the degree. */
	TASK_CHECK(split.evalDerivatives(0.0, 2).size() == 6);
	TASK_CHECK(split.evalDerivatives(1.0, 2).size() == 6);

	return 0;
}

int
edit_unsupported_transforms(void)
{
	tinyspline::BSpline rail;
	make_rail(rail);
	const std::vector<double> before = rail.evalAll(
		std::vector<double>{0.0, 0.3, 0.6, 1.0});

	/* derive, tension, elevateDegree, and morph must refuse rational
	 * splines with non-constant weights instead of dropping them. */
	{
		bool threw = false;
		try { rail.derive(); } catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}
	{
		bool threw = false;
		try { rail.tension(0.5); } catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}
	{
		bool threw = false;
		try { rail.elevateDegree(1); } catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}
	{
		bool threw = false;
		try {
			tinyspline::Morphism morph = rail.morphTo(rail);
			morph.eval(0.5);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		TASK_CHECK(threw);
	}

	/* The spline is untouched by the failed operations. */
	TASK_CHECK(close_vec(rail.evalAll(
		std::vector<double>{0.0, 0.3, 0.6, 1.0}), before));
	TASK_CHECK(close_vec(rail.weights(),
	                     std::vector<double>{1.0, 0.6, 1.3}));

	/* Constant (non-unit) weights are fine for polynomial transforms.
	 * A polynomial spline scaled to a constant weight of 2.5 has the
	 * same geometry as the all-1 spline, so elevation must preserve it. */
	tinyspline::BSpline poly(5, 2, 3);
	poly.setControlPoints(std::vector<double>{
		0, 0,
		1, 2,
		3, 2,
		4, 0,
		5, 1});
	tinyspline::BSpline poly_refined = poly.insertKnot(0.5, 1);
	std::vector<double> w(poly_refined.weights().size(), 2.5);
	poly_refined.setWeights(w);
	tinyspline::BSpline elevated = poly_refined.elevateDegree(1);
	for (int i = 0; i <= 20; ++i) {
		const double u = i / 20.0;
		TASK_CHECK(close_vec(first_point(elevated, u),
		                     first_point(poly, u), 1e-11));
	}
	for (size_t i = 0; i < elevated.weights().size(); ++i)
		TASK_CHECK(close_to(elevated.weights()[i], 2.5));

	return 0;
}

int
edit_c_api_in_place_and_continued(void)
{
	tsStatus status;
	tsBSpline spline = ts_bspline_init();
	tsBSpline result = ts_bspline_init();
	double ctrlp[6];
	double w[3];
	size_t k;
	quarter_circle_points(ctrlp);
	quarter_circle_weights(w);
	TASK_CHECK(ts_bspline_new(3, 2, 2, TS_BEZIERS, &spline, &status)
	           == TS_SUCCESS);
	TASK_CHECK(ts_bspline_set_control_points(&spline, ctrlp, &status)
	           == TS_SUCCESS);
	TASK_CHECK(ts_bspline_set_weights(&spline, w, 3, &status)
	           == TS_SUCCESS);

	/* out-of-place split, then continue editing the split result */
	TASK_CHECK(ts_bspline_split(&spline, 0.25, &result, &k, &status)
	           == TS_SUCCESS);
	TASK_CHECK(ts_bspline_num_weights(&result)
	           == ts_bspline_num_control_points(&result));
	for (size_t i = 0; i <= 20; ++i) {
		tsDeBoorNet a = ts_deboornet_init();
		tsDeBoorNet b = ts_deboornet_init();
		tsReal u = i / 20.0;
		TASK_CHECK(ts_bspline_eval(&spline, u, &a, &status)
		           == TS_SUCCESS);
		TASK_CHECK(ts_bspline_eval(&result, u, &b, &status)
		           == TS_SUCCESS);
		TASK_CHECK(ts_distance(ts_deboornet_result_ptr(&a),
		                       ts_deboornet_result_ptr(&b), 2) < EPS);
		ts_deboornet_free(&a);
		ts_deboornet_free(&b);
	}
	TASK_CHECK(ts_bspline_insert_knot(&result, 0.75, 1, &result, &k,
	                                  &status) == TS_SUCCESS);

	/* Copy the weights, modify one control point, and check that the
	 * weights are retained by set_control_points. */
	tsReal *weights = NULL;
	TASK_CHECK(ts_bspline_weights(&result, &weights, &status)
	           == TS_SUCCESS);
	const size_t nw = ts_bspline_num_weights(&result);
	TASK_CHECK(nw == ts_bspline_num_control_points(&result));

	tsReal *points = NULL;
	TASK_CHECK(ts_bspline_control_points(&result, &points, &status)
	           == TS_SUCCESS);
	points[0] += 0.0; /* still a valid control point */
	TASK_CHECK(ts_bspline_set_control_points(&result, points, &status)
	           == TS_SUCCESS);
	for (size_t i = 0; i < nw; ++i)
		TASK_CHECK(close_to(ts_bspline_weights_ptr(&result)[i],
		                    weights[i]));
	free(points);
	free(weights);

	ts_bspline_free(&spline);
	ts_bspline_free(&result);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rational_sweep: RMF tangents, orthogonality, and sweep sections.    */
/* ------------------------------------------------------------------ */

void
vec3_dot(const double *a, const double *b, double &out)
{
	out = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

void
vec3_cross(const double *a, const double *b, double *out)
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

int
sweep_rmf_tangents(void)
{
	tinyspline::BSpline rail;
	make_rail(rail);
	const std::vector<double> ctrlp = rail.controlPoints();
	const std::vector<double> w = rail.weights();

	/* Strictly increasing knots spanning the domain. */
	const size_t num = 41;
	std::vector<double> knots(num);
	for (size_t i = 0; i < num; ++i)
		knots[i] = i / (double) (num - 1);

	tinyspline::FrameSeq frames = rail.computeRMF(knots);
	TASK_CHECK(frames.size() == num);

	for (size_t i = 0; i < num; ++i) {
		const double u = knots[i];

		/* Position equals rational evaluation. */
		const std::vector<double> pos = rail.eval(u).result();
		TASK_CHECK(close_to(frames.at(i).position().x(), pos[0]));
		TASK_CHECK(close_to(frames.at(i).position().y(), pos[1]));
		TASK_CHECK(close_to(frames.at(i).position().z(), pos[2]));

		/* Tangent equals the normalized analytic rational derivative. */
		std::vector<double> d;
		rational_bezier_derivatives(u, ctrlp.data(), w.data(), 3, d);
		double len = std::sqrt(d[3]*d[3] + d[4]*d[4] + d[5]*d[5]);
		const double expected[3] = {d[3]/len, d[4]/len, d[5]/len};
		const double tangent[3] = {frames.at(i).tangent().x(),
		                           frames.at(i).tangent().y(),
		                           frames.at(i).tangent().z()};
		TASK_CHECK(close_to(tangent[0], expected[0], 1e-12));
		TASK_CHECK(close_to(tangent[1], expected[1], 1e-12));
		TASK_CHECK(close_to(tangent[2], expected[2], 1e-12));

		/* Orthonormal frame. */
		const double normal[3] = {frames.at(i).normal().x(),
		                          frames.at(i).normal().y(),
		                          frames.at(i).normal().z()};
		const double binormal[3] = {frames.at(i).binormal().x(),
		                            frames.at(i).binormal().y(),
		                            frames.at(i).binormal().z()};
		double dot;
		vec3_dot(tangent, normal, dot);
		TASK_CHECK(close_to(dot, 0.0, 1e-12));
		vec3_dot(tangent, binormal, dot);
		TASK_CHECK(close_to(dot, 0.0, 1e-12));
		vec3_dot(normal, binormal, dot);
		TASK_CHECK(close_to(dot, 0.0, 1e-12));
		vec3_dot(tangent, tangent, dot);
		TASK_CHECK(close_to(dot, 1.0, 1e-12));
		vec3_dot(normal, normal, dot);
		TASK_CHECK(close_to(dot, 1.0, 1e-12));
		vec3_dot(binormal, binormal, dot);
		TASK_CHECK(close_to(dot, 1.0, 1e-12));

		/* Right-handed frame: tangent x normal == binormal. */
		double cross[3];
		vec3_cross(tangent, normal, cross);
		TASK_CHECK(close_to(cross[0], binormal[0], 1e-12));
		TASK_CHECK(close_to(cross[1], binormal[1], 1e-12));
		TASK_CHECK(close_to(cross[2], binormal[2], 1e-12));
	}

	return 0;
}

int
sweep_cross_sections(void)
{
	tinyspline::BSpline rail;
	make_rail(rail);

	const size_t num = 33;
	std::vector<double> knots(num);
	for (size_t i = 0; i < num; ++i)
		knots[i] = i / (double) (num - 1);
	tinyspline::FrameSeq frames = rail.computeRMF(knots);

	const double radius = 0.35;
	const size_t segments = 12;
	for (size_t i = 0; i < num; ++i) {
		const tinyspline::Vec3 position = frames.at(i).position();
		const tinyspline::Vec3 tangent = frames.at(i).tangent();
		const tinyspline::Vec3 normal = frames.at(i).normal();
		const tinyspline::Vec3 binormal = frames.at(i).binormal();

		for (size_t j = 0; j < segments; ++j) {
			const double theta = 2.0 * M_PI * j / segments;
			const tinyspline::Vec3 section =
				position.add(normal.multiply(radius * std::cos(theta)))
				.add(binormal.multiply(radius * std::sin(theta)));

			/* The section point lies in the plane orthogonal to the
			 * tangent and at the sweep radius from the rail. */
			const tinyspline::Vec3 offset = section.subtract(position);
			TASK_CHECK(close_to(offset.dot(tangent), 0.0, 1e-12));
			TASK_CHECK(close_to(offset.magnitude(), radius, 1e-12));

			/* It is also at distance `radius' from the evaluated
			 * rail position. */
			const std::vector<double> rail_pos =
				rail.eval(knots[i]).result();
			const double dist = std::sqrt(
				(section.x() - rail_pos[0]) * (section.x() - rail_pos[0])
				+ (section.y() - rail_pos[1]) * (section.y() - rail_pos[1])
				+ (section.z() - rail_pos[2]) * (section.z() - rail_pos[2]));
			TASK_CHECK(close_to(dist, radius, 1e-12));
		}

		/* The non-planar rail must actually twist: the frames of two
		 * different knots are not the same. */
		if (i > 0) {
			const tinyspline::Vec3 prev = frames.at(i - 1).normal();
			TASK_CHECK(prev.distance(normal) > 0.0);
		}
	}

	/* A rotated section (same frame, different theta) maps consistently:
	 * use a single section and verify that its image under the frame
	 * basis matches the rail position plus the offset. */
	const tinyspline::Vec3 origin = frames.at(10).position();
	const tinyspline::Vec3 n = frames.at(10).normal();
	const tinyspline::Vec3 b = frames.at(10).binormal();
	const tinyspline::Vec3 sweep_point = origin.add(n.multiply(radius));
	TASK_CHECK(close_to(
		sweep_point.subtract(origin).magnitude(), radius, 1e-12));
	TASK_CHECK(close_to(
		sweep_point.subtract(origin).dot(frames.at(10).tangent()),
		0.0, 1e-12));

	return 0;
}

int
sweep_rmf_validation(void)
{
	tinyspline::BSpline rail;
	make_rail(rail);

	/* Non-increasing knots are rejected. */
	{
		std::vector<double> knots{0.0, 0.5, 0.5, 0.8};
		bool threw = false;
		try { rail.computeRMF(knots); }
		catch (const std::runtime_error &) { threw = true; }
		TASK_CHECK(threw);
	}
	{
		std::vector<double> knots{0.2, 0.1};
		bool threw = false;
		try { rail.computeRMF(knots); }
		catch (const std::runtime_error &) { threw = true; }
		TASK_CHECK(threw);
	}

	/* A zero tangent is rejected. */
	{
		tinyspline::BSpline point(3, 3, 2, tinyspline::BSpline::Beziers);
		point.setControlPoints(std::vector<double>{
			1, 1, 1,
			1, 1, 1,
			1, 1, 1});
		point.setWeights(std::vector<double>{1.0, 0.5, 2.0});
		std::vector<double> knots{0.0, 0.5, 1.0};
		bool threw = false;
		try { point.computeRMF(knots); }
		catch (const std::runtime_error &) { threw = true; }
		TASK_CHECK(threw);
	}

	/* All-1 weights behave as before (ordinary polynomial spline). */
	{
		tinyspline::BSpline poly(5, 3, 3);
		poly.setControlPoints(std::vector<double>{
			100, 200, 0,
			150, 220, 10,
			190, 120, 50,
			260, 70, 30,
			300, 200, 20});
		std::vector<double> knots = poly.uniformKnotSeq(25);
		tinyspline::FrameSeq frames = poly.computeRMF(knots);
		TASK_CHECK(frames.size() == 25);
		for (size_t i = 0; i < knots.size(); ++i) {
			const std::vector<double> d =
				poly.evalDerivatives(knots[i], 1);
			const double len = std::sqrt(d[3]*d[3]
			                             + d[4]*d[4]
			                             + d[5]*d[5]);
			TASK_CHECK(close_to(frames.at(i).tangent().x(), d[3]/len,
			                    1e-12));
			TASK_CHECK(close_to(frames.at(i).tangent().y(), d[4]/len,
			                    1e-12));
			TASK_CHECK(close_to(frames.at(i).tangent().z(), d[5]/len,
			                    1e-12));
			double dot;
			const double t[3] = {frames.at(i).tangent().x(),
			                     frames.at(i).tangent().y(),
			                     frames.at(i).tangent().z()};
			const double n[3] = {frames.at(i).normal().x(),
			                     frames.at(i).normal().y(),
			                     frames.at(i).normal().z()};
			vec3_dot(t, n, dot);
			TASK_CHECK(close_to(dot, 0.0, 1e-12));
		}
	}

	return 0;
}

int
sweep_c_api(void)
{
	tsStatus status;
	tsBSpline spline = ts_bspline_init();
	double ctrlp[9];
	double w[3];
	rail_points(ctrlp);
	rail_weights(w);
	TASK_CHECK(ts_bspline_new(3, 3, 2, TS_BEZIERS, &spline, &status)
	           == TS_SUCCESS);
	TASK_CHECK(ts_bspline_set_control_points(&spline, ctrlp, &status)
	           == TS_SUCCESS);
	TASK_CHECK(ts_bspline_set_weights(&spline, w, 3, &status)
	           == TS_SUCCESS);

	const size_t num = 17;
	tsReal knots[17];
	tsFrame frames[17];
	for (size_t i = 0; i < num; ++i)
		knots[i] = i / (tsReal) (num - 1);
	TASK_CHECK(ts_bspline_compute_rmf(&spline, knots, num, 0,
	                                  frames, &status)
	           == TS_SUCCESS);
	for (size_t i = 0; i < num; ++i) {
		tsReal *values = NULL;
		TASK_CHECK(ts_bspline_eval_derivatives(&spline, knots[i], 1,
		                                       &values, &status)
		           == TS_SUCCESS);
		tsReal tangent[3] = {values[3], values[4], values[5]};
		free(values);
		ts_vec_norm(tangent, 3, tangent);
		TASK_CHECK(ts_distance(tangent, frames[i].tangent, 3) < 1e-12);
		TASK_CHECK(std::isfinite(frames[i].position[0]));
		TASK_CHECK(std::isfinite(frames[i].normal[0]));
		TASK_CHECK(std::isfinite(frames[i].binormal[0]));
	}

	/* Non-strictly-increasing knots fail. */
	{
		tsReal bad[2] = {0.5, 0.5};
		tsFrame f[2];
		TASK_CHECK(ts_bspline_compute_rmf(&spline, bad, 2, 0, f,
		                                  &status) != TS_SUCCESS);
	}

	ts_bspline_free(&spline);
	return 0;
}

} /* namespace */

const struct task_case task_cases[] = {
	{"rational_eval", "quarter_circle_coordinates",
	 eval_quarter_circle_coordinates},
	{"rational_eval", "quarter_circle_derivatives",
	 eval_quarter_circle_derivatives},
	{"rational_eval", "c_api", eval_c_api},
	{"rational_eval", "json_and_legacy", eval_json_and_legacy},

	{"rational_edit", "insert_knot_exact", edit_insert_knot_exact},
	{"rational_edit", "to_beziers_exact", edit_to_beziers_exact},
	{"rational_edit", "full_multiplicity_knot",
	 edit_full_multiplicity_knot},
	{"rational_edit", "unsupported_transforms",
	 edit_unsupported_transforms},
	{"rational_edit", "c_api_in_place", edit_c_api_in_place_and_continued},

	{"rational_sweep", "rmf_tangents", sweep_rmf_tangents},
	{"rational_sweep", "cross_sections", sweep_cross_sections},
	{"rational_sweep", "rmf_validation", sweep_rmf_validation},
	{"rational_sweep", "c_api", sweep_c_api},
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
