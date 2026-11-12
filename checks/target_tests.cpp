// Tests for pglConservativeRaster (conservative filled-triangle rasterization).
//
// The reference model below is an independent double-precision
// Sutherland-Hodgman polygon clipper + shoelace area-moment integrator.
// It reproduces the exact coverage set, per-pixel overlap centroid and
// minimum window depth that the implementation must produce, so the tests
// can compare precise coverage sets, fragment counts, centroids, varyings
// and nearest depth rather than fuzzy pixel dumps.

#include "portablegl.h"
#include "suite.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// fragment recording
// ---------------------------------------------------------------------------

struct FragRec {
	int x, y;                 // pixel (derived as floor of gl_FragCoord.xy)
	float fx, fy, fz, fw;     // gl_FragCoord
	float v[4];               // fs_input
};

static std::vector<FragRec> g_frags;

static void rec_frag(Shader_Builtins* b, const float* in, int n)
{
	FragRec r;
	r.x = (int)std::floor(b->gl_FragCoord.x);
	r.y = (int)std::floor(b->gl_FragCoord.y);
	r.fx = b->gl_FragCoord.x;
	r.fy = b->gl_FragCoord.y;
	r.fz = b->gl_FragCoord.z;
	r.fw = b->gl_FragCoord.w;
	for (int i = 0; i < 4; ++i)
		r.v[i] = (i < n) ? in[i] : 0.0f;
	g_frags.push_back(r);
}

static void fs_plain(float* in, Shader_Builtins* b, void*)
{
	rec_frag(b, in, 0);
	b->gl_FragColor = make_v4(1, 0, 0, 1);
}

static void fs_vary4(float* in, Shader_Builtins* b, void*)
{
	rec_frag(b, in, 4);
	b->gl_FragColor = make_v4(1, 0, 0, 1);
}

static void fs_flat_color(float* in, Shader_Builtins* b, void*)
{
	rec_frag(b, in, 4);
	if (in[2] < 15.0f)
		b->gl_FragColor = make_v4(0, 0, 1, 1);
	else
		b->gl_FragColor = make_v4(1, 0, 0, 1);
}

static void fs_blend_half(float* in, Shader_Builtins* b, void*)
{
	rec_frag(b, in, 0);
	b->gl_FragColor = make_v4(1, 0, 0, 0.5f);
}

static void fs_discard_left(float* in, Shader_Builtins* b, void* u)
{
	float cut = *(float*)u;
	rec_frag(b, in, 0);   // count the call even when discarding
	if (b->gl_FragCoord.x < cut) {
		b->discard = GL_TRUE;
		return;
	}
	b->gl_FragColor = make_v4(1, 0, 0, 1);
}

static void fs_background(float*, Shader_Builtins* b, void*)
{
	b->gl_FragColor = make_v4(0, 0, 1, 1);
}

static void vs_passthrough(float*, vec4* a, Shader_Builtins* b, void*)
{
	b->gl_Position = a[0];
}

static void vs_vary4(float* out, vec4* a, Shader_Builtins* b, void*)
{
	b->gl_Position = a[0];
	out[0] = a[1].x;
	out[1] = a[1].y;
	out[2] = a[1].z;
	out[3] = a[1].w;
}

// ---------------------------------------------------------------------------
// fixture / draw helpers
// ---------------------------------------------------------------------------

struct Fix {
	glContext ctx;
	std::vector<pix_t> fb;
	int w, h;
	Fix(int w_, int h_) : w(w_), h(h_), fb((size_t)w_ * h_)
	{
		pix_t* p = fb.data();
		if (!init_glContext(&ctx, &p, w_, h_))
			throw std::runtime_error("init_glContext failed");
		set_glContext(&ctx);
	}
	~Fix() { free_glContext(&ctx); }
	pix_t pix(int x, int y) const { return fb[(h - 1 - y) * w + x]; }
};

static void clear_log() { g_frags.clear(); }

