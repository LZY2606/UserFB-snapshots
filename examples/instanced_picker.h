#ifndef INSTANCED_PICKER_H
#define INSTANCED_PICKER_H

// Windowless instanced picking example built on top of bvh/v2.
//
// Several objects (instances) can share a single local mesh, whose BVH (the
// BLAS) is built exactly once. Each instance carries its own affine transform
// and a visibility mask. Picking is performed in world space: the ray is
// transformed into the local space of every candidate instance, but the ray
// parameter t is preserved, so tmin/tmax always refer to the caller's world
// ray. Inside an instance the existing bvh/v2 traversal does the work;
// instances are never baked into duplicated world geometry and triangles are
// never scanned one by one.
//
// Everything here is double precision and depends on nothing but bvh/v2 and
// the standard library.

#include <bvh/v2/bvh.h>
#include <bvh/v2/vec.h>
#include <bvh/v2/ray.h>
#include <bvh/v2/node.h>
#include <bvh/v2/default_builder.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/tri.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace instanced_picker {

using Scalar = double;

/// A point or direction in 3D, indexed as [0], [1], [2].
using Vec3 = bvh::v2::Vec<Scalar, 3>;

/// Row-major 3x4 affine matrix [A | t], mapping local points p to
/// A * p + t (the last row of the homogeneous 4x4 matrix is [0 0 0 1]).
using Mat3x4 = std::array<Scalar, 12>;

/// A triangle described by its three vertices, in input (original) order.
struct Triangle {
    Vec3 p0, p1, p2;
};

/// Result of a successful pick.
struct Hit {
    uint32_t instance_id = 0;                          ///< Id of the picked instance.
    uint32_t primitive_id = 0;                         ///< Triangle index in the original mesh order.
    Scalar t = 0;                                      ///< Ray parameter on the world ray O + t * D.
    Scalar w0 = 0, w1 = 0, w2 = 0;                     ///< Barycentric weights at p0, p1, p2.
    Vec3 position{0, 0, 0};                            ///< Hit position in world space.
    Vec3 geometric_normal{0, 0, 1};                    ///< Unit geometric normal in world space.
};

/// Optional callback used to reject candidate hits (treated as transparent).
/// It receives a fully populated candidate Hit; returning false means "keep
/// looking for the nearest acceptable surface". The order in which candidates
/// are presented, and how many times the callback is invoked, are unspecified.
using AcceptCallback = std::function<bool(const Hit&)>;

namespace detail {

using BvhNode = bvh::v2::Node<Scalar, 3>;
using Bvh     = bvh::v2::Bvh<BvhNode>;
using BvhRay  = bvh::v2::Ray<Scalar, 3>;
using LocalTri = bvh::v2::Tri<Scalar, 3>;
using PrecomputedTri = bvh::v2::PrecomputedTri<Scalar>;

inline bool is_finite(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline bool is_finite(const Mat3x4& m) {
    for (Scalar x : m)
        if (!std::isfinite(x))
            return false;
    return true;
}

// Inverts a row-major 3x4 affine matrix. Returns false (and leaves `inv`
// untouched) when the linear part is singular or the entries are not finite.
inline bool invert_affine(const Mat3x4& m, Mat3x4& inv) {
    if (!is_finite(m))
        return false;

    // Linear part A, column-major for the 3x3 inversion.
    Scalar a[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            a[c][r] = m[r * 4 + c];

    Scalar det = 0;
    for (int i = 0; i < 3; ++i)
        det += a[0][i] * (a[1][(i + 1) % 3] * a[2][(i + 2) % 3]
                        - a[1][(i + 2) % 3] * a[2][(i + 1) % 3]);

    if (!std::isfinite(det) || det == 0)
        return false;

    Scalar inv_det = 1 / det;
    Scalar ia[3][3]; // inverse, column-major (cofactor transpose divided by det)
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            // Cofactor C[c][r]: delete row c and column r of A first.
            int rr1 = (c + 1) % 3, rr2 = (c + 2) % 3; // remaining rows
            int cc1 = (r + 1) % 3, cc2 = (r + 2) % 3; // remaining columns
            Scalar minor = a[cc1][rr1] * a[cc2][rr2] - a[cc2][rr1] * a[cc1][rr2];
            Scalar cofactor = ((r + c) % 2 == 0) ? minor : -minor;
            ia[c][r] = cofactor * inv_det;
        }
    }

    Vec3 t{m[3], m[7], m[11]};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            inv[r * 4 + c] = ia[c][r]; // back to row-major
        // Translation of the inverse: -A^-1 t
        Scalar s = 0;
        for (int c = 0; c < 3; ++c)
            s += ia[c][r] * t[c];
        inv[r * 4 + 3] = -s;
    }
    return is_finite(inv);
}

