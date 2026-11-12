// Regression tests for the pressure-aware stroke pipeline.
//
// All geometric checks use independently implemented Bernstein evaluation,
// derivatives and point-to-polyline distances rather than the production
// sampling code. No main() is defined here; task_support/runner.cpp drives
// the registered cases.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "registry.hpp"
#include "agg_pressure_path.h"
#include "agg_conv_pressure_stroke.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_scanline_u.h"
#include "agg_renderer_scanline.h"
#include "agg_pixfmt_gray.h"
#include "agg_renderer_base.h"
#include "agg_rendering_buffer.h"

namespace
{
    using agg::pressure_path;
    using agg::conv_pressure_stroke;

    struct p2 { double x, y; };

    //-------------------------------------------------------------test math
    double vec_len(double x, double y) { return std::sqrt(x * x + y * y); }

    double dist_point_segment(p2 p, p2 a, p2 b)
    {
        double dx = b.x - a.x, dy = b.y - a.y;
        double dd = dx * dx + dy * dy;
        if(dd <= 0.0) return vec_len(p.x - a.x, p.y - a.y);
        double u = ((p.x - a.x) * dx + (p.y - a.y) * dy) / dd;
        if(u < 0.0) u = 0.0;
        if(u > 1.0) u = 1.0;
        double qx = a.x + u * dx, qy = a.y + u * dy;
        return vec_len(p.x - qx, p.y - qy);
    }

    double dist_point_polyline(p2 p, const std::vector<p2>& poly)
    {
        if(poly.empty()) throw std::runtime_error("empty polyline");
        double best = 1e300;
        for(unsigned i = 0; i + 1 < poly.size(); ++i)
        {
            double d = dist_point_segment(p, poly[i], poly[i + 1]);
            if(d < best) best = d;
        }
        return best;
    }

    // Independent Bernstein basis (cubic and quadratic).
    void basis3(double t, double b[4])
    {
        double u = 1.0 - t;
        b[0] = u * u * u;
        b[1] = 3.0 * u * u * t;
        b[2] = 3.0 * u * t * t;
        b[3] = t * t * t;
    }

    void basis2(double t, double b[3])
    {
        double u = 1.0 - t;
        b[0] = u * u;
        b[1] = 2.0 * u * t;
        b[2] = t * t;
    }

    struct bezier3
    {
        p2    p[4];
        double w[4];
    };

    p2 b3_position(const bezier3& b, double t)
    {
        double k[4]; basis3(t, k);
        p2 r = {0.0, 0.0};
        for(int i = 0; i < 4; ++i) { r.x += k[i] * b.p[i].x; r.y += k[i] * b.p[i].y; }
        return r;
    }

    double b3_width(const bezier3& b, double t)
    {
        double k[4]; basis3(t, k);
        double r = 0.0;
        for(int i = 0; i < 4; ++i) r += k[i] * b.w[i];
        return r;
    }

    p2 b3_derivative(const bezier3& b, double t)
    {
        double u = 1.0 - t;
        p2 r = {0.0, 0.0};
        r.x = 3.0 * (u * u * (b.p[1].x - b.p[0].x) +
                     2.0 * u * t * (b.p[2].x - b.p[1].x) +
                     t * t * (b.p[3].x - b.p[2].x));
        r.y = 3.0 * (u * u * (b.p[1].y - b.p[0].y) +
                     2.0 * u * t * (b.p[2].y - b.p[1].y) +
                     t * t * (b.p[3].y - b.p[2].y));
        return r;
    }

    // True Q+/- boundary points. At a legal stall endpoint the one-sided
    // limit tangent is taken from a parameter a hair inside the segment.
    void b3_boundary(const bezier3& b, double t, p2& ql, p2& qr)
    {
        p2 p = b3_position(b, t);
        double w = b3_width(b, t);
        p2 d = b3_derivative(b, t);
        double eps = 1e-9;
        if(vec_len(d.x, d.y) == 0.0)
            d = b3_derivative(b, t == 0.0 ? eps : 1.0 - eps);
        double len = vec_len(d.x, d.y);
        double nx = -d.y / len, ny = d.x / len;   // unit left normal
        double h = 0.5 * w;
        ql.x = p.x + nx * h; ql.y = p.y + ny * h;
        qr.x = p.x - nx * h; qr.y = p.y - ny * h;
    }

    struct bezier2
    {
        p2    p[3];
        double w[3];
    };

    p2 b2_position(const bezier2& b, double t)
    {
        double k[3]; basis2(t, k);
        p2 r = {0.0, 0.0};
        for(int i = 0; i < 3; ++i) { r.x += k[i] * b.p[i].x; r.y += k[i] * b.p[i].y; }
        return r;
    }

    double b2_width(const bezier2& b, double t)
    {
        double k[3]; basis2(t, k);
        double r = 0.0;
        for(int i = 0; i < 3; ++i) r += k[i] * b.w[i];
        return r;
    }

    p2 b2_derivative(const bezier2& b, double t)
    {
        p2 r;
        r.x = 2.0 * ((1.0 - t) * (b.p[1].x - b.p[0].x) +
                     t * (b.p[2].x - b.p[1].x));
        r.y = 2.0 * ((1.0 - t) * (b.p[1].y - b.p[0].y) +
                     t * (b.p[2].y - b.p[1].y));
        return r;
    }

    void b2_boundary(const bezier2& b, double t, p2& ql, p2& qr)
    {
        p2 p = b2_position(b, t);
        double w = b2_width(b, t);
        p2 d = b2_derivative(b, t);
        double len = vec_len(d.x, d.y);
        double nx = -d.y / len, ny = d.x / len;
        double h = 0.5 * w;
        ql.x = p.x + nx * h; ql.y = p.y + ny * h;
        qr.x = p.x - nx * h; qr.y = p.y - ny * h;
    }

    //-----------------------------------------------------stream extraction
    struct out_vertex { double x, y; unsigned cmd; };

    std::vector<out_vertex> read_stream(conv_pressure_stroke<pressure_path>& s)
    {
        std::vector<out_vertex> v;
        double x, y;
        for(unsigned count = 0; count < 1000000; ++count)
        {
            unsigned cmd = s.vertex(&x, &y);
            if(agg::is_stop(cmd)) return v;
            if(agg::is_vertex(cmd))
            {
                if(!std::isfinite(x) || !std::isfinite(y))
                    throw std::runtime_error("non-finite output vertex");
            }
            v.push_back({x, y, cmd});
        }
        throw std::runtime_error("unbounded vertex stream");
    }

    // All line_to vertices of one closed contour (contours are separated by
    // end_poly commands).
    std::vector<std::vector<p2> > extract_contours(
        const std::vector<out_vertex>& stream)
    {
        std::vector<std::vector<p2> > all;
        std::vector<p2> cur;
        for(const auto& v : stream)
        {
            if(agg::is_end_poly(v.cmd))
            {
                if(!cur.empty()) all.push_back(cur);
                cur.clear();
            }
            else if(agg::is_vertex(v.cmd))
                cur.push_back({v.x, v.y});
        }
        if(!cur.empty()) all.push_back(cur);
        return all;
    }

