#pragma once
#include <object/Triangle.hpp>

namespace SoftRasterizer {
class DisplacedMesh;

/*One micro triangle of a DisplacedMesh. It is an ordinary Triangle, so the
 *existing BVH and renderers hold it unchanged, but its model-space vertices
 *can be rewritten in place. Only the owning mesh commits validated edits; the
 *primitive address and stable identity never change across edits.*/
class MicroTri : public Triangle {
public:
  MicroTri(const std::array<Vertex, 3> &vertices,
           std::shared_ptr<Material> material, DisplacedMesh *owner,
           std::uint32_t face, std::uint32_t micro, std::uint64_t stableId);

  DisplacedMesh *owner() const { return m_owner; }
  std::uint32_t face() const { return m_face; }
  std::uint32_t micro() const { return m_micro; }

  /*Micro triangles store world-space positions and shading normals and always
   *keep the identity model matrix; the owning mesh refreshes them directly.*/
  void useWorldSpace() {
    setSourceNormalsAreWorld(true);
    setAlignNormalsToFace(false);
  }

  /*Stable identity: owner serial combined with the global micro ordinal. The
   *primitive address is also fixed for the object lifetime.*/
  std::uint64_t stableId() const override { return m_stableId; }
  const void *identity() const { return this; }

  /*Every micro triangle groups under its owning mesh in the two-level
   *emissive area tree.*/
  const Object *samplingOwner() const override;

private:
  DisplacedMesh *m_owner = nullptr;
  std::uint32_t m_face = 0, m_micro = 0;
  std::uint64_t m_stableId = 0;
};
} // namespace SoftRasterizer
