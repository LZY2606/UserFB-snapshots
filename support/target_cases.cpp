// Four required groups: tetrahedra, pipeline, alpha, ownership.
// This file is #included by support/target_main.cpp, which supplies FB_TEST/FB_CHECK.

#include "api/zimg.h"
#include "lut3d/lut3d.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {

struct AlignedBlock {
	void *ptr = nullptr;

	explicit AlignedBlock(size_t bytes)
	{
		size_t size = (bytes + 63) & ~static_cast<size_t>(63);
		if (posix_memalign(&ptr, 64, size ? size : 64) != 0)
			std::abort();
		std::memset(ptr, 0, size);
	}

	~AlignedBlock() { std::free(ptr); }

	AlignedBlock(const AlignedBlock &) = delete;
	AlignedBlock &operator=(const AlignedBlock &) = delete;
};

struct Image {
	zimg_image_format format{};
	unsigned planes = 0;
	size_t stride = 0;
	std::vector<std::unique_ptr<AlignedBlock>> blocks;
	zimg_image_buffer buffer{};

	explicit Image(const zimg_image_format &f) : format(f)
	{
		unsigned bps = f.pixel_type == ZIMG_PIXEL_FLOAT ? sizeof(float)
			: f.pixel_type == ZIMG_PIXEL_WORD ? sizeof(uint16_t) : sizeof(uint8_t);

		planes = f.alpha != ZIMG_ALPHA_NONE ? 4 : f.color_family == ZIMG_COLOR_GREY ? 1 : 3;
		stride = (static_cast<size_t>(f.width) * bps + 63) & ~static_cast<size_t>(63);

		for (unsigned p = 0; p < planes; ++p)
			blocks.push_back(std::make_unique<AlignedBlock>(stride * f.height));

		buffer.version = ZIMG_API_VERSION;
		for (unsigned p = 0; p < planes; ++p)
			buffer.plane[p] = { blocks[p]->ptr, static_cast<ptrdiff_t>(stride), ZIMG_BUFFER_MAX };
	}

	float *f32(unsigned p, unsigned y) const
	{
		return reinterpret_cast<float *>(static_cast<unsigned char *>(blocks[p]->ptr) + static_cast<size_t>(y) * stride);
	}

	uint8_t *u8(unsigned p, unsigned y) const
	{
		return static_cast<uint8_t *>(blocks[p]->ptr) + static_cast<size_t>(y) * stride;
	}

	zimg_image_buffer_const const_buffer() const
	{
		zimg_image_buffer_const in{};
		in.version = ZIMG_API_VERSION;
		for (unsigned p = 0; p < planes; ++p)
			in.plane[p] = { buffer.plane[p].data, buffer.plane[p].stride, buffer.plane[p].mask };
		return in;
	}
};

struct TmpBuffer {
	std::unique_ptr<AlignedBlock> block;

	explicit TmpBuffer(zimg_filter_graph *graph)
	{
		size_t size = 0;
		if (zimg_filter_graph_get_tmp_size(graph, &size) != ZIMG_ERROR_SUCCESS)
			std::abort();
		block = std::make_unique<AlignedBlock>(size);
	}

	void *ptr() const { return block->ptr; }
};

static zimg_error_code_e run_graph(zimg_filter_graph *graph, const Image &src, Image &dst)
{
	TmpBuffer tmp(graph);
	zimg_image_buffer_const src_buf = src.const_buffer();
	return zimg_filter_graph_process(graph, &src_buf, &dst.buffer, tmp.ptr(), nullptr, nullptr, nullptr, nullptr);
}

static zimg_image_format rgb_f32(unsigned width, unsigned height)
{
	zimg_image_format f{};
	zimg_image_format_default(&f, ZIMG_API_VERSION);
	f.width = width;
	f.height = height;
	f.pixel_type = ZIMG_PIXEL_FLOAT;
	f.depth = 32;
	f.color_family = ZIMG_COLOR_RGB;
	f.matrix_coefficients = ZIMG_MATRIX_RGB;
	f.transfer_characteristics = ZIMG_TRANSFER_LINEAR;
	f.color_primaries = ZIMG_PRIMARIES_BT709;
	f.pixel_range = ZIMG_RANGE_FULL;
	f.alpha = ZIMG_ALPHA_NONE;
	return f;
}

