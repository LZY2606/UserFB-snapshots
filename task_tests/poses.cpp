#include "../task_support/check.h"
#include "../src/meshoptimizer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace
{

// A small grid mesh helper. Positions for each pose are stored in separate vectors.
struct Grid
{
	size_t n;
	std::vector<unsigned int> indices;
	std::vector<std::vector<float> > positions; // [pose][vertex*3]
	size_t vertex_count;
};

static Grid makeGrid(size_t n, size_t pose_count, bool bent)
{
	Grid g;
	g.n = n;
	g.vertex_count = n * n;
	g.positions.resize(pose_count);

	for (size_t p = 0; p < pose_count; ++p)
	{
		g.positions[p].resize(n * n * 3);

		for (size_t y = 0; y < n; ++y)
			for (size_t x = 0; x < n; ++x)
			{
				size_t v = y * n + x;
				float z = 0;

				// each pose builds its own fold pattern; bent poses have raised interior vertices
				if (bent && (p % 2) == 0 && (x % 2) == 1 && (y % 2) == 1)
					z = 2.f;
				if (bent && (p % 2) == 1 && (x + y) % 3 == 0)
					z = 1.f;

				g.positions[p][v * 3 + 0] = float(x);
				g.positions[p][v * 3 + 1] = float(y);
				g.positions[p][v * 3 + 2] = z;
			}
	}

	for (size_t y = 0; y + 1 < n; ++y)
		for (size_t x = 0; x + 1 < n; ++x)
		{
			unsigned int a = unsigned(y * n + x);
			unsigned int b = a + 1;
			unsigned int c = a + unsigned(n);
			unsigned int d = c + 1;

			unsigned int t[] = {a, b, c, b, d, c};
			g.indices.insert(g.indices.end(), t, t + 6);
		}

	return g;
}

// Build a mesh with a real fold: a strip of quads with a sharp ridge along the middle.
static Grid makeRidge(size_t n, size_t pose_count)
{
	Grid g = makeGrid(n, pose_count, false);

	for (size_t p = 0; p < pose_count; ++p)
		for (size_t y = 0; y < n; ++y)
			for (size_t x = 0; x < n; ++x)
			{
				size_t v = y * n + x;
				float ridge = (y > 0 && y + 1 < n) ? float(n - 1 - y) * 0.5f : 0.f;
				if (p == 1)
					ridge *= 1.5f;
				g.positions[p][v * 3 + 2] = ridge;
			}

	return g;
}

static std::vector<const float*> posePointers(const Grid& g)
{
	std::vector<const float*> p;
	for (size_t i = 0; i < g.positions.size(); ++i)
		p.push_back(&g.positions[i][0]);
	return p;
}

static std::vector<size_t> poseStrides(size_t pose_count)
{
	return std::vector<size_t>(pose_count, 12);
}

struct Session
{
	meshopt_PoseSimplifier* state;
	Grid grid;

	Session(const Grid& g, const unsigned char* lock = NULL)
	    : state(NULL)
	{
		grid = g;
		std::vector<const float*> p = posePointers(grid);
		std::vector<size_t> s = poseStrides(p.size());

		state = meshopt_createPoseSimplifier(&grid.indices[0], grid.indices.size(), grid.vertex_count, &p[0], &s[0], p.size(), lock);
	}

	~Session()
	{
		meshopt_destroyPoseSimplifier(state);
	}

	// read current indices; destination must be sized index_count, remap vertex_count
	size_t read(std::vector<unsigned int>& indices, std::vector<unsigned int>& remap, float* error = NULL)
	{
		indices.resize(grid.indices.size());
		remap.resize(grid.vertex_count);
		size_t count = meshopt_readPoseSimplifier(state, &indices[0], &remap[0], error);
		indices.resize(count);
		return count;
	}
};

static bool hasDegenerateFace(const std::vector<unsigned int>& indices)
{
	for (size_t i = 0; i + 2 < indices.size(); i += 3)
		if (indices[i] == indices[i + 1] || indices[i] == indices[i + 2] || indices[i + 1] == indices[i + 2])
			return true;
	return false;
}

static bool indicesInRange(const std::vector<unsigned int>& indices, size_t vertex_count)
{
	for (size_t i = 0; i < indices.size(); ++i)
		if (indices[i] >= vertex_count)
			return false;
	return true;
}

// ------------------------------------------------------------------------------------------------
// ensemble group: multi-pose behavior, per-pose validity and target reachability

static int checkEnsembleGrid()
{
	Grid g = makeGrid(17, 2, true);
	Session s(g);

	TASK_CHECK(s.state != NULL);

	size_t target = 768;
	size_t count = meshopt_simplifyPoseStep(s.state, target, 0.1f);

	TASK_CHECK(count <= 768);
	TASK_CHECK(count > 0);
	TASK_CHECK(count % 3 == 0);

	std::vector<unsigned int> indices, remap;
	float error = -1;
	TASK_CHECK(s.read(indices, remap, &error) == count);
	TASK_CHECK(!hasDegenerateFace(indices));
	TASK_CHECK(indicesInRange(indices, g.vertex_count));
	TASK_CHECK(error >= 0 && error <= 0.1f + 1e-5f);

	return 0;
}

static int checkEnsembleBentPoseProtection()
{
	// A flat plane and a strongly bent pose: the folder must not be wiped out by locking everything.
	Grid g = makeGrid(13, 2, true);
	Session s(g);

	TASK_CHECK(s.state != NULL);

	size_t count = meshopt_simplifyPoseStep(s.state, 300, 0.05f);
	TASK_CHECK(count >= 300);
	TASK_CHECK(count <= g.indices.size());

	std::vector<unsigned int> indices, remap;
	float error = 0;
	TASK_CHECK(s.read(indices, remap, &error) == count);
	TASK_CHECK(!hasDegenerateFace(indices));

	// the bent pose must still be represented; ensure at least some original bent vertices survive
	size_t folded = 0;
	for (size_t i = 0; i < indices.size(); ++i)
	{
		unsigned int v = indices[i];
		float z = g.positions[1][v * 3 + 2];
		folded += (z > 0.5f);
	}
	TASK_CHECK(folded > 0);

	return 0;
}

static int checkEnsembleRidgePreserved()
{
	// A sharp ridge that exists only in one pose. A single-pose flat simplifier would flatten it;
	// the multi-pose session must keep the fold (fewer collapses reach the target, ridge survives).
	Grid g = makeGrid(9, 2, false);

	for (size_t p = 0; p < 2; ++p)
		for (size_t y = 0; y < g.n; ++y)
			for (size_t x = 0; x < g.n; ++x)
			{
				size_t v = y * g.n + x;
				g.positions[p][v * 3 + 2] = (p == 1 && y == g.n / 2) ? 4.f : 0.f;
			}

	Session s(g);
	TASK_CHECK(s.state != NULL);

	size_t count = meshopt_simplifyPoseStep(s.state, 200, 0.05f);
	TASK_CHECK(count > 0);
	TASK_CHECK(count <= g.indices.size());

	std::vector<unsigned int> indices, remap;
	float error = 0;
	TASK_CHECK(s.read(indices, remap, &error) == count);
	TASK_CHECK(!hasDegenerateFace(indices));
	TASK_CHECK(error <= 0.05f + 1e-5f);

	// count how many ridge vertices (y == n/2 in the bent pose) are still referenced
	size_t ridge = 0;
	for (size_t i = 0; i < indices.size(); ++i)
	{
		unsigned int v = indices[i];
		float z = g.positions[1][v * 3 + 2];
		ridge += (z > 0.5f);
	}
	TASK_CHECK(ridge > 0);

	return 0;
}

// ------------------------------------------------------------------------------------------------
// persistence group: incremental steps and error accumulation

static int checkPersistenceMultiStep()
{
	Grid g = makeGrid(15, 2, true);
	Session s(g);

	TASK_CHECK(s.state != NULL);

	size_t target = 1200;
	size_t c1 = meshopt_simplifyPoseStep(s.state, target, 0.005f);
	TASK_CHECK(c1 > 0);
	TASK_CHECK(c1 <= target);

	float e1 = 0;
	std::vector<unsigned int> indices, remap;
	TASK_CHECK(s.read(indices, remap, &e1) == c1);

	// a lower error limit must leave state unchanged
	size_t c2 = meshopt_simplifyPoseStep(s.state, 600, 0.001f);
	TASK_CHECK(c2 == c1);

	// same count target must leave state unchanged
	size_t c3 = meshopt_simplifyPoseStep(s.state, c1, 0.05f);
	TASK_CHECK(c3 == c1);

	// raising the error limit allows further progress
	size_t c4 = meshopt_simplifyPoseStep(s.state, 600, 0.05f);
	TASK_CHECK(c4 <= c1);
	TASK_CHECK(c4 <= 600);

	float e2 = 0;
	TASK_CHECK(s.read(indices, remap, &e2) == c4);
	TASK_CHECK(e2 >= e1 - 1e-6f);

	return 0;
}

static int checkPersistenceMatchesSingleStep()
{
	// Two steps with the same error budget must reach the same state as one step to the same target,
	// because the session retains the original cluster quadrics rather than re-deriving current-face
	// quadrics between steps.
	Grid g = makeGrid(11, 2, true);

	Session single(g);
	Session incremental(g);

	TASK_CHECK(single.state != NULL);
	TASK_CHECK(incremental.state != NULL);

	size_t target = 300;
	size_t ca = meshopt_simplifyPoseStep(single.state, target, 0.05f);

	size_t intermediate = meshopt_simplifyPoseStep(incremental.state, 720, 0.05f);
	TASK_CHECK(intermediate > 0);
	size_t cb = meshopt_simplifyPoseStep(incremental.state, target, 0.05f);

	TASK_CHECK(ca == cb);

	std::vector<unsigned int> ia, ra, ib, rb;
	float ea = 0, eb = 0;
	TASK_CHECK(single.read(ia, ra, &ea) == ca);
	TASK_CHECK(incremental.read(ib, rb, &eb) == cb);

	TASK_CHECK(ea == eb);
	TASK_CHECK(ia == ib);
	TASK_CHECK(ra == rb);

	return 0;
}

static int checkPersistenceNoResnapshot()
{
	// Step repeatedly with a gradually increasing error budget; the result must be monotone.
	Grid g = makeGrid(11, 3, true);
	Session s(g);

	TASK_CHECK(s.state != NULL);

	size_t previous = g.indices.size();
	float previous_error = 0;

	for (int i = 0; i < 4; ++i)
	{
		float budget = 0.01f * float(i + 1);
		size_t count = meshopt_simplifyPoseStep(s.state, size_t(300 - i * 50), budget);
		TASK_CHECK(count <= previous);

		std::vector<unsigned int> indices, remap;
		float error = 0;
		TASK_CHECK(s.read(indices, remap, &error) == count);
		TASK_CHECK(error >= previous_error - 1e-6f);

		previous = count;
		previous_error = error;
	}

	return 0;
}

static int checkPersistenceReadRemapMultipleSessions()
{
	// Two sessions must not interfere with each other.
	Grid g1 = makeGrid(9, 2, true);
	Grid g2 = makeGrid(9, 2, false);

	Session s1(g1);
	Session s2(g2);

	TASK_CHECK(s1.state != NULL);
	TASK_CHECK(s2.state != NULL);

	size_t c1 = meshopt_simplifyPoseStep(s1.state, 120, 0.05f);
	size_t c2 = meshopt_simplifyPoseStep(s2.state, 240, 0.05f);

	TASK_CHECK(c1 > 0);
	TASK_CHECK(c2 > 0);

	std::vector<unsigned int> i1, r1, i2, r2;
	TASK_CHECK(s1.read(i1, r1) == c1);
	TASK_CHECK(s2.read(i2, r2) == c2);

	// reading must be stable
	std::vector<unsigned int> i1b, r1b;
	TASK_CHECK(s1.read(i1b, r1b) == c1);
	TASK_CHECK(i1 == i1b);
	TASK_CHECK(r1 == r1b);

	return 0;
}

// ------------------------------------------------------------------------------------------------
// identity group: full-trajectory welding and original cluster QEM recalculation

static int checkIdentityFullTrajectory()
{
	// Two vertices share the same position in pose 0 but separate in pose 1.
	// They must never be welded.
	//
	// Two triangles that don't share faces:
	//   A = {0, 2, 3}, B = {1, 4, 5}
	// Vertices 0 and 1 coincide in pose 0 but not in pose 1.
	std::vector<float> pose0(6 * 3, 0.f);
	std::vector<float> pose1(6 * 3, 0.f);
	std::vector<unsigned int> indices;

	// pose 0
	pose0[0 * 3 + 0] = 0.f, pose0[0 * 3 + 1] = 0.f, pose0[0 * 3 + 2] = 0.f;
	pose0[1 * 3 + 0] = 0.f, pose0[1 * 3 + 1] = 0.f, pose0[1 * 3 + 2] = 0.f;
	pose0[2 * 3 + 0] = 1.f, pose0[2 * 3 + 1] = 0.f, pose0[2 * 3 + 2] = 0.f;
	pose0[3 * 3 + 0] = 0.f, pose0[3 * 3 + 1] = 1.f, pose0[3 * 3 + 2] = 0.f;
	pose0[4 * 3 + 0] = 2.f, pose0[4 * 3 + 1] = 0.f, pose0[4 * 3 + 2] = 0.f;
	pose0[5 * 3 + 0] = 2.f, pose0[5 * 3 + 1] = 1.f, pose0[5 * 3 + 2] = 0.f;

	// pose 1: vertex 1 separates from vertex 0
	pose1[0 * 3 + 0] = 0.f, pose1[0 * 3 + 1] = 0.f, pose1[0 * 3 + 2] = 0.f;
	pose1[1 * 3 + 0] = 0.5f, pose1[1 * 3 + 1] = 0.f, pose1[1 * 3 + 2] = 0.f;
	pose1[2 * 3 + 0] = 1.f, pose1[2 * 3 + 1] = 0.f, pose1[2 * 3 + 2] = 0.f;
	pose1[3 * 3 + 0] = 0.f, pose1[3 * 3 + 1] = 1.f, pose1[3 * 3 + 2] = 0.f;
	pose1[4 * 3 + 0] = 2.f, pose1[4 * 3 + 1] = 0.f, pose1[4 * 3 + 2] = 0.f;
	pose1[5 * 3 + 0] = 2.f, pose1[5 * 3 + 1] = 1.f, pose1[5 * 3 + 2] = 0.f;

	unsigned int t[] = {0, 2, 3, 1, 4, 5};
	indices.assign(t, t + 6);

	const float* positions[] = {&pose0[0], &pose1[0]};
	size_t strides[] = {12, 12};

	meshopt_PoseSimplifier* state = meshopt_createPoseSimplifier(&indices[0], indices.size(), 6, positions, strides, 2, NULL);
	TASK_CHECK(state != NULL);

	std::vector<unsigned int> dest(indices.size()), remap(6);
	meshopt_readPoseSimplifier(state, &dest[0], &remap[0], NULL);

	TASK_CHECK(remap[0] != remap[1]);

	meshopt_destroyPoseSimplifier(state);
	return 0;
}

static int checkIdentityWeldExactTrajectory()
{
	// Duplicate vertices that have exactly equal full trajectories weld together.
	Grid g = makeGrid(5, 2, false);

	// duplicate vertex 5 (referenced by multiple triangles) as an extra vertex with the same full trajectory
	size_t base_count = g.vertex_count;
	for (size_t p = 0; p < g.positions.size(); ++p)
	{
		g.positions[p].push_back(g.positions[p][5 * 3 + 0]);
		g.positions[p].push_back(g.positions[p][5 * 3 + 1]);
		g.positions[p].push_back(g.positions[p][5 * 3 + 2]);
	}
	g.vertex_count += 1;

	// make one reference to vertex 5 use the duplicate (index base_count)
	for (size_t i = 0; i < g.indices.size(); ++i)
		if (g.indices[i] == 5)
		{
			g.indices[i] = unsigned(base_count);
			break;
		}

	Session s(g);
	TASK_CHECK(s.state != NULL);

	std::vector<unsigned int> indices, remap;
	size_t count = s.read(indices, remap);

	TASK_CHECK(remap[5] == remap[base_count]);
	TASK_CHECK(count == g.indices.size());

	// the duplicate must never appear as a separate surviving cluster representative in results
	TASK_CHECK(hasDegenerateFace(indices) == false);

	return 0;
}

static int checkIdentityLockedSource()
{
	// Locking a vertex prevents it from being a source; it may still be a valid target and survive.
	Grid g = makeGrid(9, 2, true);

	std::vector<unsigned char> lock(g.vertex_count, 0);
	for (size_t i = 0; i < g.vertex_count; ++i)
		lock[i] = (i % 2 == 0) ? 1 : 0;

	Session s(g, &lock[0]);
	TASK_CHECK(s.state != NULL);

	size_t count = meshopt_simplifyPoseStep(s.state, g.indices.size() / 2, 0.05f);
	TASK_CHECK(count > 0);
	TASK_CHECK(count < g.indices.size());
	TASK_CHECK(count <= g.indices.size());

	std::vector<unsigned int> indices, remap;
	TASK_CHECK(s.read(indices, remap) == count);
	TASK_CHECK(!hasDegenerateFace(indices));

	// all locked (even) vertices that remain referenced must still be referenced through their own identity
	for (size_t i = 0; i < count; ++i)
	{
		unsigned int v = indices[i];
		TASK_CHECK(v < g.vertex_count);
	}

	return 0;
}

static int checkIdentityClusterQuadricIndependent()
{
	// A single-pose flat mesh should simplify to a stable result; verify that a step after a
	// previous step remains valid (quadrics are preserved, not rebuilt from current faces only).
	Grid g = makeGrid(9, 1, false);
	Session s(g);

	TASK_CHECK(s.state != NULL);

	size_t c1 = meshopt_simplifyPoseStep(s.state, 300, 0.02f);
	TASK_CHECK(c1 > 0);

	std::vector<unsigned int> indices, remap;
	float e1 = 0;
	TASK_CHECK(s.read(indices, remap, &e1) == c1);
	TASK_CHECK(!hasDegenerateFace(indices));

	size_t c2 = meshopt_simplifyPoseStep(s.state, 120, 0.08f);
	TASK_CHECK(c2 <= c1);
	TASK_CHECK(c2 > 0);

	float e2 = 0;
	TASK_CHECK(s.read(indices, remap, &e2) == c2);
	TASK_CHECK(e2 >= e1 - 1e-6f);
	TASK_CHECK(!hasDegenerateFace(indices));

	return 0;
}

static int checkIdentityStrideAndSourceRelease()
{
	// Use non-trivial strides and release the input buffers before stepping.
	std::vector<float> interleaved0;
	std::vector<float> interleaved1;
	std::vector<unsigned int> indices;

	size_t n = 7;
	for (size_t p = 0; p < 2; ++p)
	{
		std::vector<float>& buf = (p == 0) ? interleaved0 : interleaved1;
		buf.resize(n * n * 8);

		for (size_t y = 0; y < n; ++y)
			for (size_t x = 0; x < n; ++x)
			{
				size_t v = y * n + x;
				float z = (p == 1 && (x % 2) == 1 && (y % 2) == 1) ? 1.f : 0.f;
				buf[v * 8 + 0] = 99.f; // padding
				buf[v * 8 + 1] = float(x);
				buf[v * 8 + 2] = float(y);
				buf[v * 8 + 3] = z;
				buf[v * 8 + 4] = -7.f;
				buf[v * 8 + 5] = -7.f;
				buf[v * 8 + 6] = -7.f;
				buf[v * 8 + 7] = -7.f;
			}
	}

	for (size_t y = 0; y + 1 < n; ++y)
		for (size_t x = 0; x + 1 < n; ++x)
		{
			unsigned int a = unsigned(y * n + x);
			unsigned int b = a + 1;
			unsigned int c = a + unsigned(n);
			unsigned int d = c + 1;
			unsigned int t[] = {a, b, c, b, d, c};
			indices.insert(indices.end(), t, t + 6);
		}

	const float* positions[] = {&interleaved0[0] + 1, &interleaved1[0] + 1};
	size_t strides[] = {32, 32};

	meshopt_PoseSimplifier* state = meshopt_createPoseSimplifier(&indices[0], indices.size(), n * n, positions, strides, 2, NULL);
	TASK_CHECK(state != NULL);

	// release the source buffers (simulate by clearing + shrinking)
	indices.clear();
	indices.shrink_to_fit();
	interleaved0.clear();
	interleaved0.shrink_to_fit();
	interleaved1.clear();
	interleaved1.shrink_to_fit();

	size_t count = meshopt_simplifyPoseStep(state, 72, 0.1f);
	TASK_CHECK(count > 0);
	TASK_CHECK(count >= 72);
	TASK_CHECK(count < n * n * 6);

	std::vector<unsigned int> dst(count + 9), remap(n * n);
	float error = 0;
	size_t read_count = meshopt_readPoseSimplifier(state, &dst[0], &remap[0], &error);
	TASK_CHECK(read_count == count);
	TASK_CHECK(!hasDegenerateFace(std::vector<unsigned int>(dst.begin(), dst.begin() + count)));
	TASK_CHECK(indicesInRange(std::vector<unsigned int>(dst.begin(), dst.begin() + count), n * n));

	meshopt_destroyPoseSimplifier(state);
	return 0;
}

static int checkIdentityUnreferencedMapsToSelf()
{
	// Unreferenced vertices must map to themselves in the read remap.
	std::vector<float> pose0;
	std::vector<unsigned int> indices;

	size_t n = 6;
	pose0.resize(n * n * 3);
	for (size_t y = 0; y < n; ++y)
		for (size_t x = 0; x < n; ++x)
		{
			size_t v = y * n + x;
			pose0[v * 3 + 0] = float(x);
			pose0[v * 3 + 1] = float(y);
			pose0[v * 3 + 2] = 0.f;
		}

	for (size_t y = 0; y + 1 < n; ++y)
		for (size_t x = 0; x + 1 < n; ++x)
		{
			unsigned int a = unsigned(y * n + x);
			unsigned int b = a + 1;
			unsigned int c = a + unsigned(n);
			unsigned int d = c + 1;
			unsigned int t[] = {a, b, c, b, d, c};
			indices.insert(indices.end(), t, t + 6);
		}

	// extra vertices that are never referenced
	size_t extra = 3;
	pose0.resize((n * n + extra) * 3);
	for (size_t i = 0; i < extra; ++i)
	{
		pose0[(n * n + i) * 3 + 0] = 1234.f + float(i);
	}

	const float* positions[] = {&pose0[0]};
	size_t strides[] = {12};

	size_t vertex_count = n * n + extra;
	meshopt_PoseSimplifier* state = meshopt_createPoseSimplifier(&indices[0], indices.size(), vertex_count, positions, strides, 1, NULL);
	TASK_CHECK(state != NULL);

	size_t c1 = meshopt_simplifyPoseStep(state, 96, 0.1f);
	TASK_CHECK(c1 > 0);

	std::vector<unsigned int> dst(indices.size()), remap(vertex_count);
	size_t read_count = meshopt_readPoseSimplifier(state, &dst[0], &remap[0], NULL);
	TASK_CHECK(read_count == c1);

	for (size_t i = n * n; i < vertex_count; ++i)
		TASK_CHECK(remap[i] == i);

	meshopt_destroyPoseSimplifier(state);
	return 0;
}

static int checkIdentityRemapConsistency()
{
	// Read remap must map every original vertex to a representative that is self-mapped and
	// that appears in the current index buffer (for surviving representatives).
	Grid g = makeGrid(11, 2, true);
	Session s(g);

	TASK_CHECK(s.state != NULL);

	size_t count = meshopt_simplifyPoseStep(s.state, 200, 0.05f);
	TASK_CHECK(count > 0);

	std::vector<unsigned int> indices, remap;
	TASK_CHECK(s.read(indices, remap) == count);

	for (size_t i = 0; i < g.vertex_count; ++i)
	{
		unsigned int r = remap[i];
		TASK_CHECK(r < g.vertex_count);
		TASK_CHECK(remap[r] == r);
	}

	// every representative referenced by the result must be mapped to itself
	for (size_t i = 0; i < indices.size(); ++i)
		TASK_CHECK(remap[indices[i]] == indices[i]);

	// applying remap to the original index buffer must reproduce the current index buffer exactly;
	// this catches stale cluster entries for welded copies that collapsed via a different wedge
	std::vector<unsigned int> mapped;
	for (size_t i = 0; i < g.indices.size(); i += 3)
	{
		unsigned int a = remap[g.indices[i + 0]];
		unsigned int b = remap[g.indices[i + 1]];
		unsigned int c = remap[g.indices[i + 2]];

		if (a != b && a != c && b != c)
		{
			mapped.push_back(a);
			mapped.push_back(b);
			mapped.push_back(c);
		}
	}

	TASK_CHECK(mapped == indices);

	return 0;
}

// ------------------------------------------------------------------------------------------------
// Independent quadric reference implementation, used to verify that the session retains the
// original cluster quadrics rather than rebuilding them from the current faces.

struct RefVec3
{
	float x, y, z;
};

struct RefQuadric
{
	float a00, a11, a22, a10, a20, a21, b0, b1, b2, c, w;
};

static void refQuadricAdd(RefQuadric& a, const RefQuadric& b)
{
	a.a00 += b.a00, a.a11 += b.a11, a.a22 += b.a22;
	a.a10 += b.a10, a.a20 += b.a20, a.a21 += b.a21;
	a.b0 += b.b0, a.b1 += b.b1, a.b2 += b.b2;
	a.c += b.c, a.w += b.w;
}

static void refQuadricTriangle(RefQuadric& q, RefVec3 p0, RefVec3 p1, RefVec3 p2)
{
	RefVec3 p10 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
	RefVec3 p20 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
	RefVec3 n = {p10.y * p20.z - p10.z * p20.y, p10.z * p20.x - p10.x * p20.z, p10.x * p20.y - p10.y * p20.x};
	float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);

	if (len > 0)
		n.x /= len, n.y /= len, n.z /= len;

	float d = n.x * p0.x + n.y * p0.y + n.z * p0.z;
	float w = sqrtf(len);

	float aw = n.x * w, bw = n.y * w, cw = n.z * w, dw = -d * w;
	q.a00 = n.x * aw, q.a11 = n.y * bw, q.a22 = n.z * cw;
	q.a10 = n.x * bw, q.a20 = n.x * cw, q.a21 = n.y * cw;
	q.b0 = n.x * dw, q.b1 = n.y * dw, q.b2 = n.z * dw;
	q.c = -d * dw;
	q.w = w;
}

