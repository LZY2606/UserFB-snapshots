// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
//
// Continuous self-intersection guard for edge collapse simplification.
//
// During a collapse the source geometric vertex (all of its position wedges) travels along a straight
// line to the target. Each moving triangle with fixed vertices A,B sweeps conv(A,B,P0,P1): a tetrahedron.
// A continuous collision query over t in [0,1] is therefore a static triangle-vs-tetrahedron test,
// implemented with the separating axis theorem (1 triangle normal + 4 tetrahedron face normals +
// 3*6 edge cross products = 23 candidate axes). No time sampling is used, and the interval is closed:
// point/edge contacts block the collapse.
//
// Alive triangles are kept in a binary BVH with axis-aligned bounds. Accepted collapses mutate only the
// leaves they touch (removal, vertex replacement, refit up the parent chain); a full rebuild is triggered
// once dead leaves reach the number of alive leaves, which keeps total rebuild work linear in input size.
#include "collision.h"

#include <algorithm>
#include <assert.h>
#include <float.h>
#include <math.h>
#include <string.h>

namespace meshopt
{

// Process-wide counters; guarded simplification calls accumulate into these until explicitly reset.
CollisionCounters gCollisionCounters = {};

// Must match the definition in simplifier.cpp; collision.h only forward-declares the type.
struct Vector3
{
	float x, y, z;
};

namespace
{

struct V3d
{
	double x, y, z;
};

static V3d vd(const Vector3& v)
{
	V3d r = {double(v.x), double(v.y), double(v.z)};
	return r;
}

static V3d vsub(const V3d& a, const V3d& b)
{
	V3d r = {a.x - b.x, a.y - b.y, a.z - b.z};
	return r;
}

static V3d vcross(const V3d& a, const V3d& b)
{
	V3d r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	return r;
}

static double vdot(const V3d& a, const V3d& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double vlen2(const V3d& a)
{
	return vdot(a, a);
}

struct Projection
{
	double mn, mx;
};

static Projection projectPoints(const V3d* points, int count, const V3d& axis)
{
	Projection r = {FLT_MAX, -FLT_MAX};

	for (int i = 0; i < count; ++i)
	{
		double p = vdot(points[i], axis);
		r.mn = r.mn < p ? r.mn : p;
		r.mx = r.mx > p ? r.mx : p;
	}

	return r;
}

// SAT tolerance in normalized simplifier units (mesh extents map to 1). The guard may conservatively
// reject numerically ambiguous candidates, but it must never miss a clear penetration.
static double axisEpsilon(const V3d* points, int count)
{
	double maxabs = 0.0;

	for (int i = 0; i < count; ++i)
	{
		double a = fabs(points[i].x);
		double b = fabs(points[i].y);
		double c = fabs(points[i].z);
		maxabs = maxabs > a ? maxabs : a;
		maxabs = maxabs > b ? maxabs : b;
		maxabs = maxabs > c ? maxabs : c;
	}

	return 1e-7 + 1e-9 * maxabs;
}

// Returns true when triangle T intersects tetrahedron S, contact included (closed interval semantics).
static bool triangleTetrahedronIntersect(const V3d t[3], const V3d s[4])
{
	V3d points[7];
	for (int i = 0; i < 3; ++i)
		points[i] = t[i];
	for (int i = 0; i < 4; ++i)
		points[3 + i] = s[i];

	double eps = axisEpsilon(points, 7);
	bool skipped = false;

	V3d tedge[3] = {vsub(t[1], t[0]), vsub(t[2], t[1]), vsub(t[0], t[2])};
	V3d sedge[6] = {
	    vsub(s[1], s[0]), vsub(s[2], s[0]), vsub(s[3], s[0]),
	    vsub(s[2], s[1]), vsub(s[3], s[1]), vsub(s[3], s[2])};

	// face normal of the triangle
	{
		V3d n = vcross(tedge[0], tedge[1]);
		double l2 = vlen2(n);

		if (l2 <= 1e-24)
		{
			skipped = true;
		}
		else
		{
			double inv = 1.0 / sqrt(l2);
			V3d axis = {n.x * inv, n.y * inv, n.z * inv};
			Projection pa = projectPoints(t, 3, axis);
			Projection pb = projectPoints(s, 4, axis);
			if (pa.mn > pb.mx + eps || pb.mn > pa.mx + eps)
				return false;
		}
	}

	// face normals of the tetrahedron
	static const int tfaces[4][3] = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};

	for (int f = 0; f < 4; ++f)
	{
		V3d e1 = vsub(s[tfaces[f][1]], s[tfaces[f][0]]);
		V3d e2 = vsub(s[tfaces[f][2]], s[tfaces[f][0]]);
		V3d n = vcross(e1, e2);
		double l2 = vlen2(n);

		if (l2 <= 1e-24)
		{
			skipped = true;
			continue;
		}

		double inv = 1.0 / sqrt(l2);
		V3d axis = {n.x * inv, n.y * inv, n.z * inv};
		Projection pa = projectPoints(t, 3, axis);
		Projection pb = projectPoints(s, 4, axis);
		if (pa.mn > pb.mx + eps || pb.mn > pa.mx + eps)
			return false;
	}

	// edge cross products: 3 triangle edges against 6 tetrahedron edges
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 6; ++j)
		{
			V3d n = vcross(tedge[i], sedge[j]);
			double l2 = vlen2(n);

			if (l2 <= 1e-24)
			{
				// parallel (or zero-length) edges: this axis carries no information
				skipped = true;
				continue;
			}

			double inv = 1.0 / sqrt(l2);
			V3d axis = {n.x * inv, n.y * inv, n.z * inv};
			Projection pa = projectPoints(t, 3, axis);
			Projection pb = projectPoints(s, 4, axis);
			if (pa.mn > pb.mx + eps || pb.mn > pa.mx + eps)
				return false;
		}

