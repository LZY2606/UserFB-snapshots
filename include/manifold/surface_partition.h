#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "manifold/manifold.h"
#include "manifold/mesh.h"

namespace manifold {

/**
 * @brief A common surface arrangement for up to four closed Manifold solids.
 *
 * The arrangement is built once from read-only inputs.  Every interface
 * triangle is owned by the partition and can be selected by Extract() without
 * intersecting or remeshing the operands a second time.
 */
class SurfacePartition {
 public:
  /// Construction/validation status.  Extract() is only usable when this is
  /// Ok and OperandCount() is non-zero.
  enum class Status {
    Ok,
    TooManyOperands,
    OperandError,
    NonFiniteInput,
    InvalidPropertyCount,
    UnsupportedGeometry,
    InternalError,
  };

  /// A directed triangle in the common arrangement.
  ///
  /// vertex indices refer to SurfacePartition::vertices().  The vertex order is
  /// the source triangle's outward order.  Consequently the source solid's
  /// interior is on the negative geometric-normal side.  minusMask contains
  /// the source operand's bit and plusMask does not; all other occupancy bits
  /// are identical on the two sides.
  struct Face {
    /// Indices into SurfacePartition::vertices(), in outward source order.
    std::array<uint64_t, 3> vertices = {0, 0, 0};

    /// Occupancy mask on the negative-normal/source-interior side.
    uint8_t minusMask = 0;

    /// Occupancy mask on the positive-normal/source-exterior side.
    uint8_t plusMask = 0;

    /// Zero-based source operand index.
    uint32_t sourceOperand = 0;

    /// Triangle index in SourceMeshes()[sourceOperand].
    uint64_t sourceTriangle = 0;

    /// Barycentric weights for each of the three directed corners.  Component
    /// j is the weight of source corner j of sourceTriangle.
    std::array<vec3, 3> sourceBarycentric{};
  };

  SurfacePartition();
  explicit SurfacePartition(const std::vector<Manifold>& operands);
  SurfacePartition(std::initializer_list<Manifold> operands);
  SurfacePartition(const Manifold& a);
  SurfacePartition(const Manifold& a, const Manifold& b);
  SurfacePartition(const Manifold& a, const Manifold& b, const Manifold& c);
  SurfacePartition(const Manifold& a, const Manifold& b, const Manifold& c,
                   const Manifold& d);

  SurfacePartition(const SurfacePartition&) = default;
  SurfacePartition(SurfacePartition&&) noexcept = default;
  SurfacePartition& operator=(const SurfacePartition&) = default;
  SurfacePartition& operator=(SurfacePartition&&) noexcept = default;
  ~SurfacePartition();

  Status GetStatus() const noexcept;
  bool Valid() const noexcept { return GetStatus() == Status::Ok; }
  explicit operator bool() const noexcept { return Valid(); }

  /// Number of operand slots, including empty Manifold operands.
  uint32_t OperandCount() const noexcept;

  /// Number of extra scalar channels shared by non-empty sources.
  uint32_t ExtraPropertyCount() const noexcept;

  /// Shared geometric vertices used by Face::vertices.
  const std::vector<vec3>& Vertices() const noexcept;

  /// Every arranged interface triangle, stored exactly once.
  const std::vector<Face>& Faces() const noexcept;

  /// Owned GetMeshGL64() snapshots taken during construction.
  const std::vector<MeshGL64>& SourceMeshes() const noexcept;

  /// Extracts the closed region whose exact occupancy mask is @p mask.
  ///
  /// Mask zero is the unbounded exterior and is rejected.  Unknown bits and
  /// invalid partitions are also rejected.  An empty, bounded mask returns an
  /// empty Manifold.  Throws std::invalid_argument for an invalid mask and
  /// std::runtime_error if the partition itself failed to construct.
  Manifold Extract(uint32_t mask) const;

  /// Non-throwing form of Extract().  On rejection/construction failure,
  /// @p out is assigned an empty Manifold and false is returned.
  bool TryExtract(uint32_t mask, Manifold& out) const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace manifold
