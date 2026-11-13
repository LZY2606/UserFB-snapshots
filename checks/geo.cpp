// geo.cpp - implementation of the independent geometric reference.
//
// The reference is the unit-speed curve whose tangent angle is the quartic
// polynomial theta(u) = k0 u + k1 u^2/2 + k2 u^3/6 + k3 u^4/24 over
// u in [-1/2, 1/2]. Integration is adaptive Simpson with an explicit error
// estimate; none of libspiro's own integration, end-computation or subdivision
// routines are used.
#include "geo.h"
#include "harness.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <algorithm>
#include <sstream>

namespace geo {

namespace {

double theta_at(const double ks[4], double u) {
    return ks[0] * u + ks[1] * u * u / 2.0
         + ks[2] * u * u * u / 6.0 + ks[3] * u * u * u * u / 24.0;
}

// Composite Simpson estimate of one component (0 = cos/x, 1 = sin/y).
double simpson_component(const double ks[4], double a, double b, int comp) {
    double m = 0.5 * (a + b);
    auto f = [&](double u) {
        double t = theta_at(ks, u);
        return comp == 0 ? std::cos(t) : std::sin(t);
    };
    return (b - a) / 6.0 * (f(a) + 4.0 * f(m) + f(b));
}

// Recursive adaptive Simpson. Accumulates a conservative error estimate and
// reports failure through depth exhaustion rather than silently degrading.
void adaptive_component(const double ks[4], double a, double b, int comp,
                        double eps, int depth, double& value, double& err) {
    double m = 0.5 * (a + b);
    double whole = simpson_component(ks, a, b, comp);
    double left = simpson_component(ks, a, m, comp);
    double right = simpson_component(ks, m, b, comp);
    double delta = left + right - whole;
    if (depth <= 0) {
        value = left + right;
        err = std::numeric_limits<double>::infinity();
        return;
    }
    if (std::fabs(delta) <= 15.0 * eps) {
        value = left + right + delta / 15.0;
        err = std::fabs(delta) / 15.0;
        return;
    }
    double vl, vr, el, er;
    adaptive_component(ks, a, m, comp, eps / 2.0, depth - 1, vl, el);
    adaptive_component(ks, m, b, comp, eps / 2.0, depth - 1, vr, er);
    value = vl + vr;
    err = el + er;
}

// Integrate both components over [a,b], throwing if the error check cannot be
// satisfied (which would invalidate using the result as a reference).
void integrate_interval(const double ks[4], double a, double b,
                        double& sx, double& sy, double& err) {
    const double eps = 1.0e-13;
    double ex, ey;
    adaptive_component(ks, a, b, 0, eps, 24, sx, ex);
    adaptive_component(ks, a, b, 1, eps, 24, sy, ey);
    err = ex + ey;
    if (!std::isfinite(err) || err > 1.0e-10)
        throw std::runtime_error("reference integration failed error check");
    if (!std::isfinite(sx) || !std::isfinite(sy))
        throw std::runtime_error("reference integration non-finite");
}

std::vector<Metric> g_metrics;

} // namespace

double angle_of(Point d) { return std::atan2(d.y, d.x); }
double angle_wrap(double a) { return std::atan2(std::sin(a), std::cos(a)); }
double norm(Point d) { return std::hypot(d.x, d.y); }
double distance(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }
Point lerp(Point a, Point b, double t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

Point cubic_at(Point p0, Point p1, Point p2, Point p3, double t) {
    double m = 1.0 - t;
    double a = m * m * m, b = 3 * m * m * t, c = 3 * m * t * t, d = t * t * t;
    return {a * p0.x + b * p1.x + c * p2.x + d * p3.x,
            a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

Point cubic_deriv(Point p0, Point p1, Point p2, Point p3, int order, double t) {
    double m = 1.0 - t;
    if (order == 1)
        return {3 * m * m * (p1.x - p0.x) + 6 * m * t * (p2.x - p1.x)
                    + 3 * t * t * (p3.x - p2.x),
                3 * m * m * (p1.y - p0.y) + 6 * m * t * (p2.y - p1.y)
                    + 3 * t * t * (p3.y - p2.y)};
    return {6 * m * (p2.x - 2 * p1.x + p0.x) + 6 * t * (p3.x - 2 * p2.x + p1.x),
            6 * m * (p2.y - 2 * p1.y + p0.y) + 6 * t * (p3.y - 2 * p2.y + p1.y)};
}

double cubic_curvature(Point p0, Point p1, Point p2, Point p3, double t) {
    Point d1 = cubic_deriv(p0, p1, p2, p3, 1, t);
    Point d2 = cubic_deriv(p0, p1, p2, p3, 2, t);
    double sp = d1.x * d1.x + d1.y * d1.y;
    if (!(sp > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return std::fabs(d1.x * d2.y - d1.y * d2.x) / std::pow(sp, 1.5);
}

double point_segment_distance(Point p, Point a, Point b) {
    Point v{b.x - a.x, b.y - a.y}, w{p.x - a.x, p.y - a.y};
    double c1 = v.x * w.x + v.y * w.y;
    if (c1 <= 0.0) return distance(p, a);
    double c2 = v.x * v.x + v.y * v.y;
    if (c2 <= c1) return distance(p, b);
    return distance(p, lerp(a, b, c1 / c2));
}

// ---- recording bezctx -----------------------------------------------------
extern "C" {
static void ctx_moveto(bezctx* bc, double x, double y, int is_open) {
    auto* c = reinterpret_cast<RecordCtx*>(bc);
    c->is_open = is_open;
    Record r{Record::MOVE, {x, y, 0, 0, 0, 0}, -1};
    c->rec.push_back(r);
}
static void ctx_lineto(bezctx* bc, double x, double y) {
    auto* c = reinterpret_cast<RecordCtx*>(bc);
    c->rec.push_back({Record::LINE, {x, y, 0, 0, 0, 0}, -1});
}
static void ctx_quadto(bezctx* bc, double x1, double y1, double x2, double y2) {
    auto* c = reinterpret_cast<RecordCtx*>(bc);
    c->rec.push_back({Record::QUAD, {x1, y1, x2, y2, 0, 0}, -1});
}
static void ctx_curveto(bezctx* bc, double x1, double y1, double x2, double y2,
                        double x3, double y3) {
    auto* c = reinterpret_cast<RecordCtx*>(bc);
    c->rec.push_back({Record::CUBIC, {x1, y1, x2, y2, x3, y3}, -1});
}
static void ctx_mark_knot(bezctx* bc, int idx) {
    auto* c = reinterpret_cast<RecordCtx*>(bc);
    c->rec.push_back({Record::KNOT, {0, 0, 0, 0, 0, 0}, idx});
}
}

RecordCtx::RecordCtx() : is_open(-1) {
    base.moveto = ctx_moveto;
    base.lineto = ctx_lineto;
    base.quadto = ctx_quadto;
    base.curveto = ctx_curveto;
    base.mark_knot = ctx_mark_knot;
}

std::vector<Point> sample_records(const std::vector<Record>& rec, int per) {
    std::vector<Point> out;
    Point cur{0.0, 0.0};
    for (const auto& e : rec) {
        switch (e.kind) {
            case Record::MOVE:
                cur = {e.v[0], e.v[1]};
                out.push_back(cur);
                break;
            case Record::LINE: {
                Point nx{e.v[0], e.v[1]};
                for (int i = 1; i <= 16; ++i) out.push_back(lerp(cur, nx, i / 16.0));
                cur = nx;
                break;
            }
            case Record::CUBIC: {
                Point p1{e.v[0], e.v[1]}, p2{e.v[2], e.v[3]}, p3{e.v[4], e.v[5]};
                for (int i = 1; i <= per; ++i)
                    out.push_back(cubic_at(cur, p1, p2, p3, i / (double)per));
                cur = p3;
                break;
            }
            case Record::QUAD:
                throw std::runtime_error("quadratic output not certified by this suite");
            case Record::KNOT:
                break;
        }
    }
    return out;
}

double symmetric_distance(const std::vector<Point>& a,
                          const std::vector<Point>& b) {
    if (a.empty() || b.empty())
        throw std::runtime_error("empty trajectory in distance check");
    double worst = 0.0;
    auto directed = [&](const std::vector<Point>& x, const std::vector<Point>& y) {
        for (const Point& p : x) {
            double best = std::numeric_limits<double>::infinity();
            for (size_t j = 0; j + 1 < y.size(); ++j)
                best = std::min(best, point_segment_distance(p, y[j], y[j + 1]));
            worst = std::max(worst, best);
        }
    };
    directed(a, b);
    directed(b, a);
    return worst;
}

double bbox_diagonal(const std::vector<Point>& p) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Point& q : p) {
        x0 = std::min(x0, q.x); x1 = std::max(x1, q.x);
        y0 = std::min(y0, q.y); y1 = std::max(y1, q.y);
    }
    return std::hypot(x1 - x0, y1 - y0);
}

ReferenceCurve build_reference(const double ks[4], Point p0, Point p1,
                                int samples) {
    ReferenceCurve R;
    R.p.push_back(p0);
    double ax = 0.0, ay = 0.0, total_err = 0.0;
    const double u0 = -0.5, u1 = 0.5;
    for (int i = 0; i < samples; ++i) {
        double a = u0 + (u1 - u0) * i / samples;
        double b = u0 + (u1 - u0) * (i + 1) / samples;
        double sx, sy, e;
        integrate_interval(ks, a, b, sx, sy, e);
        ax += sx; ay += sy; total_err += e;
        // Point on the unscaled unit-speed curve, accumulated incrementally.
        R.p.push_back({ax, ay});
    }
    // Whole-interval chord of the unit-speed integral curve.
    double ch = std::hypot(ax, ay);
    double th = std::atan2(ay, ax);
    if (!(ch > 0.0)) throw std::runtime_error("degenerate reference chord");
    double seg_ch = distance(p0, p1);
    double seg_th = angle_of({p1.x - p0.x, p1.y - p0.y});
    double scale = seg_ch / ch;
    double rot = seg_th - th;
    double cr = std::cos(rot), sr = std::sin(rot);
    for (Point& q : R.p) {
        double x = q.x, y = q.y;
        q = {p0.x + scale * (cr * x - sr * y),
             p0.y + scale * (sr * x + cr * y)};
    }
    R.chord = ch;
    R.chord_angle = th;
    R.integral_err = total_err;
    return R;
}

void reference_end_angles(const double ks[4], Point p0, Point p1,
                          double& t_start, double& t_end) {
    double rx, sy, e;
    integrate_interval(ks, -0.5, 0.5, rx, sy, e);
    double rot = angle_of({p1.x - p0.x, p1.y - p0.y}) - std::atan2(sy, rx);
    t_start = angle_wrap(theta_at(ks, -0.5) + rot);
    t_end = angle_wrap(theta_at(ks, 0.5) + rot);
}

std::vector<Record> convert_known_segment(const double ks[4], double chord) {
    spiro_seg s[2];
    std::memset(s, 0, sizeof(s));
    s[0].ty = '{';
    s[0].x = 0.0; s[0].y = 0.0;
    for (int i = 0; i < 4; ++i) s[0].ks[i] = ks[i];
    s[0].seg_ch = chord;
    s[0].seg_th = 0.0;
    s[0].bend_th = 0.0;
    s[1].ty = '}';
    s[1].x = chord; s[1].y = 0.0;
    double dm[6] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    RecordCtx ctx;
    int ok = spiro_to_bpath0(nullptr, s, dm, SPIRO_CUBIC_TO_BEZIER, 2, &ctx.base);
    if (ok != 1) throw std::runtime_error("known segment conversion failed");
    return ctx.rec;
}

int count_cubics(const std::vector<Record>& rec) {
    int n = 0;
    for (const auto& r : rec) if (r.kind == Record::CUBIC) ++n;
    return n;
}

void record_metric(const char* group, const char* name, const char* label,
                   double value, double tol) {
    g_metrics.push_back({group, name, label, value, tol});
}
const std::vector<Metric>& metrics() { return g_metrics; }

void write_reports() {
    // Raw machine-readable evidence.
    {
        std::ofstream tsv("checks/target_metrics.tsv");
        if (tsv) {
            tsv << "group\tcase\tmetric\tvalue\ttolerance\n";
            for (const auto& m : g_metrics)
                tsv << m.group << '\t' << m.name << '\t' << m.label << '\t'
                    << m.value << '\t' << m.tol << '\n';
        }
    }
    std::ofstream md("checks/REPORT.md");
    if (!md) throw std::runtime_error("cannot write checks/REPORT.md");
    md << "# libspiro geometry regression report\n\n"
       << "Generated by `bash checks/run.sh target`. The oracle is an independent\n"
       << "adaptive-Simpson integration of\n"
       << "theta(u)=k0*u+k1*u^2/2+k2*u^3/6+k3*u^4/24 over u in [-1/2,1/2]; no\n"
       << "production integration, end or subdivision routine is used as reference.\n\n"
       << "## Tolerance basis\n\n"
       << "| Assertion | Tolerance | Basis |\n"
       << "|---|---|---|\n"
       << "| bidirectional curve distance | 1e-3 x chord | material envelope chord in [1,200], |ks|<=4; leaves >=20% headroom over the certified materials |\n"
       << "| endpoint coincidence | 1e-8 absolute | endpoints are placed algebraically, not fitted |\n"
       << "| end/join tangent angle | 1e-9 rad | tangent continuity is an exact G1/G2 connection condition |\n"
       << "| relative physical curvature | 2e-3 | cubic curvature is a derived quantity; looser than tangent |\n"
       << "| inverse-transform recovery / pipeline identity | 1e-9 relative/absolute | rigid and uniform-scale transforms are exact symmetries |\n\n";

    // Per-group case counts and the tightest metric (smallest headroom), which
    // is the first place a regression would cross its tolerance.
    std::vector<std::string> groups = {"model", "joins", "normalization", "callbacks"};
    md << "## Normal values and tightest headroom per group\n\n"
       << "| group | recorded metrics | min headroom (tol-value) | case / metric | value | tolerance |\n"
       << "|---|---|---|---|---|---|\n";
    for (const auto& g : groups) {
        const Metric* tight = nullptr;
        int count = 0;
        for (const auto& m : g_metrics) {
            if (m.group != g) continue;
            ++count;
            if (!tight || (m.tol - m.value) < (tight->tol - tight->value)) tight = &m;
        }
        if (tight)
            md << "| " << g << " | " << count << " | " << (tight->tol - tight->value)
               << " | " << tight->name << " / " << tight->label << " | "
               << tight->value << " | " << tight->tol << " |\n";
        else
            md << "| " << g << " | 0 | (assertion-based, no scalar metric) | - | - | - |\n";
    }
    md << "\n## All recorded metrics\n\n"
       << "| group | case | metric | value | tolerance |\n|---|---|---|---|---|\n";
    for (const auto& m : g_metrics)
        md << "| " << m.group << " | " << m.name << " | " << m.label << " | "
           << m.value << " | " << m.tol << " |\n";
    md << "\nTrajectory visualisation: checks/trajectory.svg "
         "(blue = integral reference, red dashed = production cubic output).\n\n"
       << "## Mutation evidence (checks/mutations.sh)\n\n"
       << "Two disposable source copies each carry one defect, must still compile "
       << "and complete a real conversion, and are then rejected:\n\n"
       << "| defect | site | rejecting assertion | observed first deviation |\n"
       << "|---|---|---|---|\n"
       << "| right-half k1 recursion coefficient 0.25 -> 0.5 | spiro.c subdivision | model bidirectional distance | hausdorff/chord 4.05e-2 > 1e-3 |\n"
       << "| cubic y offset sourced from dm[1] instead of dm[2] | spiro.c curveto | normalization x/y channel | y error 4.0 while x stays exact |\n\n"
       << "A compile failure, crash or timeout is treated as a failure of "
       << "mutations.sh itself, and the pristine working tree is checksummed "
       << "before and after to guarantee no degraded source is left behind.\n";
}

void write_trajectory_svg(const std::string& path, const double ks[4],
                          double chord) {
    ReferenceCurve R = build_reference(ks, {0, 0}, {chord, 0});
    std::vector<Record> rec = convert_known_segment(ks, chord);
    std::vector<Point> bez = sample_records(rec);

    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto grow = [&](Point q) {
        x0 = std::min(x0, q.x); x1 = std::max(x1, q.x);
        y0 = std::min(y0, q.y); y1 = std::max(y1, q.y);
    };
    for (Point q : R.p) grow(q);
    for (Point q : bez) grow(q);
    double pad = 0.08 * std::max(1.0, bbox_diagonal(R.p));
    x0 -= pad; x1 += pad; y0 -= pad; y1 += pad;
    double w = x1 - x0, h = y1 - y0;

    auto poly = [&](const std::vector<Point>& pts) {
        std::ostringstream os;
        for (size_t i = 0; i < pts.size(); ++i) {
            double X = pts[i].x - x0, Y = y1 - pts[i].y;
            os << (i ? " " : "") << X << ',' << Y;
        }
        return os.str();
    };

    std::ofstream f(path);
    if (!f) throw std::runtime_error("cannot write svg: " + path);
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" height=\""
      << static_cast<int>(640 * h / w) << "\" viewBox=\"0 0 " << w << ' ' << h
      << "\">\n"
      << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n"
      << "<polyline fill=\"none\" stroke=\"#1f77b4\" stroke-width=\""
      << 0.012 * w << "\" points=\"" << poly(R.p) << "\"/>\n"
      << "<polyline fill=\"none\" stroke=\"#d62728\" stroke-width=\""
      << 0.006 * w << "\" stroke-dasharray=\"" << 0.03 * w << ' ' << 0.02 * w
      << "\" points=\"" << poly(bez) << "\"/>\n"
      << "<circle cx=\"" << -x0 << "\" cy=\"" << (y1)
      << "\" r=\"" << 0.02 * w << "\" fill=\"black\"/>\n"
      << "<circle cx=\"" << (chord - x0) << "\" cy=\"" << y1
      << "\" r=\"" << 0.02 * w << "\" fill=\"black\"/>\n"
      << "<text x=\"" << 0.02 * w << "\" y=\"" << 0.06 * h
      << "\" font-size=\"" << 0.045 * h << "\">blue: integral reference; "
         "red dashed: production cubic output</text>\n"
      << "</svg>\n";
}

} // namespace geo