	// In-plane axes: when the swept tetrahedron is flat (the move stays in the triangle's plane), every
	// edge cross product above is parallel to the triangle normal and cannot separate two coplanar
	// polygons. Edge normals inside the plane cover that degenerate configuration; for non-degenerate
	// configurations these axes are redundant (overlapping convex sets overlap on every axis).
	{
		V3d n = vcross(tedge[0], tedge[1]);
		double nl2 = vlen2(n);

		if (nl2 > 1e-24)
		{
			double ninv = 1.0 / sqrt(nl2);
			V3d nrm = {n.x * ninv, n.y * ninv, n.z * ninv};

			for (int j = 0; j < 6; ++j)
			{
				V3d ax = vcross(nrm, sedge[j]);
				double l2 = vlen2(ax);
				if (l2 <= 1e-24)
					continue;

				double inv = 1.0 / sqrt(l2);
				V3d axis = {ax.x * inv, ax.y * inv, ax.z * inv};
				Projection pa = projectPoints(t, 3, axis);
				Projection pb = projectPoints(s, 4, axis);
				if (pa.mn > pb.mx + eps || pb.mn > pa.mx + eps)
					return false;
			}

			for (int i = 0; i < 3; ++i)
			{
				V3d ax = vcross(nrm, tedge[i]);
				double l2 = vlen2(ax);
				if (l2 <= 1e-24)
					continue;

				double inv = 1.0 / sqrt(l2);
				V3d axis = {ax.x * inv, ax.y * inv, ax.z * inv};
				Projection pa = projectPoints(t, 3, axis);
				Projection pb = projectPoints(s, 4, axis);
				if (pa.mn > pb.mx + eps || pb.mn > pa.mx + eps)
					return false;
			}
		}
		else
		{
			skipped = true;
		}
	}

	// No separating axis: the volumes overlap. If any axis was numerically undefined, the configuration
	// is ambiguous, which is also treated as an intersection (conservative rejection).
	(void)skipped;
	return true;
}

struct Face
{
	unsigned int v[3];
	unsigned char alive;
};

struct Node
{
	float bmin[3], bmax[3];
	int left, right; // children for internal nodes
	int parent;      // parent node index
	int face;        // face index for leaves, -1 for internal nodes
};

struct CollisionWorldImpl
{
	const Vector3* positions;
	const unsigned int* remap;
	const unsigned int* wedge;
	size_t vertex_count;

	Face* faces;
	size_t face_capacity; // allocation length; face ids never change
	size_t face_count;    // alive faces
	size_t dead_count;    // dead faces since the last full rebuild

