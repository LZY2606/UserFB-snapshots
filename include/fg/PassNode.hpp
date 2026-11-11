#pragma once

#include "fg/GraphNode.hpp"
#include "fg/PassEntry.hpp"
#include "fg/FrameGraphResource.hpp"
#include "fg/Subresource.hpp"
#include <memory>
#include <vector>

class PassNode final : public GraphNode {
  friend class FrameGraph;

public:
  PassNode(const PassNode &) = delete;
  PassNode(PassNode &&) noexcept = default;

  PassNode &operator=(const PassNode &) = delete;
  PassNode &operator=(PassNode &&) noexcept = delete;

  struct AccessDeclaration {
    FrameGraphResource id;
    uint32_t flags;
    SubresourceRange range; // Half-open box of the accessed subresources.

#if __cplusplus >= 202002L
    bool operator==(const AccessDeclaration &) const = default;
#endif
  };

  [[nodiscard]] bool creates(FrameGraphResource id) const;
  [[nodiscard]] bool reads(FrameGraphResource id) const;
  [[nodiscard]] bool writes(FrameGraphResource id) const;

  [[nodiscard]] bool hasSideEffect() const { return m_hasSideEffect; }
  [[nodiscard]] bool canExecute() const {
    return getRefCount() > 0 || hasSideEffect();
  }

  struct Create {};
  [[nodiscard]] decltype(auto) each(const Create) const { return m_creates; }
  struct Read {};
  [[nodiscard]] decltype(auto) each(const Read) const { return m_reads; }
  struct Write {};
  [[nodiscard]] decltype(auto) each(const Write) const { return m_writes; }

private:
  PassNode(const std::string_view name, uint32_t nodeId,
           std::unique_ptr<FrameGraphPassConcept> &&);

  FrameGraphResource _read(FrameGraphResource id, uint32_t flags,
                           const SubresourceRange &);
  [[nodiscard]] FrameGraphResource _write(FrameGraphResource id, uint32_t flags,
                                          const SubresourceRange &);

private:
  std::unique_ptr<FrameGraphPassConcept> m_exec;

  std::vector<FrameGraphResource> m_creates;
  std::vector<AccessDeclaration> m_reads;
  std::vector<AccessDeclaration> m_writes;

  bool m_hasSideEffect{false};
};

#if __cplusplus < 202002L
inline bool operator==(const PassNode::AccessDeclaration &lhs,
                       const PassNode::AccessDeclaration &rhs) {
  return lhs.id == rhs.id && lhs.flags == rhs.flags &&
         lhs.range.getFirstMip() == rhs.range.getFirstMip() &&
         lhs.range.getMipCount() == rhs.range.getMipCount() &&
         lhs.range.getFirstLayer() == rhs.range.getFirstLayer() &&
         lhs.range.getLayerCount() == rhs.range.getLayerCount() &&
         lhs.range.getAspectMask() == rhs.range.getAspectMask();
}
#endif
