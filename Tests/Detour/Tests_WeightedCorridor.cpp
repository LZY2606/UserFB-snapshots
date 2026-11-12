//
// Tests for the continuous weighted corridor straightening query
// dtNavMeshQuery::findWeightedPath() and dtPathCorridor::findWeightedPath().
//

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourWeightedPath.h"
#include "DetourPathCorridor.h"

#include "catch2/catch_amalgamated.hpp"

#include <cmath>
#include <cstring>
#include <float.h>
#include <limits>

namespace
{
	const double TEST_EPS = 1e-8;

	template <typename VectA, typename VectB>
	double dist3(const VectA* a, const VectB* b)
	{
		const double dx = (double)b[0]-a[0], dy = (double)b[1]-a[1], dz = (double)b[2]-a[2];
		return std::sqrt(dx*dx + dy*dy + dz*dz);
	}

	bool finiteVec(const double* v, const int n)
	{
		for (int i = 0; i < n; ++i)
		{
			if (!std::isfinite(v[i])) return false;
		}
		return true;
	}

	// Builds a single-tile navmesh from the same creation data shape as the
	// Detour samples: clockwise quads, zero-based neighbour indices.
	bool buildSingleTile(dtNavMesh& mesh,
						 const unsigned short* verts, int vertCount,
						 const unsigned short* polys, int polyCount, int nvp,
						 const unsigned short* flags, const unsigned char* areas,
						 const float bmin[3], const float bmax[3])
	{
		dtNavMeshCreateParams p;
		std::memset(&p, 0, sizeof(p));
		p.verts = verts;
		p.vertCount = vertCount;
		p.polys = polys;
		p.polyCount = polyCount;
		p.nvp = nvp;
		p.polyFlags = flags;
		p.polyAreas = areas;
		p.bmin[0] = bmin[0]; p.bmin[1] = bmin[1]; p.bmin[2] = bmin[2];
		p.bmax[0] = bmax[0]; p.bmax[1] = bmax[1]; p.bmax[2] = bmax[2];
		p.cs = 1.0f;
		p.ch = 1.0f;
		p.walkableHeight = 2.0f;
		p.walkableRadius = 0.0f;
		p.walkableClimb = 1.0f;
		unsigned char* data = 0;
		int size = 0;
		if (!dtCreateNavMeshData(&p, &data, &size)) return false;
		return dtStatusSucceed(mesh.init(data, size, DT_TILE_FREE_DATA));
	}

	bool buildTile(dtNavMesh& mesh,
				   const unsigned short* verts, int vertCount,
				   const unsigned short* polys, int polyCount, int nvp,
				   const unsigned short* flags, const unsigned char* areas,
				   const float bmin[3], const float bmax[3],
				   int tileX, int tileY)
	{
		dtNavMeshCreateParams p;
		std::memset(&p, 0, sizeof(p));
		p.verts = verts;
		p.vertCount = vertCount;
		p.polys = polys;
		p.polyCount = polyCount;
		p.nvp = nvp;
		p.polyFlags = flags;
		p.polyAreas = areas;
		p.tileX = tileX;
		p.tileY = tileY;
		p.bmin[0] = bmin[0]; p.bmin[1] = bmin[1]; p.bmin[2] = bmin[2];
		p.bmax[0] = bmax[0]; p.bmax[1] = bmax[1]; p.bmax[2] = bmax[2];
		p.cs = 1.0f;
		p.ch = 1.0f;
		p.walkableHeight = 2.0f;
		p.walkableRadius = 0.0f;
		p.walkableClimb = 1.0f;
		unsigned char* data = 0;
		int size = 0;
		if (!dtCreateNavMeshData(&p, &data, &size)) return false;
		return dtStatusSucceed(mesh.addTile(data, size, DT_TILE_FREE_DATA, 0, 0));
	}

	// Independent weighted polyline cost, recomputed from the raw points and
	// area costs. Zero-length segments are legal and contribute zero.
	double independentCost(const dtWeightedPathResult& r, const double* weights)	{
		double cost = 0.0;
		for (int i = 0; i < r.dualCount; ++i)
			cost += weights[i] * dist3(&r.points[i*3], &r.points[(i+1)*3]);
		return cost;
	}

