/*
deformation_transfer.cc

Task tests for the face-correspondence DeformationTransfer.  All reference
math (triangle frames, barycentric gradients, the constrained dense normal
equations, the energy) is built independently in this file instead of
reusing the library internals.
*/

#include "task_support/check.h"
#include "DeformationTransfer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace trimesh;
using namespace std;

namespace {

struct TestInit {
	TestInit() { TriMesh::set_verbose(0); }
} test_init;

dvec3 to_d(const point &p)
{
	return dvec3(p[0], p[1], p[2]);
}

// Dense 3x3, row-major storage m[row][col].
struct M3 {
	double m[3][3];
	M3()
	{
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				m[i][j] = 0.0;
	}
};

M3 operator * (const M3 &a, const M3 &b)
{
	M3 c;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double s = 0.0;
			for (int k = 0; k < 3; k++)
				s += a.m[i][k] * b.m[k][j];
			c.m[i][j] = s;
		}
	return c;
}

dvec3 operator * (const M3 &a, const dvec3 &v)
{
	return dvec3(a.m[0][0]*v[0] + a.m[0][1]*v[1] + a.m[0][2]*v[2],
	              a.m[1][0]*v[0] + a.m[1][1]*v[1] + a.m[1][2]*v[2],
	              a.m[2][0]*v[0] + a.m[2][1]*v[1] + a.m[2][2]*v[2]);
}

M3 inverse3(const M3 &a)
{
	M3 inv;
	const double (*m)[3] = a.m;
	double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
	             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
	             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
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
	return inv;
}

double frob2(const M3 &a)
{
	double s = 0.0;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			s += a.m[i][j] * a.m[i][j];
	return s;
}

// Independently constructed triangle frame F = [e1, e2, a/sqrt(|a|)].
M3 frame(const dvec3 &p0, const dvec3 &p1, const dvec3 &p2)
{
	dvec3 e1 = p1 - p0;
	dvec3 e2 = p2 - p0;
	dvec3 a = e1 CROSS e2;
	dvec3 third = a / sqrt(len(a));
	M3 F;
	for (int i = 0; i < 3; i++) {
		F.m[i][0] = e1[i];
		F.m[i][1] = e2[i];
		F.m[i][2] = third[i];
	}
	return F;
}

struct FaceData {
	dvec3 g[3];
	dvec3 n;
	M3 P;
	double area;
};

// Independent barycentric gradients, projection, area of a rest face.
FaceData face_data(const dvec3 &p0, const dvec3 &p1, const dvec3 &p2)
{
	dvec3 e1 = p1 - p0;
	dvec3 e2 = p2 - p0;
	dvec3 a = e1 CROSS e2;
	double la = len(a);
	FaceData d;
	d.area = 0.5 * la;
	d.n = a / la;
	dvec3 g1 = (len2(e2) * e1 - (e1 DOT e2) * e2) / (la * la);
	dvec3 g2 = (len2(e1) * e2 - (e1 DOT e2) * e1) / (la * la);
	d.g[0] = -(g1 + g2);
	d.g[1] = g1;
	d.g[2] = g2;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			d.P.m[i][j] = (i == j ? 1.0 : 0.0) - d.n[i] * d.n[j];
	return d;
}

// Dense LU solve with partial pivoting.
vector<double> dense_solve(vector<double> A, const vector<double> &b)
{
	int n = (int)b.size();
	vector<int> piv(n);
	for (int i = 0; i < n; i++)
		piv[i] = i;
	for (int k = 0; k < n; k++) {
		int p = k;
		double best = fabs(A[k*n + k]);
		for (int i = k + 1; i < n; i++)
			if (fabs(A[i*n + k]) > best) {
				best = fabs(A[i*n + k]);
				p = i;
			}
		if (best <= 1e-300)
			throw runtime_error("dense singular matrix");
		if (p != k) {
			for (int j = 0; j < n; j++)
				swap(A[k*n + j], A[p*n + j]);
			swap(piv[k], piv[p]);
		}
		for (int i = k + 1; i < n; i++) {
			double f = A[i*n + k] / A[k*n + k];
			A[i*n + k] = f;
			for (int j = k + 1; j < n; j++)
				A[i*n + j] -= f * A[k*n + j];
		}
	}
	vector<double> x(n);
	for (int i = 0; i < n; i++) {
		double s = b[piv[i]];
		for (int j = 0; j < i; j++)
			s -= A[i*n + j] * x[j];
		x[i] = s;
	}
	for (int i = n - 1; i >= 0; i--) {
		double s = x[i];
		for (int j = i + 1; j < n; j++)
			s -= A[i*n + j] * x[j];
		x[i] = s / A[i*n + i];
	}
	return x;
}

// ------------------------------------------------------------------
// Small non-coplanar, differently-tessellated test fixtures.
// ------------------------------------------------------------------

// Source: bent 2-triangle patch, 4 vertices.
TriMesh *make_source_patch()
{
	TriMesh *m = new TriMesh;
	m->vertices.push_back(point(0, 0, 0));
	m->vertices.push_back(point(1, 0, 0));
	m->vertices.push_back(point(0, 1, 0));
	m->vertices.push_back(point(1, 1, 0.7f));
	m->faces.push_back(TriMesh::Face(0, 1, 2));
	m->faces.push_back(TriMesh::Face(1, 3, 2));
	return m;
}

