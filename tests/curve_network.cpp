// Bidirectional curve network tests.
//
// The reference implementations below (monotone Hermite rule and natural
// cubic spline equations) are written independently from the production
// classes: they only consume declared stations, parameters and evaluated
// network points.  Production surfaces are never validated against their own
// iso-curve interfaces alone.

#include "task_support/check.h"

#include <nanospline/CurveNetwork.h>
#include <nanospline/ReparameterizedBSpline.h>

#include <cmath>
#include <functional>
#include <limits>
#include <vector>

using nanospline::BSpline;
using nanospline::BSplinePatch;
using nanospline::Bezier;
using nanospline::CurveNetwork;
using nanospline::PiecewiseCubicReparam;
using nanospline::ReparameterizedBSpline;
using nanospline::internal::natural_cubic_interpolation;

using Curve = BSpline<double, 3, -1>;
using Vec = Eigen::Matrix<double, Eigen::Dynamic, 1>;
using Points = Eigen::Matrix<double, Eigen::Dynamic, 3>;
using Row3 = Eigen::RowVector3d;

namespace {

constexpr double TIGHT = 1e-10;
constexpr double SURFACE_TOL = 1e-8;
constexpr double DERIV_TOL = 1e-7;

// ---------------------------------------------------------------------------
// Independent reference: fixed-rule monotone piecewise cubic Hermite map.
// ---------------------------------------------------------------------------
struct RefHermite
{
    Vec s, t, m;
    bool reversed = false;

    static RefHermite build(const Vec& stations, const Vec& source_params)
    {
        RefHermite r;
        r.s = stations;
        r.t = source_params;
        r.reversed = r.t[1] < r.t[0];
        if (r.reversed) r.t = -r.t;
        const int n = r.s.size();
        std::vector<double> delta(n - 1);
        for (int k = 0; k < n - 1; ++k) {
            delta[k] = (r.t[k + 1] - r.t[k]) / (r.s[k + 1] - r.s[k]);
        }
        r.m.resize(n);
        r.m[0] = delta[0];
        r.m[n - 1] = delta[n - 2];
        for (int k = 1; k < n - 1; ++k) {
            r.m[k] = 2.0 / (1.0 / delta[k - 1] + 1.0 / delta[k]);
        }
        for (int k = 0; k < n - 1; ++k) {
            const double alpha = r.m[k] / delta[k];
            const double beta = r.m[k + 1] / delta[k];
            const double norm2 = alpha * alpha + beta * beta;
            if (norm2 > 9.0) {
                const double f = 3.0 / std::sqrt(norm2);
                r.m[k] *= f;
                r.m[k + 1] *= f;
            }
        }
        return r;
    }

    int segment(double x) const
    {
        const int n = s.size();
        if (x <= s[0]) return 0;
        if (x >= s[n - 1]) return n - 2;
        int k = 0;
        while (x > s[k + 1]) ++k;
        return k;
    }

    double eval(double x) const
    {
        const int k = segment(x);
        const double h = s[k + 1] - s[k];
        const double z = std::max(0.0, std::min(1.0, (x - s[k]) / h));
        const double h00 = 1 - 3 * z * z + 2 * z * z * z;
        const double h10 = z - 2 * z * z + z * z * z;
        const double h01 = 3 * z * z - 2 * z * z * z;
        const double h11 = -z * z + z * z * z;
        const double v = t[k] * h00 + h * m[k] * h10 + t[k + 1] * h01 + h * m[k + 1] * h11;
        return reversed ? -v : v;
    }

    double deriv(double x) const
    {
        const int k = segment(x);
        const double h = s[k + 1] - s[k];
        const double z = std::max(0.0, std::min(1.0, (x - s[k]) / h));
        const double d00 = (-6 * z + 6 * z * z) / h;
        const double d10 = (1 - 4 * z + 3 * z * z) / h;
        const double d01 = (6 * z - 6 * z * z) / h;
        const double d11 = (-2 * z + 3 * z * z) / h;
        const double v = t[k] * d00 + h * m[k] * d10 + t[k + 1] * d01 + h * m[k + 1] * d11;
        return reversed ? -v : v;
    }
};

// ---------------------------------------------------------------------------
// Independent reference: natural cubic spline through (stations, values),
// solved as a full tridiagonal linear system and evaluated segment-wise.
// ---------------------------------------------------------------------------
struct RefNatural
{
    Vec x;
    Points y, M;

    static RefNatural build(const Vec& stations, const Points& values)
    {
        RefNatural r;
        r.x = stations;
        r.y = values;
        const int n = stations.size();
        r.M.resize(n, 3);
        r.M.setZero();
        if (n == 2) return r;
        std::vector<double> h(n - 1);
        for (int k = 0; k < n - 1; ++k) h[k] = stations[k + 1] - stations[k];
        const int q = n - 2;
        Eigen::MatrixXd A(q, q);
        Points rhs(q, 3);
        A.setZero();
        for (int k = 0; k < q; ++k) {
            const int idx = k + 1;
            if (k > 0) A(k, k - 1) = h[idx - 1];
            A(k, k) = 2 * (h[idx - 1] + h[idx]);
            if (k + 1 < q) A(k, k + 1) = h[idx];
            rhs.row(k) = 6.0 * ((values.row(idx + 1) - values.row(idx)) / h[idx] -
                                (values.row(idx) - values.row(idx - 1)) / h[idx - 1]);
        }
        r.M.block(1, 0, q, 3) = A.fullPivLu().solve(rhs);
        return r;
    }

