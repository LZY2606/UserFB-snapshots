// Regression driver for bvh/v2 spatial reinsertion.
//
// Contract under test:
//   * BinnedSahBuilder builds a tree from a deterministic asymmetric mesh.
//   * Moving a subset of primitives and refitting must keep every node bbox
//     containing the triangles it really owns.
//   * ReinsertionOptimizer must actually change inner-node connections.
//   * Extracted subtrees must carry the permanent primitive ids reachable from
//     the source node, for every leaf interval (including non-zero first ids).
//   * Rays must still hit the original primitive/material/UV, compared against
//     an independent long-double brute-force reference.
//   * A tiny PPM must agree pixel-by-pixel with the brute-force reference.
//
// The reference intersection below deliberately does not use PrecomputedTri,
// Node::intersect, or Bvh traversal to produce expected values.

#include <bvh/v2/bbox.h>
#include <bvh/v2/binned_sah_builder.h>
#include <bvh/v2/bvh.h>
#include <bvh/v2/index.h>
#include <bvh/v2/node.h>
#include <bvh/v2/ray.h>
#include <bvh/v2/reinsertion_optimizer.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/stream.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/vec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using bvh::v2::GrowingStack;
using bvh::v2::ReinsertionOptimizer;
using bvh::v2::StdInputStream;
using bvh::v2::StdOutputStream;

using Scalar = double;
using Vec2 = bvh::v2::Vec<Scalar, 2>;
using Vec3 = bvh::v2::Vec<Scalar, 3>;
using BBox = bvh::v2::BBox<Scalar, 3>;
using Tri = bvh::v2::Tri<Scalar, 3>;
using Node = bvh::v2::Node<Scalar, 3>;
using Bvh = bvh::v2::Bvh<Node>;
using Ray = bvh::v2::Ray<Scalar, 3>;
using PrecomputedTri = bvh::v2::PrecomputedTri<Scalar>;
using Builder = bvh::v2::BinnedSahBuilder<Node>;

static constexpr size_t kClusterCount = 4;
static constexpr size_t kPerCluster = 16;
static constexpr size_t kPrimCount = kClusterCount * kPerCluster;
static constexpr size_t kNoPrim = std::numeric_limits<size_t>::max();
static constexpr size_t kNoNode = std::numeric_limits<size_t>::max();

// ---------------------------------------------------------------------------
// Failure reporting.  A defective source copy exits with status 1 and a stable
// "DETECTED <tag>" line.  The first difference and expected/actual values are
// printed immediately above it.
// ---------------------------------------------------------------------------

[[noreturn]] static void fail(const std::string& tag, const std::string& detail) {
    std::cout << "first difference: " << detail << "\n";
    std::cout << "DETECTED " << tag << std::endl;
    std::exit(1);
}

static std::string fmt_scalar(Scalar v) {
    std::ostringstream out;
    out << std::setprecision(17) << v;
    return out.str();
}

static std::string fmt_bbox(const BBox& b) {
    std::ostringstream out;
    out << std::setprecision(17)
        << "min=(" << b.min[0] << "," << b.min[1] << "," << b.min[2] << ") "
        << "max=(" << b.max[0] << "," << b.max[1] << "," << b.max[2] << ")";
    return out.str();
}

// ---------------------------------------------------------------------------
// Deterministic mesh: four well-separated spatial clusters.  Each cluster is a
// 4x4 grid of flat, asymmetric triangles; some are thin slivers, and the four
// "shadow" primitives repeat the geometry of an earlier slot at a greater
// depth, producing genuine occlusion at different depths.
// ---------------------------------------------------------------------------

struct Face {
    Tri tri;
    std::array<Vec2, 3> uv;
    int material;
};

static Vec3 translate(const Vec3& v, Scalar dx, Scalar dy, Scalar dz) {
    return Vec3(v[0] + dx, v[1] + dy, v[2] + dz);
}

static std::vector<Face> make_mesh() {
    std::vector<Face> faces;
    faces.reserve(kPrimCount);
    for (size_t c = 0; c < kClusterCount; ++c) {
        const Scalar cluster_x = -48.0 + static_cast<Scalar>(c) * 32.0;
        for (size_t j = 0; j < kPerCluster; ++j) {
            const size_t slot = j < 12 ? j : (j - 12);
            const size_t gx = slot % 4;
            const size_t gy = slot / 4;
            const Scalar slot_x = cluster_x + (static_cast<Scalar>(gx) - 1.5) * 6.0;
            const Scalar slot_y = -9.0 + static_cast<Scalar>(gy) * 6.0;
            const Scalar base_z = 2.0 + static_cast<Scalar>((slot * 3 + c * 5) % 7) * 1.6;
            const Scalar z = j < 12
                ? base_z
                : base_z + 4.5; // the same XY footprint, higher depth

            const Scalar w = 1.70 + 0.22 * static_cast<Scalar>((j * 3 + c) % 5);
            const bool sliver = (j % 5 == 1) || (j % 7 == 3);
            Scalar h1 = 1.00 + 0.18 * static_cast<Scalar>((j * 7 + c * 2) % 5);
            Scalar h2 = -(0.80 + 0.16 * static_cast<Scalar>((j * 5 + c * 3) % 4));
            if (sliver) {
                h1 = 0.10 + 0.02 * static_cast<Scalar>(j % 3);
                h2 = -(0.08 + 0.02 * static_cast<Scalar>(j % 4));
            }
            const Scalar lean = 0.25 + 0.07 * static_cast<Scalar>(j % 4);

            Vec3 p0(slot_x, slot_y, z);
            Vec3 p1(slot_x + w, slot_y + h1, z);
            Vec3 p2(slot_x + lean, slot_y + h2, z);

            const size_t i = c * kPerCluster + j;
            Face face;
            face.tri = Tri(p0, p1, p2);
            face.uv[0] = Vec2(0.05 + 0.02 * static_cast<Scalar>(i % 5),
                              0.10 + 0.01 * static_cast<Scalar>(i % 3));
            face.uv[1] = Vec2(0.92 - 0.01 * static_cast<Scalar>(i % 7),
                              0.08 + 0.02 * static_cast<Scalar>(i % 4));
            face.uv[2] = Vec2(0.12 + 0.01 * static_cast<Scalar>(i % 6),
                              0.88 - 0.02 * static_cast<Scalar>(i % 5));
            face.material = static_cast<int>(c);
            faces.push_back(face);
        }
    }
    return faces;
}

