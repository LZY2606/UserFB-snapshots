#include "suite.h"

#include <FastBVH.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

template <typename F>
using V3 = FastBVH::Vector3<F>;

template <typename F>
using RayT = FastBVH::Ray<F>;

template <typename F>
using BBoxT = FastBVH::BBox<F>;

inline uint8_t lane_bit(int lane) { return static_cast<uint8_t>(1u << lane); }

template <typename F>
std::array<F, 4> fill4(F value) {
  return std::array<F, 4>{{value, value, value, value}};
}

template <typename F>
std::array<RayT<F>, 4> same4(const RayT<F>& ray) {
  return std::array<RayT<F>, 4>{{ray, ray, ray, ray}};
}

template <typename F>
F inf() {
  return std::numeric_limits<F>::infinity();
}

//! A representable value just above / below a boundary, so that closed
//! interval boundaries can be probed exactly for both float and double.
template <typename F>
F above(F value) {
  return std::nextafter(value, inf<F>());
}

template <typename F>
F below(F value) {
  return std::nextafter(value, -inf<F>());
}

// ---------------------------------------------------------------------------
// Box primitives. The library does not assume any primitive shape; the test
// uses boxes, and the geometry is supplied entirely through the intersector.
// ---------------------------------------------------------------------------

template <typename F>
struct BoxPrim final {
  BBoxT<F> box;
  int id;
};

template <typename F>
struct BoxPrimConverter final {
  BBoxT<F> operator()(const BoxPrim<F>& p) const noexcept { return p.box; }
};

//! Triangle primitives, used to show that the packet traversal does not
//! assume any particular primitive shape. The Möller-Trumbore test supplies
//! exactly one candidate root per triangle.
template <typename F>
struct TriPrim final {
  V3<F> a;
  V3<F> b;
  V3<F> c;
  int id;
};

template <typename F>
struct TriConverter final {
  BBoxT<F> operator()(const TriPrim<F>& t) const noexcept {
    BBoxT<F> box(t.a);
    box.expandToInclude(t.b);
    box.expandToInclude(t.c);
    return box;
  }
};

template <typename F>
struct TriIntersector final {
  uint64_t* calls;

  FastBVH::Intersection<F, TriPrim<F>> operator()(const TriPrim<F>& t, const RayT<F>& r) const noexcept {
    if (calls != nullptr) {
      ++*calls;
    }

    const V3<F> e1 = t.b - t.a;
    const V3<F> e2 = t.c - t.a;
    const V3<F> pvec = FastBVH::cross(r.d, e2);
    const F det = FastBVH::dot(e1, pvec);

    if (det == F(0)) {
      return FastBVH::Intersection<F, TriPrim<F>>{};
    }

    const F inv_det = F(1) / det;
    const V3<F> tvec = r.o - t.a;
    const F u = FastBVH::dot(tvec, pvec) * inv_det;
    if (u < F(0) || u > F(1)) {
      return FastBVH::Intersection<F, TriPrim<F>>{};
    }

    const V3<F> qvec = FastBVH::cross(tvec, e1);
    const F v = FastBVH::dot(r.d, qvec) * inv_det;
    if (v < F(0) || u + v > F(1)) {
      return FastBVH::Intersection<F, TriPrim<F>>{};
    }

    const F distance = FastBVH::dot(e2, qvec) * inv_det;

    // Only the front root is reported, as the packet contract allows.
    return FastBVH::Intersection<F, TriPrim<F>>{distance, &t};
  }
};

//! An independent, closed-interval slab test. Axis-parallel directions
//! (including negative zero) are handled with an explicit branch, so a ray
//! on a box boundary is not lost to 0 * infinity.
template <typename F>
bool slab(const BBoxT<F>& box, const RayT<F>& ray, F& tnear, F& tfar) {
  F lo = -inf<F>();
  F hi = inf<F>();

  for (unsigned int axis = 0; axis < 3; ++axis) {
    const F origin = ray.o[axis];
    const F direction = ray.d[axis];

    if (direction == F(0)) {
      if (origin < box.min[axis] || origin > box.max[axis]) {
        return false;
      }
      continue;
    }

    F near = (box.min[axis] - origin) / direction;
    F far = (box.max[axis] - origin) / direction;

    if (near > far) {
      std::swap(near, far);
    }
    if (near > lo) {
      lo = near;
    }
    if (far < hi) {
      hi = far;
    }
    if (lo > hi) {
      return false;
    }
  }

  tnear = lo;
  tfar = hi;
  return true;
}

//! Returns exactly one candidate root per primitive, as the packet
//! contract allows. Negative candidates are reported as-is; the packet is
//! responsible for applying the lane's distance window.
template <typename F>
struct BoxIntersector final {
  uint64_t* calls;

  FastBVH::Intersection<F, BoxPrim<F>> operator()(const BoxPrim<F>& p, const RayT<F>& r) const noexcept {
    if (calls != nullptr) {
      ++*calls;
    }
    F near = 0;
    F far = 0;
    if (!slab(p.box, r, near, far)) {
      return FastBVH::Intersection<F, BoxPrim<F>>{};
    }
    return FastBVH::Intersection<F, BoxPrim<F>>{near, &p};
  }
};

//! A small deterministic generator, so the randomized check is reproducible.
struct Lcg final {
  uint32_t state = 0x9e3779b9u;
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state;
  }
  double unit() { return static_cast<double>(next() >> 8) / 16777216.0; }
  double range(double lo, double hi) { return lo + (hi - lo) * unit(); }
};

//! Reference result obtained by independent enumeration over all primitives.
template <typename F>
struct Brute final {
  bool hit = false;
  F t = inf<F>();
  std::vector<int> ids;
};

