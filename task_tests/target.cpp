// Task tests for the round-tube (swept-sphere) curve primitive.
//
// Groups:
//   envelope  - varying radius, constant centerline, conservative BVH bounds
//   intervals - self overlap, interior start, tangency, finite windows
//   traversal - multi-curve BVH nearest hit, prim_ids_range, cull, reuse
//   render    - real --round-tubes P3 image + legacy triangle path
//
// Independent reference: for varying-radius / curved material the expected
// values come from ref::intervals(), a brute-force boundary finder that is
// completely separate from the production envelope-polynomial solver:
//   m(t) = min_u (|O + tD - C(u)|^2 - r(u)^2)
// is evaluated by a dense u scan (1024 samples) plus golden-section
// refinement of every local-minimum bracket, and the t intervals with
// m(t) <= 0 are found by a dense t scan (8000 samples) plus 80 bisection
// steps per sign change.  Convergence: m is continuous (min of continuous
// functions), so bisection on a sign-changing bracket converges to a true
// boundary crossing; the test geometries have boundary features orders of
// magnitude wider than the scan pitch, so no interval can be skipped.
// Straight constant-radius material is checked against the closed-form
// analytic capsule instead.

#include "registry.hpp"

#define main curves_example_main
#include "examples/curves_primitive/main.cc"
#undef main

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Independent high-precision reference (see header comment).
namespace ref {

struct Curve {
  double P[4][3];
  double r[4];
};

void evalCR(const Curve &cv, double u, double *C, double *rr) {
  double v = 1.0 - u;
  double b[4] = {v * v * v, 3 * v * v * u, 3 * v * u * u, u * u * u};
  for (int k = 0; k < 3; k++) C[k] = 0.0;
  *rr = 0.0;
  for (int i = 0; i < 4; i++) {
    for (int k = 0; k < 3; k++) C[k] += b[i] * cv.P[i][k];
    *rr += b[i] * cv.r[i];
  }
}

double h(const Curve &cv, const double *O, const double *D, double t,
         double u) {
  double C[3], r;
  evalCR(cv, u, C, &r);
  double s = 0.0;
  for (int k = 0; k < 3; k++) {
    double d = O[k] + t * D[k] - C[k];
    s += d * d;
  }
  return s - r * r;
}

// m(t) = min_u h(t,u): dense scan + golden-section refinement of every
// local-minimum bracket (and both endpoints).
double minH(const Curve &cv, const double *O, const double *D, double t) {
  const int N = 1024;
  double best = std::min(h(cv, O, D, t, 0.0), h(cv, O, D, t, 1.0));
  for (int i = 1; i < N; i++) {
    double um = double(i - 1) / N, u0 = double(i) / N, up = double(i + 1) / N;
    double v0 = h(cv, O, D, t, u0);
    if (v0 <= h(cv, O, D, t, um) && v0 <= h(cv, O, D, t, up)) {
      double a = um, b = up;
      const double gr = (std::sqrt(5.0) - 1.0) / 2.0;
      double c = b - gr * (b - a), d = a + gr * (b - a);
      double fc = h(cv, O, D, t, c), fd = h(cv, O, D, t, d);
      for (int it = 0; it < 80; it++) {
        if (fc < fd) {
          b = d; d = c; fd = fc; c = b - gr * (b - a); fc = h(cv, O, D, t, c);
        } else {
          a = c; c = d; fc = fd; d = a + gr * (b - a); fd = h(cv, O, D, t, d);
        }
      }
      best = std::min(best, h(cv, O, D, t, 0.5 * (a + b)));
    }
  }
  return best;
}

double bisectBoundary(const Curve &cv, const double *O, const double *D,
                      double lo, double hi, bool inside_at_hi) {
  for (int it = 0; it < 80; it++) {
    double m = 0.5 * (lo + hi);
    bool in = minH(cv, O, D, m) <= 0.0;
    if (in == inside_at_hi) hi = m; else lo = m;
  }
  return 0.5 * (lo + hi);
}

// All t-intervals in [t0, t1] where the ray is inside the union.
std::vector<std::pair<double, double> > intervals(const Curve &cv,
                                                  const double *O,
                                                  const double *D, double t0,
                                                  double t1) {
  const int N = 8000;
  std::vector<std::pair<double, double> > out;
  bool prev_in = minH(cv, O, D, t0) <= 0.0;
  double prev_t = t0, start = t0;
  for (int i = 1; i <= N; i++) {
    double t = t0 + (t1 - t0) * double(i) / N;
    bool in = minH(cv, O, D, t) <= 0.0;
    if (in && !prev_in) start = bisectBoundary(cv, O, D, prev_t, t, true);
    if (!in && prev_in)
      out.push_back(std::make_pair(start, bisectBoundary(cv, O, D, prev_t, t, false)));
    prev_in = in;
    prev_t = t;
  }
  if (prev_in) out.push_back(std::make_pair(start, t1));
  return out;
}

}  // namespace ref

// ---------------------------------------------------------------------------
// Scene helpers (production queries always go through the BVH).

template <typename T>
struct Scene {
  std::vector<T> verts;
  std::vector<T> radii;
  nanort::BVHAccel<T> accel;

  bool build() {
    round_tube::Geometry<T> geom(verts.data(), radii.data());
    round_tube::Pred<T> pred(verts.data());
    return accel.Build((unsigned int)(radii.size() / 4), geom, pred);
  }