	// Independent portal extraction straight from mesh links, mirroring the
	// getPortalPoints contract (edge vertices plus bmin/bmax clipping).
	void portalEndpoints(const dtNavMesh& mesh, const dtPolyRef from, const dtPolyRef to,
						 double left[3], double right[3])
	{
		const dtMeshTile* tile = 0;
		const dtPoly* poly = 0;
		REQUIRE(dtStatusSucceed(mesh.getTileAndPolyByRef(from, &tile, &poly)));
		const dtLink* link = 0;
		for (unsigned int k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
		{
			if (tile->links[k].ref == to) { link = &tile->links[k]; break; }
		}
		REQUIRE(link != 0);
		const int v0 = poly->verts[link->edge];
		const int v1 = poly->verts[(link->edge+1) % (int)poly->vertCount];
		double a[3] = { tile->verts[v0*3], tile->verts[v0*3+1], tile->verts[v0*3+2] };
		double b[3] = { tile->verts[v1*3], tile->verts[v1*3+1], tile->verts[v1*3+2] };
		if (link->side != 0xff && (link->bmin != 0 || link->bmax != 255))
		{
			const double tmin = link->bmin / 255.0;
			const double tmax = link->bmax / 255.0;
			for (int k = 0; k < 3; ++k)
			{
				left[k] = a[k] + (b[k]-a[k]) * tmin;
				right[k] = a[k] + (b[k]-a[k]) * tmax;
			}
		}
		else
		{
			for (int k = 0; k < 3; ++k) { left[k] = a[k]; right[k] = b[k]; }
		}
	}

	// Parameter t of point p on segment a->b, projected on the line.
	double portalParameter(const double* p, const double* a, const double* b)
	{
		double h[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
		double g[3] = { p[0]-a[0], p[1]-a[1], p[2]-a[2] };
		const double hh = h[0]*h[0] + h[1]*h[1] + h[2]*h[2];
		if (hh == 0.0) return 0.0;
		return (g[0]*h[0] + g[1]*h[1] + g[2]*h[2]) / hh;
	}

	// Independent dual lower bound using the task's certificate formula.
	double independentLowerBound(const dtNavMesh& mesh, const dtPolyRef* path, int npoly,
								 const dtWeightedPathResult& r)
	{
		const double* q = r.dual;
		double d = 0.0;
		for (int k = 0; k < 3; ++k)
			d += q[(npoly-1)*3+k] * r.points[npoly*3+k] - q[k] * r.points[k];
		for (int j = 1; j < npoly; ++j)
		{
			double a[3], b[3];
			portalEndpoints(mesh, path[j-1], path[j], a, b);
			double diff[3] = { q[(j-1)*3]-q[j*3], q[(j-1)*3+1]-q[j*3+1], q[(j-1)*3+2]-q[j*3+2] };
			const double ca = diff[0]*a[0] + diff[1]*a[1] + diff[2]*a[2];
			const double cb = diff[0]*b[0] + diff[1]*b[1] + diff[2]*b[2];
			d += ca < cb ? ca : cb;
		}
		return d;
	}

	// Scalar golden-section reference for a single-portal problem.
	template <typename VectA, typename VectB>
	double goldenReference(const VectA* sf, const VectB* ef,
						   const double* a, const double* b,
						   const double w0, const double w1)
	{
		const double s[3] = { (double)sf[0], (double)sf[1], (double)sf[2] };
		const double e[3] = { (double)ef[0], (double)ef[1], (double)ef[2] };
		double lo = 0.0, hi = 1.0;
		const double gr = 0.6180339887498949;
		double c[3], d[3];
		for (int it = 0; it < 200; ++it)
		{
			const double x1 = hi - gr*(hi-lo);
			const double x2 = lo + gr*(hi-lo);
			for (int k = 0; k < 3; ++k)
			{
				c[k] = a[k] + (b[k]-a[k]) * x1;
				d[k] = a[k] + (b[k]-a[k]) * x2;
			}
			const double f1 = w0 * dist3(s, c) + w1 * dist3(c, e);
			const double f2 = w0 * dist3(s, d) + w1 * dist3(d, e);
			if (f1 < f2) hi = x2; else lo = x1;
		}
		return 0.5 * (lo + hi);
	}

	// Independent multi-portal reference solver: cyclic coordinate descent
	// with golden-section 1D minimization, run to high precision.
	template <typename VectA, typename VectB>
	void referenceSolve(const VectA* sf, const VectB* ef,
						const double* portalA, const double* portalB, int m,
						const double* weights, double* outPoints)
	{
		const double s[3] = { (double)sf[0], (double)sf[1], (double)sf[2] };
		const double e[3] = { (double)ef[0], (double)ef[1], (double)ef[2] };
		for (int k = 0; k < 3; ++k)
		{
			outPoints[0*3+k] = s[k];
			outPoints[(m+1)*3+k] = e[k];
		}
		double* t = new double[m > 0 ? m : 1];
		for (int j = 0; j < m; ++j)
		{
			t[j] = 0.5;
			for (int k = 0; k < 3; ++k)
				outPoints[(j+1)*3+k] = 0.5*(portalA[j*3+k] + portalB[j*3+k]);
		}
		const double gr = 0.6180339887498949;
		for (int sweep = 0; sweep < 400; ++sweep)
		{
			for (int j = 0; j < m; ++j)
			{
				const double* prev = &outPoints[j*3];
				const double* next = &outPoints[(j+2)*3];
				double lo = 0.0, hi = 1.0;
				double pc[3], pd[3];
				for (int it = 0; it < 80; ++it)
				{
					const double x1 = hi - gr*(hi-lo);
					const double x2 = lo + gr*(hi-lo);
					for (int k = 0; k < 3; ++k)
					{
						pc[k] = portalA[j*3+k] + (portalB[j*3+k]-portalA[j*3+k]) * x1;
						pd[k] = portalA[j*3+k] + (portalB[j*3+k]-portalA[j*3+k]) * x2;
					}
					const double f1 = weights[j]*dist3(prev, pc) + weights[j+1]*dist3(pc, next);
					const double f2 = weights[j]*dist3(prev, pd) + weights[j+1]*dist3(pd, next);
					if (f1 < f2) hi = x2; else lo = x1;
				}
				t[j] = 0.5*(lo+hi);
				for (int k = 0; k < 3; ++k)
					outPoints[(j+1)*3+k] = portalA[j*3+k] + (portalB[j*3+k]-portalA[j*3+k]) * t[j];
			}
		}
		delete[] t;
	}

	// The three adjacent flat ground polygons used by the starting program.
	struct StripMesh
	{
		dtNavMesh mesh;
		dtPolyRef base;
		static const int NPOLY = 3;
	};

	void initStrip(StripMesh& sm)
	{
		static const unsigned short verts[] = {
			0,0,0, 0,0,10, 2,0,0, 2,0,10, 5,0,0, 5,0,10, 7,0,0, 7,0,10
		};
		static const unsigned short polys[] = {
			0,1,3,2, 0xffff,0xffff,1,0xffff,
			2,3,5,4, 0,0xffff,2,0xffff,
			4,5,7,6, 1,0xffff,0xffff,0xffff
		};
		static const unsigned short flags[] = {1, 1, 1};
		static const unsigned char areas[] = {0, 1, 0};
		const float bmin[3] = {0, 0, 0};
		const float bmax[3] = {7, 1, 10};
		REQUIRE(buildSingleTile(sm.mesh, verts, 8, polys, 3, 4, flags, areas, bmin, bmax));
		sm.base = sm.mesh.getPolyRefBase(static_cast<const dtNavMesh&>(sm.mesh).getTile(0));
	}
}

// ---------------------------------------------------------------------------
// optimality
// ---------------------------------------------------------------------------

TEST_CASE("Weighted single portal matches an independent scalar minimizer", "[weighted][optimality]")
{
	// Two rectangles sharing the x=3 portal; one interior portal point.
	static const unsigned short verts[] = {
		0,0,0, 0,0,10, 3,0,0, 3,0,10, 6,0,0, 6,0,10
	};
	static const unsigned short polys[] = {
		0,1,3,2, 0xffff,0xffff,1,0xffff,
		2,3,5,4, 0,0xffff,0xffff,0xffff
	};
	static const unsigned short flags[] = {1, 1};
	static const unsigned char areas[] = {0, 1};
	const float bmin[3] = {0, 0, 0};
	const float bmax[3] = {6, 1, 10};
	dtNavMesh mesh;
	REQUIRE(buildSingleTile(mesh, verts, 6, polys, 2, 4, flags, areas, bmin, bmax));
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(0, 1.0f);
	filter.setAreaCost(1, 5.0f);

	const dtPolyRef base = mesh.getPolyRefBase(static_cast<const dtNavMesh&>(mesh).getTile(0));
	const dtPolyRef path[2] = { base, base | 1 };
	const float s[3] = {1.0f, 0.0f, 1.0f};
	const float e[3] = {5.0f, 0.0f, 9.0f};

	double pts[9];
	double dual[6];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 3; r.pointCount = 0;
	r.dual = dual; r.maxDual = 2; r.dualCount = 0;
	r.upperCost = r.lowerCost = 0.0;

	const dtStatus st = query.findWeightedPath(s, e, path, 2, &filter, 1e-7, 20000, &r);
	REQUIRE(dtStatusSucceed(st));
	REQUIRE(r.pointCount == 3);
	REQUIRE(r.dualCount == 2);

	double a[3], b[3];
	portalEndpoints(mesh, path[0], path[1], a, b);
	const double tGot = portalParameter(&pts[3], a, b);
	const double tRef = goldenReference(s, e, a, b, 1.0, 5.0);
	CHECK(tGot == Catch::Approx(tRef).margin(1e-6));
	CHECK(tGot > 0.0);
	CHECK(tGot < 1.0);
	CHECK(std::abs(r.upperCost - r.lowerCost) <= 1e-7);

	// The optimum must be strictly better than the midpoint choice.
	const double mid[3] = { 0.5*(a[0]+b[0]), 0.5*(a[1]+b[1]), 0.5*(a[2]+b[2]) };
	const double midCost = 1.0 * dist3(s, mid) + 5.0 * dist3(mid, e);
	CHECK(r.upperCost < midCost - 1e-4);
}

TEST_CASE("Portal optimum can sit on an endpoint", "[weighted][optimality]")
{
	// Both endpoints prefer the low-z end of the portal, so the exact
	// optimum is the portal endpoint itself.
	static const unsigned short verts[] = {
		0,0,0, 0,0,10, 3,0,0, 3,0,10, 6,0,0, 6,0,10
	};
	static const unsigned short polys[] = {
		0,1,3,2, 0xffff,0xffff,1,0xffff,
		2,3,5,4, 0,0xffff,0xffff,0xffff
	};
	static const unsigned short flags[] = {1, 1};
	static const unsigned char areas[] = {0, 1};
	const float bmin[3] = {0, 0, 0};
	const float bmax[3] = {6, 1, 10};
	dtNavMesh mesh;
	REQUIRE(buildSingleTile(mesh, verts, 6, polys, 2, 4, flags, areas, bmin, bmax));
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(0, 1.0f);
	filter.setAreaCost(1, 100.0f);

	const dtPolyRef base = mesh.getPolyRefBase(static_cast<const dtNavMesh&>(mesh).getTile(0));
	const dtPolyRef path[2] = { base, base | 1 };
	const float s[3] = {1.0f, 0.0f, 0.0f};
	const float e[3] = {5.0f, 0.0f, 0.0f};

	double pts[9];
	double dual[6];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 3; r.pointCount = 0;
	r.dual = dual; r.maxDual = 2; r.dualCount = 0;
	const dtStatus st = query.findWeightedPath(s, e, path, 2, &filter, 1e-7, 20000, &r);
	REQUIRE(dtStatusSucceed(st));

	double a[3], b[3];
	portalEndpoints(mesh, path[0], path[1], a, b);
	const double tGot = portalParameter(&pts[3], a, b);
	const double tLowZ = (std::abs(a[2]) <= std::abs(b[2])) ? 0.0 : 1.0;
	CHECK(tGot == Catch::Approx(tLowZ).margin(1e-6));
	// The crossing is the portal endpoint at z = 0.
	CHECK(pts[3] == Catch::Approx(3.0).margin(1e-6));
	CHECK(pts[5] == Catch::Approx(0.0).margin(1e-6));
	CHECK(std::abs(r.upperCost - r.lowerCost) <= 1e-7);
	// Cost is the straight segmented line through the endpoint.
	CHECK(r.upperCost == Catch::Approx(2.0 + 200.0).epsilon(1e-9));
}

TEST_CASE("Greedy per-portal choices are suboptimal across multiple portals", "[weighted][optimality]")
{
	StripMesh sm;
	initStrip(sm);
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&sm.mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 4.0f);

	const dtPolyRef path[3] = { sm.base, sm.base | 1, sm.base | 2 };
	const float s[3] = {1.0f, 0.0f, 1.0f};
	const float e[3] = {6.0f, 0.0f, 9.0f};
	const double weights[3] = {1.0, 4.0, 1.0};

	double pts[12];
	double dual[9];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 4; r.pointCount = 0;
	r.dual = dual; r.maxDual = 3; r.dualCount = 0;
	REQUIRE(dtStatusSucceed(query.findWeightedPath(s, e, path, 3, &filter, 1e-7, 20000, &r)));

	// Independent high-precision reference over the same portal segments.
	double portalA[6], portalB[6];
	portalEndpoints(sm.mesh, path[0], path[1], &portalA[0], &portalB[0]);
	portalEndpoints(sm.mesh, path[1], path[2], &portalA[3], &portalB[3]);
	double refPts[12];
	referenceSolve(s, e, portalA, portalB, 2, weights, refPts);
	double refCost = 0.0;
	for (int i = 0; i < 3; ++i)
		refCost += weights[i] * dist3(&refPts[i*3], &refPts[(i+1)*3]);
	CHECK(r.upperCost == Catch::Approx(refCost).epsilon(1e-7));

	// One-pass greedy: minimize portal 1 with portal 2 pinned at its midpoint,
	// then portal 2 with the chosen portal 1 fixed. No joint adjustment.
	double greedy[12];
	for (int k = 0; k < 3; ++k)
	{
		greedy[0*3+k] = s[k];
		greedy[3*3+k] = e[k];
		greedy[2*3+k] = 0.5*(portalA[3+k] + portalB[3+k]);
	}
	const double t1 = goldenReference(s, &greedy[6], &portalA[0], &portalB[0], 1.0, 4.0);
	for (int k = 0; k < 3; ++k)
		greedy[1*3+k] = portalA[k] + (portalB[k]-portalA[k]) * t1;
	const double t2 = goldenReference(&greedy[3], e, &portalA[3], &portalB[3], 4.0, 1.0);
	for (int k = 0; k < 3; ++k)
		greedy[2*3+k] = portalA[3+k] + (portalB[3+k]-portalA[3+k]) * t2;
	double greedyCost = 0.0;
	for (int i = 0; i < 3; ++i)
		greedyCost += weights[i] * dist3(&greedy[i*3], &greedy[(i+1)*3]);
	CHECK(greedyCost > r.upperCost + 1e-4);
}

TEST_CASE("Equal weights reproduce the shortest polyline and survive translation/scaling", "[weighted][optimality]")
{
	StripMesh sm;
	initStrip(sm);
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&sm.mesh, 64)));
	dtQueryFilter filter; // every area costs 1 by default

	const dtPolyRef path[3] = { sm.base, sm.base | 1, sm.base | 2 };
	const float s[3] = {1.0f, 0.0f, 1.0f};
	const float e[3] = {6.0f, 0.0f, 9.0f};
	const double weights[3] = {1.0, 1.0, 1.0};

	double pts[12];
	double dual[9];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 4; r.pointCount = 0;
	r.dual = dual; r.maxDual = 3; r.dualCount = 0;
	REQUIRE(dtStatusSucceed(query.findWeightedPath(s, e, path, 3, &filter, 1e-7, 20000, &r)));

	double portalA[6], portalB[6];
	portalEndpoints(sm.mesh, path[0], path[1], &portalA[0], &portalB[0]);
	portalEndpoints(sm.mesh, path[1], path[2], &portalA[3], &portalB[3]);
	double refPts[12];
	referenceSolve(s, e, portalA, portalB, 2, weights, refPts);
	double refCost = 0.0;
	for (int i = 0; i < 3; ++i)
		refCost += dist3(&refPts[i*3], &refPts[(i+1)*3]);
	CHECK(r.upperCost == Catch::Approx(refCost).epsilon(1e-8));
	// It must be no longer than the midpoint funnel polyline.
	double midPts[12];
	for (int k = 0; k < 3; ++k)
	{
		midPts[k] = s[k];
		midPts[9+k] = e[k];
		midPts[3+k] = 0.5*(portalA[k] + portalB[k]);
		midPts[6+k] = 0.5*(portalA[3+k] + portalB[3+k]);
	}
	double midCost = 0.0;
	for (int i = 0; i < 3; ++i)
		midCost += dist3(&midPts[i*3], &midPts[(i+1)*3]);
	CHECK(r.upperCost <= midCost + 1e-9);

	// Translated copy of the same geometry: crossing parameters and cost match.
	static const unsigned short tv[] = {
		0,0,0, 0,0,10, 2,0,0, 2,0,10, 5,0,0, 5,0,10, 7,0,0, 7,0,10
	};
	static const unsigned short tp[] = {
		0,1,3,2, 0xffff,0xffff,1,0xffff,
		2,3,5,4, 0,0xffff,2,0xffff,
		4,5,7,6, 1,0xffff,0xffff,0xffff
	};
	static const unsigned short tf[] = {1, 1, 1};
	static const unsigned char ta[] = {0, 0, 0};
	dtNavMesh tmesh;
	const float tbmin[3] = {100.0f, 0.0f, -50.0f};
	const float tbmax[3] = {107.0f, 1.0f, -40.0f};
	REQUIRE(buildSingleTile(tmesh, tv, 8, tp, 3, 4, tf, ta, tbmin, tbmax));
	dtNavMeshQuery tquery;
	REQUIRE(dtStatusSucceed(tquery.init(&tmesh, 64)));
	const dtPolyRef tbase = tmesh.getPolyRefBase(static_cast<const dtNavMesh&>(tmesh).getTile(0));
	const dtPolyRef tpath[3] = { tbase, tbase | 1, tbase | 2 };
	const float ts[3] = {101.0f, 0.0f, -49.0f};
	const float te[3] = {106.0f, 0.0f, -41.0f};
	double tpts[12], tdual[9];
	dtWeightedPathResult tr;
	tr.points = tpts; tr.maxPoints = 4; tr.pointCount = 0;
	tr.dual = tdual; tr.maxDual = 3; tr.dualCount = 0;
	REQUIRE(dtStatusSucceed(tquery.findWeightedPath(ts, te, tpath, 3, &filter, 1e-7, 20000, &tr)));
	CHECK(tr.upperCost == Catch::Approx(r.upperCost).epsilon(1e-9));
	for (int j = 0; j < 2; ++j)
	{
		double aa[3], bb[3];
		portalEndpoints(tmesh, tpath[j], tpath[j+1], aa, bb);
		const double tGot = portalParameter(&tpts[(j+1)*3], aa, bb);
		const double tOrig = portalParameter(&pts[(j+1)*3], &portalA[j*3], &portalB[j*3]);
		CHECK(tGot == Catch::Approx(tOrig).epsilon(1e-7));
	}

	// Uniformly scaled copy (cs = 3): the cost scales by exactly 3.
	dtNavMeshCreateParams p;
	std::memset(&p, 0, sizeof(p));
	static const unsigned short sv[] = {
		0,0,0, 0,0,10, 2,0,0, 2,0,10, 5,0,0, 5,0,10, 7,0,0, 7,0,10
	};
	p.verts = sv; p.vertCount = 8; p.polys = tp; p.polyCount = 3; p.nvp = 4;
	p.polyFlags = tf; p.polyAreas = ta;
	p.bmax[0] = 21.0f; p.bmax[1] = 3.0f; p.bmax[2] = 30.0f;
	p.cs = 3.0f; p.ch = 3.0f;
	p.walkableHeight = 2.0f; p.walkableClimb = 1.0f;
	unsigned char* data = 0; int size = 0;
	REQUIRE(dtCreateNavMeshData(&p, &data, &size));
	dtNavMesh smesh;
	REQUIRE(dtStatusSucceed(smesh.init(data, size, DT_TILE_FREE_DATA)));
	dtNavMeshQuery squery;
	REQUIRE(dtStatusSucceed(squery.init(&smesh, 64)));
	const dtPolyRef sbase = smesh.getPolyRefBase(static_cast<const dtNavMesh&>(smesh).getTile(0));
	const dtPolyRef spath[3] = { sbase, sbase | 1, sbase | 2 };
	const float ss[3] = {3.0f, 0.0f, 3.0f};
	const float se[3] = {18.0f, 0.0f, 27.0f};
	double spts[12], sdual[9];
	dtWeightedPathResult sr;
	sr.points = spts; sr.maxPoints = 4; sr.pointCount = 0;
	sr.dual = sdual; sr.maxDual = 3; sr.dualCount = 0;
	REQUIRE(dtStatusSucceed(squery.findWeightedPath(ss, se, spath, 3, &filter, 1e-7, 20000, &sr)));
	CHECK(sr.upperCost == Catch::Approx(3.0 * r.upperCost).epsilon(1e-7));
}

