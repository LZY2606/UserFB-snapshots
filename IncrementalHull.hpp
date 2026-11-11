#ifndef QUICKHULL_INCREMENTALHULL_HPP_
#define QUICKHULL_INCREMENTALHULL_HPP_

/*
 * IncrementalHull: a stateful convex hull session for point clouds which grow
 * over time.
 *
 * The session owns its point storage, so the arrays passed to the constructor
 * and to append() may be released or modified immediately after the call
 * returns.  Every input point receives a permanent index: the index is simply
 * its position in the session point buffer.  Duplicate points and points inside
 * the hull are stored and numbered as well - they just do not change the hull.
 *
 * Faces also have stable external ids which are never reused, unlike the
 * internal face slots that MeshBuilder recycles.  The returned draw updates can
 * therefore be applied to a client-side face table which keeps per-face state
 * (for example a material id) without any chance of state leaking from a
 * deleted face into a newly created face that happens to reuse the same internal
 * slot.
 *
 * All hull computations use the same visible-face/horizon/half-edge stitching
 * implementation as the one-shot QuickHull entry points (see
 * HullExpansion.hpp).  Existing points are never reclassified: only points from
 * the current append batch are tested against the current hull.
 */

#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "Structs/Mesh.hpp"
#include "Structs/Plane.hpp"
#include "Structs/Ray.hpp"
#include "Structs/Vector3.hpp"
#include "Structs/VertexDataSource.hpp"
#include "MathUtils.hpp"
#include "HullExpansion.hpp"

namespace quickhull {

	template<typename T>
	class IncrementalHull {
	public:
		using vec3 = Vector3<T>;

		// One triangle of a snapshot.  Vertices are permanent point indices in
		// counter-clockwise (outward) order.
		struct Triangle {
			size_t faceId;
			std::array<size_t,3> vertexIndices;

			Triangle() : faceId(0), vertexIndices{{0,0,0}} { }
			Triangle(size_t id, const std::array<size_t,3>& v)
				: faceId(id), vertexIndices(v) { }
		};

		// A self-contained copy of the complete session state.  It does not share
		// any storage with the session, so it stays valid and unchanged after later
		// appends (even when the session's vectors reallocate).
		struct Snapshot {
			std::vector<vec3> points;
			std::vector<Triangle> triangles;
		};

		struct NewFace {
			size_t faceId;
			std::array<size_t,3> vertexIndices;

			NewFace() : faceId(0), vertexIndices{{0,0,0}} { }
			NewFace(size_t id, const std::array<size_t,3>& v)
				: faceId(id), vertexIndices(v) { }
		};

		// Net change of one append batch.  Faces which were created and removed
		// again inside the same batch are not reported in either list (their ids
		// are still burned and never reused).
		struct DrawUpdate {
			std::vector<size_t> deletedFaceIds;
			std::vector<NewFace> addedFaces;
		};

	private:
		using Face = typename MeshBuilder<T>::Face;

		T m_tolerance;
		T m_epsilonSquared;
		std::vector<vec3> m_points;
		MeshBuilder<T> m_mesh;
		std::vector<size_t> m_faceExternalIds;
		size_t m_nextFaceId;
		detail::ExpansionScratch<T> m_expansionScratch;
		std::deque<size_t> m_faceList;
		size_t m_failedHorizonEdges;

		// State of the append() call currently in progress.
		size_t m_batchStartId = 0;
		std::vector<size_t> m_batchDeleted;
		std::vector<NewFace> m_batchAdded;
		std::unordered_set<size_t> m_batchTransient;

		static bool isFinite(const vec3& p) {
			using std::isfinite;
			return isfinite(p.x) && isfinite(p.y) && isfinite(p.z);
		}

		std::array<size_t,6> findExtremeValues() const {
			std::array<size_t,6> outIndices{{0,0,0,0,0,0}};
			T extremeVals[6] = { m_points[0].x, m_points[0].x, m_points[0].y,
			                     m_points[0].y, m_points[0].z, m_points[0].z };
			for (size_t i = 1; i < m_points.size(); i++) {
				const vec3& pos = m_points[i];
				if (pos.x>extremeVals[0]) { extremeVals[0]=pos.x; outIndices[0]=i; }
				else if (pos.x<extremeVals[1]) { extremeVals[1]=pos.x; outIndices[1]=i; }
				if (pos.y>extremeVals[2]) { extremeVals[2]=pos.y; outIndices[2]=i; }
				else if (pos.y<extremeVals[3]) { extremeVals[3]=pos.y; outIndices[3]=i; }
				if (pos.z>extremeVals[4]) { extremeVals[4]=pos.z; outIndices[4]=i; }
				else if (pos.z<extremeVals[5]) { extremeVals[5]=pos.z; outIndices[5]=i; }
			}
			return outIndices;
		}

