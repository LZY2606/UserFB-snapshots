//
// Tests for dtNavMeshQuery::findPathTurnCost().
//
// The expected costs are never taken from the production search. Each case
// builds a small hand-authored navigation mesh, enumerates candidate polygon
// corridors with an independent graph walk in this file, scores the
// start/portal-mid/end polyline with its own distance and angle code, and only
// then compares the production result with that independent optimum.
//

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourAlloc.h"
#include "registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

// Every case captures the Registry as 'r'; this keeps the call sites terse
// while still going through Registry::check with a descriptive message.
#define TC_CHECK(cond) r.check((cond), #cond)

namespace
{
	static const double PI = 3.14159265358979323846;
	static const unsigned short MESH_NULL = 0xffff;
	static const unsigned short PORTAL_TOKEN = 0x8000;	// Builder border token.
	static const unsigned short BORDER_TOKEN = 0x800f;	// Builder border (wall).

	struct PolyDef
	{
		int verts[6];
		int neis[6];
		int nv;
		unsigned short flags;
		unsigned char area;
	};

	struct OffMeshDef
	{
		float a[3], b[3];
		float rad;
		unsigned short flags;
		unsigned char area;
	};

	struct TileDef
	{
		int tx, ty;
		float bmin[3], bmax[3];
		std::vector<float> verts;			// World-space vertices, 3 per vertex.
		std::vector<PolyDef> polys;
		std::vector<OffMeshDef> offMesh;
	};

	struct BuiltTile
	{
		int tx, ty;
		dtTileRef ref;
		dtPolyRef base;
		int polyCount;
	};

	struct BuiltMesh
	{
		dtNavMesh* nav;
		std::vector<BuiltTile> tiles;
		std::vector<PolyDef> polys;			// Concatenated polygon definitions.
		std::vector<int> tilePolyOffset;	// First global poly index of each tile.

		dtPolyRef refOf(const int globalPoly) const
		{
			int tile = 0;
			while (tile + 1 < (int)tilePolyOffset.size() &&
				   tilePolyOffset[tile + 1] <= globalPoly)
				++tile;
			return tiles[tile].base + (dtPolyRef)(globalPoly - tilePolyOffset[tile]);
		}
	};

	// One directed adjacency entry. The mid point is derived from the source
	// polygon's own edge, exactly like the production query does.
	struct Edge
	{
		int to;
		float mid[3];
	};

	struct RefGraph
	{
		std::vector<std::vector<Edge> > adj;
	};

	static void vlerp3(float* out, const float* a, const float* b, const float t)
	{
		out[0] = a[0] + (b[0]-a[0])*t;
		out[1] = a[1] + (b[1]-a[1])*t;
		out[2] = a[2] + (b[2]-a[2])*t;
	}

	static unsigned char* buildTileData(const TileDef& tile)
	{
		const int vertCount = (int)tile.verts.size() / 3;
		const int polyCount = (int)tile.polys.size();
		const int nvp = 6;

		std::vector<unsigned short> iv(vertCount*3);
		for (int i = 0; i < vertCount; ++i)
		{
			iv[i*3+0] = (unsigned short)(tile.verts[i*3+0] - tile.bmin[0]);
			iv[i*3+1] = (unsigned short)(tile.verts[i*3+1] - tile.bmin[1]);
			iv[i*3+2] = (unsigned short)(tile.verts[i*3+2] - tile.bmin[2]);
		}

		std::vector<unsigned short> pdata(polyCount*2*nvp, MESH_NULL);
		std::vector<unsigned short> pflags(polyCount);
		std::vector<unsigned char> parea(polyCount);
		for (int i = 0; i < polyCount; ++i)
		{
			const PolyDef& p = tile.polys[i];
			for (int j = 0; j < p.nv; ++j)
			{
				pdata[i*2*nvp + j] = (unsigned short)p.verts[j];
				pdata[i*2*nvp + nvp + j] = (unsigned short)p.neis[j];
			}
			pflags[i] = p.flags;
			parea[i] = p.area;
		}

		std::vector<float> omVerts;
		std::vector<float> omRad;
		std::vector<unsigned short> omFlags;
		std::vector<unsigned char> omArea, omDir;
		std::vector<unsigned int> omUserId;
		for (size_t i = 0; i < tile.offMesh.size(); ++i)
		{
			const OffMeshDef& o = tile.offMesh[i];
			omVerts.insert(omVerts.end(), o.a, o.a+3);
			omVerts.insert(omVerts.end(), o.b, o.b+3);
			omRad.push_back(o.rad);
			omFlags.push_back(o.flags);
			omArea.push_back(o.area);
			omDir.push_back(DT_OFFMESH_CON_BIDIR);
			omUserId.push_back((unsigned int)i);
		}

		dtNavMeshCreateParams cp;
		std::memset(&cp, 0, sizeof(cp));
		cp.verts = iv.data();
		cp.vertCount = vertCount;
		cp.polys = pdata.data();
		cp.polyFlags = pflags.data();
		cp.polyAreas = parea.data();
		cp.polyCount = polyCount;
		cp.nvp = nvp;
		cp.offMeshConVerts = omVerts.empty() ? 0 : omVerts.data();
		cp.offMeshConRad = omRad.empty() ? 0 : omRad.data();
		cp.offMeshConFlags = omFlags.empty() ? 0 : omFlags.data();
		cp.offMeshConAreas = omArea.empty() ? 0 : omArea.data();
		cp.offMeshConDir = omDir.empty() ? 0 : omDir.data();
		cp.offMeshConUserID = omUserId.empty() ? 0 : omUserId.data();
		cp.offMeshConCount = (int)tile.offMesh.size();
		cp.tileX = tile.tx;
		cp.tileY = tile.ty;
		cp.bmin[0] = tile.bmin[0]; cp.bmin[1] = tile.bmin[1]; cp.bmin[2] = tile.bmin[2];
		cp.bmax[0] = tile.bmax[0]; cp.bmax[1] = tile.bmax[1]; cp.bmax[2] = tile.bmax[2];
		cp.walkableHeight = 2.0f;
		cp.walkableRadius = 0.1f;
		cp.walkableClimb = 0.5f;
		cp.cs = 1.0f;
		cp.ch = 1.0f;
		cp.buildBvTree = false;

		unsigned char* data = 0;
		int size = 0;
		if (!dtCreateNavMeshData(&cp, &data, &size))
			return 0;
		return data;
	}

