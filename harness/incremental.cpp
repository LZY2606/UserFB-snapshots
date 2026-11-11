// Incremental hull harness.
//
// Exercises the IncrementalHull session on the same point set using two fixed
// batching strategies.  At every step it verifies the hull as a closed oriented
// manifold, checks the Euler characteristic, verifies that all historical
// points are inside every supporting plane, and compares volume/support
// geometry against a one-shot QuickHull build.  A client-side face table
// applies only the returned draw updates and keeps random integer materials so
// that face-slot reuse cannot silently transfer state.

#include "IncrementalHull.hpp"
#include "QuickHull.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using FloatType = double;
using Vec3 = quickhull::Vector3<FloatType>;
using Hull = quickhull::IncrementalHull<FloatType>;
using Snapshot = Hull::Snapshot;
using Triangle = Hull::Triangle;
using DrawUpdate = Hull::DrawUpdate;

static const FloatType kTolerance = 1e-7;
static const FloatType kOneShotEps = 1e-9;

#define REQUIRE(condition, what)                                                     \
	do {                                                                             \
		if (!(condition)) {                                                          \
			std::cerr << "incremental harness check failed: " << (what)               \
			          << " [" << __FILE__ << ":" << __LINE__ << "]" << std::endl;      \
			std::exit(1);                                                            \
		}                                                                            \
	} while (0)

static Vec3 crossProduct(const Vec3& a, const Vec3& b) {
	return Vec3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}

static double signedVolume(const std::vector<Vec3>& points,
                           const std::vector<Triangle>& triangles) {
	double sixVolume = 0;
	for (const auto& t : triangles) {
		const Vec3& a = points[t.vertexIndices[0]];
		const Vec3& b = points[t.vertexIndices[1]];
		const Vec3& c = points[t.vertexIndices[2]];
		sixVolume += a.dotProduct(crossProduct(b, c));
	}
	return sixVolume / 6.0;
}

static double signedVolume(const std::vector<Vec3>& points,
                           const std::vector<std::size_t>& indices) {
	double sixVolume = 0;
	for (std::size_t i=0;i<indices.size();i+=3) {
		const Vec3& a = points[indices[i+0]];
		const Vec3& b = points[indices[i+1]];
		const Vec3& c = points[indices[i+2]];
		sixVolume += a.dotProduct(crossProduct(b, c));
	}
	return sixVolume / 6.0;
}

struct EdgeKey {
	std::size_t a, b;
	EdgeKey(std::size_t va, std::size_t vb) : a(va), b(vb) { }
	bool operator<(const EdgeKey& o) const {
		return a != o.a ? a < o.a : b < o.b;
	}
};

static std::set<EdgeKey> uniqueEdges(const std::vector<Triangle>& triangles) {
	std::set<EdgeKey> edges;
	for (const auto& t : triangles) {
		for (int k=0;k<3;k++) {
			std::size_t a = t.vertexIndices[k];
			std::size_t b = t.vertexIndices[(k+1)%3];
			if (a > b) {
				std::swap(a, b);
			}
			edges.insert(EdgeKey(a, b));
		}
	}
	return edges;
}

