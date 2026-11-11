/*
Tests for trimesh::apply_affine_differential().

All reference values come from independent constructions, never from
comparing two calls to the function under test:

  - Curvature/derivative checks compare against the exact shape
    operator and third derivative tensor of an analytic local Monge
    graph, pushed through the affine map by the chain rule and
    re-expressed in the transformed tangent plane.
  - Area checks compare against the mixed Voronoi areas recomputed from
    scratch on a separate mesh that only contains the transformed
    vertices and faces.

Sign conventions follow TriMesh: the local Monge height in an
orthonormal tangent frame (u,v,n) is
  h = -1/2 (k1 u^2 + k2 v^2)
      -1/6 (d0 u^3 + 3 d1 u^2 v + 3 d2 u v^2 + d3 v^3),
so the shape operator is K = -Hess(h) and the dcurv tensor is
D = -D^3(h).
*/

#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace trimesh;

namespace {

// ----------------------------------------------------------------------
// Analytic Monge patch in the xy parameter plane:
//   z = f(x,y) = -1/2 (k1 x^2 + k2 y^2)
//               -1/6 (c30 x^3 + 3 c21 x^2 y + 3 c12 x y^2 + c03 y^3)
// ----------------------------------------------------------------------
struct Patch {
	double k1, k2, c30, c21, c12, c03;
	double f(double x, double y) const
	{
		return -0.5 * (k1*x*x + k2*y*y)
		     - (c30*x*x*x + 3.0*c21*x*x*y +
		        3.0*c12*x*y*y + c03*y*y*y) / 6.0;
	}
	double fx(double x, double y) const
	{
		return -k1*x
		     - 0.5*(c30*x*x + 2.0*c21*x*y + c12*y*y);
	}
	double fy(double x, double y) const
	{
		return -k2*y
		     - 0.5*(c21*x*x + 2.0*c12*x*y + c03*y*y);
	}
	double fxx(double x, double y) const { return -k1 - c30*x - c21*y; }
	double fxy(double x, double y) const { return -c21*x - c12*y; }
	double fyy(double x, double y) const { return -k2 - c12*x - c03*y; }
	double fxxx() const { return -c30; }
	double fxxy() const { return -c21; }
	double fxyy() const { return -c12; }
	double fyyy() const { return -c03; }
};


// One sample point: position, orthonormal tangent frame (t1 x t2 = n),
// and the TriMesh-convention tensors K (shape operator) and D (dcurv)
// expressed in that frame.
struct Probe {
	point p;
	vec normal;
	vec t1, t2;
	double K[2][2];
	double D[2][2][2];
};


static double dot3(const double a[3], const double b[3])
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}


static Probe make_probe(const Patch &patch, double x0, double y0,
                        double phi)
{
	Probe pr;
	double fx = patch.fx(x0, y0), fy = patch.fy(x0, y0);
	pr.p = point(float(x0), float(y0), float(patch.f(x0, y0)));

	double nraw[3] = { -fx, -fy, 1.0 };
	double W = std::sqrt(dot3(nraw, nraw));
	for (int i = 0; i < 3; i++)
		pr.normal[i] = float(nraw[i] / W);

	// Project an arbitrary parameter-plane direction onto the tangent.
	double d[3] = { std::cos(phi), std::sin(phi),
	                fx*std::cos(phi) + fy*std::sin(phi) };
	double ld = std::sqrt(dot3(d, d));
	for (int i = 0; i < 3; i++)
		pr.t1[i] = float(d[i] / ld);
	pr.t2 = pr.normal CROSS pr.t1;

	// Parameter-space tangent basis r_x = (1,0,fx), r_y = (0,1,fy).
	double rx[3] = { 1, 0, fx }, ry[3] = { 0, 1, fy };
	double G[2][2] = {
		{ dot3(rx, rx), dot3(rx, ry) },
		{ dot3(ry, rx), dot3(ry, ry) }
	};
	double detG = G[0][0]*G[1][1] - G[0][1]*G[1][0];
	double Ginv[2][2] = {
		{  G[1][1]/detG, -G[0][1]/detG },
		{ -G[1][0]/detG,  G[0][0]/detG }
	};
	// Parameter coordinates of each frame tangent vector.
	double c[2][2];
	for (int a = 0; a < 2; a++) {
		const vec &ta = (a == 0) ? pr.t1 : pr.t2;
		double tv[3] = { ta[0], ta[1], ta[2] };
		double rhs[2] = { dot3(tv, rx), dot3(tv, ry) };
		c[a][0] = Ginv[0][0]*rhs[0] + Ginv[0][1]*rhs[1];
		c[a][1] = Ginv[1][0]*rhs[0] + Ginv[1][1]*rhs[1];
	}

	// Geometric second fundamental form in parameter coordinates:
	// b_ij = f_ij / W.  TriMesh shape operator K = -b.
	double b[2][2] = {
		{ patch.fxx(x0,y0)/W, patch.fxy(x0,y0)/W },
		{ patch.fxy(x0,y0)/W, patch.fyy(x0,y0)/W }
	};
	for (int a = 0; a < 2; a++)
	 for (int bb = 0; bb < 2; bb++) {
		double s = 0.0;
		for (int i = 0; i < 2; i++)
		 for (int j = 0; j < 2; j++)
			s += b[i][j] * c[a][i] * c[bb][j];
		pr.K[a][bb] = -s;
	 }

	// Geometric third tensor in parameter coordinates: f_ijk / W.
	// TriMesh D = -that, pulled into the tangent frame.
	double c3[2][2][2];
	c3[0][0][0] = patch.fxxx()/W;
	c3[0][0][1] = c3[0][1][0] = c3[1][0][0] = patch.fxxy()/W;
	c3[0][1][1] = c3[1][0][1] = c3[1][1][0] = patch.fxyy()/W;
	c3[1][1][1] = patch.fyyy()/W;
	for (int a = 0; a < 2; a++)
	 for (int bb = 0; bb < 2; bb++)
	  for (int g = 0; g < 2; g++) {
		double s = 0.0;
		for (int i = 0; i < 2; i++)
		 for (int j = 0; j < 2; j++)
		  for (int k = 0; k < 2; k++)
			s += c3[i][j][k] * c[a][i] * c[bb][j] * c[g][k];
		pr.D[a][bb][g] = -s;
	  }

	return pr;
}


