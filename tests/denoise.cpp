// Task tests for the optional multi-channel linked spectral denoiser.
//
// Three groups (no main() here - task_support/driver.c drives these):
//   model     : DenoiseModel / StftPowerAnalyser checked against test-side
//               double-precision formulas and an independent naive DFT.
//               Numeric tolerance: abs 1e-8 + rel 1e-5 for the model EMA/gain
//               formulas; abs 1e-10 + rel 1e-6 for FFT-vs-naive-DFT power.
//   render    : end-to-end SignalsmithStretch::process() with generated noise,
//               narrow-band tones and anti-phase stereo.  Audio assertions use
//               an independent O(n^2) Hann-windowed DFT of the output PCM (not
//               just total energy or the getter): tone bands must retain >= 60%
//               of reference energy, noise bands <= 40%, anti-phase symmetry
//               error <= 2e-3, and 1:1 chunking PCM difference <= 2e-4.
//   lifecycle : seek / flush / reset semantics, default-off bit-exactness, and
//               float/double, 1/2/6-channel and both window/hop configurations.
//
// All sound is generated in C++ (fixed-seed Gaussian noise + sine tones).
//
// How the module enters the existing synthesis path: every analysis hop the
// stretcher's already-computed STFT spectrum is averaged over channels into
// pre-denoise powers P[b] and fed to DenoiseModel (learning only); after the
// phase-vocoder prediction and before synthesis, one shared real gain per
// output bin is multiplied into every channel's output bin.  No extra FFT and
// no separate full-recording wrapper is involved.

#include "signalsmith-stretch.h"
#include "signalsmith-denoise.h"
#include "task_support/check.h"

#include <vector>
#include <complex>
#include <cmath>
#include <string>
#include <algorithm>
#include <random>
#include <cstdint>

using namespace signalsmith::stretch;
using signalsmith::denoise::DenoiseModel;
using signalsmith::denoise::StftPowerAnalyser;

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static constexpr double PI = 3.14159265358979323846;

// Independent naive DFT (test-side reference, double precision).
// If useWindow, the same Hann window the StftPowerAnalyser uses is applied.
static void naiveDftPower(const std::vector<double> &in, std::vector<double> &outPower, bool useWindow) {
	const size_t n = in.size();
	outPower.assign(n/2 + 1, 0.0);
	for (size_t k = 0; k <= n/2; ++k) {
		double re = 0, im = 0;
		for (size_t i = 0; i < n; ++i) {
			double w = useWindow ? (0.5 - 0.5*std::cos(2*PI*i/(n - 1))) : 1.0;
			double phase = 2*PI*k*i/n;
			double v = in[i]*w;
			re += v*std::cos(phase);
			im -= v*std::sin(phase);
		}
		outPower[k] = re*re + im*im;
	}
}

static bool closeAbsRel(double a, double b, double absTol, double relTol) {
	double tol = absTol + relTol*std::max(std::fabs(a), std::fabs(b));
	return std::fabs(a - b) <= tol;
}

// Fixed-seed Gaussian noise (Box-Muller), generated in double then stored float.
static std::vector<std::vector<float>> makeNoise(int channels, int frames, uint64_t seed, double sigma) {
	std::mt19937_64 rng(seed);
	std::normal_distribution<double> dist(0.0, 1.0);
	std::vector<std::vector<float>> x(channels, std::vector<float>(frames));
	for (int i = 0; i < frames; ++i) {
		double common = dist(rng)*sigma;
		for (int c = 0; c < channels; ++c) {
			// Per-channel independent noise, same statistical level.
			x[c][i] = float(dist(rng)*sigma + 0.25*common);
		}
	}
	return x;
}

static void addTone(std::vector<float> &x, double binAt1024, double amp, double phase = 0.0) {
	double f = binAt1024/1024.0; // cycles per sample
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] += float(amp*std::sin(2*PI*f*double(i) + phase));
	}
}

// 1:1 stream runner with a fixed chunk size; output is published at the same
// absolute frame positions for every chunk size.
static void run11(SignalsmithStretch<float> &st, const std::vector<std::vector<float>> &in,
		std::vector<std::vector<float>> &out, int chunk) {
	const int ch = (int)in.size();
	const int n = (int)in[0].size();
	out.assign(ch, std::vector<float>(n, 0.f));
	for (int pos = 0; pos < n; ) {
		int len = std::min(chunk, n - pos);
		std::vector<float *> ip(ch), op(ch);
		for (int c = 0; c < ch; ++c) {
			ip[c] = const_cast<float *>(in[c].data()) + pos;
			op[c] = out[c].data() + pos;
		}
		st.process(ip.data(), len, op.data(), len);
		pos += len;
	}
}

// Hann-windowed DFT energy of one output channel over [start, start+fftSize),
// summed into the supplied accumulator.
static void addDftEnergy(const std::vector<float> &x, int start, int fftSize, std::vector<double> &acc) {
	std::vector<double> frame(fftSize);
	for (int i = 0; i < fftSize; ++i) frame[i] = x[start + i];
	std::vector<double> p;
	naiveDftPower(frame, p, true);
	if (acc.size() != p.size()) acc.assign(p.size(), 0.0);
	for (size_t k = 0; k < p.size(); ++k) acc[k] += p[k];
}

static double bandEnergy(const std::vector<double> &spectrum, int lo, int hi) {
	double e = 0;
	for (int k = lo; k <= hi && k < (int)spectrum.size(); ++k) e += spectrum[k];
	return e;
}

static bool allFinite(const std::vector<std::vector<float>> &x) {
	for (auto &ch : x) for (float v : ch) if (!std::isfinite(v)) return false;
	return true;
}

// Builds a stereo stream: noise-only learning section, then noise + tones.
static void makeStereoStream(std::vector<std::vector<float>> &in, int learnLen, int totalLen, bool antiPhase) {
	in = makeNoise(2, totalLen, 987654321u, 0.05);
	for (int i = learnLen; i < totalLen; ++i) {
		double f1 = 24.0/1024.0, f2 = 29.0/1024.0;
		double tone = 0.30*std::sin(2*PI*f1*i) + 0.12*std::sin(2*PI*f2*i + 0.7);
		in[0][i] += float(tone);
		in[1][i] += float(0.8*tone);
	}
	if (antiPhase) {
		// Re-build as exact mirror images for the anti-phase test.
		in = makeNoise(1, totalLen, 987654321u, 0.05);
		in.resize(2);
		in[1].resize(totalLen);
		for (int i = learnLen; i < totalLen; ++i) {
			double f1 = 24.0/1024.0;
			in[0][i] += float(0.30*std::sin(2*PI*f1*i));
		}
		for (int i = 0; i < totalLen; ++i) in[1][i] = -in[0][i];
	}
}