template <typename F>
Brute<F> brute(const std::vector<BoxPrim<F>>& prims, const RayT<F>& ray, F tmin, F tmax) {
  Brute<F> result;
  for (const auto& p : prims) {
    F near = 0;
    F far = 0;
    if (!slab(p.box, ray, near, far)) {
      continue;
    }
    if (near < tmin || near > tmax) {
      continue;
    }
    if (!result.hit || near < result.t) {
      result.hit = true;
      result.t = near;
      result.ids.assign(1, p.id);
    } else if (near == result.t) {
      result.ids.push_back(p.id);
    }
  }
  return result;
}

template <typename F>
void expectLane(Suite& suite, const std::array<FastBVH::Intersection<F, BoxPrim<F>>, 4>& got, int lane,
                const Brute<F>& ref, const std::string& tag) {
  if (!ref.hit) {
    suite.check(!static_cast<bool>(got[lane]), tag + ": expected a miss");
    suite.check(got[lane].object == nullptr, tag + ": missed lane must carry no primitive");
    suite.check(got[lane].t == inf<F>(), tag + ": missed lane must keep the infinity sentinel");
    return;
  }

  suite.check(static_cast<bool>(got[lane]), tag + ": expected a hit");
  suite.check(got[lane].t == ref.t, tag + ": hit distance mismatch");

  bool id_ok = false;
  if (got[lane].object != nullptr) {
    for (int id : ref.ids) {
      if (got[lane].object->id == id) {
        id_ok = true;
      }
    }
  }
  suite.check(id_ok, tag + ": hit primitive identity mismatch");
}

template <typename F>
BoxPrim<F> makeBox(F minx, F maxx, F miny, F maxy, F minz, F maxz, int id) {
  return BoxPrim<F>{BBoxT<F>(V3<F>{minx, miny, minz}, V3<F>{maxx, maxy, maxz}), id};
}

// ---------------------------------------------------------------------------
// coherent: four identical rays on a non-trivial 64-box tree.
// ---------------------------------------------------------------------------

template <typename F>
void runCoherent(Suite& suite) {
  std::vector<BoxPrim<F>> prims;
  for (int i = 0; i < 64; ++i) {
    prims.push_back(makeBox<F>(F(4 * i), F(4 * i + 1), F(0), F(1), F(0), F(1), i));
  }

  auto bvh = FastBVH::DefaultBuilder<F>{}(prims, BoxPrimConverter<F>{});

  suite.check(bvh.getNodes().size() > 1, "coherent: tree has more than one node");
  suite.check(bvh.countLeafs() > 1, "coherent: tree has more than one leaf");

  RayT<F> ray(V3<F>{F(0.5), F(0.5), F(0.5)}, V3<F>{F(1), F(0), F(0)});
  auto rays = same4(ray);
  auto mins = fill4(F(0));
  auto maxs = fill4(inf<F>());

  uint64_t calls = 0;

  FastBVH::PacketStats stats;
  stats.node_loads = 1234567;  // must be reset by the query

  FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{&calls});

  auto got = packet.traverse(rays, mins, maxs, 0x0F, &stats);

  const auto ref = brute(prims, ray, F(0), inf<F>());

  for (int lane = 0; lane < 4; ++lane) {
    expectLane(suite, got, lane, ref, "coherent lane " + std::to_string(lane));
  }

  // The same ray must produce the same hit in every lane.
  for (int lane = 1; lane < 4; ++lane) {
    suite.check(got[lane].t == got[0].t, "coherent: lanes disagree on distance");
    suite.check(got[lane].object == got[0].object, "coherent: lanes disagree on primitive");
  }

  suite.check(stats.node_loads > 0, "coherent: node_loads counted");
  suite.check(stats.box_lane_tests > 0, "coherent: box_lane_tests counted");
  suite.check(stats.primitive_lane_tests > 0, "coherent: primitive_lane_tests counted");
  suite.check(calls == stats.primitive_lane_tests, "coherent: intersector calls match reported primitive tests");

  // Four single-lane packet queries, used as the comparison baseline.
  uint64_t single_node_loads = 0;
  for (int lane = 0; lane < 4; ++lane) {
    FastBVH::PacketStats single_stats;
    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> single(bvh, BoxIntersector<F>{nullptr});
    auto single_got = single.traverse(rays, mins, maxs, lane_bit(lane), &single_stats);
    single_node_loads += single_stats.node_loads;
    suite.check(single_got[lane].t == got[lane].t, "coherent: single-lane agrees");
  }

  suite.check(single_node_loads > 0, "coherent: baseline traversal has node loads");
  // Packet node loads must not exceed 40 percent of the four single-lane totals.
  suite.check(stats.node_loads * 5 <= single_node_loads * 2, "coherent: packet does not reuse node loads");

  // Statistics must start from zero for every query.
  FastBVH::PacketStats stats2;
  stats2.node_loads = 42;
  stats2.box_lane_tests = 42;
  stats2.primitive_lane_tests = 42;
  auto got2 = packet.traverse(rays, mins, maxs, 0x0F, &stats2);

  suite.check(stats2.node_loads == stats.node_loads, "coherent: node_loads reset per query");
  suite.check(stats2.box_lane_tests == stats.box_lane_tests, "coherent: box_lane_tests reset per query");
  suite.check(stats2.primitive_lane_tests == stats.primitive_lane_tests,
              "coherent: primitive_lane_tests reset per query");
  for (int lane = 0; lane < 4; ++lane) {
    suite.check(got2[lane].t == got[lane].t, "coherent: repeat query is deterministic");
  }
}