    int locate(double u) const
    {
        int k = 0;
        while (k < x.size() - 2 && u > x[k + 1]) ++k;
        return k;
    }

    Row3 eval(double u) const
    {
        if (x.size() == 2) {
            const double z = (u - x[0]) / (x[1] - x[0]);
            return ((1 - z) * y.row(0) + z * y.row(1)).eval();
        }
        const int k = locate(u);
        const double h = x[k + 1] - x[k];
        const double a = (x[k + 1] - u) / h, b = (u - x[k]) / h;
        return (a * y.row(k) + b * y.row(k + 1) +
                ((a * a * a - a) * M.row(k) + (b * b * b - b) * M.row(k + 1)) * h * h / 6.0)
            .eval();
    }

    Row3 derivative(double u) const
    {
        if (x.size() == 2) {
            return ((y.row(1) - y.row(0)) / (x[1] - x[0])).eval();
        }
        const int k = locate(u);
        const double h = x[k + 1] - x[k];
        const double a = (x[k + 1] - u) / h, b = (u - x[k]) / h;
        return ((y.row(k + 1) - y.row(k)) / h -
                (3 * a * a - 1) * h * M.row(k) / 6.0 +
                (3 * b * b - 1) * h * M.row(k + 1) / 6.0)
            .eval();
    }
};

// Deterministic pseudo-random points.
Row3 jittered(uint32_t& seed, double scale)
{
    auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed % 2001 - 1000) / 1000.0;
    };
    return Row3(next() * scale, next() * scale, next() * scale);
}

Curve make_clamped_curve(const Points& cp, double a, double b,
    const std::vector<double>& internal, int degree)
{
    Vec knots(degree + 1 + static_cast<int>(internal.size()) + 1 + degree);
    for (int i = 0; i <= degree; ++i) knots[i] = a;
    for (size_t k = 0; k < internal.size(); ++k) knots[degree + 1 + k] = internal[k];
    for (int i = 0; i <= degree; ++i) knots[knots.size() - 1 - i] = b;
    Curve c;
    c.set_control_points(cp);
    c.set_knots(knots);
    return c;
}

struct NetworkData
{
    std::vector<Curve> transverse;
    std::vector<Curve> longitudinal;
    Vec V, U;
    std::vector<std::vector<double>> a, b;
};

// Build a compatible non-planar network with mixed degrees, internal knots,
// non-unit domains and (optionally) reversed parameter rows.
NetworkData build_network(int m, int n, uint32_t seed, bool reverse_rows,
    bool knot_at_station = false)
{
    NetworkData d;
    d.V.resize(m);
    d.U.resize(n);
    d.V[0] = 0.0;
    for (int j = 1; j < m; ++j)
        d.V[j] = d.V[j - 1] + (j == m - 1 ? 1.0 - d.V[j - 1] : 0.13 + 0.05 * (j % 2));
    d.U[0] = 0.0;
    for (int i = 1; i < n; ++i)
        d.U[i] = d.U[i - 1] + (i == n - 1 ? 1.0 - d.U[i - 1] : 0.11 + 0.06 * (i % 3));

    d.transverse.resize(m);
    d.a.assign(m, std::vector<double>(n));
    for (int j = 0; j < m; ++j) {
        const int degree = 1 + (j % 3); // 1, 2, 3 cycling
        const int num_internal = degree >= 3 ? 1 : 0;
        const int num_ctrl = degree + 1 + num_internal;
        Points cp(num_ctrl, 3);
        for (int q = 0; q < num_ctrl; ++q) {
            cp(q, 0) = q * 0.27 + j * 0.05;
            cp(q, 1) = d.V[j] + 0.12 * std::sin(q + j);
            cp(q, 2) = 0.4 * std::cos(0.8 * q + 0.5 * j) + jittered(seed, 0.08)[2];
        }
        const double dom_a = -1.2 + 0.25 * j;
        const double dom_b = 2.3 + 0.2 * j;
        for (int i = 0; i < n; ++i) {
            const double s = d.U[i];
            d.a[j][i] = dom_a + (dom_b - dom_a) * (0.2 * s + 0.8 * s * s);
        }
        d.a[j].front() = dom_a;
        d.a[j].back() = dom_b;
        if (reverse_rows && j % 2 == 1) {
            std::reverse(d.a[j].begin(), d.a[j].end());
        }
        std::vector<double> internal;
        if (num_internal) {
            // Place the source knot either between station images or exactly
            // at the middle station's declared source parameter.
            const double knot = knot_at_station
                                    ? d.a[j][n / 2]
                                    : dom_a + 0.37 * (dom_b - dom_a);
            internal.push_back(knot);
        }
        d.transverse[j] = make_clamped_curve(cp, dom_a, dom_b, internal, degree);
    }

    d.longitudinal.resize(n);
    d.b.assign(n, std::vector<double>(m));
    for (int i = 0; i < n; ++i) {
        Points grid(m, 3);
        Vec stations(m);
        for (int j = 0; j < m; ++j) {
            grid.row(j) = d.transverse[j].evaluate(d.a[j][i]);
            const double w = d.V[j];
            stations[j] = -0.6 + 2.4 * (0.25 * w + 0.75 * w * w);
        }
        Points build_grid = grid;
        if (reverse_rows && i % 2 == 0) {
            build_grid = grid.colwise().reverse().eval();
        }
        Vec knots;
        int degree;
        Points ctrl = natural_cubic_interpolation(stations, build_grid, knots, degree);
        Curve c;
        c.set_control_points(ctrl);
        c.set_knots(knots);
        d.longitudinal[i] = c;
        for (int j = 0; j < m; ++j) d.b[i][j] = stations[j];
        if (reverse_rows && i % 2 == 0) {
            std::reverse(d.b[i].begin(), d.b[i].end());
        }
    }
    return d;
}

