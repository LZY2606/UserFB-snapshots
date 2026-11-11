// Group simplification regression tests
// This file uses assert() to verify algorithm correctness
#include "../src/meshoptimizer.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <set>
#include <utility>
#include <vector>

#undef NDEBUG
#include <assert.h>

namespace
{

const unsigned int kGroupLeft = 7;
const unsigned int kGroupRight = 4000000000u; // larger than INT32_MAX to make sure labels are treated as opaque tags

struct GroupMesh
{
	int n = 0;
	bool split_seam = false;
	size_t vertices = 0;
	std::vector<float> positions; // xyz, stride 12
	std::vector<float> uvs;       // uv, stride 8
	std::vector<unsigned int> indices;
	std::vector<unsigned int> groups;
	size_t left_faces = 0;
	size_t right_faces = 0;
};

static void pushQuad(std::vector<unsigned int>& indices, std::vector<unsigned int>& groups, unsigned int a, unsigned int b, unsigned int c, unsigned int d, unsigned int group)
{
	indices.push_back(a);
	indices.push_back(b);
	indices.push_back(c);
	indices.push_back(b);
	indices.push_back(d);
	indices.push_back(c);

	groups.push_back(group);
	groups.push_back(group);
}

// builds two NxN subdivided planes sharing the geometric edge at x=1; left plane uses kGroupLeft, right plane uses kGroupRight
static GroupMesh buildPlanePair(int n, bool split_seam)
{
	GroupMesh m;
	m.n = n;
	m.split_seam = split_seam;

	if (!split_seam)
	{
		// welded grid: vertices at x=1 are shared between the two planes
		int w = 2 * n + 1;
		int h = n + 1;

		m.vertices = size_t(w) * h;
		m.positions.resize(m.vertices * 3);
		m.uvs.resize(m.vertices * 2);

		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				unsigned int id = y * w + x;
				m.positions[id * 3 + 0] = float(x) / float(n);
				m.positions[id * 3 + 1] = float(y) / float(n);
				m.positions[id * 3 + 2] = 0.f;
				m.uvs[id * 2 + 0] = float(x) / float(n);
				m.uvs[id * 2 + 1] = float(y) / float(n);
			}

		auto id = [w](int x, int y) -> unsigned int { return unsigned(y * w + x); };

		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				pushQuad(m.indices, m.groups, id(x, y), id(x + 1, y), id(x, y + 1), id(x + 1, y + 1), kGroupLeft);
				m.left_faces += 2;
			}

		for (int y = 0; y < n; ++y)
			for (int x = n; x < 2 * n; ++x)
			{
				pushQuad(m.indices, m.groups, id(x, y), id(x + 1, y), id(x, y + 1), id(x + 1, y + 1), kGroupRight);
				m.right_faces += 2;
			}
	}
	else
	{
		// split seam: each plane has its own column of vertices at x=1 with identical positions but different UVs
		int s = n + 1;
		size_t plane_vertices = size_t(s) * s;

		m.vertices = plane_vertices * 2;
		m.positions.resize(m.vertices * 3);
		m.uvs.resize(m.vertices * 2);

		for (int side = 0; side < 2; ++side)
			for (int y = 0; y < s; ++y)
				for (int x = 0; x < s; ++x)
				{
					unsigned int id = unsigned(side * plane_vertices + y * s + x);
					m.positions[id * 3 + 0] = float(side * n + x) / float(n);
					m.positions[id * 3 + 1] = float(y) / float(n);
					m.positions[id * 3 + 2] = 0.f;
					// disjoint UV ranges guarantee the seam is a real attribute discontinuity
					m.uvs[id * 2 + 0] = float(side * 10 + x) / float(n);
					m.uvs[id * 2 + 1] = float(y) / float(n);
				}

		auto idLeft = [s](int x, int y) -> unsigned int { return unsigned(y * s + x); };
		auto idRight = [s, plane_vertices](int x, int y) -> unsigned int { return unsigned(plane_vertices + y * s + x); };

		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				pushQuad(m.indices, m.groups, idLeft(x, y), idLeft(x + 1, y), idLeft(x, y + 1), idLeft(x + 1, y + 1), kGroupLeft);
				m.left_faces += 2;
			}

		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				pushQuad(m.indices, m.groups, idRight(x, y), idRight(x + 1, y), idRight(x, y + 1), idRight(x + 1, y + 1), kGroupRight);
				m.right_faces += 2;
			}
	}

	assert(m.indices.size() == (m.left_faces + m.right_faces) * 3);
	return m;
}