static void checkManifold(const Snapshot& snapshot, const char* label) {
	REQUIRE(!snapshot.triangles.empty(), std::string(label) + ": empty hull");
	REQUIRE(snapshot.points.size() >= 4, std::string(label) + ": too few points");

	std::map<std::pair<std::size_t,std::size_t>, std::size_t> directed;
	for (const auto& t : snapshot.triangles) {
		for (int k=0;k<3;k++) {
			directed[std::make_pair(t.vertexIndices[k], t.vertexIndices[(k+1)%3])]++;
		}
	}

	// Every directed edge occurs exactly once; its reverse must also occur
	// exactly once.  This is equivalent to every undirected edge having exactly
	// two incident, oppositely oriented faces.
	for (const auto& t : snapshot.triangles) {
		for (int k=0;k<3;k++) {
			const std::size_t a = t.vertexIndices[k];
			const std::size_t b = t.vertexIndices[(k+1)%3];
			REQUIRE(directed[std::make_pair(a,b)] == 1,
			        std::string(label) + ": duplicate directed edge");
			REQUIRE(directed[std::make_pair(b,a)] == 1,
			        std::string(label) + ": edge without exactly one opposite face");
		}
	}

	std::set<std::size_t> vertices;
	for (const auto& t : snapshot.triangles) {
		vertices.insert(t.vertexIndices[0]);
		vertices.insert(t.vertexIndices[1]);
		vertices.insert(t.vertexIndices[2]);
	}
	const std::set<EdgeKey> edges = uniqueEdges(snapshot.triangles);
	const long long V = static_cast<long long>(vertices.size());
	const long long E = static_cast<long long>(edges.size());
	const long long F = static_cast<long long>(snapshot.triangles.size());
	REQUIRE(V - E + F == 2,
	        std::string(label) + ": Euler characteristic is not 2");

	// Outward CCW orientation: positive signed volume and every historical
	// point on or behind every face plane.
	const double volume = signedVolume(snapshot.points, snapshot.triangles);
	REQUIRE(volume > 0, std::string(label) + ": triangles are not outward CCW");

	const double planeSlack = 4.0*kTolerance + 1e-9;
	for (const auto& t : snapshot.triangles) {
		const Vec3& a = snapshot.points[t.vertexIndices[0]];
		const Vec3& b = snapshot.points[t.vertexIndices[1]];
		const Vec3& c = snapshot.points[t.vertexIndices[2]];
		const Vec3 n = crossProduct(b-a, c-a);
		const double normalLengthSquared = n.getLengthSquared();
		REQUIRE(normalLengthSquared > 1e-24,
		        std::string(label) + ": degenerate triangle");
		const double normalLength = std::sqrt(normalLengthSquared);
		for (const auto& p : snapshot.points) {
			REQUIRE(n.dotProduct(p-a) <= planeSlack*normalLength,
			        std::string(label) + ": historical point outside a supporting plane");
		}
	}
}

struct ReferenceHull {
	std::vector<Vec3> vertices;
	std::vector<std::size_t> indices;
	double volume;
};

static ReferenceHull buildReferenceHull(const std::vector<Vec3>& points,
                                        const char* label) {
	quickhull::QuickHull<FloatType> qh;
	auto hull = qh.getConvexHull(points, false, false, kOneShotEps);
	REQUIRE(qh.getDiagnostics().m_failedHorizonEdges == 0,
	        std::string(label) + ": one-shot hull had failed horizon edges");

	ReferenceHull ref;
	const auto& vb = hull.getVertexBuffer();
	for (std::size_t i=0;i<vb.size();i++) {
		ref.vertices.push_back(vb[i]);
	}
	ref.indices = hull.getIndexBuffer();
	REQUIRE(ref.indices.size() >= 12, std::string(label) + ": one-shot hull is empty");
	ref.volume = signedVolume(ref.vertices, ref.indices);
	REQUIRE(ref.volume > 0, std::string(label) + ": one-shot reference orientation");
	return ref;
}

static const std::vector<Vec3>& supportDirections() {
	static const std::vector<Vec3> directions = {
		Vec3(1,0,0), Vec3(-1,0,0),
		Vec3(0,1,0), Vec3(0,-1,0),
		Vec3(0,0,1), Vec3(0,0,-1),
		Vec3(1,1,1), Vec3(1,-1,1), Vec3(-1,1,1), Vec3(-1,-1,1),
		Vec3(0.3,-0.7,0.5), Vec3(0.9,0.1,-0.4),
		Vec3(-0.2,0.4,-0.9), Vec3(0.55,0.83,-0.1)
	};
	return directions;
}

