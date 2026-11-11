#ifndef SIGNALSMITH_AUDIO_LINEAR_SPECTRAL_DUCKER_H
#define SIGNALSMITH_AUDIO_LINEAR_SPECTRAL_DUCKER_H

#include "./stft.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace signalsmith { namespace linear {

/// Frequency-domain sidechain ducker.
///
/// Two main channels (L/R) are processed via a round-trip-normalised `DynamicSTFT`,
/// while a single mono sidechain drives per-bin gains.  The sidechain is analysed
/// only — it is never synthesised or mixed with the main audio.
template<typename Sample, bool splitComputation=false>
class SpectralDucker {
public:
	using Complex = std::complex<Sample>;

	bool prepare(double sampleRate, size_t frameSize, size_t hopSize) {
		if (!std::isfinite(sampleRate) || sampleRate <= 0) return false;
		if (frameSize < 32 || (frameSize & (frameSize - 1)) != 0) return false;
		if (hopSize == 0 || hopSize > frameSize/4 || frameSize%hopSize != 0) return false;

		_sampleRate = sampleRate;
		_frameSize = frameSize;
		_hopSize = hopSize;

		// 3 analysis channels (L, R, sidechain), 2 synthesis channels (L, R).
		// Defaults: symmetric ACG window, round-trip normalisation.
		_stft.configure(3, 2, frameSize, 0, hopSize);
		_stft.setNormalisation(DynamicSTFT<Sample, splitComputation, STFT_SPECTRUM_PACKED>::normRoundTrip);

		Sample windowSum = 0;
		const Sample *window = _stft.analysisWindow();
		for (size_t i = 0; i < frameSize; ++i) windowSum += window[i];
		_invWindowSum = Sample(1)/windowSum;

		size_t bins = frameSize/2 + 1; // DC ... Nyquist, stored separately
		_logGains.assign(bins, Sample(0));
		_gains.assign(bins, Sample(1));
		_outputTemp.resize(2*hopSize); // per-hop output staging (in-place safe)
		_hopPhase = 0;
		_configured = true;
		updateCoefficients();
		return true;
	}

	bool setParameters(double threshold, double ratio, double attackSeconds, double releaseSeconds) {
		if (!std::isfinite(threshold) || threshold <= 0) return false;
		if (!std::isfinite(ratio) || ratio < 1) return false;
		if (!std::isfinite(attackSeconds) || attackSeconds < 0) return false;
		if (!std::isfinite(releaseSeconds) || releaseSeconds < 0) return false;

		_threshold = threshold;
		_ratio = ratio;
		_attackSeconds = attackSeconds;
		_releaseSeconds = releaseSeconds;
		if (_configured) updateCoefficients();
		return true;
	}

	/// `input` has 3 planar channels (L, R, mono sidechain), `output` has 2 (L, R).
	/// Main channels may be processed fully in-place.  No heap allocation happens here.
	void process(const Sample* const input[3], Sample* const output[2], size_t frames) {
		if (frames == 0 || !_configured) return;

		size_t done = 0;
		while (done < frames) {
			size_t chunk = std::min(frames - done, _hopSize - _hopPhase);
			// `output.pos` only advances on a hop boundary, so address each sub-hop
			// chunk explicitly by its offset within the current output block.
			size_t readOffset = _hopPhase;

			// Stage outputs first (mirrors the STFT's read-before-write ordering),
			// so aliasing input/output pointers cannot corrupt the incoming samples.
			_stft.readOutput(0, readOffset, chunk, _outputTemp.data());
			_stft.readOutput(1, readOffset, chunk, _outputTemp.data() + _hopSize);

			for (size_t c = 0; c < 3; ++c) {
				_stft.writeInput(c, chunk, input[c] + done);
			}
			_stft.moveInput(chunk);
			_hopPhase += chunk;

			if (_hopPhase == _hopSize) {
				_hopPhase = 0;
				analyseFrame();
			}

			for (size_t i = 0; i < chunk; ++i) {
				output[0][done + i] = _outputTemp[i];
				output[1][done + i] = _outputTemp[_hopSize + i];
			}
			done += chunk;
		}
	}

