#include "task_support/check.h"

#include "raytracer/world/World.hpp"
#include "raytracer/cameras/Perspective.hpp"
#include "raytracer/samplers/Simple.hpp"
#include "raytracer/geometry/Triangle.hpp"
#include "raytracer/geometry/Sphere.hpp"
#include "raytracer/geometry/Plane.hpp"
#include "raytracer/materials/Matte.hpp"
#include "raytracer/materials/HierarchicalDiffuse.hpp"
#include "raytracer/lighting/HierarchicalRadiosity.hpp"
#include "raytracer/tracers/Whitted.hpp"
#include "raytracer/utilities/Ray.hpp"
#include "raytracer/utilities/ShadeInfo.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <set>
#include <utility>
#include <vector>

namespace {

const double kPi = 3.14159265358979323846;

using D3 = std::array<double, 3>;
D3 vsub(const D3 &a, const D3 &b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
D3 vadd(const D3 &a, const D3 &b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
D3 vmul(const D3 &a, double s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
double vdot(const D3 &a, const D3 &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
double vlen(const D3 &a) { return std::sqrt(vdot(a, a)); }
D3 vnorm(const D3 &a) { return vmul(a, 1.0 / vlen(a)); }
D3 vcross(const D3 &a, const D3 &b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
D3 as_d3(const Point3D &p) { return {p.x, p.y, p.z}; }

struct RefLeaf {
    D3 centroid;
    int original;
};

// Independent DFS subdivision; child numbering matches the production tree.
void ref_subdivide(const D3 &a, const D3 &b, const D3 &c, int depth,
                   int original, std::vector<RefLeaf> &out) {
    if (depth == 0) {
        out.push_back({vmul(vadd(vadd(a, b), c), 1.0 / 3.0), original});
        return;
    }
    D3 m01 = vmul(vadd(a, b), 0.5);
    D3 m12 = vmul(vadd(b, c), 0.5);
    D3 m20 = vmul(vadd(c, a), 0.5);
    ref_subdivide(a, m01, m20, depth - 1, original, out);
    ref_subdivide(m01, b, m12, depth - 1, original, out);
    ref_subdivide(m20, m12, c, depth - 1, original, out);
    ref_subdivide(m01, m12, m20, depth - 1, original, out);
}

double geometry_kernel(const D3 &ci, const D3 &ni,
                       const D3 &cj, const D3 &nj) {
    D3 d = vsub(cj, ci);
    double r2 = vdot(d, d);
    if (r2 <= 0.0) return 0.0;
    D3 u = vmul(d, 1.0 / std::sqrt(r2));
    double fi = std::max(0.0, vdot(ni, u));
    double fj = std::max(0.0, vdot(nj, vmul(u, -1.0)));
    return fi * fj / (kPi * r2);
}

struct RefScene {
    std::vector<Triangle *> tris;
    std::vector<D3> normal;
    std::vector<double> leaf_area;
    std::vector<RefLeaf> leaves;
};

RefScene make_ref_scene(const std::vector<Triangle *> &tris, int depth) {
    RefScene ref;
    ref.tris = tris;
    for (int o = 0; o < (int)tris.size(); ++o) {
        D3 p[3] = {as_d3(tris[o]->vertex(0)),
                   as_d3(tris[o]->vertex(1)),
                   as_d3(tris[o]->vertex(2))};
        D3 e1 = vsub(p[1], p[0]);
        D3 e2 = vsub(p[2], p[0]);
        ref.normal.push_back(vnorm(vcross(e1, e2)));
        double area = 0.5 * vlen(vcross(e1, e2));
        ref.leaf_area.push_back(area / std::pow(4.0, depth));
        ref_subdivide(p[0], p[1], p[2], depth, o, ref.leaves);
    }
    return ref;
}

// Independent dense leaf-centroid kernel with real open-segment visibility.
std::vector<std::vector<double>> dense_kernel(World &world,
                                              const RefScene &ref) {
    int n = (int)ref.leaves.size();
    std::vector<std::vector<double>> K(n, std::vector<double>(n, 0.0));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (ref.leaves[i].original == ref.leaves[j].original) continue;
            const RefLeaf &li = ref.leaves[i];
            const RefLeaf &lj = ref.leaves[j];
            double k = geometry_kernel(li.centroid, ref.normal[li.original],
                                       lj.centroid, ref.normal[lj.original]);
            if (k <= 0.0) continue;
            Point3D O((float)li.centroid[0], (float)li.centroid[1],
                      (float)li.centroid[2]);
            Point3D Q((float)lj.centroid[0], (float)lj.centroid[1],
                      (float)lj.centroid[2]);
            bool blocked = world.segment_occluded(
                O, Q, ref.tris[li.original], ref.tris[lj.original]);
            if (!blocked) K[i][j] = k;
        }
    }
    return K;
}

// Independent dense Jacobi solution of B = E + rho (K A B).
std::vector<std::array<double, 3>> dense_solve(
        const RefScene &ref,
        const std::vector<std::vector<double>> &K,
        const std::vector<std::array<double, 3>> &rho,
        const std::vector<std::array<double, 3>> &emission) {
    int n = (int)ref.leaves.size();
    std::vector<std::array<double, 3>> B(n), nxt(n);
    for (int i = 0; i < n; ++i) B[i] = emission[ref.leaves[i].original];
    for (int iter = 0; iter < 200000; ++iter) {
        double residual = 0.0;
        for (int i = 0; i < n; ++i) {
            int oi = ref.leaves[i].original;
            for (int c = 0; c < 3; ++c) {
                double incident = 0.0;
                for (int j = 0; j < n; ++j)
                    incident += ref.leaf_area[ref.leaves[j].original] *
                                K[i][j] * B[j][c];
                nxt[i][c] = emission[oi][c] + rho[oi][c] * incident;
                residual = std::max(residual,
                                    std::abs(nxt[i][c] - B[i][c]));
            }
        }
        B.swap(nxt);
        if (residual < 1e-12) break;
    }
    return B;
}

Triangle *facing_tri(float z, bool upper_normal_minus_z) {
    // Winding chosen so the two triangles' normals face each other.
    if (upper_normal_minus_z)
        return new Triangle(Point3D(-1, -1, z), Point3D(-1, 1, z),
                            Point3D(1, -1, z));
    return new Triangle(Point3D(-1, -1, z), Point3D(1, -1, z),
                        Point3D(-1, 1, z));
}

} // namespace

// ----------------------------- links group ---------------------------------

static int links_are_unique_and_ordered() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();

    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, 2, 1e-3);
    TASK_CHECK(hr.ok());
    TASK_CHECK(!hr.links().empty());

    std::set<std::pair<int, int>> seen;
    for (const RadiosityLink &l : hr.links()) {
        TASK_CHECK(l.node_a < l.node_b);
        TASK_CHECK(seen.insert({l.node_a, l.node_b}).second);
        TASK_CHECK(hr.node_original(l.node_a) != hr.node_original(l.node_b));
        TASK_CHECK(l.kernel_lo <= l.kernel + 1e-18);
        TASK_CHECK(l.kernel <= l.kernel_hi + 1e-18);
        TASK_CHECK(l.kernel_lo >= 0.0 && l.kernel_hi >= l.kernel_lo);
        if (l.exact) {
            TASK_CHECK(hr.node_is_leaf(l.node_a) && hr.node_is_leaf(l.node_b));
            TASK_CHECK(l.kernel_lo == l.kernel && l.kernel_hi == l.kernel);
        }
    }
    TASK_CHECK(seen.size() == hr.links().size());
    return 0;
}

static int links_kernel_interval_contains_all_descendants() {
    World world;
    Triangle *t1 = facing_tri(2.0f, true);
    Triangle *t2 = facing_tri(-2.0f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();

    const int depth = 3;
    // Large eta guarantees genuine (non-leaf) approximated links exist.
    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, depth, 0.1);
    TASK_CHECK(hr.ok());

    RefScene ref = make_ref_scene({t1, t2}, depth);
    std::size_t approximate = 0;
    for (const RadiosityLink &l : hr.links()) {
        if (l.exact) continue;
        ++approximate;
        std::vector<std::size_t> la = hr.descendant_leaves(l.node_a);
        std::vector<std::size_t> lb = hr.descendant_leaves(l.node_b);
        for (std::size_t ia : la) {
            for (std::size_t ib : lb) {
                const RefLeaf &a = ref.leaves[ia];
                const RefLeaf &b = ref.leaves[ib];
                double k = geometry_kernel(a.centroid, ref.normal[a.original],
                                           b.centroid, ref.normal[b.original]);
                // Bounds must enclose every descendant leaf pair's kernel;
                // no corner/centroid min-max sampling shortcut.
                TASK_CHECK(k >= l.kernel_lo - 1e-18);
                TASK_CHECK(k <= l.kernel_hi + 1e-18);
            }
        }
    }
    TASK_CHECK(approximate > 0);
    return 0;
}

static int links_far_field_compression() {
    World world;
    const float edge = 0.01f;
    Triangle *t1 = new Triangle(Point3D(-edge, -edge, 5.0f),
                                Point3D(-edge, edge, 5.0f),
                                Point3D(edge, -edge, 5.0f));
    Triangle *t2 = new Triangle(Point3D(-edge, -edge, -5.0f),
                                Point3D(edge, -edge, -5.0f),
                                Point3D(-edge, edge, -5.0f));
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();

    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, 4, 1e-3);
    TASK_CHECK(hr.ok());