static double supportValue(const std::vector<Vec3>& points, const Vec3& direction) {
	double best = -std::numeric_limits<double>::infinity();
	for (const auto& p : points) {
		const double d = p.dotProduct(direction);
		if (d > best) {
			best = d;
		}
	}
	return best;
}

static void compareWithOneShot(const Snapshot& snapshot,
                               const std::vector<Vec3>& history,
                               const char* label) {
	const ReferenceHull ref = buildReferenceHull(history, label);
	const double volume = signedVolume(snapshot.points, snapshot.triangles);

	// Coplanar facets may be triangulated with different diagonals, so compare
	// the enclosed volume and the support function instead of triangle lists.
	REQUIRE(std::abs(volume - ref.volume) <= 1e-6*(1.0 + std::abs(ref.volume)),
	        std::string(label) + ": volume differs from one-shot hull");

	const double supportSlack = 8.0*kTolerance + 1e-9;
	for (const auto& direction : supportDirections()) {
		const double incrementalSupport = supportValue(snapshot.points, direction);
		const double referenceSupport = supportValue(ref.vertices, direction);
		REQUIRE(std::abs(incrementalSupport - referenceSupport) <= supportSlack,
		        std::string(label) + ": support geometry differs from one-shot hull");
	}

	const double planeSlack = 4.0*kTolerance + 1e-9;

	// Both hulls must enclose each other's vertices.
	for (const auto& t : snapshot.triangles) {
		const Vec3& a = snapshot.points[t.vertexIndices[0]];
		const Vec3& b = snapshot.points[t.vertexIndices[1]];
		const Vec3& c = snapshot.points[t.vertexIndices[2]];
		const Vec3 n = crossProduct(b-a, c-a);
		const double nl = std::sqrt(n.getLengthSquared());
		for (const auto& p : ref.vertices) {
			REQUIRE(n.dotProduct(p-a) <= planeSlack*nl,
			        std::string(label) + ": one-shot vertex outside incremental hull");
		}
	}

	for (std::size_t i=0;i<ref.indices.size();i+=3) {
		const Vec3& a = ref.vertices[ref.indices[i+0]];
		const Vec3& b = ref.vertices[ref.indices[i+1]];
		const Vec3& c = ref.vertices[ref.indices[i+2]];
		const Vec3 n = crossProduct(b-a, c-a);
		const double nl = std::sqrt(n.getLengthSquared());
		REQUIRE(nl > 1e-12, std::string(label) + ": degenerate one-shot triangle");
		for (const auto& p : snapshot.points) {
			REQUIRE(n.dotProduct(p-a) <= planeSlack*nl,
			        std::string(label) + ": incremental point outside one-shot hull");
		}
	}
}

static void checkSnapshotGeometry(const Snapshot& snapshot,
                                  const std::vector<Vec3>& history,
                                  const char* label) {
	REQUIRE(snapshot.points.size() == history.size(),
	        std::string(label) + ": stored point buffer does not match history");
	for (std::size_t i=0;i<history.size();i++) {
		REQUIRE(snapshot.points[i] == history[i],
		        std::string(label) + ": permanent point index/value mismatch");
	}
	checkManifold(snapshot, label);
	compareWithOneShot(snapshot, history, label);
}

static bool sameTopology(const Snapshot& a, const Snapshot& b) {
	if (a.triangles.size() != b.triangles.size()) {
		return false;
	}
	std::map<std::size_t, std::array<std::size_t,3>> faceMap;
	for (const auto& t : a.triangles) {
		faceMap[t.faceId] = t.vertexIndices;
	}
	for (const auto& t : b.triangles) {
		auto it = faceMap.find(t.faceId);
		if (it == faceMap.end() || it->second != t.vertexIndices) {
			return false;
		}
	}
	return true;
}

static bool sameSnapshot(const Snapshot& a, const Snapshot& b) {
	if (a.points.size() != b.points.size()) {
		return false;
	}
	for (std::size_t i=0;i<a.points.size();i++) {
		if (!(a.points[i] == b.points[i])) {
			return false;
		}
	}
	return sameTopology(a, b);
}

