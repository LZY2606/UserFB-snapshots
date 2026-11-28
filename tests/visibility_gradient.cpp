// tests/visibility_gradient.cpp
//
// Tests for the visibility_gradient entry point.  Registered through
// task_cases / task_case_count and driven by task_support/driver.c; there is
// no main() here.  Three groups are required: radiometry, boundary, render.
//
// Reference computations in this file are independent of the implementation
// under test:
//   * rectangle midpoint quadrature for the direct-light color;
//   * the analytic first moment of a spherical cap (small circle) for the
//     fully-interior shadow derivative;
//   * fine quadrature plus central differences, used only as a test
//     reference for truncated / overlapping / behind-light configurations.
#include "task_support/check.h"
#include "task_support/legacy.hpp"
#include "visibility_gradient.hpp"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

double vdot(const Vec& a, const Vec& b) { return a.dot(b); }
double vlen(const Vec& v) { return std::sqrt(vdot(v, v)); }
Vec vnorm(const Vec& v) { return v * (1.0 / vlen(v)); }
Vec vcross(const Vec& a, const Vec& b) {
  return Vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x);
}
bool finite(const Vec& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool close(double a, double b, double rel, double absTol) {
  return std::fabs(a - b) <= std::max(rel * std::max(std::fabs(a),
                                                      std::fabs(b)),
                                      absTol);
}
bool vecClose(const Vec& a, const Vec& b, double rel, double absTol) {
  return close(a.x, b.x, rel, absTol) && close(a.y, b.y, rel, absTol) &&
         close(a.z, b.z, rel, absTol);
}

// Builds an orthonormal rectangle frame around the given unit normal.
vg::RectLight makeLight(const Vec& center, const Vec& normal, double h1,
                        double h2, const Vec& emission) {
  vg::RectLight L;
  L.center = center;
  L.normal = vnorm(normal);
  Vec a1 = std::fabs(L.normal.x) > 0.9 ? Vec(0, 1, 0) : Vec(1, 0, 0);
  a1 = vnorm(a1 - L.normal * vdot(a1, L.normal));
  L.axis1 = a1;
  L.axis2 = vcross(L.normal, a1);
  L.half1 = h1;
  L.half2 = h2;
  L.emission = emission;
  return L;
}

// ---- Independent reference ----------------------------------------------

struct RefSphere {
  Vec center;
  double radius;
};

// Independent ray-sphere root solver; roots strictly inside (eps, tMax)
// block the segment.
bool refBlocked(const std::vector<RefSphere>& spheres, const Vec& o,
                const Vec& d, double tMax) {
  for (const RefSphere& s : spheres) {
    const Vec oc = o - s.center;
    const double b = vdot(oc, d);
    const double det = b * b - vdot(oc, oc) + s.radius * s.radius;
    if (det < 0.0) continue;
    const double q = std::sqrt(det);
    const double t1 = -b - q;
    const double t2 = -b + q;
    constexpr double eps = 1e-9;
    if ((t1 > eps && t1 < tMax - eps) ||
        (t2 > eps && t2 < tMax - eps))
      return true;
  }
  return false;
}

// Deterministic midpoint quadrature of the rectangle area integral.
Vec refColor(const std::vector<RefSphere>& spheres, const vg::RectLight& L,
             const Vec& x, const Vec& n, const Vec& rho, int grid) {
  Vec sum;
  const double cellArea =
      (4.0 * L.half1 * L.half2) / static_cast<double>(grid * grid);
  for (int i = 0; i < grid; ++i) {
    const double u = -L.half1 + (i + 0.5) * (2.0 * L.half1 / grid);
    for (int j = 0; j < grid; ++j) {
      const double v = -L.half2 + (j + 0.5) * (2.0 * L.half2 / grid);
      const Vec y = L.center + L.axis1 * u + L.axis2 * v;
      const Vec to = y - x;
      const double dist2 = vdot(to, to);
      const double dist = std::sqrt(dist2);
      const Vec w = to * (1.0 / dist);
      const double cosR = vdot(n, w);
      const double cosL = -vdot(L.normal, w);
      if (cosR <= 0.0 || cosL <= 0.0) continue;
      if (refBlocked(spheres, x, w, dist)) continue;
      const double g = cosR * cosL / dist2 * cellArea / kPi;
      sum = sum + L.emission.mult(rho) * g;
    }
  }
  return sum;
}

std::vector<RefSphere> shiftSphere(std::vector<RefSphere> spheres, int idx,
                                   const Vec& delta) {
  spheres[idx].center = spheres[idx].center + delta;
  return spheres;
}

// Central-difference reference gradient for one sphere / axis.
Vec fdGradient(const std::vector<RefSphere>& spheres, int idx, int axis,
               double h, const vg::RectLight& L, const Vec& x, const Vec& n,
               const Vec& rho, int grid) {
  Vec d(0, 0, 0);
  d = Vec(axis == 0 ? h : 0.0, axis == 1 ? h : 0.0, axis == 2 ? h : 0.0);
  const Vec cp = refColor(shiftSphere(spheres, idx, d), L, x, n, rho, grid);
  const Vec cm = refColor(shiftSphere(spheres, idx, d * -1.0), L, x, n, rho,
                          grid);
  return (cp - cm) * (1.0 / (2.0 * h));
}

// ---- Common scenes -------------------------------------------------------

const Vec kWhite(0.5, 0.5, 0.5);

// =====================================================================
// radiometry group
// =====================================================================

int case_no_occluder_matches_quadrature() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  const vg::RectLight L =
      makeLight(Vec(0, 0, 8), Vec(0, 0, -1), 3.0, 2.0, Vec(2.0, 3.0, 4.0));
  vg::VisibilityGradient vg0({}, L, {}, 1234, 20000, 64);
  const vg::PointResult pr = vg0.query(x, n, kWhite);
  const Vec ref = refColor({}, L, x, n, kWhite, 128);

  TASK_CHECK(vecClose(pr.color, ref, 0.015, 1e-6));
  // No specified spheres: no contour work at all.
  TASK_CHECK(pr.stats.boundarySamples == 0);
  TASK_CHECK(pr.stats.retainedArcs == 0);
  // Every area sample passes the cosine gates, so each one is queried.
  TASK_CHECK(pr.stats.segmentQueries == 20000);
  TASK_CHECK(pr.gradients.empty());
  return 0;
}

