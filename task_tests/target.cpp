// Analytic interpolation-derivative tests for pgl_Ddx/pgl_Ddy and
// pglTexture2DVarying.  Every PGL value is checked against an independent
// double-precision reference: barycentric screen Jacobian, the perspective
// quotient rule, and the screen-affine noperspective case.
#include <portablegl.h>

#include "checks.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int N_OUT = 6; // 2 smooth, 2 noperspective, 2 flat

struct FragRec {
	int x, y;
	float in[N_OUT];
	// Full-size copies so unused-slot hygiene can be checked past N_OUT.
	float ddx[GL_MAX_VERTEX_OUTPUT_COMPONENTS];
	float ddy[GL_MAX_VERTEX_OUTPUT_COMPONENTS];
	GLboolean valid;
};

// Clip-space vertex plus the six vertex-output values it carries.
struct Vtx {
	float pos[4];
	float out[N_OUT];
};

std::vector<FragRec> g_rec;
int g_slot_u = 0, g_slot_v = 1;

void grad_vs(float* vs_output, vec4* vertex_attribs, Shader_Builtins* builtins, void*)
{
	builtins->gl_Position = vertex_attribs[0];
	vs_output[0] = vertex_attribs[1].x;
	vs_output[1] = vertex_attribs[1].y;
	vs_output[2] = vertex_attribs[1].z;
	vs_output[3] = vertex_attribs[1].w;
	vs_output[4] = vertex_attribs[2].x;
	vs_output[5] = vertex_attribs[2].y;
}

void grad_fs(float* fs_input, Shader_Builtins* builtins, void*)
{
	FragRec r;
	r.x = (int)builtins->gl_FragCoord.x;
	r.y = (int)builtins->gl_FragCoord.y;
	r.valid = builtins->pgl_DerivativesValid;
	for (int i = 0; i < N_OUT; ++i)
		r.in[i] = fs_input[i];
	std::memcpy(r.ddx, builtins->pgl_Ddx, sizeof(r.ddx));
	std::memcpy(r.ddy, builtins->pgl_Ddy, sizeof(r.ddy));
	g_rec.push_back(r);
	builtins->gl_FragColor = make_v4(0, 0, 0, 1);
}

// Same geometry, but the callback rewrites depth and discards; the recorded
// derivatives must be identical to the non-discarding path.
void discard_fs(float* fs_input, Shader_Builtins* builtins, void*)
{
	grad_fs(fs_input, builtins, NULL);
	builtins->gl_FragDepth = builtins->gl_FragCoord.z * 0.5f;
	builtins->discard = GL_TRUE;
}

GLenum interp_mode[6] = {
	PGL_SMOOTH, PGL_SMOOTH,
	PGL_NOPERSPECTIVE, PGL_NOPERSPECTIVE,
	PGL_FLAT, PGL_FLAT
};

GLuint make_program(glContext& ctx, frag_func fs, GLsizei n = N_OUT)
{
	set_glContext(&ctx);
	GLenum modes[GL_MAX_VERTEX_OUTPUT_COMPONENTS];
	for (int i = 0; i < n; ++i)
		modes[i] = interp_mode[i < N_OUT ? i : N_OUT - 1];
	GLuint p = pglCreateProgram(grad_vs, fs, n, modes, fs == discard_fs ? GL_TRUE : GL_FALSE);
	glUseProgram(p);
	return p;
}