static void move_batch_one(std::vector<Face>& faces) {
    for (size_t i = 0; i < faces.size(); ++i) {
        if (i % 4 != 1)
            continue;
        const Scalar dx = ((i / 4) % 2 == 0) ? 26.0 : -26.0;
        const Scalar dy = -12.0 + static_cast<Scalar>(i % 5) * 4.0;
        const Scalar dz = 1.5 + static_cast<Scalar>(i % 3) * 1.0;
        auto& t = faces[i].tri;
        t.p0 = translate(t.p0, dx, dy, dz);
        t.p1 = translate(t.p1, dx, dy, dz);
        t.p2 = translate(t.p2, dx, dy, dz);
    }
}

static void move_batch_two(std::vector<Face>& faces) {
    for (size_t i = 0; i < faces.size(); ++i) {
        if (i % 5 != 2)
            continue;
        const Scalar dx = 10.0 + static_cast<Scalar>(i % 3) * 2.0;
        const Scalar dy = 16.0 - static_cast<Scalar>(i % 4) * 3.0;
        const Scalar dz = 0.8 - static_cast<Scalar>(i % 2) * 1.6;
        auto& t = faces[i].tri;
        t.p0 = translate(t.p0, dx, dy, dz);
        t.p1 = translate(t.p1, dx, dy, dz);
        t.p2 = translate(t.p2, dx, dy, dz);
    }
}

static BBox face_bbox(const Face& face) { return face.tri.get_bbox(); }

static void refit_with_faces(Bvh& bvh, const std::vector<Face>& faces) {
    bvh.refit([&](Node& leaf) {
        BBox box = BBox::make_empty();
        const size_t first = leaf.index.first_id();
        const size_t count = leaf.index.prim_count();
        for (size_t i = first; i < first + count; ++i)
            box.extend(face_bbox(faces[bvh.prim_ids[i]]));
        leaf.set_bbox(box);
    });
}

static std::vector<BBox> make_boxes(const std::vector<Face>& faces) {
    std::vector<BBox> boxes(faces.size());
    for (size_t i = 0; i < faces.size(); ++i)
        boxes[i] = face_bbox(faces[i]);
    return boxes;
}

static std::vector<Vec3> make_centers(const std::vector<Face>& faces) {
    std::vector<Vec3> centers(faces.size());
    for (size_t i = 0; i < faces.size(); ++i)
        centers[i] = faces[i].tri.get_center();
    return centers;
}

// ---------------------------------------------------------------------------
// Independent reference intersector.  Everything below uses its own long
// double vector type, its own plane/barycentric solve, and brute-force scans.
// It never calls PrecomputedTri, Node intersection, or Bvh traversal.
// ---------------------------------------------------------------------------

struct L3 {
    long double x, y, z;
};