int case_color_scales_with_albedo_and_emission() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  const vg::RectLight L =
      makeLight(Vec(0, 0, 6), Vec(0, 0, -1), 2.0, 2.0, Vec(1, 2, 3));
  vg::VisibilityGradient a({}, L, {}, 7, 8000, 32);
  const Vec ca = a.query(x, n, Vec(0.8, 0.2, 0.1)).color;
  vg::VisibilityGradient b({}, L, {}, 7, 8000, 32);
  const Vec cb = b.query(x, n, Vec(0.4, 0.6, 0.3)).color;
  // Same random stream + linear integrand: channel ratios equal albedo
  // ratios because the geometry is identical.
  TASK_CHECK(close(ca.x / cb.x, 2.0, 0.02, 1e-9));
  TASK_CHECK(close(ca.y / cb.y, 1.0 / 3.0, 0.02, 1e-9));
  TASK_CHECK(close(ca.z / cb.z, 1.0 / 3.0, 0.02, 1e-9));
  TASK_CHECK(ca.x > 0 && ca.y > 0 && ca.z > 0);
  return 0;
}

int case_back_facing_light_is_black() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  // Light behind the receiver plane: every ray has a negative receiver
  // cosine, so the result must be zero with no segment queries.
  const vg::RectLight L =
      makeLight(Vec(0, 0, -5), Vec(0, 0, 1), 2.0, 2.0, Vec(2, 2, 2));
  vg::VisibilityGradient vg0({}, L, {}, 11, 1000, 32);
  const vg::PointResult pr = vg0.query(x, n, kWhite);
  TASK_CHECK(pr.color.x == 0.0 && pr.color.y == 0.0 && pr.color.z == 0.0);
  TASK_CHECK(pr.stats.segmentQueries == 0);
  return 0;
}