static zimg_image_format yuv_u8(unsigned width, unsigned height, unsigned sub_w = 1, unsigned sub_h = 1)
{
	zimg_image_format f{};
	zimg_image_format_default(&f, ZIMG_API_VERSION);
	f.width = width;
	f.height = height;
	f.pixel_type = ZIMG_PIXEL_BYTE;
	f.depth = 8;
	f.color_family = ZIMG_COLOR_YUV;
	f.matrix_coefficients = ZIMG_MATRIX_BT709;
	f.transfer_characteristics = ZIMG_TRANSFER_BT709;
	f.color_primaries = ZIMG_PRIMARIES_BT709;
	f.pixel_range = ZIMG_RANGE_FULL;
	f.subsample_w = sub_w;
	f.subsample_h = sub_h;
	f.alpha = ZIMG_ALPHA_NONE;
	return f;
}

static zimg_graph_builder_params point_params()
{
	zimg_graph_builder_params p{};
	zimg_graph_builder_params_default(&p, ZIMG_API_VERSION);
	p.cpu_type = ZIMG_CPU_NONE;
	p.resample_filter = ZIMG_RESIZE_POINT;
	return p;
}

struct LutDescriptor {
	unsigned size = 2;
	std::vector<float> data;
	float domain_min[3] = { 0.0f, 0.0f, 0.0f };
	float domain_max[3] = { 1.0f, 1.0f, 1.0f };

	explicit LutDescriptor(unsigned n = 2) : size(n), data(3ULL * n * n * n) {}

	void set(unsigned r, unsigned g, unsigned b, float R, float G, float B)
	{
		size_t index = (static_cast<size_t>((b * size + g) * size + r)) * 3;
		data[index + 0] = R;
		data[index + 1] = G;
		data[index + 2] = B;
	}

	zimg_lut3d get()
	{
		zimg_lut3d lut{};
		lut.size = size;
		lut.data = data.data();
		lut.data_count = data.size();
		std::memcpy(lut.domain_min, domain_min, sizeof(domain_min));
		std::memcpy(lut.domain_max, domain_max, sizeof(domain_max));
		return lut;
	}
};

static LutDescriptor public_corner_lut()
{
	LutDescriptor lut(2);
	lut.set(0, 0, 0, 0.10f, 0.20f, 0.30f);
	lut.set(1, 0, 0, 0.80f, -0.40f, 1.20f);
	lut.set(0, 1, 0, -0.60f, 0.90f, 0.40f);
	lut.set(1, 1, 0, 1.10f, 0.70f, -0.80f);
	lut.set(0, 0, 1, 0.30f, 1.30f, -0.50f);
	lut.set(1, 0, 1, -0.90f, 0.20f, 0.80f);
	lut.set(0, 1, 1, 0.60f, -0.70f, 1.10f);
	lut.set(1, 1, 1, 0.80f, 0.70f, 0.60f);
	return lut;
}

static bool close(float a, double b, double eps = 1e-5)
{
	return std::abs(static_cast<double>(a) - b) <= eps;
}

static void reference_tetra(const float corners[8][3], double fr, double fg, double fb, float out[3])
{
	double frac[3] = { fr, fg, fb };
	int order[3] = { 0, 1, 2 };
	std::sort(order, order + 3, [&](int a, int b) { return frac[a] > frac[b]; });

	double high = frac[order[0]];
	double middle = frac[order[1]];
	double low = frac[order[2]];
	double weight[4] = { 1.0 - high, high - middle, middle - low, low };

	int coord[4][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 1, 1, 1 } };
	coord[1][order[0]] = 1;
	coord[2][order[0]] = 1;
	coord[2][order[1]] = 1;

	for (unsigned ch = 0; ch < 3; ++ch) {
		out[ch] = 0.0f;
		for (unsigned k = 0; k < 4; ++k) {
			unsigned index = static_cast<unsigned>((coord[k][2] * 2 + coord[k][1]) * 2 + coord[k][0]);
			out[ch] += static_cast<float>(weight[k] * corners[index][ch]);
		}
	}
}