class ClientFaceTable {
	struct Entry {
		std::array<std::size_t,3> vertices;
		int material;
		Entry() : vertices{{0,0,0}}, material(0) { }
		Entry(const std::array<std::size_t,3>& v, int m) : vertices(v), material(m) { }
	};

	std::map<std::size_t,Entry> m_faces;
	std::set<std::size_t> m_everUsedIds;
	std::mt19937 m_rng;
	std::uniform_int_distribution<int> m_materialDistribution;

	int newMaterial() {
		return m_materialDistribution(m_rng);
	}

public:
	ClientFaceTable(const Snapshot& initial, unsigned int seed)
		: m_rng(seed)
		, m_materialDistribution(-1000000000, 1000000000) {
		for (const auto& t : initial.triangles) {
			REQUIRE(m_faces.find(t.faceId) == m_faces.end(),
			        "initial snapshot has duplicate face ids");
			REQUIRE(m_everUsedIds.insert(t.faceId).second,
			        "initial snapshot reused a face id");
			m_faces[t.faceId] = Entry(t.vertexIndices, newMaterial());
		}
	}

	void apply(const DrawUpdate& update, const Snapshot& finalSnapshot,
	           const char* label) {
		std::map<std::size_t,int> materialsBefore;
		for (const auto& kv : m_faces) {
			materialsBefore[kv.first] = kv.second.material;
		}

		std::set<std::size_t> deleted;
		for (const auto id : update.deletedFaceIds) {
			REQUIRE(deleted.insert(id).second,
			        std::string(label) + ": face deleted twice in one update");
			auto it = m_faces.find(id);
			REQUIRE(it != m_faces.end(),
			        std::string(label) + ": update deletes a face not in the table");
			m_faces.erase(it);
		}

		for (const auto& face : update.addedFaces) {
			REQUIRE(!m_faces.count(face.faceId),
			        std::string(label) + ": added face id is still present");
			REQUIRE(!m_everUsedIds.count(face.faceId),
			        std::string(label) + ": external face id was reused");
			m_everUsedIds.insert(face.faceId);
			m_faces[face.faceId] = Entry(face.vertexIndices, newMaterial());
		}

		REQUIRE(m_faces.size() == finalSnapshot.triangles.size(),
		        std::string(label) + ": client face table size differs from snapshot");

		for (const auto& t : finalSnapshot.triangles) {
			auto it = m_faces.find(t.faceId);
			REQUIRE(it != m_faces.end(),
			        std::string(label) + ": snapshot face missing from client table");
			REQUIRE(it->second.vertices == t.vertexIndices,
			        std::string(label) + ": client face winding/vertices differ");
		}
		// Materials of faces which were not touched by the update must survive.
		for (const auto& kv : materialsBefore) {
			if (deleted.count(kv.first)) {
				continue;
			}
			auto it = m_faces.find(kv.first);
			REQUIRE(it != m_faces.end(),
			        std::string(label) + ": unchanged face disappeared");
			REQUIRE(it->second.material == kv.second,
			        std::string(label) + ": unchanged face material changed");
		}

		// Every external id that was inserted is now backed by a live face.
		for (const auto& kv : m_faces) {
			REQUIRE(m_everUsedIds.count(kv.first) == 1,
			        std::string(label) + ": live face id was never recorded");
		}
	}
};

struct BatchSpec {
	std::vector<Vec3> points;
	bool expectNoChange;
	bool releaseInput;
	BatchSpec() : expectNoChange(false), releaseInput(false) { }
	BatchSpec(const std::vector<Vec3>& p, bool noChange, bool release)
		: points(p), expectNoChange(noChange), releaseInput(release) { }
};

