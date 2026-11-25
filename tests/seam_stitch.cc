/*
seam_stitch.cc
Task tests for trimesh::stitch_boundary_loops().

All expected quantities are recomputed here from the input polylines and
original faces rather than being inferred from the returned mappings.
*/

#include "task_support/check.h"
#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

using namespace trimesh;
using namespace std;


namespace {

static const float EPS = 1e-5f;

static bool close(const point &a, const point &b, float tol)
{
	return dist(a, b) <= tol;
}

static float seg_dist(const point &x, const point &a, const point &b)
{
	vec e = b - a;
	float l2 = len2(e);
	float u = 0.0f;
	if (l2 > 0.0f)
		u = clamp(((x - a) DOT e) / l2, 0.0f, 1.0f);
	return dist(x, a + u * e);
}

static long long ekey(int a, int b)
{
	if (a > b)
		swap(a, b);
	return ((long long)(unsigned)a << 32) | (unsigned)b;
}

static long long dkey(int a, int b)
{
	return ((long long)(unsigned)a << 32) | (unsigned)b;
}

// A cluster of spatially coincident seam samples.
struct Cluster {
	vector<int> vids;
	point pos;
	int rep;
};

// Cluster all seam samples, then order the clusters along side 0's loop.
static vector<Cluster> build_clusters(const TriMesh *m,
                                      const vector<int> &loop0,
                                      const vector<int> &loop1,
                                      float tol)
{
	vector<Cluster> raw;
	const vector<int> *loops[2] = { &loop0, &loop1 };
	for (int s = 0; s < 2; s++) {
		for (size_t i = 0; i < loops[s]->size(); i++) {
			int vid = (*loops[s])[i];
			const point &p = m->vertices[vid];
			int found = -1;
			for (size_t k = 0; k < raw.size(); k++) {
				if (dist(raw[k].pos, p) <= tol) {
					found = (int)k;
					break;
				}
			}
			if (found < 0) {
				Cluster c;
				c.vids.push_back(vid);
				c.pos = p;
				c.rep = vid;
				raw.push_back(c);
			} else {
				raw[found].vids.push_back(vid);
				if (vid < raw[found].rep) {
					raw[found].rep = vid;
					raw[found].pos = m->vertices[vid];
				}
			}
		}
	}
	// Order the clusters by walking loop 0's polyline, inserting clusters
	// that only exist on side 1 at their position along the current edge.
	// A global emitted set keeps corner clusters from being added twice.
	vector<Cluster> ordered;
	set<int> emitted;
	int n0 = (int)loop0.size();
	for (int i = 0; i < n0; i++) {
		const point &a = m->vertices[loop0[i]];
		const point &b = m->vertices[loop0[(i + 1) % n0]];
		vector<pair<float,int> > on;
		for (size_t k = 0; k < raw.size(); k++) {
			if (seg_dist(raw[k].pos, a, b) <= tol) {
				float t = (len2(b - a) > 0.0f) ?
				          (((raw[k].pos - a) DOT (b - a)) / len2(b - a))
				          : 0.0f;
				on.push_back(make_pair(clamp(t, 0.0f, 1.0f), (int)k));
			}
		}
		sort(on.begin(), on.end());
		for (size_t q = 0; q < on.size(); q++) {
			int idx = on[q].second;
			if (!emitted.insert(idx).second)
				continue;
			ordered.push_back(raw[idx]);
		}
	}
	return ordered;
}

// Independently expected old-to-new map from the input loops alone.
static vector<int> expected_remap(int nv, const vector<Cluster> &clusters)
{
	vector<int> rep_of(nv, -1);
	for (size_t k = 0; k < clusters.size(); k++)
		for (size_t q = 0; q < clusters[k].vids.size(); q++)
			rep_of[clusters[k].vids[q]] = clusters[k].rep;

	vector<int> remap(nv, -1);
	int next = 0;
	for (int v = 0; v < nv; v++) {
		if (rep_of[v] >= 0) {
			if (v == rep_of[v])
				remap[v] = next++;
		} else {
			remap[v] = next++;
		}
	}
	for (int v = 0; v < nv; v++)
		if (rep_of[v] >= 0 && v != rep_of[v])
			remap[v] = remap[rep_of[v]];
	return remap;
}

struct P2 { float x, y; };
static float cross2(const P2 &a, const P2 &b)
{
	return a.x * b.y - a.y * b.x;
}
static P2 to2(const point &p, const point &o, const vec &u, const vec &v)
{
	P2 r;
	r.x = (p - o) DOT u;
	r.y = (p - o) DOT v;
	return r;
}

// Full independent verification of a successful stitch.
static int verify_stitch(const TriMesh *before, const TriMesh *after,
                         const vector<int> &loop0, const vector<int> &loop1,
                         const SeamStitchResult &result, float L)
{
	int nv = before->vertices.size();
	int nf = before->faces.size();
	float tol = 1e-6f * L;

	TASK_CHECK((int)result.old_to_new.size() == nv);
	TASK_CHECK((int)result.face_source.size() == (int)after->faces.size());
	for (size_t i = 0; i < result.face_source.size(); i++)
		TASK_CHECK(result.face_source[i] >= 0 &&
		           result.face_source[i] < nf);

	vector<Cluster> clusters = build_clusters(before, loop0, loop1, tol);
	int nn = (int)clusters.size();
	TASK_CHECK(nn >= 3);

	vector<int> expect_map = expected_remap(nv, clusters);
	TASK_CHECK(result.old_to_new == expect_map);

	// Expected output vertex count and positions.
	int expect_nv = 0;
	for (int v = 0; v < nv; v++)
		if (expect_map[v] >= 0)
			expect_nv = max(expect_nv, expect_map[v] + 1);
	TASK_CHECK((int)after->vertices.size() == expect_nv);
	for (int v = 0; v < nv; v++) {
		point expected = before->vertices[v];
		TASK_CHECK(close(after->vertices[expect_map[v]], expected, tol));
	}

	// Edge census of the output mesh, rebuilt from scratch.
	map<long long, int> use_count;
	set<long long> directed;
	for (size_t f = 0; f < after->faces.size(); f++) {
		const TriMesh::Face &face = after->faces[f];
		TASK_CHECK(face[0] != face[1] && face[1] != face[2] &&
		           face[2] != face[0]);
		for (int j = 0; j < 3; j++) {
			int a = face[j], b = face[(j + 1) % 3];
			TASK_CHECK(directed.insert(dkey(a, b)).second);
			use_count[ekey(a, b)]++;
		}
	}
	for (map<long long,int>::iterator it = use_count.begin();
	     it != use_count.end(); ++it)
		TASK_CHECK(it->second <= 2); // manifold

	// No T-junction: no output vertex may lie in the interior of an edge.
	// Vertices that merely coincide with an endpoint elsewhere (an
	// unrelated duplicate component) are allowed.
	for (map<long long,int>::iterator it = use_count.begin();
	     it != use_count.end(); ++it) {
		int a = (int)((unsigned long long)it->first >> 32);
		int b = (int)(it->first & 0xffffffffULL);
		const point &pa = after->vertices[a];
		const point &pb = after->vertices[b];
		vec e = pb - pa;
		float l2 = len2(e);
		for (int v = 0; v < (int)after->vertices.size(); v++) {
			if (v == a || v == b || l2 <= 0.0f)
				continue;
			float u = ((after->vertices[v] - pa) DOT e) / l2;
			if (u <= EPS || u >= 1.0f - EPS)
				continue;
			TASK_CHECK(seg_dist(after->vertices[v], pa, pb) > tol);
		}
	}

	// Every common-subdivision seam segment is a real, doubly-used edge
	// with the two uses pointing in opposite directions.
	for (int k = 0; k < nn; k++) {
		int a = expect_map[clusters[k].rep];
		int b = expect_map[clusters[(k + 1) % nn].rep];
		TASK_CHECK(a != b);
		map<long long,int>::const_iterator it =
			use_count.find(ekey(a, b));
		TASK_CHECK(it != use_count.end() && it->second == 2);
		TASK_CHECK(directed.count(dkey(a, b)) == 1);
		TASK_CHECK(directed.count(dkey(b, a)) == 1);
	}

	// Input seam edges and input edge census, derived independently.
	set<long long> seam_edges;
	const vector<int> *loops[2] = { &loop0, &loop1 };
	for (int s = 0; s < 2; s++)
		for (size_t i = 0; i < loops[s]->size(); i++)
			seam_edges.insert(ekey((*loops[s])[i],
			                       (*loops[s])[(i + 1) % loops[s]->size()]));
	map<long long,int> before_count;
	for (int f = 0; f < nf; f++)
		for (int j = 0; j < 3; j++)
			before_count[ekey(before->faces[f][j],
			                  before->faces[f][(j + 1) % 3])]++;

	// Parent-face coverage: children partition each original triangle,
	// keep its winding, and use only boundary points.
	vector<vector<int> > children(nf);
	for (size_t f = 0; f < result.face_source.size(); f++)
		children[result.face_source[f]].push_back((int)f);
	for (int f = 0; f < nf; f++) {
		TASK_CHECK(!children[f].empty());
		const TriMesh::Face &F = before->faces[f];

		// A face stays a single triangle exactly when no seam edge of its
		// own carries an extra subdivision point; it is then preserved.
		if (children[f].size() == 1) {
			const TriMesh::Face &g = after->faces[children[f][0]];
			for (int j = 0; j < 3; j++)
				TASK_CHECK(g[j] == expect_map[F[j]]);
			continue;
		}
		TASK_CHECK(children[f].size() >= 2);
		// A genuinely subdivided face must touch at least one seam edge.
		bool split_face = false;
		for (int j = 0; j < 3; j++)
			if (seam_edges.count(ekey(F[j], F[(j + 1) % 3])))
				split_face = true;
		TASK_CHECK(split_face);

		const point &o = before->vertices[F[0]];
		vec u = normalized(before->vertices[F[1]] - o);
		vec w = normalized(cross(before->vertices[F[1]] - o,
		                         before->vertices[F[2]] - o));
		vec v = cross(w, u);
		P2 A = to2(before->vertices[F[1]], o, u, v);
		P2 B = to2(before->vertices[F[2]], o, u, v);
		float parent_area2 = fabsf(cross2(A, B));
		TASK_CHECK(parent_area2 > EPS);

		float child_area_sum = 0.0f;
		set<TriMesh::Face> uniq;
		for (size_t q = 0; q < children[f].size(); q++) {
			const TriMesh::Face &g = after->faces[children[f][q]];
			TASK_CHECK(uniq.insert(g).second);
			P2 pg[3];
			for (int j = 0; j < 3; j++) {
				// Recover the point from an original vertex id: every
				// output vertex position equals some original position.
				point gp = after->vertices[g[j]];
				pg[j] = to2(gp, o, u, v);
				// Inside the parent triangle: all edge tests nonnegative.
				float c1 = cross2(A, pg[j]);
				float c2 = cross2(P2{B.x - A.x, B.y - A.y},
				                  P2{pg[j].x - A.x, pg[j].y - A.y});
				float c3 = cross2(P2{-B.x, -B.y},
				                  P2{pg[j].x - B.x, pg[j].y - B.y});
				TASK_CHECK(c1 >= -EPS && c2 >= -EPS && c3 >= -EPS);
			}
			float area2 = cross2(P2{pg[1].x - pg[0].x,
			                        pg[1].y - pg[0].y},
			                     P2{pg[2].x - pg[0].x,
			                        pg[2].y - pg[0].y});
			TASK_CHECK(area2 > EPS); // same winding, nonzero area
			child_area_sum += area2;
		}
		TASK_CHECK(fabsf(child_area_sum - parent_area2) <=
		           EPS * max(1.0f, parent_area2));
	}

	// Non-seam edges survive unchanged in use count.
	for (map<long long,int>::const_iterator it = before_count.begin();
	     it != before_count.end(); ++it) {
		if (seam_edges.count(it->first))
			continue;
		int a = (int)((unsigned long long)it->first >> 32);
		int b = (int)(it->first & 0xffffffffULL);
		map<long long,int>::const_iterator cur =
			use_count.find(ekey(expect_map[a], expect_map[b]));
		TASK_CHECK(cur != use_count.end() &&
		           cur->second == it->second);
	}
	return 0;
}

// Fan patch: triangles (loop[i], loop[i+1], apex), so the supplied loop
// follows each incident face's induced boundary direction.
static vector<int> append_fan(TriMesh *m, const vector<point> &loop,
                              const point &apex)
{
	int base = (int)m->vertices.size();
	for (size_t i = 0; i < loop.size(); i++)
		m->vertices.push_back(loop[i]);
	int ap = (int)m->vertices.size();
	m->vertices.push_back(apex);
	for (size_t i = 0; i < loop.size(); i++)
		m->faces.push_back(TriMesh::Face(
			base + (int)i, base + (int)((i + 1) % loop.size()), ap));
	vector<int> ids(loop.size());
	for (size_t i = 0; i < loop.size(); i++)
		ids[i] = base + (int)i;
	return ids;
}

static vector<point> rotate_pts(const vector<point> &p, int shift)
{
	int n = (int)p.size();
	vector<point> r(n);
	for (int i = 0; i < n; i++)
		r[i] = p[(i + shift) % n];
	return r;
}

static vector<int> rotate_ids(const vector<int> &p, int shift)
{
	int n = (int)p.size();
	vector<int> r(n);
	for (int i = 0; i < n; i++)
		r[i] = p[(i + shift) % n];
	return r;
}

// Dense forward spatial sampling of a closed corner polyline: one extra
// point per edge at fraction frac, all corners retained.
static vector<point> dense_forward(const vector<point> &corners, float frac)
{
	vector<point> q;
	int n = (int)corners.size();
	for (int i = 0; i < n; i++) {
		q.push_back(corners[i]);
		q.push_back((1.0f - frac) * corners[i] +
		            frac * corners[(i + 1) % n]);
	}
	return q;
}

static float mesh_scale(const TriMesh *m)
{
	box b;
	for (size_t i = 0; i < m->vertices.size(); i++)
		b += m->vertices[i];
	return len(b.max - b.min);
}

// ------------------------------------------------------------------ cases

static int case_uneven_fans()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(2, 0, 0.3f));
	corners.push_back(point(2, 2, 0));
	corners.push_back(point(0, 2, -0.3f));

	vector<int> loop0 = append_fan(&m, rotate_pts(corners, 1),
	                               point(1, 1, 1.5f));
	loop0 = rotate_ids(loop0, 1); // cyclic start shift

	vector<point> qf = dense_forward(corners, 0.35f);
	reverse(qf.begin(), qf.end()); // opposite spatial direction
	vector<int> loop1 = append_fan(&m, rotate_pts(qf, 3),
	                               point(1, 1, -1.5f));
	loop1 = rotate_ids(loop1, 3);

	// Bridge the two fans into one connected component; these faces do not
	// touch the seam and must survive verbatim.
	int apex0 = 4;
	int apex1 = (int)m.vertices.size() - 1; // second fan's apex
	m.vertices.push_back(point(3.2f, 1, 0));
	m.vertices.push_back(point(-1.2f, 1, 0));
	int w1 = apex1 + 1;
	int w2 = apex1 + 2;
	int bridge1 = (int)m.faces.size();
	m.faces.push_back(TriMesh::Face(apex0, apex1, w1));
	m.faces.push_back(TriMesh::Face(apex1, apex0, w2));

	TriMesh before = m;
	float L = mesh_scale(&m);
	SeamStitchResult result;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, result));
	TASK_CHECK(verify_stitch(&before, &m, loop0, loop1, result, L) == 0);

	// The two untouched bridge faces are preserved exactly once.
	int hits = 0;
	vector<int> emap = result.old_to_new;
	for (size_t f = 0; f < m.faces.size(); f++) {
		if (result.face_source[f] != bridge1 &&
		    result.face_source[f] != bridge1 + 1)
			continue;
		hits++;
		int src = result.face_source[f];
		for (int j = 0; j < 3; j++)
			TASK_CHECK(m.faces[f][j] == emap[before.faces[src][j]]);
	}
	TASK_CHECK(hits == 2);
	return 0;
}