TEST_CASE("Tilted portals are measured with true 3D lengths", "[weighted][optimality]")
{
	// Two planar quads on the tilted plane y = 0.5*z sharing the portal
	// segment (4,0,0) -> (4,5,10), whose true 3D length is sqrt(125),
	// whereas its horizontal projection is only 10.
	static const unsigned short verts[] = {
		0,0,0, 0,5,10, 4,5,10, 4,0,0,
		4,0,0, 4,5,10, 8,5,10, 8,0,0
	};
	static const unsigned short polys[] = {
		0,1,2,3, 0xffff,0xffff,1,0xffff,
		4,5,6,7, 0,0xffff,0xffff,0xffff
	};
	static const unsigned short flags[] = {1, 1};
	static const unsigned char areas[] = {0, 1};
	const float bmin[3] = {0, 0, 0};
	const float bmax[3] = {8, 5, 10};
	dtNavMesh mesh;
	REQUIRE(buildSingleTile(mesh, verts, 8, polys, 2, 4, flags, areas, bmin, bmax));
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 3.0f);

	const dtPolyRef base = mesh.getPolyRefBase(static_cast<const dtNavMesh&>(mesh).getTile(0));
	const dtPolyRef path[2] = { base, base | 1 };
	const float s[3] = {1.0f, 1.0f, 2.0f};   // y = 0.5*z
	const float e[3] = {6.0f, 3.0f, 6.0f};   // y = 0.5*z

	double pts[9], dual[6];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 3; r.pointCount = 0;
	r.dual = dual; r.maxDual = 2; r.dualCount = 0;
	const dtStatus st = query.findWeightedPath(s, e, path, 2, &filter, 1e-8, 20000, &r);
	REQUIRE(dtStatusSucceed(st));
	REQUIRE(finiteVec(pts, 9));

	double a[3], b[3];
	portalEndpoints(mesh, path[0], path[1], a, b);
	CHECK(dist3(a, b) == Catch::Approx(std::sqrt(125.0)).epsilon(1e-9));

	const double weights[2] = {1.0, 3.0};
	const double u3d = independentCost(r, weights);
	CHECK(u3d == Catch::Approx(r.upperCost).epsilon(1e-10));

	// The same polyline priced with horizontal-only lengths must differ,
	// proving the y component is actually part of the objective.
	double uxz = 0.0;
	for (int i = 0; i < 2; ++i)
	{
		const double dx = pts[(i+1)*3]-pts[i*3];
		const double dz = pts[(i+1)*3+2]-pts[i*3+2];
		uxz += weights[i] * std::sqrt(dx*dx + dz*dz);
	}
	CHECK(std::abs(u3d - uxz) > 1e-3);
}

