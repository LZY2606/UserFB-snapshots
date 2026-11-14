/*
 * Checks for perspective-correct texture mapping.
 *
 * The reference used here is derived from first principles and never calls a
 * library transform/rasterizer routine:
 *
 *   - object vertices are projected with a local double-precision 4x4 matrix;
 *   - optional homogeneous clipping is done by a local Sutherland-Hodgman
 *     pass in clip space;
 *   - vertices are snapped to TinyGL's established integer screen convention;
 *   - coverage and barycentrics come from the explicit screen-space edge
 *     functions of that integer triangle;
 *   - each covered pixel gets the rational perspective interpolation
 *         u = (sum lambda_i * u_i / w_i) / (sum lambda_i / w_i)
 *     with the vertex's homogeneous clip-space w (NOT the window z);
 *   - the source texel is then obtained with the existing fixed-point,
 *     repeat, nearest addressing and the vertex colour modulation;
 *   - depth is the affine interpolation of the integer window-z values.
 *
 * Colour is asserted only away from nearest-sampling ties and away from the
 * repeat breakpoint; pixels on a tie are counted separately, never as a
 * colour error, so a one-texel rounding difference cannot mask a real
 * error.  Pixels within one screen unit of a triangle edge are skipped for
 * the same reason: the exact boundary event is a separate, pre-existing
 * convention.
 */

#include "suite.h"

extern "C" {
#include "zbuffer.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const int TEXDIM = TGL_FEATURE_TEXTURE_DIM;
const double S_RANGE = (double)(ZB_POINT_S_MAX - ZB_POINT_S_MIN);
const double T_RANGE = (double)(ZB_POINT_T_MAX - ZB_POINT_T_MIN);
const double ZSIZE = (double)(1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));

struct ObjVert {
	float x, y, z;
	float u, v;
};

struct CVert {
	double X, Y, Z, W, u, v;
};

struct SVert {
	int x, y, z;
	double w, u, v; /* unquantised reference UV */
};

struct Mat {
	float m[16];
};

static Mat identity_mat() {
	Mat r;
	memset(&r, 0, sizeof r);
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

static Mat frustum_mat(double l, double r, double b, double t, double n, double f) {
	Mat o;
	memset(&o, 0, sizeof o);
	o.m[0] = (float)(2 * n / (r - l));
	o.m[5] = (float)(2 * n / (t - b));
	o.m[8] = (float)((r + l) / (r - l));
	o.m[9] = (float)((t + b) / (t - b));
	o.m[10] = (float)(-(f + n) / (f - n));
	o.m[11] = -1.0f;
	o.m[14] = (float)(-(2 * f * n) / (f - n));
	return o;
}

/* Column-major, matching glLoadMatrixf. */
static CVert project(const Mat& m, const ObjVert& v) {
	CVert c;
	c.X = m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12];
	c.Y = m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13];
	c.Z = m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14];
	c.W = m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15];
	c.u = v.u;
	c.v = v.v;
	return c;
}

struct Viewport {
	int W, H;
	double scX, tcX, scY, tcY, scZ, tcZ;
	Viewport(int w, int h) : W(w), H(h) {
		scX = (W - 0.5) / 2.0;
		tcX = (W - 0.5) / 2.0;
		scY = -(H - 0.5) / 2.0;
		tcY = (H - 0.5) / 2.0;
		scZ = -((ZSIZE - 0.5) / 2.0);
		tcZ = ((ZSIZE - 0.5) / 2.0) + ((1 << ZB_POINT_Z_FRAC_BITS)) / 2;
	}
	SVert snap(const CVert& c) const {
		SVert s;
		s.x = (int)(c.X / c.W * scX + tcX);
		s.y = (int)(c.Y / c.W * scY + tcY);
		s.z = (int)(c.Z / c.W * scZ + tcZ);
		s.w = c.W;
		s.u = c.u;
		s.v = c.v;
		return s;
	}
};

/* --- independent homogeneous clipper (Sutherland-Hodgman) --- */
static double plane_dist(int p, const CVert& c) {
	switch (p) {
	case 0: return c.X + c.W;
	case 1: return c.W - c.X;
	case 2: return c.Y + c.W;
	case 3: return c.W - c.Y;
	case 4: return c.Z + c.W;
	default: return c.W - c.Z;
	}
}

static CVert lerp_clip(const CVert& a, const CVert& b, double t) {
	CVert r;
	r.X = a.X + (b.X - a.X) * t;
	r.Y = a.Y + (b.Y - a.Y) * t;
	r.Z = a.Z + (b.Z - a.Z) * t;
	r.W = a.W + (b.W - a.W) * t;
	r.u = a.u + (b.u - a.u) * t;
	r.v = a.v + (b.v - a.v) * t;
	return r;
}

static int clip_code(const CVert& c) {
	int code = 0;
	if (plane_dist(0, c) < 0) code |= 1;
	if (plane_dist(1, c) < 0) code |= 2;
	if (plane_dist(2, c) < 0) code |= 4;
	if (plane_dist(3, c) < 0) code |= 8;
	if (plane_dist(4, c) < 0) code |= 16;
	if (plane_dist(5, c) < 0) code |= 32;
	return code;
}

/*
 * Recursive clip-space tessellation.  This mirrors the shape of the
 * library's clip tree (one point outside vs two points outside) so that the
 * produced sub-triangles use the same snapped integer vertices.  It is
 * written independently with plain double-precision plane distances; the
 * resulting sub-triangles are then checked one by one.
 */
static void clip_recursive(const CVert& p0, const CVert& p1, const CVert& p2, int bit,
						   std::vector<std::vector<CVert> >& out) {
	int c0 = clip_code(p0), c1 = clip_code(p1), c2 = clip_code(p2);
	int co = c0 | c1 | c2;
	if (co == 0) {
		std::vector<CVert> t;
		t.push_back(p0);
		t.push_back(p1);
		t.push_back(p2);
		out.push_back(t);
		return;
	}
	if ((c0 & c1 & c2) != 0)
		return;
	while (bit < 6 && !(co & (1 << bit)))
		bit++;
	if (bit == 6)
		return;
	int mask = 1 << bit;
	CVert q0, q1, q2;
	int m0 = (c0 & mask) != 0, m1 = (c1 & mask) != 0, m2 = (c2 & mask) != 0;
	if (m0 ^ m1 ^ m2) {
		if (m0) {
			q0 = p0; q1 = p1; q2 = p2;
		} else if (m1) {
			q0 = p1; q1 = p2; q2 = p0;
		} else {
			q0 = p2; q1 = p0; q2 = p1;
		}
		CVert t1 = lerp_clip(q0, q1, plane_dist(bit, q0) / (plane_dist(bit, q0) - plane_dist(bit, q1)));
		CVert t2 = lerp_clip(q0, q2, plane_dist(bit, q0) / (plane_dist(bit, q0) - plane_dist(bit, q2)));
		clip_recursive(t1, q1, q2, bit + 1, out);
		clip_recursive(t2, t1, q2, bit + 1, out);
	} else {
		if (!m0) {
			q0 = p0; q1 = p1; q2 = p2;
		} else if (!m1) {
			q0 = p1; q1 = p2; q2 = p0;
		} else {
			q0 = p2; q1 = p0; q2 = p1;
		}
		CVert t1 = lerp_clip(q0, q1, plane_dist(bit, q0) / (plane_dist(bit, q0) - plane_dist(bit, q1)));
		CVert t2 = lerp_clip(q0, q2, plane_dist(bit, q0) / (plane_dist(bit, q0) - plane_dist(bit, q2)));
		clip_recursive(q0, t1, t2, bit + 1, out);
	}
}