static int case_three_edge_split()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(3, 0, 0));
	corners.push_back(point(0, 3, 0.5f));

	// Side 0 is a single triangle whose three edges are all seam edges.
	for (size_t i = 0; i < corners.size(); i++)
		m.vertices.push_back(corners[i]);
	m.faces.push_back(TriMesh::Face(0, 1, 2));
	vector<int> loop0;
	loop0.push_back(0); loop0.push_back(1); loop0.push_back(2);

	vector<point> qf = dense_forward(corners, 0.5f);
	reverse(qf.begin(), qf.end());
	vector<int> loop1 = append_fan(&m, qf, point(1, 1, -2.0f));

	TriMesh before = m;
	float L = mesh_scale(&m);
	SeamStitchResult result;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, result));
	TASK_CHECK(verify_stitch(&before, &m, loop0, loop1, result, L) == 0);

	// The lone original face must have been split into several children.
	int nchild = 0;
	for (size_t f = 0; f < result.face_source.size(); f++)
		if (result.face_source[f] == 0)
			nchild++;
	TASK_CHECK(nchild >= 4); // 3 inserted midpoints => at least 4 triangles
	return 0;
}

static int case_two_edge_split()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(2, 0, 0));
	corners.push_back(point(2, 2, 0.2f));
	corners.push_back(point(0, 2, 0));
	for (size_t i = 0; i < corners.size(); i++)
		m.vertices.push_back(corners[i]);
	// Two triangles over the diagonal (0,2): each has two seam edges.
	m.faces.push_back(TriMesh::Face(0, 1, 2));
	m.faces.push_back(TriMesh::Face(0, 2, 3));
	vector<int> loop0;
	loop0.push_back(0); loop0.push_back(1);
	loop0.push_back(2); loop0.push_back(3);

	vector<point> qf = dense_forward(corners, 0.25f);
	reverse(qf.begin(), qf.end());
	vector<int> loop1 = append_fan(&m, rotate_pts(qf, 2),
	                               point(1, 1, -1.7f));
	loop1 = rotate_ids(loop1, 2);

	TriMesh before = m;
	float L = mesh_scale(&m);
	SeamStitchResult result;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, result));
	TASK_CHECK(verify_stitch(&before, &m, loop0, loop1, result, L) == 0);

	for (int src = 0; src < 2; src++) {
		int nchild = 0;
		for (size_t f = 0; f < result.face_source.size(); f++)
			if (result.face_source[f] == src)
				nchild++;
		TASK_CHECK(nchild >= 3); // two split edges per parent
	}
	return 0;
}