// reorders faces so that faces of the two groups are interleaved instead of being stored as two contiguous runs
static void interleaveFaces(GroupMesh& m)
{
	size_t face_count = m.groups.size();

	std::vector<size_t> left_faces;
	std::vector<size_t> right_faces;

	for (size_t f = 0; f < face_count; ++f)
	{
		if (m.groups[f] == kGroupLeft)
			left_faces.push_back(f);
		else
			right_faces.push_back(f);
	}

	assert(!left_faces.empty() && !right_faces.empty());

	std::vector<size_t> order;
	order.reserve(face_count);

	size_t li = 0, ri = 0;
	while (li < left_faces.size() || ri < right_faces.size())
	{
		if (li < left_faces.size())
			order.push_back(left_faces[li++]);
		if (ri < right_faces.size())
			order.push_back(right_faces[ri++]);
	}

	assert(order.size() == face_count);

	std::vector<unsigned int> indices(m.indices.size());
	std::vector<unsigned int> groups(m.groups.size());

	for (size_t i = 0; i < face_count; ++i)
	{
		size_t f = order[i];

		indices[i * 3 + 0] = m.indices[f * 3 + 0];
		indices[i * 3 + 1] = m.indices[f * 3 + 1];
		indices[i * 3 + 2] = m.indices[f * 3 + 2];
		groups[i] = m.groups[f];
	}

	m.indices.swap(indices);
	m.groups.swap(groups);
}

// canonical id of each vertex based on exact position equality (positions are never updated by the simplifier)
static std::vector<unsigned int> positionCanonical(const GroupMesh& m)
{
	std::vector<unsigned int> canon(m.vertices);

	for (size_t i = 0; i < m.vertices; ++i)
	{
		canon[i] = unsigned(i);

		for (size_t j = 0; j < i; ++j)
			if (m.positions[i * 3 + 0] == m.positions[j * 3 + 0] &&
			    m.positions[i * 3 + 1] == m.positions[j * 3 + 1] &&
			    m.positions[i * 3 + 2] == m.positions[j * 3 + 2])
			{
				canon[i] = canon[j];
				break;
			}
	}

	return canon;
}

typedef std::pair<unsigned int, unsigned int> Edge;

template <typename T>
static std::set<Edge> collectEdges(const T* indices, size_t index_count, const std::vector<unsigned int>& canon)
{
	std::set<Edge> edges;

	for (size_t i = 0; i < index_count; i += 3)
	{
		unsigned int v[3] = {canon[indices[i + 0]], canon[indices[i + 1]], canon[indices[i + 2]]};

		for (int e = 0; e < 3; ++e)
		{
			unsigned int a = v[e];
			unsigned int b = v[(e + 1) % 3];

			if (a != b)
				edges.insert(a < b ? Edge(a, b) : Edge(b, a));
		}
	}

	return edges;
}

