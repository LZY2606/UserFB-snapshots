#include "checks.hpp"
#include "nanort-opacity.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using nanort::BVHAccel;
using nanort::BVHBuildOptions;
using nanort::BVHTraceOptions;
using nanort::OpacityIntersector;
using nanort::OpacityMap;
using nanort::OpacityMesh;
using nanort::Ray;
using nanort::TriangleIntersection;
using nanort::TriangleSAHPred;

namespace {

struct PaddedFloatVertex {
  float x, y, z;
  float padding;
};

struct PaddedDoubleVertex {
  double x, y, z;
  double padding;
};

bool ReferenceAlpha(const unsigned char *tex, int width, int height,
                    int cutoff, double u, double v) {
  u = std::max(0.0, std::min(1.0, u));
  v = std::max(0.0, std::min(1.0, v));
  int x = static_cast<int>(std::floor(u * width));
  int y = static_cast<int>(std::floor(v * height));
  x = std::max(0, std::min(width - 1, x));
  y = std::max(0, std::min(height - 1, y));
  return tex[static_cast<size_t>(y) * width + x] >= cutoff;
}

template <typename T>
void TestClassificationPrecision(Check &check, const char *precision_name) {
  const int width = 13;
  const int height = 11;
  std::vector<unsigned char> tex(static_cast<size_t>(width) * height);
  std::mt19937 rng(static_cast<unsigned int>(sizeof(T) * 1009 + 17));
  std::uniform_int_distribution<int> byte_dist(0, 255);
  for (unsigned char &p : tex) p = static_cast<unsigned char>(byte_dist(rng));
  const int cutoff = 117;

  const T uv_layouts[][6] = {
      {T(0), T(0), T(1), T(0), T(0), T(1)},       // normal
      {T(1), T(0), T(0), T(1), T(1), T(1)},       // mirrored
      {T(.2f), T(.8f), T(.2f), T(.8f), T(.9f), T(.1f)}, // duplicate corner
      {T(.43f), T(.61f), T(.43f), T(.61f), T(.43f), T(.61f)} // degenerate
  };

  for (const auto &uvs : uv_layouts) {
    for (int depth = 0; depth <= 5; ++depth) {
      OpacityMap<T> map;
      bool built = map.Build(1, uvs, tex.data(), width, height, cutoff, depth);
      check(built, "opacity map built for float/double classification");
      check(map.GetLeafCount(0) == (size_t(1) << (2 * depth)),
            "recursive cache has 4^depth leaves");

      // Independently inspect the UV bounding rectangle recorded for every
      // leaf.  A definite label must not contain the opposite texel class.
      const size_t leaves = map.GetLeafCount(0);
      for (size_t leaf = 0; leaf < leaves; ++leaf) {
        const auto *info = map.GetLeafInfo(0, leaf);
        check(info != nullptr, "leaf info available");
        if (!info) continue;

        const double umin = std::min<double>(info->uv_min[0], info->uv_max[0]);
        const double umax = std::max<double>(info->uv_min[0], info->uv_max[0]);
        const double vmin = std::min<double>(info->uv_min[1], info->uv_max[1]);
        const double vmax = std::max<double>(info->uv_min[1], info->uv_max[1]);

        int x0 = std::max(0, static_cast<int>(std::floor(umin * width)));
        int x1 = std::min(width - 1,
                          static_cast<int>(std::floor(umax * width)));
        int y0 = std::max(0, static_cast<int>(std::floor(vmin * height)));
        int y1 = std::min(height - 1,
                          static_cast<int>(std::floor(vmax * height)));
        bool ref_opaque = false, ref_transparent = false;
        for (int y = y0; y <= y1; ++y) {
          for (int x = x0; x <= x1; ++x) {
            if (tex[static_cast<size_t>(y) * width + x] >= cutoff)
              ref_opaque = true;
            else
              ref_transparent = true;
          }
        }

        if (ref_opaque && ref_transparent) {
          check(info->region == nanort::OPACITY_REGION_MIXED,
                "UV rectangle accessing both classes must be mixed");
        }
        if (info->region == nanort::OPACITY_REGION_OPAQUE) {
          check(!ref_transparent, "opaque leaf cannot cover transparent texel");
        } else if (info->region == nanort::OPACITY_REGION_TRANSPARENT) {
          check(!ref_opaque,
                "transparent leaf cannot cover opaque texel");
        }
      }

      // Point queries exercise barycentric child selection and shared edges.
      std::uniform_real_distribution<double> dist(0.0, 1.0);
      for (int sample = 0; sample < 700; ++sample) {
        double bu = dist(rng);
        double bv = dist(rng);
        if (bu + bv > 1.0) {
          bu = 1.0 - bu;
          bv = 1.0 - bv;
        }
        const T tu = static_cast<T>(bu);
        const T tv = static_cast<T>(bv);
        const unsigned char region = map.Classify(0, tu, tv);

        T uu = 0, vv = 0;
        map.InterpolateUV(0, tu, tv, &uu, &vv);
        const bool expected = ReferenceAlpha(
            tex.data(), width, height, cutoff,
            static_cast<double>(uu), static_cast<double>(vv));
        const bool actual = map.Sample(0, tu, tv);
        check(actual == expected, "mixed/actual sample matches independent reference");

        if (region == nanort::OPACITY_REGION_OPAQUE) {
          check(expected, "definite opaque query is safe for real sampling");
        } else if (region == nanort::OPACITY_REGION_TRANSPARENT) {
          check(!expected,
                "definite transparent query is safe for real sampling");
        }
      }
    }
  }

  // Uniform textures must produce only definite leaves at every level.
  for (int depth = 0; depth <= 5; ++depth) {
    T uvs[6] = {T(0), T(0), T(1), T(0), T(0), T(1)};
    std::vector<unsigned char> white(64, 255);
    std::vector<unsigned char> black(64, 0);
    OpacityMap<T> opaque_map, transparent_map;
    check(opaque_map.Build(1, uvs, white.data(), 8, 8, 128, depth),
          "solid opaque map builds");
    check(transparent_map.Build(1, uvs, black.data(), 8, 8, 128, depth),
          "solid transparent map builds");
    for (size_t i = 0; i < opaque_map.GetLeafCount(0); ++i) {
      check(opaque_map.GetLeafRegion(0, i) ==
                nanort::OPACITY_REGION_OPAQUE,
            "solid opaque texture has only opaque regions");
      check(transparent_map.GetLeafRegion(0, i) ==
                nanort::OPACITY_REGION_TRANSPARENT,
            "solid transparent texture has only transparent regions");
    }
  }

  // Every isolated opaque texel in a transparent field must be detected near
  // its own sample point.  This is the corner/center-only failure mode.
  const int small = 7;
  for (int oy = 0; oy < small; ++oy) {
    for (int ox = 0; ox < small; ++ox) {
      std::vector<unsigned char> field(static_cast<size_t>(small) * small, 0);
      field[static_cast<size_t>(oy) * small + ox] = 255;
      T uvs[6] = {T(0), T(0), T(1), T(0), T(0), T(1)};
      OpacityMap<T> map;
      check(map.Build(1, uvs, field.data(), small, small, 1, 5),
            "isolated texel map builds");
      const double uu = (ox + 0.5) / small;
      const double vv = (oy + 0.5) / small;
      // The identity UV layout maps the barycentric triangle to u+v<=1.
      // Texels outside that UV preimage are not part of this triangle.
      if (uu + vv > 1.0) continue;
      const unsigned char region =
          map.Classify(0, static_cast<T>(uu), static_cast<T>(vv));
      check(region != nanort::OPACITY_REGION_TRANSPARENT,
            "isolated opaque texel must not be skipped as transparent");
      check(map.Sample(0, static_cast<T>(uu), static_cast<T>(vv)),
            "isolated opaque texel samples opaque");
    }
  }

  // Source memory is copied: mutating caller storage after Build cannot change
  // the cache's texture or UVs.
  T source_uvs[6] = {T(0), T(0), T(1), T(0), T(0), T(1)};
  std::vector<unsigned char> source_tex = {0, 255};
  OpacityMap<T> owned;
  check(owned.Build(1, source_uvs, source_tex.data(), 2, 1, 128, 0),
        "ownership map builds");
  check(!owned.Sample(0, T(.2), T(.2)), "original copied UV selects first texel");
  source_uvs[0] = source_uvs[1] = source_uvs[2] = source_uvs[3] = T(1);
  source_uvs[4] = source_uvs[5] = T(1);
  std::fill(source_tex.begin(), source_tex.end(), 0);
  check(!owned.Sample(0, T(.2), T(.2)),
        "copied texture/UV remain independent of reused caller memory");

  // Explicit rebuild is the supported way to replace copied UV/texture data.
  T new_uvs[6] = {T(0), T(0), T(1), T(0), T(0), T(1)};
  std::vector<unsigned char> white_tex(16, 255);
  check(owned.RebuildPrimitive(0, new_uvs, white_tex.data(), 4, 4, 255, 1),
        "explicit opacity rebuild succeeds");
  check(owned.SampleUV(0, T(.5), T(.5)), "rebuilt cutoff accepts alpha==cutoff");
  check(owned.Classify(0, T(.2), T(.2)) ==
            nanort::OPACITY_REGION_OPAQUE,
        "rebuilt solid texture has deterministic opaque region");

  // Exact cutoff and UV clamp conventions.
  unsigned char edge_tex_data[2] = {127, 128};
  T edge_uvs[6] = {T(0), T(0), T(1), T(0), T(0), T(1)};
  OpacityMap<T> edge_map;
  check(edge_map.Build(1, edge_uvs, edge_tex_data, 2, 1, 128, 0),
        "cutoff edge map builds");
  check(!edge_map.SampleUV(0, T(.25), T(.5)), "alpha below cutoff is transparent");
  check(edge_map.SampleUV(0, T(.75), T(.5)), "alpha equal to cutoff is opaque");
  check(!edge_map.SampleUV(0, T(-100), T(.5)), "negative U clamps to first texel");
  check(edge_map.SampleUV(0, T(100), T(.5)), "overflow U clamps to last texel");
  check(edge_map.SampleUV(0, T(1), T(.5)), "U=1 clamps floor(width) to last texel");

  (void)precision_name;
}

void TestSpecialBarycentricRetention(Check &check) {
  PaddedFloatVertex vertices[6] = {
      {-1.0f, -1.0f, 1.0f, 0.0f}, {3.0f, -1.0f, 1.0f, 0.0f},
      {-1.0f, 3.0f, 1.0f, 0.0f},
      {-2.0f, -2.0f, 1.0001f, 0.0f}, {2.0f, -2.0f, 1.0001f, 0.0f},
      {-2.0f, 2.0f, 1.0001f, 0.0f}};
  unsigned int faces[6] = {0, 1, 2, 3, 4, 5};
  float uvs[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};

  std::vector<unsigned char> transparent(16, 0);
  std::vector<unsigned char> opaque(16, 255);
  OpacityMap<float> map;
  check(map.BuildPrimitive(0, uvs + 0, transparent.data(), 4, 4, 128, 0),
        "near transparent map");
  check(map.BuildPrimitive(1, uvs + 6, opaque.data(), 4, 4, 128, 0),
        "far opaque map");

  Ray<float> ray;
  ray.org[0] = -0.5f;
  ray.org[1] = -0.5f;
  ray.org[2] = 0.0f;
  ray.dir[0] = ray.dir[1] = 0.0f;
  ray.dir[2] = 1.0f;
  ray.min_t = 0.0f;
  ray.max_t = 10.0f;
  BVHTraceOptions options;

  OpacityIntersector<float> intersector(&vertices[0].x, faces,
                                        sizeof(PaddedFloatVertex), &map);
  intersector.PrepareTraversal(ray, options);
  intersector.Update(ray.max_t, static_cast<unsigned int>(-1));

  // Deliberately accept the far candidate first, then test the closer face.
  float current_t = ray.max_t;
  check(intersector.Intersect(&current_t, 1), "far geometric candidate");
  const float accepted_far_t = current_t;
  intersector.Update(current_t, 1);

  float rejected_t = intersector.GetT();
  check(!intersector.Intersect(&rejected_t, 0),
        "transparent near candidate rejected after far acceptance");
  check(rejected_t == accepted_far_t,
        "rejected opacity candidate does not shrink nearest distance");
  check(intersector.GetT() == accepted_far_t,
        "intersector nearest t remains accepted far t");

  TriangleIntersection<float> committed;
  intersector.PostTraversal(ray, true, &committed);
  check(committed.prim_id == 1, "committed primitive is far face");
  check(std::fabs(committed.t - 1.0001f) < 1e-5f, "far t retained");
  check(std::fabs(committed.u - 0.375f) < 1e-5f,
        "accepted far u retained after near rejection");
  check(std::fabs(committed.v - 0.375f) < 1e-5f,
        "accepted far v retained after near rejection");
  check(intersector.deterministic_hits == 2,
        "direct sequence counts two deterministic regions");
  check(intersector.alpha_samples == 0,
        "solid maps require no exact alpha samples");

  // Exercise the triangle-level min interval directly; BVH AABB clipping can
  // otherwise hide a broken t_min test in ComputeCandidate.
  Ray<float> min_only_ray = ray;
  min_only_ray.min_t = 1.5f;
  OpacityIntersector<float> min_intersector(
      &vertices[0].x, faces, sizeof(PaddedFloatVertex), &map);
  min_intersector.PrepareTraversal(min_only_ray, options);
  float min_candidate_t = 10.0f;
  check(!min_intersector.Intersect(&min_candidate_t, 1),
        "triangle candidate respects ray.min_t independently of BVH clipping");
  check(min_candidate_t == 10.0f, "min_t rejection does not write candidate t");

  // The same scene through the unmodified BVH Build/Traverse entry points.
  OpacityMesh<float> mesh(&vertices[0].x, faces, sizeof(PaddedFloatVertex),
                          &map);
  TriangleSAHPred<float> pred(&vertices[0].x, faces,
                              sizeof(PaddedFloatVertex));
  BVHAccel<float> bvh;
  BVHBuildOptions<float> build_options;
  build_options.min_leaf_primitives = 4;
  check(bvh.Build(2, mesh, pred, build_options), "two-layer BVH builds");

  OpacityIntersector<float> traced(&vertices[0].x, faces,
                                  sizeof(PaddedFloatVertex), &map);
  TriangleIntersection<float> hit;
  check(bvh.Traverse(ray, traced, &hit), "BVH finds far opaque layer");
  check(hit.prim_id == 1, "nearest surviving entity is far primitive");
  check(std::fabs(hit.t - 1.0001f) < 1e-5f, "BVH far t correct");
  check(std::fabs(hit.u - 0.375f) < 1e-5f, "BVH far u not polluted");
  check(std::fabs(hit.v - 0.375f) < 1e-5f, "BVH far v not polluted");
  check(traced.deterministic_hits == 2, "BVH deterministic count is exact");
  check(traced.alpha_samples == 0, "BVH solid textures use zero alpha samples");
  check(traced.opaque_region_hits == 1, "one deterministic opaque hit");
  check(traced.transparent_region_rejections == 1,
        "one deterministic transparent rejection");

  // Finite shadow interval: the transparent near plane must not stand in for
  // a hit, and the far plane is outside max_t.
  Ray<float> short_ray = ray;
  short_ray.max_t = 1.00005f;
  OpacityIntersector<float> shadow(&vertices[0].x, faces,
                                   sizeof(PaddedFloatVertex), &map);
  TriangleIntersection<float> shadow_hit;
  check(!bvh.Traverse(short_ray, shadow, &shadow_hit),
        "finite shadow line reports no blocker before far layer");
  short_ray.max_t = 2.0f;
  check(bvh.Traverse(short_ray, shadow, &shadow_hit),
        "longer finite shadow line reaches far layer");
  check(shadow_hit.prim_id == 1, "finite shadow line identity correct");

  // min_t uses the same inherited interval rule.
  Ray<float> min_ray = ray;
  min_ray.min_t = 1.5f;
  check(!bvh.Traverse(min_ray, shadow, &shadow_hit),
        "ray min_t excludes both close layers");
  min_ray.min_t = 1.00005f;
  check(bvh.Traverse(min_ray, shadow, &shadow_hit),
        "ray min_t still permits the far layer");

  // Existing traversal controls remain in effect.  Reusing the intersector
  // also proves per-query counters are reset by PrepareTraversal().
  BVHTraceOptions skip_far;
  skip_far.skip_prim_id = 1;
  check(!bvh.Traverse(ray, traced, &hit, skip_far),
        "skip_prim_id still skips the far primitive");
  check(traced.deterministic_hits == 1 && traced.alpha_samples == 0,
        "counters reset for each BVH query");

  BVHTraceOptions only_near;
  only_near.prim_ids_range[0] = 0;
  only_near.prim_ids_range[1] = 1;
  check(!bvh.Traverse(ray, traced, &hit, only_near),
        "prim_ids_range can restrict to transparent near face");

  BVHTraceOptions only_far;
  only_far.prim_ids_range[0] = 1;
  only_far.prim_ids_range[1] = 2;
  check(bvh.Traverse(ray, traced, &hit, only_far),
        "prim_ids_range can restrict to far face");
  check(hit.prim_id == 1, "ranged traversal returns original prim id");

  BVHTraceOptions cull;
  cull.cull_back_face = true;
  check(!bvh.Traverse(ray, traced, &hit, cull),
        "back-face culling rejects the ray from the back side");

  Ray<float> front_ray;
  front_ray.org[0] = -0.5f;
  front_ray.org[1] = -0.5f;
  front_ray.org[2] = 3.0f;
  front_ray.dir[0] = front_ray.dir[1] = 0.0f;
  front_ray.dir[2] = -1.0f;
  front_ray.min_t = 0.0f;
  front_ray.max_t = 10.0f;
  check(bvh.Traverse(front_ray, traced, &hit, cull),
        "front-side ray survives back-face culling");
}

void TestOriginalPrimitiveIdentity(Check &check) {
  const int count = 12;
  std::vector<PaddedFloatVertex> vertices(static_cast<size_t>(count) * 3);
  std::vector<unsigned int> faces(static_cast<size_t>(count) * 3);
  std::vector<float> uvs(static_cast<size_t>(count) * 6);
  OpacityMap<float> map;

  for (int prim = 0; prim < count; ++prim) {
    const float cx = 5.0f * prim;
    vertices[3 * prim + 0] = {cx - 1, -1, 1, 0};
    vertices[3 * prim + 1] = {cx + 3, -1, 1, 0};
    vertices[3 * prim + 2] = {cx - 1, 3, 1, 0};
    faces[3 * prim + 0] = 3 * prim + 0;
    faces[3 * prim + 1] = 3 * prim + 1;
    faces[3 * prim + 2] = 3 * prim + 2;
    for (int k = 0; k < 6; ++k) uvs[6 * prim + k] = (k % 2 == 0) ? float(k / 2 == 1) : 0.0f;
    // Explicit standard UVs.
    uvs[6 * prim + 0] = 0;
    uvs[6 * prim + 1] = 0;
    uvs[6 * prim + 2] = 1;
    uvs[6 * prim + 3] = 0;
    uvs[6 * prim + 4] = 0;
    uvs[6 * prim + 5] = 1;

    const unsigned char alpha = static_cast<unsigned char>((prim * 47 + 13) & 255);
    std::vector<unsigned char> tex(1, alpha);
    check(map.BuildPrimitive(prim, uvs.data() + 6 * prim, tex.data(), 1, 1,
                             128, 0),
          "unique one-texel material builds");
  }

  OpacityMesh<float> mesh(&vertices[0].x, faces.data(),
                          sizeof(PaddedFloatVertex), &map);
  TriangleSAHPred<float> pred(&vertices[0].x, faces.data(),
                             sizeof(PaddedFloatVertex));
  BVHAccel<float> bvh;
  check(bvh.Build(count, mesh, pred), "identity BVH builds");

  for (int prim = 0; prim < count; ++prim) {
    const float cx = 5.0f * prim;
    Ray<float> ray;
    ray.org[0] = cx - 0.5f;
    ray.org[1] = -0.5f;
    ray.org[2] = 0.0f;
    ray.dir[2] = 1.0f;
    ray.max_t = 10.0f;

    OpacityIntersector<float> inter(&vertices[0].x, faces.data(),
                                    sizeof(PaddedFloatVertex), &map);
    TriangleIntersection<float> hit;
    const unsigned char alpha =
        static_cast<unsigned char>((prim * 47 + 13) & 255);
    const bool expected = alpha >= 128;
    const bool actual = bvh.Traverse(ray, inter, &hit);
    check(actual == expected, "original primitive material determines visibility");
    if (expected) {
      check(hit.prim_id == static_cast<unsigned>(prim),
            "returned prim_id is original face, not BVH permutation/material");
    }
  }
}

void TestExactMixedSamplesAndDouble(Check &check) {
  const int width = 4;
  const int height = 4;
  std::vector<unsigned char> tex(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      tex[y * width + x] = ((x + 2 * y) % 3 == 0) ? 40 : 220;
    }
  }
  const int cutoff = 128;
  float uvs[6] = {0, 0, 1, 0, 0, 1};
  OpacityMap<float> map;
  check(map.Build(1, uvs, tex.data(), width, height, cutoff, 0),
        "root mixed map builds");
  check(map.GetLeafRegion(0, 0) == nanort::OPACITY_REGION_MIXED,
        "two-class root is mixed");