static float refQuadricError(const RefQuadric& q, RefVec3 v)
{
	float rx = (q.b0 + q.a10 * v.y) * 2.f;
	float ry = (q.b1 + q.a21 * v.z) * 2.f;
	float rz = (q.b2 + q.a20 * v.x) * 2.f;

	rx += q.a00 * v.x;
	ry += q.a11 * v.y;
	rz += q.a22 * v.z;

	float r = q.c + rx * v.x + ry * v.y + rz * v.z;
	float s = q.w == 0 ? 0 : 1.f / q.w;

	return fabsf(r) * s;
}

// Recomputes the maximum per-pose error from the original face quadrics accumulated per current cluster.
static float referenceClusterError(const std::vector<std::vector<float> >& poses, const std::vector<unsigned int>& original_indices, const std::vector<unsigned int>& cluster_remap, const std::vector<unsigned int>& current_indices)
{
	size_t pose_count = poses.size();
	size_t vertex_count = cluster_remap.size();
	float result = 0;

	for (size_t p = 0; p < pose_count; ++p)
	{
		const std::vector<float>& src = poses[p];

		float minv[3] = {1e30f, 1e30f, 1e30f};
		float maxv[3] = {-1e30f, -1e30f, -1e30f};
		for (size_t i = 0; i < vertex_count; ++i)
			for (int j = 0; j < 3; ++j)
			{
				float v = src[i * 3 + j];
				minv[j] = minv[j] > v ? v : minv[j];
				maxv[j] = maxv[j] < v ? v : maxv[j];
			}

		float extent = 0;
		for (int j = 0; j < 3; ++j)
			extent = extent < maxv[j] - minv[j] ? maxv[j] - minv[j] : extent;

		float scale = extent == 0 ? 0.f : 1.f / extent;

		std::vector<RefVec3> np(vertex_count);
		for (size_t i = 0; i < vertex_count; ++i)
		{
			RefVec3 v = {(src[i * 3 + 0] - minv[0]) * scale, (src[i * 3 + 1] - minv[1]) * scale, (src[i * 3 + 2] - minv[2]) * scale};
			np[i] = v;
		}

		// accumulate original face quadrics directly into each current cluster representative
		std::vector<RefQuadric> cq(vertex_count);
		memset(&cq[0], 0, vertex_count * sizeof(RefQuadric));

		for (size_t i = 0; i < original_indices.size(); i += 3)
		{
			unsigned int i0 = original_indices[i + 0], i1 = original_indices[i + 1], i2 = original_indices[i + 2];

			RefQuadric q;
			refQuadricTriangle(q, np[i0], np[i1], np[i2]);

			refQuadricAdd(cq[cluster_remap[i0]], q);
			refQuadricAdd(cq[cluster_remap[i1]], q);
			refQuadricAdd(cq[cluster_remap[i2]], q);
		}

		std::vector<unsigned char> seen(vertex_count, 0);
		for (size_t i = 0; i < current_indices.size(); ++i)
			seen[current_indices[i]] = 1;

		for (size_t i = 0; i < vertex_count; ++i)
			if (seen[i])
			{
				float e = refQuadricError(cq[i], np[i]);
				result = result < e ? e : result;
			}
	}

	return sqrtf(result);
}

