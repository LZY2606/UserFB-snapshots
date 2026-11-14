// task_support/driver.c is compiled as C++ by the build script, so the check.h symbols use normal
// C++ linkage on both sides (the header itself deliberately has no extern "C").
#include "task_support/check.h"

#include <bvh/v2/wide_node.h>
#include <bvh/v2/default_builder.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

using namespace bvh::v2;

using N   = Node<float, 3>;
using W   = WideNode<float, 3>;
using BB  = Bvh<N>;
using WB  = Bvh<W>;
using V   = Vec<float, 3>;
using R   = Ray<float, 3>;
using Tr  = Tri<float, 3>;
using PTr = PrecomputedTri<float>;
using Box = BBox<float, 3>;

namespace {

BB build_binary(const std::vector<Tr>& tris, size_t max_leaf) {
    std::vector<Box> boxes;
    std::vector<V> centers;
    boxes.reserve(tris.size());
    centers.reserve(tris.size());
    for (const auto& t : tris) {
        boxes.push_back(t.get_bbox());
        centers.push_back(t.get_center());
    }
    DefaultBuilder<N>::Config cfg;
    cfg.quality = DefaultBuilder<N>::Quality::Low;
    cfg.min_leaf_size = 1;
    cfg.max_leaf_size = max_leaf;
    return DefaultBuilder<N>::build(
        std::span<const Box>(boxes.data(), boxes.size()),
        std::span<const V>(centers.data(), centers.size()), cfg);
}

// Independent brute-force intersection: every triangle is tested directly, with no BVH involved.
std::vector<std::pair<float, size_t>> brute_force(const std::vector<Tr>& tris, const R& ray) {
    std::vector<std::pair<float, size_t>> hits;
    R r = ray;
    for (size_t i = 0; i < tris.size(); ++i) {
        if (auto h = PTr(tris[i]).intersect(r))
            hits.emplace_back(std::get<0>(*h), i);
    }
    std::sort(hits.begin(), hits.end());
    return hits;
}

// Closest-hit query on the wide tree. Returns the original primitive id and the hit distance.
bool wide_closest(const WB& wb, const std::vector<Tr>& tris, const R& ray_in,
                  size_t& hit_id, float& hit_t, bool robust)
{
    R ray = ray_in;
    GrowingStack<W::Index> stack;
    hit_id = static_cast<size_t>(-1);
    auto leaf = [&] (size_t begin, size_t end) {
        bool any = false;
        for (size_t i = begin; i < end; ++i) {
            if (auto h = PTr(tris[wb.prim_ids[i]]).intersect(ray)) {
                hit_id = wb.prim_ids[i];
                ray.tmax = std::get<0>(*h);
                hit_t = ray.tmax;
                any = true;
            }
        }
        return any;
    };
    if (robust)
        wb.intersect<false, true>(ray, wb.nodes.empty() ? W::Index{} : wb.get_root().index, stack, leaf);
    else
        wb.intersect<false, false>(ray, wb.nodes.empty() ? W::Index{} : wb.get_root().index, stack, leaf);
    return hit_id != static_cast<size_t>(-1);
}

// Any-hit (shadow) query on the wide tree.
bool wide_any(const WB& wb, const std::vector<Tr>& tris, const R& ray_in, bool robust) {
    R ray = ray_in;
    GrowingStack<W::Index> stack;
    bool hit = false;
    auto leaf = [&] (size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (PTr(tris[wb.prim_ids[i]]).intersect(ray)) {
                hit = true;
                return true; // stop on the first valid hit
            }
        }
        return false;
    };
    if (robust)
        wb.intersect<true, true>(ray, wb.nodes.empty() ? W::Index{} : wb.get_root().index, stack, leaf);
    else
        wb.intersect<true, false>(ray, wb.nodes.empty() ? W::Index{} : wb.get_root().index, stack, leaf);
    return hit;
}

