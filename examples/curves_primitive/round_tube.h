// Round tube (swept-sphere) primitive for NanoRT.
//
// A primitive is the union of all closed balls
//     { X : |X - C(u)| <= r(u) },  u in [0, 1]
// where C(u) is a cubic Bezier centerline (4 control points) and r(u) > 0 is
// a cubic radius function given in the same Bernstein basis.  The union
// naturally includes the two spherical caps at u = 0 and u = 1.
//
// Ray intersection finds the outer boundary of this union: sphere patches
// that are hidden inside other balls of the same curve (self fold-back, a
// wide part covering a narrow part, overlapping parameter ranges) do not
// produce hits.  Different primitives are still intersected individually
// through the BVH; no scene-wide boolean union is performed.
//
// The classes in namespace `round_tube` work for both float and double and
// plug directly into nanort::BVHAccel<T>::Build / Traverse:
//
//   round_tube::Geometry<T>      - conservative primitive bounding boxes
//   round_tube::Pred<T>          - SAH split predicate
//   round_tube::Intersection<T>  - hit record (t, prim_id, u, position, normal)
//   round_tube::Intersector<T>   - ray vs. swept-sphere-union intersector
//
// Method (see README.md next to this file for the full description):
// the boundary of the union of balls is the set of points X with
//     m(X) := min_u (|X - C(u)|^2 - r(u)^2) = 0.
// Along the ray X(t) = O + t D the zeros of m(t) are found algebraically:
// interior minimizers satisfy the envelope system h = 0, dh/du = 0, which
// eliminates t into a degree-10 polynomial in u; the two cap spheres give
// quadratics.  Every candidate t is validated against the true union
// boundary by evaluating m(t) (minimization of a degree-6 polynomial in u),
// so hidden/internal sphere crossings are rejected.  Roots are isolated by
// recursive Bernstein subdivision (no fixed sampling of the surface) and
// polished with Newton steps.  All solving is done in double precision.

#ifndef NANORT_EXAMPLES_CURVES_PRIMITIVE_ROUND_TUBE_H_
#define NANORT_EXAMPLES_CURVES_PRIMITIVE_ROUND_TUBE_H_

#include <algorithm>
#include <cmath>
#include <vector>

#include "nanort.h"

