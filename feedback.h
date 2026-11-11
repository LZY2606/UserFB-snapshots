#include "./common.h"

#ifndef SIGNALSMITH_DSP_FEEDBACK_H
#define SIGNALSMITH_DSP_FEEDBACK_H

#include <algorithm> // for std::fill()
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "./mix.h"

namespace signalsmith {
namespace feedback {
	/**	@defgroup Feedback Feedback structures
		@brief Reverb-style feedback-delay networks

		@{
		@file
	*/

	/** @brief A small four-channel feedback-delay-network (FDN) reverb core.

		Each of the four delay lines has its own integer length (in samples) and
		feedback gain, cross-mixed every sample with the orthogonal 4x4
		Householder matrix from `signalsmith::mix`.  Input and output are four
		interleaved channels, and `process()` supports fully in-place operation
		(`input == output`).

		Only `prepare()` allocates or releases heap memory.  Parameter updates
		happen between `process()` calls; no thread-safety is provided.
	*/
	template<typename Sample>
	class Reverb4 {
		static constexpr int channels = 4;

		bool prepared = false;
		bool frozen = false;
		double sampleRate = 0;
		double t60 = 0;
		std::array<int, channels> delays {{1, 1, 1, 1}};
		std::array<Sample, channels> gains {{Sample(0), Sample(0), Sample(0), Sample(0)}};
		// Power-of-two ring buffers, one per delay line
		std::array<std::vector<Sample>, channels> buffers;
		std::array<unsigned int, channels> masks {{0, 0, 0, 0}};
		std::array<unsigned int, channels> heads {{0, 0, 0, 0}};

		static bool validT60(double t60) {
			return std::isfinite(t60) && t60 > 0;
		}
		/// Per-line feedback gain: -60dB after `t60` seconds in each line
		void updateGains() {
			for (int c = 0; c < channels; ++c) {
				gains[c] = Sample(std::pow(10.0, -3.0*delays[c]/(sampleRate*t60)));
			}
		}
	public:
		/** Allocates the delay lines and clears all history.  `delays` are
			positive integer lengths in samples (they do not need to be coprime).
			Returns `false` - leaving the previous state untouched - for
			non-finite/non-positive `sampleRate` or `t60`, or non-positive delays. */
		bool prepare(double sampleRate, std::array<int, channels> delays, double t60) {
			if (!(std::isfinite(sampleRate) && sampleRate > 0)) return false;
			if (!validT60(t60)) return false;
			for (int c = 0; c < channels; ++c) {
				if (!(delays[c] > 0)) return false;
			}

			this->sampleRate = sampleRate;
			this->delays = delays;
			this->t60 = t60;
			for (int c = 0; c < channels; ++c) {
				std::size_t capacity = 1;
				while (capacity < std::size_t(delays[c])) capacity *= 2;
				buffers[c].assign(capacity, Sample(0));
				masks[c] = (unsigned int)(capacity - 1);
				heads[c] = 0;
			}
			updateGains();
			frozen = false;
			prepared = true;
			return true;
		}

		/** Updates the decay time (seconds to -60dB).  During a freeze this only
			changes the decay used once the freeze is released.  Returns `false`
			(keeping the previous value) for non-finite/non-positive `t60`. */
		bool setDecay(double t60) {
			if (!prepared || !validT60(t60)) return false;
			this->t60 = t60;
			updateGains();
			return true;
		}

		/** While frozen, the delay lines recirculate through the (orthogonal)
			Householder matrix with unity gain and new input is ignored, so the
			stored energy is preserved.  The existing line contents and
			read/write positions are kept, and releasing the freeze resumes the
			previous decay behaviour. */
		void setFreeze(bool freeze) {
			frozen = freeze;
		}

		/** Clears the delay-line history and disables freeze, keeping the
			`prepare()` arguments and the most recent valid `t60`. */
		void reset() {
			for (int c = 0; c < channels; ++c) {
				std::fill(buffers[c].begin(), buffers[c].end(), Sample(0));
				heads[c] = 0;
			}
			frozen = false;
		}

		/** Processes interleaved four-channel audio.  Each frame reads all four
			delay lines (this is the output), mixes them with the Householder
			matrix, then writes `input[c] + gain[c]*mixed[c]` back into every
			line (or just `mixed[c]` while frozen) before advancing them all.

			`input` and `output` may be the same pointer.  `frames == 0` is a
			no-op (pointers may be null).  Before `prepare()`, the output is
			filled with zeros. */
		void process(const Sample *input, Sample *output, std::size_t frames) {
			if (frames == 0) return;
			if (!prepared) {
				std::fill(output, output + frames*channels, Sample(0));
				return;
			}
			for (std::size_t i = 0; i < frames; ++i) {
				// Read the input first, so `input == output` is safe
				std::array<Sample, channels> d, in;
				for (int c = 0; c < channels; ++c) in[c] = input[i*channels + c];
				// Read all four lines before writing any of them
				for (int c = 0; c < channels; ++c) {
					d[c] = buffers[c][(heads[c] - (unsigned int)delays[c]) & masks[c]];
				}
				for (int c = 0; c < channels; ++c) output[i*channels + c] = d[c];
				signalsmith::mix::Householder<Sample, channels>::inPlace(d);
				for (int c = 0; c < channels; ++c) {
					buffers[c][heads[c]] = frozen ? d[c] : in[c] + gains[c]*d[c];
					heads[c] = (heads[c] + 1) & masks[c];
				}
			}
		}

		/** Sum of squared values across the active slots of all four delay
			lines (capacity padding is not counted).  Read-only: calling this
			does not affect processing. */
		double storedEnergy() const {
			if (!prepared) return 0;
			double energy = 0;
			for (int c = 0; c < channels; ++c) {
				unsigned int index = heads[c];
				for (int k = 0; k < delays[c]; ++k) {
					index = (index - 1) & masks[c];
					double v = buffers[c][index];
					energy += v*v;
				}
			}
			return energy;
		}
	};

/** @} */
}} // signalsmith::feedback::
#endif // include guard