	static BuiltMesh* buildMesh(const std::vector<TileDef>& tiles,
								const float tileWidth, const float tileHeight,
								const int maxPolysPerTile)
	{
		BuiltMesh* bm = new BuiltMesh();
		bm->nav = dtAllocNavMesh();

		dtNavMeshParams params;
		std::memset(&params, 0, sizeof(params));
		params.tileWidth = tileWidth;
		params.tileHeight = tileHeight;
		params.maxTiles = 32;
		params.maxPolys = maxPolysPerTile;
		if (dtStatusFailed(bm->nav->init(&params)))
		{
			dtFreeNavMesh(bm->nav);
			delete bm;
			return 0;
		}

		int polyOffset = 0;
		for (size_t ti = 0; ti < tiles.size(); ++ti)
		{
			const TileDef& tile = tiles[ti];
			unsigned char* data = buildTileData(tile);
			if (!data)
			{
				delete bm;
				return 0;
			}
			dtTileRef tileRef = 0;
			if (dtStatusFailed(bm->nav->addTile(data, 0, DT_TILE_FREE_DATA, 0, &tileRef)))
			{
				dtFree(data);
				delete bm;
				return 0;
			}
			const dtMeshTile* loaded = bm->nav->getTileAt(tile.tx, tile.ty, 0);
			BuiltTile bt;
			bt.tx = tile.tx;
			bt.ty = tile.ty;
			bt.ref = tileRef;
			bt.base = bm->nav->getPolyRefBase(loaded);
			bt.polyCount = (int)tile.polys.size();
			bm->tiles.push_back(bt);
			bm->tilePolyOffset.push_back(polyOffset);
			bm->polys.insert(bm->polys.end(), tile.polys.begin(), tile.polys.end());
			polyOffset += (int)tile.polys.size();
		}
		return bm;
	}

	static void freeMesh(BuiltMesh* bm)
	{
		if (!bm) return;
		dtFreeNavMesh(bm->nav);
		delete bm;
	}

	// Independent adjacency graph. Internal edges use the source edge's own
	// mid point; cross-tile edges use the mid point of the actual overlapping
	// interval on the shared tile border.
	static RefGraph buildReferenceGraph(const BuiltMesh& bm,
										const std::vector<TileDef>& tiles)
	{
		const int n = (int)bm.polys.size();
		RefGraph g;
		g.adj.resize(n);

		for (size_t t = 0; t < tiles.size(); ++t)
		{
			const TileDef& tile = tiles[t];
			const int base = bm.tilePolyOffset[t];
			const float* tv = tile.verts.data();

			for (size_t pi = 0; pi < tile.polys.size(); ++pi)
			{
				const PolyDef& p = tile.polys[pi];
				const int from = base + (int)pi;

				for (int e = 0; e < p.nv; ++e)
				{
					const int token = p.neis[e];
					if (token == MESH_NULL || token == BORDER_TOKEN)
						continue;

					const int v0 = p.verts[e];
					const int v1 = p.verts[(e+1) % p.nv];
					const float* a = &tv[v0*3];
					const float* b = &tv[v1*3];

					Edge edge;
					if (!(token & PORTAL_TOKEN))
					{
						// Internal edge: token is the 0-based local poly index.
						edge.to = base + token;
						edge.mid[0] = (a[0]+b[0])*0.5f;
						edge.mid[1] = (a[1]+b[1])*0.5f;
						edge.mid[2] = (a[2]+b[2])*0.5f;
						g.adj[from].push_back(edge);
						continue;
					}

					// Builder input direction: 0=x-, 1=z+, 2=x+, 3=z-.
					const int dir = token & 0x000f;
					int nx = tile.tx, ny = tile.ty;
					if (dir == 0) --nx;
					else if (dir == 2) ++nx;
					else if (dir == 1) ++ny;
					else if (dir == 3) --ny;

					int ntIndex = -1;
					for (size_t k = 0; k < tiles.size(); ++k)
					{
						if (tiles[k].tx == nx && tiles[k].ty == ny)
						{
							ntIndex = (int)k;
							break;
						}
					}
					if (ntIndex < 0)
						continue;
					const TileDef& nt = tiles[ntIndex];

					// Interval of this edge along the border.
					const bool xFace = (dir == 0 || dir == 2);
					const float eLo = xFace ? std::min(a[2], b[2]) : std::min(a[0], b[0]);
					const float eHi = xFace ? std::max(a[2], b[2]) : std::max(a[0], b[0]);

					int foundTo = -1;
					float oLo = eLo, oHi = eHi;
					for (size_t q = 0; q < nt.polys.size(); ++q)
					{
						const PolyDef& np = nt.polys[q];
						for (int ne = 0; ne < np.nv; ++ne)
						{
							if (!(np.neis[ne] & PORTAL_TOKEN))
								continue;
							const float* na = &nt.verts[np.verts[ne]*3];
							const float* nb = &nt.verts[np.verts[(ne+1) % np.nv]*3];
							const float sLo = xFace ? std::min(na[2], nb[2]) : std::min(na[0], nb[0]);
							const float sHi = xFace ? std::max(na[2], nb[2]) : std::max(na[0], nb[0]);
							const float lo = std::max(eLo, sLo);
							const float hi = std::min(eHi, sHi);
							if (hi > lo)
							{
								oLo = lo;
								oHi = hi;
								foundTo = bm.tilePolyOffset[ntIndex] + (int)q;
								break;
							}
						}
						if (foundTo >= 0)
							break;
					}
					if (foundTo < 0)
						continue;
					edge.to = foundTo;

					// Parameterize the clipped interval back onto this edge.
					const float fromLo = xFace ? a[2] : a[0];
					const float fromHi = xFace ? b[2] : b[0];
					const float t0 = (oLo - fromLo) / (fromHi - fromLo);
					const float t1 = (oHi - fromLo) / (fromHi - fromLo);
					float pa[3], pb[3];
					vlerp3(pa, a, b, t0);
					vlerp3(pb, a, b, t1);
					vlerp3(edge.mid, pa, pb, 0.5f);
					g.adj[from].push_back(edge);
				}
			}
		}
		return g;
	}

	struct Candidate
	{
		std::vector<int> walk;
		double cost;
	};

	// Independent scoring of one corridor: 3D Euclidean segment lengths times
	// the area cost of the polygon containing the segment, plus turnWeight
	// times the squared angle between consecutive non-zero XZ directions.
	static double scoreWalk(const std::vector<int>& walk,
							const RefGraph& g,
							const float* startPos, const float* endPos,
							const std::vector<unsigned char>& areas,
							const dtQueryFilter& filter,
							const double startAngle, const double endAngle,
							const double turnWeight)
	{
		std::vector<float> pts;
		pts.push_back(startPos[0]); pts.push_back(startPos[1]); pts.push_back(startPos[2]);
		for (size_t i = 0; i + 1 < walk.size(); ++i)
		{
			const std::vector<Edge>& edges = g.adj[walk[i]];
			for (size_t k = 0; k < edges.size(); ++k)
			{
				if (edges[k].to == walk[i+1])
				{
					pts.push_back(edges[k].mid[0]);
					pts.push_back(edges[k].mid[1]);
					pts.push_back(edges[k].mid[2]);
					break;
				}
			}
		}
		pts.push_back(endPos[0]); pts.push_back(endPos[1]); pts.push_back(endPos[2]);

		const size_t pointCount = pts.size() / 3;
		double distCost = 0.0;
		double turnCost = 0.0;
		bool hasAngle = false;
		double angle = startAngle;

		for (size_t i = 0; i + 1 < pointCount; ++i)
		{
			const float* p0 = &pts[i*3];
			const float* p1 = &pts[(i+1)*3];
			const double dx = p1[0]-p0[0];
			const double dy = p1[1]-p0[1];
			const double dz = p1[2]-p0[2];
			distCost += std::sqrt(dx*dx + dy*dy + dz*dz) *
						(double)filter.getAreaCost(areas[walk[i]]);
			if (dx != 0.0 || dz != 0.0)
			{
				const double newAngle = std::atan2(dz, dx);
				const double prev = hasAngle ? angle : startAngle;
				double diff = newAngle - prev;
				while (diff > PI) diff -= 2.0*PI;
				while (diff < -PI) diff += 2.0*PI;
				turnCost += turnWeight * diff * diff;
				hasAngle = true;
				angle = newAngle;
			}
		}

		double diff = endAngle - (hasAngle ? angle : startAngle);
		while (diff > PI) diff -= 2.0*PI;
		while (diff < -PI) diff += 2.0*PI;
		turnCost += turnWeight * diff * diff;

		return distCost + turnCost;
	}

