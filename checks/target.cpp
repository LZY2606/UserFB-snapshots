// target.cpp - geometry-based regression suite for libspiro.
//
// Four groups are registered through checks/harness.h:
//   model         - known spiro_seg pieces vs an independent integral reference
//   joins         - end-to-end G2/G1/corner/anchor connection conditions
//   normalization - translation/rotation/scale and coordinate offset channels
//   callbacks     - public status entries, custom vs internal bezctx, knot events
//
// The oracle lives in checks/geo.{h,cpp} and is derived only from the curvature
// model theta(u) = k0 u + k1 u^2/2 + k2 u^3/6 + k3 u^4/24. No production
// integration or subdivision routine is reused as a reference.
#include "geo.h"
#include "harness.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace geo;

namespace {

// ---- small assertion / formatting helpers ----
[[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m); }

void check_le(double got, double tol, const std::string& what) {
    if (!(got <= tol))
        fail(what + " got=" + std::to_string(got) + " tol=" + std::to_string(tol));
}

void check_eq(int got, int want, const std::string& what) {
    if (got != want)
        fail(what + " got=" + std::to_string(got) + " want=" + std::to_string(want));
}

double angle_diff(double a, double b) { return std::fabs(angle_wrap(a - b)); }

// ---- unified path extraction (custom records and internal rows alike) ----
struct Seg {
    bool line;
    Point p0, p1, p2, p3;	// line uses p0,p1
};
struct PathInfo {
    std::vector<Seg> seg;
    std::vector<Point> knot_pos;
    std::vector<int> knot_idx;
    std::vector<int> knot_after_seg;
    std::vector<Record> draw;
    int ncub = 0, nline = 0;
    bool is_open = false;
};

PathInfo extract(const std::vector<Record>& rec) {
    PathInfo P;
    Point cur{0, 0};
    bool started = false;
    for (const Record& e : rec) {
        switch (e.kind) {
            case Record::MOVE:
                cur = {e.v[0], e.v[1]};
                started = true;
                P.draw.push_back(e);
                break;
            case Record::KNOT:
                if (!started) fail("knot event before moveto");
                P.knot_pos.push_back(cur);
                P.knot_idx.push_back(e.knot);
                P.knot_after_seg.push_back(P.seg.empty() ? -1
                                                          : (int)P.seg.size() - 1);
                break;
            case Record::LINE: {
                Point end{e.v[0], e.v[1]};
                P.seg.push_back({true, cur, end, {}, {}});
                P.draw.push_back(e);
                cur = end; ++P.nline;
                break;
            }
            case Record::CUBIC: {
                Point p1{e.v[0], e.v[1]}, p2{e.v[2], e.v[3]}, p3{e.v[4], e.v[5]};
                P.seg.push_back({false, cur, p1, p2, p3});
                P.draw.push_back(e);
                cur = p3; ++P.ncub;
                break;
            }
            case Record::QUAD:
                fail("quadratic output is not certified by this suite");
        }
    }
    return P;
}

Point seg_start_tangent(const Seg& s) {
    if (s.line) return {s.p1.x - s.p0.x, s.p1.y - s.p0.y};
    return {s.p1.x - s.p0.x, s.p1.y - s.p0.y};
}
Point seg_end_tangent(const Seg& s) {
    if (s.line) return {s.p1.x - s.p0.x, s.p1.y - s.p0.y};
    return {s.p3.x - s.p2.x, s.p3.y - s.p2.y};
}

// ---- pipeline wrappers ----
std::vector<Record> run_custom(const spiro_cp* src, int n) {
    double dm[6] = {0, 0, 0, 0, 0, 0};
    spiro_seg* s = run_spiro0(src, dm, SPIRO_CUBIC_TO_BEZIER, n);
    if (!s) fail("run_spiro0 returned null on material expected to converge");
    RecordCtx ctx;
    int ok = spiro_to_bpath0(src, s, dm, SPIRO_CUBIC_TO_BEZIER, n, &ctx.base);
    check_eq(ok, 1, "spiro_to_bpath0 status");
    free_spiro(s);
    return ctx.rec;
}

std::vector<Record> run_internal(spiro_cp* src, int ncq, int& status) {
    ls_bezctx* b = new_ls_bezctx(512, ncq);
    if (!b) fail("new_ls_bezctx returned null");
    status = TaggedSpiroCPsToBezier2(src, ncq, &b->base);
    std::vector<Record> rec;
    for (int i = 0; i < b->l; ++i) {
        const curve_data& d = b->cd[i];
        if (d.ty == 'm') rec.push_back({Record::MOVE, {d.x1, d.y1}, -1});
        else if (d.ty == 'l') rec.push_back({Record::LINE, {d.x1, d.y1}, -1});
        else if (d.ty == 'c')
            rec.push_back({Record::CUBIC, {d.x1, d.y1, d.x2, d.y2, d.x3, d.y3}, -1});
        else if (d.ty == 'k') rec.push_back({Record::KNOT, {}, -1});
        else if (d.ty == 'q') fail("unexpected quadratic in certified cubic run");
        else fail("unknown internal curve_data type");
    }
    free_ls_bezctx(b);
    return rec;
}

// Emit checks/target_metrics.tsv and checks/REPORT.md once every group passed.
// Invoked explicitly by checks/driver.cpp via the weak after_all_checks hook,
// so there is no cross-translation-unit static destruction ordering problem.
extern "C" void after_all_checks(void) { write_reports(); }

// =====================================================================
// model: known segments against the independent curvature-model reference
// =====================================================================
struct Material {
    const char* name;
    double ks[4];
    double chord;
    int min_cubics;		// proves recursive subdivision when >= 2
};

void run_material(const Material& m) {
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(m.ks[i])) fail("non-finite curvature coefficient");
        if (std::fabs(m.ks[i]) > 4.0) fail("curvature coefficient outside envelope");
    }
    if (!(m.chord >= 1.0 && m.chord <= 200.0)) fail("chord outside stable envelope");
    Point p0{0, 0}, p1{m.chord, 0};
    if (distance(p0, p1) == 0.0) fail("coincident endpoints cannot be geometry evidence");

    ReferenceCurve ref = build_reference(m.ks, p0, p1);
    check_le(ref.integral_err, 1.0e-10, std::string(m.name) + " integration error");
    // The oracle must itself land on the requested endpoints.
    check_le(distance(ref.p.front(), p0), END_ABS, m.name + std::string(" ref start"));
    check_le(distance(ref.p.back(), p1), END_ABS, m.name + std::string(" ref end"));

    std::vector<Record> rec = convert_known_segment(m.ks, m.chord);
    int ncub = count_cubics(rec);
    if (ncub < m.min_cubics)
        fail(std::string(m.name) + " expected recursive subdivision into >= "
             + std::to_string(m.min_cubics) + " cubics, got " + std::to_string(ncub));

    std::vector<Point> bez = sample_records(rec);
    double d = symmetric_distance(ref.p, bez);
    double rel = d / m.chord;
    check_le(rel, DIST_REL, std::string(m.name) + " bidirectional distance");
    record_metric("model", m.name, "hausdorff/chord", rel, DIST_REL);

    // Tight, independent endpoint and end-tangent assertions.
    check_le(distance(bez.front(), p0), END_ABS, m.name + std::string(" end p0"));
    check_le(distance(bez.back(), p1), END_ABS, m.name + std::string(" end p1"));
    double rs, re;
    reference_end_angles(m.ks, p0, p1, rs, re);
    PathInfo info = extract(rec);
    const Seg& first = info.seg.front();
    const Seg& last = info.seg.back();
    check_le(angle_diff(angle_of(seg_start_tangent(first)), rs), TAN_ABS,
             m.name + std::string(" start tangent"));
    check_le(angle_diff(angle_of(seg_end_tangent(last)), re), TAN_ABS,
             m.name + std::string(" end tangent"));
}