double max_network_error(const CurveNetwork& net, const NetworkData& d)
{
    double point_err = 0.0;
    for (int j = 0; j < static_cast<int>(d.transverse.size()); ++j) {
        const auto& phi = net.get_transverse_reparam(j);
        for (int q = 0; q <= 120; ++q) {
            const double u = q / 120.0;
            const double t = phi.evaluate(u);
            point_err = std::max(point_err,
                (net.get_patch().evaluate(u, d.V[j]) - d.transverse[j].evaluate(t)).norm());
        }
    }
    for (int i = 0; i < static_cast<int>(d.longitudinal.size()); ++i) {
        const auto& psi = net.get_longitudinal_reparam(i);
        for (int q = 0; q <= 120; ++q) {
            const double v = q / 120.0;
            const double s = psi.evaluate(v);
            point_err = std::max(point_err,
                (net.get_patch().evaluate(d.U[i], v) - d.longitudinal[i].evaluate(s)).norm());
        }
    }
    return point_err;
}

double max_derivative_error(const CurveNetwork& net, const NetworkData& d)
{
    double deriv_err = 0.0;
    for (int j = 0; j < static_cast<int>(d.transverse.size()); ++j) {
        const auto& phi = net.get_transverse_reparam(j);
        for (int q = 0; q <= 120; ++q) {
            const double u = q / 120.0;
            const double t = phi.evaluate(u);
            const Row3 chain = d.transverse[j].evaluate_derivative(t) * phi.derivative(u);
            deriv_err = std::max(
                deriv_err, (net.get_patch().evaluate_derivative_u(u, d.V[j]) - chain).norm());
        }
    }
    for (int i = 0; i < static_cast<int>(d.longitudinal.size()); ++i) {
        const auto& psi = net.get_longitudinal_reparam(i);
        for (int q = 0; q <= 120; ++q) {
            const double v = q / 120.0;
            const double s = psi.evaluate(v);
            const Row3 chain = d.longitudinal[i].evaluate_derivative(s) * psi.derivative(v);
            deriv_err = std::max(
                deriv_err, (net.get_patch().evaluate_derivative_v(d.U[i], v) - chain).norm());
        }
    }
    return deriv_err;
}

