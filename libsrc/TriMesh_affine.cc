/*
Szymon Rusinkiewicz
Princeton University

TriMesh_affine.cc
Apply a finite, invertible 3D affine transform to a mesh, transporting
the already-computed differential fields (normals, principal curvature
frame, dcurv) along with the geometry, and recomputing any cached
mixed-Voronoi point/corner areas.

The transport works from the definition of the cached fields: every
vertex has a local Monge patch in its (pdir1, pdir2, normal) frame,

  h(u,v) = -1/2 (k1 u^2 + k2 v^2)
           -1/6 (d0 u^3 + 3 d1 u^2 v + 3 d2 u v^2 + d3 v^3),

with terms of order 4 and above discarded.  After the affine map the
same image surface is re-expanded as a Monge patch of degree 3 in the
new orthonormal tangent frame by explicit polynomial reparameterization,
so a shear can create nonzero third-order terms even when the input
dcurv vanishes, and reflections flip signs together with the normal.
*/

#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>


namespace trimesh {

using std::size_t;

namespace {

// Polynomial in two variables, truncated to total degree 3.
// c[i][j] is the coefficient of q1^i q2^j.
struct Poly3 {
	double c[4][4];
	Poly3()
	{
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				c[i][j] = 0.0;
	}
};


static Poly3 poly_mul(const Poly3 &a, const Poly3 &b)
{
	Poly3 r;
	for (int i = 0; i <= 3; i++) {
		for (int j = 0; i + j <= 3; j++) {
			if (a.c[i][j] == 0.0)
				continue;
			for (int k = 0; k <= 3; k++) {
				for (int l = 0; k + l <= 3; l++) {
					if (b.c[k][l] == 0.0)
						continue;
					if (i + j + k + l <= 3)
						r.c[i+k][j+l] += a.c[i][j] * b.c[k][l];
				}
			}
		}
	}
	return r;
}


static void poly_add_scaled(Poly3 &r, double s, const Poly3 &a)
{
	for (int i = 0; i <= 3; i++)
		for (int j = 0; i + j <= 3; j++)
			r.c[i][j] += s * a.c[i][j];
}


// Substitute b1 = B1(q1,q2), b2 = B2(q1,q2) into F(b1,b2),
// truncating to degree 3.
static Poly3 poly_subst(const Poly3 &F, const Poly3 &B1, const Poly3 &B2)
{
	// Powers of B1 and B2 up to degree 3
	Poly3 pw1[4], pw2[4];
	pw1[0].c[0][0] = 1.0;
	pw2[0].c[0][0] = 1.0;
	pw1[1] = B1;
	pw2[1] = B2;
	for (int i = 2; i <= 3; i++) {
		pw1[i] = poly_mul(pw1[i-1], B1);
		pw2[i] = poly_mul(pw2[i-1], B2);
	}

	Poly3 r;
	for (int i = 0; i <= 3; i++) {
		for (int j = 0; i + j <= 3; j++) {
			if (F.c[i][j] == 0.0)
				continue;
			poly_add_scaled(r, F.c[i][j], poly_mul(pw1[i], pw2[j]));
		}
	}
	return r;
}


// Determinant and inverse of a 3x3 matrix, stored row-major.
static double mat3_det(const double A[3][3])
{
	return A[0][0] * (A[1][1]*A[2][2] - A[1][2]*A[2][1])
	     - A[0][1] * (A[1][0]*A[2][2] - A[1][2]*A[2][0])
	     + A[0][2] * (A[1][0]*A[2][1] - A[1][1]*A[2][0]);
}


static bool mat3_inv(const double A[3][3], double invA[3][3])
{
	double d = mat3_det(A);
	if (d == 0.0)
		return false;
	double rd = 1.0 / d;
	invA[0][0] = (A[1][1]*A[2][2] - A[1][2]*A[2][1]) * rd;
	invA[0][1] = (A[0][2]*A[2][1] - A[0][1]*A[2][2]) * rd;
	invA[0][2] = (A[0][1]*A[1][2] - A[0][2]*A[1][1]) * rd;
	invA[1][0] = (A[1][2]*A[2][0] - A[1][0]*A[2][2]) * rd;
	invA[1][1] = (A[0][0]*A[2][2] - A[0][2]*A[2][0]) * rd;
	invA[1][2] = (A[0][2]*A[1][0] - A[0][0]*A[1][2]) * rd;
	invA[2][0] = (A[1][0]*A[2][1] - A[1][1]*A[2][0]) * rd;
	invA[2][1] = (A[0][1]*A[2][0] - A[0][0]*A[2][1]) * rd;
	invA[2][2] = (A[0][0]*A[1][1] - A[0][1]*A[1][0]) * rd;
	return true;
}


static void mat3_vec(const double A[3][3], const double x[3], double y[3])
{
	for (int i = 0; i < 3; i++)
		y[i] = A[i][0]*x[0] + A[i][1]*x[1] + A[i][2]*x[2];
}


static bool finite_vec(const vec &v)
{
	for (int i = 0; i < 3; i++)
		if (!std::isfinite(v[i]))
			return false;
	return true;
}


// Jacobi eigen-decomposition of a symmetric 2x2 matrix,
// returning eigenvalues ordered by decreasing absolute value
// and the eigenvector of the first eigenvalue (in the same basis).
static void eigen2_sym(double a, double b, double c,
                       double &lambda1, double &lambda2,
                       double &ev1x, double &ev1y)
{
	double tt = 0.0, cs = 1.0, sn = 0.0;
	if (b != 0.0) {
		double h = 0.5 * (c - a) / b;
		tt = (h < 0.0) ?
			1.0 / (h - std::sqrt(1.0 + h*h)) :
			1.0 / (h + std::sqrt(1.0 + h*h));
		cs = 1.0 / std::sqrt(1.0 + tt*tt);
		sn = tt * cs;
	}
	double l1 = a - tt * b;
	double l2 = c + tt * b;
	double x1 = cs, y1 = -sn;
	if (std::fabs(l1) < std::fabs(l2)) {
		double tmp = l1; l1 = l2; l2 = tmp;
		x1 = sn; y1 = cs;
	}
	lambda1 = l1;
	lambda2 = l2;
	ev1x = x1;
	ev1y = y1;
}


// Transport the curvature frame and dcurv of a single vertex through
// the linear part A of the affine transform.  The translation does not
// affect any differential quantity.
static void transport_vertex(const double A[3][3], double detA,
                             const vec &old_n, const vec &old_u,
                             const vec &old_v, float k1, float k2,
                             const Vec<4,float> &d,
                             vec &new_n, vec &new_u, vec &new_v,
                             float &new_k1, float &new_k2,
                             Vec<4,float> *new_d, bool have_dcurv)
{
	const double n[3] = { old_n[0], old_n[1], old_n[2] };
	const double u[3] = { old_u[0], old_u[1], old_u[2] };
	const double v[3] = { old_v[0], old_v[1], old_v[2] };

	double Au[3], Av[3], An[3];
	mat3_vec(A, u, Au);
	mat3_vec(A, v, Av);
	mat3_vec(A, n, An);

	// New oriented unit normal: follows the transformed face winding.
	double w3[3] = {
		Au[1]*Av[2] - Au[2]*Av[1],
		Au[2]*Av[0] - Au[0]*Av[2],
		Au[0]*Av[1] - Au[1]*Av[0]
	};
	double nw = std::sqrt(w3[0]*w3[0] + w3[1]*w3[1] + w3[2]*w3[2]);
	double sgn = (detA < 0.0) ? -1.0 : 1.0;
	for (int i = 0; i < 3; i++)
		w3[i] *= sgn / nw;

	// Right-handed orthonormal tangent frame.
	double e1[3] = { Au[0], Au[1], Au[2] };
	double le1 = std::sqrt(e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2]);
	for (int i = 0; i < 3; i++)
		e1[i] /= le1;
	double e2[3] = {
		w3[1]*e1[2] - w3[2]*e1[1],
		w3[2]*e1[0] - w3[0]*e1[2],
		w3[0]*e1[1] - w3[1]*e1[0]
	};