// Structural validation of a wide tree: real child counts, contiguous child blocks, consistent
// parent back pointers, and the reachable leaf set covering every stored primitive exactly once.
// When `renumbered` is true the primitive ids are expected to be a permutation of [0, id_bound)
// (a full tree); otherwise they only have to be unique and within [0, id_bound) (an extracted
// subtree, whose ids still reference the original primitives).
bool validate_wide(const WB& wb, size_t id_bound, bool renumbered = true) {
    if (wb.nodes.empty())
        return wb.prim_ids.empty();
    if (wb.nodes[0].parent_id_ != W::invalid_id)
        return false;

    for (size_t i = 0; i < wb.nodes.size(); ++i) {
        const auto& nd = wb.nodes[i];
        if (nd.is_leaf()) {
            if (nd.child_count_ != 0 || nd.child_count() != 0)
                return false;
            if (nd.index.prim_count() == 0)
                return false;
        } else {
            const size_t c = nd.child_count();
            if (c < 2 || c > W::max_children)
                return false;
            const size_t first = nd.index.first_id();
            if (first + c > wb.nodes.size())
                return false;
            for (size_t s = 0; s < c; ++s) {
                if (wb.nodes[first + s].parent_id_ != i)
                    return false;
            }
        }
    }

    // DFS over reachable nodes and leaf primitive ranges.
    std::vector<size_t> node_stack { 0 };
    std::vector<size_t> reached_leaves;
    size_t reached_nodes = 0;
    while (!node_stack.empty()) {
        size_t id = node_stack.back();
        node_stack.pop_back();
        ++reached_nodes;
        const auto& nd = wb.nodes[id];
        if (nd.is_leaf()) {
            reached_leaves.push_back(id);
            continue;
        }
        const size_t first = nd.index.first_id();
        for (size_t s = nd.child_count(); s-- > 0;)
            node_stack.push_back(first + s);
    }
    if (reached_nodes != wb.nodes.size())
        return false;

    std::vector<size_t> covered;
    for (size_t leaf_id : reached_leaves) {
        const auto& nd = wb.nodes[leaf_id];
        for (size_t k = 0; k < nd.index.prim_count(); ++k)
            covered.push_back(wb.prim_ids[nd.index.first_id() + k]);
    }
    // The reachable leaves must account for every stored primitive id exactly once.
    if (covered.size() != wb.prim_ids.size())
        return false;
    auto sorted = covered;
    std::sort(sorted.begin(), sorted.end());
    if (renumbered) {
        if (sorted.size() != id_bound)
            return false;
        for (size_t i = 0; i < sorted.size(); ++i)
            if (sorted[i] != i)
                return false;
    } else {
        if (sorted.empty() || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            return false;
        if (sorted.front() >= id_bound || sorted.back() >= id_bound)
            return false;
    }
    return true;
}

Box box_at(float x) {
    return Box(V(x, 0.0f, 0.0f), V(x + 1.0f, 1.0f, 1.0f));
}

// Hand-built binary tree with three leaves (two inner nodes): root -> [inner, leaf], with the
// inner node -> [leaf, leaf]. Siblings are stored at adjacent indices as the library requires.
BB make_three_leaf_binary() {
    BB b;
    b.prim_ids = { 0, 1, 2 };
    N root;  root.set_bbox(box_at(0));  root.index  = N::Index::make_inner(1);
    N inner; inner.set_bbox(box_at(0)); inner.index = N::Index::make_inner(3);
    N l2; l2.set_bbox(box_at(2)); l2.index = N::Index::make_leaf(2, 1); // root's right leaf
    N l0; l0.set_bbox(box_at(0)); l0.index = N::Index::make_leaf(0, 1);
    N l1; l1.set_bbox(box_at(1)); l1.index = N::Index::make_leaf(1, 1);
    b.nodes = { root, inner, l2, l0, l1 };
    return b;
}

// Hand-built binary tree with five leaves laid out so the wide root gets four children and the
// remaining inner node keeps two children.
BB make_five_leaf_binary() {
    BB b;
    b.prim_ids = { 0, 1, 2, 3, 4 };
    N n0; n0.set_bbox(box_at(0)); n0.index = N::Index::make_inner(1);
    N n1; n1.set_bbox(box_at(0)); n1.index = N::Index::make_inner(3); // larger area, expanded first
    N n2; n2.set_bbox(box_at(4)); n2.index = N::Index::make_inner(5);
    N l0; l0.set_bbox(box_at(0)); l0.index = N::Index::make_leaf(0, 1);
    N l1; l1.set_bbox(box_at(1)); l1.index = N::Index::make_leaf(1, 1);
    N n5; n5.set_bbox(box_at(2)); n5.index = N::Index::make_inner(7);
    N l4; l4.set_bbox(box_at(4)); l4.index = N::Index::make_leaf(4, 1);
    N l2; l2.set_bbox(box_at(2)); l2.index = N::Index::make_leaf(2, 1);
    N l3; l3.set_bbox(box_at(3)); l3.index = N::Index::make_leaf(3, 1);
    b.nodes = { n0, n1, n2, l0, l1, n5, l4, l2, l3 };
    return b;
}

std::vector<Tr> make_triangles(size_t count) {
    std::vector<Tr> tris;
    for (size_t i = 0; i < count; ++i) {
        float x = static_cast<float>(i) * 3.0f;
        // Asymmetric triangles: different heights and slants.
        tris.emplace_back(
            V(x,        0.0f, 2.0f),
            V(x + 1.0f, 0.0f, 2.0f),
            V(x + 0.3f, 0.5f + 0.1f * static_cast<float>(i % 5), 2.0f));
    }
    return tris;
}

// Overlapping triangles stacked at increasing depths, so one ray hits several of them.
std::vector<Tr> make_depth_triangles(size_t count) {
    std::vector<Tr> tris;
    for (size_t i = 0; i < count; ++i) {
        float z = 2.0f + 2.0f * static_cast<float>(i);
        tris.emplace_back(
            V(-1.0f, -1.0f, z),
            V( 1.0f, -1.0f, z),
            V( 0.0f,  1.0f, z));
    }
    return tris;
}

// A binary tree consisting of a single leaf containing all primitives.
BB make_single_leaf_binary(const std::vector<Tr>& tris) {
    BB b;
    b.prim_ids.resize(tris.size());
    for (size_t i = 0; i < tris.size(); ++i)
        b.prim_ids[i] = i;
    N root;
    auto box = Box::make_empty();
    for (const auto& t : tris)
        box.extend(t.get_bbox());
    root.set_bbox(box);
    root.index = N::Index::make_leaf(0, tris.size());
    b.nodes.push_back(root);
    return b;
}

} // namespace

