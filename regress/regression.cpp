/*
	File: regression.cpp
	Coordinate-chain regression contracts for the MC33 library.

	Four entry points with no arguments (the fixed driver in driver.cpp calls
	them in order):
		contract_analytic, contract_ambiguous, contract_affine, contract_storage
	Each returns the number of positive assertions actually executed, or a
	negative number after printing the input, the actual value and the expected
	value. Returning 0 means no check ran and is never treated as success.

	Everything is C++11 plus the standard library. All reference geometry is
	derived here from axis lengths and pairwise axis angles (or directly from
	the scalar functions); the library matrices are only used for extra
	self-consistency checks, never as the independent reference.
*/

#include "include/MC33.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

const double PI = std::acos(-1.0);
const double POS_TOL = 1e-5;    // positions and scalar values, small well-conditioned inputs
const double NRM_TOL = 2e-4;    // unit normal direction

struct V3 {
	double v[3];
	V3() { v[0] = v[1] = v[2] = 0.0; }
	V3(double a, double b, double c) { v[0] = a; v[1] = b; v[2] = c; }
	double& operator[](int i) { return v[i]; }
	double operator[](int i) const { return v[i]; }
};
V3 operator+(const V3& a, const V3& b) { return V3(a[0]+b[0], a[1]+b[1], a[2]+b[2]); }
V3 operator-(const V3& a, const V3& b) { return V3(a[0]-b[0], a[1]-b[1], a[2]-b[2]); }
V3 operator*(const V3& a, double s) { return V3(a[0]*s, a[1]*s, a[2]*s); }
double dot(const V3& a, const V3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
double norm(const V3& a) { return std::sqrt(dot(a, a)); }

// Independent lattice basis built only from axis lengths d[] and the three
// pairwise axis angles bc, ca, ab (degrees). Columns are the lattice axes:
//   a = d0 (1,0,0);  b = d1 (cos ab, sin ab, 0);
//   c fixed by b.c = d1 d2 cos(ca) and a.c = d0 d2 cos(cb).
void latticeBasis(const double d[3], float ang[3], V3 M[3]) {
	double ca = std::cos(ang[0]*PI/180.0);
	double cb = std::cos(ang[1]*PI/180.0);
	double cg = std::cos(ang[2]*PI/180.0);
	double sg = std::sin(ang[2]*PI/180.0);
	double ax = d[0];
	double bx = d[1]*cg, by = d[1]*sg;
	double cx = d[2]*cb;
	double cy = d[2]*(ca - cb*cg)/sg;
	double cz = d[2]*std::sqrt(1.0 - cb*cb - std::pow((ca - cb*cg)/sg, 2.0));
	M[0] = V3(ax, 0.0, 0.0);
	M[1] = V3(bx, by, 0.0);
	M[2] = V3(cx, cy, cz);
}
V3 matVec(const V3 M[3], const V3& u) { return M[0]*u[0] + M[1]*u[1] + M[2]*u[2]; }
// Inverse of a matrix whose columns are M[0],M[1],M[2].
void invertBasis(const V3 M[3], V3 Minv[3]) {
	double a[3][3];
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) a[i][j] = M[j][i];
	double c00 = a[1][1]*a[2][2]-a[1][2]*a[2][1];
	double c01 = a[1][2]*a[2][0]-a[1][0]*a[2][2];
	double c02 = a[1][0]*a[2][1]-a[1][1]*a[2][0];
	double det = a[0][0]*c00+a[0][1]*c01+a[0][2]*c02;
	double iv = 1.0/det;
	double inv[3][3];
	inv[0][0] = c00*iv;
	inv[0][1] = (a[0][2]*a[2][1]-a[0][1]*a[2][2])*iv;
	inv[0][2] = (a[0][1]*a[1][2]-a[0][2]*a[1][1])*iv;
	inv[1][0] = c01*iv;
	inv[1][1] = (a[0][0]*a[2][2]-a[0][2]*a[2][0])*iv;
	inv[1][2] = (a[0][2]*a[1][0]-a[0][0]*a[1][2])*iv;
	inv[2][0] = c02*iv;
	inv[2][1] = (a[0][1]*a[2][0]-a[0][0]*a[2][1])*iv;
	inv[2][2] = (a[0][0]*a[1][1]-a[0][1]*a[1][0])*iv;
	for (int j = 0; j < 3; j++)
		Minv[j] = V3(inv[0][j], inv[1][j], inv[2][j]); // columns of inverse
}
V3 invVec(const V3 Minv[3], const V3& r) { return Minv[0]*r[0] + Minv[1]*r[1] + Minv[2]*r[2]; }

