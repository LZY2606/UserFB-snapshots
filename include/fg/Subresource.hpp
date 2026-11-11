#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Describes the subresource layout of a virtual resource:
// how many mip levels, array layers and which aspects it owns.
// Stored independently from T::Desc, backends do not need to know about it.
class SubresourceDomain {
public:
  using Aspects = uint32_t;

  // Sentinel meaning "every aspect the domain defines".
  static constexpr Aspects kAllAspects = ~Aspects{0};

  // A domain without explicit subresources behaves as a single unit.
  constexpr SubresourceDomain() = default;
  constexpr SubresourceDomain(uint32_t mipCount, uint32_t layerCount,
                              Aspects aspectMask = kAllAspects)
      : m_mipCount{mipCount}, m_layerCount{layerCount},
        m_aspectMask{aspectMask} {}

  [[nodiscard]] constexpr auto getMipCount() const { return m_mipCount; }
  [[nodiscard]] constexpr auto getLayerCount() const { return m_layerCount; }
  [[nodiscard]] constexpr auto getAspectMask() const { return m_aspectMask; }

  [[nodiscard]] constexpr bool isUnit() const {
    return m_mipCount == 1 && m_layerCount == 1 && m_aspectMask == kAllAspects;
  }

private:
  uint32_t m_mipCount{1};
  uint32_t m_layerCount{1};
  Aspects m_aspectMask{kAllAspects};
};

// A half-open [first, first + count) selection of mips/layers together with
// an aspect subset. Default-constructed range covers one complete unit.
class SubresourceRange {
public:
  constexpr SubresourceRange() = default;
  constexpr SubresourceRange(uint32_t firstMip, uint32_t mipCount,
                             uint32_t firstLayer, uint32_t layerCount,
                             SubresourceDomain::Aspects aspectMask =
                               SubresourceDomain::kAllAspects)
      : m_firstMip{firstMip}, m_mipCount{mipCount},
        m_firstLayer{firstLayer}, m_layerCount{layerCount},
        m_aspectMask{aspectMask} {}

  // The entire domain as one range.
  [[nodiscard]] static SubresourceRange entire(const SubresourceDomain &d) {
    return SubresourceRange{0, d.getMipCount(), 0, d.getLayerCount(),
                            d.getAspectMask()};
  }

  [[nodiscard]] constexpr auto getFirstMip() const { return m_firstMip; }
  [[nodiscard]] constexpr auto getMipCount() const { return m_mipCount; }
  [[nodiscard]] constexpr auto getLastMip() const {
    return m_firstMip + m_mipCount;
  }
  [[nodiscard]] constexpr auto getFirstLayer() const { return m_firstLayer; }
  [[nodiscard]] constexpr auto getLayerCount() const { return m_layerCount; }
  [[nodiscard]] constexpr auto getLastLayer() const {
    return m_firstLayer + m_layerCount;
  }
  [[nodiscard]] constexpr auto getAspectMask() const { return m_aspectMask; }

  constexpr bool operator==(const SubresourceRange &other) const {
    return m_firstMip == other.m_firstMip &&
           m_mipCount == other.m_mipCount &&
           m_firstLayer == other.m_firstLayer &&
           m_layerCount == other.m_layerCount &&
           m_aspectMask == other.m_aspectMask;
  }

private:
  uint32_t m_firstMip{0};
  uint32_t m_mipCount{1};
  uint32_t m_firstLayer{0};
  uint32_t m_layerCount{1};
  SubresourceDomain::Aspects m_aspectMask{SubresourceDomain::kAllAspects};
};

// Thrown by FrameGraph::compile() when a live pass reads a region of a
// transient resource that no live pass ever wrote.
class FrameGraphRangeError : public std::runtime_error {
public:
  FrameGraphRangeError(std::string message, std::string passName,
                       std::string resourceName, SubresourceRange missing)
      : std::runtime_error{std::move(message)}, m_passName{std::move(passName)},
        m_resourceName{std::move(resourceName)}, m_missing{missing} {}

  [[nodiscard]] const auto &getPassName() const { return m_passName; }
  [[nodiscard]] const auto &getResourceName() const { return m_resourceName; }
  [[nodiscard]] auto getMissingRange() const { return m_missing; }

private:
  std::string m_passName;
  std::string m_resourceName;
  SubresourceRange m_missing;
};

//
// Geometry helpers (all ranges are half-open boxes):
//

