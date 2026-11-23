#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include "common/checked_int.h"
#include "common/except.h"
#include "common/pixel.h"
#include "common/zassert.h"
#include "lut_filter.h"

namespace zimg::graph {

namespace {

constexpr unsigned LUT_SIZE_MIN = 2;
constexpr unsigned LUT_SIZE_MAX = 65;
constexpr unsigned LUT_CHANNELS = 3;

} // namespace


Lut3D parse_lut3d(unsigned size, const float *data, size_t data_count,
                  const float domain_min[3], const float domain_max[3])
{
	using zimg::checked_size_t;

	if (!data)
		error::throw_<error::IllegalArgument>("LUT data must not be null");
	if (!domain_min || !domain_max)
		error::throw_<error::IllegalArgument>("LUT domain must not be null");

	// Compute the expected element count with overflow checking before anything
	// else, so that an absurd size is reported as an arithmetic overflow.
	checked_size_t expected;
	try {
		expected = checked_size_t{ static_cast<size_t>(size) };
		expected *= size;
		expected *= size;
		expected *= LUT_CHANNELS;
	} catch (const std::overflow_error &) {
		error::throw_<error::IllegalArgument>("LUT data count overflows");
	}

	if (size < LUT_SIZE_MIN || size > LUT_SIZE_MAX)
		error::throw_<error::IllegalArgument>("LUT size must be between 2 and 65");
	if (data_count != expected.get())
		error::throw_<error::IllegalArgument>("LUT data count does not match size");

	Lut3D lut{};
	lut.size = size;

	for (unsigned c = 0; c < LUT_CHANNELS; ++c) {
		if (!std::isfinite(domain_min[c]) || !std::isfinite(domain_max[c]))
			error::throw_<error::IllegalArgument>("LUT domain must be finite");
		if (!(domain_min[c] < domain_max[c]))
			error::throw_<error::IllegalArgument>("LUT domain must be strictly increasing");

		lut.domain_min[c] = domain_min[c];
		lut.domain_max[c] = domain_max[c];
	}

	try {
		lut.data.assign(data, data + data_count);
	} catch (const std::bad_alloc &) {
		error::throw_<error::OutOfMemory>();
	}

	for (float x : lut.data) {
		if (!std::isfinite(x))
			error::throw_<error::IllegalArgument>("LUT table values must be finite");
	}

	return lut;
}


Lut3dFilter::Lut3dFilter(unsigned width, unsigned height, Lut3D lut) :
	PointFilter(width, height, PixelType::FLOAT),
	m_size{ lut.size },
	m_data{ std::move(lut.data) }
{
	zassert_d(width <= pixel_max_width(PixelType::FLOAT), "overflow");

	m_desc.num_deps = 3;
	m_desc.num_planes = 3;
	m_desc.flags.in_place = 1;

	for (unsigned c = 0; c < LUT_CHANNELS; ++c) {
		m_domain_min[c] = lut.domain_min[c];
		m_domain_range[c] = static_cast<double>(lut.domain_max[c]) - lut.domain_min[c];
	}
}

void Lut3dFilter::lookup(const float in[3], float out[3]) const noexcept
{
	const unsigned max_index = m_size - 2;
	unsigned index[3];
	double frac[3];

	for (unsigned c = 0; c < LUT_CHANNELS; ++c) {
		double x = (static_cast<double>(in[c]) - m_domain_min[c]) / m_domain_range[c];

		// Clamp values outside the domain to the boundary.
		if (!(x > 0.0))
			x = 0.0;
		if (x > 1.0)
			x = 1.0;

		double g = x * static_cast<double>(m_size - 1);
		unsigned idx = static_cast<unsigned>(g);
		double f = g - idx;

		// Top domain value uses the right end of the last cell.
		if (idx > max_index) {
			idx = max_index;
			f = 1.0;
		}

		index[c] = idx;
		frac[c] = f;
	}

	const unsigned i = index[0];
	const unsigned j = index[1];
	const unsigned k = index[2];
	const double fx = frac[0];
	const double fy = frac[1];
	const double fz = frac[2];

	const size_t stride_r = LUT_CHANNELS;
	const size_t stride_g = static_cast<size_t>(m_size) * LUT_CHANNELS;
	const size_t stride_b = static_cast<size_t>(m_size) * m_size * LUT_CHANNELS;
	const float *base = m_data.data() + static_cast<size_t>(k) * stride_b + static_cast<size_t>(j) * stride_g + static_cast<size_t>(i) * stride_r;

	const float *c000 = base;
	const float *c001 = base + stride_b;
	const float *c010 = base + stride_g;
	const float *c011 = base + stride_g + stride_b;
	const float *c100 = base + stride_r;
	const float *c101 = base + stride_r + stride_b;
	const float *c110 = base + stride_r + stride_g;
	const float *c111 = base + stride_r + stride_g + stride_b;

	const auto get = [](const float *p, unsigned c) { return static_cast<double>(p[c]); };

	for (unsigned c = 0; c < LUT_CHANNELS; ++c) {
		double result;

		// Tetrahedral interpolation: select the tetrahedron containing the
		// fractional coordinate by ordering of the three fractional parts.
		if (fx > fy) {
			if (fy > fz) {
				result = get(c000, c) * (1.0 - fx) + get(c100, c) * (fx - fy) +
				         get(c110, c) * (fy - fz) + get(c111, c) * fz;
			} else if (fx > fz) {
				result = get(c000, c) * (1.0 - fx) + get(c100, c) * (fx - fz) +
				         get(c101, c) * (fz - fy) + get(c111, c) * fy;
			} else {
				result = get(c000, c) * (1.0 - fz) + get(c001, c) * (fz - fx) +
				         get(c101, c) * (fx - fy) + get(c111, c) * fy;
			}
		} else {
			if (fz > fy) {
				result = get(c000, c) * (1.0 - fz) + get(c001, c) * (fz - fy) +
				         get(c011, c) * (fy - fx) + get(c111, c) * fx;
			} else if (fz > fx) {
				result = get(c000, c) * (1.0 - fy) + get(c010, c) * (fy - fz) +
				         get(c011, c) * (fz - fx) + get(c111, c) * fx;
			} else {
				result = get(c000, c) * (1.0 - fy) + get(c010, c) * (fy - fx) +
				         get(c110, c) * (fx - fz) + get(c111, c) * fz;
			}
		}

		out[c] = static_cast<float>(result);
	}
}

void Lut3dFilter::process(const graphengine::BufferDescriptor in[3], const graphengine::BufferDescriptor out[3],
                          unsigned i, unsigned left, unsigned right, void *, void *) const noexcept
{
	const float *src_p[3];
	float *dst_p[3];

	for (unsigned p = 0; p < 3; ++p) {
		src_p[p] = in[p].get_line<float>(i);
		dst_p[p] = out[p].get_line<float>(i);
	}

	for (unsigned j = left; j < right; ++j) {
		const float in_px[3] = { src_p[0][j], src_p[1][j], src_p[2][j] };
		float out_px[3];

		lookup(in_px, out_px);

		dst_p[0][j] = out_px[0];
		dst_p[1][j] = out_px[1];
		dst_p[2][j] = out_px[2];
	}
}


AlphaZeroFilter::AlphaZeroFilter(unsigned width, unsigned height) :
	PointFilter(width, height, PixelType::FLOAT)
{
	m_desc.num_deps = 2;
	m_desc.num_planes = 1;
	m_desc.flags.in_place = 1;
}

void AlphaZeroFilter::process(const graphengine::BufferDescriptor in[2], const graphengine::BufferDescriptor *out,
                              unsigned i, unsigned left, unsigned right, void *, void *) const noexcept
{
	const float *src_p = in[0].get_line<float>(i);
	const float *alpha = in[1].get_line<float>(i);
	float *dst_p = out->get_line<float>(i);

	for (unsigned j = left; j < right; ++j) {
		dst_p[j] = alpha[j] == 0.0f ? 0.0f : src_p[j];
	}
}

} // namespace zimg::graph