// core geometric + label invariants shared by all tests
template <typename T>
static void checkGroupResult(const GroupMesh& m, const T* out, size_t out_count, const unsigned int* out_groups, const char* what)
{
	assert(out_count % 3 == 0);
	assert(out_count <= m.indices.size());

	for (size_t i = 0; i < out_count; ++i)
		assert(unsigned(out[i]) < m.vertices);

	std::vector<unsigned int> canon = positionCanonical(m);
	std::set<Edge> input_edges = collectEdges(m.indices.data(), m.indices.size(), canon);
	std::set<Edge> output_edges = collectEdges(out, out_count, canon);

	// every position on the shared boundary (x=1) must still be referenced by the output
	std::set<unsigned int> seam_positions;
	for (size_t i = 0; i < m.vertices; ++i)
		if (m.positions[i * 3 + 0] == 1.f)
			seam_positions.insert(canon[i]);

	std::set<unsigned int> used_positions;
	for (size_t i = 0; i < out_count; ++i)
		used_positions.insert(canon[out[i]]);

	for (unsigned int p : seam_positions)
	{
		assert(used_positions.count(p) != 0);
		(void)p;
	}

	// every geometric edge segment along the group boundary must survive unchanged
	for (int y = 0; y < m.n; ++y)
	{
		unsigned int p0 = ~0u, p1 = ~0u;

		for (size_t i = 0; i < m.vertices; ++i)
		{
			if (m.positions[i * 3 + 0] != 1.f)
				continue;

			float py = m.positions[i * 3 + 1];

			if (py == float(y) / float(m.n))
				p0 = canon[i];
			if (py == float(y + 1) / float(m.n))
				p1 = canon[i];
		}

		assert(p0 != ~0u && p1 != ~0u);
		Edge seam_edge = p0 < p1 ? Edge(p0, p1) : Edge(p1, p0);
		assert(input_edges.count(seam_edge) != 0);
		assert(output_edges.count(seam_edge) != 0);
	}

	// both groups must survive, and both sides must have genuinely lost triangles
	size_t counts[2] = {0, 0};

	for (size_t i = 0; i < out_count / 3; ++i)
	{
		unsigned int group = out_groups[i];
		assert(group == kGroupLeft || group == kGroupRight);

		size_t slot = group == kGroupLeft ? 0 : 1;
		counts[slot]++;

		// label must match the region the triangle vertices live in; labels can't be re-guessed from positions
		for (int k = 0; k < 3; ++k)
		{
			unsigned int v = out[i * 3 + k];
			float x = m.positions[v * 3 + 0];

			if (group == kGroupLeft)
				assert(x <= 1.f);
			else
				assert(x >= 1.f);
		}
	}

	assert(counts[0] > 0 && counts[1] > 0);
	assert(counts[0] < m.left_faces);
	assert(counts[1] < m.right_faces);

	printf("%-32s: %u -> %u triangles (left %u -> %u, right %u -> %u)\n", what,
	    unsigned((m.left_faces + m.right_faces)), unsigned(out_count / 3),
	    unsigned(m.left_faces), unsigned(counts[0]), unsigned(m.right_faces), unsigned(counts[1]));
}

static void testTwoPlanesBoundary()
{
	GroupMesh m = buildPlanePair(8, /* split_seam= */ false);

	std::vector<unsigned int> out(m.indices.size());
	std::vector<unsigned int> out_groups(m.groups.size());
	float error = 0.f;

	size_t count = meshopt_simplifyWithGroups(out.data(), out_groups.data(), m.indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    m.indices.size() / 4, FLT_MAX, 0, &error);

	assert(count > 0);
	assert(error >= 0.f);

	checkGroupResult(m, out.data(), count, out_groups.data(), "two planes, shared edge");
}

static void testUvSeam()
{
	GroupMesh m = buildPlanePair(8, /* split_seam= */ true);

	std::vector<unsigned int> out(m.indices.size());
	std::vector<unsigned int> out_groups(m.groups.size());

	const float attr_weights[2] = {10.f, 10.f};

	// permissive mode is allowed across attribute seams; group boundary locks must still hold
	size_t count = meshopt_simplifyWithGroups(out.data(), out_groups.data(), m.indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, m.uvs.data(), sizeof(float) * 2, attr_weights, 2, NULL,
	    m.indices.size() / 4, FLT_MAX, meshopt_SimplifyPermissive, NULL);

	assert(count > 0);
	checkGroupResult(m, out.data(), count, out_groups.data(), "split UV seam");

	// both split copies of every boundary vertex (one per plane) must remain in use because the boundary can't move
	std::set<unsigned int> used(out.begin(), out.begin() + count);

	size_t seam_copies = 0;
	for (size_t i = 0; i < m.vertices; ++i)
		if (m.positions[i * 3 + 0] == 1.f)
		{
			assert(used.count(unsigned(i)) != 0);
			seam_copies++;
		}

	// the seam column is duplicated, so there must be two wedges per boundary position
	assert(seam_copies == size_t(2) * (m.n + 1));
}