// Upload position (attr 0, vec4), four packed varyings (attr 1, vec4) and
// two flat varyings (attr 2, vec2-ish vec4) and draw.
void upload_and_draw(glContext& ctx, const std::vector<Vtx>& verts, GLenum mode,
                     GLsizei instances = 1, const unsigned short* indices = nullptr,
                     GLsizei index_count = 0)
{
	set_glContext(&ctx);
	std::vector<float> pos, pack1, pack2;
	for (const Vtx& v : verts) {
		pos.insert(pos.end(), { v.pos[0], v.pos[1], v.pos[2], v.pos[3] });
		pack1.insert(pack1.end(), { v.out[0], v.out[1], v.out[2], v.out[3] });
		pack2.insert(pack2.end(), { v.out[4], v.out[5], 0.0f, 0.0f });
	}
	GLuint bufs[3];
	glGenBuffers(3, bufs);
	glBindBuffer(GL_ARRAY_BUFFER, bufs[0]);
	glBufferData(GL_ARRAY_BUFFER, pos.size() * sizeof(float), pos.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	glBindBuffer(GL_ARRAY_BUFFER, bufs[1]);
	glBufferData(GL_ARRAY_BUFFER, pack1.size() * sizeof(float), pack1.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);

	glBindBuffer(GL_ARRAY_BUFFER, bufs[2]);
	glBufferData(GL_ARRAY_BUFFER, pack2.size() * sizeof(float), pack2.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 0, 0);

	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	if (indices) {
		GLuint ibo;
		glGenBuffers(1, &ibo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_count * sizeof(unsigned short),
		             indices, GL_STATIC_DRAW);
		if (instances > 1)
			glDrawElementsInstanced(mode, index_count, GL_UNSIGNED_SHORT, NULL, instances);
		else
			glDrawElements(mode, index_count, GL_UNSIGNED_SHORT, NULL);
	} else if (instances > 1) {
		glDrawArraysInstanced(mode, 0, (GLsizei)verts.size(), instances);
	} else {
		glDrawArrays(mode, 0, (GLsizei)verts.size());
	}
	glDeleteBuffers(3, bufs);
}

// ---- Independent double-precision reference -------------------------------

struct RefTri {
	double sx[3], sy[3];       // snapped vertex positions, pixels
	double clipw[3];
	double val[3][N_OUT];
	// barycentric Jacobian per pixel
	double da_dx, da_dy, db_dx, db_dy, dg_dx, dg_dy;
};

double viewport_map(double ndc, int origin, int size)
{
	// Mirrors make_viewport_m4: r = origin + size - 0.01, scale = (r-origin)/2.
	double l = origin;
	double r = origin + (double)size - 0.01;
	return ndc * ((r - l) * 0.5) + (l + r) * 0.5;
}

long long snap256(double v)
{
	return (long long)std::floor(v * 256.0 + 0.5);
}

// Integer edge function e(a,b;p) in double, on subpixel coordinates.
double edge_fn(long long ax, long long ay, long long bx, long long by,
               long long px, long long py)
{
	return (double)(ay - by) * px + (double)(bx - ax) * py
	     + (double)ax * by - (double)bx * ay;
}

RefTri make_ref(const Vtx& a, const Vtx& b, const Vtx& c,
                int vx, int vy, int vw, int vh)
{
	RefTri t;
	const Vtx* v[3] = { &a, &b, &c };
	long long vpx[3], vpy[3];
	for (int k = 0; k < 3; ++k) {
		double w = v[k]->pos[3];
		double ndx = v[k]->pos[0] / w;
		double ndy = v[k]->pos[1] / w;
		t.sx[k] = snap256(viewport_map(ndx, vx, vw)) / 256.0;
		t.sy[k] = snap256(viewport_map(ndy, vy, vh)) / 256.0;
		vpx[k] = snap256(viewport_map(ndx, vx, vw));
		vpy[k] = snap256(viewport_map(ndy, vy, vh));
		t.clipw[k] = w;
		for (int i = 0; i < N_OUT; ++i)
			t.val[k][i] = v[k]->out[i];
	}
	// beta  = e20/e20_v1 (opposite v0), gamma = e01/e01_v2 (opposite v1)
	double e20_v1 = edge_fn(vpx[2], vpy[2], vpx[0], vpy[0], vpx[1], vpy[1]);
	double e01_v2 = edge_fn(vpx[0], vpy[0], vpx[1], vpy[1], vpx[2], vpy[2]);
	t.db_dx = 256.0 * (double)(vpy[2] - vpy[0]) / e20_v1;
	t.db_dy = 256.0 * (double)(vpx[0] - vpx[2]) / e20_v1;
	t.dg_dx = 256.0 * (double)(vpy[0] - vpy[1]) / e01_v2;
	t.dg_dy = 256.0 * (double)(vpx[1] - vpx[0]) / e01_v2;
	t.da_dx = -(t.db_dx + t.dg_dx);
	t.da_dy = -(t.db_dy + t.dg_dy);
	return t;
}

// Barycentrics of a pixel center against the reference triangle.
void bary(const RefTri& t, int ix, int iy, double& al, double& be, double& ga)
{
	long long px = (long long)ix * 256 + 128;
	long long py = (long long)iy * 256 + 128;
	long long vpx[3], vpy[3];
	for (int k = 0; k < 3; ++k) {
		vpx[k] = (long long)std::llround(t.sx[k] * 256.0);
		vpy[k] = (long long)std::llround(t.sy[k] * 256.0);
	}
	double e12_v0 = edge_fn(vpx[1], vpy[1], vpx[2], vpy[2], vpx[0], vpy[0]);
	double e20_v1 = edge_fn(vpx[2], vpy[2], vpx[0], vpy[0], vpx[1], vpy[1]);
	double e01_v2 = edge_fn(vpx[0], vpy[0], vpx[1], vpy[1], vpx[2], vpy[2]);
	al = edge_fn(vpx[1], vpy[1], vpx[2], vpy[2], px, py) / e12_v0;
	be = edge_fn(vpx[2], vpy[2], vpx[0], vpy[0], px, py) / e20_v1;
	ga = edge_fn(vpx[0], vpy[0], vpx[1], vpy[1], px, py) / e01_v2;
}

// Analytic derivative of one slot at a specific pixel.  The smooth value is a
// ratio of two affine functions, so its gradient is evaluated with the
// fragment's own interpolated numerator/denominator rather than assumed
// constant; noperspective is genuinely affine and flat is identically zero.
void ref_derivatives(const RefTri& t, int slot, int ix, int iy, double& ddx, double& ddy)
{
	double al, be, ga;
	bary(t, ix, iy, al, be, ga);
	double iw0 = 1.0 / t.clipw[0], iw1 = 1.0 / t.clipw[1], iw2 = 1.0 / t.clipw[2];
	double da[2] = { t.da_dx, t.da_dy };
	double db[2] = { t.db_dx, t.db_dy };
	double dg[2] = { t.dg_dx, t.dg_dy };
	if (interp_mode[slot] == PGL_SMOOTH) {
		double s0 = t.val[0][slot] * iw0, s1 = t.val[1][slot] * iw1, s2 = t.val[2][slot] * iw2;
		double num = al * s0 + be * s1 + ga * s2;
		double g = al * iw0 + be * iw1 + ga * iw2;
		for (int q = 0; q < 2; ++q) {
			double dnum = da[q] * s0 + db[q] * s1 + dg[q] * s2;
			double dgw = da[q] * iw0 + db[q] * iw1 + dg[q] * iw2;
			double r = (dnum * g - num * dgw) / (g * g);
			if (q == 0) ddx = r; else ddy = r;
		}
	} else if (interp_mode[slot] == PGL_NOPERSPECTIVE) {
		ddx = da[0] * t.val[0][slot] + db[0] * t.val[1][slot] + dg[0] * t.val[2][slot];
		ddy = da[1] * t.val[0][slot] + db[1] * t.val[1][slot] + dg[1] * t.val[2][slot];
	} else {
		ddx = ddy = 0.0;
	}
}

double ref_value(const RefTri& t, int slot, int ix, int iy)
{
	double al, be, ga;
	bary(t, ix, iy, al, be, ga);
	if (interp_mode[slot] == PGL_SMOOTH) {
		double iw0 = 1.0 / t.clipw[0], iw1 = 1.0 / t.clipw[1], iw2 = 1.0 / t.clipw[2];
		double num = al * t.val[0][slot] * iw0 + be * t.val[1][slot] * iw1 + ga * t.val[2][slot] * iw2;
		double den = al * iw0 + be * iw1 + ga * iw2;
		return num / den;
	}
	if (interp_mode[slot] == PGL_NOPERSPECTIVE)
		return al * t.val[0][slot] + be * t.val[1][slot] + ga * t.val[2][slot];
	return t.val[2][slot]; // last-vertex convention flat
}

bool close_enough(double got, double ref, double rel, double abs_tol)
{
	return std::fabs(got - ref) <= std::max(abs_tol, rel * std::fabs(ref));
}

// A record well inside the triangle (not straddling an edge), so barycentric
// comparisons avoid edge-ownership rounding.  Distinct neighbor coordinates
// only: instanced draws record each pixel once per instance.
const FragRec* interior_record(const std::vector<FragRec>& recs)
{
	for (const FragRec& r : recs) {
		bool up = false, down = false, left = false, right = false;
		for (const FragRec& q : recs) {
			if (q.x == r.x + 1 && q.y == r.y) right = true;
			if (q.x == r.x - 1 && q.y == r.y) left = true;
			if (q.x == r.x && q.y == r.y + 1) up = true;
			if (q.x == r.x && q.y == r.y - 1) down = true;
		}
		if (up && down && left && right)
			return &r;
	}
	return nullptr;
}

const FragRec* find_record(const std::vector<FragRec>& recs, int x, int y)
{
	for (const FragRec& r : recs)
		if (r.x == x && r.y == y)
			return &r;
	return nullptr;
}

// Unequal-w triangle, mixed interpolation modes, standard viewport.
std::vector<Vtx> mixed_triangle()
{
	return {
		{ { -1.2f, -1.0f, 0.0f, 1.0f },  { 0.10f, -0.40f, 0.5f, -1.0f, 7.0f, -3.0f } },
		{ {  1.1f, -0.7f, 0.0f, 2.5f },  { 0.80f,  0.20f, 1.5f,  0.5f, 7.0f, -3.0f } },
		{ { -0.3f,  1.15f, 0.0f, 0.6f }, { -0.30f, 0.90f, -1.0f, 2.0f, 7.0f, -3.0f } },
	};
}

} // namespace

