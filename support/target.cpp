// Regression tests for meshopt_buildMeshletsDisk.
// The checks below verify the output independently: edge tables, face connectivity and vertex
// links are rebuilt from the emitted local triangles, and the boundary loop is validated against
// the directed single-face edges of each cluster. Source triangles are reconstructed corner by
// corner through triangle_source and compared with the original input.
#include "src/meshoptimizer.h"
#include "checks.h"

#include <math.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct Mesh
{
	std::vector<float> positions;
	std::vector<unsigned int> indices;
};

struct DiskResult
{
	std::vector<meshopt_MeshletDisk> disks;
	std::vector<unsigned int> vertices;
	std::vector<unsigned char> triangles;
	std::vector<unsigned int> source;
	std::vector<unsigned char> boundary;
	size_t count;
};

Mesh makeGrid(int cx, int cy, int fold)
{
	Mesh m;

	for (int y = 0; y <= cy; ++y)
		for (int x = 0; x <= cx; ++x)
		{
			float px = float(x), py = float(y), pz = 0.f;

			if (fold)
			{
				// bend the sheet along x into a non-self-intersecting curved strip
				float angle = px * 0.35f;
				px = sinf(angle) * 8.f;
				pz = (1.f - cosf(angle)) * 8.f;
			}

			m.positions.push_back(px);
			m.positions.push_back(py);
			m.positions.push_back(pz);
		}

	for (int y = 0; y < cy; ++y)
		for (int x = 0; x < cx; ++x)
		{
			unsigned int v00 = unsigned(y * (cx + 1) + x), v10 = v00 + 1, v01 = v00 + unsigned(cx + 1), v11 = v01 + 1;

			m.indices.push_back(v00);
			m.indices.push_back(v10);
			m.indices.push_back(v11);
			m.indices.push_back(v00);
			m.indices.push_back(v11);
			m.indices.push_back(v01);
		}

	return m;
}

Mesh makeTetra()
{
	Mesh m;
	const float p[] = {1, 1, 1, -1, -1, 1, -1, 1, -1, 1, -1, -1};
	const unsigned int i[] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
	m.positions.assign(p, p + 12);
	m.indices.assign(i, i + 12);
	return m;
}

Mesh makeOcta()
{
	Mesh m;
	const float p[] = {1, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1};
	const unsigned int i[] = {4, 0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 5, 1, 0, 5, 2, 1, 5, 3, 2, 5, 0, 3};
	m.positions.assign(p, p + 18);
	m.indices.assign(i, i + 24);
	return m;
}

Mesh makeAnnulus(int sectors, int rings)
{
	Mesh m;

	for (int r = 0; r <= rings; ++r)
		for (int s = 0; s < sectors; ++s)
		{
			float angle = float(s) * (2.f * 3.14159265f) / float(sectors);
			float radius = 1.f + float(r);

			m.positions.push_back(cosf(angle) * radius);
			m.positions.push_back(sinf(angle) * radius);
			m.positions.push_back(0.f);
		}

	for (int r = 0; r < rings; ++r)
		for (int s = 0; s < sectors; ++s)
		{
			unsigned int v00 = unsigned(r * sectors + s), v10 = unsigned(r * sectors + (s + 1) % sectors);
			unsigned int v01 = v00 + unsigned(sectors), v11 = v10 + unsigned(sectors);

			m.indices.push_back(v00);
			m.indices.push_back(v10);
			m.indices.push_back(v11);
			m.indices.push_back(v00);
			m.indices.push_back(v11);
			m.indices.push_back(v01);
		}

	return m;
}

Mesh makePointContact()
{
	// two closed fans that only touch at the shared center vertex 0
	Mesh m;
	m.positions.insert(m.positions.end(), {0, 0, 0});

	for (int fan = 0; fan < 2; ++fan)
		for (int k = 0; k < 5; ++k)
		{
			float angle = float(k) * (2.f * 3.14159265f) / 5.f;
			m.positions.push_back(cosf(angle) + float(fan) * 3.f);
			m.positions.push_back(sinf(angle));
			m.positions.push_back(float(fan));
		}

	for (int fan = 0; fan < 2; ++fan)
		for (int k = 0; k < 5; ++k)
		{
			unsigned int a = unsigned(1 + fan * 5 + k);
			unsigned int b = unsigned(1 + fan * 5 + (k + 1) % 5);

			m.indices.push_back(0);
			m.indices.push_back(a);
			m.indices.push_back(b);
		}

	return m;
}