// Regular grid mesh over the patch.
static TriMesh *make_patch_mesh(const Patch &patch, int n, double h)
{
	TriMesh *m = new TriMesh;
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++) {
			double x = (i - (n-1)/2.0) * h;
			double y = (j - (n-1)/2.0) * h;
			m->vertices.push_back(point(float(x), float(y),
			                            float(patch.f(x, y))));
		}
	for (int j = 0; j < n-1; j++)
		for (int i = 0; i < n-1; i++) {
			int a = j*n + i, b = a + 1, c = a + n, d = c + 1;
			m->faces.push_back(TriMesh::Face(a, b, c));
			m->faces.push_back(TriMesh::Face(b, d, c));
		}
	return m;
}


// Fill the cached fields from the analytic probes (external fit
// stand-in): the stored Monge form uses (pdir1,pdir2,normal) exactly.
static void install_analytic_fields(TriMesh *m,
                                    const std::vector<Probe> &probes,
                                    bool with_dcurv)
{
	int nv = int(m->vertices.size());
	m->normals.resize(nv);
	m->pdir1.resize(nv);
	m->pdir2.resize(nv);
	m->curv1.resize(nv);
	m->curv2.resize(nv);
	if (with_dcurv)
		m->dcurv.resize(nv);
	for (int i = 0; i < nv; i++) {
		const Probe &pr = probes[i];
		m->normals[i] = pr.normal;
		double a = pr.K[0][0], b = pr.K[0][1], c = pr.K[1][1];
		double tt = 0.0, cs = 1.0, sn = 0.0;
		if (b != 0.0) {
			double hh = 0.5 * (c - a) / b;
			tt = (hh < 0.0) ?
				1.0 / (hh - std::sqrt(1.0 + hh*hh)) :
				1.0 / (hh + std::sqrt(1.0 + hh*hh));
			cs = 1.0 / std::sqrt(1.0 + tt*tt);
			sn = tt * cs;
		}
		double l1 = a - tt*b, l2 = c + tt*b;
		double ux = cs, uy = -sn;
		if (std::fabs(l1) < std::fabs(l2)) {
			double tmp = l1; l1 = l2; l2 = tmp;
			ux = sn; uy = cs;
		}
		vec pu = float(ux) * pr.t1 + float(uy) * pr.t2;
		m->pdir1[i] = pu;
		m->pdir2[i] = pr.normal CROSS pu;
		m->curv1[i] = float(l1);
		m->curv2[i] = float(l2);
		if (with_dcurv) {
			double R[2][2] = {
				{ ux, uy },
				{ m->pdir2[i] DOT pr.t1, m->pdir2[i] DOT pr.t2 }
			};
			double st[2][2][2];
			for (int aa = 0; aa < 2; aa++)
			 for (int bb = 0; bb < 2; bb++)
			  for (int gg = 0; gg < 2; gg++) {
				double acc = 0.0;
				for (int x = 0; x < 2; x++)
				 for (int y = 0; y < 2; y++)
				  for (int z = 0; z < 2; z++)
					acc += pr.D[x][y][z] * R[aa][x] *
					       R[bb][y] * R[gg][z];
				st[aa][bb][gg] = acc;
			  }
			m->dcurv[i] = Vec<4,float>(float(st[0][0][0]),
			                           float(st[0][0][1]),
			                           float(st[0][1][1]),
			                           float(st[1][1][1]));
		}
	}
}


static point aff(const double A[3][3], const double t[3], const point &p)
{
	point q;
	for (int r = 0; r < 3; r++)
		q[r] = float(A[r][0]*p[0] + A[r][1]*p[1] +
		             A[r][2]*p[2] + t[r]);
	return q;
}


static void mat3_mul(const double A[3][3], const double B[3][3],
                     double C[3][3])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double s = 0.0;
			for (int k = 0; k < 3; k++)
				s += A[i][k] * B[k][j];
			C[i][j] = s;
		}
}


static void eye3(double A[3][3])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			A[i][j] = (i == j) ? 1.0 : 0.0;
}


static double mat3_det(const double A[3][3])
{
	return A[0][0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1])
	     - A[0][1]*(A[1][0]*A[2][2]-A[1][2]*A[2][0])
	     + A[0][2]*(A[1][0]*A[2][1]-A[1][1]*A[2][0]);
}


