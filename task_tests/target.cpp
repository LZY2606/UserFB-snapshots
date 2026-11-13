//
// Tests for the NanoRT opacity (alpha-tested) triangle path.
//
// The references in this file are implemented independently of the cache:
// texel sampling, UV interpolation, micro-triangle geometry and ray/triangle
// intersection are all recomputed here so the tests do not just re-run the
// code under test.
//
#include "checks.hpp"

#include "nanort-opacity.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Small utilities.
// ---------------------------------------------------------------------------

unsigned int NextRandom(unsigned int *state) {
  *state = (*state) * 1664525u + 1013904223u;
  return (*state);
}

double Clamp01(double v) {
  return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

int TexelX(double s, int size) {
  if (size <= 0) return 0;
  double x = Clamp01(s) * static_cast<double>(size);
  int i = static_cast<int>(std::floor(x));
  if (i < 0) i = 0;
  if (i >= size) i = size - 1;
  return i;
}

// Independent texel reference: clamp UV, floor(u * width), floor(v * height),
// then clamp to the last texel.
unsigned char RefSample(const std::vector<unsigned char> &tex, int w, int h,
                        double u, double v) {
  const int x = TexelX(u, w);
  const int y = TexelX(v, h);
  return tex[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
             static_cast<std::size_t>(x)];
}

// Barycentric interpolation with corners 0, 1, 2 and weights
// (1 - bu - bv, bu, bv).
void RefInterpUv(const float uv[3][2], double bu, double bv, double *u,
                 double *v) {
  const double b0 = 1.0 - bu - bv;
  *u = b0 * uv[0][0] + bu * uv[1][0] + bv * uv[2][0];
  *v = b0 * uv[0][1] + bu * uv[1][1] + bv * uv[2][1];
}

struct Mat {
  std::vector<unsigned char> tex;
  int w;
  int h;
  unsigned char cutoff;
  float uv[3][2];
};

std::vector<unsigned char> Solid(int w, int h, unsigned char alpha) {
  return std::vector<unsigned char>(static_cast<std::size_t>(w) * h, alpha);
}

// Solid texture with one anomalous texel.
std::vector<unsigned char> SolidWithTexel(int w, int h, unsigned char alpha,
                                          int ax, int ay,
                                          unsigned char anomaly) {
  std::vector<unsigned char> t(
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h), alpha);
  t[static_cast<std::size_t>(ay) * static_cast<std::size_t>(w) +
    static_cast<std::size_t>(ax)] = anomaly;
  return t;
}

std::vector<unsigned char> RandomTexture(int w, int h, unsigned char cutoff,
                                         unsigned int *seed) {
  std::vector<unsigned char> t(static_cast<std::size_t>(w) * h);
  for (std::size_t i = 0; i < t.size(); i++) {
    t[i] = static_cast<unsigned char>(NextRandom(seed) & 0xFFu);
  }
  (void)cutoff;
  return t;
}

// Programmatic perforated texture: opaque with round holes on a grid.
std::vector<unsigned char> Perforated(int size, double phase, double radius,
                                      unsigned char cutoff) {
  std::vector<unsigned char> t(static_cast<std::size_t>(size) * size, 255);
  const int cells = 4;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      const double u = (x + 0.5) / static_cast<double>(size);
      const double v = (y + 0.5) / static_cast<double>(size);
      for (int cy = 0; cy < cells; cy++) {
        for (int cx = 0; cx < cells; cx++) {
          const double hx = (cx + 0.5 + phase) / static_cast<double>(cells);
          const double hy = (cy + 0.5 - phase) / static_cast<double>(cells);
          const double dx = (u - hx) * cells;
          const double dy = (v - hy) * cells;
          if (dx * dx + dy * dy < radius * radius) {
            t[static_cast<std::size_t>(y) * size + x] = 0;
          }
        }
      }
    }
  }
  (void)cutoff;
  return t;
}

// ---------------------------------------------------------------------------
// Scene description shared by the visibility and render tests.
// ---------------------------------------------------------------------------

struct FaceScene {
  std::vector<float> pos;
  std::vector<unsigned int> faces;
  std::vector<Mat> mats;  // one material per face

  unsigned int NumFaces() const {
    return static_cast<unsigned int>(faces.size() / 3);
  }
};

// Add an axis-aligned quad facing -z. The per-face material UVs follow the
// triangle's vertices.
void AddQuad(FaceScene *scene, float x0, float y0, float x1, float y1, float z,
             const std::vector<unsigned char> &tex, int w, int h,
             unsigned char cutoff) {
  const unsigned int base =
      static_cast<unsigned int>(scene->pos.size() / 3);
  const float p[4][3] = {{x0, y0, z}, {x1, y0, z}, {x0, y1, z}, {x1, y1, z}};
  const float t[4][2] = {
      {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};

  for (int i = 0; i < 4; i++) {
    for (int k = 0; k < 3; k++) scene->pos.push_back(p[i][k]);
  }

  // Counter clockwise from -z so the normal is -z.
  const unsigned int idx[6] = {0, 2, 1, 2, 3, 1};
  for (int f = 0; f < 2; f++) {
    unsigned int face[3];
    for (int k = 0; k < 3; k++) {
      face[k] = base + idx[3 * f + k];
      scene->faces.push_back(face[k]);
    }
    Mat m;
    m.tex = tex;
    m.w = w;
    m.h = h;
    m.cutoff = cutoff;
    for (int k = 0; k < 3; k++) {
      const unsigned int local = face[k] - base;
      m.uv[k][0] = t[local][0];
      m.uv[k][1] = t[local][1];
    }
    scene->mats.push_back(m);
  }
}

// A single triangle with explicit UVs.
void AddTriangle(FaceScene *scene, const float p[3][3], const float uv[3][2],
                 const std::vector<unsigned char> &tex, int w, int h,
                 unsigned char cutoff) {
  const unsigned int base =
      static_cast<unsigned int>(scene->pos.size() / 3);
  for (int k = 0; k < 3; k++) {
    for (int c = 0; c < 3; c++) scene->pos.push_back(p[k][c]);
    scene->faces.push_back(base + static_cast<unsigned int>(k));
  }
  Mat m;
  m.tex = tex;
  m.w = w;
  m.h = h;
  m.cutoff = cutoff;
  for (int k = 0; k < 3; k++) {
    m.uv[k][0] = uv[k][0];
    m.uv[k][1] = uv[k][1];
  }
  scene->mats.push_back(m);
}

std::vector<nanort::OpacityMap<float>::PrimitiveDesc> MakeDescs(
    const FaceScene &scene) {
  std::vector<nanort::OpacityMap<float>::PrimitiveDesc> descs(
      scene.NumFaces());
  for (unsigned int f = 0; f < scene.NumFaces(); f++) {
    const Mat &m = scene.mats[f];
    for (int k = 0; k < 3; k++) {
      descs[f].uv[2 * k + 0] = m.uv[k][0];
      descs[f].uv[2 * k + 1] = m.uv[k][1];
    }
    descs[f].width = static_cast<unsigned int>(m.w);
    descs[f].height = static_cast<unsigned int>(m.h);
    descs[f].texels = m.tex.empty() ? 0 : m.tex.data();
    descs[f].cutoff = m.cutoff;
  }
  return descs;
}

// ---------------------------------------------------------------------------
// Independent ray tracing reference.
// ---------------------------------------------------------------------------

struct RefHit {
  bool hit;
  double t;
  double u;
  double v;
  unsigned int prim;
  RefHit() : hit(false), t(0.0), u(0.0), v(0.0), prim(0) {}
};

RefHit RefTrace(const FaceScene &scene, const double org[3],
                const double dir[3], double tmin, double tmax) {
  RefHit best;
  best.t = tmax;
  const unsigned int num_faces = scene.NumFaces();

  for (unsigned int f = 0; f < num_faces; f++) {
    const float *p0 = &scene.pos[3 * scene.faces[3 * f + 0]];
    const float *p1 = &scene.pos[3 * scene.faces[3 * f + 1]];
    const float *p2 = &scene.pos[3 * scene.faces[3 * f + 2]];

    const double e1[3] = {static_cast<double>(p1[0]) - p0[0],
                          static_cast<double>(p1[1]) - p0[1],
                          static_cast<double>(p1[2]) - p0[2]};
    const double e2[3] = {static_cast<double>(p2[0]) - p0[0],
                          static_cast<double>(p2[1]) - p0[1],
                          static_cast<double>(p2[2]) - p0[2]};
    const double pv[3] = {dir[1] * e2[2] - dir[2] * e2[1],
                          dir[2] * e2[0] - dir[0] * e2[2],
                          dir[0] * e2[1] - dir[1] * e2[0]};
    const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::fabs(det) < 1e-20) continue;

    const double inv = 1.0 / det;
    const double tv[3] = {org[0] - p0[0], org[1] - p0[1], org[2] - p0[2]};
    const double bu = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
    if (bu < 0.0 || bu > 1.0) continue;

    const double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1],
                          tv[2] * e1[0] - tv[0] * e1[2],
                          tv[0] * e1[1] - tv[1] * e1[0]};
    const double bv = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * inv;
    if (bv < 0.0 || bu + bv > 1.0) continue;

    const double t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
    if (t < tmin || t > tmax || t >= best.t) continue;

    const Mat &m = scene.mats[f];
    double su, sv;
    RefInterpUv(m.uv, bu, bv, &su, &sv);
    if (RefSample(m.tex, m.w, m.h, su, sv) < m.cutoff) continue;

    best.hit = true;
    best.t = t;
    best.u = bu;
    best.v = bv;
    best.prim = f;
  }

  return best;
}

