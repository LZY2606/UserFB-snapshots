// Target feature cases for the 3D LUT graph extension.
//
// This file is #included by support/target_main.cpp and relies on the FB_TEST
// / FB_CHECK macros from support/case_harness.hpp. It intentionally has no
// main() and no skipped cases.

#ifndef FB_TEST
#include "case_harness.hpp"
#endif

#include "api/zimg.h"
#include "graph/lut3d.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Small test helpers
// ---------------------------------------------------------------------------

struct ABuf {
	void *p = nullptr;
	ptrdiff_t stride = 0; // bytes

	ABuf() = default;
	ABuf(const ABuf &) = delete;
	ABuf &operator=(const ABuf &) = delete;
	~ABuf() { std::free(p); }

	void init(unsigned w, unsigned rows, unsigned bytes_per_sample)
	{
		std::free(p);
		size_t row_bytes = static_cast<size_t>(w) * bytes_per_sample;
		stride = static_cast<ptrdiff_t>((row_bytes + 63) & ~static_cast<size_t>(63));
		size_t alloc_rows = rows ? rows : 1;
		if (posix_memalign(&p, 64, static_cast<size_t>(stride) * alloc_rows))
			std::abort();
		std::memset(p, 0, static_cast<size_t>(stride) * alloc_rows);
	}

	template <class T>
	T *line(unsigned y)
	{
		return reinterpret_cast<T *>(static_cast<unsigned char *>(p) + static_cast<ptrdiff_t>(y) * stride);
	}

	template <class T>
	const T *line(unsigned y) const
	{
		return reinterpret_cast<const T *>(static_cast<const unsigned char *>(p) + static_cast<ptrdiff_t>(y) * stride);
	}
};

struct GraphGuard {
	zimg_filter_graph *g;
	void *tmp = nullptr;

	explicit GraphGuard(zimg_filter_graph *graph) : g(graph)
	{
		size_t size = 0;
		if (zimg_filter_graph_get_tmp_size(g, &size) != ZIMG_ERROR_SUCCESS)
			std::abort();
		if (posix_memalign(&tmp, 64, size ? size : 64))
			std::abort();
	}

	~GraphGuard()
	{
		zimg_filter_graph_free(g);
		std::free(tmp);
	}
};

static zimg_graph_builder_params test_params(zimg_resample_filter_e filter = ZIMG_RESIZE_BILINEAR)
{
	zimg_graph_builder_params p;
	zimg_graph_builder_params_default(&p, ZIMG_API_VERSION);
	p.cpu_type = ZIMG_CPU_NONE;
	p.resample_filter = filter;
	p.resample_filter_uv = filter;
	return p;
}

static zimg_image_format rgb_float_format(unsigned w, unsigned h)
{
	zimg_image_format f;
	zimg_image_format_default(&f, ZIMG_API_VERSION);
	f.version = ZIMG_API_VERSION;
	f.width = w;
	f.height = h;
	f.pixel_type = ZIMG_PIXEL_FLOAT;
	f.depth = 32;
	f.color_family = ZIMG_COLOR_RGB;
	f.matrix_coefficients = ZIMG_MATRIX_RGB;
	f.transfer_characteristics = ZIMG_TRANSFER_LINEAR;
	f.color_primaries = ZIMG_PRIMARIES_BT709;
	f.pixel_range = ZIMG_RANGE_FULL;
	return f;
}

static void bind_plane(zimg_image_buffer &buf, unsigned p, ABuf &a, unsigned mask = ZIMG_BUFFER_MAX)
{
	buf.version = ZIMG_API_VERSION;
	buf.plane[p] = { a.p, a.stride, mask };
}

static void bind_plane_c(zimg_image_buffer_const &buf, unsigned p, const ABuf &a, unsigned mask = ZIMG_BUFFER_MAX)
{
	buf.version = ZIMG_API_VERSION;
	buf.plane[p] = { const_cast<void *>(a.p), a.stride, mask };
}

// Owned LUT table plus public descriptor.
struct LutData {
	unsigned n;
	std::vector<float> tab;
	float mn[3] = { 0.0f, 0.0f, 0.0f };
	float mx[3] = { 1.0f, 1.0f, 1.0f };

	explicit LutData(unsigned size) : n(size), tab(3ULL * size * size * size, 0.0f) {}

	size_t idx(unsigned r, unsigned g, unsigned b) const
	{
		return (static_cast<size_t>(b) * n * n + static_cast<size_t>(g) * n + r) * 3;
	}

	float &at(unsigned r, unsigned g, unsigned b, unsigned c) { return tab[idx(r, g, b) + c]; }

	zimg_lut3d desc()
	{
		zimg_lut3d lut{};
		lut.size = n;
		lut.data = tab.data();
		lut.data_count = tab.size();
		std::memcpy(lut.domain_min, mn, sizeof(mn));
		std::memcpy(lut.domain_max, mx, sizeof(mx));
		return lut;
	}

	std::shared_ptr<zimg::graph::Lut3D> snapshot() const
	{
		std::vector<float> copy = tab;
		float lo[3] = { mn[0], mn[1], mn[2] };
		float hi[3] = { mx[0], mx[1], mx[2] };
		return std::make_shared<zimg::graph::Lut3D>(n, std::move(copy), lo, hi);
	}
};