struct Checker {
	const char* group;
	long count;
	bool failed;
	Checker(const char* g) : group(g), count(0), failed(false) {}
	void fail(const char* what, const char* ctx, double got, double want) {
		failed = true;
		std::printf("FAIL %s %s | input: %s | actual=%.9g expected=%.9g\n",
			group, what, ctx, got, want);
	}
	void close(const char* what, const char* ctx, double got, double want, double tol) {
		++count;
		if (std::fabs(got-want) > tol) fail(what, ctx, got, want);
	}
	void yes(const char* what, const char* ctx, bool cond) {
		++count;
		if (!cond) {
			failed = true;
			std::printf("FAIL %s %s | input: %s | actual=false expected=true\n", group, what, ctx);
		}
	}
	int result() const { return failed ? (count ? -(int)count : -1) : (int)count; }
};

void generated(MC33& mc, surface& s, MC33_real iso, const char* tag) {
	int rc = mc.calculate_isosurface(s, iso);
	std::printf("GENERATED %s rc=%d vertices=%u triangles=%u\n",
		tag, rc, s.get_num_vertices(), s.get_num_triangles());
}

// Build a grid and fill it with a linear index-space field g.u + h.
grid3d makeLinearGrid(unsigned n, const double d[3], const V3& r0, float ang[3],
                      const V3& g, double h) {
	grid3d G;
	G.set_grid_dimensions(n, n, n);
	G.set_ratio_aspect(d[0], d[1], d[2]);
	G.set_r0(r0[0], r0[1], r0[2]);
	G.set_Ang(ang[0], ang[1], ang[2]);
	for (unsigned k = 0; k < n; k++)
		for (unsigned j = 0; j < n; j++)
			for (unsigned i = 0; i < n; i++) {
				double val = g[0]*i + g[1]*j + g[2]*k + h;
				G.set_grid_value(i, j, k, (GRD_data_type)val);
			}
	return G;
}

// ---------------------------------------------------------------------------
// Analytic contract: edge roots and expected positions derived independently
// from the sampled scalar field on the inclined lattice.
// ---------------------------------------------------------------------------
struct EdgeKey {
	int axis;          // 0,1,2: index coordinate that varies
	int f[2];          // fixed indices of the other two coordinates
	int seg;           // segment index along the varying axis (cell index)
	bool operator<(const EdgeKey& o) const {
		if (axis != o.axis) return axis < o.axis;
		if (f[0] != o.f[0]) return f[0] < o.f[0];
		if (f[1] != o.f[1]) return f[1] < o.f[1];
		return seg < o.seg;
	}
};

// Independent 1-D root of g.u + h = iso on every crossed grid edge.
// Returns map edge -> root position in index coordinates.
std::map<EdgeKey, V3> analyticEdgeRoots(unsigned n, const V3& g, double h, double iso) {
	std::map<EdgeKey, V3> roots;
	int fixedOrder[3][2] = {{1, 2}, {0, 2}, {0, 1}};
	for (int axis = 0; axis < 3; axis++) {
		int fa = fixedOrder[axis][0], fb = fixedOrder[axis][1];
		for (unsigned pa = 0; pa < n; pa++)
			for (unsigned pb = 0; pb < n; pb++)
				for (unsigned seg = 0; seg + 1 < n; seg++) {
					V3 u0, u1;
					u0[fa] = pa; u0[fb] = pb; u0[axis] = seg;
					u1 = u0; u1[axis] = seg + 1;
					double F0 = dot(g, u0) + h - iso;
					double F1 = dot(g, u1) + h - iso;
					if (F0*F1 < 0.0) {            // strict sign change, no endpoint on surface
						double t = F0/(F0 - F1);
						V3 r = u0 + (u1 - u0)*t;
						EdgeKey key; key.axis = axis; key.f[0] = pa; key.f[1] = pb; key.seg = seg;
						roots[key] = r;
					}
				}
	}
	return roots;
}

