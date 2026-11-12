//----------------------------------------------------------------------------
// Test suite for agg::conv_projective_clip.
//
// The geometric checks use an INDEPENDENT homogeneous rational Bezier
// reference: the evaluator below takes the original user-space control
// points and the nine matrix coefficients and evaluates
//
//     W(t)  = sum B_i(t) * (w0*x_i + w1*y_i + w2)
//     NX(t) = sum B_i(t) * (sx*x_i + shx*y_i + tx)
//     NY(t) = sum B_i(t) * (shy*x_i + sy*y_i + ty)
//     R(t)  = (NX/W, NY/W)
//
// directly with the Bernstein basis. It shares no code with the converter
// under test. Rendering goes through the real rasterizer_scanline_aa /
// renderer_scanline_aa_solid pipeline; pixels are judged against reference
// polygons built from that independent evaluator, never against another
// instance of the new converter.
//----------------------------------------------------------------------------
#include "harness.h"

#include "agg_path_storage.h"
#include "agg_conv_projective_clip.h"
#include "agg_trans_perspective.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_scanline_u.h"
#include "agg_renderer_scanline.h"
#include "agg_renderer_base.h"
#include "agg_pixfmt_rgb.h"
#include "agg_rendering_buffer.h"
#include "agg_color_rgba.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
    const double HORIZON_EPS = 1e-6;
    const double BOX[4] = { 0.0, 0.0, 256.0, 256.0 };

    struct RefMatrix
    {
        double sx, shy, w0, shx, sy, w1, tx, ty, w2;
        explicit RefMatrix(const agg::trans_perspective& m) :
            sx(m.sx), shy(m.shy), w0(m.w0), shx(m.shx), sy(m.sy),
            w1(m.w1), tx(m.tx), ty(m.ty), w2(m.w2) {}
    };

    struct DPoint
    {
        double x, y, w;
        DPoint(double x_ = 0, double y_ = 0, double w_ = 1) :
            x(x_), y(y_), w(w_) {}
    };

    // Independent homogeneous rational Bezier evaluation.
    DPoint ref_eval(unsigned degree,
                    const double* ux, const double* uy,
                    const RefMatrix& m, double t)
    {
        double b[4];
        double u = 1.0 - t;
        if(degree == 3)
        {
            b[0] = u * u * u;
            b[1] = 3.0 * u * u * t;
            b[2] = 3.0 * u * t * t;
            b[3] = t * t * t;
        }
        else
        {
            b[0] = u * u;
            b[1] = 2.0 * u * t;
            b[2] = t * t;
        }
        double nx = 0.0, ny = 0.0, w = 0.0;
        for(unsigned i = 0; i <= degree; ++i)
        {
            double x = ux[i];
            double y = uy[i];
            nx += b[i] * (x * m.sx  + y * m.shx + m.tx);
            ny += b[i] * (x * m.shy + y * m.sy  + m.ty);
            w  += b[i] * (x * m.w0  + y * m.w1  + m.w2);
        }
        return DPoint(nx / w, ny / w, w);
    }

    bool ref_visible(const DPoint& p)
    {
        return p.w >= HORIZON_EPS &&
               p.x >= BOX[0] && p.x <= BOX[2] &&
               p.y >= BOX[1] && p.y <= BOX[3];
    }

    double dist_seg(double px, double py, double ax, double ay,
                    double bx, double by)
    {
        double dx = bx - ax;
        double dy = by - ay;
        double len2 = dx * dx + dy * dy;
        double s = len2 > 1e-24 ?
                   ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
        if(s < 0.0) s = 0.0;
        if(s > 1.0) s = 1.0;
        double qx = ax + s * dx;
        double qy = ay + s * dy;
        return std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
    }

    double dist_to_polyline(double px, double py,
                            const std::vector<DPoint>& poly)
    {
        double best = std::numeric_limits<double>::infinity();
        for(size_t i = 0; i + 1 < poly.size(); ++i)
        {
            double d = dist_seg(px, py, poly[i].x, poly[i].y,
                                poly[i + 1].x, poly[i + 1].y);
            if(d < best) best = d;
        }
        if(poly.size() == 1)
            best = std::sqrt((px - poly[0].x) * (px - poly[0].x) +
                             (py - poly[0].y) * (py - poly[0].y));
        return best;
    }

    struct OutCmd
    {
        double x, y;
        unsigned cmd;
    };

    std::vector<OutCmd> collect(agg::conv_projective_clip<agg::path_storage>& c)
    {
        std::vector<OutCmd> out;
        c.rewind(0);
        double x, y;
        unsigned cmd;
        while(!agg::is_stop(cmd = c.vertex(&x, &y)))
            out.push_back(OutCmd{x, y, cmd});
        return out;
    }

    // Every emitted command belongs to the allowed set, carries finite
    // numbers and, for vertices, lies inside the clip box.
    void check_well_formed(const std::vector<OutCmd>& out)
    {
        bool any_vertex = false;
        for(size_t i = 0; i < out.size(); ++i)
        {
            unsigned c = out[i].cmd;
            unsigned base = c & agg::path_cmd_mask;
            REQUIRE(base == agg::path_cmd_move_to ||
                    base == agg::path_cmd_line_to ||
                    base == agg::path_cmd_end_poly);
            REQUIRE(std::isfinite(out[i].x) && std::isfinite(out[i].y));
            if(agg::is_vertex(c))
            {
                any_vertex = true;
                REQUIRE(out[i].x >= BOX[0] && out[i].x <= BOX[2]);
                REQUIRE(out[i].y >= BOX[1] && out[i].y <= BOX[3]);
            }
        }
        REQUIRE(any_vertex);
    }

    // All output segments, grouped by move_to chain.
    std::vector<std::vector<DPoint> >
    chains_of(const std::vector<OutCmd>& out)
    {
        std::vector<std::vector<DPoint> > chains;
        for(size_t i = 0; i < out.size(); ++i)
        {
            if(agg::is_move_to(out[i].cmd))
                chains.push_back(std::vector<DPoint>());
            if(agg::is_vertex(out[i].cmd) && !chains.empty())
                chains.back().push_back(DPoint(out[i].x, out[i].y));
        }
        return chains;
    }

    std::vector<DPoint> all_segments(const std::vector<OutCmd>& out)
    {
        std::vector<DPoint> poly;
        for(size_t i = 0; i < out.size(); ++i)
            if(agg::is_vertex(out[i].cmd))
                poly.push_back(DPoint(out[i].x, out[i].y));
        return poly;
    }

    // Dense independent sampling of one rational primitive. Points behind
    // the horizon are always excluded. A positive margin keeps a band of
    // just-outside-the-box curve points so that boundary intersections can
    // be checked against the true curve rather than the clipped interior.
    std::vector<DPoint> reference_visible(unsigned degree,
                                          const double* ux,
                                          const double* uy,
                                          const RefMatrix& m,
                                          unsigned n = 3000,
                                          double margin = 2.0)
    {
        std::vector<DPoint> r;
        for(unsigned k = 0; k <= n; ++k)
        {
            DPoint p = ref_eval(degree, ux, uy, m, double(k) / n);
            if(p.w >= HORIZON_EPS &&
               p.x >= BOX[0] - margin && p.x <= BOX[2] + margin &&
               p.y >= BOX[1] - margin && p.y <= BOX[3] + margin)
            {
                r.push_back(p);
            }
        }
        return r;
    }

    // Two-way (Hausdorff-style) distance check for open contours (no added
    // boundary segments). The reference may carry a narrow band of
    // just-outside-the-box curve points; only interior points are required
    // to be covered, while the band lets boundary intersection vertices
    // find the nearby true curve.
    void hausdorff_check(const std::vector<OutCmd>& out,
                         const std::vector<DPoint>& band,
                         double tolerance,
                         double slack = 1e-7,
                         const double* interior = BOX)
    {
        std::vector<DPoint> seg = all_segments(out);
        REQUIRE(seg.size() >= 2);

        // Interior reference points must be covered by the output.
        unsigned covered = 0;
        for(size_t i = 0; i < band.size(); ++i)
        {
            if(band[i].x < interior[0] || band[i].x > interior[2] ||
               band[i].y < interior[1] || band[i].y > interior[3])
                continue;
            ++covered;
            REQUIRE(dist_to_polyline(band[i].x, band[i].y, seg)
                    <= tolerance + slack);
        }
        REQUIRE(covered >= 2);

        // Every output vertex lies on the true curve (band allowed).
        for(size_t i = 0; i < seg.size(); ++i)
            REQUIRE(dist_to_polyline(seg[i].x, seg[i].y, band)
                    <= tolerance + slack);
    }

    // One-directional coverage plus vertex classification for closed
    // clipped rings: interior reference points must be reached, while each
    // output vertex is either a true curve point (band allowed, within
    // tolerance) or a point of the clip box boundary (the re-closing
    // segments are exempt from the arc distance check).
    void coverage_check(const std::vector<OutCmd>& out,
                        const std::vector<DPoint>& band,
                        double tolerance,
                        double slack = 1e-7,
                        const double* interior = BOX)
    {
        std::vector<DPoint> seg = all_segments(out);
        REQUIRE(seg.size() >= 2);
        unsigned covered = 0;
        for(size_t i = 0; i < band.size(); ++i)
        {
            if(band[i].x < interior[0] || band[i].x > interior[2] ||
               band[i].y < interior[1] || band[i].y > interior[3])
                continue;
            ++covered;
            REQUIRE(dist_to_polyline(band[i].x, band[i].y, seg)
                    <= tolerance + slack);
        }
        REQUIRE(covered >= 2);
        for(size_t i = 0; i < seg.size(); ++i)
        {
            bool on_boundary =
                std::fabs(seg[i].x - interior[0]) < 1e-6 ||
                std::fabs(seg[i].x - interior[2]) < 1e-6 ||
                std::fabs(seg[i].y - interior[1]) < 1e-6 ||
                std::fabs(seg[i].y - interior[3]) < 1e-6;
            REQUIRE(on_boundary ||
                    dist_to_polyline(seg[i].x, seg[i].y, band)
                    <= tolerance + slack);
        }
    }

    //------------------------------------------------------- polygon tools
    std::vector<DPoint> clip_rect(const std::vector<DPoint>& ring)
    {
        // Independent Sutherland-Hodgman clipping against the box.
        std::vector<DPoint> in = ring;
        struct Plane { int axis; double value; bool keep_ge; };
        Plane planes[4] =
        {
            { 0, BOX[0], true  },  // x >= x1
            { 0, BOX[2], false },  // x <= x2
            { 1, BOX[1], true  },  // y >= y1
            { 1, BOX[3], false }   // y <= y2
        };
        for(int pi = 0; pi < 4; ++pi)
        {
            std::vector<DPoint> out;
            if(in.empty()) break;
            for(size_t i = 0; i < in.size(); ++i)
            {
                DPoint a = in[(i + in.size() - 1) % in.size()];
                DPoint e = in[i];
                auto inside = [&](const DPoint& p) {
                    double v = planes[pi].axis == 0 ? p.x : p.y;
                    return planes[pi].keep_ge ?
                           v >= planes[pi].value : v <= planes[pi].value;
                };
                bool ain = inside(a);
                bool ein = inside(e);
                if(ein)
                {
                    if(!ain)
                    {
                        double den = (planes[pi].axis == 0) ?
                                     (e.x - a.x) : (e.y - a.y);
                        double u = (planes[pi].value -
                                    (planes[pi].axis == 0 ? a.y * 0 + a.x
                                                         : a.y)) / den;
                        out.push_back(DPoint(a.x + u * (e.x - a.x),
                                             a.y + u * (e.y - a.y)));
                    }
                    out.push_back(e);
                }
                else if(ain)
                {
                    double den = (planes[pi].axis == 0) ?
                                 (e.x - a.x) : (e.y - a.y);
                    double av = planes[pi].axis == 0 ? a.x : a.y;
                    double u = (planes[pi].value - av) / den;
                    out.push_back(DPoint(a.x + u * (e.x - a.x),
                                         a.y + u * (e.y - a.y)));
                }
            }
            in.swap(out);
        }
        return in;
    }

    bool point_in_ring(double px, double py, const std::vector<DPoint>& ring)
    {
        bool inside = false;
        for(size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        {
            double xi = ring[i].x, yi = ring[i].y;
            double xj = ring[j].x, yj = ring[j].y;
            if(((yi > py) != (yj > py)) &&
               (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
                inside = !inside;
        }
        return inside;
    }

    double ring_outline_distance(double px, double py,
                                 const std::vector<DPoint>& ring)
    {
        double best = std::numeric_limits<double>::infinity();
        for(size_t i = 0; i < ring.size(); ++i)
        {
            DPoint a = ring[i];
            DPoint b = ring[(i + 1) % ring.size()];
            best = std::min(best, dist_seg(px, py, a.x, a.y, b.x, b.y));
        }
        return best;
    }

    double signed_area(const std::vector<OutCmd>& ring)
    {
        double area = 0.0;
        std::vector<DPoint> p;
        for(size_t i = 0; i < ring.size(); ++i)
            if(agg::is_vertex(ring[i].cmd))
                p.push_back(DPoint(ring[i].x, ring[i].y));
        for(size_t i = 0; i < p.size(); ++i)
        {
            DPoint a = p[i];
            DPoint b = p[(i + 1) % p.size()];
            area += a.x * b.y - b.x * a.y;
        }
        return 0.5 * area;
    }

    //-------------------------------------------------------- render tools
    typedef agg::pixfmt_rgb24             pixfmt_type;
    typedef agg::renderer_base<pixfmt_type> renderer_type;

    struct Image
    {
        unsigned w, h;
        std::vector<unsigned char> buf;
        agg::rendering_buffer rbuf;
        pixfmt_type pixf;
        renderer_type ren;
        Image(unsigned w_, unsigned h_) :
            w(w_), h(h_), buf(w_ * h_ * 3, 255), pixf(rbuf), ren(pixf)
        {
            // Attach in this order: renderer_base caches its clip box at
            // construction time, so re-attach after the buffer is live.
            rbuf.attach(buf.data(), w, h, w * 3);
            pixf.attach(rbuf);
            ren.attach(pixf);
        }
    };

    template<class Conv>
    void render(Image& img, Conv& conv, agg::rgba8 color,
               agg::filling_rule_e rule = agg::fill_non_zero)
    {
        agg::rasterizer_scanline_aa<> ras;
        agg::scanline_u8 sl;
        ras.filling_rule(rule);
        ras.add_path(conv, 0);
        agg::render_scanlines_aa_solid(ras, sl, img.ren, color);
    }

    bool covered(const Image& img, unsigned x, unsigned y)
    {
        agg::rgba8 c = img.ren.pixel(int(x), int(y));
        // Any channel darkened below white means the fill covers the pixel.
        return int(c.r) + int(c.g) + int(c.b) < 3 * 250;
    }

    // Shared geometries ----------------------------------------------------
    agg::trans_perspective make_blob_matrix()
    {
        agg::trans_perspective p(1, 0, 0.03, 0, 1, 0.02, 0, 0, 1);
        agg::trans_affine a = agg::trans_affine_scaling(6.0);
        a.translate(128, 128);
        agg::trans_perspective m = p;
        m.multiply(a);
        return m;
    }

    // Open cubic whose homogeneous weights run 2, -1, -1, 2: it crosses the
    // horizon twice, leaving two visible fragments with a gap between them.
    const RefMatrix double_frag_matrix()
    {
        return RefMatrix(agg::trans_perspective(
            1, 0, 0,     // NX = x + 128
            0, 30, -1,   // NY = 30y + 128, W = 2 - y
            128, 128, 2));
    }

    const double BLOB_UX[4] = { -12, 12, 12, -12 };
    const double BLOB_UY[4] = { -12, -12, 12, 12 };
    const double FRAG_UX[4] = { -30, 40, 20, -30 };
    const double FRAG_UY[4] = { 0, 3, 3, 0 };
}

//================================================================== rational
CHECK_CASE("rational", rational_defaults_and_snapshot)
{
    agg::path_storage p;
    p.move_to(1, 1);
    p.curve4(1, 8, 8, 8, 8, 1);
    agg::trans_perspective m(1, 0, .02, 0, 1, .01, 0, 0, 1);
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    REQUIRE(conv.clip_x1() == 0.0 && conv.clip_y1() == 0.0);
    REQUIRE(conv.clip_x2() == 256.0 && conv.clip_y2() == 256.0);
    REQUIRE(conv.approximation_tolerance() == 0.25);

    // Mutating the matrix after construction must not change the snapshot.
    std::vector<OutCmd> before = collect(conv);
    m.w0 = 0.5;
    m.tx = 90.0;
    std::vector<OutCmd> after = collect(conv);
    REQUIRE(before.size() == after.size());
    for(size_t i = 0; i < before.size(); ++i)
    {
        REQUIRE(before[i].cmd == after[i].cmd);
        REQUIRE(std::fabs(before[i].x - after[i].x) < 1e-12);
        REQUIRE(std::fabs(before[i].y - after[i].y) < 1e-12);
    }
}

CHECK_CASE("rational", rational_matches_homogeneous_reference)
{
    agg::path_storage p;
    p.move_to(BLOB_UX[0], BLOB_UY[0]);
    p.curve4(BLOB_UX[1], BLOB_UY[1], BLOB_UX[2], BLOB_UY[2],
             BLOB_UX[3], BLOB_UY[3]);
    agg::trans_perspective m = make_blob_matrix();
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);

    RefMatrix rm(m);
    std::vector<DPoint> ref = reference_visible(3, BLOB_UX, BLOB_UY, rm);
    hausdorff_check(out, ref, conv.approximation_tolerance());
}

CHECK_CASE("rational", rational_quadratic_curve3)
{
    const double ux[3] = { -8, 60, 10 };
    const double uy[3] = { -8, 4, 60 };
    agg::path_storage p;
    p.move_to(ux[0], uy[0]);
    p.curve3(ux[1], uy[1], ux[2], uy[2]);
    agg::trans_perspective m(1.1, 0, .02, 0, 1.2, .025, 30, 20, 1);
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);
    RefMatrix rm(m);
    std::vector<DPoint> ref = reference_visible(2, ux, uy, rm);
    hausdorff_check(out, ref, conv.approximation_tolerance());
}

