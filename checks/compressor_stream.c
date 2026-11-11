//
// regression for the stream-capable sf_compressor: arbitrary chunk sizes, control period
// continuity across calls, delay-tail draining, and an end-to-end WAV check of the CLI path
//
// the old whole-32-frame algorithm is copied here independently (oldref_*) so the compression
// curve is protected by a reference that does not share compressor.c's implementation
//
// SPDX-License-Identifier: 0BSD
//

#include "compressor.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------------------------
// independent copy of the original whole-period (SF_COMPRESSOR_SPU) algorithm
// -----------------------------------------------------------------------------------------------

typedef struct {
	float metergain, meterrelease, threshold, knee, linearpregain, linearthreshold, slope;
	float attacksamplesinv, satreleasesamplesinv, wet, dry, k, kneedboffset;
	float linearthresholdknee, mastergain, a, b, c, d, detectoravg, compgain, maxcompdiffdb;
	int delaybufsize, delaywritepos, delayreadpos;
	sf_sample_st delaybuf[SF_COMPRESSOR_MAXDELAY];
} oldref;

static float rdb2lin(float db){ return powf(10.0f, 0.05f * db); }
static float rlin2db(float lin){ return 20.0f * log10f(lin); }
static float rkneecurve(float x, float k, float t){
	return t + (1.0f - expf(-k * (x - t))) / k;
}
static float rcompcurve(float x, float k, float s, float t, float tk, float th, float kn,
	float off){
	if (x < t)
		return x;
	if (kn <= 0.0f)
		return rdb2lin(th + s * (rlin2db(x) - th));
	if (x < tk)
		return rkneecurve(x, k, t);
	return rdb2lin(off + s * (rlin2db(x) - th - kn));
}
static float radaptive(float x, float a, float b, float c, float d){
	float x2 = x * x;
	return a * x2 * x + b * x2 + c * x + d;
}
static float rfix(float v, float d){ return (isnan(v) || isinf(v)) ? d : v; }
static float rclamp(float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); }
static float rabs(float v){ return v < 0.0f ? -v : v; }

static void oldref_init(oldref *o, const sf_compressor_state_st *s){
	memset(o, 0, sizeof(*o));
	o->metergain = s->metergain; o->meterrelease = s->meterrelease;
	o->threshold = s->threshold; o->knee = s->knee;
	o->linearpregain = s->linearpregain; o->linearthreshold = s->linearthreshold;
	o->slope = s->slope; o->attacksamplesinv = s->attacksamplesinv;
	o->satreleasesamplesinv = s->satreleasesamplesinv; o->wet = s->wet; o->dry = s->dry;
	o->k = s->k; o->kneedboffset = s->kneedboffset;
	o->linearthresholdknee = s->linearthresholdknee; o->mastergain = s->mastergain;
	o->a = s->a; o->b = s->b; o->c = s->c; o->d = s->d;
	o->detectoravg = s->detectoravg; o->compgain = s->compgain;
	o->maxcompdiffdb = s->maxcompdiffdb; o->delaybufsize = s->delaybufsize;
	o->delaywritepos = s->delaywritepos; o->delayreadpos = s->delayreadpos;
}