	struct Enumeration
	{
		std::vector<Candidate> candidates;
		double bestCost;
		std::vector<std::vector<int> > bestWalks;
	};

	// Depth-bounded DFS over the test graph. Polygons may be revisited.
	static Enumeration enumerate(const RefGraph& g, const int start, const int end,
								 const float* startPos, const float* endPos,
								 const std::vector<unsigned char>& areas,
								 const dtQueryFilter& filter,
								 const double startAngle, const double endAngle,
								 const double turnWeight, const int maxDepth)
	{
		Enumeration out;
		out.bestCost = std::numeric_limits<double>::infinity();

		std::vector<int> walk;
		struct Frame { int node; size_t nextEdge; };
		std::vector<Frame> stack;
		walk.push_back(start);
		stack.push_back(Frame{start, 0});

		while (!stack.empty())
		{
			Frame& top = stack.back();
			if (top.node == end)
			{
				Candidate c;
				c.walk = walk;
				c.cost = scoreWalk(walk, g, startPos, endPos, areas, filter,
									startAngle, endAngle, turnWeight);
				out.candidates.push_back(c);
				if (c.cost < out.bestCost - 1e-10)
				{
					out.bestCost = c.cost;
					out.bestWalks.clear();
					out.bestWalks.push_back(c.walk);
				}
				else if (c.cost <= out.bestCost + 1e-9)
				{
					out.bestWalks.push_back(c.walk);
				}
				stack.pop_back();
				walk.pop_back();
				continue;
			}
			if ((int)walk.size() >= maxDepth ||
				top.nextEdge >= g.adj[top.node].size())
			{
				stack.pop_back();
				walk.pop_back();
				continue;
			}

			const Edge& e = g.adj[top.node][top.nextEdge++];
			walk.push_back(e.to);
			stack.push_back(Frame{e.to, 0});
		}

		return out;
	}

	static double headingAngle(const float* h)
	{
		return std::atan2((double)h[2], (double)h[0]);
	}

	static bool approxEqual(const double a, const double b)
	{
		const double tol = std::max(1e-4, 1e-5 * std::max(std::fabs(a), std::fabs(b)));
		return std::fabs(a - b) <= tol;
	}

	static PolyDef quad(unsigned short a, unsigned short b, unsigned short c,
						unsigned short d, int na, int nb, int nc, int nd,
						unsigned char area = 0)
	{
		PolyDef p;
		p.verts[0] = a; p.verts[1] = b; p.verts[2] = c; p.verts[3] = d;
		p.verts[4] = p.verts[5] = MESH_NULL;
		p.neis[0] = na; p.neis[1] = nb; p.neis[2] = nc; p.neis[3] = nd;
		p.neis[4] = p.neis[5] = MESH_NULL;
		p.nv = 4;
		p.flags = 1;
		p.area = area;
		return p;
	}

	static std::vector<int> globalWalk(const BuiltMesh& bm, const dtPolyRef* path,
									   const int count)
	{
		std::vector<int> got;
		for (int i = 0; i < count; ++i)
			for (int gp = 0; gp < (int)bm.polys.size(); ++gp)
				if (bm.refOf(gp) == path[i]) { got.push_back(gp); break; }
		return got;
	}

	static bool walkIsOptimal(const std::vector<int>& got,
							  const std::vector<std::vector<int> >& bestWalks)
	{
		for (size_t i = 0; i < bestWalks.size(); ++i)
			if (bestWalks[i] == got)
				return true;
		return false;
	}

	// Branch mesh:
	//
	//  S ---- A ---- M ---- E
	//   \             /
	//    ---- B ------
	//
	// S=0, A=1, B=2, M=3, E=4. The two branches enter M through different
	// west-edge portals at different heights, so labels reaching M carry
	// different entry positions and both must be retained.
	static std::vector<TileDef> branchTiles()
	{
		TileDef t;
		t.tx = 0;
		t.ty = 0;
		t.bmin[0] = t.bmin[1] = t.bmin[2] = 0.0f;
		t.bmax[0] = 17.0f; t.bmax[1] = 1.0f; t.bmax[2] = 9.0f;
		const float v[24*3] = {
			0,0,0,  5,0,0,  5,0,3,  5,0,5,  5,0,8,  0,0,8,   // S  (0-5)
			5,0,0,  9,0,0,  9,0,3,  5,0,3,                    // A  (6-9)
			5,0,5,  9,0,5,  9,0,8,  5,0,8,                    // B  (10-13)
			9,0,0, 13,0,0, 13,0,8,  9,0,8,  9,0,5,  9,0,3,    // M  (14-19)
			13,0,0, 17,0,0, 17,0,8, 13,0,8                    // E  (20-23)
		};
		t.verts.assign(v, v+24*3);

		PolyDef s;
		s.nv = 6; s.flags = 1; s.area = 0;
		s.verts[0] = 0; s.verts[1] = 1; s.verts[2] = 2; s.verts[3] = 3;
		s.verts[4] = 4; s.verts[5] = 5;
		s.neis[0] = MESH_NULL; s.neis[1] = 1; s.neis[2] = MESH_NULL;
		s.neis[3] = 2; s.neis[4] = MESH_NULL; s.neis[5] = MESH_NULL;
		t.polys.push_back(s);

		t.polys.push_back(quad(6,7,8,9, MESH_NULL, 3, MESH_NULL, 0));
		t.polys.push_back(quad(10,11,12,13, MESH_NULL, 3, MESH_NULL, 0));

		PolyDef m;
		m.nv = 6; m.flags = 1; m.area = 0;
		m.verts[0] = 14; m.verts[1] = 15; m.verts[2] = 16; m.verts[3] = 17;
		m.verts[4] = 18; m.verts[5] = 19;
		m.neis[0] = MESH_NULL; m.neis[1] = 4; m.neis[2] = MESH_NULL;
		m.neis[3] = 2; m.neis[4] = MESH_NULL; m.neis[5] = 1;
		t.polys.push_back(m);

		t.polys.push_back(quad(20,21,22,23, MESH_NULL, MESH_NULL, MESH_NULL, 3));

		std::vector<TileDef> tiles;
		tiles.push_back(t);
		return tiles;
	}
}