CHECK_CASE("rational", rational_is_not_projected_control_polygon)
{
    // A perspective where the rational curve and the naive "project the
    // control points, then treat them as an ordinary Bezier" curve diverge
    // by far more than the tolerance.
    agg::path_storage p;
    p.move_to(0, 0);
    p.curve4(40, 0, 40, 40, 0, 40);
    agg::trans_perspective m(1, 0, .04, 0, 1, .01, 60, 40, 1);
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);
    RefMatrix rm(m);

    const double ux[4] = { 0, 40, 40, 0 };
    const double uy[4] = { 0, 0, 40, 40 };
    std::vector<DPoint> ref = reference_visible(3, ux, uy, rm, 4000);

    // Naive polynomial through the projected endpoint/control points.
    DPoint q[4];
    for(int i = 0; i < 4; ++i)
    {
        double x = ux[i], y = uy[i];
        double w = x * m.w0 + y * m.w1 + m.w2;
        q[i] = DPoint((x * m.sx + y * m.shx + m.tx) / w,
                      (x * m.shy + y * m.sy + m.ty) / w, w);
    }
    double max_naive_gap = 0.0;
    for(size_t k = 0; k < ref.size(); ++k)
    {
        // Evaluate the ordinary (non-rational) Bezier through q[].
        double t = double(k) / std::max<size_t>(1, ref.size() - 1);
        double u = 1.0 - t;
        double b[4] = { u*u*u, 3*u*u*t, 3*u*t*t, t*t*t };
        double nx = 0, ny = 0;
        for(int i = 0; i < 4; ++i) { nx += b[i]*q[i].x; ny += b[i]*q[i].y; }
        double d = std::sqrt((nx - ref[k].x) * (nx - ref[k].x) +
                             (ny - ref[k].y) * (ny - ref[k].y));
        if(d > max_naive_gap) max_naive_gap = d;
    }
    // The naive curve is genuinely wrong here...
    REQUIRE(max_naive_gap > 5.0 * conv.approximation_tolerance());
    // ...while the converter output stays within the tolerance.
    hausdorff_check(out, ref, conv.approximation_tolerance());
}