static L3 to_l3(const Vec3& v) { return L3{ v[0], v[1], v[2] }; }
static L3 operator-(const L3& a, const L3& b) { return L3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
static long double dot(const L3& a, const L3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static L3 cross(const L3& a, const L3& b) {
    return L3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
static long double norm(const L3& a) { return std::sqrt(dot(a, a)); }

// Solves a ray/triangle intersection in long double, returning the ray
// parameter and barycentric weights matching PrecomputedTri's convention:
//   hit = (1 - u - v) * p0 + u * p1 + v * p2.
static bool ray_triangle_ref(const Tri& tri, const Ray& ray,
                             long double& t, long double& u, long double& v) {
    const L3 p0 = to_l3(tri.p0);
    const L3 p1 = to_l3(tri.p1);
    const L3 p2 = to_l3(tri.p2);
    const L3 e1 = p1 - p0;
    const L3 e2 = p2 - p0;
    const L3 dir = to_l3(ray.dir);
    const L3 s = to_l3(ray.org) - p0;

    const L3 h = cross(dir, e2);
    const long double a = dot(e1, h);
    const long double scale = norm(e1) * norm(e2) * norm(dir);
    if (std::fabsl(a) <= 1e-18L * scale)
        return false;

    const long double f = 1.0L / a;
    u = f * dot(s, h);
    const L3 q = cross(s, e1);
    v = f * dot(dir, q);
    t = f * dot(e2, q);
    return true;
}

struct RefScan {
    bool hit = false;
    size_t prim = kNoPrim;
    long double t = 0.0L;
    long double u = 0.0L;
    long double v = 0.0L;
    bool ambiguous = false;
};

static long double scene_extent(const std::vector<Face>& faces) {
    BBox box = BBox::make_empty();
    for (const auto& face : faces)
        box.extend(face_bbox(face));
    const auto diag = box.get_diagonal();
    return std::max<long double>(1.0L, norm(L3{ diag[0], diag[1], diag[2] }));
}

// Brute-force scan of the given permanent primitive ids using the reference
// intersector.  `edge_margin` and `tie_tol` are only used to mark ambiguous
// configurations (shared edges, tangency, equal distances); the actual hit
// decision uses exact barycentrics and the ray's finite interval.
static RefScan ref_scan(const Ray& ray, const std::vector<Face>& faces,
                        const std::vector<size_t>& ids,
                        long double edge_margin, long double tie_tol) {
    RefScan out;
    std::vector<long double> hit_ts;

    const long double tmin = static_cast<long double>(ray.tmin);
    const long double tmax = static_cast<long double>(ray.tmax);

    for (size_t id : ids) {
        long double t = 0.0L, u = 0.0L, v = 0.0L;
        if (!ray_triangle_ref(faces[id].tri, ray, t, u, v))
            continue;
        const long double w = 1.0L - u - v;
        const long double bary_margin = std::min({ u, v, w });

        if (bary_margin > -1e-5L) {
            if (t < tmin) {
                if (tmin - t <= tie_tol)
                    out.ambiguous = true;
            } else if (t > tmax) {
                if (t - tmax <= tie_tol)
                    out.ambiguous = true;
            } else if (bary_margin < edge_margin) {
                out.ambiguous = true;
            }
        }

        if (bary_margin >= 0.0L && t >= tmin && t <= tmax) {
            hit_ts.push_back(t);
            if (!out.hit || t < out.t) {
                out.hit = true;
                out.prim = id;
                out.t = t;
                out.u = u;
                out.v = v;
            }
        }
    }

    if (out.hit) {
        size_t tied = 0;
        for (long double t : hit_ts) {
            if (std::fabsl(t - out.t) <= tie_tol)
                ++tied;
        }
        if (tied > 1)
            out.ambiguous = true;
        if (std::fabsl(out.t - tmin) <= tie_tol ||
            std::fabsl(out.t - tmax) <= tie_tol)
            out.ambiguous = true;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Structural and geometric validation.
// ---------------------------------------------------------------------------

static void collect_node_prim_ids(const Bvh& tree, size_t root_id,
                                  std::vector<size_t>& out) {
    std::stack<size_t> stack;
    stack.push(root_id);
    while (!stack.empty()) {
        const size_t id = stack.top();
        stack.pop();
        const Node& node = tree.nodes[id];
        if (node.is_leaf()) {
            const size_t first = node.index.first_id();
            const size_t count = node.index.prim_count();
            for (size_t i = first; i < first + count; ++i)
                out.push_back(tree.prim_ids[i]);
        } else {
            stack.push(node.index.first_id() + 1);
            stack.push(node.index.first_id());
        }
    }
}

static void collect_leaf_ranges(const Bvh& tree, size_t root_id,
                                std::vector<std::pair<size_t, size_t>>& out) {
    std::stack<size_t> stack;
    stack.push(root_id);
    while (!stack.empty()) {
        const size_t id = stack.top();
        stack.pop();
        const Node& node = tree.nodes[id];
        if (node.is_leaf()) {
            out.emplace_back(node.index.first_id(), node.index.prim_count());
        } else {
            stack.push(node.index.first_id() + 1);
            stack.push(node.index.first_id());
        }
    }
}

// Checks acyclicity, single parentage, two valid children per inner node,
// exact coverage of the expected primitive multiset by leaf intervals, and
// containment of every node's true descendant geometry in its bbox.
static void validate_tree(const std::string& tag, const Bvh& tree,
                          const std::vector<size_t>& expected_ids,
                          const std::vector<Face>& faces, long double eps) {
    const size_t n = tree.nodes.size();
    if (n == 0)
        fail(tag, "empty node array; expected>=1 actual=0");

    std::vector<size_t> parent(n, kNoNode);
    std::vector<unsigned char> seen(n, 0);
    std::vector<size_t> preorder;
    preorder.reserve(n);
    std::stack<std::pair<size_t, size_t>> stack;
    stack.emplace(0, kNoNode);
    size_t visited = 0;

    while (!stack.empty()) {
        const auto [id, pid] = stack.top();
        stack.pop();
        if (id >= n) {
            fail(tag, "child id out of range; expected < " + std::to_string(n) +
                      " actual=" + std::to_string(id));
        }
        if (seen[id] != 0) {
            fail(tag, "cycle or multiple parents at node " + std::to_string(id) +
                      "; expected exactly one parent actual parent=" +
                      std::to_string(pid) + " previous=" +
                      std::to_string(parent[id]));
        }
        seen[id] = 1;
        parent[id] = pid;
        preorder.push_back(id);
        ++visited;

        const Node& node = tree.nodes[id];
        if (node.is_leaf()) {
            const size_t first = node.index.first_id();
            const size_t count = node.index.prim_count();
            if (first > tree.prim_ids.size() ||
                count > tree.prim_ids.size() - first) {
                fail(tag, "leaf interval out of range at node " + std::to_string(id) +
                          "; expected first+count <= " +
                          std::to_string(tree.prim_ids.size()) +
                          " actual first=" + std::to_string(first) +
                          " count=" + std::to_string(count));
            }
        } else {
            const size_t first = node.index.first_id();
            if (first >= n || n - first < 2) {
                fail(tag, "inner node has invalid children at node " +
                          std::to_string(id) + "; expected two children < " +
                          std::to_string(n) + " actual first=" +
                          std::to_string(first));
            }
            stack.emplace(first + 1, id);
            stack.emplace(first, id);
        }
    }

    if (visited != n) {
        fail(tag, "unreachable nodes; expected " + std::to_string(n) +
                  " actual reachable=" + std::to_string(visited));
    }

    std::vector<unsigned char> cover(tree.prim_ids.size(), 0);
    std::vector<size_t> prims;
    prims.reserve(expected_ids.size());

    std::vector<std::pair<size_t, size_t>> ranges;
    for (size_t id : preorder) {
        const Node& node = tree.nodes[id];
        if (!node.is_leaf())
            continue;
        const size_t first = node.index.first_id();
        const size_t count = node.index.prim_count();
        ranges.emplace_back(first, count);
        for (size_t i = first; i < first + count; ++i) {
            if (cover[i] != 0) {
                fail(tag, "overlapping leaf intervals at prim_ids position " +
                          std::to_string(i));
            }
            cover[i] = 1;
            prims.push_back(tree.prim_ids[i]);
        }
    }
    for (size_t i = 0; i < cover.size(); ++i) {
        if (cover[i] == 0)
            fail(tag, "gap in leaf interval coverage at prim_ids position " +
                      std::to_string(i));
    }

    std::vector<size_t> actual = prims;
    std::vector<size_t> expected = expected_ids;
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    if (actual.size() != expected.size()) {
        fail(tag, "leaf primitive multiset size; expected=" +
                  std::to_string(expected.size()) + " actual=" +
                  std::to_string(actual.size()));
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            fail(tag, "leaf primitive multiset at rank " + std::to_string(i) +
                      "; expected=" + std::to_string(expected[i]) +
                      " actual=" + std::to_string(actual[i]));
        }
    }

    // Postorder (reverse preorder) accumulation of true descendant bounds.
    std::vector<BBox> descendant_bbox(n);
    for (size_t k = preorder.size(); k-- > 0;) {
        const size_t id = preorder[k];
        const Node& node = tree.nodes[id];
        BBox box = BBox::make_empty();
        if (node.is_leaf()) {
            const size_t first = node.index.first_id();
            const size_t count = node.index.prim_count();
            for (size_t i = first; i < first + count; ++i)
                box.extend(face_bbox(faces[tree.prim_ids[i]]));
        } else {
            const size_t first = node.index.first_id();
            box = descendant_bbox[first];
            box.extend(descendant_bbox[first + 1]);
        }
        descendant_bbox[id] = box;

        const BBox actual_box = node.get_bbox();
        for (size_t d = 0; d < 3; ++d) {
            if (static_cast<long double>(box.min[d]) <
                    static_cast<long double>(actual_box.min[d]) - eps ||
                static_cast<long double>(box.max[d]) >
                    static_cast<long double>(actual_box.max[d]) + eps) {
                fail(tag, "node bbox does not contain descendants at node " +
                          std::to_string(id) + " axis " + std::to_string(d) +
                          "; expected=" + fmt_bbox(box) +
                          " actual=" + fmt_bbox(actual_box));
            }
        }
    }
}

static size_t changed_inner_connections(const std::vector<Node>& before,
                                        const std::vector<Node>& after) {
    size_t changed = 0;
    const size_t n = std::min(before.size(), after.size());
    for (size_t i = 0; i < n; ++i) {
        const Node& a = before[i];
        const Node& b = after[i];
        if (a.is_leaf() != b.is_leaf()) {
            ++changed;
        } else if (!a.is_leaf() && a.index.value != b.index.value) {
            ++changed;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Actual traversal, using both data layouts.
// ---------------------------------------------------------------------------

enum class Layout { OriginalOrder, Permuted };

struct ActualHit {
    bool hit = false;
    size_t prim = kNoPrim;
    double t = 0.0;
    double u = 0.0;
    double v = 0.0;
    size_t tests = 0;
};

static std::vector<PrecomputedTri> make_precomputed(const std::vector<Face>& faces) {
    std::vector<PrecomputedTri> pre;
    pre.reserve(faces.size());
    for (const auto& face : faces)
        pre.emplace_back(face.tri);
    return pre;
}

static std::vector<PrecomputedTri> make_permuted_precomputed(
    const Bvh& bvh, const std::vector<PrecomputedTri>& original_order) {
    std::vector<PrecomputedTri> pre(bvh.prim_ids.size());
    for (size_t i = 0; i < bvh.prim_ids.size(); ++i)
        pre[i] = original_order[bvh.prim_ids[i]];
    return pre;
}

template <bool AnyHit>
static ActualHit actual_intersect(const Bvh& tree, size_t root_id,
                                  const Ray& initial, Layout layout,
                                  const std::vector<PrecomputedTri>& original_order,
                                  const std::vector<PrecomputedTri>& permuted,
                                  size_t& tests_out) {
    ActualHit out;
    Ray cur = initial;
    GrowingStack<Bvh::Index> stack;
    size_t tests = 0;

    tree.template intersect<AnyHit, true>(
        cur, tree.nodes[root_id].index, stack,
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                ++tests;
                const PrecomputedTri& tri = layout == Layout::OriginalOrder
                    ? original_order[tree.prim_ids[i]]
                    : permuted[i];
                auto hit = tri.intersect(cur);
                if (!hit)
                    continue;
                const auto [t, u, v] = *hit;
                if constexpr (AnyHit) {
                    out.hit = true;
                    out.prim = tree.prim_ids[i];
                    out.t = t;
                    out.u = u;
                    out.v = v;
                    return true;
                } else {
                    cur.tmax = t;
                    out.hit = true;
                    out.prim = tree.prim_ids[i];
                    out.t = t;
                    out.u = u;
                    out.v = v;
                }
            }
            return false;
        });
    tests_out = tests;
    return out;
}

// ---------------------------------------------------------------------------
// Ray corpus.
// ---------------------------------------------------------------------------

struct RayCase {
    Ray ray;
    std::string kind;
    size_t target_prim;
};

static double rand01(std::mt19937_64& rng) {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng);
}

static bool add_safe_ray(std::vector<RayCase>& rays, const Ray& ray,
                         const std::string& kind, size_t target_prim,
                         const std::vector<Face>& faces,
                         const std::vector<size_t>& all_ids,
                         long double edge_margin, long double tie_tol) {
    auto scan = ref_scan(ray, faces, all_ids, edge_margin, tie_tol);
    if (scan.ambiguous)
        return false;
    rays.push_back(RayCase{ ray, kind, target_prim });
    return true;
}

static std::vector<RayCase> make_ray_corpus(const std::vector<Face>& faces) {
    std::vector<size_t> all_ids(faces.size());
    for (size_t i = 0; i < all_ids.size(); ++i)
        all_ids[i] = i;

    const long double scale = scene_extent(faces);
    const long double edge_margin = 1e-5L;
    const long double tie_tol = 1e-7L * scale;

    std::vector<RayCase> rays;

    // Non-axis, non-unit rays aimed through two interior barycentric points of
    // every primitive.  These also guarantee ray coverage for every subtree.
    const std::array<std::array<double, 2>, 2> uv_points = {{
        {{ 0.24, 0.31 }},
        {{ 0.40, 0.22 }}
    }};
    for (size_t i = 0; i < faces.size(); ++i) {
        const Tri& tri = faces[i].tri;
        const Vec3 e1 = tri.p1 - tri.p0;
        const Vec3 e2 = tri.p2 - tri.p0;
        for (size_t k = 0; k < uv_points.size(); ++k) {
            const Scalar u = uv_points[k][0];
            const Scalar v = uv_points[k][1];
            const Vec3 point = tri.p0 + e1 * u + e2 * v;
            const Vec3 origin(
                point[0] + 2.3 + 0.4 * static_cast<Scalar>(k),
                point[1] - 1.7 - 0.3 * static_cast<Scalar>(i % 3),
                -8.0 - 0.5 * static_cast<Scalar>(i % 5));
            const Vec3 dir = point - origin;
            add_safe_ray(rays, Ray(origin, dir, 0.0, 200.0), "targeted", i,
                         faces, all_ids, edge_margin, tie_tol);
        }
    }

    // Explicit non-unit but non-axis direction.
    {
        const Tri& tri = faces[0].tri;
        const Vec3 point = tri.p0 + (tri.p1 - tri.p0) * 0.29 +
                            (tri.p2 - tri.p0) * 0.27;
        const Vec3 origin = point + Vec3(1.5, -2.0, -4.0);
        const Vec3 dir = point - origin;
        add_safe_ray(rays, Ray(origin, dir, 0.0, 200.0), "nonunit", 0,
                     faces, all_ids, edge_margin, tie_tol);
    }

    // Axis-parallel rays with positive and negative zero components, plus
    // finite intervals that stop before/after the target and an after-target
    // tmin that excludes both the target and its occluder.
    {
        const Tri& tri = faces[0].tri;
        const Vec3 point = tri.p0 + (tri.p1 - tri.p0) * 0.31 +
                            (tri.p2 - tri.p0) * 0.27;
        const Scalar dir_z = 2.5;
        const Scalar t_point = (point[2] + 8.0) / dir_z;

        const Vec3 origin_pos(point[0], point[1], -8.0);
        add_safe_ray(rays,
            Ray(origin_pos, Vec3(0.0, -0.0, dir_z), 0.0, 200.0),
            "axis-parallel", 0, faces, all_ids, edge_margin, tie_tol);
        add_safe_ray(rays,
            Ray(origin_pos, Vec3(-0.0, 0.0, 3.0), 0.0, 200.0),
            "axis-parallel", 0, faces, all_ids, edge_margin, tie_tol);
        add_safe_ray(rays,
            Ray(origin_pos, Vec3(0.0, -0.0, dir_z), 0.0, t_point * 0.6),
            "finite-before", 0, faces, all_ids, edge_margin, tie_tol);
        add_safe_ray(rays,
            Ray(origin_pos, Vec3(0.0, -0.0, dir_z), 0.0, t_point * 1.3),
            "finite-after", 0, faces, all_ids, edge_margin, tie_tol);
        add_safe_ray(rays,
            Ray(origin_pos, Vec3(-0.0, 0.0, dir_z), t_point + 2.0, 200.0),
            "finite-tmin-after-occluder", 0, faces, all_ids, edge_margin, tie_tol);
    }

    // Explicit empty rays: one pointing at empty space, one with an interval
    // whose tmin is greater than its tmax.
    {
        add_safe_ray(rays,
            Ray(Vec3(500.0, 500.0, -20.0), Vec3(0.0, 1.0, 2.0), 0.0, 100.0),
            "empty-space", kNoPrim, faces, all_ids, edge_margin, tie_tol);
        add_safe_ray(rays,
            Ray(Vec3(0.0, 0.0, -8.0), Vec3(0.0, 0.0, 2.5), 2.0, 1.5),
            "empty-interval", kNoPrim, faces, all_ids, edge_margin, tie_tol);
    }

    // Fixed-seed random rays.  Ambiguous ones (shared edge, tangency, equal
    // distances) are rejected by the reference scan.
    std::mt19937_64 rng(0x5eed1234ULL);
    size_t random_hits = 0;
    size_t random_misses = 0;
    for (size_t attempt = 0; attempt < 800 && random_hits < 32; ++attempt) {
        const size_t i = static_cast<size_t>(rng() % faces.size());
        const double u = 0.15 + 0.50 * rand01(rng);
        const double v = 0.15 + 0.50 * rand01(rng);
        if (u + v > 0.80)
            continue;
        const Tri& tri = faces[i].tri;
        const Vec3 point = tri.p0 + (tri.p1 - tri.p0) * u +
                            (tri.p2 - tri.p0) * v;
        const Vec3 origin(
            point[0] - 3.0 + 6.0 * static_cast<Scalar>(rand01(rng)),
            point[1] - 3.0 + 6.0 * static_cast<Scalar>(rand01(rng)),
            -8.0 - 4.0 * static_cast<Scalar>(rand01(rng)) * static_cast<Scalar>(i % 2));
        const Vec3 dir = point - origin;
        if (add_safe_ray(rays, Ray(origin, dir, 0.0, 200.0),
                         "random-hit", i, faces, all_ids, edge_margin, tie_tol)) {
            ++random_hits;
        }
    }
    for (size_t attempt = 0; attempt < 400 && random_misses < 12; ++attempt) {
        const Vec3 origin(
            -120.0 + 240.0 * static_cast<Scalar>(rand01(rng)),
            -60.0 + 120.0 * static_cast<Scalar>(rand01(rng)),
            70.0 + 20.0 * static_cast<Scalar>(rand01(rng)));
        const Scalar dz = -(2.0 + 2.0 * static_cast<Scalar>(rand01(rng)));
        const Vec3 dir(
            -1.0 + 2.0 * static_cast<Scalar>(rand01(rng)),
            -1.0 + 2.0 * static_cast<Scalar>(rand01(rng)),
            dz);
        if (add_safe_ray(rays, Ray(origin, dir, 0.0, 10.0),
                         "random-miss", kNoPrim, faces, all_ids,
                         edge_margin, tie_tol)) {
            ++random_misses;
        }
    }

    return rays;
}

// ---------------------------------------------------------------------------
// Scene comparison: both data layouts against the independent reference.
// ---------------------------------------------------------------------------

struct Scene {
    std::string name;
    const Bvh* tree;
    size_t root_id;
    std::vector<size_t> expected_ids;
    std::vector<PrecomputedTri> permuted;
};

struct SceneStats {
    size_t rays = 0;
    size_t closest_tests_original = 0;
    size_t closest_tests_permuted = 0;
    size_t any_tests_original = 0;
    size_t any_tests_permuted = 0;
    size_t hits = 0;
};

static const char* layout_name(Layout layout) {
    return layout == Layout::OriginalOrder ? "original-order" : "permuted";
}

static void compare_scene(const std::string& tag, const Scene& scene,
                          const std::vector<RayCase>& rays,
                          const std::vector<PrecomputedTri>& original_order,
                          const std::vector<Face>& faces,
                          long double scale, SceneStats& stats) {
    const long double t_tol = 1e-9L * scale;
    const long double bary_tol = 1e-9L;
    stats = SceneStats{};

    for (size_t ri = 0; ri < rays.size(); ++ri) {
        const RayCase& rc = rays[ri];
        auto ref = ref_scan(rc.ray, faces, scene.expected_ids,
                            1e-5L, 1e-7L * scale);
        if (ref.ambiguous) {
            fail(tag, "ambiguous reference ray in scene " + scene.name +
                      " ray#" + std::to_string(ri) + " kind=" + rc.kind);
        }
        ++stats.rays;
        if (ref.hit)
            ++stats.hits;

        for (Layout layout : { Layout::OriginalOrder, Layout::Permuted }) {
            size_t tests = 0;
            auto act = actual_intersect<false>(
                *scene.tree, scene.root_id, rc.ray, layout,
                original_order, scene.permuted, tests);
            if (layout == Layout::OriginalOrder)
                stats.closest_tests_original += tests;
            else
                stats.closest_tests_permuted += tests;

            if (act.hit != ref.hit) {
                fail(tag, "hit existence in scene " + scene.name +
                          " ray#" + std::to_string(ri) + " kind=" + rc.kind +
                          " layout=" + layout_name(layout) +
                          "; expected hit=" + std::to_string(ref.hit) +
                          " actual hit=" + std::to_string(act.hit));
            }
            if (ref.hit && act.hit) {
                if (act.prim != ref.prim) {
                    fail(tag, "primitive in scene " + scene.name +
                              " ray#" + std::to_string(ri) + " kind=" + rc.kind +
                              " layout=" + layout_name(layout) +
                              "; expected prim=" + std::to_string(ref.prim) +
                              " actual prim=" + std::to_string(act.prim));
                }
                if (std::fabsl(static_cast<long double>(act.t) - ref.t) > t_tol) {
                    fail(tag, "t in scene " + scene.name +
                              " ray#" + std::to_string(ri) + " kind=" + rc.kind +
                              " layout=" + layout_name(layout) +
                              "; expected t=" + fmt_scalar(static_cast<Scalar>(ref.t)) +
                              " actual t=" + fmt_scalar(act.t));
                }
                if (std::fabsl(static_cast<long double>(act.u) - ref.u) > bary_tol ||
                    std::fabsl(static_cast<long double>(act.v) - ref.v) > bary_tol) {
                    fail(tag, "barycentric in scene " + scene.name +
                              " ray#" + std::to_string(ri) + " kind=" + rc.kind +
                              " layout=" + layout_name(layout) +
                              "; expected u,v=" +
                              fmt_scalar(static_cast<Scalar>(ref.u)) + "," +
                              fmt_scalar(static_cast<Scalar>(ref.v)) +
                              " actual u,v=" + fmt_scalar(act.u) + "," +
                              fmt_scalar(act.v));
                }
            }

            size_t any_tests = 0;
            auto any = actual_intersect<true>(
                *scene.tree, scene.root_id, rc.ray, layout,
                original_order, scene.permuted, any_tests);
            if (layout == Layout::OriginalOrder)
                stats.any_tests_original += any_tests;
            else
                stats.any_tests_permuted += any_tests;
            if (any.hit != ref.hit) {
                fail(tag, "any-hit existence in scene " + scene.name +
                          " ray#" + std::to_string(ri) + " kind=" + rc.kind +
                          " layout=" + layout_name(layout) +
                          "; expected hit=" + std::to_string(ref.hit) +
                          " actual hit=" + std::to_string(any.hit));
            }
        }
    }
}

// Runs the same full-scene ray/reference geometry check at every stage of the
// pipeline (post-refit, post-reinsertion, post-serialization).
static void check_stage_rays(const std::string& tag, const Bvh& tree,
                             const std::vector<Face>& faces,
                             const std::vector<size_t>& all_ids) {
    const auto rays = make_ray_corpus(faces);
    const auto original_pre = make_precomputed(faces);
    Scene scene{
        "stage", &tree, 0, all_ids,
        make_permuted_precomputed(tree, original_pre)
    };
    SceneStats stats;
    compare_scene(tag, scene, rays, original_pre, faces,
                  scene_extent(faces), stats);
    if (stats.rays == 0 ||
        stats.closest_tests_original == 0 ||
        stats.closest_tests_permuted == 0 ||
        stats.any_tests_original == 0 ||
        stats.any_tests_permuted == 0) {
        fail(tag, "stage ray coverage; expected rays and primitive tests > 0 "
                  "actual rays=" + std::to_string(stats.rays) +
                  " closest(original,permuted)=(" +
                  std::to_string(stats.closest_tests_original) + "," +
                  std::to_string(stats.closest_tests_permuted) +
                  ") any(original,permuted)=(" +
                  std::to_string(stats.any_tests_original) + "," +
                  std::to_string(stats.any_tests_permuted) + ")");
    }
}

// ---------------------------------------------------------------------------
// Subtree selection: expected sets always come from the source tree, never
// from the extracted result.
// ---------------------------------------------------------------------------

struct SubtreeChoice {
    size_t node_id;
    std::vector<size_t> expected_ids;
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t nonzero_first_leaves;
};

static std::vector<SubtreeChoice> enumerate_subtrees(const Bvh& tree) {
    std::vector<SubtreeChoice> out;
    for (size_t id = 1; id < tree.nodes.size(); ++id) {
        if (tree.nodes[id].is_leaf())
            continue;
        SubtreeChoice choice;
        choice.node_id = id;
        collect_leaf_ranges(tree, id, choice.ranges);
        collect_node_prim_ids(tree, id, choice.expected_ids);
        choice.nonzero_first_leaves = 0;
        for (const auto& range : choice.ranges) {
            if (range.first != 0)
                ++choice.nonzero_first_leaves;
        }
        out.push_back(std::move(choice));
    }
    return out;
}

static std::array<SubtreeChoice, 2> choose_subtrees(const Bvh& tree) {
    auto candidates = enumerate_subtrees(tree);

    size_t a = kNoNode;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].nonzero_first_leaves < 2 ||
            candidates[i].ranges.size() < 2)
            continue;
        if (a == kNoNode ||
            candidates[i].expected_ids.size() > candidates[a].expected_ids.size())
            a = i;
    }
    if (a == kNoNode) {
        fail("subtree-offset",
             "no non-root inner subtree with multiple non-zero-start leaf "
             "intervals; expected>=1 actual=0");
    }

    // Prefer a second subtree with a disjoint or at least different permanent
    // primitive set, but any distinct non-root inner node is acceptable.
    size_t b = kNoNode;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i == a || candidates[i].expected_ids.empty())
            continue;
        if (b == kNoNode)
            b = i;
        else if (candidates[b].expected_ids == candidates[a].expected_ids &&
                 candidates[i].expected_ids != candidates[a].expected_ids)
            b = i;
    }
    if (b == kNoNode) {
        fail("subtree-offset",
             "need two distinct non-root inner subtrees; expected>=2 actual=1");
    }

    return std::array<SubtreeChoice, 2>{ candidates[a], candidates[b] };
}