static void draw_tris(const float* pos, int nverts, GLenum mode, const float* aux = 0)
{
	GLuint vbo[2] = { 0, 0 };
	glGenBuffers(aux ? 2 : 1, vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
	glBufferData(GL_ARRAY_BUFFER, nverts * 4 * sizeof(float), pos, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
	glEnableVertexAttribArray(0);
	if (aux) {
		glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
		glBufferData(GL_ARRAY_BUFFER, nverts * 4 * sizeof(float), aux, GL_STATIC_DRAW);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
		glEnableVertexAttribArray(1);
	}
	glDrawArrays(mode, 0, nverts);
	glDisableVertexAttribArray(0);
	if (aux)
		glDisableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDeleteBuffers(aux ? 2 : 1, vbo);
}

// window coord -> clip coord for the default 16x16 viewport (A = (16-0.01)/2)
static void win2clip(float out[4], double wx, double wy, double z_ndc, double w)
{
	double nx = (wx - 7.995) / 7.995;
	double ny = (wy - 7.995) / 7.995;
	out[0] = (float)(nx * w);
	out[1] = (float)(ny * w);
	out[2] = (float)(z_ndc * w);
	out[3] = (float)w;
}

// Find a clip-space x whose projected window x is *exactly* target, so that
// triangle edges can be placed exactly on pixel boundaries.  Window x depends
// only on clip x and w (vp_mat row 0 is (A,0,0,C)).
static float clip_x_for_window_x(glContext& ctx, float target, float w)
{
	float m0 = ctx.vp_mat[0], m12 = ctx.vp_mat[12];
	float x = (float)(((double)target - (double)m12 * w) / m0);
	float lo = x, hi = x;
	for (long k = 0; k < 8000000; ++k) {
		for (int t = 0; t < 2; ++t) {
			float cand = t ? hi : lo;
			vec4 s = mult_m4_v4(ctx.vp_mat, make_v4(cand, 0.0f, 0.0f, w));
			if ((double)s.x / s.w == (double)target)
				return cand;
		}
		lo = nextafterf(lo, -HUGE_VALF);
		hi = nextafterf(hi, HUGE_VALF);
	}
	throw std::runtime_error("no exact clip x found");
}

// ---------------------------------------------------------------------------
// independent double-precision reference model
// ---------------------------------------------------------------------------

struct RefFrag {
	int x, y;
	double cx, cy;          // overlap centroid (attribute sample point)
	double zmin;            // min window depth over the overlap, [0,1]
	double b0, b1, b2;      // barycentrics at the centroid
};

struct RefScene {
	std::array<std::array<double, 4>, 3> clip;
	double x[3], y[3], z[3], w[3];   // projected original triangle
	double den;
};

using Poly2 = std::vector<std::array<double, 2>>;

static Poly2 ref_clip_half(Poly2 p, int axis, double bound, int sign)
{
	Poly2 out;
	int n = (int)p.size();
	for (int i = 0; i < n; ++i) {
		std::array<double, 2> a = p[i], b = p[(i + 1) % n];
		double ai = axis ? a[1] : a[0];
		double bi = axis ? b[1] : b[0];
		double da = (bound - ai) * sign;
		double db = (bound - bi) * sign;
		if (da >= 0.0) {
			out.push_back(a);
			if (db < 0.0) {
				double t = da / (da - db);
				out.push_back({ a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]) });
			}
		} else if (db >= 0.0) {
			double t = da / (da - db);
			out.push_back({ a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]) });
		}
	}
	return out;
}

// Homogeneous near/far clip; float vertex storage mirrors the pipeline.
static std::vector<std::array<float, 4>> ref_clip_z(std::vector<std::array<float, 4>> p, bool far)
{
	std::vector<std::array<float, 4>> out;
	int n = (int)p.size();
	for (int i = 0; i < n; ++i) {
		std::array<float, 4> a = p[i], b = p[(i + 1) % n];
		double da = far ? (double)a[3] - a[2] : (double)a[2] + a[3];
		double db = far ? (double)b[3] - b[2] : (double)b[2] + b[3];
		if (da >= 0.0) {
			out.push_back(a);
			if (db < 0.0) {
				double t = da / (da - db);
				out.push_back({ (float)(a[0] + t * (b[0] - a[0])), (float)(a[1] + t * (b[1] - a[1])),
				                (float)(a[2] + t * (b[2] - a[2])), (float)(a[3] + t * (b[3] - a[3])) });
			}
		} else if (db >= 0.0) {
			double t = da / (da - db);
			out.push_back({ (float)(a[0] + t * (b[0] - a[0])), (float)(a[1] + t * (b[1] - a[1])),
			                (float)(a[2] + t * (b[2] - a[2])), (float)(a[3] + t * (b[3] - a[3])) });
		}
	}
	return out;
}

static RefScene ref_scene(const float pos[3][4], mat4 vp)
{
	RefScene s;
	for (int i = 0; i < 3; ++i) {
		vec4 cv = make_v4(pos[i][0], pos[i][1], pos[i][2], pos[i][3]);
		vec4 sc = mult_m4_v4(vp, cv);
		vec3 h = v4_to_v3h(sc);
		s.x[i] = h.x;
		s.y[i] = h.y;
		s.z[i] = h.z;
		s.w[i] = sc.w;
		s.clip[i] = { pos[i][0], pos[i][1], pos[i][2], pos[i][3] };
	}
	s.den = (s.y[1] - s.y[2]) * (s.x[0] - s.x[2]) + (s.x[2] - s.x[1]) * (s.y[0] - s.y[2]);
	return s;
}

static void ref_bary(const RefScene& s, double px, double py, double& b0, double& b1, double& b2)
{
	b0 = ((s.y[1] - s.y[2]) * (px - s.x[2]) + (s.x[2] - s.x[1]) * (py - s.y[2])) / s.den;
	b1 = ((s.y[2] - s.y[0]) * (px - s.x[2]) + (s.x[0] - s.x[2]) * (py - s.y[2])) / s.den;
	b2 = 1.0 - b0 - b1;
}