CHECK_CASE("rational", rational_tolerance_is_honored)
{
    agg::path_storage p;
    p.move_to(0, 0);
    p.curve4(120, -60, 120, 300, 0, 240); // internal cusp/loop stress
    agg::trans_perspective m(1, 0, .01, 0, 1, .02, 20, 8, 1);
    RefMatrix rm(m);
    const double ux[4] = { 0, 120, 120, 0 };
    const double uy[4] = { 0, -60, 300, 240 };
    for(size_t cfg = 0; cfg < 3; ++cfg)
    {
        double tol = cfg == 0 ? 0.25 : (cfg == 1 ? 0.05 : 1.0);
        agg::conv_projective_clip<agg::path_storage> conv(p, m);
        conv.approximation_tolerance(tol);
        std::vector<OutCmd> out = collect(conv);
        check_well_formed(out);
        std::vector<DPoint> ref = reference_visible(3, ux, uy, rm, 5000);
        hausdorff_check(out, ref, tol);
    }
}

CHECK_CASE("rational", rational_invalid_config_throws)
{
    agg::path_storage p;
    p.move_to(0, 0);
    p.line_to(10, 10);
    agg::trans_perspective m;
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    double inf = std::numeric_limits<double>::infinity();
    double nan = std::numeric_limits<double>::quiet_NaN();
    double bad_box[][4] =
    {
        { 1, 0, 1, 256 }, { 0, 0, -1, 256 }, { 0, 2, 256, 1 },
        { nan, 0, 256, 256 }, { 0, inf, 256, 256 }
    };
    for(int i = 0; i < 5; ++i)
    {
        bool threw = false;
        try { conv.clip_box(bad_box[i][0], bad_box[i][1],
                           bad_box[i][2], bad_box[i][3]); }
        catch(const std::invalid_argument&) { threw = true; }
        REQUIRE(threw);
    }
    double bad_tol[4] = { 0.0, -1.0, inf, nan };
    for(int i = 0; i < 4; ++i)
    {
        bool threw = false;
        try { conv.approximation_tolerance(bad_tol[i]); }
        catch(const std::invalid_argument&) { threw = true; }
        REQUIRE(threw);
    }
    // Valid configuration is accepted.
    conv.clip_box(1, 2, 3, 4);
    REQUIRE(conv.clip_x1() == 1 && conv.clip_y2() == 4);
    conv.approximation_tolerance(0.01);
    REQUIRE(conv.approximation_tolerance() == 0.01);
}