// ---------------------------------------------------------------------------
// divergent: rays heading in opposite directions, per-lane windows, and a
// near hit on one lane that must not tighten another lane's stack work.
// ---------------------------------------------------------------------------

template <typename F>
void runDivergent(Suite& suite) {
  std::vector<BoxPrim<F>> prims;

  for (int i = 0; i < 8; ++i) {
    prims.push_back(makeBox<F>(F(4 * i + 1), F(4 * i + 2), F(0), F(1), F(0), F(1), i));
  }
  for (int i = 0; i < 8; ++i) {
    prims.push_back(makeBox<F>(F(-4 * i - 2), F(-4 * i - 1), F(0), F(1), F(0), F(1), 8 + i));
  }
  for (int i = 0; i < 8; ++i) {
    prims.push_back(makeBox<F>(F(0), F(1), F(4 * i + 1), F(4 * i + 2), F(0), F(1), 16 + i));
  }
  for (int i = 0; i < 8; ++i) {
    prims.push_back(makeBox<F>(F(0), F(1), F(0), F(1), F(4 * i + 1), F(4 * i + 2), 24 + i));
  }

  auto bvh = FastBVH::DefaultBuilder<F>{}(prims, BoxPrimConverter<F>{});

  suite.check(bvh.getNodes().size() > 1, "divergent: tree has more than one node");

  const V3<F> origin{F(0.5), F(0.5), F(0.5)};

  auto rays = std::array<RayT<F>, 4>{{
      RayT<F>(origin, V3<F>{F(2), F(0), F(0)}),
      RayT<F>(origin, V3<F>{F(-2), F(0), F(0)}),
      RayT<F>(origin, V3<F>{F(0), F(2), F(0)}),
      RayT<F>(origin, V3<F>{F(0), F(0), F(4)}),
  }};

  auto mins = std::array<F, 4>{{F(0), F(0), above(F(2.25)), F(0)}};
  auto maxs = std::array<F, 4>{{inf<F>(), inf<F>(), inf<F>(), F(2.5)}};

  uint64_t calls = 0;
  FastBVH::PacketStats stats;

  FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{&calls});

  auto got = packet.traverse(rays, mins, maxs, 0x0F, &stats);

  for (int lane = 0; lane < 4; ++lane) {
    const auto ref = brute(prims, rays[lane], mins[lane], maxs[lane]);
    expectLane(suite, got, lane, ref, "divergent lane " + std::to_string(lane));
  }

  // The two opposite directions must resolve to distinct primitives.
  suite.check(got[0].object->id == 0, "divergent: +x lane hit the +x box");
  suite.check(got[1].object->id == 8, "divergent: -x lane hit the -x box");

  // A near hit on lane 0 must not tighten lane 1, which needs a far box.
  std::vector<BoxPrim<F>> prims2;
  prims2.push_back(makeBox<F>(F(1), F(2), F(0), F(1), F(0), F(1), 100));    // near box
  prims2.push_back(makeBox<F>(F(100), F(101), F(0), F(1), F(0), F(1), 101));  // far box
  for (int i = 0; i < 6; ++i) {
    prims2.push_back(makeBox<F>(F(200 + 4 * i), F(201 + 4 * i), F(0), F(1), F(0), F(1), 102 + i));
  }

  auto bvh2 = FastBVH::DefaultBuilder<F>{}(prims2, BoxPrimConverter<F>{});
  suite.check(bvh2.countLeafs() > 1, "divergent: split tree has multiple leaves");

  RayT<F> near_ray(V3<F>{F(0), F(0.5), F(0.5)}, V3<F>{F(2), F(0), F(0)});
  auto rays2 = same4(near_ray);

  auto mins2 = std::array<F, 4>{{F(0), above(F(2.5)), F(0), F(0)}};
  auto maxs2 = std::array<F, 4>{{inf<F>(), inf<F>(), inf<F>(), inf<F>()}};

  uint64_t calls2 = 0;
  FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet2(bvh2, BoxIntersector<F>{&calls2});

  auto got2 = packet2.traverse(rays2, mins2, maxs2, 0x0F);

  expectLane(suite, got2, 0, brute(prims2, near_ray, F(0), inf<F>()), "divergent near lane 0");
  expectLane(suite, got2, 1, brute(prims2, near_ray, above(F(2.5)), inf<F>()), "divergent far lane 1");

  suite.check(got2[0].object->id == 100, "divergent: lane 0 keeps its near box");
  suite.check(got2[1].object->id == 101, "divergent: lane 1 keeps its far box despite lane 0's near hit");
  suite.check(got2[1].t == F(50), "divergent: lane 1 far distance preserved");
}

// ---------------------------------------------------------------------------
// intervals: closed distance windows, including candidates just inside and
// just outside the window, reversed rays, infinite tmax and non-unit
// directions.
// ---------------------------------------------------------------------------