static std::vector<RefFrag> ref_coverage(const RefScene& s, mat4 vp,
                                         double rl, double rb, double rr, double rt)
{
	std::vector<RefFrag> out;
	std::vector<std::array<float, 4>> cp;
	for (int i = 0; i < 3; ++i)
		cp.push_back({ (float)s.clip[i][0], (float)s.clip[i][1], (float)s.clip[i][2], (float)s.clip[i][3] });
	cp = ref_clip_z(cp, false);
	cp = ref_clip_z(cp, true);
	if (cp.size() < 3)
		return out;

	Poly2 poly;
	for (auto& p : cp) {
		vec4 sc = mult_m4_v4(vp, make_v4(p[0], p[1], p[2], p[3]));
		poly.push_back({ (double)sc.x / sc.w, (double)sc.y / sc.w });
	}
	poly = ref_clip_half(poly, 0, rl, -1);
	poly = ref_clip_half(poly, 0, rr, 1);
	poly = ref_clip_half(poly, 1, rb, -1);
	poly = ref_clip_half(poly, 1, rt, 1);
	if (poly.size() < 3)
		return out;

	double minx = poly[0][0], maxx = poly[0][0], miny = poly[0][1], maxy = poly[0][1];
	for (auto& p : poly) {
		minx = std::min(minx, p[0]); maxx = std::max(maxx, p[0]);
		miny = std::min(miny, p[1]); maxy = std::max(maxy, p[1]);
	}
	int ix0 = std::max((int)std::floor(minx), (int)rl);
	int ix1 = std::min((int)std::ceil(maxx), (int)rr);
	int iy0 = std::max((int)std::floor(miny), (int)rb);
	int iy1 = std::min((int)std::ceil(maxy), (int)rt);

	for (int iy = iy0; iy < iy1; ++iy) {
		for (int ix = ix0; ix < ix1; ++ix) {
			Poly2 q = poly;
			q = ref_clip_half(q, 0, ix, -1);
			q = ref_clip_half(q, 0, ix + 1.0, 1);
			q = ref_clip_half(q, 1, iy, -1);
			q = ref_clip_half(q, 1, iy + 1.0, 1);
			if (q.size() < 3)
				continue;
			double a2 = 0, mx = 0, my = 0;
			for (size_t i = 0; i < q.size(); ++i) {
				size_t j = (i + 1) % q.size();
				double cross = q[i][0] * q[j][1] - q[j][0] * q[i][1];
				a2 += cross;
				mx += (q[i][0] + q[j][0]) * cross;
				my += (q[i][1] + q[j][1]) * cross;
			}
			if (std::fabs(a2) * 0.5 <= 1e-12)
				continue;
			RefFrag f;
			f.x = ix;
			f.y = iy;
			f.cx = mx / (3.0 * a2);
			f.cy = my / (3.0 * a2);
			ref_bary(s, f.cx, f.cy, f.b0, f.b1, f.b2);
			double zmin = 0;
			for (size_t i = 0; i < q.size(); ++i) {
				double b0, b1, b2;
				ref_bary(s, q[i][0], q[i][1], b0, b1, b2);
				double z = b0 * s.z[0] + b1 * s.z[1] + b2 * s.z[2];
				if (i == 0 || z < zmin)
					zmin = z;
			}
			f.zmin = (zmin + 1.0) * 0.5;   // default glDepthRange [0,1]
			out.push_back(f);
		}
	}
	return out;
}

// raster rect = viewport ∩ scissor ∩ framebuffer, mirrors pgl_viewport_raster_rect
static void raster_rect(const glContext& ctx, int& rl, int& rb, int& rr, int& rt)
{
	rl = 0; rb = 0;
	rr = ctx.back_buffer.w;
	rt = ctx.back_buffer.h;
	if (ctx.scissor_test) {
		rl = std::max(rl, (int)ctx.scissor_lx);
		rb = std::max(rb, (int)ctx.scissor_ly);
		rr = std::min(rr, (int)(ctx.scissor_lx + ctx.scissor_w));
		rt = std::min(rt, (int)(ctx.scissor_ly + ctx.scissor_h));
	}
	rl = std::max(rl, (int)ctx.xmin);
	rb = std::max(rb, (int)ctx.ymin);
	rr = std::min(rr, (int)(ctx.xmin + ctx.width));
	rt = std::min(rt, (int)(ctx.ymin + ctx.height));
}

static std::map<std::pair<int, int>, int> count_per_pixel(const std::vector<FragRec>& v)
{
	std::map<std::pair<int, int>, int> m;
	for (auto& r : v)
		m[{r.x, r.y}]++;
	return m;
}

static const FragRec* find_frag(const std::vector<FragRec>& v, int x, int y)
{
	for (auto& r : v)
		if (r.x == x && r.y == y)
			return &r;
	return nullptr;
}

// full comparison of the fragment log against the reference coverage
static void check_matches_ref(Suite& s, const std::vector<FragRec>& got,
                              const std::vector<RefFrag>& ref, double tol)
{
	std::map<std::pair<int, int>, const RefFrag*> rm;
	for (auto& f : ref)
		rm[{f.x, f.y}] = &f;
	s.check(got.size() == ref.size(), "fragment count != reference coverage size");
	std::map<std::pair<int, int>, int> seen;
	for (auto& r : got) {
		auto it = rm.find({r.x, r.y});
		s.check(it != rm.end(), "fragment at pixel not in reference coverage");
		if (it == rm.end())
			continue;
		seen[{r.x, r.y}]++;
		s.check(std::fabs(r.fx - it->second->cx) < tol, "centroid x mismatch");
		s.check(std::fabs(r.fy - it->second->cy) < tol, "centroid y mismatch");
		s.check(std::fabs(r.fz - (float)it->second->zmin) < 1e-4f, "min depth mismatch");
	}
	for (auto& f : ref)
		s.check(seen[{f.x, f.y}] == 1, "reference pixel missing or duplicated");
}