int case_validation_rejects_illegal_inputs() {
  const vg::RectLight good =
      makeLight(Vec(0, 0, 8), Vec(0, 0, -1), 2.0, 2.0, Vec(1, 1, 1));
  auto throwsArg = [](const std::vector<Sphere>& s, const vg::RectLight& L,
                      const std::vector<int>& idx, std::uint64_t seed,
                      int a, int b) {
    try {
      vg::VisibilityGradient v(s, L, idx, seed, a, b);
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  TASK_CHECK(throwsArg({}, good, {}, 1, 0, 8));
  TASK_CHECK(throwsArg({}, good, {}, 1, 8, 0));
  TASK_CHECK(throwsArg({}, good, {}, 1, -1, 8));

  vg::RectLight bad = good;
  bad.half1 = -1.0;
  TASK_CHECK(throwsArg({}, bad, {}, 1, 8, 8));
  bad = good;
  bad.emission = Vec(-1, 0, 0);
  TASK_CHECK(throwsArg({}, bad, {}, 1, 8, 8));
  bad = good;
  bad.axis2 = bad.axis1;  // no longer orthogonal
  TASK_CHECK(throwsArg({}, bad, {}, 1, 8, 8));
  bad = good;
  bad.normal = Vec(0, 0, 2);  // not unit length
  TASK_CHECK(throwsArg({}, bad, {}, 1, 8, 8));
  bad = good;
  bad.center = Vec(INFINITY, 0, 0);
  TASK_CHECK(throwsArg({}, bad, {}, 1, 8, 8));

  // Sphere crossing the light plane is illegal for a specified sphere.
  const Sphere crossing(3.0, Vec(0, 0, 7), Vec(), Vec(1, 1, 1), DIFF);
  TASK_CHECK(throwsArg({crossing}, good, {0}, 1, 8, 8));
  // Out-of-range / duplicate indices.
  const Sphere ok(1.0, Vec(0, 0, 4), Vec(), Vec(1, 1, 1), DIFF);
  TASK_CHECK(throwsArg({ok}, good, {3}, 1, 8, 8));
  TASK_CHECK(throwsArg({ok}, good, {0, 0}, 1, 8, 8));
  // Non-finite sphere.
  const Sphere nanS(1.0, Vec(NAN, 0, 4), Vec(), Vec(1, 1, 1), DIFF);
  TASK_CHECK(throwsArg({nanS}, good, {}, 1, 8, 8));

  vg::VisibilityGradient v({ok}, good, {0}, 1, 8, 8);
  bool threw = false;
  try {
    v.query(Vec(0, 0, 4), Vec(0, 0, 1), kWhite);  // sphere center: inside
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  threw = false;
  try {
    v.query(Vec(0, 0, 0), Vec(0, 0, 2), kWhite);  // non-unit normal
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  threw = false;
  try {
    v.render(vg::Camera{Vec(), Vec(0, 0, 1), Vec(1, 0, 0), Vec(0, 1, 0)}, 0,
             4);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  return 0;
}

// =====================================================================
// boundary group
// =====================================================================

// Shadow cone falls entirely inside the rectangle: derivatives must match
// the analytic first moment of the spherical cap,
//   C_blocked = rho Le R^2 (n.b)/d^3,
//   dC_color/db = rho Le R^2 [3(n.b)b/d^5 - n/d^3].
int case_cap_first_moment() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  const vg::RectLight L =
      makeLight(Vec(0, 0, 20), Vec(0, 0, -1), 25.0, 25.0, Vec(2, 3, 4));
  const double R = 2.0;

  // Centered sphere: transverse derivatives vanish, dC/dz = 2 rho Le R^2/D^3.
  {
    const double D = 8.0;
    const Sphere s(R, Vec(0, 0, D), Vec(), kWhite, DIFF);
    vg::VisibilityGradient v({s}, L, {0}, 99, 4096, 4096);
    const vg::PointResult pr = v.query(x, n, kWhite);
    TASK_CHECK(pr.gradients.size() == 1);
    const std::array<Vec, 3>& g = pr.gradients[0];
    const Vec expected = Vec(2.0, 3.0, 4.0) * (2.0 * 0.5 * R * R /
                                               (D * D * D));
    TASK_CHECK(vecClose(g[2], expected, 0.01, 1e-7));
    TASK_CHECK(std::fabs(g[0].x) < 0.01 * std::fabs(expected.x) + 1e-9);
    TASK_CHECK(std::fabs(g[1].y) < 0.01 * std::fabs(expected.y) + 1e-9);
    // Every candidate survives: cap is interior, unoccluded and front facing.
    TASK_CHECK(pr.stats.retainedArcs == 4096);
    TASK_CHECK(pr.stats.boundarySamples == 4096);
    TASK_CHECK(pr.stats.segmentQueries == 4096 + 4096);
    // The blocked energy equals unblocked minus shaded color.  Check it with
    // the independent deterministic rectangle quadrature rather than the
    // high-variance area-MC color (the shadow disk covers only a few percent
    // of this large light).
    const std::vector<RefSphere> rs = {{Vec(0, 0, D), R}};
    const Vec fullQuad = refColor({}, L, x, n, kWhite, 256);
    const Vec shadeQuad = refColor(rs, L, x, n, kWhite, 256);
    const Vec blocked = fullQuad - shadeQuad;
    const Vec blockedExpected =
        Vec(2.0, 3.0, 4.0) * (0.5 * R * R / (D * D));
    TASK_CHECK(vecClose(blocked, blockedExpected, 0.01, 1e-6));
    // The MC color must at least be finite and darker than the unblocked one.
    TASK_CHECK(pr.color.x < fullQuad.x && pr.color.y < fullQuad.y &&
               pr.color.z < fullQuad.z);
  }

  // Offset sphere: all three analytic components are nonzero.
  {
    const Vec c(2.5, -1.5, 8.0);
    const Sphere s(R, c, Vec(), kWhite, DIFF);
    vg::VisibilityGradient v({s}, L, {0}, 99, 4096, 4096);
    const vg::PointResult pr = v.query(x, n, kWhite);
    const Vec b = c - x;
    const double d2 = vdot(b, b);
    const double d5 = d2 * d2 * std::sqrt(d2);
    // dC/db_j = rho R^2 Le * [3 (n.b) b_j / d^5 - n_j / d^3], n = +z.
    const Vec le(2.0, 3.0, 4.0);
    const double k = 0.5 * R * R;
    const Vec expectedX = le * (k * 3.0 * b.z * b.x / d5);
    const Vec expectedY = le * (k * 3.0 * b.z * b.y / d5);
    const Vec expectedZ =
        le * (k * (3.0 * b.z * b.z - d2) / d5);
    const std::array<Vec, 3>& g = pr.gradients[0];
    TASK_CHECK(vecClose(g[0], expectedX, 0.02, 1e-7));
    TASK_CHECK(vecClose(g[1], expectedY, 0.02, 1e-7));
    TASK_CHECK(vecClose(g[2], expectedZ, 0.02, 1e-7));
    TASK_CHECK(pr.stats.retainedArcs == 4096);
  }
  return 0;
}

// Cone clipped by the rectangle boundary: compare against fine quadrature
// central differences and report step convergence.
int case_rectangle_truncated_arcs() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  const vg::RectLight L =
      makeLight(Vec(0, 0, 10), Vec(0, 0, -1), 2.0, 2.0, Vec(1.5, 2.0, 2.5));
  const Vec c(1.2, 0.0, 4.0);
  const double R = 1.5;
  const std::vector<RefSphere> ref = {{c, R}};
  const Sphere s(R, c, Vec(), kWhite, DIFF);
  vg::VisibilityGradient v({s}, L, {0}, 4242, 2048, 2048);
  const vg::PointResult pr = v.query(x, n, kWhite);

  // The rectangle gate must actually remove arcs while retaining some.
  TASK_CHECK(pr.stats.retainedArcs > 0);
  TASK_CHECK(pr.stats.retainedArcs < pr.stats.boundarySamples);
  TASK_CHECK(pr.stats.boundarySamples == 2048);

  // High-accuracy independent quadrature.  Coarse grids alias against the
  // moving shadow boundary, so grid 600 is used for the reference and grid
  // 300 measures the reference's own sampling error.
  constexpr int kFine = 600;
  const double hs[4] = {0.4, 0.2, 0.1, 0.05};
  std::cout << "[rect-trunc] central-difference convergence (grid=" << kFine
            << "):\n";
  Vec gz[4], gx[4];
  for (int k = 0; k < 4; ++k) {
    gz[k] = fdGradient(ref, 0, 2, hs[k], L, x, n, kWhite, kFine);
    gx[k] = fdGradient(ref, 0, 0, hs[k], L, x, n, kWhite, kFine);
    std::cout << "  h=" << std::setw(5) << hs[k]
              << "  dC/dz=" << std::setprecision(6) << gz[k].x
              << "  dC/dx=" << gx[k].x << "\n";
  }
  // Step convergence: variation within the fine step pair is smaller than
  // within the coarse pair, on both axes.
  const double coarseJump =
      vlen(gz[1] - gz[0]) + vlen(gx[1] - gx[0]);
  const double fineJump =
      vlen(gz[3] - gz[2]) + vlen(gx[3] - gx[2]);
  std::cout << "[rect-trunc] coarse-pair variation=" << coarseJump
            << " fine-pair variation=" << fineJump << "\n";
  TASK_CHECK(fineJump < coarseJump);

  // Reference sampling error between two independent quadrature grids.
  const Vec gzCoarseGrid =
      fdGradient(ref, 0, 2, 0.1, L, x, n, kWhite, 300);
  const Vec gxCoarseGrid =
      fdGradient(ref, 0, 0, 0.1, L, x, n, kWhite, 300);
  std::cout << "[rect-trunc] quadrature sampling error (grid 300 vs "
            << kFine << "): dz=" << vlen(gzCoarseGrid - gz[2])
            << " dx=" << vlen(gxCoarseGrid - gx[2]) << "\n";

  // Boundary-integral sampling error.  The contour integral is a
  // deterministic uniform-phi quadrature, so its sampling error is measured
  // by refinement rather than by a new seed.
  vg::VisibilityGradient vFine({s}, L, {0}, 4242, 2048, 8192);
  const vg::PointResult prFine = vFine.query(x, n, kWhite);
  const Vec sampleErr = prFine.gradients[0][2] - pr.gradients[0][2];
  std::cout << "[rect-trunc] contour refinement error (2048 -> 8192 arcs): "
            << sampleErr.x << "," << sampleErr.y << "," << sampleErr.z
            << "\n";

  TASK_CHECK(vecClose(pr.gradients[0][2], gz[3], 0.05, 2e-5));
  TASK_CHECK(vecClose(pr.gradients[0][0], gx[3], 0.05, 2e-5));
  TASK_CHECK(vlen(sampleErr) < 0.05 * vlen(pr.gradients[0][2]));
  return 0;
}

// Two overlapping opaque spheres: only exposed silhouette arcs contribute.
// Pushing one sphere toward the other darkens the receiver (negative
// derivative), the reverse for the other sphere.
int case_overlapping_spheres() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  const vg::RectLight L =
      makeLight(Vec(0, 0, 12), Vec(0, 0, -1), 8.0, 8.0, Vec(1.0, 1.0, 1.0));
  const Vec ca(-0.8, 0, 5.0), cb(0.8, 0, 5.0);
  const double R = 1.5;
  const std::vector<RefSphere> ref = {{ca, R}, {cb, R}};
  const Sphere sa(R, ca, Vec(), kWhite, DIFF);
  const Sphere sb(R, cb, Vec(), kWhite, DIFF);
  vg::VisibilityGradient v({sa, sb}, L, {0, 1}, 77, 2048, 2048);
  const vg::PointResult pr = v.query(x, n, kWhite);

  TASK_CHECK(pr.gradients.size() == 2);
  TASK_CHECK(pr.stats.boundarySamples == 2 * 2048);
  TASK_CHECK(pr.stats.retainedArcs > 0);
  TASK_CHECK(pr.stats.retainedArcs < pr.stats.boundarySamples);

  // Signed contributions: sphere A moving +x pulls its exposed left
  // silhouette inward, shrinking the union shadow (positive derivative);
  // sphere B moving +x extends its right silhouette outward (negative).
  TASK_CHECK(pr.gradients[0][0].x > 0.0);
  TASK_CHECK(pr.gradients[1][0].x < 0.0);
  // Mirror symmetry: the two transverse derivatives are opposites.
  TASK_CHECK(close(pr.gradients[0][0].x, -pr.gradients[1][0].x, 1e-9, 1e-12));
  // Both spheres receding along +z shrink the shadow equally.
  TASK_CHECK(close(pr.gradients[0][2].x, pr.gradients[1][2].x, 1e-9, 1e-12));

  std::cout << "[overlap] central-difference convergence (grid=600):\n";
  constexpr int kFine = 600;
  const double hs[4] = {0.4, 0.2, 0.1, 0.05};
  Vec ga[4], gb[4];
  for (int k = 0; k < 4; ++k) {
    ga[k] = fdGradient(ref, 0, 0, hs[k], L, x, n, kWhite, kFine);
    gb[k] = fdGradient(ref, 1, 0, hs[k], L, x, n, kWhite, kFine);
    std::cout << "  h=" << hs[k] << "  dCA/dx=" << std::setprecision(6)
              << ga[k].x << "  dCB/dx=" << gb[k].x << "\n";
  }
  const double coarseJump =
      std::fabs(ga[1].x - ga[0].x) + std::fabs(gb[1].x - gb[0].x);
  const double fineJump =
      std::fabs(ga[3].x - ga[2].x) + std::fabs(gb[3].x - gb[2].x);
  std::cout << "[overlap] coarse-pair variation=" << coarseJump
            << " fine-pair variation=" << fineJump << "\n";
  TASK_CHECK(fineJump < coarseJump);

  // Reference sampling error between quadrature grids.
  const Vec gaCoarse =
      fdGradient(ref, 0, 0, 0.1, L, x, n, kWhite, 300);
  std::cout << "[overlap] quadrature sampling error (grid 300 vs " << kFine
            << "): dA=" << std::fabs(gaCoarse.x - ga[2].x) << "\n";

  const Vec fdA = fdGradient(ref, 0, 0, 0.05, L, x, n, kWhite, kFine);
  const Vec fdB = fdGradient(ref, 1, 0, 0.05, L, x, n, kWhite, kFine);
  TASK_CHECK(close(pr.gradients[0][0].x, fdA.x, 0.05, 2e-5));
  TASK_CHECK(close(pr.gradients[1][0].x, fdB.x, 0.05, 2e-5));

  // A small sphere hidden entirely behind a larger one keeps no exposed
  // arc: its gradient is exactly zero.
  const Sphere big(2.0, Vec(0, 0, 6.0), Vec(), kWhite, DIFF);
  const Sphere small(0.5, Vec(0, 0, 6.3), Vec(), kWhite, DIFF);
  vg::VisibilityGradient hidden({big, small}, L, {0, 1}, 77, 1024, 1024);
  const vg::PointResult ph = hidden.query(x, n, kWhite);
  TASK_CHECK(ph.gradients[1][0].x == 0.0 &&
             ph.gradients[1][1].y == 0.0 &&
             ph.gradients[1][2].z == 0.0);
  TASK_CHECK(ph.stats.retainedArcs > 0);  // the big sphere still contributes
  return 0;
}

// Sphere wholly on the far side of the light: every tangent ray reaches the
// light plane first, so no arc survives and the color is unchanged.
int case_sphere_behind_light() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  const vg::RectLight L =
      makeLight(Vec(0, 0, 10), Vec(0, 0, -1), 8.0, 8.0, Vec(1, 1, 1));
  const Sphere behind(2.0, Vec(0, 0, 14.0), Vec(), kWhite, DIFF);
  vg::VisibilityGradient v({behind}, L, {0}, 5, 1024, 1024);
  const vg::PointResult pr = v.query(x, n, kWhite);
  TASK_CHECK(pr.stats.retainedArcs == 0);
  TASK_CHECK(pr.stats.boundarySamples == 1024);
  TASK_CHECK(pr.gradients[0][0].x == 0.0 &&
             pr.gradients[0][1].y == 0.0 &&
             pr.gradients[0][2].z == 0.0);
  const Vec ref = refColor({}, L, x, n, kWhite, 96);
  TASK_CHECK(vecClose(pr.color, ref, 0.02, 1e-6));

  const std::vector<RefSphere> rs = {{Vec(0, 0, 14.0), 2.0}};
  const Vec fd = fdGradient(rs, 0, 2, 0.1, L, x, n, kWhite, 128);
  TASK_CHECK(vlen(fd) < 1e-10);
  return 0;
}

// =====================================================================
// render group
// =====================================================================

vg::RectLight renderLight() {
  // Overhead light pointing down at the ground receiver, well clear of the
  // camera's primary rays.
  const Vec center(0, 10, 6);
  return makeLight(center, Vec(0, -1, 0), 6.0, 5.0, Vec(6, 6, 6));
}

// Large sphere acting as a y=0 ground plane (smallpt convention), plus a
// small opaque occluder floating above it.
std::vector<Sphere> renderSpheres(Vec albedo) {
  return {
      Sphere(1e5, Vec(0, -1e5, 0), Vec(), albedo, DIFF),  // ground receiver
      Sphere(2.0, Vec(0, 2.5, 6), Vec(), Vec(0.9, 0.9, 0.9),
             DIFF)  // occluder, bottom at y=0.5 above the ground
  };
}

vg::Camera renderCamera() {
  const Vec f = vnorm(Vec(0, -8, 12));
  const Vec right(1, 0, 0);
  const Vec up = vnorm(vcross(f, right));
  return vg::Camera{Vec(0, 8, -12), f, right * 0.55, up * 0.45};
}

int case_small_image_color_and_gradient() {
  const vg::RectLight L = renderLight();
  vg::VisibilityGradient v(renderSpheres(kWhite), L, {1}, 2024, 1500, 512);
  const vg::Camera cam = renderCamera();
  const vg::Image img = v.render(cam, 16, 12);
  TASK_CHECK(img.width == 16 && img.height == 12);
  TASK_CHECK(static_cast<int>(img.color.size()) == 16 * 12);
  TASK_CHECK(static_cast<int>(img.gradient.size()) == 16 * 12);

  int shaded = 0, lit = 0, gradPixels = 0, blackOccluder = 0;
  for (int j = 0; j < 12; ++j) {
    for (int i = 0; i < 16; ++i) {
      const std::size_t p = static_cast<std::size_t>(j) * 16 + i;
      TASK_CHECK(finite(img.color[p]));
      for (int a = 0; a < 3; ++a) TASK_CHECK(finite(img.gradient[p][a]));
      if (img.color[p].x > 0 || img.color[p].y > 0 || img.color[p].z > 0)
        ++lit;
      if (vlen(img.gradient[p][0]) + vlen(img.gradient[p][1]) +
              vlen(img.gradient[p][2]) >
          0.0)
        ++gradPixels;
    }
  }
  // Independently classify primary rays to count shaded / occluder pixels.
  for (int j = 0; j < 12; ++j) {
    for (int i = 0; i < 16; ++i) {
      Vec d = cam.forward +
              cam.right * ((i + 0.5) / 16.0 - 0.5) +
              cam.up * ((j + 0.5) / 12.0 - 0.5);
      const Ray ray(cam.origin, vnorm(d));
      double tBest = 1e20;
      int id = -1;
      for (std::size_t s = 0; s < v.spheres().size(); ++s) {
        const double t = v.spheres()[s].intersect(ray);
        if (t > 0 && t < tBest) {
          tBest = t;
          id = static_cast<int>(s);
        }
      }
      const std::size_t p = static_cast<std::size_t>(j) * 16 + i;
      if (id == 1) {
        ++blackOccluder;
        TASK_CHECK(img.color[p].x == 0.0 && img.color[p].y == 0.0 &&
                   img.color[p].z == 0.0);
        TASK_CHECK(vlen(img.gradient[p][0]) == 0.0 &&
                   vlen(img.gradient[p][1]) == 0.0 &&
                   vlen(img.gradient[p][2]) == 0.0);
      } else if (id == 0) {
        ++shaded;
      }
    }
  }
  TASK_CHECK(lit > 0);
  TASK_CHECK(gradPixels > 0);
  TASK_CHECK(blackOccluder > 0);
  TASK_CHECK(shaded > 0);
  // One fixed contour budget per shaded pixel: no per-axis re-render.
  TASK_CHECK(img.stats.boundarySamples ==
            static_cast<unsigned long long>(shaded) * 512);
  TASK_CHECK(img.stats.segmentQueries >= img.stats.boundarySamples);
  return 0;
}

int case_light_front_and_back_face() {
  const vg::RectLight L =
      makeLight(Vec(0, 0, 10), Vec(0, 0, -1), 4.0, 4.0, Vec(3, 4, 5));
  vg::VisibilityGradient v({}, L, {}, 1, 64, 16);
  // Camera in front of the emitting face.
  const vg::Image front =
      v.render(vg::Camera{Vec(0, 0, 0), Vec(0, 0, 1), Vec(0.2, 0, 0),
                          Vec(0, 0.2, 0)},
               4, 4);
  int emitting = 0;
  for (const Vec& c : front.color)
    if (vecClose(c, L.emission, 1e-12, 0.0)) ++emitting;
  TASK_CHECK(emitting == 16);
  // Camera behind the rectangle sees the back face: zero everywhere.
  const vg::Image back =
      v.render(vg::Camera{Vec(0, 0, 20), Vec(0, 0, -1), Vec(0.2, 0, 0),
                          Vec(0, 0.2, 0)},
               4, 4);
  for (const Vec& c : back.color)
    TASK_CHECK(c.x == 0.0 && c.y == 0.0 && c.z == 0.0);
  return 0;
}

int case_colored_albedo_gradient_channels() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  const vg::RectLight L =
      makeLight(Vec(0, 0, 10), Vec(0, 0, -1), 6.0, 6.0, Vec(2, 2, 2));
  const Sphere s(1.5, Vec(1.0, 0, 4.0), Vec(), Vec(0.8, 0.2, 0.1), DIFF);
  vg::VisibilityGradient v({s}, L, {0}, 3, 2048, 2048);
  const vg::PointResult pr = v.query(x, n, Vec(0.8, 0.2, 0.1));
  const Vec& gz = pr.gradients[0][2];
  TASK_CHECK(gz.x > 0 && gz.y > 0 && gz.z > 0);
  // White light: channel ratios of the derivative equal albedo ratios.
  TASK_CHECK(close(gz.y / gz.x, 0.25, 0.02, 1e-12));
  TASK_CHECK(close(gz.z / gz.x, 0.125, 0.02, 1e-12));
  TASK_CHECK(close(pr.color.y / pr.color.x, 0.25, 0.03, 1e-12));
  return 0;
}

