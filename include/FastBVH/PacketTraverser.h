#pragma once

#include <FastBVH/BBox.h>
#include <FastBVH/BVH.h>
#include <FastBVH/Intersection.h>
#include <FastBVH/Ray.h>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace FastBVH {

//! \brief Statistics collected by a single packet query.
//!
//! Every query starts counting from zero. The counters are only
//! touched when the caller passes a @ref PacketStats pointer.
//!
//! - \c node_loads is incremented once for each node record read out of
//!   the BVH node array: the record of a node being processed, plus the
//!   two child records examined when a node's bounding boxes are tested.
//!   A stack entry whose lanes have all been resolved before its node
//!   record is read does not count.
//! - \c box_lane_tests is incremented once for every (bounding box,
//!   ray lane) intersection test performed during traversal.
//! - \c primitive_lane_tests is incremented once for every call to
//!   the user supplied primitive intersector.
struct PacketStats final {
  //! Number of node records read from the node array.
  uint64_t node_loads = 0;

  //! Number of box/lane intersection tests.
  uint64_t box_lane_tests = 0;

  //! Number of primitive intersector invocations.
  uint64_t primitive_lane_tests = 0;
};

//! \brief Contains implementation details for the @ref PacketTraverser class.
namespace PacketTraverserImpl {

//! \brief Stack entry for the shared packet traversal.
//!
//! A node may still be relevant to only a subset of the packet's lanes,
//! and each lane keeps its own entry/far distances for that node.
template <typename Float>
struct PacketTraversal final {
  //! The index of the node to be traversed.
  uint32_t node = 0;

  //! Bit set of the lanes that still need service at this node.
  uint8_t lanes = 0;

  //! Earliest accepted hit distance for each lane at this node.
  std::array<Float, 4> near{};

  //! Latest accepted hit distance for each lane at this node.
  std::array<Float, 4> far{};
};

//! \brief Intersects an axis-aligned box with a single ray.
//!
//! This is a portable slab test with an explicit branch for zero
//! direction components. Taking the branch avoids the \c 0 * inf
//! indeterminate form (including negative zero), which otherwise lets
//! axis-parallel rays slip through boundaries of the box.
//!
//! \param box The box being tested.
//! \param ray The ray being traced.
//! \param tnear Receives the distance at which the ray enters the box.
//! \param tfar Receives the distance at which the ray leaves the box.
//! \return True if the ray intersects the box, false otherwise.
template <typename Float>
inline bool intersectBoxLane(const BBox<Float>& box, const Ray<Float>& ray, Float& tnear, Float& tfar) noexcept {
  Float t0 = -std::numeric_limits<Float>::infinity();
  Float t1 = std::numeric_limits<Float>::infinity();

  const Float zero = Float(0);

  for (unsigned int axis = 0; axis < 3; ++axis) {
    const Float origin = ray.o[axis];
    const Float direction = ray.d[axis];

    if (direction == zero) {
      // The ray stays at a constant coordinate on this axis. It only
      // intersects the box if that coordinate is inside the closed
      // interval of the box, including the boundary itself.
      if (origin < box.min[axis] || origin > box.max[axis]) {
        return false;
      }
      continue;
    }

    const Float inv = Float(1) / direction;

    Float near = (box.min[axis] - origin) * inv;
    Float far = (box.max[axis] - origin) * inv;

    if (near > far) {
      std::swap(near, far);
    }

    if (near > t0) {
      t0 = near;
    }

    if (far < t1) {
      t1 = far;
    }

    if (t0 > t1) {
      return false;
    }
  }

  tnear = t0;
  tfar = t1;

  return true;
}

}  // namespace PacketTraverserImpl

//! \brief Traverses a BVH with four rays that share the same traversal stack.
//!
//! Unlike calling a single-ray traverser four times, a packet traversal
//! visits each BVH node at most once per stack entry, and does so for
//! every lane that still needs that node. Each lane has its own distance
//! window and its own nearest hit, so progress (or an early hit) on one
//! lane can never tighten or discard another lane's work.
//!
//! The primitive geometry is entirely supplied by the user's intersector;
//! no primitive shape is assumed by this class.
//!
//! A query only reads the BVH, the rays and the primitives. Its working
//! storage is local to the call, so concurrent queries against the same
//! read-only tree are safe. Inactive lanes never reach the intersector.
//!
//! \tparam Float The floating point type used by the vectors.
//! \tparam Primitive The type of primitive stored in the BVH.
//! \tparam Intersector The user supplied primitive intersector type.
template <typename Float, typename Primitive, typename Intersector>
class PacketTraverser final {
  //! The BVH to be traversed.
  const BVH<Float, Primitive>& bvh;

  //! The ray-primitive intersector.
  Intersector intersector;