// ===========================================================================
// model group
// ===========================================================================

// First genuine spectrum seeds N; later spectra use N = a*N + (1-a)*P with
// varying source steps; zero source advance leaves N untouched.
static int model_ema_clock() {
	const int B = 8;
	DenoiseModel<double> m;
	m.configure(B);
	std::vector<double> p(B), nRef(B);
	for (int b = 0; b < B; ++b) p[b] = 1.0 + 0.1*b;

	TASK_CHECK(m.setNoiseLearning(true, 100.0));
	m.latchRequests();
	TASK_CHECK(!m.hasLearned());
	TASK_CHECK(m.noisePowerSpectrum() == nullptr);

	// First genuine spectrum at source position 0: N := P.
	m.observeSpectrum(0.0, true, p.data());
	TASK_CHECK(m.hasLearned());
	const double *n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) {
		TASK_CHECK(closeAbsRel(n[b], p[b], 1e-12, 0.0));
		nRef[b] = p[b];
	}

	// Varying source step d = 30.
	for (int b = 0; b < B; ++b) p[b] = 2.0 + 0.2*b;
	double d = 30.0, a = std::exp(-d/100.0);
	m.observeSpectrum(d, true, p.data());
	n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) {
		nRef[b] = a*nRef[b] + (1 - a)*p[b];
		TASK_CHECK(closeAbsRel(n[b], nRef[b], 1e-8, 1e-5));
	}

	// Zero advance: same source position => a = 1 => N frozen.
	m.observeSpectrum(d, true, p.data());
	n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) TASK_CHECK(closeAbsRel(n[b], nRef[b], 1e-12, 0.0));

	// Another varying step d = 75.
	for (int b = 0; b < B; ++b) p[b] = 0.5 + 0.03*b;
	d = 75.0; a = std::exp(-d/100.0);
	m.observeSpectrum(30.0 + d, true, p.data());
	n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) {
		nRef[b] = a*nRef[b] + (1 - a)*p[b];
		TASK_CHECK(closeAbsRel(n[b], nRef[b], 1e-8, 1e-5));
	}
	return 0;
}

// Toggling learning freezes / resumes N; artificial spectra never learn.
static int model_learning_toggle_and_artificial() {
	const int B = 6;
	DenoiseModel<double> m;
	m.configure(B);
	std::vector<double> p(B);
	for (int b = 0; b < B; ++b) p[b] = 3.0 + b;
	TASK_CHECK(m.setNoiseLearning(true, 50.0));
	m.latchRequests();
	m.observeSpectrum(0.0, true, p.data());
	std::vector<double> frozen(m.noisePowerSpectrum(), m.noisePowerSpectrum() + B);

	// Learning off: N freezes even as real spectra arrive.
	TASK_CHECK(m.setNoiseLearning(false, 50.0));
	m.latchRequests();
	for (int k = 1; k <= 5; ++k) {
		for (int b = 0; b < B; ++b) p[b] = 10.0 + k;
		m.observeSpectrum(double(10*k), true, p.data());
	}
	const double *n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) TASK_CHECK(closeAbsRel(n[b], frozen[b], 1e-12, 0.0));

	// Artificial input while learning would be on: still no learning, no clock move.
	TASK_CHECK(m.setNoiseLearning(true, 50.0));
	m.latchRequests();
	m.setArtificialInput(true);
	for (int b = 0; b < B; ++b) p[b] = 99.0;
	m.observeSpectrum(100.0, true, p.data());
	n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) TASK_CHECK(closeAbsRel(n[b], frozen[b], 1e-12, 0.0));
	m.setArtificialInput(false);

	// Next real spectrum uses d relative to the last *real* position (50),
	// i.e. the artificial jump did not advance the clock.
	for (int b = 0; b < B; ++b) p[b] = 7.0;
	double a = std::exp(-(120.0 - 50.0)/50.0);
	m.observeSpectrum(120.0, true, p.data());
	n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) {
		double ref = a*frozen[b] + (1 - a)*7.0;
		TASK_CHECK(closeAbsRel(n[b], ref, 1e-8, 1e-5));
	}
	return 0;
}

// Genuine silence is represented on the caller's source-position clock: the
// next real analysis after a silent gap passes an absolute position that spans
// the silence, so d includes it.  Artificial input must not move the clock.
static int model_silence_clock() {
	const int B = 4;
	DenoiseModel<double> m;
	m.configure(B);
	std::vector<double> p(B);
	for (int b = 0; b < B; ++b) p[b] = 2.0 + b;
	TASK_CHECK(m.setNoiseLearning(true, 100.0));
	m.latchRequests();
	m.observeSpectrum(0.0, true, p.data()); // seed at position 0
	std::vector<double> seed(m.noisePowerSpectrum(), m.noisePowerSpectrum() + B);

	// A 500-frame gap (which could be genuine silence: the caller's clock keeps
	// running but no spectrum arrives) is reflected in the next absolute position.
	for (int b = 0; b < B; ++b) p[b] = 8.0;
	double a = std::exp(-500.0/100.0);
	m.observeSpectrum(500.0, true, p.data());
	const double *n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) {
		double ref = a*seed[b] + (1 - a)*8.0;
		TASK_CHECK(closeAbsRel(n[b], ref, 1e-8, 1e-5));
	}
	std::vector<double> afterGap(n, n + B);

	// Artificial input must not advance the clock: a later real spectrum at the
	// same position behaves as d = 0 (N frozen).
	m.setArtificialInput(true);
	for (int b = 0; b < B; ++b) p[b] = 50.0;
	m.observeSpectrum(9999.0, true, p.data()); // ignored entirely
	m.setArtificialInput(false);
	m.observeSpectrum(500.0, true, p.data());  // same real position => a = 1
	n = m.noisePowerSpectrum();
	for (int b = 0; b < B; ++b) TASK_CHECK(closeAbsRel(n[b], afterGap[b], 1e-12, 0.0));
	return 0;
}

