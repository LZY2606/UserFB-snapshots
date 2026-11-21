#pragma once

#include <vector>

#include <Eigen/Core>

#include <nanospline/BSpline.h>
#include <nanospline/BSplinePatch.h>
#include <nanospline/ReparameterizedBSpline.h>

namespace nanospline {

/**
 * Bidirectional curve network construction (Gordon surface).
 *
 * The input consists of two families of clamped, non-periodic, non-rational
 * 3D B-spline curves:
 *
 *   * transverse curves C_j (j = 0..m-1), each associated with a common
 *     v-station V_j in [0, 1],
 *   * longitudinal curves D_i (i = 0..n-1), each associated with a common
 *     u-station U_i in [0, 1].
 *
 * The intersection tables declare that C_j(a[j][i]) and D_i(b[i][j]) are the
 * same spatial point.  Each table row is strictly monotone, its first and
 * last entries are exactly the two domain endpoints of the corresponding
 * curve, and a row may be descending (a reversed parameterization).
 *
 * Every curve is first reparameterized onto the common stations by a fixed
 * C1 monotone piecewise cubic map (see PiecewiseCubicReparam).  The Gordon
 * combination
 *
 *     S = L_U + L_V - L_UV
 *
 * is then built as a single native tensor-product BSplinePatch:
 *
 *   * L_U  interpolates the reparameterized transverse curves at the
 *     V-stations,
 *   * L_V  interpolates the reparameterized longitudinal curves at the
 *     U-stations,
 *   * L_UV interpolates the averaged intersection network in both
 *     directions,
 *
 * using natural cubic interpolation (interpolating, internal C2, zero second
 * derivative at both ends); an axis with only two stations degenerates to
 * linear interpolation.  All three terms share one knot space per axis and
 * are added as native splines.
 */
class CurveNetwork
{
public:
    using Scalar = double;
    using Curve = BSpline<Scalar, 3, -1>;
    using Patch = BSplinePatch<Scalar, 3, -1, -1>;
    using StationVector = PiecewiseCubicReparam::StationVector;
    using Point = Eigen::Matrix<Scalar, 1, 3>;

    CurveNetwork() = default;

    /**
     * Construct the network.
     *
     * @param transverse_curves  C_j, m curves (2 <= m <= 6).
     * @param longitudinal_curves D_i, n curves (2 <= n <= 6).
     * @param v_stations   V_j, strictly increasing from 0 to 1, adjacent
     *                     spacing at least 0.05.
     * @param u_stations   U_i, same requirements.
     * @param a_table  a[j][i]: parameters on C_j of the intersection with D_i.
     * @param b_table  b[i][j]: parameters on D_i of the intersection with C_j.
     *
     * @throws invalid_setting_error if any structural, finiteness,
     *     monotonicity or geometric-compatibility check fails, or if the
     *     numerical construction fails.  A thrown exception leaves any
     *     previously built output untouched.
     */
    CurveNetwork(const std::vector<Curve>& transverse_curves,
        const std::vector<Curve>& longitudinal_curves,
        const StationVector& v_stations,
        const StationVector& u_stations,
        const std::vector<std::vector<Scalar>>& a_table,
        const std::vector<std::vector<Scalar>>& b_table);

    /** Reparameterization phi_j of transverse curve C_j onto u. */
    const PiecewiseCubicReparam& get_transverse_reparam(int j) const
    {
        return m_transverse_reparams.at(static_cast<size_t>(j));
    }

    /** Reparameterization psi_i of longitudinal curve D_i onto v. */
    const PiecewiseCubicReparam& get_longitudinal_reparam(int i) const
    {
        return m_longitudinal_reparams.at(static_cast<size_t>(i));
    }

    /** Native reparameterized transverse curves C_j(phi_j(u)). */
    const std::vector<ReparameterizedBSpline>& get_reparameterized_transverse_curves() const
    {
        return m_reparam_transverse;
    }

    /** Native reparameterized longitudinal curves D_i(psi_i(v)). */
    const std::vector<ReparameterizedBSpline>& get_reparameterized_longitudinal_curves() const
    {
        return m_reparam_longitudinal;
    }

    /** The Gordon surface; owns its control grid and both knot vectors. */
    const Patch& get_patch() const { return m_patch; }

    /** Common u knot vector shared by all three Gordon terms. */
    const StationVector& get_u_knots() const { return m_u_knots; }

    /** Common v knot vector shared by all three Gordon terms. */
    const StationVector& get_v_knots() const { return m_v_knots; }

    int num_transverse() const { return static_cast<int>(m_reparam_transverse.size()); }
    int num_longitudinal() const { return static_cast<int>(m_reparam_longitudinal.size()); }

private:
    std::vector<PiecewiseCubicReparam> m_transverse_reparams;
    std::vector<PiecewiseCubicReparam> m_longitudinal_reparams;
    std::vector<ReparameterizedBSpline> m_reparam_transverse;
    std::vector<ReparameterizedBSpline> m_reparam_longitudinal;
    StationVector m_u_knots;
    StationVector m_v_knots;
    Patch m_patch;
};

namespace internal {

using CurveNetworkScalar = double;

/**
 * Natural cubic spline interpolation control points for scalar/vector values
 * y_k at stations x_k.  Returns the control points and knot vector of a
 * native clamped cubic B-spline (internal knots carry multiplicity one and
 * carry the station second-derivative data); with two stations a linear
 * B-spline is returned instead.  Exposed for testing and reuse.
 */
Eigen::Matrix<CurveNetworkScalar, Eigen::Dynamic, 3> natural_cubic_interpolation(
    const Eigen::Matrix<CurveNetworkScalar, Eigen::Dynamic, 1>& stations,
    const Eigen::Matrix<CurveNetworkScalar, Eigen::Dynamic, 3>& values,
    Eigen::Matrix<CurveNetworkScalar, Eigen::Dynamic, 1>& knots,
    int& degree);

/** Merge two clamped knot vectors into one refined clamped knot vector. */
Eigen::Matrix<CurveNetworkScalar, Eigen::Dynamic, 1> merge_knot_vectors(
    const Eigen::Matrix<CurveNetworkScalar, Eigen::Dynamic, 1>& knots_a,
    int degree_a,
    const Eigen::Matrix<CurveNetworkScalar, Eigen::Dynamic, 1>& knots_b,
    int degree_b,
    int& merged_degree);

} // namespace internal

} // namespace nanospline
