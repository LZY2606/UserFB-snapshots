#include <object/DisplacedMesh.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>

namespace SoftRasterizer {
namespace {
std::uint64_t nextMeshIdentity() {
  static std::uint64_t counter = 0;
  return ++counter;
}

std::uint64_t hashFloat(std::uint64_t h, float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  h ^= bits;
  return h * 0x9e3779b97f4a7c15ULL + (h >> 29);
}

glm::vec3 areaNormalOf(const glm::vec3 &a, const glm::vec3 &b,
                       const glm::vec3 &c) {
  return glm::cross(b - a, c - a);
}
} // namespace

std::uint32_t DisplacedMesh::latticeIndex(std::uint32_t i,
                                          std::uint32_t j) const {
  /*Row j starts after the rows 0..j-1, which hold n+1, n, ... elements:
   *sum_{k<j} (n-k+1) = j*(2n - j + 3)/2.*/
  return j * (2 * m_n - j + 3) / 2 + i;
}

void DisplacedMesh::latticeCoordinates(std::uint32_t index, std::uint32_t &i,
                                       std::uint32_t &j) const {
  j = 0;
  std::uint32_t rowStart = 0;
  while (rowStart + (m_n - j + 1) <= index) {
    rowStart += m_n - j + 1;
    ++j;
  }
  i = index - rowStart;
}

glm::vec3 DisplacedMesh::linearPosition(std::uint32_t face,
                                        std::uint32_t index) const {
  std::uint32_t i, j;
  latticeCoordinates(index, i, j);
  const auto &f = m_faces[face];
  const float n = float(m_n), w1 = float(i) / n, w2 = float(j) / n,
              w0 = 1.f - w1 - w2;
  return w0 * m_vertices[f.x].position + w1 * m_vertices[f.y].position +
         w2 * m_vertices[f.z].position;
}

glm::vec3 DisplacedMesh::rawInterpolatedNormal(std::uint32_t face,
                                               std::uint32_t index) const {
  std::uint32_t i, j;
  latticeCoordinates(index, i, j);
  const auto &f = m_faces[face];
  const float n = float(m_n), w1 = float(i) / n, w2 = float(j) / n,
              w0 = 1.f - w1 - w2;
  return w0 * m_vertices[f.x].normal + w1 * m_vertices[f.y].normal +
         w2 * m_vertices[f.z].normal;
}

glm::vec3 DisplacedMesh::candidatePosition(
    std::uint32_t face, std::uint32_t index,
    const std::vector<float> *overrideRows, std::size_t overrideFace) const {
  const float height = (overrideRows && overrideFace == face &&
                        !overrideRows->empty())
                           ? (*overrideRows)[index]
                           : m_heights[face][index];
  return linearPosition(face, index) +
         height * unit(rawInterpolatedNormal(face, index));
}

float DisplacedMesh::crossLength2(const glm::vec3 &a, const glm::vec3 &b,
                                  const glm::vec3 &c) {
  const auto cross = areaNormalOf(a, b, c);
  return glm::dot(cross, cross);
}

void DisplacedMesh::buildTopology() {
  const std::size_t faceCount_ = m_faces.size();
  m_points.assign(faceCount_ * m_pointsPerFace, Point{});

  std::vector<std::uint32_t> cornerId(m_vertices.size());
  std::uint32_t nextVertex = 0;
  for (auto &id : cornerId) {
    id = nextVertex++;
  }
  std::map<std::pair<std::uint32_t, std::uint32_t>,
           std::vector<std::uint32_t>>
      edgeTable;
  for (std::size_t face = 0; face < faceCount_; ++face) {
    const auto &f = m_faces[face];
    for (std::uint32_t j = 0; j <= m_n; ++j) {
      for (std::uint32_t i = 0; i + j <= m_n; ++i) {
        Point &point = m_points[face * m_pointsPerFace + latticeIndex(i, j)];
        if (i == 0 && j == 0) {
          point.globalVertex = cornerId[f.x];
        } else if (j == 0 && i == m_n) {
          point.globalVertex = cornerId[f.y];
        } else if (i == 0 && j == m_n) {
          point.globalVertex = cornerId[f.z];
        } else if (j == 0) {
          /*Edge x -> y, parameter i in (0, n).*/
          const std::uint32_t low = std::min(f.x, f.y),
                            high = std::max(f.x, f.y);
          const std::uint32_t t = (f.x < f.y) ? i : (m_n - i);
          auto &table = edgeTable[{low, high}];
          if (table.empty()) {
            table.assign(m_n + 1, UINT32_MAX);
          }
          if (table[t] == UINT32_MAX) {
            table[t] = nextVertex++;
          }
          point.globalVertex = table[t];
        } else if (i == 0) {
          /*Edge x -> z, parameter j in (0, n).*/
          const std::uint32_t low = std::min(f.x, f.z),
                            high = std::max(f.x, f.z);
          const std::uint32_t t = (f.x < f.z) ? j : (m_n - j);
          auto &table = edgeTable[{low, high}];
          if (table.empty()) {
            table.assign(m_n + 1, UINT32_MAX);
          }
          if (table[t] == UINT32_MAX) {
            table[t] = nextVertex++;
          }
          point.globalVertex = table[t];
        } else if (i + j == m_n) {
          /*Edge y -> z, parameter j in (0, n).*/
          const std::uint32_t low = std::min(f.y, f.z),
                            high = std::max(f.y, f.z);
          const std::uint32_t t = (f.y < f.z) ? j : (m_n - j);
          auto &table = edgeTable[{low, high}];
          if (table.empty()) {
            table.assign(m_n + 1, UINT32_MAX);
          }
          if (table[t] == UINT32_MAX) {
            table[t] = nextVertex++;
          }
          point.globalVertex = table[t];
        } else {
          point.globalVertex = nextVertex++;
        }
      }
    }
  }
  m_globalVertexCount = nextVertex;

  m_vertexContributors.assign(m_globalVertexCount, {});
  m_vertexMicros.assign(m_globalVertexCount, {});
  for (std::size_t face = 0; face < faceCount_; ++face) {
    for (std::size_t local = 0; local < m_pointsPerFace; ++local) {
      const std::uint32_t global =
          m_points[face * m_pointsPerFace + local].globalVertex;
      m_vertexContributors[global].push_back(
          static_cast<std::uint32_t>(face * m_pointsPerFace + local));
    }
  }

  for (std::size_t face = 0; face < faceCount_; ++face) {
    const auto &uv = m_faceUVs[face];
    auto addMicro = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
      const std::array<std::uint32_t, 3> local{a, b, c};
      std::array<std::uint32_t, 3> ids{};
      std::array<glm::vec2, 3> microUV{};
      for (int k = 0; k < 3; ++k) {
        ids[k] = m_points[face * m_pointsPerFace + local[k]].globalVertex;
        std::uint32_t ci, cj;
        latticeCoordinates(local[k], ci, cj);
        const float w1 = float(ci) / float(m_n), w2 = float(cj) / float(m_n),
                    w0 = 1.f - w1 - w2;
        microUV[k] = w0 * uv[0] + w1 * uv[1] + w2 * uv[2];
      }
      const std::uint32_t micro =
          static_cast<std::uint32_t>(m_microFace.size());
      m_microVertexIds.push_back(ids);
      m_microUVs.push_back(microUV);
      m_microFace.push_back(static_cast<std::uint32_t>(face));
      m_microTris.push_back(std::make_shared<MicroTri>(
          std::array<Vertex, 3>{}, m_faceMaterials[face], this,
          static_cast<std::uint32_t>(face), micro,
          (m_identity << 32) | micro));
      for (int k = 0; k < 3; ++k) {
        m_vertexMicros[ids[k]].push_back(micro);
      }
    };
    for (std::uint32_t j = 0; j < m_n; ++j) {
      for (std::uint32_t i = 0; i + j < m_n; ++i) {
        const std::uint32_t p00 = latticeIndex(i, j);
        const std::uint32_t p10 = latticeIndex(i + 1, j);
        const std::uint32_t p01 = latticeIndex(i, j + 1);
        /*Lower micro triangle always exists for i + j <= n - 1.*/
        addMicro(p00, p10, p01);
        /*Upper micro triangle needs (i+1, j+1), i.e. i + j <= n - 2.*/
        if (i + j + 2 <= m_n) {
          const std::uint32_t p11 = latticeIndex(i + 1, j + 1);
          addMicro(p10, p11, p01);
        }
      }
    }
  }
}

