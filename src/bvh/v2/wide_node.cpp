#include "bvh/v2/wide_node.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace bvh::v2 {

namespace {

/// Work item describing one wide inner node still to be emitted: its slot in the wide node array
/// and the current frontier of source binary node indices it should cover.
struct CollapseWork {
    size_t wide_id;
    std::vector<size_t> frontier;
};

} // namespace

Bvh<WideNode<float, 3>> collapse_to_wide(const Bvh<Node<float, 3>>& binary) {
    using WideNode = WideNode<float, 3>;
    using Index = typename WideNode::Index;

    Bvh<WideNode> wide;
    if (binary.nodes.empty())
        return wide;

    // The primitive ordering and every leaf primitive range is preserved verbatim.
    wide.prim_ids = binary.prim_ids;

    // A single-leaf tree collapses to a single leaf node (no enclosing wide inner node).
    if (binary.nodes[0].is_leaf()) {
        WideNode leaf;
        leaf.set_bbox(binary.nodes[0].get_bbox());
        leaf.index = binary.nodes[0].index;
        leaf.child_count_ = 0;
        leaf.parent_id_ = WideNode::invalid_id;
        wide.nodes.emplace_back(leaf);
        return wide;
    }

    std::vector<CollapseWork> work;
    work.push_back(CollapseWork { 0, std::vector<size_t> { 0 } });
    wide.nodes.emplace_back(); // root slot, filled in when its work item runs

    while (!work.empty()) {
        CollapseWork current = std::move(work.back());
        work.pop_back();

        auto& frontier = current.frontier;

        // Repeatedly expand the expandable frontier entry whose bounding box has the largest half
        // surface area, replacing it in place by its two source children, until the frontier holds
        // four entries or contains only leaves. Equal areas resolve to the smaller source index.
        while (frontier.size() < WideNode::max_children) {
            size_t best = static_cast<size_t>(-1);
            float best_area = -std::numeric_limits<float>::infinity();
            for (size_t i = 0; i < frontier.size(); ++i) {
                const auto& src_node = binary.nodes[frontier[i]];
                if (src_node.is_leaf())
                    continue;
                float area = src_node.get_bbox().get_half_area();
                if (area > best_area ||
                    (area == best_area &&
                     (best == static_cast<size_t>(-1) || frontier[i] < frontier[best])))
                {
                    best_area = area;
                    best = i;
                }
            }
            if (best == static_cast<size_t>(-1))
                break;

            const size_t left_id = binary.nodes[frontier[best]].index.first_id();
            const size_t right_id = left_id + 1;
            // Replace the entry by [left, right], keeping the rest of the frontier in order.
            frontier[best] = left_id;
            frontier.insert(frontier.begin() + static_cast<std::ptrdiff_t>(best) + 1, right_id);
        }

        auto& wide_inner = wide.nodes[current.wide_id];
        auto inner_bbox = BBox<float, 3>::make_empty();
        for (size_t src_id : frontier)
            inner_bbox.extend(binary.nodes[src_id].get_bbox());
        wide_inner.set_bbox(inner_bbox);
        wide_inner.child_count_ = static_cast<uint8_t>(frontier.size());
        wide_inner.index = Index::make_inner(wide.nodes.size());

        // The children are stored contiguously. Leaves are copied directly; inner children get a
        // placeholder node and a new work item that fills it with its own contiguous block. Every
        // child points back at the wide inner node just emitted.
        for (size_t src_id : frontier) {
            const auto& src_node = binary.nodes[src_id];
            if (src_node.is_leaf()) {
                WideNode leaf;
                leaf.set_bbox(src_node.get_bbox());
                leaf.index = src_node.index;
                leaf.child_count_ = 0;
                leaf.parent_id_ = static_cast<uint32_t>(current.wide_id);
                wide.nodes.emplace_back(leaf);
            } else {
                const size_t child_wide_id = wide.nodes.size();
                WideNode placeholder;
                placeholder.parent_id_ = static_cast<uint32_t>(current.wide_id);
                wide.nodes.emplace_back(placeholder);
                work.push_back(CollapseWork {
                    child_wide_id, std::vector<size_t> { src_id } });
            }
        }
    }

    return wide;
}

} // namespace bvh::v2
