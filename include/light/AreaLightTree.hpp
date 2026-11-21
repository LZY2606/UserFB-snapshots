#pragma once
#include <object/Object.hpp>
#include <unordered_map>

namespace SoftRasterizer {
/*Two-level, locally updatable area-light distribution.
 *
 *Leaves are emissive primitives; each primitive belongs to one owner group
 *(a DisplacedMesh groups all its micro triangles, while an ordinary emissive
 *primitive forms its own group). Both levels use a sum tree, so selecting a
 *primitive costs O(log G + log K) and changing one area costs only the leaf
 *to root paths. Every weight is the primitive's current transformed
 *world-space area.*/
class EmissiveAreaTree {
public:
  /*Rebuild both levels from scene primitives. Not counted as an update.*/
  void rebuild(const std::vector<std::shared_ptr<Object>> &primitives);

  /*Refresh the listed primitives' areas. Returns how many tree nodes were
   *recomputed; within one call each affected node is recomputed at most
   *once.*/
  std::uint64_t
  updateAreas(const std::vector<std::shared_ptr<Object>> &changed);

  bool empty() const {
    return m_totalArea <= 0 || m_groups.empty();
  }
  float totalArea() const {
    return m_totalArea;
  }
  bool contains(const Object *primitive) const {
    return m_lookup.count(primitive) != 0;
  }

  /*Pick a primitive in proportion to area and sample it. The returned
   *pdfArea is exactly 1 / total emissive area.*/
  SurfaceSample sample(Sampler &rng) const;

private:
  struct Group {
    const Object *owner = nullptr;
    std::vector<std::shared_ptr<Object>> leaves;
    std::vector<float> tree; //leaves at [size, 2*size)
    std::size_t size = 0;    //power-of-two leaf capacity
    std::vector<std::uint64_t> nodeStamp;
    float total = 0;
    std::unordered_map<const Object *, std::size_t> leafIndex;
  };

  static std::size_t treeSize(std::size_t leaves) {
    std::size_t size = 1;
    while (size < leaves) {
      size <<= 1;
    }
    return size;
  }
  static std::uint64_t findLeaf(const std::vector<float> &tree,
                                std::size_t size, float target);
  void rebuildGroup(Group &group);
  void rebuildLevelOne();

  std::vector<Group> m_groups;
  std::vector<float> m_groupTree;
  std::size_t m_groupSize = 0;
  std::vector<std::uint64_t> m_groupStamp;
  std::unordered_map<const Object *, std::pair<std::size_t, std::size_t>>
      m_lookup;
  float m_totalArea = 0;
  std::uint64_t m_generation = 0;
};
} // namespace SoftRasterizer
