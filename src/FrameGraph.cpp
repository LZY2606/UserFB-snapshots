#include "fg/FrameGraph.hpp"
#include "fg/GraphvizWriter.hpp"
#include <algorithm>
#include <sstream>
#include <vector>

//
// FrameGraph class:
//

void FrameGraph::reserve(uint32_t numPasses, uint32_t numResources) {
  m_passNodes.reserve(numPasses);
  m_resourceNodes.reserve(numResources);
  m_resourceRegistry.reserve(numResources);
}

bool FrameGraph::isValid(FrameGraphResource id) const {
  const auto &node = _getResourceNode(id);
  return node.getVersion() == _getResourceEntry(node).getVersion();
}

void FrameGraph::compile() {
  for (auto &pass : m_passNodes) pass.m_refCount = 0;
  for (auto &node : m_resourceNodes) {
    node.m_refCount = 0;
    node.m_producer = nullptr;
  }

  struct ProducerWrite {
    PassNode *pass;
    SubresourceRange range;
  };
  struct PendingRead {
    PassNode *pass;
    SubresourceRange range;
  };
  struct Analysis {
    PassNode *creator{nullptr};
    std::string name;
    std::vector<ProducerWrite> writes; // In pass registration order.
    std::vector<PendingRead> reads;
  };

  std::vector<Analysis> analyses{m_resourceRegistry.size()};

  // Collect every access per virtual resource (versions share one entry).
  for (auto &pass : m_passNodes) {
    for (const auto id : pass.m_creates) {
      auto &an = analyses[m_resourceNodes[id].m_resourceId];
      an.creator = &pass;
      if (an.name.empty()) an.name = m_resourceNodes[id].getName();
    }
    for (const auto &a : pass.m_reads) {
      const auto rid = m_resourceNodes[a.id].m_resourceId;
      auto &an = analyses[rid];
      if (an.name.empty()) an.name = m_resourceNodes[a.id].getName();
      an.reads.push_back({&pass, a.range});
    }
    for (const auto &a : pass.m_writes) {
      const auto rid = m_resourceNodes[a.id].m_resourceId;
      auto &an = analyses[rid];
      if (an.name.empty()) an.name = m_resourceNodes[a.id].getName();
      an.writes.push_back({&pass, a.range});
    }
  }

  // Retained producer edges: [passIndex][resourceId]. A pass writing several
  // regions of the same resource counts as a single edge and executes once.
  std::vector<std::vector<uint8_t>> retained(
    m_passNodes.size(),
    std::vector<uint8_t>(m_resourceRegistry.size(), 0));

  struct Need {
    PassNode *reader;
    uint32_t resourceId;
    SubresourceRange range;
  };
  std::vector<Need> work;

  // Side-effect passes (imported writes included) keep all their reads.
  for (auto &pass : m_passNodes) {
    if (!pass.hasSideEffect()) continue;
    for (const auto &a : pass.m_reads)
      work.push_back({&pass, m_resourceNodes[a.id].m_resourceId, a.range});
  }

  while (!work.empty()) {
    const auto need = work.back();
    work.pop_back();

    const auto &an = analyses[need.resourceId];
    auto remaining = std::vector<SubresourceRange>{need.range};

    // Cover the requested box with producers, newest first. Producers at or
    // after the reader cannot contribute to this particular read.
    for (auto it = an.writes.rbegin();
         it != an.writes.rend() && !remaining.empty(); ++it) {
      if (it->pass->getId() >= need.reader->getId()) continue;

      std::vector<SubresourceRange> uncovered;
      for (const auto &r : remaining) {
        if (!rangesIntersect(r, it->range)) {
          uncovered.push_back(r);
          continue;
        }
        auto &edge = retained[it->pass->getId()][need.resourceId];
        if (edge == 0) {
          edge = 1;
          it->pass->m_refCount++;
          // A retained pass keeps every input it consumes, including reads
          // of other resources (their producers must be retained too).
          for (const auto &ra : it->pass->m_reads) {
            work.push_back({it->pass,
                            m_resourceNodes[ra.id].m_resourceId, ra.range});
          }
        }
        // Overwriting write: only the intersecting box is supplied here,
        // everything else keeps inheriting older sources.
        for (auto &piece : subtractRange(r, it->range))
          uncovered.push_back(piece);
      }
      remaining = std::move(uncovered);
    }

    if (remaining.empty()) continue;

    const auto &entry = m_resourceRegistry[need.resourceId];
    if (entry.isImported())
      continue; // Imported resources are fully defined at import time.

    // A live pass consumes a region of a transient resource that no earlier
    // (live) pass ever wrote. Do not silently zero-initialize it.
    const auto &missing = remaining.front();
    std::ostringstream oss;
    oss << "Pass '" << need.reader->getName() << "' reads uninitialized region"
        << " (mips [" << missing.getFirstMip() << ", " << missing.getLastMip()
        << "), layers [" << missing.getFirstLayer() << ", "
        << missing.getLastLayer() << "), aspects 0x" << std::hex
        << missing.getAspectMask() << std::dec << ") of transient resource '"
        << an.name << "'";
    throw FrameGraphRangeError{oss.str(),
                               std::string{need.reader->getName()}, an.name,
                               missing};
  }

  // -- Resource node bookkeeping (for visualization / debugging):

  for (auto &pass : m_passNodes) {
    if (!pass.canExecute()) continue;
    for (const auto &a : pass.m_reads) m_resourceNodes[a.id].m_refCount++;
    for (const auto &a : pass.m_writes)
      m_resourceNodes[a.id].m_producer = &pass;
  }

  // -- Lifetime: one T object per virtual resource, created once before the
  // first surviving access and destroyed once after the last one.

  m_createAt.assign(m_resourceRegistry.size(), nullptr);
  m_destroyAt.assign(m_resourceRegistry.size(), nullptr);

  for (uint32_t rid = 0; rid < m_resourceRegistry.size(); ++rid) {
    const auto &an = analyses[rid];
    PassNode *first{nullptr};
    PassNode *last{nullptr};

    const auto touch = [&](PassNode *pass) {
      if (pass == nullptr || !pass->canExecute()) return;
      if (first == nullptr || pass->getId() < first->getId()) first = pass;
      if (last == nullptr || pass->getId() > last->getId()) last = pass;
    };
    touch(an.creator);
    for (const auto &r : an.reads) touch(r.pass);
    for (const auto &w : an.writes) touch(w.pass);

    if (first == nullptr) continue; // No surviving access -> never allocated.
    if (m_resourceRegistry[rid].isImported()) continue;

    m_createAt[rid] = first;
    m_destroyAt[rid] = last;
  }
}

