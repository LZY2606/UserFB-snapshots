//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Numeric implementation of the pressure-aware stroke converter.
//
// For every regular segment the two boundaries are
//     Q+(t) = P(t) + w(t)/2 * N(t)
//     Q-(t) = P(t) - w(t)/2 * N(t)
// with N(t) the unit left normal of P'(t). P(t) and w(t) share one set of
// Bernstein basis functions, so pressure carried by curve control points
// survives on both sides of the stroke.
//
// Boundary polylines are produced by adaptive midpoint subdivision driven
// by the measured point-to-chord distance on BOTH sides, followed by a
// dense two-way verification/refinement pass. A fixed subdivision depth is
// never silently accepted: running out of depth is an explicit error.
//----------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>
#include "agg_basics.h"
#include "agg_math.h"
#include "agg_conv_pressure_stroke.h"

namespace agg
{
namespace pressure_stroke_detail
{
namespace
{
    //------------------------------------------------------------------types
    struct vec2
    {
        double x, y;
        vec2() : x(0.0), y(0.0) {}
        vec2(double x_, double y_) : x(x_), y(y_) {}
    };

    struct curve_type
    {
        int    degree;                 // 1, 2 or 3
        double px[4], py[4];           // Bernstein control coefficients
        double pw[4];
    };

    struct state_type
    {
        vec2   p;                      // center position
        double w;                      // full width
        vec2   normal;                 // unit left normal
        vec2   qleft;                  // Q+
        vec2   qright;                 // Q-
    };

    struct segment_geom
    {
        curve_type          curve;
        std::vector<state_type> states;
        vec2  tan_l[2];               // Q+ tangent at t=0, t=1
        vec2  tan_r[2];               // Q- tangent at t=0, t=1
        bool  tan_l_ok[2];
        bool  tan_r_ok[2];
    };

    const int    max_subdivision_depth = 64;
    const double stall_eps             = 1e-11;
    const double tangent_eps           = 1e-12;
    const double parallel_eps          = 1e-12;
    const int    verify_subsamples     = 16;

    //-------------------------------------------------------------vec helpers
    inline vec2 operator + (const vec2& a, const vec2& b)
    { return vec2(a.x + b.x, a.y + b.y); }
    inline vec2 operator - (const vec2& a, const vec2& b)
    { return vec2(a.x - b.x, a.y - b.y); }
    inline vec2 operator * (const vec2& a, double k)
    { return vec2(a.x * k, a.y * k); }
    inline double dot(const vec2& a, const vec2& b)
    { return a.x * b.x + a.y * b.y; }
    inline double cross(const vec2& a, const vec2& b)
    { return a.x * b.y - a.y * b.x; }
    inline double length(const vec2& a)
    { return std::sqrt(a.x * a.x + a.y * a.y); }
    inline vec2 normalize(const vec2& a)
    {
        double len = length(a);
        return len > 0.0 ? vec2(a.x / len, a.y / len) : vec2(0.0, 0.0);
    }
    inline vec2 left_normal(const vec2& tangent)
    { return vec2(-tangent.y, tangent.x); }
    inline vec2 tangent_of_normal(const vec2& n)
    { return vec2(n.y, -n.x); }

    double point_segment_distance(const vec2& p, const vec2& a, const vec2& b)
    {
        vec2 d = b - a;
        double dd = dot(d, d);
        if(dd <= 0.0) return length(p - a);
        double u = dot(p - a, d) / dd;
        if(u < 0.0) u = 0.0;
        else if(u > 1.0) u = 1.0;
        return length(p - (a + d * u));
    }

    // Infinite line intersection. Returns false when the lines are parallel.
    bool line_intersection(const vec2& a, const vec2& da,
                           const vec2& b, const vec2& db,
                           vec2& out)
    {
        double den = cross(da, db);
        if(std::fabs(den) < parallel_eps) return false;
        double r = cross(b - a, db) / den;
        out = a + da * r;
        return std::isfinite(out.x) && std::isfinite(out.y);
    }