void DisplacedMesh::validateTopology() const {
  /*Every directed source edge must be unique; an undirected edge is used by
   *at most two faces, in opposite directions (oriented manifold).*/
  std::map<std::pair<std::uint32_t, std::uint32_t>, int> directed;
  for (const auto &f : m_faces) {
    for (int k = 0; k < 3; ++k) {
      const std::uint32_t a = f[k], b = f[(k + 1) % 3];
      if (a == b) {
        throw std::invalid_argument("degenerate source edge");
      }
      if (++directed[{a, b}] > 1) {
        throw std::invalid_argument(
            "non-manifold or inconsistent source orientation");
      }
    }
  }
  for (const auto &[edge, count] : directed) {
    const auto reverse = directed.find({edge.second, edge.first});
    if (1 + (reverse != directed.end() ? 1 : 0) > 2) {
      throw std::invalid_argument("non-manifold source edge");
    }
  }
}

void DisplacedMesh::validateInterpolatedNormals() const {
  for (std::size_t face = 0; face < m_faces.size(); ++face) {
    for (std::size_t local = 0; local < m_pointsPerFace; ++local) {
      const auto normal = rawInterpolatedNormal(
          static_cast<std::uint32_t>(face), static_cast<std::uint32_t>(local));
      if (!finite(normal) || glm::dot(normal, normal) < 1e-12f) {
        throw std::invalid_argument("zero interpolated normal");
      }
    }
  }
}

