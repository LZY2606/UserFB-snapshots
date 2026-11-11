#ifndef QUICKHULL_HPP_
#define QUICKHULL_HPP_
#include <deque>
#include <vector>
#include <array>
#include <limits>
#include "Structs/Vector3.hpp"
#include "Structs/Plane.hpp"
#include "Structs/Pool.hpp"
#include "Structs/Mesh.hpp"
#include "ConvexHull.hpp"
#include "HalfEdgeMesh.hpp"
#include "MathUtils.hpp"
#include "HullExpansion.hpp"

/*
 * Implementation of the 3D QuickHull algorithm by Antti Kuukka
 *
 * No copyrights. What follows is 100% Public Domain.
 *
 *
 *
 * INPUT:  a list of points in 3D space (for example, vertices of a 3D mesh)
 *
 * OUTPUT: a ConvexHull object which provides vertex and index buffers of the generated convex hull as a triangle mesh.
 *
 *
 *
 * The implementation is thread-safe if each thread is using its own QuickHull object.
 *
 *
 * SUMMARY OF THE ALGORITHM:
 *         - Create initial simplex (tetrahedron) using extreme points. We have four faces now and they form a convex mesh M.
 *         - For each point, assign them to the first face for which they are on the positive side of (so each point is assigned to at most
 *           one face). Points inside the initial tetrahedron are left behind now and no longer affect the calculations.
 *         - Add all faces that have points assigned to them to Face Stack.
 *         - Iterate until Face Stack is empty:
 *              - Pop topmost face F from the stack
 *              - From the points assigned to F, pick the point P that is farthest away from the plane defined by F.
 *              - Find all faces of M that have P on their positive side. Let us call these the "visible faces".
 *              - Because of the way M is constructed, these faces are connected. Solve their horizon edge loop.
 *				- "Extrude to P": Create new faces by connecting P with the points belonging to the horizon edge. Add the new faces to M and remove the visible
 *                faces from M.
 *              - Each point that was assigned to visible faces is now assigned to at most one of the newly created faces.
 *              - Those new faces that have points assigned to them are added to the top of Face Stack.
 *          - M is now the convex hull.
 * */

namespace quickhull {
	
	struct DiagnosticsData {
		size_t m_failedHorizonEdges; // How many times QuickHull failed to solve the horizon edge.
		                             // Failures lead to degenerate convex hulls.
		
		DiagnosticsData() : m_failedHorizonEdges(0) { }
	};

	template<typename FloatType>
	FloatType defaultEps();

	template<typename FloatType>
	class QuickHull {
		using vec3 = Vector3<FloatType>;

		FloatType m_epsilon, m_epsilonSquared, m_scale;
		bool m_planar;
		std::vector<vec3> m_planarPointCloudTemp;
		VertexDataSource<FloatType> m_vertexData;
		MeshBuilder<FloatType> m_mesh;
		std::array<size_t,6> m_extremeValues;
		DiagnosticsData m_diagnostics;

		// Temporary variables used during iteration process
		detail::ExpansionScratch<FloatType> m_expansionScratch;
		std::deque<size_t> m_faceList;

		// Create a half edge mesh representing the base tetrahedron from which the QuickHull
		// iteration proceeds. m_extremeValues must be properly set up when this is called.
		void setupInitialTetrahedron();

		// Find indices of extreme values (max x, min x, max y, min y, max z, min z) for the
		// given point cloud
		std::array<size_t,6> getExtremeValues();

		// Compute scale of the vertex data.
		FloatType getScale(const std::array<size_t,6>& extremeValues);

		// This will update m_mesh from which we create the ConvexHull object that getConvexHull
		// function returns
		void createConvexHalfEdgeMesh();
		
		// Constructs the convex hull into a MeshBuilder object which can be converted to a
		// ConvexHull or Mesh object
		void buildMesh(const VertexDataSource<FloatType>& pointCloud, FloatType eps);
		
		// The public getConvexHull functions will setup a VertexDataSource object and call this
		ConvexHull<FloatType> getConvexHull(const VertexDataSource<FloatType>& pointCloud,
											bool CCW, bool useOriginalIndices, FloatType eps);
	public:
		// Computes convex hull for a given point cloud.
		// Params:
		//   pointCloud: a vector of of 3D points
		//   CCW: whether the output mesh triangles should have CCW orientation
		//   useOriginalIndices: should the output mesh use same vertex indices as the original point
		//     cloud. If this is false, then we generate a new vertex buffer which contains only
		//     the vertices that are part of the convex hull.
		//   eps: minimum distance to a plane to consider a point being on positive of it
		//     (for a point cloud with scale 1)
		ConvexHull<FloatType> getConvexHull(const std::vector<Vector3<FloatType>>& pointCloud,
											bool CCW,
											bool useOriginalIndices,
											FloatType eps = defaultEps<FloatType>());
		
		// Computes convex hull for a given point cloud.
		// Params:
		//   vertexData: pointer to the first 3D point of the point cloud
		//   vertexCount: number of vertices in the point cloud
		ConvexHull<FloatType> getConvexHull(const Vector3<FloatType>* vertexData,
											size_t vertexCount,
											bool CCW,
											bool useOriginalIndices,
											FloatType eps = defaultEps<FloatType>());
		
		// Computes convex hull for a given point cloud.
		// This function assumes that the vertex data resides in memory in the following format:
		// x_0,y_0,z_0,x_1,y_1,z_1,...
		ConvexHull<FloatType> getConvexHull(const FloatType* vertexData,
											size_t vertexCount,
											bool CCW,
											bool useOriginalIndices,
											FloatType eps = defaultEps<FloatType>());
		
		// Convex hull of the point cloud as a mesh object with half edge structure.
		HalfEdgeMesh<FloatType, size_t> getConvexHullAsMesh(const FloatType* vertexData,
															size_t vertexCount,
															bool CCW,
															FloatType eps = defaultEps<FloatType>());
		
		// Get diagnostics about last generated convex hull
		const DiagnosticsData& getDiagnostics() {
			return m_diagnostics;
		}
	};

}


#endif /* QUICKHULL_HPP_ */