    //------------------------------------------------------------Bernstein math
    void bernstein_basis(int degree, double t, double b[4])
    {
        double u = 1.0 - t;
        if(degree == 0)
        {
            b[0] = 1.0;
        }
        else if(degree == 1)
        {
            b[0] = u;
            b[1] = t;
        }
        else if(degree == 2)
        {
            b[0] = u * u;
            b[1] = 2.0 * u * t;
            b[2] = t * t;
        }
        else
        {
            b[0] = u * u * u;
            b[1] = 3.0 * u * u * t;
            b[2] = 3.0 * u * t * t;
            b[3] = t * t * t;
        }
    }

    double bernstein_eval(const double* c, int degree, double t)
    {
        double b[4];
        bernstein_basis(degree, t, b);
        double v = 0.0;
        for(int k = 0; k <= degree; ++k) v += b[k] * c[k];
        return v;
    }

    // Derivative control vectors (degree-1 Bernstein coefficients, without
    // the leading degree factor).
    void derivative_controls(const double* c, int degree, double d[3])
    {
        for(int k = 0; k < degree; ++k) d[k] = c[k + 1] - c[k];
    }

    double bernstein_derivative(const double* c, int degree, double t)
    {
        double d[3];
        derivative_controls(c, degree, d);
        return degree * bernstein_eval(d, degree - 1, t);
    }

    vec2 curve_position(const curve_type& c, double t)
    {
        return vec2(bernstein_eval(c.px, c.degree, t),
                    bernstein_eval(c.py, c.degree, t));
    }

    vec2 curve_derivative(const curve_type& c, double t)
    {
        return vec2(bernstein_derivative(c.px, c.degree, t),
                    bernstein_derivative(c.py, c.degree, t));
    }

    // One-sided unit tangent at a stall endpoint (center derivative is zero
    // there): direction of the first non-zero Bernstein difference vector.
    vec2 endpoint_limit_tangent(const curve_type& c, bool at_start)
    {
        double dx[3], dy[3];
        derivative_controls(c.px, c.degree, dx);
        derivative_controls(c.py, c.degree, dy);
        if(at_start)
        {
            for(int k = 0; k < c.degree; ++k)
            {
                vec2 v(dx[k], dy[k]);
                if(length(v) > 0.0) return normalize(v);
            }
        }
        else
        {
            for(int k = c.degree - 1; k >= 0; --k)
            {
                vec2 v(-dx[k], -dy[k]);
                if(length(v) > 0.0) return normalize(v);
            }
        }
        throw std::invalid_argument(
            "conv_pressure_stroke: curve has a constant centerline");
    }

    bool curve_centerline_constant(const curve_type& c)
    {
        for(int k = 1; k <= c.degree; ++k)
        {
            if(c.px[k] != c.px[0] || c.py[k] != c.py[0]) return false;
        }
        return true;
    }

    // True only when P'(t) vanishes for some t strictly inside the segment.
    bool has_interior_stall(const curve_type& c)
    {
        double dx[3], dy[3];
        derivative_controls(c.px, c.degree, dx);
        derivative_controls(c.py, c.degree, dy);
        double scale = 1.0;
        for(int k = 0; k < c.degree; ++k)
        {
            double m = std::max(std::fabs(dx[k]), std::fabs(dy[k]));
            if(m > scale) scale = m;
        }
        double eps = stall_eps * scale;

        if(c.degree == 1)
            return false; // zero-length linear segments are rejected earlier

        if(c.degree == 2)
        {
            // P'(t)/2 = (1-t)a + t b; an interior zero needs collinear,
            // oppositely directed difference vectors.
            vec2 a(dx[0], dy[0]), b(dx[1], dy[1]);
            return std::fabs(cross(a, b)) <= eps && dot(a, b) < 0.0;
        }

        // degree 3: P'(t)/3 is a quadratic Bezier with control vectors
        // a, b, c. Solve both component quadratics and confirm the vector
        // derivative at the candidate root really vanishes.
        double roots[8];
        unsigned nroots = 0;
        for(int comp = 0; comp < 2; ++comp)
        {
            const double* d = comp == 0 ? dx : dy;
            double A = d[0] - 2.0 * d[1] + d[2];
            double B = 2.0 * (d[1] - d[0]);
            double C = d[0];
            if(std::fabs(A) <= eps)
            {
                if(std::fabs(B) > eps) roots[nroots++] = -C / B;
            }
            else
            {
                double disc = B * B - 4.0 * A * C;
                if(disc >= 0.0)
                {
                    double q = std::sqrt(disc);
                    roots[nroots++] = (-B - q) / (2.0 * A);
                    roots[nroots++] = (-B + q) / (2.0 * A);
                }
            }
        }
        for(unsigned i = 0; i < nroots; ++i)
        {
            double t = roots[i];
            if(t > 0.0 && t < 1.0 && length(curve_derivative(c, t)) <= 3.0 * eps)
                return true;
        }
        return false;
    }

