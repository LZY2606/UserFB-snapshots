#pragma once

#ifndef ZIMG_LUT3D_LUT3D_H_
#define ZIMG_LUT3D_LUT3D_H_

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace graphengine {
class Filter;
}

namespace zimg::lut3d {

// Immutable snapshot of a validated three-dimensional lookup table.
struct Lut3D {
	unsigned size = 0;
	std::vector<float> table;
	std::array<double, 3> domain_min{};
	std::array<double, 3> domain_max{};
};

// Non-owning description used while validating and copying a public zimg_lut3d.
struct Lut3DDescriptor {
	unsigned size;
	const float *data;
	size_t data_count;
	const float *domain_min;
	const float *domain_max;
};

// Validates desc and returns an independent, immutable snapshot.
std::shared_ptr<Lut3D> create_lut3d(const Lut3DDescriptor &desc);

// Tetrahedral interpolation. Exposed for focused numerical tests.
void interpolate_lut3d(const Lut3D &lut, const float rgb_in[3], float rgb_out[3]) noexcept;

// Creates the row-buffered three-plane filter that evaluates a LUT snapshot.
std::unique_ptr<graphengine::Filter> create_lut3d_filter(unsigned width, unsigned height, std::shared_ptr<const Lut3D> lut);

} // namespace zimg::lut3d

#endif // ZIMG_LUT3D_LUT3D_H_