namespace round_tube {

namespace detail {

// ---------------------------------------------------------------------------
// Dense scalar polynomial in power basis, degree <= 10.
// p(u) = c[0] + c[1] u + ... + c[n] u^n
struct Poly {
  int n;        // degree (0..10)
  double c[11]; // coefficients
};

inline Poly polyZero(int n = 0) {
  Poly p;
  p.n = n;
  for (int i = 0; i <= 10; i++) p.c[i] = 0.0;
  return p;
}

inline Poly polyAdd(const Poly &a, const Poly &b) {
  Poly r = polyZero(std::max(a.n, b.n));
  for (int i = 0; i <= a.n; i++) r.c[i] += a.c[i];
  for (int i = 0; i <= b.n; i++) r.c[i] += b.c[i];
  return r;
}

inline Poly polyScale(const Poly &a, double s) {
  Poly r = a;
  for (int i = 0; i <= a.n; i++) r.c[i] *= s;
  return r;
}

inline Poly polyMul(const Poly &a, const Poly &b) {
  Poly r = polyZero(a.n + b.n);
  for (int i = 0; i <= a.n; i++)
    for (int j = 0; j <= b.n; j++) r.c[i + j] += a.c[i] * b.c[j];
  return r;
}

inline Poly polyDer(const Poly &a) {
  Poly r = polyZero(a.n > 0 ? a.n - 1 : 0);
  for (int i = 0; i < a.n; i++) r.c[i] = a.c[i + 1] * double(i + 1);
  return r;
}

inline double polyEval(const Poly &p, double u) {
  double v = 0.0;
  for (int i = p.n; i >= 0; i--) v = v * u + p.c[i];
  return v;
}

inline double polySumAbs(const Poly &p) {
  double s = 0.0;
  for (int i = 0; i <= p.n; i++) s += std::fabs(p.c[i]);
  return s;
}

inline double binom(int n, int k) {
  double r = 1.0;
  for (int i = 0; i < k; i++) {
    r *= double(n - i);
    r /= double(i + 1);
  }
  return r;
}

// Evaluate a polynomial given by Bernstein coefficients b[0..n] at u.
inline double bernsteinEval(const double *b, int n, double u) {
  double t[11];
  for (int i = 0; i <= n; i++) t[i] = b[i];
  for (int j = n; j > 0; j--)
    for (int i = 0; i < j; i++) t[i] = t[i] * (1.0 - u) + t[i + 1] * u;
  return t[0];
}

// Recursive Bernstein root isolation on [lo, hi] (coefficients refer to the
// current sub-interval).  A coefficient interval is discarded only when all
// coefficients are clearly positive or clearly negative (convex hull
// property), so no root - including tangential contacts - is skipped.
inline void isolateRec(const double *b, int n, double lo, double hi, double tol,
                       double u_tol, int depth, std::vector<double> *roots) {
  double amax = 0.0;
  bool all_pos = true, all_neg = true;
  for (int i = 0; i <= n; i++) {
    amax = std::max(amax, std::fabs(b[i]));
    if (b[i] <= tol) all_pos = false;
    if (b[i] >= -tol) all_neg = false;
  }
  if (all_pos || all_neg) return;  // definitively no root in [lo, hi]
  if (amax <= tol || (hi - lo) <= u_tol || depth >= 80) {
    // Numerically zero on the whole interval, or interval small enough.
    roots->push_back(0.5 * (lo + hi));
    return;
  }
  // de Casteljau split at the midpoint.
  double tab[11][11];
  for (int i = 0; i <= n; i++) tab[0][i] = b[i];
  for (int j = 1; j <= n; j++)
    for (int i = 0; i + j <= n; i++)
      tab[j][i] = 0.5 * (tab[j - 1][i] + tab[j - 1][i + 1]);
  double left[11], right[11];
  for (int j = 0; j <= n; j++) {
    left[j] = tab[j][0];
    right[j] = tab[n - j][j];
  }
  double mid = 0.5 * (lo + hi);
  isolateRec(left, n, lo, mid, tol, u_tol, depth + 1, roots);
  isolateRec(right, n, mid, hi, tol, u_tol, depth + 1, roots);
}

// Isolate all roots of p in [0,1].  Returns polished, sorted, de-duplicated
// roots.  tol_rel scales the zero-band of the coefficients, u_tol is the
// final isolation width before Newton polishing.
inline std::vector<double> isolateRoots(const Poly &p, double tol_rel,
                                        double u_tol) {
  // Power basis -> Bernstein basis on [0,1].
  double b[11];
  for (int k = 0; k <= p.n; k++) {
    double s = 0.0;
    for (int i = 0; i <= k; i++) s += binom(k, i) / binom(p.n, i) * p.c[i];
    b[k] = s;
  }
  double scale = 0.0;
  for (int k = 0; k <= p.n; k++) scale += std::fabs(b[k]);
  double tol = tol_rel * (scale + 1e-300);

  std::vector<double> raw;
  isolateRec(b, p.n, 0.0, 1.0, tol, u_tol, 0, &raw);

  // Newton polish on the top-level Bernstein form.
  std::vector<double> roots;
  for (size_t i = 0; i < raw.size(); i++) {
    double u = raw[i];
    for (int it = 0; it < 4; it++) {
      double f = bernsteinEval(b, p.n, u);
      double db[10];
      for (int k = 0; k < p.n; k++) db[k] = p.n * (b[k + 1] - b[k]);
      double df = (p.n > 0) ? bernsteinEval(db, p.n - 1, u) : 0.0;
      if (df == 0.0) break;
      double un = u - f / df;
      if (un < 0.0 || un > 1.0) break;
      u = un;
    }
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    roots.push_back(u);
  }
  std::sort(roots.begin(), roots.end());
  std::vector<double> uniq;
  for (size_t i = 0; i < roots.size(); i++) {
    if (uniq.empty() || roots[i] - uniq.back() > 1e-9) uniq.push_back(roots[i]);
  }
  return uniq;
}

// ---------------------------------------------------------------------------
// Ray vs. one swept-sphere curve.
//
// Centerline C(u) (cubic, power basis cC[3]) and radius r(u) (power basis
// cr).  With w(u) = O - C(u):
//   h(t,u) = |O + tD - C(u)|^2 - r(u)^2 = f0(u) + 2 t g(u) + t^2 dd
//   dh/du  = A(u) t + B(u)
// Envelope roots eliminate t = -B/A into E(u) = h(-B/A, u) A(u)^2.

struct TubePolys {
  Poly cC[3];  // centerline, degree <= 3
  Poly cr;     // radius, degree <= 3
  Poly A;      // -2 D.C'(u), degree <= 2
  Poly B;      // -2 (O-C).C'(u) - 2 r r'(u), degree <= 5
  Poly f0;     // |O-C(u)|^2 - r(u)^2, degree <= 6
  Poly g;      // D.(O-C(u)), degree <= 3
  Poly E;      // envelope polynomial, degree <= 10
  double dd;   // D.D
  double O[3];
  double D[3];
  double S;    // error scale: max(1, control bbox diagonal, max radius)
};

inline void buildTubePolys(const double O[3], const double D[3],
                           const double P[4][3], const double r[4],
                           TubePolys *tp) {
  // Bezier -> power basis.
  for (int k = 0; k < 3; k++) {
    tp->cC[k] = polyZero(3);
    tp->cC[k].c[0] = P[0][k];
    tp->cC[k].c[1] = 3.0 * (P[1][k] - P[0][k]);
    tp->cC[k].c[2] = 3.0 * (P[0][k] - 2.0 * P[1][k] + P[2][k]);
    tp->cC[k].c[3] = -P[0][k] + 3.0 * P[1][k] - 3.0 * P[2][k] + P[3][k];
  }
  tp->cr = polyZero(3);
  tp->cr.c[0] = r[0];
  tp->cr.c[1] = 3.0 * (r[1] - r[0]);
  tp->cr.c[2] = 3.0 * (r[0] - 2.0 * r[1] + r[2]);
  tp->cr.c[3] = -r[0] + 3.0 * r[1] - 3.0 * r[2] + r[3];

  Poly dc[3], dr;
  for (int k = 0; k < 3; k++) dc[k] = polyDer(tp->cC[k]);
  dr = polyDer(tp->cr);

  Poly w[3];  // w(u) = O - C(u)
  for (int k = 0; k < 3; k++) {
    w[k] = polyScale(tp->cC[k], -1.0);
    w[k].c[0] += O[k];
  }

  tp->dd = D[0] * D[0] + D[1] * D[1] + D[2] * D[2];

  tp->A = polyZero(2);
  tp->g = polyZero(3);
  tp->f0 = polyZero(6);
  Poly wdotdc = polyZero(5);
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i <= 2; i++) tp->A.c[i] += -2.0 * D[k] * dc[k].c[i];
    for (int i = 0; i <= 3; i++) tp->g.c[i] += D[k] * w[k].c[i];
    tp->f0 = polyAdd(tp->f0, polyMul(w[k], w[k]));
    wdotdc = polyAdd(wdotdc, polyMul(w[k], dc[k]));
  }
  Poly rr = polyMul(tp->cr, tp->cr);
  for (int i = 0; i <= 6; i++) tp->f0.c[i] -= rr.c[i];