Mesh makeDupGrid()
{
	// two copies of the same grid at identical positions, using distinct vertex indices
	Mesh m = makeGrid(2, 2, 0);
	size_t v = m.positions.size() / 3;
	size_t f = m.indices.size();

	m.positions.insert(m.positions.end(), m.positions.begin(), m.positions.end());

	for (size_t i = 0; i < f; ++i)
		m.indices.push_back(m.indices[i] + unsigned(v));

	return m;
}

Mesh shuffleFaces(Mesh m, unsigned int seed)
{
	size_t f = m.indices.size() / 3;

	for (size_t i = f - 1; i > 0; --i)
	{
		seed = seed * 1664525u + 1013904223u;
		size_t j = (seed >> 8) % (i + 1);

		for (int k = 0; k < 3; ++k)
			std::swap(m.indices[i * 3 + k], m.indices[j * 3 + k]);
	}

	return m;
}

Mesh rotateCorners(Mesh m)
{
	for (size_t f = 0; f < m.indices.size() / 3; ++f)
	{
		unsigned int t[3] = {m.indices[f * 3], m.indices[f * 3 + 1], m.indices[f * 3 + 2]};
		size_t r = f % 3;

		m.indices[f * 3 + 0] = t[r];
		m.indices[f * 3 + 1] = t[(r + 1) % 3];
		m.indices[f * 3 + 2] = t[(r + 2) % 3];
	}

	return m;
}

DiskResult buildDisk(const Mesh& m, size_t max_vertices, size_t max_triangles, float cone_weight)
{
	DiskResult r;
	size_t f = m.indices.size() / 3;

	r.disks.resize(f ? f : 1);
	r.vertices.resize(m.indices.size());
	r.triangles.resize(m.indices.size());
	r.source.resize(f ? f : 1);
	r.boundary.resize(m.indices.size());

	r.count = meshopt_buildMeshletsDisk(r.disks.data(), r.vertices.data(), r.triangles.data(), r.source.data(), r.boundary.data(),
	    m.indices.empty() ? 0 : m.indices.data(), m.indices.size(), m.positions.empty() ? 0 : m.positions.data(), m.positions.size() / 3, sizeof(float) * 3, max_vertices, max_triangles, cone_weight);

	return r;
}

typedef std::pair<unsigned int, unsigned int> Edge;

Edge edgeKey(unsigned int a, unsigned int b)
{
	return a < b ? Edge(a, b) : Edge(b, a);
}