// Target: pyramid fan without base, 5 vertices, 4 faces.
TriMesh *make_target_fan()
{
	TriMesh *m = new TriMesh;
	m->vertices.push_back(point(0, 0, 0));
	m->vertices.push_back(point(2, 0, 0));
	m->vertices.push_back(point(2, 2, 0));
	m->vertices.push_back(point(0, 2, 0));
	m->vertices.push_back(point(1, 1, 1));
	m->faces.push_back(TriMesh::Face(0, 1, 4));
	m->faces.push_back(TriMesh::Face(1, 2, 4));
	m->faces.push_back(TriMesh::Face(2, 3, 4));
	m->faces.push_back(TriMesh::Face(3, 0, 4));
	return m;
}

vector<FaceCorrespondence> make_correspondences()
{
	// tf0 is a many-to-one correspondence with unequal weights.
	vector<FaceCorrespondence> c;
	c.push_back(FaceCorrespondence(0, 0, 1.0));
	c.push_back(FaceCorrespondence(1, 0, 0.5));
	c.push_back(FaceCorrespondence(1, 1, 1.0));
	c.push_back(FaceCorrespondence(0, 2, 0.7));
	c.push_back(FaceCorrespondence(1, 3, 1.3));
	return c;
}

const int ANCHORS[] = { 0, 1, 2 };

vector<int> make_anchors()
{
	return vector<int>(ANCHORS, ANCHORS + 3);
}

// Current source deformation gradients, built from scratch.
vector<M3> source_gradients(const TriMesh &rest,
                            const vector<point> &current)
{
	vector<M3> S(rest.faces.size());
	for (size_t f = 0; f < rest.faces.size(); f++) {
		int i0 = rest.faces[f][0], i1 = rest.faces[f][1],
		    i2 = rest.faces[f][2];
		M3 Fr = frame(to_d(rest.vertices[i0]),
		              to_d(rest.vertices[i1]),
		              to_d(rest.vertices[i2]));
		M3 Fc = frame(to_d(current[i0]),
		              to_d(current[i1]),
		              to_d(current[i2]));
		S[f] = Fc * inverse3(Fr);
	}
	return S;
}

// Reference energy evaluated at arbitrary target positions.
double reference_energy(const TriMesh &target_rest,
                        const vector<FaceCorrespondence> &corr,
                        const vector<M3> &S,
                        const vector<dvec3> &x)
{
	double E = 0.0;
	for (size_t c = 0; c < corr.size(); c++) {
		int tf = corr[c].target_face;
		int sf = corr[c].source_face;
		const TriMesh::Face &face = target_rest.faces[tf];
		FaceData fd = face_data(to_d(target_rest.vertices[face[0]]),
		                        to_d(target_rest.vertices[face[1]]),
		                        to_d(target_rest.vertices[face[2]]));
		M3 G;
		for (int k = 0; k < 3; k++)
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					G.m[i][j] += x[face[k]][i] * fd.g[k][j];
		M3 target = S[sf] * fd.P;
		M3 diff;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				diff.m[i][j] = G.m[i][j] - target.m[i][j];
		E += fd.area * corr[c].weight * frob2(diff);
	}
	return E;
}

// Dense constrained solution of the stated energy.  Returns positions and
// fills in the free-row residual and energy.
vector<dvec3> dense_solution(const TriMesh &target_rest,
                             const vector<FaceCorrespondence> &corr,
                             const vector<int> &anchors,
                             const vector<M3> &S,
                             const vector<dvec3> &anchor_pos,
                             double &residual_out, double &energy_out)
{
	int nv = (int)target_rest.vertices.size();
	int nd = 3 * nv;
	vector<double> H(nd * nd, 0.0), rhs(nd, 0.0);
	vector<FaceData> fds(target_rest.faces.size());

	for (int f = 0; f < (int)target_rest.faces.size(); f++) {
		const TriMesh::Face &face = target_rest.faces[f];
		fds[f] = face_data(to_d(target_rest.vertices[face[0]]),
		                   to_d(target_rest.vertices[face[1]]),
		                   to_d(target_rest.vertices[face[2]]));
		M3 Q;
		for (size_t c = 0; c < corr.size(); c++) {
			if (corr[c].target_face != f)
				continue;
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					Q.m[i][j] += corr[c].weight *
					             S[corr[c].source_face].m[i][j];
		}
		M3 AQP = Q * fds[f].P;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				AQP.m[i][j] *= fds[f].area;
		for (int k = 0; k < 3; k++)
			rhs[3*face[k] + 0] += (AQP * fds[f].g[k])[0],
			rhs[3*face[k] + 1] += (AQP * fds[f].g[k])[1],
			rhs[3*face[k] + 2] += (AQP * fds[f].g[k])[2];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) {
				double coeff = fds[f].area;
				double w = 0.0;
				for (size_t c = 0; c < corr.size(); c++)
					if (corr[c].target_face == f)
						w += corr[c].weight;
				coeff *= w * (fds[f].g[i] DOT fds[f].g[j]);
				for (int r = 0; r < 3; r++)
					H[nd*(3*face[i] + r) + (3*face[j] + r)] += coeff;
			}
	}

	vector<char> is_anchor(nv, 0);
	vector<int> slot(nv, -1);
	for (size_t k = 0; k < anchors.size(); k++) {
		is_anchor[anchors[k]] = 1;
		slot[anchors[k]] = (int)k;
	}
	vector<int> free_v;
	for (int v = 0; v < nv; v++)
		if (!is_anchor[v])
			free_v.push_back(v);
	int m = (int)free_v.size();
	int m3 = 3 * m;
	vector<double> Hff(m3 * m3, 0.0), bff(m3, 0.0);
	for (int a = 0; a < m3; a++) {
		int ga = 3 * free_v[a / 3] + (a % 3);
		bff[a] = rhs[ga];
		for (int b = 0; b < m3; b++) {
			int gb = 3 * free_v[b / 3] + (b % 3);
			Hff[a*m3 + b] = H[ga*nd + gb];
		}
		for (int q = 0; q < (int)anchors.size(); q++)
			for (int r = 0; r < 3; r++)
				bff[a] -= H[ga*nd + (3*anchors[q] + r)] *
				          anchor_pos[q][r];
	}
	vector<double> xf = dense_solve(Hff, bff);

	vector<dvec3> x(nv);
	for (size_t q = 0; q < anchors.size(); q++)
		x[anchors[q]] = anchor_pos[q];
	for (int a = 0; a < m3; a++)
		x[free_v[a / 3]][a % 3] = xf[a];

	double rinf = 0.0, binf = 0.0;
	for (int a = 0; a < m3; a++) {
		double hx = 0.0;
		for (int b = 0; b < m3; b++)
			hx += Hff[a*m3 + b] * xf[b];
		rinf = max(rinf, fabs(hx - bff[a]));
		binf = max(binf, fabs(bff[a]));
	}
	residual_out = rinf / max(1.0, binf);
	energy_out = reference_energy(target_rest, corr, S, x);
	return x;
}