// ---------------------------------------------------------------------------
// portals
// ---------------------------------------------------------------------------

TEST_CASE("Cross-tile portal uses the clipped link segment", "[weighted][portals]")
{
	dtNavMesh mesh;
	dtNavMeshParams params;
	std::memset(&params, 0, sizeof(params));
	params.orig[0] = 0.0f; params.orig[1] = 0.0f; params.orig[2] = 0.0f;
	params.tileWidth = 4.0f;
	params.tileHeight = 10.0f;
	params.maxTiles = 8;
	params.maxPolys = 8;
	REQUIRE(dtStatusSucceed(mesh.init(&params)));

	// Tile 0: full-height quad whose edge index 2 is the x+ portal edge.
	// Creation data encodes x+ as input dir 2 (the builder maps it to
	// runtime side 0); tile 1's x- edge uses input dir 0.
	static const unsigned short v0[] = { 0,0,0, 0,0,10, 4,0,10, 4,0,0 };
	static const unsigned short p0[] = { 0,1,2,3, 0xffff,0xffff,(DT_EXT_LINK|2),0xffff };
	static const unsigned short f0[] = {1};
	static const unsigned char a0[] = {0};
	const float bmin0[3] = {0, 0, 0};
	const float bmax0[3] = {4, 1, 10};
	REQUIRE(buildTile(mesh, v0, 4, p0, 1, 4, f0, a0, bmin0, bmax0, 0, 0));

	// Tile 1: quad covering only z in [3,7] of the shared edge.
	static const unsigned short v1[] = { 0,0,3, 0,0,7, 2,0,7, 2,0,3 };
	static const unsigned short p1[] = { 0,1,2,3, (DT_EXT_LINK|0),0xffff,0xffff,0xffff };
	static const unsigned short f1[] = {1};
	static const unsigned char a1[] = {1};
	const float bmin1[3] = {4, 0, 0};
	const float bmax1[3] = {6, 1, 10};
	REQUIRE(buildTile(mesh, v1, 4, p1, 1, 4, f1, a1, bmin1, bmax1, 1, 0));

	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 4.0f);

	const dtPolyRef base0 = mesh.getPolyRefBase(mesh.getTileAt(0, 0, 0));
	const dtPolyRef base1 = mesh.getPolyRefBase(mesh.getTileAt(1, 0, 0));
	const dtPolyRef path[2] = { base0, base1 };
	const float s[3] = {1.0f, 0.0f, 1.0f};
	const float e[3] = {5.0f, 0.0f, 5.0f};

	double pts[9], dual[6];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 3; r.pointCount = 0;
	r.dual = dual; r.maxDual = 2; r.dualCount = 0;
	const dtStatus st = query.findWeightedPath(s, e, path, 2, &filter, 1e-8, 20000, &r);
	REQUIRE(dtStatusSucceed(st));

	// The crossing must lie on the clipped portal, not the full tile edge.
	// Edge ordering follows the polygon vertices, so the low-z end may be
	// stored in either slot; check the physical interval instead.
	double left[3], right[3];
	portalEndpoints(mesh, path[0], path[1], left, right);
	const double zLow = std::min(left[2], right[2]);
	const double zHigh = std::max(left[2], right[2]);
	CHECK(zLow == Catch::Approx(3.0).margin(0.03));
	CHECK(zHigh == Catch::Approx(7.0).margin(0.03));
	CHECK(left[0] == Catch::Approx(4.0).margin(1e-6));
	CHECK(right[0] == Catch::Approx(4.0).margin(1e-6));
	const double t = portalParameter(&pts[3], left, right);
	CHECK(t >= -1e-9);
	CHECK(t <= 1.0 + 1e-9);
	const double expected[3] = {
		left[0] + (right[0]-left[0]) * t,
		left[1] + (right[1]-left[1]) * t,
		left[2] + (right[2]-left[2]) * t
	};
	CHECK(dist3(&pts[3], expected) <= 1e-9);
}

