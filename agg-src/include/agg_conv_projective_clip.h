//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
//
//----------------------------------------------------------------------------
// Contact: mcseem@antigrain.com
//          mcseemagg@yahoo.com
//          http://www.antigrain.com
//----------------------------------------------------------------------------
//
// conv_projective_clip
//
// A vertex source converter that understands projective (perspective)
// transformations of rational Bezier curves and a rectangular visible
// domain. Unlike "conv_curve followed by conv_transform", the curves are
// kept in their original Bezier parameter space: every user-space control
// point is lifted to homogeneous coordinates (numerator NX, numerator NY,
// homogeneous weight W) and the projected rational curve
//
//     R(t) = (NX(t)/W(t), NY(t)/W(t))
//
// is what gets approximated and clipped. Projected control points are
// NEVER re-interpreted as an ordinary polynomial Bezier.
//
// Visible domain (device space):
//
//   1) W(t) >= 1e-6  (the projective near-plane / horizon guard), and
//   2) the projected point lies inside the configured clip rectangle.
//
// Every retained visible arc of the ORIGINAL curve is approximated by a
// polyline whose two-way (Hausdorff) distance to the true projected arc
// does not exceed the configured approximation tolerance, measured in
// device pixels. Curve/boundary intersections carry the same guarantee.
// Boundary segments added only to re-close a clipped contour lie on the
// visible-domain boundary and are exempt from the arc distance check.
//
// Output commands are restricted to move_to, line_to, end_poly and stop.
// The source path is never modified.
//
//----------------------------------------------------------------------------
#ifndef AGG_CONV_PROJECTIVE_CLIP_INCLUDED
#define AGG_CONV_PROJECTIVE_CLIP_INCLUDED

#include <cmath>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include "agg_basics.h"
#include "agg_trans_perspective.h"

namespace agg
{
    //===============================================conv_projective_clip
    template<class VertexSource>
    class conv_projective_clip
    {
    public:
        // Minimum admissible homogeneous weight. Points with W below it
        // are on the far side of the projective horizon and are invisible.
        static const double horizon_weight_epsilon;

        conv_projective_clip(VertexSource& source,
                             const trans_perspective& perspective) :
            m_source(&source),
            m_clip_x1(0.0),
            m_clip_y1(0.0),
            m_clip_x2(256.0),
            m_clip_y2(256.0),
            m_tolerance(0.25),
            m_path_id(0),
            m_generation(0),
            m_out_index(0)
        {
            snapshot(perspective);
        }

        //--------------------------------------------------------- attach
        void attach(VertexSource& source)
        {
            m_source = &source;
            invalidate();
        }

        //--------------------------------------------------- transformer
        // Takes a snapshot of the matrix; later mutations of the passed
        // object do not affect the converter until it is re-attached.
        void transformer(const trans_perspective& perspective)
        {
            snapshot(perspective);
            invalidate();
        }

        //------------------------------------------------------ clip_box
        void clip_box(double x1, double y1, double x2, double y2)
        {
            if(!std::isfinite(x1) || !std::isfinite(y1) ||
               !std::isfinite(x2) || !std::isfinite(y2) ||
               !(x1 < x2) || !(y1 < y2))
            {
                throw std::invalid_argument(
                    "conv_projective_clip: clip box must have finite, "
                    "strictly increasing boundaries");
            }
            m_clip_x1 = x1;
            m_clip_y1 = y1;
            m_clip_x2 = x2;
            m_clip_y2 = y2;
            invalidate();
        }

        double clip_x1() const { return m_clip_x1; }
        double clip_y1() const { return m_clip_y1; }
        double clip_x2() const { return m_clip_x2; }
        double clip_y2() const { return m_clip_y2; }

        //------------------------------------------ approximation_tolerance
        void approximation_tolerance(double eps)
        {
            if(!std::isfinite(eps) || !(eps > 0.0))
            {
                throw std::invalid_argument(
                    "conv_projective_clip: approximation tolerance must be "
                    "finite and positive");
            }
            m_tolerance = eps;
            invalidate();
        }

        double approximation_tolerance() const { return m_tolerance; }

        //--------------------------------------------------------- rewind
        void rewind(unsigned path_id)
        {
            m_path_id    = path_id;
            m_generation = 0;     // force a full, deterministic rebuild
            m_out_index  = 0;
            m_output.clear();
        }

        //--------------------------------------------------------- vertex
        unsigned vertex(double* x, double* y)
        {
            if(m_generation == 0)
            {
                generate();
                m_generation = 1;
                m_out_index  = 0;
            }
            if(m_out_index >= m_output.size())
            {
                *x = 0.0;
                *y = 0.0;
                return path_cmd_stop;
            }
            const out_vertex& v = m_output[m_out_index++];
            *x = v.x;
            *y = v.y;
            return v.cmd;
        }

    private:
        conv_projective_clip(const conv_projective_clip&);
        const conv_projective_clip& operator = (const conv_projective_clip&);

        //----------------------------------------------------- data types
        struct hpoint
        {
            // Homogeneous image of a user-space Bezier control point:
            // device point = (nx / w, ny / w), w is the projective weight.
            double nx, ny, w;
            hpoint() : nx(0.0), ny(0.0), w(1.0) {}
            hpoint(double nx_, double ny_, double w_) :
                nx(nx_), ny(ny_), w(w_) {}
        };

        struct out_vertex
        {
            double   x, y;
            unsigned cmd;
            out_vertex() : x(0.0), y(0.0), cmd(0) {}
            out_vertex(double x_, double y_, unsigned cmd_) :
                x(x_), y(y_), cmd(cmd_) {}
        };