    // Flatten every contour into one closed polyline (append the first
    // point again so segment-based distance covers the closing edge).
    std::vector<p2> contour_closed_polyline(const std::vector<p2>& c)
    {
        std::vector<p2> r = c;
        if(!r.empty()) r.push_back(r.front());
        return r;
    }

    double signed_area(const std::vector<p2>& c)
    {
        double a = 0.0;
        for(unsigned i = 0; i < c.size(); ++i)
        {
            const p2& q = c[(i + 1) % c.size()];
            a += c[i].x * q.y - q.x * c[i].y;
        }
        return 0.5 * a;
    }

    // Two-way Hausdorff-style check between a densely sampled true side
    // boundary and the output contour.
    void check_boundary_two_way(task::Case& c,
                                const std::vector<p2>& truth,
                                const std::vector<p2>& poly,
                                double tol, const std::string& label)
    {
        double slack = tol + 1e-7;
        double worst_truth = 0.0, worst_poly = 0.0;
        for(const p2& t : truth)
        {
            double d = dist_point_polyline(t, poly);
            if(d > worst_truth) worst_truth = d;
        }
        for(const p2& t : poly)
        {
            double d = dist_point_polyline(t, truth);
            if(d > worst_poly) worst_poly = d;
        }
        c.check(worst_truth <= slack,
                label + ": true boundary deviates from output polyline [" +
                std::to_string(worst_truth) + "]");
        c.check(worst_poly <= slack,
                label + ": output polyline deviates from true boundary [" +
                std::to_string(worst_poly) + "]");
    }

    // Independent construction of the complete true open outline:
    // Q+ forward, end cap, Q- backward, start cap, closed.
    template<class B>
    std::vector<p2> true_open_outline(const B& b,
                                      agg::line_cap_e cap, double tol,
                                      void (*boundary)(const B&, double, p2&, p2&),
                                      p2 (*position)(const B&, double),
                                      p2 (*derivative)(const B&, double),
                                      double (*width)(const B&, double))
    {
        const unsigned n = 2000;
        std::vector<p2> r;
        auto push = [&](p2 q) {
            if(r.empty() || vec_len(r.back().x - q.x, r.back().y - q.y) > 0.0)
                r.push_back(q);
        };
        auto frame = [&](double t) {
            p2 pc = position(b, t);
            p2 d = derivative(b, t);
            if(vec_len(d.x, d.y) == 0.0)
                d = derivative(b, t == 0.0 ? 1e-9 : 1.0 - 1e-9);
            double len = vec_len(d.x, d.y);
            p2 tang = {d.x / len, d.y / len};
            p2 normal = {-tang.y, tang.x};
            double h = 0.5 * width(b, t);
            return std::make_tuple(pc, tang, normal, h);
        };
        for(unsigned i = 0; i <= n; ++i)
        {
            p2 ql, qr; boundary(b, double(i) / n, ql, qr);
            push(ql);
        }
        // End cap (t = 1): outward is +tangent.
        {
            auto [pc, tang, normal, h] = frame(1.0);
            p2 ql, qr; boundary(b, 1.0, ql, qr);
            if(cap == agg::square_cap)
            {
                push({ql.x + tang.x * h, ql.y + tang.y * h});
                push({qr.x + tang.x * h, qr.y + tang.y * h});
            }
            else if(cap == agg::round_cap && h > 0.0)
            {
                unsigned steps = 48;
                for(unsigned k = 1; k < steps; ++k)
                {
                    double phi = agg::pi * double(k) / steps;
                    double cf = std::cos(phi), sf = std::sin(phi);
                    push({pc.x + h * (normal.x * cf + tang.x * sf),
                          pc.y + h * (normal.y * cf + tang.y * sf)});
                }
            }
        }
        for(int i = int(n); i >= 0; --i)
        {
            p2 ql, qr; boundary(b, double(i) / n, ql, qr);
            push(qr);
        }
        // Start cap (t = 0): outward is -tangent, traversed qr -> ql.
        {
            auto [pc, tang, normal, h] = frame(0.0);
            p2 ql, qr; boundary(b, 0.0, ql, qr);
            if(cap == agg::square_cap)
            {
                push({qr.x - tang.x * h, qr.y - tang.y * h});
                push({ql.x - tang.x * h, ql.y - tang.y * h});
            }
            else if(cap == agg::round_cap && h > 0.0)
            {
                unsigned steps = 48;
                for(unsigned k = 1; k < steps; ++k)
                {
                    double phi = agg::pi * double(k) / steps;
                    double cf = std::cos(phi), sf = std::sin(phi);
                    push({pc.x + h * (-normal.x * cf - tang.x * sf),
                          pc.y + h * (-normal.y * cf - tang.y * sf)});
                }
            }
        }
        push(r.front());
        (void)tol;
        return r;
    }

    std::vector<p2> cubic_true_outline(const bezier3& b, agg::line_cap_e cap,
                                       double tol)
    {
        return true_open_outline<bezier3>(b, cap, tol, b3_boundary,
                                          b3_position, b3_derivative, b3_width);
    }

    std::vector<p2> quad_true_outline(const bezier2& b, agg::line_cap_e cap,
                                      double tol)
    {
        return true_open_outline<bezier2>(b, cap, tol, b2_boundary,
                                          b2_position, b2_derivative, b2_width);
    }

    //-----------------------------------------------------------profile group
    TASK_CASE("profile", pressure_path_roundtrip)
    {
        pressure_path p;
        p.move_to(1.0, 2.0, 3.0);
        p.line_to(4.0, 5.0, 6.0);
        p.curve3(7.0, 8.0, 9.0, 10.0, 11.0, 12.0);
        p.curve4(13.0, 14.0, 15.0, 16.0, 17.0, 18.0,
                 19.0, 20.0, 21.0);
        p.close_polygon();

        p.rewind(0);
        double x, y, w;
        unsigned cmd = p.vertex(&x, &y, &w);
        c.check(cmd == agg::path_cmd_move_to && x == 1.0 && y == 2.0 && w == 3.0,
                "move_to roundtrip");
        cmd = p.vertex(&x, &y, &w);
        c.check(cmd == agg::path_cmd_line_to && x == 4.0 && y == 5.0 && w == 6.0,
                "line_to roundtrip");
        cmd = p.vertex(&x, &y, &w);
        c.check(cmd == agg::path_cmd_curve3 && x == 7.0 && y == 8.0 && w == 9.0,
                "curve3 control point roundtrip");
        cmd = p.vertex(&x, &y, &w);
        c.check(cmd == agg::path_cmd_curve3 && x == 10.0 && y == 11.0 && w == 12.0,
                "curve3 end point roundtrip");
        cmd = p.vertex(&x, &y, &w);
        c.check(cmd == agg::path_cmd_curve4 && w == 15.0,
                "curve4 first control roundtrip");
        cmd = p.vertex(&x, &y, &w);
        c.check(cmd == agg::path_cmd_curve4 && w == 18.0,
                "curve4 second control roundtrip");
        cmd = p.vertex(&x, &y, &w);
        c.check(cmd == agg::path_cmd_curve4 && w == 21.0,
                "curve4 end roundtrip");
        cmd = p.vertex(&x, &y, &w);
        c.check(agg::is_close(cmd), "close polygon emitted");
        cmd = p.vertex(&x, &y, &w);
        c.check(agg::is_stop(cmd), "stop after close");
    }