  PaddedFloatVertex vertices[3] = {
      {-1, -1, 1, 0}, {3, -1, 1, 0}, {-1, 3, 1, 0}};
  unsigned int faces[3] = {0, 1, 2};
  BVHAccel<float> bvh;
  OpacityMesh<float> mesh(&vertices[0].x, faces, sizeof(PaddedFloatVertex),
                          &map);
  TriangleSAHPred<float> pred(&vertices[0].x, faces,
                              sizeof(PaddedFloatVertex));
  check(bvh.Build(1, mesh, pred), "mixed sample BVH builds");

  for (int ix = 0; ix < width; ++ix) {
    for (int iy = 0; iy < height; ++iy) {
      const float bu = static_cast<float>((ix + 0.5) / width);
      const float bv = static_cast<float>((iy + 0.5) / height);
      if (bu + bv >= 0.999) continue;
      const bool expected =
          ReferenceAlpha(tex.data(), width, height, cutoff, bu, bv);

      Ray<float> ray;
      ray.org[0] = -1.0f + 4.0f * bu;
      ray.org[1] = -1.0f + 4.0f * bv;
      ray.org[2] = 0.0f;
      ray.dir[2] = 1.0f;
      ray.max_t = 10.0f;

      OpacityIntersector<float> inter(&vertices[0].x, faces,
                                      sizeof(PaddedFloatVertex), &map);
      TriangleIntersection<float> hit;
      const bool actual = bvh.Traverse(ray, inter, &hit);
      check(actual == expected, "mixed cache result equals direct texel sample");
      check(inter.alpha_samples == 1,
            "one mixed candidate performs exactly one alpha sample");
      check(inter.deterministic_hits == 0,
            "mixed region is not counted deterministic");
      if (actual) {
        check(hit.prim_id == 0, "mixed opaque hit identity");
        check(std::fabs(hit.u - bu) < 2e-5f, "mixed hit u correct");
        check(std::fabs(hit.v - bv) < 2e-5f, "mixed hit v correct");
      }
    }
  }