// original semantics: size must be a whole multiple of SF_COMPRESSOR_SPU
static void oldref_process(oldref *o, int size, sf_sample_st *input, sf_sample_st *output){
	float metergain = o->metergain, meterrelease = o->meterrelease, threshold = o->threshold;
	float knee = o->knee, linearpregain = o->linearpregain;
	float linearthreshold = o->linearthreshold, slope = o->slope;
	float attacksamplesinv = o->attacksamplesinv, satrel = o->satreleasesamplesinv;
	float wet = o->wet, dry = o->dry, k = o->k, kneedboffset = o->kneedboffset;
	float ltk = o->linearthresholdknee, mg = o->mastergain, a = o->a, b = o->b, c = o->c, d = o->d;
	float detectoravg = o->detectoravg, compgain = o->compgain, maxcompdiffdb = o->maxcompdiffdb;
	int dbs = o->delaybufsize, dwp = o->delaywritepos, drp = o->delayreadpos;
	sf_sample_st *db = o->delaybuf;
	int spc = SF_COMPRESSOR_SPU, chunks = size / spc, samplepos = 0;
	float ang90 = (float)M_PI * 0.5f, ang90inv = 2.0f / (float)M_PI;
	float spacingdb = SF_COMPRESSOR_SPACINGDB;
	for (int ch = 0; ch < chunks; ch++){
		detectoravg = rfix(detectoravg, 1.0f);
		float scaleddesiredgain = asinf(detectoravg) * ang90inv;
		float compdiffdb = rlin2db(compgain / scaleddesiredgain);
		float enveloperate;
		if (compdiffdb < 0.0f){
			compdiffdb = rfix(compdiffdb, -1.0f);
			maxcompdiffdb = -1;
			float x = (rclamp(compdiffdb, -12.0f, 0.0f) + 12.0f) * 0.25f;
			enveloperate = rdb2lin(spacingdb / radaptive(x, a, b, c, d));
		}
		else{
			compdiffdb = rfix(compdiffdb, 1.0f);
			if (maxcompdiffdb == -1 || maxcompdiffdb < compdiffdb)
				maxcompdiffdb = compdiffdb;
			float att = maxcompdiffdb < 0.5f ? 0.5f : maxcompdiffdb;
			enveloperate = 1.0f - powf(0.25f / att, attacksamplesinv);
		}
		for (int chi = 0; chi < spc; chi++, samplepos++,
			drp = (drp + 1) % dbs, dwp = (dwp + 1) % dbs){
			float inL = input[samplepos].L * linearpregain;
			float inR = input[samplepos].R * linearpregain;
			db[dwp] = (sf_sample_st){ .L = inL, .R = inR };
			float maxin = rabs(inL) > rabs(inR) ? rabs(inL) : rabs(inR);
			float attenuation = maxin < 0.0001f ? 1.0f :
				rcompcurve(maxin, k, slope, linearthreshold, ltk, threshold, knee, kneedboffset)
				/ maxin;
			float rate;
			if (attenuation > detectoravg){
				float adb = -rlin2db(attenuation);
				if (adb < 2.0f)
					adb = 2.0f;
				rate = rdb2lin(adb * satrel) - 1.0f;
			}
			else
				rate = 1.0f;
			detectoravg += (attenuation - detectoravg) * rate;
			if (detectoravg > 1.0f)
				detectoravg = 1.0f;
			detectoravg = rfix(detectoravg, 1.0f);
			if (enveloperate < 1)
				compgain += (scaleddesiredgain - compgain) * enveloperate;
			else{
				compgain *= enveloperate;
				if (compgain > 1.0f)
					compgain = 1.0f;
			}
			float premixgain = sinf(ang90 * compgain);
			float gain = dry + wet * mg * premixgain;
			float premixgaindb = rlin2db(premixgain);
			if (premixgaindb < metergain)
				metergain = premixgaindb;
			else
				metergain += (premixgaindb - metergain) * meterrelease;
			output[samplepos] = (sf_sample_st){
				.L = db[drp].L * gain, .R = db[drp].R * gain
			};
		}
	}
	o->metergain = metergain; o->detectoravg = detectoravg; o->compgain = compgain;
	o->maxcompdiffdb = maxcompdiffdb; o->delaywritepos = dwp; o->delayreadpos = drp;
}

// -----------------------------------------------------------------------------------------------
// shared helpers
// -----------------------------------------------------------------------------------------------

// deterministic stereo pattern with deliberately unequal channel amplitudes; the final frame is
// always a strong, asymmetric pulse so a lost last frame is detectable
static void genpattern(sf_sample_st *x, int n){
	for (int i = 0; i < n; i++){
		x[i].L = (i % 7 == 0) ? 0.50f : 0.05f;
		x[i].R = (i % 11 == 0) ? -0.25f : -0.02f;
	}
	for (int i = 0; i < n; i += 29)
		x[i].L = 0.80f; // left-only pulses
	if (n > 0)
		x[n - 1] = (sf_sample_st){ .L = 0.70f, .R = -0.30f };
}

static double abserr(const sf_sample_st *a, const sf_sample_st *b, int n){
	double e = 0.0;
	for (int i = 0; i < n; i++){
		double e1 = fabs((double)a[i].L - b[i].L);
		double e2 = fabs((double)a[i].R - b[i].R);
		if (e1 > e) e = e1;
		if (e2 > e) e = e2;
	}
	return e;
}

static void checkfinite(const sf_sample_st *a, int n){
	for (int i = 0; i < n; i++)
		assert(isfinite(a[i].L) && isfinite(a[i].R));
}