static void reference_trilinear(const float corners[8][3], double fr, double fg, double fb, float out[3])
{
	for (unsigned ch = 0; ch < 3; ++ch) {
		double value = 0.0;
		for (unsigned b = 0; b < 2; ++b)
			for (unsigned g = 0; g < 2; ++g)
				for (unsigned r = 0; r < 2; ++r) {
					double wr = r ? fr : 1.0 - fr;
					double wg = g ? fg : 1.0 - fg;
					double wb = b ? fb : 1.0 - fb;
					unsigned index = (b * 2 + g) * 2 + r;
					value += wr * wg * wb * corners[index][ch];
				}
		out[ch] = static_cast<float>(value);
	}
}

static void check_ordering(const char *, float r, float g, float b)
{
	static const float corners[8][3] = {
		{ 0.05f, -0.10f,  0.20f },
		{ 0.80f,  0.15f, -0.30f },
		{ -0.25f, 0.90f,  0.35f },
		{ 1.20f,  0.55f, -0.60f },
		{ 0.45f, -0.40f,  0.75f },
		{ -0.70f, 1.30f,  0.65f },
		{ 0.35f, -0.80f,  1.10f },
		{ 0.95f,  1.05f, -0.95f },
	};

	zimg::lut3d::Lut3D lut;
	lut.size = 2;
	lut.table.resize(24);
	for (unsigned i = 0; i < 8; ++i)
		std::memcpy(&lut.table[i * 3], corners[i], sizeof(corners[i]));
	lut.domain_min = { 0.0, 0.0, 0.0 };
	lut.domain_max = { 1.0, 1.0, 1.0 };

	float input[3] = { r, g, b };
	float got[3]{};
	float expected[3]{};
	float trilinear[3]{};
	zimg::lut3d::interpolate_lut3d(lut, input, got);
	reference_tetra(corners, r, g, b, expected);
	reference_trilinear(corners, r, g, b, trilinear);

	for (unsigned ch = 0; ch < 3; ++ch)
		FB_CHECK(close(got[ch], expected[ch], 2e-6));

	double tri_diff = 0.0;
	for (unsigned ch = 0; ch < 3; ++ch)
		tri_diff = std::max(tri_diff, std::abs(static_cast<double>(got[ch] - trilinear[ch])));
	FB_CHECK(tri_diff > 0.01);
}

static double bt709_gamma(double linear)
{
	return std::pow(std::max(0.0, linear), 1.0 / 2.4);
}

static uint8_t full_byte(double x)
{
	return static_cast<uint8_t>(std::lround(std::clamp(x, 0.0, 1.0) * 255.0));
}

} // namespace

FB_TEST("tetrahedra", order_r_g_b)
{
	check_ordering("rgb", 0.82f, 0.47f, 0.19f);
}

FB_TEST("tetrahedra", order_r_b_g)
{
	check_ordering("rbg", 0.82f, 0.19f, 0.47f);
}

FB_TEST("tetrahedra", order_g_r_b)
{
	check_ordering("grb", 0.47f, 0.82f, 0.19f);
}

FB_TEST("tetrahedra", order_g_b_r)
{
	check_ordering("gbr", 0.19f, 0.82f, 0.47f);
}

FB_TEST("tetrahedra", order_b_r_g)
{
	check_ordering("brg", 0.47f, 0.19f, 0.82f);
}

FB_TEST("tetrahedra", order_b_g_r)
{
	check_ordering("bgr", 0.19f, 0.47f, 0.82f);
}

