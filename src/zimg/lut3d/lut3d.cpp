#include "lut3d/lut3d.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

#include "common/except.h"
#include "common/pixel.h"
#include "graph/filter_base.h"
#include "graphengine/filter.h"
#include "graphengine/types.h"

namespace zimg::lut3d {

namespace {

size_t checked_table_count(unsigned size)
{
	size_t n = static_cast<size_t>(size);
	size_t n2{};
	size_t n3{};
	constexpr size_t max_size_t = std::numeric_limits<size_t>::max();

	if (n != 0 && n > max_size_t / n)
		error::throw_<error::IllegalArgument>("3D LUT size product overflow");
	n2 = n * n;

	if (n2 != 0 && n2 > max_size_t / n)
		error::throw_<error::IllegalArgument>("3D LUT size product overflow");
	n3 = n2 * n;

	if (n3 > max_size_t / 3U)
		error::throw_<error::IllegalArgument>("3D LUT data count overflow");
	return n3 * 3U;
}

const float *grid_vertex(const Lut3D &lut, unsigned r, unsigned g, unsigned b) noexcept
{
	size_t index = (static_cast<size_t>(b) * lut.size + g) * lut.size + r;
	return lut.table.data() + index * 3;
}

class Lut3DFilter final : public graph::PointFilter {
	std::shared_ptr<const Lut3D> m_lut;
public:
	Lut3DFilter(unsigned width, unsigned height, std::shared_ptr<const Lut3D> lut) :
		PointFilter(width, height, PixelType::FLOAT),
		m_lut{ std::move(lut) }
	{
		m_desc.num_deps = 3;
		m_desc.num_planes = 3;
		m_desc.flags.in_place = 1;
	}

