#include "nanospline/CurveNetwork.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <Eigen/Dense>

#include <nanospline/Exceptions.h>

namespace nanospline {

namespace {

using Scalar = double;
using Vec = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
using Points = Eigen::Matrix<Scalar, Eigen::Dynamic, 3>;
using Curve = BSpline<Scalar, 3, -1>;
using BezierCurve = Bezier<Scalar, 3, -1>;
using Patch = BSplinePatch<Scalar, 3, -1, -1>;

constexpr Scalar STATION_GAP = 0.05;
constexpr Scalar INTERSECTION_TOL = 1e-10;
constexpr Scalar KNOT_EPS = 1e-12;

bool finite_point(const Eigen::RowVector3d& p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

// Unique sorted internal breakpoints of a clamped knot vector.
std::vector<Scalar> internal_breakpoints(const Vec& knots, int degree)
{
    std::vector<Scalar> result;
    const Scalar lo = knots[degree];
    const Scalar hi = knots[knots.size() - degree - 1];
    for (int i = 0; i < knots.size(); ++i) {
        if (knots[i] <= lo + KNOT_EPS || knots[i] >= hi - KNOT_EPS) continue;
        if (result.empty() || std::abs(result.back() - knots[i]) > KNOT_EPS) {
            result.push_back(knots[i]);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Full-multiplicity clamped knot vector over [0, 1] with the given internal
// breakpoints.
Vec full_knot_vector(const std::vector<Scalar>& breakpoints, int degree)
{
    const int num_segments = static_cast<int>(breakpoints.size()) + 1;
    Vec knots(num_segments * degree + degree + 2);
    for (int i = 0; i <= degree; ++i) {
        knots[i] = 0.0;
        knots[knots.size() - 1 - i] = 1.0;
    }
    for (int k = 0; k < static_cast<int>(breakpoints.size()); ++k) {
        for (int r = 0; r < degree; ++r) {
            knots[degree + 1 + k * degree + r] = breakpoints[k];
        }
    }
    return knots;
}

// Refine a native clamped B-spline curve onto a target full-multiplicity
// knot vector of (equal or higher) target degree.  Knot insertion and degree
// elevation are exact, so the geometry is unchanged.
Curve refine_curve(const Curve& curve, const Vec& target_knots, int target_degree)
{
    std::vector<BezierCurve> beziers;
    std::vector<Scalar> bounds;
    std::tie(beziers, bounds) = curve.convert_to_Bezier();

    std::vector<BezierCurve> elevated;
    elevated.reserve(beziers.size());
    for (const auto& b : beziers) {
        BezierCurve current = b;
        while (current.get_degree() < target_degree) {
            current = current.elevate_degree();
        }
        if (current.get_degree() != target_degree) {
            throw invalid_setting_error("Curve degree elevation failed during network assembly.");
        }
        elevated.push_back(std::move(current));
    }

    Curve result(elevated, bounds);

    // Insert every target internal breakpoint up to full multiplicity.
    std::vector<Scalar> breakpoints;
    for (int i = target_degree + 1; i + target_degree + 1 < target_knots.size();) {
        const Scalar t = target_knots[i];
        breakpoints.push_back(t);
        i += target_degree;
    }
    for (Scalar t : breakpoints) {
        const int k = result.locate_span(t);
        const int mult = (t == result.get_knots()[k]) ? result.get_multiplicity(k) : 0;
        if (mult < target_degree) {
            result.insert_knot(t, target_degree - mult);
        }
    }

    const auto& got = result.get_knots();
    if (got.size() != target_knots.size()) {
        throw invalid_setting_error(
            "Knot refinement failed: knot count mismatch (got " +
            std::to_string(got.size()) + " target " + std::to_string(target_knots.size()) +
            " degree " + std::to_string(target_degree) + ").");
    }
    for (int i = 0; i < got.size(); ++i) {
        if (std::abs(got[i] - target_knots[i]) > KNOT_EPS) {
            throw invalid_setting_error("Knot refinement failed: knot values mismatch.");
        }
    }
    return result;
}

// Rebuild a tensor-product patch on a common full-multiplicity knot space.
Patch refine_patch(const Patch& patch,
    const Vec& target_knots_u,
    const Vec& target_knots_v,
    int target_degree_u,
    int target_degree_v)
{
    const auto& grid = patch.get_control_grid();
    const int nu = patch.num_control_points_u();
    const int nv = patch.num_control_points_v();

    // 1. Refine along u for every fixed-v control row.
    std::vector<Points> u_rows(nv);
    int new_nu = -1;
    for (int j = 0; j < nv; ++j) {
        Points column(nu, 3);
        for (int i = 0; i < nu; ++i) column.row(i) = grid.row(i * nv + j);
        Curve c;
        c.set_control_points(column);
        c.set_knots(patch.get_knots_u());
        Curve refined = refine_curve(c, target_knots_u, target_degree_u);
        new_nu = refined.get_num_control_points();
        u_rows[j] = refined.get_control_points();
    }

    // 2. Refine along v for every fixed-u control row.
    int new_nv = -1;
    std::vector<Curve> v_refined(new_nu);
    for (int q = 0; q < new_nu; ++q) {
        Points row(nv, 3);
        for (int j = 0; j < nv; ++j) {
            row.row(j) = u_rows[j].row(q);
        }
        Curve c;
        c.set_control_points(row);
        c.set_knots(patch.get_knots_v());
        v_refined[q] = refine_curve(c, target_knots_v, target_degree_v);
        new_nv = v_refined[q].get_num_control_points();
    }

    Points final_grid(new_nu * new_nv, 3);
    for (int q = 0; q < new_nu; ++q) {
        for (int r = 0; r < new_nv; ++r) {
            final_grid.row(q * new_nv + r) = v_refined[q].get_control_points().row(r);
        }
    }

    Patch result;
    result.set_degree_u(target_degree_u);
    result.set_degree_v(target_degree_v);
    result.set_control_grid(final_grid);
    result.set_knots_u(target_knots_u);
    result.set_knots_v(target_knots_v);
    result.initialize();
    return result;
}

Patch make_patch(const Points& grid, const Vec& knots_u, const Vec& knots_v,
    int degree_u, int degree_v)
{
    Patch patch;
    patch.set_degree_u(degree_u);
    patch.set_degree_v(degree_v);
    patch.set_control_grid(grid);
    patch.set_knots_u(knots_u);
    patch.set_knots_v(knots_v);
    patch.initialize();
    return patch;
}

// Natural cubic (or linear) spline through values at stations.
Points natural_interpolate(const Vec& stations, const Points& values,
    Vec& knots, int& degree)
{
    const int n = static_cast<int>(stations.size());
    if (n == 2) {
        degree = 1;
        knots.resize(4);
        knots << stations[0], stations[0], stations[1], stations[1];
        return values;
    }

    degree = 3;
    std::vector<Scalar> h(n - 1);
    for (int k = 0; k < n - 1; ++k) {
        h[k] = stations[k + 1] - stations[k];
        if (!(h[k] > 0.0)) {
            throw invalid_setting_error("Interpolation stations must be strictly increasing.");
        }
    }

    // Solve the tridiagonal system for the interior second derivatives M.
    const int m = n - 2;
    Eigen::Matrix<Scalar, Eigen::Dynamic, 3> rhs(m, 3);
    Vec lower(m), diag(m), upper(m);
    for (int k = 0; k < m; ++k) {
        const int idx = k + 1;
        lower[k] = (k > 0) ? h[idx - 1] : 0.0;
        diag[k] = 2.0 * (h[idx - 1] + h[idx]);
        upper[k] = (k + 1 < m) ? h[idx] : 0.0;
        const Eigen::RowVector3d slope_right =
            ((values.row(idx + 1) - values.row(idx)) / h[idx]).eval();
        const Eigen::RowVector3d slope_left =
            ((values.row(idx) - values.row(idx - 1)) / h[idx - 1]).eval();
        rhs.row(k) = 6.0 * (slope_right - slope_left);
    }

    // Thomas algorithm on the three coordinate RHS simultaneously.
    Vec cprime(m);
    Eigen::Matrix<Scalar, Eigen::Dynamic, 3> dprime(m, 3);
    cprime[0] = upper[0] / diag[0];
    dprime.row(0) = rhs.row(0) / diag[0];
    for (int k = 1; k < m; ++k) {
        const Scalar denom = diag[k] - lower[k] * cprime[k - 1];
        if (!(std::abs(denom) > 0.0)) {
            throw invalid_setting_error(
                "Numerical failure: singular natural spline system.");
        }
        cprime[k] = (k + 1 < m) ? upper[k] / denom : 0.0;
        dprime.row(k) = (rhs.row(k) - lower[k] * dprime.row(k - 1)) / denom;
    }
    Eigen::Matrix<Scalar, Eigen::Dynamic, 3> second(m, 3);
    second.row(m - 1) = dprime.row(m - 1);
    for (int k = m - 2; k >= 0; --k) {
        second.row(k) = dprime.row(k) - cprime[k] * second.row(k + 1);
    }
    for (int k = 0; k < m; ++k) {
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(second(k, c))) {
                throw invalid_setting_error(
                    "Numerical failure: non-finite natural spline coefficients.");
            }
        }
    }

    Eigen::Matrix<Scalar, Eigen::Dynamic, 3> M(n, 3);
    M.setZero();
    M.block(1, 0, m, 3) = second;

    // Bezier control points of every cubic segment.  Adjacent segments share
    // the natural spline's tangent and second derivative at the joints, so
    // the full-multiplicity assembly is geometrically C2 even though the
    // knots are repeated.  Building the knot vector directly (rather than
    // relying on data-dependent knot removal) makes the result identical for
    // every interpolation row sharing the same stations.
    const int num_segments = n - 1;
    const int num_ctrl = num_segments * 3 + 1;
    Points control(num_ctrl, 3);
    for (int k = 0; k < num_segments; ++k) {
        const Points secant = (values.row(k + 1) - values.row(k)) / h[k];
        const Points tangent_left =
            (secant - h[k] * (2.0 * M.row(k) + M.row(k + 1)) / 6.0).eval();
        const Points tangent_right =
            (secant + h[k] * (M.row(k) + 2.0 * M.row(k + 1)) / 6.0).eval();
        Points bez(4, 3);
        bez.row(0) = values.row(k);
        bez.row(1) = (values.row(k) + h[k] / 3.0 * tangent_left).eval();
        bez.row(2) = (values.row(k + 1) - h[k] / 3.0 * tangent_right).eval();
        bez.row(3) = values.row(k + 1);
        control.block(k * 3, 0, 4, 3) = bez;
        // The b3 of segment k and b0 of segment k+1 are both the station
        // value, so the shared joint needs no adjustment.
    }

    knots.resize(num_ctrl + 4);
    for (int r = 0; r < 4; ++r) {
        knots[r] = stations[0];
        knots[knots.size() - 1 - r] = stations[n - 1];
    }
    for (int k = 1; k < num_segments; ++k) {
        for (int r = 0; r < 3; ++r) {
            knots[4 + (k - 1) * 3 + r] = stations[k];
        }
    }
    return control;
}

// Merge several clamped knot vectors into one full-multiplicity vector over
// [0, 1] and report the common (maximum) degree.
Vec merge_full_knots(const std::vector<std::pair<const Vec*, int>>& inputs, int& common_degree)
{
    common_degree = 0;
    std::vector<Scalar> breakpoints;
    for (const auto& entry : inputs) {
        common_degree = std::max(common_degree, entry.second);
        for (Scalar t : internal_breakpoints(*entry.first, entry.second)) {
            bool duplicate = false;
            for (Scalar existing : breakpoints) {
                if (std::abs(existing - t) <= KNOT_EPS) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) breakpoints.push_back(t);
        }
    }
    std::sort(breakpoints.begin(), breakpoints.end());
    return full_knot_vector(breakpoints, common_degree);
}

} // namespace

namespace internal {

Points natural_cubic_interpolation(const Vec& stations, const Points& values,
    Vec& knots, int& degree)
{
    return natural_interpolate(stations, values, knots, degree);
}

Vec merge_knot_vectors(const Vec& knots_a, int degree_a, const Vec& knots_b,
    int degree_b, int& merged_degree)
{
    return merge_full_knots({{&knots_a, degree_a}, {&knots_b, degree_b}}, merged_degree);
}

} // namespace internal

CurveNetwork::CurveNetwork(const std::vector<Curve>& transverse_curves,
    const std::vector<Curve>& longitudinal_curves,
    const StationVector& v_stations,
    const StationVector& u_stations,
    const std::vector<std::vector<Scalar>>& a_table,
    const std::vector<std::vector<Scalar>>& b_table)
{
    const int m = static_cast<int>(transverse_curves.size()); // C_j, along u
    const int n = static_cast<int>(longitudinal_curves.size()); // D_i, along v

    // ------------------------------------------------------------------
    // Structural validation.  Everything is validated before any output
    // member is assigned, so a failed construction never overwrites an
    // existing result.
    // ------------------------------------------------------------------
    if (m < 2 || m > 6 || n < 2 || n > 6) {
        throw invalid_setting_error("Each curve family must contain between 2 and 6 curves.");
    }
    if (v_stations.size() != m || u_stations.size() != n) {
        throw invalid_setting_error("Station count does not match curve family size.");
    }
    auto validate_stations = [](const Vec& stations, const char* name) {
        if (stations.size() < 2 || std::abs(stations[0]) > KNOT_EPS ||
            std::abs(stations[stations.size() - 1] - 1.0) > KNOT_EPS) {
            throw invalid_setting_error(std::string(name) + " must run from 0 to 1.");
        }
        for (int k = 1; k < stations.size(); ++k) {
            if (!(stations[k] > stations[k - 1])) {
                throw invalid_setting_error(std::string(name) + " must be strictly increasing.");
            }
            if (stations[k] - stations[k - 1] < STATION_GAP - KNOT_EPS) {
                throw invalid_setting_error(
                    std::string(name) + " adjacent stations must be at least 0.05 apart.");
            }
        }
    };
    validate_stations(v_stations, "V stations");
    validate_stations(u_stations, "U stations");

    if (static_cast<int>(a_table.size()) != m ||
        static_cast<int>(b_table.size()) != n) {
        throw invalid_setting_error("Intersection table dimensions do not match the families.");
    }
    for (int j = 0; j < m; ++j) {
        if (static_cast<int>(a_table[j].size()) != n) {
            throw invalid_setting_error("a-table row size mismatch.");
        }
    }
    for (int i = 0; i < n; ++i) {
        if (static_cast<int>(b_table[i].size()) != m) {
            throw invalid_setting_error("b-table row size mismatch.");
        }
    }

    auto validate_curve = [](const Curve& curve, const char* name) {
        if (curve.get_periodic()) {
            throw invalid_setting_error(std::string(name) + " must be non-periodic.");
        }
        const int degree = curve.get_degree();
        if (degree < 1 || degree > 3) {
            throw invalid_setting_error(std::string(name) + " degree must be between 1 and 3.");
        }
        for (int k = 0; k < curve.get_num_control_points(); ++k) {
            if (!finite_point(curve.get_control_point(k))) {
                throw invalid_setting_error(std::string(name) + " contains non-finite data.");
            }
        }
        for (int k = 0; k < curve.get_num_knots(); ++k) {
            if (!std::isfinite(curve.get_knot(k))) {
                throw invalid_setting_error(std::string(name) + " contains non-finite knots.");
            }
        }
        // At least C1: the one-sided tangent vectors at every internal knot
        // must agree.  Checking the Bezier segment endpoint tangents is exact
        // and also accepts geometrically smooth curves whose knots carry full
        // multiplicity.
        std::vector<BezierCurve> pieces;
        std::vector<Scalar> bounds;
        std::tie(pieces, bounds) = curve.convert_to_Bezier();
        for (int k = 1; k < static_cast<int>(pieces.size()); ++k) {
            const auto& left = pieces[k - 1];
            const auto& right = pieces[k];
            const int d = degree;
            const Eigen::RowVector3d tangent_left =
                d * (left.get_control_points().row(d) - left.get_control_points().row(d - 1)) /
                (bounds[k] - bounds[k - 1]);
            const Eigen::RowVector3d tangent_right =
                d * (right.get_control_points().row(1) - right.get_control_points().row(0)) /
                (bounds[k + 1] - bounds[k]);
            const Scalar scale = std::max(1.0, tangent_left.norm());
            if ((tangent_left - tangent_right).norm() > 1e-8 * scale) {
                throw invalid_setting_error(
                    std::string(name) + " must be at least C1 continuous.");
            }
        }
    };
    for (int j = 0; j < m; ++j) validate_curve(transverse_curves[j], "Transverse curve");
    for (int i = 0; i < n; ++i) validate_curve(longitudinal_curves[i], "Longitudinal curve");

    auto validate_row = [](const std::vector<Scalar>& row, Scalar domain_min,
                           Scalar domain_max, const char* name) {
        if (row.size() < 2) {
            throw invalid_setting_error(std::string(name) + " row is too short.");
        }
        for (Scalar t : row) {
            if (!std::isfinite(t)) {
                throw invalid_setting_error(std::string(name) + " contains non-finite parameters.");
            }
        }
        const bool increasing = row[1] > row[0];
        for (int k = 1; k < static_cast<int>(row.size()); ++k) {
            if (increasing == (row[k] < row[k - 1]) || row[k] == row[k - 1]) {
                throw invalid_setting_error(
                    std::string(name) + " row must be strictly monotone.");
            }
        }
        const Scalar expected_first = increasing ? domain_min : domain_max;
        const Scalar expected_last = increasing ? domain_max : domain_min;
        if (std::abs(row.front() - expected_first) > KNOT_EPS ||
            std::abs(row.back() - expected_last) > KNOT_EPS) {
            throw invalid_setting_error(
                std::string(name) + " row endpoints must be the curve domain endpoints.");
        }
    };
    for (int j = 0; j < m; ++j) {
        validate_row(a_table[j], transverse_curves[j].get_domain_lower_bound(),
            transverse_curves[j].get_domain_upper_bound(), "a-table");
    }
    for (int i = 0; i < n; ++i) {
        validate_row(b_table[i], longitudinal_curves[i].get_domain_lower_bound(),
            longitudinal_curves[i].get_domain_upper_bound(), "b-table");
    }

    // Geometric compatibility of the declared intersections.
    for (int j = 0; j < m; ++j) {
        for (int i = 0; i < n; ++i) {
            const auto pc = transverse_curves[j].evaluate(a_table[j][i]);
            const auto pd = longitudinal_curves[i].evaluate(b_table[i][j]);
            if (!finite_point(pc) || !finite_point(pd) ||
                (pc - pd).norm() > INTERSECTION_TOL) {
                throw invalid_setting_error(
                    "Incompatible intersection: declared points do not coincide within 1e-10.");
            }
        }
    }

    // ------------------------------------------------------------------
    // Reparameterizations.
    // ------------------------------------------------------------------
    std::vector<PiecewiseCubicReparam> transverse_reparams;
    std::vector<PiecewiseCubicReparam> longitudinal_reparams;
    transverse_reparams.reserve(m);
    longitudinal_reparams.reserve(n);
    for (int j = 0; j < m; ++j) {
        Vec params(n);
        for (int i = 0; i < n; ++i) params[i] = a_table[j][i];
        transverse_reparams.push_back(make_reparameterization(u_stations, params));
    }
    for (int i = 0; i < n; ++i) {
        Vec params(m);
        for (int j = 0; j < m; ++j) params[j] = b_table[i][j];
        longitudinal_reparams.push_back(make_reparameterization(v_stations, params));
    }

    std::vector<ReparameterizedBSpline> reparam_transverse;
    std::vector<ReparameterizedBSpline> reparam_longitudinal;
    reparam_transverse.reserve(m);
    reparam_longitudinal.reserve(n);
    for (int j = 0; j < m; ++j) {
        reparam_transverse.emplace_back(transverse_curves[j], transverse_reparams[j]);
    }
    for (int i = 0; i < n; ++i) {
        reparam_longitudinal.emplace_back(longitudinal_curves[i], longitudinal_reparams[i]);
    }

    // ------------------------------------------------------------------
    // Common u/v knot spaces for the reparameterized curves.
    // ------------------------------------------------------------------
    std::vector<std::pair<const Vec*, int>> u_inputs;
    std::vector<std::pair<const Vec*, int>> v_inputs;
    for (int j = 0; j < m; ++j) {
        const Curve& c = reparam_transverse[j].get_curve();
        u_inputs.push_back({&c.get_knots(), c.get_degree()});
    }
    for (int i = 0; i < n; ++i) {
        const Curve& c = reparam_longitudinal[i].get_curve();
        v_inputs.push_back({&c.get_knots(), c.get_degree()});
    }
    int degree_u_curves = 1;
    int degree_v_curves = 1;
    const Vec u_curve_knots = merge_full_knots(u_inputs, degree_u_curves);
    const Vec v_curve_knots = merge_full_knots(v_inputs, degree_v_curves);

    // Refine every reparameterized curve onto its family's common space.
    std::vector<Curve> refined_transverse;
    std::vector<Curve> refined_longitudinal;
    refined_transverse.reserve(m);
    refined_longitudinal.reserve(n);
    for (int j = 0; j < m; ++j) {
        refined_transverse.push_back(
            refine_curve(reparam_transverse[j].get_curve(), u_curve_knots, degree_u_curves));
    }
    for (int i = 0; i < n; ++i) {
        refined_longitudinal.push_back(
            refine_curve(reparam_longitudinal[i].get_curve(), v_curve_knots, degree_v_curves));
    }

    // ------------------------------------------------------------------
    // L_U: transverse curves interpolated at the V stations.
    // ------------------------------------------------------------------
    const int num_u_ctrl = refined_transverse[0].get_num_control_points();
    Vec lu_v_knots;
    int lu_v_degree = 3;
    // First gather the per-row interpolations; all rows share one v knot
    // vector.
    std::vector<Points> v_interpolations(num_u_ctrl);
    for (int q = 0; q < num_u_ctrl; ++q) {
        Points values(m, 3);
        for (int j = 0; j < m; ++j) {
            values.row(j) = refined_transverse[j].get_control_points().row(q);
        }
        Vec knots;
        int degree;
        v_interpolations[q] = natural_interpolate(v_stations, values, knots, degree);
        if (q == 0) {
            lu_v_knots = knots;
            lu_v_degree = degree;
        } else if (knots.size() != lu_v_knots.size() ||
                   (knots - lu_v_knots).cwiseAbs().maxCoeff() > KNOT_EPS ||
                   degree != lu_v_degree) {
            throw invalid_setting_error("Inconsistent v interpolation knot vectors.");
        }
    }
    const int num_lu_v_ctrl = v_interpolations[0].rows();
    Points lu_full_grid(num_u_ctrl * num_lu_v_ctrl, 3);
    for (int q = 0; q < num_u_ctrl; ++q) {
        for (int r = 0; r < num_lu_v_ctrl; ++r) {
            lu_full_grid.row(q * num_lu_v_ctrl + r) = v_interpolations[q].row(r);
        }
    }
    Patch lu = make_patch(lu_full_grid, u_curve_knots, lu_v_knots,
        degree_u_curves, lu_v_degree);

    // ------------------------------------------------------------------
    // L_V: longitudinal curves interpolated at the U stations.
    // ------------------------------------------------------------------
    const int num_v_ctrl = refined_longitudinal[0].get_num_control_points();
    std::vector<Points> u_interpolations(num_v_ctrl);
    Vec lv_u_knots;
    int lv_u_degree = 3;
    for (int q = 0; q < num_v_ctrl; ++q) {
        Points values(n, 3);
        for (int i = 0; i < n; ++i) {
            values.row(i) = refined_longitudinal[i].get_control_points().row(q);
        }
        Vec knots;
        int degree;
        u_interpolations[q] = natural_interpolate(u_stations, values, knots, degree);
        if (q == 0) {
            lv_u_knots = knots;
            lv_u_degree = degree;
        } else if (knots.size() != lv_u_knots.size() ||
                   (knots - lv_u_knots).cwiseAbs().maxCoeff() > KNOT_EPS ||
                   degree != lv_u_degree) {
            throw invalid_setting_error("Inconsistent u interpolation knot vectors.");
        }
    }
    const int num_lv_u_ctrl = u_interpolations[0].rows();
    Points lv_full_grid(num_lv_u_ctrl * num_v_ctrl, 3);
    for (int r = 0; r < num_v_ctrl; ++r) {
        for (int q = 0; q < num_lv_u_ctrl; ++q) {
            lv_full_grid.row(q * num_v_ctrl + r) = u_interpolations[r].row(q);
        }
    }
    Patch lv = make_patch(lv_full_grid, lv_u_knots, v_curve_knots,
        lv_u_degree, degree_v_curves);

    // ------------------------------------------------------------------
    // L_UV: bidirectional interpolation of the averaged intersection net.
    // ------------------------------------------------------------------
    Points averaged(n * m, 3);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            const auto pc = transverse_curves[j].evaluate(a_table[j][i]);
            const auto pd = longitudinal_curves[i].evaluate(b_table[i][j]);
            averaged.row(i * m + j) = 0.5 * (pc + pd);
        }
    }
    // Interpolate along u for every V station, then along v.
    Vec luv_u_knots;
    int luv_u_degree = 3;
    std::vector<Points> column_controls(m);
    for (int j = 0; j < m; ++j) {
        Points column(n, 3);
        for (int i = 0; i < n; ++i) column.row(i) = averaged.row(i * m + j);
        Vec knots;
        int degree;
        column_controls[j] = natural_interpolate(u_stations, column, knots, degree);
        if (j == 0) {
            luv_u_knots = knots;
            luv_u_degree = degree;
        } else if (knots.size() != luv_u_knots.size() ||
                   (knots - luv_u_knots).cwiseAbs().maxCoeff() > KNOT_EPS ||
                   degree != luv_u_degree) {
            throw invalid_setting_error("Inconsistent network u interpolation knots.");
        }
    }
    const int num_luv_u_ctrl = column_controls[0].rows();
    Vec luv_v_knots;
    int luv_v_degree = 3;
    std::vector<Points> row_controls(num_luv_u_ctrl);
    for (int q = 0; q < num_luv_u_ctrl; ++q) {
        Points row_values(m, 3);
        for (int j = 0; j < m; ++j) row_values.row(j) = column_controls[j].row(q);
        Vec knots;
        int degree;
        row_controls[q] = natural_interpolate(v_stations, row_values, knots, degree);
        if (q == 0) {
            luv_v_knots = knots;
            luv_v_degree = degree;
        } else if (knots.size() != luv_v_knots.size() ||
                   (knots - luv_v_knots).cwiseAbs().maxCoeff() > KNOT_EPS ||
                   degree != luv_v_degree) {
            throw invalid_setting_error("Inconsistent network v interpolation knots.");
        }
    }
    const int num_luv_v_ctrl = row_controls[0].rows();
    Points luv_grid(num_luv_u_ctrl * num_luv_v_ctrl, 3);
    for (int q = 0; q < num_luv_u_ctrl; ++q) {
        for (int r = 0; r < num_luv_v_ctrl; ++r) {
            luv_grid.row(q * num_luv_v_ctrl + r) = row_controls[q].row(r);
        }
    }
    Patch luv = make_patch(luv_grid, luv_u_knots, luv_v_knots,
        luv_u_degree, luv_v_degree);