void DisplacedMesh::computeModelGeometry() {
  /*Equal-weight average of every adjacent source face's candidate position.
   *m_vertexContributors holds face-major point ids.*/
  m_modelPositions.assign(m_globalVertexCount, glm::vec3(0));
  for (std::uint32_t v = 0; v < m_globalVertexCount; ++v) {
    const auto &contributors = m_vertexContributors[v];
    glm::vec3 sum(0);
    for (std::uint32_t packed : contributors) {
      const std::uint32_t face = packed / m_pointsPerFace;
      const std::uint32_t local = packed % m_pointsPerFace;
      sum += candidatePosition(face, local, nullptr, 0);
    }
    m_modelPositions[v] = sum / float(contributors.size());
    if (!finite(m_modelPositions[v])) {
      throw std::invalid_argument("displaced mesh position overflow");
    }
  }
}

void DisplacedMesh::computeWorldGeometryFor(
    const std::vector<std::uint32_t> &micros, DisplaceStats *stats) {
  (void)stats;
  /*World positions of every vertex referenced by the given micros.*/
  const auto model = m_model;
  std::vector<std::uint32_t> touchedVertices;
  for (std::uint32_t micro : micros) {
    for (std::uint32_t id : m_microVertexIds[micro]) {
      touchedVertices.push_back(id);
    }
  }
  std::sort(touchedVertices.begin(), touchedVertices.end());
  touchedVertices.erase(std::unique(touchedVertices.begin(),
                                    touchedVertices.end()),
                        touchedVertices.end());
  for (std::uint32_t v : touchedVertices) {
    m_worldPositions[v] = glm::vec3(model * glm::vec4(m_modelPositions[v], 1));
    if (!finite(m_worldPositions[v])) {
      throw std::invalid_argument("displaced mesh transform overflow");
    }
  }
  /*Per-micro world area normals for the given micros, then rebuild the
   *world-space shading sum at every vertex those micros touch (removing the
   *old contribution first so incremental calls stay exact).*/
  for (std::uint32_t micro : micros) {
    const auto &ids = m_microVertexIds[micro];
    const glm::vec3 cross =
        areaNormalOf(m_worldPositions[ids[0]], m_worldPositions[ids[1]],
                     m_worldPositions[ids[2]]);
    const glm::vec3 old = m_microAreaNormals[micro];
    m_microAreaNormals[micro] = cross;
    m_microWorldAreas[micro] = 0.5f * glm::length(cross);
    for (std::uint32_t v : ids) {
      m_worldNormals[v] += cross - old;
    }
  }
  for (std::uint32_t v : touchedVertices) {
    m_worldNormals[v] = unit(m_worldNormals[v], glm::vec3(0, 1, 0));
  }
}

void DisplacedMesh::writeMicro(std::uint32_t micro, DisplaceStats *stats) {
  /*MicroTri has the identity model matrix and receives world-space positions
   *and shading normals directly.*/
  const auto &ids = m_microVertexIds[micro];
  std::array<Vertex, 3> vertices;
  for (int k = 0; k < 3; ++k) {
    vertices[k].position = m_worldPositions[ids[k]];
    vertices[k].normal = m_worldNormals[ids[k]];
    vertices[k].texCoord = m_microUVs[micro][k];
  }
  m_microTris[micro]->resetSource(vertices, m_faceMaterials[m_microFace[micro]]);
  if (stats) {
    stats->vertexWrites += 3;
    ++stats->triangleUpdates;
  }
}

void DisplacedMesh::rebuildBoundTree() {
  m_boundLeaves = m_microTris.size();
  m_boundTree.assign(2 * m_boundLeaves, Bounds3{});
  m_areaTree.assign(2 * m_boundLeaves, 0.f);
  for (std::size_t i = 0; i < m_boundLeaves; ++i) {
    m_boundTree[m_boundLeaves + i] = m_microTris[i]->getBounds();
    m_areaTree[m_boundLeaves + i] = m_microTris[i]->getArea();
  }
  for (std::size_t node = m_boundLeaves; node-- > 1;) {
    m_boundTree[node] = m_boundTree[2 * node];
    m_boundTree[node].expand(m_boundTree[2 * node + 1]);
    m_areaTree[node] = m_areaTree[2 * node] + m_areaTree[2 * node + 1];
  }
  m_boundingBox = m_boundLeaves ? m_boundTree[1] : Bounds3{};
  m_area = m_boundLeaves ? m_areaTree[1] : 0.f;
  if (!std::isfinite(m_area)) {
    throw std::invalid_argument("displaced mesh area overflow");
  }
}

