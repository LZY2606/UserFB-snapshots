// Example: build a bidirectional Gordon curve network and tessellate the
// resulting native BSplinePatch as an OBJ mesh with vertex normals.
//
// Usage:
//   curve_network_example output.obj [u_samples] [v_samples]

#include <nanospline/CurveNetwork.h>

#include <Eigen/Dense>

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using Curve3 = nanospline::BSpline<double, 3, -1>;
using Bezier3 = nanospline::Bezier<double, 3, -1>;
using V3 = Eigen::Vector3d;
using RowV3 = Eigen::Matrix<double, 1, 3>;

namespace {

constexpr double PI = 3.14159265358979323846;

RowV3 row(const V3& p) { return RowV3(p.x(), p.y(), p.z()); }

V3 designPoint(double u, double v)
{
    // A regular, visibly non-planar bidirectional design network.
    return V3(
        1.2 * u + 0.35 * std::sin(PI * v) + 0.18 * u * v,
        1.1 * v + 0.28 * std::sin(1.5 * PI * u) + 0.12 * u * v,
        0.75 * std::sin(PI * u) * std::sin(PI * v) + 0.32 * u * v -
            0.18 * u * u + 0.12 * v);
}

Curve3 cubicInterpolant(const std::vector<double>& params, const std::vector<V3>& points)
{
    const int n = static_cast<int>(params.size());
    std::vector<V3> delta(n - 1), tangent(n);
    for (int i = 0; i + 1 < n; ++i) {
        delta[i] = (points[i + 1] - points[i]) / (params[i + 1] - params[i]);
    }
    tangent[0] = delta[0];
    tangent[n - 1] = delta[n - 2];
    for (int i = 1; i + 1 < n; ++i) {
        tangent[i] = 0.5 * (delta[i - 1] + delta[i]);
    }

    std::vector<Bezier3> pieces;
    for (int i = 0; i + 1 < n; ++i) {
        const double h = params[i + 1] - params[i];
        Eigen::MatrixXd cp(4, 3);
        cp.row(0) = row(points[i]);
        cp.row(1) = row(points[i] + h * tangent[i] / 3.0);
        cp.row(2) = row(points[i + 1] - h * tangent[i + 1] / 3.0);
        cp.row(3) = row(points[i + 1]);
        Bezier3 bezier;
        bezier.set_control_points(cp);
        pieces.push_back(bezier);
    }
    return Curve3(pieces, params);
}

std::vector<double> nonlinearParams(
    int count, double lower, double upper, double exponent, bool reversed)
{
    std::vector<double> params(count);
    for (int i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(count - 1);
        params[i] = lower + (upper - lower) * std::pow(t, exponent);
    }
    if (reversed) std::reverse(params.begin(), params.end());
    return params;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string outputPath = argc > 1 ? argv[1] : "curve_network.obj";
    const int uSamples = argc > 2 ? std::max(4, std::stoi(argv[2])) : 65;
    const int vSamples = argc > 3 ? std::max(4, std::stoi(argv[3])) : 49;

    constexpr int m = 4; // Horizontal C curves / V sites.
    constexpr int n = 4; // Vertical D curves / U sites.

    nanospline::CurveNetworkSpecification spec;
    spec.uSites = {0.0, 0.18, 0.53, 1.0};
    spec.vSites = {0.0, 0.27, 0.68, 1.0};
    spec.a.assign(m, {});
    spec.b.assign(n, {});
    spec.horizontalCurves.resize(m);
    spec.verticalCurves.resize(n);

    std::vector<std::vector<V3>> intersections(n, std::vector<V3>(m));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < m; ++j) {
            intersections[i][j] = designPoint(spec.uSites[i], spec.vSites[j]);
        }
    }

    for (int j = 0; j < m; ++j) {
        const double lower = -2.0 + 0.45 * j;
        const double upper = 2.5 + 0.35 * j;
        spec.a[j] = nonlinearParams(n, lower, upper, 0.72 + 0.22 * j, j == 2);

        // The example's interpolation curves use increasing parameters.  For
        // the reversed table row, feed points in source-parameter order.
        std::vector<double> sourceParams = spec.a[j];
        std::vector<V3> sourcePoints(n);
        if (spec.a[j].front() < spec.a[j].back()) {
            for (int i = 0; i < n; ++i) sourcePoints[i] = intersections[i][j];
        } else {
            std::reverse(sourceParams.begin(), sourceParams.end());
            for (int i = 0; i < n; ++i) sourcePoints[i] = intersections[n - 1 - i][j];
        }
        spec.horizontalCurves[j] = cubicInterpolant(sourceParams, sourcePoints);
    }

    for (int i = 0; i < n; ++i) {
        const double lower = -1.7 - 0.38 * i;
        const double upper = 2.2 + 0.30 * i;
        spec.b[i] = nonlinearParams(m, lower, upper, 0.82 + 0.18 * (i + 1), i == 1);

        std::vector<double> sourceParams = spec.b[i];
        std::vector<V3> sourcePoints(m);
        if (spec.b[i].front() < spec.b[i].back()) {
            for (int j = 0; j < m; ++j) sourcePoints[j] = intersections[i][j];
        } else {
            std::reverse(sourceParams.begin(), sourceParams.end());
            for (int j = 0; j < m; ++j) sourcePoints[j] = intersections[i][m - 1 - j];
        }
        spec.verticalCurves[i] = cubicInterpolant(sourceParams, sourcePoints);
    }

    nanospline::CurveNetwork network;
    std::string error;
    if (!network.build(spec, &error)) {
        std::cerr << "Curve network construction failed: " << error << '\n';
        return 1;
    }

    const auto& patch = network.surface();
    std::ofstream out(outputPath);
    if (!out) {
        std::cerr << "Could not open OBJ output path: " << outputPath << '\n';
        return 2;
    }
    out.precision(12);

    const double uMin = patch.get_u_lower_bound();
    const double uMax = patch.get_u_upper_bound();
    const double vMin = patch.get_v_lower_bound();
    const double vMax = patch.get_v_upper_bound();

    for (int iu = 0; iu < uSamples; ++iu) {
        const double fu = static_cast<double>(iu) / (uSamples - 1);
        const double u = uMin + (uMax - uMin) * fu;
        for (int iv = 0; iv < vSamples; ++iv) {
            const double fv = static_cast<double>(iv) / (vSamples - 1);
            const double v = vMin + (vMax - vMin) * fv;
            const RowV3 p = patch.evaluate(u, v);
            out << "v " << p.x() << ' ' << p.y() << ' ' << p.z() << '\n';
        }
    }

    for (int iu = 0; iu < uSamples; ++iu) {
        const double fu = static_cast<double>(iu) / (uSamples - 1);
        const double u = uMin + (uMax - uMin) * fu;
        for (int iv = 0; iv < vSamples; ++iv) {
            const double fv = static_cast<double>(iv) / (vSamples - 1);
            const double v = vMin + (vMax - vMin) * fv;
            const V3 du = V3(patch.evaluate_derivative_u(u, v));
            const V3 dv = V3(patch.evaluate_derivative_v(u, v));
            V3 normal = du.cross(dv);
            if (!normal.allFinite() || normal.norm() < 1e-12) {
                normal = V3(0.0, 0.0, 1.0);
            } else {
                normal.normalize();
            }
            out << "vn " << normal.x() << ' ' << normal.y() << ' ' << normal.z() << '\n';
        }
    }

    for (int iu = 0; iu + 1 < uSamples; ++iu) {
        for (int iv = 0; iv + 1 < vSamples; ++iv) {
            const int a = iu * vSamples + iv + 1;
            const int b = (iu + 1) * vSamples + iv + 1;
            const int c = (iu + 1) * vSamples + iv + 2;
            const int d = iu * vSamples + iv + 2;
            out << "f " << a << "//" << a << ' '
                << b << "//" << b << ' '
                << c << "//" << c << ' '
                << d << "//" << d << '\n';
        }
    }

    std::cout << "Wrote " << uSamples << 'x' << vSamples
              << " native BSplinePatch samples to " << outputPath << '\n';
    return out.good() ? 0 : 3;
}
