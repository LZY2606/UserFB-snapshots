// Disk meshlet example: generates an annulus (ring) sheet, splits it into topological disk
// clusters with meshopt_buildMeshletsDisk and saves each cluster as an OBJ group.
#include "src/meshoptimizer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
static void makeDir(const char* path)
{
	_mkdir(path);
}
#else
#include <sys/stat.h>
static void makeDir(const char* path)
{
	mkdir(path, 0755);
}
#endif

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
		return 1;
	}

	// annulus sheet: a flat ring with a center hole, triangulated as a rings x sectors grid
	const int sectors = 96;
	const int rings = 4;

	std::vector<float> positions;
	std::vector<unsigned int> indices;

	for (int r = 0; r <= rings; ++r)
		for (int s = 0; s < sectors; ++s)
		{
			float angle = float(s) * (2.f * 3.14159265f) / float(sectors);
			float radius = 1.f + 0.5f * float(r);

			positions.push_back(cosf(angle) * radius);
			positions.push_back(sinf(angle) * radius);
			positions.push_back(0.f);
		}

	for (int r = 0; r < rings; ++r)
		for (int s = 0; s < sectors; ++s)
		{
			unsigned int v00 = unsigned(r * sectors + s), v10 = unsigned(r * sectors + (s + 1) % sectors);
			unsigned int v01 = v00 + sectors, v11 = v10 + sectors;

			indices.push_back(v00);
			indices.push_back(v10);
			indices.push_back(v11);
			indices.push_back(v00);
			indices.push_back(v11);
			indices.push_back(v01);
		}

	size_t vertex_count = positions.size() / 3;
	size_t face_count = indices.size() / 3;

	size_t max_vertices = 64;
	size_t max_triangles = 128;

	// disk and triangle_source arrays are sized to the input face count, the rest to the index count
	std::vector<meshopt_MeshletDisk> disks(face_count);
	std::vector<unsigned int> meshlet_vertices(indices.size());
	std::vector<unsigned char> meshlet_triangles(indices.size());
	std::vector<unsigned int> triangle_source(face_count);
	std::vector<unsigned char> boundary_vertices(indices.size());

	size_t disk_count = meshopt_buildMeshletsDisk(disks.data(), meshlet_vertices.data(), meshlet_triangles.data(), triangle_source.data(), boundary_vertices.data(),
	    indices.data(), indices.size(), positions.data(), vertex_count, sizeof(float) * 3, max_vertices, max_triangles, 0.f);

	makeDir(argv[1]);

	std::string path = std::string(argv[1]) + "/result.obj";
	FILE* obj = fopen(path.c_str(), "w");
	if (!obj)
	{
		fprintf(stderr, "cannot open %s for writing\n", path.c_str());
		return 1;
	}

	for (size_t i = 0; i < vertex_count; ++i)
		fprintf(obj, "v %g %g %g\n", positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);

	for (size_t i = 0; i < disk_count; ++i)
	{
		const meshopt_Meshlet& m = disks[i].meshlet;

		fprintf(obj, "g cluster_%zu\n", i);

		// compute the Euler characteristic independently from the cluster faces
		std::set<std::pair<unsigned int, unsigned int> > edges;

		for (unsigned int t = 0; t < m.triangle_count; ++t)
		{
			unsigned int tri[3];

			for (int j = 0; j < 3; ++j)
			{
				unsigned int local = meshlet_triangles[m.triangle_offset + t * 3 + j];
				tri[j] = meshlet_vertices[m.vertex_offset + local];
			}

			fprintf(obj, "f %u %u %u\n", tri[0] + 1, tri[1] + 1, tri[2] + 1);

			for (int j = 0; j < 3; ++j)
			{
				unsigned int a = tri[j], b = tri[(j + 1) % 3];
				edges.insert(a < b ? std::make_pair(a, b) : std::make_pair(b, a));
			}
		}

		int euler = int(m.vertex_count) - int(edges.size()) + int(m.triangle_count);

		printf("cluster %zu: vertices=%u triangles=%u euler=%d boundary=%u\n", i, m.vertex_count, m.triangle_count, euler, disks[i].boundary_count);
	}

	fclose(obj);

	printf("wrote %s: %zu disk clusters from %zu vertices and %zu triangles\n", path.c_str(), disk_count, vertex_count, face_count);
	return 0;
}