void test_gradients(Check& check)
{
	constexpr int W = 48, H = 36;
	glContext ctx;
	pix_t* pixels = nullptr;
	check(init_glContext(&ctx, &pixels, W, H) != 0, "init context");
	make_program(ctx, grad_fs);

	std::vector<Vtx> verts = mixed_triangle();
	g_rec.clear();
	upload_and_draw(ctx, verts, GL_TRIANGLES);
	check(g_rec.size() > 10, "triangle produced fragments");

	RefTri ref = make_ref(verts[0], verts[1], verts[2], 0, 0, W, H);

	// Validity and full-array hygiene on every covered fragment.
	for (const FragRec& r : g_rec) {
		check(r.valid == GL_TRUE, "filled triangle marks derivatives valid");
		for (int i = N_OUT; i < GL_MAX_VERTEX_OUTPUT_COMPONENTS; ++i) {
			check(r.ddx[i] == 0.0f && r.ddy[i] == 0.0f, "unused derivative slots zeroed");
		}
		check(r.ddx[4] == 0.0f && r.ddx[5] == 0.0f, "flat ddx zero");
		check(r.ddy[4] == 0.0f && r.ddy[5] == 0.0f, "flat ddy zero");
	}

	const FragRec* r = interior_record(g_rec);
	check(r != nullptr, "found an interior fragment");

	// Per-fragment double-precision verification.  The smooth quotient
	// gradient varies over the triangle, so each fragment is checked at its
	// own pixel center rather than against one shared constant.
	int checked = 0;
	for (const FragRec& q : g_rec) {
		for (int slot = 0; slot < N_OUT; ++slot) {
			double edx, edy;
			ref_derivatives(ref, slot, q.x, q.y, edx, edy);
			check(close_enough(q.ddx[slot], edx, 1e-3, 1e-5), "ddx matches per-fragment double analytic reference");
			check(close_enough(q.ddy[slot], edy, 1e-3, 1e-5), "ddy matches per-fragment double analytic reference");

			double ev = ref_value(ref, slot, q.x, q.y);
			check(close_enough(q.in[slot], ev, 1e-3, 1e-5), "fs_input matches double barycentric reference");
		}
		++checked;
	}
	check(checked > 10, "many fragments verified against the double reference");

	// Noperspective is screen-affine and flat is constant, so those gradients
	// (all zero for flat) are genuinely identical on every fragment.
	for (const FragRec& q : g_rec) {
		for (int slot = 2; slot < N_OUT; ++slot) {
			check(q.ddx[slot] == r->ddx[slot] && q.ddy[slot] == r->ddy[slot],
			      "noperspective/flat Jacobian constant across fragments");
		}
	}

	// Independent finite-difference check straight from fs_input at an
	// interior pixel: the local analytic derivatives must predict the actual
	// one-pixel change (looser tolerance: a difference quotient carries the
	// second-order term the analytic derivative omits).
	const FragRec* rx = r;
	const FragRec* rx1 = find_record(g_rec, rx->x + 1, rx->y);
	const FragRec* rxm = find_record(g_rec, rx->x - 1, rx->y);
	const FragRec* ry1 = find_record(g_rec, rx->x, rx->y + 1);
	const FragRec* rym = find_record(g_rec, rx->x, rx->y - 1);
	check(rx1 && rxm && ry1 && rym, "interior fragment has all four neighbors");
	for (int slot = 0; slot < 4; ++slot) { // smooth + noperspective only
		// The library's own difference quotients must match the double
		// rational model's difference quotients at the same pixels (same
		// function sampled on the same grid), independent of any tolerance on
		// derivative-vs-difference-quotient.
		double fdx_ref = ref_value(ref, slot, rx->x + 1, rx->y) - ref_value(ref, slot, rx->x, rx->y);
		double fdy_ref = ref_value(ref, slot, rx->x, rx->y + 1) - ref_value(ref, slot, rx->x, rx->y);
		double fdx_lib = (double)rx1->in[slot] - rx->in[slot];
		double fdy_lib = (double)ry1->in[slot] - rx->in[slot];
		check(close_enough(fdx_lib, fdx_ref, 1e-3, 1e-5), "fs_input forward-x difference matches double model");
		check(close_enough(fdy_lib, fdy_ref, 1e-3, 1e-5), "fs_input forward-y difference matches double model");

		// The analytic derivative must approximate the local change: a central
		// difference (second-order accurate) at the interior pixel.
		double cdx = ((double)rx1->in[slot] - (double)rxm->in[slot]) * 0.5;
		double cdy = ((double)ry1->in[slot] - (double)rym->in[slot]) * 0.5;
		check(close_enough(cdx, rx->ddx[slot], 3e-2, 3e-4), "central x difference agrees with analytic ddx");
		check(close_enough(cdy, rx->ddy[slot], 3e-2, 3e-4), "central y difference agrees with analytic ddy");
	}

	// discard / user gl_FragDepth leave the derivatives untouched.
	std::vector<FragRec> before = g_rec;
	make_program(ctx, discard_fs);
	g_rec.clear();
	upload_and_draw(ctx, verts, GL_TRIANGLES);
	check(g_rec.size() == before.size(), "discard keeps the same coverage set");
	for (size_t i = 0; i < g_rec.size(); ++i) {
		check(g_rec[i].x == before[i].x && g_rec[i].y == before[i].y, "discard fragment order stable");
		for (int slot = 0; slot < N_OUT; ++slot) {
			check(g_rec[i].ddx[slot] == before[i].ddx[slot], "discard does not change ddx");
			check(g_rec[i].ddy[slot] == before[i].ddy[slot], "discard does not change ddy");
		}
	}
	free_glContext(&ctx);
}

