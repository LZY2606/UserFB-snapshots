#include "task_support/check.h"

#include <nanospline/CurveNetwork.h>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using nanospline::BSpline;
using nanospline::BSplinePatch;
using nanospline::CurveNetwork;
using nanospline::CurveNetworkSpecification;
using nanospline::MonotoneHermiteMap;
using nanospline::ReparameterizedBSpline;

using Curve3 = BSpline<double, 3, -1>;
using Patch3 = BSplinePatch<double, 3, -1, -1>;
using Bezier3 = nanospline::Bezier<double, 3, -1>;
using V3 = Eigen::Vector3d;
using RowV3 = Eigen::Matrix<double, 1, 3>;

namespace {

constexpr double PI = 3.14159265358979323846;

struct RefMap {
    int orientation = 1;
    std::vector<double> x, y, m;
    std::vector<double> z, mCanonical;

    static RefMap build(const std::vector<double>& xin, const std::vector<double>& yin)
    {
        RefMap r;
        r.x = xin;
        r.y = yin;
        const int n = static_cast<int>(xin.size());
        bool inc = true, dec = true;
        for (int i = 1; i < n; ++i) {
            inc = inc && yin[i] > yin[i - 1];
            dec = dec && yin[i] < yin[i - 1];
        }
        r.orientation = inc ? 1 : -1;
        r.z.resize(n);
        for (int i = 0; i < n; ++i) r.z[i] = r.orientation * yin[i];

        std::vector<double> delta(n - 1);
        for (int i = 0; i < n - 1; ++i) {
            delta[i] = (r.z[i + 1] - r.z[i]) / (xin[i + 1] - xin[i]);
        }
        r.mCanonical.resize(n);
        r.mCanonical[0] = delta[0];
        r.mCanonical[n - 1] = delta[n - 2];
        for (int i = 1; i + 1 < n; ++i) {
            r.mCanonical[i] = 2.0 / (1.0 / delta[i - 1] + 1.0 / delta[i]);
        }
        for (int i = 0; i < n - 1; ++i) {
            const double alpha = r.mCanonical[i] / delta[i];
            const double beta = r.mCanonical[i + 1] / delta[i];
            const double q = std::sqrt(alpha * alpha + beta * beta);
            if (q > 9.0) {
                const double scale = 3.0 / q;
                r.mCanonical[i] *= scale;
                r.mCanonical[i + 1] *= scale;
            }
        }
        r.m.resize(n);
        for (int i = 0; i < n; ++i) r.m[i] = r.orientation * r.mCanonical[i];
        return r;
    }

    int segment(double u) const
    {
        const int n = static_cast<int>(x.size());
        if (u <= x.front()) return 0;
        if (u >= x.back()) return n - 2;
        int k = static_cast<int>(std::upper_bound(x.begin(), x.end(), u) - x.begin()) - 1;
        return std::max(0, std::min(n - 2, k));
    }

    double eval(double u, double* derivative = nullptr) const
    {
        if (u <= x.front()) {
            if (derivative) *derivative = m.front();
            return y.front();
        }
        if (u >= x.back()) {
            if (derivative) *derivative = m.back();
            return y.back();
        }
        const int k = segment(u);
        const double h = x[k + 1] - x[k];
        const double t = (u - x[k]) / h;
        const double t2 = t * t, t3 = t2 * t;
        if (derivative) {
            const double dh00 = 6 * t2 - 6 * t;
            const double dh10 = 3 * t2 - 4 * t + 1;
            const double dh01 = -6 * t2 + 6 * t;
            const double dh11 = 3 * t2 - 2 * t;
            *derivative = (dh00 * y[k] + dh10 * h * m[k] + dh01 * y[k + 1] +
                              dh11 * h * m[k + 1]) /
                          h;
        }
        return (2 * t3 - 3 * t2 + 1) * y[k] + (t3 - 2 * t2 + t) * h * m[k] +
               (-2 * t3 + 3 * t2) * y[k + 1] + (t3 - t2) * h * m[k + 1];
    }