FB_TEST("tetrahedra", domain_endpoints_and_clamp)
{
	zimg::lut3d::Lut3D lut;
	lut.size = 3;
	lut.table.resize(3ULL * 27);
	for (unsigned v = 0; v < 27; ++v) {
		lut.table[v * 3 + 0] = 0.01f * static_cast<float>(v);
		lut.table[v * 3 + 1] = -0.02f * static_cast<float>(v);
		lut.table[v * 3 + 2] = 0.03f * static_cast<float>(v);
	}
	lut.table[0] = 1.5f;
	lut.table[1] = -2.5f;
	lut.table[2] = 3.5f;
	size_t last = lut.table.size() - 3;
	lut.table[last + 0] = -4.0f;
	lut.table[last + 1] = 5.0f;
	lut.table[last + 2] = 6.0f;
	lut.domain_min = { -2.0, -1.0, 10.0 };
	lut.domain_max = { 4.0, 1.0, 12.0 };

	float out[3]{};
	for (float x : { -100.0f, -2.0f }) {
		float in[3] = { x, -1.0f, 10.0f };
		zimg::lut3d::interpolate_lut3d(lut, in, out);
		FB_CHECK(close(out[0], 1.5f));
		FB_CHECK(close(out[1], -2.5f));
		FB_CHECK(close(out[2], 3.5f));
	}

	for (float x : { 4.0f, 100.0f }) {
		float in[3] = { x, 1.0f, 12.0f };
		zimg::lut3d::interpolate_lut3d(lut, in, out);
		FB_CHECK(close(out[0], -4.0f));
		FB_CHECK(close(out[1], 5.0f));
		FB_CHECK(close(out[2], 6.0f));
	}

	// (-2, -1, 10) + one grid step gives vertex (1, 1, 1).
	float middle[3] = { 1.0f, 0.0f, 11.0f };
	zimg::lut3d::interpolate_lut3d(lut, middle, out);
	unsigned vertex = (1U * 3 + 1U) * 3 + 1U;
	FB_CHECK(close(out[0], lut.table[vertex * 3 + 0]));
	FB_CHECK(close(out[1], lut.table[vertex * 3 + 1]));
	FB_CHECK(close(out[2], lut.table[vertex * 3 + 2]));
}

FB_TEST("pipeline", same_format_still_applies_lut)
{
	zimg_image_format format = rgb_f32(2, 1);
	// RGB has no YUV matrix; leaving this at UNSPECIFIED must not break staging.
	format.matrix_coefficients = ZIMG_MATRIX_UNSPECIFIED;
	LutDescriptor lut = public_corner_lut();
	zimg_lut3d desc = lut.get();
	zimg_graph_builder_params params = point_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&format, &format, &params, &desc);
	FB_CHECK(graph != nullptr);
	if (!graph)
		return;

	Image src(format);
	Image dst(format);
	for (unsigned p = 0; p < 3; ++p) {
		src.f32(p, 0)[0] = 0.0f;
		src.f32(p, 0)[1] = 1.0f;
	}

	FB_CHECK(run_graph(graph, src, dst) == ZIMG_ERROR_SUCCESS);
	FB_CHECK(close(dst.f32(0, 0)[0], 0.10f));
	FB_CHECK(close(dst.f32(1, 0)[0], 0.20f));
	FB_CHECK(close(dst.f32(2, 0)[0], 0.30f));
	FB_CHECK(close(dst.f32(0, 0)[1], 0.80f));
	FB_CHECK(close(dst.f32(1, 0)[1], 0.70f));
	FB_CHECK(close(dst.f32(2, 0)[1], 0.60f));

	zimg_filter_graph_free(graph);
}