    TASK_CASE("profile", pressure_path_validation_and_paths)
    {
        pressure_path p;
        bool threw = false;
        try { p.move_to(std::numeric_limits<double>::infinity(), 0.0, 1.0); }
        catch(const std::invalid_argument&) { threw = true; }
        c.check(threw, "non-finite coordinate rejected");

        threw = false;
        try { p.line_to(0.0, 0.0, -0.25); }
        catch(const std::invalid_argument&) { threw = true; }
        c.check(threw, "negative width rejected");

        threw = false;
        try { p.curve4(0.0, 0.0, 1.0, 0.0, 0.0, std::nan(""),
                       1.0, 1.0, 1.0); }
        catch(const std::invalid_argument&) { threw = true; }
        c.check(threw, "non-finite control width rejected");

        p.remove_all();
        p.move_to(0.0, 0.0, 1.0);
        p.line_to(1.0, 0.0, 1.0);
        unsigned id2 = p.start_new_path();
        c.check(id2 > 0, "start_new_path returns a new id");
        p.move_to(5.0, 5.0, 2.0);
        p.line_to(6.0, 5.0, 2.0);

        p.rewind(0);
        double x, y, w;
        c.check(p.vertex(&x, &y, &w) == agg::path_cmd_move_to, "path 0 move");
        c.check(p.vertex(&x, &y, &w) == agg::path_cmd_line_to, "path 0 line");
        c.check(agg::is_stop(p.vertex(&x, &y, &w)), "stop separator");

        p.rewind(id2);
        unsigned cmd = p.vertex(&x, &y, &w);
        c.check(cmd == agg::path_cmd_move_to && x == 5.0 && y == 5.0 && w == 2.0,
                "rewind(path_id) reaches the second subpath");
        c.check(p.vertex(&x, &y, &w) == agg::path_cmd_line_to, "path 1 line");
        c.check(agg::is_stop(p.vertex(&x, &y, &w)), "path 1 stop");

        p.remove_all();
        p.rewind(0);
        c.check(agg::is_stop(p.vertex(&x, &y, &w)), "remove_all empties storage");
    }

    TASK_CASE("profile", pressure_bernstein_width_retained)
    {
        // Straight centerline, equal endpoint widths but a large control
        // pressure. Linear endpoint interpolation would collapse to a
        // constant width; the shared Bernstein basis must keep the bulge.
        pressure_path p;
        p.move_to(0.0, 0.0, 4.0);
        p.curve4(10.0 / 3.0, 0.0, 20.0,
                 20.0 / 3.0, 0.0, 20.0,
                 10.0, 0.0, 4.0);

        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(0.05);
        s.rewind(0);
        auto stream = read_stream(s);
        auto contours = extract_contours(stream);
        c.check(contours.size() == 1, "one open outline contour");
        auto poly = contour_closed_polyline(contours[0]);

        double max_y = -1e300, min_y = 1e300;
        for(const p2& v : poly) { if(v.y > max_y) max_y = v.y; if(v.y < min_y) min_y = v.y; }
        // Bernstein width at t=0.5 is (4+3*20+3*20+4)/8 = 16 -> half 8.
        c.check(max_y > 7.5 && min_y < -7.5,
                "control-point pressure bulges both boundaries");
        c.check(max_y < 8.5 && min_y > -8.5,
                "bulge matches the Bernstein width, not endpoint interpolation");

        p2 mid_top = {5.0, 8.0};
        c.check(dist_point_polyline(mid_top, poly) <= 0.05 + 1e-7,
                "Q+ at t=0.5 represented within tolerance");
        p2 mid_bottom = {5.0, -8.0};
        c.check(dist_point_polyline(mid_bottom, poly) <= 0.05 + 1e-7,
                "Q- at t=0.5 represented within tolerance");
    }

    //-------------------------------------------------------------offset group
    TASK_CASE("offset", cubic_boundary_two_way_tolerance)
    {
        pressure_path p;
        bezier3 b;
        b.p[0] = {2.0, 10.0};  b.w[0] = 4.0;
        b.p[1] = {20.0, 45.0}; b.w[1] = 14.0;
        b.p[2] = {55.0, -20.0}; b.w[2] = 2.0;
        b.p[3] = {72.0, 30.0};  b.w[3] = 10.0;
        p.move_to(b.p[0].x, b.p[0].y, b.w[0]);
        p.curve4(b.p[1].x, b.p[1].y, b.w[1],
                 b.p[2].x, b.p[2].y, b.w[2],
                 b.p[3].x, b.p[3].y, b.w[3]);

        const double tol = 0.07;
        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(tol);
        s.rewind(0);
        auto contours = extract_contours(read_stream(s));
        c.check(contours.size() == 1, "cubic produces one contour");
        auto poly = contour_closed_polyline(contours[0]);

        check_boundary_two_way(c, cubic_true_outline(b, agg::butt_cap, tol),
                               poly, tol, "cubic full outline");
    }

    TASK_CASE("offset", quadratic_boundary_two_way_tolerance)
    {
        pressure_path p;
        bezier2 b;
        b.p[0] = {4.0, 60.0}; b.w[0] = 12.0;
        b.p[1] = {40.0, 5.0}; b.w[1] = 1.0;
        b.p[2] = {76.0, 55.0}; b.w[2] = 9.0;
        p.move_to(b.p[0].x, b.p[0].y, b.w[0]);
        p.curve3(b.p[1].x, b.p[1].y, b.w[1],
                 b.p[2].x, b.p[2].y, b.w[2]);

        const double tol = 0.05;
        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(tol);
        s.rewind(0);
        auto contours = extract_contours(read_stream(s));
        auto poly = contour_closed_polyline(contours[0]);

        check_boundary_two_way(c, quad_true_outline(b, agg::butt_cap, tol),
                               poly, tol, "quad full outline");
    }