    std::size_t full = hr.full_leaf_pair_count();
    TASK_CHECK(full == 256u * 256u);
    bool cross_level = false;
    for (const RadiosityLink &l : hr.links()) {
        if (!hr.node_is_leaf(l.node_a) || !hr.node_is_leaf(l.node_b)) {
            cross_level = true;
            break;
        }
    }
    TASK_CHECK(cross_level);
    TASK_CHECK(hr.visibility_queries() < full / 4);
    // A dense leaf-pair matrix needs at least one double per unordered pair.
    TASK_CHECK(hr.storage_peak_bytes() < full * sizeof(double));
    TASK_CHECK(hr.links().size() < full);
    return 0;
}

static int links_exact_queries_track_leaf_pairs() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    Sphere *blocker = new Sphere(Point3D(0.6f, 0.0f, 0.0f), 0.25f);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.add_geometry(blocker);
    world.build_accel();

    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, 2, 1e-4);
    TASK_CHECK(hr.ok());

    std::size_t exact = 0;
    for (const RadiosityLink &l : hr.links())
        if (l.exact) ++exact;
    TASK_CHECK(exact == hr.visibility_queries());
    TASK_CHECK(hr.visibility_queries() <= hr.full_leaf_pair_count());
    TASK_CHECK(hr.node_tests() > 0 || hr.primitive_tests() > 0);
    return 0;
}