	// Tangent-coordinate map q = P b + h(b) t, normal coordinate c h(b),
	// where b are the old tangent coordinates and q the coordinates in
	// the new orthonormal frame.
	double P[2][2], t[2];
	P[0][0] = Au[0]*e1[0] + Au[1]*e1[1] + Au[2]*e1[2];
	P[0][1] = Av[0]*e1[0] + Av[1]*e1[1] + Av[2]*e1[2];
	P[1][0] = Au[0]*e2[0] + Au[1]*e2[1] + Au[2]*e2[2];
	P[1][1] = Av[0]*e2[0] + Av[1]*e2[1] + Av[2]*e2[2];
	t[0] = An[0]*e1[0] + An[1]*e1[1] + An[2]*e1[2];
	t[1] = An[0]*e2[0] + An[1]*e2[1] + An[2]*e2[2];
	double c = An[0]*w3[0] + An[1]*w3[1] + An[2]*w3[2];

	double detP = P[0][0]*P[1][1] - P[0][1]*P[1][0];
	double rdetP = 1.0 / detP;
	double M[2][2] = {
		{  P[1][1]*rdetP, -P[0][1]*rdetP },
		{ -P[1][0]*rdetP,  P[0][0]*rdetP }
	};

	// Original Monge height h(b1,b2), truncated to degree 3.
	Poly3 h;
	h.c[2][0] = -0.5 * k1;
	h.c[0][2] = -0.5 * k2;
	if (have_dcurv) {
		h.c[3][0] = -d[0] / 6.0;
		h.c[2][1] = -0.5 * d[1];
		h.c[1][2] = -0.5 * d[2];
		h.c[0][3] = -d[3] / 6.0;
	}