static void verifyInvalidBatchRejected(Hull& hull,
                                       const std::vector<Vec3>& expectedHistory,
                                       const char* label) {
	const Snapshot before = hull.snapshot();
	const std::size_t beforePointCount = hull.pointCount();

	std::vector<Vec3> bad;
	bad.push_back(Vec3(0.3, 0.4, 0.5));
	bad.push_back(Vec3(std::numeric_limits<FloatType>::quiet_NaN(), 0.0, 0.0));
	bool threw = false;
	try {
		hull.append(bad);
	}
	catch (const std::invalid_argument&) {
		threw = true;
	}
	REQUIRE(threw, std::string(label) + ": NaN batch was accepted");
	REQUIRE(hull.pointCount() == beforePointCount,
	        std::string(label) + ": rejected batch consumed point indices");
	REQUIRE(sameSnapshot(before, hull.snapshot()),
	        std::string(label) + ": rejected NaN batch modified the hull");

	std::vector<Vec3> badInfinity;
	badInfinity.push_back(
		Vec3(std::numeric_limits<FloatType>::infinity(), 1.0, 2.0));
	threw = false;
	try {
		hull.append(badInfinity);
	}
	catch (const std::invalid_argument&) {
		threw = true;
	}
	REQUIRE(threw, std::string(label) + ": infinite batch was accepted");
	REQUIRE(hull.pointCount() == beforePointCount,
	        std::string(label) + ": rejected infinite batch consumed point indices");
	REQUIRE(sameSnapshot(before, hull.snapshot()),
	        std::string(label) + ": rejected infinite batch modified the hull");

	// The rejected points were not numbered; the stored prefix is unchanged.
	const Snapshot now = hull.snapshot();
	REQUIRE(now.points.size() == expectedHistory.size(),
	        std::string(label) + ": accepted point numbering changed");
	for (std::size_t i=0;i<expectedHistory.size();i++) {
		REQUIRE(now.points[i] == expectedHistory[i],
		        std::string(label) + ": rejected batch changed stored points");
	}
}

static std::size_t countExistingDeletedFaces(const Snapshot& before,
                                             const DrawUpdate& update) {
	std::size_t count = 0;
	std::map<std::size_t, bool> existing;
	for (const auto& t : before.triangles) {
		existing[t.faceId] = true;
	}
	for (const auto id : update.deletedFaceIds) {
		if (existing.find(id) != existing.end()) {
			count++;
		}
	}
	return count;
}