    TASK_CASE("offset", same_endpoint_loop_preserved)
    {
        // A loop that returns to its starting point: both sides fold back,
        // the directed outline is left to the non-zero fill rule.
        pressure_path p;
        bezier3 b;
        b.p[0] = {40.0, 60.0}; b.w[0] = 6.0;
        b.p[1] = {5.0, 5.0};    b.w[1] = 18.0;
        b.p[2] = {75.0, 5.0};   b.w[2] = 10.0;
        b.p[3] = {40.0, 60.0};  b.w[3] = 6.0;
        p.move_to(b.p[0].x, b.p[0].y, b.w[0]);
        p.curve4(b.p[1].x, b.p[1].y, b.w[1],
                 b.p[2].x, b.p[2].y, b.w[2],
                 b.p[3].x, b.p[3].y, b.w[3]);

        // No interior stall: the only derivative zero is excluded by design.
        p2 dmid = b3_derivative(b, 0.5);
        c.check(vec_len(dmid.x, dmid.y) > 1.0, "loop reference has no mid stall");

        const double tol = 0.05;
        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(tol);
        s.rewind(0);
        auto contours = extract_contours(read_stream(s));
        c.check(contours.size() == 1, "loop stays one directed contour");
        c.check(contours[0].size() >= 8, "loop boundary is retained, not flattened");
        auto poly = contour_closed_polyline(contours[0]);
        check_boundary_two_way(c, cubic_true_outline(b, agg::butt_cap, tol),
                               poly, tol, "loop full outline");
    }

    TASK_CASE("offset", inflection_curve_preserved)
    {
        pressure_path p;
        bezier3 b;
        b.p[0] = {4.0, 4.0};   b.w[0] = 4.0;
        b.p[1] = {64.0, 4.0};  b.w[1] = 12.0;
        b.p[2] = {4.0, 64.0};  b.w[2] = 2.0;
        b.p[3] = {64.0, 64.0}; b.w[3] = 8.0;
        p.move_to(b.p[0].x, b.p[0].y, b.w[0]);
        p.curve4(b.p[1].x, b.p[1].y, b.w[1],
                 b.p[2].x, b.p[2].y, b.w[2],
                 b.p[3].x, b.p[3].y, b.w[3]);

        const double tol = 0.05;
        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(tol);
        s.rewind(0);
        auto contours = extract_contours(read_stream(s));
        auto poly = contour_closed_polyline(contours[0]);
        check_boundary_two_way(c, cubic_true_outline(b, agg::butt_cap, tol),
                               poly, tol, "inflection full outline");
    }

    TASK_CASE("offset", short_wide_curve_preserved)
    {
        pressure_path p;
        bezier3 b;
        b.p[0] = {30.0, 40.0}; b.w[0] = 40.0;
        b.p[1] = {30.5, 62.0}; b.w[1] = 44.0;
        b.p[2] = {31.5, 18.0}; b.w[2] = 36.0;
        b.p[3] = {32.0, 40.0}; b.w[3] = 40.0;
        p.move_to(b.p[0].x, b.p[0].y, b.w[0]);
        p.curve4(b.p[1].x, b.p[1].y, b.w[1],
                 b.p[2].x, b.p[2].y, b.w[2],
                 b.p[3].x, b.p[3].y, b.w[3]);

        const double tol = 0.1;
        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(tol);
        s.rewind(0);
        auto contours = extract_contours(read_stream(s));
        c.check(contours.size() == 1, "short wide curve gives a contour");
        c.check(contours[0].size() >= 8, "short wide curve keeps its bulge");
        auto poly = contour_closed_polyline(contours[0]);
        check_boundary_two_way(c, cubic_true_outline(b, agg::butt_cap, tol),
                               poly, tol, "short wide full outline");
    }

    TASK_CASE("offset", zero_derivative_endpoint_uses_limit_tangent)
    {
        pressure_path p;
        bezier3 b;
        b.p[0] = {10.0, 10.0}; b.w[0] = 4.0;
        b.p[1] = {10.0, 10.0}; b.w[1] = 8.0;   // P'(0) = 0
        b.p[2] = {30.0, 30.0}; b.w[2] = 12.0;
        b.p[3] = {40.0, 10.0}; b.w[3] = 4.0;
        p.move_to(b.p[0].x, b.p[0].y, b.w[0]);
        p.curve4(b.p[1].x, b.p[1].y, b.w[1],
                 b.p[2].x, b.p[2].y, b.w[2],
                 b.p[3].x, b.p[3].y, b.w[3]);

        const double tol = 0.05;
        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(tol);
        s.rewind(0);
        auto contours = extract_contours(read_stream(s));
        c.check(contours.size() == 1, "stall endpoint still produces a contour");
        auto poly = contour_closed_polyline(contours[0]);
        check_boundary_two_way(c, cubic_true_outline(b, agg::butt_cap, tol),
                               poly, tol, "stall endpoint full outline");

        // The limit tangent at t=0 is normalize(P2-P0) = (1,1)/sqrt(2);
        // the first boundary points must lie on the corresponding normal.
        p2 ql0, qr0;
        b3_boundary(b, 0.0, ql0, qr0);
        c.check(dist_point_polyline(ql0, poly) <= tol + 1e-7,
                "limit-tangent Q+ start point");
        c.check(dist_point_polyline(qr0, poly) <= tol + 1e-7,
                "limit-tangent Q- start point");
    }

    TASK_CASE("offset", degenerate_inputs_rejected)
    {
        {
            pressure_path p;
            p.move_to(0.0, 0.0, 4.0);
            p.line_to(0.0, 0.0, 4.0);          // zero-length line
            conv_pressure_stroke<pressure_path> s(p);
            bool threw = false;
            try { s.rewind(0); read_stream(s); }
            catch(const std::invalid_argument&) { threw = true; }
            c.check(threw, "zero-length line rejected");
        }
        {
            pressure_path p;
            p.move_to(5.0, 5.0, 4.0);
            p.curve4(5.0, 5.0, 8.0, 5.0, 5.0, 12.0, 5.0, 5.0, 6.0);
            conv_pressure_stroke<pressure_path> s(p);
            bool threw = false;
            try { s.rewind(0); read_stream(s); }
            catch(const std::invalid_argument&) { threw = true; }
            c.check(threw, "constant centerline curve rejected");
        }
        {
            // P'(0.5) = 0 in the interior.
            pressure_path p;
            p.move_to(0.0, 0.0, 4.0);
            p.curve4(40.0, 40.0, 4.0, 0.0, 40.0, 4.0, 40.0, 0.0, 4.0);
            conv_pressure_stroke<pressure_path> s(p);
            bool threw = false;
            try { s.rewind(0); read_stream(s); }
            catch(const std::invalid_argument&) { threw = true; }
            c.check(threw, "interior zero-speed point rejected");
        }
        {
            pressure_path p;
            p.move_to(0.0, 0.0, 4.0);
            p.line_to(10.0, 0.0, 4.0);
            conv_pressure_stroke<pressure_path> s(p);
            bool threw = false;
            try { s.tolerance(0.0); }
            catch(const std::invalid_argument&) { threw = true; }
            c.check(threw, "zero tolerance rejected");
            threw = false;
            try { s.tolerance(std::nan("")); }
            catch(const std::invalid_argument&) { threw = true; }
            c.check(threw, "nan tolerance rejected");
            threw = false;
            try { s.miter_limit(0.5); }
            catch(const std::invalid_argument&) { threw = true; }
            c.check(threw, "sub-one miter limit rejected");
            threw = false;
            try { s.line_join(agg::round_join); }
            catch(const std::invalid_argument&) { threw = true; }
            c.check(threw, "unsupported join rejected");
            threw = false;
            try { s.line_cap(agg::line_cap_e(99)); }
            catch(const std::invalid_argument&) { threw = true; }
            c.check(threw, "unsupported cap rejected");
        }
    }