	// b(q) = M q + b2(q) + b3(q): invert the tangent-coordinate map
	// order by order.
	Poly3 lin_b1, lin_b2;
	lin_b1.c[1][0] = M[0][0];
	lin_b1.c[0][1] = M[0][1];
	lin_b2.c[1][0] = M[1][0];
	lin_b2.c[0][1] = M[1][1];
	Poly3 hM = poly_subst(h, lin_b1, lin_b2);

	Poly3 h2M, h3M;
	h2M.c[2][0] = hM.c[2][0];
	h2M.c[1][1] = hM.c[1][1];
	h2M.c[0][2] = hM.c[0][2];
	h3M.c[3][0] = hM.c[3][0];
	h3M.c[2][1] = hM.c[2][1];
	h3M.c[1][2] = hM.c[1][2];
	h3M.c[0][3] = hM.c[0][3];

	Poly3 b2[2], b3[2];
	for (int i = 0; i < 2; i++)
		poly_add_scaled(b2[i], -M[i][0]*t[0] - M[i][1]*t[1], h2M);

	// Gradient (in q) of the quadratic part of h(M q)
	Poly3 g1, g2;
	g1.c[1][0] = 2.0 * h2M.c[2][0];
	g1.c[0][1] = h2M.c[1][1];
	g2.c[1][0] = h2M.c[1][1];
	g2.c[0][1] = 2.0 * h2M.c[0][2];
	Poly3 dot3 = poly_mul(g1, b2[0]);
	poly_add_scaled(dot3, 1.0, poly_mul(g2, b2[1]));
	Poly3 corr = h3M;
	poly_add_scaled(corr, 1.0, dot3);
	for (int i = 0; i < 2; i++)
		poly_add_scaled(b3[i], -M[i][0]*t[0] - M[i][1]*t[1], corr);

	Poly3 b1full = lin_b1, b2full = lin_b2;
	poly_add_scaled(b1full, 1.0, b2[0]);
	poly_add_scaled(b1full, 1.0, b3[0]);
	poly_add_scaled(b2full, 1.0, b2[1]);
	poly_add_scaled(b2full, 1.0, b3[1]);

	// Re-expand the same surface as a Monge patch in the new frame.
	Poly3 hp = poly_subst(h, b1full, b2full);
	for (int i = 0; i <= 3; i++)
		for (int j = 0; i + j <= 3; j++)
			hp.c[i][j] *= c;