int checkLinearEdgeRoots(Checker& ck, const char* tag, const double d[3], const V3& r0,
                         float ang[3], const V3& g, double h, double iso) {
	const unsigned n = 3;
	grid3d G = makeLinearGrid(n, d, r0, ang, g, h);
	MC33 mc; mc.set_grid3d(G);
	surface s; generated(mc, s, (MC33_real)iso, tag);

	V3 M[3], Minv[3];
	latticeBasis(d, ang, M);
	invertBasis(M, Minv);

	std::map<EdgeKey, V3> want = analyticEdgeRoots(n, g, h, iso);
	ck.yes("analytic: nonempty surface", tag, s.get_num_vertices() > 0 && s.get_num_triangles() > 0);
	ck.close("analytic: vertex count equals number of crossed edges", tag,
		(double)s.get_num_vertices(), (double)want.size(), 0.0);

	std::map<EdgeKey, int> seen;
	int fixedOrder[3][2] = {{1, 2}, {0, 2}, {0, 1}};
	for (unsigned q = 0; q < s.get_num_vertices(); q++) {
		const MC33_real* vp = s.getVertex(q);
		V3 world(vp[0], vp[1], vp[2]);
		V3 idx = invVec(Minv, world - r0);
		// identify the edge: two index coordinates sit on integers and one
		// (the varying axis) is fractional. Use distance to the nearest integer
		// so tiny round-off on integer coordinates cannot flip the choice.
		int axis = 0; double distBest = -1.0;
		for (int a = 0; a < 3; a++) {
			double dist = std::fabs(idx[a] - (double)std::lround(idx[a]));
			if (dist > distBest) { distBest = dist; axis = a; }
		}
		int fa = fixedOrder[axis][0], fb = fixedOrder[axis][1];
		EdgeKey key;
		key.axis = axis;
		key.f[0] = (int)std::lround(idx[fa]);
		key.f[1] = (int)std::lround(idx[fb]);
		key.seg = (int)std::floor(idx[axis]);
		if (key.seg < 0) key.seg = 0;
		if (key.seg > (int)n - 2) key.seg = (int)n - 2;
		char ctx[128];
		std::snprintf(ctx, sizeof ctx, "%s vertex %u edge(axis=%d)", tag, q, axis);
		auto it = want.find(key);
		ck.yes("analytic: vertex lies on a crossed edge", ctx, it != want.end());
		if (it != want.end()) {
			seen[key]++;
			V3 wantWorld = r0 + matVec(M, it->second);
			ck.close("analytic: edge root index coordinate", ctx, idx[axis], it->second[axis], POS_TOL);
			ck.close("analytic: world position x", ctx, world[0], wantWorld[0], POS_TOL);
			ck.close("analytic: world position y", ctx, world[1], wantWorld[1], POS_TOL);
			ck.close("analytic: world position z", ctx, world[2], wantWorld[2], POS_TOL);
			ck.close("analytic: plane residual", ctx, dot(g, idx) + h, iso, POS_TOL);
		}
	}
	for (const auto& kv : want) {
		char ctx[128];
		std::snprintf(ctx, sizeof ctx, "%s edge axis=%d f=(%d,%d) seg=%d",
			tag, kv.first.axis, kv.first.f[0], kv.first.f[1], kv.first.seg);
		ck.close("analytic: exactly one vertex per crossed edge", ctx,
			(double)seen.count(kv.first) ? (double)seen[kv.first] : 0.0, 1.0, 0.0);
	}
	// The two public counting entry points must agree.
	unsigned nV = 0, nT = 0;
	mc.size_of_isosurface((MC33_real)iso, nV, nT);
	ck.close("analytic: size_of vertex count agrees", tag, nV, s.get_num_vertices(), 0.0);
	ck.close("analytic: size_of triangle count agrees", tag, nT, s.get_num_triangles(), 0.0);
	return 0;
}

} // namespace

int contract_analytic(void) {
	Checker ck("analytic");
	const double d[3] = {2.0, 3.0, 5.0};
	const V3 r0(1.0, -2.0, 3.0);
	float ang[3] = {80.0f, 100.0f, 70.0f};

	// General inclined plane: F = i + 2j + 3k, iso 2.5.
	checkLinearEdgeRoots(ck, "general-oblique-plane", d, r0, ang, V3(1, 2, 3), 0.0, 2.5);
	// Axis-aligned (in index space) plane: F = i, iso 1.5.
	checkLinearEdgeRoots(ck, "axis-plane", d, r0, ang, V3(1, 0, 0), 0.0, 1.5);

	// Isosurface passing through an ordinary grid point F(1,0,1) = 4.
	{
		const unsigned n = 3;
		grid3d G = makeLinearGrid(n, d, r0, ang, V3(1, 2, 3), 0.0);
		MC33 mc; mc.set_grid3d(G);
		surface s; generated(mc, s, 4.0, "through-grid-point");
		V3 M[3]; latticeBasis(d, ang, M);
		V3 gpWorld = r0 + matVec(M, V3(1, 0, 1));
		ck.yes("gridpoint: nonempty surface", "through-grid-point",
			s.get_num_vertices() > 0 && s.get_num_triangles() > 0);
		bool hit = false;
		for (unsigned q = 0; q < s.get_num_vertices(); q++) {
			const MC33_real* vp = s.getVertex(q);
			if (norm(V3(vp[0], vp[1], vp[2]) - gpWorld) < POS_TOL) hit = true;
		}
		ck.yes("gridpoint: a vertex coincides with the grid point", "through-grid-point", hit);
	}

	// Empty surface: every sample is on the same side of the isovalue.
	{
		grid3d G = makeLinearGrid(3, d, r0, ang, V3(1, 2, 3), 0.0);
		MC33 mc; mc.set_grid3d(G);
		surface s; generated(mc, s, 100.0, "empty-side");
		ck.close("empty: zero vertices", "empty-side", s.get_num_vertices(), 0.0, 0.0);
		ck.close("empty: zero triangles", "empty-side", s.get_num_triangles(), 0.0, 0.0);
		unsigned nV = 7, nT = 7;
		mc.size_of_isosurface(100.0, nV, nT);
		ck.close("empty: size_of reports zero vertices", "empty-side", nV, 0.0, 0.0);
		ck.close("empty: size_of reports zero triangles", "empty-side", nT, 0.0, 0.0);
	}
	return ck.result();
}