// Rebuilds edge tables, face connectivity and vertex links from the local triangles of one
// cluster and verifies all disk invariants, plus the reported boundary loop.
void verifyDisk(const DiskResult& r, size_t i, const char* tag)
{
	const meshopt_MeshletDisk& d = r.disks[i];
	const meshopt_Meshlet& m = d.meshlet;

	task_check(m.triangle_count > 0 && m.vertex_count >= 3, tag);

	// gather cluster faces in global vertex indices, validating local indices and vertex uniqueness
	std::vector<unsigned int> faces(m.triangle_count * 3);
	std::map<unsigned int, int> local_used;

	bool locals_ok = true;
	for (unsigned int t = 0; t < m.triangle_count; ++t)
		for (int j = 0; j < 3; ++j)
		{
			unsigned int local = r.triangles[m.triangle_offset + t * 3 + j];
			if (local >= m.vertex_count)
				locals_ok = false;

			local_used[local]++;
			faces[t * 3 + j] = r.vertices[m.vertex_offset + local];
		}

	task_check(locals_ok && local_used.size() == m.vertex_count, tag);

	bool unique_ok = true;
	{
		std::set<unsigned int> global;
		for (unsigned int v = 0; v < m.vertex_count; ++v)
			global.insert(r.vertices[m.vertex_offset + v]);
		unique_ok = global.size() == m.vertex_count;
	}
	task_check(unique_ok, tag);

	// edge table: directed and undirected face counts per edge
	std::map<Edge, int> directed, undirected;
	std::map<Edge, std::vector<unsigned int> > edge_faces;

	for (unsigned int t = 0; t < m.triangle_count; ++t)
		for (int j = 0; j < 3; ++j)
		{
			unsigned int a = faces[t * 3 + j], b = faces[t * 3 + (j + 1) % 3];
			directed[Edge(a, b)]++;
			undirected[edgeKey(a, b)]++;
			edge_faces[edgeKey(a, b)].push_back(t);
		}

	// interior edges must be shared by two faces with opposite directions; boundary edges have one face
	unsigned int boundary_edges = 0;
	bool degree_ok = true, orientation_ok = true;

	for (std::map<Edge, int>::iterator it = undirected.begin(); it != undirected.end(); ++it)
	{
		if (it->second == 1)
			boundary_edges++;
		else if (it->second == 2)
		{
			if (directed[Edge(it->first.first, it->first.second)] != 1 || directed[Edge(it->first.second, it->first.first)] != 1)
				orientation_ok = false;
		}
		else
			degree_ok = false;
	}

	task_check(degree_ok, tag);
	task_check(orientation_ok, tag);

	// Euler characteristic of a disk
	int chi = int(m.vertex_count) - int(undirected.size()) + int(m.triangle_count);
	task_check(chi == 1, tag);

	// faces must be connected through shared edges
	{
		std::vector<int> visit(m.triangle_count, 0);
		std::vector<unsigned int> stack(1, 0);
		visit[0] = 1;
		size_t reached = 1;

		while (!stack.empty())
		{
			unsigned int t = stack.back();
			stack.pop_back();

			for (int j = 0; j < 3; ++j)
			{
				Edge e = edgeKey(faces[t * 3 + j], faces[t * 3 + (j + 1) % 3]);
				const std::vector<unsigned int>& adjacent = edge_faces[e];

				for (size_t k = 0; k < adjacent.size(); ++k)
					if (!visit[adjacent[k]])
					{
						visit[adjacent[k]] = 1;
						stack.push_back(adjacent[k]);
						reached++;
					}
			}
		}

		task_check(reached == m.triangle_count, tag);
	}

	// each vertex must have a single fan: the link is one path (boundary vertex) or one cycle (interior vertex)
	{
		std::map<unsigned int, std::vector<Edge> > links;

		for (unsigned int t = 0; t < m.triangle_count; ++t)
		{
			unsigned int a = faces[t * 3 + 0], b = faces[t * 3 + 1], c = faces[t * 3 + 2];
			links[a].push_back(Edge(b, c));
			links[b].push_back(Edge(c, a));
			links[c].push_back(Edge(a, b));
		}

		bool fans_ok = true;

		for (unsigned int v = 0; v < m.vertex_count; ++v)
		{
			unsigned int global = r.vertices[m.vertex_offset + v];
			const std::vector<Edge>& le = links[global];

			std::map<unsigned int, std::vector<unsigned int> > adjacency;
			for (size_t k = 0; k < le.size(); ++k)
			{
				adjacency[le[k].first].push_back(le[k].second);
				adjacency[le[k].second].push_back(le[k].first);
			}

			// link degrees must be 1 or 2, and the link must be connected
			size_t endpoints = 0;
			bool link_ok = !adjacency.empty();

			for (std::map<unsigned int, std::vector<unsigned int> >::iterator it = adjacency.begin(); it != adjacency.end(); ++it)
			{
				if (it->second.size() > 2)
					link_ok = false;
				if (it->second.size() == 1)
					endpoints++;
			}

			size_t reached = 0;
			if (link_ok)
			{
				std::set<unsigned int> seen;
				std::vector<unsigned int> stack(1, adjacency.begin()->first);
				seen.insert(stack[0]);

				while (!stack.empty())
				{
					unsigned int u = stack.back();
					stack.pop_back();
					reached++;

					for (size_t k = 0; k < adjacency[u].size(); ++k)
						if (seen.insert(adjacency[u][k]).second)
							stack.push_back(adjacency[u][k]);
				}
			}

			// a vertex is on the boundary if and only if its link is a path
			bool on_boundary = false;
			for (size_t k = 0; k < le.size() && !on_boundary; ++k)
				if (undirected[edgeKey(global, le[k].first)] == 1 || undirected[edgeKey(global, le[k].second)] == 1)
					on_boundary = true;

			if (!link_ok || reached != adjacency.size() || (endpoints != 0 && endpoints != 2) || on_boundary != (endpoints == 2))
				fans_ok = false;
		}

		task_check(fans_ok, tag);
	}

	// boundary edges must chain into a single directed loop (rebuilt independently of the output)
	{
		std::map<unsigned int, unsigned int> next, prev;
		bool chain_ok = true;

		for (std::map<Edge, int>::iterator it = directed.begin(); it != directed.end(); ++it)
		{
			if (undirected[edgeKey(it->first.first, it->first.second)] != 1)
				continue;

			if (next.count(it->first.first) || prev.count(it->first.second))
				chain_ok = false;

			next[it->first.first] = it->first.second;
			prev[it->first.second] = it->first.first;
		}

		size_t walked = 0;
		if (chain_ok && !next.empty())
		{
			unsigned int start = next.begin()->first, v = start;
			do
			{
				std::map<unsigned int, unsigned int>::iterator it = next.find(v);
				if (it == next.end())
					break;
				v = it->second;
				walked++;
			} while (v != start && walked <= boundary_edges);

			if (v != start)
				chain_ok = false;
		}

		task_check(chain_ok && walked == boundary_edges, tag);
	}

	// the reported boundary loop must match the directed boundary edges exactly
	{
		bool loop_ok = d.boundary_count == boundary_edges && d.boundary_count >= 3;
		std::set<unsigned int> seen;

		for (unsigned int k = 0; k < d.boundary_count; ++k)
		{
			unsigned int la = r.boundary[d.boundary_offset + k];
			unsigned int lb = r.boundary[d.boundary_offset + (k + 1) % d.boundary_count];

			if (la >= m.vertex_count || lb >= m.vertex_count)
				loop_ok = false;
			else
			{
				unsigned int a = r.vertices[m.vertex_offset + la], b = r.vertices[m.vertex_offset + lb];

				// each loop segment must be a directed single-face edge of the cluster
				std::map<Edge, int>::iterator de = directed.find(Edge(a, b));
				if (de == directed.end() || de->second != 1 || undirected[edgeKey(a, b)] != 1)
					loop_ok = false;
			}

			// the loop must be simple: no repeated vertices
			if (!seen.insert(la).second)
				loop_ok = false;
		}

		task_check(loop_ok, tag);
	}
}