static void mat3_apply(const double A[3][3], const double x[3],
                       double y[3])
{
	for (int i = 0; i < 3; i++)
		y[i] = A[i][0]*x[0] + A[i][1]*x[1] + A[i][2]*x[2];
}


// Independent analytic transport: at the local Monge origin,
//   r_a = t_a,  r_ab = -K_ab n,  r_abc = -D_abc n.
// Apply A, build the new oriented orthonormal frame from A t1, and
// pull the tensors through the chain rule.
static void expected_under_affine(const double A[3][3], const Probe &pr,
                                  vec &exp_n, vec &exp_u, vec &exp_v,
                                  double eK[2][2], double eD[2][2][2])
{
	double t1[3] = { pr.t1[0], pr.t1[1], pr.t1[2] };
	double t2[3] = { pr.t2[0], pr.t2[1], pr.t2[2] };
	double n[3]  = { pr.normal[0], pr.normal[1], pr.normal[2] };
	double At1[3], At2[3], An[3];
	mat3_apply(A, t1, At1);
	mat3_apply(A, t2, At2);
	mat3_apply(A, n, An);

	double sgn = (mat3_det(A) < 0.0) ? -1.0 : 1.0;
	double cr[3] = {
		sgn*(At1[1]*At2[2] - At1[2]*At2[1]),
		sgn*(At1[2]*At2[0] - At1[0]*At2[2]),
		sgn*(At1[0]*At2[1] - At1[1]*At2[0])
	};
	double lcr = std::sqrt(dot3(cr, cr));
	double n3[3], e1[3], e2[3];
	for (int i = 0; i < 3; i++) {
		n3[i] = cr[i] / lcr;
		exp_n[i] = float(n3[i]);
	}
	double lu = std::sqrt(dot3(At1, At1));
	for (int i = 0; i < 3; i++)
		e1[i] = At1[i] / lu;
	e2[0] = n3[1]*e1[2] - n3[2]*e1[1];
	e2[1] = n3[2]*e1[0] - n3[0]*e1[2];
	e2[2] = n3[0]*e1[1] - n3[1]*e1[0];
	for (int i = 0; i < 3; i++) {
		exp_u[i] = float(e1[i]);
		exp_v[i] = float(e2[i]);
	}

	// Old tangent coordinates -> new tangent coordinates.
	double P[2][2] = {
		{ dot3(At1, e1), dot3(At2, e1) },
		{ dot3(At1, e2), dot3(At2, e2) }
	};
	double detP = P[0][0]*P[1][1] - P[0][1]*P[1][0];
	double J[2][2] = {
		{  P[1][1]/detP, -P[0][1]/detP },
		{ -P[1][0]/detP,  P[0][0]/detP }
	};

	// Local Monge height of the source patch: h_ij = -K_ij,
	// h_ijk = -D_ijk.  Under the affine map the new tangent
	// coordinates relate to the old ones by q = P b + h(b) t, so the
	// inverse reparametrization b = J q + phi2(q,q) + ... with
	//   phi2^i_ab = -J t^i h_ab  (alpha_i = -(J t)^i).
	// The normal component scales by c = (A n).n3.
	double tvec[2] = { dot3(An, e1), dot3(An, e2) };
	double c = dot3(An, n3);
	double alpha[2] = {
		-(J[0][0]*tvec[0] + J[0][1]*tvec[1]),
		-(J[1][0]*tvec[0] + J[1][1]*tvec[1])
	};
	double h2[2][2], h3[2][2][2];
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
		h2[a][b] = -pr.K[a][b];
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
	  for (int cc = 0; cc < 2; cc++)
		h3[a][b][cc] = -pr.D[a][b][cc];

	// h evaluated at the linearized inverse map b = J q:
	// (h o M)_ab = h_mn J_m^a J_n^b.
	double hJ[2][2];
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++) {
		double s = 0.0;
		for (int m = 0; m < 2; m++)
		 for (int n = 0; n < 2; n++)
			s += h2[m][n] * J[m][a] * J[n][b];
		hJ[a][b] = s;
	 }
	double phi2[2][2][2];
	for (int i = 0; i < 2; i++)
	 for (int a = 0; a < 2; a++)
	  for (int b = 0; b < 2; b++)
		phi2[i][a][b] = alpha[i] * hJ[a][b];

	// Second derivative of the image height: H_ab = c h_ij J_i^a J_j^b
	// (the reparametrization curvature lands in the tangent plane).
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++) {
		double s = 0.0;
		for (int i = 0; i < 2; i++)
		 for (int j = 0; j < 2; j++)
			s += h2[i][j] * J[i][a] * J[j][b];
		eK[a][b] = -c * s;
	 }

	// Third derivative, including the reparametrization term that a
	// shear turns into nonzero cubic coefficients:
	//   T_abc = c [ h_ijk J_i^a J_j^b J_k^c
	//               + 3 h_ij J_i^a phi2^j_bc  (symmetrized) ]
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
	  for (int cc = 0; cc < 2; cc++) {
		double tensor = 0.0;
		for (int i = 0; i < 2; i++)
		 for (int j = 0; j < 2; j++)
		  for (int k = 0; k < 2; k++)
			tensor += h3[i][j][k] * J[i][a] * J[j][b] * J[k][cc];
		double rep = 0.0;
		// symmetrize over the three output slots
		for (int i = 0; i < 2; i++)
		 for (int j = 0; j < 2; j++) {
			rep += h2[i][j] * (
				J[i][a] * phi2[j][b][cc] +
				J[i][b] * phi2[j][a][cc] +
				J[i][cc] * phi2[j][a][b]);
		 }
		eD[a][b][cc] = -c * (tensor + rep);
	  }
}