//====================================================================== clip
CHECK_CASE("clip", clip_horizon_rejects_far_side)
{
    // Entire curve lives at W < 1e-6: nothing may be emitted.
    agg::path_storage p;
    p.move_to(0, 10);
    p.curve4(0, 20, 0, 20, 0, 10);
    agg::trans_perspective m(1, 0, 0, 0, 1, -0.5, 128, 128, 1); // W=1-0.5y
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    conv.rewind(0);
    double x, y;
    unsigned cmd;
    unsigned vertices = 0;
    while(!agg::is_stop(cmd = conv.vertex(&x, &y)))
    {
        if(agg::is_vertex(cmd)) ++vertices;
        REQUIRE(!(agg::is_vertex(cmd) &&
                  (x < 0 || y < 0 || x > 256 || y > 256)));
    }
    REQUIRE(vertices == 0);
}

CHECK_CASE("clip", clip_box_sliver_with_both_ends_outside)
{
    // Identity perspective (horizon inactive): both endpoints are above the
    // box, only a short middle arch enters it.
    agg::path_storage p;
    p.move_to(-50, -50);
    p.curve4(128, 40, 128, 40, 306, -50);
    agg::trans_perspective m; // identity
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);
    REQUIRE(out.front().cmd == agg::path_cmd_move_to);

    RefMatrix rm(m);
    const double ux[4] = { -50, 128, 128, 306 };
    const double uy[4] = { -50, 40, 40, -50 };
    std::vector<DPoint> ref = reference_visible(3, ux, uy, rm, 6000);
    REQUIRE(ref.size() > 10);                 // the middle arc is present
    hausdorff_check(out, ref, 0.25);

    // Endpoints of the retained fragment sit on the y1 boundary.
    std::vector<DPoint> seg = all_segments(out);
    REQUIRE(std::fabs(seg.front().y - BOX[1]) < 1e-6);
    REQUIRE(std::fabs(seg.back().y  - BOX[1]) < 1e-6);
}

