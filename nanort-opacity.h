//
// NanoRT opacity (alpha-tested) triangle extension.
//
// Adds a triangle intersector which understands a per-primitive 8-bit alpha
// texture with a cutoff. Geometry is still filtered by the regular BVH; the
// intersector only decides whether a geometric candidate is actually a
// surface.
//
// The decision uses a cached classification of the triangle's barycentric
// domain. The domain is recursively split into 4^level micro triangles
// (level = 0..5). Each micro triangle is cached as
//   * fully transparent -> candidates there are rejected without a texel read
//   * fully opaque      -> candidates there are accepted without a texel read
//   * mixed             -> the actual texel must be sampled
// The classification is conservative: a micro triangle is only marked
// transparent/opaque when every texel reachable from its UV footprint is of
// that kind. A UV bounding rectangle is used, so extra "mixed" entries are
// allowed but a wrongly skipped surface is not.
//
// The MIT License (MIT)
// Copyright (c) 2015 - Present: Light Transport Entertainment Inc.
//
#ifndef NANORT_OPACITY_H_
#define NANORT_OPACITY_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "nanort.h"

namespace nanort {

///
/// Conservative opacity cache for a triangle mesh.
///
/// The map owns copies of all per-primitive UVs and texels, so the caller may
/// release or reuse its buffers right after `Build()`. Changing material data
/// requires an explicit rebuild.
///
/// @tparam T Precision(float or double)
///
template <typename T = float>
class OpacityMap {
 public:
  enum RegionState {
    TRANSPARENT = 0,  ///< every reachable texel is below the cutoff
    OPAQUE = 1,       ///< every reachable texel is at or above the cutoff
    MIXED = 2         ///< reachable texels exist on both sides
  };

  /// Per-primitive material description used by `Build()`.
  struct PrimitiveDesc {
    PrimitiveDesc()
        : width(0), height(0), texels(0), cutoff(255) {
      for (int k = 0; k < 6; k++) {
        uv[k] = static_cast<T>(0);
      }
    }

    // uv0.u, uv0.v, uv1.u, uv1.v, uv2.u, uv2.v
    T uv[6];
    unsigned int width;
    unsigned int height;
    const unsigned char *texels;  ///< row major, width * height bytes
    unsigned char cutoff;         ///< surface when alpha >= cutoff
  };

  OpacityMap() : level_(0), num_primitives_(0) {}

  ///
  /// Build the micro-region cache. All UVs and texels are copied.
  ///
  /// @param[in] prims Per-primitive materials (num_primitives entries).
  /// @param[in] num_primitives Number of primitives.
  /// @param[in] level Recursive subdivision level in [0, 5]. 0 keeps the
  ///                  whole original triangle as one region.
  /// @return false when the level or the input is invalid.
  ///
  bool Build(const PrimitiveDesc *prims, unsigned int num_primitives,
             unsigned int level) {
    if (level > 5) {
      Reset();
      return false;
    }
    if (num_primitives > 0 && prims == 0) {
      Reset();
      return false;
    }

    level_ = level;
    num_primitives_ = num_primitives;

    prims_.clear();
    micro_.clear();
    texels_.clear();

    const unsigned int n = 1u << level;  // <= 32
    const std::size_t micros_per_prim =
        static_cast<std::size_t>(n) * static_cast<std::size_t>(n);

    prims_.resize(num_primitives);
    micro_.assign(micros_per_prim * static_cast<std::size_t>(num_primitives),
                  static_cast<unsigned char>(MIXED));

    // Pass 1: copy the material inputs. The cache owns its texels.
    for (unsigned int p = 0; p < num_primitives; p++) {
      Primitive &dst = prims_[p];
      const PrimitiveDesc &src = prims[p];
      if (src.width == 0 || src.height == 0) {
        // Degenerate texture size: nothing can be sampled. The valid flag
        // makes every query transparent without out-of-bounds reads.
        Reset();
        return false;
      }

      for (int k = 0; k < 6; k++) {
        dst.uv[k] = src.uv[k];
      }
      dst.width = src.width;
      dst.height = src.height;
      dst.cutoff = src.cutoff;
      dst.texel_offset = texels_.size();
      dst.micro_offset =
          micros_per_prim * static_cast<std::size_t>(p);

      const std::size_t texel_count =
          static_cast<std::size_t>(src.width) *
          static_cast<std::size_t>(src.height);
      if (texel_count > 0) {
        if (src.texels == 0) {
          Reset();
          return false;
        }
        texels_.insert(texels_.end(), src.texels, src.texels + texel_count);
      }
    }

    // Pass 2: classify each micro triangle from the owned texels.
    for (unsigned int p = 0; p < num_primitives; p++) {
      const std::size_t offset = prims_[p].micro_offset;
      for (unsigned int j = 0; j < n; j++) {
        const unsigned int cells_in_row = n - j;
        const unsigned int row_base = 2 * j * n - j * j;
        for (unsigned int i = 0; i < cells_in_row; i++) {
          const unsigned int lower = row_base + 2 * i;

          micro_[offset + lower] = ClassifyMicro(
              p, n, i, j, i + 1, j, i, j + 1);

          // The final cell of a row has no upper triangle: its (i+1, j+1)
          // lattice point would fall outside the barycentric domain.
          if (i + 1 < cells_in_row) {
            micro_[offset + lower + 1] = ClassifyMicro(
                p, n, i + 1, j, i, j + 1, i + 1, j + 1);
          }
        }
      }
    }

    return true;
  }