	Node* nodes;
	size_t node_count;
	int* leaf_of;
	unsigned int* build_ids;

	// per-vertex doubly linked incidence lists: corner = face*3 + k.
	// replacements unlink the corner from the old vertex and link it onto the new one, so every
	// corner reachable from head[v] is genuinely owned by v; no stale links are ever traversed.
	unsigned int* head;
	unsigned int* clnext;
	unsigned int* clprev;

	int* mark;
	int mark_stamp;

	void* scratch; // single backing allocation for all arrays above

	CollisionCounters* counters;
};

static void emptyBounds(float bmin[3], float bmax[3])
{
	bmin[0] = bmin[1] = bmin[2] = FLT_MAX;
	bmax[0] = bmax[1] = bmax[2] = -FLT_MAX;
}

static void unionPoint(float bmin[3], float bmax[3], const float p[3])
{
	for (int k = 0; k < 3; ++k)
	{
		bmin[k] = bmin[k] < p[k] ? bmin[k] : p[k];
		bmax[k] = bmax[k] > p[k] ? bmax[k] : p[k];
	}
}

static void unionNodeBounds(float bmin[3], float bmax[3], const Node& n)
{
	for (int k = 0; k < 3; ++k)
	{
		bmin[k] = bmin[k] < n.bmin[k] ? bmin[k] : n.bmin[k];
		bmax[k] = bmax[k] > n.bmax[k] ? bmax[k] : n.bmax[k];
	}
}

static void computeFaceBounds(CollisionWorldImpl* w, int f, float bmin[3], float bmax[3])
{
	emptyBounds(bmin, bmax);

	const Face& face = w->faces[f];
	for (int k = 0; k < 3; ++k)
	{
		const Vector3& p = w->positions[face.v[k]];
		float pv[3] = {p.x, p.y, p.z};
		unionPoint(bmin, bmax, pv);
	}
}

struct CentroidCmp
{
	int axis;
	const CollisionWorldImpl* w;