// the standard thin sliver: ~0.25px tall, threads between pixel centers
static void make_sliver(float pos[3][4])
{
	win2clip(pos[0], 1.0, 2.10, 0.0, 1.0);
	win2clip(pos[1], 14.0, 2.10, 0.0, 1.0);
	win2clip(pos[2], 7.5, 2.35, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// test groups
// ---------------------------------------------------------------------------

static void test_coverage(Suite& s)
{
	// 1. thin sliver that misses every pixel center: normal mode draws
	//    nothing, conservative mode produces exactly the reference coverage
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		float pos[3][4];
		make_sliver(pos);
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		s.check(ref.size() >= 10, "reference sliver coverage too small");

		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.empty(), "normal mode should miss the sliver entirely");

		pglConservativeRaster(GL_TRUE);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.size() == ref.size(), "conservative coverage size mismatch");
		check_matches_ref(s, g_frags, ref, 1e-5);
	}

	// 2. edge/point contact only: an edge exactly on a pixel boundary and a
	//    vertex exactly on a pixel corner do not generate fragments
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);

		float x3 = clip_x_for_window_x(f.ctx, 3.0f, 1.0f);
		float x7 = clip_x_for_window_x(f.ctx, 7.0f, 1.0f);
		float ya, yb, yc;
		{
			float t[4];
			win2clip(t, 3.0, 1.5, 0.0, 1.0); ya = t[1];
			win2clip(t, 3.0, 6.5, 0.0, 1.0); yb = t[1];
			win2clip(t, 7.0, 4.0, 0.0, 1.0); yc = t[1];
		}
		float pos[3][4] = {
			{ x3, ya, 0.0f, 1.0f },
			{ x3, yb, 0.0f, 1.0f },
			{ x7, yc, 0.0f, 1.0f },
		};
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);

		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		check_matches_ref(s, g_frags, ref, 1e-5);
		s.check(find_frag(g_frags, 2, 3) == nullptr, "edge-only touch produced a fragment");
		s.check(find_frag(g_frags, 7, 4) == nullptr, "point-only touch produced a fragment");
		s.check(find_frag(g_frags, 3, 3) != nullptr, "interior pixel missing");
		s.check(find_frag(g_frags, 6, 4) != nullptr, "interior pixel missing");
	}

	// 3. conservative coverage is a superset of the normal center-sample
	//    coverage for an ordinary triangle
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		float pos[3][4];
		win2clip(pos[0], 2.3, 2.2, 0.0, 1.0);
		win2clip(pos[1], 13.7, 3.1, 0.0, 1.0);
		win2clip(pos[2], 4.5, 12.8, 0.0, 1.0);
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);

		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		std::vector<FragRec> normal = g_frags;
		s.check(normal.size() > 20, "normal coverage unexpectedly small");

		pglConservativeRaster(GL_TRUE);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		check_matches_ref(s, g_frags, ref, 1e-5);
		std::map<std::pair<int, int>, int> cov = count_per_pixel(g_frags);
		for (auto& r : normal)
			s.check(cov.count({r.x, r.y}) == 1, "normal-mode pixel missing from conservative coverage");
		s.check(g_frags.size() >= normal.size(), "conservative coverage smaller than normal");
	}

	// 4. winding invariance with culling off: reversed triangle gives the
	//    same coverage set and the same centroids
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		glDisable(GL_CULL_FACE);
		float pos[3][4];
		make_sliver(pos);
		float rev[3][4];
		std::memcpy(rev[0], pos[0], sizeof rev[0]);
		std::memcpy(rev[1], pos[2], sizeof rev[1]);
		std::memcpy(rev[2], pos[1], sizeof rev[2]);

		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		std::vector<FragRec> fwd = g_frags;
		clear_log();
		draw_tris(&rev[0][0], 3, GL_TRIANGLES);
		s.check(fwd.size() == g_frags.size(), "reversed winding changed coverage size");
		std::map<std::pair<int, int>, const FragRec*> rm;
		for (auto& r : g_frags)
			rm[{r.x, r.y}] = &r;
		for (auto& r : fwd) {
			auto it = rm.find({r.x, r.y});
			s.check(it != rm.end(), "reversed winding changed coverage set");
			if (it != rm.end()) {
				s.check(std::fabs(it->second->fx - r.fx) < 1e-5f, "reversed winding moved centroid x");
				s.check(std::fabs(it->second->fy - r.fy) < 1e-5f, "reversed winding moved centroid y");
			}
		}
	}
}