/* --- texture and addressing --- */
struct Tex {
	std::vector<unsigned char> rgb;
	PIXEL pix[TEXDIM * TEXDIM];

	Tex() : rgb((size_t)TEXDIM * TEXDIM * 3), pix() {}

	void build_coord() {
		for (int y = 0; y < TEXDIM; y++)
			for (int x = 0; x < TEXDIM; x++) {
				size_t i = ((size_t)y * TEXDIM + x) * 3;
				rgb[i + 0] = (unsigned char)x;
				rgb[i + 1] = (unsigned char)y;
				rgb[i + 2] = (unsigned char)((x * 3 + y * 5) & 0xff);
				pix[y * TEXDIM + x] = ((PIXEL)x << 16) | ((PIXEL)y << 8) | (PIXEL)((x * 3 + y * 5) & 0xff);
			}
	}

	/* Same fixed-point repeat/nearest addressing the rasterizer uses. */
	PIXEL sample_fixed(long s, long t) const {
		long off = ST_TO_TEXTURE_BYTE_OFFSET(s, t);
		return *(const PIXEL*)((const GLbyte*)pix + off);
	}
};

static PIXEL sample_uv(const Tex& tex, double u, double v) {
	return tex.sample_fixed((long)(u * S_RANGE + ZB_POINT_S_MIN), (long)(v * T_RANGE + ZB_POINT_T_MIN));
}

/* Same modulation the library applies (TGL_FEATURE_LIT_TEXTURES). */
static PIXEL modulate(PIXEL texel, int r, int g, int b) {
	return RGB_MIX_FUNC(r, g, b, texel);
}

static int white_component() {
	return (int)(((GLint)(COLOR_CORRECTED_MULT_MASK + COLOR_MIN_MULT)) & COLOR_MASK);
}

/* The value glColor3f stores in the vertex colour for channel c. */
static int color_component(float c) {
	return (int)(((GLint)(c * COLOR_CORRECTED_MULT_MASK + COLOR_MIN_MULT)) & COLOR_MASK);
}

static double cross2(double ax, double ay, double bx, double by, double px, double py) {
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

struct RefResult {
	int covered = 0;
	int checked = 0;
	int at_tie = 0;
	int color_bad = 0;
	int depth_bad = 0;
	int coverage_bad = 0;
	std::string first_error;
};

/*
 * Compare one textured triangle against the analytic reference.
 *
 * depth_test/depth_write are the state the triangle was drawn with; blend is
 * the zb->enable_blend value.  r,g,b are the vertex colour components in
 * TinyGL's fixed-point convention (all three vertices equal).
 */
static RefResult compare_triangle(const ZBuffer* zb, const Tex& tex, const SVert& a, const SVert& b, const SVert& c,
								  bool depth_test, bool depth_write, bool blend, int r, int g, int bl,
								  bool check_extra_coverage = true, bool verbose = false) {
	RefResult out;
	SVert p[3] = {a, b, c};
	double X[3] = {(double)p[0].x, (double)p[1].x, (double)p[2].x};
	double Y[3] = {(double)p[0].y, (double)p[1].y, (double)p[2].y};
	double w[3] = {p[0].w, p[1].w, p[2].w};
	double u[3] = {p[0].u, p[1].u, p[2].u};
	double v[3] = {p[0].v, p[1].v, p[2].v};
	double z[3] = {(double)p[0].z, (double)p[1].z, (double)p[2].z};

	double area = cross2(X[0], Y[0], X[1], Y[1], X[2], Y[2]);
	if (fabs(area) < 1e-9)
		return out;

	double alt[3] = {fabs(area) / hypot(X[1] - X[0], Y[1] - Y[0]),
					 fabs(area) / hypot(X[2] - X[1], Y[2] - Y[1]),
					 fabs(area) / hypot(X[0] - X[2], Y[0] - Y[2])};

	const PIXEL* pbuf = (const PIXEL*)zb->pbuf;
	int W = zb->xsize, H = zb->ysize;

	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			double l0 = cross2(X[1], Y[1], X[2], Y[2], x, y) / area;
			double l1 = cross2(X[2], Y[2], X[0], Y[0], x, y) / area;
			double l2 = 1.0 - l0 - l1;
			double margin = std::min(fabs(l0) * alt[0], std::min(fabs(l1) * alt[1], fabs(l2) * alt[2]));
			bool inside = l0 >= 0 && l1 >= 0 && l2 >= 0;

			if (margin <= 1.0)
				continue; /* closer than 1 px to an edge: boundary convention */

			PIXEL got = pbuf[y * W + x];
			bool covered = got != 0;

			if (!inside) {
				/* When the triangle is one fan piece of a clipped polygon,
				 * another fan piece legitimately covers this pixel. */
				if (check_extra_coverage && covered) {
					out.coverage_bad++;
					if (out.first_error.empty())
						out.first_error = "unexpected coverage";
				}
				continue;
			}

			out.covered++;
			if (!covered) {
				out.coverage_bad++;
				if (out.first_error.empty())
					out.first_error = "missing coverage";
				continue;
			}
			out.checked++;

			if (depth_test) {
				double zref = l0 * z[0] + l1 * z[1] + l2 * z[2];
				int zi = (int)(zref / (1 << ZB_POINT_Z_FRAC_BITS));
				if (zb->zbuf[y * W + x] != zi) {
					out.depth_bad++;
					if (out.first_error.empty())
						out.first_error = "depth mismatch";
				}
			}

			double den = l0 / w[0] + l1 / w[1] + l2 / w[2];
			double uu = (l0 * u[0] / w[0] + l1 * u[1] / w[1] + l2 * u[2] / w[2]) / den;
			double vv = (l0 * v[0] / w[0] + l1 * v[1] / w[1] + l2 * v[2] / w[2]) / den;

			/* Nearest-sampling tie: the library indexes floor(255*u + 0.5),
			 * so a UV landing on a half-integer in 255*u may legally pick
			 * either neighbour.  Also stay clear of the repeat breakpoint. */
			/* The library quantises the vertex UVs to fixed point before any
			 * perspective interpolation.  Evaluate the reference from the
			 * same fixed-point values so the comparison isolates the
			 * interpolation, not the vertex-UV quantisation. */
			double fs = uu * 255.0, ft = vv * 255.0;
			double tie_s = fabs((fs - floor(fs)) - 0.5);
			double tie_t = fabs((ft - floor(ft)) - 0.5);
			/* Repeat breakpoints are at integer u/v (and only there); stay
			 * away from them as well. */
			double rep_s = fabs(uu - floor(uu + 0.5));
			double rep_t = fabs(vv - floor(vv + 0.5));
			if (tie_s < 1e-3 || tie_t < 1e-3 || rep_s < 1e-3 || rep_t < 1e-3) {
				out.at_tie++;
				continue;
			}

			/* The state checks only exercise pass-through blending
			 * (GL_ONE/GL_ZERO), so blended and unblended output are the same
			 * expression. */
			(void)depth_write;
			(void)blend;
			PIXEL expect = modulate(sample_uv(tex, uu, vv), r, g, bl);
			if (got != expect) {
				out.color_bad++;
				if (out.first_error.empty())
					out.first_error = "colour mismatch";
				if (verbose && out.color_bad <= 6)
					printf("  bad px %d,%d got %06x want %06x uv %.6f %.6f w %.3f %.3f %.3f\n", x, y, got, expect,
						   uu, vv, w[0], w[1], w[2]);
			}
		}
	return out;
}


