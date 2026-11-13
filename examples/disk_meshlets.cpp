// Disk meshlet example: builds disk meshlets for an annulus (a sheet with a center hole),
// writes the clusters as OBJ groups and prints per-cluster topology statistics.
#include "src/meshoptimizer.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
		return 2;
	}

	const int ntheta = 16;
	const int nrings = 3;
	const float r0 = 0.4f;
	const float r1 = 1.0f;

	// annulus vertices: nrings+1 concentric rings, each with ntheta vertices
	std::vector<float> vertices;

	for (int r = 0; r <= nrings; ++r)
	{
		float radius = r0 + (r1 - r0) * (float(r) / float(nrings));

		for (int t = 0; t < ntheta; ++t)
		{
			float angle = 2.f * float(M_PI) * float(t) / float(ntheta);

			vertices.push_back(radius * cosf(angle));
			vertices.push_back(radius * sinf(angle));
			vertices.push_back(0.f);
		}
	}

	auto vid = [](int r, int t) -> unsigned int { return unsigned(r * ntheta + t); };

	// annulus triangles: each quad between adjacent rings is split into two consistently wound triangles
	std::vector<unsigned int> indices;

	for (int r = 0; r < nrings; ++r)
	{
		for (int t = 0; t < ntheta; ++t)
		{
			int tn = (t + 1) % ntheta;

			unsigned int a = vid(r, t), b = vid(r, tn);
			unsigned int c = vid(r + 1, t), d = vid(r + 1, tn);

			indices.push_back(a);
			indices.push_back(c);
			indices.push_back(b);

			indices.push_back(b);
			indices.push_back(c);
			indices.push_back(d);
		}
	}

	size_t index_count = indices.size();
	size_t face_count = index_count / 3;
	size_t vertex_count = vertices.size() / 3;

	const size_t max_vertices = 64;
	const size_t max_triangles = 64;

	std::vector<meshopt_MeshletDisk> disks(face_count);
	std::vector<unsigned int> meshlet_vertices(index_count);
	std::vector<unsigned char> meshlet_triangles(index_count);
	std::vector<unsigned int> triangle_source(face_count);
	std::vector<unsigned char> boundary_vertices(index_count);

	size_t disk_count = meshopt_buildMeshletsDisk(disks.data(), meshlet_vertices.data(), meshlet_triangles.data(),
	    triangle_source.data(), boundary_vertices.data(), indices.data(), index_count, vertices.data(), vertex_count,
	    sizeof(float) * 3, max_vertices, max_triangles, 0.f);

	assert(disk_count > 0);

	mkdir(argv[1], 0755);

	char path[1024];
	snprintf(path, sizeof(path), "%s/result.obj", argv[1]);

	FILE* file = fopen(path, "wb");
	if (!file)
	{
		fprintf(stderr, "failed to open %s\n", path);
		return 1;
	}

	fprintf(file, "# annulus split into %zu disk meshlets\n", disk_count);

	for (size_t i = 0; i < vertex_count; ++i)
		fprintf(file, "v %g %g %g\n", vertices[i * 3 + 0], vertices[i * 3 + 1], vertices[i * 3 + 2]);

	for (size_t di = 0; di < disk_count; ++di)
	{
		const meshopt_MeshletDisk& disk = disks[di];
		const meshopt_Meshlet& m = disk.meshlet;

		fprintf(file, "g disk%zu\n", di);

		for (unsigned int t = 0; t < m.triangle_count; ++t)
		{
			fprintf(file, "f");

			for (int k = 0; k < 3; ++k)
			{
				unsigned char local = meshlet_triangles[m.triangle_offset + t * 3 + k];
				unsigned int global = meshlet_vertices[m.vertex_offset + local];

				fprintf(file, " %u", global + 1);
			}

			fprintf(file, "\n");
		}
	}

	fclose(file);

	for (size_t di = 0; di < disk_count; ++di)
	{
		const meshopt_MeshletDisk& disk = disks[di];
		const meshopt_Meshlet& m = disk.meshlet;

		// independent Euler characteristic: count distinct undirected edges of this cluster
		std::vector<std::pair<unsigned int, unsigned int> > edges;

		for (unsigned int t = 0; t < m.triangle_count; ++t)
		{
			for (int k = 0; k < 3; ++k)
			{
				unsigned int a = meshlet_triangles[m.triangle_offset + t * 3 + k];
				unsigned int b = meshlet_triangles[m.triangle_offset + t * 3 + (k + 1) % 3];

				if (a > b)
				{
					unsigned int tmp = a;
					a = b;
					b = tmp;
				}

				edges.push_back(std::make_pair(a, b));
			}
		}

		std::sort(edges.begin(), edges.end());
		edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

		int chi = int(m.vertex_count) - int(edges.size()) + int(m.triangle_count);

		printf("disk%zu vertices=%u triangles=%u euler=%d boundary_length=%u\n", di, m.vertex_count, m.triangle_count, chi,
		    disk.boundary_count);
	}

	printf("wrote %s with %zu disk meshlets covering %zu triangles\n", path, disk_count, face_count);
	return 0;
}