namespace {

// ---------------------------------------------------------------------------
// Affine contract: world-space gradient from an independent basis, positions,
// negative unit normals, trilinear interpolation and re-binding.
// ---------------------------------------------------------------------------
void affineConfig(Checker& ck, const char* tag, const double d[3], const V3& r0, float ang[3],
                  const V3& g, double h, double iso) {
	const unsigned n = 4;
	grid3d G = makeLinearGrid(n, d, r0, ang, g, h);
	MC33 mc; mc.set_grid3d(G);
	surface s; generated(mc, s, (MC33_real)iso, tag);

	V3 M[3], Minv[3];
	latticeBasis(d, ang, M);
	invertBasis(M, Minv);

	// World-space gradient of g.u + h with world = r0 + M u is gw = M^{-T} g.
	// Minv[j] is column j of M^-1, so component j of M^{-T}g is dot(Minv[j], g).
	V3 gw(dot(Minv[0], g), dot(Minv[1], g), dot(Minv[2], g));
	double gl = norm(gw);
	V3 wantNormal = gw * (-1.0/gl);     // default orientation: negative gradient

	ck.yes("affine: nonempty surface", tag, s.get_num_vertices() > 0);
	for (unsigned q = 0; q < s.get_num_vertices(); q++) {
		const MC33_real* vp = s.getVertex(q);
		const float* np = s.getNormal(q);
		V3 world(vp[0], vp[1], vp[2]);
		V3 idx = invVec(Minv, world - r0);
		char ctx[128];
		std::snprintf(ctx, sizeof ctx, "%s vertex %u", tag, q);
		ck.close("affine: plane position residual", ctx, dot(g, idx) + h, iso, POS_TOL);
		V3 nrm(np[0], np[1], np[2]);
		ck.close("affine: normal is unit length", ctx, norm(nrm), 1.0, NRM_TOL);
		ck.close("affine: negative-gradient normal direction", ctx,
			norm(nrm - wantNormal), 0.0, NRM_TOL);
	}

	// Trilinear interpolation at non-integer interior index points.
	ck.yes("affine: trilinear enabled", tag, G.set_interpolation(1) == 0);
	const V3 probes[3] = {V3(0.4, 0.7, 0.3), V3(1.3, 0.2, 2.6), V3(2.5, 1.5, 0.9)};
	for (int p = 0; p < 3; p++) {
		V3 u = probes[p];
		V3 w = r0 + matVec(M, u);
		double got = G.interpolated_value(w[0], w[1], w[2]);
		double want = dot(g, u) + h;
		char ctx[128];
		std::snprintf(ctx, sizeof ctx, "%s trilinear u=(%.2f,%.2f,%.2f)", tag, u[0], u[1], u[2]);
		ck.close("affine: world-coordinate trilinear value", ctx, got, want, POS_TOL);
	}

	// Extra self-consistency only: the library's own matrices must be mutual
	// inverses (not used as the independent reference above).
	const double (*libA)[3] = G.get__A();
	const double (*libAi)[3] = G.get_A_();
	double resid = 0.0;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double e = (i == j ? 1.0 : 0.0);
			double sum = 0.0;
			for (int k = 0; k < 3; k++) sum += libAi[i][k]*libA[k][j];
			resid = std::fmax(resid, std::fabs(sum - e));
		}
	ck.close("affine: library A_ inverts _A (self-check)", tag, resid, 0.0, 1e-10);
}

} // namespace