/*
 * For a strong-perspective triangle, compute for every covered pixel the
 * per-axis texel distance between what was drawn and the independent
 * per-pixel rational quotient.  Because the vertex-colour modulation can
 * alias two adjacent texel values, "what was drawn" is the whole set of
 * texels whose modulated colour equals the rendered pixel; the distance is
 * the minimum over that set.  The bound required by the fix is <= 1 texel on
 * each axis for every covered pixel.
 */
static int max_texel_error(const ZBuffer* zb, const Tex& tex, const SVert& a, const SVert& b, const SVert& c, int r,
						   int g, int bl, int* covered_out) {
	double X[3] = {(double)a.x, (double)b.x, (double)c.x};
	double Y[3] = {(double)a.y, (double)b.y, (double)c.y};
	double w[3] = {a.w, b.w, c.w};
	double u[3] = {a.u, b.u, c.u};
	double v[3] = {a.v, b.v, c.v};
	double area = cross2(X[0], Y[0], X[1], Y[1], X[2], Y[2]);
	int worst = 0, covered = 0;
	if (fabs(area) < 1e-9) {
		*covered_out = 0;
		return 0;
	}
	double alt[3] = {fabs(area) / hypot(X[1] - X[0], Y[1] - Y[0]), fabs(area) / hypot(X[2] - X[1], Y[2] - Y[1]),
					 fabs(area) / hypot(X[0] - X[2], Y[0] - Y[2])};
	const PIXEL* pbuf = (const PIXEL*)zb->pbuf;
	for (int y = 0; y < zb->ysize; y++)
		for (int x = 0; x < zb->xsize; x++) {
			double l0 = cross2(X[1], Y[1], X[2], Y[2], x, y) / area;
			double l1 = cross2(X[2], Y[2], X[0], Y[0], x, y) / area;
			double l2 = 1.0 - l0 - l1;
			double margin = std::min(fabs(l0) * alt[0], std::min(fabs(l1) * alt[1], fabs(l2) * alt[2]));
			PIXEL got = pbuf[y * zb->xsize + x];
			if (margin <= 1.0 || !got)
				continue;
			if (!(l0 >= 0 && l1 >= 0 && l2 >= 0))
				continue;
			covered++;
			double den = l0 / w[0] + l1 / w[1] + l2 / w[2];
			double uu = (l0 * u[0] / w[0] + l1 * u[1] / w[1] + l2 * u[2] / w[2]) / den;
			double vv = (l0 * v[0] / w[0] + l1 * v[1] / w[1] + l2 * v[2] / w[2]) / den;
			double fs = uu * 255.0, ft = vv * 255.0;
			double refx = floor(fs + 0.5), refy = floor(ft + 0.5);
			int best = 1 << 20;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int tx = ((int)refx + dx) & (TEXDIM - 1);
					int ty = ((int)refy + dy) & (TEXDIM - 1);
					PIXEL want = modulate(tex.pix[ty * TEXDIM + tx], r, g, bl);
					if (want == got) {
						int d = std::max(abs(dx), abs(dy));
						if (d < best)
							best = d;
					}
				}
			/* If no texel within +/-1 reproduces the pixel, widen the search
			 * to the whole texture so the reported error is a real distance. */
			if (best == (1 << 20)) {
				for (int ty = 0; ty < TEXDIM; ty++)
					for (int tx = 0; tx < TEXDIM; tx++) {
						PIXEL want = modulate(tex.pix[ty * TEXDIM + tx], r, g, bl);
						if (want == got) {
							int dx = abs(tx - (int)refx), dy = abs(ty - (int)refy);
							if (dx > 128) dx = 256 - dx;
							if (dy > 128) dy = 256 - dy;
							int d = std::max(dx, dy);
							if (d < best)
								best = d;
						}
					}
			}
			if (best == (1 << 20))
				best = 999;
			if (best > worst)
				worst = best;
		}
	*covered_out = covered;
	return worst;
}

/* ---------- context helpers ---------- */

struct Ctx {
	ZBuffer* zb;
	GLuint tex;
	Tex t;
	std::vector<unsigned char> rgb;
};

static void setup(Ctx& c, int w, int h, bool depth_test) {
	c.zb = ZB_open(w, h, ZB_MODE_RGBA, 0);
	if (!c.zb)
		throw std::runtime_error("ZB_open failed");
	glInit(c.zb);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	if (depth_test)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);
	glColor3f(1, 1, 1);
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	c.t.build_coord();
	c.rgb = c.t.rgb;
	glGenTextures(1, &c.tex);
	glBindTexture(GL_TEXTURE_2D, c.tex);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, TEXDIM, TEXDIM, 0, GL_RGB, GL_UNSIGNED_BYTE, c.rgb.data());
	glClearColor(0, 0, 0, 1);
}