// Target gain formula, frequency-map interpolation and envelope smoothing.
static int model_gains_mapping_envelope() {
	const int B = 8;
	DenoiseModel<double> m;
	m.configure(B);
	std::vector<double> p(B), n(B);
	for (int b = 0; b < B; ++b) { p[b] = 4.0 + b; n[b] = 1.0 + 0.1*b; }
	TASK_CHECK(m.setNoiseLearning(true, 100.0));
	m.latchRequests();
	m.observeSpectrum(0.0, true, n.data()); // seed N
	TASK_CHECK(m.setNoiseLearning(false, 100.0)); // freeze N so the reference uses the known n
	m.latchRequests();

	const double strength = 2.0, floorGain = 0.1, attack = 128.0, release = 1024.0;
	TASK_CHECK(m.setDenoise(true, strength, floorGain, attack, release));
	m.latchRequests();

	// Before any target update gains start at 1.
	TASK_CHECK(closeAbsRel(m.gains()[0], 1.0, 1e-12, 0.0));

	// Push a new P (the model stores it) and reconstruct the spec targets.
	m.observeSpectrum(10.0, true, p.data());
	m.updateTargets();
	std::vector<double> tSource(B);
	for (int b = 0; b < B; ++b) {
		tSource[b] = std::max(floorGain, std::sqrt(std::max(0.0, 1.0 - strength*n[b]/std::max(p[b], 1e-20))));
	}

	// Fractional mapping with edge clamping, checked by hand.  Converge the
	// one-pole envelope so the gains approach their mapped targets.
	std::vector<double> src(B);
	src[0] = -3.0;            // clamps to band 0
	src[1] = 0.5;             // half way between 0 and 1
	src[2] = 2.25;
	for (int b = 3; b < B; ++b) src[b] = double(B) + 5.0; // clamps to last band
	double hop = 256.0;
	m.mapToOutput(src.data());
	for (int k = 0; k < 400; ++k) m.smoothGains(hop);
	const double *g = m.gains();
	TASK_CHECK(closeAbsRel(g[0], tSource[0], 1e-6, 1e-5));                 // clamped low
	TASK_CHECK(closeAbsRel(g[1], 0.5*(tSource[0] + tSource[1]), 1e-6, 1e-5));
	TASK_CHECK(closeAbsRel(g[2], 0.75*tSource[2] + 0.25*tSource[3], 1e-6, 1e-5));
	for (int b = 3; b < B; ++b) TASK_CHECK(closeAbsRel(g[b], tSource[B - 1], 1e-6, 1e-5)); // clamped high

	// The descending move used the attack pole: one hop gives q*1 + (1-q)*T.
	DenoiseModel<double> atk;
	atk.configure(B);
	TASK_CHECK(atk.setNoiseLearning(true, 100.0));
	atk.latchRequests();
	atk.observeSpectrum(0.0, true, n.data());
	TASK_CHECK(atk.setNoiseLearning(false, 100.0));
	atk.latchRequests();
	TASK_CHECK(atk.setDenoise(true, strength, floorGain, attack, release));
	atk.latchRequests();
	atk.observeSpectrum(10.0, true, p.data());
	std::vector<double> identity(B);
	for (int b = 0; b < B; ++b) identity[b] = double(b);
	atk.runHop(hop, identity.data());
	double qAtk = std::exp(-hop/attack);
	for (int b = 0; b < B; ++b) {
		double expect = qAtk*1.0 + (1 - qAtk)*tSource[b];
		TASK_CHECK(closeAbsRel(atk.gains()[b], expect, 1e-8, 1e-5));
	}

	// Release (upward) pole: disable denoise so the target is unity.
	DenoiseModel<double> rel;
	rel.configure(B);
	TASK_CHECK(rel.setNoiseLearning(true, 100.0));
	rel.latchRequests();
	rel.observeSpectrum(0.0, true, n.data());
	TASK_CHECK(rel.setDenoise(true, strength, floorGain, attack, release));
	rel.latchRequests();
	rel.observeSpectrum(10.0, true, p.data());
	rel.runHop(hop, identity.data()); // descend first
	TASK_CHECK(rel.setDenoise(false, strength, floorGain, attack, release));
	rel.latchRequests();
	double g0 = rel.gains()[0];
	rel.runHop(hop, identity.data()); // now target = 1, rising via release
	double qRel = std::exp(-hop/release);
	TASK_CHECK(closeAbsRel(rel.gains()[0], qRel*g0 + (1 - qRel)*1.0, 1e-8, 1e-5));
	TASK_CHECK(rel.gains()[0] > g0);

	// Before any learning, gains stay at unity.
	DenoiseModel<double> fresh;
	fresh.configure(B);
	TASK_CHECK(fresh.setDenoise(true, 2.0, 0.1, 128.0, 1024.0));
	fresh.latchRequests();
	fresh.runHop(hop, identity.data());
	for (int b = 0; b < B; ++b) TASK_CHECK(closeAbsRel(fresh.gains()[b], 1.0, 1e-12, 0.0));
	return 0;
}

// Parameter validation: illegal values rejected, previous valid settings kept.
static int model_validation() {
	DenoiseModel<double> m;
	m.configure(4);
	TASK_CHECK(m.setDenoise(true, 1.5, 0.2, 256.0, 1024.0));
	TASK_CHECK(m.setNoiseLearning(true, 2048.0));
	m.latchRequests();
	TASK_CHECK(m.params().strength == 1.5);
	TASK_CHECK(m.params().floorGain == 0.2);

	TASK_CHECK(!m.setDenoise(true, -0.01, 0.2, 256.0, 1024.0));
	TASK_CHECK(!m.setDenoise(true, 4.01, 0.2, 256.0, 1024.0));
	TASK_CHECK(!m.setDenoise(true, 1.5, -0.01, 256.0, 1024.0));
	TASK_CHECK(!m.setDenoise(true, 1.5, 1.01, 256.0, 1024.0));
	TASK_CHECK(!m.setDenoise(true, 1.5, 0.2, 0.0, 1024.0));
	TASK_CHECK(!m.setDenoise(true, 1.5, 0.2, -5.0, 1024.0));
	TASK_CHECK(!m.setDenoise(true, 1.5, 0.2, std::numeric_limits<double>::infinity(), 1024.0));
	TASK_CHECK(!m.setDenoise(true, 1.5, 0.2, std::numeric_limits<double>::quiet_NaN(), 1024.0));
	TASK_CHECK(!m.setDenoise(true, 1.5, 0.2, 256.0, 0.0));
	TASK_CHECK(!m.setNoiseLearning(true, 0.0));
	TASK_CHECK(!m.setNoiseLearning(true, -1.0));
	TASK_CHECK(!m.setNoiseLearning(true, std::numeric_limits<double>::quiet_NaN()));

	// Boundary values are accepted.
	TASK_CHECK(m.setDenoise(true, 0.0, 0.0, 1.0, 1.0));
	TASK_CHECK(m.setDenoise(true, 4.0, 1.0, 1.0, 1.0));
	TASK_CHECK(m.setNoiseLearning(true, 1.0));

	// Rejected calls kept the previous *pending* request untouched; latch and
	// confirm the last accepted boundary values are what becomes active.
	m.latchRequests();
	TASK_CHECK(m.params().strength == 4.0);
	TASK_CHECK(m.params().floorGain == 1.0);
	return 0;
}