inline Vec3 transform_point(const Mat3x4& m, const Vec3& p) {
    return Vec3(
        m[0] * p[0] + m[1] * p[1] + m[2]  * p[2] + m[3],
        m[4] * p[0] + m[5] * p[1] + m[6]  * p[2] + m[7],
        m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]);
}

inline Vec3 transform_vector(const Mat3x4& m, const Vec3& v) {
    return Vec3(
        m[0] * v[0] + m[1] * v[1] + m[2]  * v[2],
        m[4] * v[0] + m[5] * v[1] + m[6]  * v[2],
        m[8] * v[0] + m[9] * v[1] + m[10] * v[2]);
}

} // namespace detail

/// A mesh owns its triangles in their original input order and a single BVH
/// (BLAS) built over them. The BVH is constructed once and shared read-only by
/// every instance referencing this mesh.
///
///     Mesh mesh(std::vector<Triangle>{ ... });
class Mesh {
public:
    explicit Mesh(std::vector<Triangle> triangles)
        : triangles_(std::move(triangles))
    {
        const size_t count = triangles_.size();
        if (count == 0)
            return; // An empty mesh is legal; it simply never intersects.

        std::vector<bvh::v2::BBox<Scalar, 3>> bboxes(count);
        std::vector<Vec3> centers(count);
        std::vector<detail::LocalTri> tris;
        tris.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& tri = triangles_[i];
            detail::LocalTri local(tri.p0, tri.p1, tri.p2);
            tris.emplace_back(local);
            bboxes[i] = local.get_bbox();
            centers[i] = local.get_center();
        }

        typename bvh::v2::DefaultBuilder<detail::BvhNode>::Config config;
        config.quality = bvh::v2::DefaultBuilder<detail::BvhNode>::Quality::High;
        bvh_ = bvh::v2::DefaultBuilder<detail::BvhNode>::build(bboxes, centers, config);

        // Permute the precomputed triangles to match the BVH primitive order,
        // removing the indirection during traversal (as in simple_example).
        precomputed_.resize(count);
        for (size_t i = 0; i < count; ++i)
            precomputed_[i] = detail::PrecomputedTri(tris[bvh_.prim_ids[i]]);
    }

    size_t size() const { return triangles_.size(); }
    const std::vector<Triangle>& triangles() const { return triangles_; }

#ifdef INSTANCED_PICKER_ENABLE_TEST_HOOKS
    // Test-only view of the permuted BVH primitive order. The public API never
    // leaks these indices; primitive_id in a Hit is always the original one.
    const std::vector<size_t>& test_prim_ids() const { return bvh_.prim_ids; }
#endif

private:
    std::vector<Triangle> triangles_;
    detail::Bvh bvh_;
    std::vector<detail::PrecomputedTri> precomputed_;

    friend class Picker;
};

/// An instance places a shared local mesh in the world.
///
///     Instance instance(
///         /*id*/ 7u,
///         std::shared_ptr<const Mesh>(...),
///         /*row-major 3x4 local_to_world*/ mat,
///         /*mask*/ 0x1u);
struct Instance {
    uint32_t id;
    std::shared_ptr<const Mesh> mesh;
    Mat3x4 local_to_world;
    uint32_t mask;