template <typename F>
void runIntervals(Suite& suite) {
  std::vector<BoxPrim<F>> prims;
  prims.push_back(makeBox<F>(F(3), F(4), F(-1), F(1), F(-1), F(1), 0));
  prims.push_back(makeBox<F>(F(7), F(8), F(-1), F(1), F(-1), F(1), 1));
  prims.push_back(makeBox<F>(F(11), F(12), F(-1), F(1), F(-1), F(1), 2));

  auto bvh = FastBVH::DefaultBuilder<F>{}(prims, BoxPrimConverter<F>{});

  RayT<F> forward(V3<F>{F(0), F(0.5), F(0.5)}, V3<F>{F(1), F(0), F(0)});
  auto rays = same4(forward);

  // A window selects one box in the middle of three along the ray.
  {
    auto mins = std::array<F, 4>{{F(0), F(0), F(5), F(8)}};
    auto maxs = std::array<F, 4>{{inf<F>(), below(F(3)), F(9), F(12)}};

    uint64_t calls = 0;
    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{&calls});
    auto got = packet.traverse(rays, mins, maxs, 0x0F);

    for (int lane = 0; lane < 4; ++lane) {
      expectLane(suite, got, lane, brute(prims, forward, mins[lane], maxs[lane]),
                 "intervals select lane " + std::to_string(lane));
    }

    suite.check(got[0].object->id == 0, "intervals: full window picks the first box");
    suite.check(got[2].object->id == 1, "intervals: middle window picks the middle box");
    suite.check(got[3].object->id == 2, "intervals: window past the middle box picks the last box");
    suite.check(calls > 0, "intervals: in-window candidates reach the intersector");
  }

  // Exact closed boundaries: tmin == entry and tmax == entry both hit.
  {
    const F entry = F(3);

    auto mins = std::array<F, 4>{{entry, above(entry), F(0), F(0)}};
    auto maxs = std::array<F, 4>{{inf<F>(), below(F(7)), entry, below(entry)}};

    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{nullptr});
    auto got = packet.traverse(rays, mins, maxs, 0x0F);

    for (int lane = 0; lane < 4; ++lane) {
      expectLane(suite, got, lane, brute(prims, forward, mins[lane], maxs[lane]),
                 "intervals boundary lane " + std::to_string(lane));
    }

    suite.check(static_cast<bool>(got[0]), "intervals: inclusive tmin accepts the entry");
    suite.check(!static_cast<bool>(got[1]), "intervals: tmin just above the entry rejects it");
    suite.check(static_cast<bool>(got[2]), "intervals: inclusive tmax accepts the entry");
    suite.check(!static_cast<bool>(got[3]), "intervals: tmax just below the entry rejects it");
  }

  // A reversed ray, a window in the middle, and an infinite tmax.
  {
    RayT<F> reversed(V3<F>{F(13), F(0.5), F(0.5)}, V3<F>{F(-1), F(0), F(0)});
    auto rev_rays = same4(reversed);

    auto mins = std::array<F, 4>{{F(0), F(4), F(4), F(4)}};
    auto maxs = std::array<F, 4>{{inf<F>(), F(6), F(5), below(F(5))}};

    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{nullptr});
    auto got = packet.traverse(rev_rays, mins, maxs, 0x0F);

    for (int lane = 0; lane < 4; ++lane) {
      expectLane(suite, got, lane, brute(prims, reversed, mins[lane], maxs[lane]),
                 "intervals reversed lane " + std::to_string(lane));
    }

    suite.check(got[0].t == F(1) && got[0].object->id == 2, "intervals: reversed ray hits the last box first");
    suite.check(got[1].t == F(5) && got[1].object->id == 1, "intervals: reversed middle window");
    suite.check(got[2].t == F(5) && got[2].object->id == 1,
                "intervals: reversed closed upper bound accepts the candidate exactly at it");
    suite.check(!static_cast<bool>(got[3]),
                "intervals: reversed window one ulp below the candidate rejects it");
  }

  // Non-unit direction: entry distances scale with the direction magnitude.
  {
    RayT<F> fast(V3<F>{F(0), F(0.5), F(0.5)}, V3<F>{F(2), F(0), F(0)});
    auto fast_rays = same4(fast);

    auto mins = std::array<F, 4>{{F(0), F(2), above(F(3.5)), F(0)}};
    auto maxs = std::array<F, 4>{{inf<F>(), F(4), F(6), below(F(1.5))}};

    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{nullptr});
    auto got = packet.traverse(fast_rays, mins, maxs, 0x0F);

    for (int lane = 0; lane < 4; ++lane) {
      expectLane(suite, got, lane, brute(prims, fast, mins[lane], maxs[lane]),
                 "intervals non-unit lane " + std::to_string(lane));
    }

    suite.check(got[0].t == F(1.5), "intervals: non-unit direction scales the hit distance");
    suite.check(got[2].t == F(5.5) && got[2].object->id == 2, "intervals: non-unit middle window");
  }

  // Axis-parallel directions, including negative zero, grazing an exact
  // box boundary must not be lost.
  {
    std::vector<BoxPrim<F>> boxes;
    boxes.push_back(makeBox<F>(F(1), F(2), F(3), F(4), F(5), F(6), 0));

    auto single = FastBVH::DefaultBuilder<F>{}(boxes, BoxPrimConverter<F>{});

    // Origin exactly on min.y / min.z (boundary), travelling along +x.
    RayT<F> on_min(V3<F>{F(-10), F(3), F(5)}, V3<F>{F(1), F(0), F(0)});
    // Origin exactly on max.y / max.z (boundary), travelling along -x,
    // with negative zeros in the two zero direction components.
    RayT<F> on_max(V3<F>{F(20), F(4), F(6)}, V3<F>{F(-1), -F(0), -F(0)});

    auto on_min_rays = same4(on_min);
    auto on_max_rays = same4(on_max);
    auto mins = fill4(F(0));
    auto maxs = fill4(inf<F>());

    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(single, BoxIntersector<F>{nullptr});

    auto got_min = packet.traverse(on_min_rays, mins, maxs, 0x0F);
    auto got_max = packet.traverse(on_max_rays, mins, maxs, 0x0F);

    for (int lane = 0; lane < 4; ++lane) {
      suite.check(static_cast<bool>(got_min[lane]), "intervals: axis-parallel ray on the min boundary hits");
      suite.check(got_min[lane].t == F(11), "intervals: axis-parallel hit distance on the min boundary");
      suite.check(static_cast<bool>(got_max[lane]), "intervals: axis-parallel ray on the max boundary hits");
      suite.check(got_max[lane].t == F(18), "intervals: axis-parallel hit distance on the max boundary");
      suite.check(got_max[lane].object != nullptr && got_max[lane].object->id == 0,
                  "intervals: boundary hit keeps primitive identity");
    }

    // One ulp outside the boundary must miss.
    RayT<F> outside(V3<F>{F(-10), below(F(3)), F(5)}, V3<F>{F(1), F(0), F(0)});
    auto outside_rays = same4(outside);
    auto got_out = packet.traverse(outside_rays, mins, maxs, 0x0F);
    for (int lane = 0; lane < 4; ++lane) {
      suite.check(!static_cast<bool>(got_out[lane]), "intervals: axis-parallel ray outside the boundary misses");
    }
  }

  // Triangle geometry: the library makes no assumption about primitive shape.
  {
    std::vector<TriPrim<F>> tris;

    // A grid of small triangles facing the +z ray.
    for (int i = 0; i < 24; ++i) {
      const F x = F(i * 4);
      tris.push_back(TriPrim<F>{V3<F>{x, F(0), F(10)}, V3<F>{x + 2, F(0), F(10)}, V3<F>{x, F(2), F(10)}, i});
    }

    auto triangle_bvh = FastBVH::DefaultBuilder<F>{}(tris, TriConverter<F>{});

    suite.check(triangle_bvh.getNodes().size() > 1, "intervals: triangle tree is non-trivial");

    // Four rays, each aimed at a different triangle in the grid.
    auto tri_rays = std::array<RayT<F>, 4>{{
        RayT<F>(V3<F>{F(0.25), F(0.25), F(0)}, V3<F>{F(0), F(0), F(1)}),
        RayT<F>(V3<F>{F(12.25), F(0.25), F(0)}, V3<F>{F(0), F(0), F(1)}),
        RayT<F>(V3<F>{F(40.25), F(0.25), F(0)}, V3<F>{F(0), F(0), F(1)}),
        RayT<F>(V3<F>{F(4.25), F(0.25), F(0)}, V3<F>{F(0), F(0), F(3)}),
    }};

    auto tri_mins = fill4(F(0));
    auto tri_maxs = std::array<F, 4>{{inf<F>(), inf<F>(), F(9), inf<F>()}};

    uint64_t tri_calls = 0;
    FastBVH::PacketTraverser<F, TriPrim<F>, TriIntersector<F>> tri_packet(triangle_bvh,
                                                                           TriIntersector<F>{&tri_calls});
    auto tri_got = tri_packet.traverse(tri_rays, tri_mins, tri_maxs, 0x0F);

    // Independent expectation: each ray hits triangle floor(x/4) at t=10,
    // except the windowed lane 2, which is rejected at t=10 > tmax=9.
    suite.check(tri_got[0] && tri_got[0].t == F(10) && tri_got[0].object->id == 0,
                "intervals: triangle lane 0 identity and distance");
    suite.check(tri_got[1] && tri_got[1].t == F(10) && tri_got[1].object->id == 3,
                "intervals: triangle lane 1 identity and distance");
    suite.check(!tri_got[2], "intervals: triangle lane 2 rejects the out-of-window root");
    suite.check(tri_got[3] && tri_got[3].object->id == 1, "intervals: triangle lane 3 identity");
    suite.check(tri_got[3] &&
                    std::fabs(static_cast<double>(tri_got[3].t) - 10.0 / 3.0) <
                        (sizeof(F) == sizeof(float) ? 1e-5 : 1e-12),
                "intervals: triangle lane 3 scales with a non-unit direction");
    // Lane 3's parabola-shaped hit did not disturb the others.
    suite.check(tri_calls > 0, "intervals: triangle intersector was invoked");
  }

  // Exact equidistance: two coincident boxes give the same entry parameter,
  // and either hit is acceptable.
  {
    std::vector<BoxPrim<F>> tied;
    tied.push_back(makeBox<F>(F(3), F(4), F(-1), F(1), F(-1), F(1), 0));
    tied.push_back(makeBox<F>(F(3), F(4), F(-1), F(1), F(-1), F(1), 1));

    auto single = FastBVH::BuildStrategy<F, 0>{}(tied, BoxPrimConverter<F>{});

    RayT<F> ray(V3<F>{F(0), F(0), F(0)}, V3<F>{F(1), F(0), F(0)});
    auto rays = same4(ray);

    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(single, BoxIntersector<F>{nullptr});
    auto got = packet.traverse(rays, fill4(F(0)), fill4(inf<F>()), 0x0F);

    for (int lane = 0; lane < 4; ++lane) {
      const auto ref = brute(tied, ray, F(0), inf<F>());
      expectLane(suite, got, lane, ref, "intervals tie lane " + std::to_string(lane));
      suite.check(static_cast<bool>(got[lane]), "intervals: equidistant candidates still yield a hit");
      suite.check(got[lane].t == F(3), "intervals: equidistant hit distance is exact");
      suite.check(got[lane].object != nullptr && (got[lane].object->id == 0 || got[lane].object->id == 1),
                  "intervals: equidistant tie keeps one of the tied primitives");
    }
  }
}