double target_scale(const TriMesh &m)
{
	double s = 1.0;
	for (size_t i = 0; i < m.vertices.size(); i++)
		s = max(s, (double)len(to_d(m.vertices[i])));
	return s;
}

// Curved NxN grid patch with consistent orientation.
TriMesh *make_grid(int N, const dvec3 &offset)
{
	TriMesh *m = new TriMesh;
	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++) {
			double x = (double)i / (N - 1);
			double y = (double)j / (N - 1);
			double z = 0.4 * sin(2.5 * x) * cos(2.0 * y);
			m->vertices.push_back(point(
				(float)(x + offset[0]),
				(float)(y + offset[1]),
				(float)(z + offset[2])));
		}
	for (int j = 0; j + 1 < N; j++)
		for (int i = 0; i + 1 < N; i++) {
			int a = j * N + i, b = j * N + i + 1;
			int c = (j + 1) * N + i, d = (j + 1) * N + i + 1;
			m->faces.push_back(TriMesh::Face(a, b, c));
			m->faces.push_back(TriMesh::Face(b, d, c));
		}
	return m;
}

M3 rot_matrix(double ang, const dvec3 &axis)
{
	dvec3 u = normalized(axis);
	double c = cos(ang), s = sin(ang);
	M3 R;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			R.m[i][j] = (i == j ? c : 0.0) +
			            (1.0 - c) * u[i] * u[j];
	R.m[0][1] -= s * u[2]; R.m[0][2] += s * u[1];
	R.m[1][0] += s * u[2]; R.m[1][2] -= s * u[0];
	R.m[2][0] -= s * u[1]; R.m[2][1] += s * u[0];
	return R;
}

// ------------------------------------------------------------------
// gradients group
// ------------------------------------------------------------------

int gradients_frame()
{
	dvec3 p0(0.1, -0.2, 0.3), p1(1.3, 0.2, -0.1), p2(-0.4, 1.1, 0.9);
	dvec3 e1 = p1 - p0, e2 = p2 - p0;
	dvec3 a = e1 CROSS e2;
	M3 F = frame(p0, p1, p2);
	for (int i = 0; i < 3; i++) {
		TASK_CHECK(fabs(F.m[i][0] - e1[i]) < 1e-14);
		TASK_CHECK(fabs(F.m[i][1] - e2[i]) < 1e-14);
		TASK_CHECK(fabs(F.m[i][2] - a[i] / sqrt(len(a))) < 1e-14);
	}
	// Third column length is sqrt(|a|) = sqrt(2*area), not unit length.
	dvec3 third(F.m[0][2], F.m[1][2], F.m[2][2]);
	TASK_CHECK(fabs(len(third) - sqrt(len(a))) < 1e-13);
	TASK_CHECK(len(third) > 1.0 + 1e-9 || len(a) > 1.0);

	// S = F_cur F_rest^-1 maps rest edges and the scaled normal exactly.
	M3 R = rot_matrix(0.9, dvec3(0.3, -0.7, 1.0));
	dvec3 t(2.0, -1.0, 0.5);
	dvec3 q0 = R * p0 + t, q1 = R * p1 + t, q2 = R * p2 + t;
	M3 Fc = frame(q0, q1, q2);
	M3 S = Fc * inverse3(F);
	dvec3 out1 = S * e1, out2 = S * e2;
	TASK_CHECK(len(out1 - (q1 - q0)) < 1e-12);
	TASK_CHECK(len(out2 - (q2 - q0)) < 1e-12);
	dvec3 outn = S * third;
	dvec3 ac = (q1 - q0) CROSS (q2 - q0);
	TASK_CHECK(len(outn - ac / sqrt(len(a))) < 1e-12);
	// A rigid motion leaves S as the rotation, translation drops out.
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			TASK_CHECK(fabs(S.m[i][j] - R.m[i][j]) < 1e-12);
	return 0;
}

