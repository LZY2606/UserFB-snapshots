#pragma once
#include <object/MicroTri.hpp>

namespace SoftRasterizer {
class DisplacedMesh;

/*Incremental counters charged where the work happens. Height edits charge
 *only their local dependency closure; construction and model-matrix updates
 *are bulk operations and are not charged here.*/
struct DisplaceStats {
  /*Height contributions read while re-averaging welded lattice points. A
   *point shared by k source faces reads all k candidate positions.*/
  std::uint64_t contributionReads = 0;
  /*Micro-geometry vertex records written. Each rewritten micro triangle
   *writes three.*/
  std::uint64_t vertexWrites = 0;
  /*Micro triangles re-triangulated and committed to their primitive.*/
  std::uint64_t triangleUpdates = 0;

  void reset() {
    *this = DisplaceStats{};
  }
};

/*A requested height sample: source face and barycentric lattice point in the
 *layout used by the constructor's height rows (row j, column i, i + j <= n).*/
struct HeightSampleRef {
  std::uint32_t face = 0;
  std::uint32_t i = 0, j = 0;
  float height = 0;
};

/*A complete per-face height array in row-major lattice order. At most one
 *patch per face is accepted in one batch.*/
struct FaceHeightPatch {
  std::uint32_t face = 0;
  std::vector<float> heights;
};

/*Persistent, incrementally editable displaced triangle mesh, usable as a
 *Scene Object.
 *
 *The constructor consumes shared indexed model-space positions and unit
 *normals (Vertex::texCoord is ignored), oriented manifold triangles, three
 *independent UVs per source face, one non-null material per face, one height
 *array per face and a single subdivision count n in [1,64]. Every source face
 *is split on the barycentric lattice (1-(i+j)/n, i/n, j/n) into exactly n^2
 *micro triangles.
 *
 *A lattice point's candidate position is the source face's linear position
 *plus its signed height times the normalized interpolated vertex normal.
 *Candidates for the same source vertex, or for the same rational point on the
 *same source edge, are averaged with equal weight over all incident source
 *faces, giving one geometry position. Interior lattice points belong only to
 *their own face and are never welded by floating-point position. UVs always
 *come from the current source face's three UVs, so the two sides of a UV seam
 *are never averaged. Micro geometry uses true geometric normals and true
 *world-space areas; the shading normal at a geometric micro vertex is the
 *normalized sum of the world-space area normals of its adjacent micro
 *triangles.*/
class DisplacedMesh : public Object {
public:
  DisplacedMesh(std::vector<Vertex> vertices, std::vector<glm::uvec3> faces,
                std::vector<std::array<glm::vec2, 3>> faceUVs,
                std::vector<std::shared_ptr<Material>> materials,
                std::vector<std::vector<float>> heights, std::uint32_t n);

  void setMaterial(std::shared_ptr<Material> material) override;
  void bindShader2Mesh(std::shared_ptr<Shader> shader) override;

  Bounds3 getBounds() const override;
  Intersection getIntersect(const Ray &ray) const override;
  float getArea() const override {
    return m_area;
  }
  SurfaceSample sample(Sampler &rng) const override;
  void appendRasterTriangles(std::vector<RasterTriangle> &out) const override;
  void appendPrimitives(std::vector<std::shared_ptr<Object>> &out) override;

  ObjectDelta consumeDelta() override;
  const Object *samplingOwner() const override {
    return this;
  }
  void validateMaterial() const override;
  std::uint64_t lightSignature() const override;

  std::uint32_t subdivision() const {
    return m_n;
  }
  std::size_t faceCount() const {
    return m_faces.size();
  }
  std::size_t microTriangleCount() const {
    return m_microTris.size();
  }
  std::size_t pointsPerFace() const {
    return m_pointsPerFace;
  }

  /*Current per-face height rows: row j, then i = 0..n-j.*/
  const std::vector<std::vector<float>> &heights() const {
    return m_heights;
  }

  /*Batch replacement. Every face/subscript index, every height and the final
   *state of every affected micro triangle is validated before anything is
   *committed. A rejected batch leaves both the geometry and the pending dirty
   *set untouched.*/
  void replaceHeights(const std::vector<HeightSampleRef> &samples);
  void replaceHeights(const std::vector<FaceHeightPatch> &patches);

  /*Counters of the last committed batch and of all committed batches.*/
  const DisplaceStats &lastStats() const {
    return m_lastStats;
  }
  const DisplaceStats &totalStats() const {
    return m_totalStats;
  }
  void resetStats() {
    m_lastStats = {};
    m_totalStats = {};
  }

  /*Stable leaf identity. The returned primitive address is fixed for the
   *object lifetime; face is the source face, micro the face-local index.*/
  const MicroTri *microTriangle(std::uint32_t face,
                                std::uint32_t micro) const;
  std::uint64_t identity() const {
    return m_identity;
  }

