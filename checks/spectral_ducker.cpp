// Independent regression tests for signalsmith::linear::SpectralDucker.
//
// The sidechain amplitudes are checked against a from-scratch O(N^2) DFT (not the
// library FFT), and the gain recursion against an independent scalar implementation.
#include "spectral-ducker.h"
#include "include/signalsmith-linear/spectral-ducker.h" // forwarding header (include-guarded)

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <complex>
#include <vector>
#include <algorithm>
#include <string>
#include <limits>
#include <new>

// ---- Heap allocation accounting ------------------------------------------------
static long heapAllocCount = 0, heapFreeCount = 0;
void *operator new(std::size_t n) {
	++heapAllocCount;
	void *p = std::malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	return p;
}
void *operator new[](std::size_t n) {
	++heapAllocCount;
	void *p = std::malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	return p;
}
void operator delete(void *p) noexcept { if (p) { ++heapFreeCount; std::free(p); } }
void operator delete[](void *p) noexcept { if (p) { ++heapFreeCount; std::free(p); } }

static int failures = 0;
static void check(bool ok, const std::string &message) {
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", message.c_str());
	}
}

template<typename S>
static bool closeTo(S value, double reference) {
	double tol = std::is_same<S, float>::value
		? (1e-5 + 1e-4*std::fabs(reference))
		: (1e-10 + 1e-8*std::fabs(reference));
	return std::fabs(double(value) - reference) <= tol;
}

// ---- Independent reference: naive DFT + windowed analysis frame ----------------
template<typename S>
struct ReferenceAnalysis {
	size_t N, hop;
	std::vector<S> window;
	double windowSum = 0;

	ReferenceAnalysis(size_t frameSize, size_t hopSize) : N(frameSize), hop(hopSize) {
		// Obtain the same default symmetric ACG window the ducker uses.
		signalsmith::linear::DynamicSTFT<S> windowSource;
		windowSource.configure(1, 0, N, 0, hop);
		window.resize(N);
		const S *w = windowSource.analysisWindow();
		for (size_t i = 0; i < N; ++i) {
			window[i] = w[i];
			windowSum += double(w[i]);
		}
	}

	// Frame f covers input times [(f+1)*hop - N, (f+1)*hop), zero-padded before 0.
	std::complex<double> dftBin(const std::vector<S> &x, size_t frame, size_t k) const {
		long frameStart = long((frame + 1)*hop) - long(N);
		std::complex<double> sum(0, 0);
		for (size_t i = 0; i < N; ++i) {
			long n = frameStart + long(i);
			double v = (n >= 0 && n < long(x.size())) ? double(x[size_t(n)]) : 0.0;
			double phase = -2*M_PI*double(k)*double(i)/double(N);
			sum += v*double(window[i])*std::complex<double>(std::cos(phase), std::sin(phase));
		}
		return sum;
	}
	double amplitude(const std::vector<S> &x, size_t frame, size_t k) const {
		double mag = std::abs(dftBin(x, frame, k));
		return (k == 0 || k == N/2) ? mag/windowSum : 2*mag/windowSum;
	}
};

static double envelopeCoeff(double hopSeconds, double timeSeconds) {
	return timeSeconds == 0.0 ? 0.0 : std::exp(-hopSeconds/timeSeconds);
}
static double targetGain(double amplitude, double threshold, double ratio) {
	if (!(amplitude > threshold)) return 1.0;
	return std::pow(threshold/amplitude, 1.0 - 1.0/ratio);
}

// Single-bin magnitude of a segment using the same analysis window as the STFT,
// so a bin measurement matches what the STFT processing actually acts on.
static double windowedBinMagnitude(const std::vector<double> &y, size_t start, size_t N, size_t hop, size_t k) {
	signalsmith::linear::DynamicSTFT<double> windowSource;
	windowSource.configure(1, 0, N, 0, hop);
	const double *w = windowSource.analysisWindow();
	double re = 0, im = 0;
	for (size_t i = 0; i < N; ++i) {
		double phase = -2*M_PI*double(k)*double(i)/double(N);
		double v = y[start + i]*w[i];
		re += v*std::cos(phase);
		im += v*std::sin(phase);
	}
	return std::sqrt(re*re + im*im);
}

