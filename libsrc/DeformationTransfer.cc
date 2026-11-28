/*
DeformationTransfer.cc

Face-correspondence gradient-domain deformation transfer.  The sparse
normal equations are assembled and LDL^T-factorized once in the
constructor; solve() only rebuilds the right-hand side and runs two sparse
triangular solves per frame.
*/

#include "DeformationTransfer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

using namespace std;


namespace trimesh {

// Dense 3x3 matrix in double precision.
struct Mat3 {
	double m[3][3];
	Mat3()
	{
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				m[i][j] = 0.0;
	}
};

static bool finite_vec(const dvec3 &v)
{
	return std::isfinite(v[0]) && std::isfinite(v[1]) &&
	       std::isfinite(v[2]);
}

static dvec3 mat3_vec(const Mat3 &a, const dvec3 &v)
{
	return dvec3(a.m[0][0]*v[0] + a.m[0][1]*v[1] + a.m[0][2]*v[2],
	              a.m[1][0]*v[0] + a.m[1][1]*v[1] + a.m[1][2]*v[2],
	              a.m[2][0]*v[0] + a.m[2][1]*v[1] + a.m[2][2]*v[2]);
}

// out = a * b
static Mat3 mat3_mul(const Mat3 &a, const Mat3 &b)
{
	Mat3 out;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double s = 0.0;
			for (int k = 0; k < 3; k++)
				s += a.m[i][k] * b.m[k][j];
			out.m[i][j] = s;
		}
	return out;
}

