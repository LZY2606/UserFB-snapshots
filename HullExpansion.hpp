#ifndef QUICKHULL_HULLEXPANSION_HPP_
#define QUICKHULL_HULLEXPANSION_HPP_

/*
 * Internal implementation shared by the one-shot QuickHull entry points and the
 * incremental hull session.
 *
 * A single expansion takes one exterior point and replaces the (connected) set of
 * visible faces by a fan of new faces around the horizon edge loop.  The face stack
 * processing loop is shared as well.  Neither the point assignment bookkeeping nor
 * the temporary data are copied into the incremental implementation.
 */

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>
#include <algorithm>

#include "Structs/Mesh.hpp"
#include "Structs/Plane.hpp"
#include "Structs/Pool.hpp"
#include "Structs/VertexDataSource.hpp"
#include "MathUtils.hpp"

namespace quickhull {
	namespace detail {

		// Scratch storage whose lifetime may span several hull expansions (and, for
		// IncrementalHull, several append batches).  Clear operations keep capacities.
		template<typename T>
		class ExpansionScratch {
		public:
			struct FaceData {
				size_t m_faceIndex;
				size_t m_enteredFromHalfEdge; // If the face turns out not to be visible,
				                              // this half edge will be marked as horizon edge
				FaceData(size_t fi, size_t he) : m_faceIndex(fi), m_enteredFromHalfEdge(he) { }
			};

			std::vector<size_t> newFaceIndices;
			std::vector<size_t> newHalfEdgeIndices;
			std::vector< std::unique_ptr<std::vector<size_t>> > disabledFacePointVectors;
			std::vector<size_t> visibleFaces;
			std::vector<size_t> horizonEdges;
			std::vector<FaceData> possiblyVisibleFaces;
			Pool<std::vector<size_t>> indexVectorPool;

			// Visible face traversal marks each face with this counter.  The value is
			// incremented for every expansion and therefore has to survive across
			// append calls.
			size_t iteration = 0;

			void clearPerExpansion() {
				newFaceIndices.clear();
				newHalfEdgeIndices.clear();
				disabledFacePointVectors.clear();
				visibleFaces.clear();
				horizonEdges.clear();
				possiblyVisibleFaces.clear();
			}
		};

		template<typename T>
		inline std::unique_ptr<std::vector<size_t>> getIndexVectorFromPool(ExpansionScratch<T>& scratch) {
			auto r = scratch.indexVectorPool.get();
			r->clear();
			return r;
		}

		template<typename T>
		inline void reclaimToIndexVectorPool(ExpansionScratch<T>& scratch,
		                                     std::unique_ptr<std::vector<size_t>>& ptr) {
			const size_t oldSize = ptr->size();
			if ((oldSize+1)*128 < ptr->capacity()) {
				// Reduce memory usage! Huge vectors are needed at the beginning of
				// iteration when faces have many points on their positive side. Later
				// on, smaller vectors will suffice.
				ptr.reset(nullptr);
				return;
			}
			scratch.indexVectorPool.reclaim(ptr);
		}

		// Associates a point with a face if the point resides on the positive side of
		// the plane.  Returns true if the point was on the positive side.  The test is
		// exactly the one used by the original QuickHull implementation: the tolerance
		// is an absolute distance, scaled by the (non-normalized) plane normal.
		template<typename T>
		inline bool addPointToFace(typename MeshBuilder<T>::Face& f,
		                           size_t pointIndex,
		                           const VertexDataSource<T>& vertexData,
		                           T epsilonSquared,
		                           ExpansionScratch<T>& scratch) {
			const T D = mathutils::getSignedDistanceToPlane(vertexData[pointIndex], f.m_P);
			if (D>0 && D*D > epsilonSquared*f.m_P.m_sqrNLength) {
				if (!f.m_pointsOnPositiveSide) {
					f.m_pointsOnPositiveSide = std::move(getIndexVectorFromPool(scratch));
				}
				f.m_pointsOnPositiveSide->push_back(pointIndex);
				if (D > f.m_mostDistantPointDist) {
					f.m_mostDistantPointDist = D;
					f.m_mostDistantPoint = pointIndex;
				}
				return true;
			}
			return false;
		}

