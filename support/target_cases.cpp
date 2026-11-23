#include "api/zimg.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr double EPS_PIXEL = 2e-3;
constexpr double EPS_LUT = 2e-5;

// ---------------------------------------------------------------------------
// Format and buffer helpers.
// ---------------------------------------------------------------------------

zimg_image_format make_fmt(unsigned w, unsigned h, zimg_pixel_type_e pixel_type, zimg_color_family_e family,
                           zimg_matrix_coefficients_e matrix, zimg_transfer_characteristics_e transfer,
                           zimg_color_primaries_e primaries, zimg_pixel_range_e range, zimg_alpha_type_e alpha,
                           unsigned subsample_w = 0, unsigned subsample_h = 0)
{
	zimg_image_format format;
	zimg_image_format_default(&format, ZIMG_API_VERSION);

	format.width = w;
	format.height = h;
	format.pixel_type = pixel_type;
	format.color_family = family;
	format.matrix_coefficients = matrix;
	format.transfer_characteristics = transfer;
	format.color_primaries = primaries;
	format.pixel_range = range;
	format.alpha = alpha;
	format.subsample_w = subsample_w;
	format.subsample_h = subsample_h;
	format.depth = pixel_type == ZIMG_PIXEL_BYTE ? 8 : pixel_type == ZIMG_PIXEL_WORD ? 16 :
	               pixel_type == ZIMG_PIXEL_HALF ? 16 : 32;
	return format;
}

zimg_graph_builder_params make_params()
{
	zimg_graph_builder_params params;
	zimg_graph_builder_params_default(&params, ZIMG_API_VERSION);
	params.cpu_type = ZIMG_CPU_NONE;
	return params;
}

unsigned bytes_per_sample(zimg_pixel_type_e type)
{
	switch (type) {
	case ZIMG_PIXEL_BYTE: return 1;
	case ZIMG_PIXEL_WORD:
	case ZIMG_PIXEL_HALF: return 2;
	default: return 4;
	}
}

unsigned plane_count(const zimg_image_format &format)
{
	return (format.color_family == ZIMG_COLOR_GREY ? 1U : 3U) + (format.alpha != ZIMG_ALPHA_NONE ? 1U : 0U);
}

unsigned plane_height(const zimg_image_format &format, unsigned p)
{
	if (p >= 1 && p < 3 && format.color_family == ZIMG_COLOR_YUV)
		return format.height >> format.subsample_h;
	return format.height;
}

// Fully allocated planar test image.
struct Image {
	zimg_image_format format{};
	unsigned stride = 0;
	std::vector<unsigned char> storage;
	const void *cplane[4] = {};
	void *plane[4] = {};

	void allocate(const zimg_image_format &fmt)
	{
		format = fmt;

		unsigned bps = bytes_per_sample(format.pixel_type);
		stride = (format.width * bps + 63) & ~63U;

		size_t offset = 0;
		std::vector<size_t> offsets(4, 0);

		for (unsigned p = 0; p < plane_count(format); ++p) {
			offset = (offset + 63) & ~static_cast<size_t>(63);
			offsets[p] = offset;
			offset += static_cast<size_t>(stride) * plane_height(format, p);
		}

		storage.assign(offset, 0);

		for (unsigned p = 0; p < 4; ++p) {
			plane[p] = storage.data() + offsets[p];
			cplane[p] = plane[p];
		}
	}

	float *float_line(unsigned p, unsigned row) { return reinterpret_cast<float *>(plane[p]) + static_cast<size_t>(row) * (stride / sizeof(float)); }
	const float *float_line(unsigned p, unsigned row) const { return reinterpret_cast<const float *>(cplane[p]) + static_cast<size_t>(row) * (stride / sizeof(float)); }
	uint8_t *byte_line(unsigned p, unsigned row) { return reinterpret_cast<uint8_t *>(plane[p]) + static_cast<size_t>(row) * stride; }
	const uint8_t *byte_line(unsigned p, unsigned row) const { return reinterpret_cast<const uint8_t *>(cplane[p]) + static_cast<size_t>(row) * stride; }

	void set_float(unsigned p, unsigned x, unsigned y, float value) { float_line(p, y)[x] = value; }
	void set_byte(unsigned p, unsigned x, unsigned y, uint8_t value) { byte_line(p, y)[x] = value; }

	zimg_image_buffer_const const_buffer() const
	{
		zimg_image_buffer_const buf = { ZIMG_API_VERSION };
		for (unsigned p = 0; p < 4; ++p) {
			buf.plane[p].data = cplane[p];
			buf.plane[p].stride = static_cast<ptrdiff_t>(stride);
			buf.plane[p].mask = ZIMG_BUFFER_MAX;
		}
		return buf;
	}

	zimg_image_buffer buffer()
	{
		zimg_image_buffer buf = { ZIMG_API_VERSION };
		for (unsigned p = 0; p < 4; ++p) {
			buf.plane[p].data = plane[p];
			buf.plane[p].stride = static_cast<ptrdiff_t>(stride);
			buf.plane[p].mask = ZIMG_BUFFER_MAX;
		}
		return buf;
	}
};

void run_graph(const zimg_filter_graph *graph, const Image &src, Image &dst)
{
	size_t tmp_size = 0;
	if (zimg_filter_graph_get_tmp_size(graph, &tmp_size))
		std::abort();

	std::vector<unsigned char> tmp(tmp_size + 64);
	void *tmp_ptr = tmp.empty() ? nullptr : reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(tmp.data()) + 63) & ~static_cast<uintptr_t>(63));

	zimg_image_buffer_const src_buf = src.const_buffer();
	zimg_image_buffer dst_buf = dst.buffer();

	if (zimg_filter_graph_process(graph, &src_buf, &dst_buf, tmp_ptr, nullptr, nullptr, nullptr, nullptr))
		std::abort();
}

