#include <object/DisplacedMesh.hpp>
#include <object/MicroTri.hpp>

SoftRasterizer::MicroTri::MicroTri(const std::array<Vertex, 3> &vertices,
                                   std::shared_ptr<Material> material,
                                   DisplacedMesh *owner, std::uint32_t face,
                                   std::uint32_t micro, std::uint64_t stableId)
    : Triangle(vertices, std::move(material)), m_owner(owner), m_face(face),
      m_micro(micro), m_stableId(stableId) {
  /*Displaced shading normals already come from adjacent micro areas; do not
   *flip them toward the geometric face normal.*/
  setAlignNormalsToFace(false);
}

const SoftRasterizer::Object *SoftRasterizer::MicroTri::samplingOwner() const {
  return m_owner;
}
