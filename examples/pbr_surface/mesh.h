#pragma once

#include <glm.hpp>

#include <cmath>
#include <vector>

/// Status of a surface frame computed by Mesh::computeSurfaceFrame().
enum class SurfaceFrameStatus {
  /// Full orthonormal frame is valid; a tangent-space normal map may be
  /// sampled with it.
  kValid,
  /// The geometric normal is usable, but no usable tangent frame exists at
  /// this hit point (no UVs, degenerate triangle area, degenerate UV
  /// mapping or degenerate projected tangent). The caller must ignore the
  /// normal map at this hit point and shade with `normal` only.
  kNoNormalMap,
  /// Even the geometric normal is degenerate. The frame content is
  /// undefined; the caller should emit a stable debug color.
  kInvalid
};

/// Orthonormal surface frame at a ray hit point, see
/// Mesh::computeSurfaceFrame().
template <typename T>
struct SurfaceFrame {
  glm::vec<3, T, glm::precision::highp> tangent;
  glm::vec<3, T, glm::precision::highp> bitangent;
  glm::vec<3, T, glm::precision::highp> normal;
  SurfaceFrameStatus status = SurfaceFrameStatus::kInvalid;

  /// True when the frame may be used to sample a tangent-space normal map.
  bool useNormalMap() const { return status == SurfaceFrameStatus::kValid; }
};

template <typename T>
struct Mesh {
  size_t num_vertices;
  unsigned num_faces;
  std::vector<T> vertices;
  std::vector<T> facevarying_normals;
  std::vector<unsigned int> faces;
  std::vector<T> facevarying_uvs;
  std::string name;
  void lerp(T dst[3], const T v0[3], const T v1[3], const T v2[3], float u,
            float v) const {
    dst[0] = (static_cast<T>(1.0) - u - v) * v0[0] + u * v1[0] + v * v2[0];
    dst[1] = (static_cast<T>(1.0) - u - v) * v0[1] + u * v1[1] + v * v2[1];
    dst[2] = (static_cast<T>(1.0) - u - v) * v0[2] + u * v1[2] + v * v2[2];
  }

  bool has_uvs() const { return facevarying_uvs.size() > 0; }
  glm::vec2 getTextureCoord(unsigned int face, T u, T v) const {
    T t0[3], t1[3], t2[3];
    t0[0] = facevarying_uvs[6 * face + 0];
    t0[1] = facevarying_uvs[6 * face + 1];
    t0[2] = T(0);

    t1[0] = facevarying_uvs[6 * face + 2];
    t1[1] = facevarying_uvs[6 * face + 3];
    t1[2] = T(0);

    t2[0] = facevarying_uvs[6 * face + 4];
    t2[1] = facevarying_uvs[6 * face + 5];
    t2[2] = T(0);

    T tcoord[3];
    lerp(tcoord, t0, t1, t2, u, v);

    return {tcoord[0], tcoord[1]};
  }