// Standalone STFT stage agrees with an independent naive DFT (same Hann window).
static int model_stft_vs_dft() {
	const size_t n = 256;
	StftPowerAnalyser<double> analyser;
	analyser.configure(n);
	TASK_CHECK(analyser.bins() == n/2 + 1);

	std::vector<double> frame(n), ref(n);
	for (size_t i = 0; i < n; ++i) {
		frame[i] = 0.5*std::sin(2*PI*7*i/n) + 0.25*std::cos(2*PI*13*i/n + 0.3);
	}
	std::vector<double> power(n/2 + 1);
	analyser.analyse(frame.data(), power.data());
	naiveDftPower(frame, ref, true);
	// Tolerance: 1e-10 absolute + 1e-6 relative (double precision FFT vs O(n^2) DFT).
	for (size_t k = 0; k < power.size(); ++k) {
		TASK_CHECK(closeAbsRel(power[k], ref[k], 1e-10, 1e-6));
	}

	// The same stage works in float, and stays finite.
	StftPowerAnalyser<float> fAnalyser;
	fAnalyser.configure(128);
	std::vector<float> fFrame(128);
	std::vector<float> fPower(fAnalyser.bins());
	for (size_t i = 0; i < 128; ++i) fFrame[i] = float(std::sin(2*PI*5*i/128));
	fAnalyser.analyse(fFrame.data(), fPower.data());
	for (float v : fPower) TASK_CHECK(std::isfinite(v));
	return 0;
}

// ===========================================================================
// render group
// ===========================================================================

// Noise-only learning then noise+tone denoising: noise bands attenuated, the
// louder narrow-band "speech" bins preserved, checked with an output DFT.
static int render_noise_attenuation_and_tone_preservation() {
	const int total = 16384, learn = 8192;
	std::vector<std::vector<float>> in;
	makeStereoStream(in, learn, total, false);

	SignalsmithStretch<float> ref(4242), den(4242);
	ref.configure(2, 1024, 256, false);
	den.configure(2, 1024, 256, false);
	TASK_CHECK(den.setNoiseLearning(true, 1024.0f));
	TASK_CHECK(den.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));

	std::vector<std::vector<float>> outRef, outDen;
	run11(ref, in, outRef, 257);
	// Learning section first (denoise already on, but nothing is learned yet so
	// gains are unity), then keep learning on through the noise-only section.
	{
		std::vector<std::vector<float>> learnIn(2), drop(2);
		for (int c = 0; c < 2; ++c) learnIn[c].assign(in[c].begin(), in[c].begin() + learn);
		run11(den, learnIn, drop, 257);
		TASK_CHECK(den.noisePowerSpectrum() != nullptr);
		TASK_CHECK(den.noiseBandCount() == 512);
		// Freeze the model for the tone section.
		TASK_CHECK(den.setNoiseLearning(false, 1024.0f));
		std::vector<std::vector<float>> rest(2), restOut(2);
		for (int c = 0; c < 2; ++c) rest[c].assign(in[c].begin() + learn, in[c].end());
		run11(den, rest, restOut, 257);
		outDen = restOut; // steady-state portion only
	}

	// Independent DFT over four steady-state 1024-frame blocks.
	std::vector<double> eRef, eDen;
	for (int start = 1024; start + 1024 <= (int)outRef[0].size(); start += 1024) {
		if (start < learn) continue;
		int rel = start - learn;
		if (rel + 1024 > (int)outDen[0].size()) break;
		for (int c = 0; c < 2; ++c) {
			addDftEnergy(outRef[c], start, 1024, eRef);
			addDftEnergy(outDen[c], rel, 1024, eDen);
		}
	}
	TASK_CHECK(eRef.size() == 513 && eDen.size() == 513);

	double toneRef = bandEnergy(eRef, 22, 30), toneDen = bandEnergy(eDen, 22, 30);
	double noiseRef = bandEnergy(eRef, 64, 192), noiseDen = bandEnergy(eDen, 64, 192);
	TASK_CHECK(toneRef > 1.0 && noiseRef > 1.0);
	// Strong tone band retained (>= 65% of its reference energy).
	TASK_CHECK(toneDen/toneRef >= 0.65);
	// Noise floor pulled down (power ratio well below unity; floor target is 0.01).
	TASK_CHECK(noiseDen/noiseRef <= 0.4);
	return 0;
}

// Anti-phase stereo: linked per-band real gain must preserve L == -R.
static int render_antiphase_preserved() {
	const int total = 12288, learn = 6144;
	std::vector<std::vector<float>> in;
	makeStereoStream(in, learn, total, true);

	SignalsmithStretch<float> den(777);
	den.configure(2, 1024, 256, false);
	TASK_CHECK(den.setNoiseLearning(true, 1024.0f));
	TASK_CHECK(den.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));
	std::vector<std::vector<float>> out;
	run11(den, in, out, 251);
	TASK_CHECK(allFinite(out));

	double symErr = 0, energy = 0;
	for (int i = 4096; i < (int)out[0].size(); ++i) {
		symErr += std::fabs(double(out[0][i]) + double(out[1][i]));
		energy += std::fabs(double(out[0][i])) + std::fabs(double(out[1][i]));
	}
	TASK_CHECK(energy > 1.0);
	TASK_CHECK(symErr/energy <= 2e-3); // float-level linearity, same gain on both channels
	return 0;
}

