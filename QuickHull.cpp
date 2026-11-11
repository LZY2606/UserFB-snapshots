#include "QuickHull.hpp"
#include "MathUtils.hpp"
#include <cmath>
#include <cassert>
#include <iostream>
#include <algorithm>
#include <limits>
#include "Structs/Mesh.hpp"

namespace quickhull {

	template<>
	float defaultEps() { 
		return 0.0001f;
	}
	
	template<>
	double defaultEps() {
		return 0.0000001; 
	}
	
	/*
	 * Implementation of the algorithm
	 */
	
	template<typename T>
	ConvexHull<T> QuickHull<T>::getConvexHull(const std::vector<Vector3<T>>& pointCloud, bool CCW, bool useOriginalIndices, T epsilon) {
		VertexDataSource<T> vertexDataSource(pointCloud);
		return getConvexHull(vertexDataSource,CCW,useOriginalIndices,epsilon);
	}
	
	template<typename T>
	ConvexHull<T> QuickHull<T>::getConvexHull(const Vector3<T>* vertexData, size_t vertexCount, bool CCW, bool useOriginalIndices, T epsilon) {
		VertexDataSource<T> vertexDataSource(vertexData,vertexCount);
		return getConvexHull(vertexDataSource,CCW,useOriginalIndices,epsilon);
	}
	
	template<typename T>
	ConvexHull<T> QuickHull<T>::getConvexHull(const T* vertexData, size_t vertexCount, bool CCW, bool useOriginalIndices, T epsilon) {
		VertexDataSource<T> vertexDataSource((const vec3*)vertexData,vertexCount);
		return getConvexHull(vertexDataSource,CCW,useOriginalIndices,epsilon);
	}
	
	template<typename FloatType>
	HalfEdgeMesh<FloatType, size_t> QuickHull<FloatType>::getConvexHullAsMesh(const FloatType* vertexData, size_t vertexCount, bool CCW, FloatType epsilon) {
		VertexDataSource<FloatType> vertexDataSource((const vec3*)vertexData,vertexCount);
		buildMesh(vertexDataSource, epsilon);
		return HalfEdgeMesh<FloatType, size_t>(m_mesh, m_vertexData);
	}
	
	template<typename T>
	void QuickHull<T>::buildMesh(const VertexDataSource<T>& pointCloud, T epsilon)
	{
		if (pointCloud.size()==0) {
			m_mesh = MeshBuilder<T>();
			return;
		}
		m_vertexData = pointCloud;
		
		// Very first: find extreme values and use them to compute the scale of the point cloud.
		m_extremeValues = getExtremeValues();
		m_scale = getScale(m_extremeValues);
		
		// Epsilon we use depends on the scale
		m_epsilon = epsilon*m_scale;
		m_epsilonSquared = m_epsilon*m_epsilon;
		
		// Reset diagnostics
		m_diagnostics = DiagnosticsData();

		// The planar case happens when all the points appear to lie on a two dimensional
		// subspace of R^3.
		m_planar = false;
		
		createConvexHalfEdgeMesh();
		if (m_planar) {
			const size_t extraPointIndex = m_planarPointCloudTemp.size() - 1;
			for (auto& he : m_mesh.m_halfEdges) {
				if (he.m_endVertex == extraPointIndex) {
					he.m_endVertex = 0;
				}
			}
			m_vertexData = pointCloud;
			m_planarPointCloudTemp.clear();
		}
	}

	template<typename T>
	ConvexHull<T> QuickHull<T>::getConvexHull(const VertexDataSource<T>& pointCloud, bool CCW, bool useOriginalIndices, T epsilon) {
		buildMesh(pointCloud, epsilon);
		return ConvexHull<T>(m_mesh,m_vertexData, CCW, useOriginalIndices);
	}

	template<typename T>
	void QuickHull<T>::createConvexHalfEdgeMesh() {
		m_expansionScratch.clearPerExpansion();

		// Compute base tetrahedron
		setupInitialTetrahedron();
		assert(m_mesh.m_faces.size() == 4);

		// Init face stack with those faces that have points assigned to them
		m_faceList.clear();
		for (size_t i=0;i < 4;i++) {
			auto& f = m_mesh.m_faces[i];
			if (f.m_pointsOnPositiveSide && f.m_pointsOnPositiveSide->size()>0) {
				m_faceList.push_back(i);
				f.m_inFaceStack = 1;
			}
		}

		// Process faces until the face list is empty.
		detail::processFaceStack(m_mesh, m_vertexData, m_epsilonSquared,
		                         m_expansionScratch, m_faceList,
		                         m_diagnostics.m_failedHorizonEdges);

		// Cleanup
		m_expansionScratch.indexVectorPool.clear();
	}
	