void test_clipping(Check& check)
{
	constexpr int W = 64, H = 48;
	glContext ctx;
	pix_t* pixels = nullptr;
	check(init_glContext(&ctx, &pixels, W, H) != 0, "init context");

	// Offset, non-square viewport; the fan center is inside and the outer
	// ring runs well past the NDC frustum, so every output triangle is a
	// clipping fan result with interpolated (non-original) vertices.
	glViewport(6, 4, W - 12, H - 8);
	make_program(ctx, grad_fs);

	std::vector<Vtx> fan;
	fan.push_back({ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.5f, 0.5f, 0.0f, 0.0f, 2.0f, 9.0f } });
	const int RING = 6;
	for (int k = 0; k <= RING; ++k) {
		double ang = 2.0 * M_PI * k / RING;
		float nx = (float)(2.2 * std::cos(ang));
		float ny = (float)(2.0 * std::sin(ang));
		float w = 1.0f + 0.8f * (float)k; // deliberately unequal w around the ring
		fan.push_back({ { nx * w, ny * w, 0.0f, w },
		                { 0.15f * k, 0.1f * (RING - k), (float)k, -0.4f * k, 2.0f, 9.0f } });
	}
	// Indexed draw through an IBO, two instances.
	std::vector<unsigned short> idx;
	for (int k = 1; k <= RING; ++k) {
		idx.push_back(0);
		idx.push_back((unsigned short)k);
		idx.push_back((unsigned short)(k + 1));
	}
	g_rec.clear();
	upload_and_draw(ctx, fan, GL_TRIANGLE_FAN, 2, idx.data(), (GLsizei)idx.size());
	size_t one_instance = g_rec.size() / 2;
	check(one_instance > 20, "clipped fan produced fragments per instance");
	check(g_rec.size() == 2 * one_instance, "instancing doubles the fragment count");

	// The perspective rational interpolation is one fixed function of screen
	// position over each parent triangle; clipping only restricts its domain.
	// Find, per covered fragment, the parent whose screen triangle contains
	// that pixel center, and check against that parent's own double gradient.
	RefTri parents[RING];
	for (int parent = 0; parent < RING; ++parent)
		parents[parent] = make_ref(fan[0], fan[parent + 1], fan[parent + 2],
		                           6, 4, W - 12, H - 8);
	int matched = 0;
	for (const FragRec& q : g_rec) {
		for (int parent = 0; parent < RING; ++parent) {
			double al, be, ga;
			bary(parents[parent], q.x, q.y, al, be, ga);
			// Pixel center on or inside this parent (allow edge tolerance).
			if (al < -1e-6 || be < -1e-6 || ga < -1e-6)
				continue;
			++matched;
			for (int slot = 0; slot < 2; ++slot) {
				double edx, edy;
				ref_derivatives(parents[parent], slot, q.x, q.y, edx, edy);
				check(close_enough(q.ddx[slot], edx, 4e-3, 3e-5),
				      "clipped smooth ddx matches parent rational Jacobian");
				check(close_enough(q.ddy[slot], edy, 4e-3, 3e-5),
				      "clipped smooth ddy matches parent rational Jacobian");
			}
			break;
		}
	}
	check(matched > 20, "clipped fragments matched to a parent triangle");

	// Find an interior fragment whose whole 3x3 neighborhood stays inside the
	// same parent triangle, so a difference never crosses a fan-wedge edge
	// (each wedge is a different rational function).
	const FragRec* ri = nullptr;
	int ri_parent = -1;
	for (const FragRec& q : g_rec) {
		for (int parent = 0; parent < RING; ++parent) {
			bool all_in = true;
			for (int ddx = -1; ddx <= 1 && all_in; ++ddx)
				for (int ddy = -1; ddy <= 1 && all_in; ++ddy) {
					double al, be, ga;
					bary(parents[parent], q.x + ddx, q.y + ddy, al, be, ga);
					if (al < -1e-6 || be < -1e-6 || ga < -1e-6)
						all_in = false;
				}
			if (all_in) { ri = &q; ri_parent = parent; break; }
		}
		if (ri) break;
	}
	check(ri != nullptr, "clipped fan has a fragment interior to one parent");
	for (int slot = 0; slot < 4; ++slot) {
		// Library difference quotients must equal the matched parent's double
		// rational-model quotients on the same pixel grid.
		double fdx_ref = ref_value(parents[ri_parent], slot, ri->x + 1, ri->y)
		               - ref_value(parents[ri_parent], slot, ri->x, ri->y);
		double fdy_ref = ref_value(parents[ri_parent], slot, ri->x, ri->y + 1)
		               - ref_value(parents[ri_parent], slot, ri->x, ri->y);
		const FragRec* rxp = find_record(g_rec, ri->x + 1, ri->y);
		const FragRec* ryp = find_record(g_rec, ri->x, ri->y + 1);
		check(rxp && ryp, "neighbor fragments present");
		check(close_enough((double)rxp->in[slot] - ri->in[slot], fdx_ref, 1e-3, 1e-5),
		      "clipped fs_input x difference matches double model");
		check(close_enough((double)ryp->in[slot] - ri->in[slot], fdy_ref, 1e-3, 1e-5),
		      "clipped fs_input y difference matches double model");

		// And the analytic slope itself, re-checked at this interior pixel.
		double edx, edy;
		ref_derivatives(parents[ri_parent], slot, ri->x, ri->y, edx, edy);
		check(close_enough(ri->ddx[slot], edx, 2e-3, 2e-5), "clipped interior ddx matches double quotient rule");
		check(close_enough(ri->ddy[slot], edy, 2e-3, 2e-5), "clipped interior ddy matches double quotient rule");
	}
	for (const FragRec& q : g_rec) {
		check(q.valid == GL_TRUE, "clipped fragments carry valid derivatives");
		check(q.ddx[4] == 0.0f && q.ddy[4] == 0.0f && q.ddx[5] == 0.0f && q.ddy[5] == 0.0f,
		      "flat slots stay zero after clipping");
		for (int i = N_OUT; i < GL_MAX_VERTEX_OUTPUT_COMPONENTS; ++i)
			check(q.ddx[i] == 0.0f && q.ddy[i] == 0.0f, "unused slots zero after clipping");
	}

	// Strip path: same clipping guarantees, different topology.
	std::vector<Vtx> strip = {
		{ { -2.0f, -1.3f, 0, 1.0f }, { 0, 0, 0, 0, 1, 1 } },
		{ {  2.0f, -1.1f, 0, 1.7f }, { 1, 0, 1, 0, 1, 1 } },
		{ { -1.8f,  1.2f, 0, 0.7f }, { 0, 1, 0, 1, 1, 1 } },
		{ {  1.9f,  1.4f, 0, 2.2f }, { 1, 1, 1, 1, 1, 1 } },
	};
	g_rec.clear();
	upload_and_draw(ctx, strip, GL_TRIANGLE_STRIP);
	check(g_rec.size() > 10, "clipped strip produced fragments");
	for (const FragRec& q : g_rec)
		check(q.valid == GL_TRUE, "strip fragments carry valid derivatives");

	// Provoking vertex clipped away: with FIRST_VERTEX_CONVENTION the flat
	// value must still come from that original first vertex on every survivor.
	glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
	const float flat_v0 = 11.0f, flat_v1 = 22.0f, flat_v2 = 33.0f;
	std::vector<Vtx> provoked = {
		// v0 is well outside the frustum and is the (first) provoking vertex.
		{ {  3.5f,  0.0f, 0.0f, 1.0f }, { 0, 0, 0, 0, flat_v0, -1.0f } },
		{ { -0.9f, -0.8f, 0.0f, 1.0f }, { 0, 0, 0, 0, flat_v1, -1.0f } },
		{ {  0.2f,  0.9f, 0.0f, 1.0f }, { 0, 0, 0, 0, flat_v2, -1.0f } },
	};
	g_rec.clear();
	upload_and_draw(ctx, provoked, GL_TRIANGLES);
	check(!g_rec.empty(), "triangle with clipped provoking vertex still rasterizes");
	for (const FragRec& q : g_rec) {
		check(q.in[4] == flat_v0, "flat value follows the clipped first/provoking vertex");
		check(q.in[5] == -1.0f, "second flat slot follows the provoking vertex");
		check(q.ddx[4] == 0.0f && q.ddy[4] == 0.0f, "provoked flat derivative stays zero");
	}
	glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
	free_glContext(&ctx);
}