TEST_CASE("Coincident portal points and zero-length segments stay feasible", "[weighted][portals]")
{
	// Three triangles fanning out of a shared vertex V (index 0). The two
	// corridor portals both contain V, so with an expensive middle polygon
	// both crossings collapse onto V: consecutive identical crossing points
	// with a zero-length middle segment.
	// Local vertex coordinates (world = bmin + local, cs = 1):
	// V=(6,0,6) is the shared fan vertex; world V is (0,0,0).
	static const unsigned short verts[] = {
		6,0,6,       // 0: V -> world (0,0,0)
		0,0,12,      // 1 -> world (-6,0,6)
		6,0,16,      // 2 -> world (0,0,10)
		12,0,12,     // 3 -> world (6,0,6)
		12,0,0       // 4 -> world (6,0,-6)
	};
	static const unsigned short polys[] = {
		0,1,2, 0xffff,0xffff,1,
		0,2,3, 0,0xffff,2,
		0,3,4, 1,0xffff,0xffff
	};
	static const unsigned short flags[] = {1, 1, 1};
	static const unsigned char areas[] = {0, 1, 0};
	const float bmin[3] = {-6, 0, -6};
	const float bmax[3] = {6, 1, 10};
	dtNavMesh mesh;
	REQUIRE(buildSingleTile(mesh, verts, 5, polys, 3, 3, flags, areas, bmin, bmax));
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 20.0f);

	const dtPolyRef base = mesh.getPolyRefBase(static_cast<const dtNavMesh&>(mesh).getTile(0));
	const dtPolyRef path[3] = { base, base | 1, base | 2 };
	// Interior points of the first and last triangles (world coordinates).
	const float s[3] = {-2.0f, 0.0f, 5.0f};
	const float e[3] = {4.0f, 0.0f, 0.0f};
	const float sharedV[3] = {0.0f, 0.0f, 0.0f};

	double pts[12], dual[9];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 4; r.pointCount = 0;
	r.dual = dual; r.maxDual = 3; r.dualCount = 0;
	const dtStatus st = query.findWeightedPath(s, e, path, 3, &filter, 1e-7, 20000, &r);
	REQUIRE(dtStatusSucceed(st));
	REQUIRE(r.pointCount == 4);
	REQUIRE(r.dualCount == 3);
	REQUIRE(finiteVec(pts, 12));
	REQUIRE(finiteVec(dual, 9));

	// Every crossing lies on its assigned portal, including clipping bounds.
	for (int j = 0; j < 2; ++j)
	{
		double aa[3], bb[3];
		portalEndpoints(mesh, path[j], path[j+1], aa, bb);
		const double tj = portalParameter(&pts[(j+1)*3], aa, bb);
		CHECK(tj >= -1e-9);
		CHECK(tj <= 1.0 + 1e-9);
		double on[3];
		for (int k = 0; k < 3; ++k) on[k] = aa[k] + (bb[k]-aa[k]) * tj;
		CHECK(dist3(on, &pts[(j+1)*3]) <= 1e-9);
	}

	// Both crossings coincide with the shared vertex V and with each other.
	CHECK(dist3(&pts[3], &pts[6]) <= 1e-7);
	CHECK(dist3(&pts[3], sharedV) <= 1e-7);

	// The middle (zero-length) segment produces a finite, feasible zero
	// vector rather than a NaN direction, and U recomputes independently.
	const double weights[3] = {1.0, 20.0, 1.0};
	const double u = independentCost(r, weights);
	CHECK(std::isfinite(u));
	CHECK(u == Catch::Approx(r.upperCost).epsilon(1e-12));
	CHECK(dist3(&pts[3], &pts[6]) == Catch::Approx(0.0).margin(1e-9));
	for (int i = 0; i < 3; ++i)
	{
		const double nrm = std::sqrt(
			dual[i*3]*dual[i*3] + dual[i*3+1]*dual[i*3+1] + dual[i*3+2]*dual[i*3+2]);
		CHECK(std::isfinite(nrm));
		CHECK(nrm <= weights[i] + 1e-10);
	}
	// The middle segment is (numerically) zero-length and its weighted
	// contribution to U is negligible; U matches the two outer segments.
	const double middleLen = dist3(&pts[3], &pts[6]);
	CHECK(middleLen == Catch::Approx(0.0).margin(1e-9));
	CHECK(20.0 * middleLen <= 1e-6);
	CHECK(std::abs(r.upperCost - (dist3(s, &pts[3]) + dist3(&pts[6], e))) <= 1e-6);
}