	/*
	 * Private helper functions
	 */

	template <typename T>
	std::array<size_t, 6> QuickHull<T>::getExtremeValues() {
		std::array<size_t,6> outIndices{0, 0, 0, 0, 0, 0};
		T extremeVals[6] = { m_vertexData[0].x, m_vertexData[0].x, m_vertexData[0].y,
							 m_vertexData[0].y, m_vertexData[0].z, m_vertexData[0].z };
		const size_t vCount = m_vertexData.size();
		for (size_t i = 1; i < vCount; i++) {
			const Vector3<T>& pos = m_vertexData[i];
			if (pos.x>extremeVals[0]) {
				extremeVals[0]=pos.x;
				outIndices[0]=i;
			}
			else if (pos.x<extremeVals[1]) {
				extremeVals[1]=pos.x;
				outIndices[1]=i;
			}
			if (pos.y>extremeVals[2]) {
				extremeVals[2]=pos.y;
				outIndices[2]=i;
			}
			else if (pos.y<extremeVals[3]) {
				extremeVals[3]=pos.y;
				outIndices[3]=i;
			}
			if (pos.z>extremeVals[4]) {
				extremeVals[4]=pos.z;
				outIndices[4]=i;
			}
			else if (pos.z<extremeVals[5]) {
				extremeVals[5]=pos.z;
				outIndices[5]=i;
			}
		}
		return outIndices;
	}

	template <typename T>
	T QuickHull<T>::getScale(const std::array<size_t,6>& extremeValues) {
		T s = 0;
		for (size_t i=0;i<6;i++) {
			const T* v = (const T*)(&m_vertexData[extremeValues[i]]);
			v += i/2;
			auto a = std::abs(*v);
			if (a>s) {
				s = a;
			}
		}
		return s;
	}