  bool trace(const T *org, const T *dir, T min_t, T max_t,
             round_tube::Intersection<T> *out,
             const nanort::BVHTraceOptions *opt = 0) const {
    round_tube::Intersector<T> isector(verts.data(), radii.data());
    nanort::Ray<T> ray;
    for (int k = 0; k < 3; k++) {
      ray.org[k] = org[k];
      ray.dir[k] = dir[k];
    }
    ray.min_t = min_t;
    ray.max_t = max_t;
    nanort::BVHTraceOptions o;
    if (opt) o = *opt;
    return accel.Traverse(ray, isector, out, o);
  }
};

template <typename T>
void setCurve(Scene<T> *sc, unsigned idx, const double *P12, const double *R4) {
  for (int i = 0; i < 12; i++) sc->verts[12 * idx + i] = (T)P12[i];
  for (int i = 0; i < 4; i++) sc->radii[4 * idx + i] = (T)R4[i];
}

template <typename T>
Scene<T> *singleCurve(const double *P12, const double *R4) {
  Scene<T> *sc = new Scene<T>();
  sc->verts.resize(12);
  sc->radii.resize(4);
  setCurve(sc, 0, P12, R4);
  sc->build();
  return sc;
}

// Evaluate centerline/radius of a double scene curve (for u back-substitution
// checks).
void evalCurveCR(const std::vector<double> &verts,
                 const std::vector<double> &radii, unsigned idx, double u,
                 double *C, double *r) {
  double v = 1.0 - u;
  double b[4] = {v * v * v, 3 * v * v * u, 3 * v * u * u, u * u * u};
  for (int k = 0; k < 3; k++) C[k] = 0.0;
  *r = 0.0;
  for (int i = 0; i < 4; i++) {
    for (int k = 0; k < 3; k++) C[k] += b[i] * verts[12 * idx + 3 * i + k];
    *r += b[i] * radii[4 * idx + i];
  }
}

// u back-substitution: the reported (position, u) must satisfy
// |position - C(u)| = r(u), i.e. the point lies on the sphere of parameter u.
bool checkBackSubst(const Scene<double> &sc,
                    const round_tube::Intersection<double> &hit, double tol) {
  double C[3], r;
  evalCurveCR(sc.verts, sc.radii, hit.prim_id, hit.u, C, &r);
  double d2 = 0.0;
  for (int k = 0; k < 3; k++) {
    double d = hit.position[k] - C[k];
    d2 += d * d;
  }
  return std::fabs(std::sqrt(d2) - r) <= tol;
}

bool checkNormal(const Scene<double> &sc,
                 const round_tube::Intersection<double> &hit, double tol) {
  double C[3], r;
  evalCurveCR(sc.verts, sc.radii, hit.prim_id, hit.u, C, &r);
  double n[3], len = 0.0;
  for (int k = 0; k < 3; k++) {
    n[k] = hit.position[k] - C[k];
    len += n[k] * n[k];
  }
  len = std::sqrt(len);
  if (len <= 0.0) return false;
  double err = 0.0, unit = 0.0;
  for (int k = 0; k < 3; k++) {
    err = std::max(err, std::fabs(hit.normal[k] - n[k] / len));
    unit += hit.normal[k] * hit.normal[k];
  }
  return err <= tol && std::fabs(std::sqrt(unit) - 1.0) <= tol;
}

double vlen3(const double *a, const double *b) {
  double s = 0.0;
  for (int k = 0; k < 3; k++) s += (a[k] - b[k]) * (a[k] - b[k]);
  return std::sqrt(s);
}

// ---------------------------------------------------------------------------
// Shared geometry: the bulge example from the task statement.
const double kBulgeP[12] = {-1, 0, 10, -1.0 / 3, 0, 10,
                            1.0 / 3, 0, 10, 1, 0, 10};
const double kBulgeR[4] = {0.2, 2, 2, 0.2};
// r(0.5) = 1.55, C(0.5) = (0,0,10); ray (0,.5,0)+t(0,0,1) hits at
// t = 10 - sqrt(1.55^2 - 0.5^2).
const double kBulgeT = 10.0 - 1.4671400751120046;  // 10 - sqrt(2.1525)

// Straight capsule, radius 0.5, axis (0,0,0)..(0,0,9).
const double kCapsP[12] = {0, 0, 0, 0, 0, 3, 0, 0, 6, 0, 0, 9};
const double kCapsR[4] = {0.5, 0.5, 0.5, 0.5};

// Curved curve giving two disjoint ray intervals along the z axis
// (x(u) = 25(u-.3)(u-.7)(u+.5), z = 6+9u, r = 0.6).
const double kCurve2P[12] = {2.625, 0, 6, 0.2083333333, 0, 9,
                             -6.375, 0, 12, 7.875, 0, 15};
const double kCurve2R[4] = {0.6, 0.6, 0.6, 0.6};