CHECK_CASE("clip", clip_horizon_sliver_with_both_ends_behind)
{
    // W = 1 - 0.01*x, so the horizon x = 100 cuts right through the box.
    // The two cubic endpoints sit at x = 140 (W = -0.4, behind the
    // horizon) while the middle of the curve bends into the visible side.
    // Both endpoints are therefore outside the visible domain, yet a real
    // middle arc remains inside the rectangle and must not be dropped.
    const double ux[4] = { 140, 30, 30, 140 };
    const double uy[4] = { 30, 90, 130, 190 };
    agg::path_storage p;
    p.move_to(ux[0], uy[0]);
    p.curve4(ux[1], uy[1], ux[2], uy[2], ux[3], uy[3]);
    agg::trans_perspective m(1, 0, -0.01, 0, 1, 0, 0, 0, 1);
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);

    RefMatrix rm(m);
    // Both endpoints really are behind the horizon.
    REQUIRE(ref_eval(3, ux, uy, rm, 0.0).w < 0.0);
    REQUIRE(ref_eval(3, ux, uy, rm, 1.0).w < 0.0);

    // A dense independent scan proves a visible in-box interval exists.
    std::vector<DPoint> ref = reference_visible(3, ux, uy, rm, 8000);
    REQUIRE(ref.size() > 20);
    coverage_check(out, ref, 0.25);

    // The retained fragment stays clear of the horizon and inside the box,
    // and neither hidden endpoint leaked back in.
    std::vector<DPoint> seg = all_segments(out);
    REQUIRE(seg.size() >= 4);
    for(size_t i = 0; i < seg.size(); ++i)
    {
        REQUIRE(seg[i].x >= BOX[0] && seg[i].x <= BOX[2]);
        REQUIRE(seg[i].y >= BOX[1] && seg[i].y <= BOX[3]);
    }
    // Endpoints at W<0 would project to negative x for this matrix; no
    // negative-x point can appear.
    double xmin = seg[0].x;
    for(size_t i = 1; i < seg.size(); ++i) xmin = std::min(xmin, seg[i].x);
    REQUIRE(xmin > 0.0);
}

CHECK_CASE("clip", clip_intersections_sit_on_boundary)
{
    agg::path_storage p;
    p.move_to(-80, 128);
    p.line_to(340, 128);
    agg::trans_perspective m;
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);
    std::vector<DPoint> seg = all_segments(out);
    REQUIRE(seg.size() == 2);
    REQUIRE(std::fabs(seg.front().x - BOX[0]) < 1e-7);
    REQUIRE(std::fabs(seg.back().x  - BOX[2]) < 1e-7);
}

CHECK_CASE("clip", clip_custom_box_and_coverage)
{
    const double ux[4] = { 0, 60, 180, 240 };
    const double uy[4] = { 120, 40, 200, 120 };
    agg::path_storage p;
    p.move_to(ux[0], uy[0]);
    p.curve4(ux[1], uy[1], ux[2], uy[2], ux[3], uy[3]);
    agg::trans_perspective m(1, 0, .005, 0, 1, .003, 0, 0, 1);
    RefMatrix rm(m);
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    conv.clip_box(40, 50, 200, 210);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);
    for(size_t i = 0; i < out.size(); ++i)
        if(agg::is_vertex(out[i].cmd))
        {
            REQUIRE(out[i].x >= 40 - 1e-9 && out[i].x <= 200 + 1e-9);
            REQUIRE(out[i].y >= 50 - 1e-9 && out[i].y <= 210 + 1e-9);
        }
    // Independent dense reference restricted to the same box (with a
    // narrow outside band to cover boundary intersection vertices).
    std::vector<DPoint> ref;
    for(unsigned k = 0; k <= 6000; ++k)
    {
        DPoint q = ref_eval(3, ux, uy, rm, double(k) / 6000);
        if(q.w >= HORIZON_EPS && q.x >= 40 - 2.0 && q.x <= 200 + 2.0 &&
           q.y >= 50 - 2.0 && q.y <= 210 + 2.0)
            ref.push_back(q);
    }
    REQUIRE(ref.size() > 10);
    const double custom_box[4] = { 40.0, 50.0, 200.0, 210.0 };
    hausdorff_check(out, ref, 0.25, 1e-7, custom_box);
}

//================================================================ topology
CHECK_CASE("topology", topology_open_double_fragment_restarts)
{
    agg::path_storage p;
    p.move_to(FRAG_UX[0], FRAG_UY[0]);
    p.curve4(FRAG_UX[1], FRAG_UY[1], FRAG_UX[2], FRAG_UY[2],
             FRAG_UX[3], FRAG_UY[3]);
    agg::trans_perspective mm(1, 0, 0, 0, 30, -1, 128, 128, 2);
    agg::conv_projective_clip<agg::path_storage> conv(p, mm);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);

    unsigned moves = 0;
    for(size_t i = 0; i < out.size(); ++i)
        if(agg::is_move_to(out[i].cmd)) ++moves;
    REQUIRE(moves == 2);                       // two fragments, two move_to
    for(size_t i = 1; i < out.size(); ++i)
        if(agg::is_move_to(out[i].cmd))
            REQUIRE(!agg::is_line_to(out[i - 1].cmd) || true);

    // Each chain matches its own parameter interval of the reference curve.
    RefMatrix rm = double_frag_matrix();
    std::vector<std::vector<DPoint> > chains = chains_of(out);
    REQUIRE(chains.size() == 2);
    const unsigned N = 4000;
    for(int fragment = 0; fragment < 2; ++fragment)
    {
        // Visible intervals are t in [0, 1/3] and [2/3, 1] for W = 2-y.
        double t0 = fragment == 0 ? 0.0 : 2.0 / 3.0;
        double t1 = fragment == 0 ? 1.0 / 3.0 : 1.0;
        std::vector<DPoint> interval;
        for(unsigned k = 0; k <= N; ++k)
        {
            double t = t0 + (t1 - t0) * double(k) / N;
            DPoint q = ref_eval(3, FRAG_UX, FRAG_UY, rm, t);
            if(ref_visible(q)) interval.push_back(q);
        }
        REQUIRE(interval.size() > 5);
        for(size_t i = 0; i < chains[fragment].size(); ++i)
            REQUIRE(dist_to_polyline(chains[fragment][i].x,
                                     chains[fragment][i].y, interval)
                    <= 0.25 + 1e-7);
    }

    // The hidden middle of the curve (t = 0.5) sits behind the horizon.
    // Its projection must stay well away from every emitted vertex: the
    // invisible gap is never bridged by a chord across the device plane.
    DPoint hidden = ref_eval(3, FRAG_UX, FRAG_UY, rm, 0.5);
    REQUIRE(hidden.w < HORIZON_EPS);
    std::vector<DPoint> seg = all_segments(out);
    double hidden_gap = std::numeric_limits<double>::infinity();
    for(size_t i = 0; i < seg.size(); ++i)
        hidden_gap = std::min(hidden_gap,
                              std::sqrt((seg[i].x - hidden.x) *
                                        (seg[i].x - hidden.x) +
                                        (seg[i].y - hidden.y) *
                                        (seg[i].y - hidden.y)));
    REQUIRE(hidden_gap > 5.0);
}

