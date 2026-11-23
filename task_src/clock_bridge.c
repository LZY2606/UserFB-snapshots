////////////////////////////////////////////////////////////////////////////
//                        **** CLOCK BRIDGE ****                          //
//        Optional hardware-clock tracking on top of the resampler        //
////////////////////////////////////////////////////////////////////////////

// clock_bridge.c
//
// The capture and playback devices are free-running: each reports a
// cumulative frame counter (uint64_t) sampled at the same instant. The bridge
// keeps those observations, fits the source-per-output rate by ordinary least
// squares with an intercept, and uses the result as a target for a small servo
// that adjusts "q" (source frames per output frame) once per output frame. The
// nominal position U is kept as an integer part plus a fraction so that large
// counters are never folded into a single absolute double.
//
// The FIR itself is not duplicated here: the bridge only supplies the
// per-frame target and owns the observation bookkeeping, while the resampler
// runs its normal startup delay, coefficient interpolation and convolution
// (see the clock hooks in resampler.c).

#include "clock_bridge.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_BRIDGE_BASE_SPAN ((uint64_t) 1 << 40)

typedef struct {
    uint64_t k, s, p;
} ClockObservation;

struct ClockBridge {
    Resample *resampler;
    ClockTrackState state;

    ClockObservation pending [CLOCK_BRIDGE_MAX_EVENTS];
    int pending_count, pending_capacity;

    ClockObservation window [CLOCK_BRIDGE_WINDOW];
    int window_count;

    // most recently submitted observation (used for the strict-order checks)
    uint64_t last_k, last_s, last_p;
    int have_last;

    // integer origin used to difference the (possibly huge) counters before
    // any conversion to floating point
    uint64_t base_s, base_p;

    double q0;
};

static void bridge_frame_begin (void *user, uint64_t frame, double *target);

////////////////////////////////////////////////////////////////////////////////////////////////////

static int valid_rate (double rate)
{
    return rate == floor (rate) && rate >= 8000.0 && rate <= 192000.0;
}

ClockBridge *clockBridgeCreate (int numChannels, int numTaps, int numFilters,
    double sourceRate, double destinRate, int flags, int eventCapacity)
{
    ClockBridge *bridge;
    double ratio, lowpass;

    if (numChannels < 1 || numChannels > 8 || numTaps < 4 || numTaps > 1024 ||
        (numTaps & 3) || numFilters < 1 || numFilters > 1024)
        return NULL;

    if (!valid_rate (sourceRate) || !valid_rate (destinRate))
        return NULL;

    ratio = sourceRate / destinRate;

    if (ratio < 0.25 || ratio > 4.0)
        return NULL;

    if (eventCapacity < 1 || eventCapacity > CLOCK_BRIDGE_MAX_EVENTS)
        return NULL;

    bridge = calloc (1, sizeof (ClockBridge));

    if (!bridge)
        return NULL;

    // the lowpass is fixed at creation; it is never rebuilt during processing

    bridge->q0 = ratio;
    lowpass = fmin (1.0, destinRate / (sourceRate * 1.002));

    bridge->resampler = resampleInit (numChannels, numTaps, numFilters, lowpass, flags);

    if (!bridge->resampler) {
        free (bridge);
        return NULL;
    }

    bridge->resampler->track = &bridge->state;

    bridge->state.q0 = ratio;
    bridge->state.q = ratio;
    bridge->state.T = ratio;
    bridge->state.frames = 0;
    bridge->state.u_int = 0;
    bridge->state.u_frac = 0.0;

    // U = 0 is the nominal position without the filter's startup delay, and
    // the original half-tap delay is retained, exactly like the legacy path

    bridge->state.origin = bridge->resampler->numTaps / 2;
    bridge->state.user = bridge;
    bridge->state.frame_begin = bridge_frame_begin;

    bridge->pending_capacity = eventCapacity;

    return bridge;
}

// Fit the observations currently in the window with ordinary least squares of
// s on p (with an intercept). Counters are differenced against an integer base
// first so values near 2^60 never lose their low bits to a double. Returns the
// clamped target T, or the nominal q0 when fewer than two pairs are present.

static double fit_target (ClockBridge *bridge)
{
    int n = bridge->window_count, i;
    double mean_s = 0.0, mean_p = 0.0, sxy = 0.0, sxx = 0.0, slope;

    if (n < 2)
        return bridge->q0;

    for (i = 0; i < n; ++i) {
        mean_s += (double) (bridge->window [i].s - bridge->base_s);
        mean_p += (double) (bridge->window [i].p - bridge->base_p);
    }

    mean_s /= n;
    mean_p /= n;

    for (i = 0; i < n; ++i) {
        double ds = (double) (bridge->window [i].s - bridge->base_s) - mean_s;
        double dp = (double) (bridge->window [i].p - bridge->base_p) - mean_p;

        sxy += ds * dp;
        sxx += dp * dp;
    }

    if (sxx == 0.0)
        return bridge->q0;

    slope = sxy / sxx;

    if (slope > bridge->q0 * 1.002)
        slope = bridge->q0 * 1.002;
    else if (slope < bridge->q0 * 0.998)
        slope = bridge->q0 * 0.998;

    return slope;
}