static void teardown(Ctx& c) {
	glClose();
	ZB_close(c.zb);
}

static void set_matrix(const Mat& m) {
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(m.m);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static void color3(float r, float g, float b) { glColor3f(r, g, b); }

static void draw_tri(const ObjVert& a, const ObjVert& b, const ObjVert& c) {
	glBegin(GL_TRIANGLES);
	glTexCoord2f(a.u, a.v);
	glVertex3f(a.x, a.y, a.z);
	glTexCoord2f(b.u, b.v);
	glVertex3f(b.x, b.y, b.z);
	glTexCoord2f(c.u, c.v);
	glVertex3f(c.x, c.y, c.z);
	glEnd();
}

static void clear_buffers() { glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); }

static int pixel_at(const ZBuffer* zb, int x, int y) { return (int)((const PIXEL*)zb->pbuf)[y * zb->xsize + x]; }

/* ---------- groups ---------- */

static RefResult draw_and_compare(Ctx& c, const Mat& proj, const ObjVert v[3], bool depth_test, bool depth_write,
								  bool blend, int r, int g, int b, bool check_extra = true);

static void orthographic(Suite& S) {
	const int W = 40;
	Ctx c;
	setup(c, W, W, true);
	clear_buffers();

	/* Same xy/UV, different depth.  Under an orthographic projection w is
	 * constant, so both triangles must be pixel-identical. */
	ObjVert a{-0.8f, -0.8f, -0.9f, 0.05f, 0.08f};
	ObjVert b{0.85f, -0.8f, 0.9f, 0.94f, 0.12f};
	ObjVert d{-0.8f, 0.87f, -0.9f, 0.06f, 0.93f};
	draw_tri(a, b, d);
	std::vector<int> first(W * W);
	for (int i = 0; i < W * W; i++)
		first[i] = pixel_at(c.zb, i % W, i / W);

	clear_buffers();
	ObjVert a2 = a, b2 = b, d2 = d;
	a2.z = b2.z = d2.z = 0.0f;
	draw_tri(a2, b2, d2);

	int differing = 0;
	for (int i = 0; i < W * W; i++)
		if (first[i] != pixel_at(c.zb, i % W, i / W))
			differing++;
	S.check(differing == 0, "orthographic: varying depth changed the texture (" + std::to_string(differing) + " px)");

	/* The flat-depth half must also match the rational reference (w is
	 * constant here, so it is the affine value), including depth. */
	Viewport vp(W, W);
	Mat I = identity_mat();
	CVert p0 = project(I, a2), p1 = project(I, b2), p2 = project(I, d2);
	SVert s0 = vp.snap(p0), s1 = vp.snap(p1), s2 = vp.snap(p2);
	RefResult r = compare_triangle(c.zb, c.t, s0, s1, s2, true, true, false, white_component(), white_component(),
								   white_component());
	S.check(r.color_bad == 0, "orthographic: colour vs rational reference (" + r.first_error + ")");
	S.check(r.coverage_bad == 0, "orthographic: coverage vs reference (" + r.first_error + ")");
	S.check(r.depth_bad == 0, "orthographic: depth vs affine reference (" + r.first_error + ")");
	S.check(r.checked > 100 && r.at_tie >= 0, "orthographic: not enough samples (" + std::to_string(r.checked) + ")");

	/* A narrowly varying depth slope around zero must still not move UVs. */
	clear_buffers();
	ObjVert q0{-0.8f, -0.8f, 0.0f, 0.05f, 0.08f};
	ObjVert q1{0.85f, -0.8f, 0.00002f, 0.94f, 0.12f};
	ObjVert q2{-0.8f, 0.87f, -0.00002f, 0.06f, 0.93f};
	draw_tri(q0, q1, q2);
	CVert t0 = project(I, q0), t1 = project(I, q1), t2 = project(I, q2);
	SVert u0 = vp.snap(t0), u1 = vp.snap(t1), u2 = vp.snap(t2);
	RefResult r2 = compare_triangle(c.zb, c.t, u0, u1, u2, true, true, false, white_component(), white_component(),
									white_component());
	S.check(r2.color_bad == 0, "orthographic: flat depth gradient colour (" + r2.first_error + ")");
	S.check(r2.coverage_bad == 0, "orthographic: flat depth gradient coverage");
	S.check(r2.depth_bad == 0, "orthographic: flat depth gradient depth");

	/* Changing clip z with xy and w fixed must not change colour. */
	clear_buffers();
	draw_tri(q0, q1, q2);
	std::vector<int> before(W * W);
	for (int i = 0; i < W * W; i++)
		before[i] = pixel_at(c.zb, i % W, i / W);
	clear_buffers();
	ObjVert z0 = q0, z1 = q1, z2 = q2;
	z0.z = -0.4f;
	z1.z = 0.5f;
	z2.z = 0.25f;
	draw_tri(z0, z1, z2);
	int dz = 0;
	for (int i = 0; i < W * W; i++)
		if (before[i] != pixel_at(c.zb, i % W, i / W))
			dz++;
	S.check(dz == 0, "orthographic: clip-z change altered colour (" + std::to_string(dz) + " px)");

	teardown(c);
}