// ---------------------------------------------------------------------------
// occlusion: per-lane early exit, window filtering, inactive lanes, and the
// empty/degenerate inputs.
// ---------------------------------------------------------------------------

template <typename F>
void runOcclusion(Suite& suite) {
  // A root-only tree of four primitives along the ray, all hit.
  {
    std::vector<BoxPrim<F>> prims;
    prims.push_back(makeBox<F>(F(1), F(2), F(0), F(1), F(0), F(1), 0));
    prims.push_back(makeBox<F>(F(3), F(4), F(0), F(1), F(0), F(1), 1));
    prims.push_back(makeBox<F>(F(5), F(6), F(0), F(1), F(0), F(1), 2));
    prims.push_back(makeBox<F>(F(7), F(8), F(0), F(1), F(0), F(1), 3));

    auto bvh = FastBVH::BuildStrategy<F, 0>{}(prims, BoxPrimConverter<F>{});
    suite.check(bvh.getNodes().size() == 1, "occlusion: single-node fixture");

    RayT<F> ray(V3<F>{F(0), F(0.5), F(0.5)}, V3<F>{F(1), F(0), F(0)});
    auto rays = same4(ray);
    auto full_mins = fill4(F(0));
    auto full_maxs = fill4(inf<F>());

    // Non-occluding traversal visits all four primitives for one lane.
    {
      uint64_t calls = 0;
      FastBVH::PacketStats stats;
      FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{&calls});
      auto got = packet.traverse(rays, full_mins, full_maxs, 0x01, &stats);

      suite.check(stats.primitive_lane_tests == 4, "occlusion: closest query tests all four primitives");
      suite.check(got[0].t == F(1) && got[0].object->id == 0, "occlusion: closest primitive selected");
    }

    // Occluding traversal stops the lane at the first in-window hit.
    {
      uint64_t calls = 0;
      FastBVH::PacketStats stats;
      FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{&calls});
      uint8_t mask = packet.occluded(rays, full_mins, full_maxs, 0x01, &stats);

      suite.check(mask == 0x01, "occlusion: single lane occluded");
      suite.check(stats.primitive_lane_tests == 1, "occlusion: lane stops after the first in-window hit");
      suite.check(calls == 1, "occlusion: lane truly stops calling the intersector after a hit");
    }

    // Two lanes with different windows: the near lane stops early while the
    // far lane keeps being served through the later primitives.
    {
      uint64_t calls = 0;
      auto mins = std::array<F, 4>{{F(0), above(F(6)), F(0), F(0)}};
      auto maxs = fill4(inf<F>());
      FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{&calls});
      uint8_t mask = packet.occluded(rays, mins, maxs, 0x03);

      suite.check(mask == 0x03, "occlusion: both lanes occluded");
      suite.check(calls == 5, "occlusion: early-exited lane stops while the other continues");
    }

    // A candidate before the window must not count as occlusion.
    {
      auto mins = std::array<F, 4>{{F(0), above(F(6)), F(0), F(0)}};
      auto maxs = std::array<F, 4>{{below(F(1)), below(F(7)), F(0), F(0)}};
      uint64_t calls = 0;
      FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{&calls});
      uint8_t mask = packet.occluded(rays, mins, maxs, 0x03);

      suite.check(mask == 0x00, "occlusion: out-of-window candidates do not occlude");
      suite.check(calls == 8, "occlusion: every lane visits every primitive when nothing is in-window");
    }
  }

  // Full tree, four distinct lanes, one inactive, one windowed out.
  {
    std::vector<BoxPrim<F>> prims;
    for (int i = 0; i < 64; ++i) {
      prims.push_back(makeBox<F>(F(4 * i), F(4 * i + 1), F(0), F(1), F(0), F(1), i));
    }

    auto bvh = FastBVH::DefaultBuilder<F>{}(prims, BoxPrimConverter<F>{});

    auto rays = std::array<RayT<F>, 4>{{
        RayT<F>(V3<F>{F(0.5), F(0.5), F(0.5)}, V3<F>{F(1), F(0), F(0)}),
        RayT<F>(V3<F>{F(0.5), F(0.5), F(0.5)}, V3<F>{F(1), F(0), F(0)}),
        RayT<F>(V3<F>{F(0.5), F(0.5), F(0.5)}, V3<F>{F(1), F(0), F(0)}),
        RayT<F>(V3<F>{F(300), F(0.5), F(0.5)}, V3<F>{F(-2), F(0), F(0)}),
    }};

    auto mins = std::array<F, 4>{{F(0), F(2), F(0), F(0)}};
    auto maxs = std::array<F, 4>{{inf<F>(), below(F(3)), inf<F>(), inf<F>()}};

    uint64_t calls = 0;
    FastBVH::PacketStats stats;
    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{&calls});
    uint8_t mask = packet.occluded(rays, mins, maxs, 0x0B, &stats);

    // Lane 0 shares the +x origin with the inactive lane 2 and hits box 0.
    // Lane 1 heads the same way but its window ends below the first entry,
    // so it must stay clear. Lanes 0 and 3 are occluded.
    suite.check(mask == 0x09, "occlusion: lane 0 and reversed lane 3 hit, windowed-out lane 1 does not");
    suite.check((mask & lane_bit(1)) == 0, "occlusion: lane whose window excludes every candidate stays clear");
    suite.check((mask & lane_bit(2)) == 0, "occlusion: inactive lane 2 is never occluded");
    suite.check(stats.primitive_lane_tests > 0, "occlusion: active lanes did invoke the intersector");
    suite.check(calls == stats.primitive_lane_tests, "occlusion: reported tests match actual intersector calls");

    // Prove that an inactive lane's intersector is never invoked. Lane 2 is
    // given a uniquely identifiable origin; the probe intersector asserts
    // that this origin never reaches it while lane 2 is inactive.
    uint64_t probe_calls = 0;
    uint64_t inactive_seen = 0;
    {
      struct UniqueLaneProbe {
        uint64_t* all;
        uint64_t* inactive;
        F inactive_origin_x;

        FastBVH::Intersection<F, BoxPrim<F>> operator()(const BoxPrim<F>& p, const RayT<F>& r) const noexcept {
          ++*all;
          if (r.o.x == inactive_origin_x) {
            ++*inactive;
          }
          F near = 0;
          F far = 0;
          if (!slab(p.box, r, near, far)) {
            return FastBVH::Intersection<F, BoxPrim<F>>{};
          }
          return FastBVH::Intersection<F, BoxPrim<F>>{near, &p};
        }
      };

      auto lane_rays = rays;
      lane_rays[2] = RayT<F>(V3<F>{F(0.25), F(0.5), F(0.5)}, V3<F>{F(1), F(0), F(0)});

      FastBVH::PacketTraverser<F, BoxPrim<F>, UniqueLaneProbe> probe_packet(
          bvh, UniqueLaneProbe{&probe_calls, &inactive_seen, F(0.25)});

      uint8_t probe_mask = probe_packet.occluded(lane_rays, mins, maxs, 0x0B, nullptr);

      suite.check(probe_mask == 0x09, "occlusion: probe query keeps the expected occlusion mask");
      suite.check(probe_calls > 0, "occlusion: probe query did serve the active lanes");
      suite.check(inactive_seen == 0, "occlusion: an inactive lane never invokes the intersector");
    }

    // active = 0: no traversal at all, and no intersector calls.
    uint64_t idle_calls = 0;
    FastBVH::PacketStats idle_stats;
    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> idle(bvh, BoxIntersector<F>{&idle_calls});
    uint8_t idle_mask = idle.occluded(rays, mins, maxs, 0x00, &idle_stats);

    suite.check(idle_mask == 0x00, "occlusion: empty active mask yields no occlusion");
    suite.check(idle_calls == 0 && idle_stats.node_loads == 0 && idle_stats.box_lane_tests == 0 &&
                    idle_stats.primitive_lane_tests == 0,
                "occlusion: empty active mask does no work");

    // traverse() must also honour an empty active mask.
    FastBVH::PacketStats idle_traverse_stats;
    idle_traverse_stats.node_loads = 99;
    auto idle_traverse = idle.traverse(rays, mins, maxs, 0x00, &idle_traverse_stats);
    for (int lane = 0; lane < 4; ++lane) {
      suite.check(!static_cast<bool>(idle_traverse[lane]), "occlusion: empty active mask yields empty results");
    }
    suite.check(idle_traverse_stats.node_loads == 0 && idle_traverse_stats.box_lane_tests == 0 &&
                    idle_traverse_stats.primitive_lane_tests == 0,
                "occlusion: empty active mask resets and zeroes the stats");

    // The occlusion statistics are reset per query just like traverse().
    FastBVH::PacketStats occl_stats;
    occl_stats.node_loads = 7;
    occl_stats.box_lane_tests = 7;
    occl_stats.primitive_lane_tests = 7;
    uint8_t replay_mask = packet.occluded(rays, mins, maxs, 0x0B, &occl_stats);
    suite.check(replay_mask == mask, "occlusion: repeated query is deterministic");
    suite.check(occl_stats.node_loads == stats.node_loads, "occlusion: node_loads reset per query");
    suite.check(occl_stats.box_lane_tests == stats.box_lane_tests, "occlusion: box_lane_tests reset per query");
    suite.check(occl_stats.primitive_lane_tests == stats.primitive_lane_tests,
                "occlusion: primitive_lane_tests reset per query");

    // An empty tree must be handled without reading any node.
    FastBVH::NodeArray<F> no_nodes;
    FastBVH::BVH<F, BoxPrim<F>> empty_bvh(std::move(no_nodes), FastBVH::ConstIterable<BoxPrim<F>>(nullptr, 0));

    uint64_t empty_calls = 0;
    FastBVH::PacketStats empty_stats;
    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> empty_packet(empty_bvh,
                                                                           BoxIntersector<F>{&empty_calls});

    auto empty_got = empty_packet.traverse(rays, mins, maxs, 0x0F, &empty_stats);
    for (int lane = 0; lane < 4; ++lane) {
      suite.check(!static_cast<bool>(empty_got[lane]), "occlusion: empty tree miss");
    }
    suite.check(empty_packet.occluded(rays, mins, maxs, 0x0F, &empty_stats) == 0x00,
                "occlusion: empty tree yields no occlusion");
    suite.check(empty_calls == 0 && empty_stats.node_loads == 0, "occlusion: empty tree does no work");
  }
}