bool throws_invalid(const std::function<void()>& fn)
{
    try {
        fn();
    } catch (const nanospline::invalid_setting_error&) {
        return true;
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

} // namespace

// ===========================================================================
// synchronization group
// ===========================================================================

static int sync_reparam_rule()
{
    Vec stations(5);
    stations << 0.0, 0.13, 0.46, 0.72, 1.0;
    Vec params(5);
    params << -2.0, -1.1, 0.4, 0.9, 3.0;
    const RefHermite ref = RefHermite::build(stations, params);
    const PiecewiseCubicReparam prod = nanospline::make_reparameterization(stations, params);

    TASK_CHECK(prod.get_stations().size() == 5);
    for (int k = 0; k < 5; ++k) {
        TASK_CHECK(std::abs(prod.evaluate(stations[k]) - params[k]) < TIGHT);
        TASK_CHECK(std::abs(prod.get_parameters()[k] - params[k]) < TIGHT);
    }
    // Fixed slope rule: harmonic means at interior stations, corrected so the
    // monotonicity criterion alpha^2 + beta^2 <= 9 holds on every segment.
    for (int k = 0; k < 4; ++k) {
        const double delta = (params[k + 1] - params[k]) / (stations[k + 1] - stations[k]);
        const double alpha = prod.get_slopes()[k] / delta;
        const double beta = prod.get_slopes()[k + 1] / delta;
        TASK_CHECK(alpha * alpha + beta * beta <= 9.0 + 1e-12);
    }
    TASK_CHECK(std::abs(prod.get_slopes()[0] -
                            (params[1] - params[0]) / (stations[1] - stations[0])) < TIGHT);
    for (int k = 1; k < 4; ++k) {
        const double dl = (params[k] - params[k - 1]) / (stations[k] - stations[k - 1]);
        const double dr = (params[k + 1] - params[k]) / (stations[k + 1] - stations[k]);
        const double harmonic = 2.0 / (1.0 / dl + 1.0 / dr);
        // Interior slope equals the harmonic mean unless a segment correction
        // touching this station reduced it.
        TASK_CHECK(prod.get_slopes()[k] <= harmonic + TIGHT);
    }
    for (int q = 0; q <= 200; ++q) {
        const double u = q / 200.0;
        TASK_CHECK(std::abs(prod.evaluate(u) - ref.eval(u)) < 1e-13);
        TASK_CHECK(std::abs(prod.derivative(u) - ref.deriv(u)) < 1e-12);
        TASK_CHECK(prod.derivative(u) > 0.0);
    }
    return 0;
}

static int sync_reparam_reversed()
{
    Vec stations(4);
    stations << 0.0, 0.2, 0.6, 1.0;
    Vec params(4);
    params << 5.0, 3.2, -0.4, -4.0; // descending
    const RefHermite ref = RefHermite::build(stations, params);
    const PiecewiseCubicReparam prod = nanospline::make_reparameterization(stations, params);
    TASK_CHECK(prod.is_reversed());
    TASK_CHECK(std::abs(prod.evaluate(0.0) - 5.0) < TIGHT);
    TASK_CHECK(std::abs(prod.evaluate(1.0) + 4.0) < TIGHT);
    for (int q = 0; q <= 100; ++q) {
        const double u = q / 100.0;
        TASK_CHECK(std::abs(prod.evaluate(u) - ref.eval(u)) < 1e-13);
        TASK_CHECK(std::abs(prod.derivative(u) - ref.deriv(u)) < 1e-12);
        TASK_CHECK(prod.derivative(u) < 0.0);
    }
    return 0;
}

static int sync_composition_across_knots()
{
    // Cubic source curve with one internal knot; check composition on both
    // sides of the knot's inverse image, never using surface iso interfaces.
    Points cp(5, 3);
    cp << 0, 0, 0, 0.6, 0.8, 0.3, 1.1, 1.4, -0.5, 1.7, 0.9, 0.2, 2.3, 0.2, 0.7;
    const double dom_a = -0.8, dom_b = 1.7, knot = 0.25;
    const Curve source = make_clamped_curve(cp, dom_a, dom_b, {knot}, 3);

    Vec stations(3);
    stations << 0.0, 0.4, 1.0;
    Vec params(3);
    params << dom_a, dom_a + 0.7 * (dom_b - dom_a), dom_b;
    const PiecewiseCubicReparam phi = nanospline::make_reparameterization(stations, params);
    const ReparameterizedBSpline composed(source, phi);

    // A native dynamic BSpline owns its control net, no closures.
    TASK_CHECK(composed.get_curve().get_num_control_points() > 4);
    TASK_CHECK(composed.get_curve().get_degree() == 9);

    bool saw_knot_image = false;
    for (double x : composed.get_breakpoints()) {
        if (std::abs(phi.evaluate(x) - knot) < 1e-9 && x > 0.0 && x < 1.0) {
            saw_knot_image = true;
            // Values immediately on either side must both match the source.
            for (double eps : {1e-6, 1e-4}) {
                for (double u : {x - eps, x + eps}) {
                    const Row3 expect = source.evaluate(phi.evaluate(u));
                    TASK_CHECK((composed.get_curve().evaluate(u) - expect).norm() < 1e-11);
                    const Row3 chain = source.evaluate_derivative(phi.evaluate(u)) *
                                       phi.derivative(u);
                    TASK_CHECK(
                        (composed.get_curve().evaluate_derivative(u) - chain).norm() < 1e-10);
                }
            }
        }
    }
    TASK_CHECK(saw_knot_image);

    // Whole-curve checks including endpoints and station samples.
    for (int q = 0; q <= 300; ++q) {
        const double u = q / 300.0;
        TASK_CHECK(
            (composed.get_curve().evaluate(u) - source.evaluate(phi.evaluate(u))).norm() < 1e-11);
        const Row3 chain = source.evaluate_derivative(phi.evaluate(u)) * phi.derivative(u);
        TASK_CHECK((composed.get_curve().evaluate_derivative(u) - chain).norm() < 1e-10);
    }
    return 0;
}

static int sync_natural_equations()
{
    Vec stations(4);
    stations << 0.0, 0.2, 0.55, 1.0;
    Points values(4, 3);
    values << 0.1, 0.0, 0.2, 0.4, 0.9, -0.3, -0.2, 1.5, 0.4, 0.8, 2.1, 0.0;
    const RefNatural ref = RefNatural::build(stations, values);
    Vec knots;
    int degree;
    const Points ctrl = natural_cubic_interpolation(stations, values, knots, degree);
    Curve spline;
    spline.set_control_points(ctrl);
    spline.set_knots(knots);
    TASK_CHECK(degree == 3);
    for (int k = 0; k < 4; ++k) {
        TASK_CHECK((spline.evaluate(stations[k]) - values.row(k)).norm() < 1e-12);
    }
    for (int q = 0; q <= 100; ++q) {
        const double u = q / 100.0;
        TASK_CHECK((spline.evaluate(u) - ref.eval(u)).norm() < 1e-11);
        TASK_CHECK((spline.evaluate_derivative(u) - ref.derivative(u)).norm() < 1e-10);
    }
    // Natural boundary: zero second derivative at both ends.
    TASK_CHECK(spline.evaluate_2nd_derivative(0.0).norm() < 1e-11);
    TASK_CHECK(spline.evaluate_2nd_derivative(1.0).norm() < 1e-11);

    // Two stations degenerate to linear.
    Vec two(2);
    two << 0.0, 1.0;
    Points lv(2, 3);
    lv << 1, 2, 3, 4, -2, 0;
    Vec k2;
    int d2;
    const Points c2 = natural_cubic_interpolation(two, lv, k2, d2);
    TASK_CHECK(d2 == 1 && c2.rows() == 2);
    Curve linear;
    linear.set_control_points(c2);
    linear.set_knots(k2);
    for (int q = 0; q <= 20; ++q) {
        const double u = q / 20.0;
        const Row3 expect = (1 - u) * lv.row(0) + u * lv.row(1);
        TASK_CHECK((linear.evaluate(u) - expect).norm() < 1e-14);
    }
    return 0;
}

static int sync_gordon_luv_subtraction()
{
    const NetworkData d = build_network(3, 3, 1234u, false);
    const CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);

    // Independently evaluate LU, LV, LUV at an interior, non-station point.
    const double ustar = 0.31, vstar = 0.67;
    Points transverse_values(3, 3), longitudinal_values(3, 3);
    for (int j = 0; j < 3; ++j) {
        const double t = net.get_transverse_reparam(j).evaluate(ustar);
        transverse_values.row(j) = d.transverse[j].evaluate(t);
    }
    for (int i = 0; i < 3; ++i) {
        const double s = net.get_longitudinal_reparam(i).evaluate(vstar);
        longitudinal_values.row(i) = d.longitudinal[i].evaluate(s);
    }
    const RefNatural lu_ref = RefNatural::build(d.V, transverse_values);
    const RefNatural lv_ref = RefNatural::build(d.U, longitudinal_values);

    // LUV: u-interpolation per V station, then v-interpolation.
    Points u_interp_controls(3, 3);
    for (int j = 0; j < 3; ++j) {
        Points column(3, 3);
        for (int i = 0; i < 3; ++i) {
            const Row3 pc = d.transverse[j].evaluate(d.a[j][i]);
            const Row3 pd = d.longitudinal[i].evaluate(d.b[i][j]);
            column.row(i) = 0.5 * (pc + pd);
        }
        const RefNatural col = RefNatural::build(d.U, column);
        u_interp_controls.row(j) = col.eval(ustar);
    }
    const RefNatural luv_ref = RefNatural::build(d.V, u_interp_controls);

    const Row3 gordon_ref =
        (lu_ref.eval(vstar) + lv_ref.eval(ustar) - luv_ref.eval(vstar)).eval();
    TASK_CHECK((net.get_patch().evaluate(ustar, vstar) - gordon_ref).norm() < 1e-9);

    // The LUV term is genuinely active at this non-intersection point:
    // LU + LV alone disagrees with both the full Gordon result and the patch.
    const Row3 without_luv = (lu_ref.eval(vstar) + lv_ref.eval(ustar)).eval();
    TASK_CHECK((without_luv - gordon_ref).norm() > 1e-3);
    TASK_CHECK((without_luv - net.get_patch().evaluate(ustar, vstar)).norm() > 1e-3);
    TASK_CHECK((luv_ref.eval(vstar)).norm() > 0.0);

    // At an interior intersection LU and LV double-count the point: without
    // subtracting LUV the answer is wrong, with the subtraction it is exact.
    const int i = 1, j = 1;
    const Row3 intersection = d.transverse[j].evaluate(d.a[j][i]);
    Points tv(3, 3), lv2(3, 3);
    for (int jj = 0; jj < 3; ++jj)
        tv.row(jj) = d.transverse[jj].evaluate(d.a[jj][i]);
    for (int ii = 0; ii < 3; ++ii)
        lv2.row(ii) = d.longitudinal[ii].evaluate(d.b[ii][j]);
    const Row3 lu_at = RefNatural::build(d.V, tv).eval(d.V[j]);
    const Row3 lv_at = RefNatural::build(d.U, lv2).eval(d.U[i]);
    TASK_CHECK((lu_at + lv_at - intersection).norm() > 1e-4);
    TASK_CHECK((net.get_patch().evaluate(d.U[i], d.V[j]) - intersection).norm() < SURFACE_TOL);
    return 0;
}