inline void validateDomain(const SubresourceDomain &d) {
  if (d.getMipCount() == 0 || d.getLayerCount() == 0 ||
      d.getAspectMask() == 0) {
    throw std::invalid_argument{"Invalid SubresourceDomain"};
  }
}

// Resolves the "all aspects" sentinel against the owning domain and rejects
// empty, out-of-bounds or aspect-illegal declarations.
inline SubresourceRange normalizeRange(const SubresourceDomain &d,
                                       const SubresourceRange &r) {
  if (r.getMipCount() == 0 || r.getLayerCount() == 0)
    throw std::invalid_argument{"Empty SubresourceRange"};
  if (r.getAspectMask() == 0)
    throw std::invalid_argument{"SubresourceRange with no aspect"};

  const auto aspects = r.getAspectMask() == SubresourceDomain::kAllAspects
                         ? d.getAspectMask()
                         : r.getAspectMask();
  if (aspects == 0 || (aspects & ~d.getAspectMask()) != 0)
    throw std::invalid_argument{
      "SubresourceRange uses aspects not present in the domain"};

  const auto mipEnd = r.getFirstMip() + r.getMipCount();
  const auto layerEnd = r.getFirstLayer() + r.getLayerCount();
  if (mipEnd > d.getMipCount() || layerEnd > d.getLayerCount() ||
      mipEnd < r.getFirstMip() || layerEnd < r.getFirstLayer()) {
    throw std::invalid_argument{
      "SubresourceRange exceeds the SubresourceDomain bounds"};
  }
  return SubresourceRange{r.getFirstMip(), r.getMipCount(),
                          r.getFirstLayer(), r.getLayerCount(), aspects};
}

inline bool rangesIntersect(const SubresourceRange &a,
                            const SubresourceRange &b) {
  return a.getFirstMip() < b.getLastMip() &&
         b.getFirstMip() < a.getLastMip() &&
         a.getFirstLayer() < b.getLastLayer() &&
         b.getFirstLayer() < a.getLastLayer() &&
         (a.getAspectMask() & b.getAspectMask()) != 0;
}

inline bool rangeContains(const SubresourceRange &outer,
                          const SubresourceRange &inner) {
  return outer.getFirstMip() <= inner.getFirstMip() &&
         inner.getLastMip() <= outer.getLastMip() &&
         outer.getFirstLayer() <= inner.getFirstLayer() &&
         inner.getLastLayer() <= outer.getLastLayer() &&
         (inner.getAspectMask() & ~outer.getAspectMask()) == 0;
}

// Returns the boxes that cover a \ b (at most 5 pieces).
inline std::vector<SubresourceRange>
subtractRange(const SubresourceRange &a, const SubresourceRange &b) {
  std::vector<SubresourceRange> out;
  if (!rangesIntersect(a, b)) {
    out.push_back(a);
    return out;
  }

  // Mip strips spanning the whole layer extent of a.
  if (b.getFirstMip() > a.getFirstMip())
    out.emplace_back(a.getFirstMip(), b.getFirstMip() - a.getFirstMip(),
                     a.getFirstLayer(), a.getLayerCount(), a.getAspectMask());
  if (b.getLastMip() < a.getLastMip())
    out.emplace_back(b.getLastMip(), a.getLastMip() - b.getLastMip(),
                     a.getFirstLayer(), a.getLayerCount(), a.getAspectMask());

  // Layer strips inside the overlapping mip span.
  const auto firstMip = std::max(a.getFirstMip(), b.getFirstMip());
  const auto lastMip = std::min(a.getLastMip(), b.getLastMip());
  if (b.getFirstLayer() > a.getFirstLayer())
    out.emplace_back(firstMip, lastMip - firstMip, a.getFirstLayer(),
                     b.getFirstLayer() - a.getFirstLayer(), a.getAspectMask());
  if (b.getLastLayer() < a.getLastLayer())
    out.emplace_back(firstMip, lastMip - firstMip, b.getLastLayer(),
                     a.getLastLayer() - b.getLastLayer(), a.getAspectMask());

  // Geometric overlap, only for aspects b does not cover.
  const auto aspectsOnlyInA = a.getAspectMask() & ~b.getAspectMask();
  if (aspectsOnlyInA != 0)
    out.emplace_back(firstMip, lastMip - firstMip,
                     std::max(a.getFirstLayer(), b.getFirstLayer()),
                     std::min(a.getLastLayer(), b.getLastLayer()) -
                       std::max(a.getFirstLayer(), b.getFirstLayer()),
                     aspectsOnlyInA);
  return out;
}