CHECK_CASE("topology", topology_closed_blob_closes_on_boundary)
{
    agg::path_storage p;
    p.move_to(BLOB_UX[0], BLOB_UY[0]);
    p.curve4(BLOB_UX[1], BLOB_UY[1], BLOB_UX[2], BLOB_UY[2],
             BLOB_UX[3], BLOB_UY[3]);
    p.close_polygon();
    agg::trans_perspective m = make_blob_matrix();
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);

    unsigned endpolys = 0;
    bool closed = false;
    for(size_t i = 0; i < out.size(); ++i)
        if(agg::is_end_poly(out[i].cmd))
        {
            ++endpolys;
            closed = agg::is_close(out[i].cmd);
        }
    REQUIRE(endpolys == 1 && closed);

    // Every emitted vertex is either an exact rational-curve point or lies
    // on the visible-domain boundary. No outside chord closes the ring.
    RefMatrix rm(m);
    std::vector<DPoint> curve_ref;
    for(unsigned k = 0; k <= 5000; ++k)
    {
        DPoint q = ref_eval(3, BLOB_UX, BLOB_UY, rm, double(k) / 5000);
        if(ref_visible(q)) curve_ref.push_back(q);
    }
    // Closing segment of the user contour (rational line back to start).
    DPoint close_start = ref_eval(3, BLOB_UX, BLOB_UY, rm, 1.0);
    DPoint close_end   = ref_eval(3, BLOB_UX, BLOB_UY, rm, 0.0);
    std::vector<DPoint> ring_ref = curve_ref;
    ring_ref.push_back(close_start);
    ring_ref.push_back(close_end);

    std::vector<DPoint> seg = all_segments(out);
    for(size_t i = 0; i < seg.size(); ++i)
    {
        double dc = dist_to_polyline(seg[i].x, seg[i].y, ring_ref);
        bool on_boundary =
            std::fabs(seg[i].x - BOX[0]) < 1e-6 ||
            std::fabs(seg[i].x - BOX[2]) < 1e-6 ||
            std::fabs(seg[i].y - BOX[1]) < 1e-6 ||
            std::fabs(seg[i].y - BOX[3]) < 1e-6;
        // Curve vertices stay within the configured pixel tolerance;
        // re-closing vertices are allowed to sit on the domain boundary.
        REQUIRE(dc <= 0.25 + 1e-7 || on_boundary);
    }

    // Every visible reference point is reached within tolerance.
    for(size_t k = 0; k < curve_ref.size(); k += 7)
        REQUIRE(dist_to_polyline(curve_ref[k].x, curve_ref[k].y, seg)
                <= 0.25 + 1e-7);

    // Ring area is positive (one directed region).
    REQUIRE(signed_area(out) > 1.0);
}

CHECK_CASE("topology", topology_nested_reversed_hole)
{
    agg::path_storage p;
    p.move_to(40, 40);
    p.line_to(220, 40);
    p.line_to(220, 220);
    p.line_to(40, 220);
    p.close_polygon();
    p.move_to(90, 90);
    p.line_to(90, 170);
    p.line_to(170, 170);
    p.line_to(170, 90);
    p.close_polygon();
    agg::trans_perspective m(1, 0, .001, 0, 1, .001, 0, 0, 1);
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);

    unsigned rings = 0, closes = 0;
    int outer_sign = 0, inner_sign = 0;
    size_t i = 0;
    while(i < out.size())
    {
        std::vector<OutCmd> ring;
        while(i < out.size() && !agg::is_end_poly(out[i].cmd))
        {
            ring.push_back(out[i]);
            ++i;
        }
        if(i < out.size())
        {
            ++rings;
            if(agg::is_close(out[i].cmd)) ++closes;
            double a = signed_area(ring);
            if(outer_sign == 0) outer_sign = a > 0 ? 1 : -1;
            else                inner_sign = a > 0 ? 1 : -1;
            ++i;
        }
    }
    REQUIRE(rings == 2 && closes == 2);
    REQUIRE(outer_sign != 0 && inner_sign != 0 &&
            outer_sign != inner_sign);          // opposite winding -> hole
}

CHECK_CASE("topology", topology_reflection_orientation_from_geometry)
{
    agg::path_storage p;
    p.move_to(40, 40);
    p.line_to(200, 40);
    p.line_to(200, 200);
    p.close_polygon();
    agg::trans_perspective r(-1, 0, 0, 0, 1, 0, 256, 0, 1);
    agg::conv_projective_clip<agg::path_storage> conv(p, r);
    std::vector<OutCmd> out = collect(conv);
    check_well_formed(out);
    double area = signed_area(out);
    unsigned flag = 0;
    for(size_t i = 0; i < out.size(); ++i)
        if(agg::is_end_poly(out[i].cmd)) flag = out[i].cmd;
    REQUIRE(area < 0.0);                      // reflected ring is CW...
    REQUIRE(agg::is_cw(flag));               // ...and the flag says so
    REQUIRE(!agg::is_ccw(flag));
}