bool close(double a, double b, double eps)
{
	return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------
// 3D LUT construction and independent reference implementation.
// ---------------------------------------------------------------------------

struct Table {
	unsigned size = 0;
	std::vector<float> data;
	float domain_min[3] = { 0.0f, 0.0f, 0.0f };
	float domain_max[3] = { 1.0f, 1.0f, 1.0f };

	const float *at(unsigned r, unsigned g, unsigned b) const
	{
		size_t idx = ((static_cast<size_t>(b) * size + g) * size + r) * 3;
		return &data[idx];
	}

	zimg_lut3d descriptor() const
	{
		zimg_lut3d lut{};
		lut.size = size;
		lut.data = data.data();
		lut.data_count = data.size();
		for (unsigned c = 0; c < 3; ++c) {
			lut.domain_min[c] = domain_min[c];
			lut.domain_max[c] = domain_max[c];
		}
		return lut;
	}
};

// A non-affine table: each output channel is a different smooth non-linear
// function of the three inputs, so tetrahedral and trilinear interpolation
// produce different results and no channel is a linear ramp.
Table make_nonaffine_table(unsigned size)
{
	Table table{};
	table.size = size;
	table.data.resize(static_cast<size_t>(size) * size * size * 3);

	for (unsigned c = 0; c < 3; ++c) {
		table.domain_min[c] = -0.25f + 0.05f * static_cast<float>(c);
		table.domain_max[c] = 1.0f + 0.1f * static_cast<float>(c);
	}

	for (unsigned b = 0; b < size; ++b) {
		for (unsigned g = 0; g < size; ++g) {
			for (unsigned r = 0; r < size; ++r) {
				double fr = static_cast<double>(r) / (size - 1);
				double fg = static_cast<double>(g) / (size - 1);
				double fb = static_cast<double>(b) / (size - 1);
				float *px = &table.data[((static_cast<size_t>(b) * size + g) * size + r) * 3];

				px[0] = static_cast<float>(0.9 * fr * fr + 0.1 * fg - 0.05 * fb);
				px[1] = static_cast<float>(0.2 * fr + 0.7 * fg * fg + 0.05 * fb * fr + 0.01);
				px[2] = static_cast<float>(0.1 * fr * fg + 0.5 * fb * fb * fb - 0.03);
			}
		}
	}

	return table;
}

// Independent tetrahedral interpolation used to check the library.
void reference_lookup(const Table &table, const float in[3], float out[3])
{
	unsigned idx[3];
	double frac[3];

	for (unsigned c = 0; c < 3; ++c) {
		double x = (static_cast<double>(in[c]) - table.domain_min[c]) / (static_cast<double>(table.domain_max[c]) - table.domain_min[c]);
		x = std::min(std::max(x, 0.0), 1.0);

		double g = x * (table.size - 1);
		unsigned i = static_cast<unsigned>(std::floor(g));
		double f = g - i;
		if (i > table.size - 2) {
			i = table.size - 2;
			f = 1.0;
		}

		idx[c] = i;
		frac[c] = f;
	}

	const float *p000 = table.at(idx[0], idx[1], idx[2]);
	const float *p100 = table.at(idx[0] + 1, idx[1], idx[2]);
	const float *p010 = table.at(idx[0], idx[1] + 1, idx[2]);
	const float *p001 = table.at(idx[0], idx[1], idx[2] + 1);
	const float *p110 = table.at(idx[0] + 1, idx[1] + 1, idx[2]);
	const float *p101 = table.at(idx[0] + 1, idx[1], idx[2] + 1);
	const float *p011 = table.at(idx[0], idx[1] + 1, idx[2] + 1);
	const float *p111 = table.at(idx[0] + 1, idx[1] + 1, idx[2] + 1);

	double fx = frac[0], fy = frac[1], fz = frac[2];

	for (unsigned ch = 0; ch < 3; ++ch) {
		double v;

		if (fx >= fy && fy >= fz) {
			v = p000[ch] * (1 - fx) + p100[ch] * (fx - fy) + p110[ch] * (fy - fz) + p111[ch] * fz;
		} else if (fx >= fz && fz >= fy) {
			v = p000[ch] * (1 - fx) + p100[ch] * (fx - fz) + p101[ch] * (fz - fy) + p111[ch] * fy;
		} else if (fz >= fx && fx >= fy) {
			v = p000[ch] * (1 - fz) + p001[ch] * (fz - fx) + p101[ch] * (fx - fy) + p111[ch] * fy;
		} else if (fz >= fy && fy >= fx) {
			v = p000[ch] * (1 - fz) + p001[ch] * (fz - fy) + p011[ch] * (fy - fx) + p111[ch] * fx;
		} else if (fy >= fz && fz >= fx) {
			v = p000[ch] * (1 - fy) + p010[ch] * (fy - fz) + p011[ch] * (fz - fx) + p111[ch] * fx;
		} else {
			v = p000[ch] * (1 - fy) + p010[ch] * (fy - fx) + p110[ch] * (fx - fz) + p111[ch] * fz;
		}

		out[ch] = static_cast<float>(v);
	}
}

// ---------------------------------------------------------------------------
// Two-parameter sRGB transfer functions (ZIMG_TRANSFER_IEC_61966_2_1).
// ---------------------------------------------------------------------------

constexpr double SRGB_ALPHA = 1.055010718947587;
constexpr double SRGB_BETA = 0.003041282560128;

double srgb_to_linear(double x)
{
	if (x < 12.92 * SRGB_BETA)
		return x / 12.92;
	return std::pow((x + (SRGB_ALPHA - 1.0)) / SRGB_ALPHA, 2.4);
}

double linear_to_srgb(double x)
{
	x = std::max(x, 0.0);
	if (x < SRGB_BETA)
		return x * 12.92;
	return SRGB_ALPHA * std::pow(x, 1.0 / 2.4) - (SRGB_ALPHA - 1.0);
}

// BT.709 non-constant-luminance RGB to YUV in the float convention, where the
// luma is in [0, 1] and the chroma channels are centered at zero.
void rgb_to_yuv709_float(double r, double g, double b, double &y, double &u, double &v)
{
	constexpr double kr = 0.2126;
	constexpr double kb = 0.0722;
	double kg = 1.0 - kr - kb;

	y = kr * r + kg * g + kb * b;
	u = 0.5 * (b - y) / (1.0 - kb);
	v = 0.5 * (r - y) / (1.0 - kr);
}

// ---------------------------------------------------------------------------
// Tetrahedral interpolation tests.
// ---------------------------------------------------------------------------

// Cover all six orderings of the fractional parts within a single grid cell.
void test_tetrahedra_orderings()
{
	const unsigned size = 5;
	Table table = make_nonaffine_table(size);
	zimg_lut3d lut = table.descriptor();

	zimg_image_format fmt = make_fmt(6, 1, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	const double fr[6][3] = {
		{ 0.9, 0.6, 0.2 }, // fx > fy > fz
		{ 0.9, 0.2, 0.6 }, // fx > fz > fy
		{ 0.6, 0.2, 0.9 }, // fz > fx > fy
		{ 0.2, 0.6, 0.9 }, // fz > fy > fx
		{ 0.2, 0.9, 0.6 }, // fy > fz > fx
		{ 0.6, 0.9, 0.2 }, // fy > fx > fz
	};

	Image src;
	src.allocate(fmt);
	Image dst;
	dst.allocate(fmt);

	double sample[6][3];
	for (unsigned t = 0; t < 6; ++t) {
		for (unsigned c = 0; c < 3; ++c) {
			// Second grid cell, away from the domain edge.
			double coord = table.domain_min[c] + (1.0 + fr[t][c]) / (size - 1) *
			               (table.domain_max[c] - table.domain_min[c]);
			sample[t][c] = coord;
			src.set_float(c, t, 0, static_cast<float>(coord));
		}
	}

	run_graph(graph, src, dst);

	for (unsigned t = 0; t < 6; ++t) {
		float in[3] = { static_cast<float>(sample[t][0]), static_cast<float>(sample[t][1]), static_cast<float>(sample[t][2]) };
		float expected[3];
		reference_lookup(table, in, expected);

		for (unsigned c = 0; c < 3; ++c)
			FB_CHECK(close(dst.float_line(c, 0)[t], expected[c], EPS_LUT));
	}

	zimg_filter_graph_free(graph);
}

// Domain endpoints and clamping.
void test_tetrahedra_domain()
{
	const unsigned size = 4;
	Table table = make_nonaffine_table(size);
	zimg_lut3d lut = table.descriptor();

	zimg_image_format fmt = make_fmt(6, 1, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	Image src;
	src.allocate(fmt);
	Image dst;
	dst.allocate(fmt);

	float samples[6][3] = {
		{ table.domain_min[0], table.domain_min[1], table.domain_min[2] },
		{ table.domain_max[0], table.domain_max[1], table.domain_max[2] },
		{ table.domain_min[0] - 10.0f, table.domain_min[1] - 10.0f, table.domain_min[2] - 10.0f },
		{ table.domain_max[0] + 10.0f, table.domain_max[1] + 10.0f, table.domain_max[2] + 10.0f },
		{ table.domain_min[0] - 10.0f, table.domain_max[1] + 10.0f, table.domain_max[2] },
		{ table.domain_max[0], table.domain_max[1] + 10.0f, table.domain_min[2] - 10.0f },
	};

	for (unsigned t = 0; t < 6; ++t) {
		for (unsigned c = 0; c < 3; ++c)
			src.set_float(c, t, 0, samples[t][c]);
	}

	run_graph(graph, src, dst);

	for (unsigned t = 0; t < 6; ++t) {
		float clamped[3];
		for (unsigned c = 0; c < 3; ++c)
			clamped[c] = std::min(std::max(samples[t][c], table.domain_min[c]), table.domain_max[c]);

		float expected[3];
		reference_lookup(table, clamped, expected);

		for (unsigned c = 0; c < 3; ++c)
			FB_CHECK(close(dst.float_line(c, 0)[t], expected[c], EPS_LUT));
	}

	// The exact top grid point must be the last table entry.
	FB_CHECK(close(dst.float_line(0, 0)[1], table.at(size - 1, size - 1, size - 1)[0], 1e-6));
	FB_CHECK(close(dst.float_line(1, 0)[1], table.at(size - 1, size - 1, size - 1)[1], 1e-6));
	FB_CHECK(close(dst.float_line(2, 0)[1], table.at(size - 1, size - 1, size - 1)[2], 1e-6));

	zimg_filter_graph_free(graph);
}

// Values outside [0, 1] must not be clipped for float output.
void test_tetrahedra_out_of_range()
{
	Table table{};
	table.size = 2;
	table.data.assign(2 * 2 * 2 * 3, 0.0f);

	for (unsigned i = 0; i < 8; ++i) {
		table.data[i * 3 + 0] = i % 2 ? 2.5f : -0.75f;
		table.data[i * 3 + 1] = i % 2 ? -1.25f : 1.75f;
		table.data[i * 3 + 2] = 0.5f;
	}

	zimg_lut3d lut = table.descriptor();
	zimg_image_format fmt = make_fmt(1, 1, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	Image src;
	src.allocate(fmt);
	Image dst;
	dst.allocate(fmt);

	src.set_float(0, 0, 0, 1.0f);
	src.set_float(1, 0, 0, 1.0f);
	src.set_float(2, 0, 0, 1.0f);

	run_graph(graph, src, dst);

	FB_CHECK(close(dst.float_line(0, 0)[0], 2.5, 1e-6));
	FB_CHECK(close(dst.float_line(1, 0)[0], -1.25, 1e-6));
	FB_CHECK(close(dst.float_line(2, 0)[0], 0.5, 1e-6));

	zimg_filter_graph_free(graph);
}

// ---------------------------------------------------------------------------
// Pipeline tests.
// ---------------------------------------------------------------------------

// Scaling in linear light (the graph converts to the LUT working space before
// resampling), non-linear transfer, and the LUT, all chained.
void test_pipeline_scale_transfer()
{
	const unsigned src_w = 8, src_h = 8;
	const unsigned dst_w = 16, dst_h = 16;

	Table table = make_nonaffine_table(5);
	zimg_lut3d lut = table.descriptor();

	zimg_image_format src_fmt = make_fmt(src_w, src_h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                     ZIMG_TRANSFER_IEC_61966_2_1, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_image_format dst_fmt = src_fmt;
	dst_fmt.width = dst_w;
	dst_fmt.height = dst_h;

	zimg_graph_builder_params params = make_params();
	params.resample_filter = ZIMG_RESIZE_BILINEAR;
	params.filter_param_a = NAN;
	params.filter_param_b = NAN;

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	Image src;
	src.allocate(src_fmt);
	for (unsigned y = 0; y < src_h; ++y) {
		for (unsigned x = 0; x < src_w; ++x) {
			src.set_float(0, x, y, static_cast<float>(x) / (src_w - 1) * 0.8f + 0.1f);
			src.set_float(1, x, y, static_cast<float>(y) / (src_h - 1) * 0.8f + 0.1f);
			src.set_float(2, x, y, 0.35f);
		}
	}

	Image dst;
	dst.allocate(dst_fmt);

	run_graph(graph, src, dst);

	// Bilinear resampling of the linearized source: the graph converts to the
	// LUT working space (linear light) before resampling.
	auto interpolate_linear = [&](unsigned p, unsigned x, unsigned y) {
		double sx = (x + 0.5) * src_w / dst_w - 0.5;
		double sy = (y + 0.5) * src_h / dst_h - 0.5;
		sx = std::min(std::max(sx, 0.0), static_cast<double>(src_w - 1));
		sy = std::min(std::max(sy, 0.0), static_cast<double>(src_h - 1));
		unsigned x0 = static_cast<unsigned>(std::floor(sx));
		unsigned y0 = static_cast<unsigned>(std::floor(sy));
		unsigned x1 = std::min(x0 + 1, src_w - 1);
		unsigned y1 = std::min(y0 + 1, src_h - 1);
		double tx = sx - x0, ty = sy - y0;

		double v00 = srgb_to_linear(src.float_line(p, y0)[x0]);
		double v10 = srgb_to_linear(src.float_line(p, y0)[x1]);
		double v01 = srgb_to_linear(src.float_line(p, y1)[x0]);
		double v11 = srgb_to_linear(src.float_line(p, y1)[x1]);
		return (1 - ty) * ((1 - tx) * v00 + tx * v10) + ty * ((1 - tx) * v01 + tx * v11);
	};

	const unsigned probes[3][2] = { { 8, 8 }, { 4, 12 }, { 11, 3 } };

	for (const auto &probe : probes) {
		unsigned x = probe[0], y = probe[1];

		float lut_in[3];
		for (unsigned c = 0; c < 3; ++c)
			lut_in[c] = static_cast<float>(interpolate_linear(c, x, y));

		float lut_out[3];
		reference_lookup(table, lut_in, lut_out);

		for (unsigned c = 0; c < 3; ++c)
			FB_CHECK(close(dst.float_line(c, y)[x], linear_to_srgb(lut_out[c]), EPS_PIXEL));
	}

	zimg_filter_graph_free(graph);
}

// YUV output: chroma subsampling, YUV matrix, and depth quantization all
// happen after the LUT.
void test_pipeline_yuv_output()
{
	const unsigned w = 16, h = 16;

	Table table = make_nonaffine_table(5);
	zimg_lut3d lut = table.descriptor();

	zimg_image_format src_fmt = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                     ZIMG_TRANSFER_IEC_61966_2_1, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_image_format dst_fmt = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_YUV, ZIMG_MATRIX_BT709,
	                                     ZIMG_TRANSFER_IEC_61966_2_1, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE,
	                                     1, 1);
	dst_fmt.chroma_location = ZIMG_CHROMA_LEFT;

	zimg_graph_builder_params params = make_params();
	params.resample_filter = ZIMG_RESIZE_POINT;
	params.resample_filter_uv = ZIMG_RESIZE_POINT;
	params.filter_param_a = NAN;
	params.filter_param_b = NAN;

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	// Constant source so chroma subsampling is exact.
	const double in_encoded[3] = { 0.6, 0.4, 0.25 };

	Image src;
	src.allocate(src_fmt);
	for (unsigned y = 0; y < h; ++y) {
		for (unsigned x = 0; x < w; ++x) {
			for (unsigned c = 0; c < 3; ++c)
				src.set_float(c, x, y, static_cast<float>(in_encoded[c]));
		}
	}

	Image dst;
	dst.allocate(dst_fmt);
	run_graph(graph, src, dst);

	float lut_in[3];
	for (unsigned c = 0; c < 3; ++c)
		lut_in[c] = static_cast<float>(srgb_to_linear(in_encoded[c]));

	float lut_out[3];
	reference_lookup(table, lut_in, lut_out);

	double encoded_out[3];
	for (unsigned c = 0; c < 3; ++c)
		encoded_out[c] = linear_to_srgb(lut_out[c]);

	double y, u, v;
	rgb_to_yuv709_float(encoded_out[0], encoded_out[1], encoded_out[2], y, u, v);

	// Luma at full resolution; chroma at 4:2:0 resolution.
	for (unsigned row = 0; row < h; ++row)
		FB_CHECK(close(dst.float_line(0, row)[0], y, EPS_PIXEL));
	for (unsigned row = 0; row < h / 2; ++row) {
		FB_CHECK(close(dst.float_line(1, row)[0], u, EPS_PIXEL));
		FB_CHECK(close(dst.float_line(2, row)[0], v, EPS_PIXEL));
	}

	// Integer output path: quantize to 8-bit full range using the existing rule.
	zimg_image_format dst8 = dst_fmt;
	dst8.pixel_type = ZIMG_PIXEL_BYTE;
	dst8.depth = 8;

	zimg_filter_graph *graph8 = zimg_filter_graph_build_with_lut(&src_fmt, &dst8, &params, &lut);
	FB_CHECK(graph8 != nullptr);

	Image dst_i;
	dst_i.allocate(dst8);
	run_graph(graph8, src, dst_i);

	auto quant_luma = [](double value) {
		double scaled = std::min(std::max(value, 0.0), 1.0) * 255.0;
		return static_cast<int>(std::lrint(scaled));
	};
	auto quant_chroma = [](double value) {
		double scaled = std::min(std::max(value * 255.0 + 128.0, 0.0), 255.0);
		return static_cast<int>(std::lrint(scaled));
	};

	FB_CHECK(static_cast<int>(dst_i.byte_line(0, 0)[0]) == quant_luma(y));
	FB_CHECK(static_cast<int>(dst_i.byte_line(1, 0)[0]) == quant_chroma(u));
	FB_CHECK(static_cast<int>(dst_i.byte_line(2, 0)[0]) == quant_chroma(v));

	zimg_filter_graph_free(graph8);
	zimg_filter_graph_free(graph);
}

// ---------------------------------------------------------------------------
// Alpha tests.
// ---------------------------------------------------------------------------

// Premultiplied source resized to straight output: the LUT sees unpremultiplied
// color, and color under alpha == 0 is suppressed even though the LUT maps
// black to a non-zero value.
void test_alpha_premultiplied()
{
	const unsigned src_w = 4, src_h = 4;
	const unsigned dst_w = 8, dst_h = 8;

	// LUT that maps every input, including black, to a non-zero color.
	Table table{};
	table.size = 2;
	table.data.assign(2 * 2 * 2 * 3, 0.0f);
	for (unsigned i = 0; i < 8; ++i) {
		table.data[i * 3 + 0] = 0.6f;
		table.data[i * 3 + 1] = 0.3f;
		table.data[i * 3 + 2] = 0.2f;
	}

	zimg_lut3d lut = table.descriptor();

	zimg_image_format src_fmt = make_fmt(src_w, src_h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                     ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_PREMULTIPLIED);
	zimg_image_format dst_fmt = make_fmt(dst_w, dst_h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                     ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_STRAIGHT);

	zimg_graph_builder_params params = make_params();
	params.resample_filter = ZIMG_RESIZE_POINT;
	params.filter_param_a = NAN;
	params.filter_param_b = NAN;

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&src_fmt, &dst_fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	// Left half opaque, right half transparent.
	Image src;
	src.allocate(src_fmt);
	for (unsigned y = 0; y < src_h; ++y) {
		for (unsigned x = 0; x < src_w; ++x) {
			bool opaque = x < src_w / 2;
			float a = opaque ? 1.0f : 0.0f;
			src.set_float(0, x, y, opaque ? 0.4f : 0.0f);
			src.set_float(1, x, y, opaque ? 0.2f : 0.0f);
			src.set_float(2, x, y, opaque ? 0.1f : 0.0f);
			src.set_float(3, x, y, a);
		}
	}

	Image dst;
	dst.allocate(dst_fmt);
	run_graph(graph, src, dst);

	for (unsigned y = 0; y < dst_h; ++y) {
		for (unsigned x = 0; x < dst_w / 2; ++x) {
			FB_CHECK(close(dst.float_line(0, y)[x], 0.6, 1e-5));
			FB_CHECK(close(dst.float_line(1, y)[x], 0.3, 1e-5));
			FB_CHECK(close(dst.float_line(2, y)[x], 0.2, 1e-5));
		}
		for (unsigned x = dst_w / 2; x < dst_w; ++x) {
			FB_CHECK(dst.float_line(3, y)[x] == 0.0f);
			FB_CHECK(dst.float_line(0, y)[x] == 0.0f);
			FB_CHECK(dst.float_line(1, y)[x] == 0.0f);
			FB_CHECK(dst.float_line(2, y)[x] == 0.0f);
		}
	}

	zimg_filter_graph_free(graph);
}

// A premultiplied and a straight-alpha source carrying the same straight color
// must produce the same LUT result (modulo re-premultiplication).
void test_alpha_unpremultiply()
{
	const unsigned w = 4, h = 4;

	Table table = make_nonaffine_table(5);
	zimg_lut3d lut = table.descriptor();

	zimg_image_format premul = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                    ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_PREMULTIPLIED);
	zimg_image_format straight = premul;
	straight.alpha = ZIMG_ALPHA_STRAIGHT;

	zimg_graph_builder_params params = make_params();

	Image src_p, src_s;
	src_p.allocate(premul);
	src_s.allocate(straight);

	for (unsigned y = 0; y < h; ++y) {
		for (unsigned x = 0; x < w; ++x) {
			const float a = 0.5f;
			src_s.set_float(0, x, y, 0.5f);
			src_s.set_float(1, x, y, 0.4f);
			src_s.set_float(2, x, y, 0.3f);
			src_s.set_float(3, x, y, a);

			src_p.set_float(0, x, y, 0.5f * a);
			src_p.set_float(1, x, y, 0.4f * a);
			src_p.set_float(2, x, y, 0.3f * a);
			src_p.set_float(3, x, y, a);
		}
	}

	zimg_filter_graph *g_premul = zimg_filter_graph_build_with_lut(&premul, &premul, &params, &lut);
	zimg_filter_graph *g_straight = zimg_filter_graph_build_with_lut(&straight, &straight, &params, &lut);
	FB_CHECK(g_premul != nullptr);
	FB_CHECK(g_straight != nullptr);

	Image dst_p, dst_s;
	dst_p.allocate(premul);
	dst_s.allocate(straight);

	run_graph(g_premul, src_p, dst_p);
	run_graph(g_straight, src_s, dst_s);

	float lut_in[3] = { 0.5f, 0.4f, 0.3f };
	float expected[3];
	reference_lookup(table, lut_in, expected);

	for (unsigned c = 0; c < 3; ++c) {
		FB_CHECK(close(dst_s.float_line(c, 1)[1], expected[c], EPS_LUT));
		FB_CHECK(close(dst_p.float_line(c, 1)[1], expected[c] * 0.5, EPS_LUT));
	}

	zimg_filter_graph_free(g_straight);
	zimg_filter_graph_free(g_premul);
}

// ---------------------------------------------------------------------------
// Ownership tests.
// ---------------------------------------------------------------------------

// The graph takes a snapshot: modifying and freeing the source table after
// building must not affect processing.
void test_ownership_snapshot()
{
	const unsigned w = 2, h = 1;

	Table table{};
	table.size = 2;
	table.data.assign(2 * 2 * 2 * 3, 0.0f);
	for (unsigned i = 0; i < 8; ++i) {
		table.data[i * 3 + 0] = 0.1f;
		table.data[i * 3 + 1] = 0.2f;
		table.data[i * 3 + 2] = 0.3f;
	}

	zimg_lut3d lut = table.descriptor();
	zimg_image_format fmt = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	// Overwrite and release the original table.
	std::fill(table.data.begin(), table.data.end(), 0.9f);
	std::vector<float> replacement(static_cast<size_t>(2) * 2 * 2 * 3, -5.0f);
	table.data.swap(replacement);
	table.data.clear();
	table.data.shrink_to_fit();

	Image src;
	src.allocate(fmt);
	Image dst;
	dst.allocate(fmt);

	src.set_float(0, 0, 0, 0.5f);
	src.set_float(1, 0, 0, 0.5f);
	src.set_float(2, 0, 0, 0.5f);

	run_graph(graph, src, dst);

	FB_CHECK(close(dst.float_line(0, 0)[0], 0.1, 1e-6));
	FB_CHECK(close(dst.float_line(1, 0)[0], 0.2, 1e-6));
	FB_CHECK(close(dst.float_line(2, 0)[0], 0.3, 1e-6));

	zimg_filter_graph_free(graph);
}

// Two graphs built from different tables and processed alternately must not
// contaminate each other.
void test_ownership_isolation()
{
	const unsigned w = 1, h = 1;

	auto make_table = [](float value) {
		Table t{};
		t.size = 2;
		t.data.assign(2 * 2 * 2 * 3, value);
		return t;
	};

	Table table_a = make_table(0.2f);
	Table table_b = make_table(0.8f);

	zimg_lut3d lut_a = table_a.descriptor();
	zimg_lut3d lut_b = table_b.descriptor();

	zimg_image_format fmt = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph_a = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut_a);
	zimg_filter_graph *graph_b = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut_b);
	FB_CHECK(graph_a != nullptr);
	FB_CHECK(graph_b != nullptr);

	Image src;
	src.allocate(fmt);
	src.set_float(0, 0, 0, 0.5f);
	src.set_float(1, 0, 0, 0.5f);
	src.set_float(2, 0, 0, 0.5f);

	Image dst_a, dst_b;
	dst_a.allocate(fmt);
	dst_b.allocate(fmt);

	for (unsigned it = 0; it < 3; ++it) {
		run_graph(graph_a, src, dst_a);
		FB_CHECK(close(dst_a.float_line(0, 0)[0], 0.2, 1e-6));

		run_graph(graph_b, src, dst_b);
		FB_CHECK(close(dst_b.float_line(0, 0)[0], 0.8, 1e-6));
	}

	zimg_filter_graph_free(graph_b);
	zimg_filter_graph_free(graph_a);
}

// The LUT must run when source and destination formats are identical, even
// with a non-linear transfer function. The graph must linearize the source,
// apply the lookup, and re-encode, which a plain no-op path would skip.
void test_ownership_identical_formats()
{
	const unsigned w = 3, h = 2;

	Table table = make_nonaffine_table(4);
	zimg_lut3d lut = table.descriptor();

	zimg_image_format fmt = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_IEC_61966_2_1, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	Image src;
	src.allocate(fmt);
	Image dst;
	dst.allocate(fmt);

	for (unsigned y = 0; y < h; ++y) {
		for (unsigned x = 0; x < w; ++x) {
			src.set_float(0, x, y, 0.2f + 0.1f * x);
			src.set_float(1, x, y, 0.3f + 0.1f * y);
			src.set_float(2, x, y, 0.4f);
		}
	}

	run_graph(graph, src, dst);

	for (unsigned y = 0; y < h; ++y) {
		for (unsigned x = 0; x < w; ++x) {
			float lin_in[3];
			for (unsigned c = 0; c < 3; ++c)
				lin_in[c] = static_cast<float>(srgb_to_linear(src.float_line(c, y)[x]));

			float lut_out[3];
			reference_lookup(table, lin_in, lut_out);

			for (unsigned c = 0; c < 3; ++c)
				FB_CHECK(close(dst.float_line(c, y)[x], linear_to_srgb(lut_out[c]), EPS_LUT));
		}
	}

	bool changed = false;
	for (unsigned c = 0; c < 3; ++c) {
		if (!close(dst.float_line(c, 0)[0], src.float_line(c, 0)[0], 1e-4))
			changed = true;
	}
	FB_CHECK(changed);

	// The graph must be a real transform, not a no-op copy.
	size_t tmp_size = 0;
	FB_CHECK(zimg_filter_graph_get_tmp_size(graph, &tmp_size) == 0);

	zimg_filter_graph_free(graph);
}

// Invalid descriptors must be rejected through the error interface.
void test_ownership_bad_descriptor()
{
	zimg_image_format fmt = make_fmt(4, 4, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	std::vector<float> data(2 * 2 * 2 * 3, 0.5f);

	zimg_lut3d base{};
	base.size = 2;
	base.data = data.data();
	base.data_count = data.size();
	for (unsigned c = 0; c < 3; ++c) {
		base.domain_min[c] = 0.0f;
		base.domain_max[c] = 1.0f;
	}

	// Size out of range.
	{
		zimg_lut3d bad = base;
		bad.size = 1;
		FB_CHECK(zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &bad) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);

		bad.size = 66;
		FB_CHECK(zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &bad) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	}

	// Count mismatch.
	{
		zimg_lut3d bad = base;
		bad.data_count = data.size() - 1;
		FB_CHECK(zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &bad) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	}

	// Non-finite table value.
	{
		std::vector<float> nan_data = data;
		nan_data[3] = std::numeric_limits<float>::quiet_NaN();
		zimg_lut3d bad = base;
		bad.data = nan_data.data();
		FB_CHECK(zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &bad) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	}

	// Non-increasing domain.
	{
		zimg_lut3d bad = base;
		bad.domain_max[1] = bad.domain_min[1];
		FB_CHECK(zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &bad) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	}

	// Non-finite domain.
	{
		zimg_lut3d bad = base;
		bad.domain_min[2] = std::numeric_limits<float>::infinity();
		FB_CHECK(zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &bad) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	}

	// Unspecified colorspace.
	{
		zimg_image_format bad_fmt = fmt;
		bad_fmt.color_primaries = ZIMG_PRIMARIES_UNSPECIFIED;
		FB_CHECK(zimg_filter_graph_build_with_lut(&bad_fmt, &fmt, &params, &base) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	}

	// Greyscale destination.
	{
		zimg_image_format grey_fmt = fmt;
		grey_fmt.color_family = ZIMG_COLOR_GREY;
		grey_fmt.matrix_coefficients = ZIMG_MATRIX_UNSPECIFIED;
		FB_CHECK(zimg_filter_graph_build_with_lut(&fmt, &grey_fmt, &params, &base) == nullptr);
		FB_CHECK(zimg_get_last_error(nullptr, 0) != ZIMG_ERROR_SUCCESS);
	}

	// A valid descriptor still works after the failures above.
	{
		zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &base);
		FB_CHECK(graph != nullptr);
		zimg_filter_graph_free(graph);
	}
}

// Distinct source value per plane and pixel, used by the streaming test.
float stream_value(unsigned c, unsigned x, unsigned y)
{
	return static_cast<float>(0.1 + 0.03 * c + 0.02 * x + 0.01 * y);
}

// The LUT graph must keep the existing row-buffered scheduling, including
// user callbacks. Here the image is streamed through a small ring buffer with
// an unpack callback that fills rows on demand and a pack callback that
// records them.
void test_ownership_streaming()
{
	const unsigned w = 8;
	const unsigned h = 12;

	Table table = make_nonaffine_table(5);
	zimg_lut3d lut = table.descriptor();

	zimg_image_format fmt = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	unsigned in_buffering = 0;
	unsigned out_buffering = 0;
	FB_CHECK(zimg_filter_graph_get_input_buffering(graph, &in_buffering) == 0);
	FB_CHECK(zimg_filter_graph_get_output_buffering(graph, &out_buffering) == 0);
	FB_CHECK(in_buffering >= 1);
	FB_CHECK(out_buffering >= 1);

	// Ring buffers sized to the graph requirement (a power of two).
	unsigned in_mask = zimg_select_buffer_mask(in_buffering);
	unsigned out_mask = zimg_select_buffer_mask(out_buffering);
	unsigned in_rows = in_mask == ZIMG_BUFFER_MAX ? h : in_mask + 1;
	unsigned out_rows = out_mask == ZIMG_BUFFER_MAX ? h : out_mask + 1;

	unsigned stride = (w * sizeof(float) + 63) & ~63U;
	std::vector<unsigned char> in_storage(static_cast<size_t>(stride) * in_rows * 3);
	std::vector<unsigned char> out_storage(static_cast<size_t>(stride) * out_rows * 3);

	struct CallbackState {
		unsigned w;
		unsigned stride;
		unsigned in_rows;
		unsigned out_rows;
		unsigned in_mask;
		unsigned out_mask;
		const std::vector<unsigned char> *in;
		const std::vector<unsigned char> *out;
		std::vector<float> collected[3];
		std::vector<bool> written;
	};

	CallbackState state{};
	state.w = w;
	state.stride = stride;
	state.in_rows = in_rows;
	state.out_rows = out_rows;
	state.in_mask = in_mask;
	state.out_mask = out_mask;
	state.in = &in_storage;
	state.out = &out_storage;
	state.written.assign(h, false);
	for (unsigned c = 0; c < 3; ++c)
		state.collected[c].assign(static_cast<size_t>(w) * h, 0.0f);

	auto unpack = [](void *user, unsigned i, unsigned left, unsigned right) -> int {
		CallbackState *st = static_cast<CallbackState *>(user);
		for (unsigned c = 0; c < 3; ++c) {
			float *line = reinterpret_cast<float *>(const_cast<unsigned char *>(st->in->data()) +
			                                       static_cast<size_t>(c) * st->stride * st->in_rows +
			                                       static_cast<size_t>(i & st->in_mask) * st->stride);
			for (unsigned x = left; x < right; ++x)
				line[x] = stream_value(c, x, i);
		}
		return 0;
	};

	auto pack = [](void *user, unsigned i, unsigned left, unsigned right) -> int {
		CallbackState *st = static_cast<CallbackState *>(user);
		for (unsigned c = 0; c < 3; ++c) {
			const float *line = reinterpret_cast<const float *>(st->out->data() +
			                                                    static_cast<size_t>(c) * st->stride * st->out_rows +
			                                                    static_cast<size_t>(i & st->out_mask) * st->stride);
			for (unsigned x = left; x < right; ++x)
				st->collected[c][static_cast<size_t>(i) * st->w + x] = line[x];
		}
		st->written[i] = true;
		return 0;
	};

	zimg_image_buffer_const src = { ZIMG_API_VERSION };
	zimg_image_buffer dst = { ZIMG_API_VERSION };
	for (unsigned c = 0; c < 3; ++c) {
		src.plane[c].data = in_storage.data() + static_cast<size_t>(c) * stride * in_rows;
		src.plane[c].stride = stride;
		src.plane[c].mask = in_mask;

		dst.plane[c].data = out_storage.data() + static_cast<size_t>(c) * stride * out_rows;
		dst.plane[c].stride = stride;
		dst.plane[c].mask = out_mask;
	}

	size_t tmp_size = 0;
	FB_CHECK(zimg_filter_graph_get_tmp_size(graph, &tmp_size) == 0);
	std::vector<unsigned char> tmp(tmp_size + 64);
	void *tmp_ptr = tmp.empty() ? nullptr : reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(tmp.data()) + 63) & ~static_cast<uintptr_t>(63));

	FB_CHECK(zimg_filter_graph_process(graph, &src, &dst, tmp_ptr, unpack, &state, pack, &state) == 0);

	for (unsigned y = 0; y < h; ++y) {
		FB_CHECK(state.written[y]);

		for (unsigned x = 0; x < w; ++x) {
			float in[3] = { stream_value(0, x, y), stream_value(1, x, y), stream_value(2, x, y) };
			float expected[3];
			reference_lookup(table, in, expected);

			for (unsigned c = 0; c < 3; ++c)
				FB_CHECK(close(state.collected[c][static_cast<size_t>(y) * w + x], expected[c], EPS_LUT));
		}
	}

	zimg_filter_graph_free(graph);
}