  bool IsBuilt() const { return !prims_.empty(); }
  unsigned int GetLevel() const { return level_; }
  unsigned int GetNumPrimitives() const { return num_primitives_; }

  /// Number of micro triangles per original primitive (4^level).
  unsigned int GetMicroTrianglesPerPrimitive() const {
    return (1u << level_) * (1u << level_);
  }

  /// UV of an original triangle corner (0..2).
  void GetVertexUv(unsigned int prim, unsigned int corner, T *u, T *v) const {
    (*u) = prims_[prim].uv[2 * corner + 0];
    (*v) = prims_[prim].uv[2 * corner + 1];
  }

  unsigned int GetTextureWidth(unsigned int prim) const {
    return prims_[prim].width;
  }
  unsigned int GetTextureHeight(unsigned int prim) const {
    return prims_[prim].height;
  }
  unsigned char GetCutoff(unsigned int prim) const {
    return prims_[prim].cutoff;
  }

  /// Alpha texel of a primitive (direct cache access; no clamping).
  unsigned char GetTexel(unsigned int prim, unsigned int x,
                         unsigned int y) const {
    const Primitive &m = prims_[prim];
    return texels_[m.texel_offset + static_cast<std::size_t>(y) * m.width + x];
  }

  ///
  /// Cached state of the `index`-th micro triangle. The index is the compact
  /// enumeration in [0, 4^level): each grid row contributes 2 * cells - 1
  /// triangles, lower first.
  ///
  RegionState GetMicroState(unsigned int prim, unsigned int index) const {
    return static_cast<RegionState>(micro_[prims_[prim].micro_offset + index]);
  }

  ///
  /// Interpolate the corner UVs at barycentric coordinates (`bu`, `bv`).
  /// `bu`/`bv` follow the intersector convention:
  /// interp = (1 - bu - bv) * p0 + bu * p1 + bv * p2.
  ///
  void InterpolateUv(unsigned int prim, T bu, T bv, T *su, T *sv) const {
    const Primitive &m = prims_[prim];
    const T b0 = static_cast<T>(1) - bu - bv;
    (*su) = b0 * m.uv[0] + bu * m.uv[2] + bv * m.uv[4];
    (*sv) = b0 * m.uv[1] + bu * m.uv[3] + bv * m.uv[5];
  }

  ///
  /// Exact texel sample at surface UV (`su`, `sv`). UV is clamped to [0, 1],
  /// the texel coordinate is floor(s * size) and then clamped to the last
  /// texel.
  ///
  unsigned char SampleTexel(unsigned int prim, T su, T sv) const {
    const Primitive &m = prims_[prim];
    if (m.width == 0 || m.height == 0) {
      return 0;
    }

    // NaN cannot name a texel; treat it like a hole (and like class MIXED,
    // which always goes through this exact sample).
    if (!(su == su) || !(sv == sv)) {
      return 0;
    }

    T cu = std::max(static_cast<T>(0), std::min(static_cast<T>(1), su));
    T cv = std::max(static_cast<T>(0), std::min(static_cast<T>(1), sv));

    unsigned int x = static_cast<unsigned int>(std::floor(cu * m.width));
    unsigned int y = static_cast<unsigned int>(std::floor(cv * m.height));
    if (x >= m.width) x = m.width - 1;
    if (y >= m.height) y = m.height - 1;

    return GetTexel(prim, x, y);
  }

