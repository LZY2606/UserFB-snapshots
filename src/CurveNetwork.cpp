#include <nanospline/CurveNetwork.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include <Eigen/Dense>

namespace nanospline {

namespace {

using Curve3 = BSpline<double, 3, -1>;
using Patch3 = BSplinePatch<double, 3, -1, -1>;
using Bezier3 = Bezier<double, 3, -1>;
using Point3 = Eigen::Matrix<double, 1, 3>;
using Grid = Eigen::MatrixXd;

struct ValidatedIntersections {
    // x[i][j] is the averaged intersection at (U_i, V_j).
    std::vector<std::vector<Point3>> x;
};

[[noreturn]] void fail(CurveNetworkErrorCode code, const std::string& message)
{
    throw CurveNetworkError(code, message);
}

bool finite_double(double x) { return std::isfinite(x); }

bool finite_matrix(const Eigen::MatrixXd& m)
{
    for (int i = 0; i < m.rows(); ++i) {
        for (int j = 0; j < m.cols(); ++j) {
            if (!std::isfinite(m(i, j))) return false;
        }
    }
    return true;
}

double endpoint_tolerance(double value)
{
    return 1e-12 * std::max(1.0, std::abs(value));
}

void require_site_vector(const std::vector<double>& sites, size_t count, const char* name)
{
    if (sites.size() != count) {
        fail(CurveNetworkErrorCode::InvalidSites,
            std::string(name) + " size does not match the corresponding curve family.");
    }
    if (sites.size() < 2 || sites.size() > 6) {
        fail(CurveNetworkErrorCode::InvalidStructure,
            "Each curve family and site vector must contain between 2 and 6 entries.");
    }
    for (double x : sites) {
        if (!finite_double(x)) {
            fail(CurveNetworkErrorCode::NonFiniteValue,
                std::string(name) + " contains a non-finite value.");
        }
    }
    if (sites.front() != 0.0 || sites.back() != 1.0) {
        fail(CurveNetworkErrorCode::InvalidSites,
            std::string(name) + " must have first site 0 and last site 1.");
    }
    for (size_t i = 1; i < sites.size(); ++i) {
        const double gap = sites[i] - sites[i - 1];
        if (!(gap > 0.0)) {
            fail(CurveNetworkErrorCode::InvalidSites,
                std::string(name) + " contains duplicate or non-increasing sites.");
        }
        // A small floating-point tolerance keeps a nominal 0.05 spacing valid
        // when sites were produced by different arithmetic expressions.
        if (gap < 0.05 - 1e-12) {
            fail(CurveNetworkErrorCode::InvalidSites,
                std::string(name) + " adjacent spacing is smaller than 0.05.");
        }
    }
}

void check_curve(const Curve3& curve)
{
    const int degree = curve.get_degree();
    if (degree < 1 || degree > 3) {
        fail(CurveNetworkErrorCode::InvalidCurve,
            "Only dynamic-degree BSpline curves of degree 1 through 3 are supported.");
    }
    const int numControlPoints = curve.get_num_control_points();
    const int numKnots = curve.get_num_knots();
    if (numControlPoints < degree + 1 || numKnots != numControlPoints + degree + 1) {
        fail(CurveNetworkErrorCode::InvalidCurve,
            "BSpline control-point and knot-vector sizes are inconsistent.");
    }
    const auto& controlPoints = curve.get_control_points();
    if (controlPoints.cols() != 3 || !finite_matrix(controlPoints)) {
        fail(CurveNetworkErrorCode::NonFiniteValue,
            "Curve control net contains a non-finite value or wrong dimension.");
    }
    const auto& knots = curve.get_knots();
    if (knots.size() != numKnots || !finite_matrix(knots)) {
        fail(CurveNetworkErrorCode::NonFiniteValue,
            "Curve knot vector contains a non-finite value.");
    }
    for (int i = 1; i < numKnots; ++i) {
        if (knots[i] < knots[i - 1]) {
            fail(CurveNetworkErrorCode::InvalidCurve, "Curve knot vector is not non-decreasing.");
        }
    }
    if (curve.get_periodic()) {
        fail(CurveNetworkErrorCode::InvalidCurve, "Curve must be non-periodic.");
    }

    const double lower = curve.get_domain_lower_bound();
    const double upper = curve.get_domain_upper_bound();
    if (!finite_double(lower) || !finite_double(upper) || !(upper > lower)) {
        fail(CurveNetworkErrorCode::InvalidCurve, "Curve has an invalid parameter domain.");
    }

    for (int i = 0; i <= degree; ++i) {
        if (std::abs(knots[i] - lower) > endpoint_tolerance(lower)) {
            fail(CurveNetworkErrorCode::InvalidCurve, "Curve is not clamped at its lower end.");
        }
        const int upperIndex = numKnots - 1 - i;
        if (std::abs(knots[upperIndex] - upper) > endpoint_tolerance(upper)) {
            fail(CurveNetworkErrorCode::InvalidCurve, "Curve is not clamped at its upper end.");
        }
    }

    // Verify C1 geometrically.  This accepts a structurally repeated knot if
    // the adjacent polynomial pieces nevertheless have the same derivative.
    std::vector<Bezier3> pieces;
    std::vector<double> bounds;
    std::tie(pieces, bounds) = curve.convert_to_Bezier();
    if (pieces.empty() || bounds.size() != pieces.size() + 1) {
        fail(CurveNetworkErrorCode::InvalidCurve, "Curve Bezier decomposition failed.");
    }
    for (size_t k = 1; k + 1 < bounds.size(); ++k) {
        const double leftWidth = bounds[k] - bounds[k - 1];
        const double rightWidth = bounds[k + 1] - bounds[k];
        if (!(leftWidth > 0.0) || !(rightWidth > 0.0)) {
            fail(CurveNetworkErrorCode::InvalidCurve, "Curve has a repeated knot interval.");
        }
        const Point3 dLeft = pieces[k - 1].evaluate_derivative(1.0) / leftWidth;
        const Point3 dRight = pieces[k].evaluate_derivative(0.0) / rightWidth;
        if (!dLeft.allFinite() || !dRight.allFinite()) {
            fail(CurveNetworkErrorCode::InvalidCurve,
                "Non-finite one-sided derivative while checking C1 continuity.");
        }
        const double difference = (dLeft - dRight).norm();
        const double scale = std::max(1.0, std::max(dLeft.norm(), dRight.norm()));
        if (difference > 1e-8 * scale + 1e-9) {
            fail(CurveNetworkErrorCode::InvalidCurve,
                "Input curve is not C1 at an internal knot.");
        }
    }
}

bool row_is_monotone_endpoints(const std::vector<double>& row,
    const Curve3& curve,
    size_t expectedSize,
    const char* tableName,
    size_t rowIndex)
{
    if (row.size() != expectedSize) {
        std::ostringstream out;
        out << tableName << " row " << rowIndex << " has the wrong number of entries.";
        fail(CurveNetworkErrorCode::InvalidStructure, out.str());
    }
    for (double x : row) {
        if (!finite_double(x)) {
            fail(CurveNetworkErrorCode::NonFiniteValue,
                "Parameter table contains a non-finite value.");
        }
    }

    bool increasing = true;
    bool decreasing = true;
    for (size_t i = 1; i < row.size(); ++i) {
        increasing = increasing && row[i] > row[i - 1];
        decreasing = decreasing && row[i] < row[i - 1];
    }
    if (!increasing && !decreasing) {
        std::ostringstream out;
        out << tableName << " row " << rowIndex << " is not strictly monotone.";
        fail(CurveNetworkErrorCode::InvalidParameterTable, out.str());
    }

    const double lower = curve.get_domain_lower_bound();
    const double upper = curve.get_domain_upper_bound();
    const double firstExpected = increasing ? lower : upper;
    const double lastExpected = increasing ? upper : lower;
    if (std::abs(row.front() - firstExpected) > endpoint_tolerance(firstExpected) ||
        std::abs(row.back() - lastExpected) > endpoint_tolerance(lastExpected)) {
        std::ostringstream out;
        out << tableName << " row " << rowIndex
            << " must start and end at the curve domain endpoints.";
        fail(CurveNetworkErrorCode::InvalidParameterTable, out.str());
    }
    const double domainTolerance =
        1e-12 * std::max(1.0, std::max(std::abs(lower), std::abs(upper)));
    for (double x : row) {
        if (x < lower - domainTolerance || x > upper + domainTolerance) {
            fail(CurveNetworkErrorCode::InvalidParameterTable,
                "Parameter table entry lies outside its curve domain.");
        }
    }
    return increasing;
}

ValidatedIntersections validate_specification(const CurveNetworkSpecification& spec)
{
    const size_t m = spec.horizontalCurves.size(); // Number of V sites / C curves.
    const size_t n = spec.verticalCurves.size();   // Number of U sites / D curves.
    if (m < 2 || m > 6 || n < 2 || n > 6) {
        fail(CurveNetworkErrorCode::InvalidStructure,
            "Each curve family must contain between 2 and 6 curves.");
    }
    require_site_vector(spec.vSites, m, "vSites");
    require_site_vector(spec.uSites, n, "uSites");

    if (spec.a.size() != m) {
        fail(CurveNetworkErrorCode::InvalidStructure, "Table a must have one row per C curve.");
    }
    if (spec.b.size() != n) {
        fail(CurveNetworkErrorCode::InvalidStructure, "Table b must have one row per D curve.");
    }

    for (const auto& curve : spec.horizontalCurves) check_curve(curve);
    for (const auto& curve : spec.verticalCurves) check_curve(curve);

    for (size_t j = 0; j < m; ++j) {
        row_is_monotone_endpoints(
            spec.a[j], spec.horizontalCurves[j], n, "a", j);
    }
    for (size_t i = 0; i < n; ++i) {
        row_is_monotone_endpoints(
            spec.b[i], spec.verticalCurves[i], m, "b", i);
    }

    ValidatedIntersections result;
    result.x.assign(n, std::vector<Point3>(m));
    for (size_t j = 0; j < m; ++j) {
        for (size_t i = 0; i < n; ++i) {
            const Point3 pc = spec.horizontalCurves[j].evaluate(spec.a[j][i]);
            const Point3 pd = spec.verticalCurves[i].evaluate(spec.b[i][j]);
            if (!pc.allFinite() || !pd.allFinite()) {
                fail(CurveNetworkErrorCode::NonFiniteValue,
                    "Curve evaluation produced a non-finite intersection point.");
            }
            const double error = (pc - pd).norm();
            if (error > 1e-10) {
                std::ostringstream out;
                out << "Incompatible curve-network intersection at (i=" << i << ", j=" << j
                    << "): spatial error " << error << " exceeds 1e-10.";
                fail(CurveNetworkErrorCode::IncompatibleIntersections, out.str());
            }
            result.x[i][j] = 0.5 * (pc + pd);
        }
    }
    return result;
}

Eigen::VectorXd bezier_knot_vector(const std::vector<double>& breaks, int degree)
{
    const int pieceCount = static_cast<int>(breaks.size()) - 1;
    const int numKnots = pieceCount * degree + degree + 2;
    Eigen::VectorXd knots(numKnots);
    knots.segment(0, degree + 1).setConstant(breaks.front());
    for (int k = 1; k < pieceCount; ++k) {
        knots.segment(degree + 1 + (k - 1) * degree, degree)
            .setConstant(breaks[static_cast<size_t>(k)]);
    }
    knots.segment(numKnots - degree - 1, degree + 1).setConstant(breaks.back());
    return knots;
}

Point3 evaluate_bezier_sequence(const std::vector<Bezier3>& pieces,
    const std::vector<double>& bounds,
    double x)
{
    int k = static_cast<int>(
        std::upper_bound(bounds.begin(), bounds.end(), x) - bounds.begin()) - 1;
    k = std::max(0, std::min(static_cast<int>(pieces.size()) - 1, k));
    const double width = bounds[static_cast<size_t>(k + 1)] - bounds[static_cast<size_t>(k)];
    double t = (x - bounds[static_cast<size_t>(k)]) / width;
    t = std::max(0.0, std::min(1.0, t));
    return pieces[static_cast<size_t>(k)].evaluate(t);
}

Grid refine_bezier_sequence(const std::vector<Bezier3>& pieces,
    const std::vector<double>& bounds,
    const std::vector<double>& commonBreaks,
    int commonDegree)
{
    if (pieces.empty() || bounds.size() != pieces.size() + 1) {
        fail(CurveNetworkErrorCode::NumericalSolveFailure,
            "Cannot refine an empty Bezier sequence.");
    }
    const int intervals = static_cast<int>(commonBreaks.size()) - 1;
    const int numControlPoints = intervals * commonDegree + 1;
    Grid controls(numControlPoints, 3);

    int sourceIndex = 0;
    for (int k = 0; k < intervals; ++k) {
        const double a = commonBreaks[static_cast<size_t>(k)];
        const double b = commonBreaks[static_cast<size_t>(k + 1)];
        const double middle = (a + b) / 2.0;
        sourceIndex = static_cast<int>(
            std::upper_bound(bounds.begin(), bounds.end(), middle) - bounds.begin()) - 1;
        sourceIndex = std::max(0, std::min(static_cast<int>(pieces.size()) - 1, sourceIndex));

        const double lo = bounds[static_cast<size_t>(sourceIndex)];
        const double hi = bounds[static_cast<size_t>(sourceIndex + 1)];
        const double width = hi - lo;
        double t0 = (a - lo) / width;
        double t1 = (b - lo) / width;
        t0 = std::max(0.0, std::min(1.0, t0));
        t1 = std::max(0.0, std::min(1.0, t1));
        if (t1 <= t0) {
            // Extremely close breakpoint: de Casteljau still handles equality,
            // but a zero output interval has no polynomial content.
            t1 = std::min(1.0, t0 + 1e-15);
        }

        Bezier3 sub = pieces[static_cast<size_t>(sourceIndex)].subcurve(t0, t1);
        while (sub.get_degree() >= 0 && sub.get_degree() < commonDegree) {
            sub = sub.elevate_degree();
        }
        if (sub.get_degree() != commonDegree) {
            fail(CurveNetworkErrorCode::NumericalSolveFailure,
                "Failed to elevate a Bezier piece to the common tensor degree.");
        }
        const auto& block = sub.get_control_points();
        if (block.rows() != commonDegree + 1) {
            fail(CurveNetworkErrorCode::NumericalSolveFailure,
                "Refined Bezier piece has the wrong number of control points.");
        }
        controls.block(k * commonDegree, 0, commonDegree + 1, 3) = block;
    }

    for (int k = 0; k <= intervals; ++k) {
        controls.row(k * commonDegree) =
            evaluate_bezier_sequence(pieces, bounds, commonBreaks[static_cast<size_t>(k)]);
    }
    if (!finite_matrix(controls)) {
        fail(CurveNetworkErrorCode::NumericalSolveFailure,
            "Non-finite controls while refining a Bezier sequence.");
    }
    return controls;
}

Grid common_curve_controls(const Curve3& curve,
    const std::vector<double>& commonBreaks,
    int commonDegree)
{
    std::vector<Bezier3> pieces;
    std::vector<double> bounds;
    std::tie(pieces, bounds) = curve.convert_to_Bezier();
    return refine_bezier_sequence(pieces, bounds, commonBreaks, commonDegree);
}

// Natural cubic endpoint-second-derivative-zero interpolation.  For two sites
// the result is explicitly the linear segment and is elevated/refined only by
// the common-space conversion.
Grid natural_interpolation_controls(const std::vector<double>& sites,
    const Grid& values,
    const std::vector<double>& commonBreaks,
    int commonDegree)
{
    const int n = static_cast<int>(sites.size());
    if (values.rows() != n || values.cols() != 3) {
        fail(CurveNetworkErrorCode::InvalidStructure,
            "Natural interpolation values do not match sites.");
    }

    std::vector<Bezier3> pieces;
    std::vector<double> bounds = sites;

    if (n == 2) {
        Bezier3 linear;
        Grid cp(2, 3);
        cp.row(0) = values.row(0);
        cp.row(1) = values.row(1);
        linear.set_control_points(cp);
        pieces.push_back(std::move(linear));
        return refine_bezier_sequence(pieces, bounds, commonBreaks, commonDegree);
    }

    Grid delta(n - 1, 3);
    std::vector<double> h(n - 1);
    for (int i = 0; i < n - 1; ++i) {
        h[i] = sites[i + 1] - sites[i];
        delta.row(i) = (values.row(i + 1) - values.row(i)) / h[i];
    }

    Eigen::MatrixXd system = Eigen::MatrixXd::Zero(n, n);
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(n, 3);

    system(0, 0) = 2.0;
    system(0, 1) = 1.0;
    rhs.row(0) = 3.0 * delta.row(0);

    for (int i = 1; i + 1 < n; ++i) {
        system(i, i - 1) = h[i];
        system(i, i) = 2.0 * (h[i - 1] + h[i]);
        system(i, i + 1) = h[i - 1];
        rhs.row(i) = 3.0 * (h[i] * delta.row(i - 1) + h[i - 1] * delta.row(i));
    }

    system(n - 1, n - 2) = 1.0;
    system(n - 1, n - 1) = 2.0;
    rhs.row(n - 1) = 3.0 * delta.row(n - 2);

    Eigen::FullPivLU<Eigen::MatrixXd> lu(system);
    if (!lu.isInvertible()) {
        fail(CurveNetworkErrorCode::NumericalSolveFailure,
            "Natural cubic slope system is singular.");
    }
    Grid slopes = lu.solve(rhs);
    if (!finite_matrix(slopes)) {
        fail(CurveNetworkErrorCode::NumericalSolveFailure,
            "Natural cubic slope solve returned non-finite values.");
    }
    const double residual = (system * slopes - rhs).norm();
    const double rhsNorm = std::max(1.0, rhs.norm());
    if (residual > 1e-10 * rhsNorm) {
        std::ostringstream out;
        out << "Natural cubic slope solve residual is too large: " << residual;
        fail(CurveNetworkErrorCode::NumericalSolveFailure, out.str());
    }

    for (int i = 0; i < n - 1; ++i) {
        Bezier3 cubic;
        Grid cp(4, 3);
        cp.row(0) = values.row(i);
        cp.row(1) = values.row(i) + (h[i] / 3.0) * slopes.row(i);
        cp.row(2) = values.row(i + 1) - (h[i] / 3.0) * slopes.row(i + 1);
        cp.row(3) = values.row(i + 1);
        cubic.set_control_points(cp);
        pieces.push_back(std::move(cubic));
    }
    return refine_bezier_sequence(pieces, bounds, commonBreaks, commonDegree);
}

std::vector<double> collect_breaks(const std::vector<double>& sites,
    const std::vector<ReparameterizedBSpline>& curves)
{
    std::vector<double> breaks = sites;
    breaks.push_back(0.0);
    breaks.push_back(1.0);
    for (const auto& reparam : curves) {
        const auto& knots = reparam.curve().get_knots();
        const int degree = reparam.curve().get_degree();
        const int lowerIndex = degree;
        const int upperIndex = knots.size() - degree - 1;
        for (int i = lowerIndex; i <= upperIndex; ++i) {
            const double x = knots[i];
            if (x > 0.0 && x < 1.0) breaks.push_back(x);
        }
    }
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());
    if (breaks.front() != 0.0) breaks.insert(breaks.begin(), 0.0);
    if (breaks.back() != 1.0) breaks.push_back(1.0);
    return breaks;
}

int linear_grid_index(int i, int j, int numV) { return i * numV + j; }

} // namespace

