//
// sndfilter - Algorithms for sound filters, like reverb, lowpass, etc
// by Sean Connelly (@velipso), https://sean.fun
// Project Home: https://github.com/velipso/sndfilter
// SPDX-License-Identifier: 0BSD
//

#include "compressor.h"
#include <math.h>
#include <string.h>

// core algorithm extracted from Chromium source, DynamicsCompressorKernel.cpp, here:
//   https://git.io/v1uSK
//
// changed a few things though in an attempt to simplify the curves and algorithm, and also included
// a pregain so that samples can be scaled up then compressed

void sf_defaultcomp(sf_compressor_state_st *state, int rate){
	// sane defaults
	sf_advancecomp(state, rate,
		  0.000f, // pregain
		-24.000f, // threshold
		 30.000f, // knee
		 12.000f, // ratio
		  0.003f, // attack
		  0.250f, // release
		  0.006f, // predelay
		  0.090f, // releasezone1
		  0.160f, // releasezone2
		  0.420f, // releasezone3
		  0.980f, // releasezone4
		  0.000f, // postgain
		  1.000f  // wet
	);
}

void sf_simplecomp(sf_compressor_state_st *state, int rate, float pregain, float threshold,
	float knee, float ratio, float attack, float release){
	// sane defaults
	sf_advancecomp(state, rate, pregain, threshold, knee, ratio, attack, release,
		0.006f, // predelay
		0.090f, // releasezone1
		0.160f, // releasezone2
		0.420f, // releasezone3
		0.980f, // releasezone4
		0.000f, // postgain
		1.000f  // wet
	);
}

static inline float db2lin(float db){ // dB to linear
	return powf(10.0f, 0.05f * db);
}

static inline float lin2db(float lin){ // linear to dB
	return 20.0f * log10f(lin);
}

static inline float clampf(float v, float min, float max){
	return v < min ? min : (v > max ? max : v);
}

static inline float absf(float v){
	return v < 0.0f ? -v : v;
}

static inline float fixf(float v, float def){
	// fix NaN and infinity values that sneak in... not sure why this is needed, but it is
	if (isnan(v) || isinf(v))
		return def;
	return v;
}

// forward declarations of the envelope helpers used both at initialization and while processing
static inline float adaptivereleasecurve(float x, float a, float b, float c, float d);
static inline float compenvelope(float detectoravg, float compgain, float *maxcompdiffdb,
	float attacksamplesinv, float ang90inv, float a, float b, float c, float d, float spacingdb);

// for more information on the knee curve, check out the compressor-curve.html demo + source code
// included in this repo
static inline float kneecurve(float x, float k, float linearthreshold){
	return linearthreshold + (1.0f - expf(-k * (x - linearthreshold))) / k;
}

static inline float kneeslope(float x, float k, float linearthreshold){
	return k * x / ((k * linearthreshold + 1.0f) * expf(k * (x - linearthreshold)) - 1);
}

static inline float compcurve(float x, float k, float slope, float linearthreshold,
	float linearthresholdknee, float threshold, float knee, float kneedboffset){
	if (x < linearthreshold)
		return x;
	if (knee <= 0.0f) // no knee in curve
		return db2lin(threshold + slope * (lin2db(x) - threshold));
	if (x < linearthresholdknee)
		return kneecurve(x, k, linearthreshold);
	return db2lin(kneedboffset + slope * (lin2db(x) - threshold - knee));
}