// ---- 1. Amplitude calibration (library STFT vs naive DFT) ----------------------
template<typename S>
static double testAmplitudeCalibration(size_t N, size_t hop, double sampleRate,
		const std::vector<S> &sidechain, size_t frames) {
	signalsmith::linear::DynamicSTFT<S> stft;
	stft.configure(1, 0, N, 0, hop);
	stft.setNormalisation(signalsmith::linear::DynamicSTFT<S>::normRoundTrip);
	double windowSum = 0;
	const S *w = stft.analysisWindow();
	for (size_t i = 0; i < N; ++i) windowSum += double(w[i]);

	ReferenceAnalysis<S> ref(N, hop);
	double maxError = 0;
	for (size_t f = 0; f < frames; ++f) {
		for (size_t c = 0; c < 1; ++c) {
			stft.writeInput(c, hop, sidechain.data() + f*hop);
		}
		stft.moveInput(hop);
		stft.analyse();
		const std::complex<S> *spectrum = stft.spectrum(0);

		for (size_t k = 0; k <= N/2; ++k) {
			double refA = ref.amplitude(sidechain, f, k);
			double measured;
			if (k == 0) {
				measured = double(std::abs(spectrum[0].real()))/windowSum; // packed DC
			} else if (k == N/2) {
				measured = double(std::abs(spectrum[0].imag()))/windowSum; // packed Nyquist
			} else {
				measured = 2*double(std::abs(spectrum[k]))/windowSum;
			}
			maxError = std::max(maxError, std::fabs(measured - refA));
		}
	}
	(void)sampleRate;
	return maxError;
}

// ---- 2. Gain recursion vs independent scalar reference -------------------------
struct EnvelopeParams {
	double threshold, ratio, attack, release;
};

template<typename S>
static double testEnvelopeRecursion(size_t N, size_t hop, double sampleRate,
		const std::vector<S> &sidechain, size_t frames,
		const std::vector<std::pair<size_t, EnvelopeParams>> &schedule) {
	signalsmith::linear::SpectralDucker<S> duck;
	check(duck.prepare(sampleRate, N, hop), "prepare() in envelope test");
	size_t bins = N/2 + 1;
	ReferenceAnalysis<S> ref(N, hop);
	std::vector<double> refLogGain(bins, 0.0);

	double maxError = 0;
	size_t scheduleIndex = 0;
	EnvelopeParams params = schedule.front().second;
	for (size_t f = 0; f < frames; ++f) {
		while (scheduleIndex < schedule.size() && schedule[scheduleIndex].first == f) {
			params = schedule[scheduleIndex].second;
			check(duck.setParameters(params.threshold, params.ratio, params.attack, params.release),
				"setParameters() in envelope test");
			++scheduleIndex;
		}
		const S *in[3] = {nullptr, nullptr, sidechain.data() + f*hop};
		S *out[2] = {nullptr, nullptr};
		duck.process(in, out, 0); // zero-length call with null pointers must be a no-op
		std::vector<S> dummy(hop, S(0));
		const S *in2[3] = {dummy.data(), dummy.data(), sidechain.data() + f*hop};
		std::vector<S> outL(hop), outR(hop);
		S *out2[2] = {outL.data(), outR.data()};
		duck.process(in2, out2, hop);

		double hopSeconds = double(hop)/sampleRate;
		double attackCoeff = envelopeCoeff(hopSeconds, params.attack);
		double releaseCoeff = envelopeCoeff(hopSeconds, params.release);
		for (size_t k = 0; k < bins; ++k) {
			double A = ref.amplitude(sidechain, f, k);
			double target = targetGain(A, params.threshold, params.ratio);
			double logTarget = std::log(target);
			double coeff = (logTarget < refLogGain[k]) ? attackCoeff : releaseCoeff;
			refLogGain[k] = logTarget + coeff*(refLogGain[k] - logTarget);
			double refGain = std::exp(refLogGain[k]);
			maxError = std::max(maxError, std::fabs(double(duck.gains()[k]) - refGain));
			check(closeTo(S(duck.gains()[k]), refGain), "gain recursion matches scalar reference");
		}
	}
	return maxError;
}

// ---- Signal generation ---------------------------------------------------------
static double testSidechain(long n) {
	// DC + Nyquist + on-bin tone (bin 5) + off-bin tone (bin 3.5), with a silence gap.
	if (n >= 320 && n < 480) return 0.0;
	return 0.30
		+ 0.25*((n & 1) ? -1.0 : 1.0)
		+ 0.20*std::sin(2*M_PI*5*n/64.0)
		+ 0.15*std::sin(2*M_PI*3.5*n/64.0);
}

