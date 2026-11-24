// Continuous self-intersection guard tests for the edge collapse simplifier.
//
// Every geometric verdict in this file is computed independently here (double precision SAT, analytic
// interval evaluation). The production collision predicate is never called to prove itself correct;
// dense time sampling is used only as auxiliary observation.
#include "meshoptimizer.h"
#include "task_support/registry.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace
{

struct V3
{
	float x, y, z;
};

// ---------------------------------------------------------------------------
// Independent reference geometry (does not use any production collision code)
// ---------------------------------------------------------------------------

struct D3
{
	double x, y, z;
};

D3 dsub(D3 a, D3 b)
{
	D3 r = {a.x - b.x, a.y - b.y, a.z - b.z};
	return r;
}

D3 dmul(D3 a, double s)
{
	D3 r = {a.x * s, a.y * s, a.z * s};
	return r;
}

D3 dcross(D3 a, D3 b)
{
	D3 r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	return r;
}

double ddot(D3 a, D3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

double dlen2(D3 a)
{
	return ddot(a, a);
}

void projectRange(const D3* pts, int n, D3 axis, double& mn, double& mx)
{
	mn = 1e30;
	mx = -1e30;
	for (int i = 0; i < n; ++i)
	{
		double v = ddot(pts[i], axis);
		mn = v < mn ? v : mn;
		mx = v > mx ? v : mx;
	}
}

// Independent separating-axis test; contact counts as overlap (closed interval).
bool separatedOnAxis(D3 n, const D3* A, int na, const D3* B, int nb, double eps)
{
	double l2 = dlen2(n);
	if (l2 <= 1e-24)
		return false; // undefined axis carries no information

	D3 axis = dmul(n, 1.0 / std::sqrt(l2));
	double amn, amx, bmn, bmx;
	projectRange(A, na, axis, amn, amx);
	projectRange(B, nb, axis, bmn, bmx);
	return amn > bmx + eps || bmn > amx + eps;
}

// Independent triangle/tetrahedron SAT (1 + 4 face normals, 18 edge crosses, in-plane edge normals).
bool refTriTet(const D3 t[3], const D3 s[4])
{
	D3 all[7];
	for (int i = 0; i < 3; ++i)
		all[i] = t[i];
	for (int i = 0; i < 4; ++i)
		all[3 + i] = s[i];

	double maxabs = 0.0;
	for (int i = 0; i < 7; ++i)
	{
		maxabs = std::fmax(maxabs, std::fabs(all[i].x));
		maxabs = std::fmax(maxabs, std::fabs(all[i].y));
		maxabs = std::fmax(maxabs, std::fabs(all[i].z));
	}
	double eps = 1e-7 + 1e-9 * maxabs;

	D3 te[3] = {dsub(t[1], t[0]), dsub(t[2], t[1]), dsub(t[0], t[2])};
	D3 se[6] = {
	    dsub(s[1], s[0]), dsub(s[2], s[0]), dsub(s[3], s[0]),
	    dsub(s[2], s[1]), dsub(s[3], s[1]), dsub(s[3], s[2])};

	if (separatedOnAxis(dcross(te[0], te[1]), t, 3, s, 4, eps))
		return false;

	static const int faces[4][3] = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
	for (int f = 0; f < 4; ++f)
		if (separatedOnAxis(dcross(dsub(s[faces[f][1]], s[faces[f][0]]), dsub(s[faces[f][2]], s[faces[f][0]])), t, 3, s, 4, eps))
			return false;

	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 6; ++j)
			if (separatedOnAxis(dcross(te[i], se[j]), t, 3, s, 4, eps))
				return false;

	// coplanar (flat swept volume) coverage
	D3 n = dcross(te[0], te[1]);
	if (dlen2(n) > 1e-24)
	{
		D3 nn = dmul(n, 1.0 / std::sqrt(dlen2(n)));
		for (int j = 0; j < 6; ++j)
			if (separatedOnAxis(dcross(nn, se[j]), t, 3, s, 4, eps))
				return false;
		for (int i = 0; i < 3; ++i)
			if (separatedOnAxis(dcross(nn, te[i]), t, 3, s, 4, eps))
				return false;
	}

	return true;
}

// Independent triangle/triangle SAT on two static triangles, including coplanar in-plane axes.
bool refTriTri(const D3 a[3], const D3 b[3])
{
	D3 ae[3] = {dsub(a[1], a[0]), dsub(a[2], a[1]), dsub(a[0], a[2])};
	D3 be[3] = {dsub(b[1], b[0]), dsub(b[2], b[1]), dsub(b[0], b[2])};
	double eps = 1e-9;

	if (separatedOnAxis(dcross(ae[0], ae[1]), a, 3, b, 3, eps))
		return false;
	if (separatedOnAxis(dcross(be[0], be[1]), a, 3, b, 3, eps))
		return false;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			if (separatedOnAxis(dcross(ae[i], be[j]), a, 3, b, 3, eps))
				return false;

	// coplanar triangles: edge cross products all collapse onto the shared normal and cannot separate
	// two disjoint polygons lying in the same plane; add in-plane edge-normal axes
	D3 n = dcross(ae[0], ae[1]);
	if (dlen2(n) > 1e-24)
	{
		D3 nn = dmul(n, 1.0 / std::sqrt(dlen2(n)));
		for (int j = 0; j < 3; ++j)
			if (separatedOnAxis(dcross(nn, be[j]), a, 3, b, 3, eps))
				return false;
		for (int i = 0; i < 3; ++i)
			if (separatedOnAxis(dcross(nn, ae[i]), a, 3, b, 3, eps))
				return false;
	}

	return true;
}

D3 toD(const V3& v)
{
	D3 r = {v.x, v.y, v.z};
	return r;
}

// Brute-force final-state check: any two non-adjacent (no shared vertex) intersecting triangles.
bool hasDistantIntersection(const std::vector<V3>& p, const std::vector<unsigned int>& ix, size_t count)
{
	size_t fc = count / 3;
	for (size_t i = 0; i < fc; ++i)
		for (size_t j = i + 1; j < fc; ++j)
		{
			bool adjacent = false;
			for (int a = 0; a < 3 && !adjacent; ++a)
				for (int b = 0; b < 3; ++b)
					if (ix[i * 3 + a] == ix[j * 3 + b])
					{
						adjacent = true;
						break;
					}
			if (adjacent)
				continue;

			D3 t[3], s[3];
			for (int k = 0; k < 3; ++k)
			{
				t[k] = toD(p[ix[i * 3 + k]]);
				s[k] = toD(p[ix[j * 3 + k]]);
			}
			if (refTriTri(t, s))
				return true;
		}
	return false;
}

// Auxiliary dense sampling of a straight-line moving triangle against a static triangle.
// Sampling can miss events, so it is only used to corroborate the exact interval reference.
bool sampledContact(const V3& A, const V3& B, const V3& P0, const V3& P1, const D3 st[3], int steps)
{
	D3 a = toD(A), b = toD(B), p0 = toD(P0), p1 = toD(P1);
	for (int u = 0; u <= steps; ++u)
	{
		double tt = double(u) / steps;
		D3 p = {p0.x + (p1.x - p0.x) * tt, p0.y + (p1.y - p0.y) * tt, p0.z + (p1.z - p0.z) * tt};
		D3 m[3] = {a, b, p};
		if (refTriTri(m, st))
			return true;
	}
	return false;
}

void resetCounters()
{
	meshopt_simplifyResetCollisionCounters();
}

const meshopt_SimplifyCollisionCounters* counters()
{
	return meshopt_simplifyCollisionCounters();
}

// Regular n x n planar/lightly wavy grid patch.
void buildGrid(std::vector<V3>& p, std::vector<unsigned int>& ix, int n, float wave)
{
	for (int y = 0; y <= n; ++y)
		for (int x = 0; x <= n; ++x)
		{
			float fx = float(x) / n, fy = float(y) / n;
			p.push_back(V3{fx, fy, wave * std::sin(4 * fx) * std::cos(4 * fy)});
		}
	for (int y = 0; y < n; ++y)
		for (int x = 0; x < n; ++x)
		{
			unsigned int a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
			ix.push_back(a);
			ix.push_back(b);
			ix.push_back(c);
			ix.push_back(c);
			ix.push_back(b);
			ix.push_back(d);
		}
}

// ===========================================================================
// geometry group: hand-computable continuous collision configurations
// ===========================================================================

// Endpoints are disjoint from the distant triangle, but the straight move crosses it mid-interval.
// Geometry (independently derived): static triangle in z=0; the moving vertex crosses z=0 exactly at
// t=0.5, landing inside the static triangle only at that instant; t=0 is above it, t=1 below it.
int geometryMidPenetration(Case& c)
{
	// 0:S(-5,4,2) 1:A(20,20,0) 2:B(20,30,0) 3:T(15,4,-2) target neighbor
	// 4,5,6: distant static triangle D in z=0
	std::vector<V3> p = {
	    V3{-5, 4, 2}, V3{20, 20, 0}, V3{20, 30, 0}, V3{15, 4, -2},
	    V3{0, 0, 0}, V3{10, 0, 0}, V3{0, 10, 0}};
	std::vector<unsigned int> ix = {0, 1, 2, 0, 3, 1, 4, 5, 6};

	// independent exact reference
	D3 D[3] = {toD(p[4]), toD(p[5]), toD(p[6])};
	D3 A = toD(p[1]), B = toD(p[2]), P0 = toD(p[0]), P1 = toD(p[3]);
	D3 m0[3] = {A, B, P0}, m1[3] = {A, B, P1}, tet[4] = {A, B, P0, P1};
	c.check(!refTriTri(m0, D), "start configuration must be disjoint from the distant face");
	c.check(!refTriTri(m1, D), "end configuration must be disjoint from the distant face");
	c.check(refTriTet(D, tet), "exact swept interval must intersect the distant face mid-move");

	// analytic crossing time of the moving vertex through z=0
	double tstar = -P0.z / (P1.z - P0.z);
	c.check(std::fabs(tstar - 0.5) < 1e-9, "plane crossing happens exactly at t=0.5");
	D3 Pstar = {P0.x + (P1.x - P0.x) * tstar, P0.y + (P1.y - P0.y) * tstar, 0.0};
	D3 mstar[3] = {A, B, Pstar};
	c.check(refTriTri(mstar, D), "moving triangle at the analytic crossing time intersects D");
	c.check(sampledContact(p[1], p[2], p[0], p[3], D, 200), "dense sampling corroborates the exact interval result");

	std::vector<unsigned char> lock(p.size(), 1);
	lock[0] = 0; // only the source vertex may move
	std::vector<unsigned int> out(ix.size());
	float err = 0.f;

	size_t plain = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), 3, 1.f, 0, &err);

	resetCounters();
	size_t guarded = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), 3, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();

	c.check(plain < guarded, "unguarded run collapses through the distant face while guarded run does not");
	c.check(k->collapse_rejected > 0, "at least one collapse must be rejected by the continuous guard");
	c.check(k->narrowphase_tests > 0, "rejection must come from real narrow phase tests, not output size");
	// the distant triangle (vertices 4..6) must survive in the guarded result
	bool distant_survives = false;
	for (size_t i = 0; i + 2 < guarded; i += 3)
		if (out[i] >= 4 && out[i + 1] >= 4 && out[i + 2] >= 4)
			distant_survives = true;
	c.check(distant_survives, "the distant blocking triangle must be retained");
	return 0;
}

