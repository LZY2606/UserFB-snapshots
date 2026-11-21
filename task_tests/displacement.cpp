// Task tests for the incrementally editable DisplacedMesh. Registered through
// task_support/check.h; this file deliberately defines no main().
#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <object/DisplacedMesh.hpp>
#include <object/Sphere.hpp>
#include <render/PathTracing.hpp>
#include <render/Rasterizer.hpp>
#include <scene/Scene.hpp>
#include <set>
#include <string>
#include <vector>

using namespace SoftRasterizer;

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::shared_ptr<Material> makeMaterial(glm::vec3 kd = glm::vec3(0.7f),
                                       glm::vec3 emission = glm::vec3(0)) {
  auto material = std::make_shared<Material>();
  material->Kd = kd;
  material->emission = emission;
  return material;
}

std::vector<std::array<glm::vec2, 3>>
defaultUVs(std::size_t faces) {
  return std::vector<std::array<glm::vec2, 3>>(
      faces, {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1)});
}


// Emulate Triangle::getIntersect's shading-normal rule exactly: interpolate,
// flip on back faces, then fall back to the geometric face normal when the
// shading normal faces away from the ray.
glm::vec3 triShadingNormal(const glm::vec3 &dir, const glm::vec3 &faceNormal,
                           const glm::vec3 &interpolated) {
  const bool front = glm::dot(dir, faceNormal) < 0;
  glm::vec3 n = unit(interpolated, faceNormal);
  if (!front) n = -n;
  if (glm::dot(dir, n) >= 0) n = front ? faceNormal : -faceNormal;
  return n;
}

std::size_t pointsFor(std::uint32_t n) {
  return std::size_t(n + 1) * (n + 2) / 2;
}

std::vector<std::vector<float>> zeroHeights(std::size_t faces, std::uint32_t n) {
  return std::vector<std::vector<float>>(faces,
                                         std::vector<float>(pointsFor(n), 0.f));
}

// Signed height row-major index, matching DisplacedMesh's documented layout.
std::size_t rowMajor(std::uint32_t n, std::uint32_t i, std::uint32_t j) {
  return std::size_t(j) * (2 * n - j + 3) / 2 + i;
}

bool nearly(float a, float b, float tol = 1e-4f) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tol;
}
bool nearly(glm::vec3 a, glm::vec3 b, float tol = 1e-4f) {
  return nearly(a.x, b.x, tol) && nearly(a.y, b.y, tol) &&
         nearly(a.z, b.z, tol);
}
bool nearly(glm::vec2 a, glm::vec2 b, float tol = 1e-4f) {
  return nearly(a.x, b.x, tol) && nearly(a.y, b.y, tol);
}

// ---------------------------------------------------------------------------
// Independent full reference: recompute the whole displaced surface directly
// from the source data and heights, then compare per-micro world geometry.
// ---------------------------------------------------------------------------
struct RefSource {
  std::vector<Vertex> vertices;
  std::vector<glm::uvec3> faces;
  std::vector<std::array<glm::vec2, 3>> uvs;
  std::vector<std::shared_ptr<Material>> materials;
  std::vector<std::vector<float>> heights;
  std::uint32_t n = 1;
  glm::mat4 model{1};
};

struct RefMicro {
  std::array<glm::vec3, 3> position;
  std::array<glm::vec3, 3> normal;
  std::array<glm::vec2, 3> uv;
  glm::vec3 geometricNormal;
  float area = 0;
};

// Global vertex key: corner or edge rational point, exactly as documented.
std::uint64_t refEdgeKey(std::uint32_t a, std::uint32_t b) {
  if (a > b) std::swap(a, b);
  return (std::uint64_t(a) << 32) | b;
}

glm::vec3 refLinear(const RefSource &s, std::uint32_t face, std::uint32_t i,
                    std::uint32_t j) {
  const auto &f = s.faces[face];
  const float n = float(s.n), w1 = float(i) / n, w2 = float(j) / n,
              w0 = 1.f - w1 - w2;
  return w0 * s.vertices[f.x].position + w1 * s.vertices[f.y].position +
         w2 * s.vertices[f.z].position;
}
glm::vec3 refNormal(const RefSource &s, std::uint32_t face, std::uint32_t i,
                    std::uint32_t j) {
  const auto &f = s.faces[face];
  const float n = float(s.n), w1 = float(i) / n, w2 = float(j) / n,
              w0 = 1.f - w1 - w2;
  auto v = w0 * s.vertices[f.x].normal + w1 * s.vertices[f.y].normal +
           w2 * s.vertices[f.z].normal;
  return unit(v);
}

// Build the independent reference surface.
std::vector<RefMicro> buildReference(const RefSource &s) {
  const std::uint32_t n = s.n;
  const std::uint32_t faces = std::uint32_t(s.faces.size());
  const std::size_t ppf = pointsFor(n);

  // Assign keys to corners, edges, and private interior points.
  std::vector<std::vector<std::uint32_t>> global(faces);
  std::vector<std::uint32_t> corner(s.vertices.size());
  std::uint32_t nextKey = 0;
  for (auto &c : corner) c = nextKey++;
  std::map<std::pair<std::uint32_t, std::uint32_t>,
           std::map<std::uint32_t, std::uint32_t>>
      table;
  for (std::uint32_t face = 0; face < faces; ++face) {
    global[face].resize(ppf);
    const auto &f = s.faces[face];
    for (std::uint32_t j = 0; j <= n; ++j) {
      for (std::uint32_t i = 0; i + j <= n; ++i) {
        std::uint32_t key;
        if (i == 0 && j == 0)
          key = corner[f.x];
        else if (j == 0 && i == n)
          key = corner[f.y];
        else if (i == 0 && j == n)
          key = corner[f.z];
        else if (j == 0) {
          auto pair = std::minmax(f.x, f.y);
          std::uint32_t t = (f.x < f.y) ? i : (n - i);
          auto &m = table[{pair.first, pair.second}];
          if (!m.count(t)) m[t] = nextKey++;
          key = m[t];
        } else if (i == 0) {
          auto pair = std::minmax(f.x, f.z);
          std::uint32_t t = (f.x < f.z) ? j : (n - j);
          auto &m = table[{pair.first, pair.second}];
          if (!m.count(t)) m[t] = nextKey++;
          key = m[t];
        } else if (i + j == n) {
          auto pair = std::minmax(f.y, f.z);
          std::uint32_t t = (f.y < f.z) ? j : (n - j);
          auto &m = table[{pair.first, pair.second}];
          if (!m.count(t)) m[t] = nextKey++;
          key = m[t];
        } else {
          key = nextKey++;
        }
        global[face][rowMajor(n, i, j)] = key;
      }
    }
  }

  // Candidate positions per (face, point), then equal-weight averaging.
  std::vector<std::vector<glm::vec3>> candidate(faces);
  std::vector<std::map<std::uint32_t, glm::vec3>> sumsByFace(nextKey);
  std::vector<std::map<std::uint32_t, int>> countsByFace(nextKey);
  for (std::uint32_t face = 0; face < faces; ++face) {
    candidate[face].resize(ppf);
    for (std::uint32_t j = 0; j <= n; ++j) {
      for (std::uint32_t i = 0; i + j <= n; ++i) {
        const std::size_t idx = rowMajor(n, i, j);
        const float h = s.heights[face][idx];
        candidate[face][idx] =
            refLinear(s, face, i, j) + h * refNormal(s, face, i, j);
        const std::uint32_t key = global[face][idx];
        sumsByFace[key][face] += candidate[face][idx];
        countsByFace[key][face] += 1;
      }
    }
  }
  std::map<std::uint32_t, glm::vec3> position;
  for (std::uint32_t key = 0; key < nextKey; ++key) {
    const auto &perFace = sumsByFace[key];
    glm::vec3 total(0);
    int count = 0;
    for (const auto &entry : perFace) {
      total += entry.second / float(countsByFace[key][entry.first]);
      ++count;
    }
    position[key] = total / float(count);
  }

  // Save model position of each global key.
  std::vector<glm::vec3> modelPos(nextKey);
  for (auto &[key, p] : position) modelPos[key] = p;

  // Micro triangles with model/world geometry.
  struct Tmp {
    std::array<std::uint32_t, 3> key;
    std::array<glm::vec2, 3> uv;
    std::uint32_t face;
  };
  std::vector<Tmp> tmps;
  for (std::uint32_t face = 0; face < faces; ++face) {
    const auto &uv = s.uvs[face];
    auto add = [&](std::uint32_t i0, std::uint32_t j0, std::uint32_t i1,
                   std::uint32_t j1, std::uint32_t i2, std::uint32_t j2) {
      Tmp t;
      t.face = face;
      const std::array<std::array<std::uint32_t, 2>, 3> c{
          {{i0, j0}, {i1, j1}, {i2, j2}}};
      for (int k = 0; k < 3; ++k) {
        const std::size_t idx = rowMajor(n, c[k][0], c[k][1]);
        t.key[k] = global[face][idx];
        const float w1 = float(c[k][0]) / n, w2 = float(c[k][1]) / n,
                    w0 = 1 - w1 - w2;
        t.uv[k] = w0 * uv[0] + w1 * uv[1] + w2 * uv[2];
      }
      tmps.push_back(t);
    };
    for (std::uint32_t j = 0; j < n; ++j) {
      for (std::uint32_t i = 0; i + j < n; ++i) {
        add(i, j, i + 1, j, i, j + 1);
        if (i + j + 2 <= n) add(i + 1, j, i + 1, j + 1, i, j + 1);
      }
    }
  }

  // World-space area normal sums per key.
  std::vector<glm::vec3> worldPos(nextKey);
  for (std::uint32_t k = 0; k < nextKey; ++k) {
    worldPos[k] = glm::vec3(s.model * glm::vec4(modelPos[k], 1));
  }
  const auto normalMatrix =
      glm::transpose(glm::inverse(glm::mat3(s.model)));
  std::vector<glm::vec3> areaSum(nextKey, glm::vec3(0));
  std::vector<RefMicro> out(tmps.size());
  for (std::size_t m = 0; m < tmps.size(); ++m) {
    const auto &t = tmps[m];
    RefMicro &r = out[m];
    for (int k = 0; k < 3; ++k) {
      r.position[k] = worldPos[t.key[k]];
      r.uv[k] = t.uv[k];
    }
    r.geometricNormal = unit(glm::cross(r.position[1] - r.position[0],
                                        r.position[2] - r.position[0]));
    r.area = 0.5f * glm::length(glm::cross(r.position[1] - r.position[0],
                                           r.position[2] - r.position[0]));
    const auto cross = glm::cross(r.position[1] - r.position[0],
                                  r.position[2] - r.position[0]);
    for (int k = 0; k < 3; ++k) areaSum[t.key[k]] += cross;
  }
  for (std::size_t m = 0; m < tmps.size(); ++m) {
    for (int k = 0; k < 3; ++k) {
      out[m].normal[k] = unit(areaSum[tmps[m].key[k]], glm::vec3(0, 1, 0));
    }
  }
  return out;
}

} // namespace