CHECK_CASE("model", model_straight_line) {
    Material m{"straight_line", {0, 0, 0, 0}, 120, 0};
    run_material(m);
}
CHECK_CASE("model", model_constant_arc_forward) {
    Material m{"constant_arc_forward", {3.2, 0, 0, 0}, 150, 4};
    run_material(m);
    write_trajectory_svg("checks/trajectory.svg", m.ks, m.chord);
}
CHECK_CASE("model", model_constant_arc_reverse) {
    Material m{"constant_arc_reverse", {-3.2, 0, 0, 0}, 150, 4};
    run_material(m);
}
CHECK_CASE("model", model_linear_kappa_forward) {
    Material m{"linear_kappa_forward", {0.3, 2.4, 0, 0}, 100, 2};
    run_material(m);
}
CHECK_CASE("model", model_linear_kappa_reverse) {
    Material m{"linear_kappa_reverse", {-0.3, -2.4, 0, 0}, 100, 2};
    run_material(m);
}
CHECK_CASE("model", model_higher_order_forward) {
    Material m{"higher_order_forward", {0.4, 0.6, 2.4, -2.4}, 60, 2};
    run_material(m);
}
CHECK_CASE("model", model_higher_order_reverse) {
    Material m{"higher_order_reverse", {-0.4, -0.6, -2.4, 2.4}, 60, 2};
    run_material(m);
}
CHECK_CASE("model", model_mixed_full_order) {
    Material m{"mixed_full_order", {2.2, 2.6, -2.6, 2.2}, 190, 4};
    run_material(m);
}
CHECK_CASE("model", model_chord_envelope_extremes) {
    Material lo{"short_chord", {1.2, 1.2, 1.2, 1.2}, 1.0, 2};
    Material hi{"long_chord", {2.4, 0, 0, 0}, 200.0, 4};
    run_material(lo);
    run_material(hi);
}

