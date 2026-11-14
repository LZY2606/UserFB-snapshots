#ifndef BVH_V2_WIDE_NODE_H
#define BVH_V2_WIDE_NODE_H

#include "bvh/v2/node.h"
#include "bvh/v2/bvh.h"
#include "bvh/v2/index.h"
#include "bvh/v2/vec.h"
#include "bvh/v2/bbox.h"
#include "bvh/v2/ray.h"
#include "bvh/v2/stream.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stack>
#include <utility>
#include <vector>

namespace bvh::v2 {

/// Wide BVH node, whose inner nodes can hold up to `MaxChildren` children (four by default) stored
/// contiguously in the node array. Like the binary `Node`, an inner node references its children
/// through `index.first_id()`, while a leaf references a contiguous range of primitives. The actual
/// number of children of an inner node is kept in `child_count_`: unused slots are never stored, so
/// an inner node can hold two, three, or four children and there are no fake leaf nodes. By
/// definition, inner nodes do not contain primitives and leaves have no children.
template <
    typename T,
    size_t Dim,
    size_t MaxChildren = 4,
    size_t IndexBits = sizeof(T) * CHAR_BIT,
    size_t PrimCountBits = 4>
struct WideNode {
    using Scalar = T;
    using Index = bvh::v2::Index<IndexBits, PrimCountBits>;
    using Ray = bvh::v2::Ray<T, Dim>;
    using BvhType = Bvh<WideNode>;

    static constexpr size_t dimension = Dim;
    static constexpr size_t max_children = MaxChildren;
    static constexpr size_t prim_count_bits = PrimCountBits;
    static constexpr size_t index_bits = IndexBits;

    /// Sentinel parent id, used by the root node which has no parent.
    static constexpr uint32_t invalid_id = std::numeric_limits<uint32_t>::max();

    /// Bounds of the node, laid out in memory as `[min_x, max_x, min_y, max_y, ...]`.
    std::array<T, Dim * 2> bounds{};

    /// Index to the children of an inner node, or to the primitives for a leaf node.
    Index index{};

    /// Number of children actually stored by an inner node (between 2 and `MaxChildren`).
    /// Always zero for a leaf.
    uint8_t child_count_ = 0;

    /// Index of the parent node in the wide node array. Because an inner index only stores the
    /// start of its contiguous children block, this back pointer lets traversal recover the parent
    /// (and hence the real child count) in O(1). The root uses `invalid_id`.
    uint32_t parent_id_ = invalid_id;

    WideNode() = default;

    bool operator == (const WideNode&) const = default;
    bool operator != (const WideNode&) const = default;

    BVH_ALWAYS_INLINE bool is_leaf() const { return index.is_leaf(); }

    /// Returns the number of children actually stored by this node. Leaves return 0.
    BVH_ALWAYS_INLINE size_t child_count() const {
        return is_leaf() ? size_t{0} : static_cast<size_t>(child_count_);
    }

    BVH_ALWAYS_INLINE BBox<T, Dim> get_bbox() const {
        return BBox<T, Dim>(
            Vec<T, Dim>::generate([&] (size_t i) { return bounds[i * 2]; }),
            Vec<T, Dim>::generate([&] (size_t i) { return bounds[i * 2 + 1]; }));
    }

    BVH_ALWAYS_INLINE void set_bbox(const BBox<T, Dim>& bbox) {
        static_for<0, Dim>([&] (size_t i) {
            bounds[i * 2 + 0] = bbox.min[i];
            bounds[i * 2 + 1] = bbox.max[i];
        });
    }

    BVH_ALWAYS_INLINE Vec<T, Dim> get_min_bounds(const Octant& octant) const {
        return Vec<T, Dim>::generate([&] (size_t i) { return bounds[2 * static_cast<uint32_t>(i) + octant[i]]; });
    }

    BVH_ALWAYS_INLINE Vec<T, Dim> get_max_bounds(const Octant& octant) const {
        return Vec<T, Dim>::generate([&] (size_t i) { return bounds[2 * static_cast<uint32_t>(i) + 1 - octant[i]]; });
    }