DisplacedMesh::DisplacedMesh(std::vector<Vertex> vertices,
                             std::vector<glm::uvec3> faces,
                             std::vector<std::array<glm::vec2, 3>> faceUVs,
                             std::vector<std::shared_ptr<Material>> materials,
                             std::vector<std::vector<float>> heights,
                             std::uint32_t n)
    : m_vertices(std::move(vertices)), m_faces(std::move(faces)),
      m_faceUVs(std::move(faceUVs)), m_faceMaterials(std::move(materials)),
      m_heights(std::move(heights)), m_n(n) {
  if (m_n < 1 || m_n > 64) {
    throw std::invalid_argument("subdivision must be in [1,64]");
  }
  m_identity = nextMeshIdentity();
  if (m_faces.empty()) {
    throw std::invalid_argument("mesh needs at least one face");
  }
  if (m_faceUVs.size() != m_faces.size()) {
    throw std::invalid_argument("UV/face count mismatch");
  }
  if (m_faceMaterials.size() != m_faces.size()) {
    throw std::invalid_argument("material/face count mismatch");
  }
  if (m_heights.size() != m_faces.size()) {
    throw std::invalid_argument("height/face count mismatch");
  }
  m_pointsPerFace = std::size_t(m_n + 1) * (m_n + 2) / 2;
  for (const auto &vertex : m_vertices) {
    if (!finite(vertex.position) || !finite(vertex.normal)) {
      throw std::invalid_argument("invalid source vertex");
    }
    const float length2 = glm::dot(vertex.normal, vertex.normal);
    if (std::abs(length2 - 1.f) > 1e-3f) {
      throw std::invalid_argument("source normals must be unit length");
    }
  }
  for (std::size_t face = 0; face < m_faces.size(); ++face) {
    const auto &triangle = m_faces[face];
    if (triangle.x >= m_vertices.size() || triangle.y >= m_vertices.size() ||
        triangle.z >= m_vertices.size()) {
      throw std::invalid_argument("invalid face index");
    }
    if (triangle.x == triangle.y || triangle.y == triangle.z ||
        triangle.x == triangle.z) {
      throw std::invalid_argument("degenerate face index");
    }
    if (!m_faceMaterials[face]) {
      throw std::invalid_argument("null face material");
    }
    validateMaterialFields(*m_faceMaterials[face]);
    for (const auto &uv : m_faceUVs[face]) {
      if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        throw std::invalid_argument("invalid face UV");
      }
    }
    if (m_heights[face].size() != m_pointsPerFace) {
      throw std::invalid_argument("height array size mismatch");
    }
    for (float h : m_heights[face]) {
      if (!std::isfinite(h)) {
        throw std::invalid_argument("invalid height sample");
      }
    }
    const auto &a = m_vertices[triangle.x].position;
    const auto &b = m_vertices[triangle.y].position;
    const auto &c = m_vertices[triangle.z].position;
    if (!(crossLength2(a, b, c) > 1e-24f)) {
      throw std::invalid_argument("degenerate source face");
    }
  }
  m_material = m_faceMaterials.front();
  validateTopology();
  buildTopology();
  validateInterpolatedNormals();
  computeModelGeometry();
  /*Construction must produce valid micro triangles too.*/
  for (std::size_t micro = 0; micro < m_microVertexIds.size(); ++micro) {
    const auto &ids = m_microVertexIds[micro];
    if (!(crossLength2(m_modelPositions[ids[0]], m_modelPositions[ids[1]],
                       m_modelPositions[ids[2]]) > 1e-24f)) {
      throw std::invalid_argument("degenerate micro triangle at construction");
    }
  }
  /*MicroTris hold world-space data, so keep their model matrix at identity.*/
  for (auto &tri : m_microTris) {
    tri->useWorldSpace();
    tri->setModelMatrix(glm::mat4(1));
  }
  m_microStamp.assign(m_microTris.size(), 0);
  m_vertexStamp.assign(m_globalVertexCount, 0);
  m_pendingStamp.assign(m_microTris.size(), 0);
  m_pendingStampGeneration = 0;
  m_worldPositions.assign(m_globalVertexCount, glm::vec3(0));
  m_worldNormals.assign(m_globalVertexCount, glm::vec3(0));
  m_microAreaNormals.assign(m_microTris.size(), glm::vec3(0));
  m_microWorldAreas.assign(m_microTris.size(), 0.f);
  std::vector<std::uint32_t> allMicros(m_microTris.size());
  for (std::size_t i = 0; i < allMicros.size(); ++i)
    allMicros[i] = static_cast<std::uint32_t>(i);
  computeWorldGeometryFor(allMicros, nullptr);
  for (std::size_t micro = 0; micro < m_microTris.size(); ++micro) {
    writeMicro(static_cast<std::uint32_t>(micro), nullptr);
  }
  rebuildBoundTree();
  /*The initial construction is a full state, not a pending edit.*/
  m_hasPendingDelta = false;
}