        struct fragment
        {
            std::vector<point_d> pts; // device polyline, exact curve points
            unsigned start_mask;      // constraints vanishing at t=a (bit j)
            unsigned end_mask;        // constraints vanishing at t=b (bit j)
            bool start_natural;       // a == 0 (primitive junction)
            bool end_natural;         // b == 1 (primitive junction)
            fragment() :
                start_mask(0), end_mask(0),
                start_natural(false), end_natural(false) {}
        };

        struct subpath
        {
            std::vector<fragment> frags;
            std::vector<point_d>  user_points; // winding reference
            bool closed;
            subpath() : closed(false) {}
        };

        // A vertex of the convex visible polygon K = clip box intersected
        // with the device-space horizon half-plane.
        struct kpoint
        {
            double x, y;
            kpoint() : x(0.0), y(0.0) {}
            kpoint(double x_, double y_) : x(x_), y(y_) {}
        };

        // Snapshot of a trans_perspective matrix (nine coefficients).
        double m_sx, m_shy, m_w0, m_shx, m_sy, m_w1, m_tx, m_ty, m_w2;

        VertexSource* m_source;
        double        m_clip_x1, m_clip_y1, m_clip_x2, m_clip_y2;
        double        m_tolerance;
        unsigned      m_path_id;
        unsigned      m_generation;
        unsigned      m_out_index;
        std::vector<out_vertex> m_output;

        void invalidate()
        {
            m_generation = 0;
            m_out_index  = 0;
            m_output.clear();
        }

        void snapshot(const trans_perspective& p)
        {
            m_sx = p.sx;  m_shy = p.shy; m_w0 = p.w0;
            m_shx = p.shx; m_sy = p.sy;  m_w1 = p.w1;
            m_tx = p.tx;  m_ty = p.ty;  m_w2 = p.w2;
        }

        //--------------------------------------------- homogeneous lifting
        hpoint lift(double x, double y) const
        {
            return hpoint(x * m_sx + y * m_shx + m_tx,
                          x * m_shy + y * m_sy + m_ty,
                          x * m_w0 + y * m_w1 + m_w2);
        }

        static point_d project(const hpoint& h)
        {
            double inv = 1.0 / h.w;
            return point_d(h.nx * inv, h.ny * inv);
        }

        //------------------------------------------- Bezier (de Casteljau)
        static hpoint eval_h(const std::vector<hpoint>& c, double t)
        {
            std::vector<hpoint> b = c;
            unsigned n = unsigned(b.size());
            for(unsigned k = 1; k < n; ++k)
            {
                for(unsigned i = 0; i < n - k; ++i)
                {
                    b[i].nx = (1.0 - t) * b[i].nx + t * b[i + 1].nx;
                    b[i].ny = (1.0 - t) * b[i].ny + t * b[i + 1].ny;
                    b[i].w  = (1.0 - t) * b[i].w  + t * b[i + 1].w;
                }
            }
            return b[0];
        }

        static void split_h(const std::vector<hpoint>& c, double t,
                            std::vector<hpoint>& left,
                            std::vector<hpoint>& right)
        {
            unsigned n = unsigned(c.size());
            left.resize(n);
            right.resize(n);
            std::vector<hpoint> b = c;
            left[0]      = b[0];
            right[n - 1] = b[n - 1];
            for(unsigned k = 1; k < n; ++k)
            {
                for(unsigned i = 0; i < n - k; ++i)
                {
                    b[i].nx = (1.0 - t) * b[i].nx + t * b[i + 1].nx;
                    b[i].ny = (1.0 - t) * b[i].ny + t * b[i + 1].ny;
                    b[i].w  = (1.0 - t) * b[i].w  + t * b[i + 1].w;
                }
                left[k]              = b[0];
                right[n - 1 - k]     = b[n - 1 - k];
            }
        }

        // Homogeneous control polygon of the sub-arc [a, b].
        void sub_arc(const std::vector<hpoint>& c,
                     double a, double b,
                     std::vector<hpoint>& out) const
        {
            if(a <= 0.0 && b >= 1.0)
            {
                out = c;
                return;
            }
            std::vector<hpoint> left, right;
            if(a <= 0.0)
            {
                split_h(c, b, left, right);
                out = left;
                return;
            }
            split_h(c, a, left, right);
            if(b >= 1.0)
            {
                out = right;
                return;
            }
            std::vector<hpoint> l2, r2;
            double t = (b - a) / (1.0 - a);
            split_h(right, t, l2, r2);
            out = l2;
        }

        static double eval_scalar(const std::vector<double>& c, double t)
        {
            std::vector<double> b = c;
            unsigned n = unsigned(b.size());
            for(unsigned k = 1; k < n; ++k)
            {
                for(unsigned i = 0; i < n - k; ++i)
                {
                    b[i] = (1.0 - t) * b[i] + t * b[i + 1];
                }
            }
            return b[0];
        }

        static void split_scalar(const std::vector<double>& c, double t,
                                 std::vector<double>& left,
                                 std::vector<double>& right)
        {
            unsigned n = unsigned(c.size());
            left.resize(n);
            right.resize(n);
            std::vector<double> b = c;
            left[0]      = b[0];
            right[n - 1] = b[n - 1];
            for(unsigned k = 1; k < n; ++k)
            {
                for(unsigned i = 0; i < n - k; ++i)
                {
                    b[i] = (1.0 - t) * b[i] + t * b[i + 1];
                }
                left[k]          = b[0];
                right[n - 1 - k] = b[n - 1 - k];
            }
        }