static int links_different_areas_and_reordered_identity() {
    World world;
    Triangle *big = facing_tri(2.0f, true);
    Triangle *small = new Triangle(Point3D(-0.1f, -0.1f, -2.0f),
                                   Point3D(0.1f, -0.1f, -2.0f),
                                   Point3D(-0.1f, 0.1f, -2.0f));
    world.add_geometry(big);
    world.add_geometry(small);
    world.build_accel();

    HierarchicalRadiosity a(world, {big, small},
                            {RGBColor(0.5f), RGBColor(0.5f)},
                            {RGBColor(1.0f), RGBColor(0.0f)}, 2, 1e-3);
    TASK_CHECK(a.ok());
    HierarchicalRadiosity b(world, {small, big},
                            {RGBColor(0.5f), RGBColor(0.5f)},
                            {RGBColor(0.0f), RGBColor(1.0f)}, 2, 1e-3);
    TASK_CHECK(b.ok());

    // Reordering inputs preserves the compressed structure size.
    TASK_CHECK(a.links().size() == b.links().size());
    TASK_CHECK(a.visibility_queries() == b.visibility_queries());
    TASK_CHECK(a.node_count() == b.node_count());

    // Leaf areas scale by 4^-D and the big face's leaves are the larger ones.
    double area_ratio = a.leaf_area(0) / a.leaf_area(a.leaf_count() - 1);
    TASK_CHECK(std::abs(area_ratio - 100.0) / 100.0 < 1e-6);
    for (std::size_t i = 0; i < a.leaf_count(); ++i) {
        TASK_CHECK(a.leaf_triangle(i) != nullptr);
        TASK_CHECK(a.leaf_original(i) == 0 || a.leaf_original(i) == 1);
    }
    // Every link still joins the two distinct originals.
    for (const RadiosityLink &l : a.links())
        TASK_CHECK(a.node_original(l.node_a) != a.node_original(l.node_b));
    return 0;
}

// --------------------------- transport group -------------------------------