// run one input through the library using a chunk pattern and return the collected output
static void chunked(sf_compressor_state_st *st, sf_sample_st *in, int n,
	const int *pattern, int patlen, sf_sample_st *out){
	int pos = 0, pi = 0;
	while (pos < n){
		int len = pattern[pi % patlen];
		if (len > n - pos)
			len = n - pos;
		sf_compressor_process(st, len, in + pos, out + pos);
		pos += len;
		pi++;
	}
}

// -----------------------------------------------------------------------------------------------
// C-level regression groups
// -----------------------------------------------------------------------------------------------

static void group_curve_vs_oldref(void){
	const int N = 288; // multiple of 32
	sf_sample_st *x = malloc(sizeof(sf_sample_st) * N);
	sf_sample_st *nout = malloc(sizeof(sf_sample_st) * N);
	sf_sample_st *oout = malloc(sizeof(sf_sample_st) * N);
	genpattern(x, N);

	sf_compressor_state_st st;
	oldref o;
	sf_defaultcomp(&st, 48000);
	oldref_init(&o, &st);
	sf_compressor_process(&st, N, x, nout);
	oldref_process(&o, N, x, oout);
	double e = abserr(nout, oout, N);
	double merr = fabs(st.metergain - o.metergain);
	printf("group curve-vs-oldref:      maxerr=%.3g metererr=%.3g frames=%d\n", e, merr, N);
	assert(e < 1e-6 && merr < 1e-6);
	checkfinite(nout, N);
	free(x); free(nout); free(oout);
}

static void group_chunking(void){
	const int N = 300; // intentionally not a multiple of 32
	sf_sample_st *x = malloc(sizeof(sf_sample_st) * N);
	sf_sample_st *whole = malloc(sizeof(sf_sample_st) * N);
	genpattern(x, N);
	sf_compressor_state_st ref;
	sf_defaultcomp(&ref, 48000);
	sf_compressor_process(&ref, N, x, whole);

	static const int pat_one[] = {1};
	static const int pat_17335[] = {17, 33, 5};
	static const int pat_32[] = {32};
	static const int pat_33[] = {33};
	static const int pat_31[] = {31};
	static const int pat_primes[] = {2, 3, 5, 7, 11};
	static const int pat_173351[] = {17, 33, 5, 1};
	struct { const int *pat; int len; const char *name; } groups[] = {
		{ pat_one, 1, "one-frame" },
		{ pat_17335, 3, "17/33/5" },
		{ pat_32, 1, "32-blocks" },
		{ pat_33, 1, "33-blocks" },
		{ pat_31, 1, "31-blocks" },
		{ pat_primes, 5, "mixed-primes" },
		{ pat_173351, 4, "17/33/5/1" },
	};
	for (unsigned g = 0; g < sizeof(groups) / sizeof(groups[0]); g++){
		sf_compressor_state_st st;
		sf_defaultcomp(&st, 48000);
		sf_sample_st *out = malloc(sizeof(sf_sample_st) * N);
		chunked(&st, x, N, groups[g].pat, groups[g].len, out);
		double e = abserr(whole, out, N);
		double merr = fabs(st.metergain - ref.metergain);
		printf("group chunking %-11s maxerr=%.3g metererr=%.3g\n", groups[g].name, e, merr);
		assert(e < 1e-6 && merr < 1e-6);
		assert(st.spupos == ref.spupos && st.totalframes == N);
		assert(st.delaywritepos == ref.delaywritepos && st.delayreadpos == ref.delayreadpos);
		free(out);
	}
	free(x); free(whole);
}

static void group_asymmetric_pulse(void){
	// unequal L/R pulses must compress with one shared gain: L/R ratio is preserved by the gain.
	// N must exceed the pulse index plus the predelay latency so the pulse reaches the output
	const int N = 320;
	sf_sample_st x[320], out[320];
	memset(x, 0, sizeof(x));
	x[10] = (sf_sample_st){ .L = 0.80f, .R = 0.10f }; // 8:1 asymmetric pulse
	sf_compressor_state_st st;
	sf_defaultcomp(&st, 48000);
	sf_compressor_process(&st, N, x, out);
	int d = st.delaybufsize - 1;
	int at = 10 + d; // where the pulse exits the predelay buffer
	assert(at < N);
	double ratio = out[at].L / out[at].R;
	printf("group asymmetric-pulse:     ratio=%.6f expect=8.000000 maxamp=%.3g\n",
		ratio, fabs(out[at].L));
	assert(fabs(ratio - 8.0) < 1e-5);
	checkfinite(out, N);
}