		// Given a list of half edges, try to rearrange them so that they form a loop.
		// Return true on success.
		template<typename T>
		inline bool reorderHorizonEdges(MeshBuilder<T>& mesh, std::vector<size_t>& horizonEdges) {
			const size_t horizonEdgeCount = horizonEdges.size();
			for (size_t i=0;i<horizonEdgeCount-1;i++) {
				const size_t endVertex = mesh.m_halfEdges[ horizonEdges[i] ].m_endVertex;
				bool foundNext = false;
				for (size_t j=i+1;j<horizonEdgeCount;j++) {
					const size_t beginVertex = mesh.m_halfEdges[ mesh.m_halfEdges[horizonEdges[j]].m_opp ].m_endVertex;
					if (beginVertex == endVertex) {
						std::swap(horizonEdges[i+1],horizonEdges[j]);
						foundNext = true;
						break;
					}
				}
				if (!foundNext) {
					return false;
				}
			}
			assert(mesh.m_halfEdges[ horizonEdges[horizonEdges.size()-1] ].m_endVertex ==
			       mesh.m_halfEdges[ mesh.m_halfEdges[horizonEdges[0]].m_opp ].m_endVertex);
			return true;
		}

		// Observer which is interested in the net topology change caused by an
		// expansion.  It is called after the new faces and half edges have been
		// stitched and before the new faces are pushed onto the face stack.
		template<typename T>
		struct NullExpansionObserver {
			void onExpansion(MeshBuilder<T>&, ExpansionScratch<T>&) { }
		};

