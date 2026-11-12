// bc5_normal_optimizer.h
//
// Sample-position driven BC5_UNORM optimizer for tangent-space normal maps.
//
// The legacy single-block encoders (rgbcx::encode_bc5_hq) minimize XY squared
// error per 4x4 block. A GPU never samples a single block: it first bilinearly
// filters the reconstructed R and G channels (across block borders and mip
// levels), reconstructs Z, and only then renormalizes the normal. This
// optimizer minimizes the angular error observed at the caller's actual
// filtered sample positions, jointly across a whole mip chain.
#pragma once

#include <cstdint>
#include <vector>

#include "utils.h"

namespace bc5_normal
{
	// One GPU sample location.
	// u/v are normalized texture coordinates (texel-center convention),
	// lod selects (and fractionally blends between) mip levels, and weight
	// multiplies this sample's contribution to the total loss.
	struct Sample
	{
		double u;
		double v;
		double lod;
		double weight;

		Sample() :
			u(0.0), v(0.0), lod(0.0), weight(1.0)
		{
		}

		Sample(double u_, double v_, double lod_ = 0.0, double weight_ = 1.0) :
			u(u_), v(v_), lod(lod_), weight(weight_)
		{
		}
	};

	struct Options
	{
		// Wrap UVs at the texture seams instead of clamping to the edges.
		bool repeat;
		// Maximum number of coordinate sweeps (hard clamp 1..64).
		unsigned max_sweeps;

		Options() :
			repeat(false),
			max_sweeps(4)
		{
		}
	};

	struct Result
	{
		// One entry per input level, row-major standard BC5_UNORM block bytes.
		// Edge blocks still occupy a full 16 bytes.
		std::vector<std::vector<uint8_t> > blocks;

		double initial_error;
		double final_error;

		uint64_t loss_evaluations;
		uint64_t accepted_moves;

		unsigned sweeps;
		bool converged;

		Result() :
			initial_error(0.0),
			final_error(0.0),
			loss_evaluations(0),
			accepted_moves(0),
			sweeps(0),
			converged(false)
		{
		}
	};

	// Optimizes all levels jointly against the supplied samples.
	// The levels are caller-generated RGBA8 mip images; only R/G are read and
	// the inputs are never modified. Returns false (without touching result)
	// on any invalid input or non-finite accumulated cost.
	bool optimize(const std::vector<utils::image_u8>& levels,
		const std::vector<Sample>& samples,
		const Options& options,
		Result& result);

} // namespace bc5_normal
