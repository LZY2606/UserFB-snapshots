// Demonstrates the optional continuous self-intersection guard (meshopt_SimplifyNoSelfIntersections).
//
// It builds two thin shells that are close but provably disjoint: a shallow cupped dome and a small
// flat patch floating just above its rim. It writes the input as before.obj, simplifies the same mesh
// with and without the guard and writes the guarded result as after.obj. Without the guard some edge
// collapses pull dome triangles straight through the floating patch; the guard rejects exactly those
// moves while leaving ordinary simplification untouched.
//
// Usage: collision_simplify <output-directory>
// C++11 and the standard library only; no GPU, model download or scripting required.
#include "meshoptimizer.h"

#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace
{

struct Vertex
{
	float x, y, z;
};

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Two-ring shallow dome: a raised center vertex, an inner ring and an outer ring, all triangulated.
void addDome(std::vector<Vertex>& vertices, std::vector<unsigned int>& indices, int segments)
{
	unsigned int center = unsigned(vertices.size());
	vertices.push_back(Vertex{0.f, 0.f, 0.10f});

	auto ringIndex = [segments](int ring, int i) -> unsigned int {
		return 1 + (ring - 1) * segments + (i % segments);
	};

	for (int ring = 1; ring <= 2; ++ring)
	{
		float radius = 0.5f * ring;
		float z = 0.05f * (2 - ring); // inner ring higher than the outer rim
		for (int i = 0; i < segments; ++i)
		{
			float a = float(2 * M_PI * i / segments);
			vertices.push_back(Vertex{radius * cosf(a), radius * sinf(a), z});
		}
	}

	unsigned int base = center;
	for (int i = 0; i < segments; ++i)
	{
		indices.push_back(base);
		indices.push_back(base + ringIndex(1, i));
		indices.push_back(base + ringIndex(1, i + 1));
	}
	for (int i = 0; i < segments; ++i)
	{
		indices.push_back(base + ringIndex(1, i));
		indices.push_back(base + ringIndex(2, i));
		indices.push_back(base + ringIndex(1, i + 1));
		indices.push_back(base + ringIndex(1, i + 1));
		indices.push_back(base + ringIndex(2, i));
		indices.push_back(base + ringIndex(2, i + 1));
	}
}

// A small flat quad (two triangles) at height bz centered at (centerX, 0); stays strictly above the dome.
void addFloatingPatch(std::vector<Vertex>& vertices, std::vector<unsigned int>& indices,
                      float centerX, float bz, float half)
{
	unsigned int bv = unsigned(vertices.size());
	vertices.push_back(Vertex{centerX - half, -half, bz});
	vertices.push_back(Vertex{centerX + half, -half, bz});
	vertices.push_back(Vertex{centerX - half, half, bz});
	vertices.push_back(Vertex{centerX + half, half, bz});
	indices.push_back(bv);
	indices.push_back(bv + 1);
	indices.push_back(bv + 2);
	indices.push_back(bv + 2);
	indices.push_back(bv + 1);
	indices.push_back(bv + 3);
}

bool writeObj(const std::string& path, const std::vector<Vertex>& vertices,
              const std::vector<unsigned int>& indices, size_t index_count)
{
	FILE* f = fopen(path.c_str(), "w");
	if (!f)
		return false;

	for (size_t i = 0; i < vertices.size(); ++i)
		fprintf(f, "v %g %g %g\n", vertices[i].x, vertices[i].y, vertices[i].z);

	for (size_t i = 0; i + 2 < index_count; i += 3)
		fprintf(f, "f %u %u %u\n", indices[i] + 1, indices[i + 1] + 1, indices[i + 2] + 1);

	fclose(f);
	return true;
}

void printExplanation()
{
	printf("Continuous collision guard (meshopt_SimplifyNoSelfIntersections)\n");
	printf("-----------------------------------------------------------------\n");
	printf("Basis of the continuous decision:\n");
	printf("  During an edge collapse the source vertex travels along a straight line to the target.\n");
	printf("  Every moving triangle therefore sweeps the tetrahedron conv(A, B, P0, P1) over the closed\n");
	printf("  interval t in [0,1]. A continuous query is an exact triangle-vs-tetrahedron separating-axis\n");
	printf("  test (face normals plus all edge cross products, with in-plane axes for flat/coplanar\n");
	printf("  sweeps). No fixed time sampling is used and both endpoints are included, so ordinary\n");
	printf("  penetration, point/edge contact of non-adjacent faces and coplanar positive-area overlap\n");
	printf("  all block the move. Faces sharing a geometric vertex (seam wedges included) or edge with a\n");
	printf("  moving face are treated as legal adjacency and exempt, so a connected sheet is not locked.\n");
	printf("Conservative rejection scope:\n");
	printf("  Numerically ambiguous candidates are rejected rather than accepted. The guard only protects\n");
	printf("  distant (non-adjacent) faces; it does not promise to remove every local fold of adjacent\n");
	printf("  triangles, where the existing winding/flip checks still apply. Rejected candidates mutate no\n");
	printf("  state, and a blocked run returns the real partial result instead of the whole input.\n");
	printf("Option limits:\n");
	printf("  Supported together with Sparse, LockBorder and ErrorAbsolute. It is not combined with Prune,\n");
	printf("  Permissive or PreserveFolds, and meshopt_simplifySloppy is unchanged. WithUpdate treats each\n");
	printf("  solved position as a straight-line per-vertex move and keeps the original position when unsafe.\n\n");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
		return 2;
	}

	std::vector<Vertex> vertices;
	std::vector<unsigned int> indices;
	addDome(vertices, indices, 24);
	// floating patch stays strictly above the dome's highest point in its footprint; the two shells
	// are disjoint yet close enough that some collapse sweeps reach the patch
	addFloatingPatch(vertices, indices, 0.25f, 0.06f, 0.10f);

	if (!writeObj(std::string(argv[1]) + "/before.obj", vertices, indices, indices.size()))
	{
		fprintf(stderr, "failed to write before.obj\n");
		return 1;
	}

	size_t target = indices.size() / 10; // aggressive enough to expose a guard-only difference

	std::vector<unsigned int> plain(indices.size());
	float result_error = 0.f;
	size_t plain_count = meshopt_simplify(plain.data(), indices.data(), indices.size(),
	    &vertices[0].x, vertices.size(), sizeof(Vertex), target, 1.f, 0, &result_error);

	std::vector<unsigned int> guarded(indices.size());
	meshopt_simplifyResetCollisionCounters();
	size_t guarded_count = meshopt_simplify(guarded.data(), indices.data(), indices.size(),
	    &vertices[0].x, vertices.size(), sizeof(Vertex), target, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &result_error);

	if (!writeObj(std::string(argv[1]) + "/after.obj", vertices, guarded, guarded_count))
	{
		fprintf(stderr, "failed to write after.obj\n");
		return 1;
	}

	printExplanation();
	const meshopt_SimplifyCollisionCounters* k = meshopt_simplifyCollisionCounters();
	printf("input triangles : %zu\n", indices.size() / 3);
	printf("plain output    : %zu triangles\n", plain_count / 3);
	printf("guarded output  : %zu triangles\n", guarded_count / 3);
	printf("guard counters  : candidates=%zu narrowphase=%zu collapses=%zu rejected=%zu local_updates=%zu rebuilds=%zu\n",
	    k->candidates, k->narrowphase_tests, k->collapses, k->collapse_rejected, k->local_updates, k->index_rebuilds);
	printf("wrote %s/before.obj and %s/after.obj\n", argv[1], argv[1]);
	return 0;
}