        //--------------------------------------------- constraint roots
        // Isolate every root of the Bernstein polynomial g(t) == 0 on
        // (0, 1) by recursive de Casteljau subdivision. A control polygon
        // strictly one-signed cannot contain a sign-changing root, so no
        // root interval is ever missed. A zero landing exactly on a cut
        // point is recorded explicitly, otherwise both one-signed children
        // would discard it.
        static void bernstein_roots(const std::vector<double>& c,
                                    double a, double b,
                                    unsigned depth,
                                    std::vector<double>& roots)
        {
            double vmax = 0.0;
            for(size_t i = 0; i < c.size(); ++i)
            {
                double v = std::fabs(c[i]);
                if(v > vmax) vmax = v;
            }
            double eps = 1e-14 * (vmax > 1.0 ? vmax : 1.0);
            if(vmax <= eps) return;           // identically zero

            double vmin = c[0];
            double vtop = c[0];
            for(size_t i = 1; i < c.size(); ++i)
            {
                if(c[i] < vmin) vmin = c[i];
                if(c[i] > vtop) vtop = c[i];
            }
            if(vmin >= -eps) return;          // non-negative everywhere
            if(vtop <=  eps) return;          // non-positive everywhere

            if(b - a < 1e-12 || depth >= 64)
            {
                roots.push_back((a + b) * 0.5);
                return;
            }
            std::vector<double> left, right;
            split_scalar(c, 0.5, left, right);
            double mid = (a + b) * 0.5;
            if(std::fabs(eval_scalar(c, 0.5)) <= eps)
            {
                roots.push_back(mid);         // root exactly at the cut
            }
            bernstein_roots(left,  a, mid, depth + 1, roots);
            bernstein_roots(right, mid, b, depth + 1, roots);
        }

        // Visibility constraints g_j(t) >= 0, j = 0..4:
        //   0: W >= horizon_weight_epsilon
        //   1: x >= x1   2: x <= x2
        //   3: y >= y1   4: y <= y2
        // Each is a Bernstein polynomial with the same degree as the curve,
        // because NX, NY, W are Bernstein polynomials and g_j is a linear
        // combination of them (no division involved).
        void constraint_coeffs(const std::vector<hpoint>& h,
                               double g[5][4],
                               unsigned& degree) const
        {
            degree = unsigned(h.size()) - 1;
            for(unsigned i = 0; i <= degree; ++i)
            {
                g[0][i] = h[i].w - horizon_weight_epsilon;
                g[1][i] = h[i].nx - m_clip_x1 * h[i].w;
                g[2][i] = m_clip_x2 * h[i].w - h[i].nx;
                g[3][i] = h[i].ny - m_clip_y1 * h[i].w;
                g[4][i] = m_clip_y2 * h[i].w - h[i].ny;
            }
        }

        struct root_info
        {
            double   t;
            unsigned mask; // constraints that vanish at this parameter
            root_info(double t_, unsigned mask_) : t(t_), mask(mask_) {}
        };

        std::vector<root_info>
        find_roots(const std::vector<hpoint>& h) const
        {
            unsigned degree = 0;
            double g[5][4];
            constraint_coeffs(h, g, degree);

            std::vector<double> all_roots;
            for(unsigned j = 0; j < 5; ++j)
            {
                std::vector<double> c(g[j], g[j] + degree + 1);
                bernstein_roots(c, 0.0, 1.0, 0, all_roots);
            }
            std::sort(all_roots.begin(), all_roots.end());

            // Roots are kept distinct (no parameter-proximity merging): a
            // genuinely tiny visible interval between two numerically close
            // but analytically distinct crossings must not be swallowed.
            // At every root we still evaluate all five constraints, so a
            // corner crossing naturally collects several mask bits.
            std::vector<root_info> result;
            for(size_t k = 0; k < all_roots.size(); ++k)
            {
                double t = all_roots[k];
                unsigned mask = 0;
                for(unsigned j = 0; j < 5; ++j)
                {
                    std::vector<double> c(g[j], g[j] + degree + 1);
                    double scale = 1.0;
                    for(unsigned i = 0; i <= degree; ++i)
                    {
                        if(std::fabs(g[j][i]) > scale)
                            scale = std::fabs(g[j][i]);
                    }
                    if(std::fabs(eval_scalar(c, t)) <= 1e-9 * scale)
                    {
                        mask |= (1u << j);
                    }
                }
                result.push_back(root_info(t, mask));
            }
            return result;
        }

        bool visible_at(const std::vector<hpoint>& h, double t) const
        {
            hpoint v = eval_h(h, t);
            if(!(v.w >= horizon_weight_epsilon)) return false;
            point_d p = project(v);
            return p.x >= m_clip_x1 && p.x <= m_clip_x2 &&
                   p.y >= m_clip_y1 && p.y <= m_clip_y2;
        }

        //------------------------------------------- guaranteed flattening
        // With all homogeneous weights positive, the projected rational
        // curve is a convex combination of its projected control points.
        // Requiring every interior control point to stay within
        // tolerance/2 of the chord (perpendicular distance and longitudinal
        // overshoot both checked) bounds every point of the arc, and every
        // point of the chord, to tolerance/sqrt(2) < tolerance of the
        // chord segment. The midpoint alone is never trusted, so internal
        // cusps and same-endpoint loops cannot pass an under-tessellated
        // curve.
        bool flat_enough(const std::vector<point_d>& q) const
        {
            const double half = 0.5 * m_tolerance;
            point_d a = q.front();
            point_d b = q.back();
            double dx = b.x - a.x;
            double dy = b.y - a.y;
            double len2 = dx * dx + dy * dy;
            if(len2 < 1e-20)
            {
                for(size_t i = 1; i + 1 < q.size(); ++i)
                {
                    double ex = q[i].x - a.x;
                    double ey = q[i].y - a.y;
                    if(std::sqrt(ex * ex + ey * ey) > half) return false;
                }
                return true;
            }
            double len = std::sqrt(len2);
            double ux = dx / len;
            double uy = dy / len;
            double max_perp = 0.0;
            double max_over = 0.0;
            for(size_t i = 1; i + 1 < q.size(); ++i)
            {
                double ex = q[i].x - a.x;
                double ey = q[i].y - a.y;
                double s  = ex * ux + ey * uy;
                double px = ex - s * ux;
                double py = ey - s * uy;
                double perp = std::sqrt(px * px + py * py);
                if(perp > max_perp) max_perp = perp;
                double over = 0.0;
                if(s < 0.0)      over = -s;
                else if(s > len) over = s - len;
                if(over > max_over) max_over = over;
            }
            return max_perp <= half && max_over <= half;
        }

