#pragma once

#include <vector>

#include <Eigen/Core>

#include <nanospline/BSpline.h>

namespace nanospline {

/**
 * Piecewise cubic C1 monotone Hermite reparameterization.
 *
 * Given a sequence of strictly increasing common stations s_0 < ... < s_{n-1}
 * (in [0, 1]) and the corresponding source parameters t_0 < ... < t_{n-1}
 * (already oriented so that they increase), this class stores a piecewise
 * cubic map phi(s), defined on [s_0, s_{n-1}], satisfying:
 *
 *   * phi(s_k) = t_k for every station,
 *   * phi is C1 across stations,
 *   * phi is strictly increasing (positive segment slopes),
 *
 * using the fixed slope rule:
 *
 *   * segment secant slope delta_k = (t_{k+1} - t_k) / (s_{k+1} - s_k),
 *   * end slopes are the adjacent secant slopes,
 *   * interior slopes are the harmonic mean
 *     2 / (1/delta_{k-1} + 1/delta_k),
 *   * on every segment, if alpha = m_left/delta and beta = m_right/delta
 *     satisfy alpha^2 + beta^2 > 9, both endpoint slopes are multiplied by
 *     3 / sqrt(alpha^2 + beta^2).
 *
 * The cubic on each segment is the standard Hermite interpolant in s.
 * Reverse-oriented source parameters are handled by negating the parameters
 * before construction and negating the result afterwards (see
 * make_reparameterization).
 */
class PiecewiseCubicReparam
{
public:
    using Scalar = double;
    using StationVector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;

    PiecewiseCubicReparam() = default;

    /**
     * Build the map from increasing stations and increasing source
     * parameters.  Both arrays must have the same length (at least 2) and be
     * strictly increasing.
     */
    PiecewiseCubicReparam(const StationVector& stations, const StationVector& parameters);

    /** Stations at which the cubic pieces join. */
    const StationVector& get_stations() const { return m_stations; }

    /** Source parameters phi(s_k) at each station (increasing order). */
    const StationVector& get_parameters() const { return m_parameters; }

    /** Slopes phi'(s_k) at each station. */
    const StationVector& get_slopes() const { return m_slopes; }

    /** Whether the original row was reversed (parameters given descending). */
    bool is_reversed() const { return m_reversed; }

    Scalar lower_bound() const { return m_stations[0]; }
    Scalar upper_bound() const { return m_stations[m_stations.size() - 1]; }

    /** Locate the piece containing s; clamps s to the domain first. */
    int locate_segment(Scalar s) const;

    /** Evaluate phi(s). */
    Scalar evaluate(Scalar s) const;

    /** Evaluate phi'(s). */
    Scalar derivative(Scalar s) const;

private:
    StationVector m_stations;
    StationVector m_parameters;
    StationVector m_slopes;
    bool m_reversed = false;

    friend PiecewiseCubicReparam make_reparameterization(
        const StationVector& stations, const StationVector& source_parameters);
};

/**
 * Build a reparameterization from common stations and a row of source
 * parameters.  The parameter row may be strictly increasing or strictly
 * decreasing (a reversed curve): a descending row is negated internally, the
 * monotone Hermite rule is applied on the negated values, and the sign is
 * restored on evaluation.  Stations must be strictly increasing.
 */
PiecewiseCubicReparam make_reparameterization(
    const PiecewiseCubicReparam::StationVector& stations,
    const PiecewiseCubicReparam::StationVector& source_parameters);

/**
 * Result of composing a clamped, non-periodic non-rational B-spline curve
 * with a piecewise cubic reparameterization.  The composed object is a native
 * dynamic-degree BSpline: no evaluation closures are retained, and the source
 * curve can be released or modified afterwards.
 *
 * The composition is performed piece by piece.  Whenever a cubic
 * reparameterization piece crosses an internal knot of the source curve, the
 * piece is split at the exact inverse image of that knot, so every output
 * segment is the polynomial composition of one source Bezier segment with
 * one cubic.  Inverse images coinciding with stations are detected rather
 * than re-solved.
 */
class ReparameterizedBSpline
{
public:
    using Scalar = double;
    using Curve = BSpline<Scalar, 3, -1>;

    ReparameterizedBSpline() = default;

    /**
     * Compose `curve` with `reparam`.  Throws invalid_setting_error if the
     * reparameterization range does not cover the curve domain or the
     * numerical construction fails.
     */
    ReparameterizedBSpline(const Curve& curve, const PiecewiseCubicReparam& reparam);

    /** The native composed B-spline curve (owns its own control net). */
    const Curve& get_curve() const { return m_curve; }

    /** Mutable access to the composed curve (e.g. for further processing). */
    Curve& get_curve() { return m_curve; }

    const PiecewiseCubicReparam& get_reparameterization() const { return m_reparam; }

    /**
     * Breakpoints of the composed curve in station space: the reparam
     * stations together with every internal source-knot inverse image.
     */
    const PiecewiseCubicReparam::StationVector& get_breakpoints() const { return m_breakpoints; }

private:
    PiecewiseCubicReparam m_reparam;
    PiecewiseCubicReparam::StationVector m_breakpoints;
    Curve m_curve;
};

} // namespace nanospline
