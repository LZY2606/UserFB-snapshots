#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <object/Object.hpp>
#include <unordered_map>
#include <vector>

namespace SoftRasterizer {
class BVHAcceleration {
public:
  /*Node identity and structure stay fixed during a refit; only bounds
   *change. Counts are per refit batch after deduplicating shared ancestors.*/
  struct RefitCounters {
    std::uint64_t leafRefits = 0;
    std::uint64_t nodeRefits = 0;
  };

  void rebuild(std::vector<std::shared_ptr<Object>> primitives);
  /*Recompute the bounds of the leaves that own the given primitives, then
   *refresh the affected leaf-to-root paths. Node links, leaf ranges, leaf
   *ordering and primitive addresses are untouched.*/
  RefitCounters refit(const std::vector<std::shared_ptr<Object>> &dirtyPrimitives);

  Intersection getIntersection(const Ray &ray) const;
  bool occluded(const Ray &ray) const;

  bool empty() const {
    return !m_nodes;
  }

  /*Debug identity of the leaf that currently owns a primitive, or null.*/
  const void *leafIdentity(const Object *primitive) const {
    auto iter = m_leafOf.find(primitive);
    return iter == m_leafOf.end() ? nullptr : iter->second;
  }
  /*Stable node and leaf addresses, in pool order.*/
  std::vector<const void *> nodeIdentities() const {
    std::vector<const void *> ids;
    ids.reserve(m_nodeCount);
    for (std::size_t i = 0; i < m_nodeCount; ++i) {
      ids.push_back(&m_nodes[i]);
    }
    return ids;
  }
  std::vector<const void *> leafIdentities() const {
    std::vector<const void *> ids;
    for (std::size_t i = 0; i < m_nodeCount; ++i) {
      if (m_nodes[i].count) {
        ids.push_back(&m_nodes[i]);
      }
    }
    return ids;
  }

private:
  struct Node {
    Bounds3 box;
    Node *left = nullptr, *right = nullptr, *parent = nullptr;
    std::uint32_t begin = 0, count = 0, depth = 0;
    std::uint64_t refitStamp = 0;
  };

  /*Build in place over [begin, end); no per-child object arrays.*/
  Node *recursive(std::uint32_t begin, std::uint32_t end, Node *parent,
                  std::uint32_t depth);
  void buildLookup();
  void intersection(const Node *node, Ray &ray, Intersection &nearest) const;
  bool occludedNode(const Node *node, const Ray &ray) const;

  /*One owning pool; children only point into it. The first node is the root.*/
  std::unique_ptr<Node[]> m_nodes;
  std::size_t m_nodeCount = 0;
  std::vector<std::shared_ptr<Object>> m_objects;
  /*Reverse primitive -> leaf lookup for incremental refits.*/
  std::unordered_map<const Object *, Node *> m_leafOf;
  std::uint64_t m_refitGeneration = 0;
};
} // namespace SoftRasterizer