static void testInterleavedFaceOrder()
{
	GroupMesh m = buildPlanePair(8, /* split_seam= */ false);
	interleaveFaces(m);

	// make sure the input really is interleaved
	assert(m.groups[0] != m.groups[1] || m.groups[1] != m.groups[2]);

	std::vector<unsigned int> out(m.indices.size());
	std::vector<unsigned int> out_groups(m.groups.size());

	size_t count = meshopt_simplifyWithGroups(out.data(), out_groups.data(), m.indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    m.indices.size() / 4, FLT_MAX, 0, NULL);

	checkGroupResult(m, out.data(), count, out_groups.data(), "interleaved face order");
}

static void testSingleGroupParity()
{
	GroupMesh m = buildPlanePair(8, /* split_seam= */ false);

	// relabel every face with the same group; group simplification must then exactly match meshopt_simplifyWithAttributes
	std::fill(m.groups.begin(), m.groups.end(), kGroupLeft);

	const float attr_weights[2] = {10.f, 10.f};
	size_t target = m.indices.size() / 4;

	std::vector<unsigned int> ref(m.indices.size());
	std::vector<unsigned int> grp(m.indices.size());
	std::vector<unsigned int> grp_groups(m.groups.size());

	float ref_error = 0.f;
	float grp_error = 0.f;

	size_t ref_count = meshopt_simplifyWithAttributes(ref.data(), m.indices.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, m.uvs.data(), sizeof(float) * 2, attr_weights, 2, NULL,
	    target, FLT_MAX, 0, &ref_error);

	size_t grp_count = meshopt_simplifyWithGroups(grp.data(), grp_groups.data(), m.indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, m.uvs.data(), sizeof(float) * 2, attr_weights, 2, NULL,
	    target, FLT_MAX, 0, &grp_error);

	assert(grp_count == ref_count);
	assert(memcmp(grp.data(), ref.data(), grp_count * sizeof(unsigned int)) == 0);
	assert(grp_error == ref_error);

	// every surviving triangle keeps the only input label
	for (size_t i = 0; i < grp_count / 3; ++i)
		assert(grp_groups[i] == kGroupLeft);

	printf("%-32s: %u triangles, error %e matches meshopt_simplifyWithAttributes\n", "single group parity", unsigned(grp_count / 3), grp_error);
}

static void testSparse()
{
	GroupMesh base = buildPlanePair(8, /* split_seam= */ false);

	const size_t kPadding = 5;

	GroupMesh m = base;
	m.vertices = base.vertices + kPadding;
	m.positions.assign(m.vertices * 3, 0.f);
	m.uvs.assign(m.vertices * 2, 0.f);

	for (size_t i = 0; i < base.vertices; ++i)
	{
		m.positions[(i + kPadding) * 3 + 0] = base.positions[i * 3 + 0];
		m.positions[(i + kPadding) * 3 + 1] = base.positions[i * 3 + 1];
		m.positions[(i + kPadding) * 3 + 2] = base.positions[i * 3 + 2];
		m.uvs[(i + kPadding) * 2 + 0] = base.uvs[i * 2 + 0];
		m.uvs[(i + kPadding) * 2 + 1] = base.uvs[i * 2 + 1];
	}

	for (size_t i = 0; i < m.indices.size(); ++i)
		m.indices[i] += unsigned(kPadding);

	std::vector<unsigned int> out(m.indices.size());
	std::vector<unsigned int> out_groups(m.groups.size());
	float error = 0.f;

	// combine all four supported options at once to verify option composition
	unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | meshopt_SimplifyLockBorder | meshopt_SimplifyPermissive;
	size_t count = meshopt_simplifyWithGroups(out.data(), out_groups.data(), m.indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    m.indices.size() / 4, 0.25f, options, &error);

	assert(count > 0);
	checkGroupResult(m, out.data(), count, out_groups.data(), "sparse indices");

	// output indices stay in the original (sparse) vertex space
	for (size_t i = 0; i < count; ++i)
		assert(out[i] >= kPadding && out[i] < m.vertices);
}