    // Full boundary state at parameter t.
    state_type evaluate_state(const curve_type& c, double t)
    {
        state_type s;
        s.p = curve_position(c, t);
        s.w = bernstein_eval(c.pw, c.degree, t);
        vec2 dp = curve_derivative(c, t);
        double speed = length(dp);
        if(speed > 0.0)
        {
            s.normal = left_normal(vec2(dp.x / speed, dp.y / speed));
        }
        else
        {
            // Endpoint stalls are legal: take the one-sided limit tangent.
            if(t != 0.0 && t != 1.0)
            {
                throw std::invalid_argument(
                    "conv_pressure_stroke: zero-speed point inside a curve segment");
            }
            s.normal = left_normal(endpoint_limit_tangent(c, t == 0.0));
        }
        vec2 off = s.normal * (s.w * 0.5);
        s.qleft  = s.p + off;
        s.qright = s.p - off;
        return s;
    }

    // Boundary tangent at a segment endpoint. Degree-1 boundaries are
    // linear, so their tangent is exact from the two endpoint states;
    // curves use one-sided finite differences of Q+/- (which also cover
    // the legal centerline stall endpoints). Returns false when the
    // boundary itself has zero derivative there.
    bool boundary_tangent(const curve_type& c, double t, bool left_side,
                          vec2& out)
    {
        state_type s0 = evaluate_state(c, t);
        state_type s_other;
        if(c.degree == 1)
        {
            s_other = evaluate_state(c, t == 0.0 ? 1.0 : 0.0);
        }
        else
        {
            double delta = 1e-6;
            double t2 = t == 0.0 ? t + delta : t - delta;
            if(t2 < 0.0) t2 = 0.0;
            if(t2 > 1.0) t2 = 1.0;
            s_other = evaluate_state(c, t2);
        }
        vec2 q0 = left_side ? s0.qleft  : s0.qright;
        vec2 q1 = left_side ? s_other.qleft  : s_other.qright;
        vec2 d = t == 0.0 ? q1 - q0 : q0 - q1;   // outgoing direction
        double scale = std::max(1.0, std::max(length(s0.p), s0.w));
        if(length(d) < tangent_eps * scale) return false;
        out = normalize(d);
        return true;
    }

    //--------------------------------------------------------segment sampling
    void subdivide(const curve_type& c, double t0, double t1,
                   const state_type& s0, const state_type& s1,
                   int depth, double tol,
                   std::vector<std::pair<double, state_type> >& out)
    {
        if(depth >= max_subdivision_depth)
        {
            throw std::runtime_error(
                "conv_pressure_stroke: subdivision depth exhausted; "
                "requested tolerance cannot be reached");
        }
        double tm = 0.5 * (t0 + t1);
        state_type sm = evaluate_state(c, tm);

        double dl = point_segment_distance(sm.qleft,  s0.qleft,  s1.qleft);
        double dr = point_segment_distance(sm.qright, s0.qright, s1.qright);
        if(std::max(dl, dr) <= tol)
            return;

        subdivide(c, t0, tm, s0, sm, depth + 1, tol, out);
        out.push_back(std::make_pair(tm, sm));
        subdivide(c, tm, t1, sm, s1, depth + 1, tol, out);
    }

    struct interval_type
    {
        double ta, tb;
        state_type sa, sb;
        int depth;
    };