// ===========================================================================
// surface group
// ===========================================================================

static int surface_nonplanar_fidelity()
{
    const NetworkData d = build_network(4, 3, 99u, false);
    const CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    TASK_CHECK(max_network_error(net, d) < SURFACE_TOL);
    TASK_CHECK(max_derivative_error(net, d) < DERIV_TOL);
    bool nonplanar = false;
    for (int q = 0; q <= 10 && !nonplanar; ++q)
        for (int r = 0; r <= 10; ++r)
            if (std::abs(net.get_patch().evaluate(q / 10.0, r / 10.0)[2]) > 0.05) {
                nonplanar = true;
                break;
            }
    TASK_CHECK(nonplanar);
    for (int q = 0; q <= 15; ++q)
        for (int r = 0; r <= 15; ++r) {
            const Row3 p = net.get_patch().evaluate(q / 15.0, r / 15.0);
            TASK_CHECK(p.allFinite());
        }
    return 0;
}

static int surface_reversed_nonunit()
{
    const NetworkData d = build_network(3, 4, 7u, true);
    const CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    TASK_CHECK(net.get_transverse_reparam(1).is_reversed());
    TASK_CHECK(net.get_longitudinal_reparam(0).is_reversed());
    TASK_CHECK(max_network_error(net, d) < SURFACE_TOL);
    TASK_CHECK(max_derivative_error(net, d) < DERIV_TOL);
    // Domains are genuinely non-unit.
    TASK_CHECK(std::abs(d.transverse[0].get_domain_lower_bound()) > 0.5);
    return 0;
}