	template<typename T>
	void QuickHull<T>::setupInitialTetrahedron() {
		const size_t vertexCount = m_vertexData.size();
		
		// If we have at most 4 points, just return a degenerate tetrahedron:
		if (vertexCount <= 4) {
			size_t v[4] = {0,std::min((size_t)1,vertexCount-1),std::min((size_t)2,vertexCount-1),std::min((size_t)3,vertexCount-1)};
			const Vector3<T> N = mathutils::getTriangleNormal(m_vertexData[v[0]],m_vertexData[v[1]],m_vertexData[v[2]]);
			const Plane<T> trianglePlane(N,m_vertexData[v[0]]);
			if (trianglePlane.isPointOnPositiveSide(m_vertexData[v[3]])) {
				std::swap(v[0],v[1]);
			}
			return m_mesh.setup(v[0],v[1],v[2],v[3]);
		}
		
		// Find two most distant extreme points.
		T maxD = m_epsilonSquared;
		std::pair<size_t,size_t> selectedPoints;
		for (size_t i=0;i<6;i++) {
			for (size_t j=i+1;j<6;j++) {
				const T d = m_vertexData[ m_extremeValues[i] ].getSquaredDistanceTo( m_vertexData[ m_extremeValues[j] ] );
				if (d > maxD) {
					maxD=d;
					selectedPoints={m_extremeValues[i],m_extremeValues[j]};
				}
			}
		}
		if (maxD == m_epsilonSquared) {
			// A degenerate case: the point cloud seems to consists of a single point
			return m_mesh.setup(0,std::min((size_t)1,vertexCount-1),std::min((size_t)2,vertexCount-1),std::min((size_t)3,vertexCount-1));
		}
		assert(selectedPoints.first != selectedPoints.second);
		
		// Find the most distant point to the line between the two chosen extreme points.
		const Ray<T> r(m_vertexData[selectedPoints.first], (m_vertexData[selectedPoints.second] - m_vertexData[selectedPoints.first]));
		maxD = m_epsilonSquared;
		size_t maxI=std::numeric_limits<size_t>::max();
		const size_t vCount = m_vertexData.size();
		for (size_t i=0;i<vCount;i++) {
			const T distToRay = mathutils::getSquaredDistanceBetweenPointAndRay(m_vertexData[i],r);
			if (distToRay > maxD) {
				maxD=distToRay;
				maxI=i;
			}
		}
		if (maxD == m_epsilonSquared) {
			// It appears that the point cloud belongs to a 1 dimensional subspace of R^3: convex hull has no volume => return a thin triangle
			// Pick any point other than selectedPoints.first and selectedPoints.second as the third point of the triangle
			auto it = std::find_if(m_vertexData.begin(),m_vertexData.end(),[&](const vec3& ve) {
				return ve != m_vertexData[selectedPoints.first] && ve != m_vertexData[selectedPoints.second];
			});
			const size_t thirdPoint = (it == m_vertexData.end()) ? selectedPoints.first : std::distance(m_vertexData.begin(),it);
			it = std::find_if(m_vertexData.begin(),m_vertexData.end(),[&](const vec3& ve) {
				return ve != m_vertexData[selectedPoints.first] && ve != m_vertexData[selectedPoints.second] && ve != m_vertexData[thirdPoint];
			});
			const size_t fourthPoint = (it == m_vertexData.end()) ? selectedPoints.first : std::distance(m_vertexData.begin(),it);
			return m_mesh.setup(selectedPoints.first,selectedPoints.second,thirdPoint,fourthPoint);
		}

		// These three points form the base triangle for our tetrahedron.
		assert(selectedPoints.first != maxI && selectedPoints.second != maxI);
		std::array<size_t,3> baseTriangle{selectedPoints.first, selectedPoints.second, maxI};
		const Vector3<T> baseTriangleVertices[]={ m_vertexData[baseTriangle[0]], m_vertexData[baseTriangle[1]],  m_vertexData[baseTriangle[2]] };
		
		// Next step is to find the 4th vertex of the tetrahedron. We naturally choose the point farthest away from the triangle plane.
		maxD=m_epsilon;
		maxI=0;
		const Vector3<T> N = mathutils::getTriangleNormal(baseTriangleVertices[0],baseTriangleVertices[1],baseTriangleVertices[2]);
		Plane<T> trianglePlane(N,baseTriangleVertices[0]);
		for (size_t i=0;i<vCount;i++) {
			const T d = std::abs(mathutils::getSignedDistanceToPlane(m_vertexData[i],trianglePlane));
			if (d>maxD) {
				maxD=d;
				maxI=i;
			}
		}
		if (maxD == m_epsilon) {
			// All the points seem to lie on a 2D subspace of R^3. How to handle this? Well, let's add one extra point to the point cloud so that the convex hull will have volume.
			m_planar = true;
			const vec3 N1 = mathutils::getTriangleNormal(baseTriangleVertices[1],baseTriangleVertices[2],baseTriangleVertices[0]);
			m_planarPointCloudTemp.clear();
			m_planarPointCloudTemp.insert(m_planarPointCloudTemp.begin(),m_vertexData.begin(),m_vertexData.end());
			const vec3 extraPoint = N1 + m_vertexData[0];
			m_planarPointCloudTemp.push_back(extraPoint);
			maxI = m_planarPointCloudTemp.size()-1;
			m_vertexData = VertexDataSource<T>(m_planarPointCloudTemp);
		}

		// Enforce CCW orientation (if user prefers clockwise orientation, swap two vertices in each triangle when final mesh is created)
		const Plane<T> triPlane(N,baseTriangleVertices[0]);
		if (triPlane.isPointOnPositiveSide(m_vertexData[maxI])) {
			std::swap(baseTriangle[0],baseTriangle[1]);
		}

		// Create a tetrahedron half edge mesh and compute planes defined by each triangle
		m_mesh.setup(baseTriangle[0],baseTriangle[1],baseTriangle[2],maxI);
		for (auto& f : m_mesh.m_faces) {
			auto v = m_mesh.getVertexIndicesOfFace(f);
			const Vector3<T>& va = m_vertexData[v[0]];
			const Vector3<T>& vb = m_vertexData[v[1]];
			const Vector3<T>& vc = m_vertexData[v[2]];
			const Vector3<T> N1 = mathutils::getTriangleNormal(va, vb, vc);
			const Plane<T> plane(N1,va);
			f.m_P = plane;
		}

		// Finally we assign a face for each vertex outside the tetrahedron (vertices inside the tetrahedron have no role anymore)
		for (size_t i=0;i<vCount;i++) {
			for (auto& face : m_mesh.m_faces) {
				if (detail::addPointToFace(face, i, m_vertexData, m_epsilonSquared,
				                           m_expansionScratch)) {
					break;
				}
			}
		}
	}
	
	/*
	 * Explicit template specifications for float and double
	 */

	template class QuickHull<float>;
	template class QuickHull<double>;
}