static int transport_matches_dense_solution() {
    World world;
    Triangle *t1 = facing_tri(1.5f, true);
    Triangle *t2 = facing_tri(-1.5f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();

    const int depth = 3;
    std::vector<RGBColor> rho{RGBColor(0.5f, 0.6f, 0.4f),
                              RGBColor(0.3f, 0.2f, 0.7f)};
    std::vector<RGBColor> em{RGBColor(1.0f, 0.5f, 0.2f),
                             RGBColor(0.1f, 0.0f, 0.3f)};
    HierarchicalRadiosity hr(world, {t1, t2}, rho, em, depth, 1e-4);
    TASK_CHECK(hr.ok());
    TASK_CHECK(hr.compressed_row_sum_max() <= 1.1);

    RefScene ref = make_ref_scene({t1, t2}, depth);
    std::vector<std::vector<double>> K = dense_kernel(world, ref);
    std::vector<std::array<double, 3>> rho_d{
        {0.5, 0.6, 0.4}, {0.3, 0.2, 0.7}};
    std::vector<std::array<double, 3>> em_d{
        {1.0, 0.5, 0.2}, {0.1, 0.0, 0.3}};
    std::vector<std::array<double, 3>> reference =
        dense_solve(ref, K, rho_d, em_d);

    double max_error = 0.0;
    for (std::size_t i = 0; i < hr.leaf_count(); ++i) {
        double r = 0.0, g = 0.0, b = 0.0;
        hr.leaf_radiosity(i, r, g, b);
        double got[3] = {r, g, b};
        for (int c = 0; c < 3; ++c)
            max_error = std::max(max_error, std::abs(got[c] - reference[i][c]));
    }
    TASK_CHECK(max_error <= 1e-3);
    return 0;
}

static int transport_far_field_eta_tight_matches_dense() {
    World world;
    const float edge = 0.05f;
    // Far enough that eta = 1e-4 still admits cross-level approximated links.
    Triangle *t1 = new Triangle(Point3D(-edge, -edge, 3.0f),
                                Point3D(-edge, edge, 3.0f),
                                Point3D(edge, -edge, 3.0f));
    Triangle *t2 = new Triangle(Point3D(-edge, -edge, -3.0f),
                                Point3D(edge, -edge, -3.0f),
                                Point3D(-edge, edge, -3.0f));
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();

    const int depth = 3;
    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f, 0.6f, 0.4f),
                              RGBColor(0.3f, 0.2f, 0.7f)},
                             {RGBColor(1.0f, 0.5f, 0.2f), RGBColor(0.0f)},
                             depth, 1e-4);
    TASK_CHECK(hr.ok());
    bool approximate = false;
    for (const RadiosityLink &l : hr.links())
        if (!l.exact) approximate = true;
    TASK_CHECK(approximate);
    TASK_CHECK(hr.visibility_queries() < hr.full_leaf_pair_count());

    RefScene ref = make_ref_scene({t1, t2}, depth);
    std::vector<std::vector<double>> K = dense_kernel(world, ref);
    std::vector<std::array<double, 3>> rho_d{
        {0.5, 0.6, 0.4}, {0.3, 0.2, 0.7}};
    std::vector<std::array<double, 3>> em_d{
        {1.0, 0.5, 0.2}, {0.0, 0.0, 0.0}};
    std::vector<std::array<double, 3>> reference =
        dense_solve(ref, K, rho_d, em_d);

    double max_error = 0.0;
    for (std::size_t i = 0; i < hr.leaf_count(); ++i) {
        double r = 0.0, g = 0.0, b = 0.0;
        hr.leaf_radiosity(i, r, g, b);
        double got[3] = {r, g, b};
        for (int c = 0; c < 3; ++c)
            max_error = std::max(max_error, std::abs(got[c] - reference[i][c]));
    }
    TASK_CHECK(max_error <= 1e-3);
    return 0;
}

static int transport_area_reciprocity() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();

    const int depth = 2;
    RefScene ref = make_ref_scene({t1, t2}, depth);
    std::vector<std::vector<double>> K = dense_kernel(world, ref);
    int n = (int)ref.leaves.size();
    double worst = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            double ai = ref.leaf_area[ref.leaves[i].original];
            double aj = ref.leaf_area[ref.leaves[j].original];
            worst = std::max(worst, std::abs(ai * K[i][j] - aj * K[j][i]));
        }
    }
    TASK_CHECK(worst < 1e-14);
    return 0;
}

static int transport_local_shadow_from_intermediate_blocker() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    Sphere *blocker = new Sphere(Point3D(0.0f, 0.0f, 0.0f), 0.4f);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.add_geometry(blocker);
    world.build_accel();

    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, 3, 1e-4);
    TASK_CHECK(hr.ok());

    // At least one exact leaf pair must be driven to a zero kernel by the
    // blocker, and the shadowed receiver leaves must be darker than lit ones.
    bool zero_link = false;
    for (const RadiosityLink &l : hr.links())
        if (l.exact && l.kernel == 0.0) zero_link = true;
    TASK_CHECK(zero_link);

    double darkest = 1e30, brightest = 0.0;
    for (std::size_t i = 0; i < hr.leaf_count(); ++i) {
        if (hr.leaf_original(i) != 1) continue;
        double r, g, b;
        hr.leaf_radiosity(i, r, g, b);
        darkest = std::min(darkest, r);
        brightest = std::max(brightest, r);
    }
    TASK_CHECK(brightest > darkest);

    // Same scene without the blocker keeps every exact kernel positive.
    World clear;
    Triangle *c1 = facing_tri(1.0f, true);
    Triangle *c2 = facing_tri(-1.0f, false);
    clear.add_geometry(c1);
    clear.add_geometry(c2);
    clear.build_accel();
    HierarchicalRadiosity hr_clear(clear, {c1, c2},
                                   {RGBColor(0.5f), RGBColor(0.5f)},
                                   {RGBColor(1.0f), RGBColor(0.0f)}, 3, 1e-4);
    TASK_CHECK(hr_clear.ok());
    for (const RadiosityLink &l : hr_clear.links())
        if (l.exact) TASK_CHECK(l.kernel > 0.0);
    return 0;
}