int gradients_barycentric()
{
	dvec3 p0(0, 0, 0), p1(2, 0, 0), p2(0.5, 1.5, 1.0);
	FaceData d = face_data(p0, p1, p2);
	dvec3 e1 = p1 - p0, e2 = p2 - p0;
	TASK_CHECK(fabs((d.g[1] DOT e1) - 1.0) < 1e-13);
	TASK_CHECK(fabs(d.g[1] DOT e2) < 1e-13);
	TASK_CHECK(fabs(d.g[2] DOT e1) < 1e-13);
	TASK_CHECK(fabs((d.g[2] DOT e2) - 1.0) < 1e-13);
	dvec3 gsum = d.g[0] + d.g[1] + d.g[2];
	TASK_CHECK(len(gsum) < 1e-13);
	for (int k = 0; k < 3; k++)
		TASK_CHECK(fabs(d.g[k] DOT d.n) < 1e-13);
	// P is the tangent-plane projector: symmetric, idempotent, rank 2.
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			TASK_CHECK(fabs(d.P.m[i][j] - d.P.m[j][i]) < 1e-14);
	M3 P2 = d.P * d.P;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			TASK_CHECK(fabs(P2.m[i][j] - d.P.m[i][j]) < 1e-13);
	TASK_CHECK(fabs(d.P * d.n)[0] < 1e-14);
	TASK_CHECK(fabs((d.P * d.n)[1]) < 1e-14);
	TASK_CHECK(fabs((d.P * d.n)[2]) < 1e-14);
	TASK_CHECK(d.area > 0.0);
	return 0;
}

int gradients_dense_match()
{
	TriMesh *src = make_source_patch();
	TriMesh *tgt = make_target_fan();
	vector<FaceCorrespondence> corr = make_correspondences();
	vector<int> anchors = make_anchors();
	DeformationTransfer dt(*src, *tgt, corr, anchors);

	// Non-affine, non-integrable source pose: only one source vertex
	// moves, so the two source faces demand incompatible gradients.
	vector<point> pose = src->vertices;
	pose[3] = point(1.3f, 0.8f, 1.2f);
	vector<dvec3> anchor_pos(anchors.size());
	for (size_t k = 0; k < anchors.size(); k++)
		anchor_pos[k] = to_d(tgt->vertices[anchors[k]]) +
		                dvec3(0.1, -0.05, 0.2);

	vector<M3> S = source_gradients(*src, pose);
	double ref_res = 0.0, ref_E = 0.0;
	vector<dvec3> ref = dense_solution(*tgt, corr, anchors, S,
	                                   anchor_pos, ref_res, ref_E);
	DeformationTransferResult r = dt.solve(pose, anchor_pos);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.residual <= 1e-8);
	TASK_CHECK(r.residual >= 0.0);

	double scale = target_scale(*tgt);
	double tol = 3e-5 * max(1.0, scale);
	double maxpos = 0.0;
	for (int v = 0; v < (int)tgt->vertices.size(); v++)
		maxpos = max(maxpos, dist(to_d(r.mesh->vertices[v]), ref[v]));
	TASK_CHECK(maxpos <= tol);
	TASK_CHECK(fabs(r.energy - ref_E) <= 1e-10 * max(1.0, ref_E));
	TASK_CHECK(fabs(r.residual - ref_res) <= 1e-10);

	// The pose cannot be integrated exactly: positive residual energy.
	TASK_CHECK(ref_E > 1e-8);

	// Anchors are exact (in double, before the float cast).
	for (size_t k = 0; k < anchors.size(); k++) {
		dvec3 got = to_d(r.mesh->vertices[anchors[k]]);
		TASK_CHECK(len(got - anchor_pos[k]) <= 1e-6 * max(1.0, scale));
	}
	delete r.mesh;
	delete src;
	delete tgt;
	return 0;
}

// ------------------------------------------------------------------
// integration group
// ------------------------------------------------------------------