    double inverse(double target) const
    {
        for (int i = 0; i < static_cast<int>(y.size()); ++i) {
            if (target == y[i]) return x[i];
        }
        double lo = 0.0, hi = 1.0;
        for (int it = 0; it < 100; ++it) {
            const double mid = 0.5 * (lo + hi);
            const double value = eval(mid);
            if (orientation * (value - target) < 0.0) lo = mid;
            else hi = mid;
        }
        return 0.5 * (lo + hi);
    }
};

V3 toV3(const RowV3& p) { return V3(p.x(), p.y(), p.z()); }
RowV3 toRow(const V3& p) { return RowV3(p.x(), p.y(), p.z()); }

std::vector<double> makeSites(int count, int variant)
{
    if (count == 2) return {0.0, 1.0};
    if (count == 3) return variant ? std::vector<double>{0.0, 0.5, 1.0}
                                   : std::vector<double>{0.0, 0.45, 1.0};
    if (count == 4) return {0.0, 0.20, 0.55, 1.0};
    if (count == 5) return {0.0, 0.12, 0.31, 0.58, 1.0};
    return {0.0, 0.10, 0.27, 0.50, 0.76, 1.0};
}

std::vector<double> makeParams(
    int count, double lo, double hi, double exponent, bool reversed)
{
    std::vector<double> result(count);
    for (int i = 0; i < count; ++i) {
        const double q = count == 1 ? 0.0 : static_cast<double>(i) / (count - 1);
        result[i] = lo + (hi - lo) * std::pow(q, exponent);
    }
    if (reversed) std::reverse(result.begin(), result.end());
    return result;
}

std::vector<double> sortedIncreasing(const std::vector<double>& x)
{
    auto result = x;
    std::sort(result.begin(), result.end());
    return result;
}

V3 bezierPoint(const std::vector<V3>& cp, double t)
{
    auto work = cp;
    for (int k = 1; k < static_cast<int>(cp.size()); ++k) {
        for (int i = 0; i < static_cast<int>(cp.size()) - k; ++i) {
            work[i] = (1.0 - t) * work[i] + t * work[i + 1];
        }
    }
    return work.front();
}

Curve3 makeBezierCurve(const std::vector<V3>& cp, double lo, double hi)
{
    Bezier3 bezier;
    Eigen::MatrixXd controls(static_cast<int>(cp.size()), 3);
    for (int i = 0; i < static_cast<int>(cp.size()); ++i) controls.row(i) = toRow(cp[i]);
    bezier.set_control_points(controls);
    return Curve3(std::vector<Bezier3>{bezier}, std::vector<double>{lo, hi});
}

Curve3 makeCubicInterpolant(const std::vector<double>& xin, const std::vector<V3>& points)
{
    const int n = static_cast<int>(xin.size());
    std::vector<double> x = xin;
    std::vector<V3> p = points;
    if (x.back() < x.front()) {
        std::reverse(x.begin(), x.end());
        std::reverse(p.begin(), p.end());
    }

    std::vector<V3> delta(n - 1), slopes(n);
    for (int i = 0; i + 1 < n; ++i) delta[i] = (p[i + 1] - p[i]) / (x[i + 1] - x[i]);
    slopes[0] = delta[0];
    slopes[n - 1] = delta[n - 2];
    for (int i = 1; i + 1 < n; ++i) slopes[i] = 0.5 * (delta[i - 1] + delta[i]);

    std::vector<Bezier3> pieces;
    for (int i = 0; i + 1 < n; ++i) {
        const double h = x[i + 1] - x[i];
        Eigen::MatrixXd cp(4, 3);
        cp.row(0) = toRow(p[i]);
        cp.row(1) = toRow(p[i] + h * slopes[i] / 3.0);
        cp.row(2) = toRow(p[i + 1] - h * slopes[i + 1] / 3.0);
        cp.row(3) = toRow(p[i + 1]);
        Bezier3 b;
        b.set_control_points(cp);
        pieces.push_back(std::move(b));
    }
    return Curve3(pieces, x);
}

V3 surfacePoint(double u, double v)
{
    return V3(0.8 * u + 0.25 * std::sin(PI * v) + 0.15 * u * v,
        0.9 * v + 0.20 * std::sin(1.5 * PI * u) + 0.10 * u * v,
        0.55 * std::sin(PI * u) * std::sin(PI * v) + 0.25 * u * v -
            0.15 * u * u + 0.10 * v);
}

struct FixtureOptions {
    int m = 4;
    int n = 4;
    bool reverseSome = false;
    bool insertKnots = false;
    int mixedDirection = 0; // 0 none, 1 horizontal, 2 vertical
};

struct Fixture {
    CurveNetworkSpecification spec;
    std::vector<std::vector<V3>> p; // p[i][j]
    std::vector<RefMap> mapC, mapD;
    std::vector<double> extraCKnot, extraDKnot;
};

Fixture makeFixture(const FixtureOptions& options)
{
    Fixture f;
    const int m = options.m, n = options.n;
    f.spec.vSites = makeSites(m, 0);
    f.spec.uSites = makeSites(n, 1);
    f.spec.a.assign(m, {});
    f.spec.b.assign(n, {});
    f.p.assign(n, std::vector<V3>(m));

    std::vector<double> loC(m), hiC(m), loD(n), hiD(n);
    for (int j = 0; j < m; ++j) {
        loC[j] = -1.6 + 0.37 * j;
        hiC[j] = 2.1 + 0.29 * j;
        const bool rev = options.reverseSome && (j == 1 || j == m - 1);
        f.spec.a[j] = makeParams(n, loC[j], hiC[j], 0.72 + 0.27 * (j % 3), rev);
    }
    for (int i = 0; i < n; ++i) {
        loD[i] = -2.2 + 0.31 * i;
        hiD[i] = 1.7 + 0.33 * i;
        const bool rev = options.reverseSome && (i == 0 || i == n - 2);
        f.spec.b[i] = makeParams(m, loD[i], hiD[i], 0.82 + 0.23 * ((i + 1) % 3), rev);
    }

    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j) f.p[i][j] = surfacePoint(f.spec.uSites[i], f.spec.vSites[j]);