static void test_clipped(Suite& s)
{
	// 1. near plane clips the triangle to a quad: coverage comes from the
	//    real intersected part, one fragment per pixel even across the fan
	//    diagonal the classic clipper would introduce
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		float pos[3][4] = {
			{ -0.5f, -0.5f, -1.5f, 1.0f },
			{ -0.5f,  0.8f,  0.0f, 1.0f },
			{  0.8f, -0.2f,  0.0f, 1.0f },
		};
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		s.check(ref.size() > 20, "clipped reference coverage too small");

		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		check_matches_ref(s, g_frags, ref, 1e-5);
		std::map<std::pair<int, int>, int> cnt = count_per_pixel(g_frags);
		for (auto& kv : cnt)
			s.check(kv.second == 1, "pixel shaded more than once across fan edge");
		float minz = 2.0f;
		for (auto& r : g_frags)
			minz = std::min(minz, r.fz);
		s.check(minz < 0.02f, "no fragment reaches the near-plane depth");
		s.check(minz >= -1e-6f, "depth below the near plane");
	}

	// 2. far plane clip
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		float pos[3][4] = {
			{ -0.5f, -0.5f, 1.5f, 1.0f },
			{ -0.5f,  0.8f, 0.0f, 1.0f },
			{  0.8f, -0.2f, 0.0f, 1.0f },
		};
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		check_matches_ref(s, g_frags, ref, 1e-5);
		float maxz = -1.0f;
		for (auto& r : g_frags)
			maxz = std::max(maxz, r.fz);
		s.check(maxz > 0.98f, "no fragment reaches the far-plane depth");
		s.check(maxz <= 1.0f + 1e-6f, "depth beyond the far plane");
	}

	// 3. scissor box clips the conservative coverage
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		float pos[3][4];
		win2clip(pos[0], 0.5, 0.5, 0.0, 1.0);
		win2clip(pos[1], 15.0, 1.0, 0.0, 1.0);
		win2clip(pos[2], 8.0, 15.0, 0.0, 1.0);
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> full = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		s.check(full.size() > 60, "unclipped coverage too small");

		glEnable(GL_SCISSOR_TEST);
		glScissor(4, 4, 6, 6);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 4, 4, 10, 10);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		check_matches_ref(s, g_frags, ref, 1e-5);
		s.check(g_frags.size() < full.size(), "scissor did not reduce coverage");
		for (auto& r : g_frags)
			s.check(r.x >= 4 && r.x < 10 && r.y >= 4 && r.y < 10, "fragment outside scissor box");
		s.check(find_frag(g_frags, 3, 3) == nullptr, "fragment leaked outside scissor");
	}

	// 4. small viewport confines conservative coverage
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		glViewport(2, 3, 8, 8);
		float pos[3][4] = {
			{ -1.0f, -1.0f, 0.0f, 1.0f },
			{  3.0f, -1.0f, 0.0f, 1.0f },
			{ -1.0f,  3.0f, 0.0f, 1.0f },
		};
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 2, 3, 10, 11);
		s.check(ref.size() >= 60, "viewport reference coverage too small");
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		check_matches_ref(s, g_frags, ref, 1e-5);
		for (auto& r : g_frags)
			s.check(r.x >= 2 && r.x < 10 && r.y >= 3 && r.y < 11, "fragment outside viewport");
		s.check(find_frag(g_frags, 0, 0) == nullptr, "fragment leaked outside viewport");
	}
}

static void test_interpolation(Suite& s)
{
	// 1. gl_FragCoord.xy is the overlap centroid, inside the pixel square
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		float pos[3][4];
		make_sliver(pos);
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.size() == ref.size(), "coverage size mismatch");
		for (auto& r : g_frags) {
			s.check(r.fx > r.x && r.fx < r.x + 1, "centroid x outside pixel square");
			s.check(r.fy > r.y && r.fy < r.y + 1, "centroid y outside pixel square");
		}
		check_matches_ref(s, g_frags, ref, 1e-5);
	}

	// 2. smooth / noperspective varyings and gl_FragCoord.w at the centroid
	//    with vertices of different w
	{
		Fix f(16, 16);
		GLenum interp[4] = { PGL_SMOOTH, PGL_NOPERSPECTIVE, PGL_FLAT, PGL_SMOOTH };
		GLuint prog = pglCreateProgram(vs_vary4, fs_vary4, 4, interp, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		float pos[3][4], aux[3][4];
		win2clip(pos[0], 2.0, 2.0, 0.0, 1.0);
		win2clip(pos[1], 13.0, 3.0, 0.0, 2.0);
		win2clip(pos[2], 4.0, 13.0, 0.0, 0.5);
		float sval[3] = { 0.25f, 1.0f, 0.5f };
		float nval[3] = { 2.0f, -1.0f, 0.5f };
		for (int i = 0; i < 3; ++i) {
			aux[i][0] = sval[i];
			aux[i][1] = nval[i];
			aux[i][2] = 10.0f * (i + 1);
			aux[i][3] = 0.0f;
		}
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		s.check(ref.size() > 30, "coverage too small for interpolation test");
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES, &aux[0][0]);

		std::map<std::pair<int, int>, const RefFrag*> rm;
		for (auto& rf : ref)
			rm[{rf.x, rf.y}] = &rf;
		double iw[3] = { 1.0 / sc.w[0], 1.0 / sc.w[1], 1.0 / sc.w[2] };
		int checked = 0;
		for (auto& r : g_frags) {
			auto it = rm.find({r.x, r.y});
			s.check(it != rm.end(), "unexpected fragment");
			if (it == rm.end())
				continue;
			const RefFrag& rf = *it->second;
			double wsum = rf.b0 * iw[0] + rf.b1 * iw[1] + rf.b2 * iw[2];
			double smooth = (rf.b0 * sval[0] * iw[0] + rf.b1 * sval[1] * iw[1] + rf.b2 * sval[2] * iw[2]) / wsum;
			double npersp = rf.b0 * nval[0] + rf.b1 * nval[1] + rf.b2 * nval[2];
			s.check(std::fabs(r.v[0] - (float)smooth) < 1e-3f, "smooth varying mismatch at centroid");
			s.check(std::fabs(r.v[1] - (float)npersp) < 1e-3f, "noperspective varying mismatch at centroid");
			s.check(std::fabs(r.fw - (float)wsum) < 1e-3f, "gl_FragCoord.w mismatch at centroid");
			++checked;
		}
		s.check(checked == (int)ref.size(), "not all fragments checked");
	}

	// 3. flat varying comes from the original primitive's provoking vertex,
	//    not from a vertex introduced by clipping (near-clipped quad)
	{
		Fix f(16, 16);
		GLenum interp[4] = { PGL_SMOOTH, PGL_NOPERSPECTIVE, PGL_FLAT, PGL_SMOOTH };
		GLuint prog = pglCreateProgram(vs_vary4, fs_vary4, 4, interp, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		float pos[3][4] = {
			{ -0.5f, -0.5f, -1.5f, 1.0f },
			{ -0.5f,  0.8f,  0.0f, 1.0f },
			{  0.8f, -0.2f,  0.0f, 1.0f },
		};
		float aux[3][4] = {
			{ 0, 0, 11.0f, 0 },
			{ 0, 0, 22.0f, 0 },
			{ 0, 0, 33.0f, 0 },
		};
		// default: GL_LAST_VERTEX_CONVENTION -> vertex 2
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES, &aux[0][0]);
		s.check(g_frags.size() > 20, "clipped coverage too small");
		for (auto& r : g_frags)
			s.check(r.v[2] == 33.0f, "flat varying not from provoking vertex (last)");

		glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES, &aux[0][0]);
		for (auto& r : g_frags)
			s.check(r.v[2] == 11.0f, "flat varying not from provoking vertex (first)");
		glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
	}

	// 4. gl_FragCoord.z is the minimum window depth over the overlap, not
	//    the depth at the centroid; depth buffer gets the same value
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		glEnable(GL_DEPTH_TEST);
		glClear(GL_DEPTH_BUFFER_BIT);
		float pos[3][4];
		win2clip(pos[0], 2.0, 2.0, -0.8, 1.0);
		win2clip(pos[1], 13.0, 3.0, 0.2, 1.0);
		win2clip(pos[2], 4.0, 13.0, 0.6, 1.0);
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		check_matches_ref(s, g_frags, ref, 1e-5);

		std::map<std::pair<int, int>, const RefFrag*> rm;
		for (auto& rf : ref)
			rm[{rf.x, rf.y}] = &rf;
		int below = 0, zbuf_ok = 0;
		for (auto& r : g_frags) {
			const RefFrag& rf = *rm[{r.x, r.y}];
			double zcent = (rf.b0 * sc.z[0] + rf.b1 * sc.z[1] + rf.b2 * sc.z[2] + 1.0) * 0.5;
			s.check(r.fz <= (float)zcent + 1e-4f, "min depth above centroid depth");
			if (r.fz < zcent - 1e-3)
				++below;
			u32 zraw = ((u32*)f.ctx.zbuf.lastrow)[-r.y * 16 + r.x] >> 8;
			double zrec = zraw / (double)0xFFFFFF;
			if (std::fabs(zrec - rf.zmin) < 1e-3)
				++zbuf_ok;
		}
		s.check(below > 10, "depth looks interpolated at the centroid, not minimized");
		s.check(zbuf_ok == (int)g_frags.size(), "depth buffer does not hold the min depth");
	}
}