void registerEnvelope(Registry &r) {
  r.add("envelope", "varying_radius_bulge", [](Case &c) {
    // Double precision path.
    Scene<double> *sc = singleCurve<double>(kBulgeP, kBulgeR);
    double org[3] = {0, 0.5, 0}, dir[3] = {0, 0, 1};
    round_tube::Intersection<double> hit;
    c.check(sc->trace(org, dir, 0.0, 100.0, &hit), "bulge: no hit");
    const double S = 2.0;  // max(1, bbox diag = 2, max radius = 2)
    c.check(std::fabs(hit.t - kBulgeT) <= 2e-7 * S,
            "bulge: t not at true swept surface");
    c.check(hit.t != 10.0, "bulge: still returning centerline depth");
    c.check(std::fabs(hit.u - 0.5) <= 1e-5, "bulge: u not near 0.5");
    c.check(hit.prim_id == 0, "bulge: wrong prim_id");
    double nexp[3] = {0.0, 0.5 / 1.55, -1.4671400751120046 / 1.55};
    double nerr = vlen3(hit.normal.v, nexp);
    c.check(nerr <= 2e-5, "bulge: normal mismatch");
    c.check(checkBackSubst(*sc, hit, 1e-9), "bulge: u does not back-substitute");
    c.check(checkNormal(*sc, hit, 2e-5), "bulge: normal not from C(u)");
    // Independent reference agrees.
    ref::Curve cv;
    for (int i = 0; i < 4; i++) {
      for (int k = 0; k < 3; k++) cv.P[i][k] = kBulgeP[3 * i + k];
      cv.r[i] = kBulgeR[i];
    }
    std::vector<std::pair<double, double> > iv = ref::intervals(cv, org, dir, 0.0, 100.0);
    c.check(iv.size() == 1, "bulge: reference interval count");
    c.check(std::fabs(iv[0].first - hit.t) <= 1e-6 * S,
            "bulge: production vs independent reference");
    delete sc;

    // Float path through the same templates.
    Scene<float> *scf = singleCurve<float>(kBulgeP, kBulgeR);
    float orgf[3] = {0, 0.5f, 0}, dirf[3] = {0, 0, 1};
    round_tube::Intersection<float> hitf;
    c.check(scf->trace(orgf, dirf, 0.0f, 100.0f, &hitf), "bulge float: no hit");
    c.check(std::fabs(hitf.t - (float)kBulgeT) <= 3e-4f * (float)S,
            "bulge float: t error");
    float nerrf = 0.0f;
    for (int k = 0; k < 3; k++)
      nerrf = std::max(nerrf, std::fabs(hitf.normal[k] - (float)nexp[k]));
    c.check(nerrf <= 2e-3f, "bulge float: normal error");
    delete scf;
  });

  r.add("envelope", "constant_centerline", [](Case &c) {
    // Degenerate centerline: union of concentric balls = single ball with
    // radius max_u r(u) = 0.875 (Bernstein max at u = 0.5).
    const double P[12] = {0, 0, 10, 0, 0, 10, 0, 0, 10, 0, 0, 10};
    const double R[4] = {0.5, 1.0, 1.0, 0.5};
    Scene<double> *sc = singleCurve<double>(P, R);
    double org[3] = {0, 0, 0}, dir[3] = {0, 0, 1};
    round_tube::Intersection<double> hit;
    c.check(sc->trace(org, dir, 0.0, 100.0, &hit), "const-center: no hit");
    c.check(std::fabs(hit.t - 9.125) <= 2e-7, "const-center: t != 9.125");
    double nexp[3] = {0, 0, -1};
    c.check(vlen3(hit.normal.v, nexp) <= 2e-5, "const-center: normal");
    c.check(std::fabs(hit.u - 0.5) <= 1e-5,
            "const-center: u not at max-radius parameter");
    c.check(checkBackSubst(*sc, hit, 1e-9), "const-center: back-substitution");
    // Off-axis ray: t = 10 - sqrt(0.875^2 - 0.5^2).
    double org2[3] = {0.5, 0, 0};
    c.check(sc->trace(org2, dir, 0.0, 100.0, &hit), "const-center: no hit 2");
    double texp = 10.0 - std::sqrt(0.875 * 0.875 - 0.25);
    c.check(std::fabs(hit.t - texp) <= 2e-7, "const-center: off-axis t");
    c.check(checkBackSubst(*sc, hit, 1e-9), "const-center: back-subst 2");
    c.check(checkNormal(*sc, hit, 2e-5), "const-center: normal 2");
    // Independent reference agrees on both rays.
    ref::Curve cv;
    for (int i = 0; i < 4; i++) {
      for (int k = 0; k < 3; k++) cv.P[i][k] = P[3 * i + k];
      cv.r[i] = R[i];
    }
    std::vector<std::pair<double, double> > iv = ref::intervals(cv, org, dir, 0.0, 100.0);
    c.check(iv.size() == 1 && std::fabs(iv[0].first - 9.125) <= 1e-6,
            "const-center: reference interval");
    delete sc;
  });

  r.add("envelope", "conservative_bbox", [](Case &c) {
    // The BVH bounding boxes must contain the whole swept sphere, including
    // the full radius variation.
    unsigned long long seed = 12345;
    std::vector<double> verts, radii;
    const int N = 8;
    for (int i = 0; i < N; i++) {
      for (int j = 0; j < 12; j++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        double v = ((seed >> 11) / double(1ULL << 53)) * 8.0 - 4.0;
        verts.push_back(v);
      }
      for (int j = 0; j < 4; j++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        radii.push_back(0.05 + ((seed >> 11) / double(1ULL << 53)) * 1.45);
      }
    }
    round_tube::Geometry<double> geom(verts.data(), radii.data());
    for (int i = 0; i < N; i++) {
      nanort::real3<double> bmin, bmax;
      geom.BoundingBox(&bmin, &bmax, i);
      // Sample the swept sphere densely; every ball must be inside the box.
      for (int s = 0; s <= 200; s++) {
        double u = s / 200.0, C[3], r;
        evalCurveCR(verts, radii, i, u, C, &r);
        for (int k = 0; k < 3; k++) {
          c.check(C[k] - r >= bmin[k] - 1e-9 && C[k] + r <= bmax[k] + 1e-9,
                  "bbox does not contain swept sphere");
        }
      }
    }
    // BVH builds over these boxes and the scene bounds contain every prim box.
    Scene<double> sc;
    sc.verts = verts;
    sc.radii = radii;
    c.check(sc.build(), "bbox: BVH build failed");
    double sbmin[3], sbmax[3];
    sc.accel.BoundingBox(sbmin, sbmax);
    for (int i = 0; i < N; i++) {
      nanort::real3<double> bmin, bmax;
      geom.BoundingBox(&bmin, &bmax, i);
      for (int k = 0; k < 3; k++) {
        c.check(sbmin[k] <= bmin[k] + 1e-9 && sbmax[k] >= bmax[k] - 1e-9,
                "scene bbox does not contain prim bbox");
      }
    }
  });
}