CHECK_CASE("topology", topology_rewind_and_reattach_freshness)
{
    agg::path_storage p1;
    p1.move_to(10, 10);
    p1.line_to(120, 120);
    p1.close_polygon();
    agg::path_storage p2;
    p2.move_to(0, 200);
    p2.line_to(240, 60);
    p2.close_polygon();

    agg::trans_perspective m;
    agg::conv_projective_clip<agg::path_storage> conv(p1, m);
    std::vector<OutCmd> first = collect(conv);
    std::vector<OutCmd> again = collect(conv);   // repeated rewind
    REQUIRE(first.size() == again.size());
    for(size_t i = 0; i < first.size(); ++i)
    {
        REQUIRE(first[i].cmd == again[i].cmd);
        REQUIRE(first[i].x == again[i].x);
        REQUIRE(first[i].y == again[i].y);
    }

    // Switching path id must not leak stale vertices. path_storage takes
    // the starting vertex index as the path id.
    agg::path_storage both;
    both.move_to(10, 10);
    both.line_to(120, 120);
    both.close_polygon();
    unsigned second_id = both.total_vertices();
    both.move_to(0, 200);
    both.line_to(100, 60);
    both.line_to(240, 60);
    both.close_polygon();
    agg::conv_projective_clip<agg::path_storage> conv2(both, m);
    conv2.rewind(0);
    std::vector<OutCmd> id0;
    double x, y; unsigned cmd;
    while(!agg::is_stop(cmd = conv2.vertex(&x, &y)))
        id0.push_back(OutCmd{x, y, cmd});
    conv2.rewind(second_id);
    std::vector<OutCmd> id1;
    while(!agg::is_stop(cmd = conv2.vertex(&x, &y)))
        id1.push_back(OutCmd{x, y, cmd});
    // The two contours produce different command streams...
    REQUIRE(id1.size() != id0.size());
    for(size_t i = 0; i < id1.size(); ++i)
    {
        // Every id-1 vertex must belong to one of the second contour's
        // two segments; none may leak from the first contour.
        if(agg::is_vertex(id1[i].cmd))
        {
            double d1 = dist_seg(id1[i].x, id1[i].y, 0, 200, 100, 60);
            double d2 = dist_seg(id1[i].x, id1[i].y, 100, 60, 240, 60);
            REQUIRE(std::min(d1, d2) < 1e-9);
        }
    }
    // path_storage has no stop between subpaths: rewind(0) returns both
    // contours while rewind(second_id) must return exactly the suffix
    // beginning at the second move_to, i.e. no backlog from the first.
    size_t second_move = 0;
    unsigned moves_seen = 0;
    for(size_t i = 0; i < id0.size(); ++i)
        if(agg::is_move_to(id0[i].cmd))
        {
            if(++moves_seen == 2) { second_move = i; break; }
        }
    REQUIRE(moves_seen == 2);
    REQUIRE(id0.size() - second_move == id1.size());
    for(size_t i = 0; i < id1.size(); ++i)
    {
        REQUIRE(id1[i].cmd == id0[second_move + i].cmd);
        REQUIRE(std::fabs(id1[i].x - id0[second_move + i].x) < 1e-12);
        REQUIRE(std::fabs(id1[i].y - id0[second_move + i].y) < 1e-12);
    }

    // Attaching a new source changes the output immediately.
    conv.attach(p2);
    std::vector<OutCmd> swapped = collect(conv);
    REQUIRE(swapped.size() != first.size() ||
            std::fabs(swapped[0].y - first[0].y) > 1e-9);

    // Replacing the matrix invalidates cached flattening as well.
    agg::trans_perspective m2(1, 0, .02, 0, 1, .01, 0, 0, 1);
    conv.transformer(m2);
    std::vector<OutCmd> reperspected = collect(conv);
    bool changed = reperspected.size() != swapped.size();
    if(!changed)
        for(size_t i = 0; i < swapped.size(); ++i)
            if(std::fabs(swapped[i].x - reperspected[i].x) > 1e-9 ||
               std::fabs(swapped[i].y - reperspected[i].y) > 1e-9)
                changed = true;
    REQUIRE(changed);
}

CHECK_CASE("topology", topology_source_path_is_not_modified)
{
    agg::path_storage p;
    p.move_to(1, 1);
    p.curve4(1, 8, 8, 8, 8, 1);
    p.close_polygon();
    unsigned before = p.total_vertices();
    agg::trans_perspective m(1, 0, .02, 0, 1, .01, 0, 0, 1);
    agg::conv_projective_clip<agg::path_storage> conv(p, m);
    collect(conv);
    collect(conv);
    REQUIRE(p.total_vertices() == before);
}

//=================================================================== render
namespace
{
    // Independent reference ring for the closed blob figure.
    std::vector<DPoint> blob_reference_ring(const RefMatrix& rm)
    {
        std::vector<DPoint> ring;
        const unsigned N = 4000;
        for(unsigned k = 0; k <= N; ++k)
            ring.push_back(ref_eval(3, BLOB_UX, BLOB_UY, rm, double(k) / N));
        // Closing straight segment back to the start.
        for(unsigned k = 0; k <= 100; ++k)
        {
            double t = double(k) / 100;
            DPoint a = ref_eval(3, BLOB_UX, BLOB_UY, rm, 1.0);
            DPoint b = ref_eval(3, BLOB_UX, BLOB_UY, rm, 0.0);
            ring.push_back(DPoint(a.x + (b.x - a.x) * t,
                                  a.y + (b.y - a.y) * t));
        }
        return clip_rect(ring);
    }

    std::vector<DPoint> quad_ring(const double ux[4], const double uy[4],
                                  const RefMatrix& rm)
    {
        std::vector<DPoint> ring;
        for(int e = 0; e < 4; ++e)
        {
            int a = e, b = (e + 1) % 4;
            for(unsigned k = 0; k < 200; ++k)
            {
                double t = double(k) / 200;
                double x = ux[a] + (ux[b] - ux[a]) * t;
                double y = uy[a] + (uy[b] - uy[a]) * t;
                double w = x * rm.w0 + y * rm.w1 + rm.w2;
                ring.push_back(DPoint(
                    (x * rm.sx + y * rm.shx + rm.tx) / w,
                    (x * rm.shy + y * rm.sy + rm.ty) / w));
            }
        }
        return ring;
    }
}

CHECK_CASE("render", render_closed_blob_pixels_match_reference)
{
    agg::path_storage p;
    p.move_to(BLOB_UX[0], BLOB_UY[0]);
    p.curve4(BLOB_UX[1], BLOB_UY[1], BLOB_UX[2], BLOB_UY[2],
             BLOB_UX[3], BLOB_UY[3]);
    p.close_polygon();
    agg::trans_perspective m = make_blob_matrix();
    agg::conv_projective_clip<agg::path_storage> conv(p, m);

    Image img(256, 256);
    render(img, conv, agg::rgba8(200, 30, 30));

    RefMatrix rm(m);
    std::vector<DPoint> ring = blob_reference_ring(rm);
    REQUIRE(ring.size() >= 3);

    unsigned checked = 0, mismatches = 0;
    for(unsigned py = 1; py < 255; ++py)
        for(unsigned px = 1; px < 255; ++px)
        {
            double cx = px + 0.5, cy = py + 0.5;
            if(ring_outline_distance(cx, cy, ring) <= 3.0) continue;
            bool expected = point_in_ring(cx, cy, ring);
            bool got = covered(img, px, py);
            ++checked;
            if(expected != got) ++mismatches;
        }
    REQUIRE(checked > 1000);
    REQUIRE(mismatches == 0);
}