// ---------------------------------------------------------------------------
// certificate
// ---------------------------------------------------------------------------

TEST_CASE("Upper cost, lower bound and vector norms verify independently", "[weighted][certificate]")
{
	StripMesh sm;
	initStrip(sm);
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&sm.mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 4.0f);

	const dtPolyRef path[3] = { sm.base, sm.base | 1, sm.base | 2 };
	const float s[3] = {1.0f, 0.0f, 1.0f};
	const float e[3] = {6.0f, 0.0f, 9.0f};
	const double weights[3] = {1.0, 4.0, 1.0};

	double pts[12], dual[9];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 4; r.pointCount = 0;
	r.dual = dual; r.maxDual = 3; r.dualCount = 0;
	REQUIRE(dtStatusSucceed(query.findWeightedPath(s, e, path, 3, &filter, 1e-6, 20000, &r)));

	// U from the raw points and filter area costs.
	const double u = independentCost(r, weights);
	CHECK(u == Catch::Approx(r.upperCost).epsilon(1e-12));

	// D straight from the certificate formula and independently read portals.
	const double d = independentLowerBound(sm.mesh, path, 3, r);
	CHECK(d == Catch::Approx(r.lowerCost).epsilon(1e-10));
	CHECK(r.lowerCost <= r.upperCost + 1e-9);
	CHECK(r.upperCost - r.lowerCost <= 1e-6);

	// |q_i| <= w_i, with a single global feasibility slack.
	const double maxWeight = 4.0;
	const double scale = 1e-8 * std::max(1.0, std::max(r.upperCost, maxWeight));
	for (int i = 0; i < 3; ++i)
	{
		const double nrm = std::sqrt(dual[i*3]*dual[i*3] + dual[i*3+1]*dual[i*3+1] + dual[i*3+2]*dual[i*3+2]);
		CHECK(nrm <= weights[i] + scale);
		CHECK(std::isfinite(nrm));
	}

	// Endpoints are used verbatim; nothing is snapped.
	for (int k = 0; k < 3; ++k)
	{
		CHECK(pts[k] == Catch::Approx(s[k]).margin(1e-12));
		CHECK(pts[9+k] == Catch::Approx(e[k]).margin(1e-12));
	}
}

TEST_CASE("Exhausted iteration budget returns in-progress with a full certificate", "[weighted][certificate]")
{
	StripMesh sm;
	initStrip(sm);
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&sm.mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 4.0f);

	const dtPolyRef path[3] = { sm.base, sm.base | 1, sm.base | 2 };
	const float s[3] = {1.0f, 0.0f, 1.0f};
	const float e[3] = {6.0f, 0.0f, 9.0f};

	for (int budget = 0; budget <= 1; ++budget)
	{
		double pts[12], dual[9];
		dtWeightedPathResult r;
		r.points = pts; r.maxPoints = 4; r.pointCount = 0;
		r.dual = dual; r.maxDual = 3; r.dualCount = 0;
		const dtStatus st = query.findWeightedPath(s, e, path, 3, &filter, 1e-10, budget, &r);
		CHECK(dtStatusInProgress(st));
		CHECK(!dtStatusSucceed(st));
		// Complete, feasible evidence is still published.
		CHECK(r.pointCount == 4);
		CHECK(r.dualCount == 3);
		CHECK(finiteVec(pts, 12));
		CHECK(finiteVec(dual, 9));
		CHECK(r.upperCost >= r.lowerCost);
		// The honest gap misses the demanded tolerance.
		CHECK(r.upperCost - r.lowerCost > 1e-10);
		// The same points and vectors independently reproduce both bounds.
		const double weights[3] = {1.0, 4.0, 1.0};
		const double u = independentCost(r, weights);
		const double d = independentLowerBound(sm.mesh, path, 3, r);
		CHECK(u == Catch::Approx(r.upperCost).epsilon(1e-12));
		CHECK(d == Catch::Approx(r.lowerCost).epsilon(1e-10));
	}

	// Zero budget on a corridor without portals is provably optimal.
	double pts[6], dual[3];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 2; r.pointCount = 0;
	r.dual = dual; r.maxDual = 1; r.dualCount = 0;
	const dtStatus st = query.findWeightedPath(s, e, &path[0], 1, &filter, 1e-10, 0, &r);
	CHECK(dtStatusSucceed(st));
	CHECK(r.pointCount == 2);
	CHECK(r.dualCount == 1);
	CHECK(r.upperCost - r.lowerCost <= 1e-10);
}