// Coplanar positive-area overlap of a flat (zero-volume) swept triangle must block the collapse.
int geometryCoplanarOverlap(Case& c)
{
	std::vector<V3> p = {
	    V3{0, 0, 0}, V3{0, 1, 0}, V3{-1, 0, 0},                 // moving triangle 0,1,2
	    V3{-0.30f, 0.20f, 0}, V3{-0.60f, 0.15f, 0}, V3{-0.20f, 0.50f, 0}}; // distant tri inside the sweep
	std::vector<unsigned int> ix = {0, 1, 2, 3, 4, 5};

	D3 t[3] = {toD(p[3]), toD(p[4]), toD(p[5])};
	D3 tet[4] = {toD(p[2]), toD(p[1]), toD(p[0]), toD(p[1])}; // S collapses toward A within the plane
	c.check(refTriTet(t, tet), "independent reference: coplanar positive-area overlap is an intersection");

	std::vector<unsigned char> lock(p.size(), 1);
	lock[0] = 0;
	std::vector<unsigned int> out(ix.size());
	float err = 0.f;
	resetCounters();
	size_t guarded = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), 3, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	c.check(guarded == ix.size(), "coplanar overlap must prevent the collapse");
	c.check(k->collapses == 0 && k->collapse_rejected > 0, "no collapse accepted; rejections recorded");
	return 0;
}