    TASK_CASE("offset", rewind_tolerance_and_attach_regenerate)
    {
        pressure_path p;
        p.move_to(0.0, 20.0, 4.0);
        p.curve4(20.0, 60.0, 10.0, 40.0, -20.0, 6.0, 60.0, 30.0, 8.0);

        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(0.1);
        s.rewind(0);
        auto a = read_stream(s);
        s.rewind(0);
        auto b = read_stream(s);
        c.check(a.size() == b.size(), "repeat rewind: same size");
        bool same = true;
        for(unsigned i = 0; i < a.size(); ++i)
            if(a[i].cmd != b[i].cmd || a[i].x != b[i].x || a[i].y != b[i].y)
                same = false;
        c.check(same, "repeat rewind: identical stream");

        s.tolerance(0.01);
        s.rewind(0);
        auto fine = read_stream(s);
        c.check(fine.size() > a.size(),
                "tighter tolerance regenerates a denser outline");

        pressure_path q;
        q.move_to(100.0, 100.0, 2.0);
        q.line_to(110.0, 100.0, 2.0);
        s.attach(q);
        s.rewind(0);
        auto attached = read_stream(s);
        for(const auto& v : attached)
            if(agg::is_vertex(v.cmd) && (v.x < 90.0 || v.y < 90.0))
                c.check(false, "attached source: old contour leaked");
        c.check(!attached.empty(), "attached source produces its own contour");

        // Editing the source then rewinding reads the new input.
        q.remove_all();
        q.move_to(-50.0, -50.0, 3.0);
        q.line_to(-40.0, -50.0, 3.0);
        s.rewind(0);
        auto edited = read_stream(s);
        bool saw_new = false, saw_old = false;
        for(const auto& v : edited)
        {
            if(agg::is_vertex(v.cmd))
            {
                if(v.x < 0.0 && v.y < 0.0) saw_new = true;
                if(v.x > 90.0) saw_old = true;
            }
        }
        c.check(saw_new && !saw_old, "edited source is read after rewind");
    }

    TASK_CASE("offset", zero_width_path_has_no_coverage)
    {
        pressure_path p;
        p.move_to(0.0, 0.0, 0.0);
        p.curve4(10.0, 10.0, 0.0, 20.0, -10.0, 0.0, 30.0, 0.0, 0.0);
        conv_pressure_stroke<pressure_path> s(p);
        s.rewind(0);
        auto stream = read_stream(s);
        bool any_vertex = false;
        for(const auto& v : stream) if(agg::is_vertex(v.cmd)) any_vertex = true;
        c.check(!any_vertex, "all-zero width emits no contour");
    }

    //--------------------------------------------------------------joins group
    // Two pressure lines meeting at a right angle. The pressure slope makes
    // the true boundary tangent lines slanted, so they differ from the
    // constant-width offset formula.
    struct join_fixture
    {
        pressure_path p;
        p2 joint;
        p2 a_left_end, a_right_end, b_left_start, b_right_start;
        p2 da_left, da_right, db_left, db_right;   // unit boundary tangents
        double half;
    };

    join_fixture make_join_fixture()
    {
        join_fixture f;
        // A: (-10,0) w=12 -> (0,0) w=8 ; B: (0,0) w=8 -> (0,10) w=12
        f.p.move_to(-10.0, 0.0, 12.0);
        f.p.line_to(0.0, 0.0, 8.0);
        f.p.line_to(0.0, 10.0, 12.0);
        f.joint = {0.0, 0.0};
        f.half = 4.0;
        // Segment A tangent (1,0), left normal (0,1)
        f.a_left_end  = {0.0, 4.0};
        f.a_right_end = {0.0, -4.0};
        // Segment B tangent (0,1), left normal (-1,0)
        f.b_left_start  = {-4.0, 0.0};
        f.b_right_start = {4.0, 0.0};
        // True slanted boundary directions from the pressure gradient.
        p2 la0 = {-10.0, 6.0}, la1 = {0.0, 4.0};
        p2 ra0 = {-10.0, -6.0}, ra1 = {0.0, -4.0};
        p2 lb0 = {-4.0, 0.0}, lb1 = {-6.0, 10.0};
        p2 rb0 = {4.0, 0.0}, rb1 = {6.0, 10.0};
        auto unit = [](p2 a, p2 b) {
            double l = vec_len(b.x - a.x, b.y - a.y);
            return p2{(b.x - a.x) / l, (b.y - a.y) / l};
        };
        f.da_left = unit(la0, la1);
        f.da_right = unit(ra0, ra1);
        f.db_left = unit(lb0, lb1);
        f.db_right = unit(rb0, rb1);
        return f;
    }

    bool intersect_lines(p2 a, p2 da, p2 b, p2 db, p2& out)
    {
        double den = da.x * db.y - da.y * db.x;
        if(std::fabs(den) < 1e-12) return false;
        double r = ((b.x - a.x) * db.y - (b.y - a.y) * db.x) / den;
        out = {a.x + r * da.x, a.y + r * da.y};
        return true;
    }

    TASK_CASE("joins", bevel_connects_boundary_endpoints)
    {
        join_fixture f = make_join_fixture();
        conv_pressure_stroke<pressure_path> s(f.p);
        s.line_join(agg::bevel_join);
        s.tolerance(0.02);
        s.rewind(0);
        auto contours = extract_contours(read_stream(s));
        c.check(contours.size() == 1, "open joined path is one contour");
        auto poly = contour_closed_polyline(contours[0]);

        c.check(dist_point_polyline(f.a_left_end, poly) < 1e-9,
                "bevel keeps the left end of segment A");
        c.check(dist_point_polyline(f.b_left_start, poly) < 1e-9,
                "bevel keeps the left start of segment B");
        c.check(dist_point_polyline(f.a_right_end, poly) < 1e-9,
                "bevel keeps the right end of segment A");
        c.check(dist_point_polyline(f.b_right_start, poly) < 1e-9,
                "bevel keeps the right start of segment B");

        // No miter intersection vertex may appear.
        p2 mleft, mright;
        intersect_lines(f.a_left_end, f.da_left, f.b_left_start, f.db_left, mleft);
        intersect_lines(f.a_right_end, f.da_right, f.b_right_start, f.db_right, mright);
        for(const p2& v : poly)
        {
            c.check(vec_len(v.x - mleft.x, v.y - mleft.y) > 1e-6,
                    "bevel must not contain the left miter intersection");
            c.check(vec_len(v.x - mright.x, v.y - mright.y) > 1e-6,
                    "bevel must not contain the right miter intersection");
        }
    }