        void flatten_rec(const std::vector<hpoint>& c,
                         unsigned depth,
                         std::vector<point_d>& out) const
        {
            bool weights_ok = true;
            for(size_t i = 0; i < c.size(); ++i)
            {
                if(c[i].w < horizon_weight_epsilon)
                {
                    weights_ok = false;
                    break;
                }
            }
            if(weights_ok)
            {
                std::vector<point_d> q(c.size());
                for(size_t i = 0; i < c.size(); ++i)
                    q[i] = project(c[i]);
                if(flat_enough(q))
                {
                    out.push_back(q.back());
                    return;
                }
            }
            if(depth >= 64)
            {
                // Degenerate safety net; the leaf interval is ~2^-64 wide
                // and the end point is an exact evaluation with W >= eps.
                out.push_back(project(eval_h(c, 1.0)));
                return;
            }
            std::vector<hpoint> left, right;
            split_h(c, 0.5, left, right);
            flatten_rec(left,  depth + 1, out);
            flatten_rec(right, depth + 1, out);
        }

        //----------------------------------- per-primitive visible fragments
        std::vector<fragment>
        process_primitive(const std::vector<hpoint>& h) const
        {
            std::vector<fragment> result;
            std::vector<root_info> roots = find_roots(h);

            // Unique cut parameters; masks of roots landing on the very
            // same parameter (corner crossings) are unioned.
            std::vector<double> cut_t;
            std::vector<unsigned> cut_mask;
            for(size_t i = 0; i < roots.size(); ++i)
            {
                if(cut_t.empty() || roots[i].t != cut_t.back())
                {
                    cut_t.push_back(roots[i].t);
                    cut_mask.push_back(roots[i].mask);
                }
                else
                {
                    cut_mask.back() |= roots[i].mask;
                }
            }

            // Coalesce parameter-distinct crossings whose device points
            // coincide (numerical duplicates of one corner crossing).
            // Crossings with distinct device points are real slivers and
            // are never collapsed.
            std::vector<double> uniq_t;
            std::vector<unsigned> uniq_mask;
            for(size_t i = 0; i < cut_t.size(); ++i)
            {
                point_d pi = project(eval_h(h, cut_t[i]));
                if(!uniq_t.empty())
                {
                    point_d pj = project(eval_h(h, uniq_t.back()));
                    double d = std::sqrt((pi.x - pj.x) * (pi.x - pj.x) +
                                         (pi.y - pj.y) * (pi.y - pj.y));
                    double scale = std::fabs(pi.x);
                    if(std::fabs(pi.y) > scale) scale = std::fabs(pi.y);
                    if(std::fabs(pj.x) > scale) scale = std::fabs(pj.x);
                    if(std::fabs(pj.y) > scale) scale = std::fabs(pj.y);
                    if(scale < 1.0) scale = 1.0;
                    if(d <= 1e-11 * scale)
                    {
                        uniq_mask.back() |= cut_mask[i];
                        continue;
                    }
                }
                uniq_t.push_back(cut_t[i]);
                uniq_mask.push_back(cut_mask[i]);
            }

            std::vector<double> cuts;
            cuts.push_back(0.0);
            for(size_t i = 0; i < uniq_t.size(); ++i)
                cuts.push_back(uniq_t[i]);
            cuts.push_back(1.0);

            for(size_t k = 0; k + 1 < cuts.size(); ++k)
            {
                double a = cuts[k];
                double b = cuts[k + 1];
                if(!(b > a)) continue;
                if(!visible_at(h, (a + b) * 0.5)) continue;

                fragment f;
                f.start_natural = (a == 0.0);
                f.end_natural   = (b == 1.0);
                if(!f.start_natural)
                    f.start_mask = uniq_mask[k - 1];
                if(!f.end_natural)
                    f.end_mask = uniq_mask[k];

                f.pts.push_back(sanitize_point(project(eval_h(h, a))));
                // Every retained arc goes through the convex-control
                // flattener, which subdivides by geometric deviation, not
                // by parameter width: a microscopically narrow parameter
                // interval can still span many device pixels near the
                // projective horizon and must not be trusted as flat.
                std::vector<hpoint> arc;
                sub_arc(h, a, b, arc);
                flatten_rec(arc, 0, f.pts);
                result.push_back(f);
            }

            // Merge adjacent visible intervals separated only by a tangent
            // touch of a constraint (the curve stays visible on both sides).
            std::vector<fragment> merged;
            for(size_t i = 0; i < result.size(); ++i)
            {
                if(!merged.empty() &&
                   !merged.back().end_natural &&
                   !result[i].start_natural &&
                   same_point(merged.back().pts.back(),
                              result[i].pts.front()))
                {
                    fragment& prev = merged.back();
                    prev.pts.insert(prev.pts.end(),
                                    result[i].pts.begin() + 1,
                                    result[i].pts.end());
                    prev.end_natural = result[i].end_natural;
                    prev.end_mask    = result[i].end_mask;
                }
                else
                {
                    merged.push_back(result[i]);
                }
            }
            return merged;
        }