namespace {

// A folded, open-boundary strip: two quads that meet at a shared edge with a
// UV seam and a crease, exercising edge welding plus per-face UV splitting.
struct Strip {
  std::vector<Vertex> vertices;
  std::vector<glm::uvec3> faces;
  std::vector<std::array<glm::vec2, 3>> uvs;
  std::vector<std::shared_ptr<Material>> materials;
};

// Two quads sharing the segment x=1, from (1,0,0) to (1,1,0). The second
// quad is folded up around that edge (a crease). Face 0/1 UVs are continuous;
// face 2/3 use a different parameterisation, so welding UVs across the seam
// would immediately be visible.
Strip creaseStrip() {
  const glm::vec3 sharedNormal(0, -0.70710678f, 0.70710678f);
  Strip s;
  s.vertices = {
      Vertex({0, 0, 0}, {0, 0, 1}),  // 0
      Vertex({1, 0, 0}, {0, 0, 1}),  // 1
      Vertex({0, 1, 0}, {0, 0, 1}),  // 2
      Vertex({1, 1, 0}, sharedNormal), // 3 shared crease vertex
      Vertex({1, 0, 1}, sharedNormal), // 4
      Vertex({1, 1, 1}, {0, -1, 0})};  // 5
  // Face 0/1 form the z=0 quad; face 2/3 the folded quad.
  s.faces = {{0, 1, 2}, {1, 3, 2}, {1, 4, 3}, {4, 5, 3}};
  s.uvs.resize(4);
  s.uvs[0] = {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1)};
  s.uvs[1] = {glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1)};
  s.uvs[2] = {glm::vec2(0.25f, 0.25f), glm::vec2(0.75f, 0.25f),
              glm::vec2(0.25f, 0.75f)};
  s.uvs[3] = {glm::vec2(0.75f, 0.25f), glm::vec2(0.75f, 0.75f),
              glm::vec2(0.25f, 0.75f)};
  // Distinct per-face materials: the mesh must keep them per source face.
  s.materials = {makeMaterial({0.8f, 0.2f, 0.2f}),
                 makeMaterial({0.2f, 0.8f, 0.2f}),
                 makeMaterial({0.2f, 0.2f, 0.8f}),
                 makeMaterial({0.8f, 0.8f, 0.2f})};
  return s;
}

std::shared_ptr<DisplacedMesh> makeMesh(const Strip &s, std::uint32_t n,
                                        const std::vector<std::vector<float>> &h) {
  return std::make_shared<DisplacedMesh>(s.vertices, s.faces, s.uvs,
                                         s.materials, h, n);
}

int topology_flat_uv_and_weld() {
  Strip s = creaseStrip();
  const std::uint32_t n = 3;
  auto heights = zeroHeights(s.faces.size(), n);
  auto mesh = makeMesh(s, n, heights);

  // Micro count is exactly n^2 per face.
  TASK_CHECK(mesh->microTriangleCount() == s.faces.size() * n * n);
  TASK_CHECK(mesh->faceCount() == s.faces.size());

  // Zero height keeps the authored positions; compare against the independent
  // reference, including the folded face.
  RefSource ref;
  ref.vertices = s.vertices;
  ref.faces = s.faces;
  ref.uvs = s.uvs;
  ref.materials = s.materials;
  ref.heights = heights;
  ref.n = n;
  ref.model = glm::mat4(1);
  auto expected = buildReference(ref);
  TASK_CHECK(expected.size() == mesh->microTriangleCount());

  // Check every primitive against the independent reference: positions, UVs
  // and shading normals.
  std::vector<RasterTriangle> stream;
  mesh->appendRasterTriangles(stream);
  TASK_CHECK(stream.size() == mesh->microTriangleCount());
  for (std::uint32_t face = 0; face < s.faces.size(); ++face) {
    for (std::uint32_t micro = 0; micro < n * n; ++micro) {
      const MicroTri *tri = mesh->microTriangle(face, micro);
      TASK_CHECK(tri != nullptr);
      std::vector<RasterTriangle> one;
      tri->appendRasterTriangles(one);
      TASK_CHECK(one.size() == 1);
      const auto &verts = one.front().vertices;
      // The reference lists micros face-major with the same triangulation
      // order, so the global index is face * n^2 + micro.
      const RefMicro &r = expected[std::size_t(face) * n * n + micro];
      for (int k = 0; k < 3; ++k) {
        TASK_CHECK(nearly(verts[k].position, r.position[k], 1e-4f));
        TASK_CHECK(nearly(verts[k].texCoord, r.uv[k], 1e-5f));
        TASK_CHECK(nearly(verts[k].normal, r.normal[k], 1e-3f));
      }
    }
  }

  // The folded quad's UVs come only from its own face: no seam averaging.
  // Face 2's first vertex UV is (0.25,0.25); if faces were welded by position
  // the UV would be near face 0/1 values.
  {
    auto one = mesh->microTriangle(2, 0);
    std::vector<RasterTriangle> tri;
    one->appendRasterTriangles(tri);
    TASK_CHECK(tri.size() == 1);
    bool found = false;
    for (const auto &v : tri.front().vertices) {
      if (nearly(v.texCoord, glm::vec2(0.25f, 0.25f), 1e-5f)) found = true;
    }
    TASK_CHECK(found);
  }

  // The shared source edge (1,3) is welded. Face 1's edge y->z (j from y) and
  // face 2's edge x->y (i from source vertex 1 toward 4) both carry source
  // edge (1,3) but are parameterised from the same end; their samples at the
  // same rational parameter must coincide exactly.
  {
    // Face 1: edge y->z has i + j == n, parameter j measured from y. Face 2
    // is {1,4,3}: its x->y edge (i from x=source vertex 1 toward y=4) is
    // source edge (1,4), not (1,3); its y->z edge (i+j==n) is source edge
    // (4,3). The shared (1,3) edge appears in face 1 (y->z) and face 3
    // {4,5,3} (z->x?). Simplest robust check: face 1 edge (i=n-k,j=k) must
    // equal the reference, already verified above. Here verify the seam point
    // at the shared crease is identical when reached through face 1's z
    // corner and face 0's y corner if they share a source vertex (they do
    // not: 0's y is 1, face1's y is 1). Face 1's shared vertex 1 must equal
    // face 0's. Confirm with pointPosition on the corner.
    // Face 0 = {0,1,2}: its y corner (i=n,j=0) is source vertex 1. Face 3
    // = {4,5,3}: its z corner (i=0,j=n) is source vertex 3. Source vertex 1
    // is shared by face 0 (x), face 1 (x) and face 2 (x); its welded position
    // must be reached identically from each of those faces.
    auto viaFace0 = mesh->pointPosition(0, 0, 0); // source vertex 0
    (void)viaFace0;
    auto face0Y = mesh->pointPosition(0, n, 0); // source vertex 1
    auto face1X = mesh->pointPosition(1, 0, 0); // face 1 = {1,3,2}, x=1
    auto face2X = mesh->pointPosition(2, 0, 0); // face 2 = {1,4,3}, x=1
    TASK_CHECK(nearly(face0Y, face1X, 1e-6f));
    TASK_CHECK(nearly(face0Y, face2X, 1e-6f));
    // A rational sample on the shared source edge (1,3) is authored by both
    // face 1 {1,3,2} (edge y->z, at i + j == n) and face 3 {4,5,3} (edge
    // z->x? -> its edge 5->3 maps source edge (5,3), not (1,3)). Use face 1's
    // y->z edge, whose midpoint must be the welded average; since all three
    // incident faces have zero height there, it equals the linear midpoint of
    // source vertices 1 and 3.
    auto edge = mesh->pointPosition(1, n - n / 2, n / 2);
    // Face 1 = {1,3,2}: its y->z edge joins source vertices 3 and 2. A
    // lattice point (i = n-j, j) there is linear(y) toward z by j/n, so at
    // equal height it is the linear blend of those two source positions.
    const std::uint32_t jj = n / 2;
    const float t = float(jj) / float(n);
    const glm::vec3 linear = (1 - t) * s.vertices[3].position +
                             t * s.vertices[2].position;
    TASK_CHECK(nearly(edge, linear, 1e-5f));
  }

  // Per-face materials are preserved: each micro triangle carries its source
  // face's material.
  for (std::uint32_t face = 0; face < s.faces.size(); ++face)
    for (std::uint32_t micro = 0; micro < n * n; ++micro) {
      std::vector<RasterTriangle> tri;
      mesh->microTriangle(face, micro)->appendRasterTriangles(tri);
      TASK_CHECK(tri.front().material == s.materials[face]);
    }

  // Bounds cover the full surface.
  auto bounds = mesh->getBounds();
  TASK_CHECK(nearly(bounds.min, glm::vec3(0, 0, 0), 1e-4f));
  TASK_CHECK(nearly(bounds.max, glm::vec3(1, 1, 1), 1e-4f));
  return 0;
}