// ---- 3. Full processing: two-tone ducking, relations, chunks, in-place, reset --
template<typename S>
struct DuckerResults {
	std::vector<S> left, right;
};

template<typename S>
static DuckerResults<S> runDucker(size_t N, size_t hop, double sampleRate,
		const std::vector<S> &mainL, const std::vector<S> &mainR,
		const std::vector<S> &sidechain, size_t chunkSize, bool inPlace,
		EnvelopeParams params, bool doReset=false) {
	signalsmith::linear::SpectralDucker<S> duck;
	bool prepared = duck.prepare(sampleRate, N, hop);
	check(prepared, "prepare() in processing test");
	duck.setParameters(params.threshold, params.ratio, params.attack, params.release);

	std::vector<S> inL = mainL, inR = mainR, inS = sidechain;
	std::vector<S> outL(inL.size(), S(0)), outR(inR.size(), S(0));
	S *pL = inL.data(), *pR = inR.data();
	if (inPlace) {
		// Output aliases the corresponding main input exactly.
		pL = inL.data();
		pR = inR.data();
		outL.resize(0); outR.resize(0);
	}
	if (doReset) {
		duck.reset();
		for (auto &g : duck.gains()) check(g == S(1), "reset() sets every gain to 1");
	}

	size_t total = mainL.size();
	for (size_t pos = 0; pos < total; pos += chunkSize) {
		size_t len = std::min(chunkSize, total - pos);
		const S *in[3] = {pL + pos, pR + pos, inS.data() + pos};
		S *out[2] = {inPlace ? pL + pos : outL.data() + pos, inPlace ? pR + pos : outR.data() + pos};
		duck.process(in, out, len);
	}
	DuckerResults<S> r;
	r.left = inPlace ? inL : outL;
	r.right = inPlace ? inR : outR;
	return r;
}

// Independent end-to-end reference: a hand-driven DynamicSTFT whose per-bin gains
// come from the naive-DFT amplitudes and the scalar envelope — not a second ducker.
template<typename S>
static DuckerResults<S> referenceProcess(size_t N, size_t hop, double sampleRate,
		const std::vector<S> &mainL, const std::vector<S> &mainR,
		const std::vector<S> &sidechain, EnvelopeParams params) {
	signalsmith::linear::DynamicSTFT<S> stft;
	stft.configure(3, 2, N, 0, hop);
	stft.setNormalisation(signalsmith::linear::DynamicSTFT<S>::normRoundTrip);
	ReferenceAnalysis<S> refAnalysis(N, hop);
	size_t bins = N/2 + 1, frames = mainL.size()/hop;
	std::vector<double> logGain(bins, 0.0);
	double hopSeconds = double(hop)/sampleRate;
	double attackCoeff = envelopeCoeff(hopSeconds, params.attack);
	double releaseCoeff = envelopeCoeff(hopSeconds, params.release);

	DuckerResults<S> out;
	out.left.assign(mainL.size(), S(0));
	out.right.assign(mainL.size(), S(0));
	for (size_t f = 0; f < frames; ++f) {
		size_t pos = f*hop;
		// Read the previously synthesised block first (the canonical STFT ordering,
		// matching the ducker); the first read is the initial zero block.
		stft.readOutput(0, hop, out.left.data() + pos);
		stft.readOutput(1, hop, out.right.data() + pos);

		stft.writeInput(0, hop, mainL.data() + pos);
		stft.writeInput(1, hop, mainR.data() + pos);
		stft.writeInput(2, hop, sidechain.data() + pos);
		stft.moveInput(hop);
		stft.analyse();

		std::vector<S> gain(bins);
		for (size_t k = 0; k < bins; ++k) {
			double A = refAnalysis.amplitude(sidechain, f, k);
			double target = targetGain(A, params.threshold, params.ratio);
			double logTarget = std::log(target);
			double coeff = (logTarget < logGain[k]) ? attackCoeff : releaseCoeff;
			logGain[k] = logTarget + coeff*(logGain[k] - logTarget);
			gain[k] = S(std::exp(logGain[k]));
		}
		for (size_t c = 0; c < 2; ++c) {
			std::complex<S> *spec = stft.spectrum(c);
			spec[0].real(spec[0].real()*gain[0]);
			spec[0].imag(spec[0].imag()*gain[N/2]);
			for (size_t k = 1; k < N/2; ++k) spec[k] *= gain[k];
		}
		stft.moveOutput(hop);
		stft.synthesise();
	}
	return out;
}