// ---- Sampling / invalid-context tests -------------------------------------

namespace {

constexpr int TEX_BASE = 32;
constexpr int TEX_LEVELS = 6;

// Solid, distinct color per mip level.
const u8 level_rgb[TEX_LEVELS][3] = {
	{ 220,  20,  20 }, // L0 red
	{  20, 200,  20 }, // L1 green
	{  20,  40, 220 }, // L2 blue
	{ 220, 220,  40 }, // L3 yellow
	{ 200,  40, 200 }, // L4 magenta
	{  40, 200, 200 }, // L5 cyan
};

struct SampleRec {
	int x, y;
	GLboolean valid;
	GLboolean zeroed;
	vec4 color;
	int expected_level;
};

std::vector<SampleRec> g_samp;
GLuint g_tex = 0;

bool arrays_zeroed(const Shader_Builtins* b)
{
	for (int i = 0; i < GL_MAX_VERTEX_OUTPUT_COMPONENTS; ++i)
		if (b->pgl_Ddx[i] != 0.0f || b->pgl_Ddy[i] != 0.0f)
			return false;
	return true;
}

void sample_fs(float* fs_input, Shader_Builtins* b, void*)
{
	SampleRec s;
	s.x = (int)b->gl_FragCoord.x;
	s.y = (int)b->gl_FragCoord.y;
	s.valid = b->pgl_DerivativesValid;
	s.zeroed = arrays_zeroed(b) ? GL_TRUE : GL_FALSE;
	s.color = pglTexture2DVarying(g_tex, fs_input, b, g_slot_u, g_slot_v);
	// Independent expected level from the analytic Jacobian, same rho rule
	// as texture2DGrad: rho = max(length(d/dx), length(d/dy)) in texel units.
	if (b->pgl_DerivativesValid) {
		double dudx = b->pgl_Ddx[g_slot_u], dvdx = b->pgl_Ddx[g_slot_v];
		double dudy = b->pgl_Ddy[g_slot_u], dvdy = b->pgl_Ddy[g_slot_v];
		double rx = std::sqrt((dudx * TEX_BASE) * (dudx * TEX_BASE) +
		                      (dvdx * TEX_BASE) * (dvdx * TEX_BASE));
		double ry = std::sqrt((dudy * TEX_BASE) * (dudy * TEX_BASE) +
		                      (dvdy * TEX_BASE) * (dvdy * TEX_BASE));
		double rho = std::max(rx, ry);
		double lambda = rho < 1e-10 ? -16.0 : std::log2(rho);
		int lvl = (int)std::floor(lambda + 0.5);
		if (lvl < 0) lvl = 0;
		if (lvl > TEX_LEVELS - 1) lvl = TEX_LEVELS - 1;
		s.expected_level = lvl;
	} else {
		s.expected_level = -1;
	}
	g_samp.push_back(s);
	// Old automatic texture2D path stays available alongside the new one.
	vec4 legacy = texture2D(g_tex, fs_input[g_slot_u], fs_input[g_slot_v]);
	b->gl_FragColor = legacy;
}

void invalid_fs(float*, Shader_Builtins* b, void*)
{
	// Must see an explicitly invalid, zeroed context here.
	SampleRec s;
	s.valid = b->pgl_DerivativesValid;
	s.zeroed = arrays_zeroed(b) ? GL_TRUE : GL_FALSE;
	s.x = (int)b->gl_FragCoord.x;
	s.y = (int)b->gl_FragCoord.y;
	s.color = pglTexture2DVarying(g_tex, NULL, b, 0, 1);
	s.expected_level = -1;
	g_samp.push_back(s);
}

GLuint build_mip_texture(glContext& ctx)
{
	set_glContext(&ctx);
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	int dim = TEX_BASE;
	for (int lvl = 0; lvl < TEX_LEVELS; ++lvl) {
		std::vector<Color> img(dim * dim);
		for (Color& c4 : img)
			c4 = make_Color(level_rgb[lvl][0], level_rgb[lvl][1], level_rgb[lvl][2], 255);
		glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, dim, dim, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, img.data());
		dim /= 2;
	}
	return tex;
}