    // Dense two-way check of one interval: every sampled true boundary
    // point against the chord, and the chord midpoint against the true
    // boundary arc. Failed intervals are split at their midpoint.
    bool interval_within_tolerance(const curve_type& c,
                                   const interval_type& iv, double tol)
    {
        state_type grid[verify_subsamples + 1];
        for(int k = 0; k <= verify_subsamples; ++k)
        {
            double t = iv.ta + (iv.tb - iv.ta) * double(k) / verify_subsamples;
            grid[k] = evaluate_state(c, t);
            if(k > 0 && k < verify_subsamples)
            {
                double dl = point_segment_distance(grid[k].qleft,
                                                    iv.sa.qleft, iv.sb.qleft);
                double dr = point_segment_distance(grid[k].qright,
                                                    iv.sa.qright, iv.sb.qright);
                if(dl > tol || dr > tol) return false;
            }
        }
        // Reverse direction: every sampled chord point must stay close to
        // the true boundary arc (catches chords that cut across a folded
        // offset; the endpoints themselves coincide exactly).
        for(int k = 1; k < verify_subsamples; ++k)
        {
            double f = double(k) / verify_subsamples;
            vec2 cl = iv.sa.qleft  + (iv.sb.qleft  - iv.sa.qleft)  * f;
            vec2 cr = iv.sa.qright + (iv.sb.qright - iv.sa.qright) * f;
            double dl = 1e300, dr = 1e300;
            for(int j = 0; j <= verify_subsamples; ++j)
            {
                double d = length(cl - grid[j].qleft);
                if(d < dl) dl = d;
                d = length(cr - grid[j].qright);
                if(d < dr) dr = d;
            }
            if(dl > tol || dr > tol) return false;
        }
        return true;
    }

    std::vector<state_type> sample_segment(const curve_type& c, double tol)
    {
        state_type s0 = evaluate_state(c, 0.0);
        state_type s1 = evaluate_state(c, 1.0);

        std::vector<std::pair<double, state_type> > tagged;
        tagged.push_back(std::make_pair(0.0, s0));
        subdivide(c, 0.0, 1.0, s0, s1, 0, tol * 0.5, tagged);
        tagged.push_back(std::make_pair(1.0, s1));

        // Verification/refinement pass over the accepted intervals.
        std::vector<interval_type> work;
        for(unsigned i = 0; i + 1 < tagged.size(); ++i)
        {
            interval_type iv;
            iv.ta = tagged[i].first;
            iv.tb = tagged[i + 1].first;
            iv.sa = tagged[i].second;
            iv.sb = tagged[i + 1].second;
            iv.depth = 0;
            work.push_back(iv);
        }
        std::map<double, state_type> points;
        for(unsigned i = 0; i < tagged.size(); ++i)
            points[tagged[i].first] = tagged[i].second;

        while(!work.empty())
        {
            interval_type iv = work.back();
            work.pop_back();
            if(interval_within_tolerance(c, iv, tol))
                continue;
            if(iv.depth >= max_subdivision_depth)
            {
                throw std::runtime_error(
                    "conv_pressure_stroke: subdivision depth exhausted; "
                    "requested tolerance cannot be reached");
            }
            double tm = 0.5 * (iv.ta + iv.tb);
            state_type sm = evaluate_state(c, tm);
            points[tm] = sm;
            interval_type a = iv;
            a.tb = tm; a.sb = sm; a.depth = iv.depth + 1;
            interval_type b = iv;
            b.ta = tm; b.sa = sm; b.depth = iv.depth + 1;
            work.push_back(a);
            work.push_back(b);
        }

        std::vector<state_type> result;
        result.reserve(points.size());
        for(std::map<double, state_type>::const_iterator it = points.begin();
            it != points.end(); ++it)
        {
            result.push_back(it->second);
        }
        return result;
    }