// =====================================================================
// joins: connection conditions on real solved end-to-end paths
// =====================================================================
void require_smooth_join(const PathInfo& P, int left, int right,
                         const std::string& tag) {
    if (left < 0 || right >= (int)P.seg.size()) fail(tag + " missing neighbour segment");
    const Seg& a = P.seg[left];
    const Seg& b = P.seg[right];
    if (a.line || b.line) fail(tag + " expected cubic join");
    check_le(distance(a.p3, b.p0), END_ABS, tag + " position");
    check_le(angle_diff(angle_of(seg_end_tangent(a)),
                        angle_of(seg_start_tangent(b))), TAN_ABS, tag + " tangent");
    double ka = cubic_curvature(a.p0, a.p1, a.p2, a.p3, 1.0);
    double kb = cubic_curvature(b.p0, b.p1, b.p2, b.p3, 0.0);
    double scale = std::max(1.0, std::max(ka, kb));
    check_le(std::fabs(ka - kb) / scale, KAPPA_REL, tag + " physical curvature");
}

CHECK_CASE("joins", joins_open_g2) {
    spiro_cp src[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'},
                      {140, 20, 'c'}, {180, 40, '}'}};
    const int n = 5;
    PathInfo P = extract(run_custom(src, n));
    // Two interior G2 knots (indices 1 and 2).
    for (int j = 1; j <= 2; ++j)
        require_smooth_join(P, P.knot_after_seg[j], P.knot_after_seg[j] + 1,
                            "open g2 join " + std::to_string(j));
    double dm[6] = {0};
    spiro_seg* s = run_spiro0(src, dm, 0, n);
    int nseg = n - 1;
    const Seg& first = P.seg.front();
    const Seg& last = P.seg.back();
    double e0 = angle_diff(angle_of(seg_start_tangent(first)), get_knot_th(s, 0));
    double eN = angle_diff(angle_of(seg_end_tangent(last)), get_knot_th(s, nseg));
    check_le(e0, TAN_ABS, "open start tangent vs solver");
    check_le(eN, TAN_ABS, "open end tangent vs solver");
    record_metric("joins", "open_g2", "end_tangent_vs_solver_rad",
                  std::max(e0, eN), TAN_ABS);
    free_spiro(s);
    check_le(distance(P.knot_pos.front(), {src[0].x, src[0].y}), END_ABS,
             "open first knot position");
    // Open paths mark knots 0..n-2; the final point is a drawn endpoint, not a
    // knot (unless SPIRO_INCLUDE_LAST_KNOT is requested, tested separately).
    check_le(distance(P.knot_pos.back(), {src[n - 2].x, src[n - 2].y}), END_ABS,
             "open last marked knot position");
    Point drawn_end = P.seg.back().line ? P.seg.back().p1 : P.seg.back().p3;
    check_le(distance(drawn_end, {src[n - 1].x, src[n - 1].y}), END_ABS,
             "open final drawn endpoint");
}

