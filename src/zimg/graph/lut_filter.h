#pragma once

#ifndef ZIMG_GRAPH_LUT_FILTER_H_
#define ZIMG_GRAPH_LUT_FILTER_H_

#include <cstddef>
#include <vector>
#include "filter_base.h"

namespace zimg::graph {

/**
 * Validated snapshot of a 3D color lookup table.
 *
 * The table holds @p size ^ 3 RGB triplets in R-fastest, then G, then B
 * order. Values are linear RGB relative to the white point of the target
 * primaries.
 */
struct Lut3D {
	unsigned size;
	std::vector<float> data;
	float domain_min[3];
	float domain_max[3];
};

/**
 * Validate a user-supplied LUT description and take a private snapshot.
 *
 * @param size number of grid points per axis
 * @param data table data, @p size ^ 3 RGB triplets
 * @param data_count number of floats in @p data
 * @param domain_min minimum grid coordinate per channel
 * @param domain_max maximum grid coordinate per channel
 * @return validated snapshot
 * @throw error::IllegalArgument if the description is invalid
 */
Lut3D parse_lut3d(unsigned size, const float *data, size_t data_count,
                  const float domain_min[3], const float domain_max[3]);

/**
 * Apply a 3D LUT using tetrahedral interpolation.
 *
 * Expects three input planes of linear RGB and produces three output planes.
 * Values outside the domain are clamped to the boundary. Output values are
 * not clamped to [0,1].
 */
class Lut3dFilter : public PointFilter {
	unsigned m_size;
	std::vector<float> m_data;
	double m_domain_min[3];
	double m_domain_range[3];

	void lookup(const float in[3], float out[3]) const noexcept;
public:
	Lut3dFilter(unsigned width, unsigned height, Lut3D lut);

	void process(const graphengine::BufferDescriptor in[3], const graphengine::BufferDescriptor out[3],
	             unsigned i, unsigned left, unsigned right, void *, void *) const noexcept override;
};

/**
 * Zero an unpremultiplied color plane where the associated alpha is zero.
 */
class AlphaZeroFilter : public PointFilter {
public:
	AlphaZeroFilter(unsigned width, unsigned height);

	void process(const graphengine::BufferDescriptor in[2], const graphengine::BufferDescriptor *out,
	             unsigned i, unsigned left, unsigned right, void *, void *) const noexcept override;
};

} // namespace zimg::graph

#endif // ZIMG_GRAPH_LUT_FILTER_H_
