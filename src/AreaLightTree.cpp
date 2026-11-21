#include <light/AreaLightTree.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace SoftRasterizer {

void EmissiveAreaTree::rebuild(
    const std::vector<std::shared_ptr<Object>> &primitives) {
  m_groups.clear();
  m_groupTree.clear();
  m_groupSize = 0;
  m_groupStamp.clear();
  m_lookup.clear();
  m_totalArea = 0;

  /*Group leaves by their sampling owner while preserving first-seen order so
   *selection is deterministic.*/
  std::unordered_map<const Object *, std::size_t> groupIndex;
  for (const auto &primitive : primitives) {
    if (!primitive) {
      continue;
    }
    const auto *material = primitive->getMaterial().get();
    if (!material) {
      throw std::invalid_argument("null primitive material");
    }
    /*Validate every primitive's material, emissive or not, matching the
     *previous Scene.prepare behaviour.*/
    validateMaterialFields(*material);
    if (!material->hasEmission()) {
      continue;
    }
    const float area = primitive->getArea();
    if (!(area > 0) || !std::isfinite(area)) {
      continue;
    }
    const Object *owner = primitive->samplingOwner();
    auto [iter, inserted] = groupIndex.emplace(owner, m_groups.size());
    if (inserted) {
      m_groups.emplace_back();
      m_groups.back().owner = owner;
    }
    Group &group = m_groups[iter->second];
    m_lookup[primitive.get()] = {iter->second, group.leaves.size()};
    group.leaves.push_back(primitive);
  }
  m_groups.erase(std::remove_if(m_groups.begin(), m_groups.end(),
                                [](const Group &g) { return g.leaves.empty(); }),
                 m_groups.end());
  /*Reindex groups after erasing empties.*/
  m_lookup.clear();
  for (std::size_t g = 0; g < m_groups.size(); ++g) {
    Group &group = m_groups[g];
    group.size = treeSize(group.leaves.size());
    group.tree.assign(2 * group.size, 0.f);
    group.leafIndex.clear();
    for (std::size_t i = 0; i < group.leaves.size(); ++i) {
      group.tree[group.size + i] = group.leaves[i]->getArea();
      group.leafIndex[group.leaves[i].get()] = i;
      m_lookup[group.leaves[i].get()] = {g, i};
    }
    rebuildGroup(group);
  }
  rebuildLevelOne();
}

void EmissiveAreaTree::rebuildGroup(Group &group) {
  for (std::size_t node = group.size; node-- > 1;) {
    group.tree[node] = group.tree[2 * node] + group.tree[2 * node + 1];
  }
  group.total = group.tree.empty() ? 0 : group.tree[1];
  group.nodeStamp.assign(group.tree.size(), 0);
}

void EmissiveAreaTree::rebuildLevelOne() {
  m_groupSize = treeSize(m_groups.size());
  m_groupTree.assign(2 * m_groupSize, 0.f);
  for (std::size_t g = 0; g < m_groups.size(); ++g) {
    m_groupTree[m_groupSize + g] = m_groups[g].total;
  }
  for (std::size_t node = m_groupSize; node-- > 1;) {
    m_groupTree[node] = m_groupTree[2 * node] + m_groupTree[2 * node + 1];
  }
  m_groupStamp.assign(m_groupTree.size(), 0);
  m_totalArea = m_groupTree.empty() ? 0 : m_groupTree[1];
}

std::uint64_t
EmissiveAreaTree::updateAreas(const std::vector<std::shared_ptr<Object>> &changed) {
  if (m_groups.empty()) {
    return 0;
  }
  const std::uint64_t generation = ++m_generation;
  std::uint64_t updates = 0;
  std::vector<std::size_t> dirtyGroups;
  for (const auto &primitive : changed) {
    if (!primitive) {
      continue;
    }
    auto lookup = m_lookup.find(primitive.get());
    if (lookup == m_lookup.end()) {
      continue;
    }
    const auto [g, i] = lookup->second;
    Group &group = m_groups[g];
    const float area = primitive->getArea();
    if (!(area >= 0) || !std::isfinite(area)) {
      throw std::invalid_argument("invalid primitive area");
    }
    if (group.tree[group.size + i] == area) {
      continue;
    }
    group.tree[group.size + i] = area;
    ++updates;
    /*Only the leaf-to-root path. A shared ancestor is recomputed once.*/
    for (std::size_t node = (group.size + i) / 2; node >= 1; node /= 2) {
      if (group.nodeStamp[node] == generation) {
        break;
      }
      group.nodeStamp[node] = generation;
      group.tree[node] = group.tree[2 * node] + group.tree[2 * node + 1];
      ++updates;
      if (node == 1) {
        break;
      }
    }
    if (group.total != group.tree[1]) {
      group.total = group.tree[1];
      dirtyGroups.push_back(g);
    }
  }
  /*Only the groups whose totals changed walk the level-one tree; a shared
   *ancestor is still recomputed at most once.*/
  for (std::size_t g : dirtyGroups) {
    m_groupTree[m_groupSize + g] = m_groups[g].total;
    ++updates;
    for (std::size_t node = (m_groupSize + g) / 2; node >= 1; node /= 2) {
      if (m_groupStamp[node] == generation) {
        break;
      }
      m_groupStamp[node] = generation;
      m_groupTree[node] = m_groupTree[2 * node] + m_groupTree[2 * node + 1];
      ++updates;
      if (node == 1) {
        break;
      }
    }
  }
  if (!dirtyGroups.empty()) {
    m_totalArea = m_groupTree.empty() ? 0 : m_groupTree[1];
  }
  return updates;
}

std::uint64_t EmissiveAreaTree::findLeaf(const std::vector<float> &tree,
                                         std::size_t size, float target) {
  /*Descend the sum tree; target < tree[1].*/
  std::size_t node = 1;
  while (node < size) {
    if (target < tree[2 * node]) {
      node *= 2;
    } else {
      target -= tree[2 * node];
      node = 2 * node + 1;
    }
  }
  return node - size;
}

SurfaceSample EmissiveAreaTree::sample(Sampler &rng) const {
  if (empty() || !(m_totalArea > 0)) {
    return {};
  }
  /*Select a group, then a leaf inside it, both proportional to area.*/
  float target = std::min(rng.next() * m_totalArea,
                          std::nextafter(m_totalArea, 0.f));
  const std::size_t groupIndex =
      std::min<std::size_t>(findLeaf(m_groupTree, m_groupSize, target),
                            m_groups.size() - 1);
  const Group &group = m_groups[groupIndex];
  float local = std::min(rng.next() * group.total,
                         std::nextafter(group.total, 0.f));
  const std::size_t leaf =
      std::min<std::size_t>(findLeaf(group.tree, group.size, local),
                            group.leaves.size() - 1);
  auto result = group.leaves[leaf]->sample(rng);
  result.pdfArea = 1 / m_totalArea;
  return result;
}
} // namespace SoftRasterizer