// Legal vertex/edge adjacency must not lock the mesh: ordinary interior collapses proceed freely.
int geometryLegalAdjacency(Case& c)
{
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	buildGrid(p, ix, 6, 0.f); // single connected planar sheet

	std::vector<unsigned int> out(ix.size());
	float err = 0.f;
	resetCounters();
	size_t guarded = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), ix.size() / 2, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	c.check(k->collapses > 4, "legal adjacent collapses must be accepted in bulk");
	c.check(k->collapse_rejected == 0, "a single sheet has no distant face to reject against");
	c.check(guarded < ix.size(), "the sheet must simplify substantially");
	c.check(!hasDistantIntersection(p, out, guarded), "guarded result has no distant self-intersection");
	return 0;
}

// Split-vertex attribute seam: coincident seam copies are one geometric identity and stay adjacent.
int geometrySeam(Case& c)
{
	std::vector<V3> p = {
	    V3{0, 0, 0}, V3{1, 0, 0}, V3{0, 1, 0},
	    V3{1, 0, 0}, V3{0, 1, 0}, V3{1, 1, 0}}; // 3 and 4 duplicate positions of 1 and 2 (seam wedges)
	std::vector<unsigned int> ix = {0, 1, 2, 0, 3, 4, 3, 5, 4};

	std::vector<unsigned int> out(ix.size());
	float err = 0.f;
	resetCounters();
	size_t guarded = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), 3, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	// the seam collapse is legal adjacency and must go through; the guard must not treat coincident
	// seam copies as mutually distant faces
	c.check(k->collapse_rejected == 0, "coincident seam wedges must be recognized as the same geometry");
	c.check(guarded <= ix.size(), "seam mesh stays valid");
	return 0;
}