// Equal-density loops: only coincident corners are welded, no face is
// subdivided, but every seam edge must still be a real shared edge.
static int case_equal_density_weld()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(2, 0, 0.2f));
	corners.push_back(point(2, 2, 0));
	corners.push_back(point(0, 2, -0.2f));
	vector<int> loop0 = append_fan(&m, rotate_pts(corners, 2),
	                               point(1, 1, 1.6f));
	loop0 = rotate_ids(loop0, 2);
	vector<point> rev = corners;
	reverse(rev.begin(), rev.end());
	vector<int> loop1 = append_fan(&m, rotate_pts(rev, 1),
	                               point(1, 1, -1.6f));
	loop1 = rotate_ids(loop1, 1);

	TriMesh before = m;
	float L = mesh_scale(&m);
	SeamStitchResult result;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, result));
	TASK_CHECK(verify_stitch(&before, &m, loop0, loop1, result, L) == 0);

	// No subdivision: one output face per original face, all sources once.
	TASK_CHECK(result.face_source.size() == before.faces.size());
	vector<int> seen_face(before.faces.size(), 0);
	for (size_t f = 0; f < result.face_source.size(); f++) {
		TASK_CHECK(result.face_source[f] >= 0 &&
		           result.face_source[f] < (int)before.faces.size());
		seen_face[result.face_source[f]]++;
	}
	for (size_t f = 0; f < seen_face.size(); f++)
		TASK_CHECK(seen_face[f] == 1);

	// The two boundary vertex sets are fully welded into one.
	set<int> welded;
	for (size_t i = 0; i < loop0.size(); i++)
		welded.insert(result.old_to_new[loop0[i]]);
	for (size_t i = 0; i < loop1.size(); i++)
		TASK_CHECK(welded.count(result.old_to_new[loop1[i]]) == 1);
	return 0;
}