        static bool same_point(const point_d& a, const point_d& b)
        {
            double dx = a.x - b.x;
            double dy = a.y - b.y;
            return dx * dx + dy * dy < 1e-20;
        }

        //---------------------------------------- output point sanitization
        point_d sanitize_point(point_d p) const
        {
            if(!std::isfinite(p.x) || !std::isfinite(p.y))
            {
                // Defensive only: every emitted point is an exact evaluation
                // on a visible interval, so this branch is unreachable.
                return point_d((m_clip_x1 + m_clip_x2) * 0.5,
                               (m_clip_y1 + m_clip_y2) * 0.5);
            }
            const double snap = 1e-9;
            double span_x = m_clip_x2 - m_clip_x1;
            double span_y = m_clip_y2 - m_clip_y1;
            double sx = snap * (span_x > 1.0 ? span_x : 1.0);
            double sy = snap * (span_y > 1.0 ? span_y : 1.0);
            if(std::fabs(p.x - m_clip_x1) <= sx) p.x = m_clip_x1;
            if(std::fabs(p.x - m_clip_x2) <= sx) p.x = m_clip_x2;
            if(std::fabs(p.y - m_clip_y1) <= sy) p.y = m_clip_y1;
            if(std::fabs(p.y - m_clip_y2) <= sy) p.y = m_clip_y2;
            if(p.x < m_clip_x1) p.x = m_clip_x1;
            if(p.x > m_clip_x2) p.x = m_clip_x2;
            if(p.y < m_clip_y1) p.y = m_clip_y1;
            if(p.y > m_clip_y2) p.y = m_clip_y2;
            return p;
        }

        //----------------------- visible polygon K in device coordinates
        // The horizon W = eps is a line in user space and therefore also a
        // line in device space. The visible domain is the clip rectangle
        // intersected with one half-plane, i.e. a convex polygon of at most
        // five edges. Its device equation is hx*x + hy*y + hc >= 0 on the
        // visible side; hx == hy == 0 (with hc > 0) means the horizon is
        // inactive.
        void horizon_line(double& hx, double& hy, double& hc) const
        {
            trans_perspective inv(m_sx, m_shy, m_w0,
                                  m_shx, m_sy, m_w1,
                                  m_tx, m_ty, m_w2);
            double lu0 = m_w0;
            double lu1 = m_w1;
            double lu2 = m_w2 - horizon_weight_epsilon;
            if(inv.invert() && std::isfinite(inv.sx))
            {
                // Device line = inverse-matrix transpose times the
                // user-space line (w0, w1, w2 - eps).
                hx = inv.sx * lu0 + inv.shy * lu1 + inv.w0 * lu2;
                hy = inv.shx * lu0 + inv.sy  * lu1 + inv.w1 * lu2;
                hc = inv.tx * lu0 + inv.ty  * lu1 + inv.w2 * lu2;
                if(std::isfinite(hx) && std::isfinite(hy) &&
                   std::isfinite(hc) && (hx != 0.0 || hy != 0.0))
                {
                    fix_horizon_sign(hx, hy, hc);
                    return;
                }
            }

            // Fallback: map two points of the user horizon line and fit
            // the device line directly.
            if(m_w0 == 0.0 && m_w1 == 0.0)
            {
                hx = hy = 0.0;
                hc = (m_w2 >= horizon_weight_epsilon) ? 1.0 : -1.0;
                return;
            }
            double px, py, qx, qy, zx, zy;
            if(std::fabs(m_w0) >= std::fabs(m_w1))
            {
                double x0 = (horizon_weight_epsilon - m_w2) / m_w0;
                px = x0;       py = 0.0;
                qx = x0;       qy = 1.0;
                zx = x0 + 1.0; zy = 0.0; // larger W -> visible side
            }
            else
            {
                double y0 = (horizon_weight_epsilon - m_w2) / m_w1;
                px = 0.0;      py = y0;
                qx = 1.0;      qy = y0;
                zx = 0.0;      zy = y0 + 1.0;
            }
            trans_perspective tr(m_sx, m_shy, m_w0,
                                 m_shx, m_sy, m_w1,
                                 m_tx, m_ty, m_w2);
            tr.transform(&px, &py);
            tr.transform(&qx, &qy);
            tr.transform(&zx, &zy);
            if(!(std::isfinite(px) && std::isfinite(py) &&
                 std::isfinite(qx) && std::isfinite(qy) &&
                 std::isfinite(zx) && std::isfinite(zy)))
            {
                hx = hy = 0.0;
                hc = 1.0; // degenerate matrix: horizon walk disabled
                return;
            }
            hx = qy - py;
            hy = px - qx;
            hc = qx * py - px * qy;
            if(hx * zx + hy * zy + hc < 0.0)
            {
                hx = -hx; hy = -hy; hc = -hc;
            }
        }

        void fix_horizon_sign(double& hx, double& hy, double& hc) const
        {
            // Orient the inequality against a known-visible user point.
            double ux = 0.0, uy = 0.0;
            double wz = m_w2;
            if(wz < horizon_weight_epsilon)
            {
                if(m_w0 != 0.0)      ux = (1.0 - m_w2) / m_w0;
                else if(m_w1 != 0.0) uy = (1.0 - m_w2) / m_w1;
                else return;
                wz = 1.0;
            }
            if(wz < horizon_weight_epsilon) return;
            trans_perspective tr(m_sx, m_shy, m_w0,
                                 m_shx, m_sy, m_w1,
                                 m_tx, m_ty, m_w2);
            tr.transform(&ux, &uy);
            if(std::isfinite(ux) && std::isfinite(uy) &&
               hx * ux + hy * uy + hc < 0.0)
            {
                hx = -hx; hy = -hy; hc = -hc;
            }
        }

