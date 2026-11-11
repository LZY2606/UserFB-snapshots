// Component-isolation tests for HeatMethodDistanceSolver.
//
// Expected values are produced by independently solving each connected component on its own extracted mesh with a
// fresh HeatMethodDistanceSolver (same tCoef / robust options), never by reading internal arrays of the solver under
// test. Unsourced components must come back as positive infinity.

#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/surface_mesh.h"
#include "geometrycentral/surface/vertex_position_geometry.h"
#include "geometrycentral/surface/heat_method_distance.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace geometrycentral;
using namespace geometrycentral::surface;

namespace {

int g_scenarios = 0;
int g_failures = 0;

void require(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "ASSERTION FAILED: " << msg << "\n";
    ++g_failures;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Test scenes
// ---------------------------------------------------------------------------------------------------------------------

struct Scene {
  std::vector<std::vector<size_t>> faces;
  std::vector<Vector3> positions;
  std::vector<size_t> faceComponent; // component id of every face
  size_t nComponents = 0;
};

// Deterministic, smoothly varying pseudo-jitter so the "asymmetric" shells are still nondegenerate.
double jitter(size_t i, size_t salt) {
  return 0.06 * std::sin(12.9898 * static_cast<double>(i + 1) + 78.233 * static_cast<double>(salt + 1));
}

// The minimal two-square-patch witness: two independent two-triangle squares. When overlap=true the second patch is
// placed on top of the first in space while remaining topologically disconnected. The second patch is uniformly
// scaled by scaleB.
Scene twoSquarePatches(double scaleB, bool overlap) {
  Scene s;
  s.nComponents = 2;
  // Component 0: unit square.
  s.faces = {{0, 1, 2}, {0, 2, 3}};
  s.faceComponent = {0, 0};
  s.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  // Component 1: square of side scaleB.
  Vector3 shift = overlap ? Vector3{0, 0, 0} : Vector3{100, 0, 0};
  s.faces.push_back({4, 5, 6});
  s.faces.push_back({4, 6, 7});
  s.faceComponent.push_back(1);
  s.faceComponent.push_back(1);
  s.positions.push_back(shift + Vector3{0, 0, 0});
  s.positions.push_back(shift + Vector3{scaleB, 0, 0});
  s.positions.push_back(shift + Vector3{scaleB, scaleB, 0});
  s.positions.push_back(shift + Vector3{0, scaleB, 0});
  return s;
}

// An asymmetric closed triangulated shell (jittered triangulated cube) together with an open jittered grid patch.
// The open patch is uniformly scaled by scaleB.
Scene closedShellAndOpenPatch(double scaleB) {
  Scene s;
  s.nComponents = 2;

  // --- Component 0: asymmetric closed shell (12 triangles, 8 vertices).
  std::vector<Vector3> cube = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                               {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
  for (size_t i = 0; i < cube.size(); ++i) {
    Vector3 p = cube[i];
    // Asymmetric non-uniform scaling plus deterministic per-vertex jitter.
    p = Vector3{1.3 * p.x, 0.7 * p.y, 1.1 * p.z};
    p += Vector3{jitter(i, 0), jitter(i, 1), jitter(i, 2)};
    s.positions.push_back(p);
  }
  std::vector<std::vector<size_t>> shellFaces = {
      {0, 3, 2}, {0, 2, 1}, // bottom
      {4, 5, 6}, {4, 6, 7}, // top
      {0, 1, 5}, {0, 5, 4}, // front
      {2, 3, 7}, {2, 7, 6}, // back
      {0, 4, 7}, {0, 7, 3}, // left
      {1, 2, 6}, {1, 6, 5}  // right
  };
  for (const auto& f : shellFaces) {
    s.faces.push_back(f);
    s.faceComponent.push_back(0);
  }

  // --- Component 1: open jittered grid patch, scaled by scaleB and translated far away.
  const size_t nx = 4;
  const size_t ny = 3;
  size_t baseVert = s.positions.size();
  Vector3 shift{200, 0, 0};
  auto gridId = [&](size_t i, size_t j) { return baseVert + j * (nx + 1) + i; };
  for (size_t j = 0; j <= ny; ++j) {
    for (size_t i = 0; i <= nx; ++i) {
      Vector3 p{static_cast<double>(i), static_cast<double>(j),
               0.15 * std::sin(0.9 * static_cast<double>(i)) * std::cos(0.8 * static_cast<double>(j))};
      p *= scaleB;
      s.positions.push_back(shift + p);
    }
  }
  for (size_t j = 0; j < ny; ++j) {
    for (size_t i = 0; i < nx; ++i) {
      size_t v00 = gridId(i, j);
      size_t v10 = gridId(i + 1, j);
      size_t v11 = gridId(i + 1, j + 1);
      size_t v01 = gridId(i, j + 1);
      s.faces.push_back({v00, v10, v11});
      s.faceComponent.push_back(1);
      s.faces.push_back({v00, v11, v01});
      s.faceComponent.push_back(1);
    }
  }
  return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// Independent per-component reference
// ---------------------------------------------------------------------------------------------------------------------

struct ComponentReference {
  std::unique_ptr<SurfaceMesh> mesh;
  std::unique_ptr<VertexPositionGeometry> geom;
  std::vector<size_t> globalToLocalVert; // dense combined vertex index -> local index (INVALID_IND elsewhere)
  std::vector<size_t> globalToLocalFace; // dense combined face index -> local face index
};

// Extract one component into its own mesh. An implicit-twin ManifoldSurfaceMesh is used for non-robust runs and an
// explicit-twin general SurfaceMesh for robust runs, matching how the combined input mesh is built.
ComponentReference extractComponent(const Scene& scene, size_t comp, bool useGeneralMesh) {
  ComponentReference ref;
  size_t nV = scene.positions.size();
  size_t nF = scene.faces.size();
  ref.globalToLocalVert.assign(nV, INVALID_IND);
  ref.globalToLocalFace.assign(nF, INVALID_IND);

  // Local vertex numbering follows dense combined vertex order, filtered to this component.
  std::vector<size_t> localVerts;
  std::vector<char> used(nV, false);
  for (size_t fi = 0; fi < nF; ++fi) {
    if (scene.faceComponent[fi] != comp) continue;
    for (size_t v : scene.faces[fi]) {
      if (!used[v]) {
        used[v] = true;
        ref.globalToLocalVert[v] = localVerts.size();
        localVerts.push_back(v);
      }
    }
  }

  // Faces keep the combined ordering.
  std::vector<std::vector<size_t>> localFaces;
  for (size_t fi = 0; fi < nF; ++fi) {
    if (scene.faceComponent[fi] != comp) continue;
    ref.globalToLocalFace[fi] = localFaces.size();
    std::vector<size_t> lf;
    for (size_t v : scene.faces[fi]) lf.push_back(ref.globalToLocalVert[v]);
    localFaces.push_back(lf);
  }

  if (useGeneralMesh) {
    ref.mesh.reset(new SurfaceMesh(localFaces));
  } else {
    ref.mesh.reset(new ManifoldSurfaceMesh(localFaces));
  }
  VertexData<Vector3> localPos(*ref.mesh);
  for (size_t k = 0; k < localVerts.size(); ++k) {
    localPos[ref.mesh->vertex(k)] = scene.positions[localVerts[k]];
  }
  ref.geom.reset(new VertexPositionGeometry(*ref.mesh, localPos));
  return ref;
}

double componentDiameter(const Scene& scene, size_t comp, const std::vector<char>& vertexInComp) {
  double diam = 0.;
  size_t nV = scene.positions.size();
  for (size_t i = 0; i < nV; ++i) {
    if (!vertexInComp[i]) continue;
    for (size_t j = i + 1; j < nV; ++j) {
      if (!vertexInComp[j]) continue;
      diam = std::max(diam, (scene.positions[i] - scene.positions[j]).norm());
    }
  }
  require(diam > 0., "component diameter must be positive");
  return diam;
}

std::unique_ptr<SurfaceMesh> buildCombinedMesh(const Scene& scene, bool useGeneralMesh) {
  if (useGeneralMesh) return std::unique_ptr<SurfaceMesh>(new SurfaceMesh(scene.faces));
  return std::unique_ptr<SurfaceMesh>(new ManifoldSurfaceMesh(scene.faces));
}

// Abstract source location, expressed in combined-mesh indices, so SurfacePoints can be rebuilt on whatever mesh
// instance a solver owns (SurfacePoint handles are not portable across distinct mesh objects). Edge sources are
// identified by their two endpoint vertex indices rather than a mesh-specific edge index.
struct SourceSpec {
  SurfacePointType kind;
  size_t element = INVALID_IND;       // combined vertex index (Vertex) or face index (Face)
  size_t edgeA = INVALID_IND;         // edge source: first endpoint, combined vertex index
  size_t edgeB = INVALID_IND;         // edge source: second endpoint, combined vertex index
  double tEdge = 0.37;                // for edge sources
  Vector3 faceCoords{0.2, 0.3, 0.5};  // for face sources
};

SurfacePoint makeSurfacePoint(SurfaceMesh& m, const SourceSpec& s) {
  if (s.kind == SurfacePointType::Vertex) return SurfacePoint(m.vertex(s.element));
  if (s.kind == SurfacePointType::Edge) {
    Edge e = m.connectingEdge(m.vertex(s.edgeA), m.vertex(s.edgeB));
    require(e != Edge(), "source edge must exist on the combined mesh");
    return SurfacePoint(e, s.tEdge);
  }
  return SurfacePoint(m.face(s.element), s.faceCoords);
}

// Build the equivalent source point on an extracted component's mesh.
SurfacePoint mapSource(const SourceSpec& s, const ComponentReference& ref) {
  if (s.kind == SurfacePointType::Vertex) {
    return SurfacePoint(ref.mesh->vertex(ref.globalToLocalVert[s.element]));
  }
  if (s.kind == SurfacePointType::Face) {
    return SurfacePoint(ref.mesh->face(ref.globalToLocalFace[s.element]), s.faceCoords);
  }
  Vertex lA = ref.mesh->vertex(ref.globalToLocalVert[s.edgeA]);
  Vertex lB = ref.mesh->vertex(ref.globalToLocalVert[s.edgeB]);
  Edge localEdge = ref.mesh->connectingEdge(lA, lB);
  require(localEdge != Edge(), "mapped source edge must exist on reference component");
  return SurfacePoint(localEdge, s.tEdge);
}

// Component of a combined-mesh vertex index.
size_t componentOfVertex(const Scene& scene, size_t v) {
  for (size_t fi = 0; fi < scene.faces.size(); ++fi) {
    for (size_t fv : scene.faces[fi]) {
      if (fv == v) return scene.faceComponent[fi];
    }
  }
  return INVALID_IND;
}

// Component of any source, expressed via combined indices.
size_t componentOfSource(const Scene& scene, const SourceSpec& s) {
  if (s.kind == SurfacePointType::Face) return scene.faceComponent[s.element];
  if (s.kind == SurfacePointType::Vertex) return componentOfVertex(scene, s.element);
  return componentOfVertex(scene, s.edgeA);
}

// Run the solver under test and verify every vertex against an independent per-component standalone solve.
void checkAgainstReference(const Scene& scene, bool robust, const std::vector<SourceSpec>& sourceSpecs,
                           const std::string& name) {
  ++g_scenarios;
  bool useGeneralMesh = robust; // robust mode needs the tufted cover, which uses an explicit-twin general mesh
  std::unique_ptr<SurfaceMesh> mesh = buildCombinedMesh(scene, useGeneralMesh);
  VertexData<Vector3> pos(*mesh);
  for (size_t i = 0; i < scene.positions.size(); ++i) pos[mesh->vertex(i)] = scene.positions[i];
  VertexPositionGeometry geom(*mesh, pos);

  std::vector<SurfacePoint> sources;
  for (const SourceSpec& s : sourceSpecs) sources.push_back(makeSurfacePoint(*mesh, s));

  HeatMethodDistanceSolver solver(geom, 1.0, robust);
  VertexData<double> dist = solver.computeDistance(sources);

  std::vector<char> componentSourced(scene.nComponents, false);
  std::vector<std::vector<char>> vertexInComp(scene.nComponents,
                                              std::vector<char>(scene.positions.size(), false));
  for (size_t fi = 0; fi < scene.faces.size(); ++fi) {
    size_t c = scene.faceComponent[fi];
    for (size_t v : scene.faces[fi]) vertexInComp[c][v] = true;
  }
  for (const SourceSpec& s : sourceSpecs) {
    componentSourced[componentOfSource(scene, s)] = true;
  }

  for (size_t c = 0; c < scene.nComponents; ++c) {
    double diam = componentDiameter(scene, c, vertexInComp[c]);

    if (!componentSourced[c]) {
      // No source: every vertex must be positive infinity (never NaN or a finite placeholder).
      for (size_t i = 0; i < scene.positions.size(); ++i) {
        if (!vertexInComp[c][i]) continue;
        double val = dist[mesh->vertex(i)];
        require(std::isinf(val) && val > 0., name + ": unsourced component vertex must be +inf");
        require(!std::isnan(val), name + ": unsourced component vertex must not be NaN");
      }
      continue;
    }

    ComponentReference ref = extractComponent(scene, c, useGeneralMesh);
    HeatMethodDistanceSolver refSolver(*ref.geom, 1.0, robust);
    std::vector<SurfacePoint> localSources;
    for (const SourceSpec& s : sourceSpecs) {
      if (componentOfSource(scene, s) == c) localSources.push_back(mapSource(s, ref));
    }
    require(!localSources.empty(), name + ": sourced component must have at least one mapped source");
    VertexData<double> refDist = refSolver.computeDistance(localSources);

    for (size_t i = 0; i < scene.positions.size(); ++i) {
      if (ref.globalToLocalVert[i] == INVALID_IND) continue;
      double got = dist[mesh->vertex(i)];
      double expected = refDist[ref.mesh->vertex(ref.globalToLocalVert[i])];
      require(std::isfinite(got), name + ": sourced component vertex must be finite");
      require(std::isfinite(expected), name + ": reference distance must be finite");
      double relErr = std::abs(got - expected) / diam;
      if (!(relErr <= 1e-7)) {
        std::cerr << "  at vertex " << i << " got=" << got << " expected=" << expected << " relErr=" << relErr
                  << "\n";
        require(false, name + ": component solve must match standalone reference within 1e-7 of diameter");
      }
    }
  }
}

size_t firstVertexOfComponent(const Scene& scene, size_t comp) {
  for (size_t fi = 0; fi < scene.faces.size(); ++fi) {
    if (scene.faceComponent[fi] == comp) return scene.faces[fi][0];
  }
  return INVALID_IND;
}

size_t firstFaceOfComponent(const Scene& scene, size_t comp) {
  for (size_t fi = 0; fi < scene.faces.size(); ++fi) {
    if (scene.faceComponent[fi] == comp) return fi;
  }
  return INVALID_IND;
}

// Build an edge SourceSpec on the first edge whose tail vertex belongs to comp.
SourceSpec firstEdgeSpec(SurfaceMesh& m, const Scene& scene, size_t comp) {
  for (Edge e : m.edges()) {
    if (componentOfVertex(scene, e.halfedge().vertex().getIndex()) != comp) continue;
    std::array<Vertex, 2> endpoints = e.adjacentVertices();
    return SourceSpec{SurfacePointType::Edge, INVALID_IND, endpoints[0].getIndex(), endpoints[1].getIndex(), 0.37};
  }
  require(false, "component must contain an edge");
  return SourceSpec{};
}

// ---------------------------------------------------------------------------------------------------------------------
// Category checks
// ---------------------------------------------------------------------------------------------------------------------

void runComponentReferenceChecks() {
  const double scales[3] = {1.0 / 20.0, 1.0, 20.0};

  for (double scaleB : scales) {
    for (bool robust : {false, true}) {
      // Minimal two-patch witness, spatially separated.
      {
        Scene separated = twoSquarePatches(scaleB, false);
        size_t src = firstVertexOfComponent(separated, 0);
        checkAgainstReference(separated, robust, {{SurfacePointType::Vertex, src}},
                              "two-patch vertex source, separated");
      }
      // Spatially overlapping but topologically disconnected: isolation must still hold.
      {
        Scene overlapping = twoSquarePatches(scaleB, true);
        size_t src = firstVertexOfComponent(overlapping, 1);
        checkAgainstReference(overlapping, robust, {{SurfacePointType::Vertex, src}},
                              "two-patch vertex source, overlapping");
      }

      // Asymmetric closed shell plus open patch at this scale ratio.
      Scene shell = closedShellAndOpenPatch(scaleB);
      {
        size_t src = firstVertexOfComponent(shell, 0);
        checkAgainstReference(shell, robust, {{SurfacePointType::Vertex, src}}, "shell+patch vertex source on shell");
      }
      {
        // One side entirely unsourced: an edge source only on the open patch, closed shell must be all +inf.
        std::unique_ptr<SurfaceMesh> m = buildCombinedMesh(shell, robust);
        SourceSpec spec = firstEdgeSpec(*m, shell, 1);
        checkAgainstReference(shell, robust, {spec}, "shell+patch edge source on patch, shell unsourced");
      }
      {
        // Both components carry multiple sources (a vertex and an interior face point on each).
        size_t vA = firstVertexOfComponent(shell, 0);
        size_t fA = firstFaceOfComponent(shell, 0);
        size_t vB = firstVertexOfComponent(shell, 1);
        size_t fB = firstFaceOfComponent(shell, 1);
        std::vector<SourceSpec> specs{{SurfacePointType::Vertex, vA},
                                      {SurfacePointType::Face, fA, INVALID_IND, INVALID_IND, 0.37,
                                       Vector3{0.2, 0.3, 0.5}},
                                      {SurfacePointType::Vertex, vB},
                                      {SurfacePointType::Face, fB, INVALID_IND, INVALID_IND, 0.37,
                                       Vector3{0.5, 0.2, 0.3}}};
        checkAgainstReference(shell, robust, specs, "shell+patch multiple sources on both components");
      }
    }
  }
}

void runSourceKindChecks() {
  Scene scene = closedShellAndOpenPatch(1.0);
  for (bool robust : {false, true}) {
    // Vertex, edge and face sources, one per component.
    for (size_t comp = 0; comp < scene.nComponents; ++comp) {
      checkAgainstReference(scene, robust, {{SurfacePointType::Vertex, firstVertexOfComponent(scene, comp)}},
                            "vertex source");

      std::unique_ptr<SurfaceMesh> m = buildCombinedMesh(scene, robust);
      checkAgainstReference(scene, robust, {firstEdgeSpec(*m, scene, comp)}, "edge source");

      checkAgainstReference(scene, robust,
                            {{SurfacePointType::Face, firstFaceOfComponent(scene, comp), INVALID_IND, INVALID_IND,
                              0.37, Vector3{0.2, 0.3, 0.5}}},
                            "face source");
    }

    // All four high-level entry points must agree for equivalent sources.
    {
      std::unique_ptr<SurfaceMesh> m = buildCombinedMesh(scene, robust);
      VertexData<Vector3> pos(*m);
      for (size_t i = 0; i < scene.positions.size(); ++i) pos[m->vertex(i)] = scene.positions[i];
      VertexPositionGeometry geom(*m, pos);
      HeatMethodDistanceSolver solver(geom, 1.0, robust);

      Vertex v = m->vertex(firstVertexOfComponent(scene, 0));
      VertexData<double> d1 = solver.computeDistance(v);
      VertexData<double> d2 = solver.computeDistance(std::vector<Vertex>{v});
      VertexData<double> d3 = solver.computeDistance(SurfacePoint(v));
      VertexData<double> d4 = solver.computeDistance(std::vector<SurfacePoint>{SurfacePoint(v)});
      ++g_scenarios;
      for (Vertex vv : m->vertices()) {
        require(d1[vv] == d2[vv] && d2[vv] == d3[vv] && d3[vv] == d4[vv],
                "all four computeDistance entry points must agree");
      }
    }

    // Empty source lists must throw std::invalid_argument on both list entry points.
    {
      std::unique_ptr<SurfaceMesh> m = buildCombinedMesh(scene, robust);
      VertexData<Vector3> pos(*m);
      for (size_t i = 0; i < scene.positions.size(); ++i) pos[m->vertex(i)] = scene.positions[i];
      VertexPositionGeometry geom(*m, pos);
      HeatMethodDistanceSolver solver(geom, 1.0, robust);
      ++g_scenarios;
      bool threwVerts = false;
      try {
        solver.computeDistance(std::vector<Vertex>{});
      } catch (const std::invalid_argument&) {
        threwVerts = true;
      }
      require(threwVerts, "empty vertex source list must throw invalid_argument");

      bool threwPoints = false;
      try {
        solver.computeDistance(std::vector<SurfacePoint>{});
      } catch (const std::invalid_argument&) {
        threwPoints = true;
      }
      require(threwPoints, "empty surface-point source list must throw invalid_argument");
    }
  }
}

void runRobustReuseChecks() {
  for (bool robust : {false, true}) {
    Scene scene = twoSquarePatches(1.0, false);
    std::unique_ptr<SurfaceMesh> m = buildCombinedMesh(scene, robust);
    VertexData<Vector3> pos(*m);
    for (size_t i = 0; i < scene.positions.size(); ++i) pos[m->vertex(i)] = scene.positions[i];
    VertexPositionGeometry geom(*m, pos);

    // One solver, source sets switched repeatedly: A -> B -> A. Factorizations are built once and reused; infinities
    // from an unsourced component must never enter a linear solve.
    HeatMethodDistanceSolver solver(geom, 1.0, robust);
    size_t vA = firstVertexOfComponent(scene, 0);
    size_t vB = firstVertexOfComponent(scene, 1);

    std::vector<SurfacePoint> sourcesA{SurfacePoint(m->vertex(vA))};
    std::vector<SurfacePoint> sourcesB{SurfacePoint(m->vertex(vB))};

    VertexData<double> firstA = solver.computeDistance(sourcesA);
    ++g_scenarios;
    VertexData<double> onB = solver.computeDistance(sourcesB);
    ++g_scenarios;
    VertexData<double> secondA = solver.computeDistance(sourcesA);
    ++g_scenarios;

    // The third call must reproduce the first call exactly.
    for (Vertex v : m->vertices()) {
      require(firstA[v] == secondA[v], "reused solver must reproduce the earlier A-source result");
    }

    // Independently verify each call against standalone per-component solves.
    checkAgainstReference(scene, robust, {{SurfacePointType::Vertex, vA}}, "reuse sequence call A (1st)");
    checkAgainstReference(scene, robust, {{SurfacePointType::Vertex, vB}}, "reuse sequence call B");
    checkAgainstReference(scene, robust, {{SurfacePointType::Vertex, vA}}, "reuse sequence call A (2nd)");

    // During the B call, component A is unsourced and must be +inf.
    for (size_t i = 0; i < scene.positions.size(); ++i) {
      if (componentOfVertex(scene, i) == 0) {
        require(std::isinf(onB[m->vertex(i)]) && onB[m->vertex(i)] > 0.,
                "component A must be +inf while only B carries sources");
      }
    }
    // During the A calls, component B is unsourced and must be +inf.
    for (size_t i = 0; i < scene.positions.size(); ++i) {
      if (componentOfVertex(scene, i) == 1) {
        require(std::isinf(firstA[m->vertex(i)]) && firstA[m->vertex(i)] > 0.,
                "component B must be +inf while only A carries sources");
        require(std::isinf(secondA[m->vertex(i)]) && secondA[m->vertex(i)] > 0.,
                "component B must stay +inf on the repeated A call");
      }
    }
  }
}

// Independent regression lock for ordinary single-component input: the refactor must not change these numbers.
// Values are the HeatMethodDistanceSolver output for a unit two-triangle square, captured from the unmodified
// implementation (the single-component code path is still executed verbatim).
void runSingleComponentGoldenCheck() {
  ManifoldSurfaceMesh mesh({{0, 1, 2}, {0, 2, 3}});
  std::vector<Vector3> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  VertexData<Vector3> pos(mesh);
  for (size_t i = 0; i < 4; ++i) pos[mesh.vertex(i)] = p[i];
  VertexPositionGeometry geom(mesh, pos);
  HeatMethodDistanceSolver solver(geom, 1.0, false);
  VertexData<double> d = solver.computeDistance(mesh.vertex(0));
  ++g_scenarios;

  const double golden[4] = {0.0, 0.91606418643326537, 1.317093535692937, 0.91606418643326548};
  for (size_t i = 0; i < 4; ++i) {
    require(std::abs(d[mesh.vertex(i)] - golden[i]) <= 1e-12,
            "single-component golden distance must be preserved");
  }
}

} // namespace

int main() {
  runComponentReferenceChecks();
  if (g_failures == 0) std::cout << "PASS component-reference\n";

  runSourceKindChecks();
  if (g_failures == 0) std::cout << "PASS source-kinds\n";

  runRobustReuseChecks();
  if (g_failures == 0) std::cout << "PASS robust-reuse\n";

  runSingleComponentGoldenCheck();

  std::cout << "SCENARIOS=" << g_scenarios << "\n";
  require(g_scenarios > 0, "at least one scenario must execute");

  if (g_failures != 0) {
    std::cerr << "HEAT_COMPONENTS_FAILED with " << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "HEAT_COMPONENTS_OK\n";
  return 0;
}
