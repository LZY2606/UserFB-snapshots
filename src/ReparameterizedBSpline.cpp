#include <nanospline/ReparameterizedBSpline.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace nanospline {

namespace {

using V3 = Eigen::Matrix<long double, 1, 3>;
using Poly = std::vector<V3>;
using ScalarPoly = std::vector<long double>;

long double binomial_coefficient(int n, int k)
{
    if (k < 0 || k > n) return 0.0L;
    k = std::min(k, n - k);
    long double result = 1.0L;
    for (int i = 1; i <= k; ++i) {
        result *= static_cast<long double>(n - k + i) / static_cast<long double>(i);
    }
    return result;
}

bool finite(double x) { return std::isfinite(x); }

void require(bool condition, CurveNetworkErrorCode code, const std::string& message)
{
    if (!condition) throw CurveNetworkError(code, message);
}

Poly multiply_theta_and_add(const Poly& polynomial, const ScalarPoly& theta, const V3& constant)
{
    const int old_degree = polynomial.empty() ? -1 : static_cast<int>(polynomial.size()) - 1;
    const int theta_degree = static_cast<int>(theta.size()) - 1;
    const int max_degree = polynomial.empty() ? 0 : old_degree + theta_degree;
    Poly result(static_cast<size_t>(max_degree + 1), V3::Zero());

    if (!polynomial.empty()) {
        for (int i = 0; i < static_cast<int>(polynomial.size()); ++i) {
            for (int j = 0; j < static_cast<int>(theta.size()); ++j) {
                result[static_cast<size_t>(i + j)] +=
                    polynomial[static_cast<size_t>(i)] * theta[static_cast<size_t>(j)];
            }
        }
    }
    result[0] += constant;
    return result;
}

Poly compose_polynomial(const std::vector<V3>& source_power, const ScalarPoly& theta)
{
    Poly result;
    for (int i = static_cast<int>(source_power.size()) - 1; i >= 0; --i) {
        result = multiply_theta_and_add(result, theta, source_power[static_cast<size_t>(i)]);
    }
    return result;
}

std::vector<V3> bezier_to_power(const Eigen::MatrixXd& control_points, int degree)
{
    std::vector<V3> power(static_cast<size_t>(degree + 1), V3::Zero());
    for (int j = 0; j <= degree; ++j) {
        for (int k = 0; k <= j; ++k) {
            const long double coefficient = binomial_coefficient(degree, k) *
                                            binomial_coefficient(degree - k, j - k) *
                                            ((j - k) % 2 == 0 ? 1.0L : -1.0L);
            V3 point;
            point << static_cast<long double>(control_points(k, 0)),
                static_cast<long double>(control_points(k, 1)),
                static_cast<long double>(control_points(k, 2));
            power[static_cast<size_t>(j)] += coefficient * point;
        }
    }
    return power;
}

int common_segment(const std::vector<double>& sites, double u)
{
    const int n = static_cast<int>(sites.size());
    if (u <= sites.front()) return 0;
    if (u >= sites.back()) return n - 2;
    int k = static_cast<int>(
        std::upper_bound(sites.begin(), sites.end(), u) - sites.begin()) - 1;
    return std::max(0, std::min(n - 2, k));
}

} // namespace

