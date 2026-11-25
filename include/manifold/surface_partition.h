#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "manifold/manifold.h"
#include "manifold/mesh.h"

namespace manifold {

/**
 * @brief A common surface arrangement for up to four closed solids.
 *
 * Construction snapshots every input with GetMeshGL64(); the partition and
 * all Extract() results remain valid after the source Manifold objects are
 * destroyed.  Inputs are never modified.
 *
 * Supported inputs are valid, closed triangle solids with coordinates in
 * [-100, 100], source edges of at least 1e-3, and a longest-edge/minimum
 * altitude ratio no greater than 1000.  Non-convex solids, cavities, and
 * multiple disconnected components are supported.  Surface pairs may be
 * disjoint, nested, or transverse.  The intended generic-position limits are:
 * cross-section unit-normal cross product >= 1e-4, absolute determinant of
 * the three normals at a three-solid event >= 1e-4, and event/edge separation
 * >= 1e-6.  Coplanar overlap, tangencies, intersection lines through source
 * vertices, source-edge/source-edge crossings, and four-surface common points
 * are rejected.
 *
 * The partition stores one copy of every subdivided source-surface triangle.
 * For a stored triangle, winding follows the source surface's outward
 * orientation.  The negative-normal side is described by minusMask and the
 * positive-normal side by plusMask.  Bit `i` is set exactly when operand `i`
 * contains that side's point.  The two masks therefore differ in exactly the
 * source operand's bit.  Mask zero denotes the unbounded exterior and is not
 * extracted as a closed solid.
 *
 * Extract(mask) selects stored triangles whose minusMask or plusMask is
 * @p mask and reverses winding where necessary.  It performs no intersection
 * or repartition.  Extra scalar properties are interpolated from the original
 * source-triangle corners using Triangle::sourceBarycentric; geometry and
 * property vertices remain separate so native seams are preserved.  Output
 * MeshGL64 runs retain runOriginalID, runTransform, and faceID associations.
 */
class SurfacePartition {
 public:
  enum class Error {
    NoError,
    TooManyOperands,
    InvalidOperand,
    NonFiniteInput,
    PropertyCountMismatch,
    InvalidGeometry,
    UnsupportedArrangement,
    EmptyMaskIsUnbounded,
    NoSuchMask,
  };

  /// A directed, conforming triangle in the common arrangement.
  struct Triangle {
    /// Indices into SurfacePartition::vertices(), in source-outward winding.
    std::array<uint64_t, 3> vertices = {0, 0, 0};

    /// Occupancy on the negative side of the source triangle normal.
    uint32_t minusMask = 0;
    /// Occupancy on the positive side of the source triangle normal.
    uint32_t plusMask = 0;

    /// Operand that supplied the source surface.
    uint32_t sourceOperand = 0;
    /// Triangle index in SourceMesh(sourceOperand).
    uint64_t sourceTriangle = 0;

    /// Barycentric coordinates relative to the three source triangle corners.
    /// sourceBarycentric[j] corresponds to arrangement corner j.
    std::array<vec3, 3> sourceBarycentric = {vec3(0.0), vec3(0.0), vec3(0.0)};
  };

  SurfacePartition() = default;
  explicit SurfacePartition(const std::vector<Manifold>& operands);
  SurfacePartition(std::initializer_list<Manifold> operands);

  /// Build a partition.  On failure, this object is left unchanged and the
  /// returned error describes the failure.
  Error Build(const std::vector<Manifold>& operands);

  Error error() const { return error_; }
  const std::string& errorMessage() const { return message_; }
  bool ok() const { return error_ == Error::NoError; }

  size_t OperandCount() const { return operands_; }
  int ExtraPropertyCount() const { return extraProps_; }

  /// Shared geometric vertices used by Triangle::vertices.
  const std::vector<vec3>& Vertices() const { return vertices_; }
  /// Every interface triangle, stored exactly once.
  const std::vector<Triangle>& Triangles() const { return triangles_; }
  /// Independent GetMeshGL64() snapshots taken during construction.
  const MeshGL64& SourceMesh(uint32_t operand) const;

  /**
   * @brief Extract the closed region with exact occupancy @p mask.
   * @param mask Bits 0..3 identify input solids.  Mask zero is rejected.
   * @param error Optional status; receives the rejection reason instead of
   * throwing when supplied.
   * @return The region, or an empty Manifold for a legal but unoccupied mask.
   * If @p error is null and the mask/partition is invalid, throws
   * std::invalid_argument.
   */
  Manifold Extract(uint32_t mask, Error* error = nullptr) const;

  /// Convenience wrapper that throws std::invalid_argument for mask zero or
  /// an invalid partition.  Empty occupancy still returns an empty Manifold.
  Manifold ExtractChecked(uint32_t mask) const;

 private:
  Error error_ = Error::NoError;
  std::string message_;
  size_t operands_ = 0;
  int extraProps_ = 0;
  uint32_t validMask_ = 0;
  std::vector<vec3> vertices_;
  std::vector<Triangle> triangles_;
  std::vector<MeshGL64> sourceMeshes_;
};

const char* ToString(SurfacePartition::Error error);

}  // namespace manifold