    /// Robust ray-node intersection routine. See "Robust BVH Ray Traversal", by T. Ize.
    [[nodiscard]] BVH_ALWAYS_INLINE std::pair<T, T> intersect_robust(
        const Ray& ray,
        const Vec<T, Dim>& inv_dir,
        const Vec<T, Dim>& inv_dir_pad,
        const Octant& octant) const
    {
        auto tmin = (get_min_bounds(octant) - ray.org) * inv_dir;
        auto tmax = (get_max_bounds(octant) - ray.org) * inv_dir_pad;
        return make_intersection_result(ray, tmin, tmax);
    }

    [[nodiscard]] BVH_ALWAYS_INLINE std::pair<T, T> intersect_fast(
        const Ray& ray,
        const Vec<T, Dim>& inv_dir,
        const Vec<T, Dim>& inv_org,
        const Octant& octant) const
    {
        auto tmin = fast_mul_add(get_min_bounds(octant), inv_dir, inv_org);
        auto tmax = fast_mul_add(get_max_bounds(octant), inv_dir, inv_org);
        return make_intersection_result(ray, tmin, tmax);
    }

    BVH_ALWAYS_INLINE void serialize(OutputStream& stream) const {
        for (auto&& bound : bounds)
            stream.write(bound);
        stream.write(index.value);
        stream.write(static_cast<uint8_t>(child_count_));
        stream.write(parent_id_);
    }

    [[nodiscard]] static BVH_ALWAYS_INLINE WideNode deserialize(InputStream& stream) {
        WideNode node;
        for (auto& bound : node.bounds)
            bound = stream.read<T>();
        node.index = Index(stream.read<typename Index::Type>());
        node.child_count_ = stream.read<uint8_t>();
        node.parent_id_ = stream.read<uint32_t>();
        return node;
    }

private:
    BVH_ALWAYS_INLINE static std::pair<T, T> make_intersection_result(
        const Ray& ray,
        const Vec<T, Dim>& tmin,
        const Vec<T, Dim>& tmax)
    {
        auto t0 = ray.tmin;
        auto t1 = ray.tmax;
        static_for<0, Dim>([&] (size_t i) {
            t0 = robust_max(tmin[i], t0);
            t1 = robust_min(tmax[i], t1);
        });
        return std::pair<T, T> { t0, t1 };
    }

public:

    // --------------------------------------------------------------------------------------------
    // Wide implementations of the native Bvh operations. Bvh<WideNode> dispatches to these through
    // `if constexpr (requires { Node::max_children; })`, so binary nodes keep their original code.
    // --------------------------------------------------------------------------------------------

    /// Ray-box entry distance for the current ray, or +infinity when the ray misses the node.
    template <bool IsRobust>
    BVH_ALWAYS_INLINE static T entry_distance(
        const WideNode& node,
        const Ray& ray,
        const Vec<T, Dim>& inv_dir,
        const Vec<T, Dim>& inv_dir_pad,
        const Vec<T, Dim>& inv_org,
        const Octant& octant)
    {
        std::pair<T, T> intr;
        if constexpr (IsRobust)
            intr = node.intersect_robust(ray, inv_dir, inv_dir_pad, octant);
        else
            intr = node.intersect_fast(ray, inv_dir, inv_org, octant);
        return intr.first <= intr.second
            ? intr.first
            : std::numeric_limits<T>::infinity();
    }