// A failing callback aborts processing and reports the existing error code.
void test_ownership_callback_failure()
{
	const unsigned w = 4;
	const unsigned h = 4;

	Table table = make_nonaffine_table(4);

	zimg_lut3d lut = table.descriptor();
	zimg_image_format fmt = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	unsigned stride = (w * sizeof(float) + 63) & ~63U;
	std::vector<unsigned char> storage(static_cast<size_t>(stride) * h * 3);

	zimg_image_buffer_const src = { ZIMG_API_VERSION };
	zimg_image_buffer dst = { ZIMG_API_VERSION };
	for (unsigned c = 0; c < 3; ++c) {
		src.plane[c].data = storage.data() + static_cast<size_t>(c) * stride * h;
		src.plane[c].stride = stride;
		src.plane[c].mask = 0;

		dst.plane[c].data = storage.data() + static_cast<size_t>(c) * stride * h;
		dst.plane[c].stride = stride;
		dst.plane[c].mask = 0;
	}

	auto fail_cb = [](void *, unsigned, unsigned, unsigned) -> int { return 1; };

	size_t tmp_size = 0;
	FB_CHECK(zimg_filter_graph_get_tmp_size(graph, &tmp_size) == 0);
	std::vector<unsigned char> tmp(tmp_size + 64);
	void *tmp_ptr = tmp.empty() ? nullptr : reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(tmp.data()) + 63) & ~static_cast<uintptr_t>(63));

	FB_CHECK(zimg_filter_graph_process(graph, &src, &dst, tmp_ptr, fail_cb, nullptr, nullptr, nullptr) == ZIMG_ERROR_USER_CALLBACK_FAILED);

	zimg_filter_graph_free(graph);
}