// this is the main initialization function
// it does a bunch of pre-calculation so that the inner loop of signal processing is fast
void sf_advancecomp(sf_compressor_state_st *state, int rate, float pregain, float threshold,
	float knee, float ratio, float attack, float release, float predelay, float releasezone1,
	float releasezone2, float releasezone3, float releasezone4, float postgain, float wet){

	// setup the predelay buffer
	int delaybufsize = rate * predelay;
	if (delaybufsize < 1)
		delaybufsize = 1;
	else if (delaybufsize > SF_COMPRESSOR_MAXDELAY)
		delaybufsize = SF_COMPRESSOR_MAXDELAY;
        memset(state->delaybuf, 0, sizeof(sf_sample_st) * delaybufsize);

	// useful values
	float linearpregain = db2lin(pregain);
	float linearthreshold = db2lin(threshold);
	float slope = 1.0f / ratio;
	float attacksamples = rate * attack;
	float attacksamplesinv = 1.0f / attacksamples;
	float releasesamples = rate * release;
	float satrelease = 0.0025f; // seconds
	float satreleasesamplesinv = 1.0f / ((float)rate * satrelease);
	float dry = 1.0f - wet;

	// metering values (not used in core algorithm, but used to output a meter if desired)
	float meterfalloff = 0.325f; // seconds
	float meterrelease = 1.0f - expf(-1.0f / ((float)rate * meterfalloff));

	// calculate knee curve parameters
	float k = 5.0f; // initial guess
	float kneedboffset = 0.0f;
	float linearthresholdknee = 0.0f;
	if (knee > 0.0f){ // if a knee exists, search for a good k value
		float xknee = db2lin(threshold + knee);
		float mink = 0.1f;
		float maxk = 10000.0f;
		// search by comparing the knee slope at the current k guess, to the ideal slope
		for (int i = 0; i < 15; i++){
			if (kneeslope(xknee, k, linearthreshold) < slope)
				maxk = k;
			else
				mink = k;
			k = sqrtf(mink * maxk);
		}
		kneedboffset = lin2db(kneecurve(xknee, k, linearthreshold));
		linearthresholdknee = db2lin(threshold + knee);
	}

	// calculate a master gain based on what sounds good
	float fulllevel = compcurve(1.0f, k, slope, linearthreshold, linearthresholdknee,
		threshold, knee, kneedboffset);
	float mastergain = db2lin(postgain) * powf(1.0f / fulllevel, 0.6f);

	// calculate the adaptive release curve parameters
	// solve a,b,c,d in `y = a*x^3 + b*x^2 + c*x + d`
	// interescting points (0, y1), (1, y2), (2, y3), (3, y4)
	float y1 = releasesamples * releasezone1;
	float y2 = releasesamples * releasezone2;
	float y3 = releasesamples * releasezone3;
	float y4 = releasesamples * releasezone4;
	float a = (-y1 + 3.0f * y2 - 3.0f * y3 + y4) / 6.0f;
	float b = y1 - 2.5f * y2 + 2.0f * y3 - 0.5f * y4;
	float c = (-11.0f * y1 + 18.0f * y2 - 9.0f * y3 + 2.0f * y4) / 6.0f;
	float d = y1;

	// save everything
	state->metergain            = 1.0f; // large value overwritten immediately since it's always < 0
	state->meterrelease         = meterrelease;
	state->threshold            = threshold;
	state->knee                 = knee;
	state->wet                  = wet;
	state->linearpregain        = linearpregain;
	state->linearthreshold      = linearthreshold;
	state->slope                = slope;
	state->attacksamplesinv     = attacksamplesinv;
	state->satreleasesamplesinv = satreleasesamplesinv;
	state->dry                  = dry;
	state->k                    = k;
	state->kneedboffset         = kneedboffset;
	state->linearthresholdknee  = linearthresholdknee;
	state->mastergain           = mastergain;
	state->a                    = a;
	state->b                    = b;
	state->c                    = c;
	state->d                    = d;
	state->detectoravg          = 0.0f;
	state->compgain             = 1.0f;
	state->maxcompdiffdb        = -1.0f;
	state->spupos               = 0;
	state->totalframes          = 0;
	state->drainremaining       = 0;
	state->drainactive          = 0;
	state->delaybufsize         = delaybufsize;
	state->delaywritepos        = 0;
	state->delayreadpos         = delaybufsize > 1 ? 1 : 0;

	// precompute the envelope parameters the first SF_COMPRESSOR_SPU control period opens with, so
	// a stream whose very call is shorter than a period still behaves like a period started at init
	float ang90inv = 2.0f / (float)M_PI;
	float firstmaxcompdiffdb = -1.0f;
	state->enveloperate = compenvelope(state->detectoravg, state->compgain, &firstmaxcompdiffdb,
		attacksamplesinv, ang90inv, a, b, c, d, SF_COMPRESSOR_SPACINGDB);
	state->scaleddesiredgain = asinf(state->detectoravg) * ang90inv;
	state->maxcompdiffdb = firstmaxcompdiffdb;
}

// for more information on the adaptive release curve, check out adaptive-release-curve.html demo +
// source code included in this repo
static inline float adaptivereleasecurve(float x, float a, float b, float c, float d){
	// a*x^3 + b*x^2 + c*x + d
	float x2 = x * x;
	return a * x2 * x + b * x2 + c * x + d;
}