template<typename S>
static void testProcessing(const char *typeName) {
	const size_t N = 128, hop = 32;
	const double sampleRate = 48000;
	const size_t total = 2560; // 80 hops
	const size_t lowBin = 4, highBin = 20;

	std::vector<S> mainL(total), mainR(total), side(total), phaseR(total);
	for (size_t n = 0; n < total; ++n) {
		double low = std::sin(2*M_PI*lowBin*n/double(N));
		double high = std::sin(2*M_PI*highBin*n/double(N));
		mainL[n] = S(low + high);
		mainR[n] = S(0.7*(low + high));       // fixed amplitude ratio
		phaseR[n] = S(-(low + high));         // phase inversion
		side[n] = S(0.5*low);                 // sidechain contains only the low band
	}

	EnvelopeParams params{0.1, 4.0, 0.0, 0.1}; // instant attack -> exact steady gain
	double expectedDuckGain = std::pow(0.1/0.5, 1.0 - 1.0/4.0);

	auto baseline = runDucker<S>(N, hop, sampleRate, mainL, mainR, side, 31, false, params);

	// Whole outputs must match an independently built STFT chain (naive DFT + scalar envelope).
	auto reference = referenceProcess<S>(N, hop, sampleRate, mainL, mainR, side, params);
	double maxReferenceDiff = 0;
	for (size_t n = 0; n < total; ++n) {
		maxReferenceDiff = std::max(maxReferenceDiff,
				std::fabs(double(baseline.left[n]) - double(reference.left[n])));
		maxReferenceDiff = std::max(maxReferenceDiff,
				std::fabs(double(baseline.right[n]) - double(reference.right[n])));
	}
	std::printf("  [%s] max difference vs independent STFT/envelope reference: %.3e\n",
			typeName, maxReferenceDiff);
	check(closeTo(S(maxReferenceDiff), 0.0), "output matches an independently constructed reference");

	// Two-band amplitudes, measured with the analysis window so the result matches
	// what the STFT acts on, and compared against an un-ducked reference output.
	auto unDucked = runDucker<S>(N, hop, sampleRate, mainL, mainR, side, 31, false,
			EnvelopeParams{0.1, 1.0, 0.01, 0.1});
	size_t measureStart = N + 40*hop; // after latency + many converged frames
	std::vector<double> refL(mainL.begin(), mainL.end());
	std::vector<double> outL(baseline.left.begin(), baseline.left.end());
	std::vector<double> plainL(unDucked.left.begin(), unDucked.left.end());
	double inLow = windowedBinMagnitude(refL, measureStart - N, N, hop, lowBin);
	double inHigh = windowedBinMagnitude(refL, measureStart - N, N, hop, highBin);
	double outLow = windowedBinMagnitude(outL, measureStart, N, hop, lowBin);
	double outHigh = windowedBinMagnitude(outL, measureStart, N, hop, highBin);
	double plainLow = windowedBinMagnitude(plainL, measureStart, N, hop, lowBin);
	double plainHigh = windowedBinMagnitude(plainL, measureStart, N, hop, highBin);
	std::printf("  [%s] two-tone output amplitudes: ducked bin %zu = %.6f (input %.3f), "
			"preserved bin %zu = %.6f (input %.3f)\n",
			typeName, lowBin, outLow, inLow, highBin, outHigh, inHigh);
	// Target band is attenuated toward the static gain; far band stays at unity.  Window
	// leakage from neighbouring unity-gain bins sets a physical floor on each ratio.
	double duckedRatio = outLow/plainLow;
	double preservedRatio = outHigh/plainHigh;
	std::printf("  [%s] band gain ratios (vs un-ducked): target %.6f (target %.6f), far %.6f\n",
			typeName, duckedRatio, expectedDuckGain, preservedRatio);
	check(duckedRatio < 0.75 && std::fabs(duckedRatio - expectedDuckGain) < 0.10,
			"target band is ducked toward the expected gain");
	check(preservedRatio > 0.97, "far band is preserved");
	(void)inLow; (void)inHigh;

	// Left/right fixed amplitude ratio and phase inversion.
	auto phase = runDucker<S>(N, hop, sampleRate, mainL, phaseR, side, 31, false, params);
	double maxRatioError = 0, maxPhaseError = 0;
	for (size_t n = N; n + N < total; ++n) {
		maxRatioError = std::max(maxRatioError, std::fabs(double(baseline.right[n]) - 0.7*double(baseline.left[n])));
		maxPhaseError = std::max(maxPhaseError, std::fabs(double(phase.right[n]) + double(phase.left[n])));
	}
	std::printf("  [%s] L/R max errors: amplitude-ratio %.3e, phase-inversion %.3e\n",
			typeName, maxRatioError, maxPhaseError);
	check(closeTo(S(maxRatioError), 0.0), "fixed amplitude ratio survives processing");
	check(closeTo(S(maxPhaseError), 0.0), "phase inversion survives processing");

	// Latency: with ratio 1, the chain must be a perfect frameSize-delay reconstruction.
	EnvelopeParams bypass{0.1, 1.0, 0.01, 0.1};
	auto delayed = runDucker<S>(N, hop, sampleRate, mainL, mainR, side, 31, false, bypass);
	size_t latency = delayed.left.size(); // placeholder, query below
	signalsmith::linear::SpectralDucker<S> probe;
	probe.prepare(sampleRate, N, hop);
	check(probe.latency() == N, "reported latency equals frameSize");
	latency = probe.latency();
	double maxLatencyError = 0;
	for (size_t n = 0; n + latency + N < total; ++n) {
		maxLatencyError = std::max(maxLatencyError,
				std::fabs(double(delayed.left[n + latency]) - double(mainL[n])));
	}
	std::printf("  [%s] latency=%zu, reconstruction max error %.3e\n",
			typeName, latency, maxLatencyError);
	check(closeTo(S(maxLatencyError), 0.0), "bypass processing is a frameSize-delayed identity");

	// Chunk sizes 1/7/31 and one-shot must give the same processing clock/result.
	for (size_t chunk : {size_t(1), size_t(7), size_t(31), total}) {
		auto chunked = runDucker<S>(N, hop, sampleRate, mainL, mainR, side, chunk, false, params);
		double maxDiff = 0;
		for (size_t n = 0; n < total; ++n) {
			maxDiff = std::max(maxDiff, std::fabs(double(chunked.left[n]) - double(baseline.left[n])));
			maxDiff = std::max(maxDiff, std::fabs(double(chunked.right[n]) - double(baseline.right[n])));
		}
		std::printf("  [%s] chunk=%zu max output difference %.3e\n", typeName, chunk, maxDiff);
		check(closeTo(S(maxDiff), 0.0), "chunked processing matches hop-sized processing");
	}

	// Fully in-place processing.
	auto inPlace = runDucker<S>(N, hop, sampleRate, mainL, mainR, side, 7, true, params);
	double maxInPlaceDiff = 0;
	for (size_t n = 0; n < total; ++n) {
		maxInPlaceDiff = std::max(maxInPlaceDiff,
				std::fabs(double(inPlace.left[n]) - double(baseline.left[n])));
	}
	std::printf("  [%s] in-place max output difference %.3e\n", typeName, maxInPlaceDiff);
	check(closeTo(S(maxInPlaceDiff), 0.0), "in-place processing matches out-of-place");

	// Reset: a reset instance must behave like a freshly configured one.
	auto resetRun = runDucker<S>(N, hop, sampleRate, mainL, mainR, side, 31, false, params, true);
	signalsmith::linear::SpectralDucker<S> fresh;
	fresh.prepare(sampleRate, N, hop);
	fresh.setParameters(params.threshold, params.ratio, params.attack, params.release);
	std::vector<S> freshL(total, S(0)), freshR(total, S(0));
	for (size_t pos = 0; pos < total; pos += hop) {
		const S *in[3] = {mainL.data() + pos, mainR.data() + pos, side.data() + pos};
		S *out[2] = {freshL.data() + pos, freshR.data() + pos};
		fresh.process(in, out, hop);
	}
	double maxResetDiff = 0;
	for (size_t n = 0; n < total; ++n) {
		maxResetDiff = std::max(maxResetDiff,
				std::fabs(double(resetRun.left[n]) - double(freshL[n])));
	}
	std::printf("  [%s] reset vs fresh instance max difference %.3e\n", typeName, maxResetDiff);
	check(closeTo(S(maxResetDiff), 0.0), "reset() reproduces a fresh instance");
}