static int transport_geometry_behind_segment_does_not_shadow() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();
    HierarchicalRadiosity before(world, {t1, t2},
                                 {RGBColor(0.5f), RGBColor(0.5f)},
                                 {RGBColor(1.0f), RGBColor(0.0f)}, 2, 1e-3);
    TASK_CHECK(before.ok());

    // A large triangle beyond the far endpoint: intersections have s > 1.
    Triangle *behind = new Triangle(Point3D(-5, -5, -1.5f),
                                    Point3D(5, -5, -1.5f),
                                    Point3D(-5, 5, -1.5f));
    world.add_geometry(behind);
    world.build_accel();
    HierarchicalRadiosity after(world, {t1, t2},
                                {RGBColor(0.5f), RGBColor(0.5f)},
                                {RGBColor(1.0f), RGBColor(0.0f)}, 2, 1e-3);
    TASK_CHECK(after.ok());

    for (std::size_t i = 0; i < before.leaf_count(); ++i) {
        double r1, g1, b1, r2, g2, b2;
        before.leaf_radiosity(i, r1, g1, b1);
        after.leaf_radiosity(i, r2, g2, b2);
        TASK_CHECK(std::abs(r1 - r2) < 1e-12);
        TASK_CHECK(std::abs(g1 - g2) < 1e-12);
        TASK_CHECK(std::abs(b1 - b2) < 1e-12);
    }
    return 0;
}

static int transport_emission_update_reuses_tree_without_queries() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();
    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, 2, 1e-3);
    TASK_CHECK(hr.ok());

    unsigned long long vis = hr.visibility_queries();
    unsigned long long nodes = hr.node_tests();
    unsigned long long prims = hr.primitive_tests();
    double old_receiver = 0.0;
    for (std::size_t i = 0; i < hr.leaf_count(); ++i)
        if (hr.leaf_original(i) == 1) {
            double r, g, b;
            hr.leaf_radiosity(i, r, g, b);
            old_receiver = r;
            break;
        }

    TASK_CHECK(hr.set_emission(0, RGBColor(3.0f, 2.0f, 1.0f)));
    TASK_CHECK(hr.visibility_queries() == vis);
    TASK_CHECK(hr.node_tests() == nodes);
    TASK_CHECK(hr.primitive_tests() == prims);

    double new_receiver = 0.0;
    for (std::size_t i = 0; i < hr.leaf_count(); ++i)
        if (hr.leaf_original(i) == 1) {
            double r, g, b;
            hr.leaf_radiosity(i, r, g, b);
            new_receiver = r;
            break;
        }
    TASK_CHECK(new_receiver > old_receiver);

    // Invalid values are rejected and leave the complete solution intact.
    TASK_CHECK(!hr.set_emission(0, RGBColor(5.0f, 0.0f, 0.0f)));
    TASK_CHECK(!hr.set_emission(-1, RGBColor(0.0f)));
    double kept = 0.0;
    for (std::size_t i = 0; i < hr.leaf_count(); ++i)
        if (hr.leaf_original(i) == 1) {
            double r, g, b;
            hr.leaf_radiosity(i, r, g, b);
            kept = r;
            break;
        }
    TASK_CHECK(kept == new_receiver);
    return 0;
}

static int transport_row_sum_guard_rejects_explicitly() {
    World world;
    // Two large, very close facing faces: geometric form factor exceeds 1.1.
    Triangle *t1 = new Triangle(Point3D(-1, -1, 0.05f),
                                Point3D(-1, 1, 0.05f),
                                Point3D(1, -1, 0.05f));
    Triangle *t2 = new Triangle(Point3D(-1, -1, -0.05f),
                                Point3D(1, -1, -0.05f),
                                Point3D(-1, 1, -0.05f));
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();
    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, 1, 1e-3);
    TASK_CHECK(!hr.ok());
    TASK_CHECK(!hr.error().empty());
    TASK_CHECK(hr.compressed_row_sum_max() > 1.1);
    return 0;
}

static int transport_failed_solve_keeps_previous_solution() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();
    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, 2, 1e-3);
    TASK_CHECK(hr.ok());

    std::vector<double> saved;
    for (std::size_t i = 0; i < hr.leaf_count(); ++i) {
        double r, g, b;
        hr.leaf_radiosity(i, r, g, b);
        saved.push_back(r);
    }
    TASK_CHECK(!hr.solve(0));
    for (std::size_t i = 0; i < hr.leaf_count(); ++i) {
        double r, g, b;
        hr.leaf_radiosity(i, r, g, b);
        TASK_CHECK(r == saved[i]);
    }
    // A positive budget re-solves successfully.
    TASK_CHECK(hr.solve(100000));
    for (std::size_t i = 0; i < hr.leaf_count(); ++i) {
        double r, g, b;
        hr.leaf_radiosity(i, r, g, b);
        TASK_CHECK(std::abs(r - saved[i]) < 1e-10);
    }
    TASK_CHECK(hr.operator_applications() > 0);
    return 0;
}

