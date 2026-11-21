#include "nanospline/ReparameterizedBSpline.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <Eigen/Dense>

#include <nanospline/Exceptions.h>

namespace nanospline {

namespace {

using Scalar = double;
using Vec = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;

// Dense polynomial in one variable: coeff[k] is the coefficient of x^k.
struct Poly
{
    std::vector<Scalar> coeff;

    Poly() = default;
    explicit Poly(std::vector<Scalar> c)
        : coeff(std::move(c))
    {}
    Poly(std::initializer_list<Scalar> c)
        : coeff(c)
    {}

    int degree() const { return static_cast<int>(coeff.size()) - 1; }

    static Poly constant(Scalar v) { return Poly({v}); }

    Poly operator+(const Poly& o) const
    {
        std::vector<Scalar> r(std::max(coeff.size(), o.coeff.size()), 0.0);
        for (size_t i = 0; i < coeff.size(); ++i) r[i] += coeff[i];
        for (size_t i = 0; i < o.coeff.size(); ++i) r[i] += o.coeff[i];
        return Poly(std::move(r));
    }

    Poly operator*(const Poly& o) const
    {
        std::vector<Scalar> r(coeff.size() + o.coeff.size() - 1, 0.0);
        for (size_t i = 0; i < coeff.size(); ++i) {
            for (size_t j = 0; j < o.coeff.size(); ++j) {
                r[i + j] += coeff[i] * o.coeff[j];
            }
        }
        return Poly(std::move(r));
    }

    Poly scaled(Scalar s) const
    {
        std::vector<Scalar> r = coeff;
        for (auto& v : r) v *= s;
        return Poly(std::move(r));
    }