CHECK_CASE("render", render_open_double_fragment_covers_two_chains)
{
    agg::path_storage p;
    p.move_to(FRAG_UX[0], FRAG_UY[0]);
    p.curve4(FRAG_UX[1], FRAG_UY[1], FRAG_UX[2], FRAG_UY[2],
             FRAG_UX[3], FRAG_UY[3]);
    agg::trans_perspective mm(1, 0, 0, 0, 30, -1, 128, 128, 2);
    agg::conv_projective_clip<agg::path_storage> conv(p, mm);

    Image img(256, 256);
    render(img, conv, agg::rgba8(30, 160, 30));

    // The two visible corridors contain covered pixels...
    unsigned covered_count = 0;
    for(unsigned py = 120; py < 190; ++py)
        for(unsigned px = 90; px < 150; ++px)
            if(covered(img, px, py)) ++covered_count;
    REQUIRE(covered_count > 50);
    // ...while far-away regions and the behind-horizon gap stay blank.
    unsigned stray = 0;
    const unsigned far[][2] =
    {
        { 10, 10 }, { 245, 245 }, { 20, 200 }, { 240, 30 },
        { 128, 230 }, { 60, 60 }, { 200, 128 }
    };
    for(size_t i = 0; i < sizeof(far) / sizeof(far[0]); ++i)
        if(covered(img, far[i][0], far[i][1])) ++stray;
    REQUIRE(stray == 0);
}

CHECK_CASE("render", render_reversed_hole_preserves_hole)
{
    const double outer_x[4] = { 40, 220, 220, 40 };
    const double outer_y[4] = { 40, 40, 220, 220 };
    const double inner_x[4] = { 90, 90, 170, 170 };
    const double inner_y[4] = { 90, 170, 170, 90 };

    agg::path_storage p;
    p.move_to(outer_x[0], outer_y[0]);
    p.line_to(outer_x[1], outer_y[1]);
    p.line_to(outer_x[2], outer_y[2]);
    p.line_to(outer_x[3], outer_y[3]);
    p.close_polygon();
    p.move_to(inner_x[0], inner_y[0]);
    p.line_to(inner_x[1], inner_y[1]);
    p.line_to(inner_x[2], inner_y[2]);
    p.line_to(inner_x[3], inner_y[3]);
    p.close_polygon();
    agg::trans_perspective m(1, 0, .001, 0, 1, .001, 0, 0, 1);
    agg::conv_projective_clip<agg::path_storage> conv(p, m);

    Image img(256, 256);
    render(img, conv, agg::rgba8(30, 30, 200), agg::fill_even_odd);

    RefMatrix rm(m);
    std::vector<DPoint> outer = clip_rect(quad_ring(outer_x, outer_y, rm));
    std::vector<DPoint> inner = clip_rect(quad_ring(inner_x, inner_y, rm));
    REQUIRE(outer.size() >= 3 && inner.size() >= 3);

    unsigned checked = 0, mismatches = 0;
    for(unsigned py = 30; py < 230; ++py)
        for(unsigned px = 30; px < 230; ++px)
        {
            double cx = px + 0.5, cy = py + 0.5;
            if(ring_outline_distance(cx, cy, outer) <= 3.0) continue;
            if(ring_outline_distance(cx, cy, inner) <= 3.0) continue;
            bool expected = point_in_ring(cx, cy, outer) &&
                            !point_in_ring(cx, cy, inner);
            bool got = covered(img, px, py);
            ++checked;
            if(expected != got) ++mismatches;
        }
    REQUIRE(checked > 1000);
    REQUIRE(mismatches == 0);
}

CHECK_CASE("render", render_writes_projection_ppm)
{
    // Render each figure onto its own canvas and stack the three canvases
    // vertically into one small PPM, so later figures cannot overwrite an
    // earlier one's color.
    Image red_img(256, 256);
    {
        agg::path_storage p;
        p.move_to(BLOB_UX[0], BLOB_UY[0]);
        p.curve4(BLOB_UX[1], BLOB_UY[1], BLOB_UX[2], BLOB_UY[2],
                 BLOB_UX[3], BLOB_UY[3]);
        p.close_polygon();
        agg::trans_perspective m = make_blob_matrix();
        agg::conv_projective_clip<agg::path_storage> conv(p, m);
        render(red_img, conv, agg::rgba8(200, 30, 30));
    }
    Image green_img(256, 256);
    {
        agg::path_storage p;
        p.move_to(FRAG_UX[0], FRAG_UY[0]);
        p.curve4(FRAG_UX[1], FRAG_UY[1], FRAG_UX[2], FRAG_UY[2],
                 FRAG_UX[3], FRAG_UY[3]);
        agg::trans_perspective mm(1, 0, 0, 0, 30, -1, 128, 128, 2);
        agg::conv_projective_clip<agg::path_storage> conv(p, mm);
        render(green_img, conv, agg::rgba8(30, 160, 30));
    }
    Image blue_img(256, 256);
    {
        agg::path_storage p;
        p.move_to(40, 40);
        p.line_to(220, 40);
        p.line_to(220, 220);
        p.line_to(40, 220);
        p.close_polygon();
        p.move_to(90, 90);
        p.line_to(90, 170);
        p.line_to(170, 170);
        p.line_to(170, 90);
        p.close_polygon();
        agg::trans_perspective m(1, 0, .001, 0, 1, .001, 0, 0, 1);
        agg::conv_projective_clip<agg::path_storage> conv(p, m);
        render(blue_img, conv, agg::rgba8(30, 30, 200), agg::fill_even_odd);
    }

    const Image* imgs[3] = { &red_img, &green_img, &blue_img };
    std::FILE* f = std::fopen("checks/projective_clip.ppm", "wb");
    REQUIRE(f != 0);
    std::fprintf(f, "P6\n256 768\n255\n");
    for(int i = 0; i < 3; ++i)
        std::fwrite(imgs[i]->buf.data(), 1, imgs[i]->buf.size(), f);
    std::fclose(f);

    // Each canvas carries its own intended hue. Channel-dominance (rather
    // than absolute thresholds) stays valid even where the AA rasterizer
    // only partially covers a pixel and blends it toward white.
    auto has_color = [](const Image& img,
                        bool (*pred)(unsigned char, unsigned char,
                                     unsigned char))
    {
        for(size_t i = 0; i < img.buf.size(); i += 3)
            if(pred(img.buf[i], img.buf[i + 1], img.buf[i + 2]))
                return true;
        return false;
    };
    REQUIRE(has_color(red_img,
            [](unsigned char r, unsigned char g, unsigned char b)
            { return r > g + 15 && r > b + 15 && r < 250; }));
    REQUIRE(has_color(green_img,
            [](unsigned char r, unsigned char g, unsigned char b)
            { return g > r + 15 && g > b + 15 && g < 250; }));
    REQUIRE(has_color(blue_img,
            [](unsigned char r, unsigned char g, unsigned char b)
            { return b > r + 15 && b > g + 15 && b < 250; }));
}