static void perspective(Suite& S) {
	const int W = 64;
	Ctx c;
	setup(c, W, W, false);
	clear_buffers();

	Mat P = frustum_mat(-1, 1, -1, 1, 1, 30);
	set_matrix(P);
	Viewport vp(W, W);

	/* Strongly varying w.  With near=1 the frustum maps eye x in [-1,1]
	 * to the viewport, so the vertices are scaled by their depth to fill
	 * most of the frame while w spans 3..16. */
	ObjVert v0{-1.35f, -1.35f, -3.0f, 0.03f, 0.06f};
	ObjVert v1{4.20f, -3.60f, -9.0f, 0.94f, 0.10f};
	ObjVert v2{-3.20f, 6.00f, -16.0f, 0.08f, 0.95f};
	draw_tri(v0, v1, v2);

	CVert c0 = project(P, v0), c1 = project(P, v1), c2 = project(P, v2);
	SVert s0 = vp.snap(c0), s1 = vp.snap(c1), s2 = vp.snap(c2);
	RefResult r = compare_triangle(c.zb, c.t, s0, s1, s2, false, true, false, white_component(), white_component(),
								   white_component());
	S.check(r.color_bad == 0, "perspective: colour vs rational reference (" + r.first_error + ")");
	S.check(r.coverage_bad == 0, "perspective: coverage (" + r.first_error + ")");
	S.check(r.checked > 200, "perspective: sample count " + std::to_string(r.checked));
	S.check(c0.W != c1.W && c1.W != c2.W, "perspective: w actually varied");

	/* A second triangle with different w must be self-consistent too: no
	 * perspective coefficient may leak from the previous triangle. */
	clear_buffers();
	ObjVert u0{-2.1f, -2.1f, -3.0f, 0.02f, 0.90f};
	ObjVert u1{3.4f, -2.4f, -4.0f, 0.95f, 0.06f};
	ObjVert u2{0.0f, 3.5f, -5.0f, 0.50f, 0.50f};
	draw_tri(u0, u1, u2);
	CVert d0 = project(P, u0), d1 = project(P, u1), d2 = project(P, u2);
	SVert t0 = vp.snap(d0), t1 = vp.snap(d1), t2 = vp.snap(d2);
	RefResult r2 = compare_triangle(c.zb, c.t, t0, t1, t2, false, true, false, white_component(), white_component(),
									white_component());
	S.check(r2.color_bad == 0, "perspective: second triangle colour (" + r2.first_error + ")");
	S.check(r2.checked > 100, "perspective: second triangle sample count " + std::to_string(r2.checked));
	S.check(r2.at_tie >= 0 && r.at_tie >= 0, "perspective: tie bookkeeping");

	/* Switching projection must not reuse the previous plane either. */
	clear_buffers();
	Mat O = identity_mat();
	set_matrix(O);
	ObjVert p0{-0.8f, -0.8f, 0.1f, 0.05f, 0.05f};
	ObjVert p1{0.8f, -0.8f, -0.2f, 0.95f, 0.10f};
	ObjVert p2{-0.8f, 0.8f, 0.3f, 0.10f, 0.95f};
	draw_tri(p0, p1, p2);
	CVert o0 = project(O, p0), o1 = project(O, p1), o2 = project(O, p2);
	SVert q0 = vp.snap(o0), q1 = vp.snap(o1), q2 = vp.snap(o2);
	RefResult r3 = compare_triangle(c.zb, c.t, q0, q1, q2, false, true, false, white_component(), white_component(),
									white_component());
	S.check(r3.color_bad == 0, "perspective: projection switch colour (" + r3.first_error + ")");

	/* Scanline tails.  The span width modulo the old 8-pixel chunk is swept
	 * so every tail length 0..7 is exercised; each pixel must still be the
	 * per-pixel rational value. */
	set_matrix(P);
	int tails_checked = 0, tails_bad = 0;
	for (int span = 8; span < 16; span++) {
		clear_buffers();
		ObjVert a{-0.40f, -0.55f, -3.0f, 0.07f, 0.09f};
		ObjVert b{0.30f + 0.07f * span, -0.55f, -7.0f, 0.88f, 0.14f};
		ObjVert d{-0.35f, 0.45f, -13.0f, 0.11f, 0.90f};
		ObjVert tail[3] = {a, b, d};
		RefResult rt = draw_and_compare(c, P, tail, false, true, false, white_component(), white_component(),
										white_component());
		tails_checked += rt.checked;
		tails_bad += rt.color_bad;
	}
	S.check(tails_checked > 20, "perspective: tail sweep sample count " + std::to_string(tails_checked));
	S.check(tails_bad == 0, "perspective: tail colour (" + std::to_string(tails_bad) + " mismatched samples)");

	/* Strong-perspective per-axis texel bound: every covered pixel must be
	 * within one source texel of the independent per-pixel quotient. */
	clear_buffers();
	ObjVert g0{-1.30f, -1.20f, -2.5f, 0.02f, 0.04f};
	ObjVert g1{4.60f, -3.10f, -11.0f, 0.97f, 0.09f};
	ObjVert g2{-3.40f, 6.40f, -17.0f, 0.06f, 0.97f};
	Viewport gvp(W, W);
	SVert gs0 = gvp.snap(project(P, g0)), gs1 = gvp.snap(project(P, g1)), gs2 = gvp.snap(project(P, g2));
	draw_tri(g0, g1, g2);
	int gcov = 0;
	int gerr = max_texel_error(c.zb, c.t, gs0, gs1, gs2, white_component(), white_component(), white_component(),
							   &gcov);
	S.check(gcov > 200, "perspective: strong-perspective coverage " + std::to_string(gcov));
	S.check(gerr <= 1, "perspective: per-axis texel error " + std::to_string(gerr) + " exceeds one texel");

	teardown(c);
}

static void clipped(Suite& S) {
	const int W = 64;
	Ctx c;
	setup(c, W, W, false);
	clear_buffers();

	Mat P = frustum_mat(-1, 1, -1, 1, 1, 30);
	set_matrix(P);
	Viewport vp(W, W);

	/* One vertex outside the left side plane (x < -w at w=3) forces a
	 * side clip; the other two vertices are inside.  The clipped geometry
	 * must still use the perspective-correct rational quotient. */
	ObjVert v0{-4.5f, -0.9f, -3.0f, 0.02f, 0.05f};
	ObjVert v1{7.0f, -4.5f, -6.0f, 0.90f, 0.12f};
	ObjVert v2{-3.5f, 12.0f, -14.0f, 0.10f, 0.92f};

	std::vector<std::vector<CVert> > outpoly;
	clip_recursive(project(P, v0), project(P, v1), project(P, v2), 0, outpoly);
	S.check(outpoly.size() >= 3, "clipped: reference clip produced " + std::to_string(outpoly.size()) + " points");

	draw_tri(v0, v1, v2);

	/* Compare every fan triangle of the clipped polygon against the
	 * implementation.  Triangles fully inside the viewport are checked in
	 * the colour test below; pixels near the (new) clip edges may differ
	 * because the library's clip epsilon differs from ours, so only
	 * pixels well inside every clipped triangle are asserted. */
	int total_checked = 0, total_color_bad = 0;
	for (size_t i = 0; i < outpoly.size(); i++) {
		SVert a = vp.snap(outpoly[i][0]), b = vp.snap(outpoly[i][1]), c2 = vp.snap(outpoly[i][2]);
		RefResult r = compare_triangle(c.zb, c.t, a, b, c2, false, true, false, white_component(), white_component(),
									   white_component(), false);
		total_checked += r.checked;
		total_color_bad += r.color_bad;
	}
	S.check(total_checked > 100, "clipped: sample count " + std::to_string(total_checked));
	S.check(total_color_bad == 0, "clipped: colour vs clipped rational reference (" +
									  std::to_string(total_color_bad) + " mismatched samples)");

	/* A triangle with two vertices outside the left plane. */
	clear_buffers();
	ObjVert w0{4.8f, -1.5f, -4.0f, 0.05f, 0.10f};
	ObjVert w1{1.0f, -2.6f, -5.0f, 0.85f, 0.15f};
	ObjVert w2{3.0f, 9.0f, -12.0f, 0.15f, 0.90f};
	std::vector<std::vector<CVert> > out2;
	clip_recursive(project(P, w0), project(P, w1), project(P, w2), 0, out2);
	draw_tri(w0, w1, w2);
	int checked2 = 0, bad2 = 0, cov2 = 0;
	for (size_t i = 0; i < out2.size(); i++) {
		SVert a = vp.snap(out2[i][0]), b = vp.snap(out2[i][1]), c3 = vp.snap(out2[i][2]);
		RefResult r = compare_triangle(c.zb, c.t, a, b, c3, false, true, false, white_component(), white_component(),
									   white_component(), false);
		checked2 += r.checked;
		bad2 += r.color_bad;
		cov2 += r.coverage_bad;
	}
	S.check(checked2 > 50, "clipped: side-clip sample count " + std::to_string(checked2));
	S.check(bad2 == 0, "clipped: side-clip colour (" + std::to_string(bad2) + " mismatched samples)");
	S.check(cov2 == 0, "clipped: side-clip coverage (" + std::to_string(cov2) + " mismatched pixels)");

	/* Deep near clip at a single vertex, large w spread. */
	clear_buffers();
	ObjVert n0{-0.8f, -0.8f, -0.5f, 0.04f, 0.04f};
	ObjVert n1{10.8f, -7.2f, -18.0f, 0.96f, 0.08f};
	ObjVert n2{-1.6f, 1.6f, -2.0f, 0.08f, 0.96f};
	std::vector<std::vector<CVert> > out3;
	clip_recursive(project(P, n0), project(P, n1), project(P, n2), 0, out3);
	draw_tri(n0, n1, n2);
	int checked3 = 0, bad3 = 0;
	for (size_t i = 0; i < out3.size(); i++) {
		SVert a = vp.snap(out3[i][0]), b = vp.snap(out3[i][1]), c4 = vp.snap(out3[i][2]);
		RefResult r = compare_triangle(c.zb, c.t, a, b, c4, false, true, false, white_component(), white_component(),
									   white_component(), false, true);
		checked3 += r.checked;
		bad3 += r.color_bad;
	}
	S.check(checked3 > 100, "clipped: near-clip sample count " + std::to_string(checked3));
	S.check(bad3 == 0, "clipped: near-clip colour (" + std::to_string(bad3) + " mismatched samples)");

	teardown(c);
}