static void testInPlaceCompaction()
{
	GroupMesh m = buildPlanePair(8, /* split_seam= */ false);
	interleaveFaces(m);

	const size_t target = m.indices.size() / 4;

	// reference run using separate output buffers
	std::vector<unsigned int> ref(m.indices.size());
	std::vector<unsigned int> ref_groups(m.groups.size());
	float ref_error = 0.f;

	size_t ref_count = meshopt_simplifyWithGroups(ref.data(), ref_groups.data(), m.indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    target, FLT_MAX, 0, &ref_error);

	// full aliasing: both indices and groups are compacted in place at the same time
	std::vector<unsigned int> inout = m.indices;
	std::vector<unsigned int> inout_groups = m.groups;
	float inout_error = 0.f;

	size_t inout_count = meshopt_simplifyWithGroups(inout.data(), inout_groups.data(), inout.data(), inout_groups.data(), inout.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    target, FLT_MAX, 0, &inout_error);

	assert(inout_count == ref_count);
	assert(memcmp(inout.data(), ref.data(), ref_count * sizeof(unsigned int)) == 0);
	assert(memcmp(inout_groups.data(), ref_groups.data(), (ref_count / 3) * sizeof(unsigned int)) == 0);
	assert(inout_error == ref_error);

	checkGroupResult(m, inout.data(), inout_count, inout_groups.data(), "in-place index+group compaction");

	// groups-only aliasing: indices use separate buffers but groups compact in place
	std::vector<unsigned int> indices = m.indices;
	std::vector<unsigned int> groups_alias = m.groups;
	std::vector<unsigned int> out(m.indices.size());

	size_t alias_count = meshopt_simplifyWithGroups(out.data(), groups_alias.data(), indices.data(), groups_alias.data(), indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    target, FLT_MAX, 0, NULL);

	assert(alias_count == ref_count);
	assert(memcmp(out.data(), ref.data(), ref_count * sizeof(unsigned int)) == 0);
	assert(memcmp(groups_alias.data(), ref_groups.data(), (ref_count / 3) * sizeof(unsigned int)) == 0);
}

static void testEmptyMesh()
{
	unsigned int index_buffer[1] = {0};
	unsigned int group_buffer[1] = {0};

	size_t count = meshopt_simplifyWithGroups(index_buffer, group_buffer, index_buffer, group_buffer, 0,
	    NULL, 0, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    0, FLT_MAX, 0, NULL);

	assert(count == 0);
	printf("%-32s: ok\n", "empty mesh");
}