// Four output slots: U smooth at 0, an unrelated smooth at 1, nopersp at 2,
// V smooth at 3, so the U/V pair is deliberately non-adjacent.
GLenum sample_interp[4] = { PGL_SMOOTH, PGL_SMOOTH, PGL_NOPERSPECTIVE, PGL_SMOOTH };

void sample_vs(float* out, vec4* a, Shader_Builtins* b, void*)
{
	b->gl_Position = a[0];
	out[0] = a[1].x; // U
	out[1] = a[1].y; // other smooth varying
	out[2] = a[1].z; // noperspective
	out[3] = a[1].w; // V
}

void draw_sample_quad(glContext& ctx, float uv_scale)
{
	set_glContext(&ctx);
	// Full-width quad; UV range scaled to force the chosen minification.
	float pos[16] = {
		-1, -1, 0, 1,   1, -1, 0, 1,
		-1,  1, 0, 1,   1,  1, 0, 1,
	};
	float vary[16] = {
		0, 0, 0, 0,
		uv_scale, 0, 0, 0,
		0, uv_scale, 0, uv_scale,
		uv_scale, uv_scale, 0, uv_scale,
	};
	GLuint pbo, vbo;
	glGenBuffers(2, &pbo);
	glBindBuffer(GL_ARRAY_BUFFER, pbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vary), vary, GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

} // namespace