int integration_rigid_scale()
{
	TriMesh *src = make_source_patch();
	TriMesh *tgt = make_target_fan();
	vector<FaceCorrespondence> corr = make_correspondences();
	vector<int> anchors = make_anchors();
	DeformationTransfer dt(*src, *tgt, corr, anchors);
	double scale = target_scale(*tgt);
	double tol = 3e-5 * max(1.0, scale);

	// Rigid motion: rotation plus translation, propagated by anchors.
	M3 R = rot_matrix(0.7, normalized(dvec3(1.0, 2.0, -1.0)));
	dvec3 t(1.5, -2.0, 0.7);
	vector<point> pose(src->vertices.size());
	for (int v = 0; v < (int)src->vertices.size(); v++)
		pose[v] = point(R * to_d(src->vertices[v]) + t);
	vector<dvec3> anchor_pos(anchors.size());
	for (size_t k = 0; k < anchors.size(); k++)
		anchor_pos[k] = R * to_d(tgt->vertices[anchors[k]]) + t;
	DeformationTransferResult r = dt.solve(pose, anchor_pos);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.energy < 1e-10);
	for (int v = 0; v < (int)tgt->vertices.size(); v++) {
		dvec3 expect = R * to_d(tgt->vertices[v]) + t;
		TASK_CHECK(dist(to_d(r.mesh->vertices[v]), expect) <= tol);
	}
	delete r.mesh;

	// Positive uniform scale, recovered through the anchors.
	double s = 1.6;
	for (int v = 0; v < (int)src->vertices.size(); v++)
		pose[v] = point(s * to_d(src->vertices[v]) + t);
	vector<point> anchor_f(anchors.size());
	for (size_t k = 0; k < anchors.size(); k++) {
		anchor_pos[k] = s * to_d(tgt->vertices[anchors[k]]) + t;
		anchor_f[k] = point(anchor_pos[k]);
	}
	// Exercise the float-anchor overload as well.
	r = dt.solve(pose, anchor_f);
	TASK_CHECK(r.ok());
	TASK_CHECK(r.energy < 1e-10);
	for (int v = 0; v < (int)tgt->vertices.size(); v++) {
		dvec3 expect = s * to_d(tgt->vertices[v]) + t;
		TASK_CHECK(dist(to_d(r.mesh->vertices[v]), expect) <= tol);
	}
	delete r.mesh;

	// Pure source translation changes no gradient: with anchors kept at
	// the rest pose, the target must stay at rest.
	vector<point> shifted = src->vertices;
	for (size_t v = 0; v < shifted.size(); v++)
		shifted[v] = point(to_d(shifted[v]) + dvec3(4.0, -3.0, 2.0));
	vector<dvec3> rest_anchors(anchors.size());
	for (size_t k = 0; k < anchors.size(); k++)
		rest_anchors[k] = to_d(tgt->vertices[anchors[k]]);
	r = dt.solve(shifted, rest_anchors);
	TASK_CHECK(r.ok());
	// The only residual is float roundoff from translating float
	// coordinates; the gradients themselves are translation-invariant.
	TASK_CHECK(r.energy < 1e-12);
	for (int v = 0; v < (int)tgt->vertices.size(); v++)
		TASK_CHECK(dist(to_d(r.mesh->vertices[v]),
		                to_d(tgt->vertices[v])) <= tol);
	delete r.mesh;
	delete src;
	delete tgt;
	return 0;
}

int integration_components()
{
	// Two disconnected fans in each mesh, anchors in each component.
	TriMesh src, tgt;
	TriMesh *s0 = make_source_patch();
	TriMesh *t0 = make_target_fan();
	TriMesh *s1 = make_source_patch();
	TriMesh *t1 = make_target_fan();
	dvec3 shift_s(6.0, 0.0, 0.0), shift_t(7.0, 0.0, 0.0);
	int snv = (int)s0->vertices.size(), tnv = (int)t0->vertices.size();
	int snf = (int)s0->faces.size(), tnf = (int)t0->faces.size();
	src.vertices = s0->vertices;
	tgt.vertices = t0->vertices;
	src.faces = s0->faces;
	tgt.faces = t0->faces;
	for (int v = 0; v < snv; v++)
		src.vertices.push_back(point(to_d(s1->vertices[v]) + shift_s));
	for (int f = 0; f < snf; f++) {
		TriMesh::Face face = s1->faces[f];
		src.faces.push_back(TriMesh::Face(face[0] + snv,
		                                  face[1] + snv, face[2] + snv));
	}
	for (int v = 0; v < tnv; v++)
		tgt.vertices.push_back(point(to_d(t1->vertices[v]) + shift_t));
	for (int f = 0; f < tnf; f++) {
		TriMesh::Face face = t1->faces[f];
		tgt.faces.push_back(TriMesh::Face(face[0] + tnv,
		                                  face[1] + tnv, face[2] + tnv));
	}
	vector<FaceCorrespondence> corr = make_correspondences();
	size_t base = corr.size();
	for (size_t c = 0; c < base; c++)
		corr.push_back(FaceCorrespondence(
			corr[c].source_face + snf,
			corr[c].target_face + tnf, corr[c].weight));
	vector<int> anchors = make_anchors();
	for (size_t k = 0; k < 3; k++)
		anchors.push_back(ANCHORS[k] + tnv);

	DeformationTransfer dt(src, tgt, corr, anchors);

	// Deform component 0 of the source only; component 1 stays at rest.
	vector<point> pose = src.vertices;
	pose[3] = point(1.3f, 0.8f, 1.2f);
	vector<dvec3> anchor_pos(anchors.size());
	for (size_t k = 0; k < anchors.size(); k++)
		anchor_pos[k] = to_d(tgt.vertices[anchors[k]]);
	DeformationTransferResult r = dt.solve(pose, anchor_pos);
	TASK_CHECK(r.ok());

	// The untouched component must come back at its exact rest positions.
	for (int v = tnv; v < 2 * tnv; v++)
		TASK_CHECK(dist(to_d(r.mesh->vertices[v]),
		                to_d(tgt.vertices[v])) < 1e-12);
	// And it matches the dense reference on the deformed component.
	vector<M3> S = source_gradients(src, pose);
	double rr = 0.0, EE = 0.0;
	vector<dvec3> ref = dense_solution(tgt, corr, anchors, S,
	                                   anchor_pos, rr, EE);
	double tol = 3e-5 * max(1.0, target_scale(tgt));
	for (int v = 0; v < tnv; v++)
		TASK_CHECK(dist(to_d(r.mesh->vertices[v]), ref[v]) <= tol);
	TASK_CHECK(fabs(r.energy - EE) <= 1e-10 * max(1.0, EE));
	delete r.mesh;
	delete s0; delete s1; delete t0; delete t1;
	return 0;
}

