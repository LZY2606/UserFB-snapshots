// Tests for gte::NURBSSilhouette.  Registered with the task driver through
// task_cases/task_case_count in three groups: topology, visibility, failure.
//
// Verification strategy: the analytic silhouette of the sphere
// S(u,v) = (2u, 2v, u^2+v^2-1) / (1+u^2+v^2) viewed along +Z is the circle
// u^2+v^2 = 1; occluder rectangles give analytic projection intervals on
// that circle; and every reported sample is back-substituted into an
// independently constructed surface.  The output of the silhouette code is
// never used as its own reference.

#include <Mathematics/NURBSSilhouette.h>

#include "task_support/check.h"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

using V3 = gte::Vector3<double>;
using Surface = gte::NURBSSurface<3, double>;
using Sil = gte::NURBSSilhouette;

namespace
{
    Sil::Options TestOptions()
    {
        Sil::Options o;
        o.spatialTolerance = 1.0e-6;
        o.parameterTolerance = 1.0e-6;
        o.angleTolerance = 1.0e-6;
        o.regionBudget = 200000;
        return o;
    }

    // Sphere of radius 'radius' around 'center' in the stereographic
    // parametrization on [-2,2]^2, quadratic with a knot inserted at 0 in
    // both directions (positive weights).  Optionally rotates the control
    // net by R (row-major 3x3) and scales z by zScale before placing it.
    std::shared_ptr<Surface> MakeSphere(V3 center, double radius,
        double const* R = nullptr, double zScale = 1.0)
    {
        gte::BasisFunctionInput<double> b;
        b.numControls = 4;
        b.degree = 2;
        b.uniform = false;
        b.periodic = false;
        b.numUniqueKnots = 3;
        b.uniqueKnots = { { -2.0, 3 }, { 0.0, 1 }, { 2.0, 3 } };
        const double g[4] = { -2.0, -1.0, 1.0, 2.0 };
        const double q[4] = { 4.0, 0.0, 0.0, 4.0 };
        std::vector<V3> c(16);
        std::vector<double> w(16);
        for (int j = 0; j < 4; ++j)
        {
            for (int i = 0; i < 4; ++i)
            {
                int k = i + 4 * j;
                double wk = 1.0 + q[i] + q[j];
                V3 p{ 2.0 * g[i] / wk, 2.0 * g[j] / wk, (q[i] + q[j] - 1.0) / wk };
                p[2] *= zScale;
                if (R)
                {
                    p = V3{
                        R[0] * p[0] + R[1] * p[1] + R[2] * p[2],
                        R[3] * p[0] + R[4] * p[1] + R[5] * p[2],
                        R[6] * p[0] + R[7] * p[1] + R[8] * p[2] };
                }
                c[static_cast<size_t>(k)] = center + radius * p;
                w[static_cast<size_t>(k)] = wk;
            }
        }
        return std::make_shared<Surface>(b, b, c.data(), w.data());
    }

    // Flat rectangle through the four corners, degree 2 on [0,1]^2.
    std::shared_ptr<Surface> MakePlane(V3 c00, V3 c10, V3 c01, V3 c11)
    {
        gte::BasisFunctionInput<double> b;
        b.numControls = 3;
        b.degree = 2;
        b.uniform = false;
        b.periodic = false;
        b.numUniqueKnots = 2;
        b.uniqueKnots = { { 0.0, 3 }, { 1.0, 3 } };
        std::vector<V3> c(9);
        std::vector<double> w(9, 1.0);
        for (int j = 0; j < 3; ++j)
        {
            for (int i = 0; i < 3; ++i)
            {
                double s = i / 2.0;
                double t = j / 2.0;
                c[static_cast<size_t>(i + 3 * j)] =
                    ((1.0 - s) * (1.0 - t)) * c00 + (s * (1.0 - t)) * c10
                    + ((1.0 - s) * t) * c01 + (s * t) * c11;
            }
        }
        return std::make_shared<Surface>(b, b, c.data(), w.data());
    }