// recompute the envelope rate that applies for one SF_COMPRESSOR_SPU control period; the same
// values are used by every frame inside the period, regardless of where call boundaries fall.
// has a side effect on *maxcompdiffdb, which is why it must run exactly once per period
static inline float compenvelope(float detectoravg, float compgain, float *maxcompdiffdb,
	float attacksamplesinv, float ang90inv, float a, float b, float c, float d, float spacingdb){
	detectoravg = fixf(detectoravg, 1.0f);
	float desiredgain = detectoravg;
	float scaleddesiredgain = asinf(desiredgain) * ang90inv;
	float compdiffdb = lin2db(compgain / scaleddesiredgain);

	float enveloperate;
	if (compdiffdb < 0.0f){ // compgain < scaleddesiredgain, so we're releasing
		compdiffdb = fixf(compdiffdb, -1.0f);
		*maxcompdiffdb = -1; // reset for a future attack mode
		// apply the adaptive release curve
		// scale compdiffdb between 0-3
		float x = (clampf(compdiffdb, -12.0f, 0.0f) + 12.0f) * 0.25f;
		float releasesamples = adaptivereleasecurve(x, a, b, c, d);
		enveloperate = db2lin(spacingdb / releasesamples);
	}
	else{ // compresorgain > scaleddesiredgain, so we're attacking
		compdiffdb = fixf(compdiffdb, 1.0f);
		if (*maxcompdiffdb == -1 || *maxcompdiffdb < compdiffdb)
			*maxcompdiffdb = compdiffdb;
		float attenuate = *maxcompdiffdb;
		if (attenuate < 0.5f)
			attenuate = 0.5f;
		enveloperate = 1.0f - powf(0.25f / attenuate, attacksamplesinv);
	}
	return enveloperate;
}

// run one frame through the compressor; zero input (used while draining) pushes silence through
// the predelay buffer
static inline void compframe(sf_compressor_state_st *state, float inputL, float inputR,
	float linearpregain, float threshold, float knee, float linearthreshold, float slope,
	float satreleasesamplesinv, float wet, float dry, float k, float kneedboffset,
	float linearthresholdknee, float mastergain, float enveloperate, float ang90,
	float scaleddesiredgain, float meterrelease, sf_sample_st *out){

	int delaybufsize  = state->delaybufsize;
	int delaywritepos = state->delaywritepos;
	int delayreadpos  = state->delayreadpos;
	sf_sample_st *delaybuf = state->delaybuf;
	float detectoravg = state->detectoravg;
	float compgain    = state->compgain;
	float metergain   = state->metergain;

	inputL *= linearpregain;
	inputR *= linearpregain;
	delaybuf[delaywritepos] = (sf_sample_st){ .L = inputL, .R = inputR };

	float absL = absf(inputL);
	float absR = absf(inputR);
	float inputmax = absL > absR ? absL : absR;

	float attenuation;
	if (inputmax < 0.0001f)
		attenuation = 1.0f;
	else{
		float inputcomp = compcurve(inputmax, k, slope, linearthreshold,
			linearthresholdknee, threshold, knee, kneedboffset);
		attenuation = inputcomp / inputmax;
	}

	float rate;
	if (attenuation > detectoravg){ // if releasing
		float attenuationdb = -lin2db(attenuation);
		if (attenuationdb < 2.0f)
			attenuationdb = 2.0f;
		float dbpersample = attenuationdb * satreleasesamplesinv;
		rate = db2lin(dbpersample) - 1.0f;
	}
	else
		rate = 1.0f;

	detectoravg += (attenuation - detectoravg) * rate;
	if (detectoravg > 1.0f)
		detectoravg = 1.0f;
	detectoravg = fixf(detectoravg, 1.0f);

	if (enveloperate < 1) // attack, reduce gain
		compgain += (scaleddesiredgain - compgain) * enveloperate;
	else{ // release, increase gain
		compgain *= enveloperate;
		if (compgain > 1.0f)
			compgain = 1.0f;
	}

	// the final gain value!
	float premixgain = sinf(ang90 * compgain);
	float gain = dry + wet * mastergain * premixgain;

	// calculate metering (not used in core algo, but used to output a meter if desired)
	float premixgaindb = lin2db(premixgain);
	if (premixgaindb < metergain)
		metergain = premixgaindb; // spike immediately
	else
		metergain += (premixgaindb - metergain) * meterrelease; // fall slowly

	// apply the gain to the delayed sample; both channels share the one gain value
	*out = (sf_sample_st){
		.L = delaybuf[delayreadpos].L * gain,
		.R = delaybuf[delayreadpos].R * gain
	};

	state->detectoravg   = detectoravg;
	state->compgain      = compgain;
	state->metergain     = metergain;
	state->delaywritepos = (delaywritepos + 1) % delaybufsize;
	state->delayreadpos  = (delayreadpos + 1) % delaybufsize;
}

