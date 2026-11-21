#include "HierarchicalRadiosity.hpp"

#include "../acceleration/BVH.hpp"
#include "../geometry/Geometry.hpp"
#include "../geometry/Triangle.hpp"
#include "../utilities/BBox.hpp"
#include "../utilities/Vector3D.hpp"
#include "../world/World.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

const double kPi = 3.14159265358979323846;
const double kSegmentEps = 1e-8;
const double kInfinite = 1e300;

struct DVec {
    double x, y, z;
};

DVec as_dvec(const Point3D &p) { return {p.x, p.y, p.z}; }
DVec operator-(const DVec &a, const DVec &b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
DVec operator+(const DVec &a, const DVec &b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
DVec operator*(const DVec &a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}
DVec operator-(const DVec &a) {
    return {-a.x, -a.y, -a.z};
}
double dot(const DVec &a, const DVec &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double norm2(const DVec &a) { return dot(a, a); }
DVec normalized(const DVec &a) {
    double l = std::sqrt(norm2(a));
    if (l <= 0.0) return {0.0, 0.0, 0.0};
    return a * (1.0 / l);
}

} // namespace

struct HierarchicalRadiosity::Node {
    int original;
    int level;
    int parent;
    int child[4];
    Point3D corner[3];
    double corner_bary[3][2]; // (beta, gamma) weights of v1, v2 per corner.
    Point3D centroid;
    DVec normal;
    DVec hull[3];   // convex hull of every descendant leaf centroid.
    BBox box;       // bounds of the node triangle.
    double area;
    int leaf_index; // -1 while internal, global leaf index once at depth D.
};

struct HierarchicalRadiosity::Work {
    int a;
    int b;
};

HierarchicalRadiosity::HierarchicalRadiosity(World &w,
                                             const std::vector<Triangle *> &tris_,
                                             const std::vector<RGBColor> &reflectance,
                                             const std::vector<RGBColor> &emission_,
                                             int depth_, double eta_)
    : world(w), tris(tris_), depth(depth_), eta(eta_), built(false),
      vis_queries(0), peak_bytes(0), op_count(0),
      saved_node_tests(0), saved_prim_tests(0), row_sum_max(0.0),
      working_bytes(0) {
    if (!validate_inputs(reflectance, emission_)) return;

    const std::size_t n = tris.size();
    rho.resize(n);
    emission.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        rho[i] = {reflectance[i].r, reflectance[i].g, reflectance[i].b};
        emission[i] = {emission_[i].r, emission_[i].g, emission_[i].b};
    }

    build_trees();

    world.reset_query_stats();
    refine_links();
    saved_node_tests = world.node_tests();
    saved_prim_tests = world.primitive_tests();

    const std::size_t leaves = leaf_count();
    radiosity.assign(leaves, {0.0, 0.0, 0.0});

    // Compressed operator row-sum guard on B = 1; no row normalization.
    std::vector<std::array<double, 3>> ones(leaves, {1.0, 1.0, 1.0});
    std::vector<std::array<double, 3>> gathered;
    apply_operator(ones, gathered);
    double row_max = 0.0;
    for (const auto &v : gathered)
        row_max = std::max(row_max, std::max({v[0], v[1], v[2]}));
    row_sum_max = row_max;
    if (row_max > 1.1) {
        failure = "compressed operator row sum " + std::to_string(row_max) +
                  " exceeds 1.1";
        return;
    }

    if (!run_solver(100000)) {
        failure = "solver exhausted its iteration budget";
        return;
    }
    built = true;
}

HierarchicalRadiosity::~HierarchicalRadiosity() = default;

bool HierarchicalRadiosity::validate_inputs(
        const std::vector<RGBColor> &reflectance,
        const std::vector<RGBColor> &emission_) {
    if (tris.empty() || tris.size() > 32) {
        failure = "between 1 and 32 triangles are required";
        return false;
    }
    for (std::size_t i = 0; i < tris.size(); ++i) {
        if (tris[i] == nullptr) {
            failure = "triangle pointer is null";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (tris[j] == tris[i]) {
                failure = "triangle pointers must be distinct";
                return false;
            }
        }
    }
    if (reflectance.size() != tris.size() || emission_.size() != tris.size()) {
        failure = "one reflectance/emission entry per triangle is required";
        return false;
    }
    if (depth < 0 || depth > 4) {
        failure = "subdivision depth must be in [0, 4]";
        return false;
    }
    if (!(eta >= 1e-4 && eta <= 0.1)) {
        failure = "link tolerance eta must be in [1e-4, 0.1]";
        return false;
    }
    for (std::size_t i = 0; i < tris.size(); ++i) {
        const float rc[3] = {reflectance[i].r, reflectance[i].g, reflectance[i].b};
        const float ec[3] = {emission_[i].r, emission_[i].g, emission_[i].b};
        for (int c = 0; c < 3; ++c) {
            if (!(rc[c] >= 0.0f && rc[c] <= 0.8f)) {
                failure = "reflectance channels must be in [0, 0.8]";
                return false;
            }
            if (!(ec[c] >= 0.0f && ec[c] <= 4.0f)) {
                failure = "emission channels must be in [0, 4]";
                return false;
            }
        }
    }
    return true;
}

void HierarchicalRadiosity::build_trees() {
    const int n = (int)tris.size();
    roots.assign(n, -1);
    leaf_start.assign(n, 0);
    int leaf_counter = 0;

    for (int o = 0; o < n; ++o) {
        leaf_start[o] = leaf_counter;
        Triangle *tri = tris[o];
        Point3D p[3] = {tri->vertex(0), tri->vertex(1), tri->vertex(2)};

        DVec e1 = as_dvec(p[1]) - as_dvec(p[0]);
        DVec e2 = as_dvec(p[2]) - as_dvec(p[0]);
        DVec normal = normalized(DVec{e1.y * e2.z - e1.z * e2.y,
                                      e1.z * e2.x - e1.x * e2.z,
                                      e1.x * e2.y - e1.y * e2.x});
        double root_area = 0.5 * std::sqrt(norm2(DVec{e1.y * e2.z - e1.z * e2.y,
                                                       e1.z * e2.x - e1.x * e2.z,
                                                       e1.x * e2.y - e1.y * e2.x}));

        double root_bary[3][2] = {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};

        // Recursive subdivision, depth first, so parents always precede
        // their children in the node array.
        struct Frame {
            int parent;
            int slot;
            Point3D p[3];
            double bary[3][2];
            int level;
        };
        std::vector<Frame> stack;
        stack.push_back({-1, -1, {p[0], p[1], p[2]},
                         {{root_bary[0][0], root_bary[0][1]},
                          {root_bary[1][0], root_bary[1][1]},
                          {root_bary[2][0], root_bary[2][1]}}, 0});

        while (!stack.empty()) {
            Frame f = stack.back();
            stack.pop_back();

            Node node;
            node.original = o;
            node.level = f.level;
            node.parent = f.parent;
            for (int k = 0; k < 3; ++k) {
                node.corner[k] = f.p[k];
                node.corner_bary[k][0] = f.bary[k][0];
                node.corner_bary[k][1] = f.bary[k][1];
            }
            node.area = root_area / std::pow(4.0, f.level);
            node.normal = normal;
            node.leaf_index = -1;
            for (int k = 0; k < 4; ++k) node.child[k] = -1;

            DVec c = (as_dvec(f.p[0]) + as_dvec(f.p[1]) + as_dvec(f.p[2])) * (1.0 / 3.0);
            node.centroid = Point3D((float)c.x, (float)c.y, (float)c.z);
            // Every descendant leaf centroid is a convex combination of this
            // node's corners, so the node triangle is a valid enclosing hull.
            for (int k = 0; k < 3; ++k) node.hull[k] = as_dvec(f.p[k]);

            BBox b;
            b.pmin = Point3D(std::min({f.p[0].x, f.p[1].x, f.p[2].x}),
                             std::min({f.p[0].y, f.p[1].y, f.p[2].y}),
                             std::min({f.p[0].z, f.p[1].z, f.p[2].z}));
            b.pmax = Point3D(std::max({f.p[0].x, f.p[1].x, f.p[2].x}),
                             std::max({f.p[0].y, f.p[1].y, f.p[2].y}),
                             std::max({f.p[0].z, f.p[1].z, f.p[2].z}));
            node.box = b;

            int index = (int)nodes.size();
            nodes.push_back(node);
            if (f.parent >= 0) nodes[f.parent].child[f.slot] = index;
            if (f.level == 0) roots[o] = index;

            if (f.level < depth) {
                Point3D m01 = Point3D((f.p[0].x + f.p[1].x) * 0.5f,
                                      (f.p[0].y + f.p[1].y) * 0.5f,
                                      (f.p[0].z + f.p[1].z) * 0.5f);
                Point3D m12 = Point3D((f.p[1].x + f.p[2].x) * 0.5f,
                                      (f.p[1].y + f.p[2].y) * 0.5f,
                                      (f.p[1].z + f.p[2].z) * 0.5f);
                Point3D m20 = Point3D((f.p[2].x + f.p[0].x) * 0.5f,
                                      (f.p[2].y + f.p[0].y) * 0.5f,
                                      (f.p[2].z + f.p[0].z) * 0.5f);
                auto mid_bary = [](const double a[2], const double b[2]) {
                    return std::pair<double, double>{(a[0] + b[0]) * 0.5,
                                                     (a[1] + b[1]) * 0.5};
                };
                auto b01 = mid_bary(f.bary[0], f.bary[1]);
                auto b12 = mid_bary(f.bary[1], f.bary[2]);
                auto b20 = mid_bary(f.bary[2], f.bary[0]);

                Point3D cp[4][3] = {
                    {f.p[0], m01, m20},
                    {m01, f.p[1], m12},
                    {m20, m12, f.p[2]},
                    {m01, m12, m20}};
                double cb[4][3][2] = {
                    {{f.bary[0][0], f.bary[0][1]}, {b01.first, b01.second}, {b20.first, b20.second}},
                    {{b01.first, b01.second}, {f.bary[1][0], f.bary[1][1]}, {b12.first, b12.second}},
                    {{b20.first, b20.second}, {b12.first, b12.second}, {f.bary[2][0], f.bary[2][1]}},
                    {{b01.first, b01.second}, {b12.first, b12.second}, {b20.first, b20.second}}};

                // Push in reverse so child 0 is processed first; leaf numbering
                // then follows the deterministic child order.
                for (int k = 3; k >= 0; --k) {
                    Frame child;
                    child.parent = index;
                    child.slot = k;
                    for (int t = 0; t < 3; ++t) {
                        child.p[t] = cp[k][t];
                        child.bary[t][0] = cb[k][t][0];
                        child.bary[t][1] = cb[k][t][1];
                    }
                    child.level = f.level + 1;
                    stack.push_back(child);
                }
            } else {
                nodes[index].leaf_index = leaf_counter++;
            }
        }
    }
}

std::size_t HierarchicalRadiosity::node_count() const { return nodes.size(); }

int HierarchicalRadiosity::node_level(int node) const {
    return (node >= 0 && node < (int)nodes.size()) ? nodes[node].level : -1;
}

bool HierarchicalRadiosity::node_is_leaf(int node) const {
    return node >= 0 && node < (int)nodes.size() &&
           nodes[node].leaf_index >= 0;
}

int HierarchicalRadiosity::node_original(int node) const {
    return (node >= 0 && node < (int)nodes.size()) ? nodes[node].original : -1;
}

Point3D HierarchicalRadiosity::node_centroid(int node) const {
    return (node >= 0 && node < (int)nodes.size())
               ? nodes[node].centroid
               : Point3D();
}

std::vector<std::size_t> HierarchicalRadiosity::descendant_leaves(int node) const {
    std::vector<std::size_t> out;
    if (node < 0 || node >= (int)nodes.size()) return out;
    std::vector<int> stack{node};
    while (!stack.empty()) {
        int idx = stack.back();
        stack.pop_back();
        const Node &n = nodes[idx];
        if (n.leaf_index >= 0) {
            out.push_back((std::size_t)n.leaf_index);
        } else {
            for (int c = 0; c < 4; ++c)
                if (n.child[c] >= 0) stack.push_back(n.child[c]);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

int HierarchicalRadiosity::leaf_node(std::size_t leaf) const {
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].leaf_index == (int)leaf) return (int)i;
    return -1;
}

std::size_t HierarchicalRadiosity::leaf_count() const {
    return (std::size_t)tris.size() * (std::size_t)std::pow(4.0, depth);
}

std::size_t HierarchicalRadiosity::original_count() const { return tris.size(); }

double HierarchicalRadiosity::centroid_kernel(const Node &a, const Node &b) const {
    DVec d = as_dvec(b.centroid) - as_dvec(a.centroid);
    double r2 = norm2(d);
    if (r2 <= 0.0) return 0.0;
    DVec u = normalized(d);
    double ci = std::max(0.0, dot(a.normal, u));
    double cj = std::max(0.0, dot(b.normal, -u));
    return ci * cj / (kPi * r2);
}

bool HierarchicalRadiosity::kernel_interval(const Node &a, const Node &b,
                                            double &lo, double &hi) const {
    // Every descendant leaf centroid lies in the node's hull triangle.
    DVec ca = as_dvec(a.centroid);
    DVec cb = as_dvec(b.centroid);
    DVec axis = cb - ca;
    double L = std::sqrt(norm2(axis));
    double ra = 0.0;
    double rb = 0.0;
    for (int k = 0; k < 3; ++k) {
        ra = std::max(ra, std::sqrt(norm2(a.hull[k] - ca)));
        rb = std::max(rb, std::sqrt(norm2(b.hull[k] - cb)));
    }

    // Component ranges of y - x over the two hull triangles.
    double dmin[3] = {0.0, 0.0, 0.0};
    double dmax[3] = {0.0, 0.0, 0.0};
    for (int axis_idx = 0; axis_idx < 3; ++axis_idx) {
        double amin = std::numeric_limits<double>::infinity();
        double amax = -std::numeric_limits<double>::infinity();
        double bmin = std::numeric_limits<double>::infinity();
        double bmax = -std::numeric_limits<double>::infinity();
        for (int k = 0; k < 3; ++k) {
            double av = axis_idx == 0 ? a.hull[k].x : (axis_idx == 1 ? a.hull[k].y : a.hull[k].z);
            double bv = axis_idx == 0 ? b.hull[k].x : (axis_idx == 1 ? b.hull[k].y : b.hull[k].z);
            amin = std::min(amin, av);
            amax = std::max(amax, av);
            bmin = std::min(bmin, bv);
            bmax = std::max(bmax, bv);
        }
        dmin[axis_idx] = bmin - amax;
        dmax[axis_idx] = bmax - amin;
    }

    double r2_lo = 0.0;
    double r2_hi = 0.0;
    for (int k = 0; k < 3; ++k) {
        double nearest = 0.0;
        if (dmin[k] > 0.0) nearest = dmin[k];
        else if (dmax[k] < 0.0) nearest = dmax[k];
        r2_lo += nearest * nearest;
        double edge = std::max(std::abs(dmin[k]), std::abs(dmax[k]));
        r2_hi += edge * edge;
    }

    // Conservative cones around the centroid-to-centroid axis.
    double cos_lo_i = 0.0, cos_hi_i = 1.0;
    double cos_lo_j = 0.0, cos_hi_j = 1.0;
    if (L > ra + rb && L > 0.0) {
        double sin_theta = (ra + rb) / (L - ra - rb);
        sin_theta = std::min(1.0, std::max(0.0, sin_theta));
        double theta = std::asin(sin_theta);
        DVec u = axis * (1.0 / L);
        double ang_i = std::acos(std::min(1.0, std::max(-1.0, dot(a.normal, u))));
        double ang_j = std::acos(std::min(1.0, std::max(-1.0, dot(b.normal, -u))));
        cos_lo_i = std::max(0.0, std::cos(ang_i + theta));
        cos_hi_i = std::cos(std::max(0.0, ang_i - theta));
        cos_lo_j = std::max(0.0, std::cos(ang_j + theta));
        cos_hi_j = std::cos(std::max(0.0, ang_j - theta));
    }

    lo = cos_lo_i * cos_lo_j / (kPi * r2_hi);
    if (r2_lo <= 0.0) {
        hi = kInfinite; // singular geometry: force refinement to the leaves.
    } else {
        hi = cos_hi_i * cos_hi_j / (kPi * r2_lo);
    }
    hi = std::max(hi, lo);
    return true;
}

bool HierarchicalRadiosity::visible(const Node &a, const Node &b) {
    ++vis_queries;
    return !world.segment_occluded(a.centroid, b.centroid,
                                   tris[a.original], tris[b.original],
                                   kSegmentEps, 1.0 - kSegmentEps);
}

void HierarchicalRadiosity::account_storage(std::size_t stack_size) {
    std::size_t bytes = 0;
    bytes += nodes.capacity() * sizeof(Node);
    bytes += link_store.capacity() * sizeof(RadiosityLink);
    bytes += rho.capacity() * sizeof(std::array<double, 3>);
    bytes += emission.capacity() * sizeof(std::array<double, 3>);
    bytes += radiosity.capacity() * sizeof(std::array<double, 3>);
    bytes += roots.capacity() * sizeof(int);
    bytes += leaf_start.capacity() * sizeof(int);
    bytes += tris.capacity() * sizeof(Triangle *);
    bytes += stack_size * sizeof(Work);
    bytes += working_bytes;
    peak_bytes = std::max(peak_bytes, bytes);
}

void HierarchicalRadiosity::refine_links() {
    std::vector<Work> stack;
    const int n = (int)tris.size();
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            stack.push_back({roots[i], roots[j]});

    while (!stack.empty()) {
        Work w = stack.back();
        stack.pop_back();
        const Node &a = nodes[w.a];
        const Node &b = nodes[w.b];

        BBox bundle = a.box;
        bundle.extend(b.box);

        bool blocker_possible = world.bundle_may_occlude(
            bundle, tris[a.original], tris[b.original]);

        double lo = 0.0;
        double hi = 0.0;
        kernel_interval(a, b, lo, hi);

        if (!blocker_possible) {
            if (hi <= 0.0) continue; // kernel vanishes for every descendant.
            if (hi - lo <= eta * hi) {
                RadiosityLink link;
                link.node_a = w.a;
                link.node_b = w.b;
                link.kernel = centroid_kernel(a, b);
                link.kernel_lo = lo;
                link.kernel_hi = hi;
                link.exact = false;
                link_store.push_back(link);
                account_storage(stack.size());
                continue;
            }
        }

        const bool a_split = a.level < depth;
        const bool b_split = b.level < depth;
        if (!a_split && !b_split) {
            double k = 0.0;
            if (visible(a, b)) k = centroid_kernel(a, b);
            RadiosityLink link;
            link.node_a = w.a;
            link.node_b = w.b;
            link.kernel = k;
            link.kernel_lo = k;
            link.kernel_hi = k;
            link.exact = true;
            link_store.push_back(link);
            account_storage(stack.size());
            continue;
        }

        // Split the larger splittable end; ties go to the smaller original id.
        bool split_a;
        if (a_split && b_split) {
            if (a.area == b.area) split_a = a.original < b.original;
            else split_a = a.area > b.area;
        } else {
            split_a = a_split;
        }

        if (split_a) {
            for (int c = 0; c < 4; ++c)
                stack.push_back({a.child[c], w.b});
        } else {
            for (int c = 0; c < 4; ++c)
                stack.push_back({w.a, b.child[c]});
        }
        account_storage(stack.size());
    }
    account_storage(0);
}

bool HierarchicalRadiosity::apply_operator(
        const std::vector<std::array<double, 3>> &B,
        std::vector<std::array<double, 3>> &I) {
    ++op_count;
    const std::size_t n_nodes = nodes.size();
    std::vector<std::array<double, 3>> gather(n_nodes, {0.0, 0.0, 0.0});
    std::vector<std::array<double, 3>> scatter(n_nodes, {0.0, 0.0, 0.0});
    std::size_t saved_working = working_bytes;
    working_bytes += (gather.capacity() + scatter.capacity()) *
                     sizeof(std::array<double, 3>);
    account_storage(0);

    // Push phase input: leaf power A_i B_i.
    for (const Node &node : nodes) {
        if (node.leaf_index >= 0) {
            const auto &bv = B[node.leaf_index];
            gather[&node - &nodes[0]] = {node.area * bv[0],
                                         node.area * bv[1],
                                         node.area * bv[2]};
        }
    }

    // Pull: children precede parents in reverse index order.
    for (std::size_t idx = n_nodes; idx-- > 0;) {
        int parent = nodes[idx].parent;
        if (parent >= 0)
            for (int c = 0; c < 3; ++c)
                gather[parent][c] += gather[idx][c];
    }

    // Links are shared: one record drives both transport directions.
    for (const RadiosityLink &link : link_store) {
        const auto &gb = gather[link.node_b];
        const auto &ga = gather[link.node_a];
        for (int c = 0; c < 3; ++c) {
            scatter[link.node_a][c] += link.kernel * gb[c];
            scatter[link.node_b][c] += link.kernel * ga[c];
        }
    }

    // Push: parents precede children in index order.
    for (std::size_t idx = 0; idx < n_nodes; ++idx) {
        const Node &node = nodes[idx];
        if (node.parent >= 0)
            for (int c = 0; c < 3; ++c)
                scatter[idx][c] += scatter[node.parent][c];
    }

    I.resize(leaf_count());
    for (const Node &node : nodes) {
        if (node.leaf_index >= 0) {
            std::size_t idx = &node - &nodes[0];
            // Pushed link terms already are incident irradiance
            // k * sum(A_j B_j); no per-leaf area division is applied.
            I[node.leaf_index] = scatter[idx];
        }
    }
    working_bytes = saved_working;
    account_storage(0);
    return true;
}

bool HierarchicalRadiosity::run_solver(unsigned max_iterations) {
    if (max_iterations == 0) return false;

    const std::size_t leaves = leaf_count();
    std::vector<std::array<double, 3>> current(leaves, {0.0, 0.0, 0.0});
    std::vector<std::array<double, 3>> per_leaf_e(leaves, {0.0, 0.0, 0.0});
    std::vector<std::array<double, 3>> per_leaf_rho(leaves, {0.0, 0.0, 0.0});
    double e_inf = 0.0;
    for (std::size_t o = 0; o < tris.size(); ++o) {
        for (int l = leaf_start[o];
             l < (o + 1 < tris.size() ? leaf_start[o + 1] : (int)leaves); ++l) {
            per_leaf_e[l] = emission[o];
            per_leaf_rho[l] = rho[o];
            for (int c = 0; c < 3; ++c)
                e_inf = std::max(e_inf, emission[o][c]);
        }
    }
    current = per_leaf_e;
    const double tol = 1e-9 * std::max(1.0, e_inf);

    std::vector<std::array<double, 3>> incident;
    std::vector<std::array<double, 3>> next(leaves);
    working_bytes = (per_leaf_e.capacity() + per_leaf_rho.capacity() +
                     current.capacity() + incident.capacity() +
                     next.capacity()) * sizeof(std::array<double, 3>);
    account_storage(0);

    bool converged = false;
    for (unsigned iter = 0; iter < max_iterations; ++iter) {
        apply_operator(current, incident);
        double residual = 0.0;
        for (std::size_t i = 0; i < leaves; ++i) {
            for (int c = 0; c < 3; ++c) {
                next[i][c] = per_leaf_e[i][c] + per_leaf_rho[i][c] * incident[i][c];
                residual = std::max(residual, std::abs(next[i][c] - current[i][c]));
            }
        }
        current.swap(next);
        if (residual <= tol) {
            converged = true;
            break;
        }
    }
    working_bytes = 0;
    account_storage(0);
    if (!converged) return false;
    radiosity.swap(current);
    return true;
}

bool HierarchicalRadiosity::solve(unsigned max_iterations) {
    if (!built && radiosity.empty()) return false;
    std::vector<std::array<double, 3>> previous = radiosity;
    if (!run_solver(max_iterations)) {
        radiosity.swap(previous);
        return false;
    }
    return true;
}

bool HierarchicalRadiosity::set_emission(int original_index,
                                         const RGBColor &new_emission) {
    if (original_index < 0 ||
        original_index >= (int)tris.size())
        return false;
    const float ec[3] = {new_emission.r, new_emission.g, new_emission.b};
    for (int c = 0; c < 3; ++c)
        if (!(ec[c] >= 0.0f && ec[c] <= 4.0f)) return false;

    auto previous_solution = radiosity;
    auto previous_emission = emission;
    emission[original_index] = {ec[0], ec[1], ec[2]};

    const unsigned long long queries_before = vis_queries;
    const unsigned long long nodes_before = saved_node_tests;
    const unsigned long long prims_before = saved_prim_tests;
    if (!run_solver(100000)) {
        radiosity.swap(previous_solution);
        emission.swap(previous_emission);
        return false;
    }
    // Tree and links are reused: no visibility work may happen here.
    if (vis_queries != queries_before ||
        saved_node_tests != nodes_before ||
        saved_prim_tests != prims_before) {
        radiosity.swap(previous_solution);
        emission.swap(previous_emission);
        return false;
    }
    return true;
}

bool HierarchicalRadiosity::ok() const { return built; }
const std::string &HierarchicalRadiosity::error() const { return failure; }

double HierarchicalRadiosity::leaf_area(std::size_t leaf) const {
    for (const Node &n : nodes)
        if (n.leaf_index == (int)leaf) return n.area;
    return 0.0;
}

int HierarchicalRadiosity::leaf_original(std::size_t leaf) const {
    for (const Node &n : nodes)
        if (n.leaf_index == (int)leaf) return n.original;
    return -1;
}

const Triangle *HierarchicalRadiosity::leaf_triangle(std::size_t leaf) const {
    int o = leaf_original(leaf);
    return o >= 0 ? tris[o] : nullptr;
}

void HierarchicalRadiosity::leaf_bary_domain(
        std::size_t leaf, double &beta_lo, double &beta_hi,
        double &gamma_lo, double &gamma_hi) const {
    beta_lo = gamma_lo = std::numeric_limits<double>::infinity();
    beta_hi = gamma_hi = -std::numeric_limits<double>::infinity();
    for (const Node &n : nodes) {
        if (n.leaf_index != (int)leaf) continue;
        for (int k = 0; k < 3; ++k) {
            beta_lo = std::min(beta_lo, n.corner_bary[k][0]);
            beta_hi = std::max(beta_hi, n.corner_bary[k][0]);
            gamma_lo = std::min(gamma_lo, n.corner_bary[k][1]);
            gamma_hi = std::max(gamma_hi, n.corner_bary[k][1]);
        }
        return;
    }
}

Point3D HierarchicalRadiosity::leaf_centroid(std::size_t leaf) const {
    for (const Node &n : nodes)
        if (n.leaf_index == (int)leaf) return n.centroid;
    return Point3D();
}

void HierarchicalRadiosity::leaf_centroid_bary(
        std::size_t leaf, double &beta, double &gamma) const {
    for (const Node &n : nodes) {
        if (n.leaf_index != (int)leaf) continue;
        beta = (n.corner_bary[0][0] + n.corner_bary[1][0] +
                n.corner_bary[2][0]) / 3.0;
        gamma = (n.corner_bary[0][1] + n.corner_bary[1][1] +
                 n.corner_bary[2][1]) / 3.0;
        return;
    }
    beta = gamma = 0.0;
}

void HierarchicalRadiosity::leaf_radiosity(
        std::size_t leaf, double &r, double &g, double &b) const {
    if (leaf < radiosity.size()) {
        r = radiosity[leaf][0];
        g = radiosity[leaf][1];
        b = radiosity[leaf][2];
    } else {
        r = g = b = 0.0;
    }
}

void HierarchicalRadiosity::leaf_emission(
        std::size_t leaf, double &r, double &g, double &b) const {
    int o = leaf_original(leaf);
    if (o >= 0) {
        r = emission[o][0];
        g = emission[o][1];
        b = emission[o][2];
    } else {
        r = g = b = 0.0;
    }
}

void HierarchicalRadiosity::leaf_reflectance(
        std::size_t leaf, double &r, double &g, double &b) const {
    int o = leaf_original(leaf);
    if (o >= 0) {
        r = rho[o][0];
        g = rho[o][1];
        b = rho[o][2];
    } else {
        r = g = b = 0.0;
    }
}

int HierarchicalRadiosity::leaf_for_hit(const Triangle *tri,
                                        double beta, double gamma) const {
    int o = -1;
    for (std::size_t i = 0; i < tris.size(); ++i)
        if (tris[i] == tri) { o = (int)i; break; }
    if (o < 0) return -1;

    int idx = roots[o];
    for (int level = 0; level < depth; ++level) {
        const Node &node = nodes[idx];
        // The global barycentric map is affine with the same local weights as
        // the geometric one: solve against the node corners' bary values.
        double db1 = node.corner_bary[1][0] - node.corner_bary[0][0];
        double dg1 = node.corner_bary[1][1] - node.corner_bary[0][1];
        double db2 = node.corner_bary[2][0] - node.corner_bary[0][0];
        double dg2 = node.corner_bary[2][1] - node.corner_bary[0][1];
        double det = db1 * dg2 - dg1 * db2;
        if (std::abs(det) < 1e-30) return -1;
        double rb = beta - node.corner_bary[0][0];
        double rg = gamma - node.corner_bary[0][1];
        double l1 = (rb * dg2 - db2 * rg) / det;
        double l2 = (db1 * rg - rb * dg1) / det;

        // Boundary points go to the deterministically smaller child index.
        int child;
        if (l1 + l2 <= 0.5) child = 0;
        else if (l1 >= 0.5) child = 1;
        else if (l2 >= 0.5) child = 2;
        else child = 3;
        idx = node.child[child];
    }
    return nodes[idx].leaf_index;
}

const std::vector<RadiosityLink> &HierarchicalRadiosity::links() const {
    return link_store;
}

std::size_t HierarchicalRadiosity::full_leaf_pair_count() const {
    std::size_t per = (std::size_t)std::pow(4.0, depth);
    std::size_t total = 0;
    for (std::size_t i = 0; i < tris.size(); ++i)
        for (std::size_t j = i + 1; j < tris.size(); ++j)
            total += per * per;
    return total;
}

unsigned long long HierarchicalRadiosity::visibility_queries() const {
    return vis_queries;
}

std::size_t HierarchicalRadiosity::storage_peak_bytes() const {
    return peak_bytes;
}

unsigned long long HierarchicalRadiosity::operator_applications() const {
    return op_count;
}

unsigned long long HierarchicalRadiosity::node_tests() const {
    return saved_node_tests;
}

unsigned long long HierarchicalRadiosity::primitive_tests() const {
    return saved_prim_tests;
}

double HierarchicalRadiosity::compressed_row_sum_max() const {
    return row_sum_max;
}