static void test_fragments(Suite& s)
{
	// 1. one fragment per pixel across the fan diagonal, with depth writes
	//    off and blending on (depth testing cannot hide a duplicate call)
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_blend_half, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_FALSE);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		float pos[3][4] = {
			{ -0.5f, -0.5f, -1.5f, 1.0f },
			{ -0.5f,  0.8f,  0.0f, 1.0f },
			{  0.8f, -0.2f,  0.0f, 1.0f },
		};
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.size() > 20, "coverage too small");
		std::map<std::pair<int, int>, int> cnt = count_per_pixel(g_frags);
		for (auto& kv : cnt)
			s.check(kv.second == 1, "pixel shaded twice across fan edge");
		// a single 0.5-alpha blend over black gives r ~= 127; a double call
		// would give r ~= 191
		const FragRec* r = find_frag(g_frags, 8, 8);
		s.check(r != nullptr, "expected fragment at (8,8)");
		if (r) {
			Color c = PIXEL_TO_COLOR(f.pix(8, 8));
			s.check(c.r > 110 && c.r < 145, "blend result indicates duplicate shading");
		}
	}

	// 2. discard still shades once per pixel; discarded pixels show the
	//    background drawn afterwards
	{
		Fix f(16, 16);
		float cut = 8.0f;
		GLuint prog = pglCreateProgram(vs_passthrough, fs_discard_left, 0, nullptr, GL_TRUE);
		glUseProgram(prog);
		pglSetUniform(&cut);
		pglConservativeRaster(GL_TRUE);
		glEnable(GL_DEPTH_TEST);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		float pos[3][4];
		win2clip(pos[0], 0.5, 0.5, 0.0, 1.0);
		win2clip(pos[1], 15.5, 0.5, 0.0, 1.0);
		win2clip(pos[2], 8.0, 15.5, 0.0, 1.0);
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.size() == ref.size(), "discard changed the fragment call count");
		std::map<std::pair<int, int>, int> cnt = count_per_pixel(g_frags);
		for (auto& kv : cnt)
			s.check(kv.second == 1, "discarded pixel shaded twice");

		// background behind (depth 0.75 vs 0.5 for the triangle)
		GLuint bg = pglCreateProgram(vs_passthrough, fs_background, 0, nullptr, GL_FALSE);
		glUseProgram(bg);
		float back[3][4] = {
			{ -1.0f, -1.0f, 0.5f, 1.0f },
			{  3.0f, -1.0f, 0.5f, 1.0f },
			{ -1.0f,  3.0f, 0.5f, 1.0f },
		};
		draw_tris(&back[0][0], 3, GL_TRIANGLES);
		Color kept = PIXEL_TO_COLOR(f.pix(10, 2));   // centroid x ~10.5 >= cut
		Color dropped = PIXEL_TO_COLOR(f.pix(2, 2)); // centroid x ~2.5 < cut
		s.check(kept.r > 200 && kept.b < 60, "kept fragment was overwritten by background");
		s.check(dropped.b > 200 && dropped.r < 60, "discarded fragment did not show background");
	}

	// 3. stencil rejection runs before shading
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		glClearColor(0, 1, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		float pos[3][4];
		make_sliver(pos);
		RefScene sc = ref_scene(pos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);

		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_NEVER, 0, 0xFF);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.empty(), "stencil-rejected fragments were shaded");
		Color c = PIXEL_TO_COLOR(f.pix(7, 2));
		s.check(c.g > 200 && c.r < 60, "stencil-rejected fragment was written");

		glStencilFunc(GL_ALWAYS, 0, 0xFF);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		check_matches_ref(s, g_frags, ref, 1e-5);
	}

	// 4. two different primitives both shade their shared pixel (no
	//    cross-primitive dedup), as lists, strips and indexed draws
	{
		Fix f(16, 16);
		GLenum interp[4] = { PGL_SMOOTH, PGL_NOPERSPECTIVE, PGL_FLAT, PGL_SMOOTH };
		GLuint prog = pglCreateProgram(vs_vary4, fs_flat_color, 4, interp, GL_FALSE);
		glUseProgram(prog);
		pglConservativeRaster(GL_TRUE);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);

		// crossing slivers as a 2-triangle list
		float pos[6][4], aux[6][4] = {{0}};
		win2clip(pos[0], 1.0, 2.10, 0.0, 1.0);
		win2clip(pos[1], 14.0, 2.10, 0.0, 1.0);
		win2clip(pos[2], 7.5, 2.35, 0.0, 1.0);
		win2clip(pos[3], 5.10, 1.0, 0.0, 1.0);
		win2clip(pos[4], 5.10, 14.0, 0.0, 1.0);
		win2clip(pos[5], 5.35, 7.5, 0.0, 1.0);
		clear_log();
		draw_tris(&pos[0][0], 6, GL_TRIANGLES, &aux[0][0]);
		std::map<std::pair<int, int>, int> cnt = count_per_pixel(g_frags);
		s.check(cnt[{5, 2}] == 2, "shared pixel not shaded by both primitives");
		s.check(cnt[{6, 2}] == 1, "unshared pixel shaded twice");
		s.check(cnt[{5, 7}] == 1, "second primitive coverage missing");

		// triangle strip: two primitives, flat values must follow each
		// primitive's own provoking vertex and draw order must be kept
		float spos[4][4], saux[4][4] = {{0}};
		win2clip(spos[0], 2.0, 2.0, 0.0, 1.0);
		win2clip(spos[1], 8.0, 2.0, 0.0, 1.0);
		win2clip(spos[2], 2.0, 8.0, 0.0, 1.0);
		win2clip(spos[3], 8.0, 8.0, 0.0, 1.0);
		saux[2][2] = 10.0f;  // provoking vertex of prim 0 (last convention)
		saux[3][2] = 20.0f;  // provoking vertex of prim 1
		clear_log();
		draw_tris(&spos[0][0], 4, GL_TRIANGLE_STRIP, &saux[0][0]);
		std::vector<FragRec> at44;
		for (auto& r : g_frags)
			if (r.x == 4 && r.y == 5)   // diagonal x+y=10 cuts this pixel in two
				at44.push_back(r);
		s.check(at44.size() == 2, "strip prims did not both shade the shared pixel");
		if (at44.size() == 2) {
			s.check(at44[0].v[2] == 10.0f, "prim 0 flat value wrong or out of order");
			s.check(at44[1].v[2] == 20.0f, "prim 1 flat value wrong or out of order");
		}
		Color c = PIXEL_TO_COLOR(f.pix(4, 5));
		s.check(c.r > 200 && c.b < 60, "later strip prim did not win the pixel");

		// triangle fan: same per-primitive provoking/order requirements
		float fpos[4][4], faux[4][4] = {{0}};
		win2clip(fpos[0], 2.0, 2.0, 0.0, 1.0);
		win2clip(fpos[1], 8.0, 2.0, 0.0, 1.0);
		win2clip(fpos[2], 8.0, 8.0, 0.0, 1.0);
		win2clip(fpos[3], 2.0, 8.0, 0.0, 1.0);
		faux[2][2] = 10.0f;  // provoking vertex of prim 0 (last convention)
		faux[3][2] = 20.0f;  // provoking vertex of prim 1
		clear_log();
		draw_tris(&fpos[0][0], 4, GL_TRIANGLE_FAN, &faux[0][0]);
		std::vector<FragRec> fan44;
		for (auto& r : g_frags)
			if (r.x == 4 && r.y == 4)   // diagonal y=x cuts this pixel in two
				fan44.push_back(r);
		s.check(fan44.size() == 2, "fan prims did not both shade the shared pixel");
		if (fan44.size() == 2) {
			s.check(fan44[0].v[2] == 10.0f, "fan prim 0 flat value wrong or out of order");
			s.check(fan44[1].v[2] == 20.0f, "fan prim 1 flat value wrong or out of order");
		}

		// indexed draw of the sliver still rasterizes conservatively
		GLuint p2 = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(p2);
		float ipos[3][4];
		make_sliver(ipos);
		unsigned short idx[3] = { 0, 1, 2 };
		RefScene sc = ref_scene(ipos, f.ctx.vp_mat);
		std::vector<RefFrag> ref = ref_coverage(sc, f.ctx.vp_mat, 0, 0, 16, 16);
		clear_log();
		GLuint vbo;
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof ipos, ipos, GL_STATIC_DRAW);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
		glEnableVertexAttribArray(0);
		glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
		glDisableVertexAttribArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDeleteBuffers(1, &vbo);
		check_matches_ref(s, g_frags, ref, 1e-5);
	}
}

