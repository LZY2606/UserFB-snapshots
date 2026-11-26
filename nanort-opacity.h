//
// nanort-opacity.h
//
// Conservative opacity/alpha-tested triangle intersection for NanoRT.
//
// The geometry and BVH are unchanged.  An OpacityMap owns copied corner UVs
// and 8-bit alpha textures and builds a small recursive barycentric
// micro-triangle cache for every original primitive.  OpacityIntersector uses
// NanoRT's watertight candidate calculation, then either accepts/rejects a
// candidate from a deterministic cache region or performs one exact texel
// sample for a mixed region.
//

#ifndef NANORT_OPACITY_H_
#define NANORT_OPACITY_H_

#include "nanort.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace nanort {

// Conservative classification of one opacity micro-triangle.
enum OpacityRegion {
  OPACITY_REGION_TRANSPARENT = 0,
  OPACITY_REGION_OPAQUE = 1,
  OPACITY_REGION_MIXED = 2
};

template <typename T = float>
class OpacityMap {
 public:
  struct Texture {
    Texture() : width(0), height(0), cutoff(128) {}

    int width;
    int height;
    int cutoff;  // alpha >= cutoff is opaque.
    std::vector<unsigned char> alpha;
  };

  struct LeafInfo {
    LeafInfo()
        : region(OPACITY_REGION_MIXED),
          uv_min{static_cast<T>(0), static_cast<T>(0)},
          uv_max{static_cast<T>(0), static_cast<T>(0)} {}

    unsigned char region;
    T uv_min[2];
    T uv_max[2];
  };

  struct PrimitiveMap {
    PrimitiveMap()
        : material(0), depth(0), leaf_count(1), valid(false) {
      uv[0] = uv[1] = uv[2] = uv[3] = uv[4] = uv[5] = static_cast<T>(0);
    }

    // Six values: u0,v0,u1,v1,u2,v2.
    std::array<T, 6> uv;
    unsigned int material;
    int depth;
    std::size_t leaf_count;
    bool valid;
    std::vector<LeafInfo> leaves;
  };

  OpacityMap() = default;

  explicit OpacityMap(unsigned int num_primitives)
      : primitives_(num_primitives), material_ids_(num_primitives, 0) {}

  // Build one shared texture/material for every primitive.
  // `uv_corners` contains six T values per primitive unless
  // `uv_corner_stride_bytes` says otherwise.  A zero stride means tightly
  // packed two-component UV corners.
  bool Build(unsigned int num_primitives, const T *uv_corners,
             const unsigned char *alpha, int width, int height, int cutoff,
             int depth, std::size_t uv_corner_stride_bytes = 0) {
    if (!AlphaPointerValid(alpha, width, height) || num_primitives == 0) {
      return false;
    }

    Clear();
    primitives_.resize(num_primitives);
    material_ids_.assign(num_primitives, 0);

    Texture texture;
    texture.width = width;
    texture.height = height;
    texture.cutoff = ClampCutoff(cutoff);
    texture.alpha.assign(alpha,
                         alpha + static_cast<std::size_t>(width) * height);
    textures_.push_back(std::move(texture));

    for (unsigned int prim = 0; prim < num_primitives; ++prim) {
      CopyUVs(prim, uv_corners, uv_corner_stride_bytes,
              primitives_[prim].uv.data());
      BuildPrimitive(prim, 0, ClampDepth(depth));
    }

    return true;
  }

  // Build with one texture per primitive.  A null entry in `alphas` makes
  // that primitive fully opaque.
  bool BuildPerPrimitive(unsigned int num_primitives, const T *uv_corners,
                         const unsigned char *const *alphas, const int *widths,
                         const int *heights, const int *cutoffs, int depth,
                         std::size_t uv_corner_stride_bytes = 0) {
    if (num_primitives == 0 || !alphas || !widths || !heights || !cutoffs) {
      return false;
    }

    Clear();
    primitives_.resize(num_primitives);
    material_ids_.resize(num_primitives);

    for (unsigned int prim = 0; prim < num_primitives; ++prim) {
      material_ids_[prim] = prim;
      Texture texture;
      texture.width = std::max(1, widths[prim]);
      texture.height = std::max(1, heights[prim]);
      texture.cutoff = ClampCutoff(cutoffs[prim]);

      const std::size_t texel_count =
          static_cast<std::size_t>(texture.width) * texture.height;
      if (alphas[prim]) {
        texture.alpha.assign(alphas[prim], alphas[prim] + texel_count);
      } else {
        texture.alpha.assign(texel_count, 255);
      }
      textures_.push_back(std::move(texture));

      CopyUVs(prim, uv_corners, uv_corner_stride_bytes,
              primitives_[prim].uv.data());
      BuildPrimitive(prim, prim, ClampDepth(depth));
    }

    return true;
  }

