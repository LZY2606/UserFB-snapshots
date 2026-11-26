#pragma once

#ifndef ZIMG_GRAPH_LUT3D_H_
#define ZIMG_GRAPH_LUT3D_H_

#include <memory>
#include <vector>
#include "graph/filter_base.h"

struct zimg_lut3d;

namespace zimg::graph {

/**
 * Immutable, owned snapshot of a ::zimg_lut3d descriptor.
 *
 * The table is stored as 3 * size^3 floats with the R grid coordinate varying
 * fastest, followed by G, then B. Instances are shared between filter copies
 * and owned by the constructed graph.
 */
class Lut3D {
	unsigned m_size;
	std::vector<float> m_data;
	double m_domain_min[3];
	double m_domain_max[3];
	double m_scale[3];
public:
	Lut3D(unsigned size, std::vector<float> data, const float domain_min[3], const float domain_max[3]);

	unsigned size() const noexcept { return m_size; }

	const float *data() const noexcept { return m_data.data(); }

	const double *domain_min() const noexcept { return m_domain_min; }

	const double *domain_max() const noexcept { return m_domain_max; }

	/**
	 * Evaluate the LUT at an RGB coordinate using tetrahedral interpolation.
	 *
	 * Coordinates outside the domain are clamped to the boundary. The output
	 * is not clamped to [0, 1]. This function performs no allocation.
	 */
	void eval(const float rgb[3], float out[3]) const noexcept;

	/**
	 * Validate a public LUT descriptor and copy its table into an owned
	 * snapshot. Throws a zimg::error exception on invalid descriptors.
	 */
	static std::shared_ptr<Lut3D> create_snapshot(const zimg_lut3d *desc);
};


/**
 * Three-plane point filter applying tetrahedral 3D LUT interpolation.
 *
 * Inputs and outputs are FLOAT RGB planes. The filter is in-place capable and
 * performs no memory allocation during processing.
 */
class Lut3DFilter : public PointFilter {
	std::shared_ptr<const Lut3D> m_lut;
public:
	Lut3DFilter(unsigned width, unsigned height, std::shared_ptr<const Lut3D> lut);

	void process(const graphengine::BufferDescriptor in[], const graphengine::BufferDescriptor out[],
	             unsigned i, unsigned left, unsigned right, void *, void *) const noexcept override;
};


/**
 * Replaces a color sample with zero when its associated alpha is zero.
 *
 * Unlike PremultiplyFilter, the color value is otherwise preserved, which is
 * required for straight-alpha output while still preventing a non-zero LUT
 * mapping of black from leaking through fully transparent pixels.
 */
class AlphaZeroFilter : public PointFilter {
public:
	AlphaZeroFilter(unsigned width, unsigned height);

	void process(const graphengine::BufferDescriptor in[2], const graphengine::BufferDescriptor *out,
	             unsigned i, unsigned left, unsigned right, void *, void *) const noexcept override;
};

} // namespace zimg::graph

#endif // ZIMG_GRAPH_LUT3D_H_