    /// Wide `Bvh::intersect`. In closest-hit mode the children of every inner node are visited by
    /// ascending ray-box entry distance (ties broken by the child slot), while any-hit mode keeps
    /// the slot order and stops at the first valid hit. Inner nodes are expanded against the
    /// `ray.tmax` in effect at the time; leaf callbacks may shorten it.
    template <bool IsAnyHit, bool IsRobust, typename Stack, typename LeafFn, typename InnerFn>
    static void wide_intersect(
        const BvhType& bvh,
        const Ray& ray,
        Index start,
        Stack& stack,
        LeafFn&& leaf_fn,
        InnerFn&& inner_fn)
    {
        // An empty wide tree starts with a default index and must not trigger any callback.
        if (bvh.nodes.empty())
            return;

        auto inv_dir = ray.template get_inv_dir<!IsRobust>();
        auto inv_org = -inv_dir * ray.org;
        auto inv_dir_pad = Ray::pad_inv_dir(inv_dir);
        auto octant = ray.get_octant();

        struct Candidate {
            T t_entry;
            Index index;
            size_t slot;
        };

        // An inner index points at the start of its contiguous children block; the parent node is
        // recovered in O(1) through the back pointer stored on its first child.
        auto parent_of = [&bvh] (Index inner) -> const WideNode& {
            return bvh.nodes[bvh.nodes[inner.first_id()].parent_id_];
        };

        stack.push(start);
        while (!stack.is_empty()) {
            auto top = stack.pop();

            if (top.prim_count() != 0) {
                [[maybe_unused]] auto was_hit =
                    leaf_fn(top.first_id(), top.first_id() + top.prim_count());
                if constexpr (IsAnyHit) {
                    if (was_hit)
                        return;
                }
                continue;
            }

            const auto& parent = parent_of(top);
            // A sibling leaf may have shortened the ray: re-test against the current tmax.
            if (entry_distance<IsRobust>(parent, ray, inv_dir, inv_dir_pad, inv_org, octant) > ray.tmax)
                continue;

            inner_fn(parent);

            const auto first = parent.index.first_id();
            const auto child_count = parent.child_count();

            std::array<Candidate, MaxChildren> candidates{};
            size_t candidate_count = 0;
            for (size_t slot = 0; slot < child_count; ++slot) {
                const auto& child = bvh.nodes[first + slot];
                auto t_entry = entry_distance<IsRobust>(
                    child, ray, inv_dir, inv_dir_pad, inv_org, octant);
                if (t_entry <= ray.tmax)
                    candidates[candidate_count++] = Candidate { t_entry, child.index, slot };
            }
            if (candidate_count == 0)
                continue;

            if constexpr (!IsAnyHit) {
                std::sort(candidates.begin(), candidates.begin() + candidate_count,
                    [] (const Candidate& a, const Candidate& b) {
                        if (a.t_entry != b.t_entry)
                            return a.t_entry < b.t_entry;
                        return a.slot < b.slot;
                    });
            }

            // LIFO stack: push the farthest candidate first so the nearest one is popped first.
            for (size_t i = candidate_count; i-- > 0;)
                stack.push(candidates[i].index);
        }
    }

    /// Wide `Bvh::traverse_top_down`: a plain depth-first traversal in child slot order, calling
    /// the inner callback (if any) with the parent node and the leaf callback with prim ranges.
    template <bool IsAnyHit, typename Stack, typename LeafFn, typename InnerFn>
    static void wide_traverse_top_down(
        const BvhType& bvh,
        Index start,
        Stack& stack,
        LeafFn&& leaf_fn,
        InnerFn&& inner_fn)
    {
        if (bvh.nodes.empty())
            return;

        stack.push(start);
        while (!stack.is_empty()) {
            auto top = stack.pop();
            if (top.prim_count() != 0) {
                [[maybe_unused]] auto was_hit =
                    leaf_fn(top.first_id(), top.first_id() + top.prim_count());
                if constexpr (IsAnyHit) {
                    if (was_hit)
                        return;
                }
                continue;
            }

            const auto& parent = bvh.nodes[bvh.nodes[top.first_id()].parent_id_];
            inner_fn(parent);

            const auto first = parent.index.first_id();
            const auto child_count = parent.child_count();
            for (size_t slot = child_count; slot-- > 0;)
                stack.push(bvh.nodes[first + slot].index);
        }
    }

