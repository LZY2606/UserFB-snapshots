// This example demonstrates a persistent multi-pose simplification session.
// It builds a small grid mesh in two poses (a flat plane and a plane with a raised fold),
// simplifies both poses together while keeping a shared vertex numbering, and prints the
// resulting LOD and the accumulated error.
//
// Only the C++11 standard library is used.
#include "meshoptimizer.h"

#include <stdio.h>
#include <vector>

struct PoseMesh
{
	size_t columns;
	size_t rows;
	std::vector<float> positions; // XYZ per vertex
	std::vector<unsigned int> indices;
};

// Builds a grid mesh with the given number of columns/rows; fold_depth raises alternating interior vertices in Z.
static PoseMesh buildGrid(size_t columns, size_t rows, float fold_depth)
{
	PoseMesh mesh;
	mesh.columns = columns;
	mesh.rows = rows;
	mesh.positions.resize(columns * rows * 3);

	for (size_t y = 0; y < rows; ++y)
		for (size_t x = 0; x < columns; ++x)
		{
			size_t v = y * columns + x;
			mesh.positions[v * 3 + 0] = float(x);
			mesh.positions[v * 3 + 1] = float(y);
			mesh.positions[v * 3 + 2] = (fold_depth > 0 && (x % 2 == 1) && (y % 2 == 1)) ? fold_depth : 0.f;
		}

	for (size_t y = 0; y + 1 < rows; ++y)
		for (size_t x = 0; x + 1 < columns; ++x)
		{
			unsigned int a = unsigned(y * columns + x);
			unsigned int b = a + 1;
			unsigned int c = a + unsigned(columns);
			unsigned int d = c + 1;

			unsigned int t[] = {a, b, c, b, d, c};
			mesh.indices.insert(mesh.indices.end(), t, t + 6);
		}

	return mesh;
}

int main()
{
	const size_t pose_count = 2;
	PoseMesh poses[pose_count] = {
	    buildGrid(17, 17, 0.f), // flat pose
	    buildGrid(17, 17, 2.f), // folded pose
	};

	const size_t vertex_count = poses[0].positions.size() / 3;
	const size_t index_count = poses[0].indices.size();

	// all poses share connectivity
	std::vector<unsigned int> shared_indices = poses[0].indices;

	const float* positions[pose_count];
	size_t strides[pose_count];

	for (size_t k = 0; k < pose_count; ++k)
	{
		positions[k] = &poses[k].positions[0];
		strides[k] = sizeof(float) * 3;
	}

	// create the session; source buffers can be released after this call
	meshopt_PoseSimplifier* session = meshopt_createPoseSimplifier(&shared_indices[0], shared_indices.size(), vertex_count, positions, strides, pose_count, NULL);

	if (!session)
	{
		printf("failed to create pose simplifier\n");
		return 1;
	}

	// generate several LODs by stepping down the index budget while increasing the error limit
	static const size_t targets[] = {index_count / 2, index_count / 4, index_count / 8};
	static const float errors[] = {0.01f, 0.05f, 0.1f};

	std::vector<unsigned int> lod(index_count);
	std::vector<unsigned int> remap(vertex_count);

	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
	{
		size_t count = meshopt_simplifyPoseStep(session, targets[i], errors[i]);
		float result_error = 0;

		meshopt_readPoseSimplifier(session, &lod[0], &remap[0], &result_error);

		printf("LOD %d: %d indices, %d triangles, error %.4f\n", int(i), int(count), int(count / 3), result_error);
	}

	// the session retains the original cluster quadrics; releasing it frees all pose state
	meshopt_destroyPoseSimplifier(session);

	return 0;
}