// ---------------------------------------------------------------------------
// Windowless PPM rendering: reference and actual colors are compared per
// pixel, and the reference image is written out.
// ---------------------------------------------------------------------------

static std::array<int, 3> color_for(const Face& face, double u, double v) {
    // PrecomputedTri's convention: hit = (1-u-v)*p0 + u*p1 + v*p2.
    const double w = 1.0 - u - v;
    double uv_x = w * face.uv[0][0] + u * face.uv[1][0] + v * face.uv[2][0];
    double uv_y = w * face.uv[0][1] + u * face.uv[1][1] + v * face.uv[2][1];
    uv_x = std::clamp(uv_x, 0.0, 1.0);
    uv_y = std::clamp(uv_y, 0.0, 1.0);

    static const double base[kClusterCount][3] = {
        { 235.0,  70.0,  55.0 },
        {  55.0, 210.0, 100.0 },
        {  70.0,  95.0, 240.0 },
        { 245.0, 195.0,  70.0 },
    };
    const int m = std::clamp(face.material, 0, static_cast<int>(kClusterCount) - 1);
    const double r = base[m][0] * (0.55 + 0.45 * uv_x);
    const double g = base[m][1] * (0.55 + 0.45 * uv_y);
    const double b = base[m][2] * (0.60 + 0.40 * (0.5 * (uv_x + uv_y)));
    return {
        static_cast<int>(std::lround(std::clamp(r, 0.0, 255.0))),
        static_cast<int>(std::lround(std::clamp(g, 0.0, 255.0))),
        static_cast<int>(std::lround(std::clamp(b, 0.0, 255.0))),
    };
}