FB_TEST("pipeline", scale_transfer_matrix_and_yuv)
{
	zimg_image_format src_fmt = rgb_f32(2, 2);
	zimg_image_format dst_fmt = yuv_u8(4, 4, 1, 1);

	LutDescriptor lut(2);
	lut.set(0, 0, 0, 0.60f, 0.10f, 0.00f);
	lut.set(1, 1, 1, 1.00f, 0.30f, 0.20f);
	lut.set(1, 0, 0, 0.60f, 0.10f, 0.00f);
	lut.set(0, 1, 0, 0.60f, 0.10f, 0.00f);
	lut.set(0, 0, 1, 0.60f, 0.10f, 0.00f);
	lut.set(1, 1, 0, 1.00f, 0.30f, 0.20f);
	lut.set(1, 0, 1, 1.00f, 0.30f, 0.20f);
	lut.set(0, 1, 1, 1.00f, 0.30f, 0.20f);
	zimg_lut3d desc = lut.get();
	zimg_graph_builder_params params = point_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc);
	FB_CHECK(graph != nullptr);
	if (!graph)
		return;

	Image src(src_fmt);
	Image dst(dst_fmt);
	for (unsigned y = 0; y < 2; ++y)
		for (unsigned x = 0; x < 2; ++x)
			for (unsigned p = 0; p < 3; ++p)
				src.f32(p, y)[x] = 0.5f;

	FB_CHECK(run_graph(graph, src, dst) == ZIMG_ERROR_SUCCESS);

	double R = bt709_gamma(0.80);
	double G = bt709_gamma(0.20);
	double B = bt709_gamma(0.10);
	double Y = 0.2126 * R + 0.7152 * G + 0.0722 * B;
	int expected_y = full_byte(Y);

	for (unsigned y = 0; y < 4; ++y)
		for (unsigned x = 0; x < 4; ++x)
			FB_CHECK(std::abs(static_cast<int>(dst.u8(0, y)[x]) - expected_y) <= 2);

	// Color is not grey, so the chroma planes must carry real Cb/Cr values.
	FB_CHECK(dst.u8(1, 0)[0] < 128);
	FB_CHECK(dst.u8(2, 0)[0] > 128);
	FB_CHECK(std::abs(static_cast<int>(dst.u8(1, 0)[0]) - 100) <= 3);
	FB_CHECK(std::abs(static_cast<int>(dst.u8(2, 0)[0]) - 180) <= 3);

	zimg_filter_graph_free(graph);
}

FB_TEST("pipeline", scaled_transfer_yuv_with_translucent_alpha)
{
	zimg_image_format src_fmt = rgb_f32(2, 2);
	src_fmt.alpha = ZIMG_ALPHA_PREMULTIPLIED;
	zimg_image_format dst_fmt = yuv_u8(4, 4, 1, 1);
	dst_fmt.alpha = ZIMG_ALPHA_STRAIGHT;

	LutDescriptor lut = public_corner_lut();
	zimg_lut3d desc = lut.get();
	zimg_graph_builder_params params = point_params();
	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc);
	FB_CHECK(graph != nullptr);
	if (!graph)
		return;

	Image src(src_fmt);
	Image dst(dst_fmt);
	for (unsigned y = 0; y < 2; ++y) {
		for (unsigned x = 0; x < 2; ++x) {
			float alpha = (x == 0 && y == 0) ? 0.0f : (x == 1 && y == 0) ? 0.5f : 1.0f;
			for (unsigned p = 0; p < 3; ++p)
				src.f32(p, y)[x] = alpha * 0.5f;
			src.f32(3, y)[x] = alpha;
		}
	}

	FB_CHECK(run_graph(graph, src, dst) == ZIMG_ERROR_SUCCESS);

	int y_zero = dst.u8(0, 0)[0];
	int y_half = dst.u8(0, 0)[2];
	int y_one = dst.u8(0, 2)[0];
	FB_CHECK(y_zero == 0);
	FB_CHECK(std::abs(y_half - full_byte(bt709_gamma(0.45))) <= 2);
	FB_CHECK(std::abs(y_one - full_byte(bt709_gamma(0.45))) <= 2);

	FB_CHECK(dst.u8(3, 0)[0] == 0);
	FB_CHECK(std::abs(static_cast<int>(dst.u8(3, 0)[2]) - 128) <= 1);
	FB_CHECK(dst.u8(3, 2)[0] == 255);

	// Both LUT output colors are grey (black is cleared by zero alpha).
	FB_CHECK(std::abs(static_cast<int>(dst.u8(1, 0)[0]) - 128) <= 1);
	FB_CHECK(std::abs(static_cast<int>(dst.u8(2, 0)[0]) - 128) <= 1);

	zimg_filter_graph_free(graph);
}