// 1:1 processing must be chunking-independent at fixed absolute frame positions.
static int render_chunk_equivalence_1to1() {
	const int total = 6000;
	auto in = makeNoise(2, total, 555u, 0.05);
	for (auto &ch : in) addTone(ch, 24.0, 0.2);

	auto build = [&](uint64_t seed) -> SignalsmithStretch<float> {
		SignalsmithStretch<float> st{long(seed)};
		st.configure(2, 1024, 256, false);
		TASK_CHECK(st.setNoiseLearning(true, 512.0f));
		TASK_CHECK(st.setDenoise(true, 1.5f, 0.15f, 200.0f, 800.0f));
		return st;
	};
	std::vector<std::vector<float>> out1, out7, out113;
	{ auto st = build(31337); run11(st, in, out1, 1); }
	{ auto st = build(31337); run11(st, in, out7, 7); }
	{ auto st = build(31337); run11(st, in, out113, 113); }
	TASK_CHECK(allFinite(out1) && allFinite(out7) && allFinite(out113));

	double max17 = 0, max1113 = 0;
	for (int c = 0; c < 2; ++c) {
		for (int i = 0; i < total; ++i) {
			max17 = std::max(max17, double(std::fabs(out1[c][i] - out7[c][i])));
			max1113 = std::max(max1113, double(std::fabs(out1[c][i] - out113[c][i])));
		}
	}
	TASK_CHECK(max17 <= 2e-4);
	TASK_CHECK(max1113 <= 2e-4);
	return 0;
}

// Pitch shift maps source bands through the frequency map; formant handling
// still runs; tones land at the shifted bins and noise stays attenuated.
static int render_pitch_formant_mapping() {
	const int total = 12288, learn = 6144;
	// The tone only starts after the noise-only learning section, so the learned
	// model contains pure noise and cannot mistake the tone for noise.
	auto makeStream = [&](uint64_t seed, double toneBin) {
		auto x = makeNoise(1, total, seed, 0.05);
		double f = toneBin/1024.0;
		for (int i = learn; i < total; ++i) x[0][i] += float(0.3*std::sin(2*PI*f*i));
		return x;
	};

	auto makeSt = [&](uint64_t seed, double transpose) -> SignalsmithStretch<float> {
		SignalsmithStretch<float> st{long(seed)};
		st.configure(1, 1024, 256, false);
		st.setTransposeFactor(float(transpose));
		st.setFormantFactor(1.2f, true);
		return st;
	};

	// Two-phase 1:1 run: learn on during the noise-only section, frozen during
	// the tone section so the model never absorbs the tone.
	auto runTwoPhase = [&](SignalsmithStretch<float> &st, const std::vector<std::vector<float>> &stream,
			std::vector<std::vector<float>> &out) {
		out.assign(1, std::vector<float>(total, 0.f));
		for (int phase = 0; phase < 2; ++phase) {
			int start = phase ? learn : 0, end = phase ? total : learn;
			std::vector<float> oneIn(stream[0].begin() + start, stream[0].begin() + end);
			std::vector<float> oneOut(end - start, 0.f);
			for (int pos = 0; pos < end - start; ) {
				int len = std::min(256, end - start - pos);
				float *ip = oneIn.data() + pos, *op = oneOut.data() + pos;
				st.process(&ip, len, &op, len);
				pos += len;
			}
			for (int i = 0; i < end - start; ++i) out[0][start + i] = oneOut[i];
		}
	};

	// Upward shift: source bin 24 lands near output bin 36.
	{
		auto in = makeStream(246810u, 24.0);
		SignalsmithStretch<float> ref = makeSt(123, 1.5);
		std::vector<std::vector<float>> oRef;
		runTwoPhase(ref, in, oRef);

		SignalsmithStretch<float> den = makeSt(123, 1.5);
		TASK_CHECK(den.setNoiseLearning(true, 1024.0f));
		TASK_CHECK(den.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));
		std::vector<std::vector<float>> oDen;
		{
			oDen.assign(1, std::vector<float>(total, 0.f));
			std::vector<float> learnIn(in[0].begin(), in[0].begin() + learn), learnOut(learn, 0.f);
			for (int pos = 0; pos < learn; ) {
				int len = std::min(256, learn - pos);
				float *ip = learnIn.data() + pos, *op = learnOut.data() + pos;
				den.process(&ip, len, &op, len);
				pos += len;
			}
			TASK_CHECK(den.setNoiseLearning(false, 1024.0f)); // freeze before the tone section
			std::vector<float> toneIn(in[0].begin() + learn, in[0].end()), toneOut(total - learn, 0.f);
			for (int pos = 0; pos < total - learn; ) {
				int len = std::min(256, total - learn - pos);
				float *ip = toneIn.data() + pos, *op = toneOut.data() + pos;
				den.process(&ip, len, &op, len);
				pos += len;
			}
			for (int i = 0; i < total - learn; ++i) oDen[0][learn + i] = toneOut[i];
		}
		TASK_CHECK(allFinite(oRef) && allFinite(oDen));
		TASK_CHECK(den.noisePowerSpectrum() != nullptr);

		std::vector<double> eRef, eDen;
		for (int start = 8192; start + 1024 <= total; start += 1024) {
			addDftEnergy(oRef[0], start, 1024, eRef);
			addDftEnergy(oDen[0], start, 1024, eDen);
		}
		double shiftedRef = bandEnergy(eRef, 33, 40), shiftedDen = bandEnergy(eDen, 33, 40);
		double noiseRef = bandEnergy(eRef, 80, 220), noiseDen = bandEnergy(eDen, 80, 220);
		TASK_CHECK(shiftedRef > 0.5);
		TASK_CHECK(shiftedDen/shiftedRef >= 0.6);
		TASK_CHECK(noiseDen/noiseRef <= 0.4);
	}

	// Downward shift: source bin 40 lands near output bin 20.
	{
		auto in2 = makeStream(246810u, 40.0);
		SignalsmithStretch<float> ref2 = makeSt(456, 0.5);
		std::vector<std::vector<float>> r2;
		runTwoPhase(ref2, in2, r2);

		SignalsmithStretch<float> den2 = makeSt(456, 0.5);
		TASK_CHECK(den2.setNoiseLearning(true, 1024.0f));
		TASK_CHECK(den2.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));
		std::vector<std::vector<float>> d2;
		{
			d2.assign(1, std::vector<float>(total, 0.f));
			std::vector<float> learnIn(in2[0].begin(), in2[0].begin() + learn), learnOut(learn, 0.f);
			for (int pos = 0; pos < learn; ) {
				int len = std::min(256, learn - pos);
				float *ip = learnIn.data() + pos, *op = learnOut.data() + pos;
				den2.process(&ip, len, &op, len);
				pos += len;
			}
			TASK_CHECK(den2.setNoiseLearning(false, 1024.0f));
			std::vector<float> toneIn(in2[0].begin() + learn, in2[0].end()), toneOut(total - learn, 0.f);
			for (int pos = 0; pos < total - learn; ) {
				int len = std::min(256, total - learn - pos);
				float *ip = toneIn.data() + pos, *op = toneOut.data() + pos;
				den2.process(&ip, len, &op, len);
				pos += len;
			}
			for (int i = 0; i < total - learn; ++i) d2[0][learn + i] = toneOut[i];
		}
		std::vector<double> er2, ed2;
		for (int start = 8192; start + 1024 <= total; start += 1024) {
			addDftEnergy(r2[0], start, 1024, er2);
			addDftEnergy(d2[0], start, 1024, ed2);
		}
		TASK_CHECK(bandEnergy(ed2, 17, 24)/std::max(bandEnergy(er2, 17, 24), 1e-12) >= 0.6);
	}
	return 0;
}