// ---------------------------------------------------------------------------
// Registered cases
// ---------------------------------------------------------------------------

static void registerOptimality(Registry& r)
{
	r.add("optimality", "merge_prefix_overtake", [&r]()
	{
		std::vector<TileDef> tiles = branchTiles();
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		RefGraph g = buildReferenceGraph(*bm, tiles);
		TC_CHECK((int)g.adj.size() == 5);

		std::vector<unsigned char> areas;
		for (size_t i = 0; i < bm->polys.size(); ++i)
			areas.push_back(bm->polys[i].area);

		const float startPos[3] = {1.0f, 0.0f, 4.0f};
		const float endPos[3] = {16.0f, 0.0f, 6.5f};
		// Start heading aligned with the B branch: B gets the cheap prefix,
		// while the end heading rewards the A branch's geometry later.
		const float startHeading[3] = {1.0f, 0.0f, 0.625f};
		const float endHeading[3] = {1.0f, 0.0f, 0.83f};
		const double sa = headingAngle(startHeading);
		const double ea = headingAngle(endHeading);

		const Enumeration ref = enumerate(g, 0, 4, startPos, endPos, areas,
										  filter, sa, ea, 8.0, 9);
		TC_CHECK(!ref.bestWalks.empty());

		// Cheapest prefix of any corridor the moment it first reaches M (3).
		double cheapestPrefixAtM = std::numeric_limits<double>::infinity();
		for (size_t i = 0; i < ref.candidates.size(); ++i)
		{
			const std::vector<int>& w = ref.candidates[i].walk;
			for (size_t k = 1; k < w.size(); ++k)
			{
				if (w[k] == 3)
				{
					std::vector<int> prefix(w.begin(), w.begin() + k + 1);
					cheapestPrefixAtM = std::min(
						cheapestPrefixAtM,
						scoreWalk(prefix, g, startPos, endPos, areas, filter,
								  sa, ea, 8.0));
					break;
				}
			}
		}

		const std::vector<int>& winner = ref.bestWalks.front();
		double winnerPrefixAtM = std::numeric_limits<double>::infinity();
		for (size_t k = 1; k < winner.size(); ++k)
		{
			if (winner[k] == 3)
			{
				std::vector<int> prefix(winner.begin(), winner.begin() + k + 1);
				winnerPrefixAtM = scoreWalk(prefix, g, startPos, endPos, areas,
											filter, sa, ea, 8.0);
				break;
			}
		}
		// The globally optimal corridor did not have the cheapest prefix at
		// the merge polygon: discarding the costlier label would lose.
		TC_CHECK(winnerPrefixAtM > cheapestPrefixAtM + 1e-6);

		dtPolyRef path[16];
		int count = 0;
		float totalCost = -1.0f;
		dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
										 startPos, endPos, &filter,
										 path, &count, 16,
										 startHeading, endHeading, 8.0f,
										 &totalCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(count >= 4);
		TC_CHECK(approxEqual(totalCost, ref.bestCost));
		TC_CHECK(walkIsOptimal(globalWalk(*bm, path, count), ref.bestWalks));
		TC_CHECK(count < 9);
		freeMesh(bm);
	});

	r.add("optimality", "headings_change_route", [&r]()
	{
		std::vector<TileDef> tiles = branchTiles();
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		RefGraph g = buildReferenceGraph(*bm, tiles);
		std::vector<unsigned char> areas;
		for (size_t i = 0; i < bm->polys.size(); ++i)
			areas.push_back(bm->polys[i].area);

		const float startPos[3] = {1.0f, 0.0f, 4.0f};
		const float endPos[3] = {16.0f, 0.0f, 4.0f};
		const float hUp[3] = {1.0f, 0.0f, -1.0f};
		const float hDown[3] = {1.0f, 0.0f, 1.0f};

		struct Run { const float* sh; const float* eh; };
		Run runs[2] = { {hUp, hUp}, {hDown, hDown} };
		std::vector<std::vector<int> > chosen;

		for (int run = 0; run < 2; ++run)
		{
			const double sa = headingAngle(runs[run].sh);
			const double ea = headingAngle(runs[run].eh);
			const Enumeration ref = enumerate(g, 0, 4, startPos, endPos, areas,
											  filter, sa, ea, 6.0, 9);
			dtPolyRef path[16];
			int count = 0;
			float totalCost = 0.0f;
			dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
											 startPos, endPos, &filter,
											 path, &count, 16,
											 runs[run].sh, runs[run].eh,
											 6.0f, &totalCost);
			TC_CHECK(dtStatusSucceed(st));
			TC_CHECK(approxEqual(totalCost, ref.bestCost));
			const std::vector<int> got = globalWalk(*bm, path, count);
			TC_CHECK(walkIsOptimal(got, ref.bestWalks));
			chosen.push_back(got);
		}
		TC_CHECK(chosen[0] != chosen[1]);
		freeMesh(bm);
	});

	r.add("optimality", "area_cost_weights_distance", [&r]()
	{
		std::vector<TileDef> tiles = branchTiles();
		tiles[0].polys[2].area = 1;	// B becomes the expensive area.
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		filter.setAreaCost(1, 4.0f);
		RefGraph g = buildReferenceGraph(*bm, tiles);
		std::vector<unsigned char> areas;
		for (size_t i = 0; i < bm->polys.size(); ++i)
			areas.push_back(bm->polys[i].area);

		const float startPos[3] = {1.0f, 0.0f, 4.0f};
		const float endPos[3] = {16.0f, 0.0f, 4.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};
		const double sa = headingAngle(heading);
		const Enumeration ref = enumerate(g, 0, 4, startPos, endPos, areas,
										  filter, sa, sa, 0.0, 9);
		dtPolyRef path[16];
		int count = 0;
		float totalCost = 0.0f;
		dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
										 startPos, endPos, &filter,
										 path, &count, 16,
										 heading, heading, 0.0f, &totalCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(approxEqual(totalCost, ref.bestCost));
		const std::vector<int> got = globalWalk(*bm, path, count);
		bool usesA = false, usesB = false;
		for (size_t i = 0; i < got.size(); ++i)
		{
			if (got[i] == 1) usesA = true;
			if (got[i] == 2) usesB = true;
		}
		TC_CHECK(usesA && !usesB);
		freeMesh(bm);
	});

	r.add("optimality", "repeated_polygons_are_allowed", [&r]()
	{
		std::vector<TileDef> tiles = branchTiles();
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		RefGraph g = buildReferenceGraph(*bm, tiles);
		std::vector<unsigned char> areas;
		for (size_t i = 0; i < bm->polys.size(); ++i)
			areas.push_back(bm->polys[i].area);

		const float startPos[3] = {1.0f, 0.0f, 4.0f};
		const float endPos[3] = {16.0f, 0.0f, 4.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};
		const double sa = headingAngle(heading);
		const Enumeration ref = enumerate(g, 0, 4, startPos, endPos, areas,
										  filter, sa, sa, 0.2, 10);
		bool sawRepeat = false;
		for (size_t i = 0; i < ref.candidates.size(); ++i)
		{
			std::set<int> uniq(ref.candidates[i].walk.begin(),
							   ref.candidates[i].walk.end());
			if ((int)uniq.size() != (int)ref.candidates[i].walk.size())
				sawRepeat = true;
		}
		TC_CHECK(sawRepeat);

		dtPolyRef path[16];
		int count = 0;
		float totalCost = 0.0f;
		dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
										 startPos, endPos, &filter,
										 path, &count, 16,
										 heading, heading, 0.2f, &totalCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(approxEqual(totalCost, ref.bestCost));
		TC_CHECK(walkIsOptimal(globalWalk(*bm, path, count), ref.bestWalks));
		freeMesh(bm);
	});
}