    // ------------------------------------------------------------------
    // Common Gordon knot space and native addition of the three terms.
    // ------------------------------------------------------------------
    int common_degree_u = 1;
    int common_degree_v = 1;
    const Vec common_u_knots = merge_full_knots(
        {{&u_curve_knots, degree_u_curves}, {&lv_u_knots, lv_u_degree},
            {&luv_u_knots, luv_u_degree}},
        common_degree_u);
    const Vec common_v_knots = merge_full_knots(
        {{&lu_v_knots, lu_v_degree}, {&v_curve_knots, degree_v_curves},
            {&luv_v_knots, luv_v_degree}},
        common_degree_v);

    const Patch lu_refined =
        refine_patch(lu, common_u_knots, common_v_knots, common_degree_u, common_degree_v);
    const Patch lv_refined =
        refine_patch(lv, common_u_knots, common_v_knots, common_degree_u, common_degree_v);
    const Patch luv_refined =
        refine_patch(luv, common_u_knots, common_v_knots, common_degree_u, common_degree_v);

    const auto& g1 = lu_refined.get_control_grid();
    const auto& g2 = lv_refined.get_control_grid();
    const auto& g3 = luv_refined.get_control_grid();
    if (g1.rows() != g2.rows() || g1.rows() != g3.rows()) {
        throw invalid_setting_error("Gordon terms could not be put on a common knot space.");
    }
    Points gordon_grid(g1.rows(), 3);
    gordon_grid = g1 + g2 - g3;
    for (int k = 0; k < gordon_grid.rows(); ++k) {
        if (!finite_point(gordon_grid.row(k))) {
            throw invalid_setting_error(
                "Numerical failure: non-finite Gordon control points.");
        }
    }

    Patch patch;
    patch.set_degree_u(common_degree_u);
    patch.set_degree_v(common_degree_v);
    patch.set_control_grid(gordon_grid);
    patch.set_knots_u(common_u_knots);
    patch.set_knots_v(common_v_knots);
    patch.initialize();

    // Commit only after every step succeeded.
    m_transverse_reparams = std::move(transverse_reparams);
    m_longitudinal_reparams = std::move(longitudinal_reparams);
    m_reparam_transverse = std::move(reparam_transverse);
    m_reparam_longitudinal = std::move(reparam_longitudinal);
    m_u_knots = common_u_knots;
    m_v_knots = common_v_knots;
    m_patch = std::move(patch);
}

} // namespace nanospline