// Non-1:1 time stretch (0.5x and 2x): model still learns and denoises, and the
// frozen model stays frozen across the run.
static int render_time_stretch_clock() {
	const int inLen = 8192;
	auto in = makeNoise(1, inLen, 13579u, 0.05);
	addTone(in[0], 24.0, 0.25);

	for (double ratio : {0.5, 2.0}) {
		int outLen = int(inLen/ratio);
		SignalsmithStretch<float> ref(long(99)), den(long(99));
		ref.configure(1, 1024, 256, false);
		den.configure(1, 1024, 256, false);
		den.setTransposeFactor(1.0f);
		TASK_CHECK(den.setNoiseLearning(true, 1024.0f));
		TASK_CHECK(den.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));

		std::vector<float> oRef(outLen, 0.f), oDen(outLen, 0.f);
		const int chunk = 512;
		for (int op = 0; op < outLen; ) {
			int oLen = std::min(chunk, outLen - op);
			int iLen = std::min(inLen, int(std::lround(oLen*ratio)));
			int iPos = std::min(inLen - 1, std::max(0, int(std::lround(op*ratio))));
			iLen = std::min(iLen, inLen - iPos);
			float *ip = in[0].data() + iPos;
			float *rp = oRef.data() + op, *dp = oDen.data() + op;
			ref.process(&ip, iLen, &rp, oLen);
			den.process(&ip, iLen, &dp, oLen);
			op += oLen;
		}
		TASK_CHECK(std::all_of(oDen.begin(), oDen.end(), [](float v){ return std::isfinite(v); }));
		TASK_CHECK(den.noisePowerSpectrum() != nullptr);

		// Freeze learning: the learned spectrum must not move on further processing.
		std::vector<double> frozen(den.noisePowerSpectrum(), den.noisePowerSpectrum() + den.noiseBandCount());
		TASK_CHECK(den.setNoiseLearning(false, 1024.0f));
		{
			int extra = 1024;
			std::vector<float> xIn(extra, 0.05f), xOut(extra, 0.f);
			for (int i = 0; i < extra; ++i) xIn[i] = float(0.05*std::sin(0.07*i));
			float *ip = xIn.data(), *op = xOut.data();
			den.process(&ip, extra, &op, extra);
			const float *after = den.noisePowerSpectrum();
			for (int b = 0; b < den.noiseBandCount(); ++b) {
				TASK_CHECK(closeAbsRel(after[b], frozen[b], 1e-12, 0.0));
			}
		}
	}
	return 0;
}

// Split computation: parameters changed halfway through a run latch at hop
// boundaries; output stays finite and the new floor gain takes effect.
static int render_split_halfhop_param_change() {
	const int total = 12288, learn = 6144;
	auto in = makeNoise(2, total, 86420u, 0.05);
	SignalsmithStretch<float> st(long(2024));
	st.configure(2, 1024, 256, true); // split computation, one extra interval latency
	TASK_CHECK(st.setNoiseLearning(true, 512.0f));
	TASK_CHECK(st.setDenoise(true, 1.0f, 0.5f, 256.0f, 1024.0f));
	std::vector<std::vector<float>> out;
	// Drive in small chunks and change settings mid-stream.
	out.assign(2, std::vector<float>(total, 0.f));
	int pos = 0;
	bool switched = false;
	while (pos < total) {
		int len = std::min(113, total - pos);
		std::vector<float *> ip(2), op(2);
		for (int c = 0; c < 2; ++c) { ip[c] = in[c].data() + pos; op[c] = out[c].data() + pos; }
		st.process(ip.data(), len, op.data(), len);
		pos += len;
		if (!switched && pos >= learn) {
			// Rejected request must not disturb the running hop.
			TASK_CHECK(!st.setDenoise(true, 5.0f, 0.1f, 256.0f, 1024.0f));
			TASK_CHECK(st.setDenoise(true, 2.5f, 0.05f, 64.0f, 512.0f));
			TASK_CHECK(st.setNoiseLearning(false, 512.0f));
			switched = true;
		}
	}
	TASK_CHECK(switched);
	TASK_CHECK(allFinite(out));
	// After switching to a deeper floor, steady-state noise energy is low.
	std::vector<double> e;
	for (int start = total - 4096; start + 1024 <= total; start += 1024) {
		for (int c = 0; c < 2; ++c) addDftEnergy(out[c], start, 1024, e);
	}
	double mean = bandEnergy(e, 64, 200)/double(std::max(1, (200 - 64 + 1)*4));
	TASK_CHECK(mean < 0.05);
	return 0;
}