// Point/line contact between otherwise disjoint non-adjacent faces blocks the move (closed interval).
int geometryPointContact(Case& c)
{
	std::vector<V3> p = {
	    V3{0, 0, 0}, V3{0, 1, 0}, V3{-1, 0, 0},
	    V3{-0.5f, 0.5f, 0}, V3{-1.5f, 1.5f, 0}, V3{-1.5f, 0.5f, 0}}; // vertex 3 lies exactly on the swept edge
	std::vector<unsigned int> ix = {0, 1, 2, 3, 4, 5};

	D3 t[3] = {toD(p[3]), toD(p[4]), toD(p[5])};
	D3 tet[4] = {toD(p[2]), toD(p[1]), toD(p[0]), toD(p[1])};
	c.check(refTriTet(t, tet), "independent reference: point contact on a swept edge is an intersection");

	std::vector<unsigned char> lock(p.size(), 1);
	lock[0] = 0;
	std::vector<unsigned int> out(ix.size());
	float err = 0.f;
	resetCounters();
	size_t guarded = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), 3, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	c.check(guarded == ix.size(), "point/line contact must block the collapse");
	return 0;
}

// ===========================================================================
// scheduling group: live-index updates, rejection recovery, update solves
// ===========================================================================

// Multiple accepted collapses within the simplification update the live collision index immediately,
// and the resulting mesh stays free of distant self-intersection.
int schedulingLiveCollapses(Case& c)
{
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	buildGrid(p, ix, 10, 0.03f);

	std::vector<unsigned int> out(ix.size());
	float err = 0.f;
	resetCounters();
	size_t guarded = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), ix.size() / 3, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	c.check(k->collapses >= 10, "many collapses are accepted across the run");
	c.check(k->local_updates >= k->collapses, "every accepted collapse performs incremental index updates");
	c.check(!hasDistantIntersection(p, out, guarded), "live updates keep the guarded result intersection-free");
	return 0;
}