		// Select four affinely independent points.  An initial set which does not
		// span R^3 (fewer than four points, all points equal, collinear, coplanar
		// or too thin to be resolved with the configured absolute tolerance) is
		// rejected instead of being padded with synthetic points.
		std::array<size_t,4> selectInitialTetrahedron() const {
			const size_t n = m_points.size();
			if (n < 4) {
				throw std::invalid_argument(
					"IncrementalHull: initial point set must contain at least four points spanning 3D");
			}

			const std::array<size_t,6> extreme = findExtremeValues();

			// Two most distant extreme points.
			T maxDistSquared = 0;
			size_t first = extreme[0], second = extreme[0];
			for (size_t i=0;i<6;i++) {
				for (size_t j=i+1;j<6;j++) {
					const T d = m_points[extreme[i]].getSquaredDistanceTo(m_points[extreme[j]]);
					if (d > maxDistSquared) {
						maxDistSquared = d;
						first = extreme[i];
						second = extreme[j];
					}
				}
			}
			const T maxDist = maxDistSquared > 0 ? std::sqrt(maxDistSquared) : T(0);
			if (first == second || !(maxDist > m_tolerance)) {
				throw std::invalid_argument(
					"IncrementalHull: initial point set does not span 3D (collinear or coincident)");
			}

			// Farthest point from the line through the two selected points.
			const Ray<T> line(m_points[first], m_points[second] - m_points[first]);
			T maxLineDistSquared = 0;
			size_t third = first;
			for (size_t i=0;i<n;i++) {
				const T d = mathutils::getSquaredDistanceBetweenPointAndRay(m_points[i], line);
				if (d > maxLineDistSquared) {
					maxLineDistSquared = d;
					third = i;
				}
			}
			const T maxLineDist = maxLineDistSquared > 0 ? std::sqrt(maxLineDistSquared) : T(0);
			if (third == first || third == second || !(maxLineDist > m_tolerance)) {
				throw std::invalid_argument(
					"IncrementalHull: initial point set does not span 3D (collinear)");
			}

			// Farthest point from the plane of the base triangle, measured as an
			// actual Euclidean distance.
			const vec3 normal = mathutils::getTriangleNormal(
				m_points[first], m_points[second], m_points[third]);
			const Plane<T> basePlane(normal, m_points[first]);
			T maxAbsDistance = 0;
			size_t fourth = first;
			for (size_t i=0;i<n;i++) {
				const T d = std::abs(mathutils::getSignedDistanceToPlane(m_points[i], basePlane));
				if (d > maxAbsDistance) {
					maxAbsDistance = d;
					fourth = i;
				}
			}
			const T normalLength = std::sqrt(basePlane.m_sqrNLength);
			const T euclideanHeight = normalLength > 0 ? maxAbsDistance / normalLength : T(0);
			if (fourth == first || fourth == second || fourth == third ||
			    !(euclideanHeight > m_tolerance)) {
				throw std::invalid_argument(
					"IncrementalHull: initial point set does not span 3D (coplanar)");
			}

			std::array<size_t,4> tetrahedron{{first, second, third, fourth}};
			// Make the base triangle outward facing (the fourth point must be on
			// the negative side of its plane).
			if (basePlane.isPointOnPositiveSide(m_points[fourth])) {
				std::swap(tetrahedron[0], tetrahedron[1]);
			}
			return tetrahedron;
		}