  //! Shared implementation for traverse() and occluded().
  //! \param rays The four rays to trace.
  //! \param mins The inclusive lower bound of each lane's distance window.
  //! \param maxs The inclusive upper bound of each lane's distance window.
  //! \param active Low four bits selecting the lanes to serve.
  //! \param stats Optional counter destination. Counters start at zero.
  //! \param occluded Receives the low four bits of lanes that found a hit.
  //! \return The closest accepted intersection for each lane.
  template <bool OnlyOcclusion>
  std::array<Intersection<Float, Primitive>, 4> trace(const std::array<Ray<Float>, 4>& rays,
                                                      const std::array<Float, 4>& mins,
                                                      const std::array<Float, 4>& maxs,
                                                      uint8_t active,
                                                      PacketStats* stats,
                                                      uint8_t& occluded) const;

 public:
  //! Constructs a new packet traverser.
  //! \param bvh_ The BVH to be traversed.
  //! \param intersector_ The ray-primitive intersector.
  constexpr PacketTraverser(const BVH<Float, Primitive>& bvh_, const Intersector& intersector_) noexcept
      : bvh(bvh_), intersector(intersector_) {}

  //! Traces four rays through the BVH, sharing a single traversal stack.
  //! \param rays The four rays to trace.
  //! \param tmin The inclusive lower distance bound for each ray.
  //! \param tmax The inclusive upper distance bound for each ray.
  //! \param active Low four bits selecting the lanes to serve. All other
  //! lanes return an empty intersection and never invoke the intersector.
  //! \param stats Optional statistics sink. It is reset to zero by the call.
  //! \return Four intersections. A hit is accepted only when its distance
  //! lies inside the lane's closed [tmin, tmax] window.
  std::array<Intersection<Float, Primitive>, 4> traverse(const std::array<Ray<Float>, 4>& rays,
                                                        const std::array<Float, 4>& tmin,
                                                        const std::array<Float, 4>& tmax,
                                                        uint8_t active,
                                                        PacketStats* stats = nullptr) const;

  //! Tests four rays for occlusion, sharing a single traversal stack.
  //! Once a lane finds an intersection in its distance window it is removed
  //! from service; the remaining lanes keep traversing.
  //! \param rays The four rays to test.
  //! \param tmin The inclusive lower distance bound for each ray.
  //! \param tmax The inclusive upper distance bound for each ray.
  //! \param active Low four bits selecting the lanes to serve.
  //! \param stats Optional statistics sink. It is reset to zero by the call.
  //! \return The low four bits of lanes that found an intersection.
  uint8_t occluded(const std::array<Ray<Float>, 4>& rays,
                   const std::array<Float, 4>& tmin,
                   const std::array<Float, 4>& tmax,
                   uint8_t active,
                   PacketStats* stats = nullptr) const;
};