// ================================================================================================
// collapse group
// ================================================================================================

static int collapse_empty() {
    BB empty;
    WB wb = collapse_to_wide(empty);
    TASK_CHECK(wb.nodes.empty());
    TASK_CHECK(wb.prim_ids.empty());

    // An empty wide tree starts at a default index and never invokes a callback.
    GrowingStack<W::Index> stack;
    R ray(V(0), V(0, 0, 1), 0, 100);
    int leaf_calls = 0, inner_calls = 0;
    wb.intersect<false, true>(ray, W::Index{}, stack,
        [&] (size_t, size_t) { ++leaf_calls; return false; },
        [&] (const W&) { ++inner_calls; });
    TASK_CHECK(leaf_calls == 0);
    TASK_CHECK(inner_calls == 0);

    // The generic top-down traversal on an empty tree must also stay silent.
    GrowingStack<W::Index> stack2;
    wb.traverse_top_down<false>(W::Index{}, stack2,
        [&] (size_t, size_t) { ++leaf_calls; return false; },
        [&] (const W&) { ++inner_calls; });
    TASK_CHECK(leaf_calls == 0);
    TASK_CHECK(inner_calls == 0);
    return 0;
}

static int collapse_single_leaf() {
    auto tris = make_triangles(3);
    BB b = make_single_leaf_binary(tris); // root is a single leaf containing all primitives
    TASK_CHECK(b.nodes.size() == 1 && b.nodes[0].is_leaf());

    WB wb = collapse_to_wide(b);
    TASK_CHECK(wb.nodes.size() == 1);
    TASK_CHECK(wb.nodes[0].is_leaf());
    TASK_CHECK(wb.nodes[0].child_count() == 0);
    TASK_CHECK(wb.nodes[0].parent_id_ == W::invalid_id);
    TASK_CHECK(wb.nodes[0].index.prim_count() == 3);
    TASK_CHECK(wb.prim_ids == b.prim_ids);
    TASK_CHECK(validate_wide(wb, tris.size()));
    return 0;
}

static int collapse_three_child() {
    BB b = make_three_leaf_binary();
    WB wb = collapse_to_wide(b);
    TASK_CHECK(validate_wide(wb, 3));
    TASK_CHECK(wb.nodes.size() == 4); // one inner + three leaves
    TASK_CHECK(!wb.nodes[0].is_leaf());
    TASK_CHECK(wb.nodes[0].child_count() == 3);
    // No fake fourth slot: exactly three stored children.
    TASK_CHECK(wb.nodes[0].index.first_id() + 3 == wb.nodes.size());

    // Frontier order is preserved: leaves appear in source order L0, L1, L2.
    size_t first = wb.nodes[0].index.first_id();
    for (size_t s = 0; s < 3; ++s)
        TASK_CHECK(wb.prim_ids[wb.nodes[first + s].index.first_id()] == s);
    return 0;
}

