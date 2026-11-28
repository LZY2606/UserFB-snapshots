// Multi-pose simplification session example.
//
// Builds two discrete poses of the same grid (a flat rest pose and a folded pose), creates one persistent
// meshopt_PoseSimplifier session, and produces a shared-vertex LOD chain by repeatedly calling
// meshopt_simplifyPoseStep with progressively larger error budgets. Every LOD reuses original vertex ids;
// the input positions are never moved and the two poses stay perfectly registered.
//
// Compile with a C++11 compiler, e.g.:
//   c++ -std=c++11 -Isrc demo/pose_simplify.cpp libmeshoptimizer.a -o pose_simplify
//
// This example only uses the C++11 standard library and the public meshoptimizer C API.

#include "meshoptimizer.h"

#include <cstdio>
#include <vector>

int main()
{
	const unsigned n = 17;
	const size_t vertex_count = size_t(n) * n;
	const size_t stride = 3 * sizeof(float);

	// every pose uses the same connectivity and vertex identity
	std::vector<unsigned int> indices;
	for (unsigned y = 0; y + 1 < n; ++y)
		for (unsigned x = 0; x + 1 < n; ++x)
		{
			unsigned int a = y * n + x, b = a + 1, c = a + n, d = c + 1;
			unsigned int tri[6] = {a, b, c, b, d, c};
			indices.insert(indices.end(), tri, tri + 6);
		}

	// pose 0: flat grid; pose 1: the same grid with alternating vertices lifted into folds
	std::vector<float> rest, folded;
	for (unsigned y = 0; y < n; ++y)
		for (unsigned x = 0; x < n; ++x)
		{
			rest.push_back(float(x));
			rest.push_back(float(y));
			rest.push_back(0.f);

			folded.push_back(float(x));
			folded.push_back(float(y));
			folded.push_back((x % 2 && y % 2) ? 2.f : 0.f);
		}

	const float* positions[2] = {rest.data(), folded.data()};
	const size_t strides[2] = {stride, stride};

	// vertex_lock can be NULL; here every vertex is free to collapse (border geometry stays protected anyway)
	meshopt_PoseSimplifier* session =
	    meshopt_createPoseSimplifier(indices.data(), indices.size(), vertex_count, positions, strides, 2, NULL);

	if (!session)
	{
		std::printf("failed to create pose simplifier (invalid input)\n");
		return 1;
	}

	// buffers supplied by the caller; the session keeps its own copies, so the sources can change after this
	std::vector<unsigned int> lod(indices.size());
	std::vector<unsigned int> remap(vertex_count);

	const size_t lod_targets[3] = {1200, 900, 768};
	const float error_budgets[3] = {0.005f, 0.02f, 0.1f};

	for (int level = 0; level < 3; ++level)
	{
		// each step continues from the preserved quadrics/clusters instead of restarting from scratch;
		// raising target_error lets the session accept progressively more expensive collapses
		size_t index_count = meshopt_simplifyPoseStep(session, lod_targets[level], error_budgets[level]);

		float result_error = 0.f;
		meshopt_readPoseSimplifier(session, lod.data(), remap.data(), &result_error);

		std::printf("LOD %d: %zu indices (target %zu), cumulative relative error %.6f\n", level, index_count,
		            lod_targets[level], result_error);

		// lod[0..index_count) references original vertices; remap[v] is the current representative of vertex v.
		// The same index buffer can be rendered with either pose's position array.
	}

	meshopt_destroyPoseSimplifier(session);
	return 0;
}