MonotoneHermiteMap MonotoneHermiteMap::build(
    const std::vector<double>& commonSites, const std::vector<double>& sourceSites)
{
    require(commonSites.size() >= 2 && commonSites.size() <= 6,
        CurveNetworkErrorCode::InvalidStructure,
        "A reparameterization map requires between 2 and 6 sites.");
    require(commonSites.size() == sourceSites.size(),
        CurveNetworkErrorCode::InvalidStructure,
        "Common and source site arrays have different sizes.");

    const int n = static_cast<int>(commonSites.size());
    for (double x : commonSites) {
        require(finite(x), CurveNetworkErrorCode::NonFiniteValue,
            "Non-finite common site in reparameterization map.");
    }
    for (double y : sourceSites) {
        require(finite(y), CurveNetworkErrorCode::NonFiniteValue,
            "Non-finite source site in reparameterization map.");
    }
    require(commonSites.front() == 0.0 && commonSites.back() == 1.0,
        CurveNetworkErrorCode::InvalidSites,
        "Common sites must start at 0 and end at 1.");
    for (int i = 1; i < n; ++i) {
        require(commonSites[i] > commonSites[i - 1],
            CurveNetworkErrorCode::InvalidSites,
            "Common sites must be strictly increasing.");
        require(commonSites[i] - commonSites[i - 1] >= 0.05,
            CurveNetworkErrorCode::InvalidSites,
            "Adjacent common sites must be separated by at least 0.05.");
    }

    bool increasing = true;
    bool decreasing = true;
    for (int i = 1; i < n; ++i) {
        increasing = increasing && sourceSites[i] > sourceSites[i - 1];
        decreasing = decreasing && sourceSites[i] < sourceSites[i - 1];
    }
    require(increasing || decreasing,
        CurveNetworkErrorCode::InvalidParameterTable,
        "Parameter row must be strictly monotone.");

    MonotoneHermiteMap map;
    map.m_common = commonSites;
    map.m_source = sourceSites;
    map.m_orientation = increasing ? 1 : -1;
    map.m_canonicalSource.resize(n);
    for (int i = 0; i < n; ++i) {
        map.m_canonicalSource[i] = map.m_orientation * sourceSites[i];
    }

    std::vector<double> delta(n - 1);
    for (int i = 0; i < n - 1; ++i) {
        const double h = commonSites[i + 1] - commonSites[i];
        delta[i] = (map.m_canonicalSource[i + 1] - map.m_canonicalSource[i]) / h;
        require(delta[i] > 0.0 && std::isfinite(delta[i]),
            CurveNetworkErrorCode::InvalidParameterTable,
            "Secant slope must be finite and positive after orientation normalization.");
    }

    std::vector<double> m(n);
    m[0] = delta[0];
    m[n - 1] = delta[n - 2];
    for (int i = 1; i + 1 < n; ++i) {
        const double dl = delta[i - 1];
        const double dr = delta[i];
        m[i] = 2.0 / (1.0 / dl + 1.0 / dr);
        require(std::isfinite(m[i]) && m[i] > 0.0,
            CurveNetworkErrorCode::NumericalSolveFailure,
            "Harmonic slope computation produced a non-positive/non-finite value.");
    }

    // Fixed left-to-right Fritsch-Carlson style limitation.  Scaling both
    // endpoint slopes of one segment can affect the left slope seen by the
    // next segment, exactly as the prescribed sequential rule implies.
    for (int i = 0; i < n - 1; ++i) {
        const double alpha = m[i] / delta[i];
        const double beta = m[i + 1] / delta[i];
        const double norm = std::sqrt(alpha * alpha + beta * beta);
        if (norm > 9.0) {
            const double factor = 3.0 / norm;
            m[i] *= factor;
            m[i + 1] *= factor;
        }
    }

    map.m_canonicalDerivatives = m;
    map.m_derivatives.resize(n);
    for (int i = 0; i < n; ++i) {
        map.m_derivatives[i] = map.m_orientation * m[i];
    }
    return map;
}

int MonotoneHermiteMap::locateSegment(double u) const
{
    return common_segment(m_common, u);
}

double MonotoneHermiteMap::evaluateCanonical(double u, long double* derivative) const
{
    const int k = locateSegment(u);
    const long double x0 = m_common[k];
    const long double x1 = m_common[k + 1];
    const long double h = x1 - x0;
    const long double t = (static_cast<long double>(u) - x0) / h;
    const long double y0 = m_canonicalSource[k];
    const long double y1 = m_canonicalSource[k + 1];
    const long double m0 = m_canonicalDerivatives[k];
    const long double m1 = m_canonicalDerivatives[k + 1];

    const long double t2 = t * t;
    const long double t3 = t2 * t;
    const long double h00 = 2.0L * t3 - 3.0L * t2 + 1.0L;
    const long double h10 = t3 - 2.0L * t2 + t;
    const long double h01 = -2.0L * t3 + 3.0L * t2;
    const long double h11 = t3 - t2;

    const long double value = h00 * y0 + h10 * h * m0 + h01 * y1 + h11 * h * m1;
    if (derivative != nullptr) {
        const long double d_h00 = 6.0L * t2 - 6.0L * t;
        const long double d_h10 = 3.0L * t2 - 4.0L * t + 1.0L;
        const long double d_h01 = -6.0L * t2 + 6.0L * t;
        const long double d_h11 = 3.0L * t2 - 2.0L * t;
        *derivative = (d_h00 * y0 + d_h10 * h * m0 + d_h01 * y1 + d_h11 * h * m1) / h;
    }
    return static_cast<double>(value);
}

double MonotoneHermiteMap::evaluate(double u) const
{
    if (!valid()) {
        throw CurveNetworkError(CurveNetworkErrorCode::InvalidStructure,
            "Attempted to evaluate an empty reparameterization map.");
    }
    if (u <= m_common.front()) return m_source.front();
    if (u >= m_common.back()) return m_source.back();
    return m_orientation * evaluateCanonical(u, nullptr);
}