CHECK_CASE("joins", joins_closed_g2_seam) {
    spiro_cp src[] = {{0, 0, 'c'}, {60, 10, 'c'}, {80, 70, 'c'},
                      {20, 90, 'c'}, {0, 0, 'z'}};
    const int n = 5;
    PathInfo P = extract(run_custom(src, n));
    // Interior G2 knots; the closing seam (last -> first) is checked below.
    for (size_t j = 1; j + 1 < P.knot_pos.size(); ++j)
        require_smooth_join(P, P.knot_after_seg[j], P.knot_after_seg[j] + 1,
                            "closed g2 join " + std::to_string(j));
    const Seg& seam_l = P.seg.back();
    const Seg& seam_r = P.seg.front();
    check_le(distance(seam_l.p3, seam_r.p0), END_ABS, "closed seam position");
    check_le(angle_diff(angle_of(seg_end_tangent(seam_l)),
                        angle_of(seg_start_tangent(seam_r))), TAN_ABS,
             "closed seam tangent");
    double ka = cubic_curvature(seam_l.p0, seam_l.p1, seam_l.p2, seam_l.p3, 1.0);
    double kb = cubic_curvature(seam_r.p0, seam_r.p1, seam_r.p2, seam_r.p3, 0.0);
    double seam_dk = std::fabs(ka - kb) / std::max(1.0, std::max(ka, kb));
    check_le(seam_dk, KAPPA_REL, "closed seam curvature");
    record_metric("joins", "closed_g2_seam", "relative_curvature_jump",
                  seam_dk, KAPPA_REL);
    check_le(distance(seam_l.p3, {src[0].x, src[0].y}), END_ABS,
             "closed seam returns to first knot");
}

CHECK_CASE("joins", joins_corners_remain_discontinuous) {
    spiro_cp src[] = {{0, 0, '{'}, {50, 0, 'v'}, {50, 50, 'v'}, {0, 50, '}'}};
    PathInfo P = extract(run_custom(src, 4));
    // Corners are genuine tangent discontinuities: never force them smooth.
    for (size_t j = 1; j + 1 < P.knot_pos.size(); ++j) {
        const Seg& a = P.seg[P.knot_after_seg[j]];
        const Seg& b = P.seg[P.knot_after_seg[j] + 1];
        double jump = angle_diff(angle_of(seg_end_tangent(a)),
                                 angle_of(seg_start_tangent(b)));
        if (!(jump > 0.5)) fail("corner " + std::to_string(j) +
                                " wrongly smoothed, tangent jump=" + std::to_string(jump));
    }
}

CHECK_CASE("joins", joins_anchor_handle_direction) {
    spiro_cp src[] = {{0, 0, '{'}, {60, 20, 'c'}, {120, 0, 'a'},
                      {140, -30, 'h'}, {180, 20, 'c'}, {220, 0, '}'}};
    // Direction is defined solely by the input anchor/handle pair.
    double handle_th = angle_of({140.0 - 120.0, -30.0 - 0.0});
    PathInfo P = extract(run_custom(src, 6));
    int knot = -1;
    for (size_t j = 0; j < P.knot_pos.size(); ++j)
        if (distance(P.knot_pos[j], {120, 0}) <= END_ABS) knot = (int)j;
    if (knot < 0) fail("anchor knot not emitted at input anchor location");
    const Seg& in = P.seg[P.knot_after_seg[knot]];
    const Seg& out = P.seg[P.knot_after_seg[knot] + 1];
    double ein = angle_diff(angle_of(seg_end_tangent(in)), handle_th);
    double eout = angle_diff(angle_of(seg_start_tangent(out)), handle_th);
    check_le(ein, TAN_ABS, "anchor incoming tangent follows handle");
    check_le(eout, TAN_ABS, "anchor outgoing tangent follows handle");
    record_metric("joins", "anchor_handle", "tangent_vs_input_direction_rad",
                  std::max(ein, eout), TAN_ABS);
}

// =====================================================================
// normalization: rigid/uniform transforms and independent offset channels
// =====================================================================
spiro_cp transform_point(const spiro_cp& p, double m00, double m01, double m10,
                         double m11, double tx, double ty) {
    return {m00 * p.x + m01 * p.y + tx, m10 * p.x + m11 * p.y + ty, p.ty};
}