static void assign_attrs(TriMesh *m)
{
	int nv = (int)m->vertices.size();
	m->colors.resize(nv);
	m->confidences.resize(nv);
	m->flags.resize(nv);
	for (int v = 0; v < nv; v++) {
		m->colors[v] = Color(0.1f + 0.01f * (v % 7),
		                     0.2f + 0.005f * v,
		                     fmodf(0.03f * v, 1.0f));
		m->confidences[v] = 0.5f + 0.007f * v;
		m->flags[v] = (1u << (v % 20)) | (v % 3 ? 4u : 0u);
	}
}

static int case_attributes()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(2, 0, 0));
	corners.push_back(point(2, 2, 0.4f));
	corners.push_back(point(0, 2, 0));
	vector<int> loop0 = append_fan(&m, corners, point(1, 1, 1.3f));
	vector<point> qf = dense_forward(corners, 0.4f);
	reverse(qf.begin(), qf.end());
	vector<int> loop1 = append_fan(&m, qf, point(1, 1, -1.3f));
	assign_attrs(&m);
	m.flag_curr = 9;

	TriMesh before = m;
	float L = mesh_scale(&m);
	float tol = 1e-6f * L;
	SeamStitchResult result;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, result));
	TASK_CHECK(verify_stitch(&before, &m, loop0, loop1, result, L) == 0);

	vector<Cluster> clusters = build_clusters(&before, loop0, loop1, tol);
	vector<int> emap = result.old_to_new;

	// Forward spatial order of side 1, with aligned vertex ids.
	vector<point> q1 = dense_forward(corners, 0.4f);
	vector<int> ids1_fwd(q1.size());
	for (size_t i = 0; i < q1.size(); i++)
		ids1_fwd[i] = loop1[loop1.size() - 1 - i];

	for (size_t k = 0; k < clusters.size(); k++) {
		const Cluster &c = clusters[k];
		bool on0 = false, on1 = false;
		int id0 = -1, id1 = -1;
		for (size_t i = 0; i < loop0.size(); i++)
			if (dist(before.vertices[loop0[i]], c.pos) <= tol) {
				on0 = true; id0 = loop0[i];
			}
		for (size_t i = 0; i < ids1_fwd.size(); i++)
			if (dist(before.vertices[ids1_fwd[i]], c.pos) <= tol) {
				on1 = true; id1 = ids1_fwd[i];
			}
		Color ec0, ec1;
		float ef0 = 0, ef1 = 0;
		unsigned eg0 = 0, eg1 = 0;
		if (on0) {
			ec0 = before.colors[id0];
			ef0 = before.confidences[id0];
			eg0 = before.flags[id0];
		}
		if (on1) {
			ec1 = before.colors[id1];
			ef1 = before.confidences[id1];
			eg1 = before.flags[id1];
		}
		if (!on0) {
			// Interpolate side 0 along the bracketing corner edge.
			int bi = -1;
			for (size_t i = 0; i < loop0.size(); i++) {
				const point &a = before.vertices[loop0[i]];
				const point &b = before.vertices[
					loop0[(i + 1) % loop0.size()]];
				if (seg_dist(c.pos, a, b) <= tol) {
					bi = (int)i;
					break;
				}
			}
			TASK_CHECK(bi >= 0);
			int va = loop0[bi], vb = loop0[(bi + 1) % loop0.size()];
			float t = dist(c.pos, before.vertices[va]) /
			          dist(before.vertices[va], before.vertices[vb]);
			ec0 = (1.0f - t) * before.colors[va] + t * before.colors[vb];
			ef0 = (1.0f - t) * before.confidences[va] +
			      t * before.confidences[vb];
			eg0 = before.flags[va] | before.flags[vb];
		}
		if (!on1) {
			int bi = -1;
			for (size_t i = 0; i < ids1_fwd.size(); i++) {
				const point &a = before.vertices[ids1_fwd[i]];
				const point &b = before.vertices[
					ids1_fwd[(i + 1) % ids1_fwd.size()]];
				if (seg_dist(c.pos, a, b) <= tol) {
					bi = (int)i;
					break;
				}
			}
			TASK_CHECK(bi >= 0);
			int va = ids1_fwd[bi], vb = ids1_fwd[(bi + 1) % ids1_fwd.size()];
			float t = dist(c.pos, before.vertices[va]) /
			          dist(before.vertices[va], before.vertices[vb]);
			ec1 = (1.0f - t) * before.colors[va] + t * before.colors[vb];
			ef1 = (1.0f - t) * before.confidences[va] +
			      t * before.confidences[vb];
			eg1 = before.flags[va] | before.flags[vb];
		}
		Color expected_col(0.5f * (ec0 + ec1));
		float expected_conf = 0.5f * (ef0 + ef1);
		unsigned expected_flags = eg0 | eg1;
		int nid = emap[c.rep];
		for (int j = 0; j < 3; j++)
			TASK_CHECK(fabsf(m.colors[nid][j] - expected_col[j]) <= EPS);
		TASK_CHECK(fabsf(m.confidences[nid] - expected_conf) <= EPS);
		TASK_CHECK(m.flags[nid] == expected_flags);
	}

	// Non-seam vertices (the two apices) keep their exact attributes.
	for (int v = 0; v < (int)before.vertices.size(); v++) {
		bool seam = false;
		for (size_t i = 0; i < loop0.size() && !seam; i++)
			if (loop0[i] == v) seam = true;
		for (size_t i = 0; i < loop1.size() && !seam; i++)
			if (loop1[i] == v) seam = true;
		if (seam)
			continue;
		int nid = emap[v];
		for (int j = 0; j < 3; j++)
			TASK_CHECK(m.colors[nid][j] == before.colors[v][j]);
		TASK_CHECK(m.confidences[nid] == before.confidences[v]);
		TASK_CHECK(m.flags[nid] == before.flags[v]);
	}
	TASK_CHECK(m.flag_curr == 9);
	return 0;
}