void DisplacedMesh::transform() {
  /*Model-matrix changes may touch every primitive; this is the only bulk path
   *besides construction. MicroTri stays at identity and receives refreshed
   *world-space data. This is a full recompute, so the accumulated shading
   *sums are reset first.*/
  for (auto &tri : m_microTris) {
    tri->useWorldSpace();
    tri->setModelMatrix(glm::mat4(1));
  }
  std::fill(m_worldNormals.begin(), m_worldNormals.end(), glm::vec3(0));
  std::fill(m_microAreaNormals.begin(), m_microAreaNormals.end(),
            glm::vec3(0));
  std::vector<std::uint32_t> allMicros(m_microTris.size());
  for (std::size_t i = 0; i < allMicros.size(); ++i)
    allMicros[i] = static_cast<std::uint32_t>(i);
  computeWorldGeometryFor(allMicros, nullptr);
  for (std::size_t micro = 0; micro < m_microTris.size(); ++micro) {
    writeMicro(static_cast<std::uint32_t>(micro), nullptr);
  }
  rebuildBoundTree();
  m_pendingMicros.clear();
  std::fill(m_pendingStamp.begin(), m_pendingStamp.end(), 0);
  m_pendingStampGeneration = 0;
  m_hasPendingDelta = false;
  ++m_revision;
}

SoftRasterizer::Bounds3 DisplacedMesh::getBounds() const {
  return m_boundingBox;
}

SoftRasterizer::Intersection
DisplacedMesh::getIntersect(const Ray &ray) const {
  Intersection nearest;
  for (const auto &micro : m_microTris) {
    Ray limited = ray;
    limited.tMax = std::min(ray.tMax, nearest.intersect_time);
    auto hit = micro->getIntersect(limited);
    if (hit.intersected) {
      nearest = hit;
    }
  }
  return nearest;
}

SoftRasterizer::SurfaceSample DisplacedMesh::sample(Sampler &rng) const {
  if (m_area <= 0) {
    return {};
  }
  const float target =
      std::min(rng.next() * m_area, std::nextafter(m_area, 0.f));
  float accumulated = 0;
  for (const auto &micro : m_microTris) {
    accumulated += micro->getArea();
    if (target < accumulated) {
      auto result = micro->sample(rng);
      result.pdfArea = 1 / m_area;
      return result;
    }
  }
  return {};
}

void DisplacedMesh::appendRasterTriangles(
    std::vector<RasterTriangle> &out) const {
  for (const auto &micro : m_microTris) {
    micro->appendRasterTriangles(out);
  }
}

void DisplacedMesh::appendPrimitives(
    std::vector<std::shared_ptr<Object>> &out) {
  for (const auto &micro : m_microTris) {
    if (micro->getArea() > 0) {
      out.push_back(micro);
    }
  }
}

void DisplacedMesh::setMaterial(std::shared_ptr<Material> material) {
  if (!material) {
    throw std::invalid_argument("null material");
  }
  Object::setMaterial(material);
  for (auto &faceMaterial : m_faceMaterials) {
    faceMaterial = material;
  }
  for (std::size_t micro = 0; micro < m_microTris.size(); ++micro) {
    m_microTris[micro]->setMaterial(material);
  }
}

void DisplacedMesh::bindShader2Mesh(std::shared_ptr<Shader> shader) {
  Object::bindShader2Mesh(shader);
  for (auto &micro : m_microTris) {
    micro->bindShader2Mesh(shader);
  }
}

void DisplacedMesh::validateMaterial() const {
  for (const auto &material : m_faceMaterials) {
    if (!material) {
      throw std::invalid_argument("null face material");
    }
    validateMaterialFields(*material);
  }
}

std::uint64_t DisplacedMesh::lightSignature() const {
  /*Only material identity and emission affect light-tree membership. The area
   *itself is refreshed by the incremental area-tree update, so it must not be
   *hashed here; otherwise every height edit would force a full tree rebuild.*/
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const auto &material : m_faceMaterials) {
    h ^= std::hash<const void *>{}(material.get());
    h = hashFloat(h, material->emission.x);
    h = hashFloat(h, material->emission.y);
    h = hashFloat(h, material->emission.z);
  }
  return h;
}

const SoftRasterizer::MicroTri *
DisplacedMesh::microTriangle(std::uint32_t face, std::uint32_t micro) const {
  const std::size_t perFace = std::size_t(m_n) * m_n;
  if (face >= m_faces.size() || micro >= perFace) {
    return nullptr;
  }
  return m_microTris[std::size_t(face) * perFace + micro].get();
}

glm::vec3 DisplacedMesh::pointPosition(std::uint32_t face, std::uint32_t i,
                                       std::uint32_t j) const {
  if (face >= m_faces.size() || i + j > m_n) {
    throw std::invalid_argument("invalid lattice point");
  }
  return m_modelPositions[m_points[face * m_pointsPerFace + latticeIndex(i, j)]
                              .globalVertex];
}

