#include "task_support/check.h"
#include "visibility_gradient.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

// Independent test references (deterministic quadrature / analytic moments).
// Finite differences appear only here, as test references -- never inside the
// engine entry points.

namespace {

using vg::Camera;
using vg::RectLight;
using vg::VisibilityEngine;

constexpr double PI = 3.14159265358979323846;


double vlen(const Vec &v) { return std::sqrt(v.dot(v)); }

Vec vnorm(const Vec &v) {
  const double l = vlen(v);
  return Vec(v.x / l, v.y / l, v.z / l);
}

Vec vcross(const Vec &a, const Vec &b) {
  return Vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x);
}

double slack(double t) { return 1e-7 * std::max(1.0, t); }

bool refBlocked(const std::vector<Sphere> &ss, const Vec &o, const Vec &w,
                double tMax) {
  for (const Sphere &s : ss) {
    const double t = s.intersect(Ray(o, w));
    if (t > 0.0 && t < tMax - slack(tMax)) return true;
  }
  return false;
}

// Independent deterministic midpoint quadrature of the rectangle integral.
Vec refColor(const std::vector<Sphere> &ss, const RectLight &light,
             const Vec &x, const Vec &n, const Vec &rho, int gu, int gv) {
  const double area = 4.0 * light.half1 * light.half2;
  double acc[3] = {0.0, 0.0, 0.0};
  for (int iu = 0; iu < gu; ++iu) {
    const double u = (2.0 * (iu + 0.5) / gu - 1.0) * light.half1;
    for (int iv = 0; iv < gv; ++iv) {
      const double v = (2.0 * (iv + 0.5) / gv - 1.0) * light.half2;
      const Vec y = light.center + light.axis1 * u + light.axis2 * v;
      const Vec delta = y - x;
      const double r2 = delta.dot(delta);
      if (!(r2 > 0.0)) continue;
      const double r = std::sqrt(r2);
      const Vec w = delta * (1.0 / r);
      const double cosX = n.dot(w);
      const double cosY = -w.dot(light.normal);
      if (cosX <= 0.0 || cosY <= 0.0) continue;
      if (refBlocked(ss, x, w, r)) continue;
      const double wgt = area * cosX * cosY / (gu * gv * PI * r2);
      acc[0] += rho.x * light.emission.x * wgt;
      acc[1] += rho.y * light.emission.y * wgt;
      acc[2] += rho.z * light.emission.z * wgt;
    }
  }
  return Vec(acc[0], acc[1], acc[2]);
}

std::vector<Sphere> shiftedCopy(std::vector<Sphere> ss, int k, int axis,
                                double h) {
  if (axis == 0) ss[k].p.x += h;
  if (axis == 1) ss[k].p.y += h;
  if (axis == 2) ss[k].p.z += h;
  return ss;
}

// Central difference of the independent quadrature, test reference only.
Vec fdGradient(const std::vector<Sphere> &ss, const RectLight &light, int k,
               int axis, double h, const Vec &x, const Vec &n,
               const Vec &rho, int grid) {
  const Vec cp = refColor(shiftedCopy(ss, k, axis, h), light, x, n, rho,
                          grid, grid);
  const Vec cm = refColor(shiftedCopy(ss, k, axis, -h), light, x, n, rho,
                          grid, grid);
  return (cp - cm) * (1.0 / (2.0 * h));
}

// Analytic first moment of a full spherical cap: integral_cap (n . w) dw
// equals pi sin^2(alpha) (n . a), with sin(alpha) = R/D, a = d/D, so
// I = pi R^2 (n . d) / D^3.  The color carries a 1/pi factor, which cancels
// the pi above: dColor/dc = -rho Le R^2 (n/D^3 - 3(n.d)d/D^5).
vg::RGBGradient analyticCapGradient(const Sphere &sph, const Vec &x,
                                    const Vec &n, const Vec &rho,
                                    const Vec &le) {
  const Vec d = sph.p - x;
  const double D = vlen(d);
  const double R = sph.rad;
  const double d3 = D * D * D;
  const double d5 = d3 * D * D;
  const Vec base = n * (1.0 / d3) - d * (3.0 * n.dot(d) / d5);
  vg::RGBGradient g{};
  const double scale = R * R;
  const double rhoLe[3] = {rho.x * le.x, rho.y * le.y, rho.z * le.z};
  for (int a = 0; a < 3; ++a) {
    const double ba = a == 0 ? base.x : a == 1 ? base.y : base.z;
    g[a] = Vec(-rhoLe[0] * scale * ba, -rhoLe[1] * scale * ba,
               -rhoLe[2] * scale * ba);
  }
  return g;
}