static float rel_err(double got, double ref)
{
	return float(std::fabs(got - ref) /
	             std::max(1e-6, std::fabs(ref)));
}


static void check_vertex_fields(const TriMesh &m, int idx,
                                const Probe &pr, const double A[3][3],
                                bool have_curv, bool have_dcurv,
                                float tol)
{
	vec exp_n, exp_u, exp_v;
	double eK[2][2], eD[2][2][2];
	expected_under_affine(A, pr, exp_n, exp_u, exp_v, eK, eD);

	const vec &n = m.normals[idx];
	for (int i = 0; i < 3; i++)
		assert(std::fabs(n[i] - exp_n[i]) < 1e-5f);

	if (!have_curv) {
		assert(m.curv1.empty());
		return;
	}

	// Frame invariants.
	const vec &u = m.pdir1[idx], &v = m.pdir2[idx];
	assert(std::fabs(len(u) - 1.0f) < 1e-6f);
	assert(std::fabs(len(v) - 1.0f) < 1e-6f);
	assert(std::fabs(u DOT v) < 1e-6f);
	assert(std::fabs(((u CROSS v) DOT n) - 1.0f) < 1e-5f);
	assert(std::fabs(m.curv1[idx]) >= std::fabs(m.curv2[idx]) - 1e-7f);

	// Reconstruct the shape operator in the expected tangent frame.
	double eu[2] = { u DOT exp_u, u DOT exp_v };
	double ev[2] = { v DOT exp_u, v DOT exp_v };
	double gotK[2][2];
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
		gotK[a][b] = m.curv1[idx] * eu[a]*eu[b] +
		             m.curv2[idx] * ev[a]*ev[b];
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
		assert(rel_err(gotK[a][b], eK[a][b]) < tol);

	// Principal curvatures vs. eigenvalues of eK.
	double tr = eK[0][0] + eK[1][1];
	double det = eK[0][0]*eK[1][1] - eK[0][1]*eK[1][0];
	double disc = std::sqrt(std::max(0.0, 0.25*tr*tr - det));
	double l1 = 0.5*tr + disc, l2 = 0.5*tr - disc;
	if (std::fabs(l1) < std::fabs(l2)) {
		double tmp = l1; l1 = l2; l2 = tmp;
	}
	assert(rel_err(m.curv1[idx], l1) < tol);
	assert(rel_err(m.curv2[idx], l2) < tol);

	if (!have_dcurv) {
		assert(m.dcurv.empty());
		return;
	}
	assert(m.dcurv.size() == m.vertices.size());

	// Rebuild the full symmetric third tensor in the expected frame.
	const Vec<4,float> &d = m.dcurv[idx];
	double Sf[2][2][2];
	Sf[0][0][0] = d[0];
	Sf[0][0][1] = Sf[0][1][0] = Sf[1][0][0] = d[1];
	Sf[0][1][1] = Sf[1][0][1] = Sf[1][1][0] = d[2];
	Sf[1][1][1] = d[3];
	double R[2][2] = {
		{ u DOT exp_u, u DOT exp_v },
		{ v DOT exp_u, v DOT exp_v }
	};
	double scale = 1e-5;
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
	  for (int c = 0; c < 2; c++)
		scale = std::max(scale, std::fabs(eD[a][b][c]));
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
	  for (int c = 0; c < 2; c++) {
		double s = 0.0;
		for (int i = 0; i < 2; i++)
		 for (int j = 0; j < 2; j++)
		  for (int k = 0; k < 2; k++)
			s += Sf[i][j][k] * R[i][a] * R[j][b] * R[k][c];
		assert(std::fabs(s - eD[a][b][c]) < tol * scale);
	}
}


static xform make_xform(const double A[3][3],
                        double tx = 0, double ty = 0, double tz = 0)
{
	xform xf;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			xf(r, c) = A[r][c];
	xf(0, 3) = tx; xf(1, 3) = ty; xf(2, 3) = tz;
	return xf;
}


// Independent mesh holding only the transformed vertices and faces.
static TriMesh *transformed_copy(const std::vector<point> &oldverts,
                                 const std::vector<TriMesh::Face> &faces,
                                 const double A[3][3],
                                 const double t[3])
{
	TriMesh *q = new TriMesh;
	q->vertices.resize(oldverts.size());
	for (size_t i = 0; i < oldverts.size(); i++)
		q->vertices[i] = aff(A, t, oldverts[i]);
	q->faces = faces;
	return q;
}


// Rebuild the full symmetric third tensor at vertex idx in an arbitrary
// orthonormal tangent frame (fu, fv), from the stored principal dcurv.
static void reconstruct_d_tensor(const TriMesh &m, int idx,
                                 const vec &fu, const vec &fv,
                                 double out[2][2][2])
{
	const vec &u = m.pdir1[idx], &v = m.pdir2[idx];
	const Vec<4,float> &d = m.dcurv[idx];
	double Sf[2][2][2];
	Sf[0][0][0] = d[0];
	Sf[0][0][1] = Sf[0][1][0] = Sf[1][0][0] = d[1];
	Sf[0][1][1] = Sf[1][0][1] = Sf[1][1][0] = d[2];
	Sf[1][1][1] = d[3];
	double R[2][2] = {
		{ u DOT fu, u DOT fv },
		{ v DOT fu, v DOT fv }
	};
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
	  for (int c = 0; c < 2; c++) {
		double s = 0.0;
		for (int i = 0; i < 2; i++)
		 for (int j = 0; j < 2; j++)
		  for (int k = 0; k < 2; k++)
			s += Sf[i][j][k] * R[i][a] * R[j][b] * R[k][c];
		out[a][b][c] = s;
	  }
}