    TASK_CASE("joins", miter_uses_true_offset_tangents)
    {
        join_fixture f = make_join_fixture();
        conv_pressure_stroke<pressure_path> s(f.p);
        s.line_join(agg::miter_join_revert);
        s.miter_limit(4.0);
        s.tolerance(0.02);
        s.rewind(0);
        auto poly = contour_closed_polyline(extract_contours(read_stream(s))[0]);

        p2 mleft, mright;
        bool ok_l = intersect_lines(f.a_left_end, f.da_left,
                                    f.b_left_start, f.db_left, mleft);
        bool ok_r = intersect_lines(f.a_right_end, f.da_right,
                                    f.b_right_start, f.db_right, mright);
        c.check(ok_l && ok_r, "reference tangent intersections exist");
        c.check(dist_point_polyline(mleft, poly) < 1e-6,
                "left miter matches the true pressure-offset tangent intersection");
        c.check(dist_point_polyline(mright, poly) < 1e-6,
                "right miter matches the true pressure-offset tangent intersection");

        // The constant-width formula (lines parallel to the centerline
        // tangents) gives a different intersection on the slanted sides.
        p2 flat;
        intersect_lines(f.a_left_end, {1.0, 0.0}, f.b_left_start, {0.0, 1.0}, flat);
        c.check(vec_len(flat.x - mleft.x, flat.y - mleft.y) > 0.5,
                "pressure miter genuinely differs from the constant-width formula");

        // Miter budget: distance to the center joint <= local half width * limit.
        c.check(vec_len(mleft.x - f.joint.x, mleft.y - f.joint.y) <= f.half * 4.0 + 1e-9,
                "accepted miter within the miter budget");
    }

    TASK_CASE("joins", miter_limit_exceeded_reverts_to_bevel)
    {
        join_fixture f = make_join_fixture();
        conv_pressure_stroke<pressure_path> s(f.p);
        s.line_join(agg::miter_join_revert);
        s.miter_limit(1.0);
        s.tolerance(0.02);
        s.rewind(0);
        auto reverted = contour_closed_polyline(
            extract_contours(read_stream(s))[0]);

        conv_pressure_stroke<pressure_path> b(f.p);
        b.line_join(agg::bevel_join);
        b.tolerance(0.02);
        b.rewind(0);
        auto bevel = contour_closed_polyline(extract_contours(read_stream(b))[0]);

        c.check(reverted.size() == bevel.size(),
                "reverted miter has the bevel vertex count");
        for(unsigned i = 0; i < bevel.size(); ++i)
            c.check(vec_len(reverted[i].x - bevel[i].x,
                            reverted[i].y - bevel[i].y) < 1e-9,
                    "reverted miter equals the bevel geometry");

        p2 mleft;
        intersect_lines(f.a_left_end, f.da_left, f.b_left_start, f.db_left, mleft);
        c.check(vec_len(mleft.x, mleft.y) > f.half * 1.0,
                "fixture really exceeds the unit miter budget");
    }

    TASK_CASE("joins", cap_geometry_butt_square_round)
    {
        // butt: exact rectangle
        {
            pressure_path p;
            p.move_to(10.0, 10.0, 4.0);
            p.line_to(50.0, 10.0, 4.0);
            conv_pressure_stroke<pressure_path> s(p);
            s.line_cap(agg::butt_cap);
            s.rewind(0);
            auto poly = contour_closed_polyline(
                extract_contours(read_stream(s))[0]);
            p2 corners[4] = {{10, 8}, {50, 8}, {50, 12}, {10, 12}};
            for(const p2& q : corners)
                c.check(dist_point_polyline(q, poly) < 1e-9,
                        "butt cap rectangle corner");
        }
        // square: extends by the local half width
        {
            pressure_path p;
            p.move_to(10.0, 10.0, 4.0);
            p.line_to(50.0, 10.0, 4.0);
            conv_pressure_stroke<pressure_path> s(p);
            s.line_cap(agg::square_cap);
            s.rewind(0);
            auto poly = contour_closed_polyline(
                extract_contours(read_stream(s))[0]);
            p2 corners[4] = {{8, 8}, {52, 8}, {52, 12}, {8, 12}};
            for(const p2& q : corners)
                c.check(dist_point_polyline(q, poly) < 1e-9,
                        "square cap extends one half width");
        }
        // round: outer semicircles, tolerance-bounded
        {
            pressure_path p;
            p.move_to(10.0, 10.0, 4.0);
            p.line_to(50.0, 10.0, 4.0);
            const double tol = 0.05;
            conv_pressure_stroke<pressure_path> s(p);
            s.line_cap(agg::round_cap);
            s.tolerance(tol);
            s.rewind(0);
            auto poly = contour_closed_polyline(
                extract_contours(read_stream(s))[0]);
            const p2 centers[2] = {{10.0, 10.0}, {50.0, 10.0}};
            for(int side = 0; side < 2; ++side)
            {
                // Outer semicircle: start cap spans the left half (angles
                // pi/2..3pi/2), end cap the right half (-pi/2..pi/2).
                double a_lo = side == 0 ?  agg::pi * 0.5 : -agg::pi * 0.5;
                double a_hi = side == 0 ?  agg::pi * 1.5 :  agg::pi * 0.5;
                for(unsigned k = 0; k <= 60; ++k)
                {
                    double a = a_lo + (a_hi - a_lo) * double(k) / 60.0;
                    p2 q = {centers[side].x + 2.0 * std::cos(a),
                            centers[side].y + 2.0 * std::sin(a)};
                    c.check(dist_point_polyline(q, poly) <= tol + 1e-7,
                            "round cap semicircle within tolerance");
                }
            }
            c.check(dist_point_polyline({8.0, 10.0}, poly) <= tol + 1e-7,
                    "round cap reaches the full radius at the start");
            c.check(dist_point_polyline({52.0, 10.0}, poly) <= tol + 1e-7,
                    "round cap reaches the full radius at the end");
        }
    }

    TASK_CASE("joins", closed_pressure_seam_and_hole)
    {
        pressure_path p;
        p.move_to(20.0, 20.0, 4.0);
        p.line_to(60.0, 20.0, 8.0);
        p.line_to(60.0, 60.0, 4.0);
        p.line_to(20.0, 60.0, 8.0);
        p.close_polygon();

        conv_pressure_stroke<pressure_path> s(p);
        s.line_join(agg::miter_join_revert);
        s.tolerance(0.02);
        s.rewind(0);
        auto stream = read_stream(s);
        unsigned end_polys = 0;
        for(const auto& v : stream) if(agg::is_end_poly(v.cmd)) ++end_polys;
        c.check(end_polys == 2, "closed subpath gives two closed boundaries");
        auto contours = extract_contours(stream);
        c.check(contours.size() == 2, "two boundary contours");
        double a0 = signed_area(contours[0]);
        double a1 = signed_area(contours[1]);
        c.check(a0 * a1 < 0.0, "boundaries wind in opposite directions");
        c.check(std::fabs(a0) > 100.0 && std::fabs(a1) > 100.0,
                "both boundaries have substantial area");

        // The closing seam runs downward from (20,60) w=8 to (20,20) w=4.
        // Its left normal is +x (the inner side): Q+ half widths are 4 and
        // 2, Q- half widths are 4 and 2 on the outer side.
        unsigned inner = std::fabs(a0) < std::fabs(a1) ? 0 : 1;
        unsigned outer = inner ^ 1u;
        auto inner_poly = contour_closed_polyline(contours[inner]);
        auto outer_poly = contour_closed_polyline(contours[outer]);
        c.check(dist_point_polyline({24.0, 60.0}, inner_poly) < 1e-6,
                "seam inner top boundary point present");
        c.check(dist_point_polyline({22.0, 20.0}, inner_poly) < 1e-6,
                "seam inner bottom boundary point present");
        c.check(dist_point_polyline({16.0, 60.0}, outer_poly) < 1e-6,
                "seam outer top boundary point present");
        c.check(dist_point_polyline({18.0, 20.0}, outer_poly) < 1e-6,
                "seam outer bottom boundary point present");
    }

