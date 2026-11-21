#include "BVH.hpp"
#include "../geometry/Geometry.hpp"
#include "../utilities/Point3D.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/ShadeInfo.hpp"
#include "../utilities/Constants.hpp"
#include <algorithm>
#include <iostream>

BVH::BVH() : root(nullptr) {}
BVH::~BVH() { delete root; }

static float gc(Geometry *g, int ax) {
    BBox b = g->getBBox();
    if (ax == 0) return (b.pmin.x + b.pmax.x) * 0.5f;
    if (ax == 1) return (b.pmin.y + b.pmax.y) * 0.5f;
    return (b.pmin.z + b.pmax.z) * 0.5f;
}

// Conservative AABB overlap, inclusive on every face.
static bool aabb_overlap(const BBox &a, const BBox &b) {
    return a.pmin.x <= b.pmax.x && a.pmax.x >= b.pmin.x &&
           a.pmin.y <= b.pmax.y && a.pmax.y >= b.pmin.y &&
           a.pmin.z <= b.pmax.z && a.pmax.z >= b.pmin.z;
}

// Slab test of a finite segment against an AABB. Returns true only if the
// segment enters the box inside the open (s_lo, s_hi) interval.
static bool segment_box(const Point3D &O, const Point3D &Q,
                        double s_lo, double s_hi, const BBox &box) {
    double dx = (double)Q.x - O.x;
    double dy = (double)Q.y - O.y;
    double dz = (double)Q.z - O.z;
    double t0 = s_lo;
    double t1 = s_hi;
    const double lo[3] = {box.pmin.x, box.pmin.y, box.pmin.z};
    const double hi[3] = {box.pmax.x, box.pmax.y, box.pmax.z};
    const double o[3] = {O.x, O.y, O.z};
    const double d[3] = {dx, dy, dz};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-30) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) return false;
        } else {
            double inv = 1.0 / d[axis];
            double a = (lo[axis] - o[axis]) * inv;
            double b = (hi[axis] - o[axis]) * inv;
            if (a > b) std::swap(a, b);
            if (a > t0) t0 = a;
            if (b < t1) t1 = b;
            if (t0 > t1) return false;
        }
    }
    return t0 < t1;
}

BVHNode *BVH::build_node(std::vector<Geometry *> &objs, int depth) {
    BVHNode *node = new BVHNode();

    BBox box;
    box.pmin = Point3D(kHugeValue);
    box.pmax = Point3D(-kHugeValue);
    for (auto *g : objs) box.extend(g);
    node->bbox = box;

    int n = (int)objs.size();
    if (n <= 4 || depth > 30) {
        node->objs = objs;
        return node;
    }

    float dx = box.pmax.x - box.pmin.x;
    float dy = box.pmax.y - box.pmin.y;
    float dz = box.pmax.z - box.pmin.z;
    int ax = 0;
    if (dy > dx && dy > dz) ax = 1;
    else if (dz > dx) ax = 2;

    std::sort(objs.begin(), objs.end(), [ax](Geometry *a, Geometry *b) {
        return gc(a, ax) < gc(b, ax);
    });

    int m = n / 2;
    std::vector<Geometry *> left(objs.begin(), objs.begin() + m);
    std::vector<Geometry *> right(objs.begin() + m, objs.end());

    node->left  = build_node(left,  depth + 1);
    node->right = build_node(right, depth + 1);
    return node;
}

void BVH::build(std::vector<Geometry *> &objs) {
    if (objs.empty()) return;
    std::cout << "bvh: building over " << objs.size() << " prims\n";
    root = build_node(objs, 0);
}

bool BVH::hit_node(BVHNode *node, const Ray &ray, float &t, ShadeInfo &si) const {
    float t0, t1;
    if (!node->bbox.hit(ray, t0, t1)) return false;

    if (node->is_leaf()) {
        bool ok = false;
        for (auto *g : node->objs) {
            float tmp;
            ShadeInfo s(*si.w);
            if (g->hit(ray, tmp, s) && tmp < t) {
                t = tmp; si = s; ok = true;
            }
        }
        return ok;
    }

    bool hl = hit_node(node->left,  ray, t, si);
    bool hr = hit_node(node->right, ray, t, si);
    return hl || hr;
}

bool BVH::hit(const Ray &ray, float &t, ShadeInfo &si) const {
    if (!root) return false;
    t = kHugeValue;
    return hit_node(root, ray, t, si);
}

bool BVH::segment_node(BVHNode *node, const Point3D &O, const Point3D &Q,
                       double s_lo, double s_hi,
                       const Geometry *skip_a, const Geometry *skip_b,
                       QueryStats &stats) const {
    ++stats.node_tests;
    if (!segment_box(O, Q, s_lo, s_hi, node->bbox)) return false;

    if (node->is_leaf()) {
        for (auto *g : node->objs) {
            if (g == skip_a || g == skip_b) continue;
            ++stats.prim_tests;
            double s_hit;
            if (g->hit_segment(O, Q, s_lo, s_hi, s_hit)) return true;
        }
        return false;
    }

    if (segment_node(node->left, O, Q, s_lo, s_hi, skip_a, skip_b, stats))
        return true;
    return segment_node(node->right, O, Q, s_lo, s_hi, skip_a, skip_b, stats);
}

bool BVH::segment_occluded(const Point3D &O, const Point3D &Q,
                           double s_lo, double s_hi,
                           const Geometry *skip_a, const Geometry *skip_b,
                           QueryStats &stats) const {
    if (!root) return false;
    return segment_node(root, O, Q, s_lo, s_hi, skip_a, skip_b, stats);
}

bool BVH::bundle_node(BVHNode *node, const BBox &bundle,
                      const Geometry *skip_a, const Geometry *skip_b,
                      QueryStats &stats) const {
    ++stats.node_tests;
    if (!aabb_overlap(node->bbox, bundle)) return false;

    if (node->is_leaf()) {
        for (auto *g : node->objs) {
            if (g == skip_a || g == skip_b) continue;
            ++stats.prim_tests;
            if (aabb_overlap(g->getBBox(), bundle)) return true;
        }
        return false;
    }

    if (bundle_node(node->left, bundle, skip_a, skip_b, stats)) return true;
    return bundle_node(node->right, bundle, skip_a, skip_b, stats);
}

bool BVH::bundle_may_occlude(const BBox &bundle,
                             const Geometry *skip_a, const Geometry *skip_b,
                             QueryStats &stats) const {
    if (!root) return false;
    return bundle_node(root, bundle, skip_a, skip_b, stats);
}