static bool runScenario(const std::string& name,
                        const std::vector<Vec3>& initial,
                        const std::vector<BatchSpec>& batches,
                        unsigned int seed) {
	Hull hull(initial, kTolerance);
	std::vector<Vec3> history = initial;

	ClientFaceTable client(hull.snapshot(), seed);

	checkSnapshotGeometry(hull.snapshot(), history, (name + " initial").c_str());

	const Snapshot initialSnapshot = hull.snapshot();
	const std::vector<Vec3> oldPoints = initialSnapshot.points;
	const std::vector<Triangle> oldTriangles = initialSnapshot.triangles;

	bool invalidBatchDone = false;
	bool sawPartialUpdate = false;

	for (std::size_t b=0;b<batches.size();b++) {
		const BatchSpec& batch = batches[b];
		const Snapshot before = hull.snapshot();

		DrawUpdate update;
		if (batch.releaseInput) {
			// The caller releases and overwrites its input immediately after the
			// append call.  The session must not retain a pointer into it.
			std::vector<Vec3>* heapInput = new std::vector<Vec3>(batch.points);
			update = hull.append(*heapInput);
			*heapInput = std::vector<Vec3>();
			delete heapInput;
		}
		else {
			std::vector<Vec3> mutableInput = batch.points;
			update = hull.append(mutableInput);
			for (auto& p : mutableInput) {
				p = Vec3(std::numeric_limits<FloatType>::quiet_NaN(), 0.0, 0.0);
			}
			mutableInput.clear();
		}

		history.insert(history.end(), batch.points.begin(), batch.points.end());

		REQUIRE(hull.pointCount() == history.size(),
		        name + ": point count after batch");

		const Snapshot after = hull.snapshot();
		checkSnapshotGeometry(after, history, (name + " after batch").c_str());

		if (batch.expectNoChange) {
			REQUIRE(update.deletedFaceIds.empty() &&
			        update.addedFaces.empty(),
			        name + ": interior/duplicate/empty batch changed the hull");
			REQUIRE(sameTopology(before, after),
			        name + ": interior/duplicate/empty batch changed face ids");
		}
		else {
			const std::size_t existingDeleted =
				countExistingDeletedFaces(before, update);
			REQUIRE(existingDeleted == update.deletedFaceIds.size(),
			        name + ": update deletes unknown faces");
			const std::size_t unchanged =
				before.triangles.size() - existingDeleted;
			if (unchanged > 0 && (!update.deletedFaceIds.empty() ||
			                      !update.addedFaces.empty())) {
				sawPartialUpdate = true;
			}
		}

		// The client table is only built from the initial snapshot and then
		// updated sequentially.  It must always agree with the current snapshot.
		client.apply(update, after, (name + " draw update").c_str());

		if (!invalidBatchDone && !batch.points.empty()) {
			verifyInvalidBatchRejected(hull, history,
				(name + " invalid batch").c_str());
			invalidBatchDone = true;
		}
	}

	// The initial snapshot was taken before any growth.  It is a self-owned
	// value, so later appends (including vector reallocations) must leave it
	// untouched.
	const Snapshot& oldStillThere = initialSnapshot;
	REQUIRE(oldStillThere.points.size() == oldPoints.size(),
	        name + ": old snapshot point count changed");
	for (std::size_t i=0;i<oldPoints.size();i++) {
		REQUIRE(oldStillThere.points[i] == oldPoints[i],
		        name + ": old snapshot point data changed");
	}
	REQUIRE(oldStillThere.triangles.size() == oldTriangles.size(),
	        name + ": old snapshot triangle count changed");
	for (std::size_t i=0;i<oldTriangles.size();i++) {
		REQUIRE(oldStillThere.triangles[i].faceId == oldTriangles[i].faceId,
		        name + ": old snapshot face id changed");
		REQUIRE(oldStillThere.triangles[i].vertexIndices ==
		        oldTriangles[i].vertexIndices,
		        name + ": old snapshot face vertices changed");
	}

	REQUIRE(sawPartialUpdate,
	        name + ": no update preserved any unchanged face (full rebuild?)");
	REQUIRE(hull.failedHorizonEdges() == 0,
	        name + ": horizon edge solving failed");
	return true;
}