bool relClose(const Vec &a, const Vec &b, double tol, double absTol = 1e-12) {
  const double scale =
      std::max({std::fabs(a.x), std::fabs(a.y), std::fabs(a.z),
                std::fabs(b.x), std::fabs(b.y), std::fabs(b.z), 1e-30});
  return (std::fabs(a.x - b.x) < tol * scale ||
          std::fabs(a.x - b.x) < absTol) &&
         (std::fabs(a.y - b.y) < tol * scale ||
          std::fabs(a.y - b.y) < absTol) &&
         (std::fabs(a.z - b.z) < tol * scale ||
          std::fabs(a.z - b.z) < absTol);
}

double gradMaxComponent(const std::vector<vg::RGBGradient> &g) {
  double m = 0.0;
  for (const auto &ga : g)
    for (const Vec &v : ga)
      m = std::max(m, std::max({std::fabs(v.x), std::fabs(v.y),
                                std::fabs(v.z)}));
  return m;
}

double gradDistance(const std::vector<vg::RGBGradient> &a,
                    const std::vector<vg::RGBGradient> &b) {
  double m = 0.0;
  for (size_t k = 0; k < a.size(); ++k)
    for (int ax = 0; ax < 3; ++ax)
      for (int c = 0; c < 3; ++c) {
        const double av = c == 0 ? a[k][ax].x : c == 1 ? a[k][ax].y : a[k][ax].z;
        const double bv = c == 0 ? b[k][ax].x : c == 1 ? b[k][ax].y : b[k][ax].z;
        m = std::max(m, std::fabs(av - bv));
      }
  return m;
}

// Componentwise relative match with an absolute floor for near-zero
// components (symmetry-enforced zeros).
bool gradRelClose(const Vec &a, const Vec &b, double tol,
                  double absTol = 1e-10) {
  for (int c = 0; c < 3; ++c) {
    const double av = c == 0 ? a.x : c == 1 ? a.y : a.z;
    const double bv = c == 0 ? b.x : c == 1 ? b.y : b.z;
    const double scale = std::max(std::fabs(av), std::fabs(bv));
    if (!(std::fabs(av - bv) < tol * scale || std::fabs(av - bv) < absTol))
      return false;
  }
  return true;
}