CHECK_CASE("normalization", normalization_translation_distinct_xy) {
    const double dx = 7.0, dy = 11.0;	// deliberately different channels
    spiro_cp base[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'},
                       {140, 20, 'c'}, {180, 40, '}'}};
    spiro_cp moved[5];
    for (int i = 0; i < 5; ++i)
        moved[i] = transform_point(base[i], 1, 0, 0, 1, dx, dy);
    std::vector<Record> r0 = run_custom(base, 5);
    std::vector<Record> r1 = run_custom(moved, 5);
    check_eq((int)r0.size(), (int)r1.size(), "translated record count");
    double worst_x = 0, worst_y = 0;
    for (size_t i = 0; i < r0.size(); ++i) {
        if (r0[i].kind != r1[i].kind) fail("translated record kind mismatch");
        if (r0[i].kind == Record::KNOT) continue;	// tag row, not geometry
        int nv = r0[i].kind == Record::CUBIC ? 6 : 2;
        for (int k = 0; k < nv; k += 2) {
            worst_x = std::max(worst_x, std::fabs(r1[i].v[k] - (r0[i].v[k] + dx)));
            worst_y = std::max(worst_y, std::fabs(r1[i].v[k + 1]
                                                  - (r0[i].v[k + 1] + dy)));
        }
    }
    check_le(worst_x, COORD_ABS, "x translation channel");
    check_le(worst_y, COORD_ABS, "y translation channel (no x/y borrowing)");
    record_metric("normalization", "translation", "x_offset_error", worst_x, COORD_ABS);
    record_metric("normalization", "translation", "y_offset_error", worst_y, COORD_ABS);

    auto a = sample_records(r0), b = sample_records(r1);
    std::vector<Point> inv;
    for (Point q : b) inv.push_back({q.x - dx, q.y - dy});
    double rel = symmetric_distance(a, inv) / bbox_diagonal(a);
    check_le(rel, XFORM_REL, "translated trajectory recovery");
}

CHECK_CASE("normalization", normalization_rotation) {
    double a = 31.0 * PI / 180.0, c = std::cos(a), s = std::sin(a);
    spiro_cp base[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'},
                       {140, 20, 'c'}, {180, 40, '}'}};
    spiro_cp rot[5];
    for (int i = 0; i < 5; ++i)
        rot[i] = transform_point(base[i], c, -s, s, c, 0, 0);
    auto p0 = sample_records(run_custom(base, 5));
    auto pr = sample_records(run_custom(rot, 5));
    std::vector<Point> inv;
    for (Point q : pr) inv.push_back({c * q.x + s * q.y, -s * q.x + c * q.y});
    double rel = symmetric_distance(p0, inv) / bbox_diagonal(p0);
    check_le(rel, XFORM_REL, "rotated trajectory recovery");
    record_metric("normalization", "rotation", "inverse_hausdorff/bbox", rel, XFORM_REL);
}

CHECK_CASE("normalization", normalization_uniform_scale) {
    const double k = 1.7;
    spiro_cp base[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'},
                       {140, 20, 'c'}, {180, 40, '}'}};
    spiro_cp scaled[5];
    for (int i = 0; i < 5; ++i)
        scaled[i] = transform_point(base[i], k, 0, 0, k, 0, 0);
    auto p0 = sample_records(run_custom(base, 5));
    auto ps = sample_records(run_custom(scaled, 5));
    std::vector<Point> inv;
    for (Point q : ps) inv.push_back({q.x / k, q.y / k});
    double rel = symmetric_distance(p0, inv) / bbox_diagonal(p0);
    check_le(rel, XFORM_REL, "scaled trajectory recovery");
    record_metric("normalization", "scale", "inverse_hausdorff/bbox", rel, XFORM_REL);
}

CHECK_CASE("normalization", normalization_dm_offset_channels) {
    // Same known segment, identity scale but two different translation offsets
    // fed straight through the dm normalization used by the production converter.
    const double ks[4] = {1.6, 0, 0, 0};
    const double chord = 80;
    spiro_seg s[2];
    std::memset(s, 0, sizeof(s));
    s[0].ty = '{';
    for (int i = 0; i < 4; ++i) s[0].ks[i] = ks[i];
    s[0].seg_ch = chord; s[1].ty = '}'; s[1].x = chord;
    RecordCtx z, t;
    double dmz[6] = {1, 0, 0, 0, 0, 0};
    double dmt[6] = {1, 7, 11, 0, 0, 0};
    check_eq(spiro_to_bpath0(nullptr, s, dmz, 0, 2, &z.base), 1, "dm zero convert");
    check_eq(spiro_to_bpath0(nullptr, s, dmt, 0, 2, &t.base), 1, "dm offset convert");
    check_eq((int)z.rec.size(), (int)t.rec.size(), "dm record count");
    double wx = 0, wy = 0;
    for (size_t i = 0; i < z.rec.size(); ++i) {
        if (z.rec[i].kind == Record::KNOT) continue;
        int nv = z.rec[i].kind == Record::CUBIC ? 6 : 2;
        for (int k2 = 0; k2 < nv; k2 += 2) {
            wx = std::max(wx, std::fabs(t.rec[i].v[k2] - (z.rec[i].v[k2] + 7)));
            wy = std::max(wy, std::fabs(t.rec[i].v[k2 + 1]
                                        - (z.rec[i].v[k2 + 1] + 11)));
        }
    }
    check_le(wx, COORD_ABS, "dm x offset channel");
    check_le(wy, COORD_ABS, "dm y offset channel");
}