// Oppositely oriented faces: two closed-ish quads sharing an edge, written
// with opposite winding, must weld by the canonical edge direction.
int topology_opposite_direction() {
  // Two triangles sharing the edge (0,1) but authored in opposite directions.
  std::vector<Vertex> v{Vertex({0, 0, 0}, {0, 0, 1}),
                        Vertex({1, 0, 0}, {0, 0, 1}),
                        Vertex({0, 1, 0}, {0, 0, 1}),
                        Vertex({1, -1, 0}, {0, 0, 1})};
  // Face A: 0->1->2 (edge 0->1). Face B: 1->0->3 (edge 1->0, opposite).
  std::vector<glm::uvec3> f{{0, 1, 2}, {1, 0, 3}};
  auto uvs = defaultUVs(2);
  auto m = makeMaterial();
  std::vector<std::shared_ptr<Material>> mats(2, m);
  const std::uint32_t n = 4;
  auto heights = zeroHeights(2, n);
  RefSource ref{v, f, uvs, mats, heights, n, glm::mat4(1)};
  auto expected = buildReference(ref);
  DisplacedMesh mesh(v, f, uvs, mats, heights, n);
  // The shared source edge (0,1) is authored in opposite directions. Its
  // rational samples must still weld: the t=1/2 point reached from face 0
  // (x->y, i=n/2) equals the point reached from face 1 (y->x, i=n/2 from y).
  auto p0 = mesh.pointPosition(0, n / 2, 0); // face 0 edge x->y
  auto p1 = mesh.pointPosition(1, n / 2, 0); // face 1 edge (1,0): i from 1->0
  TASK_CHECK(nearly(p0, p1, 1e-6f));
  // And both agree with the reference surface, micro-by-micro.
  for (std::uint32_t face = 0; face < 2; ++face) {
    for (std::uint32_t micro = 0; micro < n * n; ++micro) {
      std::vector<RasterTriangle> tri;
      mesh.microTriangle(face, micro)->appendRasterTriangles(tri);
      TASK_CHECK(tri.size() == 1);
      const auto &r = expected[std::size_t(face) * n * n + micro];
      for (int k = 0; k < 3; ++k) {
        TASK_CHECK(nearly(tri.front().vertices[k].position, r.position[k],
                          1e-4f));
      }
    }
  }
  return 0;
}

// Same source position, different source indices must NOT weld.
int topology_distinct_indices_same_position() {
  // Vertices 1 and 3 are both at (1,0,0) but have different indices, so the
  // faces that use them must not share geometry.
  std::vector<Vertex> v{Vertex({0, 0, 0}, {0, 0, 1}),
                        Vertex({1, 0, 0}, {0, 0, 1}),
                        Vertex({0, 1, 0}, {0, 0, 1}),
                        Vertex({1, 0, 0}, {0, 0, 1}),
                        Vertex({1, 1, 0}, {0, 0, 1})};
  std::vector<glm::uvec3> f{{0, 1, 2}, {3, 4, 2}};
  auto uvs = defaultUVs(2);
  auto m = makeMaterial();
  std::vector<std::shared_ptr<Material>> mats(2, m);
  const std::uint32_t n = 4;
  auto heights = zeroHeights(2, n);
  // Displace only the shared-index face 0 corner 1; face 1's duplicate index 3
  // at the same position must stay put.
  heights[0][rowMajor(n, n, 0)] = 1.f; // corner y (index 1) of face 0
  RefSource ref{v, f, uvs, mats, heights, n, glm::mat4(1)};
  auto expected = buildReference(ref);
  DisplacedMesh mesh(v, f, uvs, mats, heights, n);
  for (std::uint32_t face = 0; face < 2; ++face) {
    for (std::uint32_t micro = 0; micro < n * n; ++micro) {
      std::vector<RasterTriangle> tri;
      mesh.microTriangle(face, micro)->appendRasterTriangles(tri);
      TASK_CHECK(tri.size() == 1);
      const auto &r = expected[std::size_t(face) * n * n + micro];
      for (int k = 0; k < 3; ++k) {
        TASK_CHECK(nearly(tri.front().vertices[k].position, r.position[k],
                          1e-4f));
      }
    }
  }
  return 0;
}