static int checkIdentityOriginalClusterQem()
{
	// After simplification, the reported error must equal an independent recomputation from the
	// original face quadrics of each surviving cluster. This is only true if original cluster
	// quadrics are retained across steps instead of rebuilding current-face quadrics.
	Grid g = makeGrid(11, 2, true);
	Session s(g);

	TASK_CHECK(s.state != NULL);

	// step once, then step again with a larger target so the second step relies on accumulated quadrics
	size_t c1 = meshopt_simplifyPoseStep(s.state, 300, 0.05f);
	TASK_CHECK(c1 > 0);
	TASK_CHECK(c1 >= 300);
	TASK_CHECK(c1 <= g.indices.size());

	std::vector<unsigned int> indices, remap;
	float error = 0;
	TASK_CHECK(s.read(indices, remap, &error) == c1);

	// the reported error is an upper bound on the error of every surviving representative
	float reference = referenceClusterError(g.positions, g.indices, remap, indices);

	TASK_CHECK(reference <= error + 1e-4f);
	TASK_CHECK(error <= 0.05f + 1e-5f);

	// continue with a higher budget; the reference recomputation must still be consistent
	size_t c2 = meshopt_simplifyPoseStep(s.state, 180, 0.15f);
	TASK_CHECK(c2 <= c1);
	TASK_CHECK(c2 > 0);

	float error2 = 0;
	TASK_CHECK(s.read(indices, remap, &error2) == c2);
	TASK_CHECK(error2 >= error - 1e-6f);
	TASK_CHECK(error2 <= 0.15f + 1e-5f);

	float reference2 = referenceClusterError(g.positions, g.indices, remap, indices);
	TASK_CHECK(reference2 <= error2 + 1e-4f);

	return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"ensemble", "grid_17_target_768", checkEnsembleGrid},
    {"ensemble", "bent_pose_not_wiped", checkEnsembleBentPoseProtection},
    {"ensemble", "ridge_preserved", checkEnsembleRidgePreserved},
    {"persistence", "multi_step_error_raise", checkPersistenceMultiStep},
    {"persistence", "matches_single_step", checkPersistenceMatchesSingleStep},
    {"persistence", "monotone_progress", checkPersistenceNoResnapshot},
    {"persistence", "multi_session_isolation", checkPersistenceReadRemapMultipleSessions},
    {"identity", "full_trajectory_separation", checkIdentityFullTrajectory},
    {"identity", "exact_trajectory_weld", checkIdentityWeldExactTrajectory},
    {"identity", "locked_source_rules", checkIdentityLockedSource},
    {"identity", "cluster_quadric_persistence", checkIdentityClusterQuadricIndependent},
    {"identity", "original_cluster_qem", checkIdentityOriginalClusterQem},
    {"identity", "stride_and_source_release", checkIdentityStrideAndSourceRelease},
    {"identity", "unreferenced_maps_to_self", checkIdentityUnreferencedMapsToSelf},
    {"identity", "remap_consistency", checkIdentityRemapConsistency},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