    //------------------------------------------------------------------joins
    // Returns true and the miter point when the two actual boundary tangent
    // lines intersect within the miter budget; false means "use bevel".
    bool try_miter(const segment_geom& a, const segment_geom& b,
                   bool left_side, double miter_limit, vec2& miter)
    {
        const state_type& sa = a.states.back();
        const state_type& sb = b.states.front();
        double half = sa.w * 0.5;
        if(half <= 0.0) return false;

        vec2 da, db;
        bool ok_a = left_side ? a.tan_l_ok[1] : a.tan_r_ok[1];
        bool ok_b = left_side ? b.tan_l_ok[0] : b.tan_r_ok[0];
        if(!ok_a || !ok_b) return false;
        da = left_side ? a.tan_l[1] : a.tan_r[1];
        db = left_side ? b.tan_l[0] : b.tan_r[0];

        vec2 pa = left_side ? sa.qleft  : sa.qright;
        vec2 pb = left_side ? sb.qleft  : sb.qright;
        vec2 m;
        if(!line_intersection(pa, da, pb, db, m)) return false;
        if(length(m - sa.p) > half * miter_limit) return false;
        miter = m;
        return true;
    }

    //-------------------------------------------------------------cap arcs
    // Number of equal arc pieces for a semicircle of radius h so that the
    // chord sagitta stays within tol.
    int cap_arc_steps(double h, double tol)
    {
        if(tol >= h || h <= 0.0) return 1;
        double n = pi / (2.0 * std::acos(1.0 - tol / h));
        int steps = int(std::ceil(n));
        return steps < 1 ? 1 : steps;
    }

    //-------------------------------------------------------------contours
    void append_dedup(std::vector<vec2>& pts, const vec2& p)
    {
        if(pts.empty() || length(pts.back() - p) > 0.0)
            pts.push_back(p);
    }

    const vec2& side_point(const state_type& s, bool left_side)
    {
        return left_side ? s.qleft : s.qright;
    }

    // Boundary of one side across all segments. Direction:
    // left side  -> segments and states forward
    // right side -> segments and states backward
    void build_side(const std::vector<segment_geom>& segs, bool left_side,
                    line_join_e join, double miter_limit, bool forward,
                    std::vector<vec2>& pts)
    {
        unsigned n = unsigned(segs.size());
        if(forward)
        {
            for(unsigned i = 0; i < n; ++i)
            {
                if(i > 0 && join == miter_join_revert)
                {
                    vec2 m;
                    if(try_miter(segs[i - 1], segs[i], left_side,
                                 miter_limit, m))
                    {
                        append_dedup(pts, m);
                    }
                }
                const std::vector<state_type>& st = segs[i].states;
                for(unsigned k = 0; k < st.size(); ++k)
                    append_dedup(pts, side_point(st[k], left_side));
            }
        }
        else
        {
            for(unsigned ii = 0; ii < n; ++ii)
            {
                unsigned i = n - 1 - ii;
                if(ii > 0 && join == miter_join_revert)
                {
                    vec2 m;
                    if(try_miter(segs[i], segs[i + 1], left_side,
                                 miter_limit, m))
                    {
                        append_dedup(pts, m);
                    }
                }
                const std::vector<state_type>& st = segs[i].states;
                for(unsigned k = st.size(); k > 0; --k)
                    append_dedup(pts, side_point(st[k - 1], left_side));
            }
        }
    }

    void append_end_cap(std::vector<vec2>& pts, const state_type& s,
                        line_cap_e cap, double tol)
    {
        vec2 t = tangent_of_normal(s.normal);
        vec2 ql = s.qleft;
        vec2 qr = s.qright;
        double h = s.w * 0.5;
        if(cap == square_cap)
        {
            append_dedup(pts, ql + t * h);
            append_dedup(pts, qr + t * h);
        }
        else if(cap == round_cap && h > 0.0)
        {
            int steps = cap_arc_steps(h, tol);
            double a0 = std::atan2(s.normal.y, s.normal.x);
            for(int k = 1; k < steps; ++k)
            {
                double a = a0 - pi * double(k) / steps;
                append_dedup(pts, s.p + vec2(std::cos(a), std::sin(a)) * h);
            }
        }
        // butt_cap: the next point (right boundary end) closes the edge.
    }