bool throwsInvalid(const std::function<void()> &f) {
  try {
    f();
  } catch (const std::invalid_argument &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

RectLight makeLight(Vec center, double h1, double h2, Vec emission,
                    Vec normal = Vec(0, 0, -1)) {
  return RectLight{center, Vec(1, 0, 0), Vec(0, 1, 0), h1, h2, normal,
                   emission};
}

// ---------------------------------------------------------------- radiometry

int test_unoccluded_quadrature() {
  const Vec x(0, 0, 0), n(0, 0, 1), rho(0.5, 0.6, 0.7);
  RectLight light = makeLight(Vec(0, 0, 10), 3.0, 2.0, Vec(1, 2, 3));
  VisibilityEngine eng({}, light, {}, 12345);
  const auto q = eng.query(x, n, rho, 200000, 64);
  const Vec ref = refColor({}, light, x, n, rho, 220, 160);
  TASK_CHECK(relClose(q.color, ref, 0.01));
  TASK_CHECK(q.gradient.empty());
  TASK_CHECK(q.boundarySamples == 0 && q.arcsKept == 0);
  TASK_CHECK(q.segmentQueries == 200000); // every area sample is a segment test

  // Offset light: cosine factors vary across the rectangle.
  RectLight tilted = makeLight(Vec(2, -1, 10), 3.0, 2.0, Vec(1, 2, 3));
  VisibilityEngine eng2({}, tilted, {}, 6789);
  const auto q2 = eng2.query(x, n, rho, 200000, 64);
  const Vec ref2 = refColor({}, tilted, x, n, rho, 220, 160);
  TASK_CHECK(relClose(q2.color, ref2, 0.01));
  // Channel ratios are exactly the rho*Le ratios.
  const double r0 = q.color.x / (rho.x * light.emission.x);
  TASK_CHECK(std::fabs(q.color.y / (rho.y * light.emission.y) - r0) < 1e-12);
  TASK_CHECK(std::fabs(q.color.z / (rho.z * light.emission.z) - r0) < 1e-12);
  return 0;
}

int test_receiver_sphere_and_full_shadow() {
  const Vec x(0, 0, 0), n(0, 0, 1), rho(0.7, 0.7, 0.7);
  RectLight light = makeLight(Vec(0, 0, 10), 3.0, 2.0, Vec(2, 2, 2));

  // A sphere whose surface passes through x: its near tangent root must be
  // rejected by the positive-root tolerance, not by skipping the sphere.
  std::vector<Sphere> ground;
  ground.emplace_back(100.0, Vec(0, 0, -100), Vec(), rho, DIFF);
  VisibilityEngine eng(ground, light, {}, 11);
  const auto q = eng.query(x, n, rho, 100000, 64);
  const Vec ref = refColor({}, light, x, n, rho, 200, 160);
  TASK_CHECK(relClose(q.color, ref, 0.01));

  // Sphere whose cone covers the whole rectangle: color is exactly zero.
  std::vector<Sphere> blocked = ground;
  blocked.emplace_back(3.0, Vec(0, 0, 5), Vec(), Vec(0.1, 0.1, 0.1), DIFF);
  VisibilityEngine eng2(blocked, light, {1}, 22);
  const auto q2 = eng2.query(x, n, rho, 4000, 256);
  TASK_CHECK(q2.color.x == 0.0 && q2.color.y == 0.0 && q2.color.z == 0.0);

  // Receiver normal facing away: no front-facing light.
  const auto q3 = eng2.query(x, Vec(0, 0, -1), rho, 100, 64);
  TASK_CHECK(q3.color.x == 0.0 && q3.color.y == 0.0 && q3.color.z == 0.0);
  return 0;
}

// ------------------------------------------------------------------ boundary

int test_interior_cap_analytic() {
  const Vec x(0, 0, 0), n(0, 0, 1), rho(0.1, 0.2, 0.3);
  // Cone footprint on the z=12 plane stays strictly inside the 10x10 light
  // even with the sphere offset sideways (footprint center x = 0.75,
  // radius ~3.10, max extent 3.86 < 5), so all three axes are exercised.
  RectLight light = makeLight(Vec(0, 0, 12), 5.0, 5.0, Vec(2, 3, 5));
  std::vector<Sphere> ss;
  ss.emplace_back(2.0, Vec(0.5, 0, 8), Vec(), Vec(0.4, 0.4, 0.4), DIFF);
  VisibilityEngine eng(ss, light, {0}, 99);
  const int N = 4096;
  const auto q = eng.query(x, n, rho, 256, N);

  const vg::RGBGradient expect =
      analyticCapGradient(ss[0], x, n, rho, light.emission);
  for (int a = 0; a < 3; ++a)
    TASK_CHECK(relClose(q.gradient[0][a], expect[a], 1e-9));

  // Every candidate survives: full cap, no clipping or coverage.
  TASK_CHECK(q.boundarySamples == (std::uint64_t)N);
  TASK_CHECK(q.arcsKept == (std::uint64_t)N);
  TASK_CHECK(q.segmentQueries == 256 + (std::uint64_t)N);

  // Sign sanity: moving the sphere toward the light shrinks the shadow, so
  // the color derivative along +z is positive on every channel.
  TASK_CHECK(q.gradient[0][2].x > 0 && q.gradient[0][2].y > 0 &&
             q.gradient[0][2].z > 0);
  // Sideways move (+x) carries the cap away from the receiver lobe peak at
  // +z, removing weighted shadow: the +x derivative is positive as well here.
  TASK_CHECK(q.gradient[0][0].x > 0 && q.gradient[0][0].y > 0 &&
             q.gradient[0][0].z > 0);
  // Mirror symmetry about y=0 makes the y derivative vanish exactly.
  TASK_CHECK(std::fabs(q.gradient[0][1].x) < 1e-12 &&
             std::fabs(q.gradient[0][1].y) < 1e-12 &&
             std::fabs(q.gradient[0][1].z) < 1e-12);
  return 0;
}

int test_rectangle_clipped_arcs() {
  const Vec x(0, 0, 0), n(0, 0, 1), rho(0.5, 0.5, 0.5);
  // Cone footprint on z=12 is a circle of radius 12 tan(alpha) ~ 3.10.  Half
  // extents of 2.6 lie between its diagonal approach (2.19) and its axial
  // extent (3.10), so the rectangle cuts the silhouette transversely: some
  // arcs survive, others miss the rectangle.
  RectLight light = makeLight(Vec(0, 0, 12), 2.6, 2.6, Vec(3, 3, 3));
  // Offset sphere breaks the mirror symmetry, so all three axes have nonzero
  // gradients; the shifted footprint (center (0.9, 0.45), radius ~3.10) still
  // overlaps the rectangle but crosses its edges.
  std::vector<Sphere> ss;
  ss.emplace_back(2.0, Vec(0.6, 0.3, 8), Vec(), Vec(0.4, 0.4, 0.4), DIFF);
  VisibilityEngine eng(ss, light, {0}, 7);
  const int N = 8192;
  const auto q = eng.query(x, n, rho, 8192, N);
  TASK_CHECK(q.arcsKept > 0 && q.arcsKept < q.boundarySamples);
  TASK_CHECK(q.boundarySamples == (std::uint64_t)N);

  std::printf("clipped-arc convergence (reference FD, grid=400):\n");
  // Steps stay in the truncation-dominated regime: at smaller steps the
  // discontinuous midpoint quadrature noise (shadow boundary toggling whole
  // cells) grows like 1/h, so the report uses steps where O(h^2) dominates.
  const double hs[3] = {0.08, 0.04, 0.02};
  double prevErr = std::numeric_limits<double>::infinity();
  for (double h : hs) {
    const Vec fd = fdGradient(ss, light, 0, 2, h, x, n, rho, 400);
    const double err = std::fabs(fd.x - q.gradient[0][2].x);
    const double scale = std::fabs(q.gradient[0][2].x);
    std::printf("  h=%.4f  fd=%.8f  boundary=%.8f  absErr=%.3e relErr=%.3e\n",
                h, fd.x, q.gradient[0][2].x, err, err / scale);
    TASK_CHECK(err < prevErr * 1.5);
    prevErr = err;
  }
  // Finest step: agreement within the combined FD/quadrature error.
  for (int axis = 0; axis < 3; ++axis) {
    const Vec fdFine =
        fdGradient(ss, light, 0, axis, 0.02, x, n, rho, 400);
    TASK_CHECK(relClose(fdFine, q.gradient[0][axis], 0.05));
  }

  // Report quadrature sampling error at the finest step (300 vs 600 grid).
  const Vec c300 = fdGradient(ss, light, 0, 2, 0.02, x, n, rho, 300);
  const Vec c600 = fdGradient(ss, light, 0, 2, 0.02, x, n, rho, 600);
  const double sampleErr =
      std::fabs(c300.z - c600.z) / std::max(std::fabs(c600.z), 1e-30);
  std::printf("  quadrature sampling estimate relErr=%.3e\n", sampleErr);
  TASK_CHECK(sampleErr < 0.03);
  TASK_CHECK(relClose(c600, q.gradient[0][2], 0.05));
  return 0;
}

int test_overlapping_spheres() {
  const Vec x(0, 0, 0), n(0, 0, 1), rho(0.6, 0.6, 0.6);
  RectLight light = makeLight(Vec(0, 0, 12), 5.0, 5.0, Vec(2, 2, 2));
  std::vector<Sphere> ss;
  ss.emplace_back(2.0, Vec(-0.8, 0.4, 8), Vec(), Vec(0.4, 0.4, 0.4), DIFF);
  ss.emplace_back(2.2, Vec(0.9, -0.5, 8.3), Vec(), Vec(0.4, 0.4, 0.4), DIFF);
  VisibilityEngine eng(ss, light, {0, 1}, 31);
  const int N = 8192;
  const auto q = eng.query(x, n, rho, 4096, N);
  TASK_CHECK(q.boundarySamples == 2 * (std::uint64_t)N);

  // Sphere 0's silhouette is partly covered by sphere 1: generic transverse
  // overlap, exposed arcs only.
  TASK_CHECK(q.arcsKept > 0);
  TASK_CHECK(q.arcsKept < 2 * (std::uint64_t)N);

  // The full-cap formula ignoring sphere 1 must disagree for sphere 0.
  const vg::RGBGradient cap0 =
      analyticCapGradient(ss[0], x, n, rho, light.emission);
  double maxDiff = 0.0;
  for (int a = 0; a < 3; ++a)
    maxDiff = std::max(maxDiff, gradDistance({q.gradient[0]}, {cap0}));
  TASK_CHECK(maxDiff > 0.05 * gradMaxComponent({cap0}));

  std::printf("overlap convergence (reference FD vs exposed-arc boundary):\n");
  for (int k = 0; k < 2; ++k) {
    for (int axis = 0; axis < 3; ++axis) {
      const Vec fd = fdGradient(ss, light, k, axis, 0.02, x, n, rho, 400);
      const double err = std::fabs(fd.x - q.gradient[k][axis].x) /
                         std::max(std::fabs(q.gradient[k][axis].x), 1e-12);
      std::printf("  sphere %d axis %d relErr=%.3e\n", k, axis, err);
      TASK_CHECK(gradRelClose(fd, q.gradient[k][axis], 0.08));
    }
  }
  return 0;
}

int test_behind_light_and_outside_cone() {
  const Vec x(0, 0, 0), n(0, 0, 1), rho(0.5, 0.5, 0.5);
  RectLight light = makeLight(Vec(0, 0, 12), 4.0, 4.0, Vec(2, 2, 2));

  // Sphere beyond the light plane: tangent points are behind the emitter.
  std::vector<Sphere> behind;
  behind.emplace_back(1.5, Vec(0, 0, 16), Vec(), Vec(0.3, 0.3, 0.3), DIFF);
  VisibilityEngine eng(behind, light, {0}, 3);
  const auto q = eng.query(x, n, rho, 256, 512);
  TASK_CHECK(q.arcsKept == 0 && q.boundarySamples == 512);
  TASK_CHECK(q.gradient[0][0].x == 0.0 && q.gradient[0][1].y == 0.0 &&
             q.gradient[0][2].z == 0.0);

  // Sphere whose cone never reaches the rectangle direction.
  std::vector<Sphere> wide;
  wide.emplace_back(1.0, Vec(8, 0, 8), Vec(), Vec(0.3, 0.3, 0.3), DIFF);
  VisibilityEngine eng2(wide, light, {0}, 4);
  const auto q2 = eng2.query(x, n, rho, 256, 512);
  TASK_CHECK(q2.arcsKept == 0);
  TASK_CHECK(q2.gradient[0][0].x == 0.0 && q2.gradient[0][2].z == 0.0);
  return 0;
}

int test_validation_rejects_illegal_inputs() {
  const Vec inf(std::numeric_limits<double>::infinity(), 0, 0);
  std::vector<Sphere> ss;
  ss.emplace_back(2.0, Vec(0, 0, 8), Vec(), Vec(0.4, 0.4, 0.4), DIFF);
  RectLight ok = makeLight(Vec(0, 0, 12), 4, 4, Vec(2, 2, 2));

  TASK_CHECK(throwsInvalid([&] {
    RectLight bad = ok;
    bad.axis1 = Vec(1, 1, 0);
    VisibilityEngine e(ss, bad, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    RectLight bad = ok;
    bad.normal = Vec(0, 0, 1);
    bad.axis2 = Vec(0, 0, 1); // axis2 no longer orthogonal to the normal
    VisibilityEngine e(ss, bad, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    RectLight bad = ok;
    bad.half1 = 0.0;
    VisibilityEngine e(ss, bad, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    RectLight bad = ok;
    bad.emission = Vec(-1, 0, 0);
    VisibilityEngine e(ss, bad, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    RectLight bad = ok;
    bad.center = inf;
    VisibilityEngine e(ss, bad, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    std::vector<Sphere> bad = ss;
    bad[0].rad = -1.0;
    VisibilityEngine e(bad, ok, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    std::vector<Sphere> bad = ss;
    bad[0].refl = SPEC; // only diffuse surfaces are supported
    VisibilityEngine e(bad, ok, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    std::vector<Sphere> bad = ss;
    bad[0].refl = REFR;
    VisibilityEngine e(bad, ok, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    std::vector<Sphere> bad = ss;
    bad[0].p = Vec(0, 0, 11); // radius 2 crosses the z=12 light plane
    VisibilityEngine e(bad, ok, {0}, 1);
  }));
  TASK_CHECK(throwsInvalid([&] {
    VisibilityEngine e(ss, ok, {5}, 1); // index out of range
  }));
  TASK_CHECK(throwsInvalid([&] {
    VisibilityEngine e(ss, ok, {0, 0}, 1); // duplicate index
  }));
  TASK_CHECK(throwsInvalid([&] {
    std::vector<Sphere> bad = ss;
    bad.emplace_back(2.0, Vec(1.0, 0, 8), Vec(), Vec(0.3, 0.3, 0.3), DIFF);
    // The two spheres overlap (centers 1 apart, radii sum 4); designating
    // only the first makes the second a receiver surface.
    VisibilityEngine e(bad, ok, {0}, 1);
  }));
  {
    // Same overlapping pair is legal when both are designated occluders.
    std::vector<Sphere> both = ss;
    both.emplace_back(2.0, Vec(1.0, 0, 8), Vec(), Vec(0.3, 0.3, 0.3), DIFF);
    VisibilityEngine e(both, ok, {0, 1}, 1);
    const auto q = e.query(Vec(0, 0, 0), Vec(0, 0, 1), Vec(0.5, 0.5, 0.5),
                           64, 64);
    TASK_CHECK(q.gradient.size() == 2);
  }
  VisibilityEngine eng(ss, ok, {0}, 1);
  TASK_CHECK(throwsInvalid([&] { eng.query(Vec(0, 0, 8), Vec(0, 0, 1),
                                           Vec(0.5, 0.5, 0.5), 16, 16); }));
  TASK_CHECK(throwsInvalid([&] { eng.query(Vec(0, 0, 0), Vec(0, 0, 2),
                                           Vec(0.5, 0.5, 0.5), 16, 16); }));
  TASK_CHECK(throwsInvalid([&] { eng.query(inf, Vec(0, 0, 1),
                                           Vec(0.5, 0.5, 0.5), 16, 16); }));
  TASK_CHECK(throwsInvalid([&] { eng.query(Vec(0, 0, 0), Vec(0, 0, 1),
                                           Vec(0.5, 0.5, 0.5), 0, 16); }));
  TASK_CHECK(throwsInvalid([&] { eng.query(Vec(0, 0, 0), Vec(0, 0, 1),
                                           Vec(0.5, 0.5, 0.5), 16, 0); }));
  TASK_CHECK(throwsInvalid([&] {
    VisibilityEngine e2(ss, ok, {0}, 1);
    e2.render(Camera{Vec(0, 0, 0), Vec(0, 0, 1), Vec(0, 1, 0), 180.0},
              8, 8, 16, 16); // fovY out of range
  }));
  TASK_CHECK(throwsInvalid([&] {
    VisibilityEngine e2(ss, ok, {0}, 1);
    e2.render(Camera{Vec(0, 0, 0), Vec(0, 0, 1), Vec(0, 0, 1), 50.0},
              8, 8, 16, 16); // viewUp parallel to forward
  }));
  TASK_CHECK(throwsInvalid([&] {
    VisibilityEngine e2(ss, ok, {0}, 1);
    e2.render(Camera{Vec(0, 0, 0), Vec(0, 0, 1), Vec(0, 1, 0), 50.0},
              0, 8, 16, 16); // non-positive width
  }));
  return 0;
}

// -------------------------------------------------------------------- render

int test_camera_light_faces() {
  RectLight light = makeLight(Vec(0, 0, 6), 2.5, 2.5, Vec(4, 5, 6));
  // Front face: narrow camera looking straight at the light fills the frame.
  VisibilityEngine eng({}, light, {}, 1);
  Camera front{Vec(0, 0, 1), Vec(0, 0, 1), Vec(0, 1, 0), 40};
  const auto img = eng.render(front, 12, 12, 32, 64);
  TASK_CHECK(img.width == 12 && img.height == 12);
  for (const Vec &c : img.color)
    TASK_CHECK(c.x == 4 && c.y == 5 && c.z == 6);

  // Back face: looking away from the emitter, nothing is hit -> black.
  Camera back{Vec(0, 0, 1), Vec(0, 0, -1), Vec(0, 1, 0), 40};
  const auto img2 = eng.render(back, 12, 12, 32, 64);
  for (const Vec &c : img2.color)
    TASK_CHECK(c.x == 0 && c.y == 0 && c.z == 0);
  return 0;
}

// Classify a camera ray the same way the engine does, for test expectations.
int classify(const std::vector<Sphere> &ss, const RectLight &light,
             const Vec &origin, const Vec &dir, int &sphereId) {
  double tSphere = 1e20;
  sphereId = -1;
  for (int i = 0; i < (int)ss.size(); ++i) {
    const double t = ss[i].intersect(Ray(origin, dir));
    if (t > 0 && t < tSphere) {
      tSphere = t;
      sphereId = i;
    }
  }
  const double denom = dir.dot(light.normal);
  bool lightHit = false;
  double tLight = 0.0;
  if (denom < -1e-9) {
    tLight = (light.center - origin).dot(light.normal) / denom;
    if (tLight > 1e-9) {
      const Vec hit = origin + dir * tLight;
      const Vec rel = hit - light.center;
      if (std::fabs(rel.dot(light.axis1)) <= light.half1 &&
          std::fabs(rel.dot(light.axis2)) <= light.half2)
        lightHit = true;
    }
  }
  if (lightHit && (sphereId < 0 || tLight < tSphere - slack(tSphere)))
    return 1; // light
  if (sphereId < 0) return 0;
  return 2;     // sphere
}

int test_camera_shading_and_gradients() {
  RectLight light = makeLight(Vec(0, 0, 6), 2.5, 2.5, Vec(4, 5, 6));
  std::vector<Sphere> ss;
  ss.emplace_back(100.0, Vec(0, 0, -100), Vec(), Vec(0.8, 0.2, 0.1), DIFF);
  ss.emplace_back(1.5, Vec(0, 0, 3), Vec(), Vec(0.3, 0.3, 0.3), DIFF);
  VisibilityEngine eng(ss, light, {1}, 555);
  const int N = 256;
  Camera cam{Vec(0, -7, 4), vnorm(Vec(0, 7, -1)), Vec(0, 0, 1), 50};
  const auto img = eng.render(cam, 16, 16, 256, N);
  TASK_CHECK(img.gradient.size() == 1);
  TASK_CHECK(img.gradient[0].size() == 256);

  const Vec fwd = vnorm(Vec(0, 7, -1));
  const Vec right = vnorm(vcross(Vec(0, 0, 1), fwd));
  const Vec up = vcross(fwd, right);
  const double tanHalf = std::tan(25.0 * PI / 180.0);
  int shaded = 0, occluderPixels = 0, litPixels = 0;
  for (int py = 0; py < 16; ++py)
    for (int px = 0; px < 16; ++px) {
      const int pixel = py * 16 + px;
      const double sx = (2 * (px + 0.5) / 16 - 1) * tanHalf;
      const double sy = (1 - 2 * (py + 0.5) / 16) * tanHalf;
      const Vec dir = vnorm(fwd + right * sx + up * sy);
      int id = -1;
      const int kind = classify(ss, light, cam.origin, dir, id);
      const auto &g = img.gradient[0][pixel];
      if (kind == 2 && id == 1) {
        ++occluderPixels;
        TASK_CHECK(img.color[pixel].x == 0 && img.color[pixel].y == 0 &&
                   img.color[pixel].z == 0);
        for (const Vec &v : g)
          TASK_CHECK(v.x == 0 && v.y == 0 && v.z == 0);
      } else if (kind == 2 && id == 0) {
        ++shaded;
        if (img.color[pixel].x > 0) {
          ++litPixels;
          double gmax = 0.0;
          for (const Vec &v : g)
            gmax = std::max(gmax, std::max({std::fabs(v.x), std::fabs(v.y),
                                            std::fabs(v.z)}));
          if (gmax > 0.0) {
            // Channel scaling: gradient channels share one geometric scalar.
            for (const Vec &v : g) {
              const double gr = v.x / (0.8 * 4.0);
              TASK_CHECK(std::fabs(v.y / (0.2 * 5.0) - gr) < 1e-12);
              TASK_CHECK(std::fabs(v.z / (0.1 * 6.0) - gr) < 1e-12);
            }
          }
        }
      }
    }
  TASK_CHECK(occluderPixels > 0 && shaded > 0 && litPixels > 0);
  // Each shaded pixel ran exactly N contour candidates, no per-axis rerender.
  TASK_CHECK(img.boundarySamples == (std::uint64_t)N * shaded);
  TASK_CHECK(img.arcsKept > 0 && img.arcsKept <= img.boundarySamples);
  TASK_CHECK(img.segmentQueries > 0);
  TASK_CHECK(eng.cumulativeBoundarySamples() == img.boundarySamples);
  return 0;
}

int test_instance_isolation_and_seeds() {
  RectLight light = makeLight(Vec(0, 0, 12), 4.0, 4.0, Vec(2, 3, 4));
  std::vector<Sphere> a;
  a.emplace_back(2.0, Vec(0, 0, 8), Vec(), Vec(0.4, 0.4, 0.4), DIFF);
  std::vector<Sphere> b = a;
  b[0].p = Vec(2, 0, 8);

  VisibilityEngine engA(a, light, {0}, 4242);
  VisibilityEngine engA2(a, light, {0}, 4242);
  VisibilityEngine engB(b, light, {0}, 4242);
  const Vec x(0, 0, 0), n(0, 0, 1), rho(0.5, 0.5, 0.5);
  const auto qA = engA.query(x, n, rho, 8192, 1024);
  const auto qA2 = engA2.query(x, n, rho, 8192, 1024);
  const auto qB = engB.query(x, n, rho, 8192, 1024);

  // Fixed seed reproduces bit-for-bit, including counters.
  TASK_CHECK(qA.color.x == qA2.color.x && qA.color.y == qA2.color.y &&
             qA.color.z == qA2.color.z);
  TASK_CHECK(qA.boundarySamples == qA2.boundarySamples &&
             qA.arcsKept == qA2.arcsKept &&
             qA.segmentQueries == qA2.segmentQueries);
  // Instance isolation: moving the sphere in engine B changes its answer.
  TASK_CHECK(!relClose(qA.color, qB.color, 0.05));
  TASK_CHECK(gradDistance(qA.gradient, qB.gradient) >
             0.05 * gradMaxComponent(qA.gradient));

  // Different seeds: colors stay unbiased and close; boundary gradients use
  // no RNG, so they match exactly.
  VisibilityEngine engA3(a, light, {0}, 9999);
  const auto qA3 = engA3.query(x, n, rho, 8192, 1024);
  TASK_CHECK(relClose(qA.color, qA3.color, 0.03));
  for (int ax = 0; ax < 3; ++ax)
    TASK_CHECK(relClose(qA.gradient[0][ax], qA3.gradient[0][ax], 1e-15));

  // Rendered images reproduce with the same seed.
  Camera cam{Vec(0, -7, 4), vnorm(Vec(0, 7, -1)), Vec(0, 0, 1), 50};
  VisibilityEngine r1(a, light, {0}, 7);
  VisibilityEngine r2(a, light, {0}, 7);
  const auto i1 = r1.render(cam, 10, 10, 64, 128);
  const auto i2 = r2.render(cam, 10, 10, 64, 128);
  TASK_CHECK(i1.color.size() == i2.color.size());
  for (size_t i = 0; i < i1.color.size(); ++i)
    TASK_CHECK(i1.color[i].x == i2.color[i].x &&
               i1.color[i].y == i2.color[i].y &&
               i1.color[i].z == i2.color[i].z);
  return 0;
}

// Legacy smallpt radiance / scene remain intact.

// smallpt's free intersect() is inlined away in legacy.o; reproduce its exact
// nearest-sphere loop over the extern spheres[9] scene.
bool legacyIntersect(const Ray &r, double &t, int &id) {
  double inf = t = 1e20;
  for (int i = 8; i >= 0; --i) {
    const double d = spheres[i].intersect(r);
    if (d && d < t) {
      t = d;
      id = i;
    }
  }
  return t < inf;
}

int test_legacy_radiance() {
  unsigned short Xi[3] = {17, 23, 41};
  const Vec up = radiance(Ray(Vec(50, 80, 81.6), Vec(0, 1, 0)), 0, Xi);
  TASK_CHECK(std::fabs(up.x - 12) < 1e-10 && std::fabs(up.y - 12) < 1e-10 &&
             std::fabs(up.z - 12) < 1e-10);
  Ray cam(Vec(50, 52, 295.6), Vec(0, -.042612, -1).norm());
  Vec cx(8 * .5135 / 6, 0, 0);
  Vec cy = (cx % cam.d).norm() * .5135;
  int hits = 0;
  Vec sum;
  for (int y = 0; y < 6; ++y)
    for (int px = 0; px < 8; ++px) {
      Vec d = (cx * ((px + .5) / 8 - .5) +
               cy * ((y + .5) / 6 - .5) + cam.d).norm();
      Ray r(cam.o + d * 140, d);
      double t;
      int id;
      if (legacyIntersect(r, t, id)) ++hits;
      for (int k = 0; k < 16; ++k) {
        const Vec c = radiance(r, 0, Xi);
        TASK_CHECK(std::isfinite(c.x) && std::isfinite(c.y) &&
                   std::isfinite(c.z));
        TASK_CHECK(c.x >= 0 && c.y >= 0 && c.z >= 0);
        sum = sum + c * (1.0 / 16.0);
      }
    }
  TASK_CHECK(hits == 48);
  TASK_CHECK(sum.x > 0 && sum.y > 0 && sum.z > 0);
  return 0;
}

} // namespace

const task_case task_cases[] = {
    {"radiometry", "unoccluded_rectangle_quadrature",
     test_unoccluded_quadrature},
    {"radiometry", "receiver_sphere_and_full_shadow",
     test_receiver_sphere_and_full_shadow},
    {"boundary", "interior_shadow_cap_analytic", test_interior_cap_analytic},
    {"boundary", "rectangle_clipped_arcs_fd", test_rectangle_clipped_arcs},
    {"boundary", "overlapping_spheres_exposed_arcs", test_overlapping_spheres},
    {"boundary", "behind_light_and_outside_cone",
     test_behind_light_and_outside_cone},
    {"boundary", "validation_rejects_illegal_inputs",
     test_validation_rejects_illegal_inputs},
    {"render", "camera_light_front_and_back", test_camera_light_faces},
    {"render", "camera_shading_and_gradients",
     test_camera_shading_and_gradients},
    {"render", "instance_isolation_and_seed_repeat",
     test_instance_isolation_and_seeds},
    {"render", "legacy_radiance_control", test_legacy_radiance},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