// ---------------------------------------------------------------------------
// Cache and traversal helpers.
// ---------------------------------------------------------------------------

struct Traced {
  bool hit;
  nanort::TriangleIntersection<float> isect;
  unsigned int definite;
  unsigned int exact;
  unsigned int accepted;

  Traced()
      : hit(false),
        definite(0),
        exact(0),
        accepted(0) {}
};

// Build the cache, the BVH and run one traversal. `stride_bytes` allows a
// padded vertex layout. The cache is copied into `out_map` so callers can
// inspect it after the traversal.
Traced TraceOnce(const FaceScene &scene, float stride_bytes,
                 unsigned int level, const nanort::Ray<float> &ray,
                 const nanort::BVHTraceOptions &options,
                 nanort::OpacityMap<float> *out_map) {
  std::vector<nanort::OpacityMap<float>::PrimitiveDesc> descs =
      MakeDescs(scene);

  std::vector<float> padded;
  const unsigned int stride_floats =
      static_cast<unsigned int>(stride_bytes / sizeof(float));
  if (stride_floats != 3) {
    padded.assign(static_cast<std::size_t>(scene.pos.size() / 3) *
                      stride_floats,
                  0.0f);
    for (std::size_t v = 0; v < scene.pos.size() / 3; v++) {
      for (int k = 0; k < 3; k++) {
        padded[v * stride_floats + k] = scene.pos[3 * v + k];
      }
      // Junk in the padding must be ignored.
      for (unsigned int k = 3; k < stride_floats; k++) {
        padded[v * stride_floats + k] = -12345.0f;
      }
    }
  }
  const float *positions =
      padded.empty() ? scene.pos.data() : padded.data();

  Traced out;
  out_map->Build(descs.data(), scene.NumFaces(), level);

  nanort::OpacityMesh<float> mesh(positions, scene.faces.data(), stride_bytes,
                                  out_map);
  nanort::TriangleSAHPred<float> pred(positions, scene.faces.data(),
                                      stride_bytes);
  nanort::BVHAccel<float> bvh;
  nanort::BVHBuildOptions<float> build_options;
  if (!bvh.Build(scene.NumFaces(), mesh, pred, build_options)) {
    throw std::runtime_error("BVH build failed");
  }

  nanort::OpacityIntersector<float> intersector(mesh);
  out.hit = bvh.Traverse(ray, intersector, &out.isect, options);
  out.definite = intersector.GetDefiniteRegionHits();
  out.exact = intersector.GetExactAlphaSamples();
  out.accepted = intersector.GetAcceptedHits();
  return out;
}

nanort::Ray<float> MakeRay(float ox, float oy, float oz, float dx, float dy,
                           float dz, float min_t, float max_t) {
  nanort::Ray<float> ray;
  ray.org[0] = ox;
  ray.org[1] = oy;
  ray.org[2] = oz;
  ray.dir[0] = dx;
  ray.dir[1] = dy;
  ray.dir[2] = dz;
  ray.min_t = min_t;
  ray.max_t = max_t;
  return ray;
}

// Enumerate the micro triangle lattice independently of the cache. Returns
// the three lattice coordinates (i, j) of the micro triangle.
void RefMicroCorners(unsigned int n, unsigned int index, unsigned int *ci,
                     unsigned int *cj) {
  unsigned int remaining = index;
  for (unsigned int row = 0; row < n; row++) {
    const unsigned int cells = n - row;
    const unsigned int count = 2 * cells - 1;
    if (remaining < count) {
      const unsigned int i = remaining / 2;
      const unsigned int j = row;
      const bool upper = (remaining % 2) != 0;
      if (!upper) {
        ci[0] = i; cj[0] = j;
        ci[1] = i + 1; cj[1] = j;
        ci[2] = i; cj[2] = j + 1;
      } else {
        ci[0] = i + 1; cj[0] = j;
        ci[1] = i; cj[1] = j + 1;
        ci[2] = i + 1; cj[2] = j + 1;
      }
      return;
    }
    remaining -= count;
  }
  throw std::runtime_error("micro index out of range");
}

// Interpolated surface UV of an independent lattice point.
void RefLatticeUv(const float uv[3][2], unsigned int n, unsigned int i,
                  unsigned int j, double *u, double *v) {
  RefInterpUv(uv, static_cast<double>(i) / n, static_cast<double>(j) / n, u,
              v);
}

enum RefState { REF_TRANSPARENT, REF_OPAQUE, REF_MIXED };