// Verifies that all input triangles are preserved exactly once through triangle_source,
// reconstructing each output triangle corner by corner from the original input.
void verifyCoverage(const Mesh& mesh, const DiskResult& r, const char* tag)
{
	size_t f = mesh.indices.size() / 3;

	size_t total = 0;
	for (size_t i = 0; i < r.count; ++i)
		total += r.disks[i].meshlet.triangle_count;
	task_check(total == f, tag);

	std::vector<int> seen(f, 0);
	bool permutation_ok = true, corners_ok = true;

	for (size_t i = 0; i < r.count; ++i)
	{
		const meshopt_Meshlet& m = r.disks[i].meshlet;

		for (unsigned int t = 0; t < m.triangle_count; ++t)
		{
			unsigned int s = r.source[m.triangle_offset / 3 + t];

			if (s >= f || seen[s]++)
				permutation_ok = false;

			for (int j = 0; j < 3; ++j)
			{
				unsigned int local = r.triangles[m.triangle_offset + t * 3 + j];
				unsigned int global = r.vertices[m.vertex_offset + local];

				if (s < f && global != mesh.indices[s * 3 + j])
					corners_ok = false;
			}
		}
	}

	task_check(permutation_ok, tag);
	task_check(corners_ok, tag);

	// offsets must chain consecutively across clusters
	bool offsets_ok = true;
	unsigned int vo = 0, to = 0, bo = 0;

	for (size_t i = 0; i < r.count; ++i)
	{
		const meshopt_MeshletDisk& d = r.disks[i];

		if (d.meshlet.vertex_offset != vo || d.meshlet.triangle_offset != to || d.boundary_offset != bo)
			offsets_ok = false;

		vo += d.meshlet.vertex_count;
		to += d.meshlet.triangle_count * 3;
		bo += d.boundary_count;
	}

	task_check(offsets_ok, tag);
}

void verifyAll(const Mesh& mesh, const DiskResult& r, const char* tag)
{
	for (size_t i = 0; i < r.count; ++i)
		verifyDisk(r, i, tag);

	verifyCoverage(mesh, r, tag);
}

