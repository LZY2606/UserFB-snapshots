#include "geometrycentral/surface/heat_method_distance.h"

#include "geometrycentral/surface/intrinsic_mollification.h"
#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/simple_idt.h"
#include "geometrycentral/surface/tufted_laplacian.h"

#include <deque>
#include <limits>
#include <stdexcept>


namespace geometrycentral {
namespace surface {

VertexData<double> heatMethodDistance(IntrinsicGeometryInterface& geom, Vertex v) {
  return HeatMethodDistanceSolver(geom).computeDistance(v);
}

HeatMethodDistanceSolver::HeatMethodDistanceSolver(IntrinsicGeometryInterface& geom_, double tCoef_,
                                                   bool useRobustLaplacian_)
    : tCoef(tCoef_), useRobustLaplacian(useRobustLaplacian_), mesh(geom_.mesh), geom(geom_) {

  // === Build & factor the linear systems
  if (useRobustLaplacian) {
    geom.requireEdgeLengths();

    // Build operators using robust Laplacian (see [Sharp & Crane. A Laplacian for Nonmanifold Triangle Meshes. SGP
    // 2020]) NOTE: we build explicitly here rather than just calling buildTuftedLaplacian() in order to use an
    // intrinsic geometry.

    // Create a copy of the mesh / geometry to operate on, and build the mollified (tufted if nonmanifold) cover
    EdgeData<double> tuftedEdgeLengths;
    if (mesh.usesImplicitTwin()) {
      tuftedMesh = mesh.copy();
      tuftedEdgeLengths = geom.edgeLengths.reinterpretTo(*tuftedMesh);
    } else {
      tuftedMesh = mesh.copyToSurfaceMesh();
      tuftedEdgeLengths = geom.edgeLengths.reinterpretTo(*tuftedMesh);
      buildIntrinsicTuftedCover(*tuftedMesh, tuftedEdgeLengths);
    }
    mollifyIntrinsic(*tuftedMesh, tuftedEdgeLengths, 1e-5);
    size_t nFlips = flipToDelaunay(*tuftedMesh, tuftedEdgeLengths);
    tuftedIntrinsicGeom.reset(new EdgeLengthGeometry(*tuftedMesh, tuftedEdgeLengths));
  }

  // Compute mean edge length and set shortTime
  getGeom().requireEdgeLengths();
  double meanEdgeLength = 0.;
  for (Edge e : getMesh().edges()) {
    meanEdgeLength += getGeom().edgeLengths[e];
  }
  meanEdgeLength /= getMesh().nEdges();
  shortTime = tCoef * meanEdgeLength * meanEdgeLength;


  // Mass matrix
  getGeom().requireVertexLumpedMassMatrix();
  SparseMatrix<double>& M = getGeom().vertexLumpedMassMatrix;

  // Laplacian
  getGeom().requireCotanLaplacian();
  SparseMatrix<double>& L = getGeom().cotanLaplacian;

  // Heat operator
  SparseMatrix<double> heatOp = M + shortTime * L;
  heatSolver.reset(new PositiveDefiniteSolver<double>(heatOp));

  // Poisson solver
  // NOTE: In theory, it should not be necessary to shift the Laplacian: cotan-Laplace is always PSD. However, when the
  // matrix is only positive SEMIdefinite, some solvers may not work (ie Eigen's Cholesky solver doesn't work, but
  // Suitesparse does).
  SparseMatrix<double> Ls = L + 1e-6 * identityMatrix<double>(mesh.nVertices());
  poissonSolver.reset(new PositiveDefiniteSolver<double>(Ls));

  getGeom().unrequireEdgeLengths();
  getGeom().unrequireCotanLaplacian();
  getGeom().unrequireVertexLumpedMassMatrix();

  // Build the per-connected-component factorizations used by the high-level computeDistance() entry points. This is
  // a no-op (and leaves the full-mesh solvers as the only state) for single-component meshes, preserving the original
  // behavior and results bit-for-bit.
  initializeComponents();
}

SurfaceMesh& HeatMethodDistanceSolver::getMesh() { return useRobustLaplacian ? *tuftedMesh : mesh; }
IntrinsicGeometryInterface& HeatMethodDistanceSolver::getGeom() {
  return useRobustLaplacian ? *tuftedIntrinsicGeom : geom;
}

void HeatMethodDistanceSolver::initializeComponents() {

  // === Label connected components on the input mesh, using true edge/vertex adjacency. Surfaces which overlap
  // spatially but share no vertices/edges remain distinct components. Dense vertex indices (getVertexIndices) are used
  // so the labeling also works for uncompressed meshes.
  VertexData<size_t> vInd = mesh.getVertexIndices();
  size_t N = mesh.nVertices();
  vertexComponent.assign(N, INVALID_IND);
  size_t nComponents = 0;
  for (Vertex seed : mesh.vertices()) {
    size_t seedInd = vInd[seed];
    if (vertexComponent[seedInd] != INVALID_IND) continue;

    std::deque<Vertex> queue;
    queue.push_back(seed);
    vertexComponent[seedInd] = nComponents;
    while (!queue.empty()) {
      Vertex v = queue.front();
      queue.pop_front();
      for (Vertex nbr : v.adjacentVertices()) {
        size_t nbrInd = vInd[nbr];
        if (vertexComponent[nbrInd] == INVALID_IND) {
          vertexComponent[nbrInd] = nComponents;
          queue.push_back(nbr);
        }
      }
    }
    nComponents++;
  }

  // Single-component meshes keep the original full-mesh path (and its exact numerical behavior).
  if (nComponents <= 1) {
    vertexComponent.clear();
    components.clear();
    return;
  }

  components.resize(nComponents);

  // Component vertex lists, in dense input-mesh index order, and a global->local vertex remap.
  std::vector<size_t> localVertexIndex(N, INVALID_IND);
  for (Vertex v : mesh.vertices()) {
    size_t c = vertexComponent[vInd[v]];
    localVertexIndex[vInd[v]] = components[c].verts.size();
    components[c].verts.push_back(vInd[v]);
  }

  // === Build an independent sub mesh / sub geometry for every component. Each one reproduces exactly what the
  // solver would construct if handed only that component, including (in robust mode) a component-local tufted cover,
  // so mollification and flips never leak scale or connectivity between components.
  geom.requireEdgeLengths();

  for (size_t c = 0; c < nComponents; c++) {
    ConnectedComponent& comp = components[c];

    // Faces of this component, with vertex indices remapped to the component's dense local numbering. Face order is
    // inherited from the input mesh.
    std::vector<std::vector<size_t>> subPolygons;
    for (Face f : mesh.faces()) {
      size_t faceComp = INVALID_IND;
      for (Vertex v : f.adjacentVertices()) {
        faceComp = vertexComponent[vInd[v]];
        break;
      }
      if (faceComp != c) continue;

      std::vector<size_t> poly;
      for (Vertex v : f.adjacentVertices()) {
        poly.push_back(localVertexIndex[vInd[v]]);
      }
      subPolygons.push_back(poly);
    }

    // Preserve the input mesh's halfedge convention: an implicit-twin input yields implicit-twin sub meshes (and
    // hence skips the tufting step for already-manifold components), matching a standalone solve on that component.
    if (mesh.usesImplicitTwin()) {
      std::unique_ptr<ManifoldSurfaceMesh> mSub(new ManifoldSurfaceMesh(subPolygons));
      comp.subMesh.reset(mSub.release());
    } else {
      comp.subMesh.reset(new SurfaceMesh(subPolygons));
    }

    // Edge lengths of the sub mesh, taken from the input geometry.
    EdgeData<double> subEdgeLengths(*comp.subMesh);
    for (Edge e : comp.subMesh->edges()) {
      std::array<Vertex, 2> endpoints = e.adjacentVertices();
      size_t iA = endpoints[0].getIndex();
      size_t iB = endpoints[1].getIndex();
      Vertex vTail = mesh.vertex(comp.verts[iA]);
      Vertex vTip = mesh.vertex(comp.verts[iB]);
      Edge origEdge = mesh.connectingEdge(vTail, vTip);
      subEdgeLengths[e] = geom.edgeLengths[origEdge];
    }
    comp.subGeom.reset(new EdgeLengthGeometry(*comp.subMesh, subEdgeLengths));

    if (useRobustLaplacian) {
      // Mirror the constructor's robust pipeline, but restricted to this single component.
      EdgeData<double> coverEdgeLengths;
      if (comp.subMesh->usesImplicitTwin()) {
        comp.coverMesh = comp.subMesh->copy();
        coverEdgeLengths = comp.subGeom->edgeLengths.reinterpretTo(*comp.coverMesh);
      } else {
        comp.coverMesh = comp.subMesh->copyToSurfaceMesh();
        coverEdgeLengths = comp.subGeom->edgeLengths.reinterpretTo(*comp.coverMesh);
        buildIntrinsicTuftedCover(*comp.coverMesh, coverEdgeLengths);
      }
      mollifyIntrinsic(*comp.coverMesh, coverEdgeLengths, 1e-5);
      flipToDelaunay(*comp.coverMesh, coverEdgeLengths);
      comp.coverGeom.reset(new EdgeLengthGeometry(*comp.coverMesh, coverEdgeLengths));

      comp.opMesh = comp.coverMesh.get();
      comp.opGeom = comp.coverGeom.get();
    } else {
      comp.opMesh = comp.subMesh.get();
      comp.opGeom = comp.subGeom.get();
    }

    // === Factor the component operators. The heat time uses this component's own mean edge length.
    comp.opGeom->requireEdgeLengths();
    double meanEdgeLength = 0.;
    for (Edge e : comp.opMesh->edges()) {
      meanEdgeLength += comp.opGeom->edgeLengths[e];
    }
    meanEdgeLength /= comp.opMesh->nEdges();
    double componentShortTime = tCoef * meanEdgeLength * meanEdgeLength;

    comp.opGeom->requireVertexLumpedMassMatrix();
    comp.opGeom->requireCotanLaplacian();
    SparseMatrix<double> Mc = comp.opGeom->vertexLumpedMassMatrix;
    SparseMatrix<double> Lc = comp.opGeom->cotanLaplacian;

    SparseMatrix<double> heatOpC = Mc + componentShortTime * Lc;
    comp.heatSolver.reset(new PositiveDefiniteSolver<double>(heatOpC));

    SparseMatrix<double> LsC = Lc + 1e-6 * identityMatrix<double>(comp.verts.size());
    comp.poissonSolver.reset(new PositiveDefiniteSolver<double>(LsC));

    comp.opGeom->unrequireEdgeLengths();
    comp.opGeom->unrequireCotanLaplacian();
    comp.opGeom->unrequireVertexLumpedMassMatrix();
  }

  geom.unrequireEdgeLengths();
}

VertexData<double> HeatMethodDistanceSolver::computeDistance(const Vertex& sourceVert) {
  // call general version
  return computeDistance({SurfacePoint(sourceVert)});
}
VertexData<double> HeatMethodDistanceSolver::computeDistance(const std::vector<Vertex>& sourceVerts) {
  std::vector<SurfacePoint> surfacePoints;
  for (Vertex v : sourceVerts) {
    surfacePoints.emplace_back(v);
  }

  // call general version
  return computeDistance(surfacePoints);
}

VertexData<double> HeatMethodDistanceSolver::computeDistance(const SurfacePoint& sourcePoint) {
  // call general version
  return computeDistance(std::vector<SurfacePoint>{sourcePoint});
}

VertexData<double> HeatMethodDistanceSolver::computeDistance(const std::vector<SurfacePoint>& sourcePoints) {
  if (sourcePoints.empty()) {
    throw std::invalid_argument("HeatMethodDistanceSolver::computeDistance() called with an empty source list");
  }

  // === Single-component fast path: the original implementation, unchanged.
  if (components.empty()) {
    getGeom().requireHalfedgeCotanWeights();
    getGeom().requireHalfedgeVectorsInFace();
    getGeom().requireEdgeLengths();
    getGeom().requireVertexIndices();
    getGeom().requireVertexDualAreas();
    geom.requireEdgeLengths();
    geom.requireVertexIndices();

    // === Build RHS
    VertexData<double> rhs(mesh, 0.);
    for (const SurfacePoint& p : sourcePoints) {
      SurfacePoint faceP = p.inSomeFace();

      // Set initial values at the three adjacent vertices
      Halfedge he = faceP.face.halfedge();
      rhs[he.vertex()] += faceP.faceCoords.x;
      rhs[he.next().vertex()] += faceP.faceCoords.y;
      rhs[he.next().next().vertex()] += faceP.faceCoords.z;
    }
    Vector<double> rhsVec = rhs.toVector();

    Vector<double> distVec = computeDistanceRHS(rhsVec);

    // ===  Shift distance to put zero at the source set

    // Helper to measure distance between two points, given their barycentric coordinates
    auto baryDist = [&](Vector3 b1, Vector3 b2, const std::array<double, 3>& edgeLengths) {
      // Shindler & Chen 2012, Barycentric Coordinates in Olympiad Geometry, Section 3.2
      Vector3 bVec = b2 - b1;
      double d2 = 0;
      for (int i = 0; i < 3; i++) {
        d2 += edgeLengths[i] * edgeLengths[i] * bVec[i] * bVec[(i + 1) % 3];
      }
      if (!(d2 <= 0)) d2 = 0.; // ensure it's negative so the sqrt below succeed
      return std::sqrt(-d2);
    };

    double distDiffAtSource = 0;
    double weightSum = 0;
    for (const SurfacePoint& p : sourcePoints) {
      SurfacePoint faceP = p.inSomeFace();

      Halfedge he0 = faceP.face.halfedge();
      std::array<double, 3> edgeLengths{geom.edgeLengths[he0.edge()], geom.edgeLengths[he0.next().edge()],
                                        geom.edgeLengths[he0.next().next().edge()]};


      int i = 0;
      for (Halfedge he : faceP.face.adjacentHalfedges()) {

        Vector3 targetP = Vector3::zero();
        targetP[i] = 1.;

        double expectedDistAtVert = baryDist(faceP.faceCoords, targetP, edgeLengths);
        double actDistAtVert = distVec[geom.vertexIndices[he.vertex()]];

        double w = faceP.faceCoords[i];
        distDiffAtSource += (actDistAtVert - expectedDistAtVert) * w;
        weightSum += w;

        i++;
      }
    }
    distDiffAtSource /= weightSum;

    double shift = -distDiffAtSource;
    distVec = distVec.array() + shift;

    getGeom().unrequireHalfedgeCotanWeights();
    getGeom().unrequireHalfedgeVectorsInFace();
    getGeom().unrequireEdgeLengths();
    getGeom().unrequireVertexIndices();
    getGeom().unrequireVertexDualAreas();
    geom.unrequireEdgeLengths();
    geom.unrequireVertexIndices();

    return VertexData<double>(mesh, distVec);
  }

  // === Multi-component path: every connected component is solved independently, as if it were its own mesh with the
  // same tCoef and robust options. Unsourced components never enter a linear solve and get positive infinity.
  size_t N = mesh.nVertices();
  size_t nComp = components.size();

  // Dense input-mesh vertex index -> position within its component's local/op-mesh vectors.
  std::vector<size_t> localIndex(N, INVALID_IND);
  for (size_t c = 0; c < nComp; c++) {
    for (size_t k = 0; k < components[c].verts.size(); k++) {
      localIndex[components[c].verts[k]] = k;
    }
  }

  // === Build a per-component RHS. Sources are located on the input mesh with the usual SurfacePoint conventions; in
  // robust mode their input faces/vertices are never confused with tufted-cover numbering. Cover vertices are in
  // one-to-one correspondence with component vertices, so the same local indices address the cover operators.
  std::vector<Vector<double>> rhsByComp(nComp);
  std::vector<char> componentHasSource(nComp, false);
  for (size_t c = 0; c < nComp; c++) {
    rhsByComp[c] = Vector<double>::Zero(components[c].verts.size());
  }
  geom.requireVertexIndices();
  for (const SurfacePoint& p : sourcePoints) {
    SurfacePoint faceP = p.inSomeFace();
    Halfedge he = faceP.face.halfedge();
    size_t c = vertexComponent[geom.vertexIndices[he.vertex()]];
    componentHasSource[c] = true;

    rhsByComp[c][localIndex[geom.vertexIndices[he.vertex()]]] += faceP.faceCoords.x;
    rhsByComp[c][localIndex[geom.vertexIndices[he.next().vertex()]]] += faceP.faceCoords.y;
    rhsByComp[c][localIndex[geom.vertexIndices[he.next().next().vertex()]]] += faceP.faceCoords.z;
  }

  // Helper to measure distance between two points, given their barycentric coordinates
  auto baryDist = [&](Vector3 b1, Vector3 b2, const std::array<double, 3>& edgeLengths) {
    // Shindler & Chen 2012, Barycentric Coordinates in Olympiad Geometry, Section 3.2
    Vector3 bVec = b2 - b1;
    double d2 = 0;
    for (int i = 0; i < 3; i++) {
      d2 += edgeLengths[i] * edgeLengths[i] * bVec[i] * bVec[(i + 1) % 3];
    }
    if (!(d2 <= 0)) d2 = 0.; // ensure it's negative so the sqrt below succeed
    return std::sqrt(-d2);
  };

  // The source shift is evaluated on input-mesh faces and input edge lengths.
  geom.requireEdgeLengths();

  // Unsourced components start (and remain) at positive infinity.
  VertexData<double> result(mesh, std::numeric_limits<double>::infinity());

  for (size_t c = 0; c < nComp; c++) {
    if (!componentHasSource[c]) continue;

    ConnectedComponent& comp = components[c];
    SurfaceMesh& cMesh = *comp.opMesh;
    EdgeLengthGeometry& cGeom = *comp.opGeom;
    size_t nCompVerts = comp.verts.size();

    cGeom.requireHalfedgeCotanWeights();
    cGeom.requireHalfedgeVectorsInFace();
    cGeom.requireEdgeLengths();
    cGeom.requireVertexIndices();
    cGeom.requireVertexDualAreas();

    // === Solve heat within this component
    Vector<double> heatVec = comp.heatSolver->solve(rhsByComp[c]);

    // === Normalize in each face of the component's operating mesh and evaluate divergence
    Vector<double> divergenceVec = Vector<double>::Zero(nCompVerts);
    for (Face f : cMesh.faces()) {

      Vector2 gradUDir = Vector2::zero(); // warning, wrong magnitude because we don't care
      for (Halfedge he : f.adjacentHalfedges()) {
        Vector2 ePerp = cGeom.halfedgeVectorsInFace[he.next()].rotate90();
        gradUDir += ePerp * heatVec(cGeom.vertexIndices[he.vertex()]);
      }

      gradUDir = gradUDir.normalizeCutoff();

      for (Halfedge he : f.adjacentHalfedges()) {
        double val = cGeom.halfedgeCotanWeights[he] * dot(cGeom.halfedgeVectorsInFace[he], gradUDir);
        divergenceVec[cGeom.vertexIndices[he.tailVertex()]] += val;
        divergenceVec[cGeom.vertexIndices[he.tipVertex()]] -= val;
      }
    }

    // === Integrate divergence to get distance within the component
    Vector<double> distVec = comp.poissonSolver->solve(divergenceVec);

    // === Shift distance to put zero at the component's own source set. Barycentric geometry comes from the input
    // mesh, matching what a standalone solve of this component would use.
    double distDiffAtSource = 0;
    double weightSum = 0;
    for (const SurfacePoint& p : sourcePoints) {
      SurfacePoint faceP = p.inSomeFace();

      Halfedge he0 = faceP.face.halfedge();
      size_t sourceComp = vertexComponent[geom.vertexIndices[he0.vertex()]];
      if (sourceComp != c) continue;

      std::array<double, 3> edgeLengths{geom.edgeLengths[he0.edge()], geom.edgeLengths[he0.next().edge()],
                                        geom.edgeLengths[he0.next().next().edge()]};

      int i = 0;
      for (Halfedge he : faceP.face.adjacentHalfedges()) {

        Vector3 targetP = Vector3::zero();
        targetP[i] = 1.;

        double expectedDistAtVert = baryDist(faceP.faceCoords, targetP, edgeLengths);
        double actDistAtVert = distVec[localIndex[geom.vertexIndices[he.vertex()]]];

        double w = faceP.faceCoords[i];
        distDiffAtSource += (actDistAtVert - expectedDistAtVert) * w;
        weightSum += w;

        i++;
      }
    }
    distDiffAtSource /= weightSum;
    double shift = -distDiffAtSource;

    for (Vertex v : mesh.vertices()) {
      size_t vDense = geom.vertexIndices[v];
      if (vertexComponent[vDense] == c) {
        result[v] = distVec[localIndex[vDense]] + shift;
      }
    }

    cGeom.unrequireHalfedgeCotanWeights();
    cGeom.unrequireHalfedgeVectorsInFace();
    cGeom.unrequireEdgeLengths();
    cGeom.unrequireVertexIndices();
    cGeom.unrequireVertexDualAreas();
  }

  geom.unrequireEdgeLengths();
  geom.unrequireVertexIndices();

  return result;
}

Vector<double> HeatMethodDistanceSolver::computeDistanceRHS(const Vector<double>& rhsVec) {
  getGeom().requireHalfedgeCotanWeights();
  getGeom().requireHalfedgeVectorsInFace();
  getGeom().requireEdgeLengths();
  getGeom().requireVertexIndices();
  getGeom().requireVertexDualAreas();

  // === Solve heat
  Vector<double> heatVec = heatSolver->solve(rhsVec);

  // === Normalize in each face and evaluate divergence
  Vector<double> divergenceVec = Vector<double>::Zero(mesh.nVertices());
  for (Face f : getMesh().faces()) {

    Vector2 gradUDir = Vector2::zero(); // warning, wrong magnitude because we don't care
    for (Halfedge he : f.adjacentHalfedges()) {
      Vector2 ePerp = getGeom().halfedgeVectorsInFace[he.next()].rotate90();
      gradUDir += ePerp * heatVec(getGeom().vertexIndices[he.vertex()]);
    }

    gradUDir = gradUDir.normalizeCutoff();

    for (Halfedge he : f.adjacentHalfedges()) {
      double val = getGeom().halfedgeCotanWeights[he] * dot(getGeom().halfedgeVectorsInFace[he], gradUDir);
      divergenceVec[getGeom().vertexIndices[he.tailVertex()]] += val;
      divergenceVec[getGeom().vertexIndices[he.tipVertex()]] += -val;
    }
  }

  // === Integrate divergence to get distance
  Vector<double> distVec = poissonSolver->solve(divergenceVec);

  getGeom().unrequireHalfedgeVectorsInFace();
  getGeom().unrequireHalfedgeCotanWeights();
  getGeom().unrequireEdgeLengths();
  getGeom().unrequireVertexIndices();
  getGeom().unrequireVertexDualAreas();

  return distVec;
}


} // namespace surface
} // namespace geometrycentral