static void render_ppm(const std::string& path, const Scene& scene,
                       const std::vector<PrecomputedTri>& original_order,
                       const std::vector<Face>& faces, long double scale) {
    constexpr int width = 64;
    constexpr int height = 48;
    constexpr std::array<int, 3> background = { 18, 20, 32 };
    constexpr int channel_tolerance = 2;

    std::vector<unsigned char> pixels(static_cast<size_t>(width * height * 3));
    std::set<int> materials;
    size_t background_pixels = 0;
    size_t hit_pixels = 0;
    size_t bright_pixels = 0;
    size_t tests_original = 0;
    size_t tests_permuted = 0;

    const Vec3 camera(0.4, -72.0, -26.0);
    const long double edge_margin = 1e-5L;
    const long double tie_tol = 1e-7L * scale;

    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            const Scalar ty = -16.0 + 32.0 * ((static_cast<Scalar>(py) + 0.5) /
                                              static_cast<Scalar>(height));
            const Scalar tx = -58.0 + 116.0 * ((static_cast<Scalar>(px) + 0.5) /
                                               static_cast<Scalar>(width));
            const Ray ray(camera, Vec3(tx, ty, 0.0) - camera, 0.0, 500.0);

            auto ref = ref_scan(ray, faces, scene.expected_ids,
                                edge_margin, tie_tol);
            if (ref.ambiguous) {
                fail("pixels", "ambiguous reference pixel; expected a unique "
                               "hit actual ambiguous at px=" + std::to_string(px) +
                               " py=" + std::to_string(py));
            }

            std::array<int, 3> ref_color = background;
            if (ref.hit) {
                ref_color = color_for(faces[ref.prim], static_cast<double>(ref.u),
                                      static_cast<double>(ref.v));
                materials.insert(faces[ref.prim].material);
                ++hit_pixels;
            } else {
                ++background_pixels;
            }
            if (std::max({ ref_color[0], ref_color[1], ref_color[2] }) > 100)
                ++bright_pixels;

            for (Layout layout : { Layout::OriginalOrder, Layout::Permuted }) {
                size_t tests = 0;
                auto act = actual_intersect<false>(
                    *scene.tree, scene.root_id, ray, layout,
                    original_order, scene.permuted, tests);
                if (layout == Layout::OriginalOrder)
                    tests_original += tests;
                else
                    tests_permuted += tests;

                if (act.hit != ref.hit) {
                    fail("pixels", "pixel hit existence at px=" +
                              std::to_string(px) + " py=" + std::to_string(py) +
                              " layout=" + layout_name(layout) +
                              "; expected hit=" + std::to_string(ref.hit) +
                              " actual hit=" + std::to_string(act.hit));
                }
                if (act.hit) {
                    const auto act_color = color_for(faces[act.prim], act.u, act.v);
                    for (size_t ch = 0; ch < 3; ++ch) {
                        if (std::abs(ref_color[ch] - act_color[ch]) >
                            channel_tolerance) {
                            fail("pixels", "pixel color at px=" +
                                      std::to_string(px) + " py=" +
                                      std::to_string(py) + " channel=" +
                                      std::to_string(ch) + " layout=" +
                                      layout_name(layout) +
                                      "; expected=" +
                                      std::to_string(ref_color[ch]) +
                                      " actual=" +
                                      std::to_string(act_color[ch]));
                        }
                    }
                }
            }

            const size_t index = static_cast<size_t>((py * width + px) * 3);
            pixels[index + 0] = static_cast<unsigned char>(ref_color[0]);
            pixels[index + 1] = static_cast<unsigned char>(ref_color[1]);
            pixels[index + 2] = static_cast<unsigned char>(ref_color[2]);
        }
    }

    if (materials.size() < 2) {
        fail("pixels", "image materials; expected>=2 actual=" +
                  std::to_string(materials.size()));
    }
    if (background_pixels == 0) {
        fail("pixels", "image background pixels; expected>0 actual=0");
    }
    if (hit_pixels == 0 || bright_pixels == 0) {
        fail("pixels", "image content; expected non-black pixels actual hit_pixels=" +
                  std::to_string(hit_pixels) + " bright_pixels=" +
                  std::to_string(bright_pixels));
    }
    if (tests_original == 0 || tests_permuted == 0) {
        fail("pixels", "primitive tests; expected>0 actual original=" +
                  std::to_string(tests_original) + " permuted=" +
                  std::to_string(tests_permuted));
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        fail("pixels", "cannot open PPM output; expected a writable path actual=" +
                  path);
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.data()),
              static_cast<std::streamsize>(pixels.size()));
    if (!out) {
        fail("pixels", "failed to write PPM; expected complete file actual=" + path);
    }
}