static void registerPortals(Registry& r)
{
	r.add("portals", "clipped_cross_tile_midpoint", [&r]()
	{
		// Two tiles sharing x=10. Tile 0 edge spans z in [0,10], tile 1 edge
		// spans z in [4,8]: the connected portal is [4,8], mid z=6, not the
		// full-edge mid z=5.
		TileDef t0;
		t0.tx = 0; t0.ty = 0;
		t0.bmin[0] = t0.bmin[1] = t0.bmin[2] = 0.0f;
		t0.bmax[0] = 10.0f; t0.bmax[1] = 1.0f; t0.bmax[2] = 10.0f;
		const float v0[4*3] = {0,0,0, 10,0,0, 10,0,10, 0,0,10};
		t0.verts.assign(v0, v0+12);
		// x+ face uses builder input direction 2.
		t0.polys.push_back(quad(0,1,2,3, MESH_NULL, PORTAL_TOKEN|2,
								MESH_NULL, MESH_NULL));

		TileDef t1;
		t1.tx = 1; t1.ty = 0;
		t1.bmin[0] = 10.0f; t1.bmin[1] = 0.0f; t1.bmin[2] = 4.0f;
		t1.bmax[0] = 20.0f; t1.bmax[1] = 1.0f; t1.bmax[2] = 8.0f;
		const float v1[4*3] = {10,0,4, 20,0,4, 20,0,8, 10,0,8};
		t1.verts.assign(v1, v1+12);
		// x- face (v3->v0) uses builder input direction 0.
		t1.polys.push_back(quad(0,1,2,3, MESH_NULL, MESH_NULL,
								MESH_NULL, PORTAL_TOKEN|0));

		std::vector<TileDef> tiles;
		tiles.push_back(t0); tiles.push_back(t1);
		BuiltMesh* bm = buildMesh(tiles, 10.0f, 10.0f, 8);
		TC_CHECK(bm != 0);
		TC_CHECK(bm->tiles.size() == 2);

		const dtMeshTile* loaded0 = bm->nav->getTileAt(0, 0, 0);
		bool foundClipped = false;
		for (unsigned int i = loaded0->polys[0].firstLink; i != DT_NULL_LINK;
			 i = loaded0->links[i].next)
		{
			if (loaded0->links[i].ref == bm->refOf(1))
			{
				TC_CHECK(loaded0->links[i].bmin == 102);
				TC_CHECK(loaded0->links[i].bmax == 204);
				foundClipped = true;
			}
		}
		TC_CHECK(foundClipped);

		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 128)));
		dtQueryFilter filter;
		RefGraph g = buildReferenceGraph(*bm, tiles);
		TC_CHECK(g.adj[0].size() == 1);
		TC_CHECK(g.adj[0][0].mid[0] == 10.0f);
		TC_CHECK(g.adj[0][0].mid[2] == 6.0f);

		std::vector<unsigned char> areas(2, 0);
		const float startPos[3] = {2.0f, 0.0f, 6.0f};
		const float endPos[3] = {18.0f, 0.0f, 6.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};
		const double sa = headingAngle(heading);
		const Enumeration ref = enumerate(g, 0, 1, startPos, endPos, areas,
										  filter, sa, sa, 1.0, 6);
		TC_CHECK(!ref.bestWalks.empty());

		dtPolyRef path[8];
		int count = 0;
		float totalCost = 0.0f;
		dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(1),
										 startPos, endPos, &filter,
										 path, &count, 8,
										 heading, heading, 1.0f, &totalCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(count == 2);
		TC_CHECK(path[0] == bm->refOf(0) && path[1] == bm->refOf(1));
		TC_CHECK(approxEqual(totalCost, ref.bestCost));

		// The naive full-edge midpoint (z=5) would score differently.
		const double wrongAngle = std::atan2(1.0, 8.0);
		const double wrongCost = 16.0 + 2.0 * wrongAngle * wrongAngle;
		TC_CHECK(!approxEqual(wrongCost, ref.bestCost));
		freeMesh(bm);
	});

	r.add("portals", "full_edge_cross_tile_midpoint", [&r]()
	{
		TileDef t0;
		t0.tx = 0; t0.ty = 0;
		t0.bmin[0] = t0.bmin[1] = t0.bmin[2] = 0.0f;
		t0.bmax[0] = 10.0f; t0.bmax[1] = 1.0f; t0.bmax[2] = 10.0f;
		const float v0[4*3] = {0,0,0, 10,0,0, 10,0,10, 0,0,10};
		t0.verts.assign(v0, v0+12);
		t0.polys.push_back(quad(0,1,2,3, MESH_NULL, PORTAL_TOKEN|2,
								MESH_NULL, MESH_NULL));

		TileDef t1;
		t1.tx = 1; t1.ty = 0;
		t1.bmin[0] = 10.0f; t1.bmin[1] = 0.0f; t1.bmin[2] = 0.0f;
		t1.bmax[0] = 20.0f; t1.bmax[1] = 1.0f; t1.bmax[2] = 10.0f;
		const float v1[4*3] = {10,0,0, 20,0,0, 20,0,10, 10,0,10};
		t1.verts.assign(v1, v1+12);
		t1.polys.push_back(quad(0,1,2,3, MESH_NULL, MESH_NULL,
								MESH_NULL, PORTAL_TOKEN|0));

		std::vector<TileDef> tiles;
		tiles.push_back(t0); tiles.push_back(t1);
		BuiltMesh* bm = buildMesh(tiles, 10.0f, 10.0f, 8);
		TC_CHECK(bm != 0);
		RefGraph g = buildReferenceGraph(*bm, tiles);
		TC_CHECK(g.adj[0][0].mid[2] == 5.0f);

		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 128)));
		dtQueryFilter filter;
		const float startPos[3] = {0.0f, 0.0f, 5.0f};
		const float endPos[3] = {20.0f, 0.0f, 5.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};
		dtPolyRef path[8];
		int count = 0;
		float totalCost = 0.0f;
		dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(1),
										 startPos, endPos, &filter,
										 path, &count, 8,
										 heading, heading, 2.0f, &totalCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(count == 2);
		TC_CHECK(approxEqual(totalCost, 20.0));
		freeMesh(bm);
	});
}