static void testStorageLifetime() {
	const std::vector<Vec3> initial = {
		Vec3(0.13, 0.21, 0.32),
		Vec3(1.27,-0.41, 0.03),
		Vec3(-0.52,1.16, 0.17),
		Vec3(0.24,-0.13, 1.38)
	};

	Hull hull(initial, kTolerance);
	const Snapshot oldSnapshot = hull.snapshot();

	const std::vector<Vec3> initialPoints = oldSnapshot.points;
	const std::vector<Triangle> initialTriangles = oldSnapshot.triangles;

	// Force several reallocations of the session's point buffer.  All points are
	// the centroid of the initial tetrahedron (deep inside), so the hull does not
	// change.
	const Vec3 centroid(0.28, 0.2075, 0.475);
	std::vector<Vec3> bulk(4000, centroid);
	hull.append(bulk);
	std::vector<Vec3> additional(2000, centroid);
	hull.append(additional);

	REQUIRE(oldSnapshot.points.size() == initialPoints.size(),
	        "storage-lifetime: old snapshot point count changed");
	for (std::size_t i=0;i<initialPoints.size();i++) {
		REQUIRE(oldSnapshot.points[i] == initialPoints[i],
		        "storage-lifetime: old snapshot point data changed");
	}
	REQUIRE(oldSnapshot.triangles.size() == initialTriangles.size(),
	        "storage-lifetime: old snapshot triangle count changed");
	for (std::size_t i=0;i<initialTriangles.size();i++) {
		REQUIRE(oldSnapshot.triangles[i].faceId ==
		        initialTriangles[i].faceId,
		        "storage-lifetime: old snapshot face id changed");
		REQUIRE(oldSnapshot.triangles[i].vertexIndices ==
		        initialTriangles[i].vertexIndices,
		        "storage-lifetime: old snapshot face vertices changed");
	}

	const Snapshot grown = hull.snapshot();
	REQUIRE(grown.points.size() == initial.size() + bulk.size() + additional.size(),
	        "storage-lifetime: point buffer did not grow");
	REQUIRE(grown.points[initial.size()] == bulk.front(),
	        "storage-lifetime: bulk point was not stored verbatim");
	REQUIRE(grown.points[initial.size()+bulk.size()] == additional.front(),
	        "storage-lifetime: additional point was not stored verbatim");
	REQUIRE(sameTopology(oldSnapshot, grown),
	        "storage-lifetime: interior bulk points changed hull topology");

	// Empty append (both vector and pointer/count overloads) is a no-op.
	const Snapshot beforeEmpty = hull.snapshot();
	hull.append(std::vector<Vec3>());
	hull.append(static_cast<const Vec3*>(nullptr), 0);
	REQUIRE(sameSnapshot(beforeEmpty, hull.snapshot()),
	        "storage-lifetime: empty append changed the session");
}

static std::vector<BatchSpec> makeBatchesA(const std::vector<Vec3>& tail,
                                           std::size_t boxCount,
                                           std::size_t far1,
                                           std::size_t far2) {
	std::vector<BatchSpec> batches;
	std::vector<Vec3> box(tail.begin(), tail.begin() + boxCount);
	batches.push_back(BatchSpec(box, false, false));
	batches.push_back(BatchSpec(std::vector<Vec3>(), true, false));
	std::vector<Vec3> f1(1, tail[far1]);
	batches.push_back(BatchSpec(f1, false, true));
	std::vector<Vec3> f2(1, tail[far2]);
	batches.push_back(BatchSpec(f2, false, false));
	std::vector<Vec3> interior(tail.begin() + far2 + 1, tail.end());
	batches.push_back(BatchSpec(interior, true, false));
	return batches;
}

static std::vector<BatchSpec> makeBatchesB(const std::vector<Vec3>& tail,
                                           std::size_t boxCount,
                                           std::size_t far1,
                                           std::size_t far2) {
	std::vector<BatchSpec> batches;
	batches.push_back(BatchSpec(
		std::vector<Vec3>(tail.begin(), tail.begin()+3), false, true));
	batches.push_back(BatchSpec(
		std::vector<Vec3>(tail.begin()+3, tail.begin()+6), false, false));
	batches.push_back(BatchSpec(
		std::vector<Vec3>(tail.begin()+6, tail.begin()+boxCount), false, false));
	batches.push_back(BatchSpec(
		std::vector<Vec3>(1, tail[far1]), false, false));
	batches.push_back(BatchSpec(
		std::vector<Vec3>(1, tail[far2]), false, false));
	std::vector<Vec3> interior(tail.begin() + far2 + 1, tail.end());
	batches.push_back(BatchSpec(
		std::vector<Vec3>(interior.begin(), interior.begin()+2), true, false));
	batches.push_back(BatchSpec(
		std::vector<Vec3>(interior.begin()+2, interior.end()), true, false));
	batches.push_back(BatchSpec(std::vector<Vec3>(), true, false));
	return batches;
}