double MonotoneHermiteMap::derivative(double u) const
{
    if (!valid()) {
        throw CurveNetworkError(CurveNetworkErrorCode::InvalidStructure,
            "Attempted to differentiate an empty reparameterization map.");
    }
    u = std::max(m_common.front(), std::min(m_common.back(), u));
    long double canonical_derivative = 0.0L;
    evaluateCanonical(u, &canonical_derivative);
    return m_orientation * static_cast<double>(canonical_derivative);
}

double MonotoneHermiteMap::inverse(double sourceParameter) const
{
    if (!valid()) {
        throw CurveNetworkError(CurveNetworkErrorCode::InvalidStructure,
            "Attempted inverse on an empty reparameterization map.");
    }
    const double target = m_orientation * sourceParameter;
    for (int i = 0; i < static_cast<int>(m_common.size()); ++i) {
        if (target == m_canonicalSource[i]) return m_common[i];
    }
    if (target <= m_canonicalSource.front()) return m_common.front();
    if (target >= m_canonicalSource.back()) return m_common.back();

    int k = 0;
    while (k + 2 < static_cast<int>(m_canonicalSource.size()) &&
           target > m_canonicalSource[k + 1]) {
        ++k;
    }
    double lo = m_common[k];
    double hi = m_common[k + 1];
    for (int iteration = 0; iteration < 120; ++iteration) {
        const double mid = (lo + hi) / 2.0;
        const double value = evaluateCanonical(mid, nullptr);
        if (value < target) lo = mid;
        else hi = mid;
        if (hi - lo <= 8.0 * std::numeric_limits<double>::epsilon()) break;
    }
    const double result = (lo + hi) / 2.0;
    const double residual = std::abs(evaluateCanonical(result, nullptr) - target);
    const double scale = std::max(1.0,
        std::abs(m_canonicalSource.back() - m_canonicalSource.front()));
    if (!std::isfinite(result) || residual > 1e-9 * scale) {
        std::ostringstream out;
        out << "Monotone map inverse solve failed at source parameter "
            << sourceParameter << " (residual=" << residual << ").";
        throw CurveNetworkError(CurveNetworkErrorCode::NumericalSolveFailure, out.str());
    }
    return result;
}