// Long genuine silence (exercises the fast path) then signal recovery: the
// envelope and clock keep advancing, the model is not corrupted by silence.
static int render_long_silence_recovery() {
	const int learn = 4096, silence = 4096, tail = 4096;
	const int total = learn + silence + tail;
	auto in = makeNoise(1, total, 31415u, 0.05);
	for (int i = learn; i < learn + silence; ++i) in[0][i] = 0.f;
	for (int i = learn + silence; i < total; ++i) {
		in[0][i] += float(0.3*std::sin(2*PI*(24.0/1024.0)*i));
	}

	SignalsmithStretch<float> st(long(2718));
	st.configure(1, 1024, 256, false);
	TASK_CHECK(st.setNoiseLearning(true, 512.0f));
	TASK_CHECK(st.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));
	std::vector<std::vector<float>> out;
	run11(st, in, out, 127);
	TASK_CHECK(allFinite(out));

	// Learning off before the silence: the frozen model must survive silence.
	// (Re-run with learning disabled after the learning section.)
	SignalsmithStretch<float> st2(long(2718));
	st2.configure(1, 1024, 256, false);
	TASK_CHECK(st2.setNoiseLearning(true, 512.0f));
	TASK_CHECK(st2.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));
	std::vector<std::vector<float>> tmp;
	std::vector<std::vector<float>> learnIn(1);
	learnIn[0].assign(in[0].begin(), in[0].begin() + learn);
	run11(st2, learnIn, tmp, 127);
	std::vector<double> modelBefore(st2.noisePowerSpectrum(), st2.noisePowerSpectrum() + st2.noiseBandCount());
	TASK_CHECK(st2.setNoiseLearning(false, 512.0f));
	std::vector<std::vector<float>> rest(1), restOut;
	rest[0].assign(in[0].begin() + learn, in[0].end());
	run11(st2, rest, restOut, 127);
	const float *modelAfter = st2.noisePowerSpectrum();
	for (int b = 0; b < st2.noiseBandCount(); ++b) {
		TASK_CHECK(closeAbsRel(modelAfter[b], modelBefore[b], 1e-12, 0.0));
	}
	// Tone after the silence is still present in the output.
	std::vector<double> e;
	for (int start = silence + 1024; start + 1024 <= (int)restOut[0].size(); start += 1024) {
		addDftEnergy(restOut[0], start, 1024, e);
	}
	TASK_CHECK(bandEnergy(e, 22, 30) > 0.5);
	return 0;
}

// ===========================================================================
// lifecycle group
// ===========================================================================

// seek() keeps the learned model, resets the envelope, re-anchors the clock.
static int lifecycle_seek() {
	const int learn = 4096;
	auto in = makeNoise(1, learn + 2048, 161803u, 0.05);
	SignalsmithStretch<float> st(long(161));
	st.configure(1, 1024, 256, false);
	TASK_CHECK(st.setNoiseLearning(true, 512.0f));
	TASK_CHECK(st.setDenoise(true, 2.0f, 0.1f, 4096.0f, 4096.0f)); // slow attack
	std::vector<std::vector<float>> tmp;
	std::vector<std::vector<float>> learnIn(1);
	learnIn[0].assign(in[0].begin(), in[0].begin() + learn);
	run11(st, learnIn, tmp, 256);
	TASK_CHECK(st.noisePowerSpectrum() != nullptr);
	std::vector<double> modelBefore(st.noisePowerSpectrum(), st.noisePowerSpectrum() + st.noiseBandCount());

	// Provide seekLength() frames of pre-roll.
	int seekLen = st.seekLength();
	std::vector<float> pre(in[0].begin(), in[0].begin() + seekLen);
	float *pp = pre.data();
	st.seek(&pp, seekLen, 1.0);

	// Model retained bit-for-bit; envelope reset to unity.
	const float *modelAfter = st.noisePowerSpectrum();
	for (int b = 0; b < st.noiseBandCount(); ++b) {
		TASK_CHECK(closeAbsRel(modelAfter[b], modelBefore[b], 1e-12, 0.0));
	}

	// First steady output block after the seek starts from unity gain (slow
	// attack means the very first hop is still close to full level), then decays.
	std::vector<float> tail(2048, 0.f);
	SignalsmithStretch<float> ref(long(161));
	ref.configure(1, 1024, 256, false);
	std::vector<float> refTail(2048, 0.f);
	std::vector<float> tailIn(in[0].begin() + learn, in[0].begin() + learn + 2048);
	float *tip = tailIn.data(), *top = tail.data(), *rop = refTail.data();
	st.process(&tip, 2048, &top, 2048);
	ref.process(&tip, 2048, &rop, 2048);
	double eEarlyDen = 0, eEarlyRef = 0, eLateDen = 0, eLateRef = 0;
	for (int i = 768; i < 1024; ++i) { eEarlyDen += tail[i]*tail[i]; eEarlyRef += refTail[i]*refTail[i]; }
	for (int i = 1536; i < 1792; ++i) { eLateDen += tail[i]*tail[i]; eLateRef += refTail[i]*refTail[i]; }
	TASK_CHECK(eEarlyRef > 0 && eLateRef > 0);
	TASK_CHECK(eEarlyDen/eEarlyRef > 0.75);  // envelope began at 1
	TASK_CHECK(eLateDen/eLateRef < 0.8);     // and subsequently attenuates
	return 0;
}

// flush() drains the tail with the existing model and never learns zeros.
static int lifecycle_flush() {
	auto in = makeNoise(1, 6144, 271828u, 0.05);
	SignalsmithStretch<float> st(long(818));
	st.configure(1, 1024, 256, false);
	TASK_CHECK(st.setNoiseLearning(true, 512.0f));
	TASK_CHECK(st.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));
	std::vector<std::vector<float>> tmp;
	run11(st, in, tmp, 256);
	std::vector<double> modelBefore(st.noisePowerSpectrum(), st.noisePowerSpectrum() + st.noiseBandCount());

	std::vector<float> tail(1024, 0.f);
	float *top = tail.data();
	st.flush(&top, 1024, 1.0);
	TASK_CHECK(std::all_of(tail.begin(), tail.end(), [](float v){ return std::isfinite(v); }));

	// Zero padding did not become the noise model.
	const float *modelAfter = st.noisePowerSpectrum();
	TASK_CHECK(modelAfter != nullptr);
	for (int b = 0; b < st.noiseBandCount(); ++b) {
		TASK_CHECK(closeAbsRel(modelAfter[b], modelBefore[b], 1e-12, 0.0));
	}
	return 0;
}

