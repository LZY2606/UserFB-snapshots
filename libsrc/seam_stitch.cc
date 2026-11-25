/*
Szymon Rusinkiewicz
Princeton University

seam_stitch.cc
Stitch together two boundary loops that trace the same closed spatial
polyline, building a common subdivision of the two samplings.
*/


#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>
#include <vector>



using namespace std;


namespace trimesh {

namespace {

static inline long long edge_key(int a, int b)
{
	if (a > b)
		swap(a, b);
	return ((long long)(unsigned int)a << 32) | (unsigned int)b;
}

static inline long long directed_key(int a, int b)
{
	return ((long long)(unsigned int)a << 32) | (unsigned int)b;
}

struct Projection {
	double t;	// Arc parameter along the polyline
	point p;	// Projected point
	float d2;	// Squared distance to the polyline
};

// Project x onto a closed polyline with precomputed cumulative arc lengths.
static bool project_on_loop(const vector<point> &poly,
                            const vector<double> &s,
                            const point &x, Projection &proj)
{
	int n = (int)poly.size();
	proj.d2 = -1.0f;
	proj.t = 0.0;
	proj.p = poly[0];
	for (int i = 0; i < n; i++) {
		const point &a = poly[i];
		const point &b = poly[(i + 1) % n];
		vec e = b - a;
		float l2 = len2(e);
		if (l2 <= 0.0f)
			return false;
		float u = ((x - a) DOT e) / l2;
		u = clamp(u, 0.0f, 1.0f);
		point p = a + u * e;
		float d2 = dist2(x, p);
		if (proj.d2 < 0.0f || d2 < proj.d2) {
			proj.d2 = d2;
			proj.p = p;
			proj.t = s[i] + (double)u * (s[i + 1] - s[i]);
		}
	}
	return proj.d2 >= 0.0f;
}

struct Event {
	double t;
	point pos;
	int vid;
	int side;
	int loc;
};

struct Node {
	point pos;
	int rep;	// Lowest-numbered contributing original vertex
	vector<pair<int,int> > contrib; // (side, local sample index)
};

} // anonymous namespace


// Stitch two complete boundary loops of the same mesh that trace the same
// closed spatial polyline in opposite directions.
bool stitch_boundary_loops(TriMesh *mesh,
                           const vector<int> &first,
                           const vector<int> &second,
                           SeamStitchResult &result)
{
	if (!mesh)
		return false;

	int nv = (int)mesh->vertices.size();
	int nf = (int)mesh->faces.size();
	int n1 = (int)first.size();
	int n2 = (int)second.size();

	// Basic argument checks.  No lazy cache of the input mesh is touched.
	if (nv < 3 || nf < 1 || n1 < 3 || n2 < 3)
		return false;
	if ((!mesh->colors.empty() && (int)mesh->colors.size() != nv) ||
	    (!mesh->confidences.empty() && (int)mesh->confidences.size() != nv) ||
	    (!mesh->flags.empty() && (int)mesh->flags.size() != nv))
		return false;

	for (int i = 0; i < n1; i++)
		if (first[i] < 0 || first[i] >= nv)
			return false;
	for (int i = 0; i < n2; i++)
		if (second[i] < 0 || second[i] >= nv)
			return false;

	vector<int> seen(nv, 0);
	for (int i = 0; i < n1; i++) {
		if (seen[first[i]])
			return false;
		seen[first[i]] = 1;
	}
	for (int i = 0; i < n2; i++) {
		if (seen[second[i]])
			return false; // duplicated point or shared loop index
		seen[second[i]] = 1;
	}

	// Finite coordinates and bounding-box diagonal L.
	box bbox;
	for (int i = 0; i < nv; i++) {
		const point &p = mesh->vertices[i];
		for (int k = 0; k < 3; k++)
			if (!std::isfinite(p[k]))
				return false;
		bbox += p;
	}
	float L = len(bbox.max - bbox.min);
	if (!(L > 0.0f))
		return false;
	float tol = 1e-6f * L;
	float tol2 = sqr(tol);

	// Edge census of the input mesh, used to verify manifoldness and that
	// the supplied loops are complete boundary loops.
	map<long long, int> edge_count;
	map<long long, int> edge_owner;
	set<long long> directed_seen;
	for (int f = 0; f < nf; f++) {
		const TriMesh::Face &face = mesh->faces[f];
		if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0])
			return false;
		for (int j = 0; j < 3; j++) {
			int a = face[j], b = face[(j + 1) % 3];
			if (a < 0 || a >= nv || b < 0 || b >= nv)
				return false;
			if (!directed_seen.insert(directed_key(a, b)).second)
				return false; // duplicate oriented edge / face
			long long key = edge_key(a, b);
			int &c = edge_count[key];
			if (c >= 2)
				return false; // non-manifold input
			if (c == 0)
				edge_owner[key] = f;
			c++;
		}
	}

	// Boundary edges incident on each vertex.
	vector<vector<long long> > bdy_at(nv);
	for (map<long long,int>::iterator it = edge_count.begin();
	     it != edge_count.end(); ++it) {
		if (it->second != 1)
			continue;
		long long key = it->first;
		int a = (int)((unsigned long long)key >> 32);
		int b = (int)(key & 0xffffffffLL);
		bdy_at[a].push_back(key);
		bdy_at[b].push_back(key);
	}

	const vector<int> *loops[2] = { &first, &second };
	for (int side = 0; side < 2; side++) {
		const vector<int> &loop = *loops[side];
		int n = (int)loop.size();
		for (int i = 0; i < n; i++) {
			int v = loop[i];
			int vp = loop[(i + n - 1) % n];
			int vn = loop[(i + 1) % n];
			long long k1 = edge_key(vp, v);
			long long k2 = edge_key(v, vn);
			if (k1 == k2)
				return false;
			map<long long,int>::const_iterator e1 = edge_count.find(k1);
			map<long long,int>::const_iterator e2 = edge_count.find(k2);
			if (e1 == edge_count.end() || e1->second != 1 ||
			    e2 == edge_count.end() || e2->second != 1)
				return false; // not a boundary edge
			if (bdy_at[v].size() != 2 ||
			    find(bdy_at[v].begin(), bdy_at[v].end(), k1) == bdy_at[v].end() ||
			    find(bdy_at[v].begin(), bdy_at[v].end(), k2) == bdy_at[v].end())
				return false; // not a complete boundary loop
		}
	}

	// Orientation sign of each loop's first edge within its incident face.
	// Both loops follow their incident-face-induced boundary direction, so
	// opposite spatial traversal shows up as equal signs.
	int loop_sign[2];
	for (int side = 0; side < 2; side++) {
		const vector<int> &loop = *loops[side];
		int a = loop[0], b = loop[1];
		map<long long,int>::const_iterator it =
			edge_owner.find(edge_key(a, b));
		if (it == edge_owner.end())
			return false;
		const TriMesh::Face &face = mesh->faces[it->second];
		int sgn = 0;
		for (int j = 0; j < 3; j++) {
			if (face[j] == a && face[(j + 1) % 3] == b)
				sgn = +1;
			if (face[j] == b && face[(j + 1) % 3] == a)
				sgn = -1;
		}
		if (sgn == 0)
			return false;
		loop_sign[side] = sgn;
	}
	if (loop_sign[0] != loop_sign[1])
		return false; // loops run in the same spatial direction

	// Polylines in space and their arc-length parameterizations.
	vector<point> poly[2];
	vector<double> cum[2];
	double total[2];
	for (int side = 0; side < 2; side++) {
		const vector<int> &loop = *loops[side];
		int n = (int)loop.size();
		poly[side].resize(n);
		cum[side].resize(n + 1);
		cum[side][0] = 0.0;
		for (int i = 0; i < n; i++) {
			poly[side][i] = mesh->vertices[loop[i]];
			double seg = dist(poly[side][i],
			                  mesh->vertices[loop[(i + 1) % n]]);
			if (!(seg > 10.0 * (double)tol))
				return false;
			cum[side][i + 1] = cum[side][i] + seg;
		}
		total[side] = cum[side][n];
	}

	// Every sample and every edge midpoint of each side must lie on the
	// other side's polyline within tol: the geometry must coincide.
	Projection proj;
	for (int side = 0; side < 2; side++) {
		int other = side ^ 1;
		int n = (int)poly[side].size();
		for (int i = 0; i < n; i++) {
			if (!project_on_loop(poly[other], cum[other],
			                     poly[side][i], proj) ||
			    proj.d2 > tol2)
				return false;
		}
		for (int i = 0; i < n; i++) {
			point mid = 0.5f * (poly[side][i] +
			                     poly[side][(i + 1) % n]);
			if (!project_on_loop(poly[other], cum[other],
			                     mid, proj) ||
			    proj.d2 > tol2)
				return false;
		}
	}

	// Parameters of side 1's samples along side 0's polyline.
	vector<double> u(n2);
	for (int j = 0; j < n2; j++) {
		if (!project_on_loop(poly[0], cum[0], poly[1][j], proj) ||
		    proj.d2 > tol2)
			return false;
		u[j] = proj.t;
	}
	// Unwrap, expecting monotonically decreasing parameters (opposite
	// spatial directions).
	vector<double> uw = u;
	for (int j = 1; j < n2; j++) {
		double d = uw[j] - uw[j - 1];
		if (d > 0.5 * total[0])
			uw[j] -= total[0];
		else if (d < -0.5 * total[0])
			uw[j] += total[0];
		d = uw[j] - uw[j - 1];
		if (d > (double)tol)
			return false;
	}
	// Closing step: after one full revolution the first sample is reached
	// again at parameter uw[0]-total.  It must continue the monotone
	// progression without backtracking or overshooting.
	double cont = uw[0] - total[0];
	if (cont > uw[n2 - 1] + (double)tol ||
	    uw[n2 - 1] - cont > total[0] + (double)tol)
		return false;

	// Collect all sampling events parameterized by arc length along side 0.
	// The same physical point has a unique parameter, so coincident samples
	// from the two sides end up adjacent after sorting.  The unwrapped
	// sequence above is only used to validate the opposite-direction order.
	vector<Event> events;
	events.reserve(n1 + n2);
	for (int i = 0; i < n1; i++) {
		Event e;
		e.t = cum[0][i];
		e.pos = poly[0][i];
		e.vid = first[i];
		e.side = 0;
		e.loc = i;
		events.push_back(e);
	}
	for (int j = 0; j < n2; j++) {
		Event e;
		e.t = u[j];
		e.pos = poly[1][j];
		e.vid = second[j];
		e.side = 1;
		e.loc = j;
		if (e.t < -(double)tol || e.t > total[0] + (double)tol)
			return false;
		events.push_back(e);
	}
	sort(events.begin(), events.end(),
	     [](const Event &a, const Event &b) {
		if (a.t != b.t)
			return a.t < b.t;
		return a.vid < b.vid;
	     });

	// Cluster events that coincide in space; distinct events are separated
	// by far more than tol by the problem's assumptions.
	vector<Node> nodes;
	for (size_t i = 0; i < events.size(); i++) {
		if (nodes.empty() ||
		    dist(events[i].pos, nodes.back().pos) > tol) {
			Node node;
			node.rep = events[i].vid;
			node.pos = mesh->vertices[node.rep];
			nodes.push_back(node);
		}
		Node &node = nodes.back();
		for (size_t k = 0; k < node.contrib.size(); k++)
			if (node.contrib[k].first == events[i].side)
				return false; // two samples of one side coincide
		node.contrib.push_back(make_pair(events[i].side, events[i].loc));
		if (events[i].vid < node.rep) {
			node.rep = events[i].vid;
			node.pos = mesh->vertices[node.rep];
		}
	}
	// Merge clusters across the cyclic cut at t = 0.
	if (nodes.size() > 1 &&
	    dist(nodes.front().pos, nodes.back().pos) <= tol) {
		Node &front = nodes.front();
		Node &back = nodes.back();
		for (size_t k = 0; k < back.contrib.size(); k++) {
			for (size_t m = 0; m < front.contrib.size(); m++)
				if (front.contrib[m].first == back.contrib[k].first)
					return false;
			front.contrib.push_back(back.contrib[k]);
		}
		if (back.rep < front.rep) {
			front.rep = back.rep;
			front.pos = mesh->vertices[front.rep];
		}
		nodes.pop_back();
	}
	int nn = (int)nodes.size();
	if (nn < 3)
		return false;
	// Every sample of each side belongs to exactly one node; side 0 walks
	// the nodes in increasing order, side 1 in decreasing order.
	vector<vector<int> > node_of(2);
	node_of[0].assign(n1, -1);
	node_of[1].assign(n2, -1);
	for (int k = 0; k < nn; k++) {
		for (size_t c = 0; c < nodes[k].contrib.size(); c++) {
			int side = nodes[k].contrib[c].first;
			int loc = nodes[k].contrib[c].second;
			if (node_of[side][loc] != -1)
				return false;
			node_of[side][loc] = k;
		}
	}
	for (int i = 0; i < n1; i++)
		if (node_of[0][i] < 0)
			return false;
	for (int j = 0; j < n2; j++)
		if (node_of[1][j] < 0)
			return false;
	// Side 0 visits the nodes in increasing cyclic order and side 1 in
	// decreasing order; other-side-only nodes may sit between samples.
	for (int side = 0; side < 2; side++) {
		int n = (side == 0) ? n1 : n2;
		int dir = (side == 0) ? +1 : -1;
		int start = node_of[side][0];
		int prev_rank = -1;
		for (int i = 0; i < n; i++) {
			int rank = dir * (node_of[side][i] - start) % nn;
			if (rank < 0)
				rank += nn;
			if (rank <= prev_rank)
				return false;
			prev_rank = rank;
		}
		if (prev_rank >= nn)
			return false;
	}

	// Locate seam edges and remember which side each belongs to.
	vector<int> seam_loc(nv, -1);
	for (int i = 0; i < n1; i++)
		seam_loc[first[i]] = i;
	for (int j = 0; j < n2; j++)
		seam_loc[second[j]] = j;
	// Every seam vertex maps to its common-subdivision node, so that a seam
	// corner shared by a split edge and an untouched edge gets one ring
	// entry rather than two coincident ones.
	vector<int> seam_vertex_to_node(nv, -1);
	for (int side = 0; side < 2; side++) {
		int n = (side == 0) ? n1 : n2;
		for (int i = 0; i < n; i++)
			seam_vertex_to_node[(*loops[side])[i]] = node_of[side][i];
	}
	auto ring_entry = [&](int vid) -> int {
		int k = seam_vertex_to_node[vid];
		return (k >= 0) ? nv + k : vid;
	};
	map<long long, int> seam_edge_side;
	for (int side = 0; side < 2; side++) {
		const vector<int> &loop = *loops[side];
		int n = (int)loop.size();
		for (int i = 0; i < n; i++) {
			long long key = edge_key(loop[i], loop[(i + 1) % n]);
			seam_edge_side[key] = side;
		}
	}

	// Build the subdivided boundary ring of every face.  Ring entries are
	// raw vertex ids for untouched corners and nv+node ids for seam nodes.
	vector<vector<int> > face_rings(nf);
	vector<char> face_touched(nf, 0);
	for (int f = 0; f < nf; f++) {
		const TriMesh::Face &face = mesh->faces[f];
		vector<int> ring;
		int splits = 0;
		for (int j = 0; j < 3; j++) {
			int a = face[j], b = face[(j + 1) % 3];
			long long key = edge_key(a, b);
			map<long long,int>::const_iterator it =
				seam_edge_side.find(key);
			vector<int> chain;
			if (it == seam_edge_side.end()) {
				chain.push_back(ring_entry(a));
				chain.push_back(ring_entry(b));
			} else {
				splits++;
				int side = it->second;
				const vector<int> &loop = *loops[side];
				int n = (int)loop.size();
				int ka = node_of[side][seam_loc[a]];
				int kb = node_of[side][seam_loc[b]];
				int step;
				if (loop[(seam_loc[a] + 1) % n] == b)
					step = (side == 0) ? +1 : -1;
				else if (loop[(seam_loc[a] + n - 1) % n] == b)
					step = (side == 0) ? -1 : +1;
				else
					return false;
				int k = ka;
				int guard = 0;
				while (true) {
					chain.push_back(nv + k);
					if (k == kb)
						break;
					k = (k + step + nn) % nn;
					if (++guard > nn)
						return false;
				}
			}
			if (ring.empty()) {
				ring = chain;
			} else {
				for (size_t q = 1; q < chain.size(); q++)
					ring.push_back(chain[q]);
			}
		}
		if (splits > 0) {
			// The last chain ends back at the ring's first entry: drop the
			// duplicated closing point before tessellation.
			if (ring.size() < 4 || ring.back() != ring.front())
				return false;
			ring.pop_back();
			face_touched[f] = 1;
		}
		face_rings[f] = ring;
	}

	// Compact vertex numbering: every original vertex survives except seam
	// vertices welded into a lower-numbered one; output order follows the
	// lowest contributing source index.  old_to_new maps every original
	// vertex, including welded ones, onto its replacement's new index.
	vector<char> kept(nv, 1);
	for (int k = 0; k < nn; k++) {
		for (size_t c = 0; c < nodes[k].contrib.size(); c++) {
			int side = nodes[k].contrib[c].first;
			int loc = nodes[k].contrib[c].second;
			int vid = (*loops[side])[loc];
			if (vid != nodes[k].rep)
				kept[vid] = 0;
		}
	}
	vector<int> old_to_new(nv, -1);
	int out_nv = 0;
	for (int v = 0; v < nv; v++) {
		if (kept[v])
			old_to_new[v] = out_nv++;
	}
	for (int k = 0; k < nn; k++) {
		int target = old_to_new[nodes[k].rep];
		for (size_t c = 0; c < nodes[k].contrib.size(); c++) {
			int side = nodes[k].contrib[c].first;
			int loc = nodes[k].contrib[c].second;
			int vid = (*loops[side])[loc];
			old_to_new[vid] = target;
		}
	}
	vector<int> node_newid(nn, -1);
	for (int k = 0; k < nn; k++)
		node_newid[k] = old_to_new[nodes[k].rep];

	auto resolve = [&](int e) -> int {
		return (e < nv) ? old_to_new[e] : node_newid[e - nv];
	};
	auto entry_pos = [&](int e) -> const point & {
		return (e < nv) ? mesh->vertices[e] : nodes[e - nv].pos;
	};

	// Tessellate each affected triangle by ear clipping of its (weakly
	// convex) subdivided boundary ring; unaffected faces pass through.
	vector<TriMesh::Face> out_faces;
	vector<int> out_face_source;
	out_faces.reserve(nf + nn);
	out_face_source.reserve(nf + nn);
	for (int f = 0; f < nf; f++) {
		if (!face_touched[f]) {
			TriMesh::Face nf3;
			for (int j = 0; j < 3; j++)
				nf3[j] = old_to_new[mesh->faces[f][j]];
			out_faces.push_back(nf3);
			out_face_source.push_back(f);
			continue;
		}
		vector<int> ring = face_rings[f];
		vec unit_normal = normalized(mesh->trinorm(f));
		// The parent triangle's three corners can never be collinear flat
		// points; only inserted seam samples on its edges are flat.
		auto is_parent_corner = [&](int e) -> bool {
			const point &p = entry_pos(e);
			for (int j = 0; j < 3; j++) {
				if (dist(p, mesh->vertices[mesh->faces[f][j]]) <= tol)
					return true;
			}
			return false;
		};
		auto ear_score = [&](int i, const vector<int> &r) -> float {
			int kr = (int)r.size();
			const point &p0 = entry_pos(r[(i + kr - 1) % kr]);
			const point &p1 = entry_pos(r[i]);
			const point &p2 = entry_pos(r[(i + 1) % kr]);
			return (cross(p1 - p0, p2 - p0) DOT unit_normal);
		};
		while (ring.size() > 3) {
			int kr = (int)ring.size();
			// A "flat" vertex is an inserted edge sample lying on the
			// segment joining its two ring neighbors.
			vector<char> flat(kr, 0);
			bool any_flat = false;
			for (int i = 0; i < kr; i++) {
				if (is_parent_corner(ring[i]))
					continue;
				const point &p0 = entry_pos(ring[(i + kr - 1) % kr]);
				const point &p1 = entry_pos(ring[i]);
				const point &p2 = entry_pos(ring[(i + 1) % kr]);
				float base = dist(p0, p2);
				float perp = (base > 0.0f) ?
				             len(cross(p1 - p0, p2 - p0)) / base : 0.0f;
				if (perp <= tol) {
					flat[i] = 1;
					any_flat = true;
				}
			}
			int best = -1;
			if (any_flat) {
				// Absorb a flat point by removing a strict-positive ear
				// next to it, so no triangulation diagonal ever skips a
				// collinear seam sample.
				for (int i = 0; i < kr; i++) {
					if (flat[i])
						continue;
					if (!flat[(i + kr - 1) % kr] && !flat[(i + 1) % kr])
						continue;
					if (ear_score(i, ring) > 0.0f) {
						best = i;
						break;
					}
				}
			} else {
				float best_score = 0.0f;
				for (int i = 0; i < kr; i++) {
					float score = ear_score(i, ring);
					if (score > best_score) {
						best_score = score;
						best = i;
					}
				}
			}
			if (best < 0)
				return false; // would create a zero-area triangle
			int kr2 = (int)ring.size();
			int prev = ring[(best + kr2 - 1) % kr2];
			int next = ring[(best + 1) % kr2];
			TriMesh::Face t;
			t[0] = resolve(prev);
			t[1] = resolve(ring[best]);
			t[2] = resolve(next);
			if (t[0] == t[1] || t[1] == t[2] || t[2] == t[0])
				return false;
			out_faces.push_back(t);
			out_face_source.push_back(f);
			ring.erase(ring.begin() + best);
		}
		{
			// Reject a final triangle containing a leftover collinear point.
			for (int i = 0; i < 3; i++) {
				const point &p0 = entry_pos(ring[(i + 2) % 3]);
				const point &p1 = entry_pos(ring[i]);
				const point &p2 = entry_pos(ring[(i + 1) % 3]);
				float base = dist(p0, p2);
				float perp = (base > 0.0f) ?
				             len(cross(p1 - p0, p2 - p0)) / base : 0.0f;
				if (perp <= tol)
					return false; // zero-area final triangle
			}
		}
		TriMesh::Face t;
		for (int j = 0; j < 3; j++)
			t[j] = resolve(ring[j]);
		if (t[0] == t[1] || t[1] == t[2] || t[2] == t[0])
			return false;
		out_faces.push_back(t);
		out_face_source.push_back(f);
	}

	// Global manifold, duplicate-face and seam-edge checks on the output.
	set<long long> out_dir;
	map<long long, int> out_count;
	set<TriMesh::Face> face_keys;
	for (size_t f = 0; f < out_faces.size(); f++) {
		const TriMesh::Face &face = out_faces[f];
		if (face[0] == face[1] || face[1] == face[2] ||
		    face[2] == face[0])
			return false;
		TriMesh::Face sorted = face;
		sort(sorted.begin(), sorted.end());
		if (!face_keys.insert(sorted).second)
			return false; // duplicate face
		for (int j = 0; j < 3; j++) {
			int a = face[j], b = face[(j + 1) % 3];
			if (!out_dir.insert(directed_key(a, b)).second)
				return false;
			long long key = edge_key(a, b);
			if (++out_count[key] > 2)
				return false; // non-manifold edge
		}
	}
	// Every seam sub-segment must be one real edge shared by exactly two
	// oppositely-oriented faces.
	for (int k = 0; k < nn; k++) {
		int a = node_newid[k];
		int b = node_newid[(k + 1) % nn];
		map<long long,int>::const_iterator it =
			out_count.find(edge_key(a, b));
		if (it == out_count.end() || it->second != 2)
			return false;
		if (out_dir.count(directed_key(a, b)) +
		    out_dir.count(directed_key(b, a)) != 2)
			return false;
	}

	// Per-node attributes, computed independently on each side and then
	// combined.  A direct sample hit takes the vertex value; an interior
	// point is linearly interpolated along its containing original edge.
	// Skipped entirely when the mesh carries no interpolatable attributes.
	bool have_any_attr = !mesh->colors.empty() ||
	                     !mesh->confidences.empty() ||
	                     !mesh->flags.empty();
	vector<Color> node_colors(nn);
	vector<float> node_conf(nn);
	vector<unsigned> node_flags(nn);

	if (have_any_attr) for (int k = 0; k < nn; k++) {
		int has_side[2] = { -1, -1 };
		for (size_t c = 0; c < nodes[k].contrib.size(); c++) {
			int side = nodes[k].contrib[c].first;
			has_side[side] = nodes[k].contrib[c].second;
		}
		Color col[2];
		float conf[2] = {0.0f, 0.0f};
		unsigned flg[2] = {0, 0};
		for (int side = 0; side < 2; side++) {
			const vector<int> &loop = *loops[side];
			int n = (int)loop.size();
			if (has_side[side] >= 0) {
				int vid = loop[has_side[side]];
				if (!mesh->colors.empty())
					col[side] = mesh->colors[vid];
				if (!mesh->confidences.empty())
					conf[side] = mesh->confidences[vid];
				if (!mesh->flags.empty())
					flg[side] = mesh->flags[vid];
				continue;
			}
			// Nearest samples of this side on either side of node k along
			// the cyclic node chain, independent of the side's direction.
			int loc_lo = -1, loc_hi = -1;
			for (int guard = 1; guard < nn && loc_lo < 0; guard++) {
				int kb = (k - guard + nn) % nn;
				for (size_t c = 0; c < nodes[kb].contrib.size(); c++)
					if (nodes[kb].contrib[c].first == side) {
						loc_lo = nodes[kb].contrib[c].second;
						break;
					}
			}
			for (int guard = 1; guard < nn && loc_hi < 0; guard++) {
				int kf = (k + guard) % nn;
				for (size_t c = 0; c < nodes[kf].contrib.size(); c++)
					if (nodes[kf].contrib[c].first == side) {
						loc_hi = nodes[kf].contrib[c].second;
						break;
					}
			}
			// The two bracketing samples must be consecutive on the side's
			// own loop, in either cyclic order.
			if (loc_lo < 0 || loc_hi < 0)
				return false;
			int va_loc;
			if (loc_hi == (loc_lo + 1) % n)
				va_loc = loc_lo;
			else if (loc_lo == (loc_hi + 1) % n)
				va_loc = loc_hi;
			else
				return false;
			int va = loop[va_loc];
			int vb = loop[(va_loc + 1) % n];
			const point &pa = mesh->vertices[va];
			const point &pb = mesh->vertices[vb];
			float elen = dist(pa, pb);
			float tt = (elen > 0.0f) ?
			           clamp(dist(nodes[k].pos, pa) / elen,
			                 0.0f, 1.0f) : 0.0f;
			if (!mesh->colors.empty())
				col[side] = (1.0f - tt) * mesh->colors[va]
				            + tt * mesh->colors[vb];
			if (!mesh->confidences.empty())
				conf[side] = (1.0f - tt) * mesh->confidences[va]
				             + tt * mesh->confidences[vb];
			if (!mesh->flags.empty())
				flg[side] = mesh->flags[va] | mesh->flags[vb];
		}
		if (!mesh->colors.empty())
			node_colors[k] = Color(0.5f * (col[0] + col[1]));
		if (!mesh->confidences.empty())
			node_conf[k] = 0.5f * (conf[0] + conf[1]);
		if (!mesh->flags.empty())
			node_flags[k] = flg[0] | flg[1];
	}

	// Find the node represented by a given surviving source vertex.
	vector<int> rep_to_node(nv, -1);
	for (int k = 0; k < nn; k++)
		rep_to_node[nodes[k].rep] = k;

	// Assemble output vertex arrays in compact source-index order.
	vector<point> out_vertices;
	vector<Color> out_colors;
	vector<float> out_conf;
	vector<unsigned> out_flags;
	out_vertices.reserve(out_nv);
	if (!mesh->colors.empty())
		out_colors.reserve(out_nv);
	if (!mesh->confidences.empty())
		out_conf.reserve(out_nv);
	if (!mesh->flags.empty())
		out_flags.reserve(out_nv);
	for (int v = 0; v < nv; v++) {
		if (!kept[v])
			continue;
		out_vertices.push_back(mesh->vertices[v]);
		int k = rep_to_node[v];
		if (!mesh->colors.empty())
			out_colors.push_back(k >= 0 ? node_colors[k]
			                            : mesh->colors[v]);
		if (!mesh->confidences.empty())
			out_conf.push_back(k >= 0 ? node_conf[k]
			                          : mesh->confidences[v]);
		if (!mesh->flags.empty())
			out_flags.push_back(k >= 0 ? node_flags[k]
			                           : mesh->flags[v]);
	}

	// All checks passed: commit.
	SeamStitchResult new_result;
	new_result.old_to_new = old_to_new;
	new_result.face_source = out_face_source;

	mesh->vertices.swap(out_vertices);
	mesh->faces.swap(out_faces);
	if (!mesh->colors.empty())
		mesh->colors.swap(out_colors);
	if (!mesh->confidences.empty())
		mesh->confidences.swap(out_conf);
	if (!mesh->flags.empty())
		mesh->flags.swap(out_flags);

	// Derived caches are stale; user attributes and flag_curr survive.
	mesh->clear_normals();
	mesh->clear_curvatures();
	mesh->clear_dcurv();
	mesh->clear_pointareas();
	mesh->clear_neighbors();
	mesh->clear_adjacentfaces();
	mesh->clear_across_edge();
	mesh->clear_tstrips();
	mesh->clear_grid();
	mesh->clear_bbox();
	mesh->clear_bsphere();

	result.old_to_new.swap(new_result.old_to_new);
	result.face_source.swap(new_result.face_source);
	return true;
}

} // namespace trimesh