static void group_wet0(void){
	const int N = 129;
	sf_compressor_state_st st;
	sf_advancecomp(&st, 48000, 0, -24, 30, 12, 0.003f, 0.25f, 0.006f, 0.09f, 0.16f, 0.42f,
		0.98f, 0, 0.0f); // wet = 0
	int d = sf_compressor_latency(&st);
	sf_sample_st *x = malloc(sizeof(sf_sample_st) * N);
	sf_sample_st *out = malloc(sizeof(sf_sample_st) * (N + d));
	genpattern(x, N);
	sf_compressor_process(&st, N, x, out);
	int tail = sf_compressor_drain(&st, d, out + N);
	assert(tail == d);

	// wet = 0 makes the gain exactly 1, so the whole N+D output is just the input delayed by d
	double e = 0.0;
	for (int i = 0; i < N + d; i++){
		sf_sample_st s = (i >= d && i - d < N) ? x[i - d] : (sf_sample_st){ 0, 0 };
		double e1 = fabs(out[i].L - s.L), e2 = fabs(out[i].R - s.R);
		if (e1 > e) e = e1;
		if (e2 > e) e = e2;
	}
	// and the delayed, asymmetric last-frame pulse must still be present in the final frame
	double ratio = out[N + d - 1].L / -out[N + d - 1].R;
	printf("group wet0:                 dry-path maxerr=%.3g last-ratio=%.4f tailframes=%d\n",
		e, ratio, tail);
	assert(e < 1e-6 && fabs(ratio - (0.70 / 0.30)) < 1e-5);
	free(x); free(out);
}

static void group_predelay_extremes(void){
	const int N = 100;
	sf_sample_st x[100], out[100];
	genpattern(x, N);

	sf_compressor_state_st zero;
	sf_advancecomp(&zero, 48000, 0, -24, 30, 12, 0.003f, 0.25f, 0.0f, 0.09f, 0.16f, 0.42f,
		0.98f, 0, 1);
	assert(sf_compressor_latency(&zero) == 0 && zero.delaybufsize == 1);
	// a 32-aligned run still matches the old reference on its single-slot ring buffer
	int M = (N / SF_COMPRESSOR_SPU) * SF_COMPRESSOR_SPU;
	sf_sample_st zout[96], zref[96];
	oldref zo;
	oldref_init(&zo, &zero);
	sf_compressor_process(&zero, M, x, zout);
	oldref_process(&zo, M, x, zref);
	double e0 = abserr(zout, zref, M);
	sf_sample_st tail[SF_COMPRESSOR_MAXDELAY];
	assert(sf_compressor_drain(&zero, 16, tail) == 0); // nothing to drain
	printf("group predelay-zero:        latency=0 oldref-maxerr=%.3g tailframes=0\n", e0);
	assert(e0 < 1e-6);
	checkfinite(zout, M);

	sf_compressor_state_st cap;
	sf_advancecomp(&cap, 48000, 0, -24, 30, 12, 0.003f, 0.25f, 1.0f, 0.09f, 0.16f, 0.42f,
		0.98f, 0, 1);
	int D = sf_compressor_latency(&cap);
	assert(D == SF_COMPRESSOR_MAXDELAY - 1 && cap.delaybufsize == SF_COMPRESSOR_MAXDELAY);
	sf_compressor_process(&cap, N, x, out);
	int got = 0, w, calls = 0;
	while ((w = sf_compressor_drain(&cap, 37, tail + got)) > 0){
		got += w;
		calls++;
	}
	printf("group predelay-cap:         latency=%d tailframes=%d draincalls=%d\n", D, got, calls);
	assert(got == D && calls > 1);
	assert(sf_compressor_drain(&cap, 37, tail) == 0);
}