static int case_empty_attributes()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(1, 0, 0));
	corners.push_back(point(1, 1, 0.2f));
	vector<int> loop0 = append_fan(&m, corners, point(0.3f, 0.3f, 1));
	vector<point> qf = dense_forward(corners, 0.5f);
	reverse(qf.begin(), qf.end());
	vector<int> loop1 = append_fan(&m, qf, point(0.3f, 0.3f, -1));

	SeamStitchResult result;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, result));
	TASK_CHECK(m.colors.empty());
	TASK_CHECK(m.confidences.empty());
	TASK_CHECK(m.flags.empty());
	return 0;
}

static int case_cache_rebuild()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(2, 0, 0));
	corners.push_back(point(2, 2, 0));
	corners.push_back(point(0, 2, 0.3f));
	vector<int> loop0 = append_fan(&m, corners, point(1, 1, 1.4f));
	vector<point> qf = dense_forward(corners, 0.3f);
	reverse(qf.begin(), qf.end());
	vector<int> loop1 = append_fan(&m, qf, point(1, 1, -1.4f));
	m.flag_curr = 7;

	// Prefill every lazy cache.
	m.need_normals();
	m.need_curvatures();
	m.need_dcurv();
	m.need_pointareas();
	m.need_neighbors();
	m.need_adjacentfaces();
	m.need_across_edge();
	m.need_tstrips();
	m.need_bbox();
	m.need_bsphere();
	TASK_CHECK(!m.normals.empty() && !m.neighbors.empty() &&
	           !m.across_edge.empty() && !m.tstrips.empty() &&
	           m.bbox.valid && m.bsphere.valid);

	SeamStitchResult result;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, result));

	// All derived caches must be invalidated, user state retained.
	TASK_CHECK(m.normals.empty() && m.pdir1.empty() && m.pdir2.empty() &&
	           m.curv1.empty() && m.curv2.empty() && m.dcurv.empty());
	TASK_CHECK(m.pointareas.empty() && m.cornerareas.empty());
	TASK_CHECK(m.neighbors.empty() && m.adjacentfaces.empty() &&
	           m.across_edge.empty());
	TASK_CHECK(m.tstrips.empty() && m.grid.empty());
	TASK_CHECK(m.grid_width == -1 && m.grid_height == -1);
	TASK_CHECK(!m.bbox.valid && !m.bsphere.valid);
	TASK_CHECK(m.flag_curr == 7);

	// Everything must rebuild correctly from the stitched mesh.
	m.need_bbox();
	box expected_box;
	for (size_t i = 0; i < m.vertices.size(); i++)
		expected_box += m.vertices[i];
	TASK_CHECK(m.bbox.valid);
	for (int j = 0; j < 3; j++) {
		TASK_CHECK(fabsf(m.bbox.min[j] - expected_box.min[j]) <= EPS);
		TASK_CHECK(fabsf(m.bbox.max[j] - expected_box.max[j]) <= EPS);
	}
	m.need_bsphere();
	TASK_CHECK(m.bsphere.valid && m.bsphere.r > 0.0f);
	m.need_normals();
	m.need_pointareas();
	m.need_neighbors();
	m.need_adjacentfaces();
	m.need_across_edge();
	m.need_curvatures();
	m.need_dcurv();
	m.need_tstrips();
	TASK_CHECK((int)m.normals.size() == (int)m.vertices.size());
	TASK_CHECK((int)m.pointareas.size() == (int)m.vertices.size());
	TASK_CHECK((int)m.neighbors.size() == (int)m.vertices.size());
	TASK_CHECK((int)m.adjacentfaces.size() == (int)m.vertices.size());
	TASK_CHECK((int)m.across_edge.size() == (int)m.faces.size());
	TASK_CHECK(!m.tstrips.empty());

	// The rebuilt across-edge map closes the seam: no boundary edge left
	// whose endpoints are both seam vertices.
	set<int> seam_new;
	for (size_t i = 0; i < loop0.size(); i++)
		seam_new.insert(result.old_to_new[loop0[i]]);
	for (size_t i = 0; i < loop1.size(); i++)
		seam_new.insert(result.old_to_new[loop1[i]]);
	for (size_t f = 0; f < m.faces.size(); f++)
		for (int j = 0; j < 3; j++) {
			int a = m.faces[f][j], b = m.faces[f][(j + 1) % 3];
			if (seam_new.count(a) && seam_new.count(b))
				TASK_CHECK(m.across_edge[f][j] >= 0);
		}
	return 0;
}