RefState RefClassifyBox(const Mat &m, double umin, double vmin, double umax,
                        double vmax) {
  umin = Clamp01(umin);
  vmin = Clamp01(vmin);
  umax = Clamp01(umax);
  vmax = Clamp01(vmax);
  const int x0 = TexelX(umin, m.w);
  const int x1 = TexelX(umax, m.w);
  const int y0 = TexelX(vmin, m.h);
  const int y1 = TexelX(vmax, m.h);

  bool any_opaque = false;
  bool any_transparent = false;
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      const unsigned char a =
          m.tex[static_cast<std::size_t>(y) * m.w + x];
      if (a >= m.cutoff) {
        any_opaque = true;
      } else {
        any_transparent = true;
      }
    }
  }
  if (any_opaque && any_transparent) return REF_MIXED;
  return any_opaque ? REF_OPAQUE : REF_TRANSPARENT;
}

// Check one primitive/UV mapping/level combination against the independent
// texel reference.
unsigned int CheckClassificationCase(Check &check, const float uv[3][2],
                                     const std::vector<unsigned char> &tex,
                                     int w, int h, unsigned char cutoff,
                                     unsigned int level,
                                     const char *label) {
  (void)label;
  nanort::OpacityMap<float>::PrimitiveDesc desc;
  for (int k = 0; k < 3; k++) {
    desc.uv[2 * k + 0] = uv[k][0];
    desc.uv[2 * k + 1] = uv[k][1];
  }
  desc.width = static_cast<unsigned int>(w);
  desc.height = static_cast<unsigned int>(h);
  desc.texels = tex.data();
  desc.cutoff = cutoff;

  nanort::OpacityMap<float> map;
  check(map.Build(&desc, 1, level), "classification: Build failed");
  check(map.GetMicroTrianglesPerPrimitive() == (1u << level) * (1u << level),
        "classification: micro count");

  Mat m;
  m.tex = tex;
  m.w = w;
  m.h = h;
  m.cutoff = cutoff;
  for (int k = 0; k < 3; k++) {
    m.uv[k][0] = uv[k][0];
    m.uv[k][1] = uv[k][1];
  }

  const unsigned int n = 1u << level;
  const unsigned int micros = n * n;
  unsigned int assertions = 0;

  for (unsigned int index = 0; index < micros; index++) {
    unsigned int ci[3];
    unsigned int cj[3];
    RefMicroCorners(n, index, ci, cj);

    double u[3];
    double v[3];
    for (int c = 0; c < 3; c++) {
      RefLatticeUv(uv, n, ci[c], cj[c], &u[c], &v[c]);
    }
    const double umin = std::min(u[0], std::min(u[1], u[2]));
    const double umax = std::max(u[0], std::max(u[1], u[2]));
    const double vmin = std::min(v[0], std::min(v[1], v[2]));
    const double vmax = std::max(v[0], std::max(v[1], v[2]));

    const RefState ref = RefClassifyBox(m, umin, vmin, umax, vmax);
    const nanort::OpacityMap<float>::RegionState state =
        map.GetMicroState(0, index);

    check(map.GetMicroState(0, index) !=
              static_cast<nanort::OpacityMap<float>::RegionState>(255),
          "classification: state initialized");
    ++assertions;

    if (ref == REF_MIXED) {
      check(state == nanort::OpacityMap<float>::MIXED,
            "classification: mixed bbox must be MIXED");
    } else if (ref == REF_OPAQUE) {
      check(state != nanort::OpacityMap<float>::TRANSPARENT,
            "classification: opaque bbox must not be TRANSPARENT");
    } else {
      check(state != nanort::OpacityMap<float>::OPAQUE,
            "classification: transparent bbox must not be OPAQUE");
    }
    ++assertions;

    // Sampling safety: the state must be valid for every real sample inside
    // the micro triangle, even with mirrored/degenerate UVs.
    const double bary[3][3] = {
        {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0},
        {0.6, 0.2, 0.2},
        {0.2, 0.6, 0.2}};
    for (int s = 0; s < 3; s++) {
      const double bu = bary[s][1];
      const double bv = bary[s][2];
      double su, sv;
      RefInterpUv(uv, bu, bv, &su, &sv);
      const unsigned char alpha = RefSample(tex, w, h, su, sv);
      const bool opaque = alpha >= cutoff;

      const nanort::OpacityMap<float>::RegionState q =
          map.ClassifyBarycentric(0, static_cast<float>(bu),
                                  static_cast<float>(bv));
      if (q == nanort::OpacityMap<float>::OPAQUE) {
        check(opaque, "classification: OPAQUE query sampled transparent");
      } else if (q == nanort::OpacityMap<float>::TRANSPARENT) {
        check(!opaque, "classification: TRANSPARENT query sampled opaque");
      }
      ++assertions;

      // The direct sample reference must match the map's own sampling.
      check(map.SampleOpaque(0, static_cast<float>(bu),
                             static_cast<float>(bv)) == opaque,
            "classification: exact sample reference mismatch");
      ++assertions;
    }
  }

  return assertions;
}

}  // namespace

// ---------------------------------------------------------------------------
// test_classification
// ---------------------------------------------------------------------------