  tp->B = polyAdd(polyScale(wdotdc, -2.0),
                  polyScale(polyMul(tp->cr, dr), -2.0));

  Poly A2 = polyMul(tp->A, tp->A);
  Poly B2 = polyMul(tp->B, tp->B);
  Poly ABg = polyMul(polyMul(tp->A, tp->B), tp->g);
  tp->E = polyAdd(polyMul(tp->f0, A2),
                  polyAdd(polyScale(ABg, -2.0), polyScale(B2, tp->dd)));

  for (int k = 0; k < 3; k++) {
    tp->O[k] = O[k];
    tp->D[k] = D[k];
  }

  // Error scale S = max(1, control-point bbox diagonal, max radius control).
  double lo[3] = {P[0][0], P[0][1], P[0][2]};
  double hi[3] = {P[0][0], P[0][1], P[0][2]};
  double rmax = r[0];
  for (int i = 1; i < 4; i++) {
    for (int k = 0; k < 3; k++) {
      lo[k] = std::min(lo[k], P[i][k]);
      hi[k] = std::max(hi[k], P[i][k]);
    }
    rmax = std::max(rmax, r[i]);
  }
  double diag = 0.0;
  for (int k = 0; k < 3; k++) diag += (hi[k] - lo[k]) * (hi[k] - lo[k]);
  tp->S = std::max(1.0, std::max(std::sqrt(diag), rmax));
}