static const int N = 9;
static const double H = 0.05;

static void build_probes(const Patch &patch, std::vector<Probe> &probes)
{
	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++) {
			double x = (i - (N-1)/2.0) * H;
			double y = (j - (N-1)/2.0) * H;
			double phi = 0.37 * (i + 1) + 0.21 * (j + 1);
			probes.push_back(make_probe(patch, x, y, phi));
		}
}


static bool interior_idx(int idx)
{
	int r = idx / N, c = idx % N;
	return r > 0 && r < N-1 && c > 0 && c < N-1;
}


// ----------------------------------------------------------------------
// 1. Saddle patch with nonzero cubic terms, tilted shear.
// ----------------------------------------------------------------------
static void test_saddle_shear()
{
	Patch patch = { 1.7, -1.1, 0.9, -0.6, 0.5, -0.7 };
	std::vector<Probe> probes;
	build_probes(patch, probes);

	TriMesh *m = make_patch_mesh(patch, N, H);
	install_analytic_fields(m, probes, true);
	m->need_pointareas();
	m->need_bbox();
	m->need_bsphere();
	std::vector<point> old_verts = m->vertices;
	std::vector<TriMesh::Face> old_faces = m->faces;

	double A[3][3] = {
		{ 1.3, 0.4, -0.25 },
		{ -0.35, 0.9, 0.3 },
		{ 0.2, -0.5, 1.15 }
	};
	double t[3] = { 0.7, -0.4, 0.25 };
	assert(apply_affine_differential(m, make_xform(A, t[0], t[1], t[2])));

	int nv = N*N;
	for (int i = 0; i < nv; i++) {
		point expect = aff(A, t, old_verts[i]);
		for (int k = 0; k < 3; k++)
			assert(std::fabs(m->vertices[i][k] - expect[k]) < 1e-5f);
		if (interior_idx(i))
			check_vertex_fields(*m, i, probes[i], A, true, true, 2e-3f);
	}

	// Areas: reference mesh with only the transformed vertices/faces.
	TriMesh *ref = transformed_copy(old_verts, old_faces, A, t);
	ref->need_pointareas();
	assert(m->pointareas.size() == ref->pointareas.size());
	assert(m->cornerareas.size() == ref->cornerareas.size());
	for (size_t i = 0; i < m->pointareas.size(); i++)
		assert(std::fabs(m->pointareas[i] - ref->pointareas[i]) <
		       1e-5f * std::max(1.0f, ref->pointareas[i]));
	for (size_t i = 0; i < m->cornerareas.size(); i++)
	 for (int j = 0; j < 3; j++)
		assert(std::fabs(m->cornerareas[i][j] -
		                 ref->cornerareas[i][j]) <
		       1e-5f * std::max(1.0f, ref->cornerareas[i][j]));
	assert(m->bbox.valid && m->bsphere.valid);
	delete ref;
	delete m;
	std::puts("test_saddle_shear ok");
}


// ----------------------------------------------------------------------
// 2. Reflection: normal sign follows the new winding.
// ----------------------------------------------------------------------
static void test_reflection()
{
	Patch patch = { 1.2, 0.8, 0.5, 0.3, -0.4, 0.2 };
	std::vector<Probe> probes;
	build_probes(patch, probes);

	TriMesh *m = make_patch_mesh(patch, N, H);
	install_analytic_fields(m, probes, true);
	m->need_pointareas();

	double A[3][3] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, -1.0 }
	};
	assert(apply_affine_differential(m, make_xform(A)));

	for (int i = 0; i < N*N; i++) {
		// The inverse-transpose image of the old normal flips its z;
		// the transformed winding flips too, adding another sign.
		vec img = probes[i].normal;
		img[2] = -img[2];
		assert(std::fabs(m->normals[i] DOT (-img)) > 1.0f - 1e-5f);
		if (interior_idx(i))
			check_vertex_fields(*m, i, probes[i], A, true, true, 2e-3f);
	}
	delete m;
	std::puts("test_reflection ok");
}


