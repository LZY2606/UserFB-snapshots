#pragma once

#include <vector>
#include "../utilities/BBox.hpp"

class Geometry;
class Point3D;
class Ray;
class ShadeInfo;
class World;

struct BVHNode {
    BBox bbox;
    BVHNode *left;
    BVHNode *right;
    std::vector<Geometry *> objs;

    BVHNode() : left(nullptr), right(nullptr) {}
    ~BVHNode() { delete left; delete right; }
    bool is_leaf() const { return left == nullptr; }
};

// Counters for actual work performed by visibility traversals.
struct QueryStats {
    unsigned long long node_tests; // BVH nodes whose box was actually tested.
    unsigned long long prim_tests; // primitive intersection/box tests.

    QueryStats() : node_tests(0), prim_tests(0) {}
    void reset() { node_tests = 0; prim_tests = 0; }
};

class BVH {
    BVHNode *root;

    BVHNode *build_node(std::vector<Geometry *> &objects, int depth);
    bool hit_node(BVHNode *node, const Ray &ray, float &t, ShadeInfo &sinfo) const;
    bool segment_node(BVHNode *node, const Point3D &O, const Point3D &Q,
                      double s_lo, double s_hi,
                      const Geometry *skip_a, const Geometry *skip_b,
                      QueryStats &stats) const;
    bool bundle_node(BVHNode *node, const BBox &bundle,
                     const Geometry *skip_a, const Geometry *skip_b,
                     QueryStats &stats) const;

public:
    BVH();
    ~BVH();

    void build(std::vector<Geometry *> &objects);
    bool hit(const Ray &ray, float &t, ShadeInfo &sinfo) const;

    // True if any primitive other than skip_a/skip_b intersects the open
    // segment O + s (Q - O), s in (s_lo, s_hi). Double sided. Every node box
    // and primitive actually examined is recorded in stats.
    bool segment_occluded(const Point3D &O, const Point3D &Q,
                          double s_lo, double s_hi,
                          const Geometry *skip_a, const Geometry *skip_b,
                          QueryStats &stats) const;

    // Conservative bundle test: true if some non-skipped primitive's
    // bounding box overlaps the bundle's AABB. Never declares a bundle clear
    // from samples; an AABB overlap is always reported as a possible blocker.
    bool bundle_may_occlude(const BBox &bundle,
                            const Geometry *skip_a, const Geometry *skip_b,
                            QueryStats &stats) const;
};