static int transport_thin_triangle_is_not_missed_by_segment_test() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    // Thin occluder spanning only a few thousandths of a unit at z = 0.
    Triangle *thin = new Triangle(Point3D(0.499f, -0.002f, 0.0f),
                                  Point3D(0.501f, -0.002f, 0.0f),
                                  Point3D(0.5f, 0.002f, 0.0f));
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.add_geometry(thin);
    world.build_accel();

    Point3D O(0.5f, 0.0f, 0.9f);
    Point3D Q(0.5f, 0.0f, -0.9f);
    TASK_CHECK(world.segment_occluded(O, Q, t1, t2));

    // A parallel render ray must also register the thin primitive's identity.
    Ray ray(O, Vector3D(0.0, 0.0, -1.0));
    ShadeInfo si = world.hit_objects(ray);
    TASK_CHECK(si.hit && si.hit_geometry == thin);

    // Without the occluder the same segment is clear.
    World clear;
    Triangle *c1 = facing_tri(1.0f, true);
    Triangle *c2 = facing_tri(-1.0f, false);
    clear.add_geometry(c1);
    clear.add_geometry(c2);
    clear.build_accel();
    TASK_CHECK(!clear.segment_occluded(O, Q, c1, c2));
    return 0;
}

static int transport_single_face_is_pure_emission() {
    World world;
    Triangle *only = facing_tri(1.0f, true);
    world.add_geometry(only);
    world.build_accel();
    HierarchicalRadiosity hr(world, {only},
                             {RGBColor(0.5f)},
                             {RGBColor(1.0f, 0.5f, 0.25f)}, 2, 1e-3);
    TASK_CHECK(hr.ok());
    TASK_CHECK(hr.links().empty());
    TASK_CHECK(hr.visibility_queries() == 0);
    for (std::size_t i = 0; i < hr.leaf_count(); ++i) {
        double r, g, b;
        hr.leaf_radiosity(i, r, g, b);
        TASK_CHECK(std::abs(r - 1.0) < 1e-12);
        TASK_CHECK(std::abs(g - 0.5) < 1e-12);
        TASK_CHECK(std::abs(b - 0.25) < 1e-12);
        TASK_CHECK(hr.leaf_original(i) == 0);
    }
    return 0;
}

static int transport_invalid_inputs_are_rejected() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();

    HierarchicalRadiosity bad_rho(world, {t1, t2},
                                  {RGBColor(0.9f), RGBColor(0.5f)},
                                  {RGBColor(1.0f), RGBColor(0.0f)}, 1, 1e-3);
    TASK_CHECK(!bad_rho.ok() && !bad_rho.error().empty());

    HierarchicalRadiosity bad_em(world, {t1, t2},
                                 {RGBColor(0.5f), RGBColor(0.5f)},
                                 {RGBColor(5.0f), RGBColor(0.0f)}, 1, 1e-3);
    TASK_CHECK(!bad_em.ok());

    HierarchicalRadiosity bad_depth(world, {t1, t2},
                                    {RGBColor(0.5f), RGBColor(0.5f)},
                                    {RGBColor(1.0f), RGBColor(0.0f)}, 5, 1e-3);
    TASK_CHECK(!bad_depth.ok());

    HierarchicalRadiosity bad_eta(world, {t1, t2},
                                  {RGBColor(0.5f), RGBColor(0.5f)},
                                  {RGBColor(1.0f), RGBColor(0.0f)}, 1, 1.0);
    TASK_CHECK(!bad_eta.ok());

    HierarchicalRadiosity duplicate(world, {t1, t1},
                                    {RGBColor(0.5f), RGBColor(0.5f)},
                                    {RGBColor(1.0f), RGBColor(0.0f)}, 1, 1e-3);
    TASK_CHECK(!duplicate.ok());
    return 0;
}

static int transport_leaf_identity_and_bary_domain() {
    World world;
    Triangle *t1 = facing_tri(1.0f, true);
    Triangle *t2 = facing_tri(-1.0f, false);
    world.add_geometry(t1);
    world.add_geometry(t2);
    world.build_accel();
    const int depth = 3;
    HierarchicalRadiosity hr(world, {t1, t2},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(1.0f), RGBColor(0.0f)}, depth, 1e-3);
    TASK_CHECK(hr.ok());

    for (std::size_t leaf = 0; leaf < hr.leaf_count(); ++leaf) {
        double blo, bhi, glo, ghi;
        hr.leaf_bary_domain(leaf, blo, bhi, glo, ghi);
        TASK_CHECK(blo >= 0.0 && bhi <= 1.0 && glo >= 0.0 && ghi <= 1.0);
        // The leaf centroid's barycentric point maps back to this leaf.
        double cb, cg;
        hr.leaf_centroid_bary(leaf, cb, cg);
        int mapped = hr.leaf_for_hit(hr.leaf_triangle(leaf), cb, cg);
        TASK_CHECK(mapped == (int)leaf);
    }

    // Root domain covers the whole original triangle.
    double blo, bhi, glo, ghi;
    hr.leaf_bary_domain(0, blo, bhi, glo, ghi);
    TASK_CHECK(blo == 0.0 && glo == 0.0);

    // A point on a sub-face edge is assigned to the deterministically
    // smaller child index; both neighbors return a valid leaf.
    int edge_leaf = hr.leaf_for_hit(t1, 0.5, 0.0);
    TASK_CHECK(edge_leaf >= 0 && hr.leaf_original(edge_leaf) == 0);
    TASK_CHECK(hr.leaf_for_hit(t2, 0.25, 0.25) >= 0);
    TASK_CHECK(hr.leaf_for_hit(t1, 1.0, 0.0) >= 0);

    // Unknown geometry cannot resolve to a leaf.
    Triangle foreign(Point3D(5, 5, 5), Point3D(6, 5, 5), Point3D(5, 6, 5));
    TASK_CHECK(hr.leaf_for_hit(&foreign, 0.1, 0.1) < 0);
    return 0;
}