static void run_float_alpha_case(zimg_alpha_type_e src_alpha, zimg_alpha_type_e dst_alpha, float expected_rgb)
{
	zimg_image_format src_fmt = rgb_f32(2, 2);
	src_fmt.alpha = src_alpha;
	zimg_image_format dst_fmt = rgb_f32(4, 4);
	dst_fmt.alpha = dst_alpha;

	LutDescriptor lut = public_corner_lut();
	zimg_lut3d desc = lut.get();
	zimg_graph_builder_params params = point_params();
	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc);
	FB_CHECK(graph != nullptr);
	if (!graph)
		return;

	Image src(src_fmt);
	Image dst(dst_fmt);
	for (unsigned y = 0; y < 2; ++y) {
		for (unsigned x = 0; x < 2; ++x) {
			float alpha = (x == 0 && y == 0) ? 0.0f : (x == 1 && y == 0) ? 0.5f : 1.0f;
			float straight = 0.5f;
			float color = src_alpha == ZIMG_ALPHA_PREMULTIPLIED ? straight * alpha : straight;
			for (unsigned p = 0; p < 3; ++p)
				src.f32(p, y)[x] = alpha == 0.0f ? 0.0f : color;
			src.f32(3, y)[x] = alpha;
		}
	}

	FB_CHECK(run_graph(graph, src, dst) == ZIMG_ERROR_SUCCESS);
	FB_CHECK(close(dst.f32(0, 0)[0], 0.0f, 1e-6));
	FB_CHECK(close(dst.f32(1, 0)[0], 0.0f, 1e-6));
	FB_CHECK(close(dst.f32(2, 0)[0], 0.0f, 1e-6));
	FB_CHECK(close(dst.f32(0, 0)[2], expected_rgb, 2e-5));
	FB_CHECK(close(dst.f32(1, 0)[2], expected_rgb, 2e-5));
	FB_CHECK(close(dst.f32(2, 0)[2], expected_rgb, 2e-5));
	FB_CHECK(close(dst.f32(3, 0)[2], 0.5f, 1e-6));

	zimg_filter_graph_free(graph);
}

FB_TEST("alpha", straight_lut_input_is_unpremultiplied)
{
	run_float_alpha_case(ZIMG_ALPHA_STRAIGHT, ZIMG_ALPHA_STRAIGHT, 0.45f);
}

FB_TEST("alpha", premultiplied_source_is_unpremultiplied_for_lut)
{
	run_float_alpha_case(ZIMG_ALPHA_PREMULTIPLIED, ZIMG_ALPHA_STRAIGHT, 0.45f);
}

FB_TEST("alpha", premultiplied_destination_is_recombined_after_lut)
{
	run_float_alpha_case(ZIMG_ALPHA_STRAIGHT, ZIMG_ALPHA_PREMULTIPLIED, 0.225f);
}