		void buildInitialHull() {
			const std::array<size_t,4> tv = selectInitialTetrahedron();
			m_mesh.setup(tv[0], tv[1], tv[2], tv[3]);

			// Compute planes defined by each tetrahedron face.
			for (auto& f : m_mesh.m_faces) {
				const auto v = m_mesh.getVertexIndicesOfFace(f);
				const vec3& va = m_points[v[0]];
				const vec3& vb = m_points[v[1]];
				const vec3& vc = m_points[v[2]];
				const vec3 n = mathutils::getTriangleNormal(va, vb, vc);
				f.m_P = Plane<T>(n, va);
			}

			m_expansionScratch.clearPerExpansion();

			const VertexDataSource<T> vertexData(m_points);

			// Assign every initial point to the first face it is above.  Points
			// inside the tetrahedron are dropped at this stage (they remain stored
			// as permanent points, though).
			for (size_t i=0;i<m_points.size();i++) {
				for (auto& face : m_mesh.m_faces) {
					if (detail::addPointToFace(face, i, vertexData, m_epsilonSquared,
					                           m_expansionScratch)) {
						break;
					}
				}
			}

			m_faceList.clear();
			for (size_t i=0;i<m_mesh.m_faces.size();i++) {
				auto& f = m_mesh.m_faces[i];
				if (f.m_pointsOnPositiveSide && f.m_pointsOnPositiveSide->size()>0) {
					m_faceList.push_back(i);
					f.m_inFaceStack = 1;
				}
			}

			detail::processFaceStack(m_mesh, vertexData, m_epsilonSquared,
			                         m_expansionScratch, m_faceList,
			                         m_failedHorizonEdges);

			// Assign external ids to all faces that survived the initial build.
			// Disabled slots intentionally keep no id; if such a slot is reused
			// later it receives a fresh id from the append observer.
			m_faceExternalIds.assign(m_mesh.m_faces.size(), size_t(0));
			m_nextFaceId = 0;
			for (size_t i=0;i<m_mesh.m_faces.size();i++) {
				if (!m_mesh.m_faces[i].isDisabled()) {
					m_faceExternalIds[i] = m_nextFaceId++;
				}
			}
		}

		void onExpansion(detail::ExpansionScratch<T>& scratch) {
			// Note: visible faces have already been disabled, but their slot still
			// carries its old external id until we overwrite it below.
			for (const size_t slot : scratch.visibleFaces) {
				const size_t oldId = m_faceExternalIds[slot];
				if (oldId >= m_batchStartId) {
					// Face was created earlier in this very batch: it is transient
					// and must not be reported as a net deletion.
					m_batchTransient.insert(oldId);
				}
				else {
					m_batchDeleted.push_back(oldId);
				}
			}

			for (const size_t slot : scratch.newFaceIndices) {
				if (slot >= m_faceExternalIds.size()) {
					m_faceExternalIds.resize(slot+1, size_t(0));
				}
				const size_t id = m_nextFaceId++;
				m_faceExternalIds[slot] = id;
				const auto vertices = m_mesh.getVertexIndicesOfFace(m_mesh.m_faces[slot]);
				m_batchAdded.push_back(NewFace(id, vertices));
			}
		}

		struct SessionObserver {
			IncrementalHull* m_self;
			explicit SessionObserver(IncrementalHull* self) : m_self(self) { }

			void onExpansion(MeshBuilder<T>&, detail::ExpansionScratch<T>& scratch) {
				m_self->onExpansion(scratch);
			}
		};

		void appendImplementation(const vec3* newPoints, size_t count) {
			if (count > 0 && newPoints == nullptr) {
				throw std::invalid_argument(
					"IncrementalHull: append received a null point array");
			}

			// Validate the whole batch before touching any persistent state.  A
			// batch containing NaN or infinity is rejected atomically.
			for (size_t i=0;i<count;i++) {
				if (!isFinite(newPoints[i])) {
					throw std::invalid_argument(
						"IncrementalHull: append batch contains a non-finite point");
				}
			}

			const size_t oldPointCount = m_points.size();
			m_points.reserve(oldPointCount + count);
			for (size_t i=0;i<count;i++) {
				m_points.push_back(newPoints[i]);
			}

			if (count == 0) {
				return;
			}

			const VertexDataSource<T> vertexData(m_points);

			m_batchStartId = m_nextFaceId;
			m_batchDeleted.clear();
			m_batchAdded.clear();
			m_batchTransient.clear();
			m_batchTransient.reserve(count*4);

			// Only the new points are classified.  Historical points, including
			// earlier points that were inside the hull, are never revisited.
			for (size_t pointIndex=oldPointCount;pointIndex<m_points.size();pointIndex++) {
				for (auto& face : m_mesh.m_faces) {
					if (!face.isDisabled() &&
					    detail::addPointToFace(face, pointIndex, vertexData,
					                           m_epsilonSquared, m_expansionScratch)) {
						break;
					}
				}
			}

			// Push every face which received points.  m_inFaceStack guards against
			// pushing the same face twice.
			for (size_t i=0;i<m_mesh.m_faces.size();i++) {
				auto& f = m_mesh.m_faces[i];
				if (!f.isDisabled() && f.m_pointsOnPositiveSide &&
				    f.m_pointsOnPositiveSide->size()>0 && !f.m_inFaceStack) {
					m_faceList.push_back(i);
					f.m_inFaceStack = 1;
				}
			}

			SessionObserver observer(this);
			detail::processFaceStack(m_mesh, vertexData, m_epsilonSquared,
			                         m_expansionScratch, m_faceList,
			                         m_failedHorizonEdges, observer);
		}