  // A directly sampled transparent near candidate must not prevent the far
  // opaque face from surviving with its own barycentrics.
  PaddedFloatVertex two_vertices[6] = {
      {-1, -1, 1, 0}, {3, -1, 1, 0}, {-1, 3, 1, 0},
      {-2, -2, 2, 0}, {2, -2, 2, 0}, {-2, 2, 2, 0}};
  unsigned int two_faces[6] = {0, 1, 2, 3, 4, 5};
  float two_uvs[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  OpacityMap<float> two_map;
  check(two_map.BuildPrimitive(0, two_uvs, tex.data(), width, height, cutoff, 0),
        "mixed near builds");
  std::vector<unsigned char> white(16, 255);
  check(two_map.BuildPrimitive(1, two_uvs + 6, white.data(), 4, 4, 128, 0),
        "opaque far builds");
  OpacityMesh<float> two_mesh(&two_vertices[0].x, two_faces,
                              sizeof(PaddedFloatVertex), &two_map);
  BVHAccel<float> two_bvh;
  TriangleSAHPred<float> two_pred(&two_vertices[0].x, two_faces,
                                 sizeof(PaddedFloatVertex));
  check(two_bvh.Build(2, two_mesh, two_pred), "mixed two-layer BVH builds");

  // Texel (x=0,y=0): 0 % 3 == 0 => alpha 40, transparent.
  Ray<float> ray;
  ray.org[0] = -1.0f + 4.0f * 0.125f;
  ray.org[1] = -1.0f + 4.0f * 0.125f;
  ray.org[2] = 0;
  ray.dir[2] = 1;
  ray.max_t = 10;
  OpacityIntersector<float> two_inter(&two_vertices[0].x, two_faces,
                                      sizeof(PaddedFloatVertex), &two_map);
  TriangleIntersection<float> two_hit;
  check(two_bvh.Traverse(ray, two_inter, &two_hit),
        "exact transparent near still leaves far hit");
  check(two_hit.prim_id == 1, "far identity after exact transparent sample");
  check(std::fabs(two_hit.u - 0.375f) < 1e-5f,
        "far u retained after exact transparent sample");
  check(two_inter.alpha_samples == 1, "only near uses exact alpha sample");
  check(two_inter.deterministic_hits == 1, "far is deterministic opaque");

  // Double precision and padded double vertex stride.
  double duvs[6] = {0, 0, 1, 0, 0, 1};
  OpacityMap<double> dmap;
  std::vector<unsigned char> dtex(4, 255);
  check(dmap.Build(1, duvs, dtex.data(), 2, 2, 128, 2), "double map builds");
  check(dmap.Classify(0, .2, .2) == nanort::OPACITY_REGION_OPAQUE,
        "double classification works");

  PaddedDoubleVertex dvertices[3] = {
      {-1, -1, 1, 0}, {3, -1, 1, 0}, {-1, 3, 1, 0}};
  unsigned int dfaces[3] = {0, 1, 2};
  OpacityMesh<double> dmesh(&dvertices[0].x, dfaces,
                            sizeof(PaddedDoubleVertex), &dmap);
  TriangleSAHPred<double> dpred(&dvertices[0].x, dfaces,
                               sizeof(PaddedDoubleVertex));
  BVHAccel<double> dbvh;
  check(dbvh.Build(1, dmesh, dpred), "double BVH builds");
  Ray<double> dray;
  dray.org[0] = -.5;
  dray.org[1] = -.5;
  dray.dir[2] = 1;
  dray.max_t = 10;
  OpacityIntersector<double> dinter(&dvertices[0].x, dfaces,
                                    sizeof(PaddedDoubleVertex), &dmap);
  TriangleIntersection<double> dhit;
  check(dbvh.Traverse(dray, dinter, &dhit), "double padded-stride trace works");
  check(std::fabs(dhit.u - .125) < 1e-10, "double barycentric precision");
  check(dinter.alpha_samples == 0, "double solid texture is deterministic");
}

struct RenderResult {
  int front = 0, back = 0, wall = 0;
  int wall_lit = 0, wall_shadow = 0;
  unsigned long long deterministic = 0;
  unsigned long long alpha_samples = 0;
};

RenderResult RenderTestScene(Check &check, std::vector<unsigned char> *pixels) {
  constexpr int kW = 96;
  constexpr int kH = 72;
  if (pixels) pixels->assign(static_cast<size_t>(kW) * kH * 3, 0);

  std::vector<PaddedFloatVertex> vertices;
  std::vector<unsigned int> faces;
  std::vector<float> uvs;

  auto add_quad = [&](float half, float z) {
    const unsigned int base = static_cast<unsigned int>(vertices.size());
    PaddedFloatVertex q[4] = {
        {-half, -half, z, 0}, {half, -half, z, 0},
        {half, half, z, 0}, {-half, half, z, 0}};
    for (const auto &v : q) vertices.push_back(v);
    const unsigned int f[6] = {base, base + 1, base + 2,
                               base, base + 2, base + 3};
    for (unsigned int id : f) faces.push_back(id);
    const float uv[12] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    for (float x : uv) uvs.push_back(x);
  };

  add_quad(1.35f, 0.0f);    // prims 0,1
  add_quad(1.75f, -0.45f);  // prims 2,3
  add_quad(4.2f, -3.0f);    // prims 4,5
  check(faces.size() / 3 == 6, "render scene has six triangles");

  constexpr int kTex = 32;
  std::vector<unsigned char> hole_a(kTex * kTex);
  std::vector<unsigned char> hole_b(kTex * kTex);
  for (int y = 0; y < kTex; ++y) {
    for (int x = 0; x < kTex; ++x) {
      const int cell = 8;
      const float dx = float(x % cell) - 3.5f;
      const float dy = float(y % cell) - 3.5f;
      const bool hole = dx * dx + dy * dy < 5.5f;
      hole_a[y * kTex + x] = hole ? 0 : 255;
      const float dx2 = float((x + 3) % cell) - 3.5f;
      const bool stripe_hole = ((y / 4) % 2 == 0) && (x % cell < 5);
      hole_b[y * kTex + x] = (stripe_hole || dx2 * dx2 + dy * dy < 3.0f) ? 0 : 255;
    }
  }
  std::vector<unsigned char> white(1, 255);

  OpacityMap<float> map(6);
  for (int prim = 0; prim < 2; ++prim) {
    check(map.BuildPrimitive(prim, uvs.data() + 6 * prim, hole_a.data(), kTex,
                             kTex, 128, 4),
          "front perforated material builds");
  }
  for (int prim = 2; prim < 4; ++prim) {
    check(map.BuildPrimitive(prim, uvs.data() + 6 * prim, hole_b.data(), kTex,
                             kTex, 100, 4),
          "back perforated material builds");
  }
  for (int prim = 4; prim < 6; ++prim) {
    check(map.BuildPrimitive(prim, uvs.data() + 6 * prim, white.data(), 1, 1,
                             1, 5),
          "wall material builds");
  }

  OpacityMesh<float> mesh(&vertices[0].x, faces.data(),
                          sizeof(PaddedFloatVertex), &map);
  TriangleSAHPred<float> pred(&vertices[0].x, faces.data(),
                             sizeof(PaddedFloatVertex));
  BVHAccel<float> bvh;
  check(bvh.Build(6, mesh, pred), "render BVH builds");

  const float light[3] = {2.2f, 2.8f, 1.4f};
  const float colors[3][3] = {
      {0.30f, 0.78f, 0.43f}, {0.95f, 0.43f, 0.22f}, {0.34f, 0.55f, 0.95f}};
  RenderResult result;

  for (int py = 0; py < kH; ++py) {
    for (int px = 0; px < kW; ++px) {
      const float aspect = float(kW) / kH;
      const float sx = (2.0f * (px + .5f) / kW - 1.0f) * aspect * .58f;
      const float sy = (1.0f - 2.0f * (py + .5f) / kH) * .58f;
      Ray<float> ray;
      ray.org[0] = 0;
      ray.org[1] = 0;
      ray.org[2] = 3.4f;
      float len = std::sqrt(sx * sx + sy * sy + 1.0f);
      ray.dir[0] = sx / len;
      ray.dir[1] = sy / len;
      ray.dir[2] = -1.0f / len;
      ray.min_t = 0;
      ray.max_t = 100;

      OpacityIntersector<float> inter(&vertices[0].x, faces.data(),
                                      sizeof(PaddedFloatVertex), &map);
      TriangleIntersection<float> hit;
      if (!bvh.Traverse(ray, inter, &hit)) continue;

      result.deterministic += inter.deterministic_hits;
      result.alpha_samples += inter.alpha_samples;

      const int layer = std::min(2, static_cast<int>(hit.prim_id / 2));
      if (layer == 0) ++result.front;
      if (layer == 1) ++result.back;
      if (layer == 2) ++result.wall;

      const float p[3] = {ray.org[0] + ray.dir[0] * hit.t,
                          ray.org[1] + ray.dir[1] * hit.t,
                          ray.org[2] + ray.dir[2] * hit.t};
      float lx = light[0] - p[0], ly = light[1] - p[1],
            lz = light[2] - p[2];
      const float dist = std::sqrt(lx * lx + ly * ly + lz * lz);
      lx /= dist;
      ly /= dist;
      lz /= dist;

      Ray<float> shadow;
      shadow.org[0] = p[0];
      shadow.org[1] = p[1];
      shadow.org[2] = p[2];
      shadow.dir[0] = lx;
      shadow.dir[1] = ly;
      shadow.dir[2] = lz;
      shadow.min_t = 0;
      shadow.max_t = dist;  // finite light segment

      BVHTraceOptions shadow_options;
      shadow_options.skip_prim_id = hit.prim_id;
      OpacityIntersector<float> shadow_inter(
          &vertices[0].x, faces.data(), sizeof(PaddedFloatVertex), &map);
      TriangleIntersection<float> shadow_hit;
      const bool occluded =
          bvh.Traverse(shadow, shadow_inter, &shadow_hit, shadow_options);

      if (layer == 2) {
        if (occluded) ++result.wall_shadow;
        else ++result.wall_lit;
      }

      const float ndotl = std::max(0.0f, lz);  // all procedural quads face +Z
      float lighting = .24f + .76f * ndotl;
      if (occluded) lighting *= .34f;
      const unsigned int out_index =
          static_cast<unsigned int>((py * kW + px) * 3);
      if (pixels) {
        (*pixels)[out_index + 0] = static_cast<unsigned char>(
            std::min(255.0f, 255.0f * colors[layer][0] * lighting));
        (*pixels)[out_index + 1] = static_cast<unsigned char>(
            std::min(255.0f, 255.0f * colors[layer][1] * lighting));
        (*pixels)[out_index + 2] = static_cast<unsigned char>(
            std::min(255.0f, 255.0f * colors[layer][2] * lighting));
      }
    }
  }

  check(result.front > 80, "front perforated layer is visible");
  check(result.back > 20, "close rear perforated layer is visible around front");
  check(result.wall > 80, "colored back wall is visible");
  check(result.wall_lit > 20, "some wall pixels receive finite light rays");
  check(result.wall_shadow > 20, "hole masks cast patterned shadows");
  check(result.deterministic > 0, "render exercises deterministic cache regions");
  check(result.alpha_samples > 0, "render exercises exact mixed-region samples");
  return result;
}

void CheckExamplePPMIfPresent(Check &check) {
  std::ifstream file(".task-build/demo.ppm", std::ios::binary);
  if (!file) return;  // Direct runner invocation need not build the example.

  std::string magic;
  int width = 0, height = 0, max_value = 0;
  file >> magic >> width >> height >> max_value;
  check(magic == "P6", "example writes binary PPM");
  check(width > 0 && height > 0 && max_value == 255,
        "example PPM header has positive dimensions");
  file.get();
  std::vector<unsigned char> data(static_cast<size_t>(width) * height * 3);
  file.read(reinterpret_cast<char *>(data.data()),
            static_cast<std::streamsize>(data.size()));
  check(file.gcount() == static_cast<std::streamsize>(data.size()),
        "example PPM contains complete pixel data");

  int dark_wall_like = 0;
  for (size_t i = 0; i + 2 < data.size(); i += 3) {
    const int luminance = (data[i] + data[i + 1] + data[i + 2]) / 3;
    if (luminance < 90) ++dark_wall_like;
  }
  check(dark_wall_like > width * height / 20,
        "example PPM contains substantial shadowed/dark pixels");
}

}  // namespace

void test_classification(Check &check) {
  TestClassificationPrecision<float>(check, "float");
  TestClassificationPrecision<double>(check, "double");
}

void test_visibility(Check &check) {
  TestSpecialBarycentricRetention(check);
  TestOriginalPrimitiveIdentity(check);
  TestExactMixedSamplesAndDouble(check);
}

void test_render(Check &check) {
  std::vector<unsigned char> pixels;
  const RenderResult result = RenderTestScene(check, &pixels);
  check(!pixels.empty(), "render produces an image");

  // The in-test renderer uses finite shadow segments to the point light.
  check(result.wall_shadow > 0 && result.wall_lit > 0,
        "render checks both lit and shadowed finite light lines");
  CheckExamplePPMIfPresent(check);
}