    std::vector<std::vector<V3>> rowControls(m);
    std::vector<int> rowDegree(m, 3);
    if (options.mixedDirection == 1 && m >= 3) {
        rowDegree[0] = 1;
        rowControls[0] = {surfacePoint(0.0, f.spec.vSites[0]) + V3(0, 0, 0.2 * m),
            surfacePoint(1.0, f.spec.vSites[0]) + V3(0.2, 0.1, 0.5)};
        rowDegree[1] = 2;
        const V3 q0 = surfacePoint(0.0, f.spec.vSites[1]);
        const V3 q2 = surfacePoint(1.0, f.spec.vSites[1]);
        rowControls[1] = {q0, 0.5 * (q0 + q2) + V3(0.1, -0.2, 0.9), q2};
    }
    for (int j = 0; j < m; ++j) {
        if (rowDegree[j] == 1 || rowDegree[j] == 2) {
            for (int i = 0; i < n; ++i) {
                const double t = (f.spec.a[j][i] - loC[j]) / (hiC[j] - loC[j]);
                f.p[i][j] = bezierPoint(rowControls[j], t);
            }
        }
    }

    std::vector<std::vector<V3>> colControls(n);
    std::vector<int> colDegree(n, 3);
    if (options.mixedDirection == 2 && n >= 3) {
        colDegree[0] = 1;
        colControls[0] = {surfacePoint(f.spec.uSites[0], 0.0),
            surfacePoint(f.spec.uSites[0], 1.0) + V3(0.1, 0, 0.3)};
        colDegree[1] = 2;
        const V3 q0 = surfacePoint(f.spec.uSites[1], 0.0);
        const V3 q2 = surfacePoint(f.spec.uSites[1], 1.0);
        colControls[1] = {q0, 0.5 * (q0 + q2) + V3(-0.2, 0.15, 0.8), q2};
    }
    for (int i = 0; i < n; ++i) {
        if (colDegree[i] == 1 || colDegree[i] == 2) {
            for (int j = 0; j < m; ++j) {
                const double t = (f.spec.b[i][j] - loD[i]) / (hiD[i] - loD[i]);
                f.p[i][j] = bezierPoint(colControls[i], t);
            }
        }
    }

    f.spec.horizontalCurves.resize(m);
    for (int j = 0; j < m; ++j) {
        if (rowDegree[j] <= 2) {
            f.spec.horizontalCurves[j] = makeBezierCurve(rowControls[j], loC[j], hiC[j]);
        } else {
            std::vector<V3> points(n);
            for (int i = 0; i < n; ++i) points[i] = f.p[i][j];
            f.spec.horizontalCurves[j] = makeCubicInterpolant(f.spec.a[j], points);
        }
    }
    f.spec.verticalCurves.resize(n);
    for (int i = 0; i < n; ++i) {
        if (colDegree[i] <= 2) {
            f.spec.verticalCurves[i] = makeBezierCurve(colControls[i], loD[i], hiD[i]);
        } else {
            std::vector<V3> points(m);
            for (int j = 0; j < m; ++j) points[j] = f.p[i][j];
            f.spec.verticalCurves[i] = makeCubicInterpolant(f.spec.b[i], points);
        }
    }

    if (options.insertKnots) {
        const int j = m >= 4 ? 2 : m - 1;
        auto xs = sortedIncreasing(f.spec.a[j]);
        const double extra = 0.5 * (xs[0] + xs[1]);
        f.spec.horizontalCurves[j].insert_knot(extra, 1);
        f.extraCKnot.push_back(extra);

        const int i = 1 % n;
        auto ys = sortedIncreasing(f.spec.b[i]);
        const double extraD = 0.5 * (ys[0] + ys[1]);
        f.spec.verticalCurves[i].insert_knot(extraD, 1);
        f.extraDKnot.push_back(extraD);
    }

    for (int j = 0; j < m; ++j) f.mapC.push_back(RefMap::build(f.spec.uSites, f.spec.a[j]));
    for (int i = 0; i < n; ++i) f.mapD.push_back(RefMap::build(f.spec.vSites, f.spec.b[i]));
    return f;
}

V3 naturalAt(const std::vector<double>& x, const std::vector<V3>& y, double query)
{
    const int n = static_cast<int>(x.size());
    if (n == 2) return y[0] + (query - x[0]) / (x[1] - x[0]) * (y[1] - y[0]);

    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(n - 2, n - 2);
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(n - 2, 3);
    for (int i = 1; i + 1 < n; ++i) {
        const int r = i - 1;
        const double hm = x[i] - x[i - 1];
        const double hp = x[i + 1] - x[i];
        if (i > 1) A(r, i - 2) = hm;
        A(r, r) = 2.0 * (hm + hp);
        if (i + 1 < n - 1) A(r, i) = hp;
        const V3 dLeft = (y[i] - y[i - 1]) / hm;
        const V3 dRight = (y[i + 1] - y[i]) / hp;
        rhs.row(r) = 6.0 * toRow(dRight - dLeft);
    }
    Eigen::MatrixXd internalM = A.fullPivLu().solve(rhs);
    std::vector<V3> M(n, V3::Zero());
    for (int i = 1; i + 1 < n; ++i) M[i] = V3(internalM.row(i - 1).transpose());

    int k = 0;
    while (k + 2 < n && query > x[k + 1]) ++k;
    const double h = x[k + 1] - x[k];
    const double a = x[k + 1] - query;
    const double b = query - x[k];
    return (M[k] * a * a * a + M[k + 1] * b * b * b) / (6.0 * h) +
           (y[k] - M[k] * h * h / 6.0) * a / h +
           (y[k + 1] - M[k + 1] * h * h / 6.0) * b / h;
}

