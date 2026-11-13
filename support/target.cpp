// Regression tests for meshopt_buildMeshletsDisk.
// All topology verification below is rebuilt from the local micro index buffer of each cluster;
// it does not reuse any predicates from the production disk clusterizer.
#include "src/meshoptimizer.h"
#include "checks.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

typedef std::pair<unsigned int, unsigned int> UPair;

namespace
{

struct Mesh
{
	std::vector<float> positions;
	std::vector<unsigned int> indices;
	size_t vertex_count;
};

struct Built
{
	std::vector<meshopt_MeshletDisk> disks;
	std::vector<unsigned int> vertices;
	std::vector<unsigned char> triangles;
	std::vector<unsigned int> source;
	std::vector<unsigned char> boundary;
	size_t count;
};

UPair edgeKey(unsigned int a, unsigned int b)
{
	return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

Built buildDisk(const Mesh& mesh, size_t max_vertices, size_t max_triangles, float cone_weight = 0.f)
{
	size_t index_count = mesh.indices.size();
	size_t face_count = index_count / 3;

	Built result;
	result.disks.resize(face_count ? face_count : 1);
	result.vertices.resize(index_count ? index_count : 1);
	result.triangles.resize(index_count ? index_count : 1);
	result.source.resize(face_count ? face_count : 1);
	result.boundary.resize(index_count ? index_count : 1);

	result.count = meshopt_buildMeshletsDisk(result.disks.data(), result.vertices.data(), result.triangles.data(),
	    result.source.data(), result.boundary.data(), mesh.indices.data(), index_count, mesh.positions.data(),
	    mesh.vertex_count, sizeof(float) * 3, max_vertices, max_triangles, cone_weight);

	return result;
}

// Independent per-cluster verification: edge table, face connectivity (DSU), vertex links and boundary ring.
struct ClusterStats
{
	int chi;
	size_t boundary_length;
};

ClusterStats verifyClusterDisk(const Built& built, size_t di, const std::vector<unsigned int>& indices)
{
	const meshopt_MeshletDisk& disk = built.disks[di];
	const meshopt_Meshlet& m = disk.meshlet;

	char message[128];

	task_check(m.vertex_count >= 3 && m.triangle_count >= 1, "cluster is non-empty");
	task_check(m.vertex_count <= 256 && m.triangle_count <= 512, "cluster within implementation limits");

	std::vector<std::array<unsigned int, 3> > local_tris(m.triangle_count);

	// independently rebuilt directed and undirected edge tables
	std::map<UPair, int> undirected_count;
	std::map<UPair, int> directed_count;

	for (unsigned int t = 0; t < m.triangle_count; ++t)
	{
		for (int k = 0; k < 3; ++k)
		{
			unsigned char lv = built.triangles[m.triangle_offset + t * 3 + k];

			task_check(lv < m.vertex_count, "local vertex id within cluster range");
			local_tris[t][k] = lv;
		}

		task_check(local_tris[t][0] != local_tris[t][1] && local_tris[t][1] != local_tris[t][2] &&
		               local_tris[t][0] != local_tris[t][2],
		    "triangle has three distinct local vertices");

		for (int k = 0; k < 3; ++k)
		{
			unsigned int a = local_tris[t][k], b = local_tris[t][(k + 1) % 3];

			undirected_count[edgeKey(a, b)]++;
			directed_count[std::make_pair(a, b)]++;
		}
	}

	// every undirected edge has one or two incident faces; two faces must use opposite directions
	for (std::map<UPair, int>::const_iterator it = undirected_count.begin(); it != undirected_count.end(); ++it)
	{
		task_check(it->second == 1 || it->second == 2, "edge has one or two incident faces");

		if (it->second == 2)
		{
			UPair e = it->first;
			task_check(directed_count[e] == 1 && directed_count[std::make_pair(e.second, e.first)] == 1,
			    "interior edge has two oppositely oriented faces");
		}
	}

	// no directed edge is ever repeated (rejects duplicate faces / same-direction shared edges)
	for (std::map<UPair, int>::const_iterator it = directed_count.begin(); it != directed_count.end(); ++it)
		task_check(it->second == 1, "directed edge is unique");

	// vertex links: fan transition map (from, to) around each vertex, derived from face corners
	std::map<UPair, unsigned int> fan_next;

	for (unsigned int t = 0; t < m.triangle_count; ++t)
	{
		unsigned int a = local_tris[t][0], b = local_tris[t][1], c = local_tris[t][2];

		task_check(fan_next.insert(std::make_pair(std::make_pair(a, c), b)).second, "fan transition at a is unique");
		task_check(fan_next.insert(std::make_pair(std::make_pair(b, a), c)).second, "fan transition at b is unique");
		task_check(fan_next.insert(std::make_pair(std::make_pair(c, b), a)).second, "fan transition at c is unique");
	}

	std::set<unsigned int> cluster_vertices;

	for (unsigned int t = 0; t < m.triangle_count; ++t)
		for (int k = 0; k < 3; ++k)
			cluster_vertices.insert(local_tris[t][k]);

	task_check(cluster_vertices.size() == m.vertex_count, "all cluster vertices are referenced");

	for (std::set<unsigned int>::const_iterator vi = cluster_vertices.begin(); vi != cluster_vertices.end(); ++vi)
	{
		unsigned int v = *vi;
		std::set<unsigned int> neighbors;

		for (std::map<UPair, int>::const_iterator it = undirected_count.begin(); it != undirected_count.end(); ++it)
		{
			if (it->first.first == v)
				neighbors.insert(it->first.second);
			else if (it->first.second == v)
				neighbors.insert(it->first.first);
		}

		size_t degree = neighbors.size();
		size_t transitions = 0;
		size_t missing_in = 0, missing_out = 0;

		for (std::set<unsigned int>::const_iterator ni = neighbors.begin(); ni != neighbors.end(); ++ni)
		{
			if (fan_next.count(std::make_pair(v, *ni)) == 0)
				missing_out++;
			else
				transitions++;

			bool has_in = false;

			for (std::set<unsigned int>::const_iterator nj = neighbors.begin(); nj != neighbors.end(); ++nj)
				if (fan_next.count(std::make_pair(v, *nj)) && fan_next[std::make_pair(v, *nj)] == *ni)
					has_in = true;

			if (!has_in)
				missing_in++;
		}

		// interior vertex: one closed fan cycle; boundary vertex: one open fan path with two endpoints
		bool interior = (missing_in == 0 && missing_out == 0 && transitions == degree);
		bool boundary = (missing_in == 1 && missing_out == 1 && transitions + 1 == degree);

		task_check(interior || boundary, "vertex has a single triangle fan");

		if (boundary)
		{
			// walk the open path and require it to cover every neighbor
			unsigned int start = 0;

			for (std::set<unsigned int>::const_iterator ni = neighbors.begin(); ni != neighbors.end(); ++ni)
			{
				bool has_in = false;

				for (std::set<unsigned int>::const_iterator nj = neighbors.begin(); nj != neighbors.end(); ++nj)
					if (fan_next.count(std::make_pair(v, *nj)) && fan_next[std::make_pair(v, *nj)] == *ni)
						has_in = true;

				if (!has_in)
				{
					start = *ni;
					break;
				}
			}

			std::set<unsigned int> walked;
			unsigned int current = start;

			for (;;)
			{
				task_check(walked.insert(current).second, "fan path does not revisit a neighbor");

				if (fan_next.count(std::make_pair(v, current)) == 0)
					break;

				current = fan_next[std::make_pair(v, current)];
			}

			task_check(walked.size() == degree, "fan path covers the whole vertex link");
		}
	}

	// face connectivity through complete shared edges via a disjoint-set union
	std::vector<unsigned int> dsu(m.triangle_count);

	for (size_t i = 0; i < dsu.size(); ++i)
		dsu[i] = (unsigned int)i;

	auto find_root = [&dsu](unsigned int x) -> unsigned int {
		unsigned int root = x;

		while (dsu[root] != root)
			root = dsu[root];

		while (dsu[x] != x)
		{
			unsigned int next = dsu[x];
			dsu[x] = root;
			x = next;
		}

		return root;
	};

	std::map<UPair, std::vector<unsigned int> > edge_faces;

	for (unsigned int t = 0; t < m.triangle_count; ++t)
		for (int k = 0; k < 3; ++k)
			edge_faces[edgeKey(local_tris[t][k], local_tris[t][(k + 1) % 3])].push_back(t);

	for (std::map<UPair, std::vector<unsigned int> >::const_iterator it = edge_faces.begin(); it != edge_faces.end(); ++it)
	{
		if (it->second.size() == 2)
		{
			unsigned int ra = find_root(it->second[0]), rb = find_root(it->second[1]);

			if (ra != rb)
				dsu[rb] = ra;
		}
	}

	unsigned int root = find_root(0);

	for (unsigned int t = 1; t < m.triangle_count; ++t)
		task_check(find_root(t) == root, "faces are edge-connected into one component");

	// independently walk the directed boundary: edges with no reverse face
	std::map<unsigned int, unsigned int> boundary_next;
	std::set<UPair> boundary_edges;

	for (std::map<UPair, int>::const_iterator it = directed_count.begin(); it != directed_count.end(); ++it)
	{
		UPair reverse = std::make_pair(it->first.second, it->first.first);

		if (directed_count.count(reverse) == 0)
		{
			task_check(boundary_next.insert(std::make_pair(it->first.first, it->first.second)).second,
			    "one outgoing boundary edge per boundary vertex");
			boundary_edges.insert(it->first);
		}
	}

	task_check(boundary_edges.size() >= 3, "disk has a non-empty boundary");

	std::vector<unsigned int> ring;
	unsigned int start = boundary_edges.begin()->first;
	unsigned int current = start;

	for (;;)
	{
		task_check(boundary_next.count(current) == 1, "boundary ring continues at every boundary vertex");
		ring.push_back(current);
		current = boundary_next[current];

		if (current == start)
			break;

		task_check(ring.size() <= boundary_edges.size(), "boundary ring terminates");
	}

	task_check(ring.size() == boundary_edges.size(), "boundary is a single simple ring");

	// compare against the reported boundary ring, allowing cyclic rotation but keeping orientation
	task_check(disk.boundary_count == ring.size(), "reported boundary length matches independent walk");

	bool rotation_match = false;

	for (size_t shift = 0; shift < ring.size() && !rotation_match; ++shift)
	{
		bool match = true;

		for (size_t i = 0; i < ring.size(); ++i)
		{
			unsigned char reported = built.boundary[disk.boundary_offset + i];

			task_check(reported < m.vertex_count, "boundary entry is a valid local vertex id");

			if (reported != ring[(shift + i) % ring.size()])
				match = false;
		}

		rotation_match = match;
	}

	task_check(rotation_match, "reported boundary ring follows the directed face edges");

	// Euler characteristic computed from the independent edge table
	int chi = int(m.vertex_count) - int(undirected_count.size()) + int(m.triangle_count);

	task_check(chi == 1, "Euler characteristic is 1");

	// triangle_source must restore the exact input triangle with original winding
	for (unsigned int t = 0; t < m.triangle_count; ++t)
	{
		unsigned int source = built.source[m.triangle_offset / 3 + t];

		task_check(source < indices.size() / 3, "triangle source id is in range");

		for (int k = 0; k < 3; ++k)
		{
			unsigned char lv = built.triangles[m.triangle_offset + t * 3 + k];
			unsigned int global = built.vertices[m.vertex_offset + lv];

			snprintf(message, sizeof(message), "corner %d restores the original input index", k);
			task_check(global == indices[source * 3 + k], message);
		}
	}

	ClusterStats stats;
	stats.chi = chi;
	stats.boundary_length = ring.size();
	return stats;
}

void verifyCoverage(const Built& built, const Mesh& mesh)
{
	size_t face_count = mesh.indices.size() / 3;

	std::vector<unsigned char> covered(face_count, 0);

	for (size_t di = 0; di < built.count; ++di)
	{
		const meshopt_Meshlet& m = built.disks[di].meshlet;

		for (unsigned int t = 0; t < m.triangle_count; ++t)
		{
			unsigned int source = built.source[m.triangle_offset / 3 + t];

			task_check(source < face_count, "source id in range");
			task_check(covered[source] == 0, "input triangle covered exactly once");
			covered[source] = 1;
		}
	}

	for (size_t f = 0; f < face_count; ++f)
		task_check(covered[f] == 1, "every input triangle is preserved once");
}

void verifyPacking(const Built& built, const Mesh& mesh, size_t max_vertices, size_t max_triangles)
{
	size_t index_count = mesh.indices.size();
	size_t used_vertices = 0, used_triangles = 0, used_boundary = 0;

	for (size_t di = 0; di < built.count; ++di)
	{
		const meshopt_MeshletDisk& disk = built.disks[di];
		const meshopt_Meshlet& m = disk.meshlet;

		task_check(m.vertex_count > 0 && m.triangle_count > 0, "disk is non-empty");
		task_check(m.vertex_count <= max_vertices, "vertex capacity respected");
		task_check(m.triangle_count <= max_triangles, "triangle capacity respected");
		task_check(m.triangle_offset == used_triangles * 3, "triangle offsets are contiguous");
		task_check(m.vertex_offset == used_vertices, "vertex offsets are contiguous");
		task_check(disk.boundary_offset == used_boundary, "boundary offsets are contiguous");
		task_check(disk.boundary_count >= 3, "boundary ring has at least three vertices");

		used_vertices += m.vertex_count;
		used_triangles += m.triangle_count;
		used_boundary += disk.boundary_count;
	}

	task_check(used_triangles == index_count / 3, "all output triangle slots used");
	task_check(used_vertices <= index_count, "vertex output within index_count budget");
	task_check(used_boundary <= index_count, "boundary output within index_count budget");
	task_check(built.count <= index_count / 3, "disk count does not exceed face count");
}

// ---- mesh generators -------------------------------------------------------

Mesh makeGrid(int nx, int ny, bool fold = false)
{
	Mesh mesh;
	mesh.vertex_count = size_t((nx + 1) * (ny + 1));
	mesh.positions.resize(mesh.vertex_count * 3);

	auto id = [nx](int x, int y) -> unsigned int { return unsigned(y * (nx + 1) + x); };

	for (int y = 0; y <= ny; ++y)
		for (int x = 0; x <= nx; ++x)
		{
			float* p = &mesh.positions[id(x, y) * 3];
			p[0] = float(x);
			p[1] = float(y);
			p[2] = fold ? float((x & 1) ? 0.5 : 0.0) : 0.f;
		}

	for (int y = 0; y < ny; ++y)
		for (int x = 0; x < nx; ++x)
		{
			unsigned int a = id(x, y), b = id(x + 1, y), c = id(x, y + 1), d = id(x + 1, y + 1);

			mesh.indices.push_back(a);
			mesh.indices.push_back(b);
			mesh.indices.push_back(c);
			mesh.indices.push_back(b);
			mesh.indices.push_back(d);
			mesh.indices.push_back(c);
		}

	return mesh;
}

Mesh shuffledGrid(int nx, int ny)
{
	Mesh mesh = makeGrid(nx, ny);
	size_t face_count = mesh.indices.size() / 3;

	std::vector<size_t> order(face_count);

	for (size_t i = 0; i < face_count; ++i)
		order[i] = i;

	// deterministic LCG permutation of face order plus per-face corner rotation
	for (size_t i = face_count; i > 1; --i)
	{
		size_t j = (i * 1103515245u + 12345u) % i;
		size_t tmp = order[i - 1];
		order[i - 1] = order[j];
		order[j] = tmp;
	}

	std::vector<unsigned int> shuffled;

	for (size_t i = 0; i < face_count; ++i)
	{
		size_t f = order[i];
		unsigned int tri[3] = {mesh.indices[f * 3], mesh.indices[f * 3 + 1], mesh.indices[f * 3 + 2]};
		int rot = int(i % 3);

		for (int k = 0; k < 3; ++k)
			shuffled.push_back(tri[(k + rot) % 3]);
	}

	mesh.indices.swap(shuffled);
	return mesh;
}

Mesh makeTetrahedron()
{
	Mesh mesh;
	mesh.vertex_count = 4;
	mesh.positions = {1, 1, 1, -1, -1, 1, -1, 1, -1, 1, -1, -1};
	mesh.indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
	return mesh;
}

Mesh makeOctahedron()
{
	Mesh mesh;
	mesh.vertex_count = 6;
	mesh.positions = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1};
	mesh.indices = {4, 0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 5, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3};
	return mesh;
}

Mesh makeAnnulus(int ntheta, int nrings)
{
	Mesh mesh;
	mesh.vertex_count = size_t((nrings + 1) * ntheta);
	mesh.positions.resize(mesh.vertex_count * 3);

	auto id = [ntheta](int r, int t) -> unsigned int { return unsigned(r * ntheta + (t % ntheta)); };

	for (int r = 0; r <= nrings; ++r)
	{
		float radius = 0.4f + 0.6f * float(r) / float(nrings);

		for (int t = 0; t < ntheta; ++t)
		{
			float angle = 2.f * float(M_PI) * float(t) / float(ntheta);
			float* p = &mesh.positions[id(r, t) * 3];
			p[0] = radius * cosf(angle);
			p[1] = radius * sinf(angle);
			p[2] = 0.f;
		}
	}

	for (int r = 0; r < nrings; ++r)
		for (int t = 0; t < ntheta; ++t)
		{
			unsigned int a = id(r, t), b = id(r, t + 1);
			unsigned int c = id(r + 1, t), d = id(r + 1, t + 1);

			mesh.indices.push_back(a);
			mesh.indices.push_back(c);
			mesh.indices.push_back(b);
			mesh.indices.push_back(b);
			mesh.indices.push_back(c);
			mesh.indices.push_back(d);
		}

	return mesh;
}

// two 2x2 grids that touch at exactly one shared input vertex index
Mesh makePointContact()
{
	Mesh a = makeGrid(2, 2);
	Mesh mesh;
	const unsigned int shared = 0;
	const unsigned int offset = 9; // patch A uses indices 0..8

	mesh.vertex_count = offset + 8; // shared vertex plus 8 private vertices of patch B
	mesh.positions.resize(mesh.vertex_count * 3);

	for (size_t i = 0; i < a.positions.size(); ++i)
		mesh.positions[i] = a.positions[i];

	// patch B reuses index 0 and shifts everything else far away in x
	for (int y = 0; y <= 2; ++y)
		for (int x = 0; x <= 2; ++x)
		{
			unsigned int local = unsigned(y * 3 + x);

			if (local == shared)
				continue; // shared vertex keeps its original position

			unsigned int remapped = offset + (local - 1);
			mesh.positions[remapped * 3 + 0] = a.positions[local * 3 + 0] + 100.f;
			mesh.positions[remapped * 3 + 1] = a.positions[local * 3 + 1];
			mesh.positions[remapped * 3 + 2] = a.positions[local * 3 + 2];
		}

	mesh.indices = a.indices;

	for (size_t i = 0; i < a.indices.size(); ++i)
	{
		unsigned int v = a.indices[i];
		mesh.indices.push_back(v == shared ? shared : offset + (v - 1));
	}

	return mesh;
}

// two disconnected components with identical positions but distinct vertex indices
Mesh makeDuplicatePositionComponents()
{
	Mesh one = makeGrid(1, 1);
	Mesh mesh;
	const unsigned int offset = 4;

	mesh.vertex_count = 8;
	mesh.positions.resize(mesh.vertex_count * 3);

	for (size_t i = 0; i < one.positions.size(); ++i)
		mesh.positions[i] = one.positions[i];

	for (size_t i = 0; i < one.positions.size(); ++i)
		mesh.positions[offset * 3 + i] = one.positions[i];

	mesh.indices = one.indices;

	for (size_t i = 0; i < one.indices.size(); ++i)
		mesh.indices.push_back(one.indices[i] + offset);

	return mesh;
}

void checkAllClusters(const Built& built, const Mesh& mesh)
{
	for (size_t di = 0; di < built.count; ++di)
		verifyClusterDisk(built, di, mesh.indices);

	verifyCoverage(built, mesh);
}

} // namespace