  // Copy and build one primitive.  This is useful when different faces are
  // added incrementally.  The map is resized to `prim + 1`.
  bool BuildPrimitive(unsigned int prim, const T uv_corners[6],
                      const unsigned char *alpha, int width, int height,
                      int cutoff, int depth) {
    if (!AlphaPointerValid(alpha, width, height)) {
      return false;
    }

    if (primitives_.size() <= prim) {
      primitives_.resize(prim + 1);
      material_ids_.resize(prim + 1, 0);
    }

    Texture texture;
    texture.width = width;
    texture.height = height;
    texture.cutoff = ClampCutoff(cutoff);
    texture.alpha.assign(alpha,
                         alpha + static_cast<std::size_t>(width) * height);

    const unsigned int material = static_cast<unsigned int>(textures_.size());
    textures_.push_back(std::move(texture));
    material_ids_[prim] = material;

    if (uv_corners) {
      std::copy(uv_corners, uv_corners + 6, primitives_[prim].uv.begin());
    } else {
      primitives_[prim].uv.fill(static_cast<T>(0));
    }
    BuildPrimitive(prim, material, ClampDepth(depth));
    return true;
  }

  // Replace one already-registered primitive and explicitly rebuild its
  // cache.  The new UV and texture are copied.
  bool RebuildPrimitive(unsigned int prim, const T uv_corners[6],
                        const unsigned char *alpha, int width, int height,
                        int cutoff, int depth) {
    if (prim >= primitives_.size()) {
      return BuildPrimitive(prim, uv_corners, alpha, width, height, cutoff,
                            depth);
    }
    if (!AlphaPointerValid(alpha, width, height)) {
      return false;
    }

    const unsigned int material = material_ids_[prim];
    if (material >= textures_.size()) {
      return false;
    }

    Texture &texture = textures_[material];
    texture.width = width;
    texture.height = height;
    texture.cutoff = ClampCutoff(cutoff);
    texture.alpha.assign(alpha,
                         alpha + static_cast<std::size_t>(width) * height);

    if (uv_corners) {
      std::copy(uv_corners, uv_corners + 6, primitives_[prim].uv.begin());
    }
    BuildPrimitive(prim, material, ClampDepth(depth));
    return true;
  }

  void Clear() {
    primitives_.clear();
    textures_.clear();
    material_ids_.clear();
  }

  bool IsValid(unsigned int prim) const {
    return prim < primitives_.size() && primitives_[prim].valid;
  }

  std::size_t GetPrimitiveCount() const { return primitives_.size(); }

  std::size_t GetLeafCount(unsigned int prim) const {
    if (prim >= primitives_.size()) return 0;
    return primitives_[prim].leaf_count;
  }

  int GetDepth(unsigned int prim) const {
    if (prim >= primitives_.size()) return 0;
    return primitives_[prim].depth;
  }

  unsigned char GetLeafRegion(unsigned int prim, std::size_t leaf) const {
    if (prim >= primitives_.size() || !primitives_[prim].valid ||
        leaf >= primitives_[prim].leaves.size()) {
      return OPACITY_REGION_MIXED;
    }
    return primitives_[prim].leaves[leaf].region;
  }

  const LeafInfo *GetLeafInfo(unsigned int prim, std::size_t leaf) const {
    if (prim >= primitives_.size() || !primitives_[prim].valid ||
        leaf >= primitives_[prim].leaves.size()) {
      return nullptr;
    }
    return &primitives_[prim].leaves[leaf];
  }