static int case_unrelated_coincident()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(1, 0, 0));
	corners.push_back(point(1, 1, 0));
	vector<int> loop0 = append_fan(&m, corners, point(0.3f, 0.3f, 1));
	vector<point> qf = dense_forward(corners, 0.5f);
	reverse(qf.begin(), qf.end());
	vector<int> loop1 = append_fan(&m, qf, point(0.3f, 0.3f, -1));

	// A disconnected component containing vertices that coincide with
	// existing vertices but are not part of either loop.
	int dup_base = (int)m.vertices.size();
	m.vertices.push_back(point(0, 0, 0));
	m.vertices.push_back(point(1, 0, 0));
	m.vertices.push_back(point(1, 0, 0.5f));
	m.faces.push_back(TriMesh::Face(dup_base, dup_base + 1, dup_base + 2));

	TriMesh before = m;
	SeamStitchResult result;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, result));

	int a = result.old_to_new[0];
	int b = result.old_to_new[dup_base];
	TASK_CHECK(a != b); // coincident but outside the loops: not welded
	TASK_CHECK(dist(m.vertices[a], m.vertices[b]) <= EPS);
	int c = result.old_to_new[1];
	int d = result.old_to_new[dup_base + 1];
	TASK_CHECK(c != d);
	TASK_CHECK(dist(m.vertices[c], m.vertices[d]) <= EPS);
	// The disconnected component keeps its own face.
	bool found = false;
	for (size_t f = 0; f < m.faces.size(); f++)
		if (result.face_source[f] == (int)before.faces.size() - 1)
			found = true;
	TASK_CHECK(found);
	return 0;
}