  ///
  /// True when the exact sample at barycentric (`bu`, `bv`) is a surface.
  ///
  bool SampleOpaque(unsigned int prim, T bu, T bv) const {
    T su, sv;
    InterpolateUv(prim, bu, bv, &su, &sv);
    return SampleTexel(prim, su, sv) >= prims_[prim].cutoff;
  }

  ///
  /// Conservative classification of the micro region containing barycentric
  /// coordinates (`bu`, `bv`). The `bu`, `bv` and `bw = 1 - bu - bv` weights
  /// refer to corners 0, 1 and 2:
  /// interp = bw * p0 + bu * p1 + bv * p2.
  ///
  RegionState ClassifyBarycentric(unsigned int prim, T bu, T bv) const {
    const unsigned int n = 1u << level_;

    // Values this far outside the barycentric triangle cannot come from a
    // real sample of the watertight test; refuse to give a definite answer.
    const T outside_tol = std::numeric_limits<T>::epsilon() * static_cast<T>(64);
    if (!(bu >= -outside_tol) || !(bv >= -outside_tol) ||
        bu + bv > static_cast<T>(1) + outside_tol) {
      return MIXED;
    }

    T x = bu * static_cast<T>(n);
    T y = bv * static_cast<T>(n);

    // Keep the lookup inside the closed barycentric domain even when the
    // watertight test produces tiny rounding excursions. Whenever the
    // coordinates have to be moved, the region is no longer known to contain
    // the sample, so fall back to the exact texel read.
    bool snapped = false;
    if (!(x >= static_cast<T>(0)) || !(y >= static_cast<T>(0))) {
      snapped = true;
    }
    x = std::max(static_cast<T>(0), std::min(static_cast<T>(n), x));
    y = std::max(static_cast<T>(0), std::min(static_cast<T>(n), y));
    if (x + y > static_cast<T>(n)) {
      snapped = true;
      const T excess = x + y - static_cast<T>(n);
      if (x > y) {
        x -= excess;
      } else {
        y -= excess;
      }
      x = std::max(static_cast<T>(0), std::min(static_cast<T>(n), x));
      y = std::max(static_cast<T>(0), std::min(static_cast<T>(n), y));
    }
    if (snapped) {
      return MIXED;
    }

    unsigned int i = static_cast<unsigned int>(std::floor(x));
    unsigned int j = static_cast<unsigned int>(std::floor(y));
    if (i >= n) i = n - 1;
    if (j >= n) j = n - 1;
    if (i + j > n - 1) {
      // Only reachable through rounding on the far edge; the exact sample is
      // still safe to take.
      return MIXED;
    }

    const T fx = x - static_cast<T>(i);
    const T fy = y - static_cast<T>(j);

    bool upper = (fx + fy > static_cast<T>(1));
    // The last cell of a row has no upper triangle; anything on its diagonal
    // belongs to the lower one.
    if (upper && (i + j >= n - 1)) {
      upper = false;
    }

    const unsigned int row_base = 2 * j * n - j * j;
    const unsigned int index = row_base + 2 * i + (upper ? 1u : 0u);
    return static_cast<RegionState>(micro_[prims_[prim].micro_offset + index]);
  }

  ///
  /// UV corners of the `index`-th micro triangle, in the order
  /// (u0, v0, u1, v1, u2, v2). These are interpolated surface UVs.
  ///
  void GetMicroTriangleUv(unsigned int prim, unsigned int index,
                          T *uv6) const {
    const unsigned int n = 1u << level_;
    unsigned int i = 0;
    unsigned int j = 0;
    bool upper = false;
    DecodeMicroIndex(n, index, &i, &j, &upper);

    unsigned int c0[2];
    unsigned int c1[2];
    unsigned int c2[2];
    MicroCorners(i, j, upper, c0, c1, c2);

    LatticeUv(prim, n, c0[0], c0[1], &uv6[0], &uv6[1]);
    LatticeUv(prim, n, c1[0], c1[1], &uv6[2], &uv6[3]);
    LatticeUv(prim, n, c2[0], c2[1], &uv6[4], &uv6[5]);
  }