// ----------------------------------------------------------------------
// 3. Two successive transforms vs. the composed transform; uniform
//    scaling rules and translation invariance.
// ----------------------------------------------------------------------
static void test_composition_and_scale()
{
	Patch patch = { 1.5, 0.9, 0.6, -0.4, 0.3, 0.5 };
	std::vector<Probe> probes;
	build_probes(patch, probes);

	double A1[3][3] = {
		{ 1.1, 0.2, 0.1 },
		{ 0.0, 0.8, -0.3 },
		{ 0.15, 0.1, 1.0 }
	};
	double A2[3][3] = {
		{ 0.9, -0.2, 0.0 },
		{ 0.3, 1.1, 0.25 },
		{ -0.1, 0.2, 0.85 }
	};
	double Ac[3][3];
	mat3_mul(A2, A1, Ac);

	TriMesh *seq = make_patch_mesh(patch, N, H);
	install_analytic_fields(seq, probes, true);
	TriMesh *comp = make_patch_mesh(patch, N, H);
	install_analytic_fields(comp, probes, true);

	assert(apply_affine_differential(seq, make_xform(A1)));
	assert(apply_affine_differential(seq, make_xform(A2)));
	assert(apply_affine_differential(comp, make_xform(Ac)));

	for (int i = 0; i < N*N; i++) {
		if (!interior_idx(i))
			continue;
		for (int k = 0; k < 3; k++) {
			assert(std::fabs(seq->vertices[i][k] -
			                 comp->vertices[i][k]) < 1e-5f);
			assert(std::fabs(seq->normals[i][k] -
			                 comp->normals[i][k]) < 1e-5f);
		}
		assert(std::fabs(seq->curv1[i] - comp->curv1[i]) <
		       2e-4f * std::max(1.0f, std::fabs(comp->curv1[i])));
		assert(std::fabs(seq->curv2[i] - comp->curv2[i]) <
		       2e-4f * std::max(1.0f, std::fabs(comp->curv2[i])));
		// Frames agree up to principal-direction sign flips.
		assert(std::fabs(std::fabs(seq->pdir1[i] DOT comp->pdir1[i]) - 1.0f)
		       < 1e-4f);
		assert(std::fabs(std::fabs(seq->pdir2[i] DOT comp->pdir2[i]) - 1.0f)
		       < 1e-4f);
		// Compare the full third tensors in comp's principal frame,
		// which is invariant to per-direction sign flips.
		double Ds[2][2][2], Dc[2][2][2];
		reconstruct_d_tensor(*seq, i, comp->pdir1[i], comp->pdir2[i], Ds);
		reconstruct_d_tensor(*comp, i, comp->pdir1[i], comp->pdir2[i], Dc);
		double dscale = 1e-5;
		for (int a = 0; a < 2; a++)
		 for (int b = 0; b < 2; b++)
		  for (int cc = 0; cc < 2; cc++)
			dscale = std::max(dscale, std::fabs(Dc[a][b][cc]));
		for (int a = 0; a < 2; a++)
		 for (int b = 0; b < 2; b++)
		  for (int cc = 0; cc < 2; cc++)
			assert(std::fabs(Ds[a][b][cc] - Dc[a][b][cc]) <
			       5e-4f * dscale);
	}

	// Uniform scaling s: curvatures /s, dcurv /s^2; translation no-op.
	TriMesh *sc = make_patch_mesh(patch, N, H);
	install_analytic_fields(sc, probes, true);
	sc->need_pointareas();
	std::vector<float> k1old = sc->curv1, k2old = sc->curv2;
	std::vector< Vec<4,float> > dold = sc->dcurv;
	std::vector<float> paold = sc->pointareas;
	float s = 2.5f;
	double S[3][3] = { { s,0,0 }, { 0,s,0 }, { 0,0,s } };
	assert(apply_affine_differential(
		sc, make_xform(S, 1.3, -2.1, 0.6)));
	for (int i = 0; i < N*N; i++) {
		assert(std::fabs(sc->curv1[i] - k1old[i]/s) < 1e-6f);
		assert(std::fabs(sc->curv2[i] - k2old[i]/s) < 1e-6f);
		for (int q = 0; q < 4; q++)
			assert(std::fabs(sc->dcurv[i][q] - dold[i][q]/(s*s))
			       < 1e-6f * std::max(1.0f, std::fabs(dold[i][q])));
		assert(std::fabs(sc->pointareas[i] - paold[i]*s*s) <
		       1e-5f * std::max(1.0f, paold[i]));
	}
	delete seq;
	delete comp;
	delete sc;
	std::puts("test_composition_and_scale ok");
}


// ----------------------------------------------------------------------
// 4. A zero-cubic patch under shear gains nonzero third-order terms.
// ----------------------------------------------------------------------
static void test_shear_creates_cubic()
{
	Patch patch = { 1.4, -0.9, 0.0, 0.0, 0.0, 0.0 };
	std::vector<Probe> probes;
	build_probes(patch, probes);

	TriMesh *m = make_patch_mesh(patch, N, H);
	install_analytic_fields(m, probes, true);
	for (int i = 0; i < N*N; i++)
		for (int q = 0; q < 4; q++)
			assert(m->dcurv[i][q] == 0.0f);

	double A[3][3] = {
		{ 1.0, 0.5, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }
	};
	assert(apply_affine_differential(m, make_xform(A)));

	int saw_nonzero = 0;
	for (int i = 0; i < N*N; i++) {
		if (!interior_idx(i))
			continue;
		check_vertex_fields(*m, i, probes[i], A, true, true, 2e-3f);
		for (int q = 0; q < 4; q++)
			if (std::fabs(m->dcurv[i][q]) > 1e-4f)
				saw_nonzero++;
	}
	assert(saw_nonzero > 0);
	delete m;
	std::puts("test_shear_creates_cubic ok");
}