int case_instance_isolation() {
  const Vec x(0, 0, 0), n(0, 0, 1);
  const vg::RectLight L =
      makeLight(Vec(0, 0, 9), Vec(0, 0, -1), 5.0, 5.0, Vec(1, 1, 1));
  std::vector<Sphere> withOcc = {
      Sphere(1.5, Vec(0, 0, 4), Vec(), kWhite, DIFF)};
  vg::VisibilityGradient a(withOcc, L, {0}, 11, 4000, 256);
  vg::VisibilityGradient b({}, L, {}, 11, 4000, 256);
  const vg::PointResult pa = a.query(x, n, kWhite);
  const vg::PointResult pb = b.query(x, n, kWhite);
  TASK_CHECK(pa.color.x < pb.color.x);  // the occluder darkens the point
  TASK_CHECK(vlen(pa.gradients[0][2]) > 0.0);
  TASK_CHECK(pb.gradients.empty());
  // Mutating the caller's scene after construction cannot affect the
  // instance: it holds its own copy.  Reconstruct with the same seed so the
  // random stream is identical and the colors match exactly.
  withOcc[0] = Sphere(9.0, Vec(0, 0, 2), Vec(), kWhite, DIFF);
  vg::VisibilityGradient a2(std::vector<Sphere>{
                               Sphere(1.5, Vec(0, 0, 4), Vec(), kWhite, DIFF)},
                           L, {0}, 11, 4000, 256);
  const vg::PointResult pa2 = a2.query(x, n, kWhite);
  TASK_CHECK(vecClose(pa2.color, pa.color, 1e-12, 0.0));
  TASK_CHECK(a.spheres().size() == 1 && a.spheres()[0].rad == 1.5);
  TASK_CHECK(a2.spheres().size() == 1 && a2.spheres()[0].rad == 1.5);
  return 0;
}