// Add a committed observation to the rolling fit window. When the window is
// full the oldest pair is evicted; after any eviction the integer base is
// reselected to the new oldest pair, keeping all fitted differences small and
// non-negative.

static void window_push (ClockBridge *bridge, uint64_t k, uint64_t s, uint64_t p)
{
    int evicted = 0;

    if (bridge->window_count == CLOCK_BRIDGE_WINDOW) {
        memmove (bridge->window, bridge->window + 1,
            (CLOCK_BRIDGE_WINDOW - 1) * sizeof (ClockObservation));
        bridge->window_count--;
        evicted = 1;
    }

    bridge->window [bridge->window_count].k = k;
    bridge->window [bridge->window_count].s = s;
    bridge->window [bridge->window_count].p = p;
    bridge->window_count++;

    if (bridge->window_count == 1 || evicted ||
        s - bridge->base_s > CLOCK_BRIDGE_BASE_SPAN ||
        p - bridge->base_p > CLOCK_BRIDGE_BASE_SPAN ||
        bridge->window [0].s < bridge->base_s || bridge->window [0].p < bridge->base_p) {
        bridge->base_s = bridge->window [0].s;
        bridge->base_p = bridge->window [0].p;
    }
}

// Called by the resampler immediately before output frame "frame" is produced.
// Any observations scheduled for that frame take effect here, before the FIR
// runs; the target is then supplied for the servo.

static void bridge_frame_begin (void *user, uint64_t frame, double *target)
{
    ClockBridge *bridge = user;

    while (bridge->pending_count > 0 && bridge->pending [0].k <= frame) {
        window_push (bridge, bridge->pending [0].k, bridge->pending [0].s, bridge->pending [0].p);
        memmove (bridge->pending, bridge->pending + 1,
            (bridge->pending_count - 1) * sizeof (ClockObservation));
        bridge->pending_count--;
    }

    *target = fit_target (bridge);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int clockBridgeSubmit (ClockBridge *bridge, uint64_t k, uint64_t s, uint64_t p)
{
    if (!bridge || bridge->pending_count >= bridge->pending_capacity)
        return -1;

    if (k < bridge->state.frames)
        return -2;

    if (bridge->pending_count > 0 && k <= bridge->pending [bridge->pending_count - 1].k)
        return -3;

    // the s and p sequences must be strictly increasing with bounded steps,
    // measured against the most recently accepted observation

    if (bridge->have_last) {
        if (k <= bridge->last_k)
            return -3;

        if (s <= bridge->last_s || p <= bridge->last_p)
            return -4;

        if (s - bridge->last_s > ((uint64_t) 1 << 32) ||
            p - bridge->last_p > ((uint64_t) 1 << 32))
            return -5;
    }

    bridge->pending [bridge->pending_count].k = k;
    bridge->pending [bridge->pending_count].s = s;
    bridge->pending [bridge->pending_count].p = p;
    bridge->pending_count++;

    bridge->last_k = k;
    bridge->last_s = s;
    bridge->last_p = p;
    bridge->have_last = 1;

    return 0;
}

ResampleResult clockBridgeProcess (ClockBridge *bridge, const artsample_t *const *input,
    int numInputFrames, artsample_t *const *output, int numOutputFrames)
{
    if (!bridge)
        return (ResampleResult) { 0, 0 };

    return resampleProcess (bridge->resampler, input, numInputFrames, output, numOutputFrames, bridge->q0);
}

ResampleResult clockBridgeProcessInterleaved (ClockBridge *bridge, const artsample_t *input,
    int numInputFrames, artsample_t *output, int numOutputFrames)
{
    if (!bridge)
        return (ResampleResult) { 0, 0 };

    return resampleProcessInterleaved (bridge->resampler, input, numInputFrames, output, numOutputFrames, bridge->q0);
}

int clockBridgeQuery (const ClockBridge *bridge, ClockBridgeInfo *info)
{
    if (!bridge || !info)
        return -1;

    info->frames = bridge->state.frames;
    info->position_int = resamplerClockSourceIndex (bridge->resampler, &info->position_frac);
    info->q = bridge->state.q;
    info->target = bridge->state.T;
    info->window = bridge->window_count;

    return 0;
}

void clockBridgeReset (ClockBridge *bridge)
{
    if (!bridge)
        return;

    resampleReset (bridge->resampler);

    bridge->pending_count = 0;
    bridge->window_count = 0;
    bridge->last_k = bridge->last_s = bridge->last_p = 0;
    bridge->have_last = 0;
    bridge->base_s = bridge->base_p = 0;

    bridge->state.q = bridge->q0;
    bridge->state.T = bridge->q0;
    bridge->state.frames = 0;
    bridge->state.u_int = 0;
    bridge->state.u_frac = 0.0;
    bridge->state.origin = bridge->resampler->numTaps / 2;
}

void clockBridgeFree (ClockBridge *bridge)
{
    if (bridge) {
        resampleFree (bridge->resampler);
        free (bridge);
    }
}
