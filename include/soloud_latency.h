/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not
   be misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#ifndef SOLOUD_LATENCY_H
#define SOLOUD_LATENCY_H

// Optional filter latency compensation.
//
// Usage:
//   1. Derive filter instances from SoLoud::FilterInstance and override
//      getLatencyFrames() to report the pure delay of the instance in
//      48 kHz frames (input frame N leaves the filter as frame N+D).
//   2. Play every voice and nested Bus of the fixed-routing render.
//   3. Call SoLoud::Soloud::prepareFilterLatencyCompensation() before the
//      first Soloud::mix(). On SO_NO_ERROR, every parent input merge gets
//      streaming delay lines that pad the shorter child branches so that
//      signals originating from the same source frame arrive aligned,
//      after which they pass through the parent Bus' own filters together.
//   4. Query SoLoud::Soloud::getFilterLatencyCompensationFrames() for the
//      total root-output latency (again in 48 kHz frames).
//
// Without prepareFilterLatencyCompensation() the original behavior is
// preserved. Once mixing has started, prepare fails with INVALID_PARAMETER.
//
// This header also declares the small delay-line/planner state used by the
// implementation in src/core/soloud_latency.cpp. The state is exposed here
// so the translation unit that implements the Soloud methods can own it;
// user code normally only needs the two Soloud methods and
// FilterInstance::getLatencyFrames().

#include "soloud.h"

#include <vector>

namespace SoLoud
{
	// Streaming per-voice compensation delay line.
	//
	// The delay is applied to one voice's planar scratch buffer at its
	// parent's input merge, after the voice filters and resampler have run
	// (so the delayed PCM is the PCM the existing filters actually
	// produced) and before pan/accumulation into the parent buffer.
	//
	// Channels are delayed independently: storage is split into one
	// mDelay-sized ring per channel. process() receives the planar buffer,
	// the number of valid frames (which may be far smaller than the plane
	// stride) and the plane stride; only aSamples frames are moved.
	class LatencyDelayLine
	{
	public:
		LatencyDelayLine();

		// Allocate zeroed ring storage for aDelayFrames of delay across
		// aChannels independent channels. Called only while preparing.
		void configure(unsigned int aDelayFrames, unsigned int aChannels);

		// Release storage and return to the unconfigured state.
		void clear();

		// Reset the ring to zeros without (re)allocating.
		void reset();

		// Delay aBuffer in place. aBuffer holds aChannels planes of
		// aStride floats each; only the first aSamples entries of every
		// plane are valid for this call.
		void process(float *aBuffer,
		             unsigned int aSamples,
		             unsigned int aStride,
		             unsigned int aChannels);

		unsigned int getDelayFrames() const { return mDelayFrames; }

	private:
		unsigned int mDelayFrames;
		unsigned int mPosition;
		std::vector<float> mRing;
	};

	// Fully prepared compensation plan. Indexed by voice slot; every
	// allocation happens while prepareFilterLatencyCompensation() runs,
	// the audio path only touches preallocated storage.
	class LatencyPlan
	{
	public:
		LatencyPlan();

		// One compensation delay line per possible voice slot.
		LatencyDelayLine mLine[VOICE_COUNT];

		// Total latency of the root output, in 48 kHz frames.
		unsigned int mRootLatencyFrames;
	};
}

#endif // SOLOUD_LATENCY_H