        static double horizon_value(double hx, double hy, double hc,
                                    double x, double y)
        {
            return hx * x + hy * y + hc;
        }

        // Build K as a CCW polygon (mathematical convention): the rectangle
        // corners clipped against the horizon half-plane. Parallel arrays:
        // edge_tags[i] is the constraint index (0 = horizon, 1..4 box
        // edges) of the boundary edge verts[i] -> verts[(i+1) % n].
        void build_k(std::vector<kpoint>& verts,
                     std::vector<unsigned>& edge_tags,
                     double hx, double hy, double hc) const
        {
            const kpoint rect[4] =
            {
                kpoint(m_clip_x1, m_clip_y1),
                kpoint(m_clip_x2, m_clip_y1),
                kpoint(m_clip_x2, m_clip_y2),
                kpoint(m_clip_x1, m_clip_y2)
            };
            // Outgoing rect edge leaving corner i:
            // 0->1: y=y1 (j=3)  1->2: x=x2 (j=2)
            // 2->3: y=y2 (j=4)  3->0: x=x1 (j=1)
            const unsigned out_tag[4] = { 3, 2, 4, 1 };

            const double tol = 1e-12;
            std::vector<kpoint> in;
            std::vector<unsigned> enter_tag; // tag of edge entering vertex
            size_t n = 4;
            for(size_t i = 0; i < n; ++i)
            {
                kpoint s = rect[(i + n - 1) % n];
                kpoint e = rect[i];
                double ds = horizon_value(hx, hy, hc, s.x, s.y);
                double de = horizon_value(hx, hy, hc, e.x, e.y);
                bool sin = ds >= -tol;
                bool ein = de >= -tol;
                if(ein)
                {
                    if(!sin)
                    {
                        double u = ds / (ds - de);
                        in.push_back(kpoint(s.x + u * (e.x - s.x),
                                            s.y + u * (e.y - s.y)));
                        enter_tag.push_back(0);
                    }
                    in.push_back(e);
                    enter_tag.push_back(out_tag[(i + n - 1) % n]);
                }
                else if(sin)
                {
                    double u = ds / (ds - de);
                    in.push_back(kpoint(s.x + u * (e.x - s.x),
                                        s.y + u * (e.y - s.y)));
                    enter_tag.push_back(out_tag[(i + n - 1) % n]);
                }
            }

            verts = in;
            edge_tags.resize(verts.size());
            for(size_t i = 0; i < verts.size(); ++i)
            {
                // Edge verts[i] -> verts[i+1] enters vertex i+1.
                edge_tags[i] = enter_tag[(i + 1) % verts.size()];
            }
        }

        static bool on_segment(kpoint a, kpoint b, point_d p, double tol)
        {
            double dx = b.x - a.x;
            double dy = b.y - a.y;
            double len2 = dx * dx + dy * dy;
            double u = len2 > 1e-24 ?
                       ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0;
            if(u < -tol || u > 1.0 + tol) return false;
            double uu = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
            double qx = a.x + uu * dx;
            double qy = a.y + uu * dy;
            return std::sqrt((p.x - qx) * (p.x - qx) +
                             (p.y - qy) * (p.y - qy)) <= tol;
        }

        // Snap p onto a K edge whose tag is one of the mask bits. A zero
        // mask scans every edge (junctions that happen to sit on a border).
        // Returns the edge index, or -1 if none matches.
        int snap_to_edge(const std::vector<kpoint>& verts,
                         const std::vector<unsigned>& edge_tags,
                         unsigned mask,
                         point_d& p) const
        {
            double tol = 1e-7 * ((m_clip_x2 - m_clip_x1) +
                                 (m_clip_y2 - m_clip_y1));
            if(tol < 1e-7) tol = 1e-7;
            int found = -1;
            for(size_t i = 0; i < verts.size(); ++i)
            {
                if(mask != 0 && (mask & (1u << edge_tags[i])) == 0) continue;
                kpoint a = verts[i];
                kpoint b = verts[(i + 1) % verts.size()];
                if(on_segment(a, b, p, tol))
                {
                    double dx = b.x - a.x;
                    double dy = b.y - a.y;
                    double len2 = dx * dx + dy * dy;
                    double u = len2 > 1e-24 ?
                               ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2
                               : 0.0;
                    if(u < 0.0) u = 0.0;
                    if(u > 1.0) u = 1.0;
                    p = point_d(a.x + u * dx, a.y + u * dy);
                    found = int(i);
                    break;
                }
            }
            return found;
        }

        // Append the boundary walk of K from point p to point q.
        void boundary_walk(const std::vector<kpoint>& verts,
                           const std::vector<unsigned>& edge_tags,
                           point_d p, unsigned mask_p,
                           point_d q, unsigned mask_q,
                           bool ccw,
                           std::vector<point_d>& out) const
        {
            point_d sp = p;
            point_d sq = q;
            int ep = snap_to_edge(verts, edge_tags, mask_p, sp);
            int eq = snap_to_edge(verts, edge_tags, mask_q, sq);
            size_t n = verts.size();
            if(ep < 0 || eq < 0 || n == 0)
            {
                if(!same_point(sp, sq)) out.push_back(sanitize_point(sq));
                return;
            }
            if(ep == eq)
            {
                if(!same_point(sp, sq)) out.push_back(sanitize_point(sq));
                return;
            }
            int i = ep;
            while(i != eq)
            {
                int corner = ccw ? int((i + 1) % n)
                                 : int((i + n - 1) % n);
                out.push_back(sanitize_point(point_d(verts[corner].x,
                                                      verts[corner].y)));
                i = ccw ? int((i + 1) % n) : int((i + n - 1) % n);
            }
            if(!same_point(sq, out.empty() ? sp :
                           point_d(out.back().x, out.back().y)))
            {
                out.push_back(sanitize_point(sq));
            }
        }