// ---- 4. splitComputation consistency + actual step usage -----------------------
template<typename S>
static void testSplitMode() {
	const size_t N = 128, hop = 32;
	const double sampleRate = 48000;
	const size_t total = 1024;
	std::vector<S> l(total), r(total), s(total);
	for (size_t n = 0; n < total; ++n) {
		l[n] = S(0.4*std::sin(2*M_PI*6*n/double(N)) + 0.3*std::sin(2*M_PI*17*n/double(N)));
		r[n] = S(-0.5*l[n]);
		s[n] = S(0.6*std::sin(2*M_PI*6*n/double(N)));
	}
	EnvelopeParams params{0.15, 3.0, 0.008, 0.05};

	signalsmith::linear::SpectralDucker<S, false> whole;
	signalsmith::linear::SpectralDucker<S, true> split;
	check(whole.prepare(sampleRate, N, hop) && split.prepare(sampleRate, N, hop), "prepare both modes");
	whole.setParameters(params.threshold, params.ratio, params.attack, params.release);
	split.setParameters(params.threshold, params.ratio, params.attack, params.release);

	// Prove the split template actually expands the step counts.
	signalsmith::linear::DynamicSTFT<S, false> stftWhole;
	signalsmith::linear::DynamicSTFT<S, true> stftSplit;
	stftWhole.configure(3, 2, N, 0, hop);
	stftSplit.configure(3, 2, N, 0, hop);
	std::printf("  analyse steps: whole=%zu split=%zu, synthesise steps: whole=%zu split=%zu\n",
			stftWhole.analyseSteps(), stftSplit.analyseSteps(),
			stftWhole.synthesiseSteps(), stftSplit.synthesiseSteps());
	check(stftSplit.analyseSteps() > stftWhole.analyseSteps(), "split mode uses analyseStep() chunks");
	check(stftSplit.synthesiseSteps() > stftWhole.synthesiseSteps(), "split mode uses synthesiseStep() chunks");

	std::vector<S> wl(total, S(0)), wr(total, S(0)), sl(total, S(0)), sr(total, S(0));
	for (size_t pos = 0; pos < total; pos += 7) {
		size_t len = std::min(size_t(7), total - pos);
		const S *inW[3] = {l.data() + pos, r.data() + pos, s.data() + pos};
		S *outW[2] = {wl.data() + pos, wr.data() + pos};
		whole.process(inW, outW, len);
		const S *inS[3] = {l.data() + pos, r.data() + pos, s.data() + pos};
		S *outS[2] = {sl.data() + pos, sr.data() + pos};
		split.process(inS, outS, len);
	}
	double maxDiff = 0, maxGainDiff = 0;
	for (size_t n = 0; n < total; ++n) {
		maxDiff = std::max(maxDiff, std::fabs(double(wl[n]) - double(sl[n])));
		maxDiff = std::max(maxDiff, std::fabs(double(wr[n]) - double(sr[n])));
	}
	for (size_t k = 0; k < whole.gains().size(); ++k) {
		maxGainDiff = std::max(maxGainDiff,
				std::fabs(double(whole.gains()[k]) - double(split.gains()[k])));
	}
	std::printf("  split vs whole-step: max output diff %.3e, max gain diff %.3e\n",
			maxDiff, maxGainDiff);
	check(closeTo(S(maxDiff), 0.0), "split and non-split outputs agree");
	check(closeTo(S(maxGainDiff), 0.0), "split and non-split gains agree");
}

