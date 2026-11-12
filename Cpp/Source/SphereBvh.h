#pragma once

#include "Maths.h"

#include <vector>

// A cacheable bounding volume hierarchy over spheres for the CPU tracer.
//
// Ownership: the BVH never takes ownership of the caller's Sphere array.
// Build/Refit copy the sphere geometry needed by the tree into internal,
// primitive-reordered storage. The caller keeps the original arrays alive and
// must keep their original numbering. Hit results always report the caller's
// original sphere index.
//
// Queries are const, do not allocate, and do not modify the tree, so any number
// of render threads may call ClosestHit/Occluded concurrently. Build and Refit
// mutate the tree and must not race with queries.
class SphereBvh
{
public:
    SphereBvh();
    ~SphereBvh();

    // Full rebuild: re-partitions primitives and rebuilds every node bound.
    void Build(const Sphere* spheres, int count);
    // Refit: re-reads sphere geometry and recomputes leaf data and node bounds.
    // If count is unchanged the existing topology (leaf membership and node
    // layout) is kept; if count changed the tree is rebuilt.
    void Refit(const Sphere* spheres, int count);

    void Clear();

    // Per-query counters. They are written by the query itself; there is no
    // shared global counter. Set the pointer to null if not needed.
    struct QueryStats
    {
        int nodesVisited;
        int sphereTests;
    };

    // Closest hit in the open interval (tMin, tMax). The returned id is the
    // caller's original sphere index, or -1. On equal distances the smallest
    // original index wins.
    int ClosestHit(const Ray& r, float tMin, float tMax, Hit& outHit, QueryStats* outStats = 0) const;
    // Any hit in the open interval (tMin, tMax)? Does not find the closest one.
    bool Occluded(const Ray& r, float tMin, float tMax, QueryStats* outStats = 0) const;

    int SphereCount() const { return m_sphereCount; }
    int NodeCount() const { return (int)m_nodes.size(); }
    int LeafCount() const { return m_leafCount; }

    // Introspection for tests: lanes past the leaf's primitive count are
    // invalid and must never be intersected.
    int GetLeafLaneCount(int leafIndex) const;
    bool IsLeafLaneValid(int leafIndex, int lane) const;
    int GetLeafPrimitive(int leafIndex, int lane) const;

private:
    struct Node
    {
        float3pack bmin;
        float3pack bmax;
        int child0; // internal: left child; leaf: leaf index
        int child1; // internal: right child; leaf: unused
        int count;  // 0 for internal, 1..4 for a leaf
        int pad;
    };

    struct Candidate
    {
        float t;
        int id;
        float centerX, centerY, centerZ;
        float invRadius;
    };

    int BuildNode(int begin, int end, const Sphere* spheres);
    void RefitNode(int nodeIndex, const Sphere* spheres);
    bool TestLeaf(int leafIndex, const Ray& r, float tMin, Candidate& best, QueryStats* stats) const;
    static bool BoxIntersects(const Node& n, const Ray& r, float tMin, float limitT);

    std::vector<Node> m_nodes;
    std::vector<int> m_buildIds;
    std::vector<int> m_leafIds;
    std::vector<float> m_leafCenterX;
    std::vector<float> m_leafCenterY;
    std::vector<float> m_leafCenterZ;
    std::vector<float> m_leafSqRadius;
    std::vector<float> m_leafInvRadius;
    std::vector<float> m_leafValid;
    std::vector<int> m_leafCounts;
    int m_sphereCount;
    int m_leafCount;
    int m_root;

    SphereBvh(const SphereBvh&);
    SphereBvh& operator=(const SphereBvh&);
};