double checkCurvesAndDerivatives(Fixture& f, const CurveNetwork& network)
{
    double worst = 0.0;
    const int m = f.spec.horizontalCurves.size();
    const int n = f.spec.verticalCurves.size();
    for (int j = 0; j < m; ++j) {
        for (int sample = 0; sample <= 24; ++sample) {
            const double u = (sample + 0.37) / 25.0;
            double mapDerivative = 0.0;
            const double sourceT = f.mapC[j].eval(u, &mapDerivative);
            const V3 expected = toV3(f.spec.horizontalCurves[j].evaluate(sourceT));
            const V3 actual = toV3(network.surface().evaluate(u, f.spec.vSites[j]));
            worst = std::max(worst, (actual - expected).norm());

            const V3 expectedDu =
                toV3(f.spec.horizontalCurves[j].evaluate_derivative(sourceT)) * mapDerivative;
            const V3 actualDu =
                toV3(network.surface().evaluate_derivative_u(u, f.spec.vSites[j]));
            const double derr = (actualDu - expectedDu).norm();
            const double scale = std::max(1.0, expectedDu.norm());
            if (derr > 2e-7 * scale + 2e-7) {
                std::printf("C derivative failure j=%d u=%.17g err=%.3e scale=%.3e\n", j, u,
                    derr, scale);
                return 1e30;
            }
        }
        for (double u : {0.0, 1.0}) {
            double mapDerivative = 0.0;
            const double sourceT = f.mapC[j].eval(u, &mapDerivative);
            const V3 expected = toV3(f.spec.horizontalCurves[j].evaluate(sourceT));
            const V3 actual = toV3(network.surface().evaluate(u, f.spec.vSites[j]));
            worst = std::max(worst, (actual - expected).norm());
            const V3 ed =
                toV3(f.spec.horizontalCurves[j].evaluate_derivative(sourceT)) * mapDerivative;
            const V3 ad = toV3(network.surface().evaluate_derivative_u(u, f.spec.vSites[j]));
            if ((ed - ad).norm() > 2e-7 * std::max(1.0, ed.norm()) + 2e-7) return 1e30;
        }

        for (double knot : f.extraCKnot) {
            const double u0 = f.mapC[j].inverse(knot);
            for (double du : {-2e-4, -3e-6, 0.0, 3e-6, 2e-4}) {
                const double u = std::max(0.0, std::min(1.0, u0 + du));
                double md = 0.0;
                const double s = f.mapC[j].eval(u, &md);
                const V3 expected = toV3(f.spec.horizontalCurves[j].evaluate(s));
                const V3 actual = toV3(network.surface().evaluate(u, f.spec.vSites[j]));
                worst = std::max(worst, (actual - expected).norm());
                const V3 ed = toV3(f.spec.horizontalCurves[j].evaluate_derivative(s)) * md;
                const V3 ad = toV3(network.surface().evaluate_derivative_u(u, f.spec.vSites[j]));
                const double err = (ed - ad).norm();
                if (err > 2e-6 * std::max(1.0, ed.norm()) + 2e-6) {
                    std::printf("C knot derivative failure j=%d u=%.17g err=%.3e\n", j, u, err);
                    return 1e30;
                }
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        for (int sample = 0; sample <= 24; ++sample) {
            const double v = (sample + 0.61) / 25.0;
            double mapDerivative = 0.0;
            const double sourceT = f.mapD[i].eval(v, &mapDerivative);
            const V3 expected = toV3(f.spec.verticalCurves[i].evaluate(sourceT));
            const V3 actual = toV3(network.surface().evaluate(f.spec.uSites[i], v));
            worst = std::max(worst, (actual - expected).norm());

            const V3 expectedDv =
                toV3(f.spec.verticalCurves[i].evaluate_derivative(sourceT)) * mapDerivative;
            const V3 actualDv =
                toV3(network.surface().evaluate_derivative_v(f.spec.uSites[i], v));
            const double derr = (actualDv - expectedDv).norm();
            const double scale = std::max(1.0, expectedDv.norm());
            if (derr > 2e-7 * scale + 2e-7) {
                std::printf("D derivative failure i=%d v=%.17g err=%.3e scale=%.3e\n", i, v,
                    derr, scale);
                return 1e30;
            }
        }
        for (double v : {0.0, 1.0}) {
            double mapDerivative = 0.0;
            const double t = f.mapD[i].eval(v, &mapDerivative);
            const V3 expected = toV3(f.spec.verticalCurves[i].evaluate(t));
            const V3 actual = toV3(network.surface().evaluate(f.spec.uSites[i], v));
            worst = std::max(worst, (actual - expected).norm());
            const V3 ed =
                toV3(f.spec.verticalCurves[i].evaluate_derivative(t)) * mapDerivative;
            const V3 ad = toV3(network.surface().evaluate_derivative_v(f.spec.uSites[i], v));
            if ((ed - ad).norm() > 2e-7 * std::max(1.0, ed.norm()) + 2e-7) return 1e30;
        }

        for (double knot : f.extraDKnot) {
            const double v0 = f.mapD[i].inverse(knot);
            for (double dv : {-2e-4, -3e-6, 0.0, 3e-6, 2e-4}) {
                const double v = std::max(0.0, std::min(1.0, v0 + dv));
                double md = 0.0;
                const double t = f.mapD[i].eval(v, &md);
                const V3 ed = toV3(f.spec.verticalCurves[i].evaluate_derivative(t)) * md;
                const V3 ad = toV3(network.surface().evaluate_derivative_v(f.spec.uSites[i], v));
                if ((ed - ad).norm() > 2e-6 * std::max(1.0, ed.norm()) + 2e-6) return 1e30;
            }
        }
    }
    std::printf("curve/surface worst point error=%.3e\n", worst);
    return worst;
}

} // namespace

static int synchronization_mapping_and_recomposition(void)
{
    FixtureOptions options;
    options.m = 4;
    options.n = 5;
    options.reverseSome = true;
    options.insertKnots = true;
    Fixture f = makeFixture(options);

    for (size_t j = 0; j < f.spec.horizontalCurves.size(); ++j) {
        const MonotoneHermiteMap production =
            MonotoneHermiteMap::build(f.spec.uSites, f.spec.a[j]);
        const RefMap reference = RefMap::build(f.spec.uSites, f.spec.a[j]);
        for (int q = 0; q <= 40; ++q) {
            const double u = q / 40.0;
            double rd = 0.0;
            const double re = reference.eval(u, &rd);
            TASK_CHECK(std::abs(production.evaluate(u) - re) < 1e-13);
            TASK_CHECK(std::abs(production.derivative(u) - rd) < 1e-12);
        }
        if (f.spec.a[j].front() > f.spec.a[j].back()) {
            TASK_CHECK(production.orientation() == -1);
            TASK_CHECK(production.derivative(0.5) < 0.0);
        } else {
            TASK_CHECK(production.orientation() == 1);
            TASK_CHECK(production.derivative(0.5) > 0.0);
        }

        ReparameterizedBSpline reparam(
            f.spec.horizontalCurves[j], f.spec.uSites, f.spec.a[j]);
        TASK_CHECK(reparam.valid());
        TASK_CHECK(reparam.curve().get_degree() >= 3);
        TASK_CHECK(reparam.curve().get_num_control_points() > 8);

        for (int q = 0; q <= 30; ++q) {
            const double u = (q + 0.2) / 31.0;
            double md = 0.0;
            const double sourceT = reference.eval(u, &md);
            const V3 expected = toV3(f.spec.horizontalCurves[j].evaluate(sourceT));
            const V3 actual = toV3(reparam.curve().evaluate(u));
            TASK_CHECK((actual - expected).norm() < 1e-9);

            const V3 ed =
                toV3(f.spec.horizontalCurves[j].evaluate_derivative(sourceT)) * md;
            const V3 ad = toV3(reparam.curve().evaluate_derivative(u));
            TASK_CHECK((ed - ad).norm() < 2e-7 * std::max(1.0, ed.norm()) + 2e-7);
        }

        auto bounds = std::get<1>(f.spec.horizontalCurves[j].convert_to_Bezier());
        for (size_t k = 1; k + 1 < bounds.size(); ++k) {
            const double u = reference.inverse(bounds[k]);
            TASK_CHECK(u > 0.0 && u < 1.0);
            for (double offset : {-1e-5, 1e-5}) {
                const double uu = std::max(0.0, std::min(1.0, u + offset));
                const V3 expected = toV3(
                    f.spec.horizontalCurves[j].evaluate(reference.eval(uu)));
                const V3 actual = toV3(reparam.curve().evaluate(uu));
                TASK_CHECK((actual - expected).norm() < 1e-9);
            }
        }
    }
    return 0;
}

static int synchronization_bidirectional_sites(void)
{
    Fixture f = makeFixture(FixtureOptions{3, 3, true, false, 0});
    CurveNetwork network;
    std::string error;
    TASK_CHECK(network.build(f.spec, &error));
    TASK_CHECK(error.empty());

    for (size_t j = 0; j < f.spec.horizontalCurves.size(); ++j) {
        for (size_t i = 0; i < f.spec.verticalCurves.size(); ++i) {
            const V3 a = toV3(network.surface().evaluate(f.spec.uSites[i], f.spec.vSites[j]));
            const V3 b = f.p[i][j];
            TASK_CHECK((a - b).norm() < 1e-9);
            TASK_CHECK(std::abs(f.mapC[j].eval(f.spec.uSites[i]) - f.spec.a[j][i]) < 1e-14);
            TASK_CHECK(std::abs(f.mapD[i].eval(f.spec.vSites[j]) - f.spec.b[i][j]) < 1e-14);
        }
    }
    return 0;
}

static int surface_affine_network(void)
{
    CurveNetworkSpecification spec;
    spec.uSites = {0.0, 0.35, 0.70, 1.0};
    spec.vSites = {0.0, 0.25, 0.65, 1.0};
    const int n = 4, m = 4;
    spec.a.assign(m, {});
    spec.b.assign(n, {});
    spec.horizontalCurves.resize(m);
    spec.verticalCurves.resize(n);

    const V3 origin(0.4, -1.2, 0.3);
    const V3 eu(2.1, 0.3, -0.4);
    const V3 ev(-0.2, 1.7, 0.8);
    for (int j = 0; j < m; ++j) {
        const double lo = -3.0 + 0.4 * j, hi = 2.0 + 0.2 * j;
        spec.a[j].resize(n);
        for (int i = 0; i < n; ++i) spec.a[j][i] = lo + (hi - lo) * spec.uSites[i];
        spec.horizontalCurves[j] = makeBezierCurve(
            {origin + ev * spec.vSites[j], origin + eu + ev * spec.vSites[j]}, lo, hi);
    }
    for (int i = 0; i < n; ++i) {
        const double lo = -1.0 - 0.3 * i, hi = 4.0 - 0.2 * i;
        spec.b[i].resize(m);
        for (int j = 0; j < m; ++j) spec.b[i][j] = lo + (hi - lo) * spec.vSites[j];
        spec.verticalCurves[i] = makeBezierCurve(
            {origin + eu * spec.uSites[i], origin + eu * spec.uSites[i] + ev}, lo, hi);
    }

    CurveNetwork network;
    std::string affineError;
    const bool affineOk = network.build(spec, &affineError);
    if (!affineOk) std::fprintf(stderr, "affine build: %s\n", affineError.c_str());
    TASK_CHECK(affineOk);
    for (int q = 0; q <= 20; ++q) {
        const double u = (q + 0.31) / 21.0;
        const double v = (q + 0.67) / 21.0;
        const V3 expected = origin + eu * u + ev * v;
        const V3 actual = toV3(network.surface().evaluate(u, v));
        TASK_CHECK((actual - expected).norm() < 1e-8);
        TASK_CHECK((toV3(network.surface().evaluate_derivative_u(u, v)) - eu).norm() < 1e-7);
        TASK_CHECK((toV3(network.surface().evaluate_derivative_v(u, v)) - ev).norm() < 1e-7);
    }

    Patch3 directPatch;
    TASK_CHECK(nanospline::buildCurveNetwork(spec, directPatch));
    TASK_CHECK((toV3(directPatch.evaluate(0.3, 0.7)) - origin - 0.3 * eu - 0.7 * ev).norm() <
                1e-8);
    TASK_CHECK(nanospline::createCurveNetwork(spec).num_control_points_u() > 0);
    return 0;
}

static int surface_gordon_natural_reference(void)
{
    Fixture f = makeFixture(FixtureOptions{3, 3, false, false, 0});
    CurveNetwork network;
    TASK_CHECK(network.build(f.spec));

    const double u = 0.271828;
    const double v = 0.39347;
    std::vector<V3> luValues, luvU;
    for (size_t j = 0; j < f.spec.horizontalCurves.size(); ++j) {
        const double t = f.mapC[j].eval(u);
        luValues.push_back(toV3(f.spec.horizontalCurves[j].evaluate(t)));
        std::vector<V3> column;
        for (size_t i = 0; i < f.spec.verticalCurves.size(); ++i) column.push_back(f.p[i][j]);
        luvU.push_back(naturalAt(f.spec.uSites, column, u));
    }
    std::vector<V3> lvValues;
    for (size_t i = 0; i < f.spec.verticalCurves.size(); ++i) {
        const double t = f.mapD[i].eval(v);
        lvValues.push_back(toV3(f.spec.verticalCurves[i].evaluate(t)));
    }
    const V3 lu = naturalAt(f.spec.vSites, luValues, v);
    const V3 lv = naturalAt(f.spec.uSites, lvValues, u);
    const V3 luv = naturalAt(f.spec.vSites, luvU, v);
    const V3 expected = lu + lv - luv;
    const V3 actual = toV3(network.surface().evaluate(u, v));
    std::printf("Gordon reference err=%.3e luv magnitude=%.3e\n", (actual - expected).norm(),
        luv.norm());
    TASK_CHECK((actual - expected).norm() < 1e-8);
    TASK_CHECK(luv.norm() > 0.05);

    const V3 withoutSubtraction = lu + lv;
    TASK_CHECK((withoutSubtraction - actual).norm() > 1e-3);
    TASK_CHECK(((withoutSubtraction - actual) - luv).norm() < 1e-8);
    return 0;
}

static int surface_full_iso_derivatives_split(void)
{
    Fixture f = makeFixture(FixtureOptions{4, 5, true, true, 0});
    CurveNetwork network;
    std::string error;
    TASK_CHECK(network.build(f.spec, &error));
    TASK_CHECK(error.empty());
    TASK_CHECK(checkCurvesAndDerivatives(f, network) < 1e-8);

    const Patch3& patch = network.surface();
    const double u = 0.371, v = 0.619;
    auto isoU = patch.compute_iso_curve_u(v);
    auto isoV = patch.compute_iso_curve_v(u);
    TASK_CHECK((toV3(isoU.evaluate(u)) - toV3(patch.evaluate(u, v))).norm() < 1e-10);
    TASK_CHECK((toV3(isoV.evaluate(v)) - toV3(patch.evaluate(u, v))).norm() < 1e-10);

    const V3 du = toV3(patch.evaluate_derivative_u(u, v));
    const V3 dv = toV3(patch.evaluate_derivative_v(u, v));
    TASK_CHECK(du.allFinite() && dv.allFinite());
    const double h = 1e-6;
    const V3 fdU = (toV3(patch.evaluate(u + h, v)) - toV3(patch.evaluate(u - h, v))) / (2 * h);
    const V3 fdV = (toV3(patch.evaluate(u, v + h)) - toV3(patch.evaluate(u, v - h))) / (2 * h);
    TASK_CHECK((du - fdU).norm() < 2e-5 * std::max(1.0, fdU.norm()));
    TASK_CHECK((dv - fdV).norm() < 2e-5 * std::max(1.0, fdV.norm()));

    auto partsU = patch.split_u(0.42);
    TASK_CHECK(partsU.size() == 2);
    TASK_CHECK((toV3(partsU[0].evaluate(0.2, v)) - toV3(patch.evaluate(0.2, v))).norm() < 1e-9);
    TASK_CHECK((toV3(partsU[1].evaluate(0.7, v)) - toV3(patch.evaluate(0.7, v))).norm() < 1e-9);

    auto partsV = patch.split_v(0.57);
    TASK_CHECK(partsV.size() == 2);
    TASK_CHECK((toV3(partsV[0].evaluate(u, 0.2)) - toV3(patch.evaluate(u, 0.2))).norm() < 1e-9);
    TASK_CHECK((toV3(partsV[1].evaluate(u, 0.8)) - toV3(patch.evaluate(u, 0.8))).norm() < 1e-9);

    auto four = patch.split(0.42, 0.57);
    TASK_CHECK(four.size() == 4);
    TASK_CHECK(four[0].num_control_points_u() > 0);
    TASK_CHECK(four[0].num_control_points_v() > 0);
    return 0;
}

static int compatibility_mixed_degrees(void)
{
    for (int direction = 1; direction <= 2; ++direction) {
        FixtureOptions options;
        options.m = 4;
        options.n = 4;
        options.reverseSome = false;
        options.insertKnots = true;
        options.mixedDirection = direction;
        Fixture f = makeFixture(options);
        CurveNetwork network;
        std::string error;
        TASK_CHECK(network.build(f.spec, &error));
        TASK_CHECK(error.empty());
        TASK_CHECK(checkCurvesAndDerivatives(f, network) < 1e-8);
        TASK_CHECK(network.surface().get_degree_u() <= 9);
        TASK_CHECK(network.surface().get_degree_v() <= 9);
    }
    return 0;
}

static int compatibility_swap_families(void)
{
    Fixture f = makeFixture(FixtureOptions{4, 3, true, true, 0});
    CurveNetwork original;
    TASK_CHECK(original.build(f.spec));

    CurveNetworkSpecification swapped;
    swapped.horizontalCurves = f.spec.verticalCurves;
    swapped.verticalCurves = f.spec.horizontalCurves;
    swapped.vSites = f.spec.uSites;
    swapped.uSites = f.spec.vSites;
    const int n = f.spec.uSites.size();
    const int m = f.spec.vSites.size();
    swapped.a.assign(n, std::vector<double>(m));
    swapped.b.assign(m, std::vector<double>(n));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j) swapped.a[i][j] = f.spec.b[i][j];
    for (int j = 0; j < m; ++j)
        for (int i = 0; i < n; ++i) swapped.b[j][i] = f.spec.a[j][i];

    CurveNetwork switched;
    TASK_CHECK(switched.build(swapped));
    for (int q = 0; q <= 18; ++q) {
        const double u = (q + 0.2) / 19.0;
        const double v = (q + 0.7) / 19.0;
        const V3 ref = toV3(original.surface().evaluate(u, v));
        const V3 swp = toV3(switched.surface().evaluate(v, u));
        TASK_CHECK((ref - swp).norm() < 2e-8);
    }
    return 0;
}