static size_t quick_traversal(const Bvh& tree, const std::vector<Face>& faces,
                              const std::vector<PrecomputedTri>& original_order) {
    auto permuted = make_permuted_precomputed(tree, original_order);
    size_t total = 0;
    for (size_t c = 0; c < kClusterCount; ++c) {
        const size_t i = c * kPerCluster;
        const Tri& tri = faces[i].tri;
        const Vec3 point = tri.p0 + (tri.p1 - tri.p0) * 0.30 +
                            (tri.p2 - tri.p0) * 0.25;
        const Ray ray(Vec3(point[0], point[1], -8.0), Vec3(0.0, 0.0, 1.0),
                      0.0, 200.0);
        for (Layout layout : { Layout::OriginalOrder, Layout::Permuted }) {
            size_t tests = 0;
            actual_intersect<false>(tree, 0, ray, layout, original_order,
                                    permuted, tests);
            total += tests;
            size_t any_tests = 0;
            actual_intersect<true>(tree, 0, ray, layout, original_order,
                                   permuted, any_tests);
            total += any_tests;
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Driver.
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const std::string ppm_path = argc > 1 ? argv[1] : "reinsert.ppm";

    auto faces = make_mesh();
    auto boxes = make_boxes(faces);
    auto centers = make_centers(faces);

    Builder::Config config;
    config.min_leaf_size = 1;
    config.max_leaf_size = 4;
    auto tree = Builder::build(boxes, centers, config);

    std::vector<size_t> all_ids(kPrimCount);
    std::iota(all_ids.begin(), all_ids.end(), size_t{0});

    const long double geom_eps = 1e-9L * scene_extent(faces);
    validate_tree("topology", tree, all_ids, faces, geom_eps);

    // Every binary (including the defective source copies) performs a finite,
    // numerical traversal of the built tree before any assertion can reject it.
    {
        const auto pre = make_precomputed(faces);
        const size_t warm_tests = quick_traversal(tree, faces, pre);
        if (warm_tests == 0) {
            fail("topology",
                 "initial traversal primitive tests; expected>0 actual=0");
        }
    }

    // Stage 1: move a subset of primitives, making the original topology
    // clearly worse, and update every leaf bbox before refitting.
    move_batch_one(faces);
    refit_with_faces(tree, faces);

    // Real traversal happens even in the defective source copies before the
    // containment assertion rejects them.
    {
        const auto pre = make_precomputed(faces);
        quick_traversal(tree, faces, pre);
    }
    validate_tree("refit-child", tree, all_ids, faces,
                 1e-9L * scene_extent(faces));
    check_stage_rays("ray-reference", tree, faces, all_ids);

    // Stage 2: explicitly run the reinsertion optimizer.  At least one inner
    // node connection must really change; merely calling the entry point is
    // not enough.
    const auto before_nodes = tree.nodes;
    ReinsertionOptimizer<Node>::Config optimizer_config;
    optimizer_config.batch_size_ratio = 0.5;
    optimizer_config.max_iter_count = 4;
    ReinsertionOptimizer<Node>::optimize(tree, optimizer_config);

    const size_t changed = changed_inner_connections(before_nodes, tree.nodes);
    if (changed == 0) {
        fail("optimized-topology",
             "inner node connection changes; expected>=1 actual=0");
    }
    validate_tree("reinsert-bounds", tree, all_ids, faces,
                  1e-9L * scene_extent(faces));
    check_stage_rays("ray-reference", tree, faces, all_ids);

    // Stage 3: move another batch, refit, and round-trip through serialization.
    move_batch_two(faces);
    refit_with_faces(tree, faces);
    validate_tree("refit-child", tree, all_ids, faces,
                  1e-9L * scene_extent(faces));
    check_stage_rays("ray-reference", tree, faces, all_ids);

    std::stringstream stream;
    {
        StdOutputStream out(stream);
        tree.serialize(out);
    }
    StdInputStream in(stream);
    auto restored = Bvh::deserialize(in);
    if (!(restored == tree)) {
        fail("topology",
             "serialization round-trip; expected identical tree actual different");
    }
    validate_tree("reinsert-bounds", restored, all_ids, faces,
                  1e-9L * scene_extent(faces));
    check_stage_rays("ray-reference", restored, faces, all_ids);

    // Stage 4: extract at least two subtrees rooted at non-root inner nodes of
    // the optimized tree.  Expected permanent ids are collected from the
    // source tree before extraction.
    auto choices = choose_subtrees(tree);
    auto sub_a = tree.extract_bvh(choices[0].node_id);
    auto sub_b = tree.extract_bvh(choices[1].node_id);
    validate_tree("subtree-offset", sub_a, choices[0].expected_ids, faces,
                  1e-9L * scene_extent(faces));
    validate_tree("subtree-offset", sub_b, choices[1].expected_ids, faces,
                  1e-9L * scene_extent(faces));

    // Stage 5: ray-reference checks over full tree, restored tree, and both
    // extracted subtrees, in both data layouts.
    const auto rays = make_ray_corpus(faces);

    auto count_kind = [&](const std::string& kind) {
        size_t count = 0;
        for (const auto& rc : rays) {
            if (rc.kind == kind)
                ++count;
        }
        return count;
    };
    const size_t n_nonunit = count_kind("nonunit");
    const size_t n_axis = count_kind("axis-parallel");
    const size_t n_before = count_kind("finite-before");
    const size_t n_after = count_kind("finite-after");
    const size_t n_empty_space = count_kind("empty-space");
    const size_t n_empty_interval = count_kind("empty-interval");
    const size_t n_random_hit = count_kind("random-hit");
    const size_t n_random_miss = count_kind("random-miss");
    if (n_nonunit < 1 || n_axis < 2 || n_before < 1 || n_after < 1 ||
        n_empty_space < 1 || n_empty_interval < 1 ||
        n_random_hit < 8 || n_random_miss < 4) {
        fail("ray-reference",
             "ray category coverage; expected nonunit>=1 axis>=2 before>=1 "
             "after>=1 empty-space>=1 empty-interval>=1 random-hit>=8 "
             "random-miss>=4 actual nonunit=" + std::to_string(n_nonunit) +
             " axis=" + std::to_string(n_axis) +
             " before=" + std::to_string(n_before) +
             " after=" + std::to_string(n_after) +
             " empty-space=" + std::to_string(n_empty_space) +
             " empty-interval=" + std::to_string(n_empty_interval) +
             " random-hit=" + std::to_string(n_random_hit) +
             " random-miss=" + std::to_string(n_random_miss));
    }

    const auto original_pre = make_precomputed(faces);
    Scene full_scene{
        "full", &tree, 0, all_ids,
        make_permuted_precomputed(tree, original_pre)
    };
    Scene restored_scene{
        "restored", &restored, 0, all_ids,
        make_permuted_precomputed(restored, original_pre)
    };
    Scene sub_a_scene{
        "subtree-A", &sub_a, 0, choices[0].expected_ids,
        make_permuted_precomputed(sub_a, original_pre)
    };
    Scene sub_b_scene{
        "subtree-B", &sub_b, 0, choices[1].expected_ids,
        make_permuted_precomputed(sub_b, original_pre)
    };

    SceneStats stats_full, stats_restored, stats_a, stats_b;
    compare_scene("ray-reference", full_scene, rays, original_pre, faces,
                  scene_extent(faces), stats_full);
    compare_scene("ray-reference", restored_scene, rays, original_pre, faces,
                  scene_extent(faces), stats_restored);
    compare_scene("ray-reference", sub_a_scene, rays, original_pre, faces,
                  scene_extent(faces), stats_a);
    compare_scene("ray-reference", sub_b_scene, rays, original_pre, faces,
                  scene_extent(faces), stats_b);

    for (const auto* stats : { &stats_full, &stats_restored, &stats_a, &stats_b }) {
        if (stats->rays == 0 ||
            stats->closest_tests_original == 0 ||
            stats->closest_tests_permuted == 0 ||
            stats->any_tests_original == 0 ||
            stats->any_tests_permuted == 0) {
            fail("ray-reference",
                 "zero test coverage; expected rays and primitive tests > 0 "
                 "actual rays=" + std::to_string(stats->rays) +
                 " closest(original,permuted)=(" +
                 std::to_string(stats->closest_tests_original) + "," +
                 std::to_string(stats->closest_tests_permuted) +
                 ") any(original,permuted)=(" +
                 std::to_string(stats->any_tests_original) + "," +
                 std::to_string(stats->any_tests_permuted) + ")");
        }
    }

    // Prove traversal really culls: a callback that scanned the whole scene
    // would test kPrimCount primitives per ray.  The aggregate over targeted
    // rays must stay strictly below that.
    size_t cull_rays = 0;
    size_t cull_tests = 0;
    for (const auto& rc : rays) {
        if (rc.kind != "targeted")
            continue;
        size_t tests = 0;
        actual_intersect<false>(tree, 0, rc.ray, Layout::OriginalOrder,
                                original_pre, full_scene.permuted, tests);
        cull_tests += tests;
        ++cull_rays;
    }
    if (cull_rays == 0) {
        fail("ray-reference", "culling measurement; expected rays>0 actual=0");
    }
    if (cull_tests >= cull_rays * 8) {
        fail("ray-reference",
             "BVH culling; expected primitive tests < 8 per ray (full scan would "
             "be " + std::to_string(kPrimCount) + ") actual=" +
             std::to_string(cull_tests) + " for " + std::to_string(cull_rays) +
             " rays");
    }

    // Stage 6: tiny PPM rendered and compared against the brute-force colors.
    render_ppm(ppm_path, full_scene, original_pre, faces, scene_extent(faces));

    std::cout << "PASS optimized-topology\n";
    std::cout << "PASS subtree-identity\n";
    std::cout << "PASS ray-reference\n";
    std::cout << "PASS pixels\n";
    return 0;
}