// ---------------------------------------------------------------------------
// legacy: the original single-ray Traverser keeps working unchanged and
// agrees with the independent enumeration, in both query modes.
// ---------------------------------------------------------------------------

template <typename F>
void runLegacy(Suite& suite) {
  std::vector<BoxPrim<F>> prims;
  for (int i = 0; i < 32; ++i) {
    prims.push_back(makeBox<F>(F(4 * i), F(4 * i + 1), F(-1), F(1), F(-1), F(1), i));
  }

  auto bvh = FastBVH::DefaultBuilder<F>{}(prims, BoxPrimConverter<F>{});

  suite.check(bvh.getNodes().size() > 1, "legacy: non-trivial tree");

  RayT<F> forward(V3<F>{F(-5), F(0.5), F(0.5)}, V3<F>{F(1), F(0), F(0)});
  RayT<F> doubled(V3<F>{F(-5), F(0.5), F(0.5)}, V3<F>{F(2), F(0), F(0)});
  RayT<F> reversed(V3<F>{F(200), F(0.5), F(0.5)}, V3<F>{F(-1), F(0), F(0)});
  RayT<F> miss(V3<F>{F(-5), F(10), F(10)}, V3<F>{F(1), F(0), F(0)});

  uint64_t calls = 0;

  FastBVH::Traverser<F, BoxPrim<F>, BoxIntersector<F>> legacy(bvh, BoxIntersector<F>{&calls});

  struct Check {
    RayT<F> ray;
    int id;
  };

  const Check checks[3] = {{forward, 0}, {doubled, 0}, {reversed, 31}};

  for (int i = 0; i < 3; ++i) {
    const auto ref = brute(prims, checks[i].ray, F(0), inf<F>());
    auto hit = legacy.traverse(checks[i].ray);

    suite.check(static_cast<bool>(hit), "legacy: forward/reversed ray hit");
    suite.check(hit.t == ref.t, "legacy: hit distance matches independent enumeration");
    suite.check(hit.object != nullptr && hit.object->id == checks[i].id,
                "legacy: primitive identity matches independent enumeration");
  }

  // A ray outside the tree: the legacy traverser must report a miss.
  suite.check(!static_cast<bool>(legacy.traverse(miss)), "legacy: miss ray produces no intersection");

  // OnlyTestOcclusion keeps its existing behavior.
  FastBVH::Traverser<F, BoxPrim<F>, BoxIntersector<F>, FastBVH::OnlyTestOcclusion> any(
      bvh, BoxIntersector<F>{&calls});

  suite.check(static_cast<bool>(any.traverse(forward)), "legacy: occlusion flag finds an existing hit");
  suite.check(!static_cast<bool>(any.traverse(miss)), "legacy: occlusion flag rejects a miss");

  // The packet and legacy traversers agree on a coherent set of rays.
  auto rays = same4(forward);
  auto mins = fill4(F(0));
  auto maxs = fill4(inf<F>());

  FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{nullptr});
  auto got = packet.traverse(rays, mins, maxs, 0x0F);

  auto legacy_hit = legacy.traverse(forward);
  for (int lane = 0; lane < 4; ++lane) {
    suite.check(got[lane].t == legacy_hit.t && got[lane].object == legacy_hit.object,
                "legacy: packet agrees with the original traverser");
  }
}