void casesDisk()
{
	// closed shells cannot be a single disk and must split into valid disk clusters
	{
		Mesh mesh = makeTetra();
		DiskResult r = buildDisk(mesh, 64, 124, 0.f);
		task_check(r.count >= 2, "tetra: closed shell must split");
		verifyAll(mesh, r, "tetra");
	}
	{
		Mesh mesh = makeOcta();
		DiskResult r = buildDisk(mesh, 64, 124, 0.f);
		task_check(r.count >= 2, "octa: closed shell must split");
		verifyAll(mesh, r, "octa");
	}

	// a ring sheet has two boundary loops and cannot be a single disk either
	{
		Mesh mesh = makeAnnulus(16, 2);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count >= 2, "annulus: must split");
		verifyAll(mesh, r, "annulus");
	}

	// fans that only touch at one vertex must not merge into one cluster
	{
		Mesh mesh = makePointContact();
		DiskResult r = buildDisk(mesh, 64, 124, 0.f);
		task_check(r.count >= 2, "contact: point contact must split");

		bool separated = true;
		for (size_t i = 0; i < r.count; ++i)
		{
			const meshopt_Meshlet& m = r.disks[i].meshlet;
			unsigned int fan = r.source[m.triangle_offset / 3] / 5;

			for (unsigned int t = 0; t < m.triangle_count; ++t)
				if (r.source[m.triangle_offset / 3 + t] / 5 != fan)
					separated = false;
		}
		task_check(separated, "contact: clusters must not mix fans");
		verifyAll(mesh, r, "contact");
	}

	// coincident components with distinct vertex indices stay distinct points
	{
		Mesh mesh = makeDupGrid();
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count >= 2, "dup: coincident components must split");

		bool separated = true;
		for (size_t i = 0; i < r.count; ++i)
		{
			const meshopt_Meshlet& m = r.disks[i].meshlet;
			unsigned int half = 9, side = r.vertices[m.vertex_offset] / half;

			for (unsigned int v = 0; v < m.vertex_count; ++v)
				if (r.vertices[m.vertex_offset + v] / half != side)
					separated = false;
		}
		task_check(separated, "dup: clusters must not mix components");
		verifyAll(mesh, r, "dup");
	}

	// capacity limits split the mesh, every piece is still a disk
	{
		Mesh mesh = makeGrid(6, 6, 0);
		DiskResult r = buildDisk(mesh, 16, 16, 0.5f);

		bool limits_ok = r.count > 1;
		for (size_t i = 0; i < r.count; ++i)
			if (r.disks[i].meshlet.vertex_count > 16 || r.disks[i].meshlet.triangle_count > 16)
				limits_ok = false;

		task_check(limits_ok, "capacity: limits must be respected");
		verifyAll(mesh, r, "capacity");
	}
}

void casesBoundary()
{
	// a single triangle is its own boundary loop
	{
		Mesh mesh;
		mesh.positions.insert(mesh.positions.end(), {0, 0, 0, 1, 0, 0, 0, 1, 0});
		mesh.indices.insert(mesh.indices.end(), {0, 1, 2});

		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "single: one cluster");
		task_check(r.disks[0].boundary_count == 3, "single: boundary of 3");
		verifyAll(mesh, r, "single");
	}

	// a rectangular grid has its perimeter as the only boundary loop
	{
		Mesh mesh = makeGrid(3, 5, 0);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "grid: one cluster");
		task_check(r.disks[0].boundary_count == 16, "grid: perimeter length");

		// the loop must visit exactly the perimeter vertices of the grid
		std::set<unsigned int> perimeter;
		for (int y = 0; y <= 5; ++y)
			for (int x = 0; x <= 3; ++x)
				if (x == 0 || x == 3 || y == 0 || y == 5)
					perimeter.insert(unsigned(y * 4 + x));

		std::set<unsigned int> loop;
		for (unsigned int k = 0; k < r.disks[0].boundary_count; ++k)
			loop.insert(r.vertices[r.disks[0].meshlet.vertex_offset + r.boundary[r.disks[0].boundary_offset + k]]);

		task_check(loop == perimeter, "grid: perimeter vertices");
		verifyAll(mesh, r, "grid");
	}

	// tetrahedron clusters have valid directed loops
	{
		Mesh mesh = makeTetra();
		DiskResult r = buildDisk(mesh, 64, 124, 0.f);
		task_check(r.count >= 2, "tetra: must split");
		verifyAll(mesh, r, "tetra");
	}

	// annulus boundary consists of the inner and outer loops plus two sides of each cut
	{
		Mesh mesh = makeAnnulus(16, 2);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);

		unsigned int total = 0;
		for (size_t i = 0; i < r.count; ++i)
			total += r.disks[i].boundary_count;

		task_check(total >= 32 && (total - 32) % 2 == 0, "annulus: boundary accounting");
		verifyAll(mesh, r, "annulus");
	}

	// folded strip keeps a single boundary loop along the directed face edges
	{
		Mesh mesh = makeGrid(3, 5, 1);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "folded: one cluster");
		task_check(r.disks[0].boundary_count == 16, "folded: perimeter length");
		verifyAll(mesh, r, "folded");
	}
}