	bool operator()(unsigned int a, unsigned int b) const
	{
		const Face& fa = w->faces[a];
		const Face& fb = w->faces[b];
		float ca = 0.f, cb = 0.f;
		for (int k = 0; k < 3; ++k)
		{
			ca += (&w->positions[fa.v[k]].x)[axis];
			cb += (&w->positions[fb.v[k]].x)[axis];
		}
		if (ca != cb)
			return ca < cb;
		return a < b;
	}
};

static int buildRecursive(CollisionWorldImpl* w, unsigned int* ids, size_t count, int parent)
{
	int node = int(w->node_count++);
	Node& n = w->nodes[node];
	n.parent = parent;
	n.left = n.right = -1;
	emptyBounds(n.bmin, n.bmax);

	if (count == 1)
	{
		n.face = int(ids[0]);
		w->leaf_of[ids[0]] = node;
		computeFaceBounds(w, int(ids[0]), n.bmin, n.bmax);
		return node;
	}

	n.face = -1;

	// pick the axis with the largest centroid spread
	float cmin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
	float cmax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

	for (size_t i = 0; i < count; ++i)
	{
		const Face& f = w->faces[ids[i]];
		for (int k = 0; k < 3; ++k)
		{
			const float* p0 = &w->positions[f.v[0]].x;
			const float* p1 = &w->positions[f.v[1]].x;
			const float* p2 = &w->positions[f.v[2]].x;
			float c = (p0[k] + p1[k] + p2[k]) * (1.f / 3.f);
			cmin[k] = cmin[k] < c ? cmin[k] : c;
			cmax[k] = cmax[k] > c ? cmax[k] : c;
		}
	}

	int axis = 0;
	if (cmax[1] - cmin[1] > cmax[axis] - cmin[axis])
		axis = 1;
	if (cmax[2] - cmin[2] > cmax[axis] - cmin[axis])
		axis = 2;

	CentroidCmp cmp = {axis, w};
	size_t mid = count / 2;

	// nth_element partitions around the median centroid and handles tied centroids without stalling
	std::nth_element(ids, ids + mid, ids + count, cmp);

	n.left = buildRecursive(w, ids, mid, node);
	n.right = buildRecursive(w, ids + mid, count - mid, node);

	unionNodeBounds(n.bmin, n.bmax, w->nodes[n.left]);
	unionNodeBounds(n.bmin, n.bmax, w->nodes[n.right]);
	return node;
}

static void fillIncidence(CollisionWorldImpl* w)
{
	for (size_t v = 0; v < w->vertex_count; ++v)
		w->head[v] = ~0u;

	for (size_t f = 0; f < w->face_capacity; ++f)
	{
		if (!w->faces[f].alive)
			continue;

		for (int k = 0; k < 3; ++k)
		{
			unsigned int corner = unsigned(f * 3 + k);
			unsigned int v = w->faces[f].v[k];

			// link corner at the head of vertex v's list
			unsigned int old = w->head[v];
			w->clnext[corner] = old;
			w->clprev[corner] = ~0u;
			if (old != ~0u)
				w->clprev[old] = corner;
			w->head[v] = corner;
		}
	}
}

// removes a corner from whatever vertex list it currently belongs to
static void unlinkCorner(CollisionWorldImpl* w, unsigned int corner)
{
	unsigned int nx = w->clnext[corner];
	unsigned int pv = w->clprev[corner];

	if (nx != ~0u)
		w->clprev[nx] = pv;

	if (pv != ~0u)
		w->clnext[pv] = nx;
	else
	{
		// corner was a list head; the head is stored per vertex but recoverable through the face
		unsigned int f = corner / 3;
		unsigned int k = corner % 3;
		w->head[w->faces[f].v[k]] = nx;
	}

	w->clnext[corner] = ~0u;
	w->clprev[corner] = ~0u;
}

// links an already-unlinked corner onto the front of vertex v's list
static void linkCorner(CollisionWorldImpl* w, unsigned int corner, unsigned int v)
{
	unsigned int old = w->head[v];
	w->clnext[corner] = old;
	w->clprev[corner] = ~0u;
	if (old != ~0u)
		w->clprev[old] = corner;
	w->head[v] = corner;
}

static int nextStamp(CollisionWorldImpl* w)
{
	int stamp = ++w->mark_stamp;
	if (stamp == 0)
	{
		memset(w->mark, 0, w->face_capacity * sizeof(int));
		w->mark_stamp = 1;
		stamp = 1;
	}
	return stamp;
}

static void rebuildTree(CollisionWorldImpl* w)
{
	size_t count = 0;
	for (size_t f = 0; f < w->face_capacity; ++f)
		if (w->faces[f].alive)
			w->build_ids[count++] = unsigned(f);

	if (count == 0)
	{
		w->node_count = 0;
		w->face_count = 0;
		w->dead_count = 0;
		return;
	}

	w->node_count = 0;
	buildRecursive(w, w->build_ids, count, -1);
	fillIncidence(w);

	w->face_count = count;
	w->dead_count = 0;
	w->counters->index_rebuilds++;
}

static void refitPath(CollisionWorldImpl* w, int node)
{
	while (node >= 0)
	{
		Node& n = w->nodes[node];

		if (n.face >= 0)
		{
			if (w->faces[n.face].alive)
				computeFaceBounds(w, n.face, n.bmin, n.bmax);
			else
				emptyBounds(n.bmin, n.bmax);
		}
		else
		{
			emptyBounds(n.bmin, n.bmax);
			unionNodeBounds(n.bmin, n.bmax, w->nodes[n.left]);
			unionNodeBounds(n.bmin, n.bmax, w->nodes[n.right]);
		}

		node = n.parent;
	}
}

static bool boxOverlap(const float amin[3], const float amax[3], const float bmin[3], const float bmax[3])
{
	for (int k = 0; k < 3; ++k)
		if (amin[k] > bmax[k] || bmin[k] > amax[k])
			return false;

	return true;
}

// Validates that the corner still references geometric identity r (head chains may contain stale links).
static bool cornerAtIdentity(const CollisionWorldImpl* w, unsigned int corner, unsigned int r)
{
	unsigned int f = corner / 3;
	if (!w->faces[f].alive)
		return false;

	unsigned int k = corner % 3;
	return w->remap[w->faces[f].v[k]] == r;
}

struct Query
{
	CollisionWorldImpl* w;
	V3d tet[4];
	float qmin[3], qmax[3];
	int stamp;
	bool hit;
};

static void queryRecursive(Query& q, int node)
{
	CollisionWorldImpl* w = q.w;
	const Node& n = w->nodes[node];

	if (!boxOverlap(n.bmin, n.bmax, q.qmin, q.qmax))
		return;

	if (n.face >= 0)
	{
		int f = n.face;

		// marked faces share a geometric vertex (identity, through remap/wedge) with a moving face at
		// t=0: legal vertex/edge adjacency, including every position wedge on attribute seams; the
		// guard only protects against distant faces
		if (!w->faces[f].alive || w->mark[f] == q.stamp)
			return;

		const Face& face = w->faces[f];

		V3d tri[3];
		for (int k = 0; k < 3; ++k)
			tri[k] = vd(w->positions[face.v[k]]);

		w->counters->narrowphase++;

		if (triangleTetrahedronIntersect(tri, q.tet))
			q.hit = true;

		return;
	}

	queryRecursive(q, n.left);
	if (!q.hit)
		queryRecursive(q, n.right);
}

} // namespace