// =====================================================================
// callbacks: public status entries, bezctx variants, knot ownership
// =====================================================================
CHECK_CASE("callbacks", callbacks_public_status_entries) {
    spiro_cp open[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'}, {180, 40, '}'}};
    spiro_cp closed[] = {{0, 0, 'c'}, {60, 10, 'c'}, {20, 90, 'c'}, {0, 0, 'z'}};
    RecordCtx a, b;
    check_eq(SpiroCPsToBezier2(open, 4, SPIRO_CUBIC_TO_BEZIER, 0, &a.base), 1,
             "explicit open status");
    check_eq(SpiroCPsToBezier2(closed, 4, SPIRO_CUBIC_TO_BEZIER, 1, &b.base), 1,
             "explicit closed status");
    check_eq(TaggedSpiroCPsToBezier2(open, SPIRO_CUBIC_TO_BEZIER, &a.base), 1,
             "tagged status");
    if (a.rec.empty() || b.rec.empty()) fail("status success produced no records");
}

CHECK_CASE("callbacks", callbacks_tagged_matches_explicit) {
    spiro_cp explicit_pts[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'},
                               {140, 20, 'c'}, {180, 40, '}'}};
    spiro_cp tagged_pts[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'},
                             {140, 20, 'c'}, {180, 40, '}'}};
    RecordCtx a, b;
    check_eq(SpiroCPsToBezier2(explicit_pts, 5, 0, 0, &a.base), 1, "explicit entry");
    check_eq(TaggedSpiroCPsToBezier2(tagged_pts, 0, &b.base), 1, "tagged entry");
    check_eq((int)a.rec.size(), (int)b.rec.size(), "tagged/explicit record count");
    double worst = 0;
    for (size_t i = 0; i < a.rec.size(); ++i) {
        if (a.rec[i].kind != b.rec[i].kind) fail("tagged/explicit kind mismatch");
        if (a.rec[i].kind == Record::KNOT) {
            if (a.rec[i].knot != b.rec[i].knot) fail("tagged/explicit knot index");
            continue;
        }
        int nv = a.rec[i].kind == Record::CUBIC ? 6 : 2;
        for (int k = 0; k < nv; ++k)
            worst = std::max(worst, std::fabs(a.rec[i].v[k] - b.rec[i].v[k]));
    }
    check_le(worst, COORD_ABS, "tagged vs explicit coordinates");
    record_metric("callbacks", "tagged_explicit", "max_coordinate_diff",
                  worst, COORD_ABS);
}

CHECK_CASE("callbacks", callbacks_internal_matches_custom) {
    spiro_cp src[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'},
                      {140, 20, 'c'}, {180, 40, '}'}};
    PathInfo custom = extract(run_custom(src, 5));
    int status = 0;
    int ncq = SPIRO_INTERNAL_BEZCTX;
    PathInfo internal = extract(run_internal(src, ncq, status));
    check_eq(status, 1, "internal bezctx status");
    check_eq(internal.ncub, custom.ncub, "internal/custom cubic count");
    check_eq((int)internal.knot_pos.size(), (int)custom.knot_pos.size(),
             "internal/custom knot count");
    auto pc = sample_records(run_custom(src, 5));
    auto pi = sample_records(internal.draw);
    double rel = symmetric_distance(pc, pi) / bbox_diagonal(pc);
    check_le(rel, XFORM_REL, "internal vs custom geometry");
    record_metric("callbacks", "internal_bezctx", "geometry_vs_custom_rel",
                  rel, XFORM_REL);
    // Knot rows are ownership markers: each sits exactly on an input knot.
    for (size_t j = 0; j < custom.knot_pos.size(); ++j) {
        const spiro_cp& q = src[j];
        check_le(distance(custom.knot_pos[j], {q.x, q.y}), END_ABS,
                 "custom knot ownership " + std::to_string(j));
        check_le(distance(internal.knot_pos[j], {q.x, q.y}), END_ABS,
                 "internal knot ownership " + std::to_string(j));
    }
    // Tag/mark rows must not appear as drawing segments.
    check_eq((int)custom.draw.size() + (int)custom.knot_pos.size(),
             (int)run_custom(src, 5).size(), "knot rows are separate from geometry");
}