namespace {
/*Two-phase edit: compute the whole proposed state, validate it, then commit.
 *Nothing touches the mesh until every check passes.*/
} // namespace

void DisplacedMesh::applyHeightChanges(
    const std::vector<std::uint32_t> &pointIds,
    const std::vector<float> &newHeights) {
  if (pointIds.empty()) {
    return;
  }
  /*Sort point ids together with their new heights so lookups stay aligned.*/
  struct Change {
    std::uint32_t point;
    float height;
  };
  std::vector<Change> changes(pointIds.size());
  for (std::size_t i = 0; i < pointIds.size(); ++i) {
    changes[i] = {pointIds[i], newHeights[i]};
  }
  std::sort(changes.begin(), changes.end(),
            [](const Change &a, const Change &b) { return a.point < b.point; });
  std::vector<std::uint32_t> sortedPoints(changes.size());
  std::vector<float> sortedHeights(changes.size());
  for (std::size_t i = 0; i < changes.size(); ++i) {
    sortedPoints[i] = changes[i].point;
    sortedHeights[i] = changes[i].height;
  }

  /*Phase 1 - dependency closure, computed only from the touched points:
   *  changed vertices    = welded vertices owning those points;
   *  geometry micros     = every micro using a changed vertex;
   *  normal vertices     = all vertices of geometry micros (their area sum
   *                        changes);
   *  affected micros     = micros using a normal vertex;
   *  affected faces      = faces owning any affected micro.*/
  /*Closure dedup uses generation stamps. The stamp arrays are sized once and
   *never cleared, so an edit costs only its own closure. Stamps reserve 0 as
   *"empty", and a rare 32-bit wrap-before-clear reseeds them.*/
  if (m_stamp == 0 || m_stamp == std::numeric_limits<std::uint32_t>::max()) {
    m_microStamp.assign(m_microTris.size(), 0);
    m_vertexStamp.assign(m_globalVertexCount, 0);
  }
  ++m_stamp;
  const std::uint32_t stamp = m_stamp;

  std::vector<std::uint32_t> changedVertices;
  std::vector<std::uint32_t> geometryMicros;
  for (std::uint32_t packed : sortedPoints) {
    const std::uint32_t vertex = m_points[packed].globalVertex;
    if (m_vertexStamp[vertex] != stamp) {
      m_vertexStamp[vertex] = stamp;
      changedVertices.push_back(vertex);
    }
    for (std::uint32_t micro : m_vertexMicros[vertex]) {
      if (m_microStamp[micro] != stamp) {
        m_microStamp[micro] = stamp;
        geometryMicros.push_back(micro);
      }
    }
  }
  std::sort(changedVertices.begin(), changedVertices.end());
  std::sort(geometryMicros.begin(), geometryMicros.end());

  std::vector<std::uint32_t> normalVertices;
  for (std::uint32_t micro : geometryMicros) {
    for (std::uint32_t vertex : m_microVertexIds[micro]) {
      if (m_vertexStamp[vertex] != stamp) {
        m_vertexStamp[vertex] = stamp;
        normalVertices.push_back(vertex);
      }
    }
  }
  std::sort(normalVertices.begin(), normalVertices.end());
  std::vector<std::uint32_t> affectedMicros = geometryMicros;
  for (std::uint32_t vertex : normalVertices) {
    for (std::uint32_t micro : m_vertexMicros[vertex]) {
      if (m_microStamp[micro] != stamp) {
        m_microStamp[micro] = stamp;
        affectedMicros.push_back(micro);
      }
    }
  }
  std::sort(affectedMicros.begin(), affectedMicros.end());

  DisplaceStats stats;

  /*Phase 2 - proposed positions of the changed vertices. A welded vertex
   *reads every contributor, including faces not named in this batch.*/
  std::vector<glm::vec3> proposedPositions(changedVertices.size());
  for (std::size_t k = 0; k < changedVertices.size(); ++k) {
    const auto &contributors = m_vertexContributors[changedVertices[k]];
    glm::vec3 sum(0);
    for (std::uint32_t packed : contributors) {
      const std::uint32_t face = packed / m_pointsPerFace;
      const std::uint32_t local = packed % m_pointsPerFace;
      float height = m_heights[face][local];
      auto it = std::lower_bound(sortedPoints.begin(), sortedPoints.end(),
                                 packed);
      if (it != sortedPoints.end() && *it == packed) {
        height = sortedHeights[it - sortedPoints.begin()];
      }
      sum += linearPosition(face, local) +
             height * unit(rawInterpolatedNormal(face, local));
      ++stats.contributionReads;
    }
    proposedPositions[k] = sum / float(contributors.size());
    if (!finite(proposedPositions[k])) {
      throw std::invalid_argument("edit produced a non-finite position");
    }
  }
  auto proposedPosition = [&](std::uint32_t vertex) {
    auto it = std::lower_bound(changedVertices.begin(), changedVertices.end(),
                               vertex);
    if (it != changedVertices.end() && *it == vertex) {
      return proposedPositions[it - changedVertices.begin()];
    }
    return m_modelPositions[vertex];
  };

  /*Phase 3 - validate every affected micro triangle against the proposed
   *world-space geometry before committing anything.*/
  auto proposedWorld = [&](std::uint32_t vertex) {
    return glm::vec3(m_model * glm::vec4(proposedPosition(vertex), 1));
  };
  struct MicroState {
    glm::vec3 areaNormal;
    float area;
  };
  std::vector<MicroState> microStates(affectedMicros.size());
  for (std::size_t m = 0; m < affectedMicros.size(); ++m) {
    const auto &ids = m_microVertexIds[affectedMicros[m]];
    const glm::vec3 a = proposedWorld(ids[0]);
    const glm::vec3 b = proposedWorld(ids[1]);
    const glm::vec3 c = proposedWorld(ids[2]);
    const glm::vec3 areaNormal = areaNormalOf(a, b, c);
    const float area = 0.5f * glm::length(areaNormal);
    if (!finite(a) || !finite(b) || !finite(c) ||
        !(glm::dot(areaNormal, areaNormal) > 1e-24f) ||
        !std::isfinite(area)) {
      throw std::invalid_argument("edit would degenerate a micro triangle");
    }
    microStates[m] = {areaNormal, area};
  }

  /*Phase 4 - commit. Model positions first, then the world positions and
   *world shading sums for the touched vertices and affected micros, then
   *rewrite the affected micro triangles.*/
  for (std::size_t k = 0; k < changedVertices.size(); ++k) {
    m_modelPositions[changedVertices[k]] = proposedPositions[k];
    m_worldPositions[changedVertices[k]] =
        glm::vec3(m_model * glm::vec4(proposedPositions[k], 1));
  }
  for (std::size_t m = 0; m < affectedMicros.size(); ++m) {
    const std::uint32_t micro = affectedMicros[m];
    const glm::vec3 oldNormal = m_microAreaNormals[micro];
    m_microAreaNormals[micro] = microStates[m].areaNormal;
    m_microWorldAreas[micro] = microStates[m].area;
    for (std::uint32_t vertex : m_microVertexIds[micro]) {
      m_worldNormals[vertex] += microStates[m].areaNormal - oldNormal;
    }
  }
  for (std::uint32_t vertex : normalVertices) {
    m_worldNormals[vertex] = unit(m_worldNormals[vertex], glm::vec3(0, 1, 0));
  }
  for (std::uint32_t micro : affectedMicros) {
    writeMicro(micro, &stats);
  }

  /*Commit heights.*/
  for (std::size_t k = 0; k < sortedPoints.size(); ++k) {
    const std::uint32_t face = sortedPoints[k] / m_pointsPerFace;
    const std::uint32_t local = sortedPoints[k] % m_pointsPerFace;
    m_heights[face][local] = sortedHeights[k];
  }

  /*Refresh exact bounds and area along the affected leaf-to-root paths. All
   *leaves are written first so a sibling update cannot leave an ancestor
   *stale; a shared ancestor is recomputed at most once per batch.*/
  if (!m_boundTree.empty()) {
    std::vector<char> touched(m_boundTree.size(), 0);
    for (std::uint32_t micro : affectedMicros) {
      const std::size_t leaf = m_boundLeaves + micro;
      m_boundTree[leaf] = m_microTris[micro]->getBounds();
      m_areaTree[leaf] = m_microTris[micro]->getArea();
    }
    for (std::uint32_t micro : affectedMicros) {
      std::size_t node = (m_boundLeaves + micro) / 2;
      while (node >= 1) {
        if (touched[node]) {
          break;
        }
        touched[node] = 1;
        Bounds3 combined = m_boundTree[2 * node];
        combined.expand(m_boundTree[2 * node + 1]);
        m_boundTree[node] = combined;
        m_areaTree[node] = m_areaTree[2 * node] + m_areaTree[2 * node + 1];
        if (node == 1) {
          break;
        }
        node /= 2;
      }
    }
    m_boundingBox = m_boundLeaves ? m_boundTree[1] : Bounds3{};
    m_area = m_boundLeaves ? m_areaTree[1] : 0.f;
  }
  if (!std::isfinite(m_area)) {
    throw std::invalid_argument("displaced mesh area overflow");
  }

  /*Record the dirty set for the next prepare(), deduplicated against
   *everything still pending from earlier edits in this frame.*/
  if (m_pendingStamp.size() != m_microTris.size()) {
    m_pendingStamp.assign(m_microTris.size(), 0);
  }
  if (m_pendingStampGeneration ==
      std::numeric_limits<std::uint32_t>::max()) {
    std::fill(m_pendingStamp.begin(), m_pendingStamp.end(), 0);
    m_pendingStampGeneration = 0;
  }
  const std::uint32_t pendingStamp = ++m_pendingStampGeneration;
  for (std::uint32_t micro : affectedMicros) {
    if (m_pendingStamp[micro] != pendingStamp) {
      m_pendingStamp[micro] = pendingStamp;
      m_pendingMicros.push_back(m_microTris[micro]);
    }
  }

  m_pendingStats.contributionReads += stats.contributionReads;
  m_pendingStats.vertexWrites += stats.vertexWrites;
  m_pendingStats.triangleUpdates += stats.triangleUpdates;
  m_hasPendingDelta = true;

  m_lastStats = stats;
  m_totalStats.contributionReads += stats.contributionReads;
  m_totalStats.vertexWrites += stats.vertexWrites;
  m_totalStats.triangleUpdates += stats.triangleUpdates;
  ++m_revision;
}