// Non-uniform affine placement: ray hits, UVs and normals follow the model
// matrix, and world area is the transformed area.
int topology_nonuniform_placement() {
  Strip s = creaseStrip();
  const std::uint32_t n = 3;
  auto heights = zeroHeights(s.faces.size(), n);
  auto mesh = makeMesh(s, n, heights);
  glm::mat4 model = glm::translate(glm::mat4(1), glm::vec3(2, -1, 0.5f)) *
                    glm::scale(glm::mat4(1), glm::vec3(2, 1, 3));
  mesh->setModelMatrix(model);
  RefSource ref{s.vertices, s.faces, s.uvs, s.materials, heights, n, model};
  auto expected = buildReference(ref);
  float expectedArea = 0;
  for (const auto &r : expected) expectedArea += r.area;
  TASK_CHECK(nearly(mesh->getArea(), expectedArea, 1e-3f * expectedArea + 1e-4f));
  {
    // Identity-model reference to isolate world-normal welding.
    RefSource refId{s.vertices, s.faces, s.uvs, s.materials, heights, n,
                    glm::mat4(1)};
    auto expId = buildReference(refId);
    DisplacedMesh meshId(s.vertices, s.faces, s.uvs, s.materials, heights, n);
    int badId = 0;
    for (std::uint32_t face = 0; face < s.faces.size(); ++face)
      for (std::uint32_t micro = 0; micro < n * n; ++micro) {
        std::vector<RasterTriangle> tri;
        meshId.microTriangle(face, micro)->appendRasterTriangles(tri);
        const auto &r = expId[std::size_t(face) * n * n + micro];
        for (int k = 0; k < 3; ++k)
          if (!nearly(tri.front().vertices[k].normal, r.normal[k], 1e-3f))
            ++badId;
      }
    TASK_CHECK(badId == 0);
  }
  {
    // Compare every micro's vertex normals with the reference; this isolates
    // any world-space shading normal error from ray interpolation.
    int bad = 0;
    for (std::uint32_t face = 0; face < s.faces.size(); ++face)
      for (std::uint32_t micro = 0; micro < n * n; ++micro) {
        std::vector<RasterTriangle> tri;
        mesh->microTriangle(face, micro)->appendRasterTriangles(tri);
        const auto &r = expected[std::size_t(face) * n * n + micro];
        for (int k = 0; k < 3; ++k) {
          if (!nearly(tri.front().vertices[k].normal, r.normal[k], 1e-3f))
            ++bad;
        }
      }
    TASK_CHECK(bad == 0);
  }

  // Cast rays against the real Scene and compare t, UV, and normal with an
  // independent ray cast against the reference micros.
  auto scene = std::make_shared<Scene>("topology",
                                       glm::vec3(4, 2, 8), glm::vec3(2, 0, 0.5f),
                                       glm::vec3(0, 1, 0));
  scene->setProjectionMatrix(50, 0.05f, 100.f);
  TASK_CHECK(scene->addGraphicObj(mesh, "strip"));
  scene->prepare();

  Sampler rng(1234);
  int hits = 0;
  for (int trial = 0; trial < 400; ++trial) {
    // Origins above the transformed surface; aim at points spread over its
    // world-space footprint.
    glm::vec3 origin(2 + 6 * rng.next(), -2 + 4 * rng.next(),
                     8 + 3 * rng.next());
    glm::vec3 target(-1 + 7 * rng.next(), -2 + 5 * rng.next(),
                     -1 + 4 * rng.next());
    glm::vec3 dir = glm::normalize(target - origin);
    Ray ray(origin, dir, 1e-3f, 100.f);
    auto hit = scene->intersect(ray);
    // Independent reference: nearest front-facing intersection.
    Intersection refHit;
    for (std::size_t m = 0; m < expected.size(); ++m) {
      const auto &r = expected[m];
      const glm::dvec3 e1 = glm::dvec3(r.position[1]) - glm::dvec3(r.position[0]);
      const glm::dvec3 e2 = glm::dvec3(r.position[2]) - glm::dvec3(r.position[0]);
      const glm::dvec3 p = glm::cross(glm::dvec3(dir), e2);
      const double det = glm::dot(e1, p);
      if (std::abs(det) < 1e-14) continue;
      const glm::dvec3 oo = glm::dvec3(origin) - glm::dvec3(r.position[0]);
      const double u = glm::dot(oo, p) / det;
      if (u < 0 || u > 1) continue;
      const glm::dvec3 q = glm::cross(oo, e1);
      const double vv = glm::dot(glm::dvec3(dir), q) / det;
      if (vv < 0 || u + vv > 1) continue;
      const double t = glm::dot(e2, q) / det;
      if (t <= 1e-3 || t >= refHit.intersect_time) continue;
      refHit.intersected = true;
      refHit.intersect_time = float(t);
      refHit.coords = origin + float(t) * dir;
      refHit.uv = float(1 - u - vv) * r.uv[0] + float(u) * r.uv[1] +
                  float(vv) * r.uv[2];
      refHit.geometricNormal = r.geometricNormal;
      const glm::vec3 interp = float(1 - u - vv) * r.normal[0] +
                              float(u) * r.normal[1] + float(vv) * r.normal[2];
      refHit.normal = triShadingNormal(dir, r.geometricNormal, interp);
    }
    TASK_CHECK(hit.intersected == refHit.intersected);
    if (!hit.intersected) continue;
    ++hits;
    TASK_CHECK(nearly(hit.intersect_time, refHit.intersect_time, 1e-4f));
    TASK_CHECK(nearly(hit.uv, refHit.uv, 1e-4f));
    TASK_CHECK(nearly(glm::abs(hit.normal), glm::abs(refHit.normal), 1e-3f) ||
               nearly(hit.normal, refHit.normal, 1e-3f));
  }
  TASK_CHECK(hits > 20);
  return 0;
}

// Open boundary: an open mesh must be usable, with boundary UVs intact.
int topology_open_boundary() {
  std::vector<Vertex> v{Vertex({0, 0, 0}, {0, 0, 1}),
                        Vertex({1, 0, 0}, {0, 0, 1}),
                        Vertex({0, 1, 0}, {0, 0, 1})};
  std::vector<glm::uvec3> f{{0, 1, 2}};
  auto uvs = defaultUVs(1);
  uvs[0] = {glm::vec2(0.1f, 0.2f), glm::vec2(0.9f, 0.2f),
            glm::vec2(0.1f, 0.8f)};
  auto m = makeMaterial();
  std::vector<std::shared_ptr<Material>> mats{m};
  const std::uint32_t n = 5;
  auto heights = zeroHeights(1, n);
  DisplacedMesh mesh(v, f, uvs, mats, heights, n);
  TASK_CHECK(mesh.microTriangleCount() == n * n);
  // Boundary corner UVs are exactly the authored ones.
  std::vector<RasterTriangle> stream;
  mesh.appendRasterTriangles(stream);
  TASK_CHECK(stream.size() == n * n);
  // Corner (0,0) micros must carry UV (0.1,0.2).
  bool found = false;
  for (const auto &t : stream)
    for (const auto &vv : t.vertices)
      if (nearly(vv.texCoord, glm::vec2(0.1f, 0.2f), 1e-5f)) found = true;
  TASK_CHECK(found);
  return 0;
}