static int surface_knot_images()
{
    const NetworkData d = build_network(3, 3, 5u, false, true);
    const CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    // Only the cubic transverse curve (j=2) carries the internal knot, and
    // it was placed at the middle station's source parameter.
    const int j = 2;
    const auto& source = d.transverse[j];
    const auto& knots = source.get_knots();
    bool knot_at_station_param = false;
    for (int k = 0; k < knots.size(); ++k) {
        if (std::abs(knots[k] - d.a[j][1]) < 1e-12) knot_at_station_param = true;
    }
    TASK_CHECK(knot_at_station_param);
    const auto& rc = net.get_reparameterized_transverse_curves()[j];
    const auto& breakpoints = rc.get_breakpoints();
    // Because the knot image coincides with a station, no extra breakpoint is
    // inserted between stations: the breakpoints are exactly the stations.
    TASK_CHECK(static_cast<int>(breakpoints.size()) == d.U.size());
    bool station_present = false;
    for (double x : breakpoints) {
        if (std::abs(x - d.U[1]) < 1e-10) station_present = true;
    }
    TASK_CHECK(station_present);
    TASK_CHECK(std::abs(net.get_transverse_reparam(j).evaluate(d.U[1]) - d.a[j][1]) < 1e-12);
    TASK_CHECK(max_network_error(net, d) < SURFACE_TOL);
    TASK_CHECK(max_derivative_error(net, d) < DERIV_TOL);

    // And the between-stations case adds one breakpoint per station interval.
    const NetworkData d_between = build_network(3, 3, 5u, false, false);
    const CurveNetwork net_between(
        d_between.transverse, d_between.longitudinal, d_between.V, d_between.U,
        d_between.a, d_between.b);
    const auto& bp2 = net_between.get_reparameterized_transverse_curves()[2].get_breakpoints();
    TASK_CHECK(static_cast<int>(bp2.size()) > d_between.U.size());

    // C1 across every newly inserted breakline: gaps must decay linearly
    // with the one-sided sampling distance.
    const auto& patch = net.get_patch();
    std::vector<double> breaks;
    for (int k = 0; k < patch.get_num_knots_u(); ++k) {
        const double x = patch.get_knot_u(k);
        if (x > 1e-12 && x < 1 - 1e-12 &&
            (breaks.empty() || std::abs(breaks.back() - x) > 1e-12))
            breaks.push_back(x);
    }
    TASK_CHECK(!breaks.empty());
    std::vector<double> deriv_gap, pos_gap;
    for (double delta : {1e-5, 1e-6, 1e-7}) {
        double dg = 0.0, pg = 0.0;
        for (double x : breaks)
            for (int q = 1; q < 8; ++q) {
                const double v = q / 8.0;
                dg = std::max(dg,
                    (patch.evaluate_derivative_u(x - delta, v) -
                        patch.evaluate_derivative_u(x + delta, v))
                        .norm());
                pg = std::max(pg,
                    (patch.evaluate(x - delta, v) - patch.evaluate(x + delta, v)).norm());
            }
        deriv_gap.push_back(dg);
        pos_gap.push_back(pg);
    }
    // A genuine C1 jump is delta-independent.  Here both the position and
    // first-derivative one-sided gaps must shrink one order of magnitude per
    // decade of delta, i.e. they converge to zero linearly.
    for (int k = 0; k < 2; ++k) {
        const double d_ratio = deriv_gap[k] / deriv_gap[k + 1];
        const double p_ratio = pos_gap[k] / pos_gap[k + 1];
        TASK_CHECK(d_ratio > 5.0 && d_ratio < 200.0);
        TASK_CHECK(p_ratio > 5.0 && p_ratio < 200.0);
    }
    return 0;
}

static int surface_affine_reproduction()
{
    std::vector<Curve> cs(2), ds(2);
    const Row3 a0(0.1, -0.2, 0.5), bu(1.3, 0.7, -0.4), cv(-0.6, 1.1, 0.9);
    for (int j = 0; j < 2; ++j) {
        Points cp(2, 3);
        cp.row(0) = a0 + cv * j;
        cp.row(1) = a0 + bu + cv * j;
        cs[j] = make_clamped_curve(cp, 0.0, 1.0, {}, 1);
    }
    for (int i = 0; i < 2; ++i) {
        Points cp(2, 3);
        cp.row(0) = a0 + bu * i;
        cp.row(1) = a0 + bu * i + cv;
        ds[i] = make_clamped_curve(cp, 0.0, 1.0, {}, 1);
    }
    Vec stations(2);
    stations << 0.0, 1.0;
    const std::vector<std::vector<double>> table = {{0.0, 1.0}, {0.0, 1.0}};
    const CurveNetwork net(cs, ds, stations, stations, table, table);
    double err = 0.0;
    for (int q = 0; q <= 25; ++q)
        for (int r = 0; r <= 25; ++r) {
            const double u = q / 25.0, v = r / 25.0;
            err = std::max(err,
                (net.get_patch().evaluate(u, v) - (a0 + bu * u + cv * v)).norm());
        }
    TASK_CHECK(err < 1e-12);
    return 0;
}

