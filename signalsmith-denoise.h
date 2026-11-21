#ifndef SIGNALSMITH_DENOISE_H
#define SIGNALSMITH_DENOISE_H

// Optional multi-channel linked spectral denoiser, designed to be driven from
// the analysis/synthesis hops of SignalsmithStretch.
//
// The model works on per-band *power* spectra (one value per source band,
// averaged across channels).  It is FFT-free itself: the stretcher feeds it the
// powers from the STFT it already computes.  StftPowerAnalyser below is a small
// standalone STFT stage built on the bundled Signalsmith Linear FFT, so the
// model can also be exercised/verified outside the stretcher.

#include "signalsmith-linear/fft.h"

#include <vector>
#include <complex>
#include <cmath>
#include <cstddef>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace signalsmith { namespace denoise {

/// Pure spectral noise model + per-output-band gain envelope.
///
/// All time constants are in *audio frames* (samples), not call counts.
template<typename Sample>
struct DenoiseModel {
	struct Params {
		bool denoiseEnabled = false;
		Sample strength = Sample(1);
		Sample floorGain = Sample(0.1);
		Sample attackFrames = Sample(256);
		Sample releaseFrames = Sample(1024);
		bool learningEnabled = false;
		Sample learnTimeFrames = Sample(2048);
	};

	DenoiseModel() {
		active = pending = Params();
	}

	/// Allocates all working memory.  After this, processing never allocates.
	/// Parameter settings are preserved across re-configure.
	void configure(int bandCount) {
		bands = bandCount;
		power.assign(bands, Sample(0));
		noise.assign(bands, Sample(0));
		targetSource.assign(bands, Sample(1));
		targetOutput.assign(bands, Sample(1));
		gain.assign(bands, Sample(1));
		clearAudioState();
	}

	int bandCount() const {
		return bands;
	}
	bool isEngaged() const {
		return engaged;
	}
	bool hasLearned() const {
		return learned;
	}

	// ---- Parameter requests (validated; rejected calls keep the old settings) ----

	/// strength in [0, 4], floorGain in [0, 1], time constants finite and > 0.
	bool setDenoise(bool enabled, Sample strength, Sample floorGain, Sample attackFrames, Sample releaseFrames) {
		if (!(strength >= Sample(0) && strength <= Sample(4))) return false;
		if (!(floorGain >= Sample(0) && floorGain <= Sample(1))) return false;
		if (!std::isfinite(attackFrames) || attackFrames <= Sample(0)) return false;
		if (!std::isfinite(releaseFrames) || releaseFrames <= Sample(0)) return false;
		pending.denoiseEnabled = enabled;
		pending.strength = strength;
		pending.floorGain = floorGain;
		pending.attackFrames = attackFrames;
		pending.releaseFrames = releaseFrames;
		engaged = true;
		return true;
	}
	/// timeFrames must be finite and > 0.
	bool setNoiseLearning(bool enabled, Sample timeFrames) {
		if (!std::isfinite(timeFrames) || timeFrames <= Sample(0)) return false;
		pending.learningEnabled = enabled;
		pending.learnTimeFrames = timeFrames;
		engaged = true;
		return true;
	}

	/// Latch all pending requests.  Called by the stretcher at the start of a
	/// synthesis hop, so a hop already in progress (e.g. a split-computation
	/// hop) keeps using the previous settings.
	void latchRequests() {
		active = pending;
	}

	const Params & params() const {
		return active;
	}

	// ---- Lifecycle ----

	/// reset(): drops the learned model, envelope and all spectral history,
	/// while keeping the configured parameters.
	void clearAudioState() {
		learned = false;
		clockValid = false;
		lastSourcePos = Sample(0);
		std::fill(power.begin(), power.end(), Sample(0));
		std::fill(noise.begin(), noise.end(), Sample(0));
		std::fill(targetSource.begin(), targetSource.end(), Sample(1));
		std::fill(targetOutput.begin(), targetOutput.end(), Sample(1));
		std::fill(gain.begin(), gain.end(), Sample(1));
	}

	/// seek(): keeps the learned noise, pulls the envelope back to 1 and
	/// re-anchors the learning clock for the following real input.
	void onSeek() {
		clockValid = false;
		std::fill(gain.begin(), gain.end(), Sample(1));
		std::fill(targetOutput.begin(), targetOutput.end(), Sample(1));
		std::fill(targetSource.begin(), targetSource.end(), Sample(1));
	}

	/// flush() uses this to mark artificial zero-padded input: such spectra are
	/// neither learned nor allowed to advance the learning clock.
	void setArtificialInput(bool artificial) {
		artificialInput = artificial;
	}

	// ---- Learning ----

	/// Feed one genuinely analysed (pre-denoise) spectrum.
	///   sourcePosition: position of this analysis centre, in source audio
	///                   frames, on the caller's monotonically increasing clock.
	///   realInput:      false for seek pre-roll, repeated synthesis of the same
	///                   source centre, or flush zero-padding.
	///   powers:         per-source-band power P[b], already averaged over channels.
	void observeSpectrum(Sample sourcePosition, bool realInput, const Sample *powers) {
		for (int b = 0; b < bands; ++b) {
			power[b] = powers[b]; // always keep the latest P, even while learning is off
		}
		if (!realInput || artificialInput) return; // no learning, no clock advancement

		if (!clockValid) {
			// First genuine spectrum after configure/reset/seek: anchor the clock.
			clockValid = true;
			lastSourcePos = sourcePosition;
			if (active.learningEnabled && !learned) {
				noise = power;
				learned = true;
			}
			return;
		}
		Sample d = std::max<Sample>(Sample(0), sourcePosition - lastSourcePos);
		lastSourcePos = sourcePosition;
		if (active.learningEnabled) {
			if (!learned) {
				noise = power;
				learned = true;
			} else {
				Sample a = std::exp(-d/active.learnTimeFrames);
				Sample keep = Sample(1) - a;
				for (int b = 0; b < bands; ++b) {
					noise[b] = a*noise[b] + keep*power[b];
				}
			}
		}
	}