// shared processing loop behind sf_compressor_process and sf_compressor_drain; a NULL input feeds
// silence (the drain path), otherwise n frames are read from input. the control period is tracked
// in state->spupos and only restarts every SF_COMPRESSOR_SPU frames, so call boundaries never
// reset it
static void comprun(sf_compressor_state_st *state, int n, const sf_sample_st *input,
	sf_sample_st *output){

	float meterrelease         = state->meterrelease;
	float threshold            = state->threshold;
	float knee                 = state->knee;
	float linearpregain        = state->linearpregain;
	float linearthreshold      = state->linearthreshold;
	float slope                = state->slope;
	float attacksamplesinv     = state->attacksamplesinv;
	float satreleasesamplesinv = state->satreleasesamplesinv;
	float wet                  = state->wet;
	float dry                  = state->dry;
	float k                    = state->k;
	float kneedboffset         = state->kneedboffset;
	float linearthresholdknee  = state->linearthresholdknee;
	float mastergain           = state->mastergain;
	float a                    = state->a;
	float b                    = state->b;
	float c                    = state->c;
	float d                    = state->d;
	float maxcompdiffdb        = state->maxcompdiffdb;
	float enveloperate         = state->enveloperate;
	float scaleddesiredgain    = state->scaleddesiredgain;
	int spupos                 = state->spupos;
	float ang90 = (float)M_PI * 0.5f;
	float ang90inv = 2.0f / (float)M_PI;
	float spacingdb = SF_COMPRESSOR_SPACINGDB;

	for (int i = 0; i < n; i++){
		if (spupos == SF_COMPRESSOR_SPU){
			// a new control period starts here (possibly inside a drain): recompute once and keep
			// the same envelope parameters for the next SF_COMPRESSOR_SPU frames
			enveloperate = compenvelope(state->detectoravg, state->compgain, &maxcompdiffdb,
				attacksamplesinv, ang90inv, a, b, c, d, spacingdb);
			scaleddesiredgain = asinf(fixf(state->detectoravg, 1.0f)) * ang90inv;
			spupos = 0;
		}

		float inputL = input != NULL ? input[i].L : 0.0f;
		float inputR = input != NULL ? input[i].R : 0.0f;
		compframe(state, inputL, inputR, linearpregain, threshold, knee, linearthreshold, slope,
			satreleasesamplesinv, wet, dry, k, kneedboffset, linearthresholdknee, mastergain,
			enveloperate, ang90, scaleddesiredgain, meterrelease, &output[i]);
		spupos++;
	}

	state->maxcompdiffdb     = maxcompdiffdb;
	state->enveloperate      = enveloperate;
	state->scaleddesiredgain = scaleddesiredgain;
	state->spupos            = spupos;
}

void sf_compressor_process(sf_compressor_state_st *state, int size, sf_sample_st *input,
	sf_sample_st *output){

	if (size <= 0 || state->drainactive)
		return; // zero-length calls (NULL pointers allowed) and finalized streams move no state

	comprun(state, size, input, output);

	state->totalframes += size;
	// the ring buffer always holds delaybufsize - 1 frames of not-yet-emitted input
	state->drainremaining = state->delaybufsize - 1;
}

int sf_compressor_latency(const sf_compressor_state_st *state){
	return state->delaybufsize - 1;
}

int sf_compressor_drain(sf_compressor_state_st *state, int capacity, sf_sample_st *output){
	if (capacity <= 0)
		return 0; // capacity zero does not finalize the stream or end anything
	if (state->totalframes == 0)
		return 0; // an input stream that never started has no tail
	state->drainactive = 1;

	int wrote = 0;
	while (wrote < capacity && state->drainremaining > 0){
		int n = capacity - wrote;
		if (n > state->drainremaining)
			n = state->drainremaining;
		// equivalent to feeding n zero frames through sf_compressor_process
		comprun(state, n, NULL, output + wrote);
		state->drainremaining -= n;
		wrote += n;
	}
	return wrote;
}
