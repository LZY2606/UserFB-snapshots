/*
tests/deformation_transfer.cc
Tests for trimesh::DeformationTransfer.

Groups:
  gradients   - triangle frame / deformation-gradient conventions and
                the per-face energy on minimal models
  integration - dense assembly of the full constrained system on
                non-coplanar, different-topology models; rigid+scale
                recovery; independent components
  state       - object lifecycle: successive poses, input release,
                failure recovery, cache rebuild, sparse operator reuse

The reference implementation below independently (and densely)
assembles exactly the constrained system of
    E = sum_c A_target * weight * || sum_i x_i g_i^T - S P ||_F^2
with triangle frames F = [e1, e2, a/sqrt(|a|)] and
S = F_current F_rest^-1, and solves it with dense Gaussian
elimination.

C++11, standard library only.  No main(): the driver provides it.
*/

#include "DeformationTransfer.h"
#include "TriMesh.h"
#include "task_support/check.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace trimesh;

namespace {

typedef DeformationTransfer::Correspondence Corr;

struct Mat3 { double m[9]; }; // row-major

Mat3 mat_from_cols(const dvec3 &c0, const dvec3 &c1, const dvec3 &c2)
{
	Mat3 r;
	r.m[0] = c0[0]; r.m[1] = c1[0]; r.m[2] = c2[0];
	r.m[3] = c0[1]; r.m[4] = c1[1]; r.m[5] = c2[1];
	r.m[6] = c0[2]; r.m[7] = c1[2]; r.m[8] = c2[2];
	return r;
}

Mat3 mat_mul(const Mat3 &a, const Mat3 &b)
{
	Mat3 c;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			c.m[3*i+j] = a.m[3*i+0]*b.m[0*3+j]
			           + a.m[3*i+1]*b.m[1*3+j]
			           + a.m[3*i+2]*b.m[2*3+j];
	return c;
}

dvec3 mat_vec(const Mat3 &a, const dvec3 &v)
{
	return dvec3(a.m[0]*v[0] + a.m[1]*v[1] + a.m[2]*v[2],
	             a.m[3]*v[0] + a.m[4]*v[1] + a.m[5]*v[2],
	             a.m[6]*v[0] + a.m[7]*v[1] + a.m[8]*v[2]);
}

// Generic adjugate inverse (independent of the library's formulas)
Mat3 mat_inv(const Mat3 &a)
{
	const double A = a.m[4]*a.m[8] - a.m[5]*a.m[7];
	const double B = a.m[2]*a.m[7] - a.m[1]*a.m[8];
	const double C = a.m[1]*a.m[5] - a.m[2]*a.m[4];
	const double det = a.m[0]*A + a.m[3]*B + a.m[6]*C;
	const double inv = 1.0 / det;
	Mat3 r;
	r.m[0] = A * inv;
	r.m[1] = B * inv;
	r.m[2] = C * inv;
	r.m[3] = (a.m[5]*a.m[6] - a.m[3]*a.m[8]) * inv;
	r.m[4] = (a.m[0]*a.m[8] - a.m[2]*a.m[6]) * inv;
	r.m[5] = (a.m[2]*a.m[3] - a.m[0]*a.m[5]) * inv;
	r.m[6] = (a.m[3]*a.m[7] - a.m[4]*a.m[6]) * inv;
	r.m[7] = (a.m[1]*a.m[6] - a.m[0]*a.m[7]) * inv;
	r.m[8] = (a.m[0]*a.m[4] - a.m[1]*a.m[3]) * inv;
	return r;
}

double mat_maxdiff(const Mat3 &a, const Mat3 &b)
{
	double d = 0.0;
	for (int i = 0; i < 9; i++)
		d = std::max(d, std::fabs(a.m[i] - b.m[i]));
	return d;
}

Mat3 scale_mat(double s)
{
	Mat3 r = {{ s, 0, 0, 0, s, 0, 0, 0, s }};
	return r;
}

Mat3 rotation_mat(const dvec3 &axis, double ang)
{
	const dvec3 u = axis * (1.0 / std::sqrt(axis DOT axis));
	const double c = std::cos(ang), s = std::sin(ang), C = 1.0 - c;
	Mat3 r;
	r.m[0] = c + u[0]*u[0]*C;
	r.m[1] = u[0]*u[1]*C - u[2]*s;
	r.m[2] = u[0]*u[2]*C + u[1]*s;
	r.m[3] = u[1]*u[0]*C + u[2]*s;
	r.m[4] = c + u[1]*u[1]*C;
	r.m[5] = u[1]*u[2]*C - u[0]*s;
	r.m[6] = u[2]*u[0]*C - u[1]*s;
	r.m[7] = u[2]*u[1]*C + u[0]*s;
	r.m[8] = c + u[2]*u[2]*C;
	return r;
}

// Spec triangle frame: columns e1 = p1-p0, e2 = p2-p0, a/sqrt(|a|)
Mat3 tri_frame(const dvec3 &p0, const dvec3 &p1, const dvec3 &p2)
{
	const dvec3 e1 = p1 - p0, e2 = p2 - p0;
	const dvec3 a = e1 CROSS e2;
	const dvec3 a3 = a * (1.0 / std::sqrt(std::sqrt(a DOT a)));
	return mat_from_cols(e1, e2, a3);
}

// In-plane barycentric basis gradients, computed by solving
// [e1 e2 n]^T g_i = (d_i1 - d_i0, d_i2 - d_i0, 0) with a unit-normal
// frame - an independent construction from the library's.
void bary_grads(const dvec3 &p0, const dvec3 &p1, const dvec3 &p2,
                dvec3 g[3], double &area, dvec3 &nrm)
{
	const dvec3 e1 = p1 - p0, e2 = p2 - p0;
	const dvec3 a = e1 CROSS e2;
	const double la = std::sqrt(a DOT a);
	nrm = a * (1.0 / la);
	area = 0.5 * la;
	const Mat3 fuinv = mat_inv(mat_from_cols(e1, e2, nrm));
	// g_1 = row 0 of fuinv, g_2 = row 1, g_0 = -(g_1 + g_2)
	g[1] = dvec3(fuinv.m[0], fuinv.m[1], fuinv.m[2]);
	g[2] = dvec3(fuinv.m[3], fuinv.m[4], fuinv.m[5]);
	g[0] = dvec3(-(g[1][0] + g[2][0]),
	             -(g[1][1] + g[2][1]),
	             -(g[1][2] + g[2][2]));
}

point to_point(const dvec3 &v)
{
	return point((float) v[0], (float) v[1], (float) v[2]);
}

// Dense Gaussian elimination with partial pivoting.
// A is n x n row-major (destroyed); B is n x nrhs (overwritten).
bool dense_solve(int n, std::vector<double> A, std::vector<double> &B, int nrhs)
{
	for (int col = 0; col < n; col++) {
		int piv = col;
		for (int r = col + 1; r < n; r++)
			if (std::fabs(A[r*n+col]) > std::fabs(A[piv*n+col]))
				piv = r;
		if (!(std::fabs(A[piv*n+col]) > 0.0))
			return false;
		if (piv != col) {
			for (int c = 0; c < n; c++)
				std::swap(A[piv*n+c], A[col*n+c]);
			for (int k = 0; k < nrhs; k++)
				std::swap(B[piv*nrhs+k], B[col*nrhs+k]);
		}
		const double d = A[col*n+col];
		for (int r = col + 1; r < n; r++) {
			const double f = A[r*n+col] / d;
			if (f == 0.0)
				continue;
			for (int c = col; c < n; c++)
				A[r*n+c] -= f * A[col*n+c];
			for (int k = 0; k < nrhs; k++)
				B[r*nrhs+k] -= f * B[col*nrhs+k];
		}
	}
	for (int col = n - 1; col >= 0; col--) {
		const double d = A[col*n+col];
		for (int k = 0; k < nrhs; k++) {
			double s = B[col*nrhs+k];
			for (int c = col + 1; c < n; c++)
				s -= A[col*n+c] * B[c*nrhs+k];
			B[col*nrhs+k] = s / d;
		}
	}
	return true;
}

// Dense reference: assembles the constrained system of the spec
// energy for a target mesh + correspondences + anchors ...
struct RefProblem {
	TriMesh tgt;
	std::vector<Corr> corr;
	std::vector<int> anchors;
	std::vector<int> free_of, anchor_of;
	int nfree;
	std::vector<dvec3> g;     // 3 per target face
	std::vector<double> area;
	std::vector<dvec3> nrm;
	std::vector<double> H;    // nfree x nfree, row-major
	std::vector<double> Hfa;  // nfree x nanchors
};

void ref_assemble(RefProblem &P, const TriMesh &tgt,
                  const std::vector<Corr> &corr,
                  const std::vector<int> &anchors)
{
	P.tgt = tgt;
	P.corr = corr;
	P.anchors = anchors;
	const int nv = (int) tgt.vertices.size();
	const int nf = (int) tgt.faces.size();
	P.g.resize(3 * nf);
	P.area.resize(nf);
	P.nrm.resize(nf);
	for (int f = 0; f < nf; f++) {
		bary_grads(dvec3(tgt.vertices[tgt.faces[f][0]]),
		           dvec3(tgt.vertices[tgt.faces[f][1]]),
		           dvec3(tgt.vertices[tgt.faces[f][2]]),
		           &P.g[3*f], P.area[f], P.nrm[f]);
	}
	P.free_of.assign(nv, -1);
	P.anchor_of.assign(nv, -1);
	for (size_t j = 0; j < anchors.size(); j++)
		P.anchor_of[anchors[j]] = (int) j;
	int nf_free = 0;
	for (int v = 0; v < nv; v++)
		if (P.anchor_of[v] < 0)
			P.free_of[v] = nf_free++;
	P.nfree = nf_free;
	const int na = (int) anchors.size();
	P.H.assign((size_t) nf_free * nf_free, 0.0);
	P.Hfa.assign((size_t) nf_free * na, 0.0);
	for (size_t c = 0; c < corr.size(); c++) {
		const int t = corr[c].target_face;
		const double aw = P.area[t] * corr[c].weight;
		for (int i = 0; i < 3; i++) {
			const int fi = P.free_of[tgt.faces[t][i]];
			if (fi < 0)
				continue;
			for (int k = 0; k < 3; k++) {
				const double gik =
					P.g[3*t+i] DOT P.g[3*t+k];
				const int colv = tgt.faces[t][k];
				if (P.free_of[colv] >= 0)
					P.H[fi*nf_free + P.free_of[colv]] += aw * gik;
				else
					P.Hfa[fi*na + P.anchor_of[colv]] += aw * gik;
			}
		}
	}
}

// ... and solves one frame densely, returning positions, the energy
// at the solution, and the free-row residual.
void ref_frame_solve(const RefProblem &P, const TriMesh &src_rest,
                     const std::vector<point> &src_cur,
                     const std::vector<point> &anchor_pos,
                     std::vector<dvec3> &pos, double &energy,
                     double &residual)
{
	const size_t nsf = src_rest.faces.size();
	std::vector<Mat3> sgrad(nsf);
	for (size_t f = 0; f < nsf; f++) {
		const Mat3 fr = tri_frame(
			dvec3(src_rest.vertices[src_rest.faces[f][0]]),
			dvec3(src_rest.vertices[src_rest.faces[f][1]]),
			dvec3(src_rest.vertices[src_rest.faces[f][2]]));
		const Mat3 fc = tri_frame(
			dvec3(src_cur[src_rest.faces[f][0]]),
			dvec3(src_cur[src_rest.faces[f][1]]),
			dvec3(src_cur[src_rest.faces[f][2]]));
		sgrad[f] = mat_mul(fc, mat_inv(fr));
	}
	const int nfree = P.nfree;
	const int na = (int) P.anchors.size();
	std::vector<double> b(3 * nfree, 0.0);
	for (size_t c = 0; c < P.corr.size(); c++) {
		const int t = P.corr[c].target_face;
		const double aw = P.area[t] * P.corr[c].weight;
		const Mat3 &s = sgrad[P.corr[c].source_face];
		for (int i = 0; i < 3; i++) {
			const int fi = P.free_of[P.tgt.faces[t][i]];
			if (fi < 0)
				continue;
			const dvec3 bg = mat_vec(s, P.g[3*t+i] * aw);
			b[3*fi+0] += bg[0];
			b[3*fi+1] += bg[1];
			b[3*fi+2] += bg[2];
		}
	}
	// Anchor elimination into the right-hand side
	std::vector<double> rhs = b;
	for (int i = 0; i < nfree; i++)
		for (int j = 0; j < na; j++) {
			const double h = P.Hfa[(size_t) i*na + j];
			if (h == 0.0)
				continue;
			for (int k = 0; k < 3; k++)
				rhs[3*i+k] -= h * anchor_pos[j][k];
		}
	// Dense solve, three right-hand sides at once
	std::vector<double> bx(3 * nfree);
	for (int i = 0; i < nfree; i++)
		for (int k = 0; k < 3; k++)
			bx[(size_t) i*3+k] = rhs[3*i+k];
	dense_solve(nfree, P.H, bx, 3);
	// Free-row residual ||Hx - b||_inf / max(1, ||b||_inf)
	double rmax = 0.0, bmax = 0.0;
	for (int i = 0; i < nfree; i++) {
		for (int k = 0; k < 3; k++) {
			double hx = 0.0;
			for (int j = 0; j < nfree; j++)
				hx += P.H[(size_t) i*nfree+j] * bx[(size_t) j*3+k];
			rmax = std::max(rmax, std::fabs(hx - rhs[3*i+k]));
			bmax = std::max(bmax, std::fabs(rhs[3*i+k]));
		}
	}
	residual = rmax / std::max(1.0, bmax);
	// Full positions; anchors exact
	pos.assign(P.tgt.vertices.size(), dvec3(0.0, 0.0, 0.0));
	for (size_t v = 0; v < pos.size(); v++) {
		if (P.anchor_of[v] >= 0) {
			pos[v] = dvec3(anchor_pos[P.anchor_of[v]]);
		} else {
			const int fi = P.free_of[v];
			pos[v] = dvec3(bx[(size_t) fi*3+0],
			               bx[(size_t) fi*3+1],
			               bx[(size_t) fi*3+2]);
		}
	}
	// Energy at the solution
	energy = 0.0;
	for (size_t c = 0; c < P.corr.size(); c++) {
		const int t = P.corr[c].target_face;
		const Mat3 &s = sgrad[P.corr[c].source_face];
		const dvec3 &n = P.nrm[t];
		const dvec3 sn = mat_vec(s, n);
		double ec = 0.0;
		for (int r = 0; r < 3; r++) {
			for (int cc = 0; cc < 3; cc++) {
				double gv = 0.0;
				for (int i = 0; i < 3; i++)
					gv += pos[P.tgt.faces[t][i]][r] *
					      P.g[3*t+i][cc];
				const double sp = s.m[3*r+cc] - sn[r] * n[cc];
				const double d = gv - sp;
				ec += d * d;
			}
		}
		energy += P.area[t] * P.corr[c].weight * ec;
	}
}

// ---------- models ----------

TriMesh tri_mesh(const point &a, const point &b, const point &c)
{
	TriMesh m;
	m.vertices.push_back(a);
	m.vertices.push_back(b);
	m.vertices.push_back(c);
	m.faces.push_back(TriMesh::Face(0, 1, 2));
	return m;
}

TriMesh make_tetra()
{
	TriMesh m;
	m.vertices.push_back(point(0, 0, 0));
	m.vertices.push_back(point(2, 0, 0));
	m.vertices.push_back(point(0, 1.5f, 0));
	m.vertices.push_back(point(0.3f, 0.4f, 1.2f));
	m.faces.push_back(TriMesh::Face(0, 2, 1));
	m.faces.push_back(TriMesh::Face(0, 1, 3));
	m.faces.push_back(TriMesh::Face(1, 2, 3));
	m.faces.push_back(TriMesh::Face(2, 0, 3));
	return m;
}

// Bent two-triangle strip, non-coplanar
TriMesh make_strip()
{
	TriMesh m;
	m.vertices.push_back(point(0, 0, 0));
	m.vertices.push_back(point(1.2f, 0, 0.3f));
	m.vertices.push_back(point(0, 0.9f, -0.2f));
	m.vertices.push_back(point(1.1f, 1.0f, 0.5f));
	m.faces.push_back(TriMesh::Face(0, 1, 2));
	m.faces.push_back(TriMesh::Face(1, 3, 2));
	return m;
}

// Flat square made of two triangles
TriMesh make_flat_pair()
{
	TriMesh m;
	m.vertices.push_back(point(0, 0, 0));
	m.vertices.push_back(point(1, 0, 0));
	m.vertices.push_back(point(0, 1, 0));
	m.vertices.push_back(point(1, 1, 0));
	m.faces.push_back(TriMesh::Face(0, 1, 2));
	m.faces.push_back(TriMesh::Face(1, 3, 2));
	return m;
}

TriMesh make_grid(int n, float step)
{
	TriMesh m;
	for (int i = 0; i < n; i++)
		for (int j = 0; j < n; j++)
			m.vertices.push_back(point(i * step, j * step, 0));
	for (int i = 0; i < n - 1; i++)
		for (int j = 0; j < n - 1; j++) {
			const int a = i * n + j, b = (i + 1) * n + j;
			const int c = i * n + j + 1, d = (i + 1) * n + j + 1;
			m.faces.push_back(TriMesh::Face(a, b, d));
			m.faces.push_back(TriMesh::Face(a, d, c));
		}
	return m;
}

// ---------- comparison helpers ----------

double tgt_scale(const TriMesh &t)
{
	dvec3 lo(t.vertices[0]), hi(t.vertices[0]);
	for (size_t i = 1; i < t.vertices.size(); i++)
		for (int k = 0; k < 3; k++) {
			lo[k] = std::min(lo[k], (double) t.vertices[i][k]);
			hi[k] = std::max(hi[k], (double) t.vertices[i][k]);
		}
	return std::sqrt((hi - lo) DOT (hi - lo));
}

double pos_tol(const TriMesh &t)
{
	return 3e-5 * std::max(1.0, tgt_scale(t));
}

double max_pos_diff(const TriMesh &out, const std::vector<dvec3> &ref)
{
	double m = 0.0;
	for (size_t i = 0; i < ref.size(); i++)
		for (int k = 0; k < 3; k++)
			m = std::max(m, std::fabs(out.vertices[i][k] - ref[i][k]));
	return m;
}

double max_pos_diff(const std::vector<point> &a, const std::vector<dvec3> &ref)
{
	double m = 0.0;
	for (size_t i = 0; i < ref.size(); i++)
		for (int k = 0; k < 3; k++)
			m = std::max(m, std::fabs(a[i][k] - ref[i][k]));
	return m;
}

// ---------- gradients group ----------

// Frame conventions and a similarity deformation on a single triangle
int case_frame_similarity()
{
	TriMesh src = tri_mesh(point(0, 0, 0), point(2, 0, 0), point(0, 1, 0));
	// Target plane tilted against the source plane, so that the
	// sqrt-area normal column of S actually enters the energy
	TriMesh tgt = tri_mesh(point(1, 0.5f, 2), point(3.5f, 0.8f, 2.4f),
	                       point(1, 2, 1.6f));

	// Independent frame construction on the source rest triangle
	const dvec3 p0(src.vertices[0]), p1(src.vertices[1]), p2(src.vertices[2]);
	const dvec3 e1 = p1 - p0, e2 = p2 - p0;
	const dvec3 a = e1 CROSS e2;
	const Mat3 f = tri_frame(p0, p1, p2);
	const dvec3 a3(f.m[2], f.m[5], f.m[8]);
	TASK_CHECK(std::fabs(a3 DOT e1) < 1e-12);
	TASK_CHECK(std::fabs(a3 DOT e2) < 1e-12);
	// sqrt-area scale, not a unit normal
	TASK_CHECK(std::fabs(std::sqrt(a3 DOT a3) - std::sqrt(std::sqrt(a DOT a))) < 1e-12);

	// Independent barycentric gradient construction on the target
	dvec3 g[3], nrm;
	double area;
	bary_grads(dvec3(tgt.vertices[0]), dvec3(tgt.vertices[1]),
	           dvec3(tgt.vertices[2]), g, area, nrm);
	const dvec3 te1 = dvec3(tgt.vertices[1]) - dvec3(tgt.vertices[0]);
	const dvec3 te2 = dvec3(tgt.vertices[2]) - dvec3(tgt.vertices[0]);
	for (int i = 0; i < 3; i++) {
		TASK_CHECK(std::fabs((g[i] DOT te1) - ((i == 1) ? 1.0 : (i == 0) ? -1.0 : 0.0)) < 1e-10);
		TASK_CHECK(std::fabs((g[i] DOT te2) - ((i == 2) ? 1.0 : (i == 0) ? -1.0 : 0.0)) < 1e-10);
		TASK_CHECK(std::fabs(g[i] DOT nrm) < 1e-10);
	}

	// Similarity deformation of the source, plus a translation
	const Mat3 m = mat_mul(scale_mat(1.6), rotation_mat(dvec3(1, 2, 2), 0.9));
	const dvec3 t(4, -2, 1);
	std::vector<point> cur(3);
	for (int i = 0; i < 3; i++)
		cur[i] = to_point(mat_vec(m, dvec3(src.vertices[i])) + t);

	std::vector<Corr> corr(1, Corr(0, 0, 1.7));
	std::vector<int> anchors(1, 0);
	std::vector<point> apos(1, point(-1, 0.5f, 2));
	DeformationTransfer dt(src, tgt, corr, anchors);
	TriMesh out;
	double energy = -1.0, residual = -1.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(residual <= 1e-8);
	TASK_CHECK(energy >= 0.0 && energy < 1e-8);

	// Expected positions via the independently constructed S
	const Mat3 fr = tri_frame(p0, p1, p2);
	const Mat3 fc = tri_frame(dvec3(cur[0]), dvec3(cur[1]), dvec3(cur[2]));
	const Mat3 s = mat_mul(fc, mat_inv(fr));
	// For a similarity, S equals the applied linear map (up to the
	// float quantization of the current source positions)
	TASK_CHECK(mat_maxdiff(s, m) < 1e-5);
	const dvec3 q(-1, 0.5, 2);
	const double tol = pos_tol(tgt);
	for (int i = 0; i < 3; i++) {
		const dvec3 expect = q + mat_vec(s,
			dvec3(tgt.vertices[i]) - dvec3(tgt.vertices[0]));
		for (int k = 0; k < 3; k++)
			TASK_CHECK(std::fabs(out.vertices[i][k] - expect[k]) <= tol);
	}
	return 0;
}

// Shear deformation: S differs from the applied linear map, and a
// global source translation must not enter the gradient
int case_frame_shear_translation()
{
	TriMesh src = tri_mesh(point(0, 0, 0), point(2, 0, 0), point(0, 1, 0));
	TriMesh tgt = tri_mesh(point(1, 0.5f, 2), point(3.5f, 0.8f, 2.4f),
	                       point(1, 2, 1.6f));
	// Power-of-two shear entries and integer translations keep the
	// current positions exactly representable in float, so the two
	// solves see bit-identical deformation gradients
	Mat3 m = {{ 1.0, 0.5, 0.25,
	            0.125, 1.0, 0.5,
	            0.0625, -0.25, 1.0 }};
	const dvec3 t1(64, -32, 16), t2(-8, 4, -2);
	std::vector<point> cur1(3), cur2(3);
	for (int i = 0; i < 3; i++) {
		const dvec3 p(src.vertices[i]);
		cur1[i] = to_point(mat_vec(m, p) + t1);
		cur2[i] = to_point(mat_vec(m, p) + t2);
	}
	std::vector<Corr> corr(1, Corr(0, 0, 0.9));
	std::vector<int> anchors(1, 0);
	std::vector<point> apos(1, point(0.5f, -1, 3));
	DeformationTransfer dt1(src, tgt, corr, anchors);
	DeformationTransfer dt2(src, tgt, corr, anchors);
	TriMesh out1, out2;
	double e1 = -1.0, r1 = -1.0, e2 = -1.0, r2 = -1.0;
	TASK_CHECK(dt1.solve(cur1, apos, out1, e1, r1));
	TASK_CHECK(dt2.solve(cur2, apos, out2, e2, r2));
	// Source translation does not enter the gradient
	TASK_CHECK(out1.vertices == out2.vertices);
	TASK_CHECK(r1 <= 1e-8 && r2 <= 1e-8);
	TASK_CHECK(e1 >= 0.0 && e1 < 1e-8);

	// Expected positions via the independently constructed S;
	// for a shear, S is not the applied linear map
	const Mat3 fr = tri_frame(dvec3(src.vertices[0]), dvec3(src.vertices[1]),
	                          dvec3(src.vertices[2]));
	const Mat3 fc = tri_frame(dvec3(cur1[0]), dvec3(cur1[1]), dvec3(cur1[2]));
	const Mat3 s = mat_mul(fc, mat_inv(fr));
	TASK_CHECK(mat_maxdiff(s, m) > 1e-3);
	const dvec3 q(0.5, -1, 3);
	const double tol = pos_tol(tgt);
	for (int i = 0; i < 3; i++) {
		const dvec3 expect = q + mat_vec(s,
			dvec3(tgt.vertices[i]) - dvec3(tgt.vertices[0]));
		for (int k = 0; k < 3; k++)
			TASK_CHECK(std::fabs(out1.vertices[i][k] - expect[k]) <= tol);
	}
	return 0;
}

// Non-integrable bent configuration: the energy cannot reach zero;
// dense comparison of coordinates, energy, and residual
int case_bent_pair_energy()
{
	TriMesh src = make_flat_pair();
	TriMesh tgt = make_strip();
	std::vector<point> cur = src.vertices;
	cur[2] = point(0, 1, 0.6f);
	cur[3] = point(1, 1, 0.6f);
	std::vector<Corr> corr;
	corr.push_back(Corr(0, 0, 1.0));
	corr.push_back(Corr(1, 1, 1.0));
	std::vector<int> anchors;
	anchors.push_back(0);
	anchors.push_back(1);
	std::vector<point> apos;
	apos.push_back(point(0.2f, -0.3f, 0.8f));
	apos.push_back(point(1.5f, 0.1f, 1.1f));

	RefProblem ref;
	ref_assemble(ref, tgt, corr, anchors);
	std::vector<dvec3> refpos;
	double refe = 0.0, refr = 0.0;
	ref_frame_solve(ref, src, cur, apos, refpos, refe, refr);
	TASK_CHECK(refr <= 1e-8);

	DeformationTransfer dt(src, tgt, corr, anchors);
	TriMesh out;
	double energy = -1.0, residual = -1.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(residual <= 1e-8);
	// Nonzero energy that cannot be fully integrated away
	TASK_CHECK(energy > 1e-6);
	TASK_CHECK(std::fabs(energy - refe) <= 1e-6 * std::max(1.0, std::fabs(refe)));
	TASK_CHECK(max_pos_diff(out, refpos) <= pos_tol(tgt));
	return 0;
}

// ---------- integration group ----------

// Non-coplanar, different-topology transfer with many-to-one
// correspondences and unequal weights; dense comparison
int case_tetra_to_strip_dense()
{
	TriMesh src = make_tetra();
	TriMesh tgt = make_strip();
	std::vector<point> cur = src.vertices;
	cur[1] = point(2.3f, 0.2f, 0.4f);
	cur[3] = point(0.8f, 0.9f, 2.1f);
	std::vector<Corr> corr;
	corr.push_back(Corr(0, 0, 1.0));
	corr.push_back(Corr(1, 0, 2.5)); // many-to-one ...
	corr.push_back(Corr(2, 1, 0.7)); // ... with unequal weights
	corr.push_back(Corr(3, 1, 1.3));
	std::vector<int> anchors(1, 0);
	std::vector<point> apos(1, point(0.5f, -0.5f, 1.0f));

	RefProblem ref;
	ref_assemble(ref, tgt, corr, anchors);
	std::vector<dvec3> refpos;
	double refe = 0.0, refr = 0.0;
	ref_frame_solve(ref, src, cur, apos, refpos, refe, refr);
	TASK_CHECK(refr <= 1e-8);

	DeformationTransfer dt(src, tgt, corr, anchors);
	TriMesh out;
	double energy = -1.0, residual = -1.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(residual <= 1e-8);
	TASK_CHECK(energy > 1e-9);
	TASK_CHECK(std::fabs(energy - refe) <= 1e-6 * std::max(1.0, std::fabs(refe)));
	TASK_CHECK(max_pos_diff(out, refpos) <= pos_tol(tgt));
	return 0;
}

// Global rigid motion plus uniform scale of the source must be
// recovered on the target exactly through the anchors
int case_rigid_scale_recovery()
{
	TriMesh src = make_tetra();
	TriMesh tgt = make_strip();
	std::vector<Corr> corr;
	corr.push_back(Corr(0, 0, 1.0));
	corr.push_back(Corr(1, 1, 1.0));
	std::vector<int> anchors;
	anchors.push_back(0);
	anchors.push_back(2);

	const Mat3 m = mat_mul(scale_mat(2.2),
	                       rotation_mat(dvec3(0.3, 1.0, -0.5), 0.8));
	const dvec3 t(-3, 5, 2), c0(0.5, -1, 2);
	std::vector<point> cur(src.vertices.size());
	for (size_t i = 0; i < cur.size(); i++)
		cur[i] = to_point(mat_vec(m, dvec3(src.vertices[i])) + t);
	std::vector<point> apos(anchors.size());
	for (size_t j = 0; j < anchors.size(); j++)
		apos[j] = to_point(mat_vec(m, dvec3(tgt.vertices[anchors[j]])) + c0);

	DeformationTransfer dt(src, tgt, corr, anchors);
	TriMesh out;
	double energy = -1.0, residual = -1.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(residual <= 1e-8);
	TASK_CHECK(energy >= 0.0 && energy < 1e-6);
	const double tol = pos_tol(tgt);
	for (size_t v = 0; v < tgt.vertices.size(); v++) {
		const dvec3 expect = mat_vec(m, dvec3(tgt.vertices[v])) + c0;
		for (int k = 0; k < 3; k++)
			TASK_CHECK(std::fabs(out.vertices[v][k] - expect[k]) <= tol);
	}
	return 0;
}

// Two disconnected components, each with its own anchor
int case_independent_components()
{
	TriMesh src;
	src.vertices.push_back(point(0, 0, 0));
	src.vertices.push_back(point(1.5f, 0, 0));
	src.vertices.push_back(point(0, 1, 0));
	src.vertices.push_back(point(5, 5, 5));
	src.vertices.push_back(point(6.4f, 5.2f, 5));
	src.vertices.push_back(point(5, 6.1f, 4.7f));
	src.faces.push_back(TriMesh::Face(0, 1, 2));
	src.faces.push_back(TriMesh::Face(3, 4, 5));
	TriMesh tgt;
	tgt.vertices.push_back(point(0, 0, 0));
	tgt.vertices.push_back(point(2, 0, 0.5f));
	tgt.vertices.push_back(point(0, 1.2f, 0));
	tgt.vertices.push_back(point(10, 0, 0));
	tgt.vertices.push_back(point(11.3f, 0.2f, 0));
	tgt.vertices.push_back(point(10.1f, 1, 0.4f));
	tgt.faces.push_back(TriMesh::Face(0, 1, 2));
	tgt.faces.push_back(TriMesh::Face(3, 4, 5));
	std::vector<Corr> corr;
	corr.push_back(Corr(0, 0, 1.0));
	corr.push_back(Corr(1, 1, 1.0));
	std::vector<int> anchors;
	anchors.push_back(0);
	anchors.push_back(3);

	std::vector<point> cur = src.vertices;
	cur[0] = point(0.3f, 0.2f, 0.5f);
	cur[1] = point(1.8f, 0.1f, 0.7f);
	cur[2] = point(0.2f, 1.3f, 0.9f);
	cur[3] = point(5.2f, 5.1f, 5.3f);
	cur[4] = point(6.9f, 5.0f, 5.2f);
	cur[5] = point(5.1f, 6.5f, 5.6f);
	std::vector<point> apos;
	apos.push_back(point(-0.5f, 0.5f, 1.5f));
	apos.push_back(point(9.5f, 0.5f, 2.0f));

	DeformationTransfer dt(src, tgt, corr, anchors);
	TriMesh out;
	double energy = -1.0, residual = -1.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(residual <= 1e-8);
	TASK_CHECK(energy >= 0.0 && energy < 1e-8);

	// Each component follows its own source face about its anchor
	const double tol = pos_tol(tgt);
	for (int comp = 0; comp < 2; comp++) {
		const Mat3 fr = tri_frame(dvec3(src.vertices[3*comp+0]),
		                          dvec3(src.vertices[3*comp+1]),
		                          dvec3(src.vertices[3*comp+2]));
		const Mat3 fc = tri_frame(dvec3(cur[3*comp+0]),
		                          dvec3(cur[3*comp+1]),
		                          dvec3(cur[3*comp+2]));
		const Mat3 s = mat_mul(fc, mat_inv(fr));
		const dvec3 q(apos[comp]);
		for (int i = 0; i < 3; i++) {
			const int v = 3 * comp + i;
			const dvec3 expect = q + mat_vec(s,
				dvec3(tgt.vertices[v]) - dvec3(tgt.vertices[3*comp]));
			for (int k = 0; k < 3; k++)
				TASK_CHECK(std::fabs(out.vertices[v][k] - expect[k]) <= tol);
		}
	}

	// Moving one component's anchor leaves the other component's
	// solution at its expected positions
	std::vector<point> apos2 = apos;
	apos2[0] = point(2.5f, 1.5f, 0.5f);
	TriMesh out2;
	double e2 = -1.0, r2 = -1.0;
	TASK_CHECK(dt.solve(cur, apos2, out2, e2, r2));
	const Mat3 frb = tri_frame(dvec3(src.vertices[3]), dvec3(src.vertices[4]),
	                             dvec3(src.vertices[5]));
	const Mat3 fcb = tri_frame(dvec3(cur[3]), dvec3(cur[4]), dvec3(cur[5]));
	const Mat3 sb = mat_mul(fcb, mat_inv(frb));
	for (int i = 0; i < 3; i++) {
		const int v = 3 + i;
		const dvec3 expect = dvec3(apos[1]) + mat_vec(sb,
			dvec3(tgt.vertices[v]) - dvec3(tgt.vertices[3]));
		for (int k = 0; k < 3; k++)
			TASK_CHECK(std::fabs(out2.vertices[v][k] - expect[k]) <= tol);
	}
	return 0;
}

// ---------- state group ----------

// The constructor validates meshes, correspondences, and anchors,
// throwing std::invalid_argument on any violation
template <class F>
bool throws_invalid_arg(F f)
{
	try {
		f();
	} catch (const std::invalid_argument &) {
		return true;
	} catch (...) {
		return false;
	}
	return false;
}

int case_constructor_validation()
{
	TriMesh src = make_tetra();
	TriMesh tgt = make_strip();
	std::vector<Corr> good_corr;
	good_corr.push_back(Corr(0, 0, 1.0));
	good_corr.push_back(Corr(1, 1, 1.0));
	std::vector<int> good_anchors;
	good_anchors.push_back(0);
	good_anchors.push_back(2);
	{
		DeformationTransfer dt(src, tgt, good_corr, good_anchors);
		TASK_CHECK(dt.operator_builds() == 1);
	}
	// Correspondence indices, weights, coverage
	TASK_CHECK(throws_invalid_arg([&]() {
		DeformationTransfer dt(src, tgt,
			std::vector<Corr>(1, Corr(4, 0, 1.0)), good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		DeformationTransfer dt(src, tgt,
			std::vector<Corr>(1, Corr(-1, 0, 1.0)), good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		DeformationTransfer dt(src, tgt,
			std::vector<Corr>(1, Corr(0, 2, 1.0)), good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		DeformationTransfer dt(src, tgt,
			std::vector<Corr>(1, Corr(0, -1, 1.0)), good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		std::vector<Corr> c = good_corr;
		c.push_back(Corr(2, 0, 0.0)); // non-positive weight
		DeformationTransfer dt(src, tgt, c, good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		std::vector<Corr> c = good_corr;
		c.push_back(Corr(2, 0, -2.0));
		DeformationTransfer dt(src, tgt, c, good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		std::vector<Corr> c = good_corr;
		c.push_back(Corr(2, 0,
			std::numeric_limits<double>::quiet_NaN()));
		DeformationTransfer dt(src, tgt, c, good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		// target face 1 left uncovered
		DeformationTransfer dt(src, tgt,
			std::vector<Corr>(1, Corr(0, 0, 1.0)), good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		DeformationTransfer dt(src, tgt, std::vector<Corr>(),
			good_anchors);
		(void) dt;
	}));
	// Anchors
	TASK_CHECK(throws_invalid_arg([&]() {
		DeformationTransfer dt(src, tgt, good_corr,
			std::vector<int>(1, 4));
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		DeformationTransfer dt(src, tgt, good_corr,
			std::vector<int>(1, -1));
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		std::vector<int> a;
		a.push_back(0);
		a.push_back(0); // duplicate
		DeformationTransfer dt(src, tgt, good_corr, a);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		DeformationTransfer dt(src, tgt, good_corr,
			std::vector<int>());
		(void) dt;
	}));
	// A connected component of the target without any anchor
	{
		TriMesh two = tgt;
		const size_t base = two.vertices.size();
		two.vertices.push_back(point(10, 0, 0));
		two.vertices.push_back(point(11.3f, 0.2f, 0));
		two.vertices.push_back(point(10.1f, 1, 0.4f));
		two.faces.push_back(TriMesh::Face((int) base, (int) base + 1,
		                                (int) base + 2));
		std::vector<Corr> c = good_corr;
		c.push_back(Corr(0, 2, 1.0));
		TASK_CHECK(throws_invalid_arg([&]() {
			DeformationTransfer dt(src, two, c, good_anchors);
			(void) dt;
		}));
		std::vector<int> a = good_anchors;
		a.push_back((int) base); // now every component has one
		DeformationTransfer dt(src, two, c, a);
		TASK_CHECK(dt.operator_builds() == 1);
	}
	// Defective rest meshes
	TASK_CHECK(throws_invalid_arg([&]() {
		TriMesh bad = make_strip();
		bad.vertices.push_back(point(7, 7, 7)); // unused vertex
		DeformationTransfer dt(src, bad, good_corr, good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		TriMesh bad = tri_mesh(point(0, 0, 0), point(1, 0, 0),
		                       point(2, 0, 0)); // degenerate face
		std::vector<Corr> c(1, Corr(0, 0, 1.0));
		std::vector<int> a(1, 0);
		DeformationTransfer dt(src, bad, c, a);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		TriMesh bad = make_tetra();
		bad.vertices[1][2] = std::numeric_limits<float>::quiet_NaN();
		DeformationTransfer dt(bad, tgt, good_corr, good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		TriMesh bad = make_strip();
		bad.faces[0] = TriMesh::Face(0, 1, 1); // repeated vertex
		DeformationTransfer dt(src, bad, good_corr, good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		TriMesh bad = make_strip();
		bad.faces[0] = TriMesh::Face(0, 1, 9); // index out of range
		DeformationTransfer dt(src, bad, good_corr, good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		// three faces sharing one edge: non-manifold edge
		TriMesh bad;
		bad.vertices.push_back(point(0, 0, 0));
		bad.vertices.push_back(point(1, 0, 0));
		bad.vertices.push_back(point(0, 1, 0));
		bad.vertices.push_back(point(0, 0, 1));
		bad.vertices.push_back(point(0, -1, -1));
		bad.faces.push_back(TriMesh::Face(0, 1, 2));
		bad.faces.push_back(TriMesh::Face(1, 0, 3));
		bad.faces.push_back(TriMesh::Face(0, 1, 4));
		std::vector<Corr> c;
		c.push_back(Corr(0, 0, 1.0));
		c.push_back(Corr(1, 1, 1.0));
		c.push_back(Corr(2, 2, 1.0));
		std::vector<int> a(1, 0);
		DeformationTransfer dt(src, bad, c, a);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		// shared edge with the same direction: inconsistent
		// orientation
		TriMesh bad = make_strip();
		bad.faces[1] = TriMesh::Face(1, 2, 3);
		DeformationTransfer dt(src, bad, good_corr, good_anchors);
		(void) dt;
	}));
	TASK_CHECK(throws_invalid_arg([&]() {
		// two fans meeting only at a vertex: non-manifold vertex
		TriMesh bad;
		bad.vertices.push_back(point(0, 0, 0));
		bad.vertices.push_back(point(1, 0, 0));
		bad.vertices.push_back(point(0, 1, 0));
		bad.vertices.push_back(point(1, 1, 1));
		bad.vertices.push_back(point(-1, 0, 1));
		bad.faces.push_back(TriMesh::Face(0, 1, 2));
		bad.faces.push_back(TriMesh::Face(0, 3, 4));
		std::vector<Corr> c;
		c.push_back(Corr(0, 0, 1.0));
		c.push_back(Corr(1, 1, 1.0));
		std::vector<int> a(1, 1);
		DeformationTransfer dt(src, bad, c, a);
		(void) dt;
	}));
	return 0;
}

// Successive poses: outputs are independent, the operator is built
// once, and returning to an earlier pose reproduces it
int case_successive_poses()
{
	TriMesh src = make_tetra();
	TriMesh tgt = make_strip();
	std::vector<Corr> corr;
	corr.push_back(Corr(0, 0, 1.0));
	corr.push_back(Corr(1, 0, 2.5));
	corr.push_back(Corr(2, 1, 0.7));
	corr.push_back(Corr(3, 1, 1.3));
	std::vector<int> anchors(1, 0);
	std::vector<point> apos(1, point(0.5f, -0.5f, 1.0f));
	DeformationTransfer dt(src, tgt, corr, anchors);

	std::vector<point> cur_a = src.vertices;
	cur_a[3] = point(0.8f, 0.9f, 2.1f);
	std::vector<point> cur_b = src.vertices;
	cur_b[1] = point(2.3f, 0.2f, 0.4f);
	cur_b[3] = point(0.5f, 0.6f, 1.8f);

	TriMesh out1, out2, out3;
	double e = 0.0, r = 0.0;
	TASK_CHECK(dt.solve(cur_a, apos, out1, e, r));
	TASK_CHECK(r <= 1e-8);
	const std::vector<point> verts1 = out1.vertices;
	TASK_CHECK(dt.solve(cur_b, apos, out2, e, r));
	TASK_CHECK(r <= 1e-8);
	TASK_CHECK(dt.solve(cur_a, apos, out3, e, r));
	TASK_CHECK(r <= 1e-8);
	// The first output is independent of later solves
	TASK_CHECK(out1.vertices == verts1);
	// Returning to an earlier pose reproduces it
	TASK_CHECK(max_pos_diff(out3.vertices,
		std::vector<dvec3>(verts1.begin(), verts1.end())) <= pos_tol(tgt));
	TASK_CHECK(dt.operator_builds() == 1);
	TASK_CHECK(dt.successful_solves() == 3);
	return 0;
}

// The object is unaffected when the original inputs are modified or
// destroyed after construction
int case_source_release()
{
	TriMesh src = make_tetra();
	TriMesh tgt = make_strip();
	std::vector<Corr> corr;
	corr.push_back(Corr(0, 0, 1.0));
	corr.push_back(Corr(1, 0, 2.5));
	corr.push_back(Corr(2, 1, 0.7));
	corr.push_back(Corr(3, 1, 1.3));
	std::vector<int> anchors(1, 0);
	std::vector<point> apos(1, point(0.5f, -0.5f, 1.0f));
	std::vector<point> cur = src.vertices;
	cur[1] = point(2.3f, 0.2f, 0.4f);
	cur[3] = point(0.8f, 0.9f, 2.1f);

	// Expected result from the dense reference, before mutation
	RefProblem ref;
	ref_assemble(ref, tgt, corr, anchors);
	std::vector<dvec3> refpos;
	double refe = 0.0, refr = 0.0;
	ref_frame_solve(ref, src, cur, apos, refpos, refe, refr);
	TASK_CHECK(refr <= 1e-8);

	DeformationTransfer dt(src, tgt, corr, anchors);
	// Mangle the originals: the object must not notice
	src.vertices.clear();
	src.faces.clear();
	src.normals.assign(3, vec(1, 2, 3));
	tgt.vertices.assign(50, point(9, 9, 9));
	tgt.faces.clear();
	tgt.colors.assign(7, Color(1, 0, 1));

	TriMesh out;
	double energy = -1.0, residual = -1.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(residual <= 1e-8);
	TASK_CHECK(std::fabs(energy - refe) <= 1e-6 * std::max(1.0, std::fabs(refe)));
	// pos_tol needs the original target scale; recompute it here
	TASK_CHECK(max_pos_diff(out, refpos) <= 3e-5 * 2.0);
	return 0;
}

// Explicit failures (degenerate source, non-finite coordinates,
// bad input sizes) leave state untouched; the next frame still solves
int case_failure_recovery()
{
	TriMesh src = make_tetra();
	TriMesh tgt = make_strip();
	std::vector<Corr> corr;
	corr.push_back(Corr(0, 0, 1.0));
	corr.push_back(Corr(1, 1, 1.0));
	std::vector<int> anchors;
	anchors.push_back(0);
	anchors.push_back(2);
	std::vector<point> apos;
	apos.push_back(point(0.2f, -0.3f, 0.8f));
	apos.push_back(point(0.1f, 1.2f, 0.4f));
	std::vector<point> cur = src.vertices;
	cur[3] = point(0.8f, 0.9f, 2.1f);

	DeformationTransfer dt(src, tgt, corr, anchors);
	TriMesh out;
	double energy = -1.0, residual = -1.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(dt.successful_solves() == 1);
	const std::vector<point> good = out.vertices;

	// Degenerate current source triangle (face 0 = (0,2,1) collapsed)
	std::vector<point> bad = cur;
	bad[0] = point(0, 0, 0);
	bad[2] = point(1, 0, 0);
	bad[1] = point(2, 0, 0);
	TriMesh sentinel;
	sentinel.vertices.assign(7, point(1, 2, 3));
	double e2 = -1.0, r2 = -1.0;
	TASK_CHECK(!dt.solve(bad, apos, sentinel, e2, r2));
	TASK_CHECK(sentinel.vertices.size() == 7);
	TASK_CHECK(sentinel.vertices[0] == point(1, 2, 3));
	TASK_CHECK(dt.successful_solves() == 1);

	// Non-finite coordinates
	const float nan = std::numeric_limits<float>::quiet_NaN();
	std::vector<point> nancur = cur;
	nancur[1][0] = nan;
	TASK_CHECK(!dt.solve(nancur, apos, sentinel, e2, r2));
	std::vector<point> nanap = apos;
	nanap[0][1] = nan;
	TASK_CHECK(!dt.solve(cur, nanap, sentinel, e2, r2));
	TASK_CHECK(sentinel.vertices.size() == 7);
	TASK_CHECK(dt.successful_solves() == 1);

	// Wrongly-sized inputs are contract violations
	bool threw = false;
	try {
		std::vector<point> short_cur(3, point(0, 0, 0));
		dt.solve(short_cur, apos, sentinel, e2, r2);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	TASK_CHECK(threw);
	threw = false;
	try {
		std::vector<point> short_ap(1, point(0, 0, 0));
		dt.solve(cur, short_ap, sentinel, e2, r2);
	} catch (const std::invalid_argument &) {
		threw = true;
	}
	TASK_CHECK(threw);
	TASK_CHECK(dt.successful_solves() == 1);

	// The next frame still solves, and reproduces the good result
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(dt.successful_solves() == 2);
	TASK_CHECK(max_pos_diff(out.vertices,
		std::vector<dvec3>(good.begin(), good.end())) <= pos_tol(tgt));
	TASK_CHECK(dt.operator_builds() == 1);
	return 0;
}

// Output keeps the target's faces and attributes, invalidates
// position-dependent caches, and rebuilds them from the new positions
int case_cache_rebuild()
{
	TriMesh::set_verbose(0);
	TriMesh src = make_tetra();
	TriMesh tgt = make_strip();
	tgt.colors.assign(4, Color(0.2f, 0.3f, 0.4f));
	tgt.confidences.assign(4, 0.7f);
	tgt.flags.assign(4, 42u);
	tgt.need_neighbors();
	tgt.need_across_edge();
	tgt.need_normals();
	tgt.need_pointareas();
	tgt.need_bbox();
	std::vector<Corr> corr;
	corr.push_back(Corr(0, 0, 1.0));
	corr.push_back(Corr(1, 1, 1.0));
	std::vector<int> anchors;
	anchors.push_back(0);
	anchors.push_back(2);
	std::vector<point> apos;
	apos.push_back(point(0.2f, -0.3f, 0.8f));
	apos.push_back(point(0.1f, 1.2f, 0.4f));
	std::vector<point> cur = src.vertices;
	cur[3] = point(0.8f, 0.9f, 2.1f);

	DeformationTransfer dt(src, tgt, corr, anchors);
	TriMesh out;
	double energy = 0.0, residual = 0.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));

	// Topology and attributes preserved from the target
	TASK_CHECK(out.faces == tgt.faces);
	TASK_CHECK(out.colors.size() == tgt.colors.size());
	TASK_CHECK(out.colors.size() == 4 && out.colors[1] == tgt.colors[1]);
	TASK_CHECK(out.confidences == tgt.confidences);
	TASK_CHECK(out.flags == tgt.flags);
	// Topological adjacency may be kept
	TASK_CHECK(out.neighbors == tgt.neighbors);
	TASK_CHECK(out.across_edge == tgt.across_edge);
	// Position-dependent caches are invalidated, and nothing is
	// carried over from the source
	TASK_CHECK(out.normals.empty());
	TASK_CHECK(out.pointareas.empty() && out.cornerareas.empty());
	TASK_CHECK(out.pdir1.empty() && out.curv1.empty() && out.dcurv.empty());
	TASK_CHECK(!out.bbox.valid && !out.bsphere.valid);

	// Rebuilt caches are consistent with the new positions
	out.need_normals();
	out.need_pointareas();
	out.need_bbox();
	TriMesh ver;
	ver.vertices = out.vertices;
	ver.faces = out.faces;
	ver.need_normals();
	ver.need_pointareas();
	ver.need_bbox();
	TASK_CHECK(out.normals.size() == out.vertices.size());
	TASK_CHECK(out.pointareas.size() == out.vertices.size());
	for (size_t i = 0; i < out.vertices.size(); i++) {
		TASK_CHECK(out.normals[i] == ver.normals[i]);
		TASK_CHECK(out.pointareas[i] == ver.pointareas[i]);
	}
	TASK_CHECK(out.bbox.valid && out.bbox.min == ver.bbox.min &&
	           out.bbox.max == ver.bbox.max);
	for (size_t v = 0; v < out.vertices.size(); v++)
		for (int k = 0; k < 3; k++)
			TASK_CHECK(out.bbox.min[k] <= out.vertices[v][k] &&
			           out.vertices[v][k] <= out.bbox.max[k]);
	double sum = 0.0, area = 0.0;
	for (size_t i = 0; i < out.pointareas.size(); i++)
		sum += out.pointareas[i];
	for (size_t f = 0; f < out.faces.size(); f++) {
		const TriMesh::Face &fc = out.faces[f];
		area += 0.5 * len((out.vertices[fc[1]] - out.vertices[fc[0]]) CROSS
		                  (out.vertices[fc[2]] - out.vertices[fc[0]]));
	}
	TASK_CHECK(std::fabs(sum - area) <= 1e-5 * std::max(1.0, area));
	return 0;
}

// A larger surface: sparse storage, no operator rebuild across
// repeated solves, exact anchors
int case_sparse_no_rebuild()
{
	const int n = 28;
	TriMesh src = make_grid(n, 0.5f);
	TriMesh tgt = make_grid(n, 0.5f);
	std::vector<Corr> corr;
	for (size_t f = 0; f < tgt.faces.size(); f++)
		corr.push_back(Corr((int) f, (int) f, 1.0));
	std::vector<int> anchors;
	anchors.push_back(0);
	anchors.push_back(n - 1);
	anchors.push_back((n - 1) * n);
	anchors.push_back(n * n - 1);
	DeformationTransfer dt(src, tgt, corr, anchors);
	TASK_CHECK(dt.operator_builds() == 1);
	TASK_CHECK(dt.operator_nonzeros() > 0);
	// Sparse: far below the n^2 of a dense operator
	TASK_CHECK(dt.operator_nonzeros() < 25 * tgt.vertices.size());

	// Pose 1: a similarity, recovered through the corner anchors
	const Mat3 m = mat_mul(scale_mat(1.8),
	                       rotation_mat(dvec3(0.2, 0.5, 0.8), 0.6));
	const dvec3 t(2, -3, 1), c0(1, 2, 3);
	std::vector<point> cur(src.vertices.size());
	for (size_t i = 0; i < cur.size(); i++)
		cur[i] = to_point(mat_vec(m, dvec3(src.vertices[i])) + t);
	std::vector<point> apos(anchors.size());
	for (size_t j = 0; j < anchors.size(); j++)
		apos[j] = to_point(mat_vec(m, dvec3(tgt.vertices[anchors[j]])) + c0);
	TriMesh out;
	double energy = -1.0, residual = -1.0;
	TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
	TASK_CHECK(residual <= 1e-8);
	TASK_CHECK(energy >= 0.0 && energy < 1e-4);
	const double tol = pos_tol(tgt);
	for (size_t v = 0; v < tgt.vertices.size(); v++) {
		const dvec3 expect = mat_vec(m, dvec3(tgt.vertices[v])) + c0;
		for (int k = 0; k < 3; k++)
			TASK_CHECK(std::fabs(out.vertices[v][k] - expect[k]) <= tol);
	}
	// Anchors are satisfied exactly
	for (size_t j = 0; j < anchors.size(); j++)
		TASK_CHECK(out.vertices[anchors[j]] == apos[j]);

	// Poses 2 and 3: smooth non-rigid fields
	for (int pose = 0; pose < 2; pose++) {
		const double amp = pose == 0 ? 1.0 : 1.7;
		for (size_t i = 0; i < cur.size(); i++) {
			const point &p = src.vertices[i];
			cur[i] = to_point(dvec3(p) + amp * dvec3(
				0.4 * std::sin(0.6 * p[0]) * std::cos(0.5 * p[1]),
				0.3 * std::cos(0.45 * p[0] + 1.0) * std::sin(0.5 * p[1]),
				0.6 * std::sin(0.4 * p[0] + 0.5) * std::sin(0.55 * p[1])));
		}
		for (size_t j = 0; j < anchors.size(); j++) {
			const point &p = tgt.vertices[anchors[j]];
			apos[j] = to_point(dvec3(p) + amp * dvec3(
				0.4 * std::sin(0.6 * p[0]) * std::cos(0.5 * p[1]),
				0.3 * std::cos(0.45 * p[0] + 1.0) * std::sin(0.5 * p[1]),
				0.6 * std::sin(0.4 * p[0] + 0.5) * std::sin(0.55 * p[1])));
		}
		TASK_CHECK(dt.solve(cur, apos, out, energy, residual));
		TASK_CHECK(residual <= 1e-8);
		TASK_CHECK(energy >= -1e-12);
	}
	// Repeated solves never rebuild the operator
	TASK_CHECK(dt.operator_builds() == 1);
	TASK_CHECK(dt.successful_solves() == 3);
	return 0;
}

} // namespace

const task_case task_cases[] = {
	{ "gradients", "frame_similarity", case_frame_similarity },
	{ "gradients", "frame_shear_translation", case_frame_shear_translation },
	{ "gradients", "bent_pair_energy", case_bent_pair_energy },
	{ "integration", "tetra_to_strip_dense", case_tetra_to_strip_dense },
	{ "integration", "rigid_scale_recovery", case_rigid_scale_recovery },
	{ "integration", "independent_components", case_independent_components },
	{ "state", "constructor_validation", case_constructor_validation },
	{ "state", "successive_poses", case_successive_poses },
	{ "state", "source_release", case_source_release },
	{ "state", "failure_recovery", case_failure_recovery },
	{ "state", "cache_rebuild", case_cache_rebuild },
	{ "state", "sparse_no_rebuild", case_sparse_no_rebuild },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