// ---- 5. Validation, zero-length calls, allocation accounting -------------------
template<typename S>
static void testValidationAndAllocations() {
	signalsmith::linear::SpectralDucker<S> duck;
	check(!duck.prepare(48000, 16, 4), "frameSize below 32 rejected");
	check(!duck.prepare(48000, 100, 25), "non-power-of-2 frameSize rejected");
	check(!duck.prepare(48000, 128, 40), "hop above frameSize/4 rejected");
	check(!duck.prepare(48000, 128, 30), "non-divisor hop rejected");
	check(!duck.prepare(48000, 128, 0), "zero hop rejected");
	check(!duck.prepare(0, 128, 32), "zero sample rate rejected");
	check(!duck.prepare(-1.0, 128, 32), "negative sample rate rejected");
	check(!duck.prepare(std::numeric_limits<double>::infinity(), 128, 32), "infinite sample rate rejected");
	check(!duck.prepare(std::numeric_limits<double>::quiet_NaN(), 128, 32), "NaN sample rate rejected");
	check(duck.prepare(48000, 128, 32), "valid configuration accepted");

	check(!duck.setParameters(0, 2, 0.01, 0.1), "zero threshold rejected");
	check(!duck.setParameters(-1, 2, 0.01, 0.1), "negative threshold rejected");
	check(!duck.setParameters(0.1, 0.5, 0.01, 0.1), "ratio below 1 rejected");
	check(!duck.setParameters(0.1, 2, -0.01, 0.1), "negative attack rejected");
	check(!duck.setParameters(0.1, 2, 0.01, std::numeric_limits<double>::infinity()), "infinite release rejected");
	check(duck.setParameters(0.2, 2.5, 0.0, 0.0), "valid edge parameters accepted");
	// Rejected calls retain the previous (valid) configuration.
	check(duck.latency() == 128, "failed setParameters keeps the old configuration");

	// Null pointers with zero frames: no-op, no advancement.
	const S *nullIn[3] = {nullptr, nullptr, nullptr};
	S *nullOut[2] = {nullptr, nullptr};
	duck.process(nullIn, nullOut, 0);
	check(true, "zero-frame process with null pointers is safe");

	// No heap traffic during processing.
	const size_t total = 2048;
	std::vector<S> l(total, S(0)), r(total, S(0)), s(total, S(0));
	for (size_t n = 0; n < total; ++n) {
		l[n] = S(0.3*std::sin(2*M_PI*9*n/128.0));
		r[n] = S(-l[n]);
		s[n] = S(0.5*std::sin(2*M_PI*9*n/128.0));
	}
	std::vector<S> ol(total), or_(total);
	heapAllocCount = 0;
	heapFreeCount = 0;
	for (size_t pos = 0; pos < total; pos += 7) {
		size_t len = std::min(size_t(7), total - pos);
		const S *in[3] = {l.data() + pos, r.data() + pos, s.data() + pos};
		S *out[2] = {ol.data() + pos, or_.data() + pos};
		duck.process(in, out, len);
	}
	std::printf("  heap during process(): allocations=%ld frees=%ld\n", heapAllocCount, heapFreeCount);
	check(heapAllocCount == 0 && heapFreeCount == 0, "process() performs no heap allocation/deallocation");
}

