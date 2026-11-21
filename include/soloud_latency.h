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

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

// Filter latency compensation.
//
// Offline renders often mix several branches whose filter chains introduce
// different amounts of pure delay (e.g. look-ahead limiters, delay lines).
// Without compensation, signals from the same source frame reach a junction
// at different times. The optional compensation mode described here delays
// the shorter branches at every parent node's input junction so that the
// aligned signals pass through the parent bus' filters together.
//
// Usage contract
// --------------
//   1. init() the engine (NULLDRIVER is sufficient), play every source and
//      nested Bus, and configure all filters. Routing is fixed afterwards.
//   2. Call Soloud::prepareFilterLatencyCompensation() exactly once, before
//      the first Soloud::mix() call. On success the plan is active for the
//      whole render; getFilterLatencyCompensationFrames() then reports the
//      total latency of the root output, measured in frames at the 48kHz
//      engine rate. If prepare is never called, mixing behaves exactly as
//      before.
//   3. After the render, deinit() frees the plan. A fresh init/play/prepare
//      cycle starts from a clean state; reused voice slots cannot see a
//      stale plan.
//
// Restrictions (violations make prepare return INVALID_PARAMETER without
// leaving a partial plan behind): prepare after mixing has started, global
// filters, unsupported channel counts (only 1, 2 and 6 channels), paused
// voices, delayed starts, and engine configurations outside the supported
// fixed-routing subset (48kHz, unit play speed, point resampling, fixed
// volume/pan, no 3D). Voices that keep returning zero samples after their
// material ends and auto-stop are supported; no natural EOF tail draining is
// performed.
//
// Semantics
// ---------
// Each FilterInstance may report its pure delay through
// FilterInstance::getLatencyFrames(); the default implementation returns 0.
// Serial filters on one stream add up. For every junction (a bus, including
// the implicit root bus) the longest arrival path among its child subtrees
// determines the junction alignment point; every other child is delayed by
// the difference, using one independent delay line per channel and the PCM
// that the child's own filters have already produced (the source is not read
// a second time). A bus' own filters then delay the aligned, summed signal,
// which is compensated at the bus' parent junction. Compensation buffers are
// allocated once during prepare, so the mixing path performs no heap
// allocation and produces identical output regardless of the mix block
// sizes used.

#ifndef SOLOUD_LATENCY_H
#define SOLOUD_LATENCY_H

#include "soloud.h"
#include "soloud_filter.h"

namespace SoLoud
{
	// Implemented in src/core/soloud_latency.cpp together with
	// FilterInstance::getLatencyFrames() and the mixing-path delay lines.
}

#endif