	void process(const graphengine::BufferDescriptor in[3], const graphengine::BufferDescriptor out[3],
	             unsigned i, unsigned left, unsigned right, void *, void *) const noexcept override
	{
		const float *r_in = in[0].get_line<float>(i);
		const float *g_in = in[1].get_line<float>(i);
		const float *b_in = in[2].get_line<float>(i);
		float *r_out = out[0].get_line<float>(i);
		float *g_out = out[1].get_line<float>(i);
		float *b_out = out[2].get_line<float>(i);

		for (unsigned j = left; j < right; ++j) {
			float rgb_in[3] = { r_in[j], g_in[j], b_in[j] };
			float rgb_out[3];

			interpolate_lut3d(*m_lut, rgb_in, rgb_out);

			r_out[j] = rgb_out[0];
			g_out[j] = rgb_out[1];
			b_out[j] = rgb_out[2];
		}
	}
};

} // namespace

std::shared_ptr<Lut3D> create_lut3d(const Lut3DDescriptor &desc)
{
	if (!desc.data || !desc.domain_min || !desc.domain_max)
		error::throw_<error::IllegalArgument>("incomplete 3D LUT descriptor");

	size_t expected_count = checked_table_count(desc.size);

	if (desc.size < 2 || desc.size > 65)
		error::throw_<error::IllegalArgument>("3D LUT size must be between 2 and 65");

	if (desc.data_count != expected_count)
		error::throw_<error::IllegalArgument>("3D LUT data count does not match size");

	auto lut = std::make_shared<Lut3D>();
	lut->size = desc.size;
	lut->table.resize(expected_count);

	for (unsigned c = 0; c < 3; ++c) {
		float lo = desc.domain_min[c];
		float hi = desc.domain_max[c];

		if (!std::isfinite(lo) || !std::isfinite(hi) || !(lo < hi))
			error::throw_<error::IllegalArgument>("3D LUT domain must be finite and strictly increasing");

		lut->domain_min[c] = lo;
		lut->domain_max[c] = hi;
	}

	for (size_t i = 0; i < expected_count; ++i) {
		float value = desc.data[i];
		if (!std::isfinite(value))
			error::throw_<error::IllegalArgument>("3D LUT table values must be finite");
		lut->table[i] = value;
	}

	return lut;
}

void interpolate_lut3d(const Lut3D &lut, const float rgb_in[3], float rgb_out[3]) noexcept
{
	unsigned base[3];
	unsigned next[3];
	double frac[3];

	for (unsigned c = 0; c < 3; ++c) {
		double normalized = (static_cast<double>(rgb_in[c]) - lut.domain_min[c]) /
			(lut.domain_max[c] - lut.domain_min[c]);
		// Explicit comparisons also map NaN to the lower boundary before casting.
		if (!(normalized >= 0.0))
			normalized = 0.0;
		if (!(normalized <= 1.0))
			normalized = 1.0;

		double position = normalized * static_cast<double>(lut.size - 1U);
		if (!(position >= 0.0))
			position = 0.0;
		if (!(position <= static_cast<double>(lut.size - 1U)))
			position = static_cast<double>(lut.size - 1U);

		unsigned index = static_cast<unsigned>(std::floor(position));
		index = std::min(index, lut.size - 2U);

		base[c] = index;
		next[c] = index + 1U;
		frac[c] = std::clamp(position - static_cast<double>(index), 0.0, 1.0);
	}

	const float *c000 = grid_vertex(lut, base[0], base[1], base[2]);
	const float *c100 = grid_vertex(lut, next[0], base[1], base[2]);
	const float *c010 = grid_vertex(lut, base[0], next[1], base[2]);
	const float *c001 = grid_vertex(lut, base[0], base[1], next[2]);
	const float *c110 = grid_vertex(lut, next[0], next[1], base[2]);
	const float *c101 = grid_vertex(lut, next[0], base[1], next[2]);
	const float *c011 = grid_vertex(lut, base[0], next[1], next[2]);
	const float *c111 = grid_vertex(lut, next[0], next[1], next[2]);

	double fr = frac[0];
	double fg = frac[1];
	double fb = frac[2];
	double w[4];
	const float *v[4];

	// The six branches correspond to the six strict orderings of (R, G, B)
	// fractional coordinates. Equality may use any adjacent tetrahedron.
	if (fr >= fg && fg >= fb) {
		w[0] = 1.0 - fr; w[1] = fr - fg; w[2] = fg - fb; w[3] = fb;
		v[0] = c000; v[1] = c100; v[2] = c110; v[3] = c111;
	} else if (fr >= fb && fb >= fg) {
		w[0] = 1.0 - fr; w[1] = fr - fb; w[2] = fb - fg; w[3] = fg;
		v[0] = c000; v[1] = c100; v[2] = c101; v[3] = c111;
	} else if (fg >= fr && fr >= fb) {
		w[0] = 1.0 - fg; w[1] = fg - fr; w[2] = fr - fb; w[3] = fb;
		v[0] = c000; v[1] = c010; v[2] = c110; v[3] = c111;
	} else if (fg >= fb && fb >= fr) {
		w[0] = 1.0 - fg; w[1] = fg - fb; w[2] = fb - fr; w[3] = fr;
		v[0] = c000; v[1] = c010; v[2] = c011; v[3] = c111;
	} else if (fb >= fr && fr >= fg) {
		w[0] = 1.0 - fb; w[1] = fb - fr; w[2] = fr - fg; w[3] = fg;
		v[0] = c000; v[1] = c001; v[2] = c101; v[3] = c111;
	} else {
		w[0] = 1.0 - fb; w[1] = fb - fg; w[2] = fg - fr; w[3] = fr;
		v[0] = c000; v[1] = c001; v[2] = c011; v[3] = c111;
	}

	for (unsigned channel = 0; channel < 3; ++channel) {
		double result = 0.0;
		for (unsigned k = 0; k < 4; ++k) {
			result += w[k] * static_cast<double>(v[k][channel]);
		}
		rgb_out[channel] = static_cast<float>(result);
	}
}

std::unique_ptr<graphengine::Filter> create_lut3d_filter(unsigned width, unsigned height, std::shared_ptr<const Lut3D> lut)
{
	return std::make_unique<Lut3DFilter>(width, height, std::move(lut));
}

} // namespace zimg::lut3d