void registerIntervals(Registry &r) {
  r.add("intervals", "straight_capsule_analytic", [](Case &c) {
    Scene<double> *sc = singleCurve<double>(kCapsP, kCapsR);
    const double S = 9.0;  // bbox diagonal of the control points
    round_tube::Intersection<double> hit;

    // Cap entry: analytic capsule t = 2 - sqrt(0.25 - 0.16) = 1.7 at u = 0.
    double org1[3] = {0.4, 0, -2}, dir1[3] = {0, 0, 1};
    c.check(sc->trace(org1, dir1, 0.0, 100.0, &hit), "capsule cap: no hit");
    c.check(std::fabs(hit.t - 1.7) <= 2e-7 * S, "capsule cap: t");
    c.check(std::fabs(hit.u) <= 1e-6, "capsule cap: u");
    double n1[3] = {0.8, 0, -0.6};
    c.check(vlen3(hit.normal.v, n1) <= 2e-5, "capsule cap: normal");

    // Side entry: t = 1.5 at u = 0.5, normal (1,0,0).
    double org2[3] = {2, 0, 4.5}, dir2[3] = {-1, 0, 0};
    c.check(sc->trace(org2, dir2, 0.0, 100.0, &hit), "capsule side: no hit");
    c.check(std::fabs(hit.t - 1.5) <= 2e-7 * S, "capsule side: t");
    c.check(std::fabs(hit.u - 0.5) <= 1e-6, "capsule side: u");
    double n2[3] = {1, 0, 0};
    c.check(vlen3(hit.normal.v, n2) <= 2e-5, "capsule side: normal");
    c.check(checkBackSubst(*sc, hit, 1e-9), "capsule side: back-substitution");

    // Interior start: min_t = 0 is inside the capsule; the exit through the
    // far cap (z = 9 + sqrt(0.25-0.01)) is the union boundary, not any
    // internal sphere crossing.
    double org3[3] = {0.1, 0, 4.5};
    c.check(sc->trace(org3, dir1, 0.0, 100.0, &hit), "capsule exit: no hit");
    double texp = 9.0 + std::sqrt(0.24) - 4.5;
    c.check(std::fabs(hit.t - texp) <= 2e-7 * S, "capsule exit: t");
    c.check(std::fabs(hit.u - 1.0) <= 1e-6, "capsule exit: u");
    double n3[3] = {0.2, 0, std::sqrt(0.96)};
    c.check(vlen3(hit.normal.v, n3) <= 2e-5, "capsule exit: normal");
    delete sc;
  });

  r.add("intervals", "curved_multi_interval", [](Case &c) {
    Scene<double> *sc = singleCurve<double>(kCurve2P, kCurve2R);
    double org[3] = {0, 0, 0}, dir[3] = {0, 0, 1};
    ref::Curve cv;
    for (int i = 0; i < 4; i++) {
      for (int k = 0; k < 3; k++) cv.P[i][k] = kCurve2P[3 * i + k];
      cv.r[i] = kCurve2R[i];
    }
    std::vector<std::pair<double, double> > iv = ref::intervals(cv, org, dir, 0.0, 20.0);
    c.check(iv.size() == 2, "curved: reference must give two ray intervals");
    const double S = 17.0;  // control bbox diagonal ~ 16.9
    round_tube::Intersection<double> hit;

    // First interval entry.
    c.check(sc->trace(org, dir, 0.0, 100.0, &hit), "curved: no first hit");
    c.check(std::fabs(hit.t - iv[0].first) <= 1e-6 * S,
            "curved: first interval entry vs reference");
    c.check(checkBackSubst(*sc, hit, 1e-7 * S), "curved: back-subst first");
    c.check(checkNormal(*sc, hit, 2e-5), "curved: normal first");

    // Starting between the intervals must find the second entry (re-entry
    // of the same curve further along the ray).
    double mid = 0.5 * (iv[0].second + iv[1].first);
    c.check(sc->trace(org, dir, mid, 100.0, &hit), "curved: no second hit");
    c.check(std::fabs(hit.t - iv[1].first) <= 1e-6 * S,
            "curved: second interval entry vs reference");

    // Starting inside the second interval must find its exit, not an
    // internal sphere crossing.
    double inside = 0.5 * (iv[1].first + iv[1].second);
    c.check(sc->trace(org, dir, inside, 100.0, &hit), "curved: no exit hit");
    c.check(std::fabs(hit.t - iv[1].second) <= 1e-6 * S,
            "curved: second interval exit vs reference");
    c.check(checkBackSubst(*sc, hit, 1e-7 * S), "curved: back-subst exit");
    delete sc;
  });

  r.add("intervals", "interior_exit_past_inner_spheres", [](Case &c) {
    // Widening then narrowing tube along z.  A ray starting on the axis at
    // z = 5 is inside; the sphere at u = 0.5 (r = 1.625) ends at z = 6.625,
    // but that sphere boundary is internal - the true union exit is the
    // u = 1 cap at z = 10.5.
    const double P[12] = {0, 0, 0, 0, 0, 10.0 / 3, 0, 0, 20.0 / 3, 0, 0, 10};
    const double R[4] = {0.5, 2, 2, 0.5};
    Scene<double> *sc = singleCurve<double>(P, R);
    double org[3] = {0, 0, 5}, dir[3] = {0, 0, 1};
    round_tube::Intersection<double> hit;
    c.check(sc->trace(org, dir, 0.0, 100.0, &hit), "inner-exit: no hit");
    const double S = 10.0;
    c.check(std::fabs(hit.t - 5.5) <= 2e-7 * S, "inner-exit: t != cap exit");
    c.check(std::fabs(hit.t - 1.625) > 1.0,
            "inner-exit: returned an internal sphere boundary");
    c.check(std::fabs(hit.u - 1.0) <= 1e-6, "inner-exit: u != 1");
    double nexp[3] = {0, 0, 1};
    c.check(vlen3(hit.normal.v, nexp) <= 2e-5, "inner-exit: normal");
    // Independent reference: first interval containing t=0 ends at 5.5.
    ref::Curve cv;
    for (int i = 0; i < 4; i++) {
      for (int k = 0; k < 3; k++) cv.P[i][k] = P[3 * i + k];
      cv.r[i] = R[i];
    }
    std::vector<std::pair<double, double> > iv = ref::intervals(cv, org, dir, 0.0, 100.0);
    c.check(iv.size() == 1 && iv[0].first == 0.0,
            "inner-exit: reference should start inside");
    c.check(std::fabs(iv[0].second - 5.5) <= 1e-6 * S,
            "inner-exit: reference exit");
    // A later interior start (z = 9, still inside) finds the same exit.
    c.check(sc->trace(org, dir, 4.0, 100.0, &hit), "inner-exit: no hit 2");
    c.check(std::fabs(hit.t - 5.5) <= 2e-7 * S, "inner-exit: t 2");
    delete sc;
  });

  r.add("intervals", "tangent_contact", [](Case &c) {
    // Analytic cylinder: axis (0,1,z), radius 1.  A ray at distance exactly
    // 1 touches without crossing - that external contact is a hit.
    const double P[12] = {0, 1, 0, 0, 1, 3, 0, 1, 6, 0, 1, 9};
    const double R[4] = {1, 1, 1, 1};
    Scene<double> *sc = singleCurve<double>(P, R);
    const double S = 9.1;  // > control bbox diagonal sqrt(1+81)
    double dir[3] = {0, 0, 1};
    round_tube::Intersection<double> hit;

    double orgT[3] = {0, 0, -5};
    c.check(sc->trace(orgT, dir, 0.0, 100.0, &hit), "tangent: no hit");
    c.check(std::fabs(hit.t - 5.0) <= 1e-4 * S, "tangent: t");
    c.check(checkBackSubst(*sc, hit, 1e-4 * S), "tangent: back-substitution");

    // Clear miss: ray stays 0.02 (and then ~1e-3*S) away from the tube.
    double orgM[3] = {0, -0.02, -5};
    c.check(!sc->trace(orgM, dir, 0.0, 100.0, &hit), "tangent: false hit at 0.02");
    double orgM2[3] = {0, -0.0095, -5};
    c.check(!sc->trace(orgM2, dir, 0.0, 100.0, &hit),
            "tangent: false hit at ~1e-3*S");
    // Clear hit: ray passes 0.5 from the axis, t = 5 - sqrt(1 - 0.25).
    double orgH[3] = {0, 0.5, -5};
    c.check(sc->trace(orgH, dir, 0.0, 100.0, &hit), "tangent: clear hit missed");
    c.check(std::fabs(hit.t - (5.0 - std::sqrt(0.75))) <= 2e-7 * S,
            "tangent: clear hit t");
    delete sc;
  });

  r.add("intervals", "finite_window", [](Case &c) {
    Scene<double> *sc = singleCurve<double>(kBulgeP, kBulgeR);
    double org[3] = {0, 0.5, 0}, dir[3] = {0, 0, 1};
    const double S = 2.0;
    round_tube::Intersection<double> hit;
    // Window ends before the true boundary: miss.
    c.check(!sc->trace(org, dir, 0.0, 8.5, &hit), "window: hit before boundary");
    // Window contains the entry: hit at the true boundary.
    c.check(sc->trace(org, dir, 0.0, 8.54, &hit), "window: entry not found");
    c.check(std::fabs(hit.t - kBulgeT) <= 2e-7 * S, "window: entry t");
    // Window starts inside the tube: the exit boundary is reported.
    const double tExit = 10.0 + 1.4671400751120046;
    c.check(sc->trace(org, dir, 8.6, 100.0, &hit), "window: exit not found");
    c.check(std::fabs(hit.t - tExit) <= 2e-7 * S, "window: exit t");
    c.check(std::fabs(hit.u - 0.5) <= 1e-5, "window: exit u");
    // Window starts past the exit: miss.
    c.check(!sc->trace(org, dir, 11.5, 100.0, &hit), "window: hit past exit");
    delete sc;
  });
}