int integration_sequence()
{
	TriMesh *src = make_source_patch();
	TriMesh *tgt = make_target_fan();
	vector<FaceCorrespondence> corr = make_correspondences();
	vector<int> anchors = make_anchors();
	DeformationTransfer dt(*src, *tgt, corr, anchors);
	double tol = 3e-5 * max(1.0, target_scale(*tgt));
	double prev_E = -1.0;

	for (int frame = 0; frame < 5; frame++) {
		double u = 0.25 * frame;
		vector<point> pose = src->vertices;
		pose[3] = point(1.0f + 0.3f * (float)u,
		                1.0f - 0.2f * (float)u,
		                0.7f + 0.5f * (float)u);
		vector<dvec3> anchor_pos(anchors.size());
		for (size_t k = 0; k < anchors.size(); k++)
			anchor_pos[k] = to_d(tgt->vertices[anchors[k]]) +
			                0.1 * u * dvec3(1, -1, 2);
		vector<M3> S = source_gradients(*src, pose);
		double rr = 0.0, EE = 0.0;
		vector<dvec3> ref = dense_solution(*tgt, corr, anchors, S,
		                                   anchor_pos, rr, EE);
		DeformationTransferResult r = dt.solve(pose, anchor_pos);
		TASK_CHECK(r.ok());
		TASK_CHECK(r.residual <= 1e-8);
		for (int v = 0; v < (int)tgt->vertices.size(); v++)
			TASK_CHECK(dist(to_d(r.mesh->vertices[v]), ref[v]) <= tol);
		TASK_CHECK(fabs(r.energy - EE) <= 1e-10 * max(1.0, EE));
		TASK_CHECK(r.energy >= 0.0);
		if (frame > 0)
			TASK_CHECK(r.energy != prev_E);
		prev_E = r.energy;
		delete r.mesh;
	}
	TASK_CHECK(dt.successful_solve_count() == 5);
	TASK_CHECK(dt.operator_build_count() == 1);
	delete src;
	delete tgt;
	return 0;
}

// ------------------------------------------------------------------
// state group
// ------------------------------------------------------------------

int state_ownership()
{
	TriMesh *src = make_source_patch();
	TriMesh *tgt = make_target_fan();
	vector<FaceCorrespondence> corr = make_correspondences();
	vector<int> anchors = make_anchors();
	DeformationTransfer dt(*src, *tgt, corr, anchors);

	vector<point> pose = src->vertices;
	pose[3] = point(1.3f, 0.8f, 1.2f);
	vector<dvec3> anchor_pos(anchors.size());
	for (size_t k = 0; k < anchors.size(); k++)
		anchor_pos[k] = to_d(tgt->vertices[anchors[k]]);
	DeformationTransferResult r1 = dt.solve(pose, anchor_pos);
	TASK_CHECK(r1.ok());

	// Mutate and then release the original input meshes.
	src->vertices[0] = point(99, 99, 99);
	tgt->vertices[0] = point(99, 99, 99);
	src->clear();
	tgt->clear();
	delete src;
	delete tgt;

	DeformationTransferResult r2 = dt.solve(pose, anchor_pos);
	TASK_CHECK(r2.ok());
	for (size_t v = 0; v < r1.mesh->vertices.size(); v++)
		TASK_CHECK(dist(to_d(r1.mesh->vertices[v]),
		                to_d(r2.mesh->vertices[v])) < 1e-12);
	TASK_CHECK(fabs(r1.energy - r2.energy) < 1e-18);
	delete r1.mesh;
	delete r2.mesh;
	return 0;
}

int state_failure_recovery()
{
	TriMesh *src = make_source_patch();
	TriMesh *tgt = make_target_fan();
	vector<FaceCorrespondence> corr = make_correspondences();
	vector<int> anchors = make_anchors();
	DeformationTransfer dt(*src, *tgt, corr, anchors);

	vector<point> good = src->vertices;
	good[3] = point(1.3f, 0.8f, 1.2f);
	vector<dvec3> anchor_pos(anchors.size());
	for (size_t k = 0; k < anchors.size(); k++)
		anchor_pos[k] = to_d(tgt->vertices[anchors[k]]);
	DeformationTransferResult ok = dt.solve(good, anchor_pos);
	TASK_CHECK(ok.ok());
	size_t solves_before = dt.successful_solve_count();

	// Degenerate current source triangle: face 1 collapses.
	vector<point> bad = good;
	bad[3] = bad[1];
	DeformationTransferResult fail1 = dt.solve(bad, anchor_pos);
	TASK_CHECK(!fail1.ok());
	TASK_CHECK(fail1.mesh == 0);
	TASK_CHECK(!std::isfinite(fail1.energy));
	TASK_CHECK(!std::isfinite(fail1.residual));

	// Non-finite coordinates.
	vector<point> nan_pose = good;
	float qnan = numeric_limits<float>::quiet_NaN();
	nan_pose[2] = point(qnan, 0, 0);
	DeformationTransferResult fail2 = dt.solve(nan_pose, anchor_pos);
	TASK_CHECK(!fail2.ok());
	TASK_CHECK(fail2.mesh == 0);
	vector<dvec3> nan_anchor = anchor_pos;
	nan_anchor[0] = dvec3(numeric_limits<double>::infinity(), 0, 0);
	DeformationTransferResult fail3 = dt.solve(good, nan_anchor);
	TASK_CHECK(!fail3.ok());

	// The last good output is untouched, and the next valid frame solves.
	point saved = ok.mesh->vertices[3];
	DeformationTransferResult again = dt.solve(good, anchor_pos);
	TASK_CHECK(again.ok());
	TASK_CHECK(ok.mesh->vertices[3] == saved);
	TASK_CHECK(dt.successful_solve_count() == solves_before + 1);
	TASK_CHECK(dt.operator_build_count() == 1);
	TASK_CHECK(fabs(again.energy - ok.energy) < 1e-18);

	// Size mismatches are reported as invalid_argument.
	vector<point> short_pose(good.begin(), good.end() - 1);
	bool threw = false;
	try {
		dt.solve(short_pose, anchor_pos);
	} catch (const invalid_argument &) {
		threw = true;
	}
	TASK_CHECK(threw);
	delete ok.mesh;
	delete again.mesh;
	delete src;
	delete tgt;
	return 0;
}