  // Return OPACITY_REGION_TRANSPARENT/OPAQUE/MIXED for the leaf containing
  // barycentric coordinates (`u`, `v`) in NanoRT's convention.
  unsigned char Classify(unsigned int prim, T u, T v) const {
    if (prim >= primitives_.size() || !primitives_[prim].valid) {
      // Safe fallback: force the intersector to perform an exact sample.
      return OPACITY_REGION_MIXED;
    }

    const PrimitiveMap &pm = primitives_[prim];
    const std::size_t leaf = FindLeaf(pm, u, v);
    if (leaf >= pm.leaves.size()) return OPACITY_REGION_MIXED;
    return pm.leaves[leaf].region;
  }

  // Exact texture sample at barycentric coordinates.  This is the single
  // implementation used for mixed regions and for reference comparisons.
  bool Sample(unsigned int prim, T u, T v) const {
    T uv_u, uv_v;
    if (!InterpolateUV(prim, u, v, &uv_u, &uv_v)) {
      return true;  // A missing map should not make geometry disappear.
    }
    if (prim >= material_ids_.size()) return true;
    const unsigned int material = material_ids_[prim];
    if (material >= textures_.size()) return true;
    return SampleTexture(textures_[material], uv_u, uv_v);
  }

  bool SampleUV(unsigned int prim, T uv_u, T uv_v) const {
    if (prim >= material_ids_.size()) return true;
    const unsigned int material = material_ids_[prim];
    if (material >= textures_.size()) return true;
    return SampleTexture(textures_[material], uv_u, uv_v);
  }

  bool InterpolateUV(unsigned int prim, T u, T v, T *uv_u, T *uv_v) const {
    if (prim >= primitives_.size() || !uv_u || !uv_v) return false;
    const std::array<T, 6> &q = primitives_[prim].uv;
    // Do not clamp the barycentric weight here: linear extrapolation followed
    // by UV clamping matches the specified sampler.  Valid hit barycentrics
    // are inside the triangle apart from tiny floating-point edge error.
    const T w0 = T(1) - u - v;
    *uv_u = w0 * q[0] + u * q[2] + v * q[4];
    *uv_v = w0 * q[1] + u * q[3] + v * q[5];
    return true;
  }

  const Texture *GetTexture(unsigned int prim) const {
    if (prim >= material_ids_.size()) return nullptr;
    const unsigned int material = material_ids_[prim];
    if (material >= textures_.size()) return nullptr;
    return &textures_[material];
  }

  static int ClampDepth(int depth) {
    return std::max(0, std::min(5, depth));
  }

  static int ClampCutoff(int cutoff) {
    return std::max(0, std::min(255, cutoff));
  }

 private:
  struct UV {
    UV(T u_ = 0, T v_ = 0) : u(u_), v(v_) {}
    T u, v;
  };

  std::vector<PrimitiveMap> primitives_;
  std::vector<Texture> textures_;
  std::vector<unsigned int> material_ids_;

  static bool AlphaPointerValid(const unsigned char *alpha, int width,
                               int height) {
    return alpha != nullptr && width > 0 && height > 0;
  }

  static T Clamp01(T x) {
    return std::max(static_cast<T>(0), std::min(static_cast<T>(1), x));
  }

  void CopyUVs(unsigned int prim, const T *uv_corners,
               std::size_t stride_bytes, T *out) const {
    if (!uv_corners) {
      std::fill(out, out + 6, static_cast<T>(0));
      return;
    }

    if (stride_bytes == 0) {
      const T *src = uv_corners + static_cast<std::size_t>(prim) * 6;
      std::copy(src, src + 6, out);
      return;
    }

    const unsigned char *base =
        reinterpret_cast<const unsigned char *>(uv_corners);
    for (int corner = 0; corner < 3; ++corner) {
      const std::size_t corner_index =
          static_cast<std::size_t>(prim) * 3 + corner;
      const T *src = reinterpret_cast<const T *>(
          base + corner_index * stride_bytes);
      out[2 * corner + 0] = src[0];
      out[2 * corner + 1] = src[1];
    }
  }

  void BuildPrimitive(unsigned int prim, unsigned int material, int depth) {
    PrimitiveMap &pm = primitives_[prim];
    pm.material = material;
    pm.depth = depth;
    // 4^depth recursive leaves.
    pm.leaf_count = static_cast<std::size_t>(1) << (2 * depth);
    pm.leaves.assign(pm.leaf_count, LeafInfo());
    pm.valid = material < textures_.size();
    if (!pm.valid) return;

    const UV uv0(pm.uv[0], pm.uv[1]);
    const UV uv1(pm.uv[2], pm.uv[3]);
    const UV uv2(pm.uv[4], pm.uv[5]);
    BuildRecursive(prim, material, 0, depth, 0, uv0, uv1, uv2);
  }

