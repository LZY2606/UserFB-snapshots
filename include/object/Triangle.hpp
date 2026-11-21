#pragma once
#include <object/Object.hpp>

namespace SoftRasterizer {
class Triangle : public Object {
public:
  explicit Triangle(
      const std::array<Vertex, 3> &vertices,
      std::shared_ptr<Material> material = std::make_shared<Material>());
  Bounds3 getBounds() const override;
  Intersection getIntersect(const Ray &ray) const override;

  /*World-space vertices after the model transform; source-space data is
   *available through sourceVertices().*/
  const std::array<Vertex, 3> &vertices() const {
    return m_vertices;
  }

  const std::array<Vertex, 3> &sourceVertices() const {
    return m_sourceVertices;
  }

  /*Replace the model-space triangle and refresh world data in place. The
   *derived owner validates the new data first; this is the single write used
   *by incremental mesh edits.*/
  void resetSource(const std::array<Vertex, 3> &vertices,
                   std::shared_ptr<Material> material);

  float getArea() const override {
    return m_area;
  }

  SurfaceSample sample(Sampler &rng) const override;

  void
  appendRasterTriangles(std::vector<RasterTriangle> &stream) const override;

protected:
  /*When false, shading normals keep the direction supplied by the owner
   *instead of being flipped toward the geometric face normal. A displaced
   *surface needs this because its shading normals are the true area-weighted
   *normals of adjacent micro triangles.*/
  void setAlignNormalsToFace(bool align) {
    m_alignNormalsToFace = align;
  }
  /*When true, source normals are already world-space and are only normalized;
   *the inverse-transpose model normal transform is skipped.*/
  void setSourceNormalsAreWorld(bool world) {
    m_sourceNormalsAreWorld = world;
  }

private:
  /*Original vertices remain in model space.*/
  std::array<Vertex, 3> m_sourceVertices;
  /*World-space vertices are shared by rasterization and ray queries.*/
  std::array<Vertex, 3> m_vertices;
  glm::vec3 m_faceNormal{0, 1, 0};
  float m_area = 0;
  bool m_alignNormalsToFace = true;
  bool m_sourceNormalsAreWorld = false;
  void transform() override;
};
} // namespace SoftRasterizer