static int compatibility_count_extremes(void)
{
    const std::pair<int, int> sizes[] = {{2, 2}, {2, 6}, {6, 2}, {5, 5}};
    for (const auto& size : sizes) {
        FixtureOptions options;
        options.m = size.first;
        options.n = size.second;
        options.reverseSome = true;
        options.insertKnots = false;
        Fixture f = makeFixture(options);
        CurveNetwork network;
        std::string error;
        const bool ok = network.build(f.spec, &error);
        if (!ok) std::fprintf(stderr, "count %dx%d: %s\n", size.first, size.second, error.c_str());
        TASK_CHECK(ok);
        TASK_CHECK(checkCurvesAndDerivatives(f, network) < 1e-8);
    }
    return 0;
}

static int compatibility_invalid_retry_and_ownership(void)
{
    Fixture good = makeFixture(FixtureOptions{3, 4, true, true, 0});

    CurveNetwork network;
    CurveNetworkSpecification invalid = good.spec;
    invalid.uSites[1] = invalid.uSites[0];
    std::string error;
    TASK_CHECK(!network.build(invalid, &error));
    TASK_CHECK(!error.empty());
    TASK_CHECK(network.lastErrorCode() == nanospline::CurveNetworkErrorCode::InvalidSites);
    TASK_CHECK(!network.hasSurface());

    CurveNetworkSpecification tooFew = good.spec;
    tooFew.horizontalCurves.resize(1);
    tooFew.a.resize(1);
    TASK_CHECK(!network.build(tooFew, &error));
    TASK_CHECK(network.lastErrorCode() ==
               nanospline::CurveNetworkErrorCode::InvalidStructure);

    CurveNetworkSpecification nonFinite = good.spec;
    nonFinite.uSites[1] = std::numeric_limits<double>::quiet_NaN();
    TASK_CHECK(!network.build(nonFinite, &error));
    TASK_CHECK(network.lastErrorCode() == nanospline::CurveNetworkErrorCode::NonFiniteValue);

    CurveNetworkSpecification wrongShape = good.spec;
    wrongShape.a[0].pop_back();
    TASK_CHECK(!network.build(wrongShape, &error));
    TASK_CHECK(network.lastErrorCode() ==
               nanospline::CurveNetworkErrorCode::InvalidStructure);

    TASK_CHECK(network.build(good.spec, &error));
    TASK_CHECK(error.empty());
    TASK_CHECK(network.hasSurface());
    Eigen::MatrixXd oldGrid = network.surface().get_control_grid();
    const RowV3 oldSample = network.surface().evaluate(0.317, 0.713);

    CurveNetworkSpecification badMonotone = good.spec;
    std::swap(badMonotone.a[0][1], badMonotone.a[0][2]);
    TASK_CHECK(!network.build(badMonotone, &error));
    TASK_CHECK(network.lastErrorCode() ==
               nanospline::CurveNetworkErrorCode::InvalidParameterTable);
    TASK_CHECK(network.hasSurface());
    TASK_CHECK((oldGrid - network.surface().get_control_grid()).cwiseAbs().maxCoeff() == 0.0);
    TASK_CHECK((network.surface().evaluate(0.317, 0.713) - oldSample).norm() < 1e-15);

    CurveNetworkSpecification badIntersection = good.spec;
    badIntersection.horizontalCurves[0].set_control_point(
        0, badIntersection.horizontalCurves[0].get_control_point(0) + RowV3(0.03, 0, 0));
    TASK_CHECK(!network.build(badIntersection, &error));
    TASK_CHECK(network.lastErrorCode() ==
               nanospline::CurveNetworkErrorCode::IncompatibleIntersections);
    TASK_CHECK(network.hasSurface());
    TASK_CHECK((oldGrid - network.surface().get_control_grid()).cwiseAbs().maxCoeff() == 0.0);

    Fixture retry = makeFixture(FixtureOptions{3, 3, false, true, 1});
    TASK_CHECK(network.build(retry.spec));
    TASK_CHECK(network.hasSurface());
    const RowV3 retrySample = network.surface().evaluate(0.283, 0.641);
    const Eigen::MatrixXd retryGrid = network.surface().get_control_grid();

    Patch3 owned = network.surface();
    {
        CurveNetworkSpecification temporary = makeFixture(FixtureOptions{4, 4, true, true, 0}).spec;
        CurveNetwork local;
        TASK_CHECK(local.build(temporary));
        owned = local.surface();
        for (size_t j = 0; j < temporary.horizontalCurves.size(); ++j) {
            for (int k = 0; k < temporary.horizontalCurves[j].get_num_control_points(); ++k) {
                temporary.horizontalCurves[j].set_control_point(k, RowV3::Zero());
            }
        }
        temporary.a[0][0] = 12345.0;
    }
    TASK_CHECK(owned.num_control_points_u() > 1);
    TASK_CHECK(owned.num_control_points_v() > 1);
    TASK_CHECK(owned.get_knots_u().size() > 0 && owned.get_knots_v().size() > 0);
    const RowV3 ownedPoint = owned.evaluate(0.41, 0.63);
    TASK_CHECK(ownedPoint.allFinite());
    auto ownedIso = owned.compute_iso_curve_u(0.63);
    TASK_CHECK((ownedIso.evaluate(0.41) - ownedPoint).norm() < 1e-10);
    auto ownedSplit = owned.split(0.4, 0.6);
    TASK_CHECK(ownedSplit.size() == 4);

    // The built result owns its control net.  Mutating the retry specification
    // after a successful build must not change the surface.
    for (auto& curve : retry.spec.horizontalCurves) {
        for (int k = 0; k < curve.get_num_control_points(); ++k) {
            curve.set_control_point(k, RowV3::Zero());
        }
    }
    TASK_CHECK((retryGrid - network.surface().get_control_grid()).cwiseAbs().maxCoeff() == 0.0);
    TASK_CHECK((network.surface().evaluate(0.283, 0.641) - retrySample).norm() < 1e-15);
    return 0;
}

const struct task_case task_cases[] = {
    {"synchronization", "monotone_map_and_native_composition",
        synchronization_mapping_and_recomposition},
    {"synchronization", "bidirectional_site_table", synchronization_bidirectional_sites},
    {"surface", "affine_gordon_network", surface_affine_network},
    {"surface", "independent_natural_gordon_reference", surface_gordon_natural_reference},
    {"surface", "full_curves_iso_derivatives_split", surface_full_iso_derivatives_split},
    {"compatibility", "mixed_source_degrees", compatibility_mixed_degrees},
    {"compatibility", "reverse_nonunit_domain_swap", compatibility_swap_families},
    {"compatibility", "family_count_extremes_and_linear_axes", compatibility_count_extremes},
    {"compatibility", "invalid_retry_and_ownership",
        compatibility_invalid_retry_and_ownership},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