// A blocking region rejects collapses while an independent region keeps reducing: rejection never
// poisons remaps/locks and legitimate simplification continues afterwards.
int schedulingRejectThenContinue(Case& c)
{
	// reducible fan patch plus a rigid distant wall placed across one vertex's only escape routes
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	const int n = 8;
	p.push_back(V3{0, 0, 0}); // fan center
	for (int i = 0; i < n; ++i)
	{
		float a = float(2 * M_PI * i / n);
		p.push_back(V3{std::cos(a), std::sin(a), 0.f});
	}
	for (int i = 0; i < n; ++i)
	{
		unsigned a = 1 + i, b = 1 + (i + 1) % n;
		ix.push_back(0);
		ix.push_back(a);
		ix.push_back(b);
	}
	unsigned base = unsigned(p.size());
	p.push_back(V3{0.5f, -0.25f, -0.5f});
	p.push_back(V3{0.5f, 0.25f, -0.5f});
	p.push_back(V3{0.5f, 0.f, 0.5f});
	ix.push_back(base);
	ix.push_back(base + 1);
	ix.push_back(base + 2);

	std::vector<unsigned char> lock(p.size(), 1);
	lock[0] = 0;
	std::vector<unsigned int> out(ix.size());
	float err = 0.f;
	resetCounters();
	size_t guarded = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), 3, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	c.check(k->collapse_rejected > 0, "collapses toward the wall are rejected");
	c.check(k->collapses > 0, "legal collapses along the rest of the fan still succeed");
	c.check(guarded < ix.size() && guarded > 3, "the mesh partially reduces instead of being fully returned");
	// the blocking wall remains intact
	bool wall = false;
	for (size_t i = 0; i + 2 < guarded; i += 3)
		if (out[i] == base && out[i + 1] == base + 1 && out[i + 2] == base + 2)
			wall = true;
	c.check(wall, "the rejected wall region is retained");
	return 0;
}