static int surface_native_patch_ops()
{
    const NetworkData d = build_network(3, 3, 31u, false);
    const CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    const auto& patch = net.get_patch();
    TASK_CHECK(patch.num_control_points_u() > 0 && patch.num_control_points_v() > 0);
    TASK_CHECK(patch.get_degree_u() >= 1 && patch.get_degree_v() >= 1);

    // Iso curves, derivatives and split go through the existing patch API.
    const auto iso_u = patch.compute_iso_curve_u(0.42);
    const auto iso_v = patch.compute_iso_curve_v(0.37);
    TASK_CHECK((iso_u.evaluate(0.61) - patch.evaluate(0.61, 0.42)).norm() < 1e-12);
    TASK_CHECK((iso_v.evaluate(0.28) - patch.evaluate(0.37, 0.28)).norm() < 1e-12);
    const Row3 du = (patch.evaluate(0.31 + 1e-5, 0.55) - patch.evaluate(0.31 - 1e-5, 0.55)) /
                    2e-5;
    const Row3 dv = (patch.evaluate(0.31, 0.55 + 1e-5) - patch.evaluate(0.31, 0.55 - 1e-5)) /
                    2e-5;
    // Relative finite-difference check (the high-degree pieces have large
    // second derivatives, so the absolute truncation floor is correspondingly
    // higher).
    TASK_CHECK((du - patch.evaluate_derivative_u(0.31, 0.55)).norm() <
               1e-3 * std::max(1.0, du.norm()));
    TASK_CHECK((dv - patch.evaluate_derivative_v(0.31, 0.55)).norm() <
               1e-3 * std::max(1.0, dv.norm()));
    const auto pieces = patch.split(0.5, 0.5);
    TASK_CHECK(pieces.size() == 4);
    const double split_err =
        std::max((pieces[0].evaluate(0.2, 0.3) - patch.evaluate(0.2, 0.3)).norm(),
            (pieces[3].evaluate(0.7, 0.8) - patch.evaluate(0.7, 0.8)).norm());
    TASK_CHECK(split_err < 1e-8);

    auto cloned = patch.clone();
    TASK_CHECK((cloned->evaluate(0.33, 0.66) - patch.evaluate(0.33, 0.66)).norm() < 1e-14);
    return 0;
}

// ===========================================================================
// compatibility group
// ===========================================================================

static int compat_family_swap()
{
    const NetworkData d = build_network(3, 4, 17u, true);
    const CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    std::vector<std::vector<double>> a2(d.longitudinal.size(),
        std::vector<double>(d.transverse.size()));
    std::vector<std::vector<double>> b2(d.transverse.size(),
        std::vector<double>(d.longitudinal.size()));
    for (int j = 0; j < static_cast<int>(d.transverse.size()); ++j)
        for (int i = 0; i < static_cast<int>(d.longitudinal.size()); ++i) {
            a2[i][j] = d.b[i][j];
            b2[j][i] = d.a[j][i];
        }
    const CurveNetwork swapped(d.longitudinal, d.transverse, d.U, d.V, a2, b2);
    double err = 0.0;
    for (int q = 0; q <= 20; ++q)
        for (int r = 0; r <= 20; ++r) {
            const double u = q / 20.0, v = r / 20.0;
            err = std::max(
                err, (swapped.get_patch().evaluate(u, v) - net.get_patch().evaluate(v, u)).norm());
        }
    // Both builds run the same native pipeline with the families exchanged;
    // degree-9 piecewise Bezier arithmetic makes the agreement numerical
    // rather than bit-exact, but it stays far below the 1e-8 fidelity target.
    TASK_CHECK(err < 1e-8);
    return 0;
}

static int compat_invalid_rejected()
{
    const NetworkData good = build_network(3, 3, 3u, false);

    // Family size out of range.
    {
        NetworkData d = good;
        d.transverse.resize(1);
        d.V.resize(1);
        d.V[0] = 0.0;
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
        }));
    }
    // Duplicate station.
    {
        NetworkData d = good;
        d.V[1] = d.V[0];
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
        }));
    }
    // Station spacing below 0.05.
    {
        NetworkData d = good;
        Vec small(3);
        small << 0.0, 0.03, 1.0;
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, small, d.U, d.a, d.b);
        }));
    }
    // Non-monotone parameter row.
    {
        NetworkData d = good;
        std::swap(d.a[0][1], d.a[0][2]);
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
        }));
    }
    // Wrong endpoint.
    {
        NetworkData d = good;
        d.a[1].front() = d.a[1].front() + 0.07;
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
        }));
    }
    // Non-finite control point.
    {
        NetworkData d = good;
        d.transverse[0].set_control_point(0, Row3(std::nan(""), 0.0, 0.0));
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
        }));
    }
    // Incompatible intersection: translate the whole longitudinal curve so
    // every declared intersection moves off the transverse network.
    {
        NetworkData d = good;
        const int num = d.longitudinal[1].get_num_control_points();
        for (int k = 0; k < num; ++k) {
            d.longitudinal[1].set_control_point(
                k, d.longitudinal[1].get_control_point(k) + Row3(0.0, 0.0, 0.01));
        }
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
        }));
    }
    // Periodic curve rejected.
    {
        NetworkData d = good;
        d.transverse[0].set_periodic(true);
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
        }));
    }
    // Kinked (only C0) piecewise linear curve rejected.
    {
        NetworkData d = good;
        const double dom_a = d.transverse[0].get_domain_lower_bound();
        const double dom_b = d.transverse[0].get_domain_upper_bound();
        const double mid = 0.5 * (dom_a + dom_b);
        Points cp(3, 3);
        cp << 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0; // 90-degree kink
        Vec knots(6);
        knots << dom_a, dom_a, mid, mid, dom_b, dom_b;
        Curve kinked;
        kinked.set_control_points(cp);
        kinked.set_knots(knots);
        d.transverse[0] = kinked;
        TASK_CHECK(throws_invalid([&]() {
            CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
        }));
    }
    return 0;
}