  /*Model-space position of a lattice point after displacement.*/
  glm::vec3 pointPosition(std::uint32_t face, std::uint32_t i,
                          std::uint32_t j) const;

private:
  struct Point {
    /*Welded global geometry vertex id.*/
    std::uint32_t globalVertex = 0;
  };

  void transform() override;
  void buildTopology();
  void validateTopology() const;
  void validateInterpolatedNormals() const;
  void computeModelGeometry();
  void computeWorldGeometryFor(const std::vector<std::uint32_t> &micros,
                               DisplaceStats *stats);
  void writeMicro(std::uint32_t micro, DisplaceStats *stats);
  void rebuildBoundTree();

  std::uint32_t latticeIndex(std::uint32_t i, std::uint32_t j) const;
  void latticeCoordinates(std::uint32_t index, std::uint32_t &i,
                          std::uint32_t &j) const;
  glm::vec3 linearPosition(std::uint32_t face, std::uint32_t index) const;
  glm::vec3 rawInterpolatedNormal(std::uint32_t face,
                                  std::uint32_t index) const;
  glm::vec3 candidatePosition(
      std::uint32_t face, std::uint32_t index,
      const std::vector<float> *overrideRows, std::size_t overrideFace) const;
  glm::vec3 proposedPosition(std::uint32_t globalVertex,
                             const std::vector<std::uint32_t> &overridePoints,
                             const std::vector<float> &overrideHeights) const;
  glm::vec3 proposalForFacePoint(std::uint32_t face, std::uint32_t index,
                                 const std::vector<std::uint32_t> &overridePoints,
                                 const std::vector<float> &overrideHeights) const;
  static float crossLength2(const glm::vec3 &a, const glm::vec3 &b,
                            const glm::vec3 &c);

  void applyHeightChanges(const std::vector<std::uint32_t> &pointIds,
                          const std::vector<float> &newHeights);

  /*Source data (model space).*/
  std::vector<Vertex> m_vertices;
  std::vector<glm::uvec3> m_faces;
  std::vector<std::array<glm::vec2, 3>> m_faceUVs;
  std::vector<std::shared_ptr<Material>> m_faceMaterials;
  std::vector<std::vector<float>> m_heights;
  std::uint32_t m_n = 1;
  std::size_t m_pointsPerFace = 0;

  /*Topology, fixed after construction.*/
  std::vector<Point> m_points; //face-major
  std::uint32_t m_globalVertexCount = 0;
  std::vector<std::vector<std::uint32_t>> m_vertexContributors; //global point
  std::vector<std::vector<std::uint32_t>> m_vertexMicros;       //micros
  std::vector<std::array<std::uint32_t, 3>> m_microVertexIds;
  std::vector<std::array<glm::vec2, 3>> m_microUVs;
  std::vector<std::uint32_t> m_microFace;
  std::vector<std::shared_ptr<MicroTri>> m_microTris;

  /*Committed geometry. Model positions are the authoritative lattice result;
   *world positions, per-micro world area normals and per-vertex world shading
   *normals are derived from them through the model matrix. MicroTri is kept at
   *the identity model matrix, so it stores world data directly and never
   *re-applies the transform.*/
  std::vector<glm::vec3> m_modelPositions; //per global vertex, model space
  std::vector<glm::vec3> m_worldPositions; //per global vertex, world space
  std::vector<glm::vec3> m_worldNormals;   //per global vertex, world shading
  std::vector<glm::vec3> m_microAreaNormals; //world space, unnormalized
  std::vector<float> m_microWorldAreas;

  /*Exact object bounds and area: a segment tree over micro world bounds and
   *areas. A leaf edit recomputes only its leaf-to-root path.*/
  std::size_t m_boundLeaves = 0;
  std::vector<Bounds3> m_boundTree;
  std::vector<float> m_areaTree;
  Bounds3 m_boundingBox;
  float m_area = 0;

  /*Pending incremental state consumed by Scene::prepare(). Generation stamps
   *avoid O(micros) rescans on every edit.*/
  std::vector<std::shared_ptr<Object>> m_pendingMicros;
  std::vector<std::uint32_t> m_microStamp;   //closure dedup per edit
  std::vector<std::uint32_t> m_vertexStamp;  //closure dedup per edit
  std::vector<std::uint32_t> m_pendingStamp; //pending-set membership
  std::uint32_t m_stamp = 0, m_pendingStampGeneration = 0;
  bool m_hasPendingDelta = false;
  DisplaceStats m_pendingStats, m_lastStats, m_totalStats;
  std::uint64_t m_identity = 0;
};
} // namespace SoftRasterizer