    TASK_CASE("joins", zero_width_endpoint_tip)
    {
        pressure_path p;
        p.move_to(0.0, 30.0, 0.0);
        p.line_to(40.0, 30.0, 8.0);
        conv_pressure_stroke<pressure_path> s(p);
        s.line_cap(agg::butt_cap);
        s.rewind(0);
        auto contours = extract_contours(read_stream(s));
        c.check(contours.size() == 1, "tapered stroke gives one contour");
        auto poly = contour_closed_polyline(contours[0]);
        c.check(dist_point_polyline({0.0, 30.0}, poly) < 1e-9,
                "zero-width tip collapses to the centerline");
        // Far end keeps its full width.
        c.check(dist_point_polyline({40.0, 26.0}, poly) < 1e-9,
                "non-zero end keeps its lower boundary");
        c.check(dist_point_polyline({40.0, 34.0}, poly) < 1e-9,
                "non-zero end keeps its upper boundary");
    }

    TASK_CASE("joins", miter_fallbacks_zero_half_width_and_parallel)
    {
        // Zero local half width at the joint: miter must fall back to bevel.
        {
            pressure_path p;
            p.move_to(0.0, 0.0, 8.0);
            p.line_to(20.0, 0.0, 0.0);
            p.line_to(40.0, 10.0, 8.0);
            conv_pressure_stroke<pressure_path> m(p);
            m.line_join(agg::miter_join_revert);
            m.rewind(0);
            auto miter_stream = read_stream(m);

            conv_pressure_stroke<pressure_path> b(p);
            b.line_join(agg::bevel_join);
            b.rewind(0);
            auto bevel_stream = read_stream(b);
            c.check(miter_stream.size() == bevel_stream.size(),
                    "zero half width: miter falls back to bevel count");
            for(unsigned i = 0; i < bevel_stream.size(); ++i)
                c.check(miter_stream[i].cmd == bevel_stream[i].cmd &&
                        vec_len(miter_stream[i].x - bevel_stream[i].x,
                                miter_stream[i].y - bevel_stream[i].y) < 1e-9,
                        "zero half width: miter falls back to bevel geometry");
            auto poly = contour_closed_polyline(extract_contours(miter_stream)[0]);
            c.check(dist_point_polyline({20.0, 0.0}, poly) < 1e-9,
                    "zero-width joint sits on the centerline");
        }
        // Parallel (collinear, constant width) tangents at the joint:
        // no finite intersection, so miter must degrade without throwing.
        {
            pressure_path p;
            p.move_to(0.0, 50.0, 8.0);
            p.line_to(20.0, 50.0, 8.0);
            p.line_to(40.0, 50.0, 8.0);
            conv_pressure_stroke<pressure_path> s(p);
            s.line_join(agg::miter_join_revert);
            s.rewind(0);
            auto contours = extract_contours(read_stream(s));
            c.check(contours.size() == 1, "collinear joint still renders");
            auto poly = contour_closed_polyline(contours[0]);
            // Constant width 8: top/bottom boundary rows are y=54 and y=46.
            c.check(dist_point_polyline({20.0, 54.0}, poly) < 1e-6,
                    "collinear joint keeps the offset boundary");
            c.check(dist_point_polyline({20.0, 46.0}, poly) < 1e-6,
                    "collinear joint keeps both sides");
        }
    }

    TASK_CASE("joins", closed_coincident_endpoint_seam)
    {
        // Last point equals the first with equal width: no closing segment,
        // the last and first curves join directly at the seam.
        pressure_path p;
        p.move_to(40.0, 20.0, 6.0);
        p.curve4(60.0, 20.0, 8.0, 60.0, 60.0, 8.0, 40.0, 60.0, 6.0);
        p.curve4(20.0, 60.0, 8.0, 20.0, 20.0, 8.0, 40.0, 20.0, 6.0);
        p.close_polygon();

        conv_pressure_stroke<pressure_path> s(p);
        s.line_join(agg::miter_join_revert);
        s.tolerance(0.02);
        s.rewind(0);
        auto stream = read_stream(s);
        unsigned end_polys = 0;
        for(const auto& v : stream) if(agg::is_end_poly(v.cmd)) ++end_polys;
        c.check(end_polys == 2, "coincident closed loop gives two boundaries");
        auto contours = extract_contours(stream);
        c.check(contours.size() == 2, "two closed contours");
        c.check(signed_area(contours[0]) * signed_area(contours[1]) < 0.0,
                "coincident-loop boundaries wind oppositely");
        // The seam point (40,20): the arriving curve leaves tangent +x, so
        // its left normal is +y; half width 3 puts Q+/Q- at (40,23)/(40,17).
        auto c0 = contour_closed_polyline(contours[0]);
        auto c1 = contour_closed_polyline(contours[1]);
        c.check(dist_point_polyline({40.0, 23.0}, c0) < 0.05 ||
                dist_point_polyline({40.0, 23.0}, c1) < 0.05,
                "seam Q+ boundary retained");
        c.check(dist_point_polyline({40.0, 17.0}, c0) < 0.05 ||
                dist_point_polyline({40.0, 17.0}, c1) < 0.05,
                "seam Q- boundary retained");

        // Same geometry but unequal endpoint widths must be rejected.
        pressure_path bad;
        bad.move_to(40.0, 20.0, 6.0);
        bad.curve4(60.0, 20.0, 8.0, 60.0, 60.0, 8.0, 40.0, 60.0, 6.0);
        bad.curve4(20.0, 60.0, 8.0, 20.0, 20.0, 8.0, 40.0, 20.0, 9.0);
        bad.close_polygon();
        conv_pressure_stroke<pressure_path> bs(bad);
        bool threw = false;
        try { bs.rewind(0); read_stream(bs); }
        catch(const std::invalid_argument&) { threw = true; }
        c.check(threw, "coincident closed endpoint with mismatched widths rejected");
    }

