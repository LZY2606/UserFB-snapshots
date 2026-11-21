#pragma once

#include <vector>
#include "../utilities/BBox.hpp"

class Geometry;
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

class BVH {
    BVHNode *root;

    BVHNode *build_node(std::vector<Geometry *> &objects, int depth);

public:
    BVH();
    ~BVH();

    void build(std::vector<Geometry *> &objects);
    bool hit(const Ray &ray, float &t, ShadeInfo &sinfo) const;

private:
    bool hit_node(BVHNode *node, const Ray &ray, float &t, ShadeInfo &sinfo) const;
};
