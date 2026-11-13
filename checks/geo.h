// geo.h - independent geometric reference and shared helpers for checks/target.cpp
//
// Everything here is written from the curvature model and ordinary numerical
// analysis only. It deliberately does not call, include or depend on any
// internal routine of libspiro (no integrate_spiro / compute_ends / subdivision
// code is reachable from this translation unit).
#pragma once
#include "spiroentrypoints.h"
#include "bezctx.h"
#include <string>
#include <vector>
#include <iosfwd>

namespace geo {

constexpr double PI = 3.14159265358979323846;

struct Point { double x, y; };

// A single drawing record captured by a user supplied bezctx.
struct Record {
    enum Kind { MOVE, LINE, QUAD, CUBIC, KNOT } kind;
    double v[6];	// geometry payload (meaning depends on kind)
    int knot;		// mark_knot index, -1 otherwise
};

// A bezctx implementation that records the full path plus mark_knot events.
struct RecordCtx {
    bezctx base;
    std::vector<Record> rec;
    int is_open;
    RecordCtx();
};

// Tolerance knobs, kept in one place so REPORT.md can cite the same numbers.
constexpr double DIST_REL = 1.0e-3;	// bidirectional distance / chord length
constexpr double END_ABS  = 1.0e-8;	// absolute endpoint coincidence
constexpr double TAN_ABS  = 1.0e-9;	// absolute end/join tangent angle (rad)
constexpr double KAPPA_REL = 2.0e-3;	// relative physical curvature mismatch
constexpr double XFORM_REL = 1.0e-9;	// inverse-transform trajectory recovery
constexpr double COORD_ABS = 1.0e-9;	// exact-pipeline coordinate identity

// ---- angle / vector helpers ----
double angle_of(Point d);
double angle_wrap(double a);
double norm(Point d);
double distance(Point a, Point b);
Point lerp(Point a, Point b, double t);

// ---- cubic Bezier geometry (de Casteljau / analytic derivatives) ----
Point cubic_at(Point p0, Point p1, Point p2, Point p3, double t);
Point cubic_deriv(Point p0, Point p1, Point p2, Point p3, int order, double t);
double cubic_curvature(Point p0, Point p1, Point p2, Point p3, double t);
double point_segment_distance(Point p, Point a, Point b);

// Dense, parameter-independent polylines of a recorded path (knot rows skipped).
std::vector<Point> sample_records(const std::vector<Record>& rec, int per = 64);
double symmetric_distance(const std::vector<Point>& a, const std::vector<Point>& b);
double bbox_diagonal(const std::vector<Point>& p);

// ---- independent curvature-model reference ----
//
// Normalised arc-length parameter u in [-1/2, 1/2], tangent angle
//   theta(u) = k0 u + k1 u^2/2 + k2 u^3/6 + k3 u^4/24.
// The unit-speed curve r(u) = integral (cos theta, sin theta) du is obtained
// with adaptive Simpson quadrature that carries an explicit error estimate and
// aborts if the estimate cannot be driven below the requested bound. It is then
// rigidly placed between the requested endpoints using its own chord length and
// chord direction, so it shares endpoints with the production segment but has
// no other dependency on the library.
struct ReferenceCurve {
    std::vector<Point> p;	// unit-speed reference, placed on the endpoints
    double chord;		// reference chord length (unscaled integral)
    double chord_angle;	// reference chord direction (unscaled)
    double integral_err;	// worst adaptive-Simpson error estimate
};

ReferenceCurve build_reference(const double ks[4], Point p0, Point p1,
                               int samples = 400);

// End tangent angles of the placed reference curve.
void reference_end_angles(const double ks[4], Point p0, Point p1,
                          double& t_start, double& t_end);

// Feed a single, publicly described spiro_seg to the production converter using
// identity normalisation (dm = 1,0,0) and return what it drew.
std::vector<Record> convert_known_segment(const double ks[4], double chord);
int count_cubics(const std::vector<Record>& rec);

// ---- metrics for checks/REPORT.md ----
struct Metric { std::string group, name, label; double value, tol; };
void record_metric(const char* group, const char* name, const char* label,
                   double value, double tol);
const std::vector<Metric>& metrics();

// Rewrite checks/target_metrics.tsv and the normal-run part of checks/REPORT.md
// from every metric recorded so far. Called eagerly (no static-destructor order
// dependency) and safe to invoke repeatedly.
void write_reports();

// Write a small SVG overlaying the integral and Bezier trajectories.
void write_trajectory_svg(const std::string& path, const double ks[4],
                          double chord);

} // namespace geo