// ----------------------------------------------------------------------
// 5. Partial caches and laziness.
// ----------------------------------------------------------------------
static void test_partial_caches()
{
	Patch patch = { 1.3, 0.7, 0.4, -0.3, 0.2, 0.5 };
	std::vector<Probe> probes;
	build_probes(patch, probes);

	double A[3][3] = {
		{ 1.2, 0.1, 0.0 },
		{ 0.0, 0.9, 0.2 },
		{ 0.1, 0.0, 1.1 }
	};
	double zero[3] = { 0, 0, 0 };
	xform xf = make_xform(A);

	// (a) No fields at all: nothing gets created.
	{
		TriMesh *m = make_patch_mesh(patch, N, H);
		assert(apply_affine_differential(m, xf));
		assert(m->normals.empty() && m->curv1.empty() &&
		       m->curv2.empty() && m->pdir1.empty() &&
		       m->pdir2.empty() && m->dcurv.empty());
		assert(m->pointareas.empty() && m->cornerareas.empty());
		assert(!m->bbox.valid && !m->bsphere.valid);
		delete m;
	}

	// (b) Normals but no curvatures: normals transported, no curv.
	{
		TriMesh *m = make_patch_mesh(patch, N, H);
		m->need_normals();
		assert(apply_affine_differential(m, xf));
		assert(m->normals.size() == m->vertices.size());
		assert(m->curv1.empty() && m->dcurv.empty() &&
		       m->pointareas.empty());
		for (int i = 0; i < N*N; i++) {
			assert(std::fabs(len(m->normals[i]) - 1.0f) < 1e-6f);
			if (interior_idx(i)) {
				vec en, eu, ev;
				double eK[2][2], eD[2][2][2];
				expected_under_affine(A, probes[i], en, eu, ev,
				                      eK, eD);
				assert(std::fabs(m->normals[i] DOT en) > 1.0f - 1e-5f);
			}
		}
		delete m;
	}

	// (c) Curvatures without dcurv: curvature transported, dcurv empty.
	{
		TriMesh *m = make_patch_mesh(patch, N, H);
		install_analytic_fields(m, probes, false);
		assert(m->dcurv.empty());
		assert(apply_affine_differential(m, xf));
		assert(m->curv1.size() == m->vertices.size());
		assert(m->dcurv.empty());
		for (int i = 0; i < N*N; i++)
			if (interior_idx(i))
				check_vertex_fields(*m, i, probes[i], A, true, false,
				                    2e-3f);
		delete m;
	}

	// (d) Areas only, no differential fields.
	{
		TriMesh *m = make_patch_mesh(patch, N, H);
		m->need_pointareas();
		std::vector<point> oldv = m->vertices;
		std::vector<TriMesh::Face> faces = m->faces;
		assert(apply_affine_differential(m, xf));
		TriMesh *ref = transformed_copy(oldv, faces, A, zero);
		ref->need_pointareas();
		for (size_t i = 0; i < m->pointareas.size(); i++)
			assert(std::fabs(m->pointareas[i] - ref->pointareas[i]) <
			       1e-5f * std::max(1.0f, ref->pointareas[i]));
		assert(m->normals.empty() && m->curv1.empty());
		delete ref;
		delete m;
	}

	// (e) Cached bbox refreshed; uncached bsphere stays lazy.
	{
		TriMesh *m = make_patch_mesh(patch, N, H);
		m->need_bbox();
		assert(apply_affine_differential(m, xf));
		assert(m->bbox.valid);
		assert(!m->bsphere.valid);
		Box<3,float> bb;
		for (size_t i = 0; i < m->vertices.size(); i++)
			bb += m->vertices[i];
		assert(bb == m->bbox);
		delete m;
	}
	std::puts("test_partial_caches ok");
}


// ----------------------------------------------------------------------
// 6. Rejection paths: mesh content is unchanged.
// ----------------------------------------------------------------------
struct MeshSnap {
	std::vector<point> v;
	std::vector<vec> n, u, w;
	std::vector<float> k1, k2, pa;
	std::vector< Vec<4,float> > d;
};

static MeshSnap snapshot(const TriMesh &m)
{
	MeshSnap s;
	s.v = m.vertices; s.n = m.normals; s.u = m.pdir1;
	s.w = m.pdir2; s.k1 = m.curv1; s.k2 = m.curv2;
	s.d = m.dcurv; s.pa = m.pointareas;
	return s;
}

static void assert_unchanged(const TriMesh &m, const MeshSnap &s)
{
	assert(m.vertices == s.v && m.normals == s.n);
	assert(m.pdir1 == s.u && m.pdir2 == s.w);
	assert(m.curv1 == s.k1 && m.curv2 == s.k2);
	assert(m.dcurv == s.d && m.pointareas == s.pa);
}