static void group_lengths(void){
	int lens[5] = {0, 1, 31, 32, 33};
	for (int li = 0; li < 5; li++){
		int L = lens[li];
		sf_sample_st x[33], whole[33], split[33];
		genpattern(x, 33);
		sf_compressor_state_st a, b;
		sf_defaultcomp(&a, 48000);
		sf_defaultcomp(&b, 48000);
		sf_compressor_process(&a, L, x, whole);
		int pos = 0, pi = 0;
		const int pat[2] = {7, 3};
		while (pos < L){
			int len = pat[pi++ & 1];
			if (len > L - pos)
				len = L - pos;
			sf_compressor_process(&b, len, x + pos, split + pos);
			pos += len;
		}
		double e = L > 0 ? abserr(whole, split, L) : 0.0;
		// spupos counts frames since the last period reset, so a full period ends at 32 and the
		// reset only happens when the next frame begins
		int expectspu = L == 0 ? 0 : ((L - 1) % SF_COMPRESSOR_SPU) + 1;
		printf("group length %2d:            maxerr=%.3g totalframes=%d spupos=%d\n",
			L, e, a.totalframes, a.spupos);
		assert(e < 1e-6 && a.totalframes == L && a.spupos == expectspu);
	}

	// length zero with NULL pointers must not move any state
	sf_compressor_state_st z, snap;
	sf_defaultcomp(&z, 48000);
	snap = z;
	sf_compressor_process(&z, 0, NULL, NULL);
	assert(memcmp(&snap, &z, sizeof(z)) == 0);
	assert(sf_compressor_drain(&z, 5, NULL) == 0 && z.drainactive == 0);
	printf("group length  0:            null-pointers no state change\n");
}

static void group_drain_lifecycle(void){
	const int N = 193;
	sf_sample_st *x = malloc(sizeof(sf_sample_st) * N);
	sf_sample_st *out = malloc(sizeof(sf_sample_st) * N);
	genpattern(x, N);

	sf_compressor_state_st st;
	sf_defaultcomp(&st, 48000);
	int D = sf_compressor_latency(&st);
	sf_compressor_process(&st, N, x, out);

	// capacity zero must not finalize the stream
	assert(sf_compressor_drain(&st, 0, NULL) == 0 && st.drainactive == 0);

	// split draining across small capacities; total must be exactly D, equivalent to zero input
	sf_sample_st *tail = malloc(sizeof(sf_sample_st) * D);
	int got = 0, w, calls = 0;
	while (got < D){
		w = sf_compressor_drain(&st, 5, tail + got);
		assert(w > 0);
		got += w;
		calls++;
	}
	assert(got == D);
	assert(sf_compressor_drain(&st, 5, tail) == 0); // exhausted: repeated calls return zero

	// reference: a parallel stream fed D zero frames, whose last D outputs are the tail
	sf_compressor_state_st ref;
	sf_defaultcomp(&ref, 48000);
	sf_sample_st *big = malloc(sizeof(sf_sample_st) * (N + D));
	sf_compressor_process(&ref, N, x, big);
	sf_sample_st *zeros = calloc(D, sizeof(sf_sample_st));
	sf_compressor_process(&ref, D, zeros, big + N);
	double e = abserr(tail, big + N, D);
	printf("group drain-lifecycle:      D=%d tailframes=%d draincalls=%d tail-maxerr=%.3g\n",
		D, got, calls, e);
	assert(e < 1e-6);

	// after a positive-capacity drain, process must leave output and state untouched
	sf_compressor_state_st snap = st;
	sf_sample_st probe[3] = {{9, 9}, {9, 9}, {9, 9}};
	sf_compressor_process(&st, 3, probe, probe);
	assert(probe[0].L == 9.0f && probe[2].R == 9.0f);
	assert(memcmp(&snap, &st, sizeof(snap)) == 0);

	// reinitialization starts a fresh stream that reproduces the original output
	sf_compressor_state_st rref;
	sf_defaultcomp(&rref, 48000);
	sf_sample_st *rrefout = malloc(sizeof(sf_sample_st) * N);
	sf_compressor_process(&rref, N, x, rrefout);
	sf_defaultcomp(&st, 48000);
	sf_sample_st *out2 = malloc(sizeof(sf_sample_st) * N);
	sf_compressor_process(&st, N, x, out2);
	double re = abserr(rrefout, out2, N);
	printf("group drain-reinit:         reinit-maxerr=%.3g metererr=%.3g\n",
		re, fabs(st.metergain - rref.metergain));
	assert(re < 1e-6 && fabs(st.metergain - rref.metergain) < 1e-6);

	// an empty stream never has a tail even with positive capacity
	sf_compressor_state_st emp;
	sf_defaultcomp(&emp, 48000);
	assert(sf_compressor_drain(&emp, 8, tail) == 0 && emp.drainactive == 0);
	printf("group drain-empty-stream:   tailframes=0\n");

	free(x); free(out); free(tail); free(big); free(zeros); free(out2); free(rrefout);
}