CollisionWorld* collisionCreate(const unsigned int* indices, size_t index_count, const Vector3* positions, size_t vertex_count,
                                const unsigned int* remap, const unsigned int* wedge, meshopt_Allocator& allocator,
                                CollisionCounters* counters)
{
	size_t face_count = index_count / 3;
	size_t faces_n = face_count ? face_count : 1;
	size_t nodes_n = face_count ? 2 * face_count : 1; // binary tree over m leaves: at most 2*m-1
	size_t verts_n = vertex_count ? vertex_count : 1;
	size_t corners_n = index_count ? index_count : 1;

	CollisionWorldImpl* w = allocator.allocate<CollisionWorldImpl>(1);
	memset(w, 0, sizeof(CollisionWorldImpl));

	w->positions = positions;
	w->remap = remap;
	w->wedge = wedge;
	w->vertex_count = vertex_count;
	w->counters = counters;
	w->face_capacity = face_count;
	w->face_count = face_count;
	w->dead_count = 0;

	// All arrays share one backing block; meshopt_Allocator only tracks a bounded number of LIFO blocks,
	// so the spatial index must not consume many of them. operator new returns max-aligned storage and
	// every sub-array is placed on a 16-byte boundary.
	const size_t align = 16;
	auto aligned_size = [](size_t bytes) { return (bytes + align - 1) & ~(align - 1); };

	size_t total = 0;
	total += aligned_size(faces_n * sizeof(Face));
	total += aligned_size(nodes_n * sizeof(Node));
	total += aligned_size(faces_n * sizeof(int));          // leaf_of
	total += aligned_size(faces_n * sizeof(unsigned int)); // build_ids
	total += aligned_size(verts_n * sizeof(unsigned int)); // head
	total += aligned_size(corners_n * sizeof(unsigned int)); // clnext
	total += aligned_size(corners_n * sizeof(unsigned int)); // clprev
	total += aligned_size(faces_n * sizeof(int));          // mark

	char* base = static_cast<char*>(allocator.allocate<char>(total));
	w->scratch = base;

	char* ptr = base;
	auto place = [&](size_t bytes) -> void* {
		void* p = ptr;
		ptr += aligned_size(bytes);
		return p;
	};

	w->faces = static_cast<Face*>(place(faces_n * sizeof(Face)));
	w->nodes = static_cast<Node*>(place(nodes_n * sizeof(Node)));
	w->leaf_of = static_cast<int*>(place(faces_n * sizeof(int)));
	w->build_ids = static_cast<unsigned int*>(place(faces_n * sizeof(unsigned int)));
	w->head = static_cast<unsigned int*>(place(verts_n * sizeof(unsigned int)));
	w->clnext = static_cast<unsigned int*>(place(corners_n * sizeof(unsigned int)));
	w->clprev = static_cast<unsigned int*>(place(corners_n * sizeof(unsigned int)));
	w->mark = static_cast<int*>(place(faces_n * sizeof(int)));
	assert(size_t(ptr - base) == total);

	for (size_t f = 0; f < face_count; ++f)
	{
		w->faces[f].v[0] = indices[f * 3 + 0];
		w->faces[f].v[1] = indices[f * 3 + 1];
		w->faces[f].v[2] = indices[f * 3 + 2];
		w->faces[f].alive = 1;
	}

	memset(w->mark, 0, faces_n * sizeof(int));
	w->mark_stamp = 0;

	for (size_t f = 0; f < face_count; ++f)
		w->build_ids[f] = unsigned(f);

	w->node_count = 0;
	if (face_count)
		buildRecursive(w, w->build_ids, face_count, -1);

	fillIncidence(w);
	return reinterpret_cast<CollisionWorld*>(w);
}