    Instance(uint32_t id,
             std::shared_ptr<const Mesh> mesh,
             const Mat3x4& local_to_world,
             uint32_t mask)
        : id(id), mesh(std::move(mesh)), local_to_world(local_to_world), mask(mask)
    {}
};

/// Holds the instances and answers world-space picking queries.
///
///     Picker picker(std::vector<Instance>{ ... });
///     auto hit = picker.pick(origin, direction, tmin, tmax, query_mask);
///     auto filtered = picker.pick(origin, direction, tmin, tmax, query_mask,
///                                 [](const Hit& h) { return h.primitive_id != 2; });
class Picker {
public:
    explicit Picker(std::vector<Instance> instances) {
        instances_.reserve(instances.size());
        for (auto& instance : instances) {
            Mat3x4 world_to_local;
            if (!detail::invert_affine(instance.local_to_world, world_to_local))
                throw std::invalid_argument(
                    "instanced_picker: instance " + std::to_string(instance.id)
                    + " has a non-finite or non-invertible transform");
            if (!instance.mesh)
                throw std::invalid_argument(
                    "instanced_picker: instance " + std::to_string(instance.id)
                    + " references a null mesh");
            if (!seen_ids_.insert(instance.id).second)
                throw std::invalid_argument(
                    "instanced_picker: duplicate instance id " + std::to_string(instance.id));

            instances_.push_back(Entry{
                instance.id,
                instance.mesh,
                instance.local_to_world,
                world_to_local,
                instance.mask
            });
        }
    }

    /// Picks the nearest acceptable intersection. Returns std::nullopt when
    /// nothing is hit. `accept` is optional; every candidate passes when it is
    /// omitted. Rejected candidates never shrink the effective tmax.
    std::optional<Hit> pick(
        const Vec3& origin,
        const Vec3& direction,
        Scalar tmin,
        Scalar tmax,
        uint32_t query_mask,
        const AcceptCallback& accept = {}) const
    {
        if (!detail::is_finite(origin) || !detail::is_finite(direction))
            throw std::invalid_argument("instanced_picker: ray origin or direction is not finite");
        if (direction[0] == 0 && direction[1] == 0 && direction[2] == 0)
            throw std::invalid_argument("instanced_picker: ray direction must not be zero");
        if (!std::isfinite(tmin) || !std::isfinite(tmax) || tmin > tmax)
            throw std::invalid_argument("instanced_picker: require finite tmin <= tmax");

        std::optional<Hit> best;

        // A growing stack keeps traversal safe even for deep BVHs; the fixed
        // SmallStack only asserts capacity in debug builds. Closest-hit
        // traversal always drains the stack, so one instance reuses the
        // storage of the previous one.
        bvh::v2::GrowingStack<detail::Bvh::Index> stack;

        // Instances may be traversed in any order; the tie-breaking rule below
        // makes the result independent of this order.
        for (const Entry& entry : instances_) {
            if ((entry.mask & query_mask) == 0)
                continue;
            const Mesh& mesh = *entry.mesh;
            if (mesh.size() == 0)
                continue;

            // Transform the ray into local space. Affine maps preserve the ray
            // parameter: world point O + t D maps to O' + t D', so the t
            // returned by the local traversal is the same t as on the world
            // ray, and tmin/tmax stay the caller's values.
            detail::BvhRay local_ray(
                detail::transform_point(entry.world_to_local, origin),
                detail::transform_vector(entry.world_to_local, direction),
                tmin,
                tmax);

            // Nearest *accepted* hit of this instance. Rejected candidates do
            // not update it, so a rejected near face cannot cull geometry.
            Scalar closest_t = tmax;
            uint32_t closest_prim = 0;
            Scalar closest_u = 0, closest_v = 0;
            bool found = false;

            // Robust traversal: the fast ray-box test multiplies infinities
            // (zero direction components) through an FMA and yields NaN for
            // origins far from the origin; the robust variant uses safe
            // inverses and T. Ize's padded inverse directions instead.
            mesh.bvh_.intersect<false, true>(local_ray, mesh.bvh_.get_root().index, stack,
                [&] (size_t begin, size_t end) {
                    bool hit_in_leaf = false;
                    for (size_t i = begin; i < end; ++i) {
                        // Triangles were permuted into BVH order, so slot i
                        // lines up with primitive slot i; the user-visible id
                        // goes through prim_ids to recover the original order.
                        auto hit = mesh.precomputed_[i].intersect(local_ray);
                        if (!hit)
                            continue;
                        auto [t, u, v] = *hit;
                        uint32_t original_id =
                            static_cast<uint32_t>(mesh.bvh_.prim_ids[i]);

                        // Build the full candidate before consulting the
                        // callback, which receives the same Hit the caller
                        // would get. Nothing is accepted until it says so.
                        Hit candidate = make_hit(entry, mesh, original_id, t, u, v);
                        if (accept && !accept(candidate))
                            continue;

                        // Exact ties are resolved by (instance_id, primitive_id);
                        // approximate equality is never treated as a tie. The
                        // instance id is constant here, so only primitive_id
                        // matters for the within-instance tie.
                        if (!found || t < closest_t ||
                            (t == closest_t && original_id < closest_prim))
                        {
                            closest_t = t;
                            closest_prim = original_id;
                            closest_u = u;
                            closest_v = v;
                            found = true;
                            hit_in_leaf = true;
                            local_ray.tmax = t; // prune farther geometry within this instance
                        }
                    }
                    return hit_in_leaf;
                });

            if (!found)
                continue;

            Hit candidate = make_hit(entry, mesh, closest_prim, closest_t, closest_u, closest_v);
            if (!best || candidate.t < best->t ||
                (candidate.t == best->t &&
                 std::tie(candidate.instance_id, candidate.primitive_id)
                 < std::tie(best->instance_id, best->primitive_id)))
                best = candidate;
        }

        return best;
    }

private:
    struct Entry {
        uint32_t id;
        std::shared_ptr<const Mesh> mesh;
        Mat3x4 local_to_world;
        Mat3x4 world_to_local;
        uint32_t mask;
    };