void FrameGraph::execute(void *context, void *allocator) {
  for (const auto &pass : m_passNodes) {
    if (!pass.canExecute()) continue;

    for (uint32_t rid = 0; rid < m_resourceRegistry.size(); ++rid) {
      if (m_createAt[rid] == &pass) m_resourceRegistry[rid].create(allocator);
    }

    for (const auto &[id, flags, _] : pass.m_reads) {
      if (flags != kFlagsIgnored) _getResourceEntry(id).preRead(flags, context);
    }
    for (const auto &[id, flags, _] : pass.m_writes) {
      if (flags != kFlagsIgnored)
        _getResourceEntry(id).preWrite(flags, context);
    }
    FrameGraphPassResources resources{*this, pass};
    std::invoke(*pass.m_exec, resources, context);

    for (uint32_t rid = 0; rid < m_resourceRegistry.size(); ++rid) {
      if (m_destroyAt[rid] == &pass)
        m_resourceRegistry[rid].destroy(allocator);
    }
  }
}

//
// (private):
//

PassNode &
FrameGraph::_createPassNode(const std::string_view name,
                            std::unique_ptr<FrameGraphPassConcept> &&base) {
  const auto id = static_cast<uint32_t>(m_passNodes.size());
  return m_passNodes.emplace_back(PassNode{name, id, std::move(base)});
}