bool CurveNetwork::build(const CurveNetworkSpecification& specification, std::string* error)
{
    try {
        const ValidatedIntersections intersections = validate_specification(specification);

        const size_t m = specification.horizontalCurves.size();
        const size_t n = specification.verticalCurves.size();

        std::vector<ReparameterizedBSpline> horizontal;
        std::vector<ReparameterizedBSpline> vertical;
        horizontal.reserve(m);
        vertical.reserve(n);

        // a[j] maps common U_i to C_j's own parameter.
        for (size_t j = 0; j < m; ++j) {
            horizontal.emplace_back(specification.horizontalCurves[j],
                specification.uSites,
                specification.a[j]);
        }
        // b[i] maps common V_j to D_i's own parameter.
        for (size_t i = 0; i < n; ++i) {
            vertical.emplace_back(specification.verticalCurves[i],
                specification.vSites,
                specification.b[i]);
        }

        int degreeU = 1;
        int degreeV = 1;
        for (const auto& c : horizontal) degreeU = std::max(degreeU, c.curve().get_degree());
        for (const auto& c : vertical) degreeV = std::max(degreeV, c.curve().get_degree());
        // Natural interpolation is cubic unless an axis has only two sites.
        degreeU = std::max(degreeU, n == 2 ? 1 : 3);
        degreeV = std::max(degreeV, m == 2 ? 1 : 3);

        std::vector<double> breaksU = collect_breaks(specification.uSites, horizontal);
        std::vector<double> breaksV = collect_breaks(specification.vSites, vertical);

        const int numU = static_cast<int>(breaksU.size()) - 1;
        const int numV = static_cast<int>(breaksV.size()) - 1;
        const int controlU = numU * degreeU + 1;
        const int controlV = numV * degreeV + 1;

        std::vector<Grid> hControls(m);
        for (size_t j = 0; j < m; ++j) {
            hControls[j] = common_curve_controls(horizontal[j].curve(), breaksU, degreeU);
            if (hControls[j].rows() != controlU) {
                fail(CurveNetworkErrorCode::NumericalSolveFailure,
                    "Horizontal common-space control count mismatch.");
            }
        }
        std::vector<Grid> dControls(n);
        for (size_t i = 0; i < n; ++i) {
            dControls[i] = common_curve_controls(vertical[i].curve(), breaksV, degreeV);
            if (dControls[i].rows() != controlV) {
                fail(CurveNetworkErrorCode::NumericalSolveFailure,
                    "Vertical common-space control count mismatch.");
            }
        }

        Grid lu = Grid::Zero(controlU * controlV, 3);
        for (int r = 0; r < controlU; ++r) {
            Grid values(static_cast<int>(m), 3);
            for (size_t j = 0; j < m; ++j) values.row(static_cast<int>(j)) = hControls[j].row(r);
            Grid vControls = natural_interpolation_controls(
                specification.vSites, values, breaksV, degreeV);
            for (int s = 0; s < controlV; ++s) {
                lu.row(linear_grid_index(r, s, controlV)) = vControls.row(s);
            }
        }

        Grid lv = Grid::Zero(controlU * controlV, 3);
        for (int s = 0; s < controlV; ++s) {
            Grid values(static_cast<int>(n), 3);
            for (size_t i = 0; i < n; ++i) values.row(static_cast<int>(i)) = dControls[i].row(s);
            Grid uControls = natural_interpolation_controls(
                specification.uSites, values, breaksU, degreeU);
            for (int r = 0; r < controlU; ++r) {
                lv.row(linear_grid_index(r, s, controlV)) = uControls.row(r);
            }
        }

        // First interpolate the intersection net in U for every V site, then
        // interpolate those common-U coefficients in V.  This is the tensor
        // product natural cubic operator L_UV.
        std::vector<Grid> uvAfterU(m);
        for (size_t j = 0; j < m; ++j) {
            Grid values(static_cast<int>(n), 3);
            for (size_t i = 0; i < n; ++i) {
                values.row(static_cast<int>(i)) = intersections.x[i][j];
            }
            uvAfterU[j] = natural_interpolation_controls(
                specification.uSites, values, breaksU, degreeU);
        }

        Grid luv = Grid::Zero(controlU * controlV, 3);
        for (int r = 0; r < controlU; ++r) {
            Grid values(static_cast<int>(m), 3);
            for (size_t j = 0; j < m; ++j) {
                values.row(static_cast<int>(j)) = uvAfterU[j].row(r);
            }
            Grid vControls = natural_interpolation_controls(
                specification.vSites, values, breaksV, degreeV);
            for (int s = 0; s < controlV; ++s) {
                luv.row(linear_grid_index(r, s, controlV)) = vControls.row(s);
            }
        }

        Grid networkGrid = lu + lv - luv;
        if (!finite_matrix(networkGrid)) {
            fail(CurveNetworkErrorCode::NumericalSolveFailure,
                "Gordon combination produced a non-finite control grid.");
        }

        Patch3 candidate;
        candidate.set_degree_u(degreeU);
        candidate.set_degree_v(degreeV);
        candidate.set_control_grid(networkGrid);
        Eigen::VectorXd knotsU = bezier_knot_vector(breaksU, degreeU);
        Eigen::VectorXd knotsV = bezier_knot_vector(breaksV, degreeV);
        candidate.set_knots_u(knotsU);
        candidate.set_knots_v(knotsV);
        candidate.set_periodic_u(false);
        candidate.set_periodic_v(false);
        candidate.initialize();

        // A few direct finite-domain evaluations catch malformed knot/index
        // spaces before the result is published.
        const Point3 sanity = candidate.evaluate(0.5, 0.5);
        if (!sanity.allFinite()) {
            fail(CurveNetworkErrorCode::NumericalSolveFailure,
                "Constructed curve network does not evaluate finitely.");
        }

        // Publish only after every assembly step has succeeded.  This is the
        // exception/transaction boundary that protects an existing surface.
        m_surface = std::move(candidate);
        m_horizontal = std::move(horizontal);
        m_vertical = std::move(vertical);
        m_hasSurface = true;
        m_lastErrorCode = CurveNetworkErrorCode::None;
        m_lastError.clear();
        if (error != nullptr) error->clear();
        return true;
    } catch (const CurveNetworkError& e) {
        m_lastErrorCode = e.code();
        m_lastError = e.what();
        if (error != nullptr) *error = e.what();
        return false;
    } catch (const std::exception& e) {
        m_lastErrorCode = CurveNetworkErrorCode::NumericalSolveFailure;
        m_lastError = std::string("Unexpected numerical/assembly failure: ") + e.what();
        if (error != nullptr) *error = m_lastError;
        return false;
    }
}

bool buildCurveNetwork(const CurveNetworkSpecification& specification,
    BSplinePatch<double, 3, -1, -1>& output,
    std::string* error)
{
    CurveNetwork builder;
    if (!builder.build(specification, error)) return false;
    output = builder.releaseSurface();
    return true;
}

BSplinePatch<double, 3, -1, -1> createCurveNetwork(
    const CurveNetworkSpecification& specification)
{
    CurveNetwork builder;
    std::string error;
    if (!builder.build(specification, &error)) {
        throw CurveNetworkError(builder.lastErrorCode(), error);
    }
    return builder.releaseSurface();
}

} // namespace nanospline