static void registerCompatibility(Registry& r)
{
	r.add("compatibility", "ordinary_findpath_unchanged", [&r]()
	{
		std::vector<TileDef> tiles = branchTiles();
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		const float startPos[3] = {1.0f, 0.0f, 4.0f};
		const float endPos[3] = {16.0f, 0.0f, 4.0f};
		dtPolyRef path[16];
		int count = 0;
		dtStatus st = q.findPath(bm->refOf(0), bm->refOf(4),
								 startPos, endPos, &filter, path, &count, 16);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(count >= 4 && path[0] == bm->refOf(0) &&
				path[count-1] == bm->refOf(4));

		const float heading[3] = {1.0f, 0.0f, 0.0f};
		float turnCost = 0.0f;
		dtPolyRef tpath[16];
		int tcount = 0;
		TC_CHECK(dtStatusSucceed(q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
												   startPos, endPos, &filter,
												   tpath, &tcount, 16,
												   heading, heading, 1.0f,
												   &turnCost)));
		dtPolyRef path2[16];
		int count2 = 0;
		st = q.findPath(bm->refOf(0), bm->refOf(4),
						startPos, endPos, &filter, path2, &count2, 16);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(count2 == count);
		for (int i = 0; i < count; ++i)
			TC_CHECK(path2[i] == path[i]);
		freeMesh(bm);
	});

	r.add("compatibility", "repeated_calls_are_stable", [&r]()
	{
		std::vector<TileDef> tiles = branchTiles();
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		const float startPos[3] = {1.0f, 0.0f, 2.0f};
		const float endPos[3] = {16.0f, 0.0f, 5.0f};
		const float sh[3] = {1.0f, 0.0f, 1.5f};
		const float eh[3] = {1.0f, 0.0f, -0.2f};

		dtPolyRef paths[3][16];
		int counts[3] = {0, 0, 0};
		float costs[3] = {0, 0, 0};
		for (int i = 0; i < 3; ++i)
		{
			dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
											 startPos, endPos, &filter,
											 paths[i], &counts[i], 16,
											 sh, eh, 3.0f, &costs[i]);
			TC_CHECK(dtStatusSucceed(st));
		}
		for (int i = 1; i < 3; ++i)
		{
			TC_CHECK(counts[i] == counts[0]);
			TC_CHECK(costs[i] == costs[0]);
			for (int j = 0; j < counts[0]; ++j)
				TC_CHECK(paths[i][j] == paths[0][j]);
		}
		freeMesh(bm);
	});

	r.add("compatibility", "offmesh_connections_impassable", [&r]()
	{
		TileDef t;
		t.tx = 0; t.ty = 0;
		t.bmin[0] = t.bmin[1] = t.bmin[2] = 0.0f;
		t.bmax[0] = 20.0f; t.bmax[1] = 1.0f; t.bmax[2] = 10.0f;
		const float v[8*3] = {
			0,0,0, 10,0,0, 10,0,10, 0,0,10,
			10,0,0, 20,0,0, 20,0,10, 10,0,10
		};
		t.verts.assign(v, v+24);
		t.polys.push_back(quad(0,1,2,3, MESH_NULL, 1, MESH_NULL, MESH_NULL));
		t.polys.push_back(quad(4,5,6,7, MESH_NULL, MESH_NULL, MESH_NULL, 0));
		OffMeshDef om;
		om.a[0] = 3.0f; om.a[1] = 0.0f; om.a[2] = 5.0f;
		om.b[0] = 17.0f; om.b[1] = 0.0f; om.b[2] = 5.0f;
		om.rad = 0.5f;
		om.flags = 1;
		om.area = 0;
		t.offMesh.push_back(om);

		std::vector<TileDef> tiles;
		tiles.push_back(t);
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		const dtMeshTile* tile = bm->nav->getTileAt(0, 0, 0);
		TC_CHECK(tile->header->offMeshConCount == 1);
		const dtPolyRef offMeshRef = bm->refOf(2);
		TC_CHECK(tile->polys[2].getType() == DT_POLYTYPE_OFFMESH_CONNECTION);

		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		const float startPos[3] = {2.0f, 0.0f, 5.0f};
		const float endPos[3] = {18.0f, 0.0f, 5.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};
		dtPolyRef path[8];
		int count = 0;
		float totalCost = 0.0f;
		dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(1),
										 startPos, endPos, &filter,
										 path, &count, 8,
										 heading, heading, 1.0f, &totalCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(count == 2);
		TC_CHECK(path[0] == bm->refOf(0));
		TC_CHECK(path[1] == bm->refOf(1));

		float cost2 = 0.0f;
		int count2 = 0;
		st = q.findPathTurnCost(offMeshRef, bm->refOf(1),
								startPos, endPos, &filter,
								path, &count2, 8,
								heading, heading, 1.0f, &cost2);
		TC_CHECK(dtStatusFailed(st));
		TC_CHECK(dtStatusDetail(st, DT_INVALID_PARAM));
		TC_CHECK(count2 == 0 && cost2 == 0.0f);
		freeMesh(bm);
	});

	r.add("compatibility", "flags_filter_routes_around", [&r]()
	{
		std::vector<TileDef> tiles = branchTiles();
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		RefGraph g = buildReferenceGraph(*bm, tiles);
		std::vector<unsigned char> areas;
		for (size_t i = 0; i < bm->polys.size(); ++i)
			areas.push_back(bm->polys[i].area);

		// Exclude flag 2: B (poly 2) carries flags 3, A only flag 1.
		filter.setExcludeFlags(0x2);
		bm->nav->setPolyFlags(bm->refOf(1), 0x1);
		bm->nav->setPolyFlags(bm->refOf(2), 0x3);

		RefGraph filtered = g;
		for (size_t i = 0; i < filtered.adj.size(); ++i)
		{
			std::vector<Edge> kept;
			for (size_t k = 0; k < filtered.adj[i].size(); ++k)
				if (filtered.adj[i][k].to != 2)
					kept.push_back(filtered.adj[i][k]);
			filtered.adj[i] = kept;
		}

		const float startPos[3] = {1.0f, 0.0f, 4.0f};
		const float endPos[3] = {16.0f, 0.0f, 4.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};
		const double sa = headingAngle(heading);
		const Enumeration ref = enumerate(filtered, 0, 4, startPos, endPos,
										  areas, filter, sa, sa, 1.0, 9);
		dtPolyRef path[16];
		int count = 0;
		float totalCost = 0.0f;
		dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
										 startPos, endPos, &filter,
										 path, &count, 16,
										 heading, heading, 1.0f, &totalCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(approxEqual(totalCost, ref.bestCost));
		const std::vector<int> got = globalWalk(*bm, path, count);
		bool usesA = false, usesB = false;
		for (size_t i = 0; i < got.size(); ++i)
		{
			if (got[i] == 1) usesA = true;
			if (got[i] == 2) usesB = true;
		}
		TC_CHECK(usesA && !usesB);
		freeMesh(bm);
	});

	r.add("compatibility", "heading_y_component_ignored", [&r]()
	{
		std::vector<TileDef> tiles = branchTiles();
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 16);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 256)));
		dtQueryFilter filter;
		const float startPos[3] = {1.0f, 0.0f, 2.0f};
		const float endPos[3] = {16.0f, 0.0f, 5.0f};
		const float sh[3] = {1.0f, 0.0f, 1.5f};
		const float eh[3] = {1.0f, 0.0f, -0.2f};
		const float shY[3] = {1.0f, 777.0f, 1.5f};
		const float ehY[3] = {1.0f, -99.0f, -0.2f};

		dtPolyRef p1[16], p2[16];
		int n1 = 0, n2 = 0;
		float c1 = 0, c2 = 0;
		TC_CHECK(dtStatusSucceed(q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
												   startPos, endPos, &filter,
												   p1, &n1, 16, sh, eh, 2.0f, &c1)));
		TC_CHECK(dtStatusSucceed(q.findPathTurnCost(bm->refOf(0), bm->refOf(4),
												   startPos, endPos, &filter,
												   p2, &n2, 16, shY, ehY, 2.0f, &c2)));
		TC_CHECK(c1 == c2 && n1 == n2);
		for (int i = 0; i < n1; ++i)
			TC_CHECK(p1[i] == p2[i]);
		freeMesh(bm);
	});
}