//! Differential check: a packet of four unrelated rays is compared lane by
//! lane against an exhaustive scan of the primitives, with per-lane windows.
template <typename F>
void runDivergentRandom(Suite& suite) {
  Lcg rng;

  std::vector<BoxPrim<F>> prims;
  for (int i = 0; i < 40; ++i) {
    const F x0 = F(rng.range(-20, 20));
    const F y0 = F(rng.range(-20, 20));
    const F z0 = F(rng.range(-20, 20));
    const F sx = F(rng.range(0.5, 4));
    const F sy = F(rng.range(0.5, 4));
    const F sz = F(rng.range(0.5, 4));
    prims.push_back(makeBox<F>(x0, x0 + sx, y0, y0 + sy, z0, z0 + sz, i));
  }

  auto bvh = FastBVH::DefaultBuilder<F>{}(prims, BoxPrimConverter<F>{});

  suite.check(bvh.getNodes().size() > 1, "divergent random: non-trivial tree");

  for (int trial = 0; trial < 8; ++trial) {
    std::array<RayT<F>, 4> rays{{
        RayT<F>(V3<F>{F(rng.range(-25, 25)), F(rng.range(-25, 25)), F(rng.range(-25, 25))},
                V3<F>{F(rng.range(-1, 1)), F(rng.range(-1, 1)), F(rng.range(-1, 1))}),
        RayT<F>(V3<F>{F(rng.range(-25, 25)), F(rng.range(-25, 25)), F(rng.range(-25, 25))},
                V3<F>{F(rng.range(-1, 1)), F(rng.range(-1, 1)), F(rng.range(-1, 1))}),
        RayT<F>(V3<F>{F(rng.range(-25, 25)), F(rng.range(-25, 25)), F(rng.range(-25, 25))},
                V3<F>{F(rng.range(-1, 1)), F(rng.range(-1, 1)), F(rng.range(-1, 1))}),
        RayT<F>(V3<F>{F(rng.range(-25, 25)), F(rng.range(-25, 25)), F(rng.range(-25, 25))},
                V3<F>{F(rng.range(-1, 1)), F(rng.range(-1, 1)), F(rng.range(-1, 1))}),
    }};

    std::array<F, 4> mins;
    std::array<F, 4> maxs;
    for (int lane = 0; lane < 4; ++lane) {
      mins[lane] = F(rng.range(0, 10));
      maxs[lane] = (lane == 3) ? inf<F>() : F(mins[lane] + rng.range(0, 30));
    }

    FastBVH::PacketTraverser<F, BoxPrim<F>, BoxIntersector<F>> packet(bvh, BoxIntersector<F>{nullptr});
    auto got = packet.traverse(rays, mins, maxs, 0x0F);

    for (int lane = 0; lane < 4; ++lane) {
      const auto ref = brute(prims, rays[lane], mins[lane], maxs[lane]);
      expectLane(suite, got, lane, ref,
                 "divergent random trial " + std::to_string(trial) + " lane " + std::to_string(lane));
    }
  }
}

}  // namespace

void register_tests(Suite& suite) {
  suite.add("coherent", [](Suite& s) {
    runCoherent<float>(s);
    runCoherent<double>(s);
  });

  suite.add("divergent", [](Suite& s) {
    runDivergent<float>(s);
    runDivergent<double>(s);
    runDivergentRandom<float>(s);
    runDivergentRandom<double>(s);
  });

  suite.add("intervals", [](Suite& s) {
    runIntervals<float>(s);
    runIntervals<double>(s);
  });

  suite.add("occlusion", [](Suite& s) {
    runOcclusion<float>(s);
    runOcclusion<double>(s);
  });

  suite.add("legacy", [](Suite& s) {
    runLegacy<float>(s);
    runLegacy<double>(s);
  });
}
