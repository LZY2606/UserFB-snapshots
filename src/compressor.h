//
// sndfilter - Algorithms for sound filters, like reverb, lowpass, etc
// by Sean Connelly (@velipso), https://sean.fun
// Project Home: https://github.com/velipso/sndfilter
// SPDX-License-Identifier: 0BSD
//

// dynamics compressor based on WebAudio specification:
//   https://webaudio.github.io/web-audio-api/#the-dynamicscompressornode-interface

#ifndef SNDFILTER_COMPRESSOR__H
#define SNDFILTER_COMPRESSOR__H

#include "snd.h"

// dynamic range compression is a complex topic with many different algorithms
//
// this API works by first initializing an sf_compressor_state_st structure, then using it to
// process a sample in chunks
//
// for example, say you're processing a stream in 128 samples per chunk:
//
//   sf_compressor_state_st simplecomp;
//   sf_simplecomp(&simplecomp, 48000, 5, -24, 30, 12, 0.003f, 0.250f);
//
//   for each 128 length sample:
//     sf_compressor_process(&simplecomp, 128, input, output);
//
// notice that sf_compressor_process will change a lot of the member variables inside of the state
// structure, since these values must be carried over across chunk boundaries
//
// the chunk size passed to sf_compressor_process is completely arbitrary: any non-negative size
// is accepted, including 1, 17, 33, or zero (where NULL pointers are allowed and no state moves)
//
// control timing is independent of how the calls are chunked: from initialization onwards, every
// SF_COMPRESSOR_SPU consecutive input frames form one control period, and the envelope update
// parameters stay fixed within a period. a call that ends mid-period simply resumes the same
// period on the next call, so splitting one stream into different chunk sizes produces identical
// output. the input and output buffers may alias (input == output is supported), and the process
// path performs no memory allocation
//
// the compressor delays its output by sf_compressor_latency() frames (the predelay ring buffer's
// actual delay, delaybufsize - 1). once the input stream ends, call sf_compressor_drain() to pull
// the delayed tail out:
//
//   int D = sf_compressor_latency(&simplecomp);
//   int emitted = 0;
//   while (emitted < D)
//     emitted += sf_compressor_drain(&simplecomp, D - emitted, tailbuf + emitted);
//
// drain() is equivalent to feeding that many zero frames into sf_compressor_process, and can be
// split across as many calls as the output capacity allows. a capacity of zero writes nothing and
// does not end the stream. the first positive-capacity drain finalizes the instance: afterwards
// sf_compressor_process leaves the output and state untouched, until sf_defaultcomp,
// sf_simplecomp or sf_advancecomp reinitializes the state for another stream. a stream that never
// processed a frame has no tail. no extra silence is emitted to align control periods

// maximum number of samples in the delay buffer
#define SF_COMPRESSOR_MAXDELAY   1024

// samples per update; the compressor works by dividing the input chunks into even smaller sizes,
// and performs heavier calculations after each mini-chunk to adjust the final envelope
#define SF_COMPRESSOR_SPU        32

// not sure what this does exactly, but it is part of the release curve
#define SF_COMPRESSOR_SPACINGDB  5.0f

typedef struct {
	// user can read the metergain state variable after processing a chunk to see how much dB the
	// compressor would have liked to compress the sample; the meter values aren't used to shape the
	// sound in any way, only used for output if desired
	float metergain;

	// everything else shouldn't really be mucked with unless you read the algorithm and feel
	// comfortable
	float meterrelease;
	float threshold;
	float knee;
	float linearpregain;
	float linearthreshold;
	float slope;
	float attacksamplesinv;
	float satreleasesamplesinv;
	float wet;
	float dry;
	float k;
	float kneedboffset;
	float linearthresholdknee;
	float mastergain;
	float a; // adaptive release polynomial coefficients
	float b;
	float c;
	float d;
	float detectoravg;
	float compgain;
	float maxcompdiffdb;
	float enveloperate;      // envelope rate of the current SF_COMPRESSOR_SPU control period
	float scaleddesiredgain; // scaled desired gain held fixed for the same period
	int spupos;          // frames consumed within the current SF_COMPRESSOR_SPU control period
	int totalframes;     // total frames processed since initialization
	int drainremaining;  // tail frames still owed once draining begins (0 before any input)
	int drainactive;     // nonzero once a positive-capacity drain has finalized the stream
	int delaybufsize;
	int delaywritepos;
	int delayreadpos;
	sf_sample_st delaybuf[SF_COMPRESSOR_MAXDELAY]; // predelay buffer
} sf_compressor_state_st;

// populate a compressor state with all default values
void sf_defaultcomp(sf_compressor_state_st *state, int rate);

// populate a compressor state with simple parameters
void sf_simplecomp(sf_compressor_state_st *state,
	int rate,        // input sample rate (samples per second)
	float pregain,   // dB, amount to boost the signal before applying compression [0 to 100]
	float threshold, // dB, level where compression kicks in [-100 to 0]
	float knee,      // dB, width of the knee [0 to 40]
	float ratio,     // unitless, amount to inversely scale the output when applying comp [1 to 20]
	float attack,    // seconds, length of the attack phase [0 to 1]
	float release    // seconds, length of the release phase [0 to 1]
);

// populate a compressor state with advanced parameters
void sf_advancecomp(sf_compressor_state_st *state,
	// these parameters are the same as the simple version above:
	int rate, float pregain, float threshold, float knee, float ratio, float attack, float release,
	// these are the advanced parameters:
	float predelay,     // seconds, length of the predelay buffer [0 to 1]
	float releasezone1, // release zones should be increasing between 0 and 1, and are a fraction
	float releasezone2, //  of the release time depending on the input dB -- these parameters define
	float releasezone3, //  the adaptive release curve, which is discussed in further detail in the
	float releasezone4, //  demo: adaptive-release-curve.html
	float postgain,     // dB, amount of gain to apply after compression [0 to 100]
	float wet           // amount to apply the effect [0 completely dry to 1 completely wet]
);

// this function will process the input sound based on the state passed
// the input and output buffers should be the same size; size may be any non-negative value, and
// when size is zero both pointers may be NULL and no state is advanced. input and output may point
// at the same buffer. after the stream has been finalized with sf_compressor_drain, calls here are
// ignored (output and state are left unchanged) until the state is reinitialized
void sf_compressor_process(sf_compressor_state_st *state, int size, sf_sample_st *input,
	sf_sample_st *output);

// number of frames the compressor delays its output by (delaybufsize - 1); this is also the exact
// number of tail frames sf_compressor_drain will emit in total after at least one input frame
int sf_compressor_latency(const sf_compressor_state_st *state);

// emit the delay tail after the input stream has ended. writes up to capacity frames to output
// (capacity zero writes nothing and does not finalize the stream, so output may be NULL) and
// returns the number of frames written. for a stream that processed at least one frame, the first
// call with a positive capacity finalizes the instance: no further sf_compressor_process calls are
// accepted until reinitialization. draining is equivalent to feeding zero frames into the
// compressor, and repeated calls after the tail is exhausted return zero. a stream that never
// received input returns zero and is not finalized, so it can still be fed frames afterwards
int sf_compressor_drain(sf_compressor_state_st *state, int capacity, sf_sample_st *output);

#endif // SNDFILTER_COMPRESSOR__H