FB_TEST("ownership", release_and_interleaved_graphs)
{
	zimg_image_format format = rgb_f32(1, 1);
	zimg_graph_builder_params params = point_params();
	zimg_filter_graph *graph_a = nullptr;
	zimg_filter_graph *graph_b = nullptr;

	{
		LutDescriptor lut_a(2);
		lut_a.set(0, 0, 0, 0.10f, 0.20f, 0.30f);
		lut_a.set(1, 1, 1, 0.80f, 0.70f, 0.60f);
		zimg_lut3d desc_a = lut_a.get();
		graph_a = zimg_filter_graph_build_with_lut(&format, &format, &params, &desc_a);

		LutDescriptor lut_b(2);
		lut_b.set(0, 0, 0, 0.0f, 0.0f, 0.0f);
		lut_b.set(1, 1, 1, 1.0f, 1.0f, 1.0f);
		zimg_lut3d desc_b = lut_b.get();
		graph_b = zimg_filter_graph_build_with_lut(&format, &format, &params, &desc_b);
	}

	FB_CHECK(graph_a != nullptr);
	FB_CHECK(graph_b != nullptr);
	if (!graph_a || !graph_b) {
		zimg_filter_graph_free(graph_a);
		zimg_filter_graph_free(graph_b);
		return;
	}

	Image src(format);
	src.f32(0, 0)[0] = src.f32(1, 0)[0] = src.f32(2, 0)[0] = 0.5f;
	Image out_a1(format), out_b(format), out_a2(format);

	FB_CHECK(run_graph(graph_a, src, out_a1) == ZIMG_ERROR_SUCCESS);
	FB_CHECK(run_graph(graph_b, src, out_b) == ZIMG_ERROR_SUCCESS);
	FB_CHECK(run_graph(graph_a, src, out_a2) == ZIMG_ERROR_SUCCESS);

	FB_CHECK(close(out_a1.f32(0, 0)[0], 0.45f));
	FB_CHECK(close(out_a1.f32(1, 0)[0], 0.45f));
	FB_CHECK(close(out_a1.f32(2, 0)[0], 0.45f));
	FB_CHECK(close(out_b.f32(0, 0)[0], 0.5f));
	FB_CHECK(close(out_a2.f32(0, 0)[0], 0.45f));
	FB_CHECK(close(out_a2.f32(1, 0)[0], 0.45f));

	zimg_filter_graph_free(graph_a);
	zimg_filter_graph_free(graph_b);
}

FB_TEST("ownership", bad_descriptions_are_rejected)
{
	zimg_image_format src_fmt = rgb_f32(2, 2);
	zimg_image_format dst_fmt = rgb_f32(2, 2);
	zimg_graph_builder_params params = point_params();
	LutDescriptor valid(2);
	valid.set(0, 0, 0, 0.0f, 0.0f, 0.0f);
	valid.set(1, 1, 1, 1.0f, 1.0f, 1.0f);
	zimg_lut3d desc = valid.get();

	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, nullptr) == nullptr);

	desc.size = 1;
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc) == nullptr);
	desc.size = 66;
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc) == nullptr);
	desc.size = 2;

	desc.data_count = valid.data.size() - 1;
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc) == nullptr);
	FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	desc.data_count = valid.data.size();

	desc.data = nullptr;
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc) == nullptr);
	desc.data = valid.data.data();

	desc.domain_min[0] = 0.5f;
	desc.domain_max[0] = 0.5f;
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc) == nullptr);
	desc.domain_min[0] = 1.0f;
	desc.domain_max[0] = 0.0f;
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc) == nullptr);
	desc.domain_min[0] = 0.0f;
	desc.domain_max[1] = std::numeric_limits<float>::quiet_NaN();
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc) == nullptr);
	desc.domain_max[1] = 1.0f;

	valid.data[7] = std::numeric_limits<float>::infinity();
	desc = valid.get();
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &desc) == nullptr);
	valid.data[7] = 0.0f;

	zimg_image_format grey_fmt = dst_fmt;
	grey_fmt.color_family = ZIMG_COLOR_GREY;
	desc = valid.get();
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &grey_fmt, &params, &desc) == nullptr);

	zimg_image_format unspecified_fmt = src_fmt;
	unspecified_fmt.transfer_characteristics = ZIMG_TRANSFER_UNSPECIFIED;
	FB_CHECK(zimg_filter_graph_build_with_lut(&unspecified_fmt, &dst_fmt, &params, &desc) == nullptr);
	unspecified_fmt = src_fmt;
	unspecified_fmt.color_primaries = ZIMG_PRIMARIES_UNSPECIFIED;
	FB_CHECK(zimg_filter_graph_build_with_lut(&unspecified_fmt, &dst_fmt, &params, &desc) == nullptr);

	zimg_lut3d overflow_desc = valid.get();
	overflow_desc.size = std::numeric_limits<unsigned>::max();
	FB_CHECK(zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &overflow_desc) == nullptr);
}