void collisionDestroy(CollisionWorld* world, meshopt_Allocator& allocator)
{
	if (!world)
		return;

	CollisionWorldImpl* w = reinterpret_cast<CollisionWorldImpl*>(world);

	allocator.deallocate(w->scratch);
	allocator.deallocate(w);
}

bool collisionTestMove(CollisionWorld* world, unsigned int r0, unsigned int r1, const Vector3& target)
{
	CollisionWorldImpl* w = reinterpret_cast<CollisionWorldImpl*>(world);

	w->counters->candidates++;

	if (w->node_count == 0)
		return true;

	int stamp = nextStamp(w);

	// marks every face incident to a wedge of geometric identity r as legal adjacency
	struct Marker
	{
		CollisionWorldImpl* w;
		int stamp;

		void mark(unsigned int r) const
		{
			unsigned int v = r;
			do
			{
				for (unsigned int corner = w->head[v]; corner != ~0u; corner = w->clnext[corner])
				{
					if (!cornerAtIdentity(w, corner, r))
						continue;

					w->mark[corner / 3] = stamp;
				}
				v = w->wedge[v];
			} while (v != r);
		}
	};

	Marker marker = {w, stamp};

	// faces incident to the target identity stay adjacent through the move
	marker.mark(r1);

	// mark every face sharing a geometric corner with a moving face (covers edge-ring neighbors)
	unsigned int v = r0;
	do
	{
		for (unsigned int corner = w->head[v]; corner != ~0u; corner = w->clnext[corner])
		{
			if (!cornerAtIdentity(w, corner, r0))
				continue;

			unsigned int fi = corner / 3;
			const Face& f = w->faces[fi];

			// mark all three corner identities even when this moving face was already marked through
			// another corner; otherwise its edge-ring neighbors sharing only the remaining corners
			// would be misclassified as distant faces
			for (int k = 0; k < 3; ++k)
				marker.mark(w->remap[f.v[k]]);
		}

		v = w->wedge[v];
	} while (v != r0);

	const V3d p1 = vd(target);

	// each moving face sweeps a tetrahedron conv(A,B,P0,P1)
	v = r0;
	do
	{
		for (unsigned int corner = w->head[v]; corner != ~0u; corner = w->clnext[corner])
		{
			if (!cornerAtIdentity(w, corner, r0))
				continue;

			unsigned int fi = corner / 3;

			const Face& f = w->faces[fi];

			// locate the source corner; the other two corners stay fixed
			const Vector3* p0;
			const Vector3* a;
			const Vector3* b;

			if (w->remap[f.v[0]] == r0)
			{
				p0 = &w->positions[f.v[0]];
				a = &w->positions[f.v[1]];
				b = &w->positions[f.v[2]];
			}
			else if (w->remap[f.v[1]] == r0)
			{
				p0 = &w->positions[f.v[1]];
				a = &w->positions[f.v[0]];
				b = &w->positions[f.v[2]];
			}
			else
			{
				p0 = &w->positions[f.v[2]];
				a = &w->positions[f.v[0]];
				b = &w->positions[f.v[1]];
			}

			Query q;
			q.w = w;
			q.stamp = stamp;
			q.hit = false;
			q.tet[0] = vd(*a);
			q.tet[1] = vd(*b);
			q.tet[2] = vd(*p0);
			q.tet[3] = p1;

			emptyBounds(q.qmin, q.qmax);
			for (int k = 0; k < 4; ++k)
			{
				float p[3] = {float(q.tet[k].x), float(q.tet[k].y), float(q.tet[k].z)};
				unionPoint(q.qmin, q.qmax, p);
			}

			queryRecursive(q, 0);
			if (q.hit)
				return false;
		}

		v = w->wedge[v];
	} while (v != r0);

	return true;
}