static int collapse_four_and_two_child() {
    BB b = make_five_leaf_binary();
    WB wb = collapse_to_wide(b);
    TASK_CHECK(validate_wide(wb, 5));

    TASK_CHECK(!wb.nodes[0].is_leaf());
    TASK_CHECK(wb.nodes[0].child_count() == 4);
    size_t root_first = wb.nodes[0].index.first_id();

    // Root children in frontier order: leaf L0, leaf L1, wide inner (L2,L3), leaf L4.
    TASK_CHECK(wb.nodes[root_first + 0].is_leaf());
    TASK_CHECK(wb.nodes[root_first + 1].is_leaf());
    TASK_CHECK(!wb.nodes[root_first + 2].is_leaf());
    TASK_CHECK(wb.nodes[root_first + 2].child_count() == 2);
    TASK_CHECK(wb.nodes[root_first + 3].is_leaf());

    const auto& deep = wb.nodes[root_first + 2];
    TASK_CHECK(wb.prim_ids[wb.nodes[root_first + 0].index.first_id()] == 0);
    TASK_CHECK(wb.prim_ids[wb.nodes[root_first + 1].index.first_id()] == 1);
    TASK_CHECK(wb.prim_ids[wb.nodes[root_first + 3].index.first_id()] == 4);
    size_t deep_first = deep.index.first_id();
    TASK_CHECK(wb.prim_ids[wb.nodes[deep_first + 0].index.first_id()] == 2);
    TASK_CHECK(wb.prim_ids[wb.nodes[deep_first + 1].index.first_id()] == 3);
    return 0;
}

static int collapse_preserves_source() {
    auto tris = make_triangles(12);
    BB b = build_binary(tris, 1);
    auto snapshot_nodes = b.nodes;
    auto snapshot_ids = b.prim_ids;

    WB wb = collapse_to_wide(b);
    TASK_CHECK(validate_wide(wb, tris.size()));

    // Source tree is untouched and the wide prim_ids are the same permutation.
    TASK_CHECK(b.nodes == snapshot_nodes);
    TASK_CHECK(b.prim_ids == snapshot_ids);
    TASK_CHECK(wb.prim_ids == b.prim_ids);

    // Every wide inner node stores between two and four real children.
    for (const auto& nd : wb.nodes) {
        if (!nd.is_leaf())
            TASK_CHECK(nd.child_count() >= 2 && nd.child_count() <= 4);
    }
    return 0;
}

static int collapse_multi_primitive_leaves() {
    auto tris = make_triangles(8);
    BB b = build_binary(tris, 3); // leaves contain several primitives
    WB wb = collapse_to_wide(b);
    TASK_CHECK(validate_wide(wb, tris.size()));

    // Leaf primitive ranges are preserved exactly from the source leaves.
    std::vector<N::Index> src_leaves;
    for (const auto& nd : b.nodes)
        if (nd.is_leaf()) src_leaves.push_back(nd.index);
    std::vector<W::Index> wide_leaves;
    for (const auto& nd : wb.nodes)
        if (nd.is_leaf()) wide_leaves.push_back(nd.index);
    TASK_CHECK(src_leaves.size() == wide_leaves.size());
    for (size_t i = 0; i < src_leaves.size(); ++i) {
        TASK_CHECK(src_leaves[i].first_id() == wide_leaves[i].first_id());
        TASK_CHECK(src_leaves[i].prim_count() == wide_leaves[i].prim_count());
    }
    return 0;
}

// ================================================================================================
// traversal group
// ================================================================================================