// ------------------------------ render group -------------------------------

static World *make_render_world(Triangle *&emitter, Triangle *&receiver,
                                Sphere *&blocker,
                                HierarchicalDiffuse *&mat_emitter,
                                HierarchicalDiffuse *&mat_receiver,
                                Matte *&mat_blocker) {
    World *world = new World();
    emitter = facing_tri(1.0f, true);
    receiver = facing_tri(-1.0f, false);
    blocker = new Sphere(Point3D(0.7f, 0.0f, 0.0f), 0.2f);
    world->add_geometry(emitter);
    world->add_geometry(receiver);
    world->add_geometry(blocker);
    mat_emitter = new HierarchicalDiffuse();
    mat_receiver = new HierarchicalDiffuse();
    mat_blocker = new Matte(0.0f, 0.0f, RGBColor(0.0f));
    emitter->set_material(mat_emitter);
    receiver->set_material(mat_receiver);
    blocker->set_material(mat_blocker);
    world->build_accel();
    return world;
}

static int render_front_face_returns_b_over_pi_and_back_is_black() {
    Triangle *emitter, *receiver;
    Sphere *blocker;
    HierarchicalDiffuse *me, *mr;
    Matte *mb;
    World *world = make_render_world(emitter, receiver, blocker, me, mr, mb);

    HierarchicalRadiosity hr(*world, {emitter, receiver},
                             {RGBColor(0.5f), RGBColor(0.5f)},
                             {RGBColor(4.0f), RGBColor(0.0f)}, 2, 1e-3);
    TASK_CHECK(hr.ok());
    me->set_solution(&hr);
    mr->set_solution(&hr);

    // Front-face ray from between the two faces toward the emitter's
    // downward-facing normal side.
    Ray front(Point3D(0.0f, 0.0f, 0.5f), Vector3D(0.0, 0.0, 1.0));
    ShadeInfo sf = world->hit_objects(front);
    TASK_CHECK(sf.hit && sf.front_face);
    TASK_CHECK(sf.hit_geometry == emitter);
    RGBColor front_color = sf.material_ptr->shade(sf);
    int leaf = hr.leaf_for_hit(emitter, sf.bary_u, sf.bary_v);
    TASK_CHECK(leaf >= 0);
    double br, bg, bb;
    hr.leaf_radiosity(leaf, br, bg, bb);
    TASK_CHECK(std::abs(front_color.r - br / PI) < 1e-5);
    // Energy above 1 is preserved, not clamped into [0, 1].
    TASK_CHECK(front_color.r > 1.0f);

    // Back-face ray: geometry is double sided for visibility, but the
    // hierarchical material shades the back as black.
    Ray back(Point3D(0.0f, 0.0f, -3.0f), Vector3D(0.0, 0.0, 1.0));
    ShadeInfo sb = world->hit_objects(back);
    TASK_CHECK(sb.hit && !sb.front_face);
    RGBColor back_color = sb.material_ptr->shade(sb);
    TASK_CHECK(back_color.r == 0.0f && back_color.g == 0.0f &&
               back_color.b == 0.0f);

    delete world;
    delete me;
    delete mr;
    delete mb;
    return 0;
}

static int render_two_frames_show_indirect_change() {
    Triangle *emitter, *receiver;
    Sphere *blocker;
    HierarchicalDiffuse *me, *mr;
    Matte *mb;
    World *world = make_render_world(emitter, receiver, blocker, me, mr, mb);

    HierarchicalRadiosity hr(*world, {emitter, receiver},
                             {RGBColor(0.6f, 0.5f, 0.4f),
                              RGBColor(0.4f, 0.6f, 0.7f)},
                             {RGBColor(1.0f, 0.2f, 0.1f), RGBColor(0.0f)},
                             2, 1e-3);
    TASK_CHECK(hr.ok());
    me->set_solution(&hr);
    mr->set_solution(&hr);

    world->set_camera(new Perspective(0.0f, 0.0f, 3.0f));
    world->vplane.top_left = Point3D(-1, 1, 0);
    world->vplane.bottom_right = Point3D(1, -1, 0);
    world->vplane.hres = 64;
    world->vplane.vres = 32;
    world->sampler_ptr = new Simple(world->camera_ptr, &world->vplane);
    world->tracer_ptr = new Whitted(world);
    // No lights are added: HierarchicalDiffuse must ignore ambient/point lights.
    world->ambient = RGBColor(0.7f, 0.0f, 0.0f);

    auto frame_energy = [&]() {
        double total = 0.0;
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 64; ++x) {
                for (const Ray &ray : world->sampler_ptr->get_rays(x, y)) {
                    RGBColor c = world->tracer_ptr->trace_ray(ray, 0);
                    total += ray.w * (c.r + c.g + c.b);
                }
            }
        }
        return total;
    };

    double first = frame_energy();
    TASK_CHECK(first > 0.0);
    TASK_CHECK(hr.set_emission(0, RGBColor(3.5f, 2.0f, 1.0f)));
    double second = frame_energy();
    TASK_CHECK(second > first);

    // Ambient changes must not influence the hierarchical material.
    world->ambient = RGBColor(0.0f);
    double third = frame_energy();
    TASK_CHECK(std::abs(third - second) < 1e-6);

    delete world;
    delete me;
    delete mr;
    delete mb;
    return 0;
}