int main() {
	// Asymmetric initial tetrahedron.  Every coordinate lies strictly inside the
	// bounding box which is added later.
	const std::vector<Vec3> initial = {
		Vec3(0.13, 0.21, 0.32),
		Vec3(1.27,-0.41, 0.03),
		Vec3(-0.52,1.16, 0.17),
		Vec3(0.24,-0.13, 1.38)
	};

	// Eight bounding-box corners.
	const std::vector<Vec3> box = {
		Vec3(-2,-2,-2), Vec3( 2,-2,-2), Vec3(-2, 2,-2), Vec3( 2, 2,-2),
		Vec3(-2,-2, 2), Vec3( 2,-2, 2), Vec3(-2, 2, 2), Vec3( 2, 2, 2)
	};

	// Two far points in clearly different directions.
	const Vec3 farPoint1(4.75, 0.35,-0.22);
	const Vec3 farPoint2(-0.31,-4.55, 0.61);

	// Points which are strictly inside the final hull, exact duplicates and a
	// near-duplicate of an existing vertex (well within the absolute tolerance).
	const std::vector<Vec3> interior = {
		Vec3(0.0, 0.0, 0.0),
		Vec3(0.13, 0.21, 0.32),
		Vec3(0.2, 0.2, 0.2),
		Vec3(-0.5, 0.4, 0.1),
		Vec3(1.27 + 0.05*kTolerance, -0.41, 0.03),
		Vec3(2.0, 2.0, 2.0)
	};

	std::vector<Vec3> tail = box;
	tail.push_back(farPoint1);
	tail.push_back(farPoint2);
	tail.insert(tail.end(), interior.begin(), interior.end());

	const std::size_t far1Index = box.size();
	const std::size_t far2Index = box.size() + 1;

	const std::vector<BatchSpec> batchesA =
		makeBatchesA(tail, box.size(), far1Index, far2Index);
	const std::vector<BatchSpec> batchesB =
		makeBatchesB(tail, box.size(), far1Index, far2Index);

	// Sanity: both batching schemes cover exactly the same point sequence.
	std::vector<Vec3> coveredA, coveredB;
	for (const auto& b : batchesA) {
		coveredA.insert(coveredA.end(), b.points.begin(), b.points.end());
	}
	for (const auto& b : batchesB) {
		coveredB.insert(coveredB.end(), b.points.begin(), b.points.end());
	}
	REQUIRE(coveredA.size() == tail.size() && coveredB.size() == tail.size(),
	        "batching schemes do not cover the same point count");
	for (std::size_t i=0;i<tail.size();i++) {
		REQUIRE(coveredA[i] == tail[i] && coveredB[i] == tail[i],
		        "batching schemes do not cover the same point sequence");
	}

	const std::size_t scenarioCount = 2;
	REQUIRE(scenarioCount > 0, "zero scenarios");

	runScenario("batching-A", initial, batchesA, 12345u);
	runScenario("batching-B", initial, batchesB, 54321u);
	testStorageLifetime();

	// Also exercise the local tetdrahedron rejection rules.
	bool threw = false;
	try {
		Hull degenerate(std::vector<Vec3>{
			Vec3(0,0,0), Vec3(1,0,0), Vec3(2,0,0), Vec3(3,0,0)}, kTolerance);
	}
	catch (const std::invalid_argument&) {
		threw = true;
	}
	REQUIRE(threw, "collinear initial point set was accepted");

	threw = false;
	try {
		Hull planar(std::vector<Vec3>{
			Vec3(0,0,0), Vec3(1,0,0), Vec3(0,1,0), Vec3(1,1,0)}, kTolerance);
	}
	catch (const std::invalid_argument&) {
		threw = true;
	}
	REQUIRE(threw, "coplanar initial point set was accepted");

	threw = false;
	try {
		Hull badTolerance(initial, 0.0);
	}
	catch (const std::invalid_argument&) {
		threw = true;
	}
	REQUIRE(threw, "non-positive tolerance was accepted");

	std::cout << "PASS hull-geometry" << std::endl;
	std::cout << "PASS draw-delta" << std::endl;
	std::cout << "PASS storage-lifetime" << std::endl;
	std::cout << "QUICKHULL_INCREMENTAL_OK" << std::endl;
	return 0;
}