	/// Clears audio history and resets every bin gain to 1; parameters are retained.
	void reset() {
		if (!_configured) return;
		_stft.reset();
		_hopPhase = 0;
		for (auto &v : _logGains) v = 0;
		for (auto &v : _gains) v = Sample(1);
	}

	size_t latency() const {
		return _configured ? _frameSize : 0;
	}

	/// Linear gains of the most recent analysis frame, one entry per bin (DC ... Nyquist).
	const std::vector<Sample> & gains() const {
		return _gains;
	}

private:
	DynamicSTFT<Sample, splitComputation, STFT_SPECTRUM_PACKED> _stft;

	bool _configured = false;
	double _sampleRate = 0;
	size_t _frameSize = 0, _hopSize = 0, _hopPhase = 0;

	double _threshold = 0.1, _ratio = 1;
	double _attackSeconds = 0.01, _releaseSeconds = 0.1;
	Sample _attackCoeff = 0, _releaseCoeff = 0;
	Sample _invWindowSum = 1;

	std::vector<Sample> _logGains;
	std::vector<Sample> _gains;
	std::vector<Sample> _outputTemp;

	void updateCoefficients() {
		double hopSeconds = double(_hopSize)/_sampleRate;
		_attackCoeff = (_attackSeconds > 0) ? Sample(std::exp(-hopSeconds/_attackSeconds)) : Sample(0);
		_releaseCoeff = (_releaseSeconds > 0) ? Sample(std::exp(-hopSeconds/_releaseSeconds)) : Sample(0);
	}

	void updateBin(size_t bin, Sample amplitude) {
		Sample target;
		if (!(amplitude > Sample(_threshold))) {
			target = Sample(1);
		} else {
			double exponent = 1.0 - 1.0/_ratio;
			target = Sample(std::pow(double(_threshold)/double(amplitude), exponent));
		}
		Sample logTarget = std::log(target);
		Sample &logGain = _logGains[bin];
		// Gain going down = attack, going up = release.
		Sample coeff = (logTarget < logGain) ? _attackCoeff : _releaseCoeff;
		logGain = logTarget + coeff*(logGain - logTarget);
		_gains[bin] = std::exp(logGain);
	}

	void analyseFrame() {
		for (size_t s = 0; s < _stft.analyseSteps(); ++s) {
			_stft.analyseStep(s);
		}

		const Complex *sidechain = _stft.spectrum(2);
		size_t nyquistBin = _frameSize/2;

		// Packed real FFT: bin 0 holds DC in its real part, Nyquist in its imaginary part.
		// They are separate bands with separate envelopes.
		Sample dcAmplitude = std::abs(sidechain[0].real())*_invWindowSum;
		Sample nyquistAmplitude = std::abs(sidechain[0].imag())*_invWindowSum;
		updateBin(0, dcAmplitude);
		updateBin(nyquistBin, nyquistAmplitude);
		for (size_t k = 1; k < nyquistBin; ++k) {
			Sample amplitude = Sample(2)*std::abs(sidechain[k])*_invWindowSum;
			updateBin(k, amplitude);
		}

		// Apply the same linear gain to both main-channel spectra.
		for (size_t c = 0; c < 2; ++c) {
			Complex *spectrum = _stft.spectrum(c);
			spectrum[0].real(spectrum[0].real()*_gains[0]);
			spectrum[0].imag(spectrum[0].imag()*_gains[nyquistBin]);
			for (size_t k = 1; k < nyquistBin; ++k) {
				spectrum[k] *= _gains[k];
			}
		}

		_stft.moveOutput(_hopSize);
		for (size_t s = 0; s < _stft.synthesiseSteps(); ++s) {
			_stft.synthesiseStep(s);
		}
	}
};

}} // namespace
#endif // include guard