TEST_CASE("A corridor revisiting the same polygon reference is handled", "[weighted][certificate]")
{
	StripMesh sm;
	initStrip(sm);
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&sm.mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 4.0f);

	// Walk into the middle polygon and straight back out: A, B, A. Both
	// directed links exist, and the same polyRef appears twice.
	const dtPolyRef path[3] = { sm.base, sm.base | 1, sm.base };
	const float s[3] = {1.0f, 0.0f, 2.0f};
	const float e[3] = {1.5f, 0.0f, 8.0f};

	double pts[12], dual[9];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 4; r.pointCount = 0;
	r.dual = dual; r.maxDual = 3; r.dualCount = 0;
	const dtStatus st = query.findWeightedPath(s, e, path, 3, &filter, 1e-7, 20000, &r);
	REQUIRE(dtStatusSucceed(st));
	CHECK(r.pointCount == 4);
	CHECK(r.dualCount == 3);
	CHECK(finiteVec(pts, 12));
	CHECK(finiteVec(dual, 9));
	for (int j = 0; j < 2; ++j)
	{
		double aa[3], bb[3];
		portalEndpoints(sm.mesh, path[j], path[j+1], aa, bb);
		const double tj = portalParameter(&pts[(j+1)*3], aa, bb);
		CHECK(tj >= -1e-9);
		CHECK(tj <= 1.0 + 1e-9);
	}
}

// ---------------------------------------------------------------------------
// integration
// ---------------------------------------------------------------------------

TEST_CASE("Path corridor runs the weighted query and refreshes after moves", "[weighted][integration]")
{
	StripMesh sm;
	initStrip(sm);
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&sm.mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 4.0f);

	dtPathCorridor corridor;
	REQUIRE(corridor.init(8));
	const float pos[3] = {1.0f, 0.0f, 1.0f};
	const float target[3] = {6.0f, 0.0f, 9.0f};
	corridor.reset(sm.base, pos);
	dtPolyRef path[3] = { sm.base, sm.base | 1, sm.base | 2 };
	corridor.setCorridor(target, path, 3);

	double pts[12], dual[9];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 4; r.pointCount = 0;
	r.dual = dual; r.maxDual = 3; r.dualCount = 0;
	dtStatus st = corridor.findWeightedPath(&query, &filter, 1e-7, 20000, &r);
	REQUIRE(dtStatusSucceed(st));
	CHECK(r.pointCount == 4);
	CHECK(r.upperCost < 19.929);
	CHECK(r.upperCost > 19.0);
	for (int k = 0; k < 3; ++k)
	{
		CHECK(pts[k] == Catch::Approx(pos[k]).margin(1e-12));
		CHECK(pts[9+k] == Catch::Approx(target[k]).margin(1e-12));
	}
	const double firstCost = r.upperCost;

	// The corridor's own path must be untouched.
	REQUIRE(corridor.getPathCount() == 3);
	CHECK(corridor.getPath()[0] == sm.base);
	CHECK(corridor.getPath()[2] == (sm.base | 2));

	// Move both ends inside their polygons and solve again; the new endpoint
	// coordinates must be consumed and no old crossing solution reused.
	const float newPos[3] = {1.5f, 0.0f, 2.0f};
	const float newTarget[3] = {5.5f, 0.0f, 8.0f};
	CHECK(corridor.movePosition(newPos, &query, &filter));
	CHECK(corridor.moveTargetPosition(newTarget, &query, &filter));

	dtWeightedPathResult r2;
	double pts2[12], dual2[9];
	r2.points = pts2; r2.maxPoints = 4; r2.pointCount = 0;
	r2.dual = dual2; r2.maxDual = 3; r2.dualCount = 0;
	st = corridor.findWeightedPath(&query, &filter, 1e-7, 20000, &r2);
	REQUIRE(dtStatusSucceed(st));
	for (int k = 0; k < 3; ++k)
	{
		CHECK(pts2[k] == Catch::Approx(corridor.getPos()[k]).margin(1e-12));
		CHECK(pts2[9+k] == Catch::Approx(corridor.getTarget()[k]).margin(1e-12));
	}
	CHECK(r2.upperCost != Catch::Approx(firstCost).epsilon(1e-6));

	// Repeating the identical query is deterministic.
	dtWeightedPathResult r3;
	double pts3[12], dual3[9];
	r3.points = pts3; r3.maxPoints = 4; r3.pointCount = 0;
	r3.dual = dual3; r3.maxDual = 3; r3.dualCount = 0;
	REQUIRE(dtStatusSucceed(corridor.findWeightedPath(&query, &filter, 1e-7, 20000, &r3)));
	for (int i = 0; i < 12; ++i) CHECK(pts3[i] == pts2[i]);
	for (int i = 0; i < 9; ++i) CHECK(dual3[i] == dual2[i]);
	CHECK(r3.upperCost == r2.upperCost);
	CHECK(r3.lowerCost == r2.lowerCost);

	// findCorners keeps its original behaviour.
	float cornerVerts[24];
	unsigned char cornerFlags[8];
	dtPolyRef cornerPolys[8];
	const int corners = corridor.findCorners(cornerVerts, cornerFlags, cornerPolys, 8, &query, &filter);
	CHECK(corners >= 1);
}