	public:
		IncrementalHull(const std::vector<vec3>& initialPoints, T tolerance)
			: m_tolerance(tolerance)
			, m_epsilonSquared(T(0))
			, m_nextFaceId(0)
			, m_failedHorizonEdges(0) {
			initialize(initialPoints.data(), initialPoints.size());
		}

		IncrementalHull(const vec3* initialPoints, size_t initialPointCount, T tolerance)
			: m_tolerance(tolerance)
			, m_epsilonSquared(T(0))
			, m_nextFaceId(0)
			, m_failedHorizonEdges(0) {
			initialize(initialPoints, initialPointCount);
		}

		// Convenience spelling with the tolerance first.
		IncrementalHull(T tolerance, const std::vector<vec3>& initialPoints)
			: IncrementalHull(initialPoints, tolerance) { }


	private:
		void initialize(const vec3* initialPoints, size_t initialPointCount) {
			using std::isfinite;
			if (!isfinite(m_tolerance) || !(m_tolerance > T(0))) {
				throw std::invalid_argument(
					"IncrementalHull: tolerance must be finite and strictly positive");
			}
			if (initialPointCount > 0 && initialPoints == nullptr) {
				throw std::invalid_argument(
					"IncrementalHull: initial point array is null");
			}
			for (size_t i=0;i<initialPointCount;i++) {
				if (!isFinite(initialPoints[i])) {
					throw std::invalid_argument(
						"IncrementalHull: initial point set contains a non-finite point");
				}
			}
			// Point storage is private to the session; the caller's array is not
			// referenced after this call.
			m_points.clear();
			if (initialPointCount > 0) {
				m_points.assign(initialPoints, initialPoints + initialPointCount);
			}
			m_epsilonSquared = m_tolerance * m_tolerance;
			buildInitialHull();
		}

	public:
		// Append a batch of points.  The input is copied; it may be released or
		// modified as soon as this function returns.  Returns the net draw update
		// for the batch.
		DrawUpdate append(const std::vector<vec3>& newPoints) {
			return append(newPoints.data(), newPoints.size());
		}

		DrawUpdate append(const vec3* newPoints, size_t newPointCount) {
			appendImplementation(newPoints, newPointCount);

			DrawUpdate update;
			if (newPointCount == 0) {
				return update;
			}

			update.deletedFaceIds = m_batchDeleted;
			update.addedFaces.reserve(m_batchAdded.size());
			for (const auto& face : m_batchAdded) {
				if (m_batchTransient.find(face.faceId) == m_batchTransient.end()) {
					update.addedFaces.push_back(face);
				}
			}

			return update;
		}

		// Complete self-owned copy of the current hull.  Points use permanent

		// Complete self-owned copy of the current hull.  Points use permanent
		// point indices and triangles are outward-facing CCW.
		Snapshot snapshot() const {
			Snapshot result;
			result.points = m_points;
			result.triangles.reserve(m_mesh.m_faces.size());
			for (size_t i=0;i<m_mesh.m_faces.size();i++) {
				if (m_mesh.m_faces[i].isDisabled()) {
					continue;
				}
				const auto vertices = m_mesh.getVertexIndicesOfFace(m_mesh.m_faces[i]);
				result.triangles.push_back(Triangle(m_faceExternalIds[i], vertices));
			}
			return result;
		}

		size_t pointCount() const {
			return m_points.size();
		}

		// Alias for callers which prefer an explicit getter name.
		Snapshot getSnapshot() const {
			return snapshot();
		}

		size_t faceCount() const {
			return m_mesh.m_faces.size() - m_mesh.m_disabledFaces.size();
		}

		const std::vector<vec3>& points() const {
			return m_points;
		}

		size_t failedHorizonEdges() const {
			return m_failedHorizonEdges;
		}
	};

}

#endif /* QUICKHULL_INCREMENTALHULL_HPP_ */