  /// Conservative UV bounding box of the micro triangle, clamped to [0, 1].
  /// Exposed for validation/debugging.
  void GetMicroTriangleUvBox(unsigned int prim, unsigned int index, T *umin,
                             T *vmin, T *umax, T *vmax) const {
    T uv6[6];
    GetMicroTriangleUv(prim, index, uv6);
    T lo_u = uv6[0];
    T lo_v = uv6[1];
    T hi_u = uv6[0];
    T hi_v = uv6[1];
    for (int c = 1; c < 3; c++) {
      lo_u = std::min(lo_u, uv6[2 * c + 0]);
      lo_v = std::min(lo_v, uv6[2 * c + 1]);
      hi_u = std::max(hi_u, uv6[2 * c + 0]);
      hi_v = std::max(hi_v, uv6[2 * c + 1]);
    }
    (*umin) = std::max(static_cast<T>(0), lo_u);
    (*vmin) = std::max(static_cast<T>(0), lo_v);
    (*umax) = std::min(static_cast<T>(1), hi_u);
    (*vmax) = std::min(static_cast<T>(1), hi_v);
  }

 private:
  struct Primitive {
    T uv[6];
    unsigned int width;
    unsigned int height;
    unsigned char cutoff;
    std::size_t texel_offset;
    std::size_t micro_offset;
  };

  void Reset() {
    prims_.clear();
    micro_.clear();
    texels_.clear();
    level_ = 0;
    num_primitives_ = 0;
  }

  static unsigned int TexelCoordD(double s, unsigned int size) {
    if (size == 0) {
      return 0;
    }
    double x = s * static_cast<double>(size);
    if (!(x > 0.0)) {
      return 0;
    }
    if (x >= static_cast<double>(size)) {
      return size - 1;
    }
    return static_cast<unsigned int>(x);
  }

  // Lattice point (i, j) has barycentric ((n-i-j)/n, i/n, j/n).
  // Computed in double so the conservative UV box does not suffer from the
  // caller's UV magnitude.
  void LatticeUvD(unsigned int prim, unsigned int n, unsigned int i,
                  unsigned int j, double *u, double *v) const {
    const Primitive &m = prims_[prim];
    const double inv_n = 1.0 / static_cast<double>(n);
    const double b0 = static_cast<double>(n - i - j) * inv_n;
    const double b1 = static_cast<double>(i) * inv_n;
    const double b2 = static_cast<double>(j) * inv_n;
    (*u) = b0 * static_cast<double>(m.uv[0]) +
           b1 * static_cast<double>(m.uv[2]) +
           b2 * static_cast<double>(m.uv[4]);
    (*v) = b0 * static_cast<double>(m.uv[1]) +
           b1 * static_cast<double>(m.uv[3]) +
           b2 * static_cast<double>(m.uv[5]);
  }

  void LatticeUv(unsigned int prim, unsigned int n, unsigned int i,
                 unsigned int j, T *u, T *v) const {
    double du, dv;
    LatticeUvD(prim, n, i, j, &du, &dv);
    (*u) = static_cast<T>(du);
    (*v) = static_cast<T>(dv);
  }

  static void DecodeMicroIndex(unsigned int n, unsigned int index,
                               unsigned int *i, unsigned int *j, bool *upper) {
    for (unsigned int row = 0; row < n; row++) {
      const unsigned int cells = n - row;
      const unsigned int count = 2 * cells - 1;
      if (index < count) {
        (*i) = index / 2;
        (*j) = row;
        (*upper) = (index % 2) != 0;
        return;
      }
      index -= count;
    }
    (*i) = 0;
    (*j) = 0;
    (*upper) = false;
  }

  static void MicroCorners(unsigned int i, unsigned int j, bool upper,
                           unsigned int *c0, unsigned int *c1,
                           unsigned int *c2) {
    if (!upper) {
      c0[0] = i;
      c0[1] = j;
      c1[0] = i + 1;
      c1[1] = j;
      c2[0] = i;
      c2[1] = j + 1;
    } else {
      c0[0] = i + 1;
      c0[1] = j;
      c1[0] = i;
      c1[1] = j + 1;
      c2[0] = i + 1;
      c2[1] = j + 1;
    }
  }