  void BuildRecursive(unsigned int prim, unsigned int material, int level,
                      int target_depth, std::size_t code, const UV &a,
                      const UV &b, const UV &c) {
    if (level == target_depth) {
      LeafInfo &leaf = primitives_[prim].leaves[code];
      leaf.region = ClassifyUVRegion(material, a, b, c, &leaf.uv_min[0],
                                     &leaf.uv_max[0]);
      return;
    }

    const UV ab((a.u + b.u) / T(2), (a.v + b.v) / T(2));
    const UV bc((b.u + c.u) / T(2), (b.v + c.v) / T(2));
    const UV ca((c.u + a.u) / T(2), (c.v + a.v) / T(2));

    BuildRecursive(prim, material, level + 1, target_depth, code * 4 + 0, a,
                   ab, ca);
    BuildRecursive(prim, material, level + 1, target_depth, code * 4 + 1, ab,
                   b, bc);
    BuildRecursive(prim, material, level + 1, target_depth, code * 4 + 2, ca,
                   bc, c);
    BuildRecursive(prim, material, level + 1, target_depth, code * 4 + 3, ab,
                   bc, ca);
  }

  unsigned char ClassifyUVRegion(unsigned int material, const UV &a,
                                 const UV &b, const UV &c, T *out_min,
                                 T *out_max) const {
    const Texture &texture = textures_[material];

    T umin = std::min(a.u, std::min(b.u, c.u));
    T umax = std::max(a.u, std::max(b.u, c.u));
    T vmin = std::min(a.v, std::min(b.v, c.v));
    T vmax = std::max(a.v, std::max(b.v, c.v));

    if (out_min) {
      out_min[0] = umin;
      out_min[1] = vmin;
    }
    if (out_max) {
      out_max[0] = umax;
      out_max[1] = vmax;
    }

    // Clamping UV is monotonic, so clamping the AABB endpoints gives the
    // clamped interval.  A few ULPs of outward inflation protect float
    // interpolation and recursive midpoint rounding.  The classification is
    // allowed to become MIXED too often, never incorrectly definite.
    const T eps = std::numeric_limits<T>::epsilon() * T(64) *
                  std::max({T(1), std::fabs(umin), std::fabs(umax),
                            std::fabs(vmin), std::fabs(vmax)});

    const T cu0 = Clamp01(umin);
    const T cu1 = Clamp01(umax);
    const T cv0 = Clamp01(vmin);
    const T cv1 = Clamp01(vmax);

    int x0 = static_cast<int>(std::floor((cu0 - eps) * T(texture.width)));
    int x1 = static_cast<int>(std::floor((cu1 + eps) * T(texture.width)));
    int y0 = static_cast<int>(std::floor((cv0 - eps) * T(texture.height)));
    int y1 = static_cast<int>(std::floor((cv1 + eps) * T(texture.height)));

    x0 = std::max(0, std::min(texture.width - 1, x0));
    x1 = std::max(0, std::min(texture.width - 1, x1));
    y0 = std::max(0, std::min(texture.height - 1, y0));
    y1 = std::max(0, std::min(texture.height - 1, y1));

    bool has_opaque = false;
    bool has_transparent = false;
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const unsigned char alpha =
            texture.alpha[static_cast<std::size_t>(y) * texture.width + x];
        if (static_cast<int>(alpha) >= texture.cutoff) {
          has_opaque = true;
        } else {
          has_transparent = true;
        }
        if (has_opaque && has_transparent) {
          return OPACITY_REGION_MIXED;
        }
      }
    }

    if (has_opaque) return OPACITY_REGION_OPAQUE;
    return OPACITY_REGION_TRANSPARENT;
  }

  static bool SampleTexture(const Texture &texture, T u, T v) {
    u = Clamp01(u);
    v = Clamp01(v);

    int x = static_cast<int>(std::floor(u * T(texture.width)));
    int y = static_cast<int>(std::floor(v * T(texture.height)));

    // u == 1 or v == 1 reaches width/height; clamp to the last texel.
    x = std::max(0, std::min(texture.width - 1, x));
    y = std::max(0, std::min(texture.height - 1, y));

    const unsigned char alpha =
        texture.alpha[static_cast<std::size_t>(y) * texture.width + x];
    return static_cast<int>(alpha) >= texture.cutoff;
  }

  static void NormalizeBary(T *a, T *b, T *c) {
    *a = std::max(T(0), *a);
    *b = std::max(T(0), *b);
    *c = std::max(T(0), *c);
    const T sum = *a + *b + *c;
    if (sum > T(0)) {
      const T inv = T(1) / sum;
      *a *= inv;
      *b *= inv;
      *c *= inv;
    } else {
      *a = T(1);
      *b = *c = T(0);
    }
  }

  std::size_t FindLeaf(const PrimitiveMap &pm, T bary_u, T bary_v) const {
    T a = T(1) - bary_u - bary_v;
    T b = bary_u;
    T c = bary_v;
    NormalizeBary(&a, &b, &c);

    std::size_t code = 0;
    for (int level = 0; level < pm.depth; ++level) {
      int child;
      if (a >= T(0.5)) {
        child = 0;
      } else if (b >= T(0.5)) {
        child = 1;
      } else if (c >= T(0.5)) {
        child = 2;
      } else {
        child = 3;
      }
      code = code * 4 + static_cast<std::size_t>(child);

      T na, nb, nc;
      switch (child) {
        case 0:
          na = 2 * a - 1;
          nb = 2 * b;
          nc = 2 * c;
          break;
        case 1:
          // Local vertex order: (a+b)/2, b, (b+c)/2.
          na = 2 * a;
          nb = 2 * b - 1;
          nc = 2 * c;
          break;
        case 2:
          // Local vertex order: (c+a)/2, (b+c)/2, c.
          na = 2 * a;
          nb = 2 * b;
          nc = 2 * c - 1;
          break;
        default:
          // Center order: ab, bc, ca.
          na = a + b - c;
          nb = b + c - a;
          nc = c + a - b;
          break;
      }

      // Rounding at a shared edge may produce a tiny negative weight.
      NormalizeBary(&na, &nb, &nc);
      a = na;
      b = nb;
      c = nc;
    }

    return code;
  }
};