int state_sparsity_reuse()
{
	const int N = 10;
	TriMesh *src = make_grid(N, dvec3(0, 0, 0));
	TriMesh *tgt = make_grid(N, dvec3(3, 0, 0));
	vector<FaceCorrespondence> corr;
	for (int f = 0; f < (int)tgt->faces.size(); f++)
		corr.push_back(FaceCorrespondence(f, f, 1.0));
	vector<int> anchors;
	anchors.push_back(0);
	anchors.push_back(N - 1);
	anchors.push_back((N - 1) * N);
	anchors.push_back(N * N - 1);
	DeformationTransfer dt(*src, *tgt, corr, anchors);

	int ndof = (int)(3 * dt.free_variable_count());
	size_t nnz = dt.stored_nonzeros();
	// Sparse: far fewer entries than a dense ndof x ndof matrix, and the
	// count grows linearly rather than quadratically.
	TASK_CHECK(nnz * 8 < (size_t)ndof * (size_t)ndof);
	TASK_CHECK(nnz < (size_t)ndof * 30);

	M3 R = rot_matrix(0.3, normalized(dvec3(0.2, 1.0, 0.4)));
	for (int rep = 0; rep < 3; rep++) {
		vector<point> pose(src->vertices.size());
		for (size_t v = 0; v < pose.size(); v++)
			pose[v] = point(R * to_d(src->vertices[v]));
		vector<dvec3> ap(anchors.size());
		for (size_t k = 0; k < anchors.size(); k++)
			ap[k] = R * to_d(tgt->vertices[anchors[k]]);
		DeformationTransferResult r = dt.solve(pose, ap);
		TASK_CHECK(r.ok());
		TASK_CHECK(r.residual <= 1e-8);
		delete r.mesh;
	}
	TASK_CHECK(dt.operator_build_count() == 1);
	TASK_CHECK(dt.successful_solve_count() == 3);
	TASK_CHECK(dt.stored_nonzeros() == nnz);
	delete src;
	delete tgt;
	return 0;
}

int state_caches_and_attributes()
{
	TriMesh *src = make_source_patch();
	TriMesh *tgt = make_target_fan();
	for (size_t v = 0; v < tgt->vertices.size(); v++) {
		tgt->colors.push_back(Color(0.1f * v, 0.2f, 0.3f));
		tgt->confidences.push_back(0.5f + 0.1f * v);
		tgt->flags.push_back(v + 1);
	}
	tgt->flag_curr = 7;
	tgt->need_neighbors();
	tgt->need_adjacentfaces();
	tgt->need_across_edge();
	tgt->need_normals();
	tgt->need_pointareas();
	tgt->need_bbox();
	TASK_CHECK(!tgt->normals.empty() && !tgt->pointareas.empty());
	TASK_CHECK(tgt->bbox.valid);

	vector<FaceCorrespondence> corr = make_correspondences();
	vector<int> anchors = make_anchors();
	DeformationTransfer dt(*src, *tgt, corr, anchors);
	vector<point> pose = src->vertices;
	pose[3] = point(1.3f, 0.8f, 1.2f);
	vector<dvec3> ap(anchors.size());
	for (size_t k = 0; k < anchors.size(); k++)
		ap[k] = to_d(tgt->vertices[anchors[k]]) + dvec3(0.1, 0, 0);
	DeformationTransferResult r = dt.solve(pose, ap);
	TASK_CHECK(r.ok());
	TriMesh *out = r.mesh;

	// Faces and per-vertex attributes survive unchanged.
	TASK_CHECK(out->faces == tgt->faces);
	TASK_CHECK(out->colors == tgt->colors);
	TASK_CHECK(out->confidences == tgt->confidences);
	TASK_CHECK(out->flags == tgt->flags);
	TASK_CHECK(out->flag_curr == 7);

	// Geometry-dependent caches are invalidated and rebuilt consistently.
	TASK_CHECK(out->normals.empty());
	TASK_CHECK(out->pointareas.empty());
	TASK_CHECK(out->cornerareas.empty());
	TASK_CHECK(!out->bbox.valid);
	TASK_CHECK(out->curv1.empty() && out->pdir1.empty());

	out->need_normals();
	out->need_pointareas();
	out->need_bbox();
	TriMesh fresh;
	fresh.vertices = out->vertices;
	fresh.faces = out->faces;
	fresh.need_normals();
	fresh.need_pointareas();
	fresh.need_bbox();
	TASK_CHECK(out->normals.size() == fresh.normals.size());
	for (size_t v = 0; v < out->normals.size(); v++)
		TASK_CHECK(len(out->normals[v] - fresh.normals[v]) < 1e-6);
	for (size_t v = 0; v < out->pointareas.size(); v++)
		TASK_CHECK(fabs(out->pointareas[v] - fresh.pointareas[v]) < 1e-7);
	TASK_CHECK(fabs(out->bbox.min[0] - fresh.bbox.min[0]) < 1e-7);
	TASK_CHECK(fabs(out->bbox.max[2] - fresh.bbox.max[2]) < 1e-7);
	// Independently recompute one face normal from the new positions.
	dvec3 e1 = to_d(out->vertices[1]) - to_d(out->vertices[0]);
	dvec3 e2 = to_d(out->vertices[4]) - to_d(out->vertices[0]);
	dvec3 fn = normalized(e1 CROSS e2);
	TASK_CHECK(len(normalized(to_d(out->trinorm(0))) - fn) < 1e-6);

	// Topology caches are retained.
	TASK_CHECK(out->neighbors.size() == tgt->neighbors.size());
	TASK_CHECK(out->across_edge.size() == tgt->across_edge.size());
	TASK_CHECK(out->adjacentfaces.size() == tgt->adjacentfaces.size());

	delete out;
	delete src;
	delete tgt;
	return 0;
}