	// Genuine silence carries no analysis spectrum, so the caller simply keeps
	// its own monotonic source-position clock advancing through the gap and
	// passes the new absolute position on the next observeSpectrum(); the
	// resulting d then naturally spans the silence.  Nothing here needs updating.

	// ---- Gain computation ----

	/// Source-band target gains from the current P and learned N.
	void updateTargets() {
		if (!learned || !active.denoiseEnabled) {
			std::fill(targetSource.begin(), targetSource.end(), Sample(1));
			return;
		}
		const Sample epsilon = Sample(1e-20);
		for (int b = 0; b < bands; ++b) {
			Sample ratio = active.strength*noise[b]/std::max(power[b], epsilon);
			Sample g = std::sqrt(std::max<Sample>(Sample(0), Sample(1) - ratio));
			targetSource[b] = std::max(active.floorGain, g);
		}
	}

	/// Linearly interpolate the source-band targets onto the output bins
	/// described by the stretcher's frequency map (inputBin per output band).
	void mapToOutput(const Sample *sourceBins) {
		for (int b = 0; b < bands; ++b) {
			Sample x = sourceBins[b];
			if (x <= Sample(0)) {
				targetOutput[b] = targetSource[0];
			} else if (x >= Sample(bands - 1)) {
				targetOutput[b] = targetSource[bands - 1];
			} else {
				int low = static_cast<int>(std::floor(x));
				Sample frac = x - low;
				targetOutput[b] = targetSource[low] + (targetSource[low + 1] - targetSource[low])*frac;
			}
		}
	}

	/// One-pole envelope per output band.  Downwards moves use the attack time,
	/// upwards moves the release time; q = exp(-hopFrames/timeConstant).
	void smoothGains(Sample hopFrames) {
		for (int b = 0; b < bands; ++b) {
			Sample tau = (targetOutput[b] < gain[b]) ? active.attackFrames : active.releaseFrames;
			Sample q = std::exp(-hopFrames/tau);
			gain[b] = q*gain[b] + (Sample(1) - q)*targetOutput[b];
		}
	}

	/// Full per-synthesis-hop update: targets, frequency mapping and envelope.
	void runHop(Sample hopFrames, const Sample *sourceBins) {
		updateTargets();
		mapToOutput(sourceBins);
		smoothGains(hopFrames);
	}

	/// Advances the envelope without a new synthesis hop (the stretcher's
	/// silence fast-path).  Targets stay where they were; only time moves on.
	void advanceEnvelope(Sample outputFrames) {
		if (!engaged || bands == 0) return;
		for (int b = 0; b < bands; ++b) {
			Sample tau = (targetOutput[b] < gain[b]) ? active.attackFrames : active.releaseFrames;
			Sample q = std::exp(-std::max<Sample>(outputFrames, Sample(0))/tau);
			gain[b] = q*gain[b] + (Sample(1) - q)*targetOutput[b];
		}
	}

	const Sample * gains() const {
		return gain.data();
	}
	/// Read-only per-source-band learned noise power N[b], one value per STFT
	/// band.  Returns nullptr until a model has been learned (or before configure).
	const Sample * noisePowerSpectrum() const {
		return (bands && learned) ? noise.data() : nullptr;
	}

private:
	int bands = 0;
	bool engaged = false;       // any denoise/learning request has succeeded
	bool learned = false;       // N holds a learned model
	bool clockValid = false;    // the learning clock has been anchored
	bool artificialInput = false;
	Sample lastSourcePos = Sample(0);

	Params active, pending;
	std::vector<Sample> power;          // latest P[b], averaged over channels
	std::vector<Sample> noise;          // learned N[b]
	std::vector<Sample> targetSource;   // T[b] in source bands
	std::vector<Sample> targetOutput;   // T mapped to output bands
	std::vector<Sample> gain;           // smoothed output-band gains G[b]
};

/// Small standalone STFT power analyser, using the bundled standard C++ Linear
/// FFT (no external packages or platform FFT backends).  Windowed Hann frames
/// go in; per-bin power (re^2 + im^2, size/2+1 bins) comes out.
template<typename Sample>
struct StftPowerAnalyser {
	using Complex = std::complex<Sample>;

	void configure(size_t fftSize) {
		size = fftSize;
		fft.resize(size);
		window.resize(size);
		time.resize(size);
		spectrum.resize(size);
		for (size_t i = 0; i < size; ++i) {
			window[i] = Sample(0.5) - Sample(0.5)*std::cos(Sample(2*M_PI)*i/Sample(size - 1));
		}
	}

	size_t fftSize() const {
		return size;
	}
	size_t bins() const {
		return size/2 + 1;
	}

	/// frame must hold fftSize() samples; output must hold bins() values.
	void analyse(const Sample *frame, Sample *outPower) {
		for (size_t i = 0; i < size; ++i) {
			time[i] = frame[i]*window[i];
		}
		fft.fft(time.data(), spectrum.data());
		for (size_t b = 0; b < bins(); ++b) {
			outPower[b] = spectrum[b].real()*spectrum[b].real() + spectrum[b].imag()*spectrum[b].imag();
		}
	}

private:
	size_t size = 0;
	signalsmith::linear::SimpleRealFFT<Sample> fft;
	std::vector<Sample> window, time;
	std::vector<Complex> spectrum;
};

}} // namespace
#endif // include guard