void collisionCommitCollapse(CollisionWorld* world, unsigned int r0, const unsigned int* collapse_remap)
{
	CollisionWorldImpl* w = reinterpret_cast<CollisionWorldImpl*>(world);

	int stamp = nextStamp(w);
	size_t touched = 0;
	size_t removed = 0;

	unsigned int v = r0;
	do
	{
		// walk the incidence list explicitly: unlinkCorner/linkCorner below rewrite the current corner's
		// clnext when it moves to another vertex, so the successor must be captured before any relink
		unsigned int corner = w->head[v];
		while (corner != ~0u)
		{
			unsigned int next_corner = w->clnext[corner];

			if (cornerAtIdentity(w, corner, r0))
			{
				unsigned int fi = corner / 3;
				if (w->mark[fi] != stamp)
				{
					w->mark[fi] = stamp;
					touched++;

					Face& f = w->faces[fi];
					unsigned int nv[3];
					for (int k = 0; k < 3; ++k)
						nv[k] = collapse_remap[f.v[k]];

					unsigned int rr0 = w->remap[nv[0]];
					unsigned int rr1 = w->remap[nv[1]];
					unsigned int rr2 = w->remap[nv[2]];

					if (rr0 == rr1 || rr0 == rr2 || rr1 == rr2)
					{
						// triangle collapses to an edge or point: alive through t in [0,1), removed at the end
						f.alive = 0;
						w->dead_count++;
						w->face_count--;
						removed++;
						refitPath(w, w->leaf_of[fi]);
					}
					else
					{
						for (int k = 0; k < 3; ++k)
						{
							unsigned int oldv = f.v[k];
							unsigned int newv = nv[k];

							if (oldv != newv)
							{
								// move the corner from the old vertex's list to the new one.
								// unlink first while f.v[k] still names oldv, since head recovery reads f.v[k]
								unsigned int c = fi * 3 + k;
								unlinkCorner(w, c);
								f.v[k] = newv;
								linkCorner(w, c, newv);
							}
						}

						refitPath(w, w->leaf_of[fi]);
					}
				}
			}

			corner = next_corner;
		}

		v = w->wedge[v];
	} while (v != r0);

	(void)removed;
	w->counters->local_updates += touched;
	w->counters->collapses++;

	// amortized full rebuild: once half of the leaves are dead, compact everything.
	// capacities then shrink as F, >=F/2, >=F/4, ... so total rebuild work stays O(initial faces).
	if (w->dead_count * 2 >= w->face_capacity)
		rebuildTree(w);
}

void collisionCommitMove(CollisionWorld* world, unsigned int r0)
{
	CollisionWorldImpl* w = reinterpret_cast<CollisionWorldImpl*>(world);

	int stamp = nextStamp(w);
	size_t touched = 0;

	unsigned int v = r0;
	do
	{
		for (unsigned int corner = w->head[v]; corner != ~0u; corner = w->clnext[corner])
		{
			if (!cornerAtIdentity(w, corner, r0))
				continue;

			unsigned int fi = corner / 3;
			if (w->mark[fi] == stamp)
				continue;

			w->mark[fi] = stamp;
			touched++;
			refitPath(w, w->leaf_of[fi]);
		}

		v = w->wedge[v];
	} while (v != r0);

	w->counters->local_updates += touched;
	w->counters->position_moves++;
}

} // namespace meshopt

extern "C" const meshopt_SimplifyCollisionCounters* meshopt_simplifyCollisionCounters(void)
{
	static meshopt_SimplifyCollisionCounters result;
	const meshopt::CollisionCounters& c = meshopt::gCollisionCounters;

	result.candidates = c.candidates;
	result.narrowphase_tests = c.narrowphase;
	result.local_updates = c.local_updates;
	result.collapses = c.collapses;
	result.position_moves = c.position_moves;
	result.collapse_rejected = c.collapse_rejected;
	result.position_rejected = c.position_rejected;
	result.index_rebuilds = c.index_rebuilds;

	return &result;
}

extern "C" void meshopt_simplifyResetCollisionCounters(void)
{
	meshopt::gCollisionCounters = meshopt::CollisionCounters();
}