    /// Wide `Bvh::extract_bvh`: copies the subtree, relocating node blocks and leaf prim slots.
    /// The extracted `prim_ids` still reference the original primitive indices.
    static BvhType wide_extract_bvh(const BvhType& bvh, size_t root_id) {
        BvhType extracted;
        extracted.nodes.emplace_back();

        // Relocation tables: `remap[src] = dst` for copied nodes, and the inverse for fixing back
        // pointers. Unreachable source nodes stay at `invalid_id`.
        std::vector<uint32_t> remap(bvh.nodes.size(), WideNode::invalid_id);
        std::vector<uint32_t> src_of_dst;
        src_of_dst.reserve(bvh.nodes.size());
        remap[root_id] = 0;
        src_of_dst.push_back(static_cast<uint32_t>(root_id));

        std::stack<std::pair<size_t, size_t>> pending;
        pending.emplace(root_id, 0);
        while (!pending.empty()) {
            auto [src_id, dst_id] = pending.top();
            pending.pop();
            const auto& src_node = bvh.nodes[src_id];
            auto& dst_node = extracted.nodes[dst_id];
            dst_node = src_node;

            if (src_node.is_leaf()) {
                dst_node.index.set_first_id(extracted.prim_ids.size());
                std::copy_n(
                    bvh.prim_ids.begin() + src_node.index.first_id(),
                    src_node.index.prim_count(),
                    std::back_inserter(extracted.prim_ids));
            } else {
                const auto count = src_node.child_count();
                const size_t base = extracted.nodes.size();
                dst_node.index.set_first_id(base);
                if (src_of_dst.size() < base + count)
                    src_of_dst.resize(base + count, WideNode::invalid_id);
                for (size_t slot = count; slot-- > 0;) {
                    const size_t src_child = src_node.index.first_id() + slot;
                    const size_t dst_child = base + slot;
                    remap[src_child] = static_cast<uint32_t>(dst_child);
                    src_of_dst[dst_child] = static_cast<uint32_t>(src_child);
                    pending.emplace(src_child, dst_child);
                }
                // Note: This may invalidate `dst_node` so has to happen after any access to it.
                extracted.nodes.resize(base + count);
            }
        }

        // Relocate parent back pointers; a parent outside the extracted subtree becomes "none".
        for (size_t dst = 0; dst < extracted.nodes.size(); ++dst) {
            const auto src_parent = bvh.nodes[src_of_dst[dst]].parent_id_;
            extracted.nodes[dst].parent_id_ =
                (dst == 0 || src_parent == WideNode::invalid_id ||
                 remap[src_parent] == WideNode::invalid_id)
                    ? WideNode::invalid_id
                    : remap[src_parent];
        }
        return extracted;
    }

    /// Wide `Bvh::traverse_bottom_up`: leaves are visited first, then each inner node once all of
    /// its (real) children have been visited.
    template <typename BvhRef, typename LeafFn, typename InnerFn>
    static void wide_traverse_bottom_up(BvhRef& bvh, LeafFn&& leaf_fn, InnerFn&& inner_fn) {
        std::vector<size_t> parents(bvh.nodes.size(), 0);
        for (size_t i = 0; i < bvh.nodes.size(); ++i) {
            if (bvh.nodes[i].is_leaf())
                continue;
            const auto first = bvh.nodes[i].index.first_id();
            const auto count = bvh.nodes[i].child_count();
            for (size_t slot = 0; slot < count; ++slot)
                parents[first + slot] = i;
        }

        std::vector<bool> seen(bvh.nodes.size(), false);
        for (size_t i = bvh.nodes.size(); i-- > 0;) {
            if (!bvh.nodes[i].is_leaf())
                continue;
            leaf_fn(bvh.nodes[i]);
            seen[i] = true;
            for (size_t j = parents[i];; j = parents[j]) {
                auto& node = bvh.nodes[j];
                if (seen[j])
                    break;
                const auto first = node.index.first_id();
                const auto count = node.child_count();
                bool all_seen = true;
                for (size_t slot = 0; slot < count; ++slot) {
                    if (!seen[first + slot]) {
                        all_seen = false;
                        break;
                    }
                }
                if (!all_seen)
                    break;
                inner_fn(node);
                seen[j] = true;
            }
        }
    }
};

/// Collapses a binary BVH into a wide BVH in which every inner node holds between two and four
/// children. The source BVH is not modified; its `prim_ids` ordering and leaf primitive ranges are
/// preserved. Empty and single-leaf trees are supported.
Bvh<WideNode<float, 3>> collapse_to_wide(const Bvh<Node<float, 3>>& binary);

using WideNode3f = WideNode<float, 3>;

} // namespace bvh::v2

#endif