  // Classify one micro triangle from three lattice points. A UV bounding
  // rectangle is scanned, so the result may be MIXED more often than
  // strictly necessary but never wrongly definite.
  unsigned char ClassifyMicro(unsigned int prim, unsigned int n,
                              unsigned int ai, unsigned int aj,
                              unsigned int bi, unsigned int bj,
                              unsigned int ci, unsigned int cj) const {
    T u[3];
    T v[3];
    LatticeUv(prim, n, ai, aj, &u[0], &v[0]);
    LatticeUv(prim, n, bi, bj, &u[1], &v[1]);
    LatticeUv(prim, n, ci, cj, &u[2], &v[2]);

    // Non-finite UVs make no statement about which texel a sample reaches.
    if (!std::isfinite(static_cast<double>(u[0])) ||
        !std::isfinite(static_cast<double>(u[1])) ||
        !std::isfinite(static_cast<double>(u[2])) ||
        !std::isfinite(static_cast<double>(v[0])) ||
        !std::isfinite(static_cast<double>(v[1])) ||
        !std::isfinite(static_cast<double>(v[2]))) {
      return static_cast<unsigned char>(MIXED);
    }

    // The UVs of a micro triangle are an affine function of its barycentric
    // coordinates, so every real sample's UV lies inside this corner
    // bounding box (also for mirrored, repeated or degenerate mappings).
    T umin = std::min(u[0], std::min(u[1], u[2]));
    T umax = std::max(u[0], std::max(u[1], u[2]));
    T vmin = std::min(v[0], std::min(v[1], v[2]));
    T vmax = std::max(v[0], std::max(v[1], v[2]));

    const Primitive &m = prims_[prim];
    if (m.width == 0 || m.height == 0) {
      return static_cast<unsigned char>(TRANSPARENT);
    }

    // Widen the UV box by a small, magnitude-scaled margin before flooring.
    // The exact sample interpolates the corner UVs at a barycentric point
    // produced by the watertight test; both steps round. This margin covers
    // that rounding, so the texel range computed here always contains the
    // texels the exact sample can reach. Widening may add texels and turn a
    // definite region into MIXED, which is allowed; it can never hide an
    // opaque texel that a real sample reaches.
    double max_abs = 0.0;
    max_abs = std::max(max_abs, std::fabs(static_cast<double>(umin)));
    max_abs = std::max(max_abs, std::fabs(static_cast<double>(umax)));
    max_abs = std::max(max_abs, std::fabs(static_cast<double>(vmin)));
    max_abs = std::max(max_abs, std::fabs(static_cast<double>(vmax)));
    const double margin = static_cast<double>(std::numeric_limits<T>::epsilon()) *
                          static_cast<double>(64) * (1.0 + max_abs);

    double dumin = std::max(0.0, static_cast<double>(umin) - margin);
    double dvmin = std::max(0.0, static_cast<double>(vmin) - margin);
    double dumax = std::min(1.0, static_cast<double>(umax) + margin);
    double dvmax = std::min(1.0, static_cast<double>(vmax) + margin);

    const unsigned int x0 = TexelCoordD(dumin, m.width);
    const unsigned int x1 = TexelCoordD(dumax, m.width);
    const unsigned int y0 = TexelCoordD(dvmin, m.height);
    const unsigned int y1 = TexelCoordD(dvmax, m.height);

    bool has_opaque = false;
    bool has_transparent = false;

    for (unsigned int y = y0; y <= y1; y++) {
      for (unsigned int x = x0; x <= x1; x++) {
        const unsigned char a = GetTexel(prim, x, y);
        if (a >= m.cutoff) {
          has_opaque = true;
        } else {
          has_transparent = true;
        }
        if (has_opaque && has_transparent) {
          return static_cast<unsigned char>(MIXED);
        }
      }
    }

    return static_cast<unsigned char>(has_opaque ? OPAQUE : TRANSPARENT);
  }

  unsigned int level_;
  unsigned int num_primitives_;
  std::vector<Primitive> prims_;
  std::vector<unsigned char> micro_;  // num_prims * 4^level
  std::vector<unsigned char> texels_;
};

///
/// Triangle mesh with an attached opacity map.
///
/// Position/index layout is identical to `TriangleMesh` (including the
/// byte-stride vertex rule), so it can be passed directly to
/// `BVHAccel::Build`.
///
template <typename T = float>
class OpacityMesh {
 public:
  OpacityMesh(const T *vertices, const unsigned int *faces,
              const size_t vertex_stride_bytes, const OpacityMap<T> *opacity)
      : geometry_(vertices, faces, vertex_stride_bytes), opacity_(opacity) {}

  void BoundingBox(real3<T> *bmin, real3<T> *bmax,
                   unsigned int prim_index) const {
    geometry_.BoundingBox(bmin, bmax, prim_index);
  }

  void BoundingBoxAndCenter(real3<T> *bmin, real3<T> *bmax, real3<T> *center,
                            unsigned int prim_index) const {
    geometry_.BoundingBoxAndCenter(bmin, bmax, center, prim_index);
  }