void task_cases(const char* group)
{
	std::string name = group;

	if (name == "disk")
	{
		// a regular 3x5 grid must fit in a single 64/64 disk with cone_weight 0
		Mesh grid = makeGrid(3, 5);
		Built bgrid = buildDisk(grid, 64, 64, 0.f);
		task_check(bgrid.count == 1, "3x5 grid is one disk");
		checkAllClusters(bgrid, grid);
		task_check(bgrid.disks[0].meshlet.vertex_count == 24 && bgrid.disks[0].meshlet.triangle_count == 30,
		    "grid disk keeps every grid vertex and face");

		// shuffled face order plus corner rotation must still produce one disk
		Mesh shuffled = shuffledGrid(3, 5);
		Built bshuffled = buildDisk(shuffled, 64, 64, 0.f);
		task_check(bshuffled.count == 1, "shuffled rotated grid is one disk");
		checkAllClusters(bshuffled, shuffled);

		// same connectivity folded into a non-self-intersecting strip
		Mesh folded = makeGrid(3, 5, true);
		Built bfolded = buildDisk(folded, 64, 64, 0.f);
		task_check(bfolded.count == 1, "folded strip is one disk");
		checkAllClusters(bfolded, folded);

		// closed tetrahedral shell can never be a single disk
		Mesh tetra = makeTetrahedron();
		Built btetra = buildDisk(tetra, 64, 64, 0.f);
		task_check(btetra.count >= 2, "tetrahedron is split into multiple disks");
		checkAllClusters(btetra, tetra);

		for (size_t di = 0; di < btetra.count; ++di)
			task_check(btetra.disks[di].meshlet.triangle_count < 4, "no disk contains the whole closed shell");

		// point-contacting patches stay in separate disks
		Mesh contact = makePointContact();
		Built bcontact = buildDisk(contact, 64, 64, 0.f);
		task_check(bcontact.count == 2, "point contacts produce two disks");
		checkAllClusters(bcontact, contact);

		for (size_t di = 0; di < bcontact.count; ++di)
		{
			const meshopt_Meshlet& m = bcontact.disks[di].meshlet;
			bool patch_a = true, patch_b = true;

			for (unsigned int t = 0; t < m.triangle_count; ++t)
			{
				unsigned int source = bcontact.source[m.triangle_offset / 3 + t];
				// each 2x2 grid patch contributes 8 input faces, A first then B
				patch_a = patch_a && source < 8;
				patch_b = patch_b && source >= 8;
			}

			task_check(patch_a || patch_b, "point-contact patches are never merged");
		}
	}
	else if (name == "boundary")
	{
		// rectangular grid: boundary has 2*(nx+ny) distinct vertices
		Mesh grid = makeGrid(4, 3);
		Built bgrid = buildDisk(grid, 64, 64, 0.f);
		task_check(bgrid.count == 1, "4x3 grid is one disk");
		ClusterStats stats = verifyClusterDisk(bgrid, 0, grid.indices);
		task_check(stats.boundary_length == size_t(2 * (4 + 3)), "grid boundary length is 2*(nx+ny)");
		task_check(stats.chi == 1, "grid disk Euler characteristic is 1");

		// single triangle: boundary is exactly its three vertices
		Mesh tri;
		tri.vertex_count = 3;
		tri.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
		tri.indices = {0, 1, 2};
		Built btri = buildDisk(tri, 64, 64, 0.f);
		task_check(btri.count == 1 && btri.disks[0].boundary_count == 3, "single triangle boundary length is 3");
		verifyClusterDisk(btri, 0, tri.indices);

		// tetrahedron fragments: every disk keeps a valid simple boundary ring
		Mesh tetra = makeTetrahedron();
		Built btetra = buildDisk(tetra, 64, 64, 0.f);

		for (size_t di = 0; di < btetra.count; ++di)
		{
			ClusterStats s = verifyClusterDisk(btetra, di, tetra.indices);
			task_check(s.boundary_length >= 3, "tetra fragment has a closed boundary walk");
		}

		// annulus cannot be one disk; summed boundaries stay consistent with the independent walk
		Mesh annulus = makeAnnulus(8, 2);
		Built bannulus = buildDisk(annulus, 64, 64, 0.f);
		task_check(bannulus.count >= 2, "annulus with center hole is split");
		checkAllClusters(bannulus, annulus);

		// point contact: each patch boundary is verified independently through the ring walk
		Mesh contact = makePointContact();
		Built bcontact = buildDisk(contact, 64, 64, 0.f);

		for (size_t di = 0; di < bcontact.count; ++di)
		{
			ClusterStats s = verifyClusterDisk(bcontact, di, contact.indices);
			task_check(s.chi == 1 && s.boundary_length >= 4, "contact patch remains a disk");
		}

		// cone weighting does not break boundary orientation
		Built bweighted = buildDisk(grid, 64, 64, 0.5f);
		task_check(bweighted.count >= 1, "cone-weighted build produces disks");
		checkAllClusters(bweighted, grid);
	}
	else if (name == "coverage")
	{
		// closed sphere topology: every octahedron face survives with its winding intact
		Mesh sphere = makeOctahedron();
		Built bsphere = buildDisk(sphere, 64, 64, 0.f);
		task_check(bsphere.count >= 2, "sphere shell is split into disks");
		checkAllClusters(bsphere, sphere);
		verifyCoverage(bsphere, sphere);

		// annulus coverage with source restoration checked corner by corner
		Mesh annulus = makeAnnulus(10, 2);
		Built bannulus = buildDisk(annulus, 64, 64, 0.f);
		checkAllClusters(bannulus, annulus);
		task_check(bannulus.count >= 2, "annulus needs more than one disk");

		// point contact coverage
		Mesh contact = makePointContact();
		Built bcontact = buildDisk(contact, 64, 64, 0.f);
		verifyCoverage(bcontact, contact);

		// coincident positions with distinct indices remain distinct vertices
		Mesh coincident = makeDuplicatePositionComponents();
		Built bcoincident = buildDisk(coincident, 64, 64, 0.f);
		task_check(bcoincident.count == 2, "coincident-position components stay separate");
		checkAllClusters(bcoincident, coincident);

		// empty input produces no disks
		Mesh empty;
		empty.vertex_count = 0;
		Built bempty = buildDisk(empty, 64, 64, 0.f);
		task_check(bempty.count == 0, "empty input returns zero disks");

		// shuffled grid coverage restores every original face id
		Mesh shuffled = shuffledGrid(4, 4);
		Built bshuffled = buildDisk(shuffled, 64, 64, 0.f);
		verifyCoverage(bshuffled, shuffled);
	}
	else if (name == "packing")
	{
		// capacity forces multiple disks; each one stays within 64 vertices / 64 triangles
		Mesh big = makeGrid(8, 8);
		Built bbig = buildDisk(big, 64, 64, 0.f);
		task_check(bbig.count >= 2, "capacity limit splits the large grid");
		verifyPacking(bbig, big, 64, 64);
		checkAllClusters(bbig, big);

		// tight limits exercise both vertex and triangle bounds
		Mesh small = makeGrid(4, 4);
		Built bsmall = buildDisk(small, 16, 16, 0.f);
		task_check(bsmall.count >= 2, "tight limits split the grid");
		verifyPacking(bsmall, small, 16, 16);
		checkAllClusters(bsmall, small);

		// the 16-bit index wrapper yields the same coverage as the 32-bit entry point
		{
			Mesh mesh = makeGrid(3, 4);
			size_t index_count = mesh.indices.size();
			size_t face_count = index_count / 3;

			std::vector<unsigned short> indices16(index_count);

			for (size_t i = 0; i < index_count; ++i)
				indices16[i] = (unsigned short)mesh.indices[i];

			std::vector<meshopt_MeshletDisk> disks(face_count);
			std::vector<unsigned int> out_vertices(index_count);
			std::vector<unsigned char> out_triangles(index_count);
			std::vector<unsigned int> out_source(face_count);
			std::vector<unsigned char> out_boundary(index_count);

			size_t count16 = meshopt_buildMeshletsDisk(disks.data(), out_vertices.data(), out_triangles.data(),
			    out_source.data(), out_boundary.data(), indices16.data(), index_count, mesh.positions.data(),
			    mesh.vertex_count, sizeof(float) * 3, 64, 64, 0.f);

			Built b32 = buildDisk(mesh, 64, 64, 0.f);
			task_check(count16 == b32.count, "16-bit wrapper matches 32-bit disk count");

			Built b16;
			b16.disks = disks;
			b16.vertices = out_vertices;
			b16.triangles = out_triangles;
			b16.source = out_source;
			b16.boundary = out_boundary;
			b16.count = count16;
			verifyCoverage(b16, mesh);
		}

		// tetrahedron packing keeps every fragment valid and full
		Mesh tetra = makeTetrahedron();
		Built btetra = buildDisk(tetra, 64, 64, 0.f);
		verifyPacking(btetra, tetra, 64, 64);

		// maximum documented limits are accepted on a disk-shaped input
		Mesh grid = makeGrid(2, 2);
		Built bmax = buildDisk(grid, 256, 512, 0.f);
		task_check(bmax.count == 1, "small grid is one disk at maximum limits");
		verifyPacking(bmax, grid, 256, 512);
	}
	else
	{
		task_check(0, "unknown test group");
	}
}