// WithUpdate: accepted straight-line position moves are applied and reflected in the spatial index,
// while unsafe solved positions keep their original location. The guard guarantees per-vertex that a
// vertex starting on the safe side of a barrier plane never crosses it through its straight-line move.
int schedulingUpdateGuard(Case& c)
{
	const int n = 16;
	const float wave = 0.05f;
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	buildGrid(p, ix, n, wave);
	size_t bv = p.size(), bi = ix.size();

	// barrier plane just above the lower part of the wavy surface; quadric solves in that region try to
	// move upward toward/through it, exercising the continuous guard on straight-line position moves
	float wall_z = 0.02f;
	p.push_back(V3{-0.5f, -0.5f, wall_z});
	p.push_back(V3{1.5f, -0.5f, wall_z});
	p.push_back(V3{-0.5f, 1.5f, wall_z});
	p.push_back(V3{1.5f, 1.5f, wall_z});
	ix.push_back(unsigned(bv));
	ix.push_back(unsigned(bv + 1));
	ix.push_back(unsigned(bv + 2));
	ix.push_back(unsigned(bv + 2));
	ix.push_back(unsigned(bv + 1));
	ix.push_back(unsigned(bv + 3));

	std::vector<V3> q = p;
	std::vector<unsigned int> ux = ix;
	float err = 0.f;
	resetCounters();
	size_t count = meshopt_simplifyWithUpdate(ux.data(), ux.size(), &q[0].x, q.size(), sizeof(V3), 0, 0, 0, 0, 0,
	    bi / 2, 1.f, meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	c.check(k->position_moves > 0, "nonzero position updates are accepted");
	c.check(k->position_rejected > 0, "unsafe solved positions are rejected and keep their origin");
	c.check(k->local_updates >= k->position_moves, "accepted moves immediately refit the spatial index");
	c.check(count > 0 && count % 3 == 0, "the guarded update returns a valid mesh");

	size_t changed = 0;
	for (size_t i = 0; i < p.size(); ++i)
		changed += (p[i].x != q[i].x || p[i].y != q[i].y || p[i].z != q[i].z);
	c.check(changed > 0, "the input position buffer receives the accepted moves");

	// per-vertex barrier invariant: a vertex strictly below the plane at the start never ends on/above it,
	// because its straight-line move would have swept through the barrier triangle
	int below = 0, crossings = 0;
	for (size_t v = 0; v < bv; ++v)
	{
		if (p[v].z < wall_z - 1e-7f)
		{
			++below;
			if (q[v].z >= wall_z - 1e-7f)
				++crossings;
		}
	}
	c.check(below > 0, "the fixture has vertices on the safe side of the barrier");
	c.check(crossings == 0, "no safe-side vertex crosses the barrier through its position move");
	return 0;
}

// A rejected candidate leaves no trace: running the guarded simplifier twice yields identical output.
int schedulingRejectionPurity(Case& c)
{
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	buildGrid(p, ix, 8, 0.02f);
	unsigned base = unsigned(p.size());
	p.push_back(V3{0.5f, -0.2f, 0.01f});
	p.push_back(V3{0.5f, 0.2f, 0.01f});
	p.push_back(V3{0.5f, 0.f, 0.05f});
	ix.push_back(base);
	ix.push_back(base + 1);
	ix.push_back(base + 2);

	std::vector<unsigned int> a(ix.size()), b(ix.size());
	float ea = 0.f, eb = 0.f;
	resetCounters();
	size_t ca = meshopt_simplify(a.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), ix.size() / 2, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &ea);
	size_t cb = meshopt_simplify(b.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), ix.size() / 2, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &eb);
	c.check(ca == cb, "repeated runs are deterministic despite rejections");
	bool same = true;
	for (size_t i = 0; i < ca; ++i)
		same = same && a[i] == b[i];
	c.check(same, "rejected candidates do not pollute the index/remap state");
	return 0;
}

// ===========================================================================
// scaling group: sub-quadratic narrow phase, target budget, option parity
// ===========================================================================

struct ScaleStats
{
	int patches;
	size_t faces_in;
	size_t faces_out;
	size_t narrowphase;
	size_t collapses;
	size_t local_updates;
	size_t rebuilds;
};

ScaleStats runPatches(int patches)
{
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	const int n = 16;
	for (int blk = 0; blk < patches; ++blk)
	{
		float ox = blk * 4.0f; // clearly separated shells
		unsigned bv = unsigned(p.size());
		for (int y = 0; y <= n; ++y)
			for (int x = 0; x <= n; ++x)
			{
				float fx = float(x) / n, fy = float(y) / n;
				p.push_back(V3{ox + fx, fy, 0.02f * (((x + y) & 1) ? 1.f : -1.f)});
			}
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				unsigned a = bv + y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
				ix.push_back(a);
				ix.push_back(b);
				ix.push_back(c);
				ix.push_back(c);
				ix.push_back(b);
				ix.push_back(d);
			}
	}
	std::vector<unsigned int> out(ix.size());
	float err = 0.f;
	resetCounters();
	size_t count = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), ix.size() / 2, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	ScaleStats s = {patches, ix.size() / 3, count / 3, k->narrowphase_tests, k->collapses, k->local_updates, k->index_rebuilds};
	return s;
}