        static double ring_signed_area(const std::vector<out_vertex>& ring)
        {
            double area = 0.0;
            for(size_t i = 0; i < ring.size(); ++i)
            {
                const out_vertex& a = ring[i];
                const out_vertex& b = ring[(i + 1) % ring.size()];
                area += a.x * b.y - b.x * a.y;
            }
            return 0.5 * area;
        }

        static double user_signed_area(const std::vector<point_d>& pts)
        {
            if(pts.size() < 3) return 0.0;
            double area = 0.0;
            for(size_t i = 0; i < pts.size(); ++i)
            {
                const point_d& a = pts[i];
                const point_d& b = pts[(i + 1) % pts.size()];
                area += a.x * b.y - b.x * a.y;
            }
            return 0.5 * area;
        }

        //--------------------------------------------- source path ingestion
        void append_fragment_points(const fragment& f,
                                    std::vector<out_vertex>& ring)
        {
            for(size_t i = 0; i < f.pts.size(); ++i)
            {
                point_d p = sanitize_point(f.pts[i]);
                if(!ring.empty() &&
                   same_point(point_d(ring.back().x, ring.back().y), p))
                {
                    continue;
                }
                unsigned cmd = ring.empty() ? path_cmd_move_to
                                            : path_cmd_line_to;
                ring.push_back(out_vertex(p.x, p.y, cmd));
            }
        }

        void emit_open_subpath(subpath& sub)
        {
            // Every fragment cut off by the visibility boundary restarts
            // with move_to; invisible gaps are never bridged. Fragments
            // joined naturally across a primitive boundary stay one chain.
            bool pen_down = false;
            point_d pen(0.0, 0.0);
            for(size_t k = 0; k < sub.frags.size(); ++k)
            {
                const fragment& f = sub.frags[k];
                if(f.pts.empty()) continue;
                bool joined = pen_down &&
                              f.start_natural &&
                              same_point(pen, sanitize_point(f.pts.front()));
                for(size_t i = 0; i < f.pts.size(); ++i)
                {
                    point_d p = sanitize_point(f.pts[i]);
                    if(pen_down && same_point(pen, p)) continue;
                    unsigned cmd = (!pen_down || (!joined && i == 0)) ?
                                   path_cmd_move_to : path_cmd_line_to;
                    m_output.push_back(out_vertex(p.x, p.y, cmd));
                    pen = p;
                    pen_down = true;
                }
            }
            if(pen_down)
            {
                m_output.push_back(
                    out_vertex(0.0, 0.0, path_cmd_end_poly | path_flags_none));
            }
        }

        // Choose the boundary walk direction. K is stored in its positive
        // (CCW) order. The image contour winding is the user-space winding
        // times the sign of the projective Jacobian det(M)/W^3, whose sign
        // is constant everywhere on the W > 0 side.
        bool choose_walk_forward(const subpath& sub,
                                 const std::vector<kpoint>& verts,
                                 const std::vector<unsigned>& edge_tags) const
        {
            double ua = user_signed_area(sub.user_points);
            trans_perspective tr(m_sx, m_shy, m_w0,
                                 m_shx, m_sy, m_w1,
                                 m_tx, m_ty, m_w2);
            double det = tr.determinant();
            if(ua != 0.0 && std::isfinite(det) && det != 0.0)
            {
                return (ua * det) > 0.0;
            }

            // Fallback: at the first clipped exit compare the curve travel
            // tangent with the forward (CCW) direction of its K edge.
            for(size_t k = 0; k < sub.frags.size(); ++k)
            {
                const fragment& f = sub.frags[k];
                if(f.end_natural || f.pts.size() < 2) continue;
                point_d p = sanitize_point(f.pts.back());
                point_d a = sanitize_point(f.pts[f.pts.size() - 2]);
                double vx = p.x - a.x;
                double vy = p.y - a.y;
                if(vx == 0.0 && vy == 0.0) continue;
                point_d sp = p;
                int edge = snap_to_edge(verts, edge_tags, f.end_mask, sp);
                if(edge < 0) continue;
                kpoint e0 = verts[edge];
                kpoint e1 = verts[(edge + 1) % verts.size()];
                double tx = e1.x - e0.x;
                double ty = e1.y - e0.y;
                return (vx * ty - vy * tx) >= 0.0;
            }
            return true;
        }