    void append_start_cap(std::vector<vec2>& pts, const state_type& s,
                          line_cap_e cap, double tol)
    {
        vec2 t = tangent_of_normal(s.normal);
        vec2 ql = s.qleft;
        vec2 qr = s.qright;
        double h = s.w * 0.5;
        if(cap == square_cap)
        {
            append_dedup(pts, qr - t * h);
            append_dedup(pts, ql - t * h);
        }
        else if(cap == round_cap && h > 0.0)
        {
            // Outer semicircle bulging in the -t direction: from qr at
            // angle (a0 - pi) down to ql at angle (a0 - 2*pi).
            int steps = cap_arc_steps(h, tol);
            double a0 = std::atan2(s.normal.y, s.normal.x);
            for(int k = 1; k < steps; ++k)
            {
                double a = a0 - pi - pi * double(k) / steps;
                append_dedup(pts, s.p + vec2(std::cos(a), std::sin(a)) * h);
            }
        }
        // butt_cap: end_poly closes back to the first (left) point.
    }

    pressure_contour make_contour(const std::vector<vec2>& pts)
    {
        pressure_contour c;
        c.points.reserve(pts.size());
        for(unsigned i = 0; i < pts.size(); ++i)
            c.points.push_back(std::make_pair(pts[i].x, pts[i].y));
        return c;
    }

    //-------------------------------------------------------------parsing
    void check_finite_node(const pressure_node& n)
    {
        if(!std::isfinite(n.x) || !std::isfinite(n.y) ||
           !std::isfinite(n.w) || n.w < 0.0)
        {
            throw std::invalid_argument(
                "conv_pressure_stroke: source coordinates must be finite "
                "and widths non-negative");
        }
    }