ResourceNode &FrameGraph::_createResourceNode(const std::string_view name,
                                              uint32_t resourceId,
                                              uint32_t version) {
  const auto id = static_cast<uint32_t>(m_resourceNodes.size());
  return m_resourceNodes.emplace_back(
    ResourceNode{name, id, resourceId, version});
}
FrameGraphResource FrameGraph::_clone(FrameGraphResource id) {
  const auto &node = _getResourceNode(id);
  auto &entry = _getResourceEntry(node);
  entry.m_version++;

  const auto &clone = _createResourceNode(node.getName(), node.getResourceId(),
                                          entry.getVersion());
  return clone.getId();
}

SubresourceRange
FrameGraph::_normalizeRange(FrameGraphResource id,
                            const SubresourceRange &range) const {
  return normalizeRange(_getResourceEntry(id).getDomain(), range);
}

const ResourceNode &FrameGraph::_getResourceNode(FrameGraphResource id) const {
  assert(id < m_resourceNodes.size());
  return m_resourceNodes[id];
}
const ResourceEntry &
FrameGraph::_getResourceEntry(FrameGraphResource id) const {
  return _getResourceEntry(_getResourceNode(id));
}
const ResourceEntry &
FrameGraph::_getResourceEntry(const ResourceNode &node) const {
  assert(node.m_resourceId < m_resourceRegistry.size());
  return m_resourceRegistry[node.m_resourceId];
}

// ---

std::ostream &operator<<(std::ostream &os, const FrameGraph &fg) {
  return fg.debugOutput(os, graphviz::Writer{});
}

//
// FrameGraph::Builder class:
//

FrameGraphResource FrameGraph::Builder::read(FrameGraphResource id,
                                             uint32_t flags) {
  assert(m_frameGraph.isValid(id));
  const auto range =
    SubresourceRange::entire(m_frameGraph._getResourceEntry(id).getDomain());
  return m_passNode._read(id, flags, range);
}

FrameGraphResource FrameGraph::Builder::write(FrameGraphResource id,
                                              uint32_t flags) {
  assert(m_frameGraph.isValid(id));
  const auto &entry = m_frameGraph._getResourceEntry(id);
  const auto range = SubresourceRange::entire(entry.getDomain());
  if (entry.isImported()) setSideEffect();

  if (m_passNode.creates(id)) {
    return m_passNode._write(id, flags, range);
  } else {
    // Writing to a texture produces a renamed handle.
    // This allows us to catch errors when resources are modified in
    // undefined order (when same resource is written by different passes).
    // Renaming resources enforces a specific execution order of the render
    // passes. A whole-resource rewrite implicitly reads the old full version.
    m_passNode._read(id, kFlagsIgnored, range);
    return m_passNode._write(m_frameGraph._clone(id), flags, range);
  }
}

FrameGraphResource FrameGraph::Builder::readRange(FrameGraphResource id,
                                                  const SubresourceRange &range,
                                                  uint32_t flags) {
  assert(m_frameGraph.isValid(id));
  const auto normalized = m_frameGraph._normalizeRange(id, range);
  return m_passNode._read(id, flags, normalized);
}

FrameGraphResource FrameGraph::Builder::writeRange(
  FrameGraphResource id, const SubresourceRange &range, uint32_t flags) {
  assert(m_frameGraph.isValid(id));
  const auto normalized = m_frameGraph._normalizeRange(id, range);
  if (m_frameGraph._getResourceEntry(id).isImported()) setSideEffect();

  // Overwriting write: only the given range is redefined, there is no
  // implicit read of the previous version, and the handle is renamed.
  return m_passNode._write(m_frameGraph._clone(id), flags, normalized);
}