int state_invalid_arguments()
{
	TriMesh *src = make_source_patch();
	TriMesh *tgt = make_target_fan();
	vector<FaceCorrespondence> corr = make_correspondences();
	vector<int> anchors = make_anchors();

	// Baseline construction is valid.
	bool threw = false;
	try {
		DeformationTransfer good(*src, *tgt, corr, anchors);
	} catch (...) {
		threw = true;
	}
	TASK_CHECK(!threw);

	// Bad correspondence indices.
	vector<FaceCorrespondence> bad = corr;
	bad[0].source_face = 99;
	try { DeformationTransfer d(*src, *tgt, bad, anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;
	bad = corr; bad[1].target_face = -1;
	try { DeformationTransfer d(*src, *tgt, bad, anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;

	// Non-positive / non-finite weights.
	bad = corr; bad[0].weight = 0.0;
	try { DeformationTransfer d(*src, *tgt, bad, anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;
	bad = corr; bad[0].weight = -2.0;
	try { DeformationTransfer d(*src, *tgt, bad, anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;
	bad = corr; bad[0].weight = numeric_limits<double>::quiet_NaN();
	try { DeformationTransfer d(*src, *tgt, bad, anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;

	// Uncovered target face.
	bad.clear();
	for (size_t c = 0; c < corr.size(); c++)
		if (corr[c].target_face != 3)
			bad.push_back(corr[c]);
	try { DeformationTransfer d(*src, *tgt, bad, anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;

	// Bad and duplicate anchors.
	vector<int> bad_anchors = anchors;
	bad_anchors[0] = 100;
	try { DeformationTransfer d(*src, *tgt, corr, bad_anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;
	bad_anchors = anchors;
	bad_anchors.push_back(anchors[0]);
	try { DeformationTransfer d(*src, *tgt, corr, bad_anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;

	// Component without an anchor: two-component mesh, anchors only in
	// the first one.
	TriMesh two = *tgt;
	int nv = (int)tgt->vertices.size(), nf = (int)tgt->faces.size();
	dvec3 shift(7, 0, 0);
	for (int v = 0; v < nv; v++)
		two.vertices.push_back(point(to_d(tgt->vertices[v]) + shift));
	for (int f = 0; f < nf; f++) {
		TriMesh::Face face = tgt->faces[f];
		two.faces.push_back(TriMesh::Face(face[0] + nv, face[1] + nv,
		                                  face[2] + nv));
	}
	vector<FaceCorrespondence> two_corr = corr;
	for (size_t c = 0; c < corr.size(); c++)
		two_corr.push_back(FaceCorrespondence(
			corr[c].source_face, corr[c].target_face + nf,
			corr[c].weight));
	try { DeformationTransfer d(*src, two, two_corr, anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw); threw = false;

	// Inconsistent orientation is rejected.
	TriMesh flipped = *tgt;
	TriMesh::Face f0 = flipped.faces[0];
	flipped.faces[0] = TriMesh::Face(f0[0], f0[2], f0[1]);
	try { DeformationTransfer d(*src, flipped, corr, anchors); }
	catch (const invalid_argument &) { threw = true; }
	TASK_CHECK(threw);

	delete src;
	delete tgt;
	return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
	{ "gradients",   "frame",              gradients_frame },
	{ "gradients",   "barycentric",        gradients_barycentric },
	{ "gradients",   "dense_match",        gradients_dense_match },
	{ "integration", "rigid_scale",        integration_rigid_scale },
	{ "integration", "components",         integration_components },
	{ "integration", "sequence",           integration_sequence },
	{ "state",       "ownership",          state_ownership },
	{ "state",       "failure_recovery",   state_failure_recovery },
	{ "state",       "sparsity_reuse",     state_sparsity_reuse },
	{ "state",       "caches_attributes",  state_caches_and_attributes },
	{ "state",       "invalid_arguments",  state_invalid_arguments },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
