#pragma once

#include <string>
#include <vector>

#include <nanospline/BSplinePatch.h>
#include <nanospline/ReparameterizedBSpline.h>

namespace nanospline {

/**
 * @brief Input description for a bidirectional Gordon curve network.
 *
 * Horizontal curves C_j are stored in @ref horizontalCurves.  Their common
 * v sites are @ref vSites and table `a[j][i]` gives C_j's parameter at the
 * intersection with vertical curve D_i.
 *
 * Vertical curves D_i are stored in @ref verticalCurves.  Their common u
 * sites are @ref uSites and table `b[i][j]` gives D_i's parameter at the same
 * intersection.
 */
struct CurveNetworkSpecification {
    using Curve = BSpline<double, 3, -1>;

    std::vector<Curve> horizontalCurves;
    std::vector<Curve> verticalCurves;
    std::vector<double> uSites;
    std::vector<double> vSites;

    /// a[j][i] = parameter of the C_j/D_i intersection on C_j.
    std::vector<std::vector<double>> a;

    /// b[i][j] = parameter of the C_j/D_i intersection on D_i.
    std::vector<std::vector<double>> b;
};

/**
 * @brief Result/owner object for a curve-network surface.
 *
 * A successful build stores one native BSplinePatch together with the
 * reparameterized horizontal and vertical curves.  Failed builds do not
 * replace a previously owned successful surface.
 */
class CurveNetwork {
public:
    using Patch = BSplinePatch<double, 3, -1, -1>;
    using Curve = CurveNetworkSpecification::Curve;

    CurveNetwork() = default;

    /**
     * @brief Validate and assemble S = L_U + L_V - L_UV.
     * @return true on success; on failure @p error (if supplied) and
     *         lastErrorCode() describe the rejection reason.
     */
    bool build(const CurveNetworkSpecification& specification, std::string* error = nullptr);

    bool hasSurface() const { return m_hasSurface; }
    CurveNetworkErrorCode lastErrorCode() const { return m_lastErrorCode; }
    const std::string& lastError() const { return m_lastError; }

    const Patch& surface() const { return m_surface; }
    Patch releaseSurface() {
        m_hasSurface = false;
        return std::move(m_surface);
    }

    const std::vector<ReparameterizedBSpline>& horizontalReparameterizations() const
    {
        return m_horizontal;
    }

    const std::vector<ReparameterizedBSpline>& verticalReparameterizations() const
    {
        return m_vertical;
    }

private:
    bool m_hasSurface = false;
    Patch m_surface;
    std::vector<ReparameterizedBSpline> m_horizontal;
    std::vector<ReparameterizedBSpline> m_vertical;
    CurveNetworkErrorCode m_lastErrorCode = CurveNetworkErrorCode::None;
    std::string m_lastError;
};

/**
 * @brief Convenience entry point that publishes directly into @p output.
 *
 * @p output is untouched if validation or assembly fails.
 */
bool buildCurveNetwork(const CurveNetworkSpecification& specification,
    BSplinePatch<double, 3, -1, -1>& output,
    std::string* error = nullptr);

/**
 * @brief Throwing convenience overload returning an owned native patch.
 */
BSplinePatch<double, 3, -1, -1> createCurveNetwork(
    const CurveNetworkSpecification& specification);

} // namespace nanospline