int contract_affine(void) {
	Checker ck("affine");
	// Configuration 1: the reported inclined lattice.
	{
		const double d[3] = {2.0, 3.0, 5.0};
		const V3 r0(1.0, -2.0, 3.0);
		float ang[3] = {80.0f, 100.0f, 70.0f};
		affineConfig(ck, "config-one", d, r0, ang, V3(1, 2, 3), 0.0, 2.5);
	}
	// Configuration 2: different unequal legal angles, anisotropic scale, shift.
	{
		const double d[3] = {1.5, 2.5, 4.0};
		const V3 r0(-3.0, 2.0, -1.0);
		float ang[3] = {70.0f, 110.0f, 80.0f};
		affineConfig(ck, "config-two", d, r0, ang, V3(2, -1, 3), 1.0, 3.2);
	}

	// Re-binding: the same grid3d goes back to orthogonal with a new scale and
	// origin. After set_grid3d again, no previous inclined transform may remain.
	{
		const char* tag = "rebind-orthogonal";
		grid3d G = makeLinearGrid(3, (const double[3]){2.0, 3.0, 5.0}, V3(1, -2, 3),
		                          (float[3]){80, 100, 70}, V3(1, 2, 3), 0.0);
		MC33 mc; mc.set_grid3d(G);
		// Rebuild the SAME object as an orthogonal grid.
		G.set_grid_dimensions(3, 3, 3);
		const double nd[3] = {1.5, 1.5, 1.5};
		const V3 nr0(5.0, -7.0, 2.0);
		G.set_ratio_aspect(nd[0], nd[1], nd[2]);
		G.set_r0(nr0[0], nr0[1], nr0[2]);   // angles stay 90 after set_grid_dimensions
		const V3 g(1, 2, 3);
		for (unsigned k = 0; k < 3; k++)
			for (unsigned j = 0; j < 3; j++)
				for (unsigned i = 0; i < 3; i++)
					G.set_grid_value(i, j, k, (GRD_data_type)(g[0]*i + g[1]*j + g[2]*k));
		mc.set_grid3d(G);

		surface empty; generated(mc, empty, 999.0, "rebind-empty");
		ck.close("rebind: empty has no vertices", tag, empty.get_num_vertices(), 0.0, 0.0);
		ck.close("rebind: empty has no triangles", tag, empty.get_num_triangles(), 0.0, 0.0);

		surface s; generated(mc, s, 2.5, tag);
		ck.yes("rebind: nonempty surface", tag, s.get_num_vertices() > 0);
		// Orthogonal mapping: index = (world - r0) / d componentwise.
		V3 wantNormal = V3(g[0]/nd[0], g[1]/nd[1], g[2]/nd[2]);
		wantNormal = wantNormal * (-1.0/norm(wantNormal));
		for (unsigned q = 0; q < s.get_num_vertices(); q++) {
			const MC33_real* vp = s.getVertex(q);
			const float* np = s.getNormal(q);
			V3 world(vp[0], vp[1], vp[2]);
			V3 idx((world[0]-nr0[0])/nd[0], (world[1]-nr0[1])/nd[1], (world[2]-nr0[2])/nd[2]);
			char ctx[96];
			std::snprintf(ctx, sizeof ctx, "%s vertex %u", tag, q);
			ck.close("rebind: orthogonal position residual", ctx, dot(g, idx), 2.5, POS_TOL);
			ck.close("rebind: orthogonal negative-gradient normal", ctx,
				norm(V3(np[0], np[1], np[2]) - wantNormal), 0.0, NRM_TOL);
		}
		// Interpolation must use the new orthogonal geometry as well.
		G.set_interpolation(1);
		V3 u(0.4, 0.7, 0.3);
		V3 w = nr0 + V3(nd[0]*u[0], nd[1]*u[1], nd[2]*u[2]);
		double got = G.interpolated_value(w[0], w[1], w[2]);
		ck.close("rebind: orthogonal trilinear value", tag, got, dot(g, u), POS_TOL);
	}
	return ck.result();
}