// Triangle geometry with the same positional layout as TriangleMesh and an
// optional opacity cache.  BVH building only uses the inherited bounding-box
// methods; UV and texture data do not alter geometric screening.
template <typename T = float>
class OpacityMesh : public TriangleMesh<T> {
 public:
  OpacityMesh(const T *vertices, const unsigned int *faces,
              std::size_t vertex_stride_bytes,
              const OpacityMap<T> *opacity_map = nullptr)
      : TriangleMesh<T>(vertices, faces, vertex_stride_bytes),
        opacity_map_(opacity_map) {}

  OpacityMesh(const T *vertices, const unsigned int *faces,
              std::size_t vertex_stride_bytes, const T *uv_corners,
              const OpacityMap<T> *opacity_map = nullptr)
      : TriangleMesh<T>(vertices, faces, vertex_stride_bytes),
        uv_corners_(uv_corners),
        opacity_map_(opacity_map) {}

  const T *GetUVCorners() const { return uv_corners_; }
  const OpacityMap<T> *GetOpacityMap() const { return opacity_map_; }
  void SetOpacityMap(const OpacityMap<T> *map) { opacity_map_ = map; }

 private:
  const T *uv_corners_ = nullptr;
  const OpacityMap<T> *opacity_map_ = nullptr;
};

template <typename T = float, class H = TriangleIntersection<T> >
class OpacityIntersector : public TriangleIntersector<T, H> {
 public:
  typedef TriangleIntersector<T, H> Base;
  typedef typename Base::TriangleCandidate TriangleCandidate;

  mutable unsigned long long deterministic_hits;
  mutable unsigned long long alpha_samples;
  mutable unsigned long long opaque_region_hits;
  mutable unsigned long long transparent_region_rejections;
  mutable unsigned long long mixed_region_hits;

  OpacityIntersector(const T *vertices, const unsigned int *faces,
                     std::size_t vertex_stride_bytes,
                     const OpacityMap<T> *opacity_map)
      : Base(vertices, faces, vertex_stride_bytes),
        deterministic_hits(0),
        alpha_samples(0),
        opaque_region_hits(0),
        transparent_region_rejections(0),
        mixed_region_hits(0),
        opacity_map_(opacity_map) {}