static int render_legacy_matte_and_geometry_regression() {
    World world;
    Matte matte(0.4f, 0.6f, RGBColor(0.8f, 0.2f, 0.1f));
    Sphere *sphere = new Sphere(Point3D(0, 0, -3), 1.5f);
    sphere->set_material(&matte);
    Plane *plane = new Plane(Point3D(0, -1.5f, 0), Vector3D(0, 1, 0));
    plane->set_material(&matte);
    Triangle *tri = new Triangle(Point3D(-2, -2, -6),
                                 Point3D(2, -2, -6), Point3D(0, 2, -6));
    tri->set_material(&matte);
    world.add_geometry(sphere);
    world.add_geometry(plane);
    world.add_geometry(tri);
    world.ambient = RGBColor(0.5f);
    world.build_accel();

    Ray ray(Point3D(0, 0, 1), Vector3D(0, 0, -1));
    ShadeInfo si = world.hit_objects(ray);
    TASK_CHECK(si.hit);
    TASK_CHECK(si.hit_geometry == sphere);
    TASK_CHECK(si.front_face);
    RGBColor legacy = si.material_ptr->shade(si);
    // The legacy Matte response (ambient 0.5 * ka 0.4 * cd 0.8 = 0.16) holds.
    TASK_CHECK(std::abs(legacy.r - 0.16f) < 1e-6);
    TASK_CHECK(std::abs(legacy.g - 0.04f) < 1e-6);
    TASK_CHECK(std::abs(legacy.b - 0.02f) < 1e-6);

    // Double-sided visibility: a back hit still reports the primitive
    // identity. The triangle's winding gives a +z normal, so a ray from
    // behind (z < -6) traveling +z strikes its back face. The ray is offset
    // to miss the sphere and pass through the triangle's interior.
    Ray back(Point3D(1.2f, -1.5f, -7.0f), Vector3D(0, 0, 1));
    ShadeInfo sb = world.hit_objects(back);
    TASK_CHECK(sb.hit && sb.hit_geometry == tri && !sb.front_face);
    TASK_CHECK(sb.bary_u >= 0.0 && sb.bary_v >= 0.0 &&
               sb.bary_u + sb.bary_v <= 1.0);
    return 0;
}

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
    {"links", "unique_ordered_links", links_are_unique_and_ordered},
    {"links", "kernel_interval_contains_descendants",
     links_kernel_interval_contains_all_descendants},
    {"links", "far_field_compression", links_far_field_compression},
    {"links", "exact_queries_track_leaf_pairs",
     links_exact_queries_track_leaf_pairs},
    {"links", "different_areas_and_reordered_identity",
     links_different_areas_and_reordered_identity},

    {"transport", "matches_dense_solution", transport_matches_dense_solution},
    {"transport", "far_field_tight_eta_matches_dense",
     transport_far_field_eta_tight_matches_dense},
    {"transport", "area_reciprocity", transport_area_reciprocity},
    {"transport", "row_sum_guard_rejects", transport_row_sum_guard_rejects_explicitly},
    {"transport", "single_face_pure_emission", transport_single_face_is_pure_emission},
    {"transport", "thin_triangle_segment_hit", transport_thin_triangle_is_not_missed_by_segment_test},
    {"transport", "invalid_inputs_rejected", transport_invalid_inputs_are_rejected},
    {"transport", "local_shadow", transport_local_shadow_from_intermediate_blocker},
    {"transport", "behind_segment_not_shadowing",
     transport_geometry_behind_segment_does_not_shadow},
    {"transport", "emission_update_no_new_queries",
     transport_emission_update_reuses_tree_without_queries},
    {"transport", "failed_solve_keeps_old_solution",
     transport_failed_solve_keeps_previous_solution},
    {"transport", "leaf_identity_bary_domain",
     transport_leaf_identity_and_bary_domain},

    {"render", "front_b_over_pi_back_black",
     render_front_face_returns_b_over_pi_and_back_is_black},
    {"render", "two_frames_indirect_change",
     render_two_frames_show_indirect_change},
    {"render", "legacy_matte_regression",
     render_legacy_matte_and_geometry_regression},
};
const std::size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