void registerTraversal(Registry &r) {
  r.add("traversal", "bvh_nearest_multi", [](Case &c) {
    // Five straight tubes crossing the z axis at increasing depth.
    Scene<double> sc;
    sc.verts.resize(12 * 5);
    sc.radii.resize(4 * 5);
    for (unsigned i = 0; i < 5; i++) {
      double y = 0.2 * i, z = 10.0 + 5.0 * i;
      double P[12] = {-2, y, z, -2.0 / 3, y, z, 2.0 / 3, y, z, 2, y, z};
      double R[4] = {1, 1, 1, 1};
      setCurve(&sc, i, P, R);
    }
    c.check(sc.build(), "multi: build failed");
    double org[3] = {0, 0, 0}, dir[3] = {0, 0, 1};
    const double S = 34.0;  // scene scale
    round_tube::Intersection<double> hit;

    // Nearest of five hits through the BVH.
    c.check(sc.trace(org, dir, 0.0, 100.0, &hit), "multi: no hit");
    c.check(hit.prim_id == 0, "multi: nearest prim_id != 0");
    c.check(std::fabs(hit.t - 9.0) <= 2e-7 * S, "multi: nearest t");

    // Test-only cross-check: brute-force every primitive with a bare
    // intersector (no BVH) must give the same minimum.
    {
      round_tube::Intersector<double> brute(sc.verts.data(), sc.radii.data());
      nanort::Ray<double> ray;
      for (int k = 0; k < 3; k++) { ray.org[k] = org[k]; ray.dir[k] = dir[k]; }
      ray.min_t = 0.0; ray.max_t = 100.0;
      nanort::BVHTraceOptions opt;
      brute.PrepareTraversal(ray, opt);
      double best = 100.0;
      for (unsigned i = 0; i < 5; i++) {
        double t = best;
        if (brute.Intersect(&t, i) && t < best) best = t;
      }
      c.check(std::fabs(best - hit.t) <= 1e-9 * S,
              "multi: BVH result differs from brute force");
    }

    // prim_ids_range filtering.
    nanort::BVHTraceOptions opt;
    opt.prim_ids_range[0] = 2;
    opt.prim_ids_range[1] = 4;
    c.check(sc.trace(org, dir, 0.0, 100.0, &hit, &opt), "multi: no ranged hit");
    c.check(hit.prim_id == 2, "multi: ranged prim_id != 2");
    double t2 = 20.0 - std::sqrt(1.0 - 0.16);
    c.check(std::fabs(hit.t - t2) <= 2e-7 * S, "multi: ranged t");
    opt.prim_ids_range[0] = 4;
    opt.prim_ids_range[1] = 5;
    c.check(sc.trace(org, dir, 0.0, 100.0, &hit, &opt), "multi: no ranged hit 2");
    c.check(hit.prim_id == 4 && std::fabs(hit.t - 29.4) <= 2e-7 * S,
            "multi: ranged hit 2");
    opt.prim_ids_range[0] = 5;
    opt.prim_ids_range[1] = 7;
    c.check(!sc.trace(org, dir, 0.0, 100.0, &hit, &opt),
            "multi: empty range must miss");
  });

  r.add("traversal", "cull_back_face", [](Case &c) {
    // Two capsules on the z axis: A covers z in [0,4], B covers [10,14].
    Scene<double> sc;
    sc.verts.resize(24);
    sc.radii.resize(8);
    const double R[4] = {0.5, 0.5, 0.5, 0.5};
    const double PA[12] = {0, 0, 0, 0, 0, 4.0 / 3, 0, 0, 8.0 / 3, 0, 0, 4};
    const double PB[12] = {0, 0, 10, 0, 0, 10 + 4.0 / 3,
                           0, 0, 10 + 8.0 / 3, 0, 0, 14};
    setCurve(&sc, 0, PA, R);
    setCurve(&sc, 1, PB, R);
    c.check(sc.build(), "cull: build failed");
    double org[3] = {0, 0, 2}, dir[3] = {0, 0, 1};  // starts inside A
    round_tube::Intersection<double> hit;

    // No culling: the exit of capsule A (normal . D > 0) is reported.
    c.check(sc.trace(org, dir, 0.0, 100.0, &hit), "cull: no exit hit");
    c.check(std::fabs(hit.t - 2.5) <= 2e-6, "cull: exit t");
    double ndot = hit.normal[2] * dir[2];
    c.check(ndot > 0, "cull: exit should face away");

    // With cull_back_face the exit is rejected and the search continues to
    // the next entry (capsule B at z = 9.5).
    nanort::BVHTraceOptions opt;
    opt.cull_back_face = true;
    c.check(sc.trace(org, dir, 0.0, 100.0, &hit, &opt), "cull: no re-entry");
    c.check(std::fabs(hit.t - 7.5) <= 2e-6, "cull: re-entry t");
    c.check(hit.prim_id == 1, "cull: re-entry prim");
    ndot = hit.normal[2] * dir[2];
    c.check(ndot < 0, "cull: re-entry must face the ray");

    // Culling with only capsule A present: no boundary accepted at all.
    Scene<double> *single = singleCurve<double>(PA, R);
    c.check(!single->trace(org, dir, 0.0, 100.0, &hit, &opt),
            "cull: exit must be rejected");
    delete single;
  });

  r.add("traversal", "intersector_reuse", [](Case &c) {
    // One intersector, interleaved queries: directions, windows, misses.
    Scene<double> sc;
    sc.verts.resize(12 * 3);
    sc.radii.resize(4 * 3);
    for (unsigned i = 0; i < 3; i++) {
      double y = 0.2 * i, z = 10.0 + 5.0 * i;
      double P[12] = {-2, y, z, -2.0 / 3, y, z, 2.0 / 3, y, z, 2, y, z};
      double R[4] = {1, 1, 1, 1};
      setCurve(&sc, i, P, R);
    }
    c.check(sc.build(), "reuse: build failed");

    round_tube::Intersector<double> shared(sc.verts.data(), sc.radii.data());
    round_tube::Intersector<double> fresh(sc.verts.data(), sc.radii.data());

    double org[3] = {0, 0, 0}, dir[3] = {0, 0, 1};
    nanort::Ray<double> ray;
    for (int k = 0; k < 3; k++) { ray.org[k] = org[k]; ray.dir[k] = dir[k]; }
    ray.min_t = 0.0; ray.max_t = 100.0;

    round_tube::Intersection<double> hs, hf;
    // q1: plain hit with the shared intersector.
    c.check(sc.accel.Traverse(ray, shared, &hs), "reuse: q1 miss");
    c.check(std::fabs(hs.t - 9.0) <= 1e-6 && hs.prim_id == 0, "reuse: q1 hit");
    // q2: a miss (ray starts past everything) must not leave stale state.
    nanort::Ray<double> missRay = ray;
    missRay.org[2] = 100.0;
    missRay.min_t = 0.0;
    c.check(!sc.accel.Traverse(missRay, shared, &hs), "reuse: q2 stale hit");
    // q3: windowed query after the miss.
    nanort::Ray<double> winRay = ray;
    winRay.min_t = 9.5;
    c.check(sc.accel.Traverse(winRay, shared, &hs), "reuse: q3 miss");
    c.check(sc.accel.Traverse(winRay, fresh, &hf), "reuse: q3 fresh miss");
    c.check(hs.t == hf.t && hs.prim_id == hf.prim_id && hs.u == hf.u,
            "reuse: q3 shared != fresh (stale candidate)");
    c.check(std::fabs(hs.t - 11.0) <= 1e-6, "reuse: q3 expected exit at 11");
    // q4: repeat of q1 must reproduce the original hit exactly.
    c.check(sc.accel.Traverse(ray, shared, &hs), "reuse: q4 miss");
    c.check(std::fabs(hs.t - 9.0) <= 1e-6 && hs.prim_id == 0,
            "reuse: q4 differs from q1");
    // q5: different direction after hits and misses.
    nanort::Ray<double> ray2 = ray;
    ray2.org[0] = 0.0; ray2.org[1] = 0.6; ray2.org[2] = 0.0;
    ray2.dir[0] = 0.0; ray2.dir[1] = -0.02; ray2.dir[2] = 1.0;
    c.check(sc.accel.Traverse(ray2, shared, &hs) ==
                sc.accel.Traverse(ray2, fresh, &hf),
            "reuse: q5 hit flag mismatch");
    c.check(hs.t == hf.t && hs.prim_id == hf.prim_id,
            "reuse: q5 shared != fresh");
  });
}