static void group_inplace(void){
	const int N = 201;
	sf_sample_st *a = malloc(sizeof(sf_sample_st) * N);
	sf_sample_st *b = malloc(sizeof(sf_sample_st) * N);
	sf_sample_st *ref = malloc(sizeof(sf_sample_st) * N);
	genpattern(a, N);
	memcpy(b, a, sizeof(sf_sample_st) * N);
	sf_compressor_state_st s1, s2;
	sf_defaultcomp(&s1, 48000);
	sf_defaultcomp(&s2, 48000);
	sf_compressor_process(&s1, N, a, a);       // input == output
	sf_compressor_process(&s2, N, b, ref);     // separate buffers
	double e = abserr(a, ref, N);
	printf("group inplace:              maxerr=%.3g\n", e);
	assert(e < 1e-6);
	free(a); free(b); free(ref);
}

static int run_ctests(void){
	group_curve_vs_oldref();
	group_chunking();
	group_asymmetric_pulse();
	group_wet0();
	group_predelay_extremes();
	group_lengths();
	group_drain_lifecycle();
	group_inplace();
	printf("compressor_stream: all C groups passed\n");
	return 0;
}

// -----------------------------------------------------------------------------------------------
// independent WAV writer/reader (deliberately not using src/wav.c) for the end-to-end CLI check
// -----------------------------------------------------------------------------------------------

static void wput32(FILE *fp, uint32_t v){
	fputc(v & 0xff, fp); fputc((v >> 8) & 0xff, fp);
	fputc((v >> 16) & 0xff, fp); fputc((v >> 24) & 0xff, fp);
}
static void wput16(FILE *fp, uint16_t v){ fputc(v & 0xff, fp); fputc((v >> 8) & 0xff, fp); }
static uint32_t rget32(FILE *fp){
	uint32_t v = 0;
	for (int i = 0; i < 4; i++){
		int c = fgetc(fp);
		if (c == EOF)
			return 0;
		v |= (uint32_t)c << (8 * i);
	}
	return v;
}
static uint16_t rget16(FILE *fp){
	int b1 = fgetc(fp), b2 = fgetc(fp);
	if (b1 == EOF || b2 == EOF)
		return 0;
	return (uint16_t)(b1 | (b2 << 8));
}

static int16_t to16(float v){
	if (v > 1.0f) v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	return (int16_t)(v < 0 ? v * 32768.0f : v * 32767.0f);
}

// wavgen <file> <frames> <rate>
static int cmd_wavgen(int argc, char **argv){
	assert(argc == 5);
	const char *file = argv[2];
	int n = atoi(argv[3]), rate = atoi(argv[4]);
	FILE *fp = fopen(file, "wb");
	assert(fp != NULL);
	uint32_t databytes = (uint32_t)n * 4;
	wput32(fp, 0x46464952); wput32(fp, databytes + 36); wput32(fp, 0x45564157);
	wput32(fp, 0x20746d66); wput32(fp, 16); wput16(fp, 1); wput16(fp, 2);
	wput32(fp, (uint32_t)rate); wput32(fp, (uint32_t)rate * 4); wput16(fp, 4); wput16(fp, 16);
	wput32(fp, 0x61746164); wput32(fp, databytes);
	sf_sample_st *x = n > 0 ? malloc(sizeof(sf_sample_st) * n) : NULL;
	if (n > 0)
		genpattern(x, n);
	for (int i = 0; i < n; i++){
		wput16(fp, (uint16_t)to16(x[i].L));
		wput16(fp, (uint16_t)to16(x[i].R));
	}
	free(x);
	fclose(fp);
	printf("wavgen: %s frames=%d rate=%d\n", file, n, rate);
	return 0;
}

typedef struct { int frames; int rate; sf_sample_st *s; } wavdata;