static bool mesh_identical(const TriMesh &a, const TriMesh &b)
{
	if (a.vertices.size() != b.vertices.size() ||
	    a.faces.size() != b.faces.size() ||
	    a.colors.size() != b.colors.size() ||
	    a.confidences.size() != b.confidences.size() ||
	    a.flags.size() != b.flags.size())
		return false;
	for (size_t i = 0; i < a.vertices.size(); i++)
		for (int j = 0; j < 3; j++)
			if (a.vertices[i][j] != b.vertices[i][j])
				return false;
	for (size_t i = 0; i < a.faces.size(); i++)
		for (int j = 0; j < 3; j++)
			if (a.faces[i][j] != b.faces[i][j])
				return false;
	for (size_t i = 0; i < a.colors.size(); i++)
		for (int j = 0; j < 3; j++)
			if (a.colors[i][j] != b.colors[i][j])
				return false;
	for (size_t i = 0; i < a.confidences.size(); i++)
		if (a.confidences[i] != b.confidences[i])
			return false;
	for (size_t i = 0; i < a.flags.size(); i++)
		if (a.flags[i] != b.flags[i])
			return false;
	return true;
}

static int case_failure_unchanged()
{
	TriMesh m;
	vector<point> corners;
	corners.push_back(point(0, 0, 0));
	corners.push_back(point(2, 0, 0));
	corners.push_back(point(2, 2, 0));
	corners.push_back(point(0, 2, 0.2f));
	vector<int> loop0 = append_fan(&m, corners, point(1, 1, 1.2f));
	vector<point> qf = dense_forward(corners, 0.4f);
	reverse(qf.begin(), qf.end());
	vector<int> loop1 = append_fan(&m, qf, point(1, 1, -1.2f));
	assign_attrs(&m);

	// NULL mesh.
	SeamStitchResult result;
	TASK_CHECK(!stitch_boundary_loops(NULL, loop0, loop1, result));

	// Out-of-range and duplicated loop points.
	vector<int> bad = loop0;
	bad[0] = (int)m.vertices.size() + 5;
	TASK_CHECK(!stitch_boundary_loops(&m, bad, loop1, result));
	bad = loop0;
	bad[1] = loop0[0];
	TASK_CHECK(!stitch_boundary_loops(&m, bad, loop1, result));

	// Not a boundary loop: use apex vertices and seam verts mixed in.
	vector<int> notloop;
	notloop.push_back(4);
	notloop.push_back(loop0[1]);
	notloop.push_back(loop0[2]);
	TASK_CHECK(!stitch_boundary_loops(&m, notloop, loop1, result));

	// Same spatial direction: reverse the second loop's ordering.
	vector<int> same_dir = loop1;
	reverse(same_dir.begin(), same_dir.end());
	TASK_CHECK(!stitch_boundary_loops(&m, loop0, same_dir, result));

	// Geometry that does not coincide: separate small mesh.
	TriMesh gm;
	vector<point> c1 = corners;
	vector<int> g0 = append_fan(&gm, c1, point(1, 1, 1));
	vector<point> c2 = corners;
	c2[1] = point(2, 0.35f, 0);
	vector<point> gq = dense_forward(c2, 0.4f);
	reverse(gq.begin(), gq.end());
	vector<int> g1 = append_fan(&gm, gq, point(1, 1, -1));
	TASK_CHECK(!stitch_boundary_loops(&gm, g0, g1, result));

	// Illegal attribute length.
	TriMesh saved = m;
	m.colors.resize(m.colors.size() - 1);
	TASK_CHECK(!stitch_boundary_loops(&m, loop0, loop1, result));
	m = saved;

	// Pre-seed result content: failures must not touch it either.
	result.old_to_new.assign(7, -11);
	result.face_source.assign(3, -22);
	TASK_CHECK(!stitch_boundary_loops(&m, bad, loop1, result));
	TASK_CHECK(result.old_to_new.size() == 7);
	TASK_CHECK(result.face_source.size() == 3);
	TASK_CHECK(result.old_to_new[3] == -11);
	TASK_CHECK(result.face_source[1] == -22);

	// Failures never fill lazy caches.
	TASK_CHECK(m.neighbors.empty() && m.adjacentfaces.empty() &&
	           m.across_edge.empty() && m.normals.empty() &&
	           m.tstrips.empty());

	// The mesh is untouched after every failed call, and a subsequent
	// valid stitch on the same object succeeds.
	TASK_CHECK(mesh_identical(m, saved));
	SeamStitchResult ok;
	float L = mesh_scale(&m);
	TriMesh before = m;
	TASK_CHECK(stitch_boundary_loops(&m, loop0, loop1, ok));
	TASK_CHECK(verify_stitch(&before, &m, loop0, loop1, ok, L) == 0);
	return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
	{ "refinement", "uneven_fans_shifted", case_uneven_fans },
	{ "refinement", "three_edge_split", case_three_edge_split },
	{ "refinement", "two_edge_split", case_two_edge_split },
	{ "refinement", "equal_density_weld_only", case_equal_density_weld },
	{ "attributes", "interpolated_values", case_attributes },
	{ "attributes", "empty_attributes_stay_empty", case_empty_attributes },
	{ "state", "cache_invalidation_and_rebuild", case_cache_rebuild },
	{ "state", "unrelated_coincident_vertices", case_unrelated_coincident },
	{ "state", "failure_leaves_mesh_unchanged", case_failure_unchanged },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
