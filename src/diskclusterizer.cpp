// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#include "meshoptimizer.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <string.h>

// This builder extends the meshlet growth strategy from clusterizer.cpp with a hard topology
// contract: every non-empty cluster is an oriented 2-manifold disk defined on input vertex indices.
// All adjacency, spatial seeding and candidate scoring machinery mirrors the regular meshlet builder;
// the difference is that candidate admission additionally requires the cluster to remain a disk.
namespace meshopt_disk
{

// To avoid excessive recursion for malformed inputs, we limit the maximum depth of the tree
const int kMaxTreeDepth = 50;

const size_t kMaxVertices = 256;
const size_t kMaxTriangles = 512;

// hash table for directed edge lookup within a cluster; holds at most kMaxTriangles * 3 edges
const size_t kEdgeHashSize = 4096;
const unsigned short kEdgeNil = 0xffff;

struct Cone
{
	float px, py, pz;
	float nx, ny, nz;
};

struct TriangleAdjacency
{
	unsigned int* counts;
	unsigned int* offsets;
	unsigned int* data;
};

static void buildTriangleAdjacency(TriangleAdjacency& adjacency, const unsigned int* indices, size_t index_count, size_t vertex_count, meshopt_Allocator& allocator)
{
	size_t face_count = index_count / 3;

	// allocate arrays
	adjacency.counts = allocator.allocate<unsigned int>(vertex_count);
	adjacency.offsets = allocator.allocate<unsigned int>(vertex_count);
	adjacency.data = allocator.allocate<unsigned int>(index_count);

	// fill triangle counts
	memset(adjacency.counts, 0, vertex_count * sizeof(unsigned int));

	for (size_t i = 0; i < index_count; ++i)
	{
		assert(indices[i] < vertex_count);

		adjacency.counts[indices[i]]++;
	}

	// fill offset table
	unsigned int offset = 0;

	for (size_t i = 0; i < vertex_count; ++i)
	{
		adjacency.offsets[i] = offset;
		offset += adjacency.counts[i];
	}

	assert(offset == index_count);

	// fill triangle data
	for (size_t i = 0; i < face_count; ++i)
	{
		unsigned int a = indices[i * 3 + 0], b = indices[i * 3 + 1], c = indices[i * 3 + 2];

		adjacency.data[adjacency.offsets[a]++] = unsigned(i);
		adjacency.data[adjacency.offsets[b]++] = unsigned(i);
		adjacency.data[adjacency.offsets[c]++] = unsigned(i);
	}

	// fix offsets that were disturbed by the previous pass
	for (size_t i = 0; i < vertex_count; ++i)
	{
		assert(adjacency.offsets[i] >= adjacency.counts[i]);
		adjacency.offsets[i] -= adjacency.counts[i];
	}
}

static float getMeshletScore(float distance, float spread, float cone_weight, float expected_radius)
{
	float cone = 1.f - spread * cone_weight;
	float cone_clamped = cone < 1e-3f ? 1e-3f : cone;

	return (1 + distance / expected_radius * (1 - cone_weight)) * cone_clamped;
}

static Cone getMeshletCone(const Cone& acc, unsigned int triangle_count)
{
	Cone result = acc;

	float center_scale = triangle_count == 0 ? 0.f : 1.f / float(triangle_count);

	result.px *= center_scale;
	result.py *= center_scale;
	result.pz *= center_scale;

	float axis_length = result.nx * result.nx + result.ny * result.ny + result.nz * result.nz;
	float axis_scale = axis_length == 0.f ? 0.f : 1.f / sqrtf(axis_length);

	result.nx *= axis_scale;
	result.ny *= axis_scale;
	result.nz *= axis_scale;

	return result;
}

static float computeTriangleCones(Cone* triangles, const unsigned int* indices, size_t index_count, const float* vertex_positions, size_t vertex_count, size_t vertex_positions_stride)
{
	(void)vertex_count;

	size_t vertex_stride_float = vertex_positions_stride / sizeof(float);
	size_t face_count = index_count / 3;

	float mesh_area = 0;

	for (size_t i = 0; i < face_count; ++i)
	{
		unsigned int a = indices[i * 3 + 0], b = indices[i * 3 + 1], c = indices[i * 3 + 2];
		assert(a < vertex_count && b < vertex_count && c < vertex_count);

		const float* p0 = vertex_positions + vertex_stride_float * a;
		const float* p1 = vertex_positions + vertex_stride_float * b;
		const float* p2 = vertex_positions + vertex_stride_float * c;

		float p10[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
		float p20[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};

		float normalx = p10[1] * p20[2] - p10[2] * p20[1];
		float normaly = p10[2] * p20[0] - p10[0] * p20[2];
		float normalz = p10[0] * p20[1] - p10[1] * p20[0];

		float area = sqrtf(normalx * normalx + normaly * normaly + normalz * normalz);
		float invarea = (area == 0.f) ? 0.f : 1.f / area;

		triangles[i].px = (p0[0] + p1[0] + p2[0]) / 3.f;
		triangles[i].py = (p0[1] + p1[1] + p2[1]) / 3.f;
		triangles[i].pz = (p0[2] + p1[2] + p2[2]) / 3.f;

		triangles[i].nx = normalx * invarea;
		triangles[i].ny = normaly * invarea;
		triangles[i].nz = normalz * invarea;

		mesh_area += area;
	}

	return mesh_area;
}

struct KDNode
{
	union
	{
		float split;
		unsigned int index;
	};

	// leaves: axis = 3, children = number of points including this one
	// branches: axis != 3, left subtree = skip 1, right subtree = skip 1+children
	unsigned int axis : 2;
	unsigned int children : 30;
};

static size_t kdtreePartition(unsigned int* indices, size_t count, const float* points, size_t stride, int axis, float pivot)
{
	size_t m = 0;

	// invariant: elements in range [0, m) are < pivot, elements in range [m, i) are >= pivot
	for (size_t i = 0; i < count; ++i)
	{
		float v = points[indices[i] * stride + axis];

		// swap(m, i) unconditionally
		unsigned int t = indices[m];
		indices[m] = indices[i];
		indices[i] = t;

		// when v >= pivot, we swap i with m without advancing it, preserving invariants
		m += v < pivot;
	}

	return m;
}

static size_t kdtreeBuildLeaf(size_t offset, KDNode* nodes, size_t node_count, unsigned int* indices, size_t count)
{
	assert(offset + count <= node_count);
	(void)node_count;

	KDNode& result = nodes[offset];

	result.index = indices[0];
	result.axis = 3;
	result.children = unsigned(count);

	// all remaining points are stored in nodes immediately following the leaf
	for (size_t i = 1; i < count; ++i)
	{
		KDNode& tail = nodes[offset + i];

		tail.index = indices[i];
		tail.axis = 3;
		tail.children = ~0u >> 2; // bogus value to prevent misuse
	}

	return offset + count;
}

static size_t kdtreeBuild(size_t offset, KDNode* nodes, size_t node_count, const float* points, size_t stride, unsigned int* indices, size_t count, size_t leaf_size, int depth)
{
	assert(count > 0);
	assert(offset < node_count);

	if (count <= leaf_size)
		return kdtreeBuildLeaf(offset, nodes, node_count, indices, count);

	float mean[3] = {};
	float vars[3] = {};
	float runc = 1, runs = 1;

	// gather statistics on the points in the subtree using Welford's algorithm
	for (size_t i = 0; i < count; ++i, runc += 1.f, runs = 1.f / runc)
	{
		const float* point = points + indices[i] * stride;

		for (int k = 0; k < 3; ++k)
		{
			float delta = point[k] - mean[k];
			mean[k] += delta * runs;
			vars[k] += delta * (point[k] - mean[k]);
		}
	}

	// split axis is one where the variance is largest
	int axis = (vars[0] >= vars[1] && vars[0] >= vars[2]) ? 0 : (vars[1] >= vars[2] ? 1 : 2);

	float split = mean[axis];
	size_t middle = kdtreePartition(indices, count, points, stride, axis, split);

	// when the partition is degenerate, consolidate the points into a single node; also ensure recursion depth is bounded on pathological inputs
	if (middle == 0 || middle == count || depth >= kMaxTreeDepth)
		return kdtreeBuildLeaf(offset, nodes, node_count, indices, count);

	// when the partition is unbalanced, consolidate the points into a single small node; large nodes are still split to prevent outlier points from creating very large leaves
	if ((middle <= leaf_size / 2 || middle >= count - leaf_size / 2) && count <= leaf_size * 8)
		return kdtreeBuildLeaf(offset, nodes, node_count, indices, count);

	KDNode& result = nodes[offset];

	result.split = split;
	result.axis = axis;

	// left subtree is right after our node
	size_t next_offset = kdtreeBuild(offset + 1, nodes, node_count, points, stride, indices, middle, leaf_size, depth + 1);

	// distance to the right subtree is represented explicitly
	assert(next_offset - offset > 1);
	result.children = unsigned(next_offset - offset - 1);

	return kdtreeBuild(next_offset, nodes, node_count, points, stride, indices + middle, count - middle, leaf_size, depth + 1);
}

static void kdtreeNearest(KDNode* nodes, unsigned int root, const float* points, size_t stride, const unsigned char* emitted_flags, const float* position, unsigned int& result, float& limit)
{
	const KDNode& node = nodes[root];

	if (node.children == 0)
		return;

	if (node.axis == 3)
	{
		// leaf
		bool inactive = true;

		for (unsigned int i = 0; i < node.children; ++i)
		{
			unsigned int index = nodes[root + i].index;

			if (emitted_flags[index])
				continue;

			inactive = false;

			const float* point = points + index * stride;

			float dx = point[0] - position[0], dy = point[1] - position[1], dz = point[2] - position[2];
			float distance = sqrtf(dx * dx + dy * dy + dz * dz);

			if (distance < limit)
			{
				result = index;
				limit = distance;
			}
		}

		// deactivate leaves that no longer have items to emit
		if (inactive)
			nodes[root].children = 0;
	}
	else
	{
		// branch; we order recursion to process the node that search position is in first
		float delta = position[node.axis] - node.split;
		unsigned int first = (delta <= 0) ? 0 : node.children;
		unsigned int second = first ^ node.children;

		// deactivate branches that no longer have items to emit to accelerate traversal
		// note that we do this *before* recursing which delays deactivation but keeps tail calls
		if ((nodes[root + 1 + first].children | nodes[root + 1 + second].children) == 0)
			nodes[root].children = 0;

		// recursion depth is bounded by tree depth (which is limited by construction)
		kdtreeNearest(nodes, root + 1 + first, points, stride, emitted_flags, position, result, limit);

		// only process the other node if it can have a match based on closest distance so far
		if (fabsf(delta) <= limit)
			kdtreeNearest(nodes, root + 1 + second, points, stride, emitted_flags, position, result, limit);
	}
}

// A directed edge of a cluster triangle; links are stored as record index + 1 with 0 meaning empty.
struct EdgeRecord
{
	unsigned short va;
	unsigned short vb;
	unsigned short face;
	unsigned short hash_prev;
	unsigned short vert_next;
};

struct DiskCluster
{
	// cluster triangles and vertices in insertion order; vertices use input indices
	unsigned int faces[kMaxTriangles];
	unsigned int vertices[kMaxVertices];
	size_t face_count;
	size_t vertex_count;

	// directed edge table with a hash over local vertex pairs
	EdgeRecord edges[kMaxTriangles * 3];
	size_t edge_count;
	unsigned short edge_head[kEdgeHashSize];
	unsigned short vert_head[kMaxVertices];

	// incremental disk invariants: boundary edge incidence per local vertex, face DSU and running counts
	unsigned char boundary_in[kMaxVertices];
	unsigned char boundary_out[kMaxVertices];
	unsigned short dsu_parent[kMaxTriangles];
	size_t undirected_edge_count;
	size_t boundary_length;
	size_t components;

	void reset()
	{
		face_count = 0;
		vertex_count = 0;
		edge_count = 0;
		undirected_edge_count = 0;
		boundary_length = 0;
		components = 0;
		memset(edge_head, 0, sizeof(edge_head));
		memset(vert_head, 0, sizeof(vert_head));
		memset(boundary_in, 0, sizeof(boundary_in));
		memset(boundary_out, 0, sizeof(boundary_out));
	}

	unsigned int dsuFind(unsigned int x) const
	{
		while (dsu_parent[x] != x)
			x = dsu_parent[x];

		return x;
	}

	static size_t hashEdge(unsigned int a, unsigned int b)
	{
		return (a * 2654435761u ^ b * 2246822519u) & (kEdgeHashSize - 1);
	}

	unsigned short findEdge(unsigned int a, unsigned int b) const
	{
		size_t h = hashEdge(a, b);

		for (unsigned int link = edge_head[h]; link != 0; link = edges[link - 1].hash_prev)
		{
			const EdgeRecord& e = edges[link - 1];

			if (e.va == a && e.vb == b)
				return link - 1;
		}

		return kEdgeNil;
	}

	bool isBoundaryEdge(size_t edge) const
	{
		const EdgeRecord& e = edges[edge];

		return findEdge(e.vb, e.va) == kEdgeNil;
	}

	void addEdge(unsigned short a, unsigned short b, unsigned short face)
	{
		size_t h = hashEdge(a, b);
		unsigned short link = (unsigned short)(edge_count + 1);

		EdgeRecord& e = edges[edge_count++];
		e.va = a;
		e.vb = b;
		e.face = face;
		e.hash_prev = edge_head[h];
		e.vert_next = vert_head[a];

		edge_head[h] = link;
		vert_head[a] = link;
	}

	// Full topology predicate: the current triangle set must be an oriented 2-manifold disk.
	// Rebuilt from scratch purely off the edge table so it independently cross-checks the incremental
	// admission invariants; a cluster never exceeds kMaxVertices/kMaxTriangles so this stays small.
	bool isDisk() const
	{
		size_t f = face_count;
		assert(f > 0);

		// count paired (interior) directed edges and boundary directed edges
		size_t paired = 0;

		for (size_t i = 0; i < edge_count; ++i)
			if (!isBoundaryEdge(i))
				paired++;

		size_t boundary = edge_count - paired;

		// a disk has a non-empty boundary; this rejects closed shells such as a tetrahedron
		if (boundary == 0)
			return false;

		// every vertex has at most one outgoing boundary edge; boundary vertices have exactly one while
		// interior vertices have none. Point contacts (two fans at one vertex) produce two and are rejected.
		unsigned char boundary_at_vertex[kMaxVertices] = {};

		for (size_t i = 0; i < edge_count; ++i)
		{
			if (isBoundaryEdge(i))
				boundary_at_vertex[edges[i].va]++;
		}

		for (size_t i = 0; i < vertex_count; ++i)
			if (boundary_at_vertex[i] > 1)
				return false;

		// faces must be connected through complete shared edges, not just shared vertices
		unsigned short parent[kMaxTriangles];

		for (size_t i = 0; i < f; ++i)
			parent[i] = (unsigned short)i;

		auto find_root = [&](unsigned int x) -> unsigned int {
			unsigned int root = x;

			while (parent[root] != root)
				root = parent[root];

			while (parent[x] != x)
			{
				unsigned int next = parent[x];
				parent[x] = (unsigned short)root;
				x = next;
			}

			return root;
		};

		for (size_t i = 0; i < edge_count; ++i)
		{
			const EdgeRecord& e = edges[i];
			unsigned short r = findEdge(e.vb, e.va);

			// process each undirected interior edge once
			if (r != kEdgeNil && i < r)
			{
				unsigned int ra = find_root(e.face), rb = find_root(edges[r].face);

				if (ra != rb)
					parent[rb] = (unsigned short)ra;
			}
		}

		unsigned int root = find_root(0);

		for (size_t i = 1; i < f; ++i)
			if (find_root(unsigned(i)) != root)
				return false;

		// boundary must be a single simple ring; walk directed boundary edges without repeating a vertex
		unsigned short start = kEdgeNil;

		for (size_t i = 0; i < edge_count; ++i)
		{
			if (isBoundaryEdge(i))
			{
				start = (unsigned short)i;
				break;
			}
		}

		assert(start != kEdgeNil);

		unsigned char seen[kMaxVertices] = {};
		unsigned short current = start;
		size_t steps = 0;

		for (;;)
		{
			unsigned short v = edges[current].va;

			if (seen[v])
				return false;

			seen[v] = 1;
			steps++;

			unsigned short next = kEdgeNil;

			for (unsigned int link = vert_head[edges[current].vb]; link != 0; link = edges[link - 1].vert_next)
			{
				unsigned short edge = link - 1;

				if (isBoundaryEdge(edge))
				{
					// at a boundary vertex there is exactly one outgoing boundary edge
					if (next != kEdgeNil)
						return false;

					next = edge;
				}
			}

			if (next == kEdgeNil)
				return false;

			if (next == start)
				break;

			current = next;

			if (steps >= edge_count)
				return false;
		}

		if (steps != boundary)
			return false;

		// Euler characteristic of a disk is exactly 1; E = interior edges + boundary edges
		size_t interior = paired / 2;
		size_t edges_undirected = interior + boundary;
		int chi = int(vertex_count) - int(edges_undirected) + int(face_count);

		return chi == 1;
	}

	// Admission classification for a candidate triangle with global corners a/b/c:
	//  0 = not addable right now (vertex-only contact, a transient bridge/hole or over capacity); retry later
	//  1 = permanently illegal for this disk (duplicate directed edge or an edge already shared by two faces)
	//  2 = eligible; gluing it preserves the oriented manifold disk invariants
	// Resolved local corners are written to locals[3].
	int evaluate(unsigned int a, unsigned int b, unsigned int c, const short* used, size_t max_vertices, unsigned short* locals) const
	{
		unsigned int globals[3] = {a, b, c};
		size_t extra = 0;

		for (size_t k = 0; k < 3; ++k)
		{
			if (used[globals[k]] >= 0)
				locals[k] = (unsigned short)used[globals[k]];
			else
				locals[k] = (unsigned short)(vertex_count + extra++);
		}

		if (vertex_count + extra > max_vertices)
			return 0; // extra vertices may join the disk through other faces first

		size_t glued = 0;
		size_t glued_vertex_mask = 0;

		for (size_t k = 0; k < 3; ++k)
		{
			unsigned short u = locals[k], v = locals[(k + 1) % 3];
			unsigned short forward = findEdge(u, v);
			unsigned short reverse = findEdge(v, u);

			// a directed edge already exists in this direction: duplicate face or same-direction shared edge,
			// which can never become legal as the disk grows
			if (forward != kEdgeNil)
				return 1;

			// reverse boundary edge exists: the face would glue along it, removing the edge from the boundary
			if (reverse != kEdgeNil)
			{
				if (!isBoundaryEdge(reverse))
					return 1; // edge already interior (three faces share it); permanently impossible

				glued++;
				glued_vertex_mask |= (1u << k) | (1u << ((k + 1) % 3));
			}
		}

		// no shared edge: the face can only attach at vertices for now
		if (glued == 0)
			return 0;

		// gluing along three boundary edges would close the last hole and produce a closed shell;
		// retry because other faces may change the boundary before this one is reconsidered
		if (glued == 3)
			return 0;

		// with one glued edge, the corner opposite that edge must be a new vertex; reusing an existing
		// vertex there would bridge two boundary locations. This is transient: a second shared edge may appear
		if (glued == 1)
		{
			for (size_t k = 0; k < 3; ++k)
				if ((glued_vertex_mask & (1u << k)) == 0 && locals[k] < vertex_count)
					return 0;
		}

		return 2;
	}

	// Commit a triangle whose eligibility was already established; local corners are resolved up front.
	// The seed triangle (face_count == 0) is accepted directly and starts a valid one-face disk.
	bool commitFace(unsigned int face, unsigned int a, unsigned int b, unsigned int c, const short* used, const unsigned short* locals, size_t max_triangles)
	{
		if (face_count >= max_triangles)
			return false;

		unsigned int globals[3] = {a, b, c};
		unsigned short corners[3] = {locals[0], locals[1], locals[2]};

#ifndef NDEBUG
		size_t old_vertices = vertex_count;
		size_t old_edges = edge_count;
		size_t old_faces = face_count;
		size_t old_undirected = undirected_edge_count;
		size_t old_boundary = boundary_length;
		size_t old_components = components;
		unsigned char old_bi[kMaxVertices] = {};
		unsigned char old_bo[kMaxVertices] = {};

		for (size_t k = 0; k < 3; ++k)
		{
			old_bi[corners[k]] = boundary_in[corners[k]];
			old_bo[corners[k]] = boundary_out[corners[k]];
		}
#endif

		// register new vertices
		for (size_t k = 0; k < 3; ++k)
		{
			if (used[globals[k]] < 0)
				vertices[vertex_count++] = globals[k];
		}

		faces[face_count] = face;
		dsu_parent[face_count] = (unsigned short)face_count;

		bool merged_component = false;

		for (size_t k = 0; k < 3; ++k)
		{
			unsigned short u = corners[k], v = corners[(k + 1) % 3];
			unsigned short reverse = findEdge(v, u);

			if (reverse != kEdgeNil)
			{
				// gluing along a boundary edge removes it from the boundary ring; the undirected edge is already counted
				assert(boundary_out[v] > 0 && boundary_in[u] > 0);
				boundary_out[v]--;
				boundary_in[u]--;
				boundary_length--;

				unsigned int ra = dsuFind(face_count), rb = dsuFind(edges[reverse].face);

				if (ra != rb)
				{
					dsu_parent[rb] = (unsigned short)ra;
					merged_component = true;
				}
			}
			else
			{
				// a brand new undirected edge contributes a boundary edge oriented u -> v
				boundary_out[u]++;
				boundary_in[v]++;
				boundary_length++;
				undirected_edge_count++;
			}

			addEdge(u, v, (unsigned short)face_count);
		}

		face_count++;
		components += merged_component ? 0 : 1;

		// authoritative cross-check of the incremental invariants; compiled out in release builds
		assert(isDisk());
		assert(int(vertex_count) - int(undirected_edge_count) + int(face_count) == 1);
		assert(boundary_length > 0);
		assert(components == 1);

#ifndef NDEBUG
		(void)old_vertices;
		(void)old_edges;
		(void)old_faces;
		(void)old_undirected;
		(void)old_boundary;
		(void)old_components;
		(void)old_bi;
		(void)old_bo;
#endif
		return true;
	}

	// Extract the boundary ring as local vertex ids without repeating the first vertex.
	size_t extractBoundary(unsigned char* output) const
	{
		unsigned short start = kEdgeNil;

		for (size_t i = 0; i < edge_count; ++i)
		{
			if (isBoundaryEdge(i))
			{
				start = (unsigned short)i;
				break;
			}
		}

		assert(start != kEdgeNil);

		unsigned short current = start;
		size_t count = 0;

		for (;;)
		{
			output[count++] = (unsigned char)edges[current].va;

			unsigned short next = kEdgeNil;

			for (unsigned int link = vert_head[edges[current].vb]; link != 0; link = edges[link - 1].vert_next)
			{
				unsigned short edge = link - 1;

				if (isBoundaryEdge(edge))
				{
					assert(next == kEdgeNil);
					next = edge;
				}
			}

			assert(next != kEdgeNil);

			if (next == start)
				break;

			current = next;
		}

		return count;
	}
};

} // namespace meshopt_disk

size_t meshopt_buildMeshletsDisk(meshopt_MeshletDisk* disks, unsigned int* meshlet_vertices, unsigned char* meshlet_triangles, unsigned int* triangle_source, unsigned char* boundary_vertices, const unsigned int* indices, size_t index_count, const float* vertex_positions, size_t vertex_count, size_t vertex_positions_stride, size_t max_vertices, size_t max_triangles, float cone_weight)
{
	using namespace meshopt_disk;

	assert(index_count % 3 == 0);
	assert(vertex_positions_stride >= 12 && vertex_positions_stride <= 256);
	assert(vertex_positions_stride % sizeof(float) == 0);

	assert(max_vertices >= 3 && max_vertices <= kMaxVertices);
	assert(max_triangles >= 1 && max_triangles <= kMaxTriangles);

	assert(cone_weight >= 0 && cone_weight <= 1);

	if (index_count == 0)
		return 0;

	size_t face_count = index_count / 3;

	meshopt_Allocator allocator;

	TriangleAdjacency adjacency = {};
	buildTriangleAdjacency(adjacency, indices, index_count, vertex_count, allocator);

	// live triangle counts per vertex; decremented as triangles get emitted into disks
	unsigned int* live_triangles = allocator.allocate<unsigned int>(vertex_count);
	memcpy(live_triangles, adjacency.counts, vertex_count * sizeof(unsigned int));

	unsigned char* emitted_flags = allocator.allocate<unsigned char>(face_count);
	memset(emitted_flags, 0, face_count);

	// per-face bookkeeping for the frontier: flag 0 = absent, 1 = active, 2 = sleeping (vertex-only for now)
	unsigned char* frontier_flag = allocator.allocate<unsigned char>(face_count);
	// permanently illegal faces are tagged with the active disk serial so later disks reconsider them
	unsigned int* reject_mark = allocator.allocate<unsigned int>(face_count);
	// position of a face inside whichever frontier list currently holds it
	unsigned int* frontier_pos = allocator.allocate<unsigned int>(face_count);
	unsigned int* active = allocator.allocate<unsigned int>(face_count);
	unsigned int* sleeping = allocator.allocate<unsigned int>(face_count);
	// per-step scratch for faces that fall asleep or get rejected during ranking
	unsigned int* sleep_scratch = allocator.allocate<unsigned int>(face_count);
	unsigned int* reject_scratch = allocator.allocate<unsigned int>(face_count);
	memset(frontier_flag, 0, face_count);
	memset(reject_mark, 0, face_count * sizeof(unsigned int));

	// for each triangle, precompute centroid & normal to use for scoring
	Cone* triangles = allocator.allocate<Cone>(face_count);
	float mesh_area = computeTriangleCones(triangles, indices, index_count, vertex_positions, vertex_count, vertex_positions_stride);

	// assuming each meshlet is a square patch, expected radius is sqrt(expected area)
	float triangle_area_avg = face_count == 0 ? 0.f : mesh_area / float(face_count) * 0.5f;
	float meshlet_expected_radius = sqrtf(triangle_area_avg * max_triangles) * 0.5f;

	// build a kd-tree for nearest neighbor lookup to seed disconnected components
	unsigned int* kdindices = allocator.allocate<unsigned int>(face_count);
	for (size_t i = 0; i < face_count; ++i)
		kdindices[i] = unsigned(i);

	KDNode* nodes = allocator.allocate<KDNode>(face_count * 2);
	kdtreeBuild(0, nodes, face_count * 2, &triangles[0].px, sizeof(Cone) / sizeof(float), kdindices, face_count, /* leaf_size= */ 8, 0);

	// find a specific corner of the mesh to use as a starting point for disk flow
	float cornerx = FLT_MAX, cornery = FLT_MAX, cornerz = FLT_MAX;

	for (size_t i = 0; i < face_count; ++i)
	{
		const Cone& tri = triangles[i];

		cornerx = cornerx > tri.px ? tri.px : cornerx;
		cornery = cornery > tri.py ? tri.py : cornery;
		cornerz = cornerz > tri.pz ? tri.pz : cornerz;
	}

	// local index of the vertex in the active disk, -1 if the vertex isn't used
	short* used = allocator.allocate<short>(vertex_count);

	if (vertex_count <= index_count)
		memset(used, -1, vertex_count * sizeof(short));
	else
		for (size_t i = 0; i < index_count; ++i)
			used[indices[i]] = -1;

	DiskCluster cluster;
	cluster.reset();

	size_t disk_offset = 0;
	size_t vertex_output_offset = 0;
	size_t triangle_output_offset = 0;
	size_t boundary_output_offset = 0;

	unsigned int cluster_serial = 0;
	size_t emitted_total = 0;

	while (emitted_total < face_count)
	{
		// start a new disk from the unemitted triangle closest to the mesh corner
		float position[3] = {cornerx, cornery, cornerz};
		unsigned int seed = ~0u;
		float distance = FLT_MAX;

		kdtreeNearest(nodes, 0, &triangles[0].px, sizeof(Cone) / sizeof(float), emitted_flags, position, seed, distance);

		if (seed == ~0u)
			break;

		cluster_serial++;

		Cone disk_cone_acc = {};

		// partitioned frontier: active candidates are ranked each step; sleeping faces only touched the disk
		// at vertices and are woken when a committed face shares one of their vertices
		size_t active_count = 0;
		size_t sleep_count = 0;
		size_t sleep_scratch_count = 0;
		size_t reject_scratch_count = 0;

		auto remove_active = [&](unsigned int triangle) {
			size_t pos = frontier_pos[triangle];
			unsigned int last = active[active_count - 1];

			active[pos] = last;
			frontier_pos[last] = (unsigned int)pos;
			active_count--;
		};

		auto remove_sleeping = [&](unsigned int triangle) {
			size_t pos = frontier_pos[triangle];
			unsigned int last = sleeping[sleep_count - 1];

			sleeping[pos] = last;
			frontier_pos[last] = (unsigned int)pos;
			sleep_count--;
		};

		// introduce a face to the active frontier unless it is emitted, rejected or already tracked
		auto push_active = [&](unsigned int triangle) {
			if (emitted_flags[triangle] || frontier_flag[triangle] != 0 || reject_mark[triangle] == cluster_serial)
				return;

			frontier_flag[triangle] = 1;
			frontier_pos[triangle] = (unsigned int)active_count;
			active[active_count++] = triangle;
		};

		auto wake = [&](unsigned int triangle) {
			if (frontier_flag[triangle] != 2)
				return;

			remove_sleeping(triangle);

			frontier_flag[triangle] = 1;
			frontier_pos[triangle] = (unsigned int)active_count;
			active[active_count++] = triangle;
		};

		// drop an emitted triangle from the shrinking adjacency lists so each list is traversed linearly overall
		auto remove_from_adjacency = [&](unsigned int triangle) {
			for (size_t k = 0; k < 3; ++k)
			{
				unsigned int vertex = indices[triangle * 3 + k];
				unsigned int* neighbors = &adjacency.data[0] + adjacency.offsets[vertex];
				size_t neighbors_size = adjacency.counts[vertex];

				for (size_t i = 0; i < neighbors_size; ++i)
				{
					if (neighbors[i] == triangle)
					{
						neighbors[i] = neighbors[neighbors_size - 1];
						adjacency.counts[vertex]--;
						break;
					}
				}
			}
		};

		// reconsider every neighbor of a vertex: sleeping faces wake up, unseen faces join the frontier
		auto refresh_vertex = [&](unsigned int vertex) {
			unsigned int* neighbors = &adjacency.data[0] + adjacency.offsets[vertex];
			size_t neighbors_size = adjacency.counts[vertex];

			for (size_t j = 0; j < neighbors_size; ++j)
			{
				unsigned int triangle = neighbors[j];

				if (frontier_flag[triangle] == 2)
					wake(triangle);
				else
					push_active(triangle);
			}
		};

		for (;;)
		{
			unsigned int chosen = ~0u;
			unsigned short chosen_locals[3] = {0, 0, 0};

			if (cluster.face_count == 0)
			{
				chosen = seed;
				chosen_locals[0] = 0;
				chosen_locals[1] = 1;
				chosen_locals[2] = 2;
			}
			else
			{
				// a disk that reached the triangle limit cannot accept any more faces
				if (cluster.face_count >= max_triangles)
					break;

				Cone disk_cone = getMeshletCone(disk_cone_acc, unsigned(cluster.face_count));

				// rank active candidates exactly like the regular meshlet builder; the incremental topology
				// predicate decides eligibility, and only legal candidates compete on priority and cone score
				int best_priority = 5;
				float best_score = FLT_MAX;
				sleep_scratch_count = 0;
				reject_scratch_count = 0;

				for (size_t i = 0; i < active_count; ++i)
				{
					unsigned int triangle = active[i];

					unsigned int a = indices[triangle * 3 + 0], b = indices[triangle * 3 + 1], c = indices[triangle * 3 + 2];
					unsigned short locals[3];

					int verdict = cluster.evaluate(a, b, c, used, max_vertices, locals);

					if (verdict == 0)
					{
						sleep_scratch[sleep_scratch_count++] = triangle; // vertex-only for now; wake on a shared-vertex commit
						continue;
					}

					if (verdict == 1)
					{
						reject_scratch[reject_scratch_count++] = triangle; // permanently illegal for this disk
						continue;
					}

					int extra = (used[a] < 0) + (used[b] < 0) + (used[c] < 0);
					assert(extra <= 2);

					int priority = -1;

					// triangles that don't add new vertices to disks are max. priority
					if (extra == 0)
						priority = 0;
					// artificially increase the priority of dangling triangles as they're expensive to add to new disks
					else if (live_triangles[a] == 1 || live_triangles[b] == 1 || live_triangles[c] == 1)
						priority = 1;
					// if two vertices have live count of 2, removing this triangle will make another triangle dangling which is good for overall flow
					else if ((live_triangles[a] == 2) + (live_triangles[b] == 2) + (live_triangles[c] == 2) >= 2)
						priority = 1 + extra;
					// otherwise adjust priority to be after the above cases, 3 or 4 based on used[] count
					else
						priority = 2 + extra;

					if (priority > best_priority)
						continue;

					const Cone& tri_cone = triangles[triangle];

					float dx = tri_cone.px - disk_cone.px, dy = tri_cone.py - disk_cone.py, dz = tri_cone.pz - disk_cone.pz;
					float tri_distance = sqrtf(dx * dx + dy * dy + dz * dz);
					float spread = tri_cone.nx * disk_cone.nx + tri_cone.ny * disk_cone.ny + tri_cone.nz * disk_cone.nz;

					float score = getMeshletScore(tri_distance, spread, cone_weight, meshlet_expected_radius);

					// note that topology-based priority is always more important than the score
					if (priority < best_priority || score < best_score)
					{
						chosen = triangle;
						chosen_locals[0] = locals[0];
						chosen_locals[1] = locals[1];
						chosen_locals[2] = locals[2];
						best_priority = priority;
						best_score = score;
					}
				}

				// retire faces that were rejected or fell asleep while keeping the active region compact
				for (size_t i = 0; i < reject_scratch_count; ++i)
				{
					unsigned int triangle = reject_scratch[i];

					if (frontier_flag[triangle] != 1)
						continue;

					reject_mark[triangle] = cluster_serial;
					frontier_flag[triangle] = 0;
					remove_active(triangle);
				}

				for (size_t i = 0; i < sleep_scratch_count; ++i)
				{
					unsigned int triangle = sleep_scratch[i];

					if (frontier_flag[triangle] != 1)
						continue;

					remove_active(triangle);

					frontier_flag[triangle] = 2;
					frontier_pos[triangle] = (unsigned int)sleep_count;
					sleeping[sleep_count++] = triangle;
				}

				// no addable candidate remains: sleeping faces cannot wake without another commit, so the disk is done
				if (chosen == ~0u)
					break;
			}

			unsigned int a = indices[chosen * 3 + 0], b = indices[chosen * 3 + 1], c = indices[chosen * 3 + 2];
			assert(a < vertex_count && b < vertex_count && c < vertex_count);

			bool ok = cluster.commitFace(chosen, a, b, c, used, chosen_locals, max_triangles);
			assert(ok);
			(void)ok;

			// keep the global->local vertex mapping in sync with the cluster vertex table
			for (size_t i = 0; i < cluster.vertex_count; ++i)
				used[cluster.vertices[i]] = short(i);

			// the committed face leaves the active frontier and the live adjacency
			if (frontier_flag[chosen] == 1)
			{
				frontier_flag[chosen] = 0;
				remove_active(chosen);
			}

			remove_from_adjacency(chosen);

			// committing this face may make sleeping neighbors edge-adjacent; refresh all three vertices
			for (size_t k = 0; k < 3; ++k)
			{
				unsigned int vertex = indices[chosen * 3 + k];
				live_triangles[vertex]--;
				refresh_vertex(vertex);
			}

			disk_cone_acc.px += triangles[chosen].px;
			disk_cone_acc.py += triangles[chosen].py;
			disk_cone_acc.pz += triangles[chosen].pz;
			disk_cone_acc.nx += triangles[chosen].nx;
			disk_cone_acc.ny += triangles[chosen].ny;
			disk_cone_acc.nz += triangles[chosen].nz;
		}

		assert(cluster.face_count > 0);

		// emit the finished disk: vertices, micro-index triangles, source face ids and boundary ring
		meshopt_Meshlet& meshlet = disks[disk_offset].meshlet;
		meshlet.vertex_offset = unsigned(vertex_output_offset);
		meshlet.triangle_offset = unsigned(triangle_output_offset);
		meshlet.vertex_count = unsigned(cluster.vertex_count);
		meshlet.triangle_count = unsigned(cluster.face_count);

		for (size_t i = 0; i < cluster.vertex_count; ++i)
			meshlet_vertices[vertex_output_offset + i] = cluster.vertices[i];

		for (size_t i = 0; i < cluster.face_count; ++i)
		{
			unsigned int face = cluster.faces[i];

			triangle_source[triangle_output_offset / 3 + i] = face;

			for (size_t k = 0; k < 3; ++k)
			{
				unsigned int global = indices[face * 3 + k];
				assert(used[global] >= 0);
				meshlet_triangles[triangle_output_offset + i * 3 + k] = (unsigned char)used[global];
			}
		}

		size_t boundary_count = cluster.extractBoundary(boundary_vertices + boundary_output_offset);

		disks[disk_offset].boundary_offset = unsigned(boundary_output_offset);
		disks[disk_offset].boundary_count = unsigned(boundary_count);

		vertex_output_offset += cluster.vertex_count;
		triangle_output_offset += cluster.face_count * 3;
		boundary_output_offset += boundary_count;

		// mark every face of the finished disk emitted and reset the frontier state for the next disk
		for (size_t i = 0; i < cluster.face_count; ++i)
		{
			unsigned int face = cluster.faces[i];

			assert(!emitted_flags[face]);
			emitted_flags[face] = 1;
			emitted_total++;
		}

		for (size_t i = 0; i < active_count; ++i)
			frontier_flag[active[i]] = 0;

		for (size_t i = 0; i < sleep_count; ++i)
			frontier_flag[sleeping[i]] = 0;

		// release the vertices of this disk before starting the next one
		for (size_t i = 0; i < cluster.vertex_count; ++i)
			used[cluster.vertices[i]] = -1;

		disk_offset++;
		cluster.reset();
	}

	assert(triangle_output_offset == index_count);
	assert(vertex_output_offset <= index_count);
	assert(boundary_output_offset <= index_count);

	return disk_offset;
}