// independent RIFF parse; returns false on any structural problem
static int wavread(const char *file, wavdata *w){
	w->frames = 0; w->rate = 0; w->s = NULL;
	FILE *fp = fopen(file, "rb");
	if (fp == NULL)
		return 0;
	if (rget32(fp) != 0x46464952 || rget32(fp) == 0 || rget32(fp) != 0x45564157){
		fclose(fp);
		return 0;
	}
	int gotfmt = 0, gotdata = 0;
	uint16_t channels = 0, bps = 0;
	uint32_t datasize = 0;
	while (!feof(fp)){
		uint32_t id = rget32(fp);
		uint32_t size = rget32(fp);
		if (ferror(fp))
			break;
		if (id == 0x20746d66){
			if (gotfmt || size < 16){ fclose(fp); return 0; }
			gotfmt = 1;
			if (rget16(fp) != 1){ fclose(fp); return 0; }
			channels = rget16(fp);
			w->rate = (int)rget32(fp);
			rget32(fp); rget16(fp); bps = rget16(fp);
			if (channels != 2 || bps != 16){ fclose(fp); return 0; }
			if (size > 16)
				fseek(fp, (long)(size - 16), SEEK_CUR);
		}
		else if (id == 0x61746164){
			if (!gotfmt || (size % 4) != 0){ fclose(fp); return 0; }
			gotdata = 1;
			datasize = size;
			break;
		}
		else if (size > 0)
			fseek(fp, (long)size, SEEK_CUR);
	}
	if (!gotdata)
		return 0;
	w->frames = (int)(datasize / 4);
	w->s = w->frames > 0 ? malloc(sizeof(sf_sample_st) * w->frames) : NULL;
	for (int i = 0; i < w->frames; i++){
		int16_t L = (int16_t)rget16(fp);
		int16_t R = (int16_t)rget16(fp);
		w->s[i].L = L < 0 ? (float)L / 32768.0f : (float)L / 32767.0f;
		w->s[i].R = R < 0 ? (float)R / 32768.0f : (float)R / 32767.0f;
	}
	fclose(fp);
	return 1;
}

// wavcheck <file> <expect_frames> <input_n> <expect_tail>
static int cmd_wavcheck(int argc, char **argv){
	assert(argc == 6);
	const char *file = argv[2];
	int expect = atoi(argv[3]), n = atoi(argv[4]), expecttail = atoi(argv[5]);
	wavdata w;
	assert(wavread(file, &w));
	printf("wavcheck: %s frames=%d expected=%d tailframes=%d expected-tail=%d\n",
		file, w.frames, expect, w.frames - n, expecttail);
	assert(w.frames == expect && w.frames - n == expecttail);

	if (n == 0){
		assert(w.frames == 0); // empty input stays an empty file
		printf("wavcheck: empty input produced empty WAV\n");
		return 0;
	}

	// the final input frame's asymmetric pulse must survive in the final output frame
	sf_sample_st last = w.s[w.frames - 1];
	double ratio = last.L / -last.R;
	printf("wavcheck: last-frame pulse L=%.4f R=%.4f ratio=%.4f expect=2.3333\n",
		last.L, last.R, ratio);
	assert(last.L > 0.1f && last.R < -0.04f && fabs(ratio - (0.70 / 0.30)) < 0.05);

	// for 32-aligned inputs, compare every processed frame against the independent old reference;
	// 16-bit quantization dominates the tolerance
	if (n % SF_COMPRESSOR_SPU == 0){
		sf_sample_st *x = malloc(sizeof(sf_sample_st) * n);
		sf_sample_st *ref = malloc(sizeof(sf_sample_st) * n);
		genpattern(x, n);
		sf_compressor_state_st st;
		oldref o;
		sf_simplecomp(&st, w.rate, 0, -24, 30, 12, 0.003f, 0.25f);
		oldref_init(&o, &st);
		oldref_process(&o, n, x, ref);
		double e = abserr(w.s, ref, n);
		printf("wavcheck: oldref curve over %d frames maxerr=%.3g (16-bit quantized)\n", n, e);
		assert(e < 2e-3);
		free(x); free(ref);
	}
	free(w.s);
	return 0;
}

// latency <rate>: report the CLI compressor's delay using its exact parameters
static int cmd_latency(int argc, char **argv){
	assert(argc == 3);
	sf_compressor_state_st st;
	sf_simplecomp(&st, atoi(argv[2]), 0, -24, 30, 12, 0.003f, 0.25f);
	printf("%d\n", sf_compressor_latency(&st));
	return 0;
}

int main(int argc, char **argv){
	if (argc >= 2 && strcmp(argv[1], "wavgen") == 0)
		return cmd_wavgen(argc, argv);
	if (argc >= 2 && strcmp(argv[1], "wavcheck") == 0)
		return cmd_wavcheck(argc, argv);
	if (argc >= 2 && strcmp(argv[1], "latency") == 0)
		return cmd_latency(argc, argv);
	return run_ctests();
}