	// Second fundamental form in the new frame: h = -1/2 H q^2 + ...
	double Hm[2][2] = {
		{ -2.0 * hp.c[2][0], -hp.c[1][1] },
		{ -hp.c[1][1], -2.0 * hp.c[0][2] }
	};

	double l1, l2, evx, evy;
	eigen2_sym(Hm[0][0], Hm[0][1], Hm[1][1], l1, l2, evx, evy);
	new_k1 = float(l1);
	new_k2 = float(l2);

	double pu[3] = {
		evx*e1[0] + evy*e2[0],
		evx*e1[1] + evy*e2[1],
		evx*e1[2] + evy*e2[2]
	};
	double pv[3] = {
		w3[1]*pu[2] - w3[2]*pu[1],
		w3[2]*pu[0] - w3[0]*pu[2],
		w3[0]*pu[1] - w3[1]*pu[0]
	};
	new_n = vec(float(w3[0]), float(w3[1]), float(w3[2]));
	new_u = vec(float(pu[0]), float(pu[1]), float(pu[2]));
	new_v = vec(float(pv[0]), float(pv[1]), float(pv[2]));

	if (!have_dcurv)
		return;

	// Symmetric third-order tensor in the (e1,e2) frame from the
	// cubic part of h = -1/6 S_{ijk} q_i q_j q_k.
	double S[2][2][2];
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 2; j++)
			for (int k = 0; k < 2; k++)
				S[i][j][k] = 0.0;
	S[0][0][0] = -6.0 * hp.c[3][0];
	S[0][0][1] = S[0][1][0] = S[1][0][0] = -2.0 * hp.c[2][1];
	S[0][1][1] = S[1][0][1] = S[1][1][0] = -2.0 * hp.c[1][2];
	S[1][1][1] = -6.0 * hp.c[0][3];

	// Rotate the tensor into the principal frame.  Rows of R are the
	// principal-direction coordinates in the (e1,e2) basis; the second
	// principal direction is w3 cross the first, so its coordinates are
	// (-evy, evx).
	double R[2][2] = { { evx, evy }, { -evy, evx } };
	double St[2][2][2];
	for (int a = 0; a < 2; a++)
	 for (int b = 0; b < 2; b++)
	  for (int g = 0; g < 2; g++) {
		double acc = 0.0;
		for (int i = 0; i < 2; i++)
		 for (int j = 0; j < 2; j++)
		  for (int k = 0; k < 2; k++)
			acc += S[i][j][k] * R[a][i] * R[b][j] * R[g][k];
		St[a][b][g] = acc;
	  }
	*new_d = Vec<4,float>(float(St[0][0][0]), float(St[0][0][1]),
	                      float(St[0][1][1]), float(St[1][1][1]));
}

} // anonymous namespace