ReparameterizedBSpline::ReparameterizedBSpline(const CurveType& source,
    const std::vector<double>& commonSites,
    const std::vector<double>& sourceSites)
{
    m_map = MonotoneHermiteMap::build(commonSites, sourceSites);

    const int p = source.get_degree();
    require(p >= 1 && p <= 3, CurveNetworkErrorCode::InvalidCurve,
        "ReparameterizedBSpline requires a dynamic-degree BSpline of degree 1, 2, or 3.");
    require(source.get_num_control_points() >= p + 1 && source.get_num_knots() > 0,
        CurveNetworkErrorCode::InvalidCurve,
        "Source curve has no valid control net or knot vector.");

    std::vector<Bezier<double, 3, -1>> bezierPieces;
    std::vector<double> bounds;
    std::tie(bezierPieces, bounds) = source.convert_to_Bezier();
    require(!bezierPieces.empty() && bounds.size() == bezierPieces.size() + 1,
        CurveNetworkErrorCode::InvalidCurve,
        "Source curve could not be decomposed into polynomial pieces.");

    std::vector<std::vector<V3>> sourcePower;
    sourcePower.reserve(bezierPieces.size());
    for (const auto& piece : bezierPieces) {
        require(piece.get_degree() == p, CurveNetworkErrorCode::InvalidCurve,
            "Inconsistent polynomial degree while decomposing source curve.");
        sourcePower.push_back(bezier_to_power(piece.get_control_points(), p));
    }

    std::vector<double> breaks = commonSites;
    for (size_t k = 1; k + 1 < bounds.size(); ++k) {
        breaks.push_back(m_map.inverse(bounds[k]));
    }
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());
    require(breaks.size() >= 2 && breaks.front() == 0.0 && breaks.back() == 1.0,
        CurveNetworkErrorCode::NumericalSolveFailure,
        "Reparameterization breakpoints do not cover the common [0,1] domain.");
    for (size_t k = 1; k < breaks.size(); ++k) {
        require(breaks[k] > breaks[k - 1] && std::isfinite(breaks[k]),
            CurveNetworkErrorCode::NumericalSolveFailure,
            "Reparameterization produced a repeated or non-finite breakpoint.");
    }

    const int D = 3 * p;
    const int pieceCount = static_cast<int>(breaks.size()) - 1;
    CurveType::ControlPoints controlPoints(pieceCount * D + 1, 3);

    for (int piece = 0; piece < pieceCount; ++piece) {
        const double L = breaks[static_cast<size_t>(piece)];
        const double R = breaks[static_cast<size_t>(piece + 1)];
        const double W = R - L;
        require(W > 0.0, CurveNetworkErrorCode::NumericalSolveFailure,
            "Zero-width composition piece.");

        const int mk = common_segment(m_map.commonSites(), L);
        const double x0 = m_map.commonSites()[static_cast<size_t>(mk)];
        const double x1 = m_map.commonSites()[static_cast<size_t>(mk + 1)];
        const double h = x1 - x0;
        const double y0 = m_map.sourceSites()[static_cast<size_t>(mk)];
        const double y1 = m_map.sourceSites()[static_cast<size_t>(mk + 1)];
        const double m0 = m_map.derivatives()[static_cast<size_t>(mk)];
        const double m1 = m_map.derivatives()[static_cast<size_t>(mk + 1)];

        const double a0 = y0;
        const double a1 = m0;
        const double a2 = (3.0 * (y1 - y0) - h * (2.0 * m0 + m1)) / (h * h);
        const double a3 = (2.0 * (y0 - y1) + h * (m0 + m1)) / (h * h * h);
        const double d = L - x0;

        // Shift the cubic map from powers of (u-x0) to powers of w=u-L.
        const long double q0 = static_cast<long double>(a0 + d * (a1 + d * (a2 + d * a3)));
        const long double q1 = static_cast<long double>(a1 + d * (2.0 * a2 + 3.0 * d * a3));
        const long double q2 = static_cast<long double>(a2 + 3.0 * d * a3);
        const long double q3 = static_cast<long double>(a3);

        const double middleParameter = m_map.evaluate((L + R) / 2.0);
        int si = static_cast<int>(std::upper_bound(
            bounds.begin(), bounds.end(), middleParameter) - bounds.begin()) - 1;
        si = std::max(0, std::min(static_cast<int>(bezierPieces.size()) - 1, si));
        const double s0 = bounds[static_cast<size_t>(si)];
        const double s1 = bounds[static_cast<size_t>(si + 1)];
        const double sourceWidth = s1 - s0;
        require(sourceWidth > 0.0 && std::isfinite(sourceWidth),
            CurveNetworkErrorCode::InvalidCurve,
            "Source curve has a zero-width polynomial interval.");

        ScalarPoly theta(4);
        theta[0] = static_cast<long double>((q0 - s0) / sourceWidth);
        theta[1] = static_cast<long double>(q1 / sourceWidth);
        theta[2] = static_cast<long double>(q2 / sourceWidth);
        theta[3] = static_cast<long double>(q3 / sourceWidth);

        Poly composed = compose_polynomial(sourcePower[static_cast<size_t>(si)], theta);
        require(static_cast<int>(composed.size()) == D + 1,
            CurveNetworkErrorCode::NumericalSolveFailure,
            "Polynomial composition did not reach the expected dynamic degree.");

        std::vector<V3> bezierControls(D + 1, V3::Zero());
        for (int k = 0; k <= D; ++k) {
            for (int j = 0; j <= k; ++j) {
                const V3 monomial = composed[static_cast<size_t>(j)] *
                                    std::pow(static_cast<long double>(W), j);
                const long double conversion =
                    binomial_coefficient(k, j) / binomial_coefficient(D, j);
                bezierControls[static_cast<size_t>(k)] += conversion * monomial;
            }
        }

        for (int r = 0; r <= D; ++r) {
            controlPoints.row(piece * D + r) <<
                static_cast<double>(bezierControls[static_cast<size_t>(r)].x()),
                static_cast<double>(bezierControls[static_cast<size_t>(r)].y()),
                static_cast<double>(bezierControls[static_cast<size_t>(r)].z());
        }
    }

    // Use one shared endpoint value at every repeated Bézier join.  This keeps
    // the stored tensor grid and curve control net unambiguous.
    for (int k = 0; k <= pieceCount; ++k) {
        const double u = breaks[static_cast<size_t>(k)];
        const auto point = source.evaluate(m_map.evaluate(u));
        controlPoints.row(k * D) = point;
    }

    const int numKnots = pieceCount * D + D + 2;
    Eigen::VectorXd knots(numKnots);
    knots.segment(0, D + 1).setConstant(breaks.front());
    for (int k = 1; k < pieceCount; ++k) {
        knots.segment(D + 1 + (k - 1) * D, D).setConstant(breaks[static_cast<size_t>(k)]);
    }
    knots.segment(numKnots - D - 1, D + 1).setConstant(breaks.back());

    m_curve.set_control_points(controlPoints);
    m_curve.set_knots(knots);
    m_curve.set_periodic(false);
}

} // namespace nanospline