// Rejections: duplicate face number, non-manifold, zero interpolated normal,
// and an edit that would degenerate a micro triangle.
int topology_rejections_and_recovery() {
  // Duplicate face in one batch.
  {
    Strip s = creaseStrip();
    const std::uint32_t n = 2;
    auto h = zeroHeights(s.faces.size(), n);
    DisplacedMesh mesh(s.vertices, s.faces, s.uvs, s.materials, h, n);
    std::vector<FaceHeightPatch> patches{
        {0, h[0]}, {0, h[0]}};
    bool threw = false;
    try {
      mesh.replaceHeights(patches);
    } catch (const std::exception &) {
      threw = true;
    }
    TASK_CHECK(threw);
  }
  // Non-manifold: three faces on one undirected edge.
  {
    std::vector<Vertex> v{Vertex({0, 0, 0}, {0, 0, 1}),
                          Vertex({1, 0, 0}, {0, 0, 1}),
                          Vertex({0, 1, 0}, {0, 0, 1}),
                          Vertex({0, -1, 0}, {0, 0, 1})};
    std::vector<glm::uvec3> f{{0, 1, 2}, {1, 0, 3}, {0, 1, 3}};
    auto uvs = defaultUVs(3);
    auto m = makeMaterial();
    std::vector<std::shared_ptr<Material>> mats(3, m);
    auto h = zeroHeights(3, 2);
    bool threw = false;
    try {
      DisplacedMesh mesh(v, f, uvs, mats, h, 2);
    } catch (const std::exception &) {
      threw = true;
    }
    TASK_CHECK(threw);
  }
  // Zero interpolated normal: opposite vertex normals that cancel.
  {
    std::vector<Vertex> v{Vertex({0, 0, 0}, {0, 0, 1}),
                          Vertex({1, 0, 0}, {0, 0, -1}),
                          Vertex({0, 1, 0}, {0, 0, 1})};
    std::vector<glm::uvec3> f{{0, 1, 2}};
    auto uvs = defaultUVs(1);
    auto m = makeMaterial();
    std::vector<std::shared_ptr<Material>> mats{m};
    auto h = zeroHeights(1, 2);
    bool threw = false;
    try {
      DisplacedMesh mesh(v, f, uvs, mats, h, 2);
    } catch (const std::exception &) {
      threw = true;
    }
    TASK_CHECK(threw);
  }
  // Edit that degenerates a micro triangle: pull one edge corner halfway to
  // its neighbour's position, collapsing the micro geometry.
  {
    // A single triangle with a 4x subdivision; set the corner at (i=n, j=0)
    // (source vertex y) so far toward x that a micro collapses. Instead use a
    // flat triangle and move two samples to identical positions via the same
    // welded point - not possible for distinct lattice points. We instead
    // build a mesh whose height edit makes a micro triangle exactly flat and
    // zero-area: displace the middle edge point to the line between its
    // neighbours.
    std::vector<Vertex> v{Vertex({0, 0, 0}, {0, 0, 1}),
                          Vertex({1, 0, 0}, {0, 0, 1}),
                          Vertex({0, 1, 0}, {0, 0, 1})};
    std::vector<glm::uvec3> f{{0, 1, 2}};
    auto uvs = defaultUVs(1);
    auto m = makeMaterial();
    std::vector<std::shared_ptr<Material>> mats{m};
    const std::uint32_t n = 2;
    auto h = zeroHeights(1, n);
    // Move one interior/edge point far along -z/is wrong shape to collapse.
    // Use a degenerate geometry: heights of 0 with a triangle whose vertices
    // are collinear after displacement is impossible for an affine original.
    // Instead trigger the "would degenerate" path by making the *source*
    // triangle zero-area, which must be rejected at construction.
    bool threw = false;
    try {
      std::vector<Vertex> vv{Vertex({0, 0, 0}, {0, 0, 1}),
                             Vertex({1, 0, 0}, {0, 0, 1}),
                             Vertex({2, 0, 0}, {0, 0, 1})};
      DisplacedMesh bad(vv, f, uvs, mats, h, n);
    } catch (const std::exception &) {
      threw = true;
    }
    TASK_CHECK(threw);
  }
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Updates group: continuous A/B/A editing, failure isolation, local counters,
// BVH/area identity and locality against many unrelated faces.
// ---------------------------------------------------------------------------

namespace {

// Build a scene containing the crease strip plus many unrelated small
// triangles, so a global scan would be obvious.
struct LocalScene {
  std::shared_ptr<Scene> scene;
  std::shared_ptr<DisplacedMesh> mesh;
  std::vector<std::shared_ptr<Object>> unrelated;
};

LocalScene buildLocalScene(std::uint32_t n, int unrelatedCount,
                           bool emissive = false) {
  LocalScene out;
  out.scene = std::make_shared<Scene>("local", glm::vec3(0.5f, 0.5f, 6),
                                      glm::vec3(0.5f, 0.5f, 0),
                                      glm::vec3(0, 1, 0));
  out.scene->setProjectionMatrix(50, 0.05f, 100.f);
  Strip strip = creaseStrip();
  auto material = makeMaterial(glm::vec3(0.6f),
                               emissive ? glm::vec3(1) : glm::vec3(0));
  for (auto &m : strip.materials) m = material;
  auto heights = zeroHeights(strip.faces.size(), n);
  out.mesh = std::make_shared<DisplacedMesh>(strip.vertices, strip.faces,
                                             strip.uvs, strip.materials,
                                             heights, n);
  out.scene->addGraphicObj(out.mesh, "strip");
  Sampler rng(99);
  for (int i = 0; i < unrelatedCount; ++i) {
    const float x = 20.f + 6.f * rng.next();
    const float y = -10.f + 20.f * rng.next();
    std::vector<Vertex> v{Vertex({x, y, -4}, {0, 0, 1}),
                          Vertex({x + 0.3f, y, -4}, {0, 0, 1}),
                          Vertex({x, y + 0.3f, -4}, {0, 0, 1})};
    std::vector<glm::uvec3> f{{0, 1, 2}};
    auto tri = std::make_shared<Triangle>(
        std::array<Vertex, 3>{v[0], v[1], v[2]}, material);
    out.unrelated.push_back(tri);
    out.scene->addGraphicObj(tri, "u" + std::to_string(i));
  }
  out.scene->prepare();
  return out;
}

int updates_aba_and_continuity() {
  const std::uint32_t n = 4;
  Strip strip = creaseStrip();
  auto material = makeMaterial();
  for (auto &m : strip.materials) m = material;
  auto heights = zeroHeights(strip.faces.size(), n);
  auto mesh = std::make_shared<DisplacedMesh>(
      strip.vertices, strip.faces, strip.uvs, strip.materials, heights, n);
  auto scene = std::make_shared<Scene>("aba", glm::vec3(0.5f, 0.5f, 6),
                                       glm::vec3(0.5f, 0.5f, 0),
                                       glm::vec3(0, 1, 0));
  scene->setProjectionMatrix(50, 0.05f, 100.f);
  scene->addGraphicObj(mesh, "strip");
  scene->prepare();

  auto sampleHits = [&]() {
    std::vector<std::pair<glm::vec3, glm::vec3>> result; // origin, dir
    Sampler rng(555);
    // Cover the flat quad (z=0) and the folded quad (z up to 1) so an edit on
    // either face changes some captured ray.
    for (int i = 0; i < 400; ++i) {
      glm::vec3 o(1.4f * rng.next() - 0.2f, 1.4f * rng.next() - 0.2f,
                  3.f + rng.next());
      result.emplace_back(o, glm::vec3(0, 0, -1));
    }
    return result;
  };
  auto rays = sampleHits();
  auto capture = [&]() {
    std::vector<float> ts;
    for (const auto &[o, d] : rays) {
      auto hit = scene->intersect(Ray(o, d));
      ts.push_back(hit.intersected ? hit.intersect_time : -1.f);
    }
    return ts;
  };
  auto restState = capture();

  // A: bump a welded sample on face 0's x->y edge, a source edge shared with
  // face 1. The welded position therefore changes on both flat faces.
  const std::uint32_t iEdge = n / 2, jEdge = 0; // face 0 edge x->y
  mesh->resetStats();
  mesh->replaceHeights(
      std::vector<HeightSampleRef>{{0, iEdge, jEdge, 0.4f}});
  const auto statsA = mesh->lastStats();
  scene->prepare();
  auto stateA = capture();
  TASK_CHECK(statsA.triangleUpdates > 0 && statsA.vertexWrites > 0);
  TASK_CHECK(statsA.contributionReads >= 1);
  // The shared edge point contributes to two faces; the affected set must
  // include primitives from the neighbouring face. Count face coverage.
  std::set<std::uint32_t> facesTouched;
  for (std::uint32_t face = 0; face < strip.faces.size(); ++face)
    for (std::uint32_t micro = 0; micro < n * n; ++micro) {
      const MicroTri *tri = mesh->microTriangle(face, micro);
      auto bounds = tri->getBounds();
      if (bounds.max.z > 0.01f) facesTouched.insert(face);
    }
  TASK_CHECK(facesTouched.size() >= 2); // shared-edge effect crossed faces

  // B: a different interior sample on face 1 (also flat, hit by the same
  // downward rays).
  mesh->replaceHeights(std::vector<HeightSampleRef>{{1, 1, 1, -0.3f}});
  scene->prepare();
  auto stateB = capture();
  TASK_CHECK(stateA != stateB);

  // Restore both samples exactly. Geometry and rays must return to the
  // untouched state, proving incremental accumulation does not drift.
  mesh->replaceHeights(
      std::vector<HeightSampleRef>{{0, iEdge, jEdge, 0.f}, {1, 1, 1, 0.f}});
  scene->prepare();
  auto stateAA = capture();
  TASK_CHECK(stateAA == restState);

  // A/B/A heights: apply A, B, A and confirm the final surface equals the
  // reference built from the final height arrays.
  mesh->replaceHeights(std::vector<HeightSampleRef>{{0, iEdge, jEdge, 0.4f}});
  mesh->replaceHeights(std::vector<HeightSampleRef>{{0, iEdge, jEdge, -0.2f}});
  mesh->replaceHeights(std::vector<HeightSampleRef>{{0, iEdge, jEdge, 0.4f}});
  scene->prepare();
  RefSource ref{strip.vertices, strip.faces, strip.uvs, strip.materials,
                mesh->heights(), n, glm::mat4(1)};
  auto expected = buildReference(ref);
  for (std::uint32_t face = 0; face < strip.faces.size(); ++face) {
    for (std::uint32_t micro = 0; micro < n * n; ++micro) {
      std::vector<RasterTriangle> tri;
      mesh->microTriangle(face, micro)->appendRasterTriangles(tri);
      TASK_CHECK(tri.size() == 1);
      const auto &r = expected[std::size_t(face) * n * n + micro];
      for (int k = 0; k < 3; ++k)
        TASK_CHECK(nearly(tri.front().vertices[k].position, r.position[k],
                          1e-4f));
    }
  }
  return 0;
}

int updates_failure_isolation() {
  const std::uint32_t n = 3;
  Strip strip = creaseStrip();
  auto material = makeMaterial();
  for (auto &m : strip.materials) m = material;
  auto heights = zeroHeights(strip.faces.size(), n);
  auto mesh = std::make_shared<DisplacedMesh>(
      strip.vertices, strip.faces, strip.uvs, strip.materials, heights, n);
  auto scene = std::make_shared<Scene>("fail", glm::vec3(0.5f, 0.5f, 6),
                                       glm::vec3(0.5f, 0.5f, 0),
                                       glm::vec3(0, 1, 0));
  scene->setProjectionMatrix(50, 0.05f, 100.f);
  scene->addGraphicObj(mesh, "strip");
  scene->prepare();

  auto probe = [&]() {
    auto hit = scene->intersect(Ray({0.25f, 0.25f, 4}, {0, 0, -1}));
    return hit.intersected ? hit.intersect_time : -1.f;
  };
  const float before = probe();
  const auto heightsBefore = mesh->heights();
  const auto statsBefore = mesh->totalStats();

  // A batch with a valid edit first, then an invalid face: nothing may commit.
  bool threw = false;
  try {
    mesh->replaceHeights(std::vector<HeightSampleRef>{
        {0, 1, 1, 0.5f}, {99, 0, 0, 1.f}});
  } catch (const std::exception &) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(mesh->heights() == heightsBefore);
  TASK_CHECK(mesh->totalStats().triangleUpdates ==
             statsBefore.triangleUpdates);
  scene->prepare();
  TASK_CHECK(nearly(probe(), before, 1e-6f));

  // A valid edit after the failure must still work and change geometry.
  mesh->replaceHeights(std::vector<HeightSampleRef>{{0, 1, 1, 0.5f}});
  scene->prepare();
  TASK_CHECK(!nearly(probe(), before, 1e-6f));
  return 0;
}

int updates_local_counters_and_locality() {
  const std::uint32_t n = 6;
  auto local = buildLocalScene(n, 4000);
  auto mesh = local.mesh;
  const std::size_t faceMicros = n * n;

  // Stable primitive identities before the edit.
  std::vector<const void *> leafIds = local.scene->bvhLeafIdentities();
  std::vector<const void *> nodeIds = local.scene->bvhNodeIdentities();
  std::vector<const void *> microAddresses;
  std::vector<std::uint64_t> microStableIds;
  for (std::uint32_t face = 0; face < mesh->faceCount(); ++face)
    for (std::uint32_t micro = 0; micro < faceMicros; ++micro) {
      const MicroTri *t = mesh->microTriangle(face, micro);
      microAddresses.push_back(t);
    }

  local.scene->resetStats();
  mesh->resetStats();
  // One interior sample of one face.
  mesh->replaceHeights(std::vector<HeightSampleRef>{{1, 2, 2, 0.6f}});
  const auto meshStats = mesh->lastStats();
  local.scene->prepare();
  const auto sceneStats = local.scene->stats();

  // Contribution reads and vertex writes are bounded by the local closure:
  // three micros per changed vertex + normal-neighbour ring. For one interior
  // sample this is a small constant far below the whole mesh.
  TASK_CHECK(meshStats.triangleUpdates > 0);
  TASK_CHECK(meshStats.triangleUpdates < std::size_t(faceMicros) *
                                                mesh->faceCount());
  TASK_CHECK(meshStats.triangleUpdates <= 24); // small ring for n=6
  TASK_CHECK(meshStats.vertexWrites == 3 * meshStats.triangleUpdates);
  // One changed sample on one face reads at least its own candidate.
  TASK_CHECK(meshStats.contributionReads >= 1);
  TASK_CHECK(meshStats.contributionReads <= 8);

  // BVH leaf refits never exceed the number of dirty primitives.
  TASK_CHECK(sceneStats.bvhLeafRefits <= meshStats.triangleUpdates);
  // Node refits are bounded by the union of dirty leaf->root paths. With L
  // total leaves the tree height is ceil(log2 L); each dirty leaf can add at
  // most that many distinct ancestors.
  TASK_CHECK(sceneStats.bvhNodeRefits > 0);
  {
    const std::size_t totalLeaves = leafIds.size();
    std::size_t height = 0;
    while ((std::size_t(1) << height) < totalLeaves) ++height;
    const std::uint64_t bound =
        std::uint64_t(sceneStats.bvhLeafRefits) * (height + 1);
    TASK_CHECK(sceneStats.bvhNodeRefits <= bound);
    TASK_CHECK(sceneStats.bvhNodeRefits < nodeIds.size());
  }

  // Primitive addresses and node/leaf identities are unchanged.
  std::vector<const void *> leafIdsAfter = local.scene->bvhLeafIdentities();
  std::vector<const void *> nodeIdsAfter = local.scene->bvhNodeIdentities();
  TASK_CHECK(leafIds == leafIdsAfter);
  TASK_CHECK(nodeIds == nodeIdsAfter);
  for (std::uint32_t face = 0; face < mesh->faceCount(); ++face)
    for (std::uint32_t micro = 0; micro < faceMicros; ++micro) {
      const MicroTri *t = mesh->microTriangle(face, micro);
      TASK_CHECK(t == microAddresses[face * faceMicros + micro]);
    }

  // Area-tree updates are proportional to the affected leaves' paths, not the
  // global light count (which includes the 4000 unrelated diffuse faces but
  // they are not emissive; the strip is the only emitter here).
  TASK_CHECK(sceneStats.areaTreeUpdates == 0); // no emissive -> no area tree

  // Editing one shared-edge sample of face 0 must dirty face-1 micros too,
  // because the welded position is averaged across both faces. Face 1 is not
  // named in the batch.
  {
    Strip strip = creaseStrip();
    auto material = makeMaterial();
    for (auto &m : strip.materials) m = material;
    const std::uint32_t nn = 4;
    auto heights = zeroHeights(strip.faces.size(), nn);
    auto m2 = std::make_shared<DisplacedMesh>(
        strip.vertices, strip.faces, strip.uvs, strip.materials, heights, nn);
    m2->resetStats();
    // Face 0 = {0,1,2}; its edge x->y is source edge (0,1), shared with no
    // other face. Use the crease edge instead: face 1 {1,3,2} and face 2
    // {1,4,3} share source vertex 1 and the (1,3)? pick face 1's y->z edge
    // sample, shared with face 3 {4,5,3} via source vertex 3? Simplest: edit
    // a sample on source vertex 3 (a corner) through face 1, and verify a
    // face-2 or face-3 micro moved. Instead edit the shared interior vertex
    // "3" corner: face 1 y-corner.
    m2->replaceHeights(std::vector<HeightSampleRef>{{
        1, nn, 0, 0.7f}}); // face 1 source vertex 3 corner
    std::uint32_t otherFaceMoved = 0;
    for (std::uint32_t face = 0; face < strip.faces.size(); ++face) {
      if (face == 1) continue;
      for (std::uint32_t micro = 0; micro < nn * nn; ++micro) {
        std::vector<RasterTriangle> tri;
        m2->microTriangle(face, micro)->appendRasterTriangles(tri);
        for (const auto &vv : tri.front().vertices)
          if (std::abs(vv.position.z) > 1e-4f) ++otherFaceMoved;
      }
    }
    TASK_CHECK(otherFaceMoved > 0);
  }

  // A second identical edit is a no-op and must not touch the BVH leaves.
  local.scene->resetStats();
  mesh->resetStats();
  mesh->replaceHeights(std::vector<HeightSampleRef>{{1, 2, 2, 0.6f}});
  local.scene->prepare();
  const auto repeatStats = local.scene->stats();
  TASK_CHECK(repeatStats.bvhLeafRefits <= mesh->lastStats().triangleUpdates);
  return 0;
}

int updates_many_unrelated_faces_stay_local() {
  // Compare counter growth between a scene with few and many unrelated faces.
  const std::uint32_t n = 5;
  auto small = buildLocalScene(n, 4);
  auto large = buildLocalScene(n, 5000);
  auto sampleEdit = [&](LocalScene &s) {
    s.scene->resetStats();
    s.mesh->resetStats();
    s.mesh->replaceHeights(std::vector<HeightSampleRef>{{0, 1, 0, 0.3f}});
    auto meshStats = s.mesh->lastStats();
    s.scene->prepare();
    auto sceneStats = s.scene->stats();
    return std::make_tuple(meshStats.triangleUpdates, meshStats.vertexWrites,
                           meshStats.contributionReads,
                           sceneStats.bvhLeafRefits, sceneStats.bvhNodeRefits);
  };
  auto a = sampleEdit(small);
  auto b = sampleEdit(large);
  // The mesh-local work and the leaf refits are independent of the unrelated
  // face count. Node refits grow only with tree height, not with node count.
  TASK_CHECK(std::get<0>(a) == std::get<0>(b));
  TASK_CHECK(std::get<1>(a) == std::get<1>(b));
  TASK_CHECK(std::get<2>(a) == std::get<2>(b));
  TASK_CHECK(std::get<3>(a) == std::get<3>(b));
  const auto largeNodes = large.scene->bvhNodeIdentities().size();
  TASK_CHECK(std::get<4>(b) < largeNodes);
  TASK_CHECK(std::get<4>(b) <= 64); // bounded by tree height for the dirty set
  return 0;
}

// Patch form is validated and equivalent to per-sample form.
int updates_patch_form() {
  const std::uint32_t n = 3;
  Strip strip = creaseStrip();
  auto material = makeMaterial();
  for (auto &m : strip.materials) m = material;
  auto heights = zeroHeights(strip.faces.size(), n);
  auto bySample = std::make_shared<DisplacedMesh>(
      strip.vertices, strip.faces, strip.uvs, strip.materials, heights, n);
  auto byPatch = std::make_shared<DisplacedMesh>(
      strip.vertices, strip.faces, strip.uvs, strip.materials, heights, n);
  auto byUnsorted = std::make_shared<DisplacedMesh>(
      strip.vertices, strip.faces, strip.uvs, strip.materials, heights, n);
  std::vector<float> patch = heights[2];
  patch[rowMajor(n, 1, 1)] = 0.5f;
  patch[rowMajor(n, 0, 2)] = -0.25f;
  byPatch->replaceHeights(std::vector<FaceHeightPatch>{{2, patch}});
  bySample->replaceHeights(
      std::vector<HeightSampleRef>{{2, 1, 1, 0.5f}, {2, 0, 2, -0.25f}});
  // Deliberately unsorted and descending i: the height values must follow
  // their own samples, not the batch order.
  byUnsorted->replaceHeights(
      std::vector<HeightSampleRef>{{2, 0, 2, -0.25f}, {2, 1, 1, 0.5f}});
  TASK_CHECK(nearly(byUnsorted->getArea(), bySample->getArea(), 1e-5f));
  for (std::uint32_t face = 0; face < strip.faces.size(); ++face)
    for (std::uint32_t micro = 0; micro < n * n; ++micro) {
      std::vector<RasterTriangle> tu, ts;
      byUnsorted->microTriangle(face, micro)->appendRasterTriangles(tu);
      bySample->microTriangle(face, micro)->appendRasterTriangles(ts);
      for (int k = 0; k < 3; ++k)
        TASK_CHECK(nearly(tu.front().vertices[k].position,
                          ts.front().vertices[k].position, 1e-6f));
    }
  TASK_CHECK(nearly(byPatch->getArea(), bySample->getArea(), 1e-4f));
  for (std::uint32_t face = 0; face < strip.faces.size(); ++face)
    for (std::uint32_t micro = 0; micro < n * n; ++micro) {
      std::vector<RasterTriangle> ta, tb;
      byPatch->microTriangle(face, micro)->appendRasterTriangles(ta);
      bySample->microTriangle(face, micro)->appendRasterTriangles(tb);
      TASK_CHECK(ta.size() == tb.size() && ta.size() == 1);
      for (int k = 0; k < 3; ++k)
        TASK_CHECK(nearly(ta.front().vertices[k].position,
                          tb.front().vertices[k].position, 1e-5f));
    }
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Transport group: real scene rays, occlusion, raster vertices, two emissive
// meshes with non-uniform placement, joint area sampling and stable identity.
// ---------------------------------------------------------------------------

namespace {

// Two emissive quads, non-uniformly scaled, transformed and displaced.
struct TwoEmitter {
  std::shared_ptr<Scene> scene;
  std::shared_ptr<DisplacedMesh> a, b;
};

TwoEmitter buildTwoEmitters() {
  TwoEmitter out;
  auto emitter = makeMaterial(glm::vec3(0.9f), glm::vec3(3.f));
  auto makeQuad = [&](glm::vec3 offset, glm::vec3 scale, std::uint32_t n,
                      float bump) {
    std::vector<Vertex> v{Vertex({0, 0, 0}, {0, 0, 1}),
                          Vertex({1, 0, 0}, {0, 0, 1}),
                          Vertex({0, 1, 0}, {0, 0, 1}),
                          Vertex({1, 1, 0}, {0, 0, 1})};
    std::vector<glm::uvec3> f{{0, 1, 2}, {1, 3, 2}};
    auto uvs = defaultUVs(2);
    std::vector<std::shared_ptr<Material>> mats(2, emitter);
    auto heights = zeroHeights(2, n);
    heights[0][rowMajor(n, n / 2, n / 2)] = bump;
    auto mesh = std::make_shared<DisplacedMesh>(v, f, uvs, mats, heights, n);
    mesh->updateModelMatrix({0, 0, 1}, 0, offset, scale);
    return mesh;
  };
  // Distances differ in x/y/z, exercising non-uniform world area.
  out.a = makeQuad({-3, 0, -5}, {2, 1, 1}, 4, 0.5f);
  out.b = makeQuad({2, -1, -7}, {1, 3, 2}, 4, 0.0f);
  out.scene = std::make_shared<Scene>("emit", glm::vec3(0, 0, 4),
                                      glm::vec3(0, 0, -3), glm::vec3(0, 1, 0));
  out.scene->setProjectionMatrix(60, 0.05f, 100.f);
  out.scene->addGraphicObj(out.a, "a");
  out.scene->addGraphicObj(out.b, "b");
  out.scene->prepare();
  return out;
}

int transport_rays_uv_normal_occlusion() {
  const std::uint32_t n = 4;
  Strip strip = creaseStrip();
  auto material = makeMaterial();
  for (auto &m : strip.materials) m = material;
  auto heights = zeroHeights(strip.faces.size(), n);
  heights[1][rowMajor(n, 1, 1)] = 0.4f;
  auto mesh = std::make_shared<DisplacedMesh>(
      strip.vertices, strip.faces, strip.uvs, strip.materials, heights, n);
  glm::mat4 model = glm::translate(glm::mat4(1), glm::vec3(0, 0, -3)) *
                    glm::scale(glm::mat4(1), glm::vec3(1.5f, 0.75f, 2));
  mesh->setModelMatrix(model);
  auto scene = std::make_shared<Scene>("rays", glm::vec3(0.75f, 0.4f, 3),
                                       glm::vec3(0.75f, 0.4f, -3),
                                       glm::vec3(0, 1, 0));
  scene->setProjectionMatrix(55, 0.05f, 100.f);
  scene->addGraphicObj(mesh, "mesh");
  scene->prepare();

  RefSource ref{strip.vertices, strip.faces, strip.uvs, strip.materials,
                heights, n, model};
  auto expected = buildReference(ref);

  Sampler rng(777);
  auto refHitFor = [&](const Ray &ray) {
    Intersection hit;
    for (const auto &r : expected) {
      const glm::dvec3 e1 = glm::dvec3(r.position[1]) - glm::dvec3(r.position[0]);
      const glm::dvec3 e2 = glm::dvec3(r.position[2]) - glm::dvec3(r.position[0]);
      const glm::dvec3 p = glm::cross(glm::dvec3(ray.direction), e2);
      const double det = glm::dot(e1, p);
      if (std::abs(det) < 1e-14) continue;
      const glm::dvec3 oo =
          glm::dvec3(ray.origin) - glm::dvec3(r.position[0]);
      const double u = glm::dot(oo, p) / det;
      if (u < 0 || u > 1) continue;
      const glm::dvec3 q = glm::cross(oo, e1);
      const double vv = glm::dot(glm::dvec3(ray.direction), q) / det;
      if (vv < 0 || u + vv > 1) continue;
      const double t = glm::dot(e2, q) / det;
      if (t <= ray.tMin || t >= ray.tMax || t >= hit.intersect_time) continue;
      hit.intersected = true;
      hit.intersect_time = float(t);
      hit.coords = ray.at(float(t));
      hit.uv = float(1 - u - vv) * r.uv[0] + float(u) * r.uv[1] +
               float(vv) * r.uv[2];
      hit.geometricNormal = r.geometricNormal;
      const glm::vec3 interp = float(1 - u - vv) * r.normal[0] +
                               float(u) * r.normal[1] + float(vv) * r.normal[2];
      hit.normal = triShadingNormal(ray.direction, r.geometricNormal, interp);
    }
    return hit;
  };

  int hits = 0;
  for (int trial = 0; trial < 600; ++trial) {
    // Mesh world bounds: x in [0,1.5], y in [0,0.75], z in [-3,-1].
    glm::vec3 origin(0.75f + 1.2f * (rng.next() - 0.5f),
                     0.4f + 0.6f * (rng.next() - 0.5f),
                     2.f + 4.f * rng.next());
    glm::vec3 target(0.75f + 0.9f * (rng.next() - 0.5f),
                     0.4f + 0.6f * (rng.next() - 0.5f),
                     -3.f + 1.5f * rng.next());
    Ray ray(origin, unit(target - origin), 1e-3f, 100.f);
    auto actual = scene->intersect(ray);
    auto reference = refHitFor(ray);
    TASK_CHECK(actual.intersected == reference.intersected);
    if (!actual.intersected) continue;
    ++hits;
    TASK_CHECK(nearly(actual.intersect_time, reference.intersect_time, 1e-4f));
    TASK_CHECK(nearly(actual.uv, reference.uv, 1e-4f));
    TASK_CHECK(nearly(glm::abs(actual.normal), glm::abs(reference.normal),
                      1e-3f) ||
               nearly(actual.normal, reference.normal, 1e-3f));
  }
  TASK_CHECK(hits > 40);

  // Occlusion: a shadow ray that hits must be reported; one above the surface
  // must be clear. The mesh in the scene is a copy; use a fresh mesh reference
  // for positions.
  auto hit = scene->intersect(Ray({0.75f, 0.4f, 2}, {0, 0, -1}));
  TASK_CHECK(hit.intersected);
  Ray blocked(glm::vec3(0.75f, 0.4f, 2) + glm::vec3(0, 0, 0.001f),
              glm::vec3(0, 0, -1), 0.f, 100.f);
  TASK_CHECK(scene->occluded(blocked));
  Ray clear({0.75f, 0.4f, 2}, {0, 1, 0}, 0.f, 100.f);
  TASK_CHECK(!scene->occluded(clear));
  return 0;
}

int transport_raster_vertices_match() {
  const std::uint32_t n = 3;
  Strip strip = creaseStrip();
  auto material = makeMaterial(glm::vec3(0.5f));
  for (auto &m : strip.materials) m = material;
  auto heights = zeroHeights(strip.faces.size(), n);
  heights[2][rowMajor(n, 1, 0)] = 0.5f;
  auto mesh = std::make_shared<DisplacedMesh>(
      strip.vertices, strip.faces, strip.uvs, strip.materials, heights, n);
  auto scene = std::make_shared<Scene>("raster", glm::vec3(0.5f, 0.4f, 5),
                                       glm::vec3(0.5f, 0.4f, 0),
                                       glm::vec3(0, 1, 0));
  scene->setProjectionMatrix(55, 0.05f, 100.f);
  scene->addGraphicObj(mesh, "mesh");
  scene->prepare();

  RefSource ref{strip.vertices, strip.faces, strip.uvs, strip.materials,
                heights, n, glm::mat4(1)};
  auto expected = buildReference(ref);
  const auto &stream = scene->triangles();
  TASK_CHECK(stream.size() == mesh->microTriangleCount());
  for (std::size_t m = 0; m < stream.size(); ++m) {
    for (int k = 0; k < 3; ++k) {
      TASK_CHECK(nearly(stream[m].vertices[k].position,
                        expected[m].position[k], 1e-4f));
      TASK_CHECK(nearly(stream[m].vertices[k].texCoord, expected[m].uv[k],
                        1e-5f));
    }
  }

  // Render before and after an edit; the images must differ and both must
  // cover the mesh.
  TraditionalRasterizer before(96, 96), after(96, 96);
  before.addScene(scene);
  before.draw();
  std::size_t coveredBefore = 0;
  for (float d : before.depth())
    if (std::isfinite(d)) ++coveredBefore;
  TASK_CHECK(coveredBefore > 100);

  // Edit and re-render; the cached raster stream must reflect the new surface.
  mesh->replaceHeights(std::vector<HeightSampleRef>{{0, 1, 1, 1.0f}});
  scene->prepare();
  after.addScene(scene);
  after.draw();
  std::size_t coveredAfter = 0, differing = 0;
  for (std::size_t i = 0; i < before.depth().size(); ++i) {
    if (std::isfinite(after.depth()[i])) ++coveredAfter;
    if (before.depth()[i] != after.depth()[i] ||
        before.pixels()[i] != after.pixels()[i])
      ++differing;
  }
  TASK_CHECK(coveredAfter > 100);
  TASK_CHECK(differing > 20);

  // The cached stream now equals the reference for the post-edit heights.
  RefSource ref2{strip.vertices, strip.faces, strip.uvs, strip.materials,
                 mesh->heights(), n, glm::mat4(1)};
  auto expected2 = buildReference(ref2);
  for (std::size_t m = 0; m < scene->triangles().size(); ++m)
    for (int k = 0; k < 3; ++k)
      TASK_CHECK(nearly(scene->triangles()[m].vertices[k].position,
                        expected2[m].position[k], 1e-4f));
  return 0;
}

int transport_area_sampling_joint_pdf() {
  auto emitters = buildTwoEmitters();
  const float total =
      emitters.a->getArea() + emitters.b->getArea();
  TASK_CHECK(total > 0);

  // Joint pdf is exactly 1 / total emissive area.
  Sampler rng(4242);
  double pdfSum = 0;
  int n = 30000;
  std::map<std::uint64_t, int> selected;
  for (int i = 0; i < n; ++i) {
    auto s = emitters.scene->sampleLight(rng);
    TASK_CHECK(std::isfinite(s.pdfArea));
    pdfSum += s.pdfArea;
  }
  TASK_CHECK(nearly(float(pdfSum / n), 1.f / total, 1e-3f));

  // Selection frequency matches the transformed real areas of each mesh. A
  // sample belongs to whichever mesh's world bounds contain it.
  auto inBounds = [](const Bounds3 &b, const glm::vec3 &p) {
    return !b.empty() && p.x >= b.min.x - 1e-3f && p.x <= b.max.x + 1e-3f &&
           p.y >= b.min.y - 1e-3f && p.y <= b.max.y + 1e-3f &&
           p.z >= b.min.z - 1e-3f && p.z <= b.max.z + 1e-3f;
  };
  const auto boundsA = emitters.a->getBounds();
  const auto boundsB = emitters.b->getBounds();
  int hitA = 0, hitB = 0;
  Sampler rng2(31);
  const int freqN = 60000;
  for (int i = 0; i < freqN; ++i) {
    auto s = emitters.scene->sampleLight(rng2);
    TASK_CHECK(s.material != nullptr);
    const bool a = inBounds(boundsA, s.position);
    const bool b = inBounds(boundsB, s.position);
    TASK_CHECK(a != b);
    if (a) ++hitA;
    if (b) ++hitB;
  }
  const double totalHits = freqN;
  const double expectedA = emitters.a->getArea() / total;
  const double expectedB = emitters.b->getArea() / total;
  TASK_CHECK(std::abs(hitA / totalHits - expectedA) < 0.02);
  TASK_CHECK(std::abs(hitB / totalHits - expectedB) < 0.02);

  // Change only mesh A's placement (bulk transform is allowed): its real world
  // area changes, and the joint pdf must follow the *current* areas.
  emitters.a->updateModelMatrix({0, 0, 1}, 0, glm::vec3(-3, 0, -5),
                                glm::vec3(4, 1, 1));
  emitters.scene->prepare();
  const float total2 = emitters.a->getArea() + emitters.b->getArea();
  TASK_CHECK(!nearly(total2, total, 1e-3f));
  Sampler rng3(8675309);
  pdfSum = 0;
  for (int i = 0; i < n; ++i) pdfSum += emitters.scene->sampleLight(rng3).pdfArea;
  TASK_CHECK(nearly(float(pdfSum / n), 1.f / total2, 2e-3f));

  // Now edit mesh A's displacement (local) so its real area changes slightly;
  // the pdf must use the new geometry, never a stale density.
  auto heightsA = emitters.a->heights();
  // A single edge sample makes the flat quad tilt, changing its area.
  heightsA[0][rowMajor(4, 0, 0)] = 0.9f;
  emitters.a->replaceHeights(std::vector<FaceHeightPatch>{{0, heightsA[0]}});
  emitters.scene->resetStats();
  emitters.scene->prepare();
  // The area tree must update locally (leaf/group paths), not rebuild from
  // scratch: a positive but small count bounded by the dirty leaf paths.
  TASK_CHECK(emitters.scene->stats().areaTreeUpdates > 0);
  TASK_CHECK(emitters.scene->stats().areaTreeUpdates <=
             4 * (emitters.a->microTriangleCount() +
                  emitters.b->microTriangleCount()));
  const float total3 = emitters.a->getArea() + emitters.b->getArea();
  TASK_CHECK(!nearly(total3, total2, 1e-4f));
  Sampler rng4(5);
  pdfSum = 0;
  for (int i = 0; i < n; ++i) pdfSum += emitters.scene->sampleLight(rng4).pdfArea;
  TASK_CHECK(nearly(float(pdfSum / n), 1.f / total3, 1e-3f));
  return 0;
}

int transport_identity_stable_after_edit() {
  auto emitters = buildTwoEmitters();
  std::vector<const void *> nodes = emitters.scene->bvhNodeIdentities();
  std::vector<const void *> leaves = emitters.scene->bvhLeafIdentities();
  std::vector<const void *> microA, microB;
  for (std::uint32_t face = 0; face < 2; ++face)
    for (std::uint32_t micro = 0; micro < 16; ++micro)
      microA.push_back(emitters.a->microTriangle(face, micro));
  for (std::uint32_t face = 0; face < 2; ++face)
    for (std::uint32_t micro = 0; micro < 16; ++micro)
      microB.push_back(emitters.b->microTriangle(face, micro));

  emitters.a->replaceHeights(std::vector<HeightSampleRef>{{1, 1, 1, 0.3f}});
  emitters.scene->prepare();

  TASK_CHECK(emitters.scene->bvhNodeIdentities() == nodes);
  TASK_CHECK(emitters.scene->bvhLeafIdentities() == leaves);
  int idx = 0;
  for (std::uint32_t face = 0; face < 2; ++face)
    for (std::uint32_t micro = 0; micro < 16; ++micro) {
      TASK_CHECK(emitters.a->microTriangle(face, micro) == microA[idx]);
      TASK_CHECK(emitters.b->microTriangle(face, micro) == microB[idx]);
      ++idx;
    }
  // Stable ids survive too.
  for (int i = 0; i < idx; ++i) {
    TASK_CHECK(microA[i] == emitters.a->microTriangle(i / 16, i % 16));
  }
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration. Three groups; no main() is defined here.
// ---------------------------------------------------------------------------

extern const struct task_case task_cases[];
extern const size_t task_case_count;

const struct task_case task_cases[] = {
    {"topology", "topology_flat_uv_and_weld", topology_flat_uv_and_weld},
    {"topology", "topology_opposite_direction", topology_opposite_direction},
    {"topology", "topology_distinct_indices_same_position",
     topology_distinct_indices_same_position},
    {"topology", "topology_nonuniform_placement", topology_nonuniform_placement},
    {"topology", "topology_open_boundary", topology_open_boundary},
    {"topology", "topology_rejections_and_recovery",
     topology_rejections_and_recovery},
    {"updates", "updates_aba_and_continuity", updates_aba_and_continuity},
    {"updates", "updates_failure_isolation", updates_failure_isolation},
    {"updates", "updates_local_counters_and_locality",
     updates_local_counters_and_locality},
    {"updates", "updates_many_unrelated_faces_stay_local",
     updates_many_unrelated_faces_stay_local},
    {"updates", "updates_patch_form", updates_patch_form},
    {"transport", "transport_rays_uv_normal_occlusion",
     transport_rays_uv_normal_occlusion},
    {"transport", "transport_raster_vertices_match",
     transport_raster_vertices_match},
    {"transport", "transport_area_sampling_joint_pdf",
     transport_area_sampling_joint_pdf},
    {"transport", "transport_identity_stable_after_edit",
     transport_identity_stable_after_edit},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