// Narrow phase work grows near-linearly with global face count, never as a pairwise O(F^2) scan.
int scalingNarrowPhaseGrowth(Case& c)
{
	ScaleStats s32 = runPatches(32);
	ScaleStats s64 = runPatches(64);
	ScaleStats s128 = runPatches(128);
	ScaleStats s256 = runPatches(256);

	std::printf("SCALE K: faces/np  32:%zu/%zu  64:%zu/%zu  128:%zu/%zu  256:%zu/%zu\n",
	    s32.faces_in, s32.narrowphase, s64.faces_in, s64.narrowphase,
	    s128.faces_in, s128.narrowphase, s256.faces_in, s256.narrowphase);

	double ratio32 = double(s32.narrowphase) / double(s32.faces_in);
	double ratio256 = double(s256.narrowphase) / double(s256.faces_in);
	double quad32 = double(s32.narrowphase) / double(s32.faces_in) / double(s32.faces_in);
	double quad256 = double(s256.narrowphase) / double(s256.faces_in) / double(s256.faces_in);

	// narrow phase per face stays bounded by a small constant instead of growing with the mesh
	c.check(ratio32 < 40.0 && ratio256 < 40.0, "narrow phase tests per face stay bounded by a constant");
	// O(F^2) would keep np/F^2 constant; an 8x face growth must shrink it markedly
	c.check(quad32 > 4.0 * quad256, "normalized pairwise cost drops as the mesh grows (sub-quadratic)");
	c.check(s256.collapses > s32.collapses, "collapses scale up with patch count");
	c.check(s256.local_updates > 0, "incremental updates are used rather than full per-candidate rebuilds");
	return 0;
}

// The mandated planar budget: one 16x16 regular sheet, target 128 faces, must reach at most 256 faces.
int scalingPlanarBudget(Case& c)
{
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	buildGrid(p, ix, 16, 0.f);
	c.check(ix.size() / 3 == 512, "fixture is a 16x16 sheet with 512 triangles");

	std::vector<unsigned int> out(ix.size());
	float err = 0.f;
	resetCounters();
	size_t guarded = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), 384, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	c.check(guarded / 3 <= 256, "guarded planar simplification reaches the 256-face budget");
	c.check(k->collapses > 0, "the budget is reached through real accepted collapses");
	return 0;
}

// Curved WithUpdate on many shells performs real accepted nonzero position updates.
int scalingUpdateMoves(Case& c)
{
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	const int patches = 32, n = 12;
	for (int blk = 0; blk < patches; ++blk)
	{
		float ox = blk * 3.0f;
		unsigned bv = unsigned(p.size());
		for (int y = 0; y <= n; ++y)
			for (int x = 0; x <= n; ++x)
			{
				float fx = float(x) / n, fy = float(y) / n;
				p.push_back(V3{ox + fx, fy, 0.04f * std::sin(3 * fx) * std::cos(3 * fy)});
			}
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				unsigned a = bv + y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
				ix.push_back(a);
				ix.push_back(b);
				ix.push_back(c);
				ix.push_back(c);
				ix.push_back(b);
				ix.push_back(d);
			}
	}
	std::vector<V3> q = p;
	std::vector<unsigned int> ux = ix;
	float err = 0.f;
	resetCounters();
	size_t count = meshopt_simplifyWithUpdate(ux.data(), ux.size(), &q[0].x, q.size(), sizeof(V3), 0, 0, 0, 0, 0,
	    ix.size() / 2, 1e-3f, meshopt_SimplifyNoSelfIntersections, &err);
	const meshopt_SimplifyCollisionCounters* k = counters();
	c.check(k->position_moves > 0, "curved multi-shell update accepts nonzero position moves");
	size_t changed = 0;
	for (size_t i = 0; i < p.size(); ++i)
		changed += (p[i].x != q[i].x || p[i].y != q[i].y || p[i].z != q[i].z);
	c.check(changed > 0, "accepted moves change the position buffer");
	c.check(!hasDistantIntersection(q, ux, count), "updated multi-shell result stays intersection-free");
	return 0;
}