void registerRender(Registry &r) {
  r.add("render", "round_tubes_ppm", [](Case &c) {
    const char *path = "round_tubes_test_out.ppm";
    std::remove(path);
    char arg0[] = "curves_primitive";
    char arg1[] = "--round-tubes";
    char arg3[] = "round_tubes_test_out.ppm";
    char *argv[] = {arg0, arg1, arg3};
    // The new mode must run to completion and exit successfully.
    int rc = curves_example_main(3, argv);
    c.check(rc == 0, "render: --round-tubes did not exit successfully");

    std::ifstream in(path);
    c.check(in.good(), "render: no output file");
    std::string magic;
    int w = 0, h = 0, maxv = 0;
    in >> magic >> w >> h >> maxv;
    c.check(magic == "P3", "render: not a P3 image");
    c.check(w == 64 && h == 64, "render: wrong dimensions");
    c.check(maxv == 255, "render: wrong maxval");
    std::vector<int> px(64 * 64 * 3, -1);
    for (size_t i = 0; i < px.size(); i++) {
      in >> px[i];
      c.check(px[i] >= 0 && px[i] <= 255, "render: pixel out of range");
    }
    in.close();

    // Background and tube pixel counts.
    int bg = 0, tubes = 0;
    int firstTube = -1, firstBg = -1;
    for (int i = 0; i < 64 * 64; i++) {
      bool isBg = (px[3 * i] == 108 && px[3 * i + 1] == 108 &&
                   px[3 * i + 2] == 123);
      if (isBg) {
        bg++;
        if (firstBg < 0) firstBg = i;
      } else {
        tubes++;
        if (firstTube < 0) firstTube = i;
      }
    }
    c.check(bg >= 1000, "render: background missing");
    c.check(tubes >= 100, "render: not enough tube pixels");
    c.check(firstTube >= 0 && firstBg >= 0, "render: pixel classes missing");

    // Geometry content: rebuild the same scene and camera, and verify that
    // a tube pixel and a background pixel match independent traversal.
    std::vector<float> verts, thik;
    float3 center(0.0f, 0.0f, 0.0f);
    genFur(&verts, &thik, center, 4.0f, 0.01f);
    round_tube::Geometry<float> geom(verts.data(), thik.data());
    round_tube::Pred<float> pred(verts.data());
    nanort::BVHAccel<float> accel;
    c.check(accel.Build((unsigned int)(thik.size() / 4), geom, pred),
            "render: rebuild failed");
    round_tube::Intersector<float> isector(verts.data(), thik.data());

    for (int which = 0; which < 2; which++) {
      int pix = which == 0 ? firstTube : firstBg;
      int fileRow = pix / 64, col = pix % 64;
      int ycam = 63 - fileRow;  // renderer flips Y
      nanort::Ray<float> ray;
      ray.org[0] = 0.0f; ray.org[1] = 0.0f; ray.org[2] = 20.0f;
      float3 dir;
      dir[0] = col / 64.0f - 0.5f;
      dir[1] = ycam / 64.0f - 0.5f;
      dir[2] = -1.0f;
      dir = vnormalize(dir);
      ray.dir[0] = dir[0]; ray.dir[1] = dir[1]; ray.dir[2] = dir[2];
      ray.min_t = 0.0f; ray.max_t = 1.0e+30f;
      round_tube::Intersection<float> isect;
      bool hit = accel.Traverse(ray, isector, &isect);
      if (which == 0) {
        c.check(hit, "render: tube pixel does not hit");
        float expected[3] = {fabsf(isect.normal[0]), fabsf(isect.normal[1]),
                             fabsf(isect.normal[2])};
        for (int k = 0; k < 3; k++) {
          int want = (int)fclamp(expected[k]);
          c.check(px[3 * pix + k] == want, "render: tube pixel color mismatch");
        }
        float nl = std::sqrt(isect.normal[0] * isect.normal[0] +
                             isect.normal[1] * isect.normal[1] +
                             isect.normal[2] * isect.normal[2]);
        c.check(std::fabs(nl - 1.0f) <= 1e-3f, "render: normal not unit");
      } else {
        c.check(!hit, "render: background pixel unexpectedly hits");
      }
    }
    std::remove(path);
  });

  r.add("render", "legacy_triangle_path", [](Case &c) {
    // The pre-existing triangle path (regression possible-accuracy-problem-30)
    // must keep working in both modes.
    double vertices[9];
    unsigned int triangles[3] = {0, 1, 2};
    vertices[0] = 1.0;  vertices[1] = 2.0; vertices[2] = -3.0;
    vertices[3] = -1.0; vertices[4] = 2.0; vertices[5] = -3.0;
    vertices[6] = 1.0;  vertices[7] = 2.0; vertices[8] = 3.0;
    for (int mode = 0; mode < 2; mode++) {
      nanort::TriangleMesh<double> mesh(vertices, triangles, sizeof(double) * 3);
      nanort::TriangleSAHPred<double> pred(vertices, triangles,
                                           sizeof(double) * 3);
      nanort::BVHAccel<double> accel;
      nanort::BVHBuildOptions<double> bopt;
      bopt.cache_bbox = true;
      c.check(accel.Build(1, mesh, pred, bopt), "triangle: build failed");
      nanort::Ray<double> ray;
      ray.org[0] = -0.36; ray.org[1] = 7.93890843; ray.org[2] = 1.2160368;
      double dir[3] = {mode == 0 ? 0.0 : -5.30287619e-17, -8.66025404e-01, -0.5};
      double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      for (int k = 0; k < 3; k++) ray.dir[k] = dir[k] / len;
      ray.min_t = 0.0;
      ray.max_t = 1.0e+30;
      nanort::TriangleIntersector<double, nanort::TriangleIntersection<double> >
          isector(vertices, triangles, sizeof(double) * 3);
      nanort::TriangleIntersection<double> isect;
      c.check(accel.Traverse(ray, isector, &isect),
              "triangle: expected intersection missing");
      c.check(isect.u >= 0.0 && isect.v >= 0.0 && isect.u + isect.v <= 1.0 + 1e-9,
              "triangle: barycentric coordinates out of range");
    }
  });
}

}  // namespace

void register_task_tests(Registry &r) {
  registerEnvelope(r);
  registerIntervals(r);
  registerTraversal(r);
  registerRender(r);
}