inline double evalH(const TubePolys &tp, double t, double u) {
  return polyEval(tp.f0, u) + 2.0 * t * polyEval(tp.g, u) + t * t * tp.dd;
}

// m(t) = min over u in [0,1] of h(t,u); negative means the ray point is
// strictly inside the union of balls.
inline double minH(const TubePolys &tp, double t) {
  Poly s = polyAdd(polyScale(tp.A, t), tp.B);  // dh/du, degree <= 5
  std::vector<double> roots = isolateRoots(s, 1e-11, 1e-11);
  double best = evalH(tp, t, 0.0);
  best = std::min(best, evalH(tp, t, 1.0));
  for (size_t i = 0; i < roots.size(); i++)
    best = std::min(best, evalH(tp, t, roots[i]));
  return best;
}

inline void evalCenterRadius(const TubePolys &tp, double u, double C[3],
                             double *r) {
  for (int k = 0; k < 3; k++) C[k] = polyEval(tp.cC[k], u);
  *r = polyEval(tp.cr, u);
}

// Find the smallest t in [min_t, t_limit) where the ray crosses the outer
// boundary of the union of balls.  Returns false when there is no such
// boundary in the window.  When cull_back is true only boundary points with
// normal . D < 0 (entries) are accepted; exit points are skipped and the
// search continues behind them.
inline bool solveTube(const TubePolys &tp, double min_t, double t_limit,
                      bool cull_back, double *t_out, double *u_out) {
  const double m_tol = 1e-9 * tp.S * tp.S;
  const double t_slack = 1e-9 * (1.0 + std::fabs(min_t));
  const double normA = polySumAbs(tp.A);
  const double normB = polySumAbs(tp.B);

  struct Cand {
    double t, u;
    bool check_residual;
  };
  std::vector<Cand> cands;

  // Envelope candidates: roots of E(u) = 0.
  std::vector<double> us = isolateRoots(tp.E, 1e-12, 1e-12);
  for (size_t i = 0; i < us.size(); i++) {
    double u = us[i];
    double gu = polyEval(tp.g, u);
    double f0u = polyEval(tp.f0, u);
    double Au = polyEval(tp.A, u);
    double Bu = polyEval(tp.B, u);
    // Quadratic h(t,u) = dd t^2 + 2 gu t + f0u = 0.
    double disc = gu * gu - tp.dd * f0u;
    double dscale = gu * gu + tp.dd * std::fabs(f0u) + 1e-300;
    if (disc >= -1e-12 * dscale) {
      double sq = std::sqrt(std::max(0.0, disc));
      double q = (gu >= 0.0) ? -(gu + sq) : -(gu - sq);
      if (q != 0.0) {
        Cand c1 = {q / tp.dd, u, true};
        Cand c2 = {f0u / q, u, true};
        cands.push_back(c1);
        cands.push_back(c2);
      } else {
        Cand c0 = {0.0, u, true};  // double root at t = 0
        cands.push_back(c0);
      }
    }
    if (Au != 0.0) {
      Cand ce = {-Bu / Au, u, true};
      cands.push_back(ce);
    }
  }

  // Cap spheres at u = 0 and u = 1 (no envelope residual required).
  for (int e = 0; e < 2; e++) {
    double u = double(e);
    double gu = polyEval(tp.g, u);
    double f0u = polyEval(tp.f0, u);
    double disc = gu * gu - tp.dd * f0u;
    double dscale = gu * gu + tp.dd * std::fabs(f0u) + 1e-300;
    if (disc >= -1e-12 * dscale) {
      double sq = std::sqrt(std::max(0.0, disc));
      double q = (gu >= 0.0) ? -(gu + sq) : -(gu - sq);
      if (q != 0.0) {
        Cand c1 = {q / tp.dd, u, false};
        Cand c2 = {f0u / q, u, false};
        cands.push_back(c1);
        cands.push_back(c2);
      } else {
        Cand c0 = {0.0, u, false};
        cands.push_back(c0);
      }
    }
  }

  double best_t = t_limit;
  double best_u = 0.0;
  bool found = false;
  for (size_t i = 0; i < cands.size(); i++) {
    double t = cands[i].t;
    double u = cands[i].u;
    if (!(t >= min_t - t_slack && t < best_t)) continue;
    if (cands[i].check_residual) {
      double res =
          std::fabs(polyEval(tp.A, u) * t + polyEval(tp.B, u));
      double rt = 1e-7 * (normA * std::fabs(t) + normB + 1e-300);
      if (res > rt) continue;  // not actually on the envelope
    }
    // Union-boundary test: reject points hidden inside another ball.
    if (minH(tp, t) < -m_tol) continue;
    if (cull_back) {
      double C[3], ru;
      evalCenterRadius(tp, u, C, &ru);
      double dot = 0.0;
      for (int k = 0; k < 3; k++)
        dot += (tp.O[k] + t * tp.D[k] - C[k]) * tp.D[k];
      if (!(dot < 0.0)) continue;  // exit point: keep searching behind it
    }
    best_t = t;
    best_u = u;
    found = true;
  }

  if (!found) return false;
  if (best_t < min_t) best_t = min_t;
  *t_out = best_t;
  *u_out = best_u;
  return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Primitive accessor: conservative bounding boxes for the BVH builder.
// Layout matches the legacy example: 12 coordinates + 4 radii per curve.
template <typename T>
class Geometry {
 public:
  Geometry(const T *vertices, const T *radii)
      : vertices_(vertices), radii_(radii) {}

  // Conservative box: the Bezier centerline stays inside the control polygon
  // hull and r(u) never exceeds the largest Bernstein radius coefficient, so
  // control points expanded by max(r_i) contain the whole swept sphere.
  void BoundingBox(nanort::real3<T> *bmin, nanort::real3<T> *bmax,
                   unsigned int prim_index) const {
    T rmax = radii_[4 * prim_index + 0];
    rmax = std::max(rmax, radii_[4 * prim_index + 1]);
    rmax = std::max(rmax, radii_[4 * prim_index + 2]);
    rmax = std::max(rmax, radii_[4 * prim_index + 3]);
    for (int k = 0; k < 3; k++) {
      T lo = vertices_[3 * (4 * prim_index + 0) + k];
      T hi = lo;
      for (int i = 1; i < 4; i++) {
        lo = std::min(lo, vertices_[3 * (4 * prim_index + i) + k]);
        hi = std::max(hi, vertices_[3 * (4 * prim_index + i) + k]);
      }
      (*bmin)[k] = lo - rmax;
      (*bmax)[k] = hi + rmax;
    }
  }

  void BoundingBoxAndCenter(nanort::real3<T> *bmin, nanort::real3<T> *bmax,
                            nanort::real3<T> *center,
                            unsigned int prim_index) const {
    BoundingBox(bmin, bmax, prim_index);
    for (int k = 0; k < 3; k++) {
      T c = static_cast<T>(0);
      for (int i = 0; i < 4; i++) c += vertices_[3 * (4 * prim_index + i) + k];
      (*center)[k] = c / static_cast<T>(4);
    }
  }

  const T *vertices_;
  const T *radii_;
};

// SAH split predicate (same convention as the legacy CurvePred).
template <typename T>
class Pred {
 public:
  explicit Pred(const T *vertices) : axis_(0), pos_(0), vertices_(vertices) {}

  void Set(int axis, T pos) const {
    axis_ = axis;
    pos_ = pos;
  }

  bool operator()(unsigned int i) const {
    T center = static_cast<T>(0);
    for (int j = 0; j < 4; j++)
      center += vertices_[3 * (4 * i + j) + axis_];
    center /= static_cast<T>(4);
    return center < pos_;
  }

 private:
  mutable int axis_;
  mutable T pos_;
  const T *vertices_;
};

// Hit record for the round tube primitive.
template <typename T>
class Intersection {
 public:
  Intersection() {}
  T t;
  unsigned int prim_id;
  T u;                       // curve parameter of the boundary sphere
  nanort::real3<T> position; // O + t D
  nanort::real3<T> normal;   // unit, from C(u) towards position
};

// Ray vs. swept-sphere-union intersector.  All mutable state is reset in
// PrepareTraversal, so one instance can be reused across interleaved queries
// (different directions, windows, misses) without stale candidates.
template <typename T>
class Intersector {
 public:
  Intersector(const T *vertices, const T *radii)
      : vertices_(vertices), radii_(radii) {}

  bool Intersect(T *t_inout, const unsigned int prim_index) const {
    if ((prim_index < trace_options_.prim_ids_range[0]) ||
        (prim_index >= trace_options_.prim_ids_range[1])) {
      return false;
    }
    if (prim_index == trace_options_.skip_prim_id) {
      return false;
    }

    double P[4][3], r[4];
    for (int i = 0; i < 4; i++) {
      for (int k = 0; k < 3; k++)
        P[i][k] = static_cast<double>(vertices_[12 * prim_index + 3 * i + k]);
      r[i] = static_cast<double>(radii_[4 * prim_index + i]);
    }

    detail::TubePolys tp;
    detail::buildTubePolys(org_, dir_, P, r, &tp);

    double t_limit = static_cast<double>(*t_inout);
    double t_best, u_best;
    if (detail::solveTube(tp, min_t_, t_limit,
                          trace_options_.cull_back_face, &t_best, &u_best)) {
      *t_inout = static_cast<T>(t_best);
      u_ = u_best;
      return true;
    }
    return false;
  }

  T GetT() const { return t_; }

  void Update(T t, unsigned int prim_idx) const {
    t_ = t;
    prim_id_ = prim_idx;
  }

  void PrepareTraversal(const nanort::Ray<T> &ray,
                        const nanort::BVHTraceOptions &trace_options) const {
    for (int k = 0; k < 3; k++) {
      org_[k] = static_cast<double>(ray.org[k]);
      dir_[k] = static_cast<double>(ray.dir[k]);
    }
    min_t_ = static_cast<double>(ray.min_t);
    trace_options_ = trace_options;
    // Reset per-traversal state: no stale candidate survives a reuse.
    t_ = ray.max_t;
    u_ = 0.0;
    prim_id_ = static_cast<unsigned int>(-1);
  }

  void PostTraversal(const nanort::Ray<T> &ray, bool hit,
                     Intersection<T> *isect) const {
    if (hit && isect) {
      unsigned int p = prim_id_;
      // Rebuild the curve polynomials for the hit primitive and evaluate the
      // centerline at the stored parameter u_.
      double P[4][3], r[4];
      for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 3; k++)
          P[i][k] = static_cast<double>(vertices_[12 * p + 3 * i + k]);
        r[i] = static_cast<double>(radii_[4 * p + i]);
      }
      detail::TubePolys tp;
      detail::buildTubePolys(org_, dir_, P, r, &tp);
      double C[3], ru;
      detail::evalCenterRadius(tp, u_, C, &ru);

      T t = t_;
      nanort::real3<T> pos, nrm;
      double n[3];
      for (int k = 0; k < 3; k++) {
        pos[k] = ray.org[k] + t * ray.dir[k];
        n[k] = static_cast<double>(pos[k]) - C[k];
      }
      double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (len > 0.0) {
        for (int k = 0; k < 3; k++) nrm[k] = static_cast<T>(n[k] / len);
      } else {
        nrm = nanort::real3<T>(static_cast<T>(0), static_cast<T>(0),
                               static_cast<T>(1));
      }

      (*isect).t = t;
      (*isect).u = static_cast<T>(u_);
      (*isect).prim_id = p;
      (*isect).position = pos;
      (*isect).normal = nrm;
    }
  }

  const T *vertices_;
  const T *radii_;
  mutable nanort::BVHTraceOptions trace_options_;

 private:
  mutable double org_[3];
  mutable double dir_[3];
  mutable double min_t_;
  mutable T t_;
  mutable double u_;
  mutable unsigned int prim_id_;
};

}  // namespace round_tube

#endif  // NANORT_EXAMPLES_CURVES_PRIMITIVE_ROUND_TUBE_H_
