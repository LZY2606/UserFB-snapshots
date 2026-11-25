//=====================================================================
//
// tests/radiosity_target.c - target checks for the radiosity module
//
// Three case groups are registered with task_cases:
//   visibility  - a per-pixel ray/triangle reference re-derives the
//                 hemicube ID integrals and is compared to the built
//                 transfer matrix; scenes cross the near plane and the
//                 side planes, contain back-face occluders and two
//                 layers ordered front/back.
//   transport   - analytic areas and the matrix read back through the
//                 public API are used to re-run progressive shooting
//                 independently; area ratios, colour bleeding, shooter
//                 selection and remaining energy are checked.
//   integration - chunked vs. whole stepping, reset, two independent
//                 interleaved scenes and an exact comparison against
//                 the original device_draw_primitive pipeline.
//
// Every case returns 0 on success and uses TASK_CHECK, so the shared
// driver reports the number of checks performed.
//
//=====================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "mini3d.h"
#include "radiosity.h"
#include "task_support/check.h"

#define PI_D 3.14159265358979323846

//---------------------------------------------------------------------
// double precision vector helpers used by the independent reference
//---------------------------------------------------------------------
typedef struct { double x, y, z; } dvec;

static dvec dv(double x, double y, double z) {
	dvec v; v.x = x; v.y = y; v.z = z; return v;
}
static dvec dadd(dvec a, dvec b) { return dv(a.x+b.x, a.y+b.y, a.z+b.z); }
static dvec dsub(dvec a, dvec b) { return dv(a.x-b.x, a.y-b.y, a.z-b.z); }
static dvec dmul(dvec a, double s) { return dv(a.x*s, a.y*s, a.z*s); }
static double ddot(dvec a, dvec b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static dvec dcross(dvec a, dvec b) {
	return dv(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static double dlen(dvec a) { return sqrt(ddot(a, a)); }
static dvec dnorm(dvec a) {
	double l = dlen(a);
	return (l > 0.0) ? dmul(a, 1.0/l) : dv(0, 0, 0);
}
static dvec p2d(point_t p) { return dv(p.x, p.y, p.z); }

//---------------------------------------------------------------------
// patch helpers
//---------------------------------------------------------------------
static point_t pt(float x, float y, float z) {
	point_t p; p.x = x; p.y = y; p.z = z; p.w = 1.0f; return p;
}

static radiosity_patch patch3(point_t a, point_t b, point_t c,
		float er, float eg, float eb, float rr, float rg, float rb) {
	radiosity_patch p;
	p.v[0] = a; p.v[1] = b; p.v[2] = c;
	p.emission.r = er; p.emission.g = eg; p.emission.b = eb;
	p.reflectance.r = rr; p.reflectance.g = rg; p.reflectance.b = rb;
	return p;
}

static radiosity_patch black_patch(point_t a, point_t b, point_t c) {
	return patch3(a, b, c, 0, 0, 0, 0, 0, 0);
}

static void tri_frame(const radiosity_patch *p, dvec *n, dvec *cent,
		double *area) {
	dvec a = p2d(p->v[0]), b = p2d(p->v[1]), c = p2d(p->v[2]);
	dvec e1 = dsub(b, a), e2 = dsub(c, a);
	dvec cr = dcross(e1, e2);
	double l = dlen(cr);
	*n = (l > 0.0) ? dmul(cr, 1.0/l) : dv(0, 0, 0);
	*cent = dmul(dadd(dadd(a, b), c), 1.0/3.0);
	*area = l * 0.5;
}

// Moller-Trumbore, double precision.  Returns 1 when the ray hits the
// triangle at a non-negative parameter; *t receives the distance.
static int ray_tri(dvec o, dvec d, const point_t v[3], double *t) {
	dvec a = p2d(v[0]), b = p2d(v[1]), c = p2d(v[2]);
	dvec e1 = dsub(b, a), e2 = dsub(c, a);
	dvec pv = dcross(d, e2);
	double det = ddot(e1, pv);
	double inv, u, vv, lam;
	dvec tv, qv;
	if (fabs(det) < 1e-12) return 0;
	inv = 1.0 / det;
	tv = dsub(o, a);
	u = ddot(tv, pv) * inv;
	if (u < 0.0 || u > 1.0) return 0;
	qv = dcross(tv, e1);
	vv = ddot(d, qv) * inv;
	if (vv < 0.0 || u + vv > 1.0) return 0;
	lam = ddot(e2, qv) * inv;
	*t = lam;
	return 1;
}

// Common discrete hemicube weight total (top face plus four sides).
static double hemi_total_weight(int res) {
	double dx = 2.0 / (double)res;
	double sum = 0.0, side = 0.0;
	int px, py, half = res / 2;
	for (py = 0; py < res; py++) {
		double y = 1.0 - (2.0*py + 1.0) * dx * 0.5;
		for (px = 0; px < res; px++) {
			double x = -1.0 + (2.0*px + 1.0) * dx * 0.5;
			double d = 1.0 + x*x + y*y;
			sum += dx*dx / (PI_D * d * d);
		}
	}
	for (py = 0; py < half; py++) {
		double z = 1.0 - (2.0*py + 1.0) * dx * 0.5;
		for (px = 0; px < res; px++) {
			double x = -1.0 + (2.0*px + 1.0) * dx * 0.5;
			double d = 1.0 + x*x + z*z;
			side += dx*dx * z / (PI_D * d * d);
		}
	}
	return sum + 4.0 * side;
}

// Reference transfer row by per-pixel ray casting.  `acc` receives the
// normalized F row.  When `face` is non-NULL it is treated as a
// count*5 matrix and receives each receiver's contribution per hemicube
// face (so a test can prove which face sees which receiver).
static void ref_row(const radiosity_patch *ps, size_t count, size_t src,
		int res, float offset, double *acc, double *face) {
	dvec n, cent, aux, t, b, eye;
	dvec dirs[5], right[5], up[5];
	int top[5];
	double area, zn, zf, maxd = 0.0, total, dx;
	double rsum = 0.0;
	size_t i, j;
	int v, px, py;

	tri_frame(&ps[src], &n, &cent, &area);
	aux = (fabs(n.y) < 0.9) ? dv(0, 1, 0) : dv(1, 0, 0);
	t = dnorm(dcross(aux, n));
	b = dcross(n, t);
	eye = dadd(cent, dmul(n, (double)offset));
	zn = (double)offset * 0.25;
	for (i = 0; i < count; i++) {
		if (i == src) continue;
		for (j = 0; j < 3; j++) {
			double l = dlen(dsub(p2d(ps[i].v[j]), eye));
			if (l > maxd) maxd = l;
		}
	}
	zf = ((maxd > (double)offset) ? maxd : (double)offset) + (double)offset;

	dirs[0] = n;  right[0] = t;  up[0] = b;  top[0] = 1;
	dirs[1] = t;  dirs[2] = dmul(t, -1.0);
	dirs[3] = b;  dirs[4] = dmul(b, -1.0);
	for (v = 1; v < 5; v++) {
		right[v] = dcross(n, dirs[v]);
		up[v] = n;
		top[v] = 0;
	}

	total = hemi_total_weight(res);
	dx = 2.0 / (double)res;
	for (v = 0; v < 5; v++) {
		int rows = top[v] ? res : res / 2;
		for (py = 0; py < rows; py++) {
			/* top: the "second" axis is the local y, in [-1,1];
			 * side: it is z, sampled on the upper half [0,1]. */
			double second = 1.0 - (2.0*py + 1.0) * dx * 0.5;
			for (px = 0; px < res; px++) {
				double first = -1.0 + (2.0*px + 1.0) * dx * 0.5;
				dvec dir = dadd(dirs[v], dadd(dmul(right[v], first),
					dmul(up[v], second)));
				int best = -1;
				double bestlam = 1e300;
				double w, dd;
				dvec nn, cc;
				double aa;
				for (i = 0; i < count; i++) {
					double lam;
					if (i == src) continue;
					if (!ray_tri(eye, dir, ps[i].v, &lam)) continue;
					if (lam < zn || lam > zf) continue;
					if (lam < bestlam - 1e-9 ||
							(fabs(lam - bestlam) <= 1e-9 &&
							 (best < 0 || (int)i < best))) {
						bestlam = lam;
						best = (int)i;
					}
				}
				if (best < 0) continue;
				tri_frame(&ps[best], &nn, &cc, &aa);
				if (ddot(dir, nn) >= 0.0) continue;   // back face absorbs
				dd = 1.0 + first*first + second*second;
				if (top[v]) w = dx*dx / (PI_D * dd * dd);
				else w = dx*dx * second / (PI_D * dd * dd);
				acc[best] += w / total;
				if (face) face[best * 5 + v] += w / total;
			}
		}
	}
	for (j = 0; j < count; j++) rsum += acc[j];
	if (rsum > 1.0) {
		double inv = 1.0 / rsum;
		for (j = 0; j < count; j++) acc[j] *= inv;
		if (face) {
			size_t q;
			for (q = 0; q < count * 5; q++) face[q] *= inv;
		}
	}
}

//---------------------------------------------------------------------
// numeric comparison helpers
//---------------------------------------------------------------------
static int near_rel(double a, double b, double rtol, double atol) {
	double diff = fabs(a - b);
	double scale = fabs(a) + fabs(b);
	return diff <= atol + rtol * scale;
}

static int check_row(const char *tag, radiosity_scene *s, size_t src,
		size_t count, const double *ref) {
	size_t j;
	for (j = 0; j < count; j++) {
		double got = (double)radiosity_factor(s, src, j);
		if (!near_rel(got, ref[j], 2e-3, 1e-4)) {
			fprintf(stderr, "  %s row %zu col %zu model=%.8f ref=%.8f\n",
				tag, src, j, got, ref[j]);
			return 0;
		}
	}
	(void)count;
	return 1;
}

//=====================================================================
// visibility group
//=====================================================================
// A source plus five small receivers, each placed so that it is only
// covered by exactly one hemicube face (top, +t, -t, +b, -b).  This
// exercises the ID integration of every face in isolation.
static int vis_five_faces(void) {
	const int res = 48;
	const float off = 0.05f;
	radiosity_patch ps[6];
	radiosity_scene *s;
	size_t i, j;
	int f;
	int expect_face[5] = {0, 1, 2, 3, 4};   // receiver -> hemicube face
	double face_ref[5];

	ps[0] = black_patch(pt(-0.5f,-0.5f,0), pt(0.5f,-0.5f,0), pt(0.0f,0.5f,0));
	/* +n (z) receiver, normal -z */
	ps[1] = black_patch(pt(0.30f,0.00f,1.2f), pt(-0.30f,0.00f,1.2f),
		pt(0.0f,0.30f,1.2f));
	/* +t (+x) receiver, normal -x */
	ps[2] = black_patch(pt(1.2f,0.30f,0.0f), pt(1.2f,-0.30f,0.0f),
		pt(1.2f,0.0f,0.30f));
	/* -t (-x) receiver, normal +x */
	ps[3] = black_patch(pt(-1.2f,-0.30f,0.0f), pt(-1.2f,0.30f,0.0f),
		pt(-1.2f,0.0f,0.30f));
	/* +b (+y) receiver, normal -y */
	ps[4] = black_patch(pt(0.0f,1.2f,0.30f), pt(0.0f,1.2f,-0.30f),
		pt(0.30f,1.2f,0.0f));
	/* -b (-y) receiver, normal +y */
	ps[5] = black_patch(pt(0.0f,-1.2f,-0.30f), pt(0.0f,-1.2f,0.30f),
		pt(0.30f,-1.2f,0.0f));

	/* Confirm the winding/normal choices the receivers were built for:
	 * each one must face back toward the source. */
	{
		const dvec expect_n[5] = {
			{0,0,-1}, {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}
		};
		for (i = 1; i <= 5; i++) {
			dvec n, c; double a;
			tri_frame(&ps[i], &n, &c, &a);
			TASK_CHECK(near_rel(n.x, expect_n[i-1].x, 1e-5, 1e-6));
			TASK_CHECK(near_rel(n.y, expect_n[i-1].y, 1e-5, 1e-6));
			TASK_CHECK(near_rel(n.z, expect_n[i-1].z, 1e-5, 1e-6));
		}
	}
	s = radiosity_create(ps, 6, res, off);
	TASK_CHECK(s != NULL);
	if (!s) return 1;

	for (i = 1; i <= 5; i++) {
		double acc[6], face[6 * 5];
		memset(acc, 0, sizeof(acc));
		memset(face, 0, sizeof(face));
		ref_row(ps, 6, 0, res, off, acc, face);
		f = expect_face[i - 1];
		/* every other face must be blind to this receiver */
		{
			int k;
			for (k = 0; k < 5; k++) {
				if (k == f) continue;
				TASK_CHECK(face[i * 5 + k] < 1e-6);
			}
		}
		face_ref[f] = face[i * 5 + f];
		TASK_CHECK(face[i * 5 + f] > 1e-5);    // this face does see it
		if (!near_rel(acc[i], radiosity_factor(s, 0, i), 2e-3, 2e-4)) {
			fprintf(stderr, "  five-faces receiver %zu face %d ref=%.8f model=%.8f\n",
				i, f, acc[i], radiosity_factor(s, 0, i));
			return 1;
		}
	}
	/* all five faces contribute a positive, distinct-direction share */
	for (f = 0; f < 5; f++) TASK_CHECK(face_ref[f] > 1e-5);

	/* compare the whole row against the reference */
	{
		double acc[6];
		memset(acc, 0, sizeof(acc));
		ref_row(ps, 6, 0, res, off, acc, NULL);
		TASK_CHECK(check_row("five-faces", s, 0, 6, acc));
	}
	TASK_CHECK(radiosity_factor(s, 0, 0) == 0.0f);
	{
		double rs = 0.0;
		for (j = 0; j < 6; j++) rs += radiosity_factor(s, 0, j);
		TASK_CHECK(rs <= 1.0 + 1e-6);
		TASK_CHECK(rs > 0.0);
	}
	radiosity_destroy(s);
	return 0;
}

// Triangles that cross the near plane and the side planes must keep
// their visible part.  Both receivers are wound to face the source and
// the whole row is checked against the ray reference.
static int vis_clip_planes(void) {
	const int res = 48;
	const float off = 0.05f;
	radiosity_patch ps[3];
	radiosity_scene *s;
	double acc[3];

	ps[0] = black_patch(pt(-0.5f,-0.5f,0), pt(0.5f,-0.5f,0), pt(0.0f,0.5f,0));
	/* wall on the left, tilted through the near plane (z=-0.4) */
	ps[1] = black_patch(pt(-1.0f,-2.0f,-0.4f), pt(-3.5f,-2.0f,-0.4f),
		pt(-2.25f,2.0f,1.6f));
	/* wide back wall spanning both horizontal side planes */
	ps[2] = black_patch(pt(4.5f,-1.5f,1.2f), pt(0.5f,-1.5f,1.2f),
		pt(2.5f,1.5f,1.2f));

	s = radiosity_create(ps, 3, res, off);
	TASK_CHECK(s != NULL);
	if (!s) return 1;

	memset(acc, 0, sizeof(acc));
	ref_row(ps, 3, 0, res, off, acc, NULL);
	TASK_CHECK(check_row("clip-planes", s, 0, 3, acc));

	/* both walls are visible to the source despite the clipping */
	TASK_CHECK(radiosity_factor(s, 0, 1) > 1e-3);
	TASK_CHECK(radiosity_factor(s, 0, 2) > 1e-3);
	TASK_CHECK(radiosity_factor(s, 0, 0) == 0.0f);
	{
		double rs = 0.0;
		size_t j;
		for (j = 0; j < 3; j++) rs += radiosity_factor(s, 0, j);
		TASK_CHECK(rs > 0.0 && rs <= 1.0 + 1e-6);
	}
	radiosity_destroy(s);
	return 0;
}

// Two layers ordered front/back, plus a back-facing occluder.  The
// nearer layer takes the energy, the farther one stays hidden, and the
// occluder absorbs instead of letting energy through to the receiver.
//
// Two scenes are built so the layers and the occluder each get a clean
// line of sight to the source.
static int vis_layers_occlusion(void) {
	const int res = 48;
	const float off = 0.05f;
	point_t s0 = pt(-0.5f,-0.5f,0), s1 = pt(0.5f,-0.5f,0), s2 = pt(0.0f,0.5f,0);
	radiosity_scene *s;

	/* --- scene A: two layers on the axis --- */
	{
		radiosity_patch ps[4];
		radiosity_patch no_front[4];
		radiosity_scene *s_none;
		double acc[4];

		ps[0] = black_patch(s0, s1, s2);
		/* front layer facing the source (-z) */
		ps[1] = black_patch(pt(-0.7f,-0.7f,1.4f), pt(0.0f,0.7f,1.4f),
			pt(0.7f,-0.7f,1.4f));
		/* same triangle one unit further, also facing -z */
		ps[2] = black_patch(pt(-0.7f,-0.7f,2.4f), pt(0.0f,0.7f,2.4f),
			pt(0.7f,-0.7f,2.4f));
		/* side receiver, facing -x */
		ps[3] = black_patch(pt(2.0f,0.5f,0.8f), pt(2.0f,-0.5f,0.8f),
			pt(2.0f,0.0f,1.4f));

		s = radiosity_create(ps, 4, res, off);
		TASK_CHECK(s != NULL);
		if (!s) return 1;
		memset(acc, 0, sizeof(acc));
		ref_row(ps, 4, 0, res, off, acc, NULL);
		TASK_CHECK(check_row("layers", s, 0, 4, acc));
		TASK_CHECK(radiosity_factor(s, 0, 1) > 1e-3);   // front lit
		TASK_CHECK(radiosity_factor(s, 0, 2) < 1e-3);   // back hidden
		TASK_CHECK(radiosity_factor(s, 0, 3) > 1e-3);   // side lit
		TASK_CHECK(radiosity_factor(s, 0, 0) == 0.0f);
		radiosity_destroy(s);

		/* remove the front layer and the back layer becomes visible,
		 * proving the zero was occlusion rather than bad geometry */
		memcpy(no_front, ps, sizeof(ps));
		no_front[1] = black_patch(pt(-5.0f,-5.0f,1.4f), pt(-4.0f,-5.0f,1.4f),
			pt(-4.5f,-4.0f,1.4f));
		s_none = radiosity_create(no_front, 4, res, off);
		TASK_CHECK(s_none != NULL);
		if (!s_none) return 1;
		TASK_CHECK(radiosity_factor(s_none, 0, 2) > 1e-3);
		radiosity_destroy(s_none);
	}

	/* --- scene B: back-face occluder in front of a receiver --- */
	{
		radiosity_patch ps[4];
		radiosity_patch flipped[4];
		radiosity_scene *sf;
		double acc[4];

		ps[0] = black_patch(s0, s1, s2);
		/* occluder on the axis, wound away from the source (+z) */
		ps[1] = black_patch(pt(-1.2f,-1.2f,1.0f), pt(1.2f,-1.2f,1.0f),
			pt(0.0f,1.2f,1.0f));
		/* receiver right behind it, facing -z */
		ps[2] = black_patch(pt(-0.8f,-0.8f,2.0f), pt(0.0f,0.8f,2.0f),
			pt(0.8f,-0.8f,2.0f));
		/* side receiver, facing -x */
		ps[3] = black_patch(pt(2.0f,0.5f,0.8f), pt(2.0f,-0.5f,0.8f),
			pt(2.0f,0.0f,1.4f));

		s = radiosity_create(ps, 4, res, off);
		TASK_CHECK(s != NULL);
		if (!s) return 1;
		memset(acc, 0, sizeof(acc));
		ref_row(ps, 4, 0, res, off, acc, NULL);
		TASK_CHECK(check_row("occluder-back", s, 0, 4, acc));
		TASK_CHECK(radiosity_factor(s, 0, 1) < 1e-3);   // back absorbs
		TASK_CHECK(radiosity_factor(s, 0, 2) < 1e-3);   // receiver shadowed
		TASK_CHECK(radiosity_factor(s, 0, 3) > 1e-3);   // side unaffected
		radiosity_destroy(s);

		/* flip the occluder: now it is lit, but it still blocks the
		 * receiver behind it with its front face */
		memcpy(flipped, ps, sizeof(ps));
		flipped[1] = black_patch(pt(1.2f,-1.2f,1.0f), pt(-1.2f,-1.2f,1.0f),
			pt(0.0f,1.2f,1.0f));
		sf = radiosity_create(flipped, 4, res, off);
		TASK_CHECK(sf != NULL);
		if (!sf) return 1;
		TASK_CHECK(radiosity_factor(sf, 0, 1) > 0.1);
		TASK_CHECK(radiosity_factor(sf, 0, 2) < 1e-3);
		TASK_CHECK(radiosity_factor(sf, 0, 3) > 1e-3);
		radiosity_destroy(sf);
	}
	return 0;
}

// Coincident receivers: at equal depth the smaller original patch index
// wins, so the first duplicate takes all the energy and the second gets
// none.  Two back-facing duplicates must also absorb without being
// credited (proving the ID, not the depth, drives the credit).
static int vis_depth_tie(void) {
	const int res = 32;
	radiosity_patch ps[4];
	radiosity_scene *s;
	point_t a = pt(-0.4f,-0.4f,1.5f), b = pt(0.0f,0.4f,1.5f),
		c = pt(0.4f,-0.4f,1.5f);
	double acc[4], f01, f02;

	ps[0] = black_patch(pt(-0.5f,-0.5f,0), pt(0.5f,-0.5f,0), pt(0.0f,0.5f,0));
	/* two coincident receivers, both facing the source (-z) */
	ps[1] = black_patch(a, b, c);
	ps[2] = black_patch(a, b, c);
	/* one farther receiver so the scene is not a closed pair */
	ps[3] = black_patch(pt(-0.4f,-0.4f,2.5f), pt(0.0f,0.4f,2.5f),
		pt(0.4f,-0.4f,2.5f));

	s = radiosity_create(ps, 4, res, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	f01 = radiosity_factor(s, 0, 1);
	f02 = radiosity_factor(s, 0, 2);
	TASK_CHECK(f01 > 1e-3);              // the first duplicate is credited
	TASK_CHECK(f02 == 0.0f);             // the second is fully shadowed
	memset(acc, 0, sizeof(acc));
	ref_row(ps, 4, 0, res, 0.05f, acc, NULL);
	/* the reference ray caster breaks the tie the same way */
	TASK_CHECK(near_rel(acc[1], f01, 2e-3, 2e-4));
	TASK_CHECK(acc[2] == 0.0);
	radiosity_destroy(s);

	/* swap the winding so the coincident pair faces away: both absorb,
	 * and neither may receive any credit. */
	ps[1] = black_patch(c, b, a);
	ps[2] = black_patch(c, b, a);
	s = radiosity_create(ps, 4, res, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	TASK_CHECK(radiosity_factor(s, 0, 1) == 0.0f);
	TASK_CHECK(radiosity_factor(s, 0, 2) == 0.0f);
	TASK_CHECK(radiosity_factor(s, 0, 3) == 0.0f);   // blocked behind them
	radiosity_destroy(s);
	return 0;
}

//=====================================================================
// transport group
//=====================================================================
static int trans_areas(void) {
	radiosity_patch ps[3];
	radiosity_scene *s;
	color_t B, U;
	float area;
	double expect[3];
	size_t i;

	ps[0] = patch3(pt(0,0,0), pt(2,0,0), pt(0,2,0), 1,1,1, 0.5f,0.5f,0.5f);
	ps[1] = patch3(pt(0,0,3), pt(0,3,3), pt(3,0,3), 0,0,0, 0.5f,0.5f,0.5f);
	ps[2] = patch3(pt(-1,-1,1), pt(1,-1,1), pt(0,2,1), 0,0,0, 0.2f,0.3f,0.4f);
	s = radiosity_create(ps, 3, 24, 0.02f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	expect[0] = 2.0;   // right triangle legs 2,2
	expect[1] = 4.5;   // legs 3,3
	expect[2] = 3.0;   // base 2, height 3
	for (i = 0; i < 3; i++) {
		TASK_CHECK(radiosity_get(s, i, &B, &U, &area) == 1);
		TASK_CHECK(near_rel(area, expect[i], 1e-5, 1e-6));
	}
	TASK_CHECK(radiosity_get(s, 3, &B, &U, &area) == 0);
	TASK_CHECK(radiosity_get(s, 0, NULL, NULL, NULL) == 1);
	radiosity_destroy(s);
	return 0;
}

// Independently re-run progressive shooting from the matrix read back
// through the public API and compare every intermediate state.
static int trans_recurrence(void) {
	radiosity_patch ps[4];
	radiosity_scene *s;
	color_t B[4], U[4];
	float area[4];
	double eb[4][3], eu[4][3];
	size_t i, k;
	int c;

	ps[0] = patch3(pt(-1,-1,0), pt(1,-1,0), pt(0,1,0), 2.0f,1.0f,0.5f,
		0.0f,0.0f,0.0f);
	ps[1] = patch3(pt(-1.5f,-1.5f,2), pt(0,1.5f,2), pt(1.5f,-1.5f,2),
		0,0,0, 0.6f,0.3f,0.1f);
	ps[2] = patch3(pt(2,-1,1), pt(2,1,1), pt(2.5f,0,2),
		0,0,0, 0.2f,0.8f,0.5f);
	ps[3] = patch3(pt(-2,-1,1.5f), pt(-2,1,1.5f), pt(-2.5f,0,2.5f),
		0.5f,0.5f,0.5f, 0.4f,0.4f,0.4f);

	s = radiosity_create(ps, 4, 40, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;

	for (i = 0; i < 4; i++) {
		TASK_CHECK(radiosity_get(s, i, &B[i], &U[i], &area[i]) == 1);
		eb[i][0] = ps[i].emission.r; eb[i][1] = ps[i].emission.g;
		eb[i][2] = ps[i].emission.b;
		eu[i][0] = ps[i].emission.r; eu[i][1] = ps[i].emission.g;
		eu[i][2] = ps[i].emission.b;
	}

	for (k = 0; k < 12; k++) {
		size_t best = (size_t)-1;
		double best_e = 0.0;
		double shot[3];
		size_t j;
		for (i = 0; i < 4; i++) {
			double e = (double)area[i] *
				(eu[i][0] + eu[i][1] + eu[i][2]);
			if (e > best_e) { best_e = e; best = i; }
		}
		TASK_CHECK(best != (size_t)-1);
		if (best == (size_t)-1) break;
		for (c = 0; c < 3; c++) { shot[c] = eu[best][c]; eu[best][c] = 0.0; }
		for (j = 0; j < 4; j++) {
			double f, ratio;
			if (j == best) continue;
			f = (double)radiosity_factor(s, best, j);
			if (!(f > 0.0)) continue;
			ratio = (double)area[best] / (double)area[j];
			for (c = 0; c < 3; c++) {
				double refl = (c == 0) ? ps[j].reflectance.r :
					(c == 1) ? ps[j].reflectance.g : ps[j].reflectance.b;
				double add = refl * shot[c] * ratio * f;
				eb[j][c] += add;
				eu[j][c] += add;
			}
		}
		TASK_CHECK(radiosity_step(s, 1) == 1);
		TASK_CHECK(radiosity_last_shooter(s) == best);
		for (i = 0; i < 4; i++) {
			color_t gb, gu;
			float ga;
			TASK_CHECK(radiosity_get(s, i, &gb, &gu, &ga) == 1);
			TASK_CHECK(near_rel(gb.r, eb[i][0], 2e-4, 2e-5));
			TASK_CHECK(near_rel(gb.g, eb[i][1], 2e-4, 2e-5));
			TASK_CHECK(near_rel(gb.b, eb[i][2], 2e-4, 2e-5));
			TASK_CHECK(near_rel(gu.r, eu[i][0], 2e-4, 2e-5));
			TASK_CHECK(near_rel(gu.g, eu[i][1], 2e-4, 2e-5));
			TASK_CHECK(near_rel(gu.b, eu[i][2], 2e-4, 2e-5));
		}
	}
	/* U of the most recent shooter is exactly zero */
	{
		size_t last = radiosity_last_shooter(s);
		color_t gb, gu; float ga;
		TASK_CHECK(last < 4);
		TASK_CHECK(radiosity_get(s, last, &gb, &gu, &ga) == 1);
		TASK_CHECK(gu.r == 0.0f && gu.g == 0.0f && gu.b == 0.0f);
	}
	radiosity_destroy(s);
	return 0;
}

// Area ratio and per-channel colour transfer with starkly unequal
// areas: the receiver's gain must scale with Ai/Aj, not with F alone.
static int trans_area_ratio_color(void) {
	radiosity_patch ps[2];
	radiosity_scene *s;
	color_t B, U;
	float area;
	double f, expect_r, expect_g, expect_b, ratio;

	ps[0] = patch3(pt(0,0,0), pt(2,0,0), pt(0,2,0), 1.0f,0.5f,0.25f,
		0,0,0);                                     // area 2.0
	ps[1] = patch3(pt(-1,-1,2), pt(0,1,2), pt(1,-1,2), 0,0,0,
		0.8f,0.4f,0.1f);                            // area 2.0 too
	s = radiosity_create(ps, 2, 32, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	TASK_CHECK(radiosity_get(s, 0, &B, &U, &area) == 1);
	TASK_CHECK(near_rel(area, 2.0, 1e-5, 1e-6));
	f = (double)radiosity_factor(s, 0, 1);
	TASK_CHECK(f > 0.0);
	TASK_CHECK(radiosity_step(s, 1) == 1);
	TASK_CHECK(radiosity_last_shooter(s) == 0);
	TASK_CHECK(radiosity_get(s, 1, &B, &U, &area) == 1);
	ratio = 2.0 / 2.0;
	expect_r = 0.8 * 1.0 * ratio * f;
	expect_g = 0.4 * 0.5 * ratio * f;
	expect_b = 0.1 * 0.25 * ratio * f;
	TASK_CHECK(near_rel(B.r, expect_r, 1e-5, 1e-7));
	TASK_CHECK(near_rel(B.g, expect_g, 1e-5, 1e-7));
	TASK_CHECK(near_rel(B.b, expect_b, 1e-5, 1e-7));
	/* colour bleeding: red gains more than green gains more than blue */
	TASK_CHECK(B.r > B.g && B.g > B.b);
	/* the shot patch keeps only its emission and loses its unshot part */
	TASK_CHECK(radiosity_get(s, 0, &B, &U, &area) == 1);
	TASK_CHECK(U.r == 0.0f && U.g == 0.0f && U.b == 0.0f);
	TASK_CHECK(near_rel(B.r, 1.0, 1e-6, 1e-7));

	/* now scale the receiver area and confirm the Ai/Aj factor */
	radiosity_destroy(s);
	ps[1] = patch3(pt(-2,-2,2), pt(0,2,2), pt(2,-2,2), 0,0,0,
		0.8f,0.4f,0.1f);                            // area 8.0
	s = radiosity_create(ps, 2, 32, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	f = (double)radiosity_factor(s, 0, 1);
	TASK_CHECK(radiosity_step(s, 1) == 1);
	TASK_CHECK(radiosity_get(s, 1, &B, &U, &area) == 1);
	TASK_CHECK(near_rel(area, 8.0, 1e-5, 1e-6));
	expect_r = 0.8 * 1.0 * (2.0 / 8.0) * f;
	TASK_CHECK(near_rel(B.r, expect_r, 1e-5, 1e-7));
	radiosity_destroy(s);
	return 0;
}

// Shooting order follows A*(Ur+Ug+Ub), with the smaller index first on
// an exact tie.
static int trans_shooting_order(void) {
	radiosity_patch ps[4];
	radiosity_scene *s;

	/* p0: area 1, energy 1; p1: area 4, energy 2 -> p1 shoots first */
	ps[0] = patch3(pt(0,0,0), pt(1,0,0), pt(0,1,0), 1,0,0, 0,0,0);
	ps[1] = patch3(pt(0,0,2), pt(2,0,2), pt(0,2,2), 0.5f,0,0, 0,0,0);
	ps[2] = patch3(pt(3,0,1), pt(3,1,1), pt(3,0,2), 0,0,0, 0,0,0);
	ps[3] = patch3(pt(-3,0,1), pt(-3,1,1), pt(-3,0,2), 0,0,0, 0,0,0);
	s = radiosity_create(ps, 4, 20, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	TASK_CHECK(radiosity_last_shooter(s) == SIZE_MAX);
	TASK_CHECK(radiosity_step(s, 1) == 1);
	TASK_CHECK(radiosity_last_shooter(s) == 1);
	TASK_CHECK(radiosity_step(s, 1) == 1);
	TASK_CHECK(radiosity_last_shooter(s) == 0);
	/* with reflectance zero, the two black patches never shoot */
	TASK_CHECK(radiosity_step(s, 8) == 0);
	radiosity_destroy(s);

	/* exact tie: two equal-energy emitters -> index 0 first, then 1 */
	ps[0] = patch3(pt(0,0,0), pt(1,0,0), pt(0,1,0), 1,1,1, 0,0,0);
	ps[1] = patch3(pt(2,0,0), pt(3,0,0), pt(2,1,0), 1,1,1, 0,0,0);
	ps[2] = patch3(pt(0,2,0), pt(1,2,0), pt(0,3,0), 0,0,0, 0,0,0);
	s = radiosity_create(ps, 3, 20, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	TASK_CHECK(radiosity_step(s, 1) == 1);
	TASK_CHECK(radiosity_last_shooter(s) == 0);
	TASK_CHECK(radiosity_step(s, 1) == 1);
	TASK_CHECK(radiosity_last_shooter(s) == 1);
	radiosity_destroy(s);
	return 0;
}

static int trans_remaining_energy(void) {
	radiosity_patch ps[2];
	radiosity_scene *s;
	color_t B, U;
	float area;

	/* two purely emissive patches, reflectance 0: exactly two shots */
	ps[0] = patch3(pt(0,0,0), pt(1,0,0), pt(0,1,0), 1,1,1, 0,0,0);
	ps[1] = patch3(pt(0,0,2), pt(1,0,2), pt(0,1,2), 1,1,1, 0,0,0);
	s = radiosity_create(ps, 2, 20, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	TASK_CHECK(radiosity_step(s, 10) == 2);
	TASK_CHECK(radiosity_step(s, 10) == 0);
	TASK_CHECK(radiosity_get(s, 1, &B, &U, &area) == 1);
	TASK_CHECK(U.r == 0.0f && U.g == 0.0f && U.b == 0.0f);
	TASK_CHECK(B.r > 0.0f);                 // it received from patch 0
	radiosity_destroy(s);

	/* no emission anywhere: no step at all */
	ps[0] = patch3(pt(0,0,0), pt(1,0,0), pt(0,1,0), 0,0,0, 0.5f,0.5f,0.5f);
	ps[1] = patch3(pt(0,0,2), pt(1,0,2), pt(0,1,2), 0,0,0, 0.5f,0.5f,0.5f);
	s = radiosity_create(ps, 2, 20, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) return 1;
	TASK_CHECK(radiosity_step(s, 5) == 0);
	TASK_CHECK(radiosity_last_shooter(s) == SIZE_MAX);
	radiosity_destroy(s);
	return 0;
}

//---------------------------------------------------------------------
// integration group
//---------------------------------------------------------------------
// The integration scene geometry, kept explicit so the draw test can
// rebuild the very same triangles independently of the module.
static void scene_patches(radiosity_patch out[4]) {
	out[0] = patch3(pt(-1,-1,0), pt(1,-1,0), pt(0,1,0), 1.0f,0.8f,0.4f,
		0.3f,0.3f,0.3f);
	out[1] = patch3(pt(-1.5f,-1.5f,2), pt(0,1.5f,2), pt(1.5f,-1.5f,2),
		0,0,0, 0.6f,0.2f,0.1f);
	out[2] = patch3(pt(2,-1,1), pt(2,1,1), pt(2.5f,0,2),
		0.2f,0.4f,0.6f, 0.5f,0.5f,0.5f);
	out[3] = patch3(pt(-2,-1,1.5f), pt(-2,1,1.5f), pt(-2.5f,0,2.5f),
		0,0,0, 0.4f,0.5f,0.6f);
}

static radiosity_scene *make_scene(void) {
	radiosity_patch ps[4];
	scene_patches(ps);
	return radiosity_create(ps, 4, 28, 0.05f);
}

static int int_chunked(void) {
	radiosity_scene *a = make_scene();
	radiosity_scene *b = make_scene();
	size_t i;

	TASK_CHECK(a != NULL && b != NULL);
	if (!a || !b) return 1;
	TASK_CHECK(radiosity_step(a, 9) == 9);
	{
		size_t total = 0;
		size_t chunks[4] = {1, 3, 2, 3};
		int k;
		for (k = 0; k < 4; k++) total += radiosity_step(b, chunks[k]);
		TASK_CHECK(total == 9);
	}
	for (i = 0; i < 4; i++) {
		color_t ba, ua, bb, ub;
		float aa, ab;
		TASK_CHECK(radiosity_get(a, i, &ba, &ua, &aa) == 1);
		TASK_CHECK(radiosity_get(b, i, &bb, &ub, &ab) == 1);
		TASK_CHECK(ba.r == bb.r && ba.g == bb.g && ba.b == bb.b);
		TASK_CHECK(ua.r == ub.r && ua.g == ub.g && ua.b == ub.b);
		TASK_CHECK(aa == ab);
	}
	TASK_CHECK(radiosity_last_shooter(a) == radiosity_last_shooter(b));
	radiosity_destroy(a);
	radiosity_destroy(b);
	return 0;
}

static int int_reset(void) {
	radiosity_scene *s = make_scene();
	radiosity_scene *fresh = make_scene();
	size_t i;
	float f_before, f_after;

	TASK_CHECK(s != NULL && fresh != NULL);
	if (!s || !fresh) return 1;
	f_before = radiosity_factor(s, 0, 1);
	TASK_CHECK(radiosity_step(s, 5) == 5);
	TASK_CHECK(radiosity_last_shooter(s) != SIZE_MAX);
	radiosity_reset(s);
	TASK_CHECK(radiosity_last_shooter(s) == SIZE_MAX);
	f_after = radiosity_factor(s, 0, 1);
	TASK_CHECK(f_before == f_after);        // matrix preserved
	for (i = 0; i < 4; i++) {
		color_t b, u;
		float a;
		TASK_CHECK(radiosity_get(s, i, &b, &u, &a) == 1);
		TASK_CHECK(near_rel(b.r, u.r, 0, 0) && near_rel(b.g, u.g, 0, 0) &&
			near_rel(b.b, u.b, 0, 0));
	}
	/* a reset scene steps exactly like a fresh one */
	TASK_CHECK(radiosity_step(s, 6) == 6);
	TASK_CHECK(radiosity_step(fresh, 6) == 6);
	for (i = 0; i < 4; i++) {
		color_t b1, u1, b2, u2;
		float a1, a2;
		TASK_CHECK(radiosity_get(s, i, &b1, &u1, &a1) == 1);
		TASK_CHECK(radiosity_get(fresh, i, &b2, &u2, &a2) == 1);
		TASK_CHECK(b1.r == b2.r && b1.g == b2.g && b1.b == b2.b);
		TASK_CHECK(u1.r == u2.r && u1.g == u2.g && u1.b == u2.b);
	}
	radiosity_destroy(s);
	radiosity_destroy(fresh);
	return 0;
}

static int int_two_scenes(void) {
	radiosity_scene *a = make_scene();
	radiosity_scene *b = make_scene();
	radiosity_scene *solo_a = make_scene();
	radiosity_scene *solo_b = make_scene();
	size_t i;

	TASK_CHECK(a && b && solo_a && solo_b);
	if (!(a && b && solo_a && solo_b)) return 1;
	/* interleave the two scenes in an irregular rhythm */
	TASK_CHECK(radiosity_step(a, 1) == 1);
	TASK_CHECK(radiosity_step(b, 2) == 2);
	TASK_CHECK(radiosity_step(a, 4) == 4);
	TASK_CHECK(radiosity_step(b, 1) == 1);
	TASK_CHECK(radiosity_step(a, 2) == 2);
	/* and run the same totals without interleaving */
	{
		size_t na = radiosity_step(solo_a, 7);
		size_t nb = radiosity_step(solo_b, 3);
		TASK_CHECK(na == 7 && nb == 3);
	}
	for (i = 0; i < 4; i++) {
		color_t ba, ua, bb, ub, bsa, usa, bsb, usb;
		float aa, ab, asa, asb;
		TASK_CHECK(radiosity_get(a, i, &ba, &ua, &aa) == 1);
		TASK_CHECK(radiosity_get(solo_a, i, &bsa, &usa, &asa) == 1);
		TASK_CHECK(radiosity_get(b, i, &bb, &ub, &ab) == 1);
		TASK_CHECK(radiosity_get(solo_b, i, &bsb, &usb, &asb) == 1);
		TASK_CHECK(ba.r == bsa.r && ba.g == bsa.g && ba.b == bsa.b);
		TASK_CHECK(ua.r == usa.r && ua.g == usa.g && ua.b == usa.b);
		TASK_CHECK(bb.r == bsb.r && bb.g == bsb.g && bb.b == bsb.b);
		TASK_CHECK(ub.r == usb.r && ub.g == usb.g && ub.b == usb.b);
	}
	radiosity_destroy(a);
	radiosity_destroy(b);
	radiosity_destroy(solo_a);
	radiosity_destroy(solo_b);
	return 0;
}

// Draw must be pixel-identical to feeding the same constant vertex
// colours through device_draw_primitive, and must leave the caller's
// transform / render state and pre-existing buffers untouched.
static int int_draw(void) {
	device_t d1, d2;
	radiosity_scene *s;
	radiosity_patch ps[4];
	int w = 96, h = 96, x, y;
	transform_t saved_transform;
	int saved_state;
	int same = 1;
	size_t i;

	device_init(&d1, w, h, NULL);
	device_init(&d2, w, h, NULL);
	device_clear(&d1, 0);
	device_clear(&d2, 0);
	d2.render_state = RENDER_STATE_COLOR;

	scene_patches(ps);
	s = radiosity_create(ps, 4, 28, 0.05f);
	TASK_CHECK(s != NULL);
	if (!s) { device_destroy(&d1); device_destroy(&d2); return 1; }
	TASK_CHECK(radiosity_step(s, 5) == 5);     // non-trivial colours

	/* corrupt a far corner to prove nothing is cleared */
	d1.framebuffer[0][0] = 0x123456;
	d1.zbuffer[0][0] = -7.0f;
	saved_transform = d1.transform;
	saved_state = d1.render_state;

	radiosity_draw(s, &d1, 1.0f);

	TASK_CHECK(d1.render_state == saved_state);
	TASK_CHECK(memcmp(&d1.transform, &saved_transform,
		sizeof(transform_t)) == 0);
	TASK_CHECK(d1.framebuffer[0][0] == 0x123456);
	TASK_CHECK(d1.zbuffer[0][0] == -7.0f);

	/* independent path: rebuild the triangles from the test's own copy
	 * of the geometry, with each vertex coloured by the current B. */
	{
		transform_t orig = d2.transform;
		for (i = 0; i < 4; i++) {
			vertex_t v[3];
			color_t B, U;
			float area;
			int k;
			TASK_CHECK(radiosity_get(s, i, &B, &U, &area) == 1);
			for (k = 0; k < 3; k++) {
				v[k].pos = ps[i].v[k];
				v[k].tc.u = 0.0f;
				v[k].tc.v = 0.0f;
				v[k].color = B;
				v[k].rhw = 1.0f;
			}
			device_draw_primitive(&d2, &v[0], &v[1], &v[2]);
		}
		d2.transform = orig;
	}
	/* restore the probe corner on d1 so the comparison skips it */
	d1.framebuffer[0][0] = 0xc0c0c0;
	d1.zbuffer[0][0] = 0.0f;
	for (y = 0; y < h && same; y++) {
		for (x = 0; x < w; x++) {
			if (d1.framebuffer[y][x] != d2.framebuffer[y][x] ||
					d1.zbuffer[y][x] != d2.zbuffer[y][x]) {
				same = 0;
				break;
			}
		}
	}
	TASK_CHECK(same);
	/* at least one pixel was actually written */
	{
		int lit = 0;
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				if (d1.framebuffer[y][x] != 0xc0c0c0) lit = 1;
		TASK_CHECK(lit);
	}

	/* exposure scales the constant vertex colour before byte clamping */
	{
		device_t d3, d4;
		int px, py, found = 0;
		device_init(&d3, w, h, NULL);
		device_init(&d4, w, h, NULL);
		device_clear(&d3, 0);
		device_clear(&d4, 0);
		radiosity_draw(s, &d3, 1.0f);
		radiosity_draw(s, &d4, 0.5f);
		for (py = 0; py < h && !found; py++) {
			for (px = 0; px < w; px++) {
				IUINT32 c1 = d3.framebuffer[py][px];
				IUINT32 c2 = d4.framebuffer[py][px];
				IUINT32 bg = 0xc0c0c0;
				int r1, g1, b1, r2, g2, b2;
				if (c1 == bg && c2 == bg) continue;
				if (c1 == bg || c2 == bg) continue;
				r1 = (int)((c1 >> 16) & 255); g1 = (int)((c1 >> 8) & 255);
				b1 = (int)(c1 & 255);
				r2 = (int)((c2 >> 16) & 255); g2 = (int)((c2 >> 8) & 255);
				b2 = (int)(c2 & 255);
				/* half exposure -> roughly half the byte values; the
				 * module rounds once at the end, so allow 2 levels. */
				TASK_CHECK(abs(r2 * 2 - r1) <= 2);
				TASK_CHECK(abs(g2 * 2 - g1) <= 2);
				TASK_CHECK(abs(b2 * 2 - b1) <= 2);
				found = 1;
				break;
			}
		}
		TASK_CHECK(found);
		device_destroy(&d3);
		device_destroy(&d4);
	}

	radiosity_destroy(s);
	device_destroy(&d1);
	device_destroy(&d2);
	return 0;
}

//=====================================================================
// case table
//=====================================================================
// Invalid inputs and degenerate geometry must yield a NULL scene; the
// query helpers must tolerate out-of-range indices and NULL handles.
static int int_validation(void) {
	radiosity_patch p;
	radiosity_patch q;

	p = patch3(pt(0,0,0), pt(1,0,0), pt(0,1,0), 1,1,1, 0.5f,0.5f,0.5f);
	TASK_CHECK(radiosity_create(NULL, 1, 8, 0.1f) == NULL);
	TASK_CHECK(radiosity_create(&p, 0, 8, 0.1f) == NULL);
	TASK_CHECK(radiosity_create(&p, 1, 3, 0.1f) == NULL);    // odd
	TASK_CHECK(radiosity_create(&p, 1, 2, 0.1f) == NULL);    // < 4
	TASK_CHECK(radiosity_create(&p, 1, 7, 0.1f) == NULL);    // odd
	TASK_CHECK(radiosity_create(&p, 1, 8, 0.0f) == NULL);
	TASK_CHECK(radiosity_create(&p, 1, 8, -1.0f) == NULL);
	TASK_CHECK(radiosity_create(&p, 1, 8, NAN) == NULL);

	q = p; q.v[0].w = 2.0f;
	TASK_CHECK(radiosity_create(&q, 1, 8, 0.1f) == NULL);   // w != 1
	q = p; q.v[1].x = INFINITY;
	TASK_CHECK(radiosity_create(&q, 1, 8, 0.1f) == NULL);   // not finite
	q = p; q.v[1] = q.v[0]; q.v[2] = q.v[0];
	TASK_CHECK(radiosity_create(&q, 1, 8, 0.1f) == NULL);   // degenerate
	q = p; q.reflectance.g = 1.0f;
	TASK_CHECK(radiosity_create(&q, 1, 8, 0.1f) == NULL);   // refl == 1
	q = p; q.reflectance.b = -0.5f;
	TASK_CHECK(radiosity_create(&q, 1, 8, 0.1f) == NULL);   // refl < 0
	q = p; q.emission.r = -1.0f;
	TASK_CHECK(radiosity_create(&q, 1, 8, 0.1f) == NULL);   // emission < 0

	/* the smallest legal configuration must work */
	{
		radiosity_scene *s = radiosity_create(&p, 1, 4, 0.1f);
		TASK_CHECK(s != NULL);
		if (!s) return 1;
		TASK_CHECK(radiosity_factor(s, 5, 0) == 0.0f);
		TASK_CHECK(radiosity_factor(s, 0, 9) == 0.0f);
		TASK_CHECK(radiosity_get(s, 5, NULL, NULL, NULL) == 0);
		TASK_CHECK(radiosity_get(s, 0, NULL, NULL, NULL) == 1);
		TASK_CHECK(radiosity_last_shooter(s) == SIZE_MAX);
		radiosity_reset(s);
		radiosity_destroy(s);
	}
	radiosity_destroy(NULL);              // tolerated
	TASK_CHECK(radiosity_step(NULL, 3) == 0);
	TASK_CHECK(radiosity_last_shooter(NULL) == SIZE_MAX);
	radiosity_reset(NULL);
	return 0;
}

const struct task_case task_cases[] = {
	{ "visibility", "five_faces", vis_five_faces },
	{ "visibility", "clip_planes", vis_clip_planes },
	{ "visibility", "layers_occlusion", vis_layers_occlusion },
	{ "visibility", "depth_tie", vis_depth_tie },
	{ "transport", "areas", trans_areas },
	{ "transport", "recurrence", trans_recurrence },
	{ "transport", "area_ratio_color", trans_area_ratio_color },
	{ "transport", "shooting_order", trans_shooting_order },
	{ "transport", "remaining_energy", trans_remaining_energy },
	{ "integration", "chunked", int_chunked },
	{ "integration", "reset", int_reset },
	{ "integration", "two_scenes", int_two_scenes },
	{ "integration", "draw", int_draw },
	{ "integration", "validation", int_validation },
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