void test_sampling(Check& check)
{
	constexpr int W = 64, H = 64;
	glContext ctx;
	pix_t* pixels = nullptr;
	check(init_glContext(&ctx, &pixels, W, H) != 0, "init context");
	set_glContext(&ctx);
	// Several cases deliberately raise GL_INVALID_OPERATION; verify them via
	// glGetError without the default debug callback printing each fragment.
	glDisable(GL_DEBUG_OUTPUT);
	g_tex = build_mip_texture(ctx);
	GLuint prog = pglCreateProgram(sample_vs, sample_fs, 4, sample_interp, GL_FALSE);
	glUseProgram(prog);
	glClearColor(0, 0, 0, 1);

	g_slot_u = 0;
	g_slot_v = 3; // non-adjacent slots

	// Magnified region: UV 0..1 over the whole target -> rho < 1 -> level 0.
	g_samp.clear();
	draw_sample_quad(ctx, 1.0f);
	check(!g_samp.empty(), "mag quad sampled");
	int mag_level = g_samp[g_samp.size() / 2].expected_level;
	check(mag_level == 0, "magnified region analytically selects level 0");
	for (const SampleRec& s : g_samp) {
		check(s.valid == GL_TRUE, "quad fragments have valid derivatives");
		check(s.expected_level >= 0, "expected level computed");
		const u8* rgb = level_rgb[s.expected_level];
		check(std::fabs(s.color.x * 255.0f - rgb[0]) < 4 &&
		      std::fabs(s.color.y * 255.0f - rgb[1]) < 4 &&
		      std::fabs(s.color.z * 255.0f - rgb[2]) < 4 &&
		      std::fabs(s.color.w - 1.0f) < 1e-5, "sampled color matches analytically selected level");
	}

	// Strongly minified region: UV 0..16 over the same pixels -> high level.
	g_samp.clear();
	draw_sample_quad(ctx, 16.0f);
	int min_level = g_samp[g_samp.size() / 2].expected_level;
	check(min_level >= 3, "minified region analytically selects a high mip level");
	check(min_level != mag_level, "near and far regions choose different levels");
	for (const SampleRec& s : g_samp) {
		const u8* rgb = level_rgb[s.expected_level];
		check(std::fabs(s.color.x * 255.0f - rgb[0]) < 4 &&
		      std::fabs(s.color.y * 255.0f - rgb[1]) < 4 &&
		      std::fabs(s.color.z * 255.0f - rgb[2]) < 4,
		      "minified sample matches its level color");
	}
	check(glGetError() == GL_NO_ERROR, "valid sampling raises no error");

	// Adjacent-slot variant gives the same answer.
	g_slot_v = 1;
	g_samp.clear();
	draw_sample_quad(ctx, 1.0f);
	for (const SampleRec& s : g_samp) {
		const u8* rgb = level_rgb[s.expected_level];
		check(std::fabs(s.color.x * 255.0f - rgb[0]) < 4 &&
		      std::fabs(s.color.y * 255.0f - rgb[1]) < 4 &&
		      std::fabs(s.color.z * 255.0f - rgb[2]) < 4,
		      "non-adjacent and adjacent slot pairs follow the same Grad rules");
	}
	g_slot_v = 3;

	// Out-of-range slot inside an otherwise valid triangle: transparent black
	// and GL_INVALID_OPERATION, never a fallback to the constant-LOD path.
	set_glContext(&ctx);
	vec4 bad;
	{
		Shader_Builtins probe;
		probe.pgl_DerivativesValid = GL_TRUE;
		memset(probe.pgl_Ddx, 0, sizeof(probe.pgl_Ddx));
		memset(probe.pgl_Ddy, 0, sizeof(probe.pgl_Ddy));
		float fs[4] = { 0.5f, 0.5f, 0, 0 };
		bad = pglTexture2DVarying(g_tex, fs, &probe, 0, 99);
		check(glGetError() == GL_INVALID_OPERATION, "out-of-range slot raises INVALID_OPERATION");
		check(bad.x == 0 && bad.y == 0 && bad.z == 0 && bad.w == 0, "bad slot returns transparent black");
		bad = pglTexture2DVarying(g_tex, fs, &probe, 4, 0);
		check(glGetError() == GL_INVALID_OPERATION, "boundary slot raises INVALID_OPERATION");
		check(bad.w == 0, "boundary slot returns transparent black");
	}

	// --- Invalid derivative contexts --------------------------------------
	GLuint inv_prog = pglCreateProgram(sample_vs, invalid_fs, 4, sample_interp, GL_FALSE);
	glUseProgram(inv_prog);

	// Points.
	g_samp.clear();
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_POINTS, 0, 4);
	check(!g_samp.empty(), "point fragments ran");
	for (const SampleRec& s : g_samp) {
		check(s.valid == GL_FALSE, "point context invalid");
		check(s.zeroed == GL_TRUE, "point derivative arrays zeroed");
		check(s.color.w == 0 && s.color.x == 0, "point sampling returns transparent black");
	}
	check(glGetError() == GL_INVALID_OPERATION, "point sampling raises INVALID_OPERATION");

	// Lines.
	g_samp.clear();
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_LINES, 0, 4);
	check(!g_samp.empty(), "line fragments ran");
	for (const SampleRec& s : g_samp) {
		check(s.valid == GL_FALSE && s.color.w == 0, "line context invalid and black");
		check(s.zeroed == GL_TRUE, "line derivative arrays zeroed");
	}
	check(glGetError() == GL_INVALID_OPERATION, "line sampling raises INVALID_OPERATION");

	// Non-FILL polygon mode on a triangle.
	glUseProgram(prog);
	glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
	g_samp.clear();
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	check(!g_samp.empty(), "polygon-point fragments ran");
	for (const SampleRec& s : g_samp) {
		check(s.valid == GL_FALSE && s.color.w == 0, "polygon-point context invalid and black");
		check(s.zeroed == GL_TRUE, "polygon-point derivative arrays zeroed");
	}
	check(glGetError() == GL_INVALID_OPERATION, "polygon-point sampling raises INVALID_OPERATION");
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	// Full-screen fragment program.
	g_samp.clear();
	pglDrawFrame2(invalid_fs, NULL);
	check((int)g_samp.size() == W * H, "full-screen program covered every pixel");
	for (const SampleRec& s : g_samp) {
		check(s.valid == GL_FALSE && s.color.w == 0, "full-screen context invalid and black");
		check(s.zeroed == GL_TRUE, "full-screen derivative arrays zeroed");
	}
	check(glGetError() == GL_INVALID_OPERATION, "full-screen sampling raises INVALID_OPERATION");

	// Two contexts interleaved: the second context's invalid state must never
	// leak into the first, and each keeps its own derivative storage.
	glContext other;
	pix_t* other_pixels = nullptr;
	check(init_glContext(&other, &other_pixels, 32, 32) != 0, "init second context");
	set_glContext(&other);
	glDisable(GL_DEBUG_OUTPUT);
	GLuint other_prog = pglCreateProgram(sample_vs, invalid_fs, 4, sample_interp, GL_FALSE);
	glUseProgram(other_prog);
	g_samp.clear();
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_POINTS, 0, 3);
	for (const SampleRec& s : g_samp) {
		check(s.valid == GL_FALSE, "interleaved context point draw is invalid");
		check(s.zeroed == GL_TRUE, "interleaved context arrays zeroed, no stale values");
	}
	free_glContext(&other);

	set_glContext(&ctx);
	glUseProgram(prog);
	g_samp.clear();
	draw_sample_quad(ctx, 1.0f);
	for (const SampleRec& s : g_samp)
		check(s.valid == GL_TRUE, "first context triangle still valid after interleaving");
	check(glGetError() == GL_NO_ERROR, "interleaving leaves no pending error");

	free_glContext(&ctx);
}