// verifies boundary geometry and labels without requiring any reduction
template <typename T>
static void checkBoundaryAndLabels(const GroupMesh& m, const T* out, size_t out_count, const unsigned int* out_groups, const char* what)
{
	assert(out_count % 3 == 0);
	assert(out_count <= m.indices.size());

	std::vector<unsigned int> canon = positionCanonical(m);
	std::set<Edge> input_edges = collectEdges(m.indices.data(), m.indices.size(), canon);
	std::set<Edge> output_edges = collectEdges(out, out_count, canon);

	std::set<unsigned int> used_positions;
	for (size_t i = 0; i < out_count; ++i)
		used_positions.insert(canon[out[i]]);

	for (size_t i = 0; i < m.vertices; ++i)
		if (m.positions[i * 3 + 0] == 1.f)
			assert(used_positions.count(canon[i]) != 0);

	for (int y = 0; y < m.n; ++y)
	{
		unsigned int p0 = ~0u, p1 = ~0u;

		for (size_t i = 0; i < m.vertices; ++i)
		{
			if (m.positions[i * 3 + 0] != 1.f)
				continue;

			float py = m.positions[i * 3 + 1];

			if (py == float(y) / float(m.n))
				p0 = canon[i];
			if (py == float(y + 1) / float(m.n))
				p1 = canon[i];
		}

		assert(p0 != ~0u && p1 != ~0u);
		Edge seam_edge = p0 < p1 ? Edge(p0, p1) : Edge(p1, p0);
		assert(input_edges.count(seam_edge) != 0);
		assert(output_edges.count(seam_edge) != 0);
	}

	for (size_t i = 0; i < out_count / 3; ++i)
	{
		unsigned int group = out_groups[i];
		assert(group == kGroupLeft || group == kGroupRight);

		for (int k = 0; k < 3; ++k)
		{
			unsigned int v = out[i * 3 + k];
			float x = m.positions[v * 3 + 0];

			if (group == kGroupLeft)
				assert(x <= 1.f);
			else
				assert(x >= 1.f);
		}
	}

	printf("%-32s: %u triangles retained, boundary and labels intact\n", what, unsigned(out_count / 3));
}

static void testLocksPreventTarget()
{
	GroupMesh m = buildPlanePair(8, /* split_seam= */ false);

	std::vector<unsigned char> locks(m.vertices, 0);
	for (size_t i = 0; i < m.vertices; ++i)
		locks[i] = meshopt_SimplifyVertex_Lock;

	std::vector<unsigned int> out(m.indices.size());
	std::vector<unsigned int> out_groups(m.groups.size());

	// ask for an aggressive target that an unlocked mesh reaches easily; locking every vertex makes it unreachable
	size_t count = meshopt_simplifyWithGroups(out.data(), out_groups.data(), m.indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, locks.data(),
	    3, FLT_MAX, 0, NULL);

	assert(count == m.indices.size());
	assert(memcmp(out.data(), m.indices.data(), count * sizeof(unsigned int)) == 0);
	assert(memcmp(out_groups.data(), m.groups.data(), m.groups.size() * sizeof(unsigned int)) == 0);

	checkBoundaryAndLabels(m, out.data(), count, out_groups.data(), "fully locked mesh");

	// locking the exterior column makes the aggressive target unreachable, but the mesh interiors still reduce
	GroupMesh partial = buildPlanePair(8, /* split_seam= */ false);

	std::vector<unsigned char> partial_locks(partial.vertices, 0);
	for (size_t i = 0; i < partial.vertices; ++i)
		if (partial.positions[i * 3 + 0] == 0.f)
			partial_locks[i] = meshopt_SimplifyVertex_Lock;

	std::vector<unsigned int> partial_out(partial.indices.size());
	std::vector<unsigned int> partial_groups(partial.groups.size());

	size_t partial_count = meshopt_simplifyWithGroups(partial_out.data(), partial_groups.data(), partial.indices.data(), partial.groups.data(), partial.indices.size(),
	    partial.positions.data(), partial.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, partial_locks.data(),
	    3, FLT_MAX, 0, NULL);

	assert(partial_count > 3);
	checkGroupResult(partial, partial_out.data(), partial_count, partial_groups.data(), "partially locked mesh");
	checkBoundaryAndLabels(partial, partial_out.data(), partial_count, partial_groups.data(), "partially locked mesh");
}