    // Return p(a + b*y) as a polynomial in y.
    Poly substitute_linear(Scalar a, Scalar b) const
    {
        const int n = degree();
        // Powers of (a + b y) built incrementally.
        std::vector<Poly> powers;
        powers.reserve(n + 1);
        powers.push_back(Poly::constant(1.0));
        const Poly linear({a, b});
        for (int k = 1; k <= n; ++k) {
            powers.push_back(powers.back() * linear);
        }
        Poly result;
        for (int k = 0; k <= n; ++k) {
            result = result + powers[k].scaled(coeff[k]);
        }
        return result;
    }
};

Scalar binomial(int n, int k)
{
    if (k < 0 || k > n) return 0.0;
    k = std::min(k, n - k);
    Scalar r = 1.0;
    for (int i = 0; i < k; ++i) {
        r *= static_cast<Scalar>(n - i) / static_cast<Scalar>(i + 1);
    }
    return r;
}

// Hermite basis on a unit interval.
Scalar h00(Scalar x) { return 1.0 - 3.0 * x * x + 2.0 * x * x * x; }
Scalar h10(Scalar x) { return x - 2.0 * x * x + x * x * x; }
Scalar h01(Scalar x) { return 3.0 * x * x - 2.0 * x * x * x; }
Scalar h11(Scalar x) { return -x * x + x * x * x; }

// Build the monotone slope sequence following the fixed rule.
Vec build_slopes(const Vec& stations, const Vec& parameters)
{
    const int n = static_cast<int>(stations.size());
    std::vector<Scalar> delta(n - 1);
    for (int k = 0; k < n - 1; ++k) {
        delta[k] = (parameters[k + 1] - parameters[k]) / (stations[k + 1] - stations[k]);
        if (!(delta[k] > 0.0)) {
            throw invalid_setting_error(
                "Reparameterization requires strictly increasing stations and parameters.");
        }
    }

    Vec slopes(n);
    slopes[0] = delta[0];
    slopes[n - 1] = delta[n - 2];
    for (int k = 1; k < n - 1; ++k) {
        // Harmonic mean of the two adjacent secant slopes.
        slopes[k] = 2.0 / (1.0 / delta[k - 1] + 1.0 / delta[k]);
    }

    // Segment-wise monotonicity correction.
    for (int k = 0; k < n - 1; ++k) {
        const Scalar alpha = slopes[k] / delta[k];
        const Scalar beta = slopes[k + 1] / delta[k];
        const Scalar norm2 = alpha * alpha + beta * beta;
        if (norm2 > 9.0) {
            const Scalar factor = 3.0 / std::sqrt(norm2);
            slopes[k] *= factor;
            slopes[k + 1] *= factor;
        }
    }
    return slopes;
}

// Power-basis coefficients of the Hermite cubic on [s0, s1] as a polynomial
// in s.
Poly hermite_poly(Scalar s0, Scalar s1, Scalar t0, Scalar t1, Scalar m0, Scalar m1)
{
    const Scalar h = s1 - s0;
    // g(z) = t0 h00 + h m0 h10 + t1 h01 + h m1 h11, z = (s - s0)/h.
    const Poly pz({
        t0,
        h * m0,
        -3.0 * t0 - 2.0 * h * m0 + 3.0 * t1 - h * m1,
        2.0 * t0 + h * m0 - 2.0 * t1 + h * m1
    });
    return pz.substitute_linear(-s0 / h, 1.0 / h);
}

// Convert a power-basis polynomial c(y) on y in [0, 1] into Bernstein
// control points of the same degree.  The identity
//     y^k = sum_{j>=k} C(k,j)/C(n,j) B_j^n(y)
// gives b_j = sum_{k=0}^{j} c_k * C(j,k) / C(n,k).
Vec power_to_bernstein(const std::vector<Scalar>& c)
{
    const int n = static_cast<int>(c.size()) - 1;
    Vec b(n + 1);
    for (int j = 0; j <= n; ++j) {
        Scalar v = 0.0;
        for (int k = 0; k <= j; ++k) {
            v += c[k] * binomial(j, k) / binomial(n, k);
        }
        b[j] = v;
    }
    return b;
}

} // namespace

PiecewiseCubicReparam::PiecewiseCubicReparam(const StationVector& stations,
    const StationVector& parameters)
{
    if (stations.size() < 2 || stations.size() != parameters.size()) {
        throw invalid_setting_error(
            "Reparameterization needs at least two matching stations and parameters.");
    }
    for (int k = 1; k < stations.size(); ++k) {
        if (!(stations[k] > stations[k - 1])) {
            throw invalid_setting_error("Reparameterization stations must be strictly increasing.");
        }
        if (!(parameters[k] > parameters[k - 1])) {
            throw invalid_setting_error(
                "Reparameterization parameters must be strictly increasing after orientation.");
        }
    }
    m_stations = stations;
    m_parameters = parameters;
    m_slopes = build_slopes(stations, parameters);
}

PiecewiseCubicReparam make_reparameterization(
    const PiecewiseCubicReparam::StationVector& stations,
    const PiecewiseCubicReparam::StationVector& source_parameters)
{
    if (stations.size() != source_parameters.size() || stations.size() < 2) {
        throw invalid_setting_error(
            "Reparameterization needs at least two matching stations and parameters.");
    }
    bool reversed = source_parameters[1] < source_parameters[0];
    for (int k = 1; k < source_parameters.size(); ++k) {
        const bool increasing = source_parameters[k] > source_parameters[k - 1];
        if (increasing == reversed) {
            throw invalid_setting_error(
                "Source parameter row must be strictly monotone.");
        }
    }

    PiecewiseCubicReparam::StationVector oriented = source_parameters;
    if (reversed) {
        oriented = -oriented;
    }
    PiecewiseCubicReparam result(stations, oriented);
    result.m_reversed = reversed;
    return result;
}

int PiecewiseCubicReparam::locate_segment(Scalar s) const
{
    const int n = static_cast<int>(m_stations.size());
    if (s <= m_stations[0]) return 0;
    if (s >= m_stations[n - 1]) return n - 2;
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (m_stations[mid] <= s)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

Scalar PiecewiseCubicReparam::evaluate(Scalar s) const
{
    const int k = locate_segment(s);
    const Scalar s0 = m_stations[k];
    const Scalar s1 = m_stations[k + 1];
    const Scalar h = s1 - s0;
    const Scalar x = std::max(0.0, std::min(1.0, (s - s0) / h));
    Scalar value = m_parameters[k] * h00(x) + h * m_slopes[k] * h10(x) +
                   m_parameters[k + 1] * h01(x) + h * m_slopes[k + 1] * h11(x);
    return m_reversed ? -value : value;
}

Scalar PiecewiseCubicReparam::derivative(Scalar s) const
{
    const int k = locate_segment(s);
    const Scalar s0 = m_stations[k];
    const Scalar s1 = m_stations[k + 1];
    const Scalar h = s1 - s0;
    const Scalar x = std::max(0.0, std::min(1.0, (s - s0) / h));
    // Derivatives of the Hermite basis with respect to x.
    const Scalar d00 = (-6.0 * x + 6.0 * x * x) / h;
    const Scalar d10 = (1.0 - 4.0 * x + 3.0 * x * x) / h;
    const Scalar d01 = (6.0 * x - 6.0 * x * x) / h;
    const Scalar d11 = (-2.0 * x + 3.0 * x * x) / h;
    Scalar value = m_parameters[k] * d00 + h * m_slopes[k] * d10 +
                   m_parameters[k + 1] * d01 + h * m_slopes[k + 1] * d11;
    return m_reversed ? -value : value;
}

ReparameterizedBSpline::ReparameterizedBSpline(
    const Curve& curve, const PiecewiseCubicReparam& reparam)
{
    m_reparam = reparam;

    // Native piecewise Bezier decomposition of the source curve, together with
    // the parameter bounds of every piece.
    std::vector<Bezier<Scalar, 3, -1>> source_beziers;
    std::vector<Scalar> source_bounds;
    std::tie(source_beziers, source_bounds) = curve.convert_to_Bezier();
    const int source_degree = curve.get_degree();
    const int composed_degree = 3 * source_degree;
    if (composed_degree <= 0) {
        throw invalid_setting_error("Cannot reparameterize a degree-0 curve.");
    }

    const auto& stations = reparam.get_stations();
    const int num_pieces = static_cast<int>(stations.size()) - 1;

    const Scalar t_domain_min = curve.get_domain_lower_bound();
    const Scalar t_domain_max = curve.get_domain_upper_bound();
    const Scalar t_start = reparam.evaluate(stations[0]);
    const Scalar t_end = reparam.evaluate(stations[stations.size() - 1]);
    const Scalar t_lo = std::min(t_start, t_end);
    const Scalar t_hi = std::max(t_start, t_end);
    if (std::abs(t_lo - t_domain_min) > 1e-10 * std::max(1.0, std::abs(t_domain_min)) ||
        std::abs(t_hi - t_domain_max) > 1e-10 * std::max(1.0, std::abs(t_domain_max))) {
        throw invalid_setting_error(
            "Reparameterization does not span the curve parameter domain.");
    }

    // Collect every breakpoint in station space: reparam stations plus the
    // inverse images of the source curve's internal knots.
    std::vector<Scalar> breakpoints;
    breakpoints.reserve(2 * stations.size());
    for (int k = 0; k < stations.size(); ++k) breakpoints.push_back(stations[k]);

    const Scalar orientation = reparam.derivative(stations[0]) >= 0.0 ? 1.0 : -1.0;
    auto inverse_image = [&](Scalar knot, Scalar s_lo, Scalar s_hi) {
        // Bisection of reparam(s) = knot on a monotone piece.
        Scalar lo = s_lo, hi = s_hi;
        for (int iter = 0; iter < 80; ++iter) {
            const Scalar mid = 0.5 * (lo + hi);
            const Scalar diff = orientation * (reparam.evaluate(mid) - knot);
            if (diff > 0.0)
                hi = mid;
            else
                lo = mid;
        }
        return 0.5 * (lo + hi);
    };

    for (int k = 0; k < num_pieces; ++k) {
        const Scalar s0 = stations[k];
        const Scalar s1 = stations[k + 1];
        const Scalar g0 = reparam.evaluate(s0);
        const Scalar g1 = reparam.evaluate(s1);
        const Scalar lo_t = std::min(g0, g1);
        const Scalar hi_t = std::max(g0, g1);
        for (int j = 1; j + 1 < static_cast<int>(source_bounds.size()); ++j) {
            const Scalar knot = source_bounds[j];
            if (knot <= lo_t + 1e-14 || knot >= hi_t - 1e-14) continue;
            breakpoints.push_back(inverse_image(knot, s0, s1));
        }
    }
    std::sort(breakpoints.begin(), breakpoints.end());
    breakpoints.erase(
        std::unique(breakpoints.begin(), breakpoints.end(),
            [](Scalar a, Scalar b) { return std::abs(a - b) < 1e-12; }),
        breakpoints.end());
    m_breakpoints = Eigen::Map<Vec>(breakpoints.data(), breakpoints.size()).eval();

    // For every subpiece, compose the covering source Bezier segment with the
    // cubic reparameterization.
    std::vector<Bezier<Scalar, 3, -1>> composed_beziers;
    std::vector<Scalar> composed_bounds;
    composed_beziers.reserve(breakpoints.size() - 1);
    composed_bounds.reserve(breakpoints.size());
    composed_bounds.push_back(breakpoints[0]);

    for (int k = 0; k + 1 < static_cast<int>(breakpoints.size()); ++k) {
        const Scalar a = breakpoints[k];
        const Scalar b = breakpoints[k + 1];
        const Scalar ta = reparam.evaluate(a);
        const Scalar tb = reparam.evaluate(b);

        // Locate the source Bezier segment covering [ta, tb].
        int seg = static_cast<int>(
            std::upper_bound(source_bounds.begin(), source_bounds.end(),
                std::min(ta, tb) + 1e-14) -
            source_bounds.begin()) -
                  1;
        seg = std::max(0, std::min(seg, static_cast<int>(source_bounds.size()) - 2));
        const Scalar tb0 = source_bounds[seg];
        const Scalar tb1 = source_bounds[seg + 1];
        const Scalar width = tb1 - tb0;
        if (!(width > 0.0)) {
            throw invalid_setting_error("Degenerate source Bezier segment.");
        }

        // Hermite cubic g(s) on the reparam piece containing a, expressed in s.
        const int piece = reparam.locate_segment(a);
        const Poly g_poly = hermite_poly(reparam.get_stations()[piece],
            reparam.get_stations()[piece + 1],
            reparam.get_parameters()[piece],
            reparam.get_parameters()[piece + 1],
            reparam.get_slopes()[piece],
            reparam.get_slopes()[piece + 1]);
        // g(s) carries the negated orientation internally; evaluation restores
        // the sign, so mirror the polynomial for reversed rows.
        Poly g_oriented = reparam.is_reversed() ? g_poly.scaled(-1.0) : g_poly;

        // x(s) = (g(s) - tb0) / width, rewritten in y = (s - a) / (b - a).
        Poly x_of_s = g_oriented;
        x_of_s.coeff[0] -= tb0;
        x_of_s = x_of_s.scaled(1.0 / width);
        const Scalar length = b - a;
        if (!(length > 0.0)) {
            throw invalid_setting_error("Degenerate reparameterization segment.");
        }
        Poly x_of_y = x_of_s.substitute_linear(a, length);

        // Power coefficients of the source segment from its Bernstein form.
        const auto& source_ctrl = source_beziers[seg].get_control_points();
        std::vector<Eigen::RowVector3d> power_ctrl(source_degree + 1);
        for (int r = 0; r <= source_degree; ++r) {
            power_ctrl[r].setZero();
            for (int i = 0; i <= r; ++i) {
                const Scalar sign = ((r - i) % 2 == 0) ? 1.0 : -1.0;
                const Scalar factor =
                    binomial(source_degree, i) * binomial(source_degree - i, r - i) * sign;
                power_ctrl[r] += factor * source_ctrl.row(i);
            }
        }

        // q(y) = sum_r power_ctrl[r] * x(y)^r, accumulated as polynomials per
        // coordinate.
        std::vector<std::vector<Scalar>> q(3, std::vector<Scalar>(composed_degree + 1, 0.0));
        Poly x_power = Poly::constant(1.0);
        for (int r = 0; r <= source_degree; ++r) {
            for (int c = 0; c < 3; ++c) {
                for (int e = 0; e <= x_power.degree(); ++e) {
                    q[c][e] += power_ctrl[r][c] * x_power.coeff[e];
                }
            }
            x_power = x_power * x_of_y;
        }

        typename Curve::ControlPoints ctrl(composed_degree + 1, 3);
        for (int c = 0; c < 3; ++c) {
            const Vec bern = power_to_bernstein(q[c]);
            for (int j = 0; j <= composed_degree; ++j) {
                if (!std::isfinite(bern[j])) {
                    throw invalid_setting_error(
                        "Numerical failure while composing reparameterized curve.");
                }
                ctrl(j, c) = bern[j];
            }
        }

        Bezier<Scalar, 3, -1> segment;
        segment.set_control_points(std::move(ctrl));
        composed_beziers.push_back(std::move(segment));
        composed_bounds.push_back(b);
    }

    // Assemble a native clamped BSpline in piecewise Bezier form: every
    // internal breakpoint carries full multiplicity, which is allowed by the
    // construction contract.  All segments share the composed degree.
    const int num_segments = static_cast<int>(composed_beziers.size());
    const int num_ctrl = num_segments * composed_degree + 1;
    const int num_knots = num_ctrl + composed_degree + 1;
    typename Curve::ControlPoints grid(num_ctrl, 3);
    typename Curve::KnotVector knots(num_knots);

    for (int k = 0; k < num_segments; ++k) {
        grid.block(k * composed_degree, 0, composed_degree + 1, 3) =
            composed_beziers[k].get_control_points();
        // Adjacent pieces overlap at the shared joint row; both endpoint
        // controls are the value at the breakpoint, so no override is needed.
    }

    for (int i = 0; i <= composed_degree; ++i) {
        knots[i] = composed_bounds.front();
        knots[num_knots - 1 - i] = composed_bounds.back();
    }
    for (int k = 1; k < num_segments; ++k) {
        for (int r = 0; r < composed_degree; ++r) {
            knots[composed_degree + 1 + (k - 1) * composed_degree + r] = composed_bounds[k];
        }
    }

    m_curve.set_control_points(std::move(grid));
    m_curve.set_knots(std::move(knots));
    if (m_curve.get_degree() != composed_degree) {
        throw invalid_setting_error("Failed to assemble reparameterized BSpline.");
    }
}

} // namespace nanospline
