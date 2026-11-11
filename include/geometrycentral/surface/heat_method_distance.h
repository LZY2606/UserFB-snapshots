#pragma once

#include "geometrycentral/numerical/linear_solvers.h"
#include "geometrycentral/surface/edge_length_geometry.h"
#include "geometrycentral/surface/intrinsic_geometry_interface.h"
#include "geometrycentral/surface/surface_mesh.h"
#include "geometrycentral/surface/surface_point.h"
#include "geometrycentral/utilities/vector2.h"
#include "geometrycentral/utilities/vector3.h"

#include <memory>
#include <vector>

namespace geometrycentral {


namespace pointcloud {
class PointCloudHeatSolver; // forward declare to friend below
}

namespace surface {

// One-off function to compute distance from a vertex
VertexData<double> heatMethodDistance(IntrinsicGeometryInterface& geom, Vertex v);


// Stateful class. Allows efficient repeated solves
//
// Disconnected components: the input mesh may contain several topologically disconnected components (components are
// determined by edge/vertex adjacency; surfaces which merely overlap in space but share no vertices remain isolated).
// The high-level computeDistance() entry points below solve each connected component independently, exactly as if that
// component were extracted into its own mesh and solved with the same tCoef / robust options:
//   - the heat time of each component is tCoef times the squared mean edge length of that component,
//   - the zero-shift at the source set is evaluated separately within each sourced component,
//   - vertices of components which carry no source are assigned positive infinity.
// The low-level computeDistanceRHS() entry point is unaffected: it performs a single unshifted solve over the whole
// mesh and never injects infinities into the linear systems.
class HeatMethodDistanceSolver {

public:
  // === Constructor
  HeatMethodDistanceSolver(IntrinsicGeometryInterface& geom, double tCoef = 1.0, bool useRobustLaplacian = false);

  // === Methods

  // Solve for distance from a single vertex
  // Vertices in connected components which do not contain a source get positive infinity.
  VertexData<double> computeDistance(const Vertex& sourceVert);

  // Solve for distance from a collection of vertices
  // Throws std::invalid_argument if the source list is empty.
  // Vertices in connected components which do not contain a source get positive infinity.
  VertexData<double> computeDistance(const std::vector<Vertex>& sourceVerts);

  // Solve for distance from a single surface point
  // Vertices in connected components which do not contain a source get positive infinity.
  VertexData<double> computeDistance(const SurfacePoint& sourcePoint);

  // Solve for distance from a collection of surface points
  // Throws std::invalid_argument if the source list is empty.
  // Vertices in connected components which do not contain a source get positive infinity.
  VertexData<double> computeDistance(const std::vector<SurfacePoint>& sourcePoints);

  // Solve for distance from a custom right hand side
  // (returns WITHOUT performing constant shift to 0)
  //
  // This is the low-level interface: a single heat + Poisson solve is performed on the full mesh (using the factorized
  // operators from construction), with no per-component decomposition, no zero-shift, and no positive-infinity values.
  // The RHS may hold finite values of either sign. The component-aware behavior and infinite unsourced results of the
  // high-level computeDistance() entry points are intentionally not applied here.
  Vector<double> computeDistanceRHS(const Vector<double>& rhs);

  // === Options and parameters

  const double tCoef; // the time parameter used for heat flow, measured as time = tCoef * mean_edge_length^2
                      // default: 1.0
  const bool useRobustLaplacian;

private:
  // === Members

  // Input mesh and geometry
  SurfaceMesh& mesh;
  IntrinsicGeometryInterface& geom;

  // Tufted cover mesh & geometry (these will only be popualted if useRobustLaplacian = true)
  std::unique_ptr<SurfaceMesh> tuftedMesh;
  std::unique_ptr<EdgeLengthGeometry> tuftedIntrinsicGeom;

  // Parameters
  double shortTime; // the actual time used for heat flow computed from tCoef

  // Solvers
  std::unique_ptr<PositiveDefiniteSolver<double>> heatSolver;
  std::unique_ptr<PositiveDefiniteSolver<double>> poissonSolver;

  // === Per-component state for the high-level computeDistance() entry points.
  //
  // Components are defined on the *input* mesh via edge/vertex adjacency. When useRobustLaplacian=true each component
  // gets its own tufted cover with component-local mollification/flips, so cover-specific numbering is never mixed
  // with input vertices or source faces. All of this state is built once at construction and reused across calls.
  struct ConnectedComponent {
    // Dense input-mesh vertex indices belonging to this component, in dense index order; local vertex k of this
    // component corresponds to verts[k].
    std::vector<size_t> verts;

    // A mesh containing just this component (not tufted), and its edge-length geometry.
    std::unique_ptr<SurfaceMesh> subMesh;
    std::unique_ptr<EdgeLengthGeometry> subGeom;

    // Robust mode only: the component's own tufted cover, with component-local mollification and Delaunay flips.
    std::unique_ptr<SurfaceMesh> coverMesh;
    std::unique_ptr<EdgeLengthGeometry> coverGeom;

    // The mesh/geometry actually used for the operators. Aliases subMesh/subGeom for the plain Laplacian, and
    // coverMesh/coverGeom for the robust Laplacian.
    SurfaceMesh* opMesh = nullptr;
    EdgeLengthGeometry* opGeom = nullptr;

    std::unique_ptr<PositiveDefiniteSolver<double>> heatSolver;
    std::unique_ptr<PositiveDefiniteSolver<double>> poissonSolver;
  };

  // One entry per connected component of the input mesh; empty if the input mesh has a single component.
  std::vector<ConnectedComponent> components;

  // Component id of every input-mesh vertex, indexed by dense input-mesh vertex index; empty in the
  // single-component fast path.
  std::vector<size_t> vertexComponent;

  // Helpers

  // Return either the input mesh/geometry, or the tufted mesh/geometry, based on whether useRobustLaplacian=true
  SurfaceMesh& getMesh();
  IntrinsicGeometryInterface& getGeom();

  // Label the connected components of the input mesh via edge/vertex adjacency and, when there is more than one
  // component, build the per-component factorizations used by computeDistance().
  void initializeComponents();

  // The point cloud version bootstraps off of this code
  friend class pointcloud::PointCloudHeatSolver;
};


} // namespace surface
} // namespace geometrycentral
