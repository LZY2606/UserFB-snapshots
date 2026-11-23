////////////////////////////////////////////////////////////////////////////
//                         **** RESAMPLER ****                            //
//                     Sinc-based Audio Resampling                        //
//                Copyright (c) 2006 - 2023 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// resampler.h

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#define _USE_MATH_DEFINES
#include <math.h>

#ifdef ENABLE_THREADS
#include "workers.h"
#endif

#if defined(PATH_WIDTH) && (PATH_WIDTH==64)
typedef double artsample_t;
#else
typedef float artsample_t;
#endif

#define SUBSAMPLE_INTERPOLATE   0x1
#define BLACKMAN_HARRIS         0x2
#define INCLUDE_LOWPASS         0x4
#define RESAMPLE_MULTITHREADED  0x8
#define NO_FILTER_REDUCTION     0x10
#define RESAMPLE_FIXED_RATIO    0x20    // internal use only, do not set
#define EXTRAPOLATE_ENDPOINTS   0x40
#define EXTRAPOLATE_PREFILL     0x80    // internal use only, do not set
#define EXTEND_CONVOLUTION_MATH 0x100
#define RESAMPLER_FLUSHED       0x200   // internal use only, do not set
#define RESAMPLER_SNAP_OFFSET   0x400   // internal use only, do not set

typedef struct {
    unsigned int input_used, output_generated;
} ResampleResult;

// State for the optional clock-tracking mode (used by clock_bridge.c). The
// resampler owns the FIR and sample-history machinery and advances this state
// once for every output frame that is actually produced. The nominal position
// "U" is kept as an integer part plus a fractional part so that large frame
// counts are never accumulated in a single absolute double.

typedef struct {
    double q0;          // nominal source frames per output frame (Fs / Fd)
    double q;           // current source frames per output frame
    double T;           // target rate from the clock estimator
    uint64_t frames;    // cumulative output frames produced since creation/reset
    uint64_t u_int;     // integer part of the absolute nominal source position U
    double u_frac;      // fractional part of U, always in [0, 1)
    int64_t origin;     // buffer index of U = 0 (includes the filter startup delay)
    void *user;         // caller pointer passed to frame_begin()
    void (*frame_begin)(void *user, uint64_t frame, double *target);
} ClockTrackState;

typedef struct resample {
    int numChannels, numSamples, numFilters, numTaps, inputIndex, flags;
    double *tempFilter, outputOffset, fixedRatio, lowpassRatio;
    double (*subsample)(struct resample *cxt, artsample_t *source, double offset);
    artsample_t **buffers, **filters;

    // optional clock-tracking mode (see clock_bridge.c); active when track != NULL
    ClockTrackState *track;

#ifdef ENABLE_THREADS
    Workers *workers;
    const artsample_t *input;
    artsample_t *cbuffer, *output;
    int numInputFrames, numOutputFrames, stride;
    ResampleResult res;
    double ratio;
#endif
} Resample;

#ifdef __cplusplus
extern "C" {
#endif

Resample *resampleInit (int numChannels, int numTaps, int numFilters, double lowpassRatio, int flags);
Resample *resampleFixedRatioInit (int numChannels, int numTaps, int maxFilters, double sourceRate, double destinRate, int lowpassFreq, int flags);
ResampleResult resampleProcess (Resample *cxt, const artsample_t *const *input, int numInputFrames, artsample_t *const *output, int numOutputFrames, double ratio);
ResampleResult resampleProcessInterleaved (Resample *cxt, const artsample_t *input, int numInputFrames, artsample_t *output, int numOutputFrames, double ratio);
ResampleResult resampleProcessAndFlush (Resample *cxt, const artsample_t *const *input, int numInputFrames, artsample_t *const *output, int numOutputFrames, double ratio);
ResampleResult resampleProcessAndFlushInterleaved (Resample *cxt, const artsample_t *input, int numInputFrames, artsample_t *output, int numOutputFrames, double ratio);
unsigned int resampleGetRequiredSamples (Resample *cxt, int numOutputFrames, double ratio);
unsigned int resampleGetExpectedOutput (Resample *cxt, int numInputFrames, double ratio);
void resampleAdvancePosition (Resample *cxt, double delta);
double resampleGetLowpassRatio (Resample *cxt);
double resampleGetPosition (Resample *cxt);
int resampleGetNumFilters (Resample *cxt);
int resampleInterpolationUsed (Resample *cxt);
void resampleReset (Resample *cxt);
void resampleFree (Resample *cxt);

// Internal entry points used by the clock-tracking bridge (clock_bridge.c).
// These are not part of the legacy API and are only valid while a clock
// tracking state has been attached to the context.

uint64_t resamplerClockSourceIndex (Resample *cxt, double *frac);
void resamplerClockBeginFrame (Resample *cxt);
void resamplerClockAdvance (Resample *cxt);
void resamplerClockSlide (Resample *cxt, int drop);
void resamplerClockEnsureRoom (Resample *cxt);

#ifdef __cplusplus
}
#endif