// reset() clears model/envelope/history but retains the parameter requests.
static int lifecycle_reset() {
	auto in = makeNoise(1, 4096, 141421u, 0.05);
	SignalsmithStretch<float> st(long(909));
	st.configure(1, 1024, 256, false);
	TASK_CHECK(st.setNoiseLearning(true, 512.0f));
	TASK_CHECK(st.setDenoise(true, 2.0f, 0.1f, 128.0f, 1024.0f));
	std::vector<std::vector<float>> tmp;
	run11(st, in, tmp, 256);
	TASK_CHECK(st.noisePowerSpectrum() != nullptr);

	st.reset();
	TASK_CHECK(st.noisePowerSpectrum() == nullptr);
	TASK_CHECK(st.noiseBandCount() == 512);

	// Parameters retained: re-learning works without re-issuing setDenoise.
	std::vector<std::vector<float>> out;
	run11(st, in, out, 256);
	TASK_CHECK(st.noisePowerSpectrum() != nullptr);
	TASK_CHECK(allFinite(out));

	// Invalid requests are still rejected after reset (validators retained).
	TASK_CHECK(!st.setDenoise(true, 9.0f, 0.1f, 128.0f, 1024.0f));
	TASK_CHECK(!st.setNoiseLearning(true, -1.0f));
	return 0;
}

// Never enabled: identical sound to a second default instance and unchanged latency.
static int lifecycle_default_off_bitexact() {
	auto in = makeNoise(2, 8192, 577215u, 0.05);
	addTone(in[0], 24.0, 0.2);
	addTone(in[1], 29.0, 0.15);

	SignalsmithStretch<float> a(long(11)), b(long(11));
	a.configure(2, 1024, 256, false);
	b.configure(2, 1024, 256, false);
	// Neither instance ever calls setDenoise/setNoiseLearning.
	TASK_CHECK(a.noisePowerSpectrum() == nullptr && b.noisePowerSpectrum() == nullptr);
	std::vector<std::vector<float>> oa, ob;
	run11(a, in, oa, 127);
	run11(b, in, ob, 256); // different chunking, default path stays deterministic
	double maxDiff = 0;
	for (int c = 0; c < 2; ++c)
		for (int i = 0; i < (int)oa[0].size(); ++i)
			maxDiff = std::max(maxDiff, double(std::fabs(oa[c][i] - ob[c][i])));
	TASK_CHECK(maxDiff == 0.0); // bit-identical when the denoiser is off
	TASK_CHECK(a.inputLatency() == 512 && a.outputLatency() == 512);
	TASK_CHECK(allFinite(oa));
	return 0;
}

// Coverage for float/double, 1/2/6 channels and 2048/512 windows.
static int lifecycle_configs_and_types() {
	{
		// 6 channels, 2048/512, float.
		auto in = makeNoise(6, 4096, 424242u, 0.04);
		SignalsmithStretch<float> st(long(606));
		st.configure(6, 2048, 512, false);
		TASK_CHECK(st.noiseBandCount() == 1024);
		TASK_CHECK(st.setNoiseLearning(true, 1024.0f));
		TASK_CHECK(st.setDenoise(true, 1.5f, 0.1f, 256.0f, 1024.0f));
		std::vector<std::vector<float>> out;
		out.assign(6, std::vector<float>(4096, 0.f));
		for (int pos = 0; pos < 4096; ) {
			int len = std::min(512, 4096 - pos);
			std::vector<float *> ip(6), op(6);
			for (int c = 0; c < 6; ++c) { ip[c] = in[c].data() + pos; op[c] = out[c].data() + pos; }
			st.process(ip.data(), len, op.data(), len);
			pos += len;
		}
		TASK_CHECK(allFinite(out));
		TASK_CHECK(st.noisePowerSpectrum() != nullptr);
	}
	{
		// double precision, mono, 1024/256, split computation.
		const int n = 3000;
		std::vector<std::vector<double>> in(1, std::vector<double>(n));
		std::mt19937_64 rng(1234);
		std::normal_distribution<double> d(0.0, 0.05);
		for (int i = 0; i < n; ++i) in[0][i] = d(rng);
		SignalsmithStretch<double> st(long(707));
		st.configure(1, 1024, 256, true);
		TASK_CHECK(st.setNoiseLearning(true, 512.0));
		TASK_CHECK(st.setDenoise(true, 1.5, 0.1, 256.0, 1024.0));
		std::vector<std::vector<double>> out(1, std::vector<double>(n, 0.0));
		for (int pos = 0; pos < n; ) {
			int len = std::min(251, n - pos);
			double *ip = in[0].data() + pos, *op = out[0].data() + pos;
			st.process(&ip, len, &op, len);
			pos += len;
		}
		for (double v : out[0]) TASK_CHECK(std::isfinite(v));
		TASK_CHECK(st.noisePowerSpectrum() != nullptr);
	}
	return 0;
}

} // namespace

const struct task_case task_cases[] = {
	{"model", "ema_clock", model_ema_clock},
	{"model", "learning_toggle_and_artificial", model_learning_toggle_and_artificial},
	{"model", "silence_clock", model_silence_clock},
	{"model", "gains_mapping_envelope", model_gains_mapping_envelope},
	{"model", "validation", model_validation},
	{"model", "stft_vs_dft", model_stft_vs_dft},

	{"render", "noise_attenuation_and_tone_preservation", render_noise_attenuation_and_tone_preservation},
	{"render", "antiphase_preserved", render_antiphase_preserved},
	{"render", "chunk_equivalence_1to1", render_chunk_equivalence_1to1},
	{"render", "pitch_formant_mapping", render_pitch_formant_mapping},
	{"render", "time_stretch_clock", render_time_stretch_clock},
	{"render", "split_halfhop_param_change", render_split_halfhop_param_change},
	{"render", "long_silence_recovery", render_long_silence_recovery},

	{"lifecycle", "seek", lifecycle_seek},
	{"lifecycle", "flush", lifecycle_flush},
	{"lifecycle", "reset", lifecycle_reset},
	{"lifecycle", "default_off_bitexact", lifecycle_default_off_bitexact},
	{"lifecycle", "configs_and_types", lifecycle_configs_and_types},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
