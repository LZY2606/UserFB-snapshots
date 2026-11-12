//
// NanoRT bilinear patch (non-planar quad) primitive.
//
// This header adds a bilinearly interpolated quadrilateral primitive that
// plugs directly into nanort::BVHAccel::Build() / Traverse(), so non-planar
// quads no longer need to be split into two triangles along a fixed
// diagonal.
//
// Surface definition (corners are given per face in the order
// p00, p10, p11, p01):
//
//   P(u,v) = (1-u)(1-v) p00 + u(1-v) p10 + uv p11 + (1-u)v p01,
//   u, v in [0, 1].
//
// Each face may carry an optional parametric rectangular hole, stored as
// four bounds (u0, v0, u1, v1). The strict interior of the hole does not
// belong to the surface; its boundary still does.
//
// A ray may cross the same bilinear patch up to two times. The intersector
// first filters candidates that are outside the parameter domain, inside
// the hole, or outside the ray interval, and then selects the nearest
// valid one.
//
// See examples/bilinear_patch/README.md for the interface contract and the
// documented numerical degeneracy behaviour.
//
// No third-party runtime dependency is introduced; only nanort.h and the
// C++ standard library are required.
//

/*
The MIT License (MIT)

Copyright (c) 2015 - Present: Light Transport Entertainment Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef NANORT_PATCH_H_
#define NANORT_PATCH_H_

#include "nanort.h"

namespace nanort {

// Numerical tolerances used by the bilinear patch intersector.
//
// All of them are *relative* (scale-invariant) measures, so uniformly
// scaling the scene does not turn an ordinary patch into a "degenerate"
// one or vice versa.
//
// kBilinearPatchUVEpsilon<T>:
//   Absolute slack applied to the [0,1] parameter domain test. u and v are
//   dimensionless quantities in [0,1], so an absolute tolerance is a
//   relative tolerance here.
//
// kBilinearPatchDegenerateEpsilon<T>:
//   A candidate is rejected as degenerate when the two parametric
//   derivatives dP/du and dP/dv are (nearly) linearly dependent at the
//   candidate point, i.e. when
//       |du x dv| <= eps * |du| * |dv|
//   (sin of the angle between the derivatives is below eps), because no
//   reliable unit normal exists there. The same measure is used to detect
//   iso-parametric lines parallel to the ray and rays tangent to the
//   surface. Fully degenerate patches (zero area everywhere) and rays
//   lying in/coincident with the surface are rejected through these
//   relative tests.
template <typename T>
inline T kBilinearPatchUVEpsilon() {
  return static_cast<T>(16.0) * std::numeric_limits<T>::epsilon();
}

template <typename T>
inline T kBilinearPatchDegenerateEpsilon() {
  return static_cast<T>(64.0) * std::numeric_limits<T>::epsilon();
}

///
/// Bilinear patch (quad) geometry accessor. Usable as `Prim` template
/// argument of BVHAccel::Build().
///
/// Vertex layout follows TriangleMesh: `vertices` is a raw vertex array
/// with `vertex_stride_bytes` stride, `faces` stores 4 vertex indices per
/// patch in the order (p00, p10, p11, p01).
///
/// `trims` is optional (may be NULL). When given, it stores 4 values per
/// patch (u0, v0, u1, v1) describing a rectangular hole in parameter
/// space. The strict interior of the rectangle is excluded from the
/// surface; the boundary remains part of the surface. A face with
/// u0 >= u1 or v0 >= v1 has no hole.
///
template <typename T = float>
class BilinearPatchMesh {
 public:
  BilinearPatchMesh(const T *vertices, const unsigned int *faces,
                    const size_t vertex_stride_bytes, const T *trims = NULL)
      : vertices_(vertices),
        faces_(faces),
        vertex_stride_bytes_(vertex_stride_bytes),
        trims_(trims) {}

  /// Compute bounding box for `prim_index`th patch.
  /// A bilinear patch is contained in the convex hull of its four corners
  /// (the interpolation weights are non-negative and sum to one), so the
  /// corner AABB conservatively bounds the whole surface.
  void BoundingBox(real3<T> *bmin, real3<T> *bmax,
                   unsigned int prim_index) const {
    real3<T> p(get_vertex_addr(vertices_, faces_[4 * prim_index + 0],
                               vertex_stride_bytes_));
    (*bmin) = p;
    (*bmax) = p;
    for (unsigned int i = 1; i < 4; i++) {
      real3<T> q(get_vertex_addr(vertices_, faces_[4 * prim_index + i],
                                 vertex_stride_bytes_));
      for (int k = 0; k < 3; k++) {
        (*bmin)[k] = std::min((*bmin)[k], q[k]);
        (*bmax)[k] = std::max((*bmax)[k], q[k]);
      }
    }
  }

  void BoundingBoxAndCenter(real3<T> *bmin, real3<T> *bmax, real3<T> *center,
                            unsigned int prim_index) const {
    real3<T> p[4];
    for (unsigned int i = 0; i < 4; i++) {
      p[i] = real3<T>(get_vertex_addr<T>(
          vertices_, faces_[4 * prim_index + i], vertex_stride_bytes_));
    }
    for (int k = 0; k < 3; k++) {
      (*bmin)[k] = std::min(std::min(p[0][k], p[1][k]),
                            std::min(p[2][k], p[3][k]));
      (*bmax)[k] = std::max(std::max(p[0][k], p[1][k]),
                            std::max(p[2][k], p[3][k]));
    }
    *center = (p[0] + p[1] + p[2] + p[3]) * (static_cast<T>(1.0) /
                                             static_cast<T>(4.0));
  }

  const T *vertices_;
  const unsigned int *faces_;
  const size_t vertex_stride_bytes_;
  const T *trims_;

  //
  // Accessors
  //
  const T *GetVertices() const { return vertices_; }

  const unsigned int *GetFaces() const { return faces_; }

  size_t GetVertexStrideBytes() const { return vertex_stride_bytes_; }

  /// May return NULL (no per-face holes).
  const T *GetTrims() const { return trims_; }
};

///
/// SAH split predicate for bilinear patches. Usable as `Pred` template
/// argument of BVHAccel::Build(). Partitions by the centroid of the four
/// corner vertices, mirroring TriangleSAHPred.
///
template <typename T = float>
class BilinearPatchSAHPred {
 public:
  BilinearPatchSAHPred(const T *vertices, const unsigned int *faces,
                       size_t vertex_stride_bytes)
      : axis_(0),
        pos_(static_cast<T>(0.0)),
        vertices_(vertices),
        faces_(faces),
        vertex_stride_bytes_(vertex_stride_bytes) {}

  BilinearPatchSAHPred(const BilinearPatchSAHPred<T> &rhs)
      : axis_(rhs.axis_),
        pos_(rhs.pos_),
        vertices_(rhs.vertices_),
        faces_(rhs.faces_),
        vertex_stride_bytes_(rhs.vertex_stride_bytes_) {}

  void Set(int axis, T pos) const {
    axis_ = axis;
    pos_ = pos;
  }

  bool operator()(unsigned int i) const {
    int axis = axis_;
    T pos = pos_;

    T center = static_cast<T>(0.0);
    for (unsigned int k = 0; k < 4; k++) {
      center += get_vertex_addr<T>(vertices_, faces_[4 * i + k],
                                   vertex_stride_bytes_)[axis];
    }
    return (center < pos * static_cast<T>(4.0));
  }

 private:
  mutable int axis_;
  mutable T pos_;
  const T *vertices_;
  const unsigned int *faces_;
  const size_t vertex_stride_bytes_;
};

///
/// Intersection information for bilinear patch geometry.
///
template <typename T = float>
class BilinearPatchIntersection {
 public:
  // Parametric coordinates of the hit on the patch, usable for shading.
  T u;
  T v;

  // Unit geometric normal, normalize(dP/du x dP/dv), flipped if necessary
  // so that it faces against the ray direction.
  T normal[3];

  // Required member variables.
  T t;
  unsigned int prim_id;  // Index of the input face (not a BVH-internal id).
};

///
/// Ray/bilinear-patch intersector. Drop-in replacement for
/// TriangleIntersector usable with BVHAccel::Traverse().
///
/// The ray direction is used as given; it is never normalized, so the
/// reported `t` keeps the caller's parameterization (P = org + t * dir).
///
/// Solves the ray/patch system by projecting onto two axes perpendicular
/// to the ray direction, yielding a quadratic in v. Near-planar patches
/// (tiny quadratic coefficient) fall back to the linear equation, so no
/// NaN is produced and the finite root is not lost. Each root is then
/// filtered by: parameter domain, hole, ray interval [min_t, t_inout],
/// normal well-definedness, and (optionally) back-face culling; the
/// nearest surviving candidate wins.
///
template <typename T = float, class H = BilinearPatchIntersection<T> >
class BilinearPatchIntersector {
 public:
  // Initialize from a BilinearPatchMesh-like object (must provide
  // GetVertices()/GetFaces()/GetVertexStrideBytes()/GetTrims()).
  template <class M>
  BilinearPatchIntersector(const M &m)
      : vertices_(m.GetVertices()),
        faces_(m.GetFaces()),
        vertex_stride_bytes_(m.GetVertexStrideBytes()),
        trims_(m.GetTrims()) {}

  template <class M>
  BilinearPatchIntersector(const M *m)
      : vertices_(m->GetVertices()),
        faces_(m->GetFaces()),
        vertex_stride_bytes_(m->GetVertexStrideBytes()),
        trims_(m->GetTrims()) {}

  BilinearPatchIntersector(const T *vertices, const unsigned int *faces,
                           const size_t vertex_stride_bytes,
                           const T *trims = NULL)
      : vertices_(vertices),
        faces_(faces),
        vertex_stride_bytes_(vertex_stride_bytes),
        trims_(trims) {}

  /// Do ray intersection stuff for `prim_index`th patch and update hit
  /// distance `t_inout` when a nearer valid hit is found.
  /// Returns true if there's a valid intersection.
  bool Intersect(T *t_inout, const unsigned int prim_index) const {
    if ((prim_index < trace_options_.prim_ids_range[0]) ||
        (prim_index >= trace_options_.prim_ids_range[1])) {
      return false;
    }

    // Self-intersection test.
    if (prim_index == trace_options_.skip_prim_id) {
      return false;
    }

    const real3<T> p00(get_vertex_addr(vertices_, faces_[4 * prim_index + 0],
                                       vertex_stride_bytes_));
    const real3<T> p10(get_vertex_addr(vertices_, faces_[4 * prim_index + 1],
                                       vertex_stride_bytes_));
    const real3<T> p11(get_vertex_addr(vertices_, faces_[4 * prim_index + 2],
                                       vertex_stride_bytes_));
    const real3<T> p01(get_vertex_addr(vertices_, faces_[4 * prim_index + 3],
                                       vertex_stride_bytes_));

    // P(u,v) = p00 + u*b + v*c + u*v*e
    const real3<T> b = p10 - p00;
    const real3<T> c = p01 - p00;
    const real3<T> e = (p00 - p10) + (p11 - p01);

    const real3<T> w0 = ray_org_ - p00;

    // Two independent vectors perpendicular to the (unnormalized) ray
    // direction. Crossing with the smallest-magnitude axis keeps m well
    // away from zero since dir is non-zero.
    int kmin = 0;
    if (std::fabs(ray_dir_[1]) < std::fabs(ray_dir_[kmin])) kmin = 1;
    if (std::fabs(ray_dir_[2]) < std::fabs(ray_dir_[kmin])) kmin = 2;
    real3<T> axis(static_cast<T>(0.0), static_cast<T>(0.0),
                  static_cast<T>(0.0));
    axis[kmin] = static_cast<T>(1.0);
    const real3<T> m = vcross(ray_dir_, axis);
    const real3<T> n = vcross(ray_dir_, m);

    // Projecting w0 + t d = u b + v c + uv e onto m and n eliminates t:
    //   alpha_i = u (gamma_i + delta_i v) + beta_i v,  i = 1, 2
    const T alpha1 = vdot(m, w0), beta1 = vdot(m, c);
    const T gamma1 = vdot(m, b), delta1 = vdot(m, e);
    const T alpha2 = vdot(n, w0), beta2 = vdot(n, c);
    const T gamma2 = vdot(n, b), delta2 = vdot(n, e);

    // Eliminating u gives A v^2 + B v + C = 0.
    const T qa = beta2 * delta1 - beta1 * delta2;
    const T qb = alpha1 * delta2 - beta1 * gamma2 + beta2 * gamma1 -
                 alpha2 * delta1;
    const T qc = alpha1 * gamma2 - alpha2 * gamma1;

    // Solve for v. When the quadratic term vanishes (planar or
    // near-planar patch, or an affinely parameterized parallelogram) the
    // equation degrades to a linear one; the finite root is kept instead
    // of producing NaN.
    T vs[2];
    int num_roots = 0;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
    if (qa != static_cast<T>(0.0)) {
      const T disc = qb * qb - static_cast<T>(4.0) * qa * qc;
      if (disc < static_cast<T>(0.0)) {
        return false;
      }
      const T sq = std::sqrt(disc);
      // Numerically stable form (Numerical Recipes q-formula).
      const T q = -static_cast<T>(0.5) *
                  (qb + ((qb >= static_cast<T>(0.0)) ? sq : -sq));
      if (q != static_cast<T>(0.0)) {
        vs[0] = q / qa;
        vs[1] = qc / q;
      } else {
        // q == 0 implies qc == 0: roots are 0 and -qb/qa.
        vs[0] = static_cast<T>(0.0);
        vs[1] = (qb != static_cast<T>(0.0)) ? (-qb / qa)
                                            : static_cast<T>(0.0);
      }
      num_roots = 2;
    } else if (qb != static_cast<T>(0.0)) {
      vs[0] = -qc / qb;
      num_roots = 1;
    } else {
      // qa == qb == 0: either no solution or the ray lies in the surface
      // (a coincident-ray configuration). Both are rejected.
      return false;
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif

    const T uv_eps = kBilinearPatchUVEpsilon<T>();
    const T degen_eps = kBilinearPatchDegenerateEpsilon<T>();
    const T dd = vdot(ray_dir_, ray_dir_);

    T best_t = (*t_inout);
    T best_u = static_cast<T>(0.0);
    T best_v = static_cast<T>(0.0);
    real3<T> best_n(static_cast<T>(0.0), static_cast<T>(0.0),
                    static_cast<T>(0.0));
    bool found = false;

    for (int r = 0; r < num_roots; r++) {
      const T v = vs[r];

      // Filter: parameter domain.
      if (v < -uv_eps || v > static_cast<T>(1.0) + uv_eps) continue;

      // Recover u from the iso-v line P(u) = q + u s with
      // q = p00 + v c, s = b + v e, intersected with the ray:
      //   (w0 - v c) = u s - t d  =>  u = ((w0 - v c) x d) . (s x d) / |s x d|^2
      const real3<T> s = b + e * v;
      const real3<T> wv = w0 - c * v;
      const real3<T> sxd = vcross(s, ray_dir_);
      const T sxd2 = vdot(sxd, sxd);
      const T ss = vdot(s, s);
      // Reject when the iso-v line is (nearly) parallel to the ray: u is
      // not well defined there (scale-invariant sin^2 measure).
      if (sxd2 <= degen_eps * degen_eps * ss * dd) continue;
      const T u = vdot(vcross(wv, ray_dir_), sxd) / sxd2;

      // Filter: parameter domain.
      if (u < -uv_eps || u > static_cast<T>(1.0) + uv_eps) continue;

      // Filter: parametric hole. The strict interior of the hole is not
      // part of the surface; its boundary is.
      if (trims_ != NULL) {
        const T u0 = trims_[4 * prim_index + 0];
        const T v0 = trims_[4 * prim_index + 1];
        const T u1 = trims_[4 * prim_index + 2];
        const T v1 = trims_[4 * prim_index + 3];
        if ((u > u0) && (u < u1) && (v > v0) && (v < v1)) continue;
      }

      // Hit point and ray parameter (direction is used as given, so t
      // keeps the caller's parameterization).
      const real3<T> p = (p00 + b * u) + (c + e * u) * v;
      const T t = vdot(p - ray_org_, ray_dir_) / dd;

      // Filter: ray interval.
      if (t < t_min_ || t > best_t) continue;

      // Geometric normal from the parametric derivatives.
      const real3<T> du = b + e * v;
      const real3<T> dv = c + e * u;
      const real3<T> gn = vcross(du, dv);
      const T gn2 = vdot(gn, gn);
      // Reject when the derivatives are (nearly) linearly dependent: no
      // reliable unit normal exists at this candidate (scale-invariant
      // sin^2 measure). Other roots may still be valid.
      if (gn2 <= degen_eps * degen_eps * vdot(du, du) * vdot(dv, dv)) {
        continue;
      }
      // Reject rays (nearly) tangent to the surface at the candidate;
      // this also covers rays lying in/coincident with the surface.
      const T gnd = vdot(gn, ray_dir_);
      if (gnd * gnd <= degen_eps * degen_eps * gn2 * dd) continue;

      // Back-face culling: the front face is the side the geometric
      // normal dP/du x dP/dv points to.
      if (trace_options_.cull_back_face && (gnd > static_cast<T>(0.0))) {
        continue;
      }

      best_t = t;
      best_u = u;
      best_v = v;
      // Orient the unit normal against the ray direction.
      best_n = gn * (static_cast<T>(1.0) / std::sqrt(gn2));
      if (gnd > static_cast<T>(0.0)) {
        best_n = -best_n;
      }
      found = true;
    }

    if (!found) {
      return false;
    }

    (*t_inout) = best_t;
    u_ = best_u;
    v_ = best_v;
    normal_ = best_n;
    return true;
  }

  /// Returns the nearest hit distance.
  T GetT() const { return t_; }

  /// Update is called when initializing intersection and nearest hit is
  /// found.
  void Update(T t, unsigned int prim_idx) const {
    t_ = t;
    prim_id_ = prim_idx;
  }

  /// Prepare BVH traversal. Called once per Traverse(); resets all
  /// per-ray state so no candidate or parameter leaks between rays.
  void PrepareTraversal(const Ray<T> &ray,
                        const BVHTraceOptions &trace_options) const {
    ray_org_[0] = ray.org[0];
    ray_org_[1] = ray.org[1];
    ray_org_[2] = ray.org[2];

    ray_dir_[0] = ray.dir[0];
    ray_dir_[1] = ray.dir[1];
    ray_dir_[2] = ray.dir[2];

    trace_options_ = trace_options;

    t_min_ = ray.min_t;

    u_ = static_cast<T>(0.0);
    v_ = static_cast<T>(0.0);
    normal_ = real3<T>(static_cast<T>(0.0), static_cast<T>(0.0),
                       static_cast<T>(0.0));
  }

  /// Post BVH traversal stuff. Fill `isect` if there is a hit.
  void PostTraversal(const Ray<T> &ray, bool hit, H *isect) const {
    if (hit && isect) {
      (*isect).t = t_;
      (*isect).u = u_;
      (*isect).v = v_;
      (*isect).normal[0] = normal_[0];
      (*isect).normal[1] = normal_[1];
      (*isect).normal[2] = normal_[2];
      (*isect).prim_id = prim_id_;
    }
    (void)ray;
  }

 private:
  const T *vertices_;
  const unsigned int *faces_;
  const size_t vertex_stride_bytes_;
  const T *trims_;

  mutable real3<T> ray_org_;
  mutable real3<T> ray_dir_;
  mutable BVHTraceOptions trace_options_;
  mutable T t_min_;

  mutable T t_;
  mutable T u_;
  mutable T v_;
  mutable real3<T> normal_;
  mutable unsigned int prim_id_;
};

}  // namespace nanort

#endif  // NANORT_PATCH_H_