int case_fixed_seed_is_reproducible() {
  const vg::RectLight L = renderLight();
  auto build = [&](std::uint64_t seed) {
    return vg::VisibilityGradient(renderSpheres(kWhite), L, {1}, seed, 1200,
                                  256);
  };
  // Ground receiver point with an upward normal, under the overhead light.
  const Vec x(0, 0, 6), n(0, 1, 0);
  vg::PointResult q1 = build(777).query(x, n, kWhite);
  vg::PointResult q2 = build(777).query(x, n, kWhite);
  TASK_CHECK(q1.color.x == q2.color.x && q1.color.y == q2.color.y &&
             q1.color.z == q2.color.z);
  for (std::size_t k = 0; k < q1.gradients.size(); ++k)
    for (int a = 0; a < 3; ++a)
      TASK_CHECK(q1.gradients[k][a].x == q2.gradients[k][a].x &&
                 q1.gradients[k][a].y == q2.gradients[k][a].y &&
                 q1.gradients[k][a].z == q2.gradients[k][a].z);

  const vg::Camera cam = renderCamera();
  const vg::Image i1 = build(777).render(cam, 10, 8);
  const vg::Image i2 = build(777).render(cam, 10, 8);
  TASK_CHECK(i1.color.size() == i2.color.size());
  for (std::size_t p = 0; p < i1.color.size(); ++p)
    TASK_CHECK(i1.color[p].x == i2.color[p].x &&
               i1.color[p].y == i2.color[p].y &&
               i1.color[p].z == i2.color[p].z);

  // A different seed is an independent random stream (results stay finite
  // and in the same ballpark, but the streams differ).
  vg::PointResult q3 = build(778).query(x, n, kWhite);
  TASK_CHECK(finite(q3.color) && vecClose(q3.color, q1.color, 0.15, 1e-6));
  return 0;
}