static int compat_retry_after_failure()
{
    const NetworkData d = build_network(3, 3, 9u, false);
    CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    const Row3 kept = net.get_patch().evaluate(0.37, 0.61);

    bool threw = false;
    try {
        auto bad_a = d.a;
        std::reverse(bad_a[0].begin(), bad_a[0].end());
        CurveNetwork ignored(d.transverse, d.longitudinal, d.V, d.U, bad_a, d.b);
        (void)ignored;
    } catch (const nanospline::invalid_setting_error&) {
        threw = true;
    }
    TASK_CHECK(threw);
    // The previously built output is untouched.
    TASK_CHECK((net.get_patch().evaluate(0.37, 0.61) - kept).norm() < 1e-15);
    TASK_CHECK(max_network_error(net, d) < SURFACE_TOL);

    // A legal retry on fresh data still succeeds.
    const NetworkData d2 = build_network(2, 3, 11u, true);
    const CurveNetwork net2(d2.transverse, d2.longitudinal, d2.V, d2.U, d2.a, d2.b);
    TASK_CHECK(max_network_error(net2, d2) < SURFACE_TOL);
    return 0;
}

static CurveNetwork build_owned(NetworkData& snapshot, Row3& probe)
{
    NetworkData d = build_network(3, 3, 21u, true);
    CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    probe = net.get_patch().evaluate(0.44, 0.58);
    snapshot = std::move(d); // sources leave scope afterwards
    return net;
}

static int compat_ownership()
{
    NetworkData snapshot;
    Row3 probe;
    CurveNetwork net = build_owned(snapshot, probe);
    // Source curves are out of scope in the caller; mutate the snapshot too.
    snapshot.transverse[0].set_control_point(0, Row3(8.0, 8.0, 8.0));
    snapshot.longitudinal[1].set_control_point(0, Row3(-7.0, 2.0, 1.0));
    TASK_CHECK((net.get_patch().evaluate(0.44, 0.58) - probe).norm() < 1e-15);
    TASK_CHECK(max_network_error(net, snapshot) > 0.0); // sources now disagree...
    // ...while the owned composed curves and patch remain self-consistent.
    const auto& rc = net.get_reparameterized_transverse_curves()[0].get_curve();
    double err = 0.0;
    for (int q = 0; q <= 50; ++q) {
        const double u = q / 50.0;
        err = std::max(err,
            (net.get_patch().evaluate(u, net.get_patch().get_v_lower_bound()) -
                rc.evaluate(u))
                .norm());
    }
    TASK_CHECK(err < 1e-11);

    // Composed control nets are native data, not references to source points.
    TASK_CHECK(rc.get_control_points().rows() !=
               snapshot.transverse[0].get_control_points().rows());
    return 0;
}

static int compat_structure()
{
    const NetworkData d = build_network(2, 2, 41u, false);
    const CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    const auto& patch = net.get_patch();
    TASK_CHECK(patch.get_u_lower_bound() == 0.0 && patch.get_u_upper_bound() == 1.0);
    TASK_CHECK(patch.get_v_lower_bound() == 0.0 && patch.get_v_upper_bound() == 1.0);
    TASK_CHECK(patch.get_num_knots_u() ==
               patch.num_control_points_u() + patch.get_degree_u() + 1);
    TASK_CHECK(patch.get_num_knots_v() ==
               patch.num_control_points_v() + patch.get_degree_v() + 1);
    for (int k = 0; k < patch.num_control_points(); ++k) {
        TASK_CHECK(patch.get_control_grid().row(k).allFinite());
    }
    for (int j = 0; j < net.num_transverse(); ++j) {
        const auto& phi = net.get_transverse_reparam(j);
        TASK_CHECK(std::abs(phi.lower_bound()) < 1e-14);
        TASK_CHECK(std::abs(phi.upper_bound() - 1.0) < 1e-14);
        for (int q = 0; q <= 20; ++q) {
            const double u = q / 20.0;
            TASK_CHECK(std::isfinite(phi.evaluate(u)) && std::isfinite(phi.derivative(u)));
        }
    }
    return 0;
}

static int surface_largest_family()
{
    // Both families at the maximum supported size of six curves.
    const NetworkData d = build_network(6, 6, 55u, true);
    const CurveNetwork net(d.transverse, d.longitudinal, d.V, d.U, d.a, d.b);
    TASK_CHECK(net.num_transverse() == 6 && net.num_longitudinal() == 6);
    TASK_CHECK(max_network_error(net, d) < SURFACE_TOL);
    TASK_CHECK(max_derivative_error(net, d) < DERIV_TOL);
    return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

extern const struct task_case task_cases[] = {
    {"synchronization", "reparam_rule", &sync_reparam_rule},
    {"synchronization", "reparam_reversed", &sync_reparam_reversed},
    {"synchronization", "composition_across_knots", &sync_composition_across_knots},
    {"synchronization", "natural_equations", &sync_natural_equations},
    {"synchronization", "gordon_luv_subtraction", &sync_gordon_luv_subtraction},
    {"surface", "nonplanar_fidelity", &surface_nonplanar_fidelity},
    {"surface", "reversed_nonunit", &surface_reversed_nonunit},
    {"surface", "knot_images", &surface_knot_images},
    {"surface", "affine_reproduction", &surface_affine_reproduction},
    {"surface", "native_patch_ops", &surface_native_patch_ops},
    {"surface", "largest_family", &surface_largest_family},
    {"compatibility", "family_swap", &compat_family_swap},
    {"compatibility", "invalid_rejected", &compat_invalid_rejected},
    {"compatibility", "retry_after_failure", &compat_retry_after_failure},
    {"compatibility", "ownership", &compat_ownership},
    {"compatibility", "structure", &compat_structure},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