void DisplacedMesh::replaceHeights(
    const std::vector<HeightSampleRef> &samples) {
  if (samples.empty()) {
    return;
  }
  std::vector<char> faceSeen(m_faces.size(), 0);
  std::vector<std::uint64_t> pointSeen;
  std::vector<std::uint32_t> pointIds;
  std::vector<float> newHeights;
  pointIds.reserve(samples.size());
  newHeights.reserve(samples.size());
  for (const auto &sample : samples) {
    if (sample.face >= m_faces.size()) {
      throw std::invalid_argument("invalid height face index");
    }
    if (sample.i + sample.j > m_n) {
      throw std::invalid_argument("invalid height lattice subscript");
    }
    if (!std::isfinite(sample.height)) {
      throw std::invalid_argument("invalid height value");
    }
    const std::uint32_t local = latticeIndex(sample.i, sample.j);
    const std::uint32_t packed =
        sample.face * static_cast<std::uint32_t>(m_pointsPerFace) + local;
    pointIds.push_back(packed);
    newHeights.push_back(sample.height);
    (void)faceSeen;
  }
  /*Reject duplicate samples for the same point; applying two values would be
   *ambiguous.*/
  {
    std::vector<std::uint32_t> sorted = pointIds;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      throw std::invalid_argument("duplicate height sample");
    }
  }
  applyHeightChanges(pointIds, newHeights);
}

