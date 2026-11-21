#include "BVH.hpp"
#include "../geometry/Geometry.hpp"
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
