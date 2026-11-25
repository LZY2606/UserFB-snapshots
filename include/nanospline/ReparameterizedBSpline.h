#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <Eigen/Core>

#include <nanospline/BSpline.h>

namespace nanospline {

/**
 * @brief Error category used by the curve-network and reparameterization code.
 *
 * The code uses error codes so that callers can distinguish malformed input
 * from a failure of the small numerical solves used for inverse maps and
 * natural splines.
 */
enum class CurveNetworkErrorCode {
    None = 0,
    InvalidStructure,
    NonFiniteValue,
    InvalidSites,
    InvalidParameterTable,
    InvalidCurve,
    IncompatibleIntersections,
    NumericalSolveFailure
};

class CurveNetworkError : public std::runtime_error {
public:
    CurveNetworkError(CurveNetworkErrorCode code, const std::string& message)
        : std::runtime_error(message), m_code(code)
    {
    }

    CurveNetworkErrorCode code() const noexcept { return m_code; }

private:
    CurveNetworkErrorCode m_code;
};

/**
 * @brief The fixed C1 monotone cubic Hermite map used by CurveNetwork.
 *
 * For an increasing parameter row, orientation is +1 and sourceValues are the
 * interpolation values.  For a decreasing row, the values are negated while
 * slopes are prepared, then the sign is restored.  The stored values and
 * derivatives are always those of the final map from common sites to the
 * curve's own parameter.
 */
class MonotoneHermiteMap {
public:
    MonotoneHermiteMap() = default;

    /**
     * @brief Construct the map using the slope and Fritsch-Carlson limiting
     * rule required by the curve-network entry point.
     */
    static MonotoneHermiteMap build(const std::vector<double>& commonSites,
        const std::vector<double>& sourceSites);

    bool valid() const { return !m_common.empty(); }
    int orientation() const { return m_orientation; }
    const std::vector<double>& commonSites() const { return m_common; }
    const std::vector<double>& sourceSites() const { return m_source; }
    const std::vector<double>& derivatives() const { return m_derivatives; }

    double evaluate(double u) const;
    double derivative(double u) const;

    /**
     * @brief Inverse map from a source parameter to the common parameter.
     *
     * The map is strictly monotone.  Endpoint and node values are handled
     * exactly; interior values use a bounded bisection solve.
     */
    double inverse(double sourceParameter) const;

private:
    int locateSegment(double u) const;
    double evaluateCanonical(double u, long double* derivative) const;

private:
    int m_orientation = 1;
    std::vector<double> m_common;
    std::vector<double> m_source;
    std::vector<double> m_derivatives;

    // Increasing canonical interpolation data after the optional sign flip.
    std::vector<double> m_canonicalSource;
    std::vector<double> m_canonicalDerivatives;
};

/**
 * @brief A BSpline composed with a MonotoneHermiteMap.
 *
 * The resulting curve is a genuine dynamic-degree nanospline BSpline.  Each
 * polynomial composition piece is stored in Bézier form and joined with the
 * usual repeated-knot representation; no evaluation closure or sampled fit is
 * retained.
 */
class ReparameterizedBSpline {
public:
    using CurveType = BSpline<double, 3, -1>;

    ReparameterizedBSpline() = default;
    ReparameterizedBSpline(const CurveType& source,
        const std::vector<double>& commonSites,
        const std::vector<double>& sourceSites);

    bool valid() const { return m_map.valid() && m_curve.get_num_control_points() > 0; }
    const MonotoneHermiteMap& map() const { return m_map; }
    const CurveType& curve() const { return m_curve; }
    CurveType releaseCurve() { return std::move(m_curve); }

private:
    MonotoneHermiteMap m_map;
    CurveType m_curve;
};

} // namespace nanospline