// Inverse of a 3x3 matrix.  Returns false if the matrix is singular.
static bool mat3_inverse(const Mat3 &a, Mat3 &inv)
{
	const double (*m)[3] = a.m;
	double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
	             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
	             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	if (!(det > 0.0 || det < 0.0))
		return false;
	double idet = 1.0 / det;
	inv.m[0][0] =  (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * idet;
	inv.m[0][1] = -(m[0][1] * m[2][2] - m[0][2] * m[2][1]) * idet;
	inv.m[0][2] =  (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * idet;
	inv.m[1][0] = -(m[1][0] * m[2][2] - m[1][2] * m[2][0]) * idet;
	inv.m[1][1] =  (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * idet;
	inv.m[1][2] = -(m[0][0] * m[1][2] - m[0][2] * m[1][0]) * idet;
	inv.m[2][0] =  (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * idet;
	inv.m[2][1] = -(m[0][0] * m[2][1] - m[0][1] * m[2][0]) * idet;
	inv.m[2][2] =  (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * idet;
	return true;
}

static double mat3_frob2(const Mat3 &a)
{
	double s = 0.0;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			s += a.m[i][j] * a.m[i][j];
	return s;
}

// Triangle frame F = [ e1, e2, a / sqrt(|a|) ] with
// e1 = p1-p0, e2 = p2-p0, a = e1 x e2.  Returns false for a degenerate
// triangle.  The third column has length sqrt(|a|) (sqrt-area-scaled
// normal), not unit length.
static bool triangle_frame(const dvec3 &p0, const dvec3 &p1,
                           const dvec3 &p2, Mat3 &F)
{
	dvec3 e1 = p1 - p0;
	dvec3 e2 = p2 - p0;
	dvec3 a = e1 CROSS e2;
	double la = len(a);
	if (!(la > 0.0))
		return false;
	dvec3 nscaled = a / sqrt(la);
	for (int i = 0; i < 3; i++) {
		F.m[i][0] = e1[i];
		F.m[i][1] = e2[i];
		F.m[i][2] = nscaled[i];
	}
	return true;
}

// Union-find for connected-component labeling.
namespace {

struct DSU {
	vector<int> parent;
	explicit DSU(int n) : parent(n)
	{
		for (int i = 0; i < n; i++)
			parent[i] = i;
	}
	int find(int x)
	{
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	}
	void unite(int a, int b)
	{
		a = find(a);
		b = find(b);
		if (a != b)
			parent[b] = a;
	}
};

} // anonymous namespace

// Validate the mesh assumptions and label vertex connected components.
// Throws std::invalid_argument on any violation.
static void validate_manifold(const TriMesh &mesh, const char *which,
                              vector<int> &comp, int &ncomp)
{
	int nv = (int)mesh.vertices.size();
	int nf = (int)mesh.faces.size();
	if (nv < 3 || nf < 1)
		throw invalid_argument(string("DeformationTransfer: ") + which +
		                       " mesh has no geometry");

	for (int v = 0; v < nv; v++) {
		if (!finite_vec(dvec3(mesh.vertices[v][0],
		                      mesh.vertices[v][1],
		                      mesh.vertices[v][2])))
			throw invalid_argument(string("DeformationTransfer: ") + which +
			                       " mesh contains non-finite coordinates");
	}

	vector<char> used(nv, 0);
	// Undirected edge -> first incident face and its directed traversal.
	// face == -2 marks an interior (two-face) edge.
	struct EdgeInfo { int face; int a; int b; };
	map< pair<int,int>, EdgeInfo > edges;

	for (int f = 0; f < nf; f++) {
		const TriMesh::Face &face = mesh.faces[f];
		for (int k = 0; k < 3; k++) {
			int idx = face[k];
			if (idx < 0 || idx >= nv)
				throw invalid_argument(string("DeformationTransfer: ") +
				                       which + " face index out of range");
			used[idx] = 1;
		}
		if (face[0] == face[1] || face[1] == face[2] ||
		    face[0] == face[2])
			throw invalid_argument(string("DeformationTransfer: ") +
			                       which + " mesh has a degenerate face");

		dvec3 p[3];
		for (int k = 0; k < 3; k++)
			p[k] = dvec3(mesh.vertices[face[k]][0],
			             mesh.vertices[face[k]][1],
			             mesh.vertices[face[k]][2]);
		dvec3 a = (p[1] - p[0]) CROSS (p[2] - p[0]);
		if (!(len(a) > 0.0))
			throw invalid_argument(string("DeformationTransfer: ") +
			                       which + " mesh has a degenerate face");

		// Edge k is opposite vertex k: (face[(k+1)%3], face[(k+2)%3])
		for (int k = 0; k < 3; k++) {
			int ea = face[(k + 1) % 3];
			int eb = face[(k + 2) % 3];
			pair<int,int> key(min(ea, eb), max(ea, eb));
			map< pair<int,int>, EdgeInfo >::iterator it = edges.find(key);
			if (it == edges.end()) {
				edges[key] = EdgeInfo{f, ea, eb};
			} else {
				if (it->second.face == -2)
					throw invalid_argument(
						string("DeformationTransfer: ") + which +
						" mesh is non-manifold (edge with >2 faces)");
				if (it->second.face == f)
					throw invalid_argument(
						string("DeformationTransfer: ") + which +
						" mesh contains a repeated edge");
				// Consistent orientation: the shared edge is traversed
				// in opposite directions by the two faces.
				if (it->second.a != eb || it->second.b != ea)
					throw invalid_argument(
						string("DeformationTransfer: ") + which +
						" mesh is inconsistently oriented");
				it->second.face = -2;
			}
		}
	}

	for (int v = 0; v < nv; v++)
		if (!used[v])
			throw invalid_argument(string("DeformationTransfer: ") + which +
			                       " mesh contains an isolated/unused vertex");

	DSU dsu(nv);
	for (map< pair<int,int>, EdgeInfo >::const_iterator it = edges.begin();
	     it != edges.end(); ++it)
		dsu.unite(it->first.first, it->first.second);

	comp.assign(nv, -1);
	ncomp = 0;
	for (int v = 0; v < nv; v++) {
		int root = dsu.find(v);
		if (comp[root] == -1)
			comp[root] = ncomp++;
		comp[v] = comp[root];
	}
}

// Merge-sum duplicate (column, value) pairs in a sorted sparse vector.
static void coalesce(vector< pair<int,double> > &entries)
{
	sort(entries.begin(), entries.end());
	size_t out = 0;
	for (size_t i = 0; i < entries.size(); ) {
		int col = entries[i].first;
		double sum = 0.0;
		size_t j = i;
		while (j < entries.size() && entries[j].first == col) {
			sum += entries[j].second;
			j++;
		}
		if (sum != 0.0)
			entries[out++] = make_pair(col, sum);
		i = j;
	}
	entries.resize(out);
}

DeformationTransfer::DeformationTransfer(
	const TriMesh &source_rest_mesh,
	const TriMesh &target_rest_mesh,
	const vector<FaceCorrespondence> &corr,
	const vector<int> &target_anchors)
	: nsource_verts(0), ntarget_verts(0), ntarget_faces(0),
	  target_grid_width(-1), target_grid_height(-1),
	  nfree(0), operator_builds(0), successful_solves(0)
{
	vector<int> source_comp, target_comp;
	int nsource_comp = 0, ntarget_comp = 0;
	validate_manifold(source_rest_mesh, "source", source_comp, nsource_comp);
	validate_manifold(target_rest_mesh, "target", target_comp, ntarget_comp);

	nsource_verts = (int)source_rest_mesh.vertices.size();
	ntarget_verts = (int)target_rest_mesh.vertices.size();
	ntarget_faces = (int)target_rest_mesh.faces.size();
	int nsource_faces = (int)source_rest_mesh.faces.size();

	// Own all rest geometry and target attributes.
	source_rest.resize(nsource_verts);
	for (int v = 0; v < nsource_verts; v++)
		source_rest[v] = dvec3(source_rest_mesh.vertices[v][0],
		                       source_rest_mesh.vertices[v][1],
		                       source_rest_mesh.vertices[v][2]);
	target_rest.resize(ntarget_verts);
	for (int v = 0; v < ntarget_verts; v++)
		target_rest[v] = dvec3(target_rest_mesh.vertices[v][0],
		                       target_rest_mesh.vertices[v][1],
		                       target_rest_mesh.vertices[v][2]);
	target_faces = target_rest_mesh.faces;

	target_colors = target_rest_mesh.colors;
	target_confidences = target_rest_mesh.confidences;
	target_flags = target_rest_mesh.flags;
	target_flag_curr = target_rest_mesh.flag_curr;
	target_tstrips = target_rest_mesh.tstrips;
	target_grid = target_rest_mesh.grid;
	target_grid_width = target_rest_mesh.grid_width;
	target_grid_height = target_rest_mesh.grid_height;
	target_neighbors = target_rest_mesh.neighbors;
	target_adjacentfaces = target_rest_mesh.adjacentfaces;
	target_across_edge = target_rest_mesh.across_edge;

	// Anchors: valid, unique, and at least one per target component.
	if (target_anchors.empty())
		throw invalid_argument("DeformationTransfer: no target anchors given");
	vector<char> is_anchor(ntarget_verts, 0);
	anchors.reserve(target_anchors.size());
	for (size_t k = 0; k < target_anchors.size(); k++) {
		int a = target_anchors[k];
		if (a < 0 || a >= ntarget_verts)
			throw invalid_argument("DeformationTransfer: anchor index out of range");
		if (is_anchor[a])
			throw invalid_argument("DeformationTransfer: duplicate anchor");
		is_anchor[a] = 1;
		anchors.push_back(a);
	}
	anchor_lookup.assign(ntarget_verts, -1);
	for (size_t k = 0; k < anchors.size(); k++)
		anchor_lookup[anchors[k]] = (int)k;
	vector<char> anchored_comp(ntarget_comp, 0);
	for (size_t k = 0; k < anchors.size(); k++)
		anchored_comp[target_comp[anchors[k]]] = 1;
	for (int c = 0; c < ntarget_comp; c++)
		if (!anchored_comp[c])
			throw invalid_argument(
				"DeformationTransfer: every target connected component needs an anchor");

	// Correspondences: valid indices, positive finite weights, full coverage.
	if (corr.empty())
		throw invalid_argument("DeformationTransfer: no correspondences given");
	correspondences.resize(ntarget_faces);
	vector<char> covered(ntarget_faces, 0);
	for (size_t c = 0; c < corr.size(); c++) {
		int sf = corr[c].source_face;
		int tf = corr[c].target_face;
		double w = corr[c].weight;
		if (sf < 0 || sf >= nsource_faces || tf < 0 || tf >= ntarget_faces)
			throw invalid_argument(
				"DeformationTransfer: correspondence face index out of range");
		if (!std::isfinite(w) || w <= 0.0)
			throw invalid_argument(
				"DeformationTransfer: correspondence weights must be positive and finite");
		correspondences[tf].push_back(Correspondence(sf, w));
		covered[tf] = 1;
	}
	for (int f = 0; f < ntarget_faces; f++)
		if (!covered[f])
			throw invalid_argument(
				"DeformationTransfer: every target face needs a correspondence");

	// Source rest frame inverses.
	source_faces.resize(nsource_faces);
	for (int f = 0; f < nsource_faces; f++) {
		const TriMesh::Face &face = source_rest_mesh.faces[f];
		for (int k = 0; k < 3; k++)
			source_faces[f].v[k] = face[k];
		Mat3 Frest, Finv;
		bool ok = triangle_frame(source_rest[face[0]],
		                         source_rest[face[1]],
		                         source_rest[face[2]], Frest);
		if (!ok || !mat3_inverse(Frest, Finv))
			throw invalid_argument(
				"DeformationTransfer: degenerate source rest triangle");
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				source_faces[f].inv_frame[i][j] = Finv.m[i][j];
	}

	// Target rest face data: area, barycentric gradients, plane projection.
	target_face_data.resize(ntarget_faces);
	for (int f = 0; f < ntarget_faces; f++) {
		TargetFace &tf = target_face_data[f];
		const TriMesh::Face &face = target_faces[f];
		for (int k = 0; k < 3; k++)
			tf.v[k] = face[k];
		dvec3 p0 = target_rest[face[0]];
		dvec3 p1 = target_rest[face[1]];
		dvec3 p2 = target_rest[face[2]];
		dvec3 e1 = p1 - p0;
		dvec3 e2 = p2 - p0;
		dvec3 a = e1 CROSS e2;
		double la = len(a);
		if (!(la > 0.0))
			throw invalid_argument(
				"DeformationTransfer: degenerate target rest triangle");
		tf.area = 0.5 * la;
		dvec3 n = a / la;
		double a2 = la * la;
		dvec3 g1 = (len2(e2) * e1 - (e1 DOT e2) * e2) / a2;
		dvec3 g2 = (len2(e1) * e2 - (e1 DOT e2) * e1) / a2;
		dvec3 g0 = -(g1 + g2);
		dvec3 g[3] = { g0, g1, g2 };
		for (int i = 0; i < 3; i++)
			for (int k = 0; k < 3; k++)
				tf.grad[i][k] = g[i][k];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				tf.proj[i][j] = (i == j ? 1.0 : 0.0) - n[i] * n[j];
	}

	// Free/anchor vertex numbering.
	free_index.assign(ntarget_verts, -1);
	nfree = 0;
	for (int v = 0; v < ntarget_verts; v++)
		if (!is_anchor[v])
			free_index[v] = nfree++;
	int ndof = 3 * nfree;

	// Full vertex-pair operator, keyed by (smaller vertex, larger vertex).
	// The Frobenius expansion gives blocks A*w*(g_i . g_j) times the 3x3
	// identity: G = sum x_i g_i^T already lives in the tangent plane, so
	// ambient (including normal) vertex motions are penalized through the
	// ordinary dot product of positions.  P only enters the right-hand
	// side and the constant part of the energy.  Store one scalar per
	// unordered vertex pair.
	vector< map<int, double> > vcoeff(ntarget_verts);
	for (int f = 0; f < ntarget_faces; f++) {
		const TargetFace &tf = target_face_data[f];
		double face_weight = 0.0;
		for (size_t c = 0; c < correspondences[f].size(); c++)
			face_weight += correspondences[f][c].weight;
		double C = tf.area * face_weight;
		dvec3 g[3];
		for (int i = 0; i < 3; i++)
			g[i] = dvec3(tf.grad[i][0], tf.grad[i][1], tf.grad[i][2]);
		for (int i = 0; i < 3; i++) {
			for (int j = i; j < 3; j++) {
				int vi = tf.v[i], vj = tf.v[j];
				double coeff = C * (g[i] DOT g[j]);
				vcoeff[min(vi, vj)][max(vi, vj)] += coeff;
			}
		}
	}

	// Exact anchor elimination: split the full operator into the reduced
	// free-free matrix (diagonal + upper sparse rows) and the free-anchor
	// coupling used to update the right-hand side per frame.  Each scalar
	// vertex-pair coefficient applies identically to the x, y, z dofs.
	h_diag.assign(ndof, 0.0);
	h_upper.resize(ndof);
	anchor_terms.resize(ndof);
	for (int vlo = 0; vlo < ntarget_verts; vlo++) {
		for (map<int, double>::const_iterator it = vcoeff[vlo].begin();
		     it != vcoeff[vlo].end(); ++it) {
			int vhi = it->first;
			double c = it->second;
			int flo = free_index[vlo], fhi = free_index[vhi];
			bool lo_free = (flo >= 0), hi_free = (fhi >= 0);
			if (lo_free && hi_free) {
				for (int r = 0; r < 3; r++) {
					int row = 3 * flo + r;
					int col = 3 * fhi + r;
					if (row == col)
						h_diag[row] += c;
					else
						h_upper[min(row, col)].push_back(
							make_pair(max(row, col), c));
				}
			} else if (lo_free || hi_free) {
				int fv = lo_free ? flo : fhi;
				int ak = anchor_lookup[lo_free ? vhi : vlo];
				for (int r = 0; r < 3; r++)
					anchor_terms[3*fv + r].push_back(
						make_pair(3*ak + r, c));
			}
		}
	}
	for (int i = 0; i < ndof; i++) {
		coalesce(h_upper[i]);
		coalesce(anchor_terms[i]);
	}
	// The factorization overwrites diag with the D factor; keep the
	// original operator diagonal separately for residual checking.
	diag = h_diag;

	// Symbolic LDL^T: find the sparsity pattern of L, including fill-in.
	vector< vector<int> > pattern(ndof);
	for (int i = 0; i < ndof; i++) {
		pattern[i].reserve(h_upper[i].size());
		for (size_t t = 0; t < h_upper[i].size(); t++)
			pattern[i].push_back(h_upper[i][t].first);
	}
	vector<int> marker(ndof, -1);
	vector< vector<int> > col_rows(ndof);
	for (int j = 0; j < ndof; j++) {
		for (size_t t = 0; t < pattern[j].size(); t++)
			marker[pattern[j][t]] = j;
		for (size_t tk = 0; tk < col_rows[j].size(); tk++) {
			int k = col_rows[j][tk];
			for (size_t t = 0; t < pattern[k].size(); t++) {
				int i = pattern[k][t];
				if (i > j)
					marker[i] = j;
			}
		}
		vector<int> pat;
		for (int i = j + 1; i < ndof; i++)
			if (marker[i] == j)
				pat.push_back(i);
		pattern[j].swap(pat);
		for (size_t t = 0; t < pattern[j].size(); t++)
			col_rows[pattern[j][t]].push_back(j);
	}

	// Numeric LDL^T on the symbolic pattern.  l_upper stores L (the
	// off-diagonal entries), diag stores D.
	l_upper.resize(ndof);
	vector<double> buf(ndof, 0.0);
	vector<int> gather_mark(ndof, -1);
	double diag_scale = 0.0;
	for (int i = 0; i < ndof; i++)
		diag_scale = max(diag_scale, fabs(diag[i]));

	for (int j = 0; j < ndof; j++) {
		// All candidate L entries of this column.
		gather_mark[j] = j;
		buf[j] = diag[j];
		for (size_t t = 0; t < pattern[j].size(); t++) {
			int i = pattern[j][t];
			gather_mark[i] = j;
			buf[i] = 0.0;
		}
		// Original upper-triangle nonzeros of H.
		for (size_t t = 0; t < h_upper[j].size(); t++) {
			int i = h_upper[j][t].first;
			gather_mark[i] = j;
			buf[i] = h_upper[j][t].second;
		}
		for (size_t tk = 0; tk < col_rows[j].size(); tk++) {
			int k = col_rows[j][tk];
			double ljk = 0.0;
			for (size_t t = 0; t < l_upper[k].size(); t++)
				if (l_upper[k][t].first == j) {
					ljk = l_upper[k][t].second;
					break;
				}
			double dk = diag[k];
			buf[j] -= ljk * ljk * dk;
			for (size_t t = 0; t < l_upper[k].size(); t++) {
				int i = l_upper[k][t].first;
				if (i > j && gather_mark[i] == j)
					buf[i] -= l_upper[k][t].second * dk * ljk;
			}
		}
		double dj = buf[j];
		if (!std::isfinite(dj) ||
		    dj <= 1e-300 * max(1.0, diag_scale))
			throw runtime_error(
				"DeformationTransfer: sparse factorization failed");
		diag[j] = dj;
		l_upper[j].reserve(pattern[j].size());
		for (size_t t = 0; t < pattern[j].size(); t++) {
			int i = pattern[j][t];
			l_upper[j].push_back(make_pair(i, buf[i] / dj));
		}
	}

	operator_builds = 1;
}

DeformationTransfer::~DeformationTransfer()
{}

size_t DeformationTransfer::stored_nonzeros() const
{
	size_t nnz = (size_t)(3 * nfree); // diagonal
	for (size_t i = 0; i < h_upper.size(); i++)
		nnz += h_upper[i].size();
	return nnz;
}

DeformationTransferResult DeformationTransfer::solve(
	const vector<point> &source_positions,
	const vector<dvec3> &anchor_positions) const
{
	return solve_impl(source_positions, anchor_positions);
}

DeformationTransferResult DeformationTransfer::solve(
	const vector<point> &source_positions,
	const vector<point> &anchor_positions) const
{
	vector<dvec3> anchors_d(anchor_positions.size());
	for (size_t i = 0; i < anchor_positions.size(); i++)
		anchors_d[i] = dvec3(anchor_positions[i][0],
		                     anchor_positions[i][1],
		                     anchor_positions[i][2]);
	return solve_impl(source_positions, anchors_d);
}

DeformationTransferResult DeformationTransfer::solve_impl(
	const vector<point> &source_positions,
	const vector<dvec3> &anchor_positions) const
{
	DeformationTransferResult failure;
	failure.mesh = 0;
	double nan = numeric_limits<double>::quiet_NaN();
	failure.energy = nan;
	failure.residual = nan;

	if ((int)source_positions.size() != nsource_verts ||
	    (int)anchor_positions.size() != (int)anchors.size())
		throw invalid_argument(
			"DeformationTransfer::solve: input size mismatch");

	for (int v = 0; v < nsource_verts; v++) {
		if (!std::isfinite((double)source_positions[v][0]) ||
		    !std::isfinite((double)source_positions[v][1]) ||
		    !std::isfinite((double)source_positions[v][2]))
			return failure;
	}
	for (size_t k = 0; k < anchor_positions.size(); k++)
		if (!finite_vec(anchor_positions[k]))
			return failure;

	// Current source deformation gradients S = F_current F_rest^-1.
	int nsource_faces = (int)source_faces.size();
	vector<Mat3> source_S(nsource_faces);
	for (int f = 0; f < nsource_faces; f++) {
		const SourceFace &sf = source_faces[f];
		dvec3 p[3];
		for (int k = 0; k < 3; k++) {
			const point &q = source_positions[sf.v[k]];
			p[k] = dvec3(q[0], q[1], q[2]);
			if (!finite_vec(p[k]))
				return failure;
		}
		Mat3 Fcur;
		if (!triangle_frame(p[0], p[1], p[2], Fcur))
			return failure; // degenerate current source triangle
		Mat3 Finv;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				Finv.m[i][j] = sf.inv_frame[i][j];
		source_S[f] = mat3_mul(Fcur, Finv);
	}

	// Right-hand side of the full (unconstrained) normal equations:
	// rhs_v = sum_f A_f (sum_c w_c S_c) P_f g_{f,v}
	vector<dvec3> full_rhs(ntarget_verts, dvec3(0.0, 0.0, 0.0));
	for (int f = 0; f < ntarget_faces; f++) {
		Mat3 Q;
		for (size_t c = 0; c < correspondences[f].size(); c++) {
			const Correspondence &cc = correspondences[f][c];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					Q.m[i][j] += cc.weight *
					             source_S[cc.source_face].m[i][j];
		}
		const TargetFace &tf = target_face_data[f];
		Mat3 P;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				P.m[i][j] = tf.proj[i][j];
		Mat3 AQP = mat3_mul(Q, P);
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				AQP.m[i][j] *= tf.area;
		for (int k = 0; k < 3; k++) {
			dvec3 g(tf.grad[k][0], tf.grad[k][1], tf.grad[k][2]);
			full_rhs[tf.v[k]] += mat3_vec(AQP, g);
		}
	}

	// Reduced right-hand side: subtract the exact anchor contributions.
	int ndof = 3 * nfree;
	vector<double> rhs(ndof, 0.0);
	for (int v = 0; v < ntarget_verts; v++) {
		int fi = free_index[v];
		if (fi < 0)
			continue;
		for (int c = 0; c < 3; c++) {
			double r = full_rhs[v][c];
			const vector< pair<int,double> > &terms = anchor_terms[3*fi + c];
			for (size_t t = 0; t < terms.size(); t++)
				r -= terms[t].second *
				     anchor_positions[terms[t].first / 3]
				                     [terms[t].first % 3];
			rhs[3*fi + c] = r;
		}
	}

	// Sparse triangular solves with the stored L factor.
	vector<double> y = rhs;
	for (int j = 0; j < ndof; j++)
		for (size_t t = 0; t < l_upper[j].size(); t++)
			y[l_upper[j][t].first] -= l_upper[j][t].second * y[j];
	vector<double> z(ndof);
	for (int i = 0; i < ndof; i++)
		z[i] = y[i] / diag[i];
	vector<double> x = z;
	for (int j = ndof - 1; j >= 0; j--)
		for (size_t t = 0; t < l_upper[j].size(); t++)
			x[j] -= l_upper[j][t].second * x[l_upper[j][t].first];

	// Free-variable residual against the original reduced operator (the
	// diag vector now holds the LDL D factor, so use h_diag here).  Only
	// the upper triangle is stored, so apply it symmetrically.
	vector<double> hx(ndof, 0.0);
	for (int i = 0; i < ndof; i++)
		hx[i] = h_diag[i] * x[i];
	for (int j = 0; j < ndof; j++) {
		const vector< pair<int,double> > &row = h_upper[j];
		for (size_t t = 0; t < row.size(); t++) {
			int k = row[t].first;
			double v = row[t].second;
			hx[j] += v * x[k];
			hx[k] += v * x[j];
		}
	}
	double rinf = 0.0, binf = 0.0;
	for (int i = 0; i < ndof; i++) {
		rinf = max(rinf, fabs(hx[i] - rhs[i]));
		binf = max(binf, fabs(rhs[i]));
	}
	double residual = rinf / max(1.0, binf);
	if (!std::isfinite(residual) || residual > 1e-8)
		return failure;

	// Full double-precision solution.  Anchors come straight from the
	// anchor input, so constraints are exact before the float cast.
	vector<dvec3> positions(ntarget_verts);
	for (int v = 0; v < ntarget_verts; v++) {
		int fi = free_index[v];
		if (fi >= 0)
			positions[v] = dvec3(x[3*fi], x[3*fi + 1], x[3*fi + 2]);
		else
			positions[v] = anchor_positions[anchor_lookup[v]];
	}

	// Energy of the double-precision solution under the stated formula.
	double energy = 0.0;
	for (int f = 0; f < ntarget_faces; f++) {
		const TargetFace &tf = target_face_data[f];
		Mat3 G; // sum_i x_i g_i^T
		for (int k = 0; k < 3; k++) {
			const dvec3 &xv = positions[tf.v[k]];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					G.m[i][j] += xv[i] * tf.grad[k][j];
		}
		Mat3 P;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				P.m[i][j] = tf.proj[i][j];
		for (size_t c = 0; c < correspondences[f].size(); c++) {
			const Correspondence &cc = correspondences[f][c];
			Mat3 target = mat3_mul(source_S[cc.source_face], P);
			Mat3 diff;
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					diff.m[i][j] = G.m[i][j] - target.m[i][j];
			energy += tf.area * cc.weight * mat3_frob2(diff);
		}
	}

	// Assemble the independent output mesh.
	TriMesh *out = new TriMesh;
	out->vertices.resize(ntarget_verts);
	for (int v = 0; v < ntarget_verts; v++)
		out->vertices[v] = point((float)positions[v][0],
		                         (float)positions[v][1],
		                         (float)positions[v][2]);
	out->faces = target_faces;
	out->colors = target_colors;
	out->confidences = target_confidences;
	out->flags = target_flags;
	out->flag_curr = target_flag_curr;
	out->tstrips = target_tstrips;
	out->grid = target_grid;
	out->grid_width = target_grid_width;
	out->grid_height = target_grid_height;
	out->neighbors = target_neighbors;
	out->adjacentfaces = target_adjacentfaces;
	out->across_edge = target_across_edge;
	// Geometry-dependent caches (normals, curvatures, areas, bounding
	// structures) intentionally stay empty/invalid on the output.

	++successful_solves;
	DeformationTransferResult result;
	result.mesh = out;
	result.energy = energy;
	result.residual = residual;
	return result;
}

} // namespace trimesh
