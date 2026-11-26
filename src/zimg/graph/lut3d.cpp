#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include "common/except.h"
#include "common/pixel.h"
#include "api/zimg.h"
#include "lut3d.h"

namespace zimg::graph {

Lut3D::Lut3D(unsigned size, std::vector<float> data, const float domain_min[3], const float domain_max[3]) :
	m_size{ size },
	m_data{ std::move(data) },
	m_domain_min{},
	m_domain_max{},
	m_scale{}
{
	double cells = static_cast<double>(size) - 1.0;

	for (unsigned c = 0; c < 3; ++c) {
		m_domain_min[c] = static_cast<double>(domain_min[c]);
		m_domain_max[c] = static_cast<double>(domain_max[c]);
		m_scale[c] = cells / (m_domain_max[c] - m_domain_min[c]);
	}
}

void Lut3D::eval(const float rgb[3], float out[3]) const noexcept
{
	const unsigned n = m_size;
	const unsigned last = n - 1;

	unsigned base[3];
	double frac[3];

	for (unsigned c = 0; c < 3; ++c) {
		double coord = (static_cast<double>(rgb[c]) - m_domain_min[c]) * m_scale[c];

		// Clamp out-of-domain coordinates to the boundary grid planes.
		if (!std::isfinite(coord))
			coord = 0.0;
		coord = coord < 0.0 ? 0.0 : coord > static_cast<double>(last) ? static_cast<double>(last) : coord;

		unsigned k = static_cast<unsigned>(std::floor(coord));
		if (k >= last)
			k = last - 1; // Top domain endpoint: right edge of the last cell.

		base[c] = k;
		frac[c] = coord - static_cast<double>(k);
	}

	const unsigned r0 = base[0];
	const unsigned g0 = base[1];
	const unsigned b0 = base[2];
	const unsigned r1 = r0 + 1;
	const unsigned g1 = g0 + 1;
	const unsigned b1 = b0 + 1;

	// R varies fastest, then G, then B.
	auto vertex = [&](unsigned r, unsigned g, unsigned b) -> const float *
	{
		size_t offset = ((static_cast<size_t>(b) * n + g) * n + r) * 3;
		return m_data.data() + offset;
	};

	const float *c000 = vertex(r0, g0, b0);
	const float *c100 = vertex(r1, g0, b0);
	const float *c010 = vertex(r0, g1, b0);
	const float *c110 = vertex(r1, g1, b0);
	const float *c001 = vertex(r0, g0, b1);
	const float *c101 = vertex(r1, g0, b1);
	const float *c011 = vertex(r0, g1, b1);
	const float *c111 = vertex(r1, g1, b1);

	double fr = frac[0];
	double fg = frac[1];
	double fb = frac[2];

	// Four tetrahedron vertices and their barycentric weights, selected by the
	// ordering of the three fractional grid coordinates.
	const float *v[4];
	double w[4];

	if (fr >= fg) {
		if (fg >= fb) {
			// R >= G >= B: 000, 100, 110, 111
			v[0] = c000; v[1] = c100; v[2] = c110; v[3] = c111;
			w[0] = 1.0 - fr; w[1] = fr - fg; w[2] = fg - fb; w[3] = fb;
		} else if (fr >= fb) {
			// R >= B > G: 000, 100, 101, 111
			v[0] = c000; v[1] = c100; v[2] = c101; v[3] = c111;
			w[0] = 1.0 - fr; w[1] = fr - fb; w[2] = fb - fg; w[3] = fg;
		} else {
			// B > R >= G: 000, 001, 101, 111
			v[0] = c000; v[1] = c001; v[2] = c101; v[3] = c111;
			w[0] = 1.0 - fb; w[1] = fb - fr; w[2] = fr - fg; w[3] = fg;
		}
	} else {
		if (fr >= fb) {
			// G > R >= B: 000, 010, 110, 111
			v[0] = c000; v[1] = c010; v[2] = c110; v[3] = c111;
			w[0] = 1.0 - fg; w[1] = fg - fr; w[2] = fr - fb; w[3] = fb;
		} else if (fg >= fb) {
			// G >= B > R: 000, 010, 011, 111
			v[0] = c000; v[1] = c010; v[2] = c011; v[3] = c111;
			w[0] = 1.0 - fg; w[1] = fg - fb; w[2] = fb - fr; w[3] = fr;
		} else {
			// B > G > R: 000, 001, 011, 111
			v[0] = c000; v[1] = c001; v[2] = c011; v[3] = c111;
			w[0] = 1.0 - fb; w[1] = fb - fg; w[2] = fg - fr; w[3] = fr;
		}
	}

	for (unsigned c = 0; c < 3; ++c) {
		double value = w[0] * static_cast<double>(v[0][c]) +
		               w[1] * static_cast<double>(v[1][c]) +
		               w[2] * static_cast<double>(v[2][c]) +
		               w[3] * static_cast<double>(v[3][c]);
		out[c] = static_cast<float>(value);
	}
}

std::shared_ptr<Lut3D> Lut3D::create_snapshot(const zimg_lut3d *desc)
{
	if (!desc)
		error::throw_<error::IllegalArgument>("null 3D LUT descriptor");

	unsigned size = desc->size;

	// Compute 3 * size^3 with explicit overflow checking.
	size_t count = 1;
	bool overflow = false;

	auto multiply = [&](size_t factor)
	{
		if (overflow)
			return;
		if (factor != 0 && count > std::numeric_limits<size_t>::max() / factor)
			overflow = true;
		else
			count *= factor;
	};

	multiply(size);
	multiply(size);
	multiply(size);
	multiply(3);

	if (overflow)
		error::throw_<error::IllegalArgument>("3D LUT table size overflow");
	if (size < 2 || size > 65)
		error::throw_<error::IllegalArgument>("3D LUT size must be between 2 and 65");
	if (!desc->data)
		error::throw_<error::IllegalArgument>("null 3D LUT data pointer");
	if (desc->data_count != count)
		error::throw_<error::IllegalArgument>("3D LUT data count does not match size");

	for (unsigned c = 0; c < 3; ++c) {
		float lo = desc->domain_min[c];
		float hi = desc->domain_max[c];

		if (!std::isfinite(lo) || !std::isfinite(hi))
			error::throw_<error::IllegalArgument>("3D LUT domain endpoints must be finite");
		if (!(lo < hi))
			error::throw_<error::IllegalArgument>("3D LUT domain must be strictly increasing");
	}

	for (size_t i = 0; i < count; ++i) {
		if (!std::isfinite(desc->data[i]))
			error::throw_<error::IllegalArgument>("3D LUT table values must be finite");
	}

	try {
		std::vector<float> copy(count);
		for (size_t i = 0; i < count; ++i) {
			copy[i] = desc->data[i];
		}
		return std::make_shared<Lut3D>(size, std::move(copy), desc->domain_min, desc->domain_max);
	} catch (const std::bad_alloc &) {
		error::throw_<error::OutOfMemory>();
	}
}


Lut3DFilter::Lut3DFilter(unsigned width, unsigned height, std::shared_ptr<const Lut3D> lut) :
	PointFilter(width, height, PixelType::FLOAT),
	m_lut{ std::move(lut) }
{
	m_desc.num_deps = 3;
	m_desc.num_planes = 3;
	m_desc.flags.in_place = 1;
}

void Lut3DFilter::process(const graphengine::BufferDescriptor in[], const graphengine::BufferDescriptor out[],
                          unsigned i, unsigned left, unsigned right, void *, void *) const noexcept
{
	const float *r_in = in[0].get_line<float>(i);
	const float *g_in = in[1].get_line<float>(i);
	const float *b_in = in[2].get_line<float>(i);
	float *r_out = out[0].get_line<float>(i);
	float *g_out = out[1].get_line<float>(i);
	float *b_out = out[2].get_line<float>(i);

	for (unsigned j = left; j < right; ++j) {
		float rgb[3] = { r_in[j], g_in[j], b_in[j] };
		float mapped[3];

		m_lut->eval(rgb, mapped);

		// Read all inputs for this sample before writing outputs, which makes
		// in-place operation safe even when output planes alias input planes.
		r_out[j] = mapped[0];
		g_out[j] = mapped[1];
		b_out[j] = mapped[2];
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
	const float *src = in[0].get_line<float>(i);
	const float *alpha = in[1].get_line<float>(i);
	float *dst = out->get_line<float>(i);

	for (unsigned j = left; j < right; ++j) {
		float a = std::clamp(alpha[j], 0.0f, 1.0f);
		dst[j] = a == 0.0f ? 0.0f : src[j];
	}
}

} // namespace zimg::graph
