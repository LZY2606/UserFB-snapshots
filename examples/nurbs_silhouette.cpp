// Example: draw the silhouette of a sphere (full contour) and its
// visibility intervals (visible arcs vs. occluded arcs) as an SVG drawing.
//
// Usage: nurbs_silhouette <output.svg>
//
// The scene is a unit sphere in the stereographic parametrization
//     S(u,v) = (2u, 2v, u^2+v^2-1) / (1+u^2+v^2),   (u,v) in [-2,2]^2,
// viewed along +Z, with two plane rectangles in front of it: one covers a
// small arc of the contour, the other a narrow band through the middle.
// The full silhouette is drawn as a thin gray line, visible intervals as a
// thick green line and hidden intervals as a dashed red line.

#include <Mathematics/NURBSSilhouette.h>

#include <cmath>
#include <fstream>
#include <memory>
#include <vector>

using V3 = gte::Vector3<double>;
using Surface = gte::NURBSSurface<3, double>;
using Sil = gte::NURBSSilhouette;

namespace
{
    std::shared_ptr<Surface> MakeSphere(V3 center, double radius)
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
                c[static_cast<size_t>(k)] = center + radius
                    * V3{ 2.0 * g[i] / wk, 2.0 * g[j] / wk, (q[i] + q[j] - 1.0) / wk };
                w[static_cast<size_t>(k)] = wk;
            }
        }
        return std::make_shared<Surface>(b, b, c.data(), w.data());
    }

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

    // Project to the view plane (orthographic along +Z) and map to SVG
    // coordinates (y down).
    std::pair<double, double> Project(V3 const& p)
    {
        return { 320.0 + 150.0 * p[0], 320.0 - 150.0 * p[1] };
    }

    void WritePolyline(std::ofstream& out, Sil::Curve const& curve,
        double t0, double t1, char const* style)
    {
        // Sample the parameter interval (wrapping when t0 > t1).
        double length = (t0 <= t1 ? t1 - t0 : t1 + 1.0 - t0);
        int steps = std::max(2, static_cast<int>(length * 400.0));
        out << "<polyline fill=\"none\" " << style << " points=\"";
        for (int i = 0; i <= steps; ++i)
        {
            double t = t0 + length * (static_cast<double>(i) / steps);
            if (t >= 1.0)
            {
                t -= 1.0;
            }
            auto xy = Project(CurvePosition(curve, t));
            out << xy.first << ',' << xy.second << ' ';
        }
        out << "\"/>\n";
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        return 1;
    }

    auto sphere = MakeSphere({ 0, 0, 0 }, 1.0);
    auto rect1 = MakePlane({ 0.85, -0.4, 0.5 }, { 1.5, -0.4, 0.5 },
        { 0.85, 0.4, 0.5 }, { 1.5, 0.4, 0.5 });
    auto rect2 = MakePlane({ -0.04, -1.5, 0.5 }, { 0.04, -1.5, 0.5 },
        { -0.04, 1.5, 0.5 }, { 0.04, 1.5, 0.5 });

    Sil::Options options;
    options.spatialTolerance = 1.0e-6;
    options.parameterTolerance = 1.0e-6;
    options.angleTolerance = 1.0e-6;
    options.regionBudget = 200000;

    Sil silhouette({ sphere, rect1, rect2 }, V3{ 0, 0, 1 }, options);
    if (!silhouette.IsValid())
    {
        return 1;
    }
    Sil::Result result;
    if (!silhouette.Compute(result))
    {
        return 1;
    }

    std::ofstream out(argv[1]);
    if (!out)
    {
        return 1;
    }
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" height=\"640\""
        << " viewBox=\"0 0 640 640\">\n";
    out << "<rect x=\"0\" y=\"0\" width=\"640\" height=\"640\" fill=\"white\"/>\n";

    // Occluder rectangles, projected along the view direction.
    auto drawRect = [&](V3 c00, V3 c10, V3 c11, V3 c01)
    {
        auto p0 = Project(c00);
        auto p1 = Project(c10);
        auto p2 = Project(c11);
        auto p3 = Project(c01);
        out << "<polygon fill=\"none\" stroke=\"#4477cc\" stroke-width=\"1.5\" points=\""
            << p0.first << ',' << p0.second << ' '
            << p1.first << ',' << p1.second << ' '
            << p2.first << ',' << p2.second << ' '
            << p3.first << ',' << p3.second << "\"/>\n";
    };
    drawRect({ 0.85, -0.4, 0.5 }, { 1.5, -0.4, 0.5 }, { 1.5, 0.4, 0.5 }, { 0.85, 0.4, 0.5 });
    drawRect({ -0.04, -1.5, 0.5 }, { 0.04, -1.5, 0.5 }, { 0.04, 1.5, 0.5 }, { -0.04, 1.5, 0.5 });

    for (auto const& curve : result.curves)
    {
        // Full silhouette contour.
        WritePolyline(out, curve, 0.0, 1.0, "stroke=\"#888888\" stroke-width=\"1.2\"");
        // Visibility intervals with distinct line styles.
        for (auto const& iv : curve.intervals)
        {
            if (iv.state == Sil::IntervalState::Visible)
            {
                WritePolyline(out, curve, iv.t0, iv.t1,
                    "stroke=\"#118833\" stroke-width=\"4\"");
            }
            else if (iv.state == Sil::IntervalState::Hidden)
            {
                WritePolyline(out, curve, iv.t0, iv.t1,
                    "stroke=\"#cc2211\" stroke-width=\"4\" stroke-dasharray=\"8 5\"");
            }
            else
            {
                WritePolyline(out, curve, iv.t0, iv.t1,
                    "stroke=\"#dd8811\" stroke-width=\"4\" stroke-dasharray=\"2 4\"");
            }
        }
    }

    out << "</svg>\n";
    return (out ? 0 : 1);
}