namespace {

// ---------------------------------------------------------------------------
// Ambiguous contract: F = (u-a)(v-b) - c extruded along the third axis.
// Pairing is decided independently from the 2-D roots and the saddle value.
// ---------------------------------------------------------------------------
const int AMB_N = 4;                 // 4 points, 3 cells along each axis
const double AMB_A = 0.35;
const double AMB_B = 0.62;

double ambF(double u, double v, double c) { return (u-AMB_A)*(v-AMB_B) - c; }

// Canonical (u,v,w) of a physical point for a given symmetry mode.
//   mode 0 identity, 1 cyclic axis rotation, 2 reverse u, 3 global negation
void toCanon(int mode, double x, double y, double z, V3& q) {
	if (mode == 1)      q = V3(z, x, y);
	else if (mode == 2) q = V3(AMB_N-1-x, y, z);
	else                q = V3(x, y, z);
}
double ambScalar(int mode, int i, int j, int k, double c) {
	V3 q; toCanon(mode, i, j, k, q);
	double val = ambF(q[0], q[1], c);
	if (mode == 3) val = -val;
	return val;
}
struct AEdge {
	int dir;          // 0 = u-directed, 1 = v-directed
	int fixed, layer, seg;
	double root;      // varying canonical coordinate of the root
	bool operator<(const AEdge& o) const {
		if (dir != o.dir) return dir < o.dir;
		if (fixed != o.fixed) return fixed < o.fixed;
		if (layer != o.layer) return layer < o.layer;
		return seg < o.seg;
	}
};
std::map<AEdge, double> ambRoots(double c) {
	std::map<AEdge, double> out;
	// u-directed edges: v fixed at integer j, root u = a + c/(j-b).
	for (int j = 0; j < AMB_N; j++)
		for (int layer = 0; layer < AMB_N; layer++) {
			double u = AMB_A + c/(j-AMB_B);
			if (u > 1e-9 && u < AMB_N-1-1e-9) {
				AEdge e; e.dir = 0; e.fixed = j; e.layer = layer; e.seg = (int)std::floor(u); e.root = u;
				out[e] = u;
			}
		}
	// v-directed edges: u fixed at integer i, root v = b + c/(i-a).
	for (int i = 0; i < AMB_N; i++)
		for (int layer = 0; layer < AMB_N; layer++) {
			double v = AMB_B + c/(i-AMB_A);
			if (v > 1e-9 && v < AMB_N-1-1e-9) {
				AEdge e; e.dir = 1; e.fixed = i; e.layer = layer; e.seg = (int)std::floor(v); e.root = v;
				out[e] = v;
			}
		}
	return out;
}

void ambiguousMode(Checker& ck, int mode, double c) {
	char tag[64];
	std::snprintf(tag, sizeof tag, "ambiguous mode=%d c=%+g", mode, c);
	grid3d G; G.set_grid_dimensions(AMB_N, AMB_N, AMB_N);
	for (int k = 0; k < AMB_N; k++)
		for (int j = 0; j < AMB_N; j++)
			for (int i = 0; i < AMB_N; i++)
				G.set_grid_value(i, j, k, (GRD_data_type)ambScalar(mode, i, j, k, c));
	MC33 mc; mc.set_grid3d(G);
	surface s; generated(mc, s, 0.0, tag);
	ck.yes("ambiguous: nonempty triangulation", tag,
		s.get_num_vertices() > 0 && s.get_num_triangles() > 0);

	std::map<AEdge, double> want = ambRoots(c);
	ck.close("ambiguous: vertex count equals crossed edges", tag,
		(double)s.get_num_vertices(), (double)want.size(), 0.0);

	// Connected components recovered from the output triangle graph.
	std::vector<int> par(s.get_num_vertices());
	for (size_t i = 0; i < par.size(); i++) par[i] = (int)i;
	std::function<int(int)> find = [&](int x) {
		return par[x] == x ? x : (par[x] = find(par[x]));
	};
	auto unite = [&](int a, int b) { a = find(a); b = find(b); if (a != b) par[a] = b; };
	for (unsigned t = 0; t < s.get_num_triangles(); t++) {
		const unsigned* tr = s.getTriangle(t);
		unite(tr[0], tr[1]); unite(tr[1], tr[2]); unite(tr[0], tr[2]);
	}

	std::map<AEdge, int> identities;
	for (unsigned q = 0; q < s.get_num_vertices(); q++) {
		const MC33_real* vp = s.getVertex(q);
		V3 canon; toCanon(mode, vp[0], vp[1], vp[2], canon);
		double fu = std::fabs(canon[0]-std::floor(canon[0]));
		double fv = std::fabs(canon[1]-std::floor(canon[1]));
		AEdge key;
		char ctx[128];
		if (fv < fu) {           // v is the integer coordinate -> u-directed edge
			key.dir = 0; key.fixed = (int)std::lround(canon[1]);
			key.layer = (int)std::lround(canon[2]); key.seg = (int)std::floor(canon[0]);
		} else {
			key.dir = 1; key.fixed = (int)std::lround(canon[0]);
			key.layer = (int)std::lround(canon[2]); key.seg = (int)std::floor(canon[1]);
		}
		std::snprintf(ctx, sizeof ctx, "%s vertex %u", tag, q);
		auto it = want.find(key);
		ck.yes("ambiguous: vertex sits on a crossed edge", ctx, it != want.end());
		if (it != want.end()) {
			identities[key]++;
			double got = (key.dir == 0 ? canon[0] : canon[1]);
			ck.close("ambiguous: independent 2-D root coordinate", ctx, got, it->second, POS_TOL);
		}
	}
	// One and only one vertex identity per physical grid edge.
	for (const auto& kv : want) {
		char ctx[128];
		std::snprintf(ctx, sizeof ctx, "%s edge dir=%d fixed=%d layer=%d",
			tag, kv.first.dir, kv.first.fixed, kv.first.layer);
		ck.close("ambiguous: single identity per edge", ctx,
			identities.count(kv.first) ? (double)identities[kv.first] : 0.0, 1.0, 0.0);
	}

	// Saddle cell (0,0), canonical layer 0: four roots and the pairing decided
	// from the corner signs and the saddle value f(a,b) = -c.
	double u0 = AMB_A + c/(0.0-AMB_B);     // u root on edge v=0 (between corners (0,0),(1,0))
	double u1 = AMB_A + c/(1.0-AMB_B);     // u root on edge v=1 (between (0,1),(1,1))
	double v0 = AMB_B + c/(0.0-AMB_A);     // v root on edge u=0 (between (0,0),(0,1))
	double v1 = AMB_B + c/(1.0-AMB_A);     // v root on edge u=1 (between (1,0),(1,1))
	double corner00 = ambF(0, 0, c), corner11 = ambF(1, 1, c);
	double saddle = -c;
	auto vertexAt = [&](int dir, int fixed, double varying) -> int {
		for (unsigned q = 0; q < s.get_num_vertices(); q++) {
			const MC33_real* vp = s.getVertex(q);
			V3 canon; toCanon(mode, vp[0], vp[1], vp[2], canon);
			if (dir == 0 && std::fabs(canon[1]-fixed) < 1e-6 && std::fabs(canon[2]) < 1e-6
			    && std::fabs(canon[0]-varying) < 1e-4) return (int)q;
			if (dir == 1 && std::fabs(canon[0]-fixed) < 1e-6 && std::fabs(canon[2]) < 1e-6
			    && std::fabs(canon[1]-varying) < 1e-4) return (int)q;
		}
		return -1;
	};
	int eU0 = vertexAt(0, 0, u0), eU1 = vertexAt(0, 1, u1);
	int eV0 = vertexAt(1, 0, v0), eV1 = vertexAt(1, 1, v1);
	ck.yes("ambiguous: four saddle-cell roots present", tag,
		eU0 >= 0 && eU1 >= 0 && eV0 >= 0 && eV1 >= 0);
	// The chosen interior points keep this a genuine ambiguous face: the two
	// diagonal corners share one sign and the other diagonal the opposite.
	double corner10 = ambF(1, 0, c), corner01 = ambF(0, 1, c);
	ck.yes("ambiguous: diagonal corners share signs (true ambiguous face)", tag,
		corner00*corner11 > 0.0 && corner10*corner01 > 0.0 && corner00*corner10 < 0.0);
	if (eU0 >= 0 && eU1 >= 0 && eV0 >= 0 && eV1 >= 0) {
		// If saddle and the (0,0)/(1,1) corners share a sign, those positive
		// (or negative) regions connect the two roots bordering each corner.
		bool cornerSign = corner00 > 0.0;
		bool saddleSame = (saddle > 0.0) == cornerSign;
		bool pair00, pair11;
		if (!saddleSame) {
			// Saddle has the opposite sign: each same-sign corner is an isolated
			// region, so the two roots bordering that same corner join.
			pair00 = find(eU0) == find(eV0);   // roots around corner (0,0)
			pair11 = find(eU1) == find(eV1);   // roots around corner (1,1)
		} else {
			// Saddle has the same sign: that region bridges the two corners,
			// giving the cross (diagonal) pairing.
			pair00 = find(eU0) == find(eV1);
			pair11 = find(eU1) == find(eV0);
		}
		char ctx[96];
		std::snprintf(ctx, sizeof ctx, "%s saddle=%+g corner00=%+g", tag, saddle, corner00);
		ck.yes("ambiguous: saddle pairing A", ctx, pair00);
		ck.yes("ambiguous: saddle pairing B", ctx, pair11);
		ck.yes("ambiguous: two distinct boundary components", tag,
			find(eU0) != find(eU1) && find(eV0) != find(eV1));
	}
	// Extrusion consistency: each (u,v) root repeats at every w layer with the
	// same canonical planar coordinates (shared-face intersection agreement).
	for (const auto& kv : want) {
		if (kv.first.layer != 0) continue;
		for (int layer = 1; layer < AMB_N; layer++) {
			AEdge other = kv.first; other.layer = layer;
			auto it2 = want.find(other);
			char ctx[128];
			std::snprintf(ctx, sizeof ctx, "%s shared face dir=%d fixed=%d layer=%d",
				tag, other.dir, other.fixed, layer);
			ck.yes("ambiguous: shared-face edge continues", ctx, it2 != want.end());
			if (it2 != want.end())
				ck.close("ambiguous: shared-face root coordinate", ctx, it2->second, kv.second, 0.0);
		}
	}
}

} // namespace