static void testIntegerWrappers()
{
	// unsigned short indices through the C++ template wrapper
	GroupMesh m = buildPlanePair(8, /* split_seam= */ false);
	assert(m.vertices < 65536);

	std::vector<unsigned short> short_indices(m.indices.size());
	for (size_t i = 0; i < m.indices.size(); ++i)
		short_indices[i] = (unsigned short)(m.indices[i]);

	std::vector<unsigned short> out(m.indices.size());
	std::vector<unsigned int> out_groups(m.groups.size());

	size_t count = meshopt_simplifyWithGroups(out.data(), out_groups.data(), short_indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    m.indices.size() / 4, FLT_MAX, 0, NULL);

	assert(count > 0);
	checkGroupResult(m, out.data(), count, out_groups.data(), "unsigned short wrapper");

	// unsigned char indices on a small mesh
	GroupMesh small = buildPlanePair(2, /* split_seam= */ false);
	assert(small.vertices < 256);

	std::vector<unsigned char> byte_indices(small.indices.size());
	for (size_t i = 0; i < small.indices.size(); ++i)
		byte_indices[i] = (unsigned char)(small.indices[i]);

	std::vector<unsigned char> byte_out(small.indices.size());
	std::vector<unsigned int> byte_groups(small.groups.size());

	size_t byte_count = meshopt_simplifyWithGroups(byte_out.data(), byte_groups.data(), byte_indices.data(), small.groups.data(), small.indices.size(),
	    small.positions.data(), small.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    6, FLT_MAX, 0, NULL);

	assert(byte_count > 0);
	checkGroupResult(small, byte_out.data(), byte_count, byte_groups.data(), "unsigned char wrapper");
}

static void testSpecialLabelValues()
{
	GroupMesh m = buildPlanePair(6, /* split_seam= */ false);

	// 0 and UINT_MAX are valid opaque labels and must not be treated as missing values or float attributes
	for (size_t i = 0; i < m.groups.size(); ++i)
		m.groups[i] = (m.groups[i] == kGroupLeft) ? 0u : UINT_MAX;

	std::vector<unsigned int> out(m.indices.size());
	std::vector<unsigned int> out_groups(m.groups.size());

	size_t count = meshopt_simplifyWithGroups(out.data(), out_groups.data(), m.indices.data(), m.groups.data(), m.indices.size(),
	    m.positions.data(), m.vertices, sizeof(float) * 3, NULL, 0, NULL, 0, NULL,
	    m.indices.size() / 4, FLT_MAX, 0, NULL);

	assert(count > 0);

	bool has_zero = false;
	bool has_max = false;

	for (size_t i = 0; i < count / 3; ++i)
	{
		assert(out_groups[i] == 0 || out_groups[i] == UINT_MAX);
		has_zero |= out_groups[i] == 0;
		has_max |= out_groups[i] == UINT_MAX;

		for (int k = 0; k < 3; ++k)
		{
			float x = m.positions[out[i * 3 + k] * 3 + 0];

			if (out_groups[i] == 0)
				assert(x <= 1.f);
			else
				assert(x >= 1.f);
		}
	}

	assert(has_zero && has_max);

	std::vector<unsigned int> canon = positionCanonical(m);
	std::set<Edge> output_edges = collectEdges(out.data(), count, canon);

	for (int y = 0; y < m.n; ++y)
	{
		unsigned int p0 = ~0u, p1 = ~0u;

		for (size_t i = 0; i < m.vertices; ++i)
		{
			if (m.positions[i * 3 + 0] != 1.f)
				continue;

			float py = m.positions[i * 3 + 1];

			if (py == float(y) / float(m.n))
				p0 = canon[i];
			if (py == float(y + 1) / float(m.n))
				p1 = canon[i];
		}

		assert(p0 != ~0u && p1 != ~0u);
		Edge seam_edge = p0 < p1 ? Edge(p0, p1) : Edge(p1, p0);
		assert(output_edges.count(seam_edge) != 0);
	}

	printf("%-32s: labels 0 and UINT_MAX preserved, boundary intact\n", "special label values");
}

} // namespace

int main()
{
	testTwoPlanesBoundary();
	testUvSeam();
	testInterleavedFaceOrder();
	testSingleGroupParity();
	testSparse();
	testInPlaceCompaction();
	testEmptyMesh();
	testLocksPreventTarget();
	testIntegerWrappers();
	testSpecialLabelValues();

	printf("group simplification PASS\n");
	return 0;
}