// Deterministic non-affine table with values outside [0, 1].
static void fill_nonaffine(LutData &lut)
{
	for (unsigned b = 0; b < lut.n; ++b) {
		for (unsigned g = 0; g < lut.n; ++g) {
			for (unsigned r = 0; r < lut.n; ++r) {
				for (unsigned c = 0; c < 3; ++c) {
					float v = std::sin((r + 1) * 12.9898f + (g + 1) * 78.233f + (b + 1) * 37.719f + c * 11.13f);
					lut.at(r, g, b, c) = 2.25f * v + 0.17f * (c - 1);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Independent tetrahedral reference (generic barycentric solver)
// ---------------------------------------------------------------------------

using V3 = std::array<double, 3>;

static double det3(const double m[3][3])
{
	return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
	       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
	       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// Barycentric coordinates of p with respect to tetrahedron (a,b,c,d).
static bool barycentric(V3 p, V3 a, V3 b, V3 c, V3 d, double w[4])
{
	double M[3][3] = {
		{ b[0] - a[0], c[0] - a[0], d[0] - a[0] },
		{ b[1] - a[1], c[1] - a[1], d[1] - a[1] },
		{ b[2] - a[2], c[2] - a[2], d[2] - a[2] },
	};
	double rhs[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
	double D = det3(M);
	if (std::fabs(D) < 1e-14)
		return false;

	double lambda[3];
	for (unsigned k = 0; k < 3; ++k) {
		double Mk[3][3];
		std::memcpy(Mk, M, sizeof(M));
		for (unsigned i = 0; i < 3; ++i)
			Mk[i][k] = rhs[i];
		lambda[k] = det3(Mk) / D;
	}

	w[1] = lambda[0];
	w[2] = lambda[1];
	w[3] = lambda[2];
	w[0] = 1.0 - w[1] - w[2] - w[3];

	for (unsigned k = 0; k < 4; ++k) {
		if (w[k] < -1e-7 || w[k] > 1.0 + 1e-7)
			return false;
	}
	return true;
}

// Corner bit assignment: bit0=R, bit1=G, bit2=B.
static const int REF_TETS[6][4] = {
	{ 0, 1, 3, 7 }, // fr >= fg >= fb
	{ 0, 1, 5, 7 }, // fr >= fb >= fg
	{ 0, 2, 3, 7 }, // fg >= fr >= fb
	{ 0, 2, 6, 7 }, // fg >= fb >= fr
	{ 0, 4, 5, 7 }, // fb >= fr >= fg
	{ 0, 4, 6, 7 }, // fb >= fg >= fr
};

static int ref_tetra_eval(const LutData &lut, const float xin[3], float out[3])
{
	unsigned n = lut.n;
	unsigned base[3];
	double coord[3];

	for (unsigned c = 0; c < 3; ++c) {
		double u = (static_cast<double>(xin[c]) - lut.mn[c]) /
		            (static_cast<double>(lut.mx[c]) - lut.mn[c]) * static_cast<double>(n - 1);
		if (!std::isfinite(u))
			u = 0.0;
		u = std::max(0.0, std::min(static_cast<double>(n - 1), u));
		unsigned k = static_cast<unsigned>(std::floor(u));
		if (k >= n - 1)
			k = n - 2;
		base[c] = k;
		coord[c] = u;
	}

	V3 p{ coord[0], coord[1], coord[2] };

	for (unsigned t = 0; t < 6; ++t) {
		V3 v[4];
		unsigned cr[4], cg[4], cb[4];
		for (unsigned q = 0; q < 4; ++q) {
			int corner = REF_TETS[t][q];
			unsigned rr = base[0] + ((corner & 1) ? 1u : 0u);
			unsigned gg = base[1] + ((corner & 2) ? 1u : 0u);
			unsigned bb = base[2] + ((corner & 4) ? 1u : 0u);
			cr[q] = rr; cg[q] = gg; cb[q] = bb;
			v[q] = { static_cast<double>(rr), static_cast<double>(gg), static_cast<double>(bb) };
		}

		double w[4];
		if (!barycentric(p, v[0], v[1], v[2], v[3], w))
			continue;

		for (unsigned ch = 0; ch < 3; ++ch) {
			double sum = 0.0;
			for (unsigned q = 0; q < 4; ++q)
				sum += w[q] * static_cast<double>(lut.tab[((static_cast<size_t>(cb[q]) * n + cg[q]) * n + cr[q]) * 3 + ch]);
			out[ch] = static_cast<float>(sum);
		}
		return static_cast<int>(t);
	}
	return -1;
}

static void check_near(const char *what, float got, float expected, float tol)
{
	if (!(std::fabs(got - expected) <= tol)) {
		std::fprintf(stderr, "NEAR FAIL %s: got=%.8g expected=%.8g tol=%.2g\n", what, got, expected, tol);
		FB_CHECK(false);
	}
	FB_CHECK(true);
}

// sRGB transfer encoding used by independent pipeline expectations.
static float srgb_encode(float x)
{
	if (x <= 0.0031308f)
		return 12.92f * x;
	return 1.055f * std::pow(std::max(x, 0.0f), 1.0f / 2.4f) - 0.055f;
}

} // namespace

// ===========================================================================
// tetrahedra group
// ===========================================================================

FB_TEST("tetrahedra", six_orderings_barycentric)
{
	LutData lut(2);
	fill_nonaffine(lut);
	auto core = lut.snapshot();

	const float fracs[6][3] = {
		{ 0.80f, 0.50f, 0.20f }, // R > G > B
		{ 0.80f, 0.20f, 0.50f }, // R > B > G
		{ 0.50f, 0.80f, 0.20f }, // G > R > B
		{ 0.20f, 0.80f, 0.50f }, // G > B > R
		{ 0.50f, 0.20f, 0.80f }, // B > R > G
		{ 0.20f, 0.50f, 0.80f }, // B > G > R
	};

	for (unsigned t = 0; t < 6; ++t) {
		float in[3] = { fracs[t][0], fracs[t][1], fracs[t][2] };
		float ref[3], got[3];
		int chosen = ref_tetra_eval(lut, in, ref);
		FB_CHECK(chosen == static_cast<int>(t));

		core->eval(in, got);
		for (unsigned c = 0; c < 3; ++c)
			check_near("ordering", got[c], ref[c], 2e-5f);
	}

	// Ties exercise shared faces and must agree as well.
	const float ties[][3] = {
		{ 0.5f, 0.5f, 0.2f }, { 0.5f, 0.2f, 0.5f }, { 0.2f, 0.5f, 0.5f },
		{ 0.7f, 0.7f, 0.7f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f },
	};
	for (const auto &p : ties) {
		float ref[3], got[3];
		FB_CHECK(ref_tetra_eval(lut, p, ref) >= 0);
		core->eval(p, got);
		for (unsigned c = 0; c < 3; ++c)
			check_near("tie", got[c], ref[c], 2e-5f);
	}
}

FB_TEST("tetrahedra", domain_endpoints_and_clamp)
{
	LutData lut(3);
	fill_nonaffine(lut);
	lut.mn[0] = -2.0f; lut.mn[1] = 3.0f; lut.mn[2] = 0.5f;
	lut.mx[0] = 4.0f;  lut.mx[1] = 9.0f; lut.mx[2] = 2.5f;
	auto core = lut.snapshot();

	// Exact lower endpoint, exact upper endpoint, mid-domain grid vertex,
	// below-domain and above-domain coordinates.
	const float points[][3] = {
		{ -2.0f, 3.0f, 0.5f },
		{ 4.0f, 9.0f, 2.5f },
		{ 1.0f, 6.0f, 1.5f },
		{ -9.0f, 0.0f, -4.0f },
		{ 99.0f, 40.0f, 8.0f },
		{ -2.0001f, 9.0001f, 1.5f },
	};
	for (const auto &p : points) {
		float ref[3], got[3];
		FB_CHECK(ref_tetra_eval(lut, p, ref) >= 0);
		core->eval(p, got);
		for (unsigned c = 0; c < 3; ++c)
			check_near("endpoint", got[c], ref[c], 2e-5f);
	}

	// The top domain value must equal the last grid vertex exactly.
	float top_in[3] = { 4.0f, 9.0f, 2.5f };
	float top_got[3];
	core->eval(top_in, top_got);
	for (unsigned c = 0; c < 3; ++c)
		check_near("top-vertex", top_got[c], lut.at(2, 2, 2, c), 1e-6f);

	float low_in[3] = { -2.0f, 3.0f, 0.5f };
	float low_got[3];
	core->eval(low_in, low_got);
	for (unsigned c = 0; c < 3; ++c)
		check_near("low-vertex", low_got[c], lut.at(0, 0, 0, c), 1e-6f);

	// Randomized coverage over both cells and all orderings.
	unsigned seed = 1234567;
	auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return static_cast<float>((seed >> 8) & 0xffffff) / 0xffffff; };
	for (unsigned i = 0; i < 200; ++i) {
		float p[3];
		for (unsigned c = 0; c < 3; ++c)
			p[c] = lut.mn[c] + rnd() * (lut.mx[c] - lut.mn[c]);
		float ref[3], got[3];
		FB_CHECK(ref_tetra_eval(lut, p, ref) >= 0);
		core->eval(p, got);
		for (unsigned c = 0; c < 3; ++c)
			check_near("random-domain", got[c], ref[c], 3e-5f);
	}
}

// ===========================================================================
// pipeline group
// ===========================================================================

FB_TEST("pipeline", public_rgb_float_matches_tetra)
{
	const unsigned W = 5, H = 3;
	LutData lut(3);
	fill_nonaffine(lut);

	zimg_image_format fmt = rgb_float_format(W, H);
	zimg_graph_builder_params params = test_params();
	zimg_lut3d desc = lut.desc();

	zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &desc);
	FB_CHECK(fg != nullptr);
	if (!fg) {
		std::fprintf(stderr, "build: %s\n", "unexpected null graph");
		return;
	}
	GraphGuard graph(fg);

	ABuf in[3], out[3];
	for (unsigned p = 0; p < 3; ++p) {
		in[p].init(W, H, sizeof(float));
		out[p].init(W, H, sizeof(float));
	}

	for (unsigned y = 0; y < H; ++y) {
		for (unsigned x = 0; x < W; ++x) {
			in[0].line<float>(y)[x] = -0.15f + 1.2f * (x + 0.5f) / W;
			in[1].line<float>(y)[x] = -0.10f + 1.1f * (y + 0.5f) / H;
			in[2].line<float>(y)[x] = 0.1f + 0.8f * ((x + y) % 3) / 2.0f;
		}
	}

	zimg_image_buffer_const ib{};
	zimg_image_buffer ob{};
	for (unsigned p = 0; p < 3; ++p) {
		bind_plane_c(ib, p, in[p]);
		bind_plane(ob, p, out[p]);
	}

	zimg_error_code_e err = zimg_filter_graph_process(graph.g, &ib, &ob, graph.tmp, nullptr, nullptr, nullptr, nullptr);
	FB_CHECK(err == ZIMG_ERROR_SUCCESS);

	auto core = lut.snapshot();
	for (unsigned y = 0; y < H; ++y) {
		for (unsigned x = 0; x < W; ++x) {
			float rgb[3] = { in[0].line<float>(y)[x], in[1].line<float>(y)[x], in[2].line<float>(y)[x] };
			float expected[3];
			core->eval(rgb, expected);
			for (unsigned p = 0; p < 3; ++p)
				check_near("public-float", out[p].line<float>(y)[x], expected[p], 2e-5f);
		}
	}
}

FB_TEST("pipeline", same_format_still_luts)
{
	// Constant LUT maps every input to the same linear color C, making it
	// obvious that the identical-format no-op path did not swallow the LUT.
	const unsigned W = 4, H = 4;
	LutData lut(2);
	const float C[3] = { 0.80f, 0.20f, 0.10f };
	for (unsigned i = 0; i < lut.tab.size(); i += 3) {
		lut.tab[i + 0] = C[0];
		lut.tab[i + 1] = C[1];
		lut.tab[i + 2] = C[2];
	}
	zimg_lut3d desc = lut.desc();

	zimg_image_format f;
	zimg_image_format_default(&f, ZIMG_API_VERSION);
	f.width = W; f.height = H;
	f.pixel_type = ZIMG_PIXEL_BYTE; f.depth = 8;
	f.color_family = ZIMG_COLOR_RGB;
	f.matrix_coefficients = ZIMG_MATRIX_RGB;
	f.transfer_characteristics = ZIMG_TRANSFER_IEC_61966_2_1;
	f.color_primaries = ZIMG_PRIMARIES_BT709;
	f.pixel_range = ZIMG_RANGE_FULL;

	zimg_graph_builder_params params = test_params();

	zimg_filter_graph *with_lut = zimg_filter_graph_build_with_lut(&f, &f, &params, &desc);
	zimg_filter_graph *plain = zimg_filter_graph_build(&f, &f, &params);
	FB_CHECK(with_lut != nullptr);
	FB_CHECK(plain != nullptr);

	ABuf src[3], dst_lut[3], dst_plain[3];
	for (unsigned p = 0; p < 3; ++p) {
		src[p].init(W, H, 1);
		dst_lut[p].init(W, H, 1);
		dst_plain[p].init(W, H, 1);
		for (unsigned y = 0; y < H; ++y)
			for (unsigned x = 0; x < W; ++x)
				src[p].line<unsigned char>(y)[x] = static_cast<unsigned char>(40 + 40 * ((x + y + p) % 5));
	}

	zimg_image_buffer_const ib{};
	zimg_image_buffer olut{};
	zimg_image_buffer oplain{};
	for (unsigned p = 0; p < 3; ++p) {
		bind_plane_c(ib, p, src[p]);
		bind_plane(olut, p, dst_lut[p]);
		bind_plane(oplain, p, dst_plain[p]);
	}

	size_t ts = 0;
	void *tmp_lut = nullptr, *tmp_plain = nullptr;
	FB_CHECK(zimg_filter_graph_get_tmp_size(with_lut, &ts) == 0);
	posix_memalign(&tmp_lut, 64, ts ? ts : 64);
	FB_CHECK(zimg_filter_graph_get_tmp_size(plain, &ts) == 0);
	posix_memalign(&tmp_plain, 64, ts ? ts : 64);

	FB_CHECK(zimg_filter_graph_process(with_lut, &ib, &olut, tmp_lut, nullptr, nullptr, nullptr, nullptr) == 0);
	FB_CHECK(zimg_filter_graph_process(plain, &ib, &oplain, tmp_plain, nullptr, nullptr, nullptr, nullptr) == 0);

	unsigned differences = 0;
	for (unsigned y = 0; y < H; ++y) {
		for (unsigned x = 0; x < W; ++x) {
			for (unsigned p = 0; p < 3; ++p) {
				unsigned char lv = dst_lut[p].line<unsigned char>(y)[x];
				unsigned char pv = dst_plain[p].line<unsigned char>(y)[x];
				if (lv != pv)
					++differences;
				// Plain identical-format graph must preserve the sample.
				FB_CHECK(pv == src[p].line<unsigned char>(y)[x]);
				// LUT graph must encode the constant mapped color.
				int expected = static_cast<int>(std::lround(srgb_encode(C[p]) * 255.0));
				FB_CHECK(std::abs(static_cast<int>(lv) - expected) <= 1);
			}
		}
	}
	FB_CHECK(differences > 0);

	zimg_filter_graph_free(with_lut);
	zimg_filter_graph_free(plain);
	std::free(tmp_lut);
	std::free(tmp_plain);
}

FB_TEST("pipeline", scale_transfer_yuv_bytes)
{
	const unsigned Ws = 2, Hs = 2, Wd = 7, Hd = 5;

	// Affine LUT: y = k*x + add, represented exactly on a size-2 grid.
	LutData lut(2);
	const float k[3] = { 1.0f, 0.5f, 1.2f };
	const float add[3] = { 0.08f, 0.25f, -0.10f };
	for (unsigned b = 0; b < 2; ++b)
		for (unsigned g = 0; g < 2; ++g)
			for (unsigned r = 0; r < 2; ++r) {
				unsigned coord[3] = { r, g, b };
				for (unsigned c = 0; c < 3; ++c)
					lut.at(r, g, b, c) = k[c] * coord[c] + add[c];
			}
	zimg_lut3d desc = lut.desc();

	zimg_image_format sfmt = rgb_float_format(Ws, Hs);
	zimg_image_format dfmt;
	zimg_image_format_default(&dfmt, ZIMG_API_VERSION);
	dfmt.width = Wd; dfmt.height = Hd;
	dfmt.pixel_type = ZIMG_PIXEL_BYTE; dfmt.depth = 8;
	dfmt.color_family = ZIMG_COLOR_YUV;
	dfmt.matrix_coefficients = ZIMG_MATRIX_BT709;
	dfmt.transfer_characteristics = ZIMG_TRANSFER_IEC_61966_2_1;
	dfmt.color_primaries = ZIMG_PRIMARIES_BT709;
	dfmt.pixel_range = ZIMG_RANGE_LIMITED;
	dfmt.subsample_w = 0; dfmt.subsample_h = 0;

	zimg_graph_builder_params params = test_params(ZIMG_RESIZE_BICUBIC);
	zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&sfmt, &dfmt, &params, &desc);
	FB_CHECK(fg != nullptr);
	if (!fg)
		return;
	GraphGuard graph(fg);

	// Uniform source color: any normalizing resampler preserves it.
	const float c[3] = { 0.20f, 0.45f, 0.70f };
	ABuf in[3], yuv[3];
	for (unsigned p = 0; p < 3; ++p) {
		in[p].init(Ws, Hs, sizeof(float));
		yuv[p].init(Wd, Hd, 1);
		for (unsigned y = 0; y < Hs; ++y)
			for (unsigned x = 0; x < Ws; ++x)
				in[p].line<float>(y)[x] = c[p];
	}

	zimg_image_buffer_const ib{};
	zimg_image_buffer ob{};
	for (unsigned p = 0; p < 3; ++p) {
		bind_plane_c(ib, p, in[p]);
		bind_plane(ob, p, yuv[p]);
	}

	FB_CHECK(zimg_filter_graph_process(graph.g, &ib, &ob, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);

	float lin[3];
	for (unsigned ch = 0; ch < 3; ++ch)
		lin[ch] = k[ch] * c[ch] + add[ch];
	float enc[3] = { srgb_encode(lin[0]), srgb_encode(lin[1]), srgb_encode(lin[2]) };
	float yval = 0.2126f * enc[0] + 0.7152f * enc[1] + 0.0722f * enc[2];
	float cb = (enc[2] - yval) / 1.8556f;
	float cr = (enc[0] - yval) / 1.5748f;
	int ey = static_cast<int>(std::lround(yval * 219.0f + 16.0f));
	int eu = static_cast<int>(std::lround(cb * 224.0f + 128.0f));
	int ev = static_cast<int>(std::lround(cr * 224.0f + 128.0f));

	for (unsigned yy = 0; yy < Hd; ++yy) {
		for (unsigned xx = 0; xx < Wd; ++xx) {
			int yq = yuv[0].line<unsigned char>(yy)[xx];
			int uq = yuv[1].line<unsigned char>(yy)[xx];
			int vq = yuv[2].line<unsigned char>(yy)[xx];
			FB_CHECK(std::abs(yq - ey) <= 2);
			FB_CHECK(std::abs(uq - eu) <= 3);
			FB_CHECK(std::abs(vq - ev) <= 3);
		}
	}
}

FB_TEST("pipeline", yuv420_output_and_ring_mask)
{
	const unsigned W = 8, H = 8;
	LutData lut(3);
	fill_nonaffine(lut);
	zimg_lut3d desc = lut.desc();

	zimg_image_format sfmt = rgb_float_format(W, H);
	zimg_image_format dfmt;
	zimg_image_format_default(&dfmt, ZIMG_API_VERSION);
	dfmt.width = W; dfmt.height = H;
	dfmt.pixel_type = ZIMG_PIXEL_BYTE; dfmt.depth = 8;
	dfmt.color_family = ZIMG_COLOR_YUV;
	dfmt.matrix_coefficients = ZIMG_MATRIX_BT709;
	dfmt.transfer_characteristics = ZIMG_TRANSFER_IEC_61966_2_1;
	dfmt.color_primaries = ZIMG_PRIMARIES_BT709;
	dfmt.pixel_range = ZIMG_RANGE_LIMITED;
	dfmt.subsample_w = 1; dfmt.subsample_h = 1;

	zimg_graph_builder_params params = test_params();
	zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&sfmt, &dfmt, &params, &desc);
	FB_CHECK(fg != nullptr);
	if (!fg)
		return;
	GraphGuard graph(fg);

	ABuf in[3], full[3], ring[3];
	for (unsigned p = 0; p < 3; ++p) {
		in[p].init(W, H, sizeof(float));
		unsigned pw = W >> (p ? 1 : 0);
		unsigned ph = H >> (p ? 1 : 0);
		full[p].init(pw, ph, 1);
	}
	// Non-MAX mask that still addresses every row exactly once.
	unsigned mask = zimg_select_buffer_mask(H);
	FB_CHECK(mask != ZIMG_BUFFER_MAX);
	for (unsigned p = 0; p < 3; ++p) {
		unsigned plane_mask = mask >> (p ? 1 : 0);
		unsigned rows = plane_mask + 1;
		unsigned pw = W >> (p ? 1 : 0);
		ring[p].init(pw, rows, 1);
	}

	for (unsigned y = 0; y < H; ++y)
		for (unsigned x = 0; x < W; ++x) {
			in[0].line<float>(y)[x] = 0.1f + 0.8f * x / (W - 1);
			in[1].line<float>(y)[x] = 0.1f + 0.8f * y / (H - 1);
			in[2].line<float>(y)[x] = 0.35f;
		}

	zimg_image_buffer_const ib{};
	for (unsigned p = 0; p < 3; ++p)
		bind_plane_c(ib, p, in[p]);

	zimg_image_buffer ob_full{};
	for (unsigned p = 0; p < 3; ++p)
		bind_plane(ob_full, p, full[p]);
	FB_CHECK(zimg_filter_graph_process(graph.g, &ib, &ob_full, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);

	zimg_image_buffer ob_ring{};
	for (unsigned p = 0; p < 3; ++p) {
		unsigned plane_mask = mask >> (p ? 1 : 0);
		bind_plane(ob_ring, p, ring[p], plane_mask);
	}
	FB_CHECK(zimg_filter_graph_process(graph.g, &ib, &ob_ring, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);

	for (unsigned p = 0; p < 3; ++p) {
		unsigned ph = H >> (p ? 1 : 0);
		unsigned pw = W >> (p ? 1 : 0);
		unsigned plane_mask = mask >> (p ? 1 : 0);
		for (unsigned y = 0; y < ph; ++y) {
			for (unsigned x = 0; x < pw; ++x) {
				unsigned char a = full[p].line<unsigned char>(y)[x];
				unsigned char b = reinterpret_cast<unsigned char *>(ring[p].p)[static_cast<size_t>(y & plane_mask) * ring[p].stride + x];
				FB_CHECK(a == b);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Ring callbacks and negative stride
// ---------------------------------------------------------------------------

namespace {

struct FullFloatImage {
	unsigned w = 0, h = 0;
	std::vector<float> pl[3];
};

struct StreamCtx {
	const zimg_image_buffer_const *ring_in = nullptr;
	const zimg_image_buffer *ring_out = nullptr;
	const FullFloatImage *src = nullptr;
	FullFloatImage *dst = nullptr;
	bool fail_unpack = false;
};

static int stream_unpack(void *user, unsigned i, unsigned left, unsigned right)
{
	StreamCtx *c = static_cast<StreamCtx *>(user);
	if (c->fail_unpack)
		return 1;
	for (unsigned p = 0; p < 3; ++p) {
		unsigned mask = c->ring_in->plane[p].mask;
		ptrdiff_t stride = c->ring_in->plane[p].stride;
		char *base = static_cast<char *>(const_cast<void *>(c->ring_in->plane[p].data));
		float *dst = reinterpret_cast<float *>(base + static_cast<ptrdiff_t>(i & mask) * stride) + left;
		const float *src = c->src->pl[p].data() + static_cast<size_t>(i) * c->src->w + left;
		std::memcpy(dst, src, static_cast<size_t>(right - left) * sizeof(float));
	}
	return 0;
}

static int stream_pack(void *user, unsigned i, unsigned left, unsigned right)
{
	StreamCtx *c = static_cast<StreamCtx *>(user);
	for (unsigned p = 0; p < 3; ++p) {
		unsigned mask = c->ring_out->plane[p].mask;
		ptrdiff_t stride = c->ring_out->plane[p].stride;
		const char *base = static_cast<const char *>(c->ring_out->plane[p].data);
		const float *src = reinterpret_cast<const float *>(base + static_cast<ptrdiff_t>(i & mask) * stride) + left;
		float *dst = c->dst->pl[p].data() + static_cast<size_t>(i) * c->dst->w + left;
		std::memcpy(dst, src, static_cast<size_t>(right - left) * sizeof(float));
	}
	return 0;
}

} // namespace

FB_TEST("pipeline", callbacks_ring_and_negative_stride)
{
	const unsigned W = 6, H = 8;
	LutData lut(2);
	for (unsigned i = 0; i < lut.tab.size(); i += 3) {
		lut.tab[i + 0] = 0.05f + 0.9f * (i / 3 % 2);
		lut.tab[i + 1] = 0.05f;
		lut.tab[i + 2] = 0.95f;
	}
	zimg_lut3d desc = lut.desc();
	zimg_image_format fmt = rgb_float_format(W, H);
	zimg_graph_builder_params params = test_params();

	zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &desc);
	FB_CHECK(fg != nullptr);
	if (!fg)
		return;
	GraphGuard graph(fg);

	FullFloatImage src_ref, dst_ref, dst_stream;
	src_ref.w = dst_ref.w = dst_stream.w = W;
	src_ref.h = dst_ref.h = dst_stream.h = H;
	for (unsigned p = 0; p < 3; ++p) {
		src_ref.pl[p].assign(W * H, 0.0f);
		dst_ref.pl[p].assign(W * H, 0.0f);
		dst_stream.pl[p].assign(W * H, 0.0f);
	}
	for (unsigned y = 0; y < H; ++y)
		for (unsigned x = 0; x < W; ++x) {
			src_ref.pl[0][y * W + x] = 0.1f + 0.7f * x / W;
			src_ref.pl[1][y * W + x] = 0.2f + 0.6f * y / H;
			src_ref.pl[2][y * W + x] = 0.4f;
		}

	// Reference run with MAX masks.
	ABuf max_in[3], max_out[3];
	zimg_image_buffer_const ib_max{};
	zimg_image_buffer ob_max{};
	for (unsigned p = 0; p < 3; ++p) {
		max_in[p].init(W, H, sizeof(float));
		max_out[p].init(W, H, sizeof(float));
		bind_plane_c(ib_max, p, max_in[p]);
		bind_plane(ob_max, p, max_out[p]);
	}
	for (unsigned p = 0; p < 3; ++p)
		for (unsigned y = 0; y < H; ++y)
			for (unsigned x = 0; x < W; ++x)
				max_in[p].line<float>(y)[x] = src_ref.pl[p][y * W + x];

	FB_CHECK(zimg_filter_graph_process(graph.g, &ib_max, &ob_max, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);
	for (unsigned p = 0; p < 3; ++p)
		for (unsigned y = 0; y < H; ++y)
			for (unsigned x = 0; x < W; ++x)
				dst_ref.pl[p][y * W + x] = max_out[p].line<float>(y)[x];

	// True small-ring run with callbacks.
	unsigned in_count = 0, out_count = 0;
	FB_CHECK(zimg_filter_graph_get_input_buffering(graph.g, &in_count) == 0);
	FB_CHECK(zimg_filter_graph_get_output_buffering(graph.g, &out_count) == 0);
	unsigned in_mask = zimg_select_buffer_mask(in_count);
	unsigned out_mask = zimg_select_buffer_mask(out_count);
	unsigned in_rows = in_mask == ZIMG_BUFFER_MAX ? H : in_mask + 1;
	unsigned out_rows = out_mask == ZIMG_BUFFER_MAX ? H : out_mask + 1;

	ABuf ring_in[3], ring_out[3];
	zimg_image_buffer_const ib_ring{};
	zimg_image_buffer ob_ring{};
	for (unsigned p = 0; p < 3; ++p) {
		ring_in[p].init(W, in_rows, sizeof(float));
		ring_out[p].init(W, out_rows, sizeof(float));
		unsigned im = in_mask == ZIMG_BUFFER_MAX ? ZIMG_BUFFER_MAX : in_mask;
		unsigned om = out_mask == ZIMG_BUFFER_MAX ? ZIMG_BUFFER_MAX : out_mask;
		bind_plane_c(ib_ring, p, ring_in[p], im);
		bind_plane(ob_ring, p, ring_out[p], om);
	}

	StreamCtx ctx;
	ctx.ring_in = &ib_ring;
	ctx.ring_out = &ob_ring;
	ctx.src = &src_ref;
	ctx.dst = &dst_stream;
	FB_CHECK(zimg_filter_graph_process(graph.g, &ib_ring, &ob_ring, graph.tmp, stream_unpack, &ctx, stream_pack, &ctx) == 0);

	for (unsigned p = 0; p < 3; ++p)
		for (size_t i = 0; i < W * H; ++i)
			check_near("ring", dst_stream.pl[p][i], dst_ref.pl[p][i], 1e-6f);

	// Callback failure must surface through the existing error interface.
	StreamCtx fail_ctx = ctx;
	fail_ctx.fail_unpack = true;
	zimg_error_code_e fail_err = zimg_filter_graph_process(graph.g, &ib_ring, &ob_ring, graph.tmp, stream_unpack, &fail_ctx, stream_pack, &ctx);
	FB_CHECK(fail_err == ZIMG_ERROR_USER_CALLBACK_FAILED);

	// Negative stride must produce the same logical image.
	ABuf neg_in[3], neg_out[3];
	zimg_image_buffer_const ib_neg{};
	zimg_image_buffer ob_neg{};
	for (unsigned p = 0; p < 3; ++p) {
		neg_in[p].init(W, H, sizeof(float));
		neg_out[p].init(W, H, sizeof(float));
		for (unsigned y = 0; y < H; ++y)
			for (unsigned x = 0; x < W; ++x)
				neg_in[p].line<float>(H - 1 - y)[x] = src_ref.pl[p][y * W + x];
		ib_neg.version = ZIMG_API_VERSION;
		ib_neg.plane[p] = { neg_in[p].line<float>(H - 1), -neg_in[p].stride, ZIMG_BUFFER_MAX };
		ob_neg.version = ZIMG_API_VERSION;
		ob_neg.plane[p] = { neg_out[p].line<float>(H - 1), -neg_out[p].stride, ZIMG_BUFFER_MAX };
	}
	FB_CHECK(zimg_filter_graph_process(graph.g, &ib_neg, &ob_neg, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);
	for (unsigned p = 0; p < 3; ++p)
		for (unsigned y = 0; y < H; ++y)
			for (unsigned x = 0; x < W; ++x)
				check_near("neg-stride", neg_out[p].line<float>(H - 1 - y)[x], dst_ref.pl[p][y * W + x], 1e-6f);
}

// ===========================================================================
// alpha group
// ===========================================================================

namespace {

static zimg_image_format rgba_float_format(unsigned w, unsigned h, zimg_alpha_type_e alpha)
{
	zimg_image_format f = rgb_float_format(w, h);
	f.alpha = alpha;
	return f;
}

// Affine LUT L(x) = x + add, exact on a size-2 grid.
static void fill_add_lut(LutData &lut, const float add[3])
{
	for (unsigned b = 0; b < 2; ++b)
		for (unsigned g = 0; g < 2; ++g)
			for (unsigned r = 0; r < 2; ++r) {
				unsigned coord[3] = { r, g, b };
				for (unsigned c = 0; c < 3; ++c)
					lut.at(r, g, b, c) = static_cast<float>(coord[c]) + add[c];
			}
}

} // namespace

FB_TEST("alpha", unpremul_zero_guard_and_repremul)
{
	const unsigned W = 2, H = 2;
	const float add[3] = { 0.05f, 0.07f, 0.09f };
	LutData lut(2);
	fill_add_lut(lut, add);
	zimg_lut3d desc = lut.desc();

	const float C[4][3] = {
		{ 0.20f, 0.40f, 0.60f },
		{ 0.55f, 0.15f, 0.35f },
		{ 0.10f, 0.80f, 0.30f },
		{ 0.70f, 0.65f, 0.05f },
	};
	const float A[4] = { 0.0f, 0.25f, 0.5f, 1.0f };

	auto run_case = [&](zimg_alpha_type_e src_alpha, zimg_alpha_type_e dst_alpha, bool scaled) {
		unsigned wd = scaled ? 4 : W;
		unsigned hd = scaled ? 4 : H;
		zimg_image_format sfmt = rgba_float_format(W, H, src_alpha);
		zimg_image_format dfmt = rgba_float_format(wd, hd, dst_alpha);
		zimg_graph_builder_params params = test_params(ZIMG_RESIZE_BILINEAR);
		zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&sfmt, &dfmt, &params, &desc);
		FB_CHECK(fg != nullptr);
		if (!fg)
			return;
		GraphGuard graph(fg);

		ABuf sp[4], dp[4];
		for (unsigned p = 0; p < 4; ++p) {
			sp[p].init(W, H, sizeof(float));
			dp[p].init(wd, hd, sizeof(float));
		}
		for (unsigned i = 0; i < 4; ++i) {
			unsigned x = i % W, y = i / W;
			float a = A[i];
			for (unsigned c = 0; c < 3; ++c) {
				float value = C[i][c];
				if (src_alpha == ZIMG_ALPHA_PREMULTIPLIED)
					value *= a;
				sp[c].line<float>(y)[x] = value;
			}
			sp[3].line<float>(y)[x] = a;
		}

		zimg_image_buffer_const ib{};
		zimg_image_buffer ob{};
		for (unsigned p = 0; p < 4; ++p) {
			bind_plane_c(ib, p, sp[p]);
			bind_plane(ob, p, dp[p]);
		}
		zimg_error_code_e err = zimg_filter_graph_process(graph.g, &ib, &ob, graph.tmp, nullptr, nullptr, nullptr, nullptr);
		FB_CHECK(err == ZIMG_ERROR_SUCCESS);

		if (!scaled) {
			for (unsigned i = 0; i < 4; ++i) {
				unsigned x = i % W, y = i / W;
				float a = A[i];
				for (unsigned c = 0; c < 3; ++c) {
					float value = dp[c].line<float>(y)[x];
					if (a == 0.0f) {
						check_near("alpha-zero-rgb", value, 0.0f, 1e-6f);
					} else if (dst_alpha == ZIMG_ALPHA_PREMULTIPLIED) {
						float expected = (C[i][c] + add[c]) * a;
						check_near("repremul", value, expected, 2e-5f);
					} else {
						float expected = C[i][c] + add[c];
						check_near("straight-lut", value, expected, 2e-5f);
					}
				}
				check_near("alpha-plane", dp[3].line<float>(y)[x], a, 1e-6f);
			}
		} else {
			// Uniform-alpha 0.5 image scaled: every output sample keeps the
			// recombined value.
			for (unsigned y = 0; y < hd; ++y)
				for (unsigned x = 0; x < wd; ++x) {
					check_near("scaled-alpha", dp[3].line<float>(y)[x], 0.5f, 1e-5f);
					for (unsigned c = 0; c < 3; ++c) {
						float value = dp[c].line<float>(y)[x];
						if (dst_alpha == ZIMG_ALPHA_PREMULTIPLIED)
							check_near("scaled-repremul", value, (0.35f + add[c]) * 0.5f, 1e-4f);
					}
				}
		}
	};

	run_case(ZIMG_ALPHA_STRAIGHT, ZIMG_ALPHA_STRAIGHT, false);
	run_case(ZIMG_ALPHA_PREMULTIPLIED, ZIMG_ALPHA_STRAIGHT, false);
	run_case(ZIMG_ALPHA_STRAIGHT, ZIMG_ALPHA_PREMULTIPLIED, false);
	run_case(ZIMG_ALPHA_PREMULTIPLIED, ZIMG_ALPHA_PREMULTIPLIED, false);

	// Scaling path continues to use the premultiplication rule. Use a uniform
	// straight color with constant alpha 0.5.
	{
		const unsigned W2 = 3, H2 = 3;
		zimg_image_format sfmt = rgba_float_format(W2, H2, ZIMG_ALPHA_STRAIGHT);
		zimg_image_format dfmt = rgba_float_format(7, 5, ZIMG_ALPHA_PREMULTIPLIED);
		zimg_graph_builder_params params = test_params(ZIMG_RESIZE_BICUBIC);
		zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&sfmt, &dfmt, &params, &desc);
		FB_CHECK(fg != nullptr);
		if (fg) {
			GraphGuard graph(fg);
			ABuf sp[4], dp[4];
			for (unsigned p = 0; p < 4; ++p) {
				sp[p].init(W2, H2, sizeof(float));
				dp[p].init(7, 5, sizeof(float));
			}
			for (unsigned y = 0; y < H2; ++y)
				for (unsigned x = 0; x < W2; ++x) {
					for (unsigned c = 0; c < 3; ++c)
						sp[c].line<float>(y)[x] = 0.35f;
					sp[3].line<float>(y)[x] = 0.5f;
				}
			zimg_image_buffer_const ib{};
			zimg_image_buffer ob{};
			for (unsigned p = 0; p < 4; ++p) {
				bind_plane_c(ib, p, sp[p]);
				bind_plane(ob, p, dp[p]);
			}
			FB_CHECK(zimg_filter_graph_process(graph.g, &ib, &ob, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);
			for (unsigned y = 0; y < 5; ++y)
				for (unsigned x = 0; x < 7; ++x) {
					check_near("scale-alpha-plane", dp[3].line<float>(y)[x], 0.5f, 1e-5f);
					for (unsigned c = 0; c < 3; ++c)
						check_near("scale-rgb", dp[c].line<float>(y)[x], (0.35f + add[c]) * 0.5f, 1e-4f);
				}
		}
	}
}

FB_TEST("alpha", zero_alpha_black_guard_with_strong_lut)
{
	// LUT maps black to a strong non-zero color. Fully transparent pixels
	// must still come out black for straight and premultiplied outputs.
	const float strong[3] = { 0.7f, 0.2f, 0.9f };
	LutData lut(2);
	for (unsigned i = 0; i < lut.tab.size(); i += 3) {
		lut.tab[i + 0] = strong[0];
		lut.tab[i + 1] = strong[1];
		lut.tab[i + 2] = strong[2];
	}
	zimg_lut3d desc = lut.desc();

	zimg_image_format sfmt = rgba_float_format(2, 1, ZIMG_ALPHA_STRAIGHT);
	zimg_image_format dfmt_s = rgba_float_format(2, 1, ZIMG_ALPHA_STRAIGHT);
	zimg_image_format dfmt_p = rgba_float_format(2, 1, ZIMG_ALPHA_PREMULTIPLIED);
	zimg_graph_builder_params params = test_params();

	for (unsigned mode = 0; mode < 2; ++mode) {
		zimg_image_format dfmt = mode ? dfmt_p : dfmt_s;
		zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&sfmt, &dfmt, &params, &desc);
		FB_CHECK(fg != nullptr);
		if (!fg)
			continue;
		GraphGuard graph(fg);

		ABuf sp[4], dp[4];
		for (unsigned p = 0; p < 4; ++p) {
			sp[p].init(2, 1, sizeof(float));
			dp[p].init(2, 1, sizeof(float));
		}
		// Pixel 0 transparent with arbitrary straight color; pixel 1 opaque.
		sp[0].line<float>(0)[0] = 0.9f; sp[1].line<float>(0)[0] = 0.1f; sp[2].line<float>(0)[0] = 0.4f;
		sp[3].line<float>(0)[0] = 0.0f;
		sp[0].line<float>(0)[1] = 0.0f; sp[1].line<float>(0)[1] = 0.0f; sp[2].line<float>(0)[1] = 0.0f;
		sp[3].line<float>(0)[1] = 1.0f;

		zimg_image_buffer_const ib{};
		zimg_image_buffer ob{};
		for (unsigned p = 0; p < 4; ++p) {
			bind_plane_c(ib, p, sp[p]);
			bind_plane(ob, p, dp[p]);
		}
		FB_CHECK(zimg_filter_graph_process(graph.g, &ib, &ob, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);
		for (unsigned c = 0; c < 3; ++c)
			check_near("transparent-black", dp[c].line<float>(0)[0], 0.0f, 1e-6f);
		// Opaque black pixel reveals the non-zero LUT mapping.
		for (unsigned c = 0; c < 3; ++c)
			check_near("opaque-lut-black", dp[c].line<float>(0)[1], strong[c], 1e-5f);
	}
}

// ===========================================================================
// ownership group
// ===========================================================================

FB_TEST("ownership", release_source_table_before_process)
{
	const unsigned W = 4, H = 2, N = 3;
	const size_t count = 3ULL * N * N * N;
	std::vector<float> expected(count);
	for (size_t i = 0; i < count; ++i)
		expected[i] = std::sin(static_cast<float>(i) * 0.37f) * 1.5f;

	float *raw = new float[count];
	std::memcpy(raw, expected.data(), count * sizeof(float));

	zimg_lut3d desc{};
	desc.size = N;
	desc.data = raw;
	desc.data_count = count;
	for (int c = 0; c < 3; ++c) {
		desc.domain_min[c] = 0.0f;
		desc.domain_max[c] = 1.0f;
	}

	zimg_image_format fmt = rgb_float_format(W, H);
	zimg_graph_builder_params params = test_params();
	zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &desc);
	FB_CHECK(fg != nullptr);

	// Caller releases the original table immediately after construction.
	delete[] raw;

	FB_CHECK(fg != nullptr);
	if (!fg)
		return;
	GraphGuard graph(fg);

	ABuf in[3], out[3];
	for (unsigned p = 0; p < 3; ++p) {
		in[p].init(W, H, sizeof(float));
		out[p].init(W, H, sizeof(float));
		for (unsigned y = 0; y < H; ++y)
			for (unsigned x = 0; x < W; ++x)
				in[p].line<float>(y)[x] = 0.15f + 0.7f * ((x + p) % W) / W;
	}
	zimg_image_buffer_const ib{};
	zimg_image_buffer ob{};
	for (unsigned p = 0; p < 3; ++p) {
		bind_plane_c(ib, p, in[p]);
		bind_plane(ob, p, out[p]);
	}
	FB_CHECK(zimg_filter_graph_process(graph.g, &ib, &ob, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);

	LutData verify(N);
	verify.tab = expected;
	zimg_lut3d vd = verify.desc();
	auto core = zimg::graph::Lut3D::create_snapshot(&vd);

	for (unsigned y = 0; y < H; ++y)
		for (unsigned x = 0; x < W; ++x) {
			float rgb[3] = { in[0].line<float>(y)[x], in[1].line<float>(y)[x], in[2].line<float>(y)[x] };
			float want[3];
			core->eval(rgb, want);
			for (unsigned p = 0; p < 3; ++p)
				check_near("released-table", out[p].line<float>(y)[x], want[p], 2e-5f);
		}
}

FB_TEST("ownership", mutate_source_after_build)
{
	const unsigned W = 3, H = 3;
	LutData lut(2);
	fill_nonaffine(lut);
	std::vector<float> original = lut.tab;
	zimg_lut3d desc = lut.desc();

	zimg_image_format fmt = rgb_float_format(W, H);
	zimg_graph_builder_params params = test_params();
	zimg_filter_graph *fg = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &desc);
	FB_CHECK(fg != nullptr);

	// Mutate the caller-owned table violently after a successful build.
	for (float &v : lut.tab)
		v = 12345.0f;
	lut.mn[0] = lut.mn[1] = lut.mn[2] = 50.0f;
	lut.mx[0] = lut.mx[1] = lut.mx[2] = 51.0f;

	if (!fg)
		return;
	GraphGuard graph(fg);
	ABuf in[3], out[3];
	for (unsigned p = 0; p < 3; ++p) {
		in[p].init(W, H, sizeof(float));
		out[p].init(W, H, sizeof(float));
		for (unsigned y = 0; y < H; ++y)
			for (unsigned x = 0; x < W; ++x)
				in[p].line<float>(y)[x] = 0.5f;
	}
	zimg_image_buffer_const ib{};
	zimg_image_buffer ob{};
	for (unsigned p = 0; p < 3; ++p) {
		bind_plane_c(ib, p, in[p]);
		bind_plane(ob, p, out[p]);
	}
	FB_CHECK(zimg_filter_graph_process(graph.g, &ib, &ob, graph.tmp, nullptr, nullptr, nullptr, nullptr) == 0);

	LutData verify(2);
	verify.tab = original;
	auto core = verify.snapshot();
	for (unsigned y = 0; y < H; ++y)
		for (unsigned x = 0; x < W; ++x) {
			float rgb[3] = { 0.5f, 0.5f, 0.5f };
			float want[3];
			core->eval(rgb, want);
			for (unsigned p = 0; p < 3; ++p)
				check_near("mutated-table", out[p].line<float>(y)[x], want[p], 2e-5f);
		}
}

FB_TEST("ownership", interleaved_graphs_do_not_pollute)
{
	const unsigned W = 4, H = 2;
	LutData lut_a(2), lut_b(2);
	for (unsigned i = 0; i < lut_a.tab.size(); i += 3) {
		lut_a.tab[i + 0] = 0.9f; lut_a.tab[i + 1] = 0.1f; lut_a.tab[i + 2] = 0.1f;
		lut_b.tab[i + 0] = 0.1f; lut_b.tab[i + 1] = 0.9f; lut_b.tab[i + 2] = 0.2f;
	}
	zimg_lut3d da = lut_a.desc(), db = lut_b.desc();

	zimg_image_format fmt = rgb_float_format(W, H);
	zimg_graph_builder_params params = test_params();
	zimg_filter_graph *ga = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &da);
	zimg_filter_graph *gb = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &db);
	FB_CHECK(ga && gb);
	if (!ga || !gb)
		return;

	size_t sa = 0, sb = 0;
	zimg_filter_graph_get_tmp_size(ga, &sa);
	zimg_filter_graph_get_tmp_size(gb, &sb);
	void *ta = nullptr, *tb = nullptr;
	posix_memalign(&ta, 64, sa ? sa : 64);
	posix_memalign(&tb, 64, sb ? sb : 64);

	ABuf in[3], oa1[3], ob1[3], oa2[3];
	for (unsigned p = 0; p < 3; ++p) {
		in[p].init(W, H, sizeof(float));
		oa1[p].init(W, H, sizeof(float));
		ob1[p].init(W, H, sizeof(float));
		oa2[p].init(W, H, sizeof(float));
		for (unsigned y = 0; y < H; ++y)
			for (unsigned x = 0; x < W; ++x)
				in[p].line<float>(y)[x] = 0.2f + 0.5f * x / W;
	}
	zimg_image_buffer_const ib{};
	zimg_image_buffer baoa1{}, bbob1{}, baoa2{};
	for (unsigned p = 0; p < 3; ++p) {
		bind_plane_c(ib, p, in[p]);
		bind_plane(baoa1, p, oa1[p]);
		bind_plane(bbob1, p, ob1[p]);
		bind_plane(baoa2, p, oa2[p]);
	}

	// Interleave A, B, A processing.
	FB_CHECK(zimg_filter_graph_process(ga, &ib, &baoa1, ta, nullptr, nullptr, nullptr, nullptr) == 0);
	FB_CHECK(zimg_filter_graph_process(gb, &ib, &bbob1, tb, nullptr, nullptr, nullptr, nullptr) == 0);
	FB_CHECK(zimg_filter_graph_process(ga, &ib, &baoa2, ta, nullptr, nullptr, nullptr, nullptr) == 0);

	for (unsigned y = 0; y < H; ++y)
		for (unsigned x = 0; x < W; ++x) {
			// A is the constant red-ish mapping.
			check_near("interleave-a-r", oa1[0].line<float>(y)[x], 0.9f, 1e-6f);
			check_near("interleave-a-g", oa1[1].line<float>(y)[x], 0.1f, 1e-6f);
			check_near("interleave-a-b", oa1[2].line<float>(y)[x], 0.1f, 1e-6f);
			check_near("interleave-b-r", ob1[0].line<float>(y)[x], 0.1f, 1e-6f);
			check_near("interleave-b-g", ob1[1].line<float>(y)[x], 0.9f, 1e-6f);
			check_near("interleave-b-b", ob1[2].line<float>(y)[x], 0.2f, 1e-6f);
			for (unsigned p = 0; p < 3; ++p)
				FB_CHECK(oa1[p].line<float>(y)[x] == oa2[p].line<float>(y)[x]);
		}

	zimg_filter_graph_free(ga);
	zimg_filter_graph_free(gb);
	std::free(ta);
	std::free(tb);
}

namespace {

static void expect_bad_lut(const zimg_lut3d &lut)
{
	zimg_image_format sfmt = rgb_float_format(4, 2);
	zimg_image_format dfmt = rgb_float_format(4, 2);
	zimg_graph_builder_params params = test_params();
	zimg_filter_graph *g = zimg_filter_graph_build_with_lut(&sfmt, &dfmt, &params, &lut);
	FB_CHECK(g == nullptr);
	zimg_error_code_e code = zimg_get_last_error(nullptr, 0);
	FB_CHECK(code != ZIMG_ERROR_SUCCESS);
	if (g)
		zimg_filter_graph_free(g);
}

} // namespace

FB_TEST("ownership", bad_descriptors_rejected)
{
	// size too small / too large / zero.
	{
		zimg_lut3d l{};
		float d[24] = {};
		l.data = d; l.data_count = 24; l.size = 1;
		expect_bad_lut(l);
	}
	{
		zimg_lut3d l{};
		l.size = 66; l.data = nullptr; l.data_count = 3ULL * 66 * 66 * 66;
		expect_bad_lut(l);
	}
	{
		zimg_lut3d l{};
		l.size = 0; l.data = nullptr; l.data_count = 0;
		expect_bad_lut(l);
	}
	// Multiplication overflow with a malicious huge size.
	{
		zimg_lut3d l{};
		l.size = 0xFFFFFFFEu;
		l.data = reinterpret_cast<const float *>(0x1);
		l.data_count = 24;
		expect_bad_lut(l);
	}

	LutData good(2);
	fill_nonaffine(good);

	// Null data pointer.
	{
		zimg_lut3d l = good.desc();
		l.data = nullptr;
		expect_bad_lut(l);
	}
	// Count mismatch (too few and too many).
	{
		zimg_lut3d l = good.desc();
		--l.data_count;
		expect_bad_lut(l);
	}
	{
		zimg_lut3d l = good.desc();
		++l.data_count;
		expect_bad_lut(l);
	}
	// Non-finite table values.
	{
		LutData bad = good;
		bad.tab[7] = INFINITY;
		zimg_lut3d l = bad.desc();
		expect_bad_lut(l);
	}
	{
		LutData bad = good;
		bad.tab[3] = NAN;
		zimg_lut3d l = bad.desc();
		expect_bad_lut(l);
	}
	// Bad domains.
	{
		LutData bad = good;
		bad.mn[1] = bad.mx[1];
		expect_bad_lut(bad.desc());
	}
	{
		LutData bad = good;
		bad.mn[2] = 2.0f; bad.mx[2] = 1.0f;
		expect_bad_lut(bad.desc());
	}
	{
		LutData bad = good;
		bad.mn[0] = NAN;
		expect_bad_lut(bad.desc());
	}
	{
		LutData bad = good;
		bad.mx[1] = INFINITY;
		expect_bad_lut(bad.desc());
	}
	// Null descriptor.
	{
		zimg_image_format sfmt = rgb_float_format(2, 2);
		zimg_graph_builder_params params = test_params();
		FB_CHECK(zimg_filter_graph_build_with_lut(&sfmt, &sfmt, &params, nullptr) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	}
}

FB_TEST("ownership", reject_grey_target_and_unspecified_color)
{
	LutData lut(2);
	fill_nonaffine(lut);
	zimg_lut3d desc = lut.desc();
	zimg_graph_builder_params params = test_params();

	// Greyscale destination is explicitly rejected.
	{
		zimg_image_format s = rgb_float_format(4, 4);
		zimg_image_format d;
		zimg_image_format_default(&d, ZIMG_API_VERSION);
		d.width = 4; d.height = 4;
		d.pixel_type = ZIMG_PIXEL_BYTE; d.depth = 8;
		d.color_family = ZIMG_COLOR_GREY;
		d.transfer_characteristics = ZIMG_TRANSFER_BT709;
		d.color_primaries = ZIMG_PRIMARIES_BT709;
		d.pixel_range = ZIMG_RANGE_FULL;
		zimg_filter_graph *g = zimg_filter_graph_build_with_lut(&s, &d, &params, &desc);
		FB_CHECK(g == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
		if (g)
			zimg_filter_graph_free(g);
	}

	// Unspecified primaries on the source are rejected.
	{
		zimg_image_format s = rgb_float_format(4, 4);
		s.color_primaries = ZIMG_PRIMARIES_UNSPECIFIED;
		zimg_image_format d = rgb_float_format(4, 4);
		zimg_filter_graph *g = zimg_filter_graph_build_with_lut(&s, &d, &params, &desc);
		FB_CHECK(g == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
		if (g)
			zimg_filter_graph_free(g);
	}
	// Unspecified transfer on the destination is rejected.
	{
		zimg_image_format s = rgb_float_format(4, 4);
		zimg_image_format d = rgb_float_format(4, 4);
		d.transfer_characteristics = ZIMG_TRANSFER_UNSPECIFIED;
		zimg_filter_graph *g = zimg_filter_graph_build_with_lut(&s, &d, &params, &desc);
		FB_CHECK(g == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
		if (g)
			zimg_filter_graph_free(g);
	}
}