int contract_ambiguous(void) {
	Checker ck("ambiguous");
	const double cs[2] = {0.1, -0.1};       // both signs, away from critical values
	for (int mode = 0; mode < 4; mode++)
		for (int ci = 0; ci < 2; ci++)
			ambiguousMode(ck, mode, cs[ci]);
	return ck.result();
}

namespace {

// ---------------------------------------------------------------------------
// Storage contract: size accounting, binary round trip and no stale results.
// ---------------------------------------------------------------------------
std::string tempBinPath() {
	const char* dir = std::getenv("TMPDIR");
	std::string base = (dir && *dir) ? dir : ".";
	return base + "/mc33_regression_roundtrip.bin";
}

} // namespace

int contract_storage(void) {
	Checker ck("storage");
	const double d[3] = {2.0, 3.0, 5.0};
	const V3 r0(1.0, -2.0, 3.0);
	float ang[3] = {80.0f, 100.0f, 70.0f};
	grid3d G = makeLinearGrid(3, d, r0, ang, V3(1, 2, 3), 0.0);
	MC33 mc; mc.set_grid3d(G);

	// Alternate empty / non-empty processing, reusing objects: nothing may leak.
	surface work;
	generated(mc, work, 100.0, "storage-empty-1");
	ck.close("storage: first empty vertices", "storage-empty-1", work.get_num_vertices(), 0.0, 0.0);
	ck.close("storage: first empty triangles", "storage-empty-1", work.get_num_triangles(), 0.0, 0.0);

	generated(mc, work, 2.5, "storage-nonempty-1");
	unsigned nV = 0, nT = 0;
	std::size_t bytes = mc.size_of_isosurface(2.5, nV, nT);
	ck.yes("storage: nonzero reported size", "storage-nonempty-1", bytes > 0);
	ck.close("storage: size_of vertices", "storage-nonempty-1", nV, work.get_num_vertices(), 0.0);
	ck.close("storage: size_of triangles", "storage-nonempty-1", nT, work.get_num_triangles(), 0.0);

	// Reusing the same surface for an empty result must clear the old geometry.
	generated(mc, work, 100.0, "storage-empty-2");
	ck.close("storage: second empty vertices", "storage-empty-2", work.get_num_vertices(), 0.0, 0.0);
	ck.close("storage: second empty triangles", "storage-empty-2", work.get_num_triangles(), 0.0, 0.0);

	// Fresh non-empty surface for the binary round trip.
	surface src; generated(mc, src, 2.5, "storage-roundtrip-src");
	ck.yes("storage: roundtrip source nonempty", "storage-roundtrip-src",
		src.get_num_vertices() > 0 && src.get_num_triangles() > 0);
	unsigned srcV = src.get_num_vertices(), srcT = src.get_num_triangles();

	std::string path = tempBinPath();
	ck.yes("storage: save_bin ok", path.c_str(), src.save_bin(path.c_str()) == 0);
	surface dst;
	ck.yes("storage: read_bin ok", path.c_str(), dst.read_bin(path.c_str()) == 0);
	std::remove(path.c_str());

	ck.close("storage: isovalue round trip", "roundtrip", dst.get_isovalue(), src.get_isovalue(), 0.0);
	ck.close("storage: vertex count round trip", "roundtrip", dst.get_num_vertices(), srcV, 0.0);
	ck.close("storage: triangle count round trip", "roundtrip", dst.get_num_triangles(), srcT, 0.0);
	for (unsigned q = 0; q < srcV; q++) {
		const MC33_real* sv = src.getVertex(q);
		const MC33_real* dv = dst.getVertex(q);
		const float* sn = src.getNormal(q);
		const float* dn = dst.getNormal(q);
		char ctx[96];
		std::snprintf(ctx, sizeof ctx, "roundtrip vertex %u", q);
		for (int a = 0; a < 3; a++) {
			ck.close("storage: vertex coordinate round trip", ctx, dv[a], sv[a], 1e-6);
			ck.close("storage: normal component round trip", ctx, dn[a], sn[a], 1e-6);
		}
	}
	for (unsigned t = 0; t < srcT; t++) {
		const unsigned* st = src.getTriangle(t);
		const unsigned* dt = dst.getTriangle(t);
		char ctx[96];
		std::snprintf(ctx, sizeof ctx, "roundtrip triangle %u", t);
		for (int a = 0; a < 3; a++)
			ck.close("storage: triangle index round trip", ctx, dt[a], st[a], 0.0);
	}
	return ck.result();
}