    curve_type make_line(const pressure_node& a, const pressure_node& b)
    {
        curve_type c;
        c.degree = 1;
        c.px[0] = a.x; c.py[0] = a.y; c.pw[0] = a.w;
        c.px[1] = b.x; c.py[1] = b.y; c.pw[1] = b.w;
        return c;
    }
}

//---------------------------------------------------------------------public
void stroke_subpath(const std::vector<pressure_node>& nodes,
                    double tolerance,
                    line_cap_e cap,
                    line_join_e join,
                    double miter_limit,
                    std::vector<pressure_contour>& out)
{
    if(nodes.empty() || !is_move_to(nodes.front().cmd))
        return;

    for(unsigned i = 0; i < nodes.size(); ++i)
        check_finite_node(nodes[i]);

    // Parse the node stream into Bernstein segments.
    std::vector<curve_type> curves;
    pressure_node first = nodes.front();
    pressure_node cur   = first;
    bool closed = false;
    unsigned idx = 1;
    while(idx < nodes.size())
    {
        const pressure_node& n = nodes[idx];
        if(is_end_poly(n.cmd))
        {
            closed = is_close(n.cmd);
            ++idx;
            continue;
        }
        if(is_stop(n.cmd) || is_move_to(n.cmd))
            break;

        if(is_line_to(n.cmd))
        {
            curves.push_back(make_line(cur, n));
            cur = n;
            ++idx;
        }
        else if(is_curve3(n.cmd))
        {
            if(idx + 1 >= nodes.size() || !is_curve3(nodes[idx + 1].cmd))
                throw std::invalid_argument(
                    "conv_pressure_stroke: incomplete curve3 command");
            const pressure_node& ctrl = nodes[idx];
            const pressure_node& end  = nodes[idx + 1];
            curve_type c;
            c.degree = 2;
            c.px[0] = cur.x;  c.py[0] = cur.y;  c.pw[0] = cur.w;
            c.px[1] = ctrl.x; c.py[1] = ctrl.y; c.pw[1] = ctrl.w;
            c.px[2] = end.x;  c.py[2] = end.y;  c.pw[2] = end.w;
            curves.push_back(c);
            cur = end;
            idx += 2;
        }
        else if(is_curve4(n.cmd))
        {
            if(idx + 2 >= nodes.size() ||
               !is_curve4(nodes[idx + 1].cmd) ||
               !is_curve4(nodes[idx + 2].cmd))
                throw std::invalid_argument(
                    "conv_pressure_stroke: incomplete curve4 command");
            const pressure_node& c1 = nodes[idx];
            const pressure_node& c2 = nodes[idx + 1];
            const pressure_node& end = nodes[idx + 2];
            curve_type c;
            c.degree = 3;
            c.px[0] = cur.x;  c.py[0] = cur.y;  c.pw[0] = cur.w;
            c.px[1] = c1.x;   c.py[1] = c1.y;   c.pw[1] = c1.w;
            c.px[2] = c2.x;   c.py[2] = c2.y;   c.pw[2] = c2.w;
            c.px[3] = end.x;  c.py[3] = end.y;  c.pw[3] = end.w;
            curves.push_back(c);
            cur = end;
            idx += 3;
        }
        else
        {
            throw std::invalid_argument(
                "conv_pressure_stroke: unsupported path command");
        }
    }

    if(curves.empty())
        return;

    // Closed subpath: either the endpoints already coincide (widths must
    // match) or an implicit linear closing segment is appended.
    bool same_endpoint = cur.x == first.x && cur.y == first.y;
    if(closed)
    {
        if(same_endpoint)
        {
            if(cur.w != first.w)
                throw std::invalid_argument(
                    "conv_pressure_stroke: closed subpath endpoint widths differ");
        }
        else
        {
            curves.push_back(make_line(cur, first));
        }
    }

    // Zero width everywhere means no coverage at all.
    bool any_width = false;
    for(unsigned i = 0; i < nodes.size(); ++i)
        if(nodes[i].w > 0.0) { any_width = true; break; }
    if(!any_width)
        return;

    // Validate and sample every segment.
    std::vector<segment_geom> segs;
    segs.reserve(curves.size());
    for(unsigned i = 0; i < curves.size(); ++i)
    {
        const curve_type& c = curves[i];
        if(c.degree == 1)
        {
            if(c.px[0] == c.px[1] && c.py[0] == c.py[1])
                throw std::invalid_argument(
                    "conv_pressure_stroke: zero-length line segment");
        }
        else
        {
            if(curve_centerline_constant(c))
                throw std::invalid_argument(
                    "conv_pressure_stroke: curve has a constant centerline");
            if(has_interior_stall(c))
                throw std::invalid_argument(
                    "conv_pressure_stroke: zero-speed point inside a curve segment");
        }

        segment_geom g;
        g.curve = c;
        g.states = sample_segment(c, tolerance);
        g.tan_l_ok[0] = boundary_tangent(c, 0.0, true,  g.tan_l[0]);
        g.tan_l_ok[1] = boundary_tangent(c, 1.0, true,  g.tan_l[1]);
        g.tan_r_ok[0] = boundary_tangent(c, 0.0, false, g.tan_r[0]);
        g.tan_r_ok[1] = boundary_tangent(c, 1.0, false, g.tan_r[1]);
        segs.push_back(g);
    }

    if(closed)
    {
        // Two independently closed boundaries with opposite traversal.
        std::vector<vec2> left;
        build_side(segs, true, join, miter_limit, true, left);
        if(join == miter_join_revert)
        {
            vec2 m;
            if(try_miter(segs.back(), segs.front(), true, miter_limit, m))
                append_dedup(left, m);
        }
        append_dedup(left, segs.front().states.front().qleft);

        std::vector<vec2> right;
        build_side(segs, false, join, miter_limit, false, right);
        if(join == miter_join_revert)
        {
            vec2 m;
            if(try_miter(segs.back(), segs.front(), false, miter_limit, m))
                append_dedup(right, m);
        }
        append_dedup(right, segs.front().states.front().qright);

        if(left.size() >= 3) out.push_back(make_contour(left));
        if(right.size() >= 3) out.push_back(make_contour(right));
    }
    else
    {
        // One closed outline: left forward, end cap, right backward,
        // start cap.
        std::vector<vec2> pts;
        build_side(segs, true, join, miter_limit, true, pts);
        append_end_cap(pts, segs.back().states.back(), cap, tolerance);
        build_side(segs, false, join, miter_limit, false, pts);
        append_start_cap(pts, segs.front().states.front(), cap, tolerance);
        append_dedup(pts, pts.front());
        if(pts.size() >= 4) out.push_back(make_contour(pts));
    }
}

}
}
