/*
DeformationTransfer.cc
Face-correspondence gradient-domain deformation transfer.
See include/DeformationTransfer.h for the exact conventions and contract.

C++11, standard library only.
*/

#include "DeformationTransfer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <stdexcept>
#include <utility>


namespace trimesh {

namespace {

struct Mat3 { double m[9]; }; // row-major 3x3

// Frames are assembled from columns.

// Union-find root with path halving
int uf_find(::std::vector<int> &par, int i)
{
	while (par[i] != i) {
		par[i] = par[par[i]];
		i = par[i];
	}
	return i;
}

inline Mat3 mat_from_cols(const dvec3 &c0, const dvec3 &c1, const dvec3 &c2)
{
	Mat3 m;
	m.m[0] = c0[0]; m.m[1] = c1[0]; m.m[2] = c2[0];
	m.m[3] = c0[1]; m.m[4] = c1[1]; m.m[5] = c2[1];
	m.m[6] = c0[2]; m.m[7] = c1[2]; m.m[8] = c2[2];
	return m;
}

inline Mat3 mat_mul(const Mat3 &a, const Mat3 &b)
{
	Mat3 c;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			c.m[3*i+j] = a.m[3*i+0] * b.m[0*3+j]
			           + a.m[3*i+1] * b.m[1*3+j]
			           + a.m[3*i+2] * b.m[2*3+j];
	return c;
}

inline dvec3 mat_vec(const Mat3 &a, const dvec3 &v)
{
	return dvec3(a.m[0] * v[0] + a.m[1] * v[1] + a.m[2] * v[2],
	             a.m[3] * v[0] + a.m[4] * v[1] + a.m[5] * v[2],
	             a.m[6] * v[0] + a.m[7] * v[1] + a.m[8] * v[2]);
}

// Inverse of the matrix with the given columns, via cross products.
inline Mat3 mat_inv_cols(const dvec3 &c0, const dvec3 &c1, const dvec3 &c2)
{
	const double det = c0 DOT (c1 CROSS c2);
	const dvec3 r0 = (c1 CROSS c2) * (1.0 / det);
	const dvec3 r1 = (c2 CROSS c0) * (1.0 / det);
	const dvec3 r2 = (c0 CROSS c1) * (1.0 / det);
	Mat3 m;
	m.m[0] = r0[0]; m.m[1] = r0[1]; m.m[2] = r0[2];
	m.m[3] = r1[0]; m.m[4] = r1[1]; m.m[5] = r1[2];
	m.m[6] = r2[0]; m.m[7] = r2[1]; m.m[8] = r2[2];
	return m;
}

inline bool finite3(const dvec3 &v)
{
	return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline bool finite9(const Mat3 &m)
{
	for (int i = 0; i < 9; i++)
		if (!std::isfinite(m.m[i]))
			return false;
	return true;
}

// Triangle frame: columns e1 = p1-p0, e2 = p2-p0, a/sqrt(|a|) with
// a = e1 x e2.  Returns false iff the triangle is degenerate.
inline bool tri_frame(const dvec3 &p0, const dvec3 &p1, const dvec3 &p2,
                      dvec3 &e1, dvec3 &e2, dvec3 &a3)
{
	e1 = p1 - p0;
	e2 = p2 - p0;
	const dvec3 a = e1 CROSS e2;
	const double a2 = a DOT a;
	if (!(a2 > 0.0) || !std::isfinite(a2))
		return false;
	// sqrt-area scale: |a3| = sqrt(|a|), i.e. a3 = a / (a.a)^(1/4)
	a3 = a * (1.0 / std::sqrt(std::sqrt(a2)));
	return true;
}

[[noreturn]] inline void bad(const char *what, const char *why)
{
	throw ::std::invalid_argument(
		::std::string("DeformationTransfer: ") + what + " " + why);
}

// Validate a rest mesh: finite, non-degenerate, consistently oriented
// 2-manifold triangle mesh without isolated or unused vertices.
void validate_mesh(const ::std::vector<dvec3> &verts,
                   const ::std::vector<TriMesh::Face> &faces,
                   const char *what)
{
	if (verts.empty())
		bad(what, "has no vertices");
	if (faces.empty())
		bad(what, "has no faces");
	const int nv = (int) verts.size();
	for (size_t i = 0; i < verts.size(); i++)
		if (!finite3(verts[i]))
			bad(what, "has a non-finite vertex coordinate");
	::std::vector<char> used(nv, 0);
	for (size_t f = 0; f < faces.size(); f++) {
		for (int k = 0; k < 3; k++) {
			const int idx = faces[f][k];
			if (idx < 0 || idx >= nv)
				bad(what, "has a face index out of range");
			used[idx] = 1;
		}
		if (faces[f][0] == faces[f][1] ||
		    faces[f][1] == faces[f][2] ||
		    faces[f][2] == faces[f][0])
			bad(what, "has a face with repeated vertices");
	}
	for (int v = 0; v < nv; v++)
		if (!used[v])
			bad(what, "has an isolated or unused vertex");
	for (size_t f = 0; f < faces.size(); f++) {
		dvec3 e1, e2, a3;
		if (!tri_frame(verts[faces[f][0]], verts[faces[f][1]],
		               verts[faces[f][2]], e1, e2, a3))
			bad(what, "has a degenerate face");
	}
	// Edge manifoldness and orientation consistency: each directed
	// edge may occur at most once, each undirected edge at most twice.
	::std::map< ::std::pair<int,int>, int > directed, undirected;
	for (size_t f = 0; f < faces.size(); f++) {
		for (int k = 0; k < 3; k++) {
			const int a = faces[f][k];
			const int b = faces[f][(k + 1) % 3];
			if (++directed[::std::make_pair(a, b)] > 1)
				bad(what, "is not consistently oriented");
			const int lo = a < b ? a : b, hi = a < b ? b : a;
			if (++undirected[::std::make_pair(lo, hi)] > 2)
				bad(what, "has a non-manifold edge");
		}
	}
	// Vertex manifoldness: the faces incident to each vertex must
	// form a single fan (one cycle, or one chain at a boundary).
	::std::vector< ::std::vector<int> > vfaces(nv);
	for (size_t f = 0; f < faces.size(); f++)
		for (int k = 0; k < 3; k++)
			vfaces[faces[f][k]].push_back((int) f);
	for (int v = 0; v < nv; v++) {
		const ::std::vector<int> &fl = vfaces[v];
		const size_t m = fl.size();
		if (m <= 1)
			continue;
		::std::vector<int> par(m);
		for (size_t i = 0; i < m; i++)
			par[i] = (int) i;
		::std::map<int,int> outedge, inedge;
		for (size_t li = 0; li < m; li++) {
			const TriMesh::Face &f = faces[fl[li]];
			const int k = f[0] == v ? 0 : (f[1] == v ? 1 : 2);
			outedge[f[(k + 1) % 3]] = (int) li;
			inedge[f[(k + 2) % 3]] = (int) li;
		}
		int unmatched = 0;
		for (::std::map<int,int>::const_iterator it = outedge.begin();
		     it != outedge.end(); ++it) {
			::std::map<int,int>::const_iterator in =
				inedge.find(it->first);
			if (in == inedge.end()) {
				unmatched++;
				continue;
			}
			int ra = uf_find(par, it->second);
			int rb = uf_find(par, in->second);
			if (ra != rb)
				par[rb] = ra;
		}
		if (unmatched > 1)
			bad(what, "has a non-manifold vertex");
		const int root = uf_find(par, 0);
		for (size_t li = 1; li < m; li++)
			if (uf_find(par, (int) li) != root)
				bad(what, "has a non-manifold vertex");
	}
}

// Sparse matvec y = A x for scalar CSR storage
void csr_matvec(const ::std::vector< ::std::size_t > &row,
                const ::std::vector<int> &col,
                const ::std::vector<double> &val,
                const double *x, double *y)
{
	const size_t n = row.size() - 1;
	for (size_t i = 0; i < n; i++) {
		double s = 0.0;
		for (size_t j = row[i]; j < row[i+1]; j++)
			s += val[j] * x[col[j]];
		y[i] = s;
	}
}

double norm_inf(const ::std::vector<double> &v)
{
	double m = 0.0;
	for (size_t i = 0; i < v.size(); i++)
		m = ::std::max(m, ::std::fabs(v[i]));
	return m;
}

// Preconditioned (Jacobi) conjugate gradients on the free-vertex
// system.  Warm-started from x.  Succeeds only if the true residual
// ends up at or below tol_ok; tol_in is the tighter CG stopping
// tolerance that provides margin against residual drift.
bool pcg(const ::std::vector< ::std::size_t > &row,
         const ::std::vector<int> &col,
         const ::std::vector<double> &val,
         const ::std::vector<double> &diag,
         const ::std::vector<double> &b,
         ::std::vector<double> &x,
         double tol_in, double tol_ok, size_t maxiter)
{
	const size_t n = diag.size();
	if (n == 0)
		return true;
	::std::vector<double> r(n), z(n), p(n), ap(n);
	csr_matvec(row, col, val, x.data(), ap.data());
	for (size_t i = 0; i < n; i++)
		r[i] = b[i] - ap[i];
	if (norm_inf(r) <= tol_in)
		return true;
	double rz = 0.0;
	for (size_t i = 0; i < n; i++) {
		z[i] = r[i] / diag[i];
		p[i] = z[i];
		rz += r[i] * z[i];
	}
	for (size_t k = 0; k < maxiter; k++) {
		csr_matvec(row, col, val, p.data(), ap.data());
		double pap = 0.0;
		for (size_t i = 0; i < n; i++)
			pap += p[i] * ap[i];
		if (!std::isfinite(pap) || pap <= 0.0)
			return false;
		const double alpha = rz / pap;
		for (size_t i = 0; i < n; i++) {
			x[i] += alpha * p[i];
			r[i] -= alpha * ap[i];
		}
		if (norm_inf(r) <= tol_in)
			break;
		if ((k & 255) == 255) {
			// Guard against residual drift: recompute the true
			// residual, and stop early if it is already well
			// below the acceptance tolerance.
			csr_matvec(row, col, val, x.data(), ap.data());
			for (size_t i = 0; i < n; i++)
				r[i] = b[i] - ap[i];
			if (norm_inf(r) <= 0.5 * tol_ok)
				break;
			for (size_t i = 0; i < n; i++)
				z[i] = r[i] / diag[i];
			rz = 0.0;
			for (size_t i = 0; i < n; i++)
				rz += r[i] * z[i];
			for (size_t i = 0; i < n; i++)
				p[i] = z[i];
			continue;
		}
		double rz_new = 0.0;
		for (size_t i = 0; i < n; i++) {
			z[i] = r[i] / diag[i];
			rz_new += r[i] * z[i];
		}
		if (!std::isfinite(rz_new) || rz_new <= 0.0)
			break;
		const double beta = rz_new / rz;
		for (size_t i = 0; i < n; i++)
			p[i] = z[i] + beta * p[i];
		rz = rz_new;
	}
	// Final acceptance test on the true residual
	csr_matvec(row, col, val, x.data(), ap.data());
	for (size_t i = 0; i < n; i++)
		r[i] = b[i] - ap[i];
	return norm_inf(r) <= tol_ok;
}

} // namespace


DeformationTransfer::DeformationTransfer(
	const TriMesh &source_rest,
	const TriMesh &target_rest,
	const ::std::vector<Correspondence> &correspondences,
	const ::std::vector<int> &anchors)
	: have_last_x(false), builds(0), solves_ok(0)
{
	// Deep-copy and validate the rest meshes
	src_rest.reserve(source_rest.vertices.size());
	for (size_t i = 0; i < source_rest.vertices.size(); i++)
		src_rest.push_back(dvec3(source_rest.vertices[i]));
	src_faces = source_rest.faces;
	validate_mesh(src_rest, src_faces, "source rest mesh");

	tgt_rest.reserve(target_rest.vertices.size());
	for (size_t i = 0; i < target_rest.vertices.size(); i++)
		tgt_rest.push_back(dvec3(target_rest.vertices[i]));
	validate_mesh(tgt_rest, target_rest.faces, "target rest mesh");

	const int nsf = (int) src_faces.size();
	const int ntf = (int) target_rest.faces.size();
	const int ntv = (int) tgt_rest.size();

	// Validate correspondences and target-face coverage
	if (correspondences.empty())
		bad("correspondence list", "is empty");
	::std::vector<char> covered(ntf, 0);
	for (size_t c = 0; c < correspondences.size(); c++) {
		const Correspondence &cor = correspondences[c];
		if (cor.source_face < 0 || cor.source_face >= nsf)
			bad("correspondence", "has a source face out of range");
		if (cor.target_face < 0 || cor.target_face >= ntf)
			bad("correspondence", "has a target face out of range");
		if (!(cor.weight > 0.0) || !std::isfinite(cor.weight))
			bad("correspondence", "has a non-positive or non-finite weight");
		covered[cor.target_face] = 1;
	}
	for (int f = 0; f < ntf; f++)
		if (!covered[f])
			bad("correspondence list", "leaves a target face uncovered");
	corr = correspondences;

	// Validate anchors: non-empty, in range, no duplicates
	if (anchors.empty())
		bad("anchor list", "is empty");
	::std::vector<char> is_anchor(ntv, 0);
	for (size_t i = 0; i < anchors.size(); i++) {
		const int a = anchors[i];
		if (a < 0 || a >= ntv)
			bad("anchor", "vertex index out of range");
		if (is_anchor[a])
			bad("anchor", "vertex listed twice");
		is_anchor[a] = 1;
	}

	// Every connected component of the target needs an anchor
	{
		::std::vector<int> par(ntv);
		for (int i = 0; i < ntv; i++)
			par[i] = i;
		for (int f = 0; f < ntf; f++) {
			for (int e = 0; e < 2; e++) {
				const int ra = uf_find(par, target_rest.faces[f][e]);
				const int rb = uf_find(par, target_rest.faces[f][e + 1]);
				if (ra != rb)
					par[rb] = ra;
			}
		}
		::std::vector<char> root_ok(ntv, 0);
		for (size_t i = 0; i < anchors.size(); i++)
			root_ok[uf_find(par, anchors[i])] = 1;
		for (int v = 0; v < ntv; v++)
			if (!root_ok[uf_find(par, v)])
				bad("anchor list",
				    "leaves a connected component without an anchor");
	}

	// Per-source-face rest data: inverse rest frames
	src_inv_frest.resize(9 * (size_t) nsf);
	for (int f = 0; f < nsf; f++) {
		dvec3 e1, e2, a3;
		tri_frame(src_rest[src_faces[f][0]], src_rest[src_faces[f][1]],
		          src_rest[src_faces[f][2]], e1, e2, a3);
		const Mat3 inv = mat_inv_cols(e1, e2, a3);
		for (int i = 0; i < 9; i++)
			src_inv_frest[9*f+i] = inv.m[i];
	}

	// Per-target-face rest data: barycentric basis gradients,
	// unit normal (for the tangent-plane projection), and area
	tgt_grad.resize(3 * (size_t) ntf);
	tgt_normal.resize(ntf);
	tgt_area.resize(ntf);
	for (int f = 0; f < ntf; f++) {
		const dvec3 &p0 = tgt_rest[target_rest.faces[f][0]];
		const dvec3 &p1 = tgt_rest[target_rest.faces[f][1]];
		const dvec3 &p2 = tgt_rest[target_rest.faces[f][2]];
		const dvec3 e1 = p1 - p0, e2 = p2 - p0;
		const dvec3 a = e1 CROSS e2;
		const double la = std::sqrt(a DOT a);
		const dvec3 n = a * (1.0 / la);
		tgt_normal[f] = n;
		tgt_area[f] = 0.5 * la;
		const dvec3 g1 = (e2 CROSS n) * (1.0 / la);
		const dvec3 g2 = (n CROSS e1) * (1.0 / la);
		tgt_grad[3*f+0] = dvec3(-(g1[0] + g2[0]),
		                        -(g1[1] + g2[1]),
		                        -(g1[2] + g2[2]));
		tgt_grad[3*f+1] = g1;
		tgt_grad[3*f+2] = g2;
	}

	// Free/anchor structure
	anchor_vertices = anchors;
	anchor_of_vertex.assign(ntv, -1);
	free_of_vertex.assign(ntv, -1);
	for (size_t i = 0; i < anchors.size(); i++)
		anchor_of_vertex[anchors[i]] = (int) i;
	int nfree = 0;
	for (int v = 0; v < ntv; v++)
		if (anchor_of_vertex[v] < 0)
			free_of_vertex[v] = nfree++;

	// Assemble the operator: H += A_target * weight * (g_i . g_k)
	// per correspondence, then split into the free-free operator
	// and the free-anchor coupling used for elimination.
	::std::map< ::std::pair<int,int>, double > acc;
	for (size_t c = 0; c < corr.size(); c++) {
		const Correspondence &cor = corr[c];
		const double aw = tgt_area[cor.target_face] * cor.weight;
		const TriMesh::Face &f = target_rest.faces[cor.target_face];
		for (int i = 0; i < 3; i++) {
			const dvec3 &gi = tgt_grad[3*cor.target_face+i];
			for (int k = 0; k < 3; k++) {
				const double gik = gi DOT tgt_grad[3*cor.target_face+k];
				acc[::std::make_pair(f[i], f[k])] += aw * gik;
			}
		}
	}
	::std::vector< ::std::map<int,double> > ff_rows(nfree), fa_rows(nfree);
	for (::std::map< ::std::pair<int,int>, double >::const_iterator it =
	     acc.begin(); it != acc.end(); ++it) {
		const int fi = free_of_vertex[it->first.first];
		if (fi < 0)
			continue;
		const int vcol = it->first.second;
		const int fj = free_of_vertex[vcol];
		if (fj >= 0)
			ff_rows[fi][fj] += it->second;
		else
			fa_rows[fi][anchor_of_vertex[vcol]] += it->second;
	}
	ff_row.assign(nfree + 1, 0);
	fa_row.assign(nfree + 1, 0);
	for (int i = 0; i < nfree; i++) {
		ff_row[i+1] = ff_row[i] + ff_rows[i].size();
		fa_row[i+1] = fa_row[i] + fa_rows[i].size();
	}
	ff_col.resize(ff_row[nfree]);
	ff_val.resize(ff_row[nfree]);
	fa_col.resize(fa_row[nfree]);
	fa_val.resize(fa_row[nfree]);
	ff_diag.assign(nfree, 0.0);
	for (int i = 0; i < nfree; i++) {
		size_t j = ff_row[i];
		for (::std::map<int,double>::const_iterator it =
		     ff_rows[i].begin(); it != ff_rows[i].end(); ++it, ++j) {
			ff_col[j] = it->first;
			ff_val[j] = it->second;
			if (it->first == i)
				ff_diag[i] = it->second;
		}
		j = fa_row[i];
		for (::std::map<int,double>::const_iterator it =
		     fa_rows[i].begin(); it != fa_rows[i].end(); ++it, ++j) {
			fa_col[j] = it->first;
			fa_val[j] = it->second;
		}
		if (!(ff_diag[i] > 0.0))
			bad("target rest mesh",
			    "produces a singular free-vertex operator");
	}

	// Template for the output mesh: target topology, attributes,
	// and adjacency caches.  Position-dependent caches are
	// invalidated on every output.
	target_template = target_rest;

	builds = 1;
}


bool DeformationTransfer::solve(const ::std::vector<point> &source_positions,
                                const ::std::vector<point> &anchor_positions,
                                TriMesh &output, double &energy,
                                double &residual)
{
	if (source_positions.size() != src_rest.size())
		throw ::std::invalid_argument(
			"DeformationTransfer::solve: source position count "
			"does not match the source rest mesh");
	if (anchor_positions.size() != anchor_vertices.size())
		throw ::std::invalid_argument(
			"DeformationTransfer::solve: anchor position count "
			"does not match the anchor list");

	const size_t nsf = src_faces.size();
	const size_t nfree = ff_diag.size();
	const size_t nanchor = anchor_vertices.size();

	// Current source deformation gradients S = F_cur F_rest^-1
	::std::vector<Mat3> sgrad(nsf);
	for (size_t f = 0; f < nsf; f++) {
		const point &q0 = source_positions[src_faces[f][0]];
		const point &q1 = source_positions[src_faces[f][1]];
		const point &q2 = source_positions[src_faces[f][2]];
		const dvec3 p0(q0), p1(q1), p2(q2);
		if (!finite3(p0) || !finite3(p1) || !finite3(p2))
			return false;
		dvec3 e1, e2, a3;
		if (!tri_frame(p0, p1, p2, e1, e2, a3))
			return false; // degenerate current source triangle
		Mat3 inv_rest;
		for (int i = 0; i < 9; i++)
			inv_rest.m[i] = src_inv_frest[9*f+i];
		sgrad[f] = mat_mul(mat_from_cols(e1, e2, a3), inv_rest);
		if (!finite9(sgrad[f]))
			return false;
	}
	::std::vector<double> xa(3 * nanchor);
	for (size_t j = 0; j < nanchor; j++) {
		const dvec3 a(anchor_positions[j]);
		if (!finite3(a))
			return false;
		xa[3*j+0] = a[0];
		xa[3*j+1] = a[1];
		xa[3*j+2] = a[2];
	}

	// Right-hand side: b_v += A_target * weight * S g_i,
	// then eliminate the anchors: b -= H_fa x_anchor.
	::std::vector<double> b(3 * nfree, 0.0);
	for (size_t c = 0; c < corr.size(); c++) {
		const Correspondence &cor = corr[c];
		const double aw = tgt_area[cor.target_face] * cor.weight;
		const Mat3 &s = sgrad[cor.source_face];
		const TriMesh::Face &f = target_template.faces[cor.target_face];
		for (int i = 0; i < 3; i++) {
			const int fi = free_of_vertex[f[i]];
			if (fi < 0)
				continue;
			const dvec3 g = tgt_grad[3*cor.target_face+i] * aw;
			const dvec3 bg = mat_vec(s, g);
			b[3*fi+0] += bg[0];
			b[3*fi+1] += bg[1];
			b[3*fi+2] += bg[2];
		}
	}
	for (size_t i = 0; i < nfree; i++)
		for (size_t j = fa_row[i]; j < fa_row[i+1]; j++) {
			const double v = fa_val[j];
			const size_t a = (size_t) fa_col[j];
			b[3*i+0] -= v * xa[3*a+0];
			b[3*i+1] -= v * xa[3*a+1];
			b[3*i+2] -= v * xa[3*a+2];
		}

	// Solve the three coordinate components against the same
	// scalar operator, warm-started from the last success.
	double bnorm = 0.0;
	for (size_t i = 0; i < b.size(); i++)
		bnorm = ::std::max(bnorm, ::std::fabs(b[i]));
	const double scale = ::std::max(1.0, bnorm);
	const double tol_ok = 1e-8 * scale;
	const double tol_in = 1e-10 * scale;
	const size_t maxiter = 1000 + 100 * nfree;
	::std::vector<double> x(3 * nfree, 0.0);
	if (have_last_x)
		x = last_x;
	double resmax = 0.0;
	for (int comp = 0; comp < 3; comp++) {
		::std::vector<double> bc(nfree), xc(nfree);
		for (size_t i = 0; i < nfree; i++) {
			bc[i] = b[3*i+comp];
			xc[i] = x[3*i+comp];
		}
		if (!pcg(ff_row, ff_col, ff_val, ff_diag, bc, xc,
		         tol_in, tol_ok, maxiter))
			return false; // solve did not converge
		// True residual of the free rows, for reporting
		::std::vector<double> hx(nfree);
		csr_matvec(ff_row, ff_col, ff_val, xc.data(), hx.data());
		for (size_t i = 0; i < nfree; i++) {
			resmax = ::std::max(resmax, ::std::fabs(hx[i] - bc[i]));
			x[3*i+comp] = xc[i];
		}
	}

	// Full vertex positions in double; anchors are exact
	::std::vector<dvec3> pos(tgt_rest.size());
	for (size_t v = 0; v < tgt_rest.size(); v++) {
		const int ai = anchor_of_vertex[v];
		if (ai >= 0) {
			pos[v] = dvec3(xa[3*ai+0], xa[3*ai+1], xa[3*ai+2]);
			continue;
		}
		const int fi = free_of_vertex[(int) v];
		pos[v] = dvec3(x[3*fi+0], x[3*fi+1], x[3*fi+2]);
	}

	// Energy E = sum_c A w || sum_i x_i g_i^T - S P ||_F^2,
	// with S P = S - (S n) n^T evaluated per correspondence.
	double e_total = 0.0;
	for (size_t c = 0; c < corr.size(); c++) {
		const Correspondence &cor = corr[c];
		const int t = cor.target_face;
		const Mat3 &s = sgrad[cor.source_face];
		const dvec3 &n = tgt_normal[t];
		const dvec3 sn = mat_vec(s, n);
		const TriMesh::Face &f = target_template.faces[t];
		double ec = 0.0;
		for (int r = 0; r < 3; r++) {
			for (int cc = 0; cc < 3; cc++) {
				double g = 0.0;
				for (int i = 0; i < 3; i++)
					g += pos[f[i]][r] *
					     tgt_grad[3*t+i][cc];
				const double sp = s.m[3*r+cc] - sn[r] * n[cc];
				const double d = g - sp;
				ec += d * d;
			}
		}
		e_total += tgt_area[t] * cor.weight * ec;
	}

	// Fresh, independent output mesh: target topology and
	// attributes, adjacency caches kept, position-dependent
	// caches invalidated.
	TriMesh out = target_template;
	for (size_t v = 0; v < pos.size(); v++)
		out.vertices[v] = point((float) pos[v][0],
		                        (float) pos[v][1],
		                        (float) pos[v][2]);
	out.clear_normals();
	out.clear_curvatures();
	out.clear_dcurv();
	out.clear_pointareas();
	out.clear_bbox();
	out.clear_bsphere();
	output = out;

	energy = e_total;
	residual = resmax / scale;
	last_x.swap(x);
	have_last_x = true;
	solves_ok++;
	return true;
}

} // namespace trimesh