    //--------------------------------------------------------------render group
    struct gray_canvas
    {
        static const int width = 80;
        static const int height = 80;
        static const int stride = 84;             // 4 row-end sentinels
        std::vector<unsigned char> data;
        gray_canvas() : data(size_t(stride) * height, 0xCD)
        {
            for(int y = 0; y < height; ++y)
                for(int x = 0; x < width; ++x) data[y * stride + x] = 0;
        }
        unsigned char pixel(int x, int y) const { return data[y * stride + x]; }
        bool sentinels_intact() const
        {
            for(int y = 0; y < height; ++y)
                for(int k = 0; k < 4; ++k)
                    if(data[y * stride + width + k] != 0xCD) return false;
            return true;
        }
    };

    void paint(gray_canvas& canvas, conv_pressure_stroke<pressure_path>& s)
    {
        agg::rendering_buffer buf(canvas.data.data(),
                                  gray_canvas::width, gray_canvas::height,
                                  gray_canvas::stride);
        agg::pixfmt_gray8 pix(buf);
        agg::renderer_base<agg::pixfmt_gray8> ren(pix);
        agg::rasterizer_scanline_aa<> ras;
        agg::scanline_u8 sl;
        s.rewind(0);
        ras.add_path(s);
        agg::render_scanlines_aa_solid(ras, sl, ren, agg::gray8(255));
    }

    TASK_CASE("render", variable_width_curve_pixels)
    {
        gray_canvas canvas;
        pressure_path p;
        p.move_to(6.0, 20.0, 4.0);
        p.curve4(20.0, 60.0, 16.0, 55.0, 5.0, 10.0, 60.0, 26.0, 6.0);
        conv_pressure_stroke<pressure_path> s(p);
        s.tolerance(0.05);
        paint(canvas, s);

        c.check(canvas.sentinels_intact(), "row-end sentinels untouched");
        // A point well inside the thick middle of the stroke is covered;
        // points well outside the bounding region are empty.
        c.check(canvas.pixel(38, 30) >= 250, "inner stroke pixel fully covered");
        c.check(canvas.pixel(2, 2) == 0, "outer background pixel empty");
        c.check(canvas.pixel(78, 78) == 0, "far corner background pixel empty");
        c.check(canvas.pixel(38, 2) == 0, "top background pixel empty");

        // Persist the scene (ring is added by the next case before writing).
        c.check(true, "variable curve rendered");
    }

    TASK_CASE("render", hollow_closed_ring_keeps_hole)
    {
        gray_canvas canvas;
        pressure_path ring;
        ring.move_to(20.0, 20.0, 6.0);
        ring.line_to(60.0, 20.0, 10.0);
        ring.line_to(60.0, 60.0, 6.0);
        ring.line_to(20.0, 60.0, 10.0);
        ring.close_polygon();
        conv_pressure_stroke<pressure_path> s(ring);
        s.tolerance(0.05);
        paint(canvas, s);

        c.check(canvas.sentinels_intact(), "ring render keeps sentinels");
        c.check(canvas.pixel(40, 40) == 0, "hole interior stays empty");
        c.check(canvas.pixel(4, 40) == 0, "exterior stays empty");
        // On the top band around y=20 with width between 6 and 10:
        // y=20 itself is inside the stroke; a few rows out it is not.
        c.check(canvas.pixel(40, 20) >= 250, "stroke band pixel covered");
        c.check(canvas.pixel(40, 12) == 0, "above the band stays background");

        // Write a PPM showing both required shapes on one canvas.
        pressure_path both;
        both.move_to(6.0, 74.0, 3.0);
        both.curve4(18.0, 30.0, 8.0, 50.0, 66.0, 12.0, 70.0, 50.0, 9.0);
        unsigned id2 = both.start_new_path();
        c.check(id2 > 0, "ppm scene uses two subpaths");
        both.move_to(14.0, 14.0, 4.0);
        both.line_to(34.0, 14.0, 7.0);
        both.line_to(34.0, 34.0, 4.0);
        both.line_to(14.0, 34.0, 7.0);
        both.close_polygon();

        gray_canvas out;
        conv_pressure_stroke<pressure_path> t(both);
        t.tolerance(0.05);
        agg::rendering_buffer buf(out.data.data(),
                                  gray_canvas::width, gray_canvas::height,
                                  gray_canvas::stride);
        agg::pixfmt_gray8 pix(buf);
        agg::renderer_base<agg::pixfmt_gray8> ren(pix);
        agg::rasterizer_scanline_aa<> ras;
        agg::scanline_u8 sl;
        ras.add_path(t, 0);
        ras.add_path(t, id2);
        agg::render_scanlines_aa_solid(ras, sl, ren, agg::gray8(255));
        c.check(out.sentinels_intact(), "ppm scene keeps sentinels");
        c.check(out.pixel(24, 24) == 0, "ppm ring hole empty");
        c.check(out.pixel(24, 14) >= 250, "ppm ring band covered");

        std::ofstream ppm("task_output/pressure.ppm", std::ios::binary);
        if(!ppm) throw std::runtime_error("cannot open task_output/pressure.ppm");
        ppm << "P6\n" << gray_canvas::width << " " << gray_canvas::height
            << "\n255\n";
        for(int y = 0; y < gray_canvas::height; ++y)
            for(int x = 0; x < gray_canvas::width; ++x)
            {
                unsigned char g = out.pixel(x, y);
                char rgb[3] = {char(g), char(g), char(g)};
                ppm.write(rgb, 3);
            }
        ppm.close();
        if(!ppm) throw std::runtime_error("failed writing pressure.ppm");
    }

    TASK_CASE("render", stream_isolation_between_paths_and_attach)
    {
        pressure_path a;
        a.move_to(0.0, 0.0, 4.0);
        a.line_to(10.0, 0.0, 4.0);
        unsigned a_id2 = a.start_new_path();
        a.move_to(70.0, 70.0, 2.0);
        a.line_to(75.0, 70.0, 2.0);

        conv_pressure_stroke<pressure_path> s(a);
        s.rewind(0);
        auto first = read_stream(s);
        s.rewind(a_id2);
        auto second = read_stream(s);
        c.check(!first.empty() && !second.empty(), "both subpaths render");
        for(const auto& v : first)
            if(agg::is_vertex(v.cmd))
                c.check(v.x <= 12.0, "first path stream contains only path 0");
        for(const auto& v : second)
            if(agg::is_vertex(v.cmd))
                c.check(v.x >= 68.0, "second path stream contains only path 1");

        pressure_path b;
        b.move_to(30.0, 30.0, 5.0);
        b.curve3(45.0, 45.0, 9.0, 60.0, 30.0, 5.0);
        s.attach(b);
        s.rewind(0);
        auto attached = read_stream(s);
        for(const auto& v : attached)
            if(agg::is_vertex(v.cmd))
            {
                c.check(v.x >= 25.0 && v.x <= 65.0 &&
                        v.y >= 25.0 && v.y <= 50.0,
                        "attached stream carries no previous geometry");
            }
        s.rewind(0);
        auto again = read_stream(s);
        c.check(again.size() == attached.size(),
                "repeat rewind after attach is stable");
    }
}