    std::vector<Entry> instances_;
    std::unordered_set<uint32_t> seen_ids_;

    static Hit make_hit(const Entry& entry, const Mesh& mesh,
                        uint32_t primitive_id, Scalar t, Scalar u, Scalar v) {
        const Triangle& tri = mesh.triangles_[primitive_id];
        Scalar w0 = 1 - u - v; // PrecomputedTri reports (u, v) as weights of p1 and p2.
        Scalar w1 = u;
        Scalar w2 = v;

        Vec3 local_position(
            w0 * tri.p0[0] + w1 * tri.p1[0] + w2 * tri.p2[0],
            w0 * tri.p0[1] + w1 * tri.p1[1] + w2 * tri.p2[1],
            w0 * tri.p0[2] + w1 * tri.p1[2] + w2 * tri.p2[2]);

        // Geometric normal from the actual transformed vertex winding. Under a
        // negative determinant the winding flips, which the cross product
        // reproduces automatically; no back-face culling is performed.
        Vec3 wp0 = detail::transform_point(entry.local_to_world, tri.p0);
        Vec3 wp1 = detail::transform_point(entry.local_to_world, tri.p1);
        Vec3 wp2 = detail::transform_point(entry.local_to_world, tri.p2);
        Vec3 normal = bvh::v2::cross(wp1 - wp0, wp2 - wp0);
        Scalar len = bvh::v2::dot(normal, normal);
        if (len > 0)
            normal = normal * (1 / std::sqrt(len));

        Hit hit;
        hit.instance_id = entry.id;
        hit.primitive_id = primitive_id;
        hit.t = t;
        hit.w0 = w0;
        hit.w1 = w1;
        hit.w2 = w2;
        hit.position = detail::transform_point(entry.local_to_world, local_position);
        hit.geometric_normal = normal;
        return hit;
    }
};

} // namespace instanced_picker

#endif // INSTANCED_PICKER_H