template <typename Float, typename Primitive, typename Intersector>
template <bool OnlyOcclusion>
std::array<Intersection<Float, Primitive>, 4> PacketTraverser<Float, Primitive, Intersector>::trace(
    const std::array<Ray<Float>, 4>& rays,
    const std::array<Float, 4>& mins,
    const std::array<Float, 4>& maxs,
    uint8_t active,
    PacketStats* stats,
    uint8_t& occluded) const {
  using PacketTraverserImpl::intersectBoxLane;
  using PacketTraverserImpl::PacketTraversal;

  std::array<Intersection<Float, Primitive>, 4> result{};

  PacketStats counters;

  uint8_t hit_mask = 0;

  active = static_cast<uint8_t>(active & 0x0Fu);

  const auto nodes = bvh.getNodes();

  if (active == 0 || nodes.size() == 0) {
    if (stats != nullptr) {
      *stats = counters;
    }
    occluded = hit_mask;
    return result;
  }

  const auto primitives = bvh.getPrimitives();

  const Float infinity = std::numeric_limits<Float>::infinity();

  // Working storage belongs to this call. It grows as needed so that any
  // tree depth is supported without a fixed stack limit.
  std::vector<PacketTraversal<Float>> stack;
  stack.reserve(64);

  // Push the root. No box test is performed for the root itself, so every
  // active lane starts at its own window entry bound.
  PacketTraversal<Float> entry;
  entry.node = 0;
  entry.lanes = active;
  for (unsigned int lane = 0; lane < 4; ++lane) {
    entry.near[lane] = mins[lane];
    entry.far[lane] = maxs[lane];
  }
  stack.push_back(entry);

  while (!stack.empty()) {
    const auto item = stack.back();
    stack.pop_back();

    // Work out which lanes still need this node. Each lane is culled
    // independently: one lane's nearest hit says nothing about another
    // lane's window or about which child another lane needs.
    uint8_t lanes = static_cast<uint8_t>(item.lanes & active);
    for (unsigned int lane = 0; lane < 4; ++lane) {
      const uint8_t bit = static_cast<uint8_t>(1u << lane);
      if ((lanes & bit) == 0) {
        continue;
      }
      if ((hit_mask & bit) != 0) {
        lanes = static_cast<uint8_t>(lanes & ~bit);
        continue;
      }
      if (item.near[lane] > result[lane].t) {
        lanes = static_cast<uint8_t>(lanes & ~bit);
      }
    }

    if (lanes == 0) {
      continue;
    }

    const auto& node = nodes[item.node];

    ++counters.node_loads;

    if (node.isLeaf()) {
      for (uint32_t o = 0; o < node.primitive_count; ++o) {
        const auto& primitive = primitives[node.start + o];

        for (unsigned int lane = 0; lane < 4; ++lane) {
          const uint8_t bit = static_cast<uint8_t>(1u << lane);
          if ((lanes & bit) == 0) {
            continue;
          }

          ++counters.primitive_lane_tests;

          const auto current = intersector(primitive, rays[lane]);

          if (!current) {
            continue;
          }

          // A candidate outside the lane's closed distance window is
          // ignored. The intersector is not asked for another root.
          if (current.t < mins[lane] || current.t > maxs[lane]) {
            continue;
          }

          if (OnlyOcclusion) {
            hit_mask = static_cast<uint8_t>(hit_mask | bit);
            lanes = static_cast<uint8_t>(lanes & ~bit);
          } else if (current.t < result[lane].t) {
            result[lane] = current;
          }
        }

        if (OnlyOcclusion && lanes == 0) {
          break;
        }
      }
    } else {
      uint8_t left_lanes = 0;
      uint8_t right_lanes = 0;

      std::array<Float, 4> left_near{};
      std::array<Float, 4> left_far{};
      std::array<Float, 4> right_near{};
      std::array<Float, 4> right_far{};

      Float left_min = infinity;
      Float right_min = infinity;

      // Reading the two child node records below is counted as two node
      // loads, in addition to the record of this node read above.
      const auto& left_node = nodes[item.node + 1];
      const auto& right_node = nodes[item.node + node.right_offset];

      counters.node_loads += 2;

      for (unsigned int lane = 0; lane < 4; ++lane) {
        const uint8_t bit = static_cast<uint8_t>(1u << lane);
        if ((lanes & bit) == 0) {
          continue;
        }

        ++counters.box_lane_tests;

        Float test_near = 0;
        Float test_far = 0;

        if (intersectBoxLane(left_node.bbox, rays[lane], test_near, test_far)) {
          // Clamp the box interval to the lane's distance window. Hits
          // outside the window are irrelevant to that lane.
          const Float near = (test_near > mins[lane]) ? test_near : mins[lane];
          const Float far = (test_far < maxs[lane]) ? test_far : maxs[lane];

          if (near <= far && (OnlyOcclusion || near <= result[lane].t)) {
            left_lanes = static_cast<uint8_t>(left_lanes | bit);
            left_near[lane] = near;
            left_far[lane] = far;
            if (near < left_min) {
              left_min = near;
            }
          }
        }

        ++counters.box_lane_tests;

        if (intersectBoxLane(right_node.bbox, rays[lane], test_near, test_far)) {
          const Float near = (test_near > mins[lane]) ? test_near : mins[lane];
          const Float far = (test_far < maxs[lane]) ? test_far : maxs[lane];

          if (near <= far && (OnlyOcclusion || near <= result[lane].t)) {
            right_lanes = static_cast<uint8_t>(right_lanes | bit);
            right_near[lane] = near;
            right_far[lane] = far;
            if (near < right_min) {
              right_min = near;
            }
          }
        }
      }

      // Push the farther child first so the closer one is processed first.
      // The two children may serve different lane sets; both are kept.
      const bool left_first = left_min <= right_min;

      auto push_child = [&stack](uint32_t child, uint8_t lanes_, const std::array<Float, 4>& near_,
                                 const std::array<Float, 4>& far_) {
        PacketTraversal<Float> child_entry;
        child_entry.node = child;
        child_entry.lanes = lanes_;
        child_entry.near = near_;
        child_entry.far = far_;
        stack.push_back(child_entry);
      };

      if (left_first) {
        if (right_lanes != 0) {
          push_child(item.node + node.right_offset, right_lanes, right_near, right_far);
        }
        if (left_lanes != 0) {
          push_child(item.node + 1, left_lanes, left_near, left_far);
        }
      } else {
        if (left_lanes != 0) {
          push_child(item.node + 1, left_lanes, left_near, left_far);
        }
        if (right_lanes != 0) {
          push_child(item.node + node.right_offset, right_lanes, right_near, right_far);
        }
      }
    }
  }

  if (stats != nullptr) {
    *stats = counters;
  }

  occluded = hit_mask;

  return result;
}

template <typename Float, typename Primitive, typename Intersector>
std::array<Intersection<Float, Primitive>, 4> PacketTraverser<Float, Primitive, Intersector>::traverse(
    const std::array<Ray<Float>, 4>& rays,
    const std::array<Float, 4>& tmin,
    const std::array<Float, 4>& tmax,
    uint8_t active,
    PacketStats* stats) const {
  uint8_t occluded = 0;
  return trace<false>(rays, tmin, tmax, active, stats, occluded);
}

template <typename Float, typename Primitive, typename Intersector>
uint8_t PacketTraverser<Float, Primitive, Intersector>::occluded(const std::array<Ray<Float>, 4>& rays,
                                                                 const std::array<Float, 4>& tmin,
                                                                 const std::array<Float, 4>& tmax,
                                                                 uint8_t active,
                                                                 PacketStats* stats) const {
  uint8_t occluded_mask = 0;
  trace<true>(rays, tmin, tmax, active, stats, occluded_mask);
  return occluded_mask;
}

}  // namespace FastBVH