        void emit_closed_subpath(subpath& sub)
        {
            // Stitch the visible fragments with walks on the boundary of
            // the convex visible polygon K, so the clipped contour keeps a
            // directed interior suitable for the AGG fill rasterizer.
            double hx, hy, hc;
            horizon_line(hx, hy, hc);
            std::vector<kpoint> kverts;
            std::vector<unsigned> ktags;
            build_k(kverts, ktags, hx, hy, hc);
            bool forward = choose_walk_forward(sub, kverts, ktags);

            std::vector<out_vertex> ring;
            append_fragment_points(sub.frags.front(), ring);
            point_d prev_end  = sanitize_point(sub.frags.front().pts.back());
            unsigned prev_mask = sub.frags.front().end_mask;

            for(size_t k = 1; k < sub.frags.size(); ++k)
            {
                const fragment& f = sub.frags[k];
                point_d next_start = sanitize_point(f.pts.front());
                if(!same_point(prev_end, next_start))
                {
                    // A gap between two retained fragments means the curve
                    // left the visible domain in between; both endpoints lie
                    // on the boundary of K and the gap is closed by a walk
                    // along K's boundary. Natural endpoints carry mask 0 and
                    // the edge is located by scanning every boundary edge.
                    std::vector<point_d> walk;
                    boundary_walk(kverts, ktags,
                                  prev_end, prev_mask,
                                  next_start, f.start_mask,
                                  forward, walk);
                    for(size_t i = 0; i < walk.size(); ++i)
                    {
                        point_d p = sanitize_point(walk[i]);
                        if(!ring.empty() &&
                           same_point(point_d(ring.back().x, ring.back().y), p))
                            continue;
                        ring.push_back(out_vertex(p.x, p.y,
                                                  path_cmd_line_to));
                    }
                }
                append_fragment_points(f, ring);
                prev_end  = sanitize_point(f.pts.back());
                prev_mask = f.end_mask;
            }

            if(ring.size() > 1)
            {
                point_d first_p(ring.front().x, ring.front().y);
                if(!same_point(prev_end, first_p))
                {
                    std::vector<point_d> walk;
                    boundary_walk(kverts, ktags,
                                  prev_end, prev_mask,
                                  first_p, sub.frags.front().start_mask,
                                  forward, walk);
                    for(size_t i = 0; i < walk.size(); ++i)
                    {
                        point_d p = sanitize_point(walk[i]);
                        if(!ring.empty() &&
                           same_point(point_d(ring.back().x, ring.back().y), p))
                            continue;
                        ring.push_back(out_vertex(p.x, p.y,
                                                  path_cmd_line_to));
                    }
                }
            }

            if(ring.size() < 3) return;

            // The orientation flag is derived from the actual emitted
            // geometry, never copied from the source end_poly command.
            // AGG convention (path_storage): negative shoelace area => CW.
            double area = ring_signed_area(ring);
            unsigned orientation = path_flags_none;
            if(area > 0.0)      orientation = path_flags_ccw;
            else if(area < 0.0) orientation = path_flags_cw;

            for(size_t i = 0; i < ring.size(); ++i)
                m_output.push_back(ring[i]);
            m_output.push_back(
                out_vertex(0.0, 0.0,
                           path_cmd_end_poly | path_flags_close |
                           orientation));
        }

        void emit_subpath(subpath& sub)
        {
            if(sub.frags.empty()) return;
            if(sub.closed) emit_closed_subpath(sub);
            else           emit_open_subpath(sub);
        }

        void generate()
        {
            m_output.clear();
            m_out_index = 0;
            m_source->rewind(m_path_id);

            std::vector<subpath> subs;
            subpath cur;
            bool have_sub = false;
            double last_x = 0.0, last_y = 0.0;
            double start_x = 0.0, start_y = 0.0;

            auto add_fragments = [&](const std::vector<hpoint>& h, subpath& s)
            {
                std::vector<fragment> f = process_primitive(h);
                s.frags.insert(s.frags.end(), f.begin(), f.end());
            };

            for(;;)
            {
                double x, y;
                unsigned cmd = m_source->vertex(&x, &y);
                if(is_stop(cmd)) break;

                if(is_end_poly(cmd))
                {
                    if(have_sub)
                    {
                        if(is_close(cmd))
                        {
                            // Implicit closing segment back to the start.
                            std::vector<hpoint> line;
                            line.push_back(lift(last_x, last_y));
                            line.push_back(lift(start_x, start_y));
                            add_fragments(line, cur);
                            cur.closed = true;
                        }
                        subs.push_back(cur);
                        cur = subpath();
                        have_sub = false;
                    }
                    continue;
                }
                if(!is_vertex(cmd)) continue;

                if(is_move_to(cmd))
                {
                    if(have_sub)
                    {
                        // Missing end_poly: finalize the previous contour
                        // as open.
                        subs.push_back(cur);
                        cur = subpath();
                    }
                    have_sub = true;
                    start_x = x;
                    start_y = y;
                    last_x  = x;
                    last_y  = y;
                    cur.user_points.push_back(point_d(x, y));
                    std::vector<hpoint> point;
                    point.push_back(lift(x, y));
                    add_fragments(point, cur);
                }
                else if(is_line_to(cmd))
                {
                    std::vector<hpoint> line;
                    line.push_back(lift(last_x, last_y));
                    line.push_back(lift(x, y));
                    add_fragments(line, cur);
                    cur.user_points.push_back(point_d(x, y));
                    last_x = x;
                    last_y = y;
                }
                else if(is_curve3(cmd))
                {
                    // The command vertex is the control point; one more
                    // vertex carries the end point (see conv_curve).
                    double ex, ey;
                    m_source->vertex(&ex, &ey);
                    std::vector<hpoint> quad;
                    quad.push_back(lift(last_x, last_y));
                    quad.push_back(lift(x, y));
                    quad.push_back(lift(ex, ey));
                    add_fragments(quad, cur);
                    cur.user_points.push_back(point_d(ex, ey));
                    last_x = ex;
                    last_y = ey;
                }
                else if(is_curve4(cmd))
                {
                    // The command vertex is the first control point; two
                    // more vertices carry the second control point and the
                    // end point (see conv_curve).
                    double c2x, c2y, ex, ey;
                    m_source->vertex(&c2x, &c2y);
                    m_source->vertex(&ex, &ey);
                    std::vector<hpoint> cubic;
                    cubic.push_back(lift(last_x, last_y));
                    cubic.push_back(lift(x, y));
                    cubic.push_back(lift(c2x, c2y));
                    cubic.push_back(lift(ex, ey));
                    add_fragments(cubic, cur);
                    cur.user_points.push_back(point_d(ex, ey));
                    last_x = ex;
                    last_y = ey;
                }
            }
            if(have_sub)
            {
                subs.push_back(cur);
            }

            for(size_t i = 0; i < subs.size(); ++i)
            {
                emit_subpath(subs[i]);
            }
        }
    };

    template<class VertexSource>
    const double conv_projective_clip<VertexSource>::horizon_weight_epsilon = 1e-6;

}

#endif