  const T *GetVertices() const { return geometry_.GetVertices(); }
  const unsigned int *GetFaces() const { return geometry_.GetFaces(); }
  size_t GetVertexStrideBytes() const {
    return geometry_.GetVertexStrideBytes();
  }
  const OpacityMap<T> *GetOpacityMap() const { return opacity_; }

 private:
  TriangleMesh<T> geometry_;
  const OpacityMap<T> *opacity_;
};

///
/// Alpha-tested triangle intersector.
///
/// Reuses `TriangleIntersectorBase` for the watertight candidate test, so the
/// ray interval, back-face culling, `skip_prim_id`, `prim_ids_range` and the
/// padded vertex stride all behave exactly like `TriangleIntersector`. A
/// candidate is only committed after its material region accepts it, hence a
/// rejected candidate can never move the accepted distance, overwrite the
/// accepted barycentrics or terminate a shadow query.
///
/// @tparam T Precision(float or double)
/// @tparam H Intersection point information struct
///
template <typename T = float, class H = TriangleIntersection<T> >
class OpacityIntersector : public TriangleIntersectorBase<T, H> {
 public:
  typedef TriangleIntersectorBase<T, H> Base;
  typedef typename OpacityMap<T>::RegionState RegionState;

  template <class M>
  OpacityIntersector(const M &m)
      : Base(m),
        opacity_(m.GetOpacityMap()),
        definite_region_hits_(0),
        exact_alpha_samples_(0),
        accepted_hits_(0) {}

  template <class M>
  OpacityIntersector(const M *m)
      : Base(m),
        opacity_(m->GetOpacityMap()),
        definite_region_hits_(0),
        exact_alpha_samples_(0),
        accepted_hits_(0) {}

  OpacityIntersector(const T *vertices, const unsigned int *faces,
                     const size_t vertex_stride_bytes,
                     const OpacityMap<T> *opacity)
      : Base(vertices, faces, vertex_stride_bytes),
        opacity_(opacity),
        definite_region_hits_(0),
        exact_alpha_samples_(0),
        accepted_hits_(0) {}

  /// Test `prim_index` and commit it when the sampled material is a surface.
  bool Intersect(T *t_inout, const unsigned int prim_index) const {
    TriangleIntersectionCandidate<T> cand;
    if (!this->IntersectCandidate(prim_index, (*t_inout), &cand)) {
      return false;
    }

    // Without a built map nothing can be said about the material; treat the
    // surface as a hole rather than inventing an opaque hit. A primitive
    // outside the cached range has no material either.
    if (opacity_ == 0 || !opacity_->IsBuilt() ||
        prim_index >= opacity_->GetNumPrimitives()) {
      return false;
    }

    const RegionState state =
        opacity_->ClassifyBarycentric(prim_index, cand.u, cand.v);

    if (state == OpacityMap<T>::TRANSPARENT) {
      // Definite hit on a hole: reject without touching the hit state.
      ++definite_region_hits_;
      return false;
    }

    if (state == OpacityMap<T>::OPAQUE) {
      ++definite_region_hits_;
      ++accepted_hits_;
      (*t_inout) = cand.t;
      this->Commit(cand);
      return true;
    }

    // Mixed region: read the real texel and use the exact alpha.
    ++exact_alpha_samples_;
    if (!opacity_->SampleOpaque(prim_index, cand.u, cand.v)) {
      return false;
    }

    ++accepted_hits_;
    (*t_inout) = cand.t;
    this->Commit(cand);
    return true;
  }

  /// Reset per-query counters and traversal state.
  void PrepareTraversal(const Ray<T> &ray,
                        const BVHTraceOptions &trace_options) const {
    Base::PrepareTraversal(ray, trace_options);
    definite_region_hits_ = 0;
    exact_alpha_samples_ = 0;
    accepted_hits_ = 0;
  }

  /// Candidates whose micro region was definitively opaque or transparent.
  unsigned int GetDefiniteRegionHits() const { return definite_region_hits_; }

  /// Candidates in a mixed micro region that required an exact texel read.
  unsigned int GetExactAlphaSamples() const { return exact_alpha_samples_; }

  /// Candidates accepted as surfaces (definite opaque or mixed+opaque).
  unsigned int GetAcceptedHits() const { return accepted_hits_; }

  const OpacityMap<T> *GetOpacityMap() const { return opacity_; }

 private:
  const OpacityMap<T> *opacity_;

  mutable unsigned int definite_region_hits_;
  mutable unsigned int exact_alpha_samples_;
  mutable unsigned int accepted_hits_;
};

}  // namespace nanort

#endif  // NANORT_OPACITY_H_