    // Curve parameter t -> interpolated position (chord interpolation).
    V3 CurvePosition(Sil::Curve const& curve, double t)
    {
        size_t n = curve.samples.size();
        size_t seg = curve.closed ? n : n - 1;
        size_t k = 0;
        while (k + 1 < seg && curve.parameter[k + 1] < t)
        {
            ++k;
        }
        double ta = curve.parameter[k];
        double tb = (k + 1 < n ? curve.parameter[k + 1] : 1.0);
        double lambda = (tb > ta ? (t - ta) / (tb - ta) : 0.0);
        V3 const& a = curve.samples[k].position;
        V3 const& b = curve.samples[(k + 1) % n].position;
        return a + lambda * (b - a);
    }

    double CurveAngle(Sil::Curve const& curve, double t)
    {
        V3 p = CurvePosition(curve, t);
        return std::atan2(p[1], p[0]);
    }

    double IntervalMid(double t0, double t1)
    {
        if (t0 <= t1)
        {
            return 0.5 * (t0 + t1);
        }
        double m = 0.5 * (t0 + t1 + 1.0);
        return (m >= 1.0 ? m - 1.0 : m);
    }

    // ------------------------------------------------------------------
    // topology group
    // ------------------------------------------------------------------
    int CasePlaneNoSilhouette()
    {
        auto plane = MakePlane({ -2, -2, 1 }, { 2, -2, 1 }, { -2, 2, 1 }, { 2, 2, 1 });
        Sil sil({ plane }, V3{ 0, 0, 1 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(res.complete);
        TASK_CHECK(res.curves.empty());
        TASK_CHECK(res.unresolved.empty());
        return 0;
    }

    int CaseSphereClosedCircle()
    {
        auto sph = MakeSphere({ 0, 0, 0 }, 1.0);
        Sil::Result res;
        {
            Sil sil({ sph }, V3{ 0, 0, 1 }, TestOptions());
            TASK_CHECK(sil.IsValid());
            TASK_CHECK(sil.Compute(res));
        }
        // The result owns its data: release the input and keep checking.
        sph.reset();
        TASK_CHECK(res.complete);
        TASK_CHECK(res.unresolved.empty());
        TASK_CHECK(res.curves.size() == 1);
        auto const& curve = res.curves[0];
        TASK_CHECK(curve.closed);
        TASK_CHECK(curve.samples.size() >= 60);

        auto check = MakeSphere({ 0, 0, 0 }, 1.0);
        bool uNeg = false, uPos = false, vNeg = false, vPos = false;
        for (auto const& s : curve.samples)
        {
            // Analytic silhouette: u^2+v^2 = 1, z = 0 on the unit sphere.
            TASK_CHECK(std::fabs(s.u * s.u + s.v * s.v - 1.0) <= 1.0e-4);
            TASK_CHECK(std::fabs(gte::Length(s.position) - 1.0) <= 1.0e-6);
            TASK_CHECK(std::fabs(s.position[2]) <= 1.0e-4);
            // Angle tolerance against an independent evaluation.
            std::array<V3, 6> jet{};
            check->Evaluate(s.u, s.v, 1, jet.data());
            V3 n = gte::Cross(jet[1], jet[2]);
            TASK_CHECK(std::fabs(gte::Dot(n, V3{ 0, 0, 1 })) / gte::Length(n) <= 1.0e-6);
            // Position matches back-substitution into the source surface.
            TASK_CHECK(gte::Length(jet[0] - s.position) <= 1.0e-6);
            // Oriented unit tangent, lying in the tangent plane.
            TASK_CHECK(std::fabs(gte::Length(s.tangent) - 1.0) <= 1.0e-6);
            TASK_CHECK(std::fabs(gte::Dot(s.tangent, n)) / gte::Length(n) <= 1.0e-4);
            uNeg = uNeg || (s.u < -0.5);
            uPos = uPos || (s.u > 0.5);
            vNeg = vNeg || (s.v < -0.5);
            vPos = vPos || (s.v > 0.5);
        }
        // One connected branch across all four knot elements.
        TASK_CHECK(uNeg && uPos && vNeg && vPos);
        // No occluders: a single visible interval covering the curve.
        TASK_CHECK(curve.intervals.size() == 1);
        TASK_CHECK(curve.intervals[0].state == Sil::IntervalState::Visible);
        TASK_CHECK(curve.intervals[0].t0 <= 1.0e-9 && curve.intervals[0].t1 >= 1.0 - 1.0e-9);
        return 0;
    }

    int CaseRotatedTranslatedSphere()
    {
        // Rodrigues rotation, axis (1,2,3)/sqrt(14), angle 0.9.
        double th = 0.9;
        double ax = 1.0 / std::sqrt(14.0), ay = 2.0 / std::sqrt(14.0), az = 3.0 / std::sqrt(14.0);
        double c = std::cos(th), s = std::sin(th), C = 1.0 - c;
        double R[9] = {
            c + ax * ax * C, ax * ay * C - az * s, ax * az * C + ay * s,
            ay * ax * C + az * s, c + ay * ay * C, ay * az * C - ax * s,
            az * ax * C - ay * s, az * ay * C + ax * s, c + az * az * C };
        V3 center{ 0.3, -0.2, 0.7 };
        auto sph = MakeSphere(center, 1.0, R);
        Sil sil({ sph }, V3{ 0, 0, 1 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(res.complete);
        TASK_CHECK(res.unresolved.empty());
        TASK_CHECK(res.curves.size() == 1);
        auto const& curve = res.curves[0];
        TASK_CHECK(curve.closed);
        // The silhouette is the unit circle in the plane through the center
        // perpendicular to D, regardless of the parametrization.
        for (auto const& smp : curve.samples)
        {
            V3 d = smp.position - center;
            TASK_CHECK(std::fabs(gte::Length(d) - 1.0) <= 1.0e-5);
            TASK_CHECK(std::fabs(gte::Dot(d, V3{ 0, 0, 1 })) <= 1.0e-4);
        }
        return 0;
    }

    int CaseSmallClosedLoop()
    {
        // A small sphere produces a small interior closed silhouette loop.
        V3 center{ 0.4, -0.3, 0.2 };
        double radius = 0.15;
        auto sph = MakeSphere(center, radius);
        Sil sil({ sph }, V3{ 0, 0, 1 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(res.complete);
        TASK_CHECK(res.curves.size() == 1);
        auto const& curve = res.curves[0];
        TASK_CHECK(curve.closed);
        TASK_CHECK(curve.samples.size() >= 12);
        for (auto const& smp : curve.samples)
        {
            V3 d = smp.position - center;
            TASK_CHECK(std::fabs(gte::Length(d) - radius) <= 1.0e-5);
            TASK_CHECK(std::fabs(d[2]) <= 1.0e-4);
            // Interior loop: parameters stay away from the domain boundary.
            TASK_CHECK(smp.u > -2.0 + 1.0e-3 && smp.u < 2.0 - 1.0e-3);
            TASK_CHECK(smp.v > -2.0 + 1.0e-3 && smp.v < 2.0 - 1.0e-3);
        }
        return 0;
    }

    int CaseSamePointDifferentUV()
    {
        // Two coincident spheres: identical silhouette points, but the
        // branches live on different surfaces and must not be welded.
        auto s0 = MakeSphere({ 0, 0, 0 }, 1.0);
        auto s1 = MakeSphere({ 0, 0, 0 }, 1.0);
        Sil sil({ s0, s1 }, V3{ 0, 0, 1 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(res.complete);
        TASK_CHECK(res.curves.size() == 2);
        TASK_CHECK(res.curves[0].closed && res.curves[1].closed);
        TASK_CHECK(res.curves[0].surface != res.curves[1].surface);
        // Same spatial points appear on both branches.
        bool shared = false;
        V3 p0 = res.curves[0].samples[0].position;
        for (auto const& smp : res.curves[1].samples)
        {
            if (gte::Length(smp.position - p0) <= 1.0e-5)
            {
                shared = true;
                break;
            }
        }
        TASK_CHECK(shared);
        // Coincident surfaces touch the ray tangentially at t=0: both
        // branches remain fully visible.
        for (auto const& curve : res.curves)
        {
            TASK_CHECK(curve.intervals.size() == 1);
            TASK_CHECK(curve.intervals[0].state == Sil::IntervalState::Visible);
        }
        return 0;
    }

    int CaseViewFromBelow()
    {
        // Same sphere, camera on the other side: the silhouette is the same
        // circle u^2+v^2 = 1.
        auto sph = MakeSphere({ 0, 0, 0 }, 1.0);
        Sil sil({ sph }, V3{ 0, 0, -1 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(res.complete);
        TASK_CHECK(res.curves.size() == 1);
        auto const& curve = res.curves[0];
        TASK_CHECK(curve.closed);
        for (auto const& smp : curve.samples)
        {
            TASK_CHECK(std::fabs(smp.u * smp.u + smp.v * smp.v - 1.0) <= 1.0e-4);
            TASK_CHECK(std::fabs(smp.position[2]) <= 1.0e-4);
        }
        TASK_CHECK(curve.intervals.size() == 1);
        TASK_CHECK(curve.intervals[0].state == Sil::IntervalState::Visible);
        return 0;
    }

    int CaseOpenBranchDegree3()
    {
        // Parabolic cylinder S(u,v) = (2u-1, 2v-1, (2u-1)^2) on [0,1]^2,
        // cubic in u, quadratic in v, viewed along +X.  The silhouette is
        // the line u = 1/2, an open branch ending on the v-domain boundary.
        gte::BasisFunctionInput<double> bu;
        bu.numControls = 4;
        bu.degree = 3;
        bu.uniform = false;
        bu.periodic = false;
        bu.numUniqueKnots = 2;
        bu.uniqueKnots = { { 0.0, 4 }, { 1.0, 4 } };
        gte::BasisFunctionInput<double> bv;
        bv.numControls = 3;
        bv.degree = 2;
        bv.uniform = false;
        bv.periodic = false;
        bv.numUniqueKnots = 2;
        bv.uniqueKnots = { { 0.0, 3 }, { 1.0, 3 } };
        // Cubic Bezier coefficients of x = 2u-1 and z = (2u-1)^2 (degree
        // elevated), quadratic coefficients of y = 2v-1.
        double const cx[4] = { -1.0, -1.0 / 3.0, 1.0 / 3.0, 1.0 };
        double const cz[4] = { 1.0, -1.0 / 3.0, -1.0 / 3.0, 1.0 };
        double const cy[3] = { -1.0, 0.0, 1.0 };
        std::vector<V3> c(12);
        std::vector<double> w(12, 1.0);
        for (int j = 0; j < 3; ++j)
        {
            for (int i = 0; i < 4; ++i)
            {
                c[static_cast<size_t>(i + 4 * j)] = V3{ cx[i], cy[j], cz[i] };
            }
        }
        auto surf = std::make_shared<Surface>(bu, bv, c.data(), w.data());
        Sil sil({ surf }, V3{ 1, 0, 0 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(res.complete);
        TASK_CHECK(res.unresolved.empty());
        TASK_CHECK(res.curves.size() == 1);
        auto const& curve = res.curves[0];
        TASK_CHECK(!curve.closed);
        TASK_CHECK(curve.samples.size() >= 8);
        for (auto const& smp : curve.samples)
        {
            TASK_CHECK(std::fabs(smp.u - 0.5) <= 1.0e-5);
            TASK_CHECK(std::fabs(smp.position[0]) <= 1.0e-4);
            TASK_CHECK(std::fabs(smp.position[2]) <= 1.0e-4);
        }
        // Open branch ends on the domain boundary v=0 and v=1.
        double vFirst = curve.samples.front().v;
        double vLast = curve.samples.back().v;
        double vLo = std::min(vFirst, vLast);
        double vHi = std::max(vFirst, vLast);
        TASK_CHECK(vLo <= 1.0e-4);
        TASK_CHECK(vHi >= 1.0 - 1.0e-4);
        // Fully visible (the ray is tangent to the parabola at t=0 only).
        TASK_CHECK(curve.intervals.size() == 1);
        TASK_CHECK(curve.intervals[0].state == Sil::IntervalState::Visible);
        return 0;
    }

    // ------------------------------------------------------------------
    // visibility group
    // ------------------------------------------------------------------
    int CaseOccludedArcAndBand()
    {
        auto sph = MakeSphere({ 0, 0, 0 }, 1.0);
        // Small arc near angle 0: x >= 0.85, |y| <= 0.4 at z = 0.5.
        auto rect1 = MakePlane({ 0.85, -0.4, 0.5 }, { 1.5, -0.4, 0.5 },
            { 0.85, 0.4, 0.5 }, { 1.5, 0.4, 0.5 });
        // Narrow band through the middle: |x| <= 0.04 at z = 0.5.
        auto rect2 = MakePlane({ -0.04, -1.5, 0.5 }, { 0.04, -1.5, 0.5 },
            { -0.04, 1.5, 0.5 }, { 0.04, 1.5, 0.5 });
        Sil sil({ sph, rect1, rect2 }, V3{ 0, 0, 1 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(res.complete);
        TASK_CHECK(res.unresolved.empty());
        TASK_CHECK(res.curves.size() == 1);
        auto const& curve = res.curves[0];
        TASK_CHECK(curve.closed);

        auto occludedAnalytic = [](double theta)
        {
            double cs = std::cos(theta);
            double sn = std::sin(theta);
            return (cs >= 0.85 && std::fabs(sn) <= 0.4) || (std::fabs(cs) <= 0.04);
        };

        size_t numHidden = 0;
        size_t numVisible = 0;
        for (auto const& iv : curve.intervals)
        {
            TASK_CHECK(iv.state != Sil::IntervalState::Unknown);
            double tm = IntervalMid(iv.t0, iv.t1);
            double theta = CurveAngle(curve, tm);
            if (iv.state == Sil::IntervalState::Hidden)
            {
                ++numHidden;
                TASK_CHECK(occludedAnalytic(theta));
            }
            else
            {
                ++numVisible;
                TASK_CHECK(!occludedAnalytic(theta));
            }
        }
        // The small arc and the two band crossings: 3 hidden, 3 visible
        // intervals, never joined across the occluded gaps.
        TASK_CHECK(numHidden == 3);
        TASK_CHECK(numVisible == 3);

        // Occlusion boundaries match the analytic projection intervals.
        double const expectedCenter[3] = { 0.0, 3.141592653589793 / 2.0, -3.141592653589793 / 2.0 };
        double const expectedHalf[3] = { std::asin(0.4), std::asin(0.04), std::asin(0.04) };
        bool matched[3] = { false, false, false };
        for (auto const& iv : curve.intervals)
        {
            if (iv.state != Sil::IntervalState::Hidden)
            {
                continue;
            }
            double a0 = CurveAngle(curve, iv.t0);
            double a1 = CurveAngle(curve, iv.t1);
            while (a1 < a0 - 3.141592653589793)
            {
                a1 += 2.0 * 3.141592653589793;
            }
            while (a1 > a0 + 3.141592653589793)
            {
                a1 -= 2.0 * 3.141592653589793;
            }
            double center = 0.5 * (a0 + a1);
            double half = 0.5 * std::fabs(a1 - a0);
            for (int k = 0; k < 3; ++k)
            {
                if (std::fabs(center - expectedCenter[k]) <= 0.02
                    && std::fabs(half - expectedHalf[k]) <= 0.02)
                {
                    matched[k] = true;
                }
            }
        }
        TASK_CHECK(matched[0] && matched[1] && matched[2]);
        return 0;
    }

    int CaseSelfOcclusionNone()
    {
        // A flattened spheroid: the silhouette ray is tangent to the surface
        // at t=0, which must not count as self-occlusion.
        auto sph = MakeSphere({ 0, 0, 0 }, 1.0, nullptr, 0.5);
        Sil sil({ sph }, V3{ 0, 0, 1 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(res.complete);
        TASK_CHECK(res.curves.size() == 1);
        auto const& curve = res.curves[0];
        TASK_CHECK(curve.closed);
        TASK_CHECK(curve.intervals.size() == 1);
        TASK_CHECK(curve.intervals[0].state == Sil::IntervalState::Visible);
        for (auto const& smp : curve.samples)
        {
            TASK_CHECK(std::fabs(smp.position[2]) <= 1.0e-4);
            TASK_CHECK(std::fabs(std::sqrt(smp.position[0] * smp.position[0]
                + smp.position[1] * smp.position[1]) - 1.0) <= 1.0e-4);
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // failure group
    // ------------------------------------------------------------------
    std::shared_ptr<Surface> MakeBadWeightSphere(double w0)
    {
        auto sph = MakeSphere({ 0, 0, 0 }, 1.0);
        // Rebuild with one overridden weight.
        gte::BasisFunctionInput<double> b;
        b.numControls = 4;
        b.degree = 2;
        b.uniform = false;
        b.periodic = false;
        b.numUniqueKnots = 3;
        b.uniqueKnots = { { -2.0, 3 }, { 0.0, 1 }, { 2.0, 3 } };
        std::vector<V3> c(sph->GetControls(), sph->GetControls() + 16);
        std::vector<double> w(sph->GetWeights(), sph->GetWeights() + 16);
        w[0] = w0;
        return std::make_shared<Surface>(b, b, c.data(), w.data());
    }

    std::shared_ptr<Surface> MakeCustomKnots(int32_t degree, bool periodic,
        std::vector<gte::UniqueKnot<double>> const& knots, int32_t numControls)
    {
        gte::BasisFunctionInput<double> b;
        b.numControls = numControls;
        b.degree = degree;
        b.uniform = false;
        b.periodic = periodic;
        b.numUniqueKnots = static_cast<int32_t>(knots.size());
        b.uniqueKnots = knots;
        std::vector<V3> c(static_cast<size_t>(numControls) * static_cast<size_t>(numControls));
        std::vector<double> w(c.size(), 1.0);
        for (size_t k = 0; k < c.size(); ++k)
        {
            c[k] = V3{ 0.1 * static_cast<double>(k % 7), 0.1 * static_cast<double>(k % 5), 1.0 };
        }
        return std::make_shared<Surface>(b, b, c.data(), w.data());
    }

    int CaseInvalidInputsRejected()
    {
        auto good = MakeSphere({ 0, 0, 0 }, 1.0);
        Sil::Result sentinel;
        sentinel.complete = true;
        sentinel.curves.resize(1);
        sentinel.unresolved.resize(1);

        auto rejected = [&](std::vector<std::shared_ptr<Surface>> const& surfs,
            V3 const& d, Sil::Options const& o) -> int
        {
            Sil sil(surfs, d, o);
            TASK_CHECK(!sil.IsValid());
            Sil::Result res = sentinel;
            TASK_CHECK(!sil.Compute(res));
            TASK_CHECK(res.complete == sentinel.complete);
            TASK_CHECK(res.curves.size() == sentinel.curves.size());
            TASK_CHECK(res.unresolved.size() == sentinel.unresolved.size());
            return 0;
        };

        Sil::Options o = TestOptions();

        // Illegal cameras.
        TASK_CHECK(rejected({ good }, V3{ 0, 0, 2 }, o) == 0);
        TASK_CHECK(rejected({ good }, V3{ 0, 0, 0 }, o) == 0);

        // Illegal weights.
        TASK_CHECK(rejected({ MakeBadWeightSphere(0.0) }, V3{ 0, 0, 1 }, o) == 0);
        TASK_CHECK(rejected({ MakeBadWeightSphere(-1.0) }, V3{ 0, 0, 1 }, o) == 0);

        // Degree outside {2,3}.
        TASK_CHECK(rejected({ MakeCustomKnots(1, false, { { 0.0, 2 }, { 1.0, 2 } }, 3) },
            V3{ 0, 0, 1 }, o) == 0);
        TASK_CHECK(rejected({ MakeCustomKnots(4, false, { { 0.0, 5 }, { 1.0, 5 } }, 5) },
            V3{ 0, 0, 1 }, o) == 0);

        // Non-clamped knot vector (end multiplicity below degree+1).
        TASK_CHECK(rejected({ MakeCustomKnots(2, false, { { 0.0, 2 }, { 1.0, 2 } }, 3) },
            V3{ 0, 0, 1 }, o) == 0);

        // Periodic knot vector.
        TASK_CHECK(rejected({ MakeCustomKnots(2, true, { { 0.0, 1 }, { 0.5, 1 }, { 1.0, 1 } }, 3) },
            V3{ 0, 0, 1 }, o) == 0);

        // Interior knot with multiplicity == degree: only C0 there.
        TASK_CHECK(rejected({ MakeCustomKnots(2, false, { { 0.0, 3 }, { 0.5, 2 }, { 1.0, 3 } }, 5) },
            V3{ 0, 0, 1 }, o) == 0);

        // Illegal tolerances and budget.
        Sil::Options bad = TestOptions();
        bad.spatialTolerance = -1.0e-6;
        TASK_CHECK(rejected({ good }, V3{ 0, 0, 1 }, bad) == 0);
        bad = TestOptions();
        bad.parameterTolerance = 0.0;
        TASK_CHECK(rejected({ good }, V3{ 0, 0, 1 }, bad) == 0);
        bad = TestOptions();
        bad.angleTolerance = 0.0;
        TASK_CHECK(rejected({ good }, V3{ 0, 0, 1 }, bad) == 0);
        bad = TestOptions();
        bad.angleTolerance = 1.0;
        TASK_CHECK(rejected({ good }, V3{ 0, 0, 1 }, bad) == 0);
        bad = TestOptions();
        bad.regionBudget = 0;
        TASK_CHECK(rejected({ good }, V3{ 0, 0, 1 }, bad) == 0);
        return 0;
    }

    int CaseTangentPlaneUnresolved()
    {
        // Plane in the z=0 plane viewed along its own tangent direction:
        // f vanishes identically, the silhouette is not an isolated curve.
        auto plane = MakePlane({ -2, -2, 0 }, { 2, -2, 0 }, { -2, 2, 0 }, { 2, 2, 0 });
        Sil sil({ plane }, V3{ 1, 0, 0 }, TestOptions());
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        TASK_CHECK(!res.complete);
        TASK_CHECK(res.curves.empty());
        TASK_CHECK(!res.unresolved.empty());
        bool found = false;
        for (auto const& r : res.unresolved)
        {
            if (r.surface == 0
                && r.reason == Sil::UnresolvedReason::NonIsolatedZeroSet
                && r.umin >= -1.0e-9 && r.umax <= 1.0 + 1.0e-9
                && r.vmin >= -1.0e-9 && r.vmax <= 1.0 + 1.0e-9)
            {
                found = true;
            }
        }
        TASK_CHECK(found);
        return 0;
    }

    int CaseBudgetExhausted()
    {
        auto sph = MakeSphere({ 0, 0, 0 }, 1.0);
        Sil::Options o = TestOptions();
        o.regionBudget = 20;
        Sil sil({ sph }, V3{ 0, 0, 1 }, o);
        TASK_CHECK(sil.IsValid());
        Sil::Result res;
        TASK_CHECK(sil.Compute(res));
        // Budget exhaustion is reported, never treated as empty space.
        TASK_CHECK(!res.complete);
        TASK_CHECK(!res.unresolved.empty());
        bool hasBudget = false;
        for (auto const& r : res.unresolved)
        {
            hasBudget = hasBudget || (r.reason == Sil::UnresolvedReason::BudgetExhausted);
        }
        TASK_CHECK(hasBudget);
        return 0;
    }
}

const task_case task_cases[] =
{
    { "topology", "plane_no_silhouette", CasePlaneNoSilhouette },
    { "topology", "sphere_closed_circle", CaseSphereClosedCircle },
    { "topology", "rotated_translated_sphere", CaseRotatedTranslatedSphere },
    { "topology", "small_closed_loop", CaseSmallClosedLoop },
    { "topology", "same_point_different_uv", CaseSamePointDifferentUV },
    { "topology", "view_from_below", CaseViewFromBelow },
    { "topology", "open_branch_degree3", CaseOpenBranchDegree3 },
    { "visibility", "occluded_arc_and_band", CaseOccludedArcAndBand },
    { "visibility", "self_occlusion_none", CaseSelfOcclusionNone },
    { "failure", "invalid_inputs_rejected", CaseInvalidInputsRejected },
    { "failure", "tangent_plane_unresolved", CaseTangentPlaneUnresolved },
    { "failure", "budget_exhausted", CaseBudgetExhausted }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