void casesCoverage()
{
	// empty input produces no clusters
	{
		Mesh mesh;
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 0, "empty: no clusters");
	}

	// vertices without faces produce no clusters either
	{
		Mesh mesh;
		mesh.positions.insert(mesh.positions.end(), {0, 0, 0, 1, 0, 0});
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 0, "empty: no faces");
	}

	{
		Mesh mesh = makeTetra();
		DiskResult r = buildDisk(mesh, 64, 124, 0.f);
		verifyCoverage(mesh, r, "tetra");
	}
	{
		Mesh mesh = shuffleFaces(makeGrid(3, 5, 0), 7);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		verifyCoverage(mesh, r, "shuffled");
	}
	{
		Mesh mesh = makeAnnulus(16, 2);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		verifyCoverage(mesh, r, "annulus");
	}
	{
		Mesh mesh = makePointContact();
		DiskResult r = buildDisk(mesh, 64, 124, 0.f);
		verifyCoverage(mesh, r, "contact");
	}
	{
		Mesh mesh = makeDupGrid();
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		verifyCoverage(mesh, r, "dup");
	}
	{
		Mesh mesh = makeOcta();
		DiskResult r = buildDisk(mesh, 16, 8, 0.25f);
		verifyCoverage(mesh, r, "octa-capacity");
	}
}

void casesPacking()
{
	// a 3x5 cell grid fits into one 64/64 cluster, regardless of face order, corner rotation or folding
	{
		Mesh mesh = makeGrid(3, 5, 0);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "pack: plain grid");
	}
	{
		Mesh mesh = shuffleFaces(makeGrid(3, 5, 0), 11);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "pack: shuffled grid 1");
	}
	{
		Mesh mesh = shuffleFaces(makeGrid(3, 5, 0), 23);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "pack: shuffled grid 2");
	}
	{
		Mesh mesh = shuffleFaces(makeGrid(3, 5, 0), 47);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "pack: shuffled grid 3");
	}
	{
		Mesh mesh = rotateCorners(makeGrid(3, 5, 0));
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "pack: rotated grid");
	}
	{
		Mesh mesh = makeGrid(3, 5, 1);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "pack: folded grid");
	}
	{
		Mesh mesh = shuffleFaces(makeGrid(3, 5, 1), 13);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count == 1, "pack: folded shuffled grid");
	}

	// a tetrahedron and a ring sheet cannot be packed into a single cluster
	{
		Mesh mesh = makeTetra();
		DiskResult r = buildDisk(mesh, 64, 124, 0.f);
		task_check(r.count > 1, "pack: tetra splits");
	}
	{
		Mesh mesh = makeAnnulus(16, 2);
		DiskResult r = buildDisk(mesh, 64, 64, 0.f);
		task_check(r.count > 1, "pack: annulus splits");
	}

	// capacity limits are respected and drive the split
	{
		Mesh mesh = makeGrid(3, 5, 0);
		DiskResult r = buildDisk(mesh, 64, 8, 0.f);

		bool limits_ok = r.count >= 4;
		for (size_t i = 0; i < r.count; ++i)
			if (r.disks[i].meshlet.triangle_count > 8)
				limits_ok = false;

		task_check(limits_ok, "pack: triangle limit");
		verifyAll(mesh, r, "pack: triangle limit");
	}
	{
		Mesh mesh = makeGrid(3, 5, 0);
		DiskResult r = buildDisk(mesh, 8, 64, 0.f);

		bool limits_ok = r.count > 1;
		for (size_t i = 0; i < r.count; ++i)
			if (r.disks[i].meshlet.vertex_count > 8)
				limits_ok = false;

		task_check(limits_ok, "pack: vertex limit");
		verifyAll(mesh, r, "pack: vertex limit");
	}
}

} // namespace

void task_cases(const char* group)
{
	std::string g = group;

	if (g == "disk")
		casesDisk();
	else if (g == "boundary")
		casesBoundary();
	else if (g == "coverage")
		casesCoverage();
	else if (g == "packing")
		casesPacking();
}