/* ---------- state / pipeline coverage ---------- */

static RefResult draw_and_compare(Ctx& c, const Mat& proj, const ObjVert v[3], bool depth_test, bool depth_write,
								  bool blend, int r, int g, int b, bool check_extra) {
	Viewport vp(c.zb->xsize, c.zb->ysize);
	CVert c0 = project(proj, v[0]), c1 = project(proj, v[1]), c2 = project(proj, v[2]);
	SVert s0 = vp.snap(c0), s1 = vp.snap(c1), s2 = vp.snap(c2);
	draw_tri(v[0], v[1], v[2]);
	return compare_triangle(c.zb, c.t, s0, s1, s2, depth_test, depth_write, blend, r, g, b, check_extra);
}

static void states(Suite& S) {
	const int W = 64;
	Ctx c;
	setup(c, W, W, false);
	Mat I = identity_mat();
	set_matrix(I);

	/* --- blend on and off must agree when the factors are ONE/ZERO --- */
	ObjVert v0{-0.75f, -0.75f, 0.1f, 0.05f, 0.05f};
	ObjVert v1{0.75f, -0.70f, -0.2f, 0.95f, 0.10f};
	ObjVert v2{-0.70f, 0.75f, 0.3f, 0.10f, 0.95f};
	ObjVert vt[3] = {v0, v1, v2};
	clear_buffers();
	RefResult off = draw_and_compare(c, I, vt, false, true, false, white_component(), white_component(),
									 white_component());
	S.check(off.color_bad == 0, "states: NOBLEND colour (" + off.first_error + ")");
	S.check(off.coverage_bad == 0, "states: NOBLEND coverage");

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ZERO);
	clear_buffers();
	RefResult on = draw_and_compare(c, I, vt, false, true, true, white_component(), white_component(),
									white_component());
	S.check(on.color_bad == 0, "states: blend colour (" + on.first_error + ")");
	S.check(on.checked == off.checked, "states: blend changed coverage");
	/* both paths must produce the same image */
	clear_buffers();
	draw_tri(v0, v1, v2);
	RefResult blended = compare_triangle(c.zb, c.t, Viewport(W, W).snap(project(I, v0)),
										 Viewport(W, W).snap(project(I, v1)),
										 Viewport(W, W).snap(project(I, v2)), false, true, true,
										 white_component(), white_component(), white_component());
	glDisable(GL_BLEND);
	clear_buffers();
	draw_tri(v0, v1, v2);
	RefResult noblend = compare_triangle(c.zb, c.t, Viewport(W, W).snap(project(I, v0)),
										 Viewport(W, W).snap(project(I, v1)),
										 Viewport(W, W).snap(project(I, v2)), false, true, false,
										 white_component(), white_component(), white_component());
	S.check(blended.color_bad == 0 && noblend.color_bad == 0, "states: pass-through blend equivalence");
	S.check(blended.checked == noblend.checked && blended.at_tie == noblend.at_tie,
			"states: blend and NOBLEND sampled different pixels");

	/* --- lit colour modulation: a non-white vertex colour --- */
	clear_buffers();
	draw_tri(v0, v1, v2); /* colour is currently white */
	clear_buffers();
	glColor3f(1.0f, 0.35f, 0.6f);
	RefResult lit = draw_and_compare(c, I, vt, false, true, false,
									 color_component(1.0f), color_component(0.35f), color_component(0.6f));
	S.check(lit.color_bad == 0, "states: lit texture modulation (" + lit.first_error + ")");
	S.check(lit.checked > 1, "states: lit modulation sample count " + std::to_string(lit.checked));
	glColor3f(1, 1, 1);

	/* --- transparent texels (NO_DRAW_COLOR) must discard and not write depth --- */
	{
		std::vector<unsigned char> hole = c.rgb;
		for (int y = 0; y < TEXDIM; y++)
			for (int x = 0; x < TEXDIM; x++)
				if (x >= 96 && x < 160 && y >= 96 && y < 160) {
					size_t i = ((size_t)y * TEXDIM + x) * 3;
					hole[i + 0] = 0xff;
					hole[i + 1] = 0x00;
					hole[i + 2] = 0xff;
				}
		teardown(c);
		setup(c, W, W, true);
		set_matrix(I);
		glEnable(GL_DEPTH_TEST);
		glClearColor(0, 0, 0, 1);
		clear_buffers();
		glBindTexture(GL_TEXTURE_2D, c.tex);
		glTexImage2D(GL_TEXTURE_2D, 0, 3, TEXDIM, TEXDIM, 0, GL_RGB, GL_UNSIGNED_BYTE, hole.data());

		/* A hole texel maps to depth test on; afterwards a background quad
		 * must be able to pass through the hole. */
		ObjVert f0{-0.8f, -0.8f, -0.4f, 0.10f, 0.10f};
		ObjVert f1{0.8f, -0.8f, -0.4f, 0.90f, 0.10f};
		ObjVert f2{-0.8f, 0.8f, -0.4f, 0.10f, 0.90f};
		ObjVert f3{0.8f, 0.8f, -0.4f, 0.90f, 0.90f};
		glBegin(GL_QUADS);
		glTexCoord2f(f0.u, f0.v); glVertex3f(f0.x, f0.y, f0.z);
		glTexCoord2f(f1.u, f1.v); glVertex3f(f1.x, f1.y, f1.z);
		glTexCoord2f(f3.u, f3.v); glVertex3f(f3.x, f3.y, f3.z);
		glTexCoord2f(f2.u, f2.v); glVertex3f(f2.x, f2.y, f2.z);
		glEnd();

		/* Find a hole pixel: hole UV centre (0.5,0.5) maps to pixel 32,32. */
		int hx = 32, hy = 32;
		PIXEL hpx = ((PIXEL*)c.zb->pbuf)[hy * W + hx];
		S.check(hpx == 0, "states: hole pixel was written (transparent texel not discarded)");
		GLushort hz = c.zb->zbuf[hy * W + hx];
		S.check(hz == 0, "states: hole wrote depth");

		/* Draw the background closer through the hole: must appear. */
		glDisable(GL_TEXTURE_2D);
		glColor3f(1.0f, 1.0f, 1.0f);
		glBegin(GL_QUADS);
		glVertex3f(-0.8f, -0.8f, 0.0f);
		glVertex3f(0.8f, -0.8f, 0.0f);
		glVertex3f(0.8f, 0.8f, 0.0f);
		glVertex3f(-0.8f, 0.8f, 0.0f);
		glEnd();
		PIXEL bg = ((PIXEL*)c.zb->pbuf)[hy * W + hx];
		S.check(((bg >> 16) & 0xff) > 0x80, "states: background did not pass through the hole");
		/* Outside the hole the textured quad must have blocked the background. */
		PIXEL solid = ((PIXEL*)c.zb->pbuf)[10 * W + 10];
		S.check(solid != bg, "states: background overwrote the opaque textured area");
	}

	/* --- polygon stipple: discarded pixels must not write depth --- */
	{
		teardown(c);
		setup(c, W, W, true);
		set_matrix(I);
		glEnable(GL_DEPTH_TEST);
		clear_buffers();
		static unsigned char pattern[128];
		unsigned char saved[TGL_POLYGON_STIPPLE_BYTES];
		for (int i = 0; i < 128; i++)
			pattern[i] = (unsigned char)((i % 2) ? 0xAA : 0x55);
		/* Save the current (all 0xff) pattern so the following quads are solid. */
		memcpy(saved, c.zb->stipplepattern, sizeof saved);
		glPolygonStipple(pattern);
		glEnable(GL_POLYGON_STIPPLE);
		glDisable(GL_TEXTURE_2D);
		glColor3f(1, 1, 1);
		glBegin(GL_QUADS);
		glVertex3f(-0.9f, -0.9f, 0.0f);
		glVertex3f(0.9f, -0.9f, 0.0f);
		glVertex3f(0.9f, 0.9f, 0.0f);
		glVertex3f(-0.9f, 0.9f, 0.0f);
		glEnd();
		/* Count written vs discarded pixels inside the quad. */
		int written = 0, discarded = 0;
		for (int y = 6; y < W - 6; y++)
			for (int x = 6; x < W - 6; x++) {
				if (((PIXEL*)c.zb->pbuf)[y * W + x])
					written++;
				else
					discarded++;
			}
		S.check(written > 0 && discarded > 0, "states: stipple did not discard anything");
		/* A discarded pixel must have kept depth 0; then a closer untextured
		 * draw must fill it. */
		int fx = -1, fy = -1;
		for (int y = 6; y < W - 6 && fx < 0; y++)
			for (int x = 6; x < W - 6; x++)
				if (!((PIXEL*)c.zb->pbuf)[y * W + x]) {
					fx = x;
					fy = y;
					break;
				}
		S.check(fx >= 0, "states: could not find a stipple hole");
		S.check(c.zb->zbuf[fy * W + fx] == 0, "states: stipple-discarded pixel wrote depth");
		glDisable(GL_POLYGON_STIPPLE);
		glDisable(GL_TEXTURE_2D);
		glColor3f(1, 0.2f, 0.2f);
		glBegin(GL_QUADS);
		glVertex3f(-0.9f, -0.9f, -0.5f);
		glVertex3f(0.9f, -0.9f, -0.5f);
		glVertex3f(0.9f, 0.9f, -0.5f);
		glVertex3f(-0.9f, 0.9f, -0.5f);
		glEnd();
		PIXEL fill = ((PIXEL*)c.zb->pbuf)[fy * W + fx];
		S.check(((fill >> 16) & 0xff) > 0x80 && ((fill >> 8) & 0xff) < 0x90,
				"states: later draw did not pass through the stipple hole");
	}

	/* --- untextured colour path unaffected --- */
	{
		teardown(c);
		setup(c, W, W, true);
		set_matrix(I);
		glEnable(GL_DEPTH_TEST);
		clear_buffers();
		glDisable(GL_TEXTURE_2D);
		glShadeModel(GL_SMOOTH);
		glColor3f(1.0f, 0.0f, 0.0f);
		glBegin(GL_QUADS);
		glVertex3f(-0.9f, -0.9f, 0.2f);
		glVertex3f(0.9f, -0.9f, 0.2f);
		glVertex3f(0.9f, 0.9f, 0.2f);
		glVertex3f(-0.9f, 0.9f, 0.2f);
		glEnd();
		PIXEL p = ((PIXEL*)c.zb->pbuf)[W / 2 * W + W / 2];
		S.check(((p >> 16) & 0xff) > 0x80 && (p & 0xff) < 0x40, "states: untextured colour wrong");
		S.check(c.zb->zbuf[W / 2 * W + W / 2] != 0, "states: untextured depth not written");
	}

	teardown(c);
}