  /// Compute an orthonormal surface frame (tangent, bitangent, normal) at
  /// the hit point described by a face index and the barycentric
  /// coordinates (u, v) of the ray hit, from this face's three positions
  /// and three UVs.
  ///
  ///  * `normal` is the barycentric interpolation of the face-varying
  ///    normals, normalized (falling back to the geometric face normal
  ///    when no normals are stored or the interpolation degenerates).
  ///  * `tangent` points towards increasing u: it is the direction of the
  ///    u gradient on the triangle, re-orthogonalized against `normal` at
  ///    the hit point and normalized.
  ///  * `bitangent` is cross(normal, tangent), negated when needed so that
  ///    it points towards increasing v; this preserves the handedness of
  ///    mirrored UV islands.
  ///
  /// All degeneracy tests are scale-relative: they compare ratios of
  /// lengths (i.e. sines of angles), never absolute lengths, so the same
  /// model authored in millimetres or metres yields the same status. With
  /// kEpsilon = 1e-6 the frame is considered degenerate when:
  ///  * triangle area:     |e1 x e2|      <= kEpsilon * |e1| * |e2|
  ///  * UV determinant:    |det|          <= kEpsilon * |duv1| * |duv2|
  ///  * projected tangent: |T - N (N.T)|  <= kEpsilon * |T|
  ///
  /// The method is const and keeps no state: a degenerate hit never reuses
  /// the frame of a previous hit, and the mesh data is never modified.
  SurfaceFrame<T> computeSurfaceFrame(unsigned int face, T u, T v) const {
    using tvec2 = glm::vec<2, T, glm::precision::highp>;
    using tvec3 = glm::vec<3, T, glm::precision::highp>;

    // Scale-relative degeneracy threshold, see the interface doc above.
    const T kEpsilon = T(1e-6);

    SurfaceFrame<T> frame;

    const unsigned int i0 = faces[3 * face + 0];
    const unsigned int i1 = faces[3 * face + 1];
    const unsigned int i2 = faces[3 * face + 2];

    const tvec3 p0{vertices[3 * i0 + 0], vertices[3 * i0 + 1],
                   vertices[3 * i0 + 2]};
    const tvec3 p1{vertices[3 * i1 + 0], vertices[3 * i1 + 1],
                   vertices[3 * i1 + 2]};
    const tvec3 p2{vertices[3 * i2 + 0], vertices[3 * i2 + 1],
                   vertices[3 * i2 + 2]};

    const tvec3 e1 = p1 - p0;
    const tvec3 e2 = p2 - p0;

    // ---- geometric normal at the hit point ----------------------------
    tvec3 n(T(0));
    bool haveNormal = false;
    if (facevarying_normals.size() >= 9 * size_t(face) + 9) {
      const tvec3 n0{facevarying_normals[9 * face + 0],
                     facevarying_normals[9 * face + 1],
                     facevarying_normals[9 * face + 2]};
      const tvec3 n1{facevarying_normals[9 * face + 3],
                     facevarying_normals[9 * face + 4],
                     facevarying_normals[9 * face + 5]};
      const tvec3 n2{facevarying_normals[9 * face + 6],
                     facevarying_normals[9 * face + 7],
                     facevarying_normals[9 * face + 8]};
      const T w = T(1) - u - v;
      n = w * n0 + u * n1 + v * n2;
      // Normals are unit-less, so an absolute test on the interpolated
      // length is already scale-relative.
      if (glm::length(n) > kEpsilon) {
        n = glm::normalize(n);
        haveNormal = true;
      }
    }

    const tvec3 faceCross = glm::cross(e1, e2);
    const T crossLen = glm::length(faceCross);
    const T edgeScale = glm::length(e1) * glm::length(e2);
    // `!(a > b)` also catches NaNs: they are treated as degenerate.
    const bool areaDegenerate = !(crossLen > kEpsilon * edgeScale);

    if (!haveNormal && !areaDegenerate) {
      n = faceCross / crossLen;
      haveNormal = true;
    }
    if (!haveNormal) {
      frame.status = SurfaceFrameStatus::kInvalid;
      return frame;
    }
    frame.normal = n;

    // ---- tangent frame from the UV mapping -----------------------------
    if (facevarying_uvs.size() < 6 * size_t(face) + 6 || areaDegenerate) {
      frame.status = SurfaceFrameStatus::kNoNormalMap;
      return frame;
    }

    const tvec2 uv0{facevarying_uvs[6 * face + 0],
                    facevarying_uvs[6 * face + 1]};
    const tvec2 uv1{facevarying_uvs[6 * face + 2],
                    facevarying_uvs[6 * face + 3]};
    const tvec2 uv2{facevarying_uvs[6 * face + 4],
                    facevarying_uvs[6 * face + 5]};

    const tvec2 duv1 = uv1 - uv0;
    const tvec2 duv2 = uv2 - uv0;
    const T det = duv1.x * duv2.y - duv2.x * duv1.y;
    const T uvScale = glm::length(duv1) * glm::length(duv2);
    // Also catches uvScale == 0 (all UVs identical).
    if (!(std::fabs(det) > kEpsilon * uvScale)) {
      frame.status = SurfaceFrameStatus::kNoNormalMap;
      return frame;
    }

    // Directions of increasing u / v on the surface (the UV gradient
    // mapped through the triangle). Dividing by det keeps the direction
    // correct when the UV island is mirrored (det < 0).
    const tvec3 tRaw = (duv2.y * e1 - duv1.y * e2) / det;
    const tvec3 bRaw = (duv1.x * e2 - duv2.x * e1) / det;

    // Re-orthogonalize against the shading normal at the hit point.
    const tvec3 tProj = tRaw - n * glm::dot(n, tRaw);
    if (!(glm::length(tProj) > kEpsilon * glm::length(tRaw))) {
      frame.status = SurfaceFrameStatus::kNoNormalMap;
      return frame;
    }

    const tvec3 t = glm::normalize(tProj);
    tvec3 b = glm::cross(n, t);
    // Keep the bitangent aligned with increasing v, so mirrored UV islands
    // keep their handedness.
    if (glm::dot(b, bRaw) < T(0)) b = -b;

    frame.tangent = t;
    frame.bitangent = b;
    frame.status = SurfaceFrameStatus::kValid;
    return frame;
  }
};