static void test_rejections()
{
	Patch patch = { 1.1, 0.6, 0.3, 0.2, -0.2, 0.4 };
	std::vector<Probe> probes;
	build_probes(patch, probes);

	auto fresh = [&]() {
		TriMesh *m = make_patch_mesh(patch, N, H);
		install_analytic_fields(m, probes, true);
		m->need_pointareas();
		return m;
	};

	// Projection matrix (last row != (0,0,0,1)).
	{
		TriMesh *m = fresh();
		MeshSnap s = snapshot(*m);
		xform xf = xform::frustum(-1, 1, -1, 1, 1, 10);
		assert(!apply_affine_differential(m, xf));
		assert_unchanged(*m, s);
		delete m;
	}

	// Singular matrix.
	{
		TriMesh *m = fresh();
		MeshSnap s = snapshot(*m);
		double A[3][3] = {
			{ 1.0, 2.0, 3.0 },
			{ 2.0, 4.0, 6.0 },
			{ 1.0, 0.0, 1.0 }
		};
		assert(!apply_affine_differential(m, make_xform(A)));
		assert_unchanged(*m, s);
		delete m;
	}

	// Near-singular: |det| == 1e-12 s^3 is rejected by the <= rule.
	// With two diagonal entries equal to sbig, the third one at the
	// threshold is 1e-12 * sbig.
	{
		TriMesh *m = fresh();
		MeshSnap s = snapshot(*m);
		double sbig = 10.0;
		double eps = 1.0e-12 * sbig;
		double A[3][3] = {
			{ sbig, 0.0, 0.0 },
			{ 0.0, sbig, 0.0 },
			{ 0.0, 0.0, eps }
		};
		assert(!apply_affine_differential(m, make_xform(A)));
		assert_unchanged(*m, s);
		// Just above the threshold is accepted and stays finite.
		double B[3][3] = {
			{ sbig, 0.0, 0.0 },
			{ 0.0, sbig, 0.0 },
			{ 0.0, 0.0, eps * 1.5 }
		};
		assert(apply_affine_differential(m, make_xform(B)));
		delete m;
	}

	// Non-finite entry.
	{
		TriMesh *m = fresh();
		MeshSnap s = snapshot(*m);
		double A[3][3] = {
			{ 1.0, 0.0, 0.0 },
			{ 0.0, 1.0, 0.0 },
			{ 0.0, 0.0, std::nan("") }
		};
		assert(!apply_affine_differential(m, make_xform(A)));
		assert_unchanged(*m, s);
		delete m;
	}

	// Incomplete curvature fields.
	{
		TriMesh *m = fresh();
		MeshSnap s = snapshot(*m);
		m->pdir2.pop_back();
		double I[3][3]; eye3(I);
		assert(!apply_affine_differential(m, make_xform(I)));
		assert(m->vertices == s.v);
		delete m;
	}

	// dcurv without curvatures.
	{
		TriMesh *m = make_patch_mesh(patch, N, H);
		m->need_normals();
		m->dcurv.resize(m->vertices.size());
		double I[3][3]; eye3(I);
		assert(!apply_affine_differential(m, make_xform(I)));
		assert(m->curv1.empty());
		delete m;
	}

	// Curvatures without normals.
	{
		TriMesh *m = make_patch_mesh(patch, N, H);
		size_t nv = m->vertices.size();
		m->curv1.resize(nv, 0.0f);
		m->curv2.resize(nv, 0.0f);
		m->pdir1.resize(nv, vec(1, 0, 0));
		m->pdir2.resize(nv, vec(0, 1, 0));
		double I[3][3]; eye3(I);
		assert(!apply_affine_differential(m, make_xform(I)));
		assert(m->normals.empty());
		delete m;
	}

	// Non-finite cached value.
	{
		TriMesh *m = fresh();
		MeshSnap s = snapshot(*m);
		m->curv1[3] = std::nan("");
		double B[3][3] = {
			{ 2.0, 0.0, 0.0 },
			{ 0.0, 1.0, 0.0 },
			{ 0.0, 0.0, 1.0 }
		};
		assert(!apply_affine_differential(m, make_xform(B)));
		assert(m->vertices == s.v);
		delete m;
	}

	// Null mesh.
	{
		double I[3][3]; eye3(I);
		assert(!apply_affine_differential(0, make_xform(I)));
	}

	std::puts("test_rejections ok");
}


// ----------------------------------------------------------------------
// 7. Invariants: connectivity, face order, colors, confidences, flags.
// ----------------------------------------------------------------------
static void test_untouched_data()
{
	Patch patch = { 1.0, 0.5, 0.3, 0.1, -0.2, 0.2 };
	std::vector<Probe> probes;
	build_probes(patch, probes);
	TriMesh *m = make_patch_mesh(patch, N, H);
	install_analytic_fields(m, probes, true);
	int nv = N*N;
	m->colors.resize(nv);
	m->confidences.resize(nv);
	m->flags.resize(nv);
	for (int i = 0; i < nv; i++) {
		m->colors[i] = Color(float(i)/nv, 0.3f, 0.7f);
		m->confidences[i] = 0.5f + 0.01f * i;
		m->flags[i] = 7u * i;
	}
	std::vector<TriMesh::Face> faces_before = m->faces;
	double A[3][3] = {
		{ 0.8, 0.2, 0.1 },
		{ 0.0, 1.1, -0.2 },
		{ 0.15, 0.0, 0.9 }
	};
	assert(apply_affine_differential(m, make_xform(A)));
	assert(m->faces == faces_before);
	for (int i = 0; i < nv; i++) {
		assert(m->colors[i] == Color(float(i)/nv, 0.3f, 0.7f));
		assert(m->confidences[i] == 0.5f + 0.01f * i);
		assert(m->flags[i] == 7u * i);
	}
	delete m;
	std::puts("test_untouched_data ok");
}

} // anonymous namespace


int main()
{
	TriMesh::set_verbose(0);
	test_saddle_shear();
	test_reflection();
	test_composition_and_scale();
	test_shear_creates_cubic();
	test_partial_caches();
	test_rejections();
	test_untouched_data();
	std::puts("AFFINE_DIFFERENTIAL_OK");
	return 0;
}