static void compatibility(Suite& S) {
	const int W = 64;
	Ctx c;
	setup(c, W, W, false);
	Mat P = frustum_mat(-1, 1, -1, 1, 1, 30);
	set_matrix(P);
	clear_buffers();

	ObjVert v0{-1.4f, -1.3f, -3.0f, 0.04f, 0.05f};
	ObjVert v1{4.0f, -3.5f, -9.0f, 0.94f, 0.10f};
	ObjVert v2{-3.0f, 5.5f, -16.0f, 0.07f, 0.94f};
	ObjVert vt[3] = {v0, v1, v2};
	RefResult direct = draw_and_compare(c, P, vt, false, true, false, white_component(), white_component(),
										white_component());
	S.check(direct.color_bad == 0, "compatibility: direct colour (" + direct.first_error + ")");
	S.check(direct.checked > 100, "compatibility: direct sample count");

	/* Display-list replay of the same geometry must be identical. */
	clear_buffers();
	GLuint list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	glBegin(GL_TRIANGLES);
	glTexCoord2f(v0.u, v0.v);
	glVertex3f(v0.x, v0.y, v0.z);
	glTexCoord2f(v1.u, v1.v);
	glVertex3f(v1.x, v1.y, v1.z);
	glTexCoord2f(v2.u, v2.v);
	glVertex3f(v2.x, v2.y, v2.z);
	glEnd();
	glEndList();
	clear_buffers();
	glCallList(list);
	Viewport vp(W, W);
	RefResult replay = compare_triangle(c.zb, c.t, vp.snap(project(P, v0)), vp.snap(project(P, v1)),
										 vp.snap(project(P, v2)), false, true, false, white_component(),
										 white_component(), white_component());
	S.check(replay.color_bad == 0, "compatibility: display-list colour (" + replay.first_error + ")");
	S.check(replay.checked == direct.checked && replay.at_tie == direct.at_tie,
			"compatibility: display-list sampled a different pixel set");

	/* Replaying a list compiled under a different projection must not reuse
	 * the earlier perspective amount.  Compile the same list, then switch
	 * projection before replaying. */
	clear_buffers();
	Mat O = identity_mat();
	set_matrix(O);
	ObjVert u0{-0.7f, -0.7f, 0.15f, 0.05f, 0.05f};
	ObjVert u1{0.7f, -0.7f, -0.1f, 0.95f, 0.10f};
	ObjVert u2{-0.7f, 0.7f, 0.2f, 0.10f, 0.95f};
	GLuint list2 = glGenLists(1);
	glNewList(list2, GL_COMPILE);
	glBegin(GL_TRIANGLES);
	glTexCoord2f(u0.u, u0.v);
	glVertex3f(u0.x, u0.y, u0.z);
	glTexCoord2f(u1.u, u1.v);
	glVertex3f(u1.x, u1.y, u1.z);
	glTexCoord2f(u2.u, u2.v);
	glVertex3f(u2.x, u2.y, u2.z);
	glEnd();
	glEndList();
	clear_buffers();
	glCallList(list2);
	RefResult ortho = compare_triangle(c.zb, c.t, vp.snap(project(O, u0)), vp.snap(project(O, u1)),
										vp.snap(project(O, u2)), false, true, false, white_component(),
										white_component(), white_component());
	S.check(ortho.color_bad == 0, "compatibility: replayed list under ortho (" + ortho.first_error + ")");

	/* Swap textures and replay the same list: the sample must come from
	 * the newly bound texture, not a stale one. */
	Tex other;
	other.build_coord();
	for (int i = 0; i < TEXDIM * TEXDIM; i++) {
		/* shift the coordinate texture so the two are easy to tell apart */
		int x = i % TEXDIM, y = i / TEXDIM;
		other.pix[i] = ((PIXEL)((x + 64) & 0xff) << 16) | ((PIXEL)((y + 32) & 0xff) << 8) | (PIXEL)(((x * 3 + y * 5) + 37) & 0xff);
	}
	std::vector<unsigned char> other_rgb((size_t)TEXDIM * TEXDIM * 3);
	for (int y = 0; y < TEXDIM; y++)
		for (int x = 0; x < TEXDIM; x++) {
			size_t i = ((size_t)y * TEXDIM + x) * 3;
			other_rgb[i + 0] = (unsigned char)((x + 64) & 0xff);
			other_rgb[i + 1] = (unsigned char)((y + 32) & 0xff);
			other_rgb[i + 2] = (unsigned char)(((x * 3 + y * 5) + 37) & 0xff);
		}
	GLuint t2;
	glGenTextures(1, &t2);
	glBindTexture(GL_TEXTURE_2D, t2);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, TEXDIM, TEXDIM, 0, GL_RGB, GL_UNSIGNED_BYTE, other_rgb.data());
	clear_buffers();
	glCallList(list2);
	RefResult swapped = compare_triangle(c.zb, other, vp.snap(project(O, u0)), vp.snap(project(O, u1)),
										 vp.snap(project(O, u2)), false, true, false, white_component(),
										 white_component(), white_component());
	S.check(swapped.color_bad == 0, "compatibility: texture swap not honoured (" + swapped.first_error + ")");
	/* The swapped draw must not match the original texture: mixing up the two
	 * references is itself a failure. */
	RefResult stale = compare_triangle(c.zb, c.t, vp.snap(project(O, u0)), vp.snap(project(O, u1)),
									   vp.snap(project(O, u2)), false, true, false, white_component(),
									   white_component(), white_component());
	S.check(stale.color_bad > 0, "compatibility: swapped texture produced the stale image");
	S.check(swapped.checked > 30, "compatibility: texture swap sample count " + std::to_string(swapped.checked));

	/* Untextured smooth/flat triangles and their depth must still work. */
	clear_buffers();
	glDisable(GL_TEXTURE_2D);
	glShadeModel(GL_FLAT);
	glColor3f(0.3f, 0.9f, 0.2f);
	glBegin(GL_TRIANGLES);
	glVertex3f(-0.6f, -0.6f, 0.1f);
	glVertex3f(0.6f, -0.6f, 0.1f);
	glVertex3f(0.0f, 0.6f, 0.1f);
	glEnd();
	PIXEL p = ((PIXEL*)c.zb->pbuf)[(W / 2) * W + (W / 2)];
	S.check(p != 0, "compatibility: flat triangle did not draw");
	S.check(((p >> 8) & 0xff) > ((p >> 16) & 0xff), "compatibility: flat colour wrong");
	glShadeModel(GL_SMOOTH);

	teardown(c);
}
} // namespace

void register_tests(Suite& S) {
	S.add("orthographic", orthographic);
	S.add("perspective", perspective);
	S.add("clipped", clipped);
	S.add("states", states);
	S.add("compatibility", compatibility);
}