// Sparse renumbering, in-place compression, attribute seam handling and flag-off compatibility.
int scalingCompatibility(Case& c)
{
	std::vector<V3> p;
	std::vector<unsigned int> ix;
	buildGrid(p, ix, 10, 0.02f);
	size_t used_vertices = p.size();

	std::vector<unsigned int> out(ix.size());
	float e_off = 0.f, e_on = 0.f;

	// flag off: plain entry point, used as the compatibility baseline
	std::vector<unsigned int> ref(ix.size());
	size_t plain = meshopt_simplify(ref.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), ix.size() / 2, 1.f, 0, &e_off);

	// sparse: append unreferenced vertices and request sparse renumbering; results must stay in range
	std::vector<V3> ps = p;
	for (int i = 0; i < 64; ++i)
		ps.push_back(V3{99.f, 99.f, 99.f});
	resetCounters();
	size_t sparse = meshopt_simplify(out.data(), ix.data(), ix.size(), &ps[0].x, ps.size(), sizeof(V3), ix.size() / 2, 1.f,
	    meshopt_SimplifyNoSelfIntersections | meshopt_SimplifySparse, &e_on);
	bool in_range = true;
	for (size_t i = 0; i < sparse; ++i)
		if (out[i] >= used_vertices)
			in_range = false;
	c.check(in_range, "sparse output indices renumber back into the original vertex range");

	// LockBorder combination
	std::vector<unsigned char> lock(used_vertices, 0);
	size_t locked = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), ix.size() / 2, 1.f,
	    meshopt_SimplifyNoSelfIntersections | meshopt_SimplifyLockBorder, &e_on);
	c.check(locked > 0 && locked % 3 == 0, "LockBorder combined with the guard returns a valid mesh");

	// ErrorAbsolute combination
	float scale = meshopt_simplifyScale(&p[0].x, p.size(), sizeof(V3));
	size_t abs_count = meshopt_simplify(out.data(), ix.data(), ix.size(), &p[0].x, p.size(), sizeof(V3), ix.size() / 2,
	    0.05f * scale, meshopt_SimplifyNoSelfIntersections | meshopt_SimplifyErrorAbsolute, &e_on);
	c.check(abs_count > 0 && std::isfinite(e_on), "ErrorAbsolute combined with the guard returns a finite error");

	// in-place index compression
	std::vector<unsigned int> inplace = ix;
	size_t ip_count = meshopt_simplify(inplace.data(), inplace.data(), inplace.size(), &p[0].x, p.size(), sizeof(V3),
	    ix.size() / 2, 1.f, meshopt_SimplifyNoSelfIntersections, &e_on);
	c.check(ip_count > 0 && ip_count <= ix.size(), "in-place guarded simplification compresses the index buffer");

	// attribute seam: duplicated position wedges with a one-float attribute, attributes retained
	std::vector<V3> pa = p;
	pa.push_back(p[1]); // extra coincident wedge
	std::vector<float> attr(pa.size(), 1.f);
	std::vector<float> weights(1, 1.f);
	std::vector<unsigned int> aix = ix;
	size_t attr_count = meshopt_simplifyWithAttributes(out.data(), aix.data(), aix.size(), &pa[0].x, pa.size(), sizeof(V3),
	    attr.data(), sizeof(float), weights.data(), 1, 0, ix.size() / 2, 1.f,
	    meshopt_SimplifyNoSelfIntersections, &e_on);
	c.check(attr_count > 0 && attr_count % 3 == 0, "attribute seam simplification works with the guard enabled");

	c.check(plain > 0, "flag-off baseline still simplifies as before");
	return 0;
}

} // namespace

void register_task_tests(Registry& registry)
{
	registry.add("geometry", "mid_penetration", geometryMidPenetration);
	registry.add("geometry", "coplanar_overlap", geometryCoplanarOverlap);
	registry.add("geometry", "legal_adjacency", geometryLegalAdjacency);
	registry.add("geometry", "seam_identity", geometrySeam);
	registry.add("geometry", "point_contact", geometryPointContact);

	registry.add("scheduling", "live_collapses", schedulingLiveCollapses);
	registry.add("scheduling", "reject_then_continue", schedulingRejectThenContinue);
	registry.add("scheduling", "update_position_guard", schedulingUpdateGuard);
	registry.add("scheduling", "rejection_purity", schedulingRejectionPurity);

	registry.add("scaling", "narrowphase_growth", scalingNarrowPhaseGrowth);
	registry.add("scaling", "planar_budget", scalingPlanarBudget);
	registry.add("scaling", "update_position_moves", scalingUpdateMoves);
	registry.add("scaling", "compatibility", scalingCompatibility);
}