  explicit OpacityIntersector(const OpacityMesh<T> &mesh)
      : Base(mesh.GetVertices(), mesh.GetFaces(),
             mesh.GetVertexStrideBytes()),
        deterministic_hits(0),
        alpha_samples(0),
        opaque_region_hits(0),
        transparent_region_rejections(0),
        mixed_region_hits(0),
        opacity_map_(mesh.GetOpacityMap()) {}

  explicit OpacityIntersector(const OpacityMesh<T> *mesh)
      : Base(mesh->GetVertices(), mesh->GetFaces(),
             mesh->GetVertexStrideBytes()),
        deterministic_hits(0),
        alpha_samples(0),
        opaque_region_hits(0),
        transparent_region_rejections(0),
        mixed_region_hits(0),
        opacity_map_(mesh ? mesh->GetOpacityMap() : nullptr) {}

  template <class M>
  OpacityIntersector(const M &mesh, const OpacityMap<T> *opacity_map)
      : Base(mesh),
        deterministic_hits(0),
        alpha_samples(0),
        opaque_region_hits(0),
        transparent_region_rejections(0),
        mixed_region_hits(0),
        opacity_map_(opacity_map) {}

  void SetOpacityMap(const OpacityMap<T> *map) { opacity_map_ = map; }
  const OpacityMap<T> *GetOpacityMap() const { return opacity_map_; }

  void ResetCounters() const {
    deterministic_hits = 0;
    alpha_samples = 0;
    opaque_region_hits = 0;
    transparent_region_rejections = 0;
    mixed_region_hits = 0;
  }

  unsigned long long GetDeterministicHits() const {
    return deterministic_hits;
  }
  unsigned long long GetDeterministicRegionHits() const {
    return deterministic_hits;
  }
  unsigned long long GetAlphaSamples() const { return alpha_samples; }
  unsigned long long GetExactAlphaSamples() const { return alpha_samples; }
  unsigned long long GetOpaqueRegionHits() const {
    return opaque_region_hits;
  }
  unsigned long long GetTransparentRegionRejections() const {
    return transparent_region_rejections;
  }

  // BVH traversal calls this once per query.
  void PrepareTraversal(const Ray<T> &ray,
                        const BVHTraceOptions &trace_options) const {
    Base::PrepareTraversal(ray, trace_options);
    ResetCounters();
  }

  bool Intersect(T *t_inout, const unsigned int prim_index) const {
    TriangleCandidate candidate;
    if (!this->ComputeCandidate(prim_index, *t_inout, &candidate)) {
      return false;
    }

    // Without a map, preserve ordinary triangle visibility.
    if (!opacity_map_ || !opacity_map_->IsValid(prim_index)) {
      this->CommitCandidate(candidate, t_inout);
      return true;
    }

    const unsigned char region =
        opacity_map_->Classify(prim_index, candidate.u, candidate.v);

    if (region == OPACITY_REGION_TRANSPARENT) {
      ++deterministic_hits;
      ++transparent_region_rejections;
      // Deliberately do not write `*t_inout` or the base u_/v_ state.
      return false;
    }

    if (region == OPACITY_REGION_OPAQUE) {
      ++deterministic_hits;
      ++opaque_region_hits;
      this->CommitCandidate(candidate, t_inout);
      return true;
    }

    ++mixed_region_hits;
    ++alpha_samples;
    if (!opacity_map_->Sample(prim_index, candidate.u, candidate.v)) {
      // Exact transparent texel: likewise leave the accepted hit untouched.
      ++transparent_region_rejections;
      return false;
    }

    this->CommitCandidate(candidate, t_inout);
    return true;
  }

 private:
  const OpacityMap<T> *opacity_map_;
};

typedef OpacityMesh<float> OpacityMeshf;
typedef OpacityMesh<double> OpacityMeshd;
typedef OpacityMap<float> OpacityMapf;
typedef OpacityMap<double> OpacityMapd;
typedef OpacityIntersector<float> OpacityIntersectorf;
typedef OpacityIntersector<double> OpacityIntersectord;

}  // namespace nanort

#endif  // NANORT_OPACITY_H_