static void test_compatibility(Suite& s)
{
	// 1. normal mode is unchanged: the classic 15-fragment starting point
	{
		Fix f(8, 8);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		float p[] = { -.75f, -.75f, 0, 1, .75f, -.75f, 0, 1, -.75f, .75f, 0, 1 };
		GLuint vbo;
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(p), p, GL_STATIC_DRAW);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
		glEnableVertexAttribArray(0);
		glEnable(GL_DEPTH_TEST);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		clear_log();
		glDrawArrays(GL_TRIANGLES, 0, 3);
		s.check(g_frags.size() == 15, "normal mode no longer produces 15 fragments");
		for (int i = 2; i < 12; i += 4)
			p[i] = .8f;
		glBufferData(GL_ARRAY_BUFFER, sizeof(p), p, GL_STATIC_DRAW);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		s.check(g_frags.size() == 15, "occluded fragments were shaded");
		glDisableVertexAttribArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDeleteBuffers(1, &vbo);
		s.check(glGetError() == GL_NO_ERROR, "GL error during baseline check");
	}

	// 2. conservative mode defaults to off and toggles cleanly
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		float pos[3][4];
		make_sliver(pos);
		s.check(f.ctx.conservative_raster == GL_FALSE, "conservative raster not off by default");
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.empty(), "fresh context rasterized conservatively");
		pglConservativeRaster(GL_TRUE);
		s.check(f.ctx.conservative_raster == GL_TRUE, "state not stored on the context");
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(!g_frags.empty(), "enabling had no effect");
		pglConservativeRaster(GL_FALSE);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.empty(), "disabling had no effect");
	}

	// 3. state is per-context and follows set_glContext
	{
		Fix a(16, 16), b(16, 16);
		float pos[3][4];
		make_sliver(pos);

		set_glContext(&a.ctx);
		GLuint prog_a = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog_a);
		pglConservativeRaster(GL_TRUE);

		set_glContext(&b.ctx);
		GLuint prog_b = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog_b);
		s.check(b.ctx.conservative_raster == GL_FALSE, "state leaked into other context");
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.empty(), "other context rasterized conservatively");

		set_glContext(&a.ctx);
		s.check(a.ctx.conservative_raster == GL_TRUE, "state lost across context switch");
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(!g_frags.empty(), "state not restored on switch back");
		pglConservativeRaster(GL_FALSE);
	}

	// 4. points, lines and polygon wireframe/point modes are unaffected
	{
		Fix f(16, 16);
		GLuint prog = pglCreateProgram(vs_passthrough, fs_plain, 0, nullptr, GL_FALSE);
		glUseProgram(prog);
		float pos[3][4];
		win2clip(pos[0], 2.0, 2.0, 0.0, 1.0);
		win2clip(pos[1], 13.0, 3.0, 0.0, 1.0);
		win2clip(pos[2], 4.0, 13.0, 0.0, 1.0);

		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		std::vector<FragRec> wire_off = g_frags;
		pglConservativeRaster(GL_TRUE);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.size() == wire_off.size(), "wireframe changed under conservative mode");
		std::map<std::pair<int, int>, int> a = count_per_pixel(wire_off), b = count_per_pixel(g_frags);
		s.check(a == b, "wireframe coverage changed under conservative mode");

		glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		std::vector<FragRec> pts_on = g_frags;
		pglConservativeRaster(GL_FALSE);
		clear_log();
		draw_tris(&pos[0][0], 3, GL_TRIANGLES);
		s.check(g_frags.size() == pts_on.size(), "point mode changed under conservative mode");
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		float lpos[4][4];
		win2clip(lpos[0], 1.0, 1.0, 0.0, 1.0);
		win2clip(lpos[1], 14.0, 5.0, 0.0, 1.0);
		win2clip(lpos[2], 2.0, 14.0, 0.0, 1.0);
		win2clip(lpos[3], 9.0, 4.0, 0.0, 1.0);
		clear_log();
		draw_tris(&lpos[0][0], 4, GL_LINES);
		std::vector<FragRec> lines_off = g_frags;
		pglConservativeRaster(GL_TRUE);
		clear_log();
		draw_tris(&lpos[0][0], 4, GL_LINES);
		s.check(count_per_pixel(g_frags) == count_per_pixel(lines_off), "lines changed under conservative mode");
		pglConservativeRaster(GL_FALSE);
	}
}

void register_tests(Suite& s)
{
	s.add("coverage", test_coverage);
	s.add("clipped", test_clipped);
	s.add("interpolation", test_interpolation);
	s.add("fragments", test_fragments);
	s.add("compatibility", test_compatibility);
}