template<typename S>
static void suite(const char *typeName) {
	std::printf("=== SpectralDucker<%s> ===\n", typeName);
	const size_t N = 64, hop = 16;
	const double sampleRate = 48000;
	const size_t frames = 48;
	std::vector<S> sidechain(frames*hop);
	for (size_t n = 0; n < sidechain.size(); ++n) sidechain[n] = S(testSidechain(long(n)));

	double calibError = testAmplitudeCalibration<S>(N, hop, sampleRate, sidechain, frames);
	std::printf("  amplitude calibration max error (vs naive DFT): %.3e\n", calibError);
	check(closeTo(S(calibError), 0.0), "amplitude calibration matches naive DFT");

	// Instant times, including ratio boundaries.
	std::vector<std::pair<size_t, EnvelopeParams>> instantSchedule = {
		{0, EnvelopeParams{0.10, 4.0, 0.0, 0.0}},
	};
	double instantError = testEnvelopeRecursion<S>(N, hop, sampleRate, sidechain, frames, instantSchedule);

	// Non-zero times, a mid-run parameter change, and ratio=1 (must release back to 1).
	std::vector<std::pair<size_t, EnvelopeParams>> mixedSchedule = {
		{0,  EnvelopeParams{0.05, 2.0, 0.01, 0.10}},
		{16, EnvelopeParams{0.10, 4.0, 0.00, 0.00}},
		{32, EnvelopeParams{0.10, 1.0, 0.005, 0.02}},
	};
	double mixedError = testEnvelopeRecursion<S>(N, hop, sampleRate, sidechain, frames, mixedSchedule);
	double gainError = std::max(instantError, mixedError);
	std::printf("  gain recursion max error (vs scalar reference): %.3e\n", gainError);
	check(closeTo(S(gainError), 0.0), "envelope recursion matches scalar reference");

	testProcessing<S>(typeName);
	testSplitMode<S>();
	testValidationAndAllocations<S>();
}

int main() {
	suite<double>("double");
	suite<float>("float");
	if (failures) {
		std::printf("\nSpectralDucker checks FAILED: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("\nSpectralDucker checks passed.\n");
	return 0;
}