		/*
		 * Process the face stack until it is empty.  Each popped face with points on
		 * its positive side performs one expansion.  This is the loop which used to
		 * live inside QuickHull::createConvexHalfEdgeMesh.
		 */
		template<typename T, typename Observer>
		void processFaceStack(MeshBuilder<T>& mesh,
		                      const VertexDataSource<T>& vertexData,
		                      T epsilonSquared,
		                      ExpansionScratch<T>& s,
		                      std::deque<size_t>& faceList,
		                      size_t& failedHorizonEdges,
		                      Observer& observer) {
			while (!faceList.empty()) {
				if (s.iteration == std::numeric_limits<size_t>::max()) {
					// Visible face traversal marks visited faces with iteration counter
					// (to mark that the face has been visited on this iteration) and the
					// max value represents unvisited faces. At this point we have to
					// reset iteration counter.
					for (auto& f : mesh.m_faces) {
						f.m_visibilityCheckedOnIteration = 0;
					}
					s.iteration = 1;
				}
				else {
					++s.iteration;
				}
				const size_t iter = s.iteration;

				const size_t topFaceIndex = faceList.front();
				faceList.pop_front();

				auto& tf = mesh.m_faces[topFaceIndex];
				tf.m_inFaceStack = 0;

				assert(!tf.m_pointsOnPositiveSide || tf.m_pointsOnPositiveSide->size()>0);
				if (!tf.m_pointsOnPositiveSide || tf.isDisabled()) {
					continue;
				}

				// Pick the most distant point to this triangle plane as the point to
				// which we extrude
				const Vector3<T>& activePoint = vertexData[tf.m_mostDistantPoint];
				const size_t activePointIndex = tf.m_mostDistantPoint;

				// Find out the faces that have our active point on their positive side
				// (these are the "visible faces"). The face on top of the stack of course
				// is one of them. At the same time, we create a list of horizon edges.
				s.horizonEdges.clear();
				s.possiblyVisibleFaces.clear();
				s.visibleFaces.clear();
				s.possiblyVisibleFaces.emplace_back(topFaceIndex,std::numeric_limits<size_t>::max());
				while (s.possiblyVisibleFaces.size()) {
					const auto faceData = s.possiblyVisibleFaces.back();
					s.possiblyVisibleFaces.pop_back();
					auto& pvf = mesh.m_faces[faceData.m_faceIndex];
					assert(!pvf.isDisabled());

					if (pvf.m_visibilityCheckedOnIteration == iter) {
						if (pvf.m_isVisibleFaceOnCurrentIteration) {
							continue;
						}
					}
					else {
						const Plane<T>& P = pvf.m_P;
						pvf.m_visibilityCheckedOnIteration = iter;
						const T d = P.m_N.dotProduct(activePoint)+P.m_D;
						if (d>0) {
							pvf.m_isVisibleFaceOnCurrentIteration = 1;
							pvf.m_horizonEdgesOnCurrentIteration = 0;
							s.visibleFaces.push_back(faceData.m_faceIndex);
							for (auto heIndex : mesh.getHalfEdgeIndicesOfFace(pvf)) {
								if (mesh.m_halfEdges[heIndex].m_opp != faceData.m_enteredFromHalfEdge) {
									s.possiblyVisibleFaces.emplace_back(
										mesh.m_halfEdges[mesh.m_halfEdges[heIndex].m_opp].m_face,
										heIndex);
								}
							}
							continue;
						}
						assert(faceData.m_faceIndex != topFaceIndex);
					}

					// The face is not visible. Therefore, the half edge we entered from
					// is part of the horizon edge.
					pvf.m_isVisibleFaceOnCurrentIteration = 0;
					s.horizonEdges.push_back(faceData.m_enteredFromHalfEdge);

					// Store which half edge is the horizon edge. The other half edges of
					// the face will not be part of the final mesh so their data slots can
					// be recycled.
					const auto halfEdges = mesh.getHalfEdgeIndicesOfFace(
						mesh.m_faces[mesh.m_halfEdges[faceData.m_enteredFromHalfEdge].m_face]);
					const std::int8_t ind = (halfEdges[0]==faceData.m_enteredFromHalfEdge) ? 0 :
						(halfEdges[1]==faceData.m_enteredFromHalfEdge ? 1 : 2);
					mesh.m_faces[mesh.m_halfEdges[faceData.m_enteredFromHalfEdge].m_face]
						.m_horizonEdgesOnCurrentIteration |= (1<<ind);
				}
				const size_t horizonEdgeCount = s.horizonEdges.size();

				// Order horizon edges so that they form a loop. This may fail due to
				// numerical inaccuracy in which case we give up trying to solve horizon
				// edge for this point and accept a minor degeneration in the convex hull.
				if (!reorderHorizonEdges(mesh, s.horizonEdges)) {
					failedHorizonEdges++;
					std::cerr << "Failed to solve horizon edge." << std::endl;
					auto it = std::find(tf.m_pointsOnPositiveSide->begin(),
					                    tf.m_pointsOnPositiveSide->end(),
					                    activePointIndex);
					if (it != tf.m_pointsOnPositiveSide->end()) {
						tf.m_pointsOnPositiveSide->erase(it);
					}
					if (tf.m_pointsOnPositiveSide->size()==0) {
						reclaimToIndexVectorPool(s, tf.m_pointsOnPositiveSide);
					}
					continue;
				}

				// Except for the horizon edges, all half edges of the visible faces can
				// be marked as disabled. Their data slots will be reused. The faces will
				// be disabled as well, but we need to remember the points that were on
				// the positive side of them - therefore we save pointers to them.
				s.newFaceIndices.clear();
				s.newHalfEdgeIndices.clear();
				s.disabledFacePointVectors.clear();
				size_t disableCounter = 0;
				for (auto faceIndex : s.visibleFaces) {
					auto& disabledFace = mesh.m_faces[faceIndex];
					auto halfEdges = mesh.getHalfEdgeIndicesOfFace(disabledFace);
					for (size_t j=0;j<3;j++) {
						if ((disabledFace.m_horizonEdgesOnCurrentIteration & (1<<j)) == 0) {
							if (disableCounter < horizonEdgeCount*2) {
								// Use on this iteration
								s.newHalfEdgeIndices.push_back(halfEdges[j]);
								disableCounter++;
							}
							else {
								// Mark for reusal on later iteration step
								mesh.disableHalfEdge(halfEdges[j]);
							}
						}
					}
					// Disable the face, but retain pointer to the points that were on
					// the positive side of it. We need to assign those points to the new
					// faces we create shortly.
					auto t = mesh.disableFace(faceIndex);
					if (t) {
						assert(t->size()); // Because we should not assign point vectors to faces unless needed...
						s.disabledFacePointVectors.push_back(std::move(t));
					}
				}
				if (disableCounter < horizonEdgeCount*2) {
					const size_t newHalfEdgesNeeded = horizonEdgeCount*2-disableCounter;
					for (size_t i=0;i<newHalfEdgesNeeded;i++) {
						s.newHalfEdgeIndices.push_back(mesh.addHalfEdge());
					}
				}

				// Create new faces using the edge loop
				for (size_t i = 0; i < horizonEdgeCount; i++) {
					const size_t AB = s.horizonEdges[i];

					auto horizonEdgeVertexIndices =
						mesh.getVertexIndicesOfHalfEdge(mesh.m_halfEdges[AB]);
					size_t A,B,C;
					A = horizonEdgeVertexIndices[0];
					B = horizonEdgeVertexIndices[1];
					C = activePointIndex;

					const size_t newFaceIndex = mesh.addFace();
					s.newFaceIndices.push_back(newFaceIndex);

					const size_t CA = s.newHalfEdgeIndices[2*i+0];
					const size_t BC = s.newHalfEdgeIndices[2*i+1];

					mesh.m_halfEdges[AB].m_next = BC;
					mesh.m_halfEdges[BC].m_next = CA;
					mesh.m_halfEdges[CA].m_next = AB;

					mesh.m_halfEdges[BC].m_face = newFaceIndex;
					mesh.m_halfEdges[CA].m_face = newFaceIndex;
					mesh.m_halfEdges[AB].m_face = newFaceIndex;

					mesh.m_halfEdges[CA].m_endVertex = A;
					mesh.m_halfEdges[BC].m_endVertex = C;

					auto& newFace = mesh.m_faces[newFaceIndex];

					const Vector3<T> planeNormal =
						mathutils::getTriangleNormal(vertexData[A],vertexData[B],activePoint);
					newFace.m_P = Plane<T>(planeNormal,activePoint);
					newFace.m_he = AB;

					mesh.m_halfEdges[CA].m_opp =
						s.newHalfEdgeIndices[i>0 ? i*2-1 : 2*horizonEdgeCount-1];
					mesh.m_halfEdges[BC].m_opp =
						s.newHalfEdgeIndices[((i+1)*2) % (horizonEdgeCount*2)];
				}

				// Assign points that were on the positive side of the disabled faces
				// to the new faces.
				for (auto& disabledPoints : s.disabledFacePointVectors) {
					assert(disabledPoints);
					for (const auto& point : *(disabledPoints)) {
						if (point == activePointIndex) {
							continue;
						}
						for (size_t j=0;j<horizonEdgeCount;j++) {
							if (addPointToFace(mesh.m_faces[s.newFaceIndices[j]], point,
							                   vertexData, epsilonSquared, s)) {
								break;
							}
						}
					}
					// The points are no longer needed: we can move them to the vector
					// pool for reuse.
					reclaimToIndexVectorPool(s, disabledPoints);
				}

				// Let the caller observe the new topology before the faces are pushed.
				observer.onExpansion(mesh, s);

				// Increase face stack size if needed
				for (const auto newFaceIndex : s.newFaceIndices) {
					auto& newFace = mesh.m_faces[newFaceIndex];
					if (newFace.m_pointsOnPositiveSide) {
						assert(newFace.m_pointsOnPositiveSide->size()>0);
						if (!newFace.m_inFaceStack) {
							faceList.push_back(newFaceIndex);
							newFace.m_inFaceStack = 1;
						}
					}
				}
			}
		}

		template<typename T>
		inline void processFaceStack(MeshBuilder<T>& mesh,
		                             const VertexDataSource<T>& vertexData,
		                             T epsilonSquared,
		                             ExpansionScratch<T>& s,
		                             std::deque<size_t>& faceList,
		                             size_t& failedHorizonEdges) {
			NullExpansionObserver<T> observer;
			processFaceStack(mesh, vertexData, epsilonSquared, s, faceList,
			                 failedHorizonEdges, observer);
		}

	}
}

#endif /* QUICKHULL_HULLEXPANSION_HPP_ */