void test_classification(Check &check) {
  // Level validation.
  {
    std::vector<unsigned char> tex = Solid(1, 1, 255);
    nanort::OpacityMap<float>::PrimitiveDesc desc;
    desc.uv[0] = 0.0f; desc.uv[1] = 0.0f;
    desc.uv[2] = 1.0f; desc.uv[3] = 0.0f;
    desc.uv[4] = 0.0f; desc.uv[5] = 1.0f;
    desc.width = 1;
    desc.height = 1;
    desc.texels = tex.data();
    desc.cutoff = 128;

    nanort::OpacityMap<float> map;
    check(map.Build(&desc, 1, 6) == false, "levels above 5 rejected");
    for (unsigned int level = 0; level <= 5; level++) {
      check(map.Build(&desc, 1, level), "levels 0..5 accepted");
      check(map.GetLevel() == level, "level round trips");
      check(map.GetMicroTrianglesPerPrimitive() ==
                (1u << level) * (1u << level),
            "micro count is 4^level");
    }
  }

  const float identity[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};

  // A solid texture must become fully definite, independent of level.
  for (unsigned int level = 0; level <= 5; level++) {
    std::vector<unsigned char> tex = Solid(8, 8, 255);
    nanort::OpacityMap<float>::PrimitiveDesc desc;
    for (int k = 0; k < 3; k++) {
      desc.uv[2 * k + 0] = identity[k][0];
      desc.uv[2 * k + 1] = identity[k][1];
    }
    desc.width = 8;
    desc.height = 8;
    desc.texels = tex.data();
    desc.cutoff = 128;

    nanort::OpacityMap<float> map;
    check(map.Build(&desc, 1, level), "solid build");
    const unsigned int micros = map.GetMicroTrianglesPerPrimitive();
    for (unsigned int i = 0; i < micros; i++) {
      check(map.GetMicroState(0, i) == nanort::OpacityMap<float>::OPAQUE,
            "solid texture is definitely opaque");
    }
    // Fully transparent solid too.
    std::vector<unsigned char> transparent = Solid(8, 8, 0);
    desc.texels = transparent.data();
    check(map.Build(&desc, 1, level), "transparent solid build");
    for (unsigned int i = 0; i < micros; i++) {
      check(map.GetMicroState(0, i) ==
                nanort::OpacityMap<float>::TRANSPARENT,
            "solid texture is definitely transparent");
    }
  }

  // One transparent texel inside an opaque texture must not be skipped. This
  // is the "corner/center sampling is not enough" case.
  {
    const int size = 64;
    const int ax = 31;
    const int ay = 33;
    std::vector<unsigned char> tex = SolidWithTexel(size, size, 255, ax, ay, 0);
    nanort::OpacityMap<float> map;
    nanort::OpacityMap<float>::PrimitiveDesc desc;
    for (int k = 0; k < 3; k++) {
      desc.uv[2 * k + 0] = identity[k][0];
      desc.uv[2 * k + 1] = identity[k][1];
    }
    desc.width = size;
    desc.height = size;
    desc.texels = tex.data();
    desc.cutoff = 128;
    check(map.Build(&desc, 1, 5), "single texel build");

    const double hu = (ax + 0.5) / size;
    const double hv = (ay + 0.5) / size;
    check(!map.SampleOpaque(0, static_cast<float>(hu), static_cast<float>(hv)),
          "single transparent texel samples transparent");
    check(map.ClassifyBarycentric(0, static_cast<float>(hu),
                                  static_cast<float>(hv)) !=
              nanort::OpacityMap<float>::OPAQUE,
          "single transparent texel is not classified opaque");

    unsigned int mixed = 0;
    const unsigned int micros = map.GetMicroTrianglesPerPrimitive();
    for (unsigned int i = 0; i < micros; i++) {
      if (map.GetMicroState(0, i) == nanort::OpacityMap<float>::MIXED) {
        ++mixed;
      }
    }
    check(mixed > 0, "single transparent texel creates a mixed region");
    check(mixed < micros, "single transparent texel does not poison all regions");
  }

  // Random textures and mappings, including mirrored, repeated, degenerate
  // and out-of-range UVs. This exercises the conservative invariant over the
  // whole desc.
  {
    unsigned int seed = 0x12345678u;
    std::vector<float> uv_cases[5];

    float normal[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    uv_cases[0].assign(normal, normal + 6);

    float mirrored[6] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    uv_cases[1].assign(mirrored, mirrored + 6);

    float repeated[6] = {0.25f, 0.75f, 0.9f, 0.2f, 0.25f, 0.75f};
    uv_cases[2].assign(repeated, repeated + 6);

    float degenerate[6] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    uv_cases[3].assign(degenerate, degenerate + 6);

    float wide[6] = {-1.2f, 0.3f, 2.3f, -0.7f, 0.4f, 1.8f};
    uv_cases[4].assign(wide, wide + 6);

    for (int c = 0; c < 5; c++) {
      for (unsigned int level = 0; level <= 5; level++) {
        const int w = 5;
        const int h = 7;
        std::vector<unsigned char> tex =
            RandomTexture(w, h, 128, &seed);
        const unsigned char cutoff =
            static_cast<unsigned char>(16 + (NextRandom(&seed) % 224));

        float uv[3][2];
        for (int k = 0; k < 3; k++) {
          uv[k][0] = uv_cases[c][2 * k + 0];
          uv[k][1] = uv_cases[c][2 * k + 1];
        }
        CheckClassificationCase(check, uv, tex, w, h, cutoff, level,
                                "random");
      }
    }
  }

  // A small random texture over the identity mapping, checking the exact
  // mixed/definite boundary at the finest level.
  {
    unsigned int seed = 0xABCDEF01u;
    const int w = 3;
    const int h = 2;
    std::vector<unsigned char> tex = RandomTexture(w, h, 128, &seed);
    CheckClassificationCase(check, identity, tex, w, h, 100, 5, "fine");
  }

  // Direct state query and the micro triangle UV accessor agree.
  {
    std::vector<unsigned char> tex = Solid(4, 4, 255);
    nanort::OpacityMap<float> map;
    nanort::OpacityMap<float>::PrimitiveDesc desc;
    for (int k = 0; k < 3; k++) {
      desc.uv[2 * k + 0] = identity[k][0];
      desc.uv[2 * k + 1] = identity[k][1];
    }
    desc.width = 4;
    desc.height = 4;
    desc.texels = tex.data();
    desc.cutoff = 128;
    check(map.Build(&desc, 1, 2), "uv accessor build");

    float uv6[6];
    map.GetMicroTriangleUv(0, 0, uv6);
    check(uv6[0] == 0.0f && uv6[1] == 0.0f, "micro uv origin");
    float umin, vmin, umax, vmax;
    map.GetMicroTriangleUvBox(0, 0, &umin, &vmin, &umax, &vmax);
    check(umin >= 0.0f && vmin >= 0.0f && umax <= 1.0f && vmax <= 1.0f,
          "micro uv box clamped");
    check(umax > umin || vmax > vmin, "micro uv box non-degenerate");
  }
}

// ---------------------------------------------------------------------------
// test_visibility
// ---------------------------------------------------------------------------

void test_visibility(Check &check) {
  // ---- Near transparent face, far opaque face. ----
  FaceScene scene;
  {
    const float near_p[3][3] = {
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    // The near face is fully transparent. The ray passes through it and the
    // far, opaque face must become the accepted hit.
    std::vector<unsigned char> tex = Solid(2, 2, 0);
    AddTriangle(&scene, near_p, uv, tex, 2, 2, 128);

    const float far_p[3][3] = {
        {-2.0f, -1.0f, 2.0f}, {2.0f, -1.0f, 2.0f}, {0.0f, 2.0f, 2.0f}};
    AddTriangle(&scene, far_p, uv, Solid(1, 1, 255), 1, 1, 128);
  }

  const unsigned int near_prim = 0;
  const unsigned int far_prim = 1;

  {
    const nanort::Ray<float> ray = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                           0.0f, 10.0f);
    nanort::OpacityMap<float> map;
    const Traced t = TraceOnce(scene, 12.0f, 3, ray, nanort::BVHTraceOptions(),
                               &map);
    check(t.hit, "visibility: far surface hit through the near hole");
    check(t.isect.prim_id == far_prim, "visibility: nearest entity is far face");
    check(std::fabs(t.isect.t - 2.0f) < 1e-5f, "visibility: far hit distance");
    check(std::fabs(t.isect.u - 1.0f / 3.0f) < 1e-5f,
          "visibility: far u preserved");
    check(std::fabs(t.isect.v - 1.0f / 3.0f) < 1e-5f,
          "visibility: far v preserved");
    check(t.definite == 2, "visibility: two definite region hits");
    check(t.exact == 0, "visibility: no exact samples for definite regions");
    check(t.accepted == 1, "visibility: exactly one accepted surface");
  }

  // Rejected near candidate must not move the accepted far distance or
  // barycentrics. Process the far triangle first and then the near one
  // through the public intersector interface.
  {
    const float *pos = scene.pos.data();
    nanort::OpacityMap<float> map;
    std::vector<nanort::OpacityMap<float>::PrimitiveDesc> descs =
        MakeDescs(scene);
    check(map.Build(descs.data(), scene.NumFaces(), 3), "manual build");

    nanort::OpacityMesh<float> mesh(pos, scene.faces.data(), 12, &map);
    nanort::OpacityIntersector<float> intersector(mesh);

    const nanort::Ray<float> ray = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                           0.0f, 10.0f);
    nanort::BVHTraceOptions options;
    intersector.PrepareTraversal(ray, options);

    float t_bound = ray.max_t;
    check(intersector.Intersect(&t_bound, far_prim),
          "visibility: far candidate accepted first");
    check(std::fabs(t_bound - 2.0f) < 1e-5f, "visibility: bound is far t");
    check(std::fabs(intersector.GetT() - 2.0f) < 1e-5f,
          "visibility: committed far t");

    nanort::TriangleIntersection<float> before;
    intersector.PostTraversal(ray, true, &before);
    const float far_u = before.u;
    const float far_v = before.v;
    const unsigned int far_id = before.prim_id;
    check(std::fabs(far_u - 1.0f / 3.0f) < 1e-5f &&
              std::fabs(far_v - 1.0f / 3.0f) < 1e-5f,
          "visibility: far barycentrics committed");

    // The near candidate is geometrically nearer, but lands on a hole.
    check(!intersector.Intersect(&t_bound, near_prim),
          "visibility: near hole rejected");
    check(std::fabs(t_bound - 2.0f) < 1e-5f,
          "visibility: rejection did not shrink nearest distance");
    check(std::fabs(intersector.GetT() - 2.0f) < 1e-5f,
          "visibility: rejection did not move committed t");

    nanort::TriangleIntersection<float> after;
    intersector.PostTraversal(ray, true, &after);
    check(after.u == far_u, "visibility: accepted u preserved after rejection");
    check(after.v == far_v, "visibility: accepted v preserved after rejection");
    check(after.prim_id == far_id,
          "visibility: accepted prim id preserved after rejection");
    check(intersector.GetDefiniteRegionHits() == 2,
          "visibility: rejection counted as definite");
    check(intersector.GetExactAlphaSamples() == 0,
          "visibility: definite rejection needed no sample");
    check(intersector.GetAcceptedHits() == 1, "visibility: one accepted");
  }

  // Finite shadow ray: the transparent near face must not terminate the
  // query; the opaque far face inside the finite range must occlude.
  {
    const nanort::Ray<float> shadow = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                              1.0f, 0.0f, 2.5f);
    nanort::OpacityMap<float> map;
    const Traced t =
        TraceOnce(scene, 12.0f, 3, shadow, nanort::BVHTraceOptions(), &map);
    check(t.hit, "visibility: shadow ray continues past rejected near face");
    check(t.isect.prim_id == far_prim, "visibility: shadow hit is far face");
    check(t.definite == 2, "visibility: shadow two definite candidates");
    check(t.accepted == 1, "visibility: shadow one accepted occluder");
  }

  // Same shadow ray, finite to just short of the far face: no occlusion, but
  // the near hole was still evaluated.
  {
    const nanort::Ray<float> shadow = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                              1.0f, 0.0f, 1.9f);
    nanort::OpacityMap<float> map;
    const Traced t =
        TraceOnce(scene, 12.0f, 3, shadow, nanort::BVHTraceOptions(), &map);
    check(!t.hit, "visibility: finite shadow ends before the far face");
    check(t.definite == 1, "visibility: only the near candidate was relevant");
    check(t.accepted == 0, "visibility: no accepted occluder");
  }

  // ---- Mixed region: exact alpha sampling. ----
  {
    FaceScene mixed_scene;
    const float p[3][3] = {
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    // 1x2: texel (0,1) is transparent; at uv (0.5, 0.5) the sample hits x=0,
    // y=1 and level 0 keeps the whole triangle MIXED.
    std::vector<unsigned char> tex = Solid(1, 2, 255);
    tex[1] = 0;
    AddTriangle(&mixed_scene, p, uv, tex, 1, 2, 128);

    const nanort::Ray<float> ray = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                           0.0f, 10.0f);
    nanort::OpacityMap<float> map;
    const Traced t = TraceOnce(mixed_scene, 12.0f, 0, ray,
                               nanort::BVHTraceOptions(), &map);
    check(!t.hit, "visibility: mixed sample rejects transparent texel");
    check(t.definite == 0, "visibility: mixed region is not definite");
    check(t.exact == 1, "visibility: mixed region reads one texel");
    check(t.accepted == 0, "visibility: mixed transparent not accepted");

    // Same ray against the opaque texel half: exact sample accepts.
    const nanort::Ray<float> ray2 = MakeRay(
        0.0f, 0.0f, 0.0f, 0.0f, -0.9f, 1.0f, 0.0f, 10.0f);
    const Traced t2 = TraceOnce(mixed_scene, 12.0f, 0, ray2,
                                nanort::BVHTraceOptions(), &map);
    check(t2.hit, "visibility: mixed opaque sample accepted");
    check(t2.exact == 1, "visibility: exact count for mixed accept");
    check(t2.accepted == 1, "visibility: mixed accepted count");
  }

  // ---- Solid texture uses only definite regions. ----
  {
    FaceScene solid_scene;
    const float p[3][3] = {
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&solid_scene, p, uv, Solid(16, 16, 255), 16, 16, 128);

    unsigned int rays = 0;
    for (int i = 0; i < 16; i++) {
      // The triangle spans x in [-0.45, 0.45] at y = 0.1.
      const float ox = -0.4f + 0.05f * i;
      const nanort::Ray<float> ray =
          MakeRay(ox, 0.1f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 10.0f);
      nanort::OpacityMap<float> map;
      const Traced t =
          TraceOnce(solid_scene, 12.0f, 4, ray, nanort::BVHTraceOptions(),
                    &map);
      check(t.hit, "visibility: solid ray hits");
      check(t.exact == 0, "visibility: solid texture never samples exactly");
      check(t.definite == 1, "visibility: solid texture definite hit");
      check(t.accepted == 1, "visibility: solid texture accepted");
      ++rays;
    }
    check(rays == 16, "visibility: solid ray count");
  }

  // ---- Trace options keep working. ----
  {
    FaceScene opt_scene;
    const float p[3][3] = {
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&opt_scene, p, uv, Solid(4, 4, 255), 4, 4, 128);
    const float near2_p[3][3] = {
        {-2.0f, -1.0f, 2.0f}, {2.0f, -1.0f, 2.0f}, {0.0f, 2.0f, 2.0f}};
    AddTriangle(&opt_scene, near2_p, uv, Solid(4, 4, 255), 4, 4, 128);

    const nanort::Ray<float> ray = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                           0.0f, 10.0f);

    {
      nanort::BVHTraceOptions options;
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(opt_scene, 12.0f, 2, ray, options, &map);
      check(t.hit && t.isect.prim_id == 0, "visibility: baseline near hit");
    }

    {
      nanort::BVHTraceOptions options;
      options.skip_prim_id = 0;
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(opt_scene, 12.0f, 2, ray, options, &map);
      check(t.hit && t.isect.prim_id == 1,
            "visibility: skip_prim_id selects the far face");
    }

    {
      nanort::BVHTraceOptions options;
      options.prim_ids_range[0] = 1;
      options.prim_ids_range[1] = 2;
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(opt_scene, 12.0f, 2, ray, options, &map);
      check(t.hit && t.isect.prim_id == 1,
            "visibility: prim_ids_range selects the far face");
    }

    {
      nanort::BVHTraceOptions options;
      options.prim_ids_range[0] = 0;
      options.prim_ids_range[1] = 1;
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(opt_scene, 12.0f, 2, ray, options, &map);
      check(t.hit && t.isect.prim_id == 0,
            "visibility: prim_ids_range limits to the near face");
    }

    {
      const nanort::Ray<float> clipped = MakeRay(
          0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.5f, 10.0f);
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(opt_scene, 12.0f, 2, clipped,
                                 nanort::BVHTraceOptions(), &map);
      check(t.hit && t.isect.prim_id == 1, "visibility: min_t skips near face");
    }

    {
      const nanort::Ray<float> clipped = MakeRay(
          0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.5f);
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(opt_scene, 12.0f, 2, clipped,
                                 nanort::BVHTraceOptions(), &map);
      check(t.hit && t.isect.prim_id == 0, "visibility: max_t clips far face");
    }
  }

  // ---- Back-face culling. ----
  {
    FaceScene cull_scene;
    const float front_p[3][3] = {
        {-1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}};
    AddTriangle(&cull_scene, front_p, uv, Solid(2, 2, 255), 2, 2, 128);

    const nanort::Ray<float> ray = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                           0.0f, 10.0f);
    {
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(cull_scene, 12.0f, 2, ray,
                                 nanort::BVHTraceOptions(), &map);
      check(t.hit, "visibility: front facing triangle hit");
    }
    {
      nanort::BVHTraceOptions options;
      options.cull_back_face = true;
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(cull_scene, 12.0f, 2, ray, options, &map);
      check(t.hit, "visibility: cull keeps front facing triangle");
    }

    FaceScene back_scene;
    const float back_p[3][3] = {
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    AddTriangle(&back_scene, back_p, uv, Solid(2, 2, 255), 2, 2, 128);
    {
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(back_scene, 12.0f, 2, ray,
                                 nanort::BVHTraceOptions(), &map);
      check(t.hit, "visibility: back facing triangle hit without culling");
    }
    {
      nanort::BVHTraceOptions options;
      options.cull_back_face = true;
      nanort::OpacityMap<float> map;
      const Traced t = TraceOnce(back_scene, 12.0f, 2, ray, options, &map);
      check(!t.hit, "visibility: cull drops back facing triangle");
    }
  }

  // ---- Padded vertex stride (16 bytes) behaves like 12 bytes. ----
  {
    nanort::Ray<float> ray = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                     10.0f);
    nanort::OpacityMap<float> packed_map;
    nanort::OpacityMap<float> padded_map;
    const Traced packed = TraceOnce(scene, 12.0f, 3, ray,
                                    nanort::BVHTraceOptions(), &packed_map);
    const Traced padded = TraceOnce(scene, 16.0f, 3, ray,
                                    nanort::BVHTraceOptions(), &padded_map);
    check(packed.hit && padded.hit, "visibility: padded stride hits");
    check(packed.isect.prim_id == padded.isect.prim_id,
          "visibility: padded stride same primitive");
    check(std::fabs(packed.isect.t - padded.isect.t) < 1e-6f,
          "visibility: padded stride same t");
    check(std::fabs(packed.isect.u - padded.isect.u) < 1e-6f,
          "visibility: padded stride same u");
    check(std::fabs(packed.isect.v - padded.isect.v) < 1e-6f,
          "visibility: padded stride same v");
  }

  // ---- The cache owns its data; rebuild is explicit. ----
  {
    FaceScene own_scene;
    const float p[3][3] = {
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    std::vector<unsigned char> tex = Solid(2, 2, 255);
    tex[3] = 0;
    AddTriangle(&own_scene, p, uv, tex, 2, 2, 128);

    nanort::OpacityMap<float> map;
    std::vector<nanort::OpacityMap<float>::PrimitiveDesc> descs =
        MakeDescs(own_scene);
    check(map.Build(descs.data(), own_scene.NumFaces(), 2), "ownership build");

    // Mutate the caller's texture after Build(): the cache must be unaffected.
    for (std::size_t i = 0; i < own_scene.mats[0].tex.size(); i++) {
      own_scene.mats[0].tex[i] = 255;
    }
    check(!map.SampleOpaque(0, 0.5f, 0.5f),
          "visibility: cache holds its own texels");

    // An explicit rebuild picks up the new data.
    descs = MakeDescs(own_scene);
    check(map.Build(descs.data(), own_scene.NumFaces(), 2), "rebuild");
    check(map.SampleOpaque(0, 0.5f, 0.5f), "visibility: rebuild sees new texels");
  }

  // ---- Mixed cell results agree with direct sampling at random points. ----
  {
    unsigned int seed = 0x0BADF00Du;
    FaceScene sample_scene;
    const float p[3][3] = {
        {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    std::vector<unsigned char> tex = RandomTexture(9, 9, 128, &seed);
    AddTriangle(&sample_scene, p, uv, tex, 9, 9, 117);

    nanort::OpacityMap<float> map;
    std::vector<nanort::OpacityMap<float>::PrimitiveDesc> descs =
        MakeDescs(sample_scene);
    check(map.Build(descs.data(), 1, 3), "mixed sampling build");

    for (int i = 0; i < 200; i++) {
      const double bu = (NextRandom(&seed) % 1000) / 1000.0;
      const double bv = (NextRandom(&seed) % 1000) / 1000.0 * (1.0 - bu);
      double su, sv;
      RefInterpUv(uv, bu, bv, &su, &sv);
      const bool expected =
          RefSample(tex, 9, 9, su, sv) >= static_cast<unsigned char>(117);
      check(map.SampleOpaque(0, static_cast<float>(bu),
                             static_cast<float>(bv)) == expected,
            "visibility: mixed sampling matches direct texel reference");
    }
  }

  // ---- Degenerate (zero-area) geometry is handled without spurious hits. ----
  {
    FaceScene degenerate_scene;
    const float p[3][3] = {
        {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}};
    const float uv[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    AddTriangle(&degenerate_scene, p, uv, Solid(4, 4, 255), 4, 4, 128);
    const nanort::Ray<float> ray = MakeRay(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                           0.0f, 10.0f);
    nanort::OpacityMap<float> map;
    const Traced t = TraceOnce(degenerate_scene, 12.0f, 3, ray,
                               nanort::BVHTraceOptions(), &map);
    check(!t.hit, "visibility: degenerate triangle is not hit");
  }

  // ---- Double precision path matches the float reference. ----
  {
    const double p[3][3] = {
        {-1.0, -1.0, 1.0}, {1.0, -1.0, 1.0}, {0.0, 1.0, 1.0}};
    const double uv[6] = {0.0, 0.0, 1.0, 0.0, 0.0, 1.0};
    std::vector<unsigned char> tex = Solid(2, 2, 255);
    tex[0] = 0;  // texel (0, 0) is a hole; the sample below hits (0, 1)

    std::vector<double> positions;
    for (int k = 0; k < 3; k++) {
      for (int c = 0; c < 3; c++) positions.push_back(p[k][c]);
    }
    const unsigned int faces[3] = {0, 1, 2};

    nanort::OpacityMap<double>::PrimitiveDesc desc;
    for (int k = 0; k < 6; k++) desc.uv[k] = uv[k];
    desc.width = 2;
    desc.height = 2;
    desc.texels = tex.data();
    desc.cutoff = 128;

    nanort::OpacityMap<double> map;
    check(map.Build(&desc, 1, 3), "visibility: double cache build");
    nanort::OpacityMesh<double> mesh(positions.data(), faces,
                                     sizeof(double) * 3, &map);
    nanort::TriangleSAHPred<double> pred(positions.data(), faces,
                                         sizeof(double) * 3);
    nanort::BVHAccel<double> bvh;
    nanort::BVHBuildOptions<double> build_options;
    check(bvh.Build(1, mesh, pred, build_options), "visibility: double BVH");

    nanort::Ray<double> ray;
    ray.org[0] = 0.25;
    ray.org[1] = 0.5;
    ray.org[2] = 0.0;
    ray.dir[0] = 0.0;
    ray.dir[1] = 0.0;
    ray.dir[2] = 1.0;
    ray.min_t = 0.0;
    ray.max_t = 10.0;

    nanort::OpacityIntersector<double> intersector(mesh);
    nanort::TriangleIntersection<double> isect;
    const bool hit = bvh.Traverse(ray, intersector, &isect,
                                  nanort::BVHTraceOptions());
    check(hit, "visibility: double hit");
    check(std::fabs(isect.t - 1.0) < 1e-12, "visibility: double t");
    check(std::fabs(isect.u - 0.25) < 1e-12, "visibility: double u");
    check(std::fabs(isect.v - 0.75) < 1e-12, "visibility: double v");
    // The sample at barycentric (0.25, 0.75) is UV (0.25, 0.75), i.e. texel
    // (0, 1), which is opaque; the hole at texel (0, 0) is outside the exact
    // sample. The opacity path must accept it.
    check(intersector.GetAcceptedHits() == 1,
          "visibility: double opaque texel accepted");
    check(intersector.GetDefiniteRegionHits() +
                  intersector.GetExactAlphaSamples() ==
              1,
          "visibility: double candidate accounted for");
  }
}

// ---------------------------------------------------------------------------
// test_render
// ---------------------------------------------------------------------------

namespace {

struct RenderPixel {
  bool hit;
  unsigned int layer;  // 0 front, 1 back, 2 wall
  bool lit;
};

// Brute-force reference for the layered scene, in double precision.
RenderPixel RefRenderPixel(const FaceScene &scene, const double org[3],
                           const double dir[3], const double light[3]) {
  RenderPixel px;
  px.hit = false;
  px.layer = 0;
  px.lit = false;

  const RefHit primary = RefTrace(scene, org, dir, 0.0, 1.0e30);
  if (!primary.hit) return px;

  px.hit = true;
  px.layer = (primary.prim < 2) ? 0u : ((primary.prim < 4) ? 1u : 2u);

  const double pos[3] = {org[0] + dir[0] * primary.t,
                         org[1] + dir[1] * primary.t,
                         org[2] + dir[2] * primary.t};
  const double to_light[3] = {light[0] - pos[0], light[1] - pos[1],
                              light[2] - pos[2]};
  const double dist = std::sqrt(to_light[0] * to_light[0] +
                                to_light[1] * to_light[1] +
                                to_light[2] * to_light[2]);
  if (dist <= 1e-9) {
    px.lit = true;
    return px;
  }
  const double sdir[3] = {to_light[0] / dist, to_light[1] / dist,
                          to_light[2] / dist};
  // Offset along the surface normal (-z) to avoid self intersection.
  const double sOrg[3] = {pos[0], pos[1], pos[2] - 1.0e-3};
  const RefHit shadow =
      RefTrace(scene, sOrg, sdir, 1.0e-4, dist - 1.0e-3);
  px.lit = !shadow.hit;
  return px;
}

}  // namespace

void test_render(Check &check) {
  FaceScene scene;
  const int tex_size = 16;
  AddQuad(&scene, -3.0f, -2.3f, 3.0f, 2.3f, 0.0f,
          Perforated(tex_size, 0.0, 0.62, 128), tex_size, tex_size, 128);
  AddQuad(&scene, -3.0f, -2.3f, 3.0f, 2.3f, 1.1f,
          Perforated(tex_size, 0.22, 0.55, 128), tex_size, tex_size, 128);
  AddQuad(&scene, -9.0f, -7.0f, 9.0f, 7.0f, 4.0f, Solid(1, 1, 255), 1, 1,
          128);

  const float *positions = scene.pos.data();
  std::vector<nanort::OpacityMap<float>::PrimitiveDesc> descs =
      MakeDescs(scene);
  nanort::OpacityMap<float> map;
  check(map.Build(descs.data(), scene.NumFaces(), 4), "render: cache build");

  nanort::OpacityMesh<float> mesh(positions, scene.faces.data(), 12, &map);
  nanort::TriangleSAHPred<float> pred(positions, scene.faces.data(), 12);
  nanort::BVHAccel<float> bvh;
  nanort::BVHBuildOptions<float> build_options;
  check(bvh.Build(scene.NumFaces(), mesh, pred, build_options),
        "render: BVH build");

  const int width = 96;
  const int height = 72;
  const double camera[3] = {0.0, 0.0, -4.0};
  const double light[3] = {2.4, 3.0, -1.6};
  const double tan_half = 0.62;
  const double aspect = static_cast<double>(width) / height;

  unsigned long long primary_definite = 0;
  unsigned long long primary_exact = 0;
  unsigned long long shadow_definite = 0;
  unsigned long long shadow_exact = 0;

  unsigned int layer_mismatch = 0;
  unsigned int shadow_mismatch = 0;
  unsigned int lit_wall = 0;
  unsigned int shadowed_wall = 0;
  unsigned int hits = 0;

  nanort::OpacityIntersector<float> intersector(mesh);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const double px =
          (2.0 * (x + 0.5) / width - 1.0) * tan_half * aspect;
      const double py = (1.0 - 2.0 * (y + 0.5) / height) * tan_half;
      const double dir[3] = {px, py, 1.0};

      RenderPixel ref = RefRenderPixel(scene, camera, dir, light);

      nanort::Ray<float> ray;
      ray.org[0] = static_cast<float>(camera[0]);
      ray.org[1] = static_cast<float>(camera[1]);
      ray.org[2] = static_cast<float>(camera[2]);
      ray.dir[0] = static_cast<float>(dir[0]);
      ray.dir[1] = static_cast<float>(dir[1]);
      ray.dir[2] = static_cast<float>(dir[2]);
      ray.min_t = 0.0f;
      ray.max_t = 1.0e30f;

      nanort::TriangleIntersection<float> isect;
      const bool hit = bvh.Traverse(ray, intersector, &isect,
                                    nanort::BVHTraceOptions());
      primary_definite += intersector.GetDefiniteRegionHits();
      primary_exact += intersector.GetExactAlphaSamples();

      if (hit != ref.hit) {
        ++layer_mismatch;
        continue;
      }
      if (!hit) continue;
      ++hits;

      const unsigned int layer = (isect.prim_id < 2)
                                     ? 0u
                                     : ((isect.prim_id < 4) ? 1u : 2u);
      if (layer != ref.layer) {
        ++layer_mismatch;
        continue;
      }

      const float t = isect.t;
      const float pos[3] = {
          static_cast<float>(camera[0]) + static_cast<float>(dir[0]) * t,
          static_cast<float>(camera[1]) + static_cast<float>(dir[1]) * t,
          static_cast<float>(camera[2]) + static_cast<float>(dir[2]) * t};

      const float to_light[3] = {static_cast<float>(light[0]) - pos[0],
                                 static_cast<float>(light[1]) - pos[1],
                                 static_cast<float>(light[2]) - pos[2]};
      const float dist = std::sqrt(to_light[0] * to_light[0] +
                                   to_light[1] * to_light[1] +
                                   to_light[2] * to_light[2]);
      if (dist <= 1e-9f) continue;

      nanort::Ray<float> shadow;
      shadow.org[0] = pos[0];
      shadow.org[1] = pos[1];
      shadow.org[2] = pos[2] - 1.0e-3f;
      shadow.dir[0] = to_light[0] / dist;
      shadow.dir[1] = to_light[1] / dist;
      shadow.dir[2] = to_light[2] / dist;
      shadow.min_t = 1.0e-4f;
      shadow.max_t = dist - 1.0e-3f;

      nanort::TriangleIntersection<float> shadow_isect;
      const bool occluded = bvh.Traverse(shadow, intersector, &shadow_isect,
                                         nanort::BVHTraceOptions());
      shadow_definite += intersector.GetDefiniteRegionHits();
      shadow_exact += intersector.GetExactAlphaSamples();

      if (occluded == ref.lit) {
        ++shadow_mismatch;
      }

      if (layer == 2) {
        if (ref.lit) {
          ++lit_wall;
        } else {
          ++shadowed_wall;
        }
      }
    }
  }

  const unsigned int pixels = static_cast<unsigned int>(width) * height;
  check(hits > pixels / 2, "render: most rays hit the scene");
  check(layer_mismatch <= (pixels + 49) / 50,
        "render: nearest entity matches the reference");
  check(shadow_mismatch <= (pixels + 49) / 50,
        "render: finite shadow lines match the reference");

  // The point light must actually produce holes in the shadow: both lit and
  // shadowed wall pixels exist because light passes through the front layers.
  check(lit_wall > 200, "render: light reaches the wall through the holes");
  check(shadowed_wall > 200, "render: layers still cast shadows");

  check(primary_definite > 0, "render: definite primary regions used");
  check(primary_exact > 0, "render: mixed primary regions sampled");
  check(shadow_definite > 0, "render: definite shadow regions used");
  check(shadow_exact > 0, "render: mixed shadow regions sampled");

  // Solid textures must never require an exact alpha read.
  {
    FaceScene solid_scene;
    const int size = 8;
    AddQuad(&solid_scene, -2.0f, -2.0f, 2.0f, 2.0f, 0.0f,
            Solid(size, size, 255), size, size, 128);
    std::vector<nanort::OpacityMap<float>::PrimitiveDesc> solid_descs =
        MakeDescs(solid_scene);
    nanort::OpacityMap<float> solid_map;
    check(solid_map.Build(solid_descs.data(), solid_scene.NumFaces(), 5),
          "render: solid cache build");
    nanort::OpacityMesh<float> solid_mesh(
        solid_scene.pos.data(), solid_scene.faces.data(), 12, &solid_map);
    nanort::TriangleSAHPred<float> solid_pred(
        solid_scene.pos.data(), solid_scene.faces.data(), 12);
    nanort::BVHAccel<float> solid_bvh;
    check(solid_bvh.Build(solid_scene.NumFaces(), solid_mesh, solid_pred,
                          build_options),
          "render: solid BVH build");

    nanort::OpacityIntersector<float> solid_intersector(solid_mesh);
    unsigned int ray_count = 0;
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 8; x++) {
        nanort::Ray<float> ray = MakeRay(
            -1.5f + 0.4f * x, -1.5f + 0.4f * y, -2.0f, 0.0f, 0.0f, 1.0f, 0.0f,
            10.0f);
        nanort::TriangleIntersection<float> s;
        const bool h = solid_bvh.Traverse(ray, solid_intersector, &s,
                                          nanort::BVHTraceOptions());
        check(h, "render: solid pixel hit");
        check(solid_intersector.GetExactAlphaSamples() == 0,
              "render: solid texture uses only definite regions");
        check(solid_intersector.GetDefiniteRegionHits() == 1,
              "render: solid texture one definite region hit");
        ++ray_count;
      }
    }
    check(ray_count == 64, "render: solid ray count");
  }

  // A cache built from the same scene at level 5 must still agree with the
  // brute-force reference, and a level-0 cache must produce the same visible
  // entities (mixed regions fall back to exact samples).
  for (unsigned int level = 0; level <= 5; level += 5) {
    nanort::OpacityMap<float> level_map;
    std::vector<nanort::OpacityMap<float>::PrimitiveDesc> level_descs =
        MakeDescs(scene);
    check(level_map.Build(level_descs.data(), scene.NumFaces(), level),
          "render: level cache build");
    nanort::OpacityMesh<float> level_mesh(
        scene.pos.data(), scene.faces.data(), 12, &level_map);
    nanort::OpacityIntersector<float> level_intersector(level_mesh);

    unsigned int mismatch = 0;
    for (int y = 0; y < height; y += 4) {
      for (int x = 0; x < width; x += 4) {
        const double px =
            (2.0 * (x + 0.5) / width - 1.0) * tan_half * aspect;
        const double py = (1.0 - 2.0 * (y + 0.5) / height) * tan_half;
        const double dir[3] = {px, py, 1.0};
        const RenderPixel ref = RefRenderPixel(scene, camera, dir, light);

        nanort::Ray<float> ray;
        ray.org[0] = 0.0f;
        ray.org[1] = 0.0f;
        ray.org[2] = -4.0f;
        ray.dir[0] = static_cast<float>(dir[0]);
        ray.dir[1] = static_cast<float>(dir[1]);
        ray.dir[2] = 1.0f;
        ray.min_t = 0.0f;
        ray.max_t = 1.0e30f;

        nanort::TriangleIntersection<float> isect;
        const bool hit =
            bvh.Traverse(ray, level_intersector, &isect,
                         nanort::BVHTraceOptions());
        if (hit != ref.hit) {
          ++mismatch;
          continue;
        }
        if (hit) {
          const unsigned int layer = (isect.prim_id < 2)
                                         ? 0u
                                         : ((isect.prim_id < 4) ? 1u : 2u);
          if (layer != ref.layer) ++mismatch;
        }
      }
    }
    check(mismatch <= 2, "render: level cache entity identity");
  }
}