TEST_CASE("Invalid corridors, filters, buffers and parameters fail cleanly", "[weighted][integration]")
{
	StripMesh sm;
	initStrip(sm);
	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&sm.mesh, 64)));
	dtQueryFilter filter;
	filter.setAreaCost(1, 4.0f);

	const dtPolyRef good[3] = { sm.base, sm.base | 1, sm.base | 2 };
	const float s[3] = {1.0f, 0.0f, 1.0f};
	const float e[3] = {6.0f, 0.0f, 9.0f};

	double pts[12], dual[9];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 4; r.pointCount = 0;
	r.dual = dual; r.maxDual = 3; r.dualCount = 0;

	auto resetResult = [&]() {
		r.pointCount = r.dualCount = 0;
		r.upperCost = r.lowerCost = 0.0;
	};

	// Null result and null arrays.
	CHECK(dtStatusFailed(query.findWeightedPath(s, e, good, 3, &filter, 1e-7, 10, 0)));
	dtWeightedPathResult bad;
	bad.points = 0; bad.maxPoints = 4; bad.dual = dual; bad.maxDual = 3;
	bad.pointCount = bad.dualCount = 0;
	CHECK(dtStatusFailed(query.findWeightedPath(s, e, good, 3, &filter, 1e-7, 10, &bad)));
	bad.points = pts; bad.dual = 0;
	CHECK(dtStatusFailed(query.findWeightedPath(s, e, good, 3, &filter, 1e-7, 10, &bad)));

	// Bad tolerance and iteration budget.
	resetResult();
	CHECK(dtStatusFailed(query.findWeightedPath(s, e, good, 3, &filter, 0.0, 10, &r)));
	CHECK(dtStatusFailed(query.findWeightedPath(s, e, good, 3, &filter, -1.0, 10, &r)));
	CHECK(dtStatusFailed(query.findWeightedPath(s, e, good, 3, &filter, DBL_MAX, 10, &r)));
	CHECK(dtStatusFailed(query.findWeightedPath(s, e, good, 3, &filter,
		std::numeric_limits<double>::quiet_NaN(), 10, &r)));
	CHECK(dtStatusFailed(query.findWeightedPath(s, e, good, 3, &filter, 1e-7, -1, &r)));

	// Bad polygon reference.
	const dtPolyRef broken[3] = { sm.base, sm.base | 1, 0x99999999u };
	resetResult();
	dtStatus st = query.findWeightedPath(s, e, broken, 3, &filter, 1e-7, 10, &r);
	CHECK(dtStatusFailed(st));
	CHECK((st & DT_INVALID_PARAM) != 0);
	CHECK(r.pointCount == 0);
	CHECK(r.dualCount == 0);
	CHECK(r.upperCost == 0.0);
	CHECK(r.lowerCost == 0.0);

	// Disconnected corridor: two polygons with no link between them.
	const dtPolyRef disconnected[2] = { sm.base, sm.base | 2 };
	resetResult();
	st = query.findWeightedPath(s, e, disconnected, 2, &filter, 1e-7, 10, &r);
	CHECK(dtStatusFailed(st));
	CHECK((st & DT_INVALID_PARAM) != 0);
	CHECK(r.pointCount == 0);

	// Filter rejecting every polygon.
	dtQueryFilter locked;
	locked.setIncludeFlags(0xffff);
	locked.setExcludeFlags(0xffff);
	resetResult();
	st = query.findWeightedPath(s, e, good, 3, &locked, 1e-7, 10, &r);
	CHECK(dtStatusFailed(st));
	CHECK((st & DT_INVALID_PARAM) != 0);

	// Non-positive and non-finite area costs are rejected.
	dtQueryFilter zeroCost;
	zeroCost.setAreaCost(0, 0.0f);
	resetResult();
	st = query.findWeightedPath(s, e, good, 3, &zeroCost, 1e-7, 10, &r);
	CHECK(dtStatusFailed(st));
	CHECK((st & DT_INVALID_PARAM) != 0);
	dtQueryFilter infCost;
	infCost.setAreaCost(0, std::numeric_limits<float>::infinity());
	resetResult();
	st = query.findWeightedPath(s, e, good, 3, &infCost, 1e-7, 10, &r);
	CHECK(dtStatusFailed(st));
	CHECK((st & DT_INVALID_PARAM) != 0);

	// Short output buffers fail without publishing truncated evidence.
	dtWeightedPathResult small;
	double smallPts[6], smallVec[9];
	small.points = smallPts; small.maxPoints = 2; small.dual = smallVec; small.maxDual = 3;
	small.pointCount = 7; small.dualCount = 7; small.upperCost = 7.0; small.lowerCost = 7.0;
	st = query.findWeightedPath(s, e, good, 3, &filter, 1e-7, 10, &small);
	CHECK(dtStatusFailed(st));
	CHECK((st & DT_BUFFER_TOO_SMALL) != 0);
	CHECK(small.pointCount == 0);
	CHECK(small.dualCount == 0);
	CHECK(small.upperCost == 0.0);
	CHECK(small.lowerCost == 0.0);
	dtWeightedPathResult smallDual;
	double smallDualVec[3];
	smallDual.points = pts; smallDual.maxPoints = 4;
	smallDual.dual = smallDualVec; smallDual.maxDual = 1;
	smallDual.pointCount = 7; smallDual.dualCount = 7;
	st = query.findWeightedPath(s, e, good, 3, &filter, 1e-7, 10, &smallDual);
	CHECK(dtStatusFailed(st));
	CHECK((st & DT_BUFFER_TOO_SMALL) != 0);
	CHECK(smallDual.dualCount == 0);
}

TEST_CASE("Off-mesh polygons are rejected by the weighted query", "[weighted][integration]")
{
	static const unsigned short verts[] = {
		0,0,0, 0,0,10, 4,0,10, 4,0,0
	};
	static const unsigned short polys[] = {
		0,1,2,3, 0xffff,0xffff,0xffff,0xffff
	};
	static const unsigned short flags[] = {1};
	static const unsigned char areas[] = {0};
	const float bmin[3] = {0, 0, 0};
	const float bmax[3] = {4, 1, 10};

	dtNavMeshCreateParams p;
	std::memset(&p, 0, sizeof(p));
	p.verts = verts; p.vertCount = 4; p.polys = polys; p.polyCount = 1; p.nvp = 4;
	p.polyFlags = flags; p.polyAreas = areas;
	p.bmax[0] = 4; p.bmax[1] = 1; p.bmax[2] = 10;
	p.cs = 1; p.ch = 1; p.walkableHeight = 2; p.walkableClimb = 1;
	static const float conVerts[6] = { 1.0f, 0.0f, 2.0f, 3.0f, 0.0f, 8.0f };
	static const float conRad[1] = { 0.2f };
	static const unsigned short conFlags[1] = {1};
	static const unsigned char conAreas[1] = {0};
	static const unsigned char conDir[1] = { DT_OFFMESH_CON_BIDIR };
	p.offMeshConVerts = conVerts;
	p.offMeshConRad = conRad;
	p.offMeshConFlags = conFlags;
	p.offMeshConAreas = conAreas;
	p.offMeshConDir = conDir;
	p.offMeshConCount = 1;

	unsigned char* data = 0; int size = 0;
	REQUIRE(dtCreateNavMeshData(&p, &data, &size));
	dtNavMesh mesh;
	REQUIRE(dtStatusSucceed(mesh.init(data, size, DT_TILE_FREE_DATA)));
	const dtPolyRef base = mesh.getPolyRefBase(static_cast<const dtNavMesh&>(mesh).getTile(0));

	dtNavMeshQuery query;
	REQUIRE(dtStatusSucceed(query.init(&mesh, 64)));
	dtQueryFilter filter;

	const dtPolyRef path[2] = { base, base | 1 }; // second ref is the off-mesh poly
	const float s[3] = {1.0f, 0.0f, 1.0f};
	const float e[3] = {3.0f, 0.0f, 9.0f};

	double pts[9], dual[6];
	dtWeightedPathResult r;
	r.points = pts; r.maxPoints = 3; r.pointCount = 0;
	r.dual = dual; r.maxDual = 2; r.dualCount = 0;
	const dtStatus st = query.findWeightedPath(s, e, path, 2, &filter, 1e-7, 100, &r);
	CHECK(dtStatusFailed(st));
	CHECK((st & DT_INVALID_PARAM) != 0);
	CHECK(r.pointCount == 0);
	CHECK(r.dualCount == 0);
}