static int traversal_matches_brute_force() {
    auto tris = make_triangles(16);
    BB b = build_binary(tris, 1);
    WB wb = collapse_to_wide(b);

    // Rays along several directions and sign combinations, with finite tmax (shadow-line style).
    const R rays[] = {
        R(V(0.2f, 0.2f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f),
        R(V(20.0f, 0.2f, 0.0f), V(-1.0f, 0.0f, 0.0f), 0.0f, 100.0f),
        R(V(0.0f, 5.0f, 2.0f), V(1.0f, -0.3f, 0.0f), 0.0f, 100.0f),
        R(V(45.0f, 0.0f, 5.0f), V(-1.0f, 0.0f, -1.0f), 0.0f, 100.0f),
        R(V(2.0f, 2.0f, 0.0f), V(0.1f, 0.1f, 1.0f), 0.0f, 5.0f), // finite shadow line
        R(V(-5.0f, -5.0f, -5.0f), V(1.0f, 1.0f, 1.0f), 0.0f, 50.0f),
    };

    for (const auto& ray : rays) {
        auto expected = brute_force(tris, ray);
        for (int robust = 0; robust < 2; ++robust) {
            size_t id; float t;
            bool got = wide_closest(wb, tris, ray, id, t, robust == 1);
            if (expected.empty()) {
                TASK_CHECK(!got);
            } else {
                TASK_CHECK(got);
                TASK_CHECK(id == expected.front().second);
                TASK_CHECK(std::abs(t - expected.front().first) <= 1e-4f);
            }
            bool any = wide_any(wb, tris, ray, robust == 1);
            TASK_CHECK(any == !expected.empty());
        }
    }
    return 0;
}

static int traversal_any_hit_stops_early() {
    auto tris = make_triangles(10);
    BB b = build_binary(tris, 1);
    WB wb = collapse_to_wide(b);

    R ray(V(0.2f, 0.2f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f);
    // The triangles live at z == 2, so this ray misses: any-hit must report no hit.
    R miss(V(0.0f, 50.0f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f);
    TASK_CHECK(wide_any(wb, tris, ray, true));
    TASK_CHECK(wide_any(wb, tris, ray, false));
    TASK_CHECK(!wide_any(wb, tris, miss, true));
    TASK_CHECK(!wide_any(wb, tris, miss, false));

    // A shadow ray whose finite tmax stops before the geometry must not report a hit.
    R short_ray(V(0.2f, 0.2f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 1.0f);
    TASK_CHECK(!wide_any(wb, tris, short_ray, true));
    TASK_CHECK(!wide_any(wb, tris, short_ray, false));
    return 0;
}

static int traversal_leaf_callback_shortens_tmax() {
    auto tris = make_triangles(10);
    BB b = build_binary(tris, 1);
    WB wb = collapse_to_wide(b);

    R ray(V(0.2f, 0.2f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f);
    auto full = brute_force(tris, ray);
    TASK_CHECK(!full.empty());

    // Clamp tmax inside the closest callback to a value before the real hit: it must win.
    GrowingStack<W::Index> stack;
    R clamped = ray;
    size_t calls = 0;
    wb.intersect<false, true>(clamped, wb.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            ++calls;
            for (size_t i = begin; i < end; ++i) {
                if (auto h = PTr(tris[wb.prim_ids[i]]).intersect(clamped))
                    clamped.tmax = std::get<0>(*h);
            }
            // Artificially shrink the ray so later candidates are pruned.
            clamped.tmax = std::min(clamped.tmax, 1.5f);
            return true;
        });
    TASK_CHECK(calls >= 1);
    TASK_CHECK(clamped.tmax <= 1.5f + 1e-6f);
    return 0;
}

static int traversal_ordering_and_slots() {
    // Triangles stacked at increasing depths along one ray, so several leaves are actually hit.
    auto tris = make_depth_triangles(8);
    BB b = build_binary(tris, 1);
    WB wb = collapse_to_wide(b);
    TASK_CHECK(validate_wide(wb, tris.size()));

    // Record the order in which leaf slots are visited in closest-hit mode. The callback does not
    // shorten tmax here, so every leaf whose box the ray enters is visited; children must be
    // processed by ascending ray-box entry distance (nearest depth first).
    R ray(V(0.0f, 0.0f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f);
    GrowingStack<W::Index> stack;
    std::vector<float> entry_order;
    wb.intersect<false, true>(ray, wb.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                R probe = ray;
                if (auto h = PTr(tris[wb.prim_ids[i]]).intersect(probe))
                    entry_order.push_back(std::get<0>(*h));
            }
            return false;
        });
    // Every triangle overlaps the ray, so every leaf is reached exactly once.
    TASK_CHECK(entry_order.size() == tris.size());
    // Visits are in ascending entry (hit) distance.
    for (size_t i = 1; i < entry_order.size(); ++i)
        TASK_CHECK(entry_order[i - 1] <= entry_order[i] + 1e-5f);

    // traverse_top_down still works and reaches every leaf exactly once.
    GrowingStack<W::Index> stack2;
    size_t visited_prims = 0;
    size_t inner_calls = 0;
    wb.traverse_top_down<false>(wb.get_root().index, stack2,
        [&] (size_t begin, size_t end) { visited_prims += end - begin; return false; },
        [&] (const W&) { ++inner_calls; });
    TASK_CHECK(visited_prims == tris.size());
    TASK_CHECK(inner_calls > 0);
    return 0;
}

// ================================================================================================
// maintenance group (refit, deformation, subtree extraction)
// ================================================================================================

static int maintenance_refit_after_deformation() {
    auto tris = make_triangles(12);
    BB b = build_binary(tris, 1);
    WB wb = collapse_to_wide(b);

    // Deform every triangle by shifting it one unit along +z; leaf callbacks recompute the boxes.
    for (auto& t : tris) {
        t.p0[2] += 1.0f; t.p1[2] += 1.0f; t.p2[2] += 1.0f;
    }
    wb.refit([&] (W& nd) {
        auto box = Box::make_empty();
        for (size_t i = nd.index.first_id();
             i < nd.index.first_id() + nd.index.prim_count(); ++i)
            box.extend(tris[wb.prim_ids[i]].get_bbox());
        nd.set_bbox(box);
    });

    // Inner boxes must equal the reduction of all their real children.
    for (const auto& nd : wb.nodes) {
        if (nd.is_leaf()) continue;
        auto expected = Box::make_empty();
        size_t first = nd.index.first_id();
        for (size_t s = 0; s < nd.child_count(); ++s)
            expected.extend(wb.nodes[first + s].get_bbox());
        auto got = nd.get_bbox();
        for (size_t k = 0; k < 3; ++k) {
            TASK_CHECK(std::abs(got.min[k] - expected.min[k]) <= 1e-5f);
            TASK_CHECK(std::abs(got.max[k] - expected.max[k]) <= 1e-5f);
        }
    }

    // The hit distance moved from z == 2 to z == 3.
    R ray(V(0.2f, 0.2f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f);
    auto expected = brute_force(tris, ray);
    size_t id; float t;
    TASK_CHECK(wide_closest(wb, tris, ray, id, t, true));
    TASK_CHECK(id == expected.front().second);
    TASK_CHECK(std::abs(t - 3.0f) <= 1e-4f);
    return 0;
}

static int maintenance_extract_subtree() {
    auto tris = make_triangles(12);
    BB b = build_binary(tris, 1);
    WB wb = collapse_to_wide(b);

    // Extract a non-root inner subtree (the first child block of the root).
    size_t root_first = wb.nodes[0].index.first_id();
    size_t sub_root = root_first;
    // Pick an inner child if the first slot is a leaf.
    for (size_t s = 0; s < wb.nodes[0].child_count(); ++s) {
        if (!wb.nodes[root_first + s].is_leaf()) {
            sub_root = root_first + s;
            break;
        }
    }
    WB sub = wb.extract_bvh(sub_root);
    TASK_CHECK(validate_wide(sub, tris.size(), false));
    TASK_CHECK(sub.nodes[0].parent_id_ == W::invalid_id);
    TASK_CHECK(!sub.prim_ids.empty());
    TASK_CHECK(sub.prim_ids.size() < tris.size());

    // Extracted prim ids still point at original triangles and are a subset.
    for (size_t id : sub.prim_ids)
        TASK_CHECK(id < tris.size());

    // The extracted tree is independently traversable.
    R ray(V(0.2f, 0.2f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f);
    size_t id; float t;
    bool got = wide_closest(sub, tris, ray, id, t, true);
    auto full = brute_force(tris, ray);
    if (got) {
        // Any hit reported by the subtree must be a real hit of the whole scene.
        bool found = false;
        for (auto& h : full) if (h.second == id) found = true;
        TASK_CHECK(found);
    }
    return 0;
}

static int maintenance_deform_then_extract() {
    auto tris = make_triangles(10);
    BB b = build_binary(tris, 1);
    WB wb = collapse_to_wide(b);

    for (auto& t : tris) { t.p0[1] += 2.0f; t.p1[1] += 2.0f; t.p2[1] += 2.0f; }
    wb.refit([&] (W& nd) {
        auto box = Box::make_empty();
        for (size_t i = nd.index.first_id();
             i < nd.index.first_id() + nd.index.prim_count(); ++i)
            box.extend(tris[wb.prim_ids[i]].get_bbox());
        nd.set_bbox(box);
    });

    size_t root_first = wb.nodes[0].index.first_id();
    size_t sub_root = root_first;
    for (size_t s = 0; s < wb.nodes[0].child_count(); ++s) {
        if (!wb.nodes[root_first + s].is_leaf()) { sub_root = root_first + s; break; }
    }
    WB sub = wb.extract_bvh(sub_root);
    TASK_CHECK(validate_wide(sub, tris.size(), false));

    // Refit the extracted subtree on its own, then query it.
    sub.refit([&] (W& nd) {
        auto box = Box::make_empty();
        for (size_t i = nd.index.first_id();
             i < nd.index.first_id() + nd.index.prim_count(); ++i)
            box.extend(tris[sub.prim_ids[i]].get_bbox());
        nd.set_bbox(box);
    });
    // Aim a well-conditioned ray (all direction components nonzero, so the fast path cannot
    // overflow its inverse-origin term) at a triangle in the extracted subtree.
    const auto& target_tri = tris[sub.prim_ids.front()];
    auto center = target_tri.get_center();
    V dir = normalize(V(0.3f, 0.5f, 1.0f));
    R ray(center - dir * 2.0f, dir, 0.0f, 100.0f);
    GrowingStack<W::Index> stack;
    bool any = false;
    sub.intersect<true, false>(ray, sub.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                if (PTr(tris[sub.prim_ids[i]]).intersect(ray)) { any = true; return true; }
            return false;
        });
    TASK_CHECK(any);
    return 0;
}

// ================================================================================================
// persistence group (serialize / deserialize, legacy binary layout)
// ================================================================================================

static int persistence_roundtrip() {
    auto tris = make_triangles(14);
    BB b = build_binary(tris, 2);
    WB wb = collapse_to_wide(b);

    std::ostringstream os(std::ios::binary);
    StdOutputStream out(os);
    wb.serialize(out);
    TASK_CHECK(!os.str().empty());

    std::istringstream is(os.str(), std::ios::binary);
    StdInputStream in(is);
    WB loaded = WB::deserialize(in);

    // Equality covers bounds, index, the stored child count and the parent back pointer.
    TASK_CHECK(loaded == wb);
    TASK_CHECK(loaded.prim_ids == wb.prim_ids);

    // Child counts survive the round trip.
    TASK_CHECK(loaded.nodes.size() == wb.nodes.size());
    for (size_t i = 0; i < wb.nodes.size(); ++i)
        TASK_CHECK(loaded.nodes[i].child_count() == wb.nodes[i].child_count());

    // The loaded tree works without any reference to the original binary tree.
    R ray(V(0.2f, 0.2f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f);
    auto expected = brute_force(tris, ray);
    size_t id; float t;
    TASK_CHECK(wide_closest(loaded, tris, ray, id, t, true));
    TASK_CHECK(id == expected.front().second);
    return 0;
}

static int persistence_refit_and_extract_after_load() {
    auto tris = make_triangles(12);
    BB b = build_binary(tris, 1);
    WB wb = collapse_to_wide(b);

    std::ostringstream os(std::ios::binary);
    StdOutputStream out(os);
    wb.serialize(out);
    std::istringstream is(os.str(), std::ios::binary);
    StdInputStream in(is);
    WB loaded = WB::deserialize(in);

    // Deform, refit, then extract, all starting from the deserialized wide tree only.
    for (auto& t : tris) { t.p0[2] += 4.0f; t.p1[2] += 4.0f; t.p2[2] += 4.0f; }
    loaded.refit([&] (W& nd) {
        auto box = Box::make_empty();
        for (size_t i = nd.index.first_id();
             i < nd.index.first_id() + nd.index.prim_count(); ++i)
            box.extend(tris[loaded.prim_ids[i]].get_bbox());
        nd.set_bbox(box);
    });

    size_t root_first = loaded.nodes[0].index.first_id();
    size_t sub_root = root_first;
    for (size_t s = 0; s < loaded.nodes[0].child_count(); ++s) {
        if (!loaded.nodes[root_first + s].is_leaf()) { sub_root = root_first + s; break; }
    }
    WB sub = loaded.extract_bvh(sub_root);
    TASK_CHECK(validate_wide(sub, tris.size(), false));
    TASK_CHECK(sub.nodes[0].parent_id_ == W::invalid_id);

    R ray(V(0.2f, 0.2f, 0.0f), V(0.0f, 0.0f, 1.0f), 0.0f, 100.0f);
    size_t id; float tt;
    TASK_CHECK(wide_closest(loaded, tris, ray, id, tt, false));
    TASK_CHECK(std::abs(tt - 6.0f) <= 1e-4f);
    return 0;
}

static int persistence_legacy_binary_layout() {
    // The reference 16-triangle sample produced by the original library.
    std::ifstream f("task_support/legacy-binary.bvh", std::ios::binary);
    TASK_CHECK(static_cast<bool>(f));
    StdInputStream in(f);
    BB legacy = BB::deserialize(in);
    TASK_CHECK(legacy.nodes.size() == 31);
    TASK_CHECK(legacy.prim_ids.size() == 16);

    // Re-serializing the binary tree must reproduce the exact on-disk byte layout.
    std::ostringstream os(std::ios::binary);
    StdOutputStream out(os);
    legacy.serialize(out);
    std::ifstream raw("task_support/legacy-binary.bvh", std::ios::binary);
    std::stringstream buffer;
    buffer << raw.rdbuf();
    TASK_CHECK(os.str() == buffer.str());

    // The wide tree built from that legacy binary is valid and queryable on its own.
    WB wb = collapse_to_wide(legacy);
    TASK_CHECK(validate_wide(wb, 16));

    std::vector<Tr> tris;
    for (int i = 0; i < 16; ++i) {
        float x = static_cast<float>((i * 7) % 16) * 3.0f;
        tris.emplace_back(V(x, 0, 2), V(x + 1, 0, 2), V(x, 1, 2));
    }
    R ray(V(0.2f, 0.2f, 0), V(0, 0, 1), 0, 100);
    size_t id; float t;
    TASK_CHECK(wide_closest(wb, tris, ray, id, t, true));
    TASK_CHECK(id == 0);
    TASK_CHECK(std::abs(t - 2.0f) <= 1e-5f);
    return 0;
}

static int persistence_empty_and_single_roundtrip() {
    {
        WB empty;
        std::ostringstream os(std::ios::binary);
        StdOutputStream out(os);
        empty.serialize(out);
        std::istringstream is(os.str(), std::ios::binary);
        StdInputStream in(is);
        WB loaded = WB::deserialize(in);
        TASK_CHECK(loaded.nodes.empty());
        TASK_CHECK(loaded.prim_ids.empty());
    }
    {
        auto tris = make_triangles(3);
        WB single = collapse_to_wide(make_single_leaf_binary(tris));
        std::ostringstream os(std::ios::binary);
        StdOutputStream out(os);
        single.serialize(out);
        std::istringstream is(os.str(), std::ios::binary);
        StdInputStream in(is);
        WB loaded = WB::deserialize(in);
        TASK_CHECK(loaded == single);
        TASK_CHECK(loaded.nodes[0].is_leaf());
        TASK_CHECK(loaded.nodes[0].child_count() == 0);
    }
    return 0;
}

// ================================================================================================
// Registration (no main: task_support/driver.c provides it).
// ================================================================================================

const struct task_case task_cases[] = {
    { "collapse",    "empty",                 collapse_empty },
    { "collapse",    "single_leaf",           collapse_single_leaf },
    { "collapse",    "three_child",           collapse_three_child },
    { "collapse",    "four_and_two_child",    collapse_four_and_two_child },
    { "collapse",    "preserves_source",      collapse_preserves_source },
    { "collapse",    "multi_primitive_leaves", collapse_multi_primitive_leaves },

    { "traversal",   "matches_brute_force",   traversal_matches_brute_force },
    { "traversal",   "any_hit_stops_early",   traversal_any_hit_stops_early },
    { "traversal",   "callback_shortens_tmax", traversal_leaf_callback_shortens_tmax },
    { "traversal",   "ordering_and_slots",    traversal_ordering_and_slots },

    { "maintenance", "refit_after_deformation", maintenance_refit_after_deformation },
    { "maintenance", "extract_subtree",       maintenance_extract_subtree },
    { "maintenance", "deform_then_extract",   maintenance_deform_then_extract },

    { "persistence", "roundtrip",             persistence_roundtrip },
    { "persistence", "refit_extract_after_load", persistence_refit_and_extract_after_load },
    { "persistence", "legacy_binary_layout",  persistence_legacy_binary_layout },
    { "persistence", "empty_single_roundtrip", persistence_empty_and_single_roundtrip },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
