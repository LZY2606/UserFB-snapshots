////////////////////////////////////////////////////////////////////////////
//                        **** CLOCK BRIDGE ****                          //
//        Optional hardware-clock tracking on top of the resampler        //
////////////////////////////////////////////////////////////////////////////

// clock_bridge.h
//
// This module adds an optional clock-tracking mode to the sinc resampler. A
// capture device and a playback device each run on their own sample clock and
// report cumulative frame counters. From time to time the application submits
// an observation (k, s, p): "before output frame k is produced, the capture
// hardware had counted s frames and the playback hardware p frames". The
// bridge fits the rate of source frames per output frame from those
// observations and steers the resampler's position with a small servo, while
// the resampler's FIR (startup delay, filter tables, interpolation and
// convolution order) is used unchanged.
//
// The legacy fixed-ratio and raw-ratio interfaces are untouched; a context
// only uses this mode when it is owned by a ClockBridge.

#ifndef CLOCK_BRIDGE_H
#define CLOCK_BRIDGE_H

#include "resampler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_BRIDGE_MAX_EVENTS   64
#define CLOCK_BRIDGE_WINDOW       8

typedef struct {
    uint64_t frames;        // cumulative output frames produced since creation/reset
    uint64_t position_int;  // integer part of the nominal source position U
    double position_frac;   // fractional part of U, in [0, 1)
    double q;               // current source frames advanced per output frame
    double target;          // target rate T from the clock estimator
    int window;             // number of observations currently in the fit window
} ClockBridgeInfo;

typedef struct ClockBridge ClockBridge;

// Create a clock-tracking bridge. The source and destination rates must be
// integers in [8000, 192000] with sourceRate / destinRate in [0.25, 4].
// numTaps / numFilters / flags are passed to the resampler's sinc filter
// configuration. eventCapacity is the number of observations that may be
// pending for future frames (1 - CLOCK_BRIDGE_MAX_EVENTS). The lowpass is
// fixed at creation to min(1, destinRate / (sourceRate * 1.002)).
// Returns NULL on invalid parameters.

ClockBridge *clockBridgeCreate (int numChannels, int numTaps, int numFilters,
    double sourceRate, double destinRate, int flags, int eventCapacity);

// Submit one observation. k is the absolute output frame before which this
// observation takes effect; s and p are the cumulative capture and playback
// frame counts sampled at the same instant. Observations must satisfy
// k >= frames already produced and k strictly increasing, with s and p
// strictly increasing and each adjacent difference at most 2^32.
// Returns 0 on success, a negative value on failure. On failure neither the
// pending queue, the fit window nor the audio state is modified.

int clockBridgeSubmit (ClockBridge *bridge, uint64_t k, uint64_t s, uint64_t p);

// Process audio, planar (array of per-channel pointers) or interleaved. Both
// share the same frame clock; per-channel history is independent and the
// caller may consume only part of the input. No EOF/flush is required.

ResampleResult clockBridgeProcess (ClockBridge *bridge, const artsample_t *const *input,
    int numInputFrames, artsample_t *const *output, int numOutputFrames);

ResampleResult clockBridgeProcessInterleaved (ClockBridge *bridge, const artsample_t *input,
    int numInputFrames, artsample_t *output, int numOutputFrames);

// Query the current clock state. Returns 0 on success.

int clockBridgeQuery (const ClockBridge *bridge, ClockBridgeInfo *info);

// Discard history, pending observations and the fit window; return q to its
// nominal value and U to zero.

void clockBridgeReset (ClockBridge *bridge);

// Free the bridge and its resampler.
void clockBridgeFree (ClockBridge *bridge);

#ifdef __cplusplus
}
#endif

#endif