CHECK_CASE("callbacks", callbacks_last_knot_flag) {
    spiro_cp src[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'}, {180, 40, '}'}};
    int s0 = 0, s1 = 0;
    auto without = run_internal(src, SPIRO_INTERNAL_BEZCTX, s0);
    auto with = run_internal(src,
                             SPIRO_INTERNAL_BEZCTX | SPIRO_INCLUDE_LAST_KNOT, s1);
    check_eq(s0, 1, "internal no-last-knot status");
    check_eq(s1, 1, "internal last-knot status");
    int k0 = 0, k1 = 0;
    for (const Record& r : without) if (r.kind == Record::KNOT) ++k0;
    for (const Record& r : with) if (r.kind == Record::KNOT) ++k1;
    check_eq(k1, k0 + 1, "SPIRO_INCLUDE_LAST_KNOT adds exactly one knot row");
    if (with.back().kind != Record::KNOT) fail("last knot row not trailing");
    // Geometry is identical regardless of the flag.
    PathInfo P0 = extract(without), P1 = extract(with);
    check_eq(P1.ncub, P0.ncub, "last-knot flag leaves cubic count unchanged");
    double rel = symmetric_distance(sample_records(P0.draw),
                                    sample_records(P1.draw))
                 / bbox_diagonal(sample_records(P0.draw));
    check_le(rel, XFORM_REL, "last-knot flag leaves geometry unchanged");

    RecordCtx ctx;
    check_eq(SpiroCPsToBezier2(src, 4,
                               SPIRO_INCLUDE_LAST_KNOT, 0, &ctx.base), 1,
             "custom ctx last-knot status");
}

CHECK_CASE("callbacks", callbacks_optional_mark_callback) {
    // A missing optional mark_knot must still allow a full conversion.
    spiro_cp src[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, 'c'}, {180, 40, '}'}};
    RecordCtx ctx;
    ctx.base.mark_knot = nullptr;
    check_eq(SpiroCPsToBezier2(src, 4, 0, 0, &ctx.base), 1,
             "conversion with absent mark_knot");
    if (count_cubics(ctx.rec) <= 0) fail("absent mark_knot lost drawing output");
}

CHECK_CASE("callbacks", callbacks_error_inputs_rejected) {
    spiro_cp good[] = {{0, 0, '{'}, {40, 60, 'c'}, {90, 70, '}'}};
    RecordCtx ctx;
    // Missing context / bad arguments are status failures, never geometry.
    check_eq(SpiroCPsToBezier2(good, 3, 0, 0, nullptr), 0, "null explicit ctx");
    check_eq(TaggedSpiroCPsToBezier2(good, 0, nullptr), 0, "null tagged ctx");
    check_eq(SpiroCPsToBezier2(good, 0, 0, 0, &ctx.base), 0, "non-positive n");
    check_eq(SpiroCPsToBezier2(good, 3, -1, 0, &ctx.base), 0, "negative ncq");
    // Coincident control points do not converge: rejected, not used as evidence.
    spiro_cp coincident[] = {{5, 5, '{'}, {5, 5, 'c'}, {5, 5, '}'}};
    double dm[6] = {0};
    if (run_spiro0(coincident, dm, 0, 3) != nullptr)
        fail("coincident control points unexpectedly converged");
    check_eq(SpiroCPsToBezier2(coincident, 3, 0, 0, &ctx.base), 0,
             "coincident points rejected by status entry");
    spiro_cp closed_coin[] = {{5, 5, 'c'}, {5, 5, 'c'}, {0, 0, 'z'}};
    if (run_spiro0(closed_coin, dm, 0, 3) != nullptr)
        fail("closed coincident control points unexpectedly converged");
}

} // namespace