int case_legacy_radiance_control() {
  // The original smallpt entry point and scene are preserved and still
  // behave as in task_support/baseline.cpp.
  unsigned short Xi[3] = {17, 23, 41};
  const Vec up = radiance(Ray(Vec(50, 80, 81.6), Vec(0, 1, 0)), 0, Xi);
  TASK_CHECK(std::fabs(up.x - 12.0) < 1e-10 &&
             std::fabs(up.y - 12.0) < 1e-10 &&
             std::fabs(up.z - 12.0) < 1e-10);
  const Sphere unit(1, Vec(), Vec(), Vec(.7, .6, .5), DIFF);
  TASK_CHECK(std::fabs(unit.intersect(Ray(Vec(0, 0, 3),
                                          Vec(0, 0, -1))) -
                       2.0) < 1e-12);
  TASK_CHECK(unit.intersect(Ray(Vec(0, 0, 3), Vec(0, 1, 0))) == 0.0);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"radiometry", "no_occluder_matches_quadrature",
     case_no_occluder_matches_quadrature},
    {"radiometry", "color_scales_with_albedo_and_emission",
     case_color_scales_with_albedo_and_emission},
    {"radiometry", "back_facing_light_is_black",
     case_back_facing_light_is_black},
    {"radiometry", "validation_rejects_illegal_inputs",
     case_validation_rejects_illegal_inputs},
    {"boundary", "cap_first_moment", case_cap_first_moment},
    {"boundary", "rectangle_truncated_arcs",
     case_rectangle_truncated_arcs},
    {"boundary", "overlapping_spheres", case_overlapping_spheres},
    {"boundary", "sphere_behind_light", case_sphere_behind_light},
    {"render", "small_image_color_and_gradient",
     case_small_image_color_and_gradient},
    {"render", "light_front_and_back_face",
     case_light_front_and_back_face},
    {"render", "colored_albedo_gradient_channels",
     case_colored_albedo_gradient_channels},
    {"render", "instance_isolation", case_instance_isolation},
    {"render", "fixed_seed_reproducible", case_fixed_seed_is_reproducible},
    {"render", "legacy_radiance_control", case_legacy_radiance_control},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