void DisplacedMesh::replaceHeights(
    const std::vector<FaceHeightPatch> &patches) {
  if (patches.empty()) {
    return;
  }
  std::vector<char> faceSeen(m_faces.size(), 0);
  std::vector<std::uint32_t> pointIds;
  std::vector<float> newHeights;
  for (const auto &patch : patches) {
    if (patch.face >= m_faces.size()) {
      throw std::invalid_argument("invalid height face index");
    }
    if (faceSeen[patch.face]) {
      throw std::invalid_argument("duplicate face in height batch");
    }
    faceSeen[patch.face] = 1;
    if (patch.heights.size() != m_pointsPerFace) {
      throw std::invalid_argument("height patch size mismatch");
    }
    for (std::uint32_t local = 0; local < m_pointsPerFace; ++local) {
      if (!std::isfinite(patch.heights[local])) {
        throw std::invalid_argument("invalid height value");
      }
      pointIds.push_back(patch.face *
                             static_cast<std::uint32_t>(m_pointsPerFace) +
                         local);
      newHeights.push_back(patch.heights[local]);
    }
  }
  applyHeightChanges(pointIds, newHeights);
}

SoftRasterizer::ObjectDelta DisplacedMesh::consumeDelta() {
  ObjectDelta delta;
  if (!m_hasPendingDelta || m_pendingMicros.empty()) {
    m_hasPendingDelta = false;
    m_pendingMicros.clear();
      std::fill(m_pendingStamp.begin(), m_pendingStamp.end(), 0);
    m_pendingStampGeneration = 0;
    m_pendingStats = {};
    return delta;
  }
  delta.canUpdateIncrementally = true;
  delta.structurePreserved = true;
  delta.dirtyPrimitives = m_pendingMicros;
  delta.dirtyRasterTriangles.reserve(m_pendingMicros.size());
  for (const auto &primitive : m_pendingMicros) {
    std::vector<RasterTriangle> stream;
    primitive->appendRasterTriangles(stream);
    if (!stream.empty()) {
      delta.dirtyRasterTriangles.push_back(std::move(stream.front()));
    } else {
      delta.dirtyRasterTriangles.push_back({});
    }
  }
  delta.counters.contributionReads = m_pendingStats.contributionReads;
  delta.counters.vertexWrites = m_pendingStats.vertexWrites;
  delta.counters.triangleUpdates = m_pendingStats.triangleUpdates;
  m_hasPendingDelta = false;
  m_pendingMicros.clear();
  std::fill(m_pendingStamp.begin(), m_pendingStamp.end(), 0);
  m_pendingStampGeneration = 0;
  m_pendingStats = {};
  return delta;
}
} // namespace SoftRasterizer