// Negative strides are supported for both input and output.
void test_ownership_negative_stride()
{
	const unsigned w = 8;
	const unsigned h = 8;

	Table table = make_nonaffine_table(5);
	zimg_lut3d lut = table.descriptor();

	zimg_image_format fmt = make_fmt(w, h, ZIMG_PIXEL_FLOAT, ZIMG_COLOR_RGB, ZIMG_MATRIX_RGB,
	                                 ZIMG_TRANSFER_LINEAR, ZIMG_PRIMARIES_BT709, ZIMG_RANGE_FULL, ZIMG_ALPHA_NONE);
	zimg_graph_builder_params params = make_params();

	zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&fmt, &fmt, &params, &lut);
	FB_CHECK(graph != nullptr);

	unsigned stride = (w * sizeof(float) + 63) & ~63U;
	std::vector<unsigned char> src_storage(static_cast<size_t>(stride) * h * 3);
	std::vector<unsigned char> dst_storage(static_cast<size_t>(stride) * h * 3);

	zimg_image_buffer_const src = { ZIMG_API_VERSION };
	zimg_image_buffer dst = { ZIMG_API_VERSION };
	for (unsigned c = 0; c < 3; ++c) {
		// Data is stored bottom-up: the last physical row is image row 0.
		src.plane[c].data = src_storage.data() + static_cast<size_t>(c) * stride * h + static_cast<size_t>(h - 1) * stride;
		src.plane[c].stride = -static_cast<ptrdiff_t>(stride);
		src.plane[c].mask = ZIMG_BUFFER_MAX;

		dst.plane[c].data = dst_storage.data() + static_cast<size_t>(c) * stride * h + static_cast<size_t>(h - 1) * stride;
		dst.plane[c].stride = -static_cast<ptrdiff_t>(stride);
		dst.plane[c].mask = ZIMG_BUFFER_MAX;
	}

	for (unsigned y = 0; y < h; ++y) {
		for (unsigned x = 0; x < w; ++x) {
			for (unsigned c = 0; c < 3; ++c) {
				float *row = reinterpret_cast<float *>(src_storage.data() + c * stride * h + (h - 1 - y) * stride);
				row[x] = stream_value(c, x, y);
			}
		}
	}

	size_t tmp_size = 0;
	FB_CHECK(zimg_filter_graph_get_tmp_size(graph, &tmp_size) == 0);
	std::vector<unsigned char> tmp(tmp_size + 64);
	void *tmp_ptr = tmp.empty() ? nullptr : reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(tmp.data()) + 63) & ~static_cast<uintptr_t>(63));

	FB_CHECK(zimg_filter_graph_process(graph, &src, &dst, tmp_ptr, nullptr, nullptr, nullptr, nullptr) == 0);

	for (unsigned y = 0; y < h; ++y) {
		for (unsigned x = 0; x < w; ++x) {
			float in[3] = { stream_value(0, x, y), stream_value(1, x, y), stream_value(2, x, y) };
			float expected[3];
			reference_lookup(table, in, expected);

			for (unsigned c = 0; c < 3; ++c) {
				const float *row = reinterpret_cast<const float *>(dst_storage.data() + c * stride * h + (h - 1 - y) * stride);
				FB_CHECK(close(row[x], expected[c], EPS_LUT));
			}
		}
	}

	zimg_filter_graph_free(graph);
}

} // namespace


FB_TEST("tetrahedra", orderings) { test_tetrahedra_orderings(); }
FB_TEST("tetrahedra", domain) { test_tetrahedra_domain(); }
FB_TEST("tetrahedra", out_of_range) { test_tetrahedra_out_of_range(); }

FB_TEST("pipeline", scale_transfer) { test_pipeline_scale_transfer(); }
FB_TEST("pipeline", yuv_output) { test_pipeline_yuv_output(); }

FB_TEST("alpha", premultiplied) { test_alpha_premultiplied(); }
FB_TEST("alpha", unpremultiply) { test_alpha_unpremultiply(); }

FB_TEST("ownership", snapshot) { test_ownership_snapshot(); }
FB_TEST("ownership", isolation) { test_ownership_isolation(); }
FB_TEST("ownership", identical_formats) { test_ownership_identical_formats(); }
FB_TEST("ownership", bad_descriptor) { test_ownership_bad_descriptor(); }
FB_TEST("ownership", streaming) { test_ownership_streaming(); }
FB_TEST("ownership", callback_failure) { test_ownership_callback_failure(); }
FB_TEST("ownership", negative_stride) { test_ownership_negative_stride(); }