bool apply_affine_differential(TriMesh *mesh, const xform &xf)
{
	if (!mesh)
		return false;

	// Reject projection matrices: the last row must be (0,0,0,1).
	if (xf[3] != 0.0 || xf[7] != 0.0 || xf[11] != 0.0 || xf[15] != 1.0)
		return false;

	// Reject anything non-finite.
	for (int i = 0; i < 16; i++)
		if (!std::isfinite(xf[i]))
			return false;

	double A[3][3];
	double trans[3];
	double smax = 0.0;
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			A[r][c] = xf(r, c);
			smax = std::max(smax, std::fabs(A[r][c]));
		}
		trans[r] = xf(r, 3);
	}
	if (smax == 0.0)
		return false;

	double detA = mat3_det(A);
	if (std::fabs(detA) <= 1.0e-12 * smax * smax * smax)
		return false;

	int nv = int(mesh->vertices.size());

	// Field consistency: the curvature arrays come all or nothing,
	// curvatures require normals, dcurv requires curvatures.
	size_t ncurv = mesh->curv1.size();
	if (mesh->curv2.size() != ncurv || mesh->pdir1.size() != ncurv ||
	    mesh->pdir2.size() != ncurv)
		return false;
	if (ncurv != 0 && ncurv != size_t(nv))
		return false;
	if (ncurv != 0 && mesh->normals.size() != size_t(nv))
		return false;
	if (!mesh->dcurv.empty()) {
		if (mesh->dcurv.size() != size_t(nv) || ncurv == 0)
			return false;
	}
	if (!mesh->normals.empty() && mesh->normals.size() != size_t(nv))
		return false;

	// All cached field values must be finite.
	for (size_t i = 0; i < mesh->normals.size(); i++)
		if (!finite_vec(mesh->normals[i]))
			return false;
	for (size_t i = 0; i < mesh->pdir1.size(); i++) {
		if (!finite_vec(mesh->pdir1[i]) || !finite_vec(mesh->pdir2[i]) ||
		    !std::isfinite(mesh->curv1[i]) || !std::isfinite(mesh->curv2[i]))
			return false;
	}
	for (size_t i = 0; i < mesh->dcurv.size(); i++)
		for (int j = 0; j < 4; j++)
			if (!std::isfinite(mesh->dcurv[i][j]))
				return false;

	// Transform the vertices into a temporary buffer first, so a
	// non-finite result leaves the mesh untouched.
	::std::vector<point> newverts;
	newverts.resize(mesh->vertices.size());
	for (int i = 0; i < nv; i++) {
		const point &p = mesh->vertices[i];
		double q[3];
		for (int r = 0; r < 3; r++) {
			q[r] = A[r][0]*p[0] + A[r][1]*p[1] +
			       A[r][2]*p[2] + trans[r];
			if (!std::isfinite(q[r]))
				return false;
		}
		newverts[i] = point(float(q[0]), float(q[1]), float(q[2]));
	}

	bool have_curv = (ncurv != 0);
	bool have_dcurv = !mesh->dcurv.empty();

	if (have_curv) {
		::std::vector<vec> new_n(nv), new_u(nv), new_v(nv);
		::std::vector<float> new_k1(nv), new_k2(nv);
		::std::vector< Vec<4,float> > new_d;
		if (have_dcurv)
			new_d.resize(nv);
		const Vec<4,float> empty_d;
		for (int i = 0; i < nv; i++) {
			transport_vertex(A, detA,
				mesh->normals[i], mesh->pdir1[i], mesh->pdir2[i],
				mesh->curv1[i], mesh->curv2[i],
				have_dcurv ? mesh->dcurv[i] : empty_d,
				new_n[i], new_u[i], new_v[i], new_k1[i], new_k2[i],
				have_dcurv ? &new_d[i] : 0,
				have_dcurv);
		}
		mesh->vertices.swap(newverts);
		mesh->normals.swap(new_n);
		mesh->pdir1.swap(new_u);
		mesh->pdir2.swap(new_v);
		mesh->curv1.swap(new_k1);
		mesh->curv2.swap(new_k2);
		if (have_dcurv)
			mesh->dcurv.swap(new_d);
	} else {
		mesh->vertices.swap(newverts);
		if (!mesh->normals.empty()) {
			double invA[3][3];
			mat3_inv(A, invA);
			double B[3][3];
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					B[r][c] = invA[c][r];
			double sgn = (detA < 0.0) ? -1.0 : 1.0;
			for (int i = 0; i < nv; i++) {
				double n[3] = { mesh->normals[i][0],
				                mesh->normals[i][1],
				                mesh->normals[i][2] };
				double bn[3];
				mat3_vec(B, n, bn);
				double l = std::sqrt(bn[0]*bn[0] + bn[1]*bn[1] +
				                     bn[2]*bn[2]);
				for (int r = 0; r < 3; r++)
					mesh->normals[i][r] = float(sgn * bn[r] / l);
			}
		}
	}

	// Recompute cached mixed-Voronoi areas from the actual transformed
	// triangles; never create the cache if it wasn't there already.
	if (!mesh->pointareas.empty() || !mesh->cornerareas.empty()) {
		clear_and_release(mesh->pointareas);
		clear_and_release(mesh->cornerareas);
		mesh->need_pointareas();
	}

	// Keep cached bounding structures usable, leave the rest lazy.
	if (mesh->bbox.valid) {
		mesh->bbox.valid = false;
		mesh->need_bbox();
	}
	if (mesh->bsphere.valid) {
		mesh->bsphere.valid = false;
		mesh->need_bsphere();
	}

	return true;
}

} // namespace trimesh