static void registerErrors(Registry& r)
{
	struct Fixture
	{
		std::vector<TileDef> tiles;
		BuiltMesh* bm;
		dtNavMeshQuery q;
		dtQueryFilter filter;
	};
	auto makeBranch = [](Fixture& f)
	{
		f.tiles = branchTiles();
		f.bm = buildMesh(f.tiles, 64.0f, 64.0f, 16);
		f.q.init(f.bm->nav, 256);
	};

	r.add("errors", "no_path_clears_outputs", [&r]()
	{
		TileDef t;
		t.tx = 0; t.ty = 0;
		t.bmin[0] = t.bmin[1] = t.bmin[2] = 0.0f;
		t.bmax[0] = 20.0f; t.bmax[1] = 1.0f; t.bmax[2] = 10.0f;
		const float v[8*3] = {
			0,0,0, 8,0,0, 8,0,8, 0,0,8,
			12,0,0, 20,0,0, 20,0,8, 12,0,8
		};
		t.verts.assign(v, v+24);
		t.polys.push_back(quad(0,1,2,3, MESH_NULL, MESH_NULL, MESH_NULL, MESH_NULL));
		t.polys.push_back(quad(4,5,6,7, MESH_NULL, MESH_NULL, MESH_NULL, MESH_NULL));
		std::vector<TileDef> tiles;
		tiles.push_back(t);
		BuiltMesh* bm = buildMesh(tiles, 64.0f, 64.0f, 8);
		TC_CHECK(bm != 0);
		dtNavMeshQuery q;
		TC_CHECK(dtStatusSucceed(q.init(bm->nav, 128)));
		dtQueryFilter filter;
		const float startPos[3] = {2.0f, 0.0f, 4.0f};
		const float endPos[3] = {18.0f, 0.0f, 4.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};
		dtPolyRef path[8] = {1,2,3,4,5,6,7,8};
		int count = 7;
		float totalCost = 123.0f;
		dtStatus st = q.findPathTurnCost(bm->refOf(0), bm->refOf(1),
										 startPos, endPos, &filter,
										 path, &count, 8,
										 heading, heading, 1.0f, &totalCost);
		TC_CHECK(dtStatusFailed(st));
		TC_CHECK(!dtStatusDetail(st, DT_INVALID_PARAM));
		TC_CHECK(count == 0);
		TC_CHECK(totalCost == 0.0f);
		freeMesh(bm);
	});

	r.add("errors", "invalid_params_reset_outputs", [&r, makeBranch]()
	{
		Fixture f;
		makeBranch(f);
		const float startPos[3] = {1.0f, 0.0f, 4.0f};
		const float endPos[3] = {16.0f, 0.0f, 4.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};
		dtPolyRef path[16];
		int count = 0;
		float totalCost = 0.0f;

		TC_CHECK(dtStatusSucceed(f.q.findPathTurnCost(f.bm->refOf(0), f.bm->refOf(4),
													  startPos, endPos, &f.filter,
													  path, &count, 16,
													  heading, heading, 1.0f,
													  &totalCost)));
		TC_CHECK(count > 0 && totalCost > 0.0f);

		const dtPolyRef goodStart = f.bm->refOf(0);
		const dtPolyRef goodEnd = f.bm->refOf(4);
		const float zeroH[3] = {0.0f, 0.0f, 0.0f};
		const float nanH[3] = {1.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f};
		const float nanPos[3] = {1.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f};

		struct Bad
		{
			dtPolyRef s, e;
			const float *sp, *ep, *sh, *eh;
			float w;
			dtQueryFilter* flt;
			dtPolyRef* outPath;
			int maxPath;
		};
		Bad bads[] = {
			{goodStart, goodEnd, startPos, endPos, heading, heading, 1.0f, &f.filter, 0, 16},
			{goodStart, goodEnd, startPos, endPos, heading, heading, 1.0f, &f.filter, path, 0},
			{goodStart, goodEnd, startPos, endPos, zeroH, heading, 1.0f, &f.filter, path, 16},
			{goodStart, goodEnd, startPos, endPos, nanH, heading, 1.0f, &f.filter, path, 16},
			{goodStart, goodEnd, nanPos, endPos, heading, heading, 1.0f, &f.filter, path, 16},
			{goodStart, goodEnd, startPos, endPos, heading, heading, -1.0f, &f.filter, path, 16},
			{goodStart, goodEnd, startPos, endPos, heading, heading,
				std::numeric_limits<float>::quiet_NaN(), &f.filter, path, 16},
			{goodStart, goodEnd, startPos, endPos, heading, heading, 1.0f, 0, path, 16},
			{0, goodEnd, startPos, endPos, heading, heading, 1.0f, &f.filter, path, 16},
		};
		for (size_t i = 0; i < sizeof(bads)/sizeof(bads[0]); ++i)
		{
			count = 55;
			totalCost = 99.0f;
			dtStatus st = f.q.findPathTurnCost(bads[i].s, bads[i].e,
												bads[i].sp, bads[i].ep, bads[i].flt,
												bads[i].outPath, &count, bads[i].maxPath,
												bads[i].sh, bads[i].eh,
												bads[i].w, &totalCost);
			TC_CHECK(dtStatusFailed(st));
			TC_CHECK(dtStatusDetail(st, DT_INVALID_PARAM));
			TC_CHECK(count == 0);
			TC_CHECK(totalCost == 0.0f);
		}

		dtQueryFilter broken;
		const float badCosts[4] = {0.0f, -1.0f,
								   std::numeric_limits<float>::infinity(),
								   std::numeric_limits<float>::quiet_NaN()};
		for (size_t i = 0; i < 4; ++i)
		{
			broken.setAreaCost(7, badCosts[i]);
			count = 55;
			totalCost = 99.0f;
			dtStatus st = f.q.findPathTurnCost(goodStart, goodEnd,
												startPos, endPos, &broken,
												path, &count, 16,
												heading, heading, 1.0f, &totalCost);
			TC_CHECK(dtStatusDetail(st, DT_INVALID_PARAM));
			TC_CHECK(count == 0 && totalCost == 0.0f);
		}

		TC_CHECK(dtStatusFailed(f.q.findPathTurnCost(goodStart, goodEnd,
													startPos, endPos, &f.filter,
													path, 0, 16,
													heading, heading, 1.0f,
													&totalCost)));
		TC_CHECK(dtStatusFailed(f.q.findPathTurnCost(goodStart, goodEnd,
													startPos, endPos, &f.filter,
													path, &count, 16,
													heading, heading, 1.0f,
													0)));
		freeMesh(f.bm);
	});

	r.add("errors", "short_buffer_keeps_prefix_and_full_cost", [&r, makeBranch]()
	{
		Fixture f;
		makeBranch(f);
		const float startPos[3] = {1.0f, 0.0f, 2.0f};
		const float endPos[3] = {16.0f, 0.0f, 5.0f};
		const float sh[3] = {1.0f, 0.0f, 1.5f};
		const float eh[3] = {1.0f, 0.0f, -0.2f};

		dtPolyRef full[16];
		int fullCount = 0;
		float fullCost = 0.0f;
		dtStatus st = f.q.findPathTurnCost(f.bm->refOf(0), f.bm->refOf(4),
										   startPos, endPos, &f.filter,
										   full, &fullCount, 16,
										   sh, eh, 3.0f, &fullCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(fullCount >= 4);

		dtPolyRef small[2];
		int smallCount = 0;
		float smallCost = 0.0f;
		st = f.q.findPathTurnCost(f.bm->refOf(0), f.bm->refOf(4),
								  startPos, endPos, &f.filter,
								  small, &smallCount, 2,
								  sh, eh, 3.0f, &smallCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(dtStatusDetail(st, DT_BUFFER_TOO_SMALL));
		TC_CHECK(smallCount == 2);
		TC_CHECK(small[0] == full[0]);
		TC_CHECK(small[1] == full[1]);
		TC_CHECK(smallCost == fullCost);
		freeMesh(f.bm);
	});

	r.add("errors", "out_of_memory_status", [&r, makeBranch]()
	{
		Fixture f;
		makeBranch(f);
		const float startPos[3] = {1.0f, 0.0f, 4.0f};
		const float endPos[3] = {16.0f, 0.0f, 4.0f};
		const float heading[3] = {1.0f, 0.0f, 0.0f};

		static int allocCount;
		static int allocLimit;
		allocCount = 0;
		allocLimit = 1;
		auto failAlloc = [](size_t size, dtAllocHint) -> void*
		{
			if (allocCount++ >= allocLimit)
				return 0;
			return malloc(size);
		};
		auto passthroughFree = [](void* p) { free(p); };
		dtAllocSetCustom(failAlloc, passthroughFree);

		dtPolyRef path[16];
		int count = 9;
		float totalCost = 42.0f;
		dtStatus st = f.q.findPathTurnCost(f.bm->refOf(0), f.bm->refOf(4),
										   startPos, endPos, &f.filter,
										   path, &count, 16,
										   heading, heading, 1.0f, &totalCost);
		dtAllocSetCustom(0, 0);
		TC_CHECK(dtStatusFailed(st));
		TC_CHECK(dtStatusDetail(st, DT_OUT_OF_MEMORY));
		TC_CHECK(count == 0);
		TC_CHECK(totalCost == 0.0f);

		st = f.q.findPathTurnCost(f.bm->refOf(0), f.bm->refOf(4),
								  startPos, endPos, &f.filter,
								  path, &count, 16,
								  heading, heading, 1.0f, &totalCost);
		TC_CHECK(dtStatusSucceed(st));
		TC_CHECK(count > 0 && totalCost > 0.0f);
		freeMesh(f.bm);
	});

	r.add("errors", "same_poly_and_vertical_segments", [&r, makeBranch]()
	{
		Fixture f;
		makeBranch(f);
		const dtPolyRef s = f.bm->refOf(0);
		const float headingX[3] = {1.0f, 0.0f, 0.0f};
		const float headingZ[3] = {0.0f, 0.0f, 1.0f};

		// Same polygon, planar segment, collinear headings.
		{
			const float p0[3] = {1.0f, 0.0f, 2.0f};
			const float p1[3] = {4.0f, 0.0f, 2.0f};
			dtPolyRef path[4];
			int count = 0;
			float totalCost = 0.0f;
			dtStatus st = f.q.findPathTurnCost(s, s, p0, p1, &f.filter,
											   path, &count, 4,
											   headingX, headingX, 2.0f,
											   &totalCost);
			TC_CHECK(dtStatusSucceed(st));
			TC_CHECK(count == 1 && path[0] == s);
			TC_CHECK(approxEqual(totalCost, 3.0));
		}

		// Vertical-only segment: distance counts, no new heading, single turn
		// between the two supplied headings.
		{
			const float p0[3] = {2.0f, 0.0f, 2.0f};
			const float p1[3] = {2.0f, 5.0f, 2.0f};
			dtPolyRef path[4];
			int count = 0;
			float totalCost = 0.0f;
			dtStatus st = f.q.findPathTurnCost(s, s, p0, p1, &f.filter,
											   path, &count, 4,
											   headingX, headingZ, 3.0f,
											   &totalCost);
			TC_CHECK(dtStatusSucceed(st));
			TC_CHECK(count == 1);
			const double expected = 5.0 + 3.0 * (PI/2.0) * (PI/2.0);
			TC_CHECK(approxEqual(totalCost, expected));
		}

		// Zero-length segment: only the start-to-end heading turn remains.
		{
			const float p[3] = {2.0f, 0.0f, 2.0f};
			dtPolyRef path[4];
			int count = 0;
			float totalCost = 0.0f;
			dtStatus st = f.q.findPathTurnCost(s, s, p, p, &f.filter,
											   path, &count, 4,
											   headingX, headingZ, 3.0f,
											   &totalCost);
			TC_CHECK(dtStatusSucceed(st));
			TC_CHECK(count == 1);
			const double expected = 3.0 * (PI/2.0) * (PI/2.0);
			TC_CHECK(approxEqual(totalCost, expected));
		}
		freeMesh(f.bm);
	});
}

void register_task_tests(Registry& r)
{
	registerOptimality(r);
	registerPortals(r);
	registerCompatibility(r);
	registerErrors(r);
}
