#ifndef SIGNALSMITH_HARMONIZER_H
#define SIGNALSMITH_HARMONIZER_H

#include "signalsmith-linear/stft.h" // https://github.com/Signalsmith-Audio/linear

#include <vector>
#include <array>
#include <algorithm>
#include <random>
#include <limits>
#include <type_traits>
#include <cmath>

namespace signalsmith { namespace stretch {

namespace _harmImpl {
	template<bool conjugateSecond=false, typename V>
	static std::complex<V> mul(const std::complex<V> &a, const std::complex<V> &b) {
		return conjugateSecond ? std::complex<V>{
			b.real()*a.real() + b.imag()*a.imag(),
				b.real()*a.imag() - b.imag()*a.real()
		} : std::complex<V>{
			a.real()*b.real() - a.imag()*b.imag(),
			a.real()*b.imag() + a.imag()*b.real()
		};
	}
	template<typename V>
	static V norm(const std::complex<V> &a) {
		V r = a.real(), i = a.imag();
		return r*r + i*i;
	}
}

/// Multi-voice pitch/formant processor with a single shared analysis stage.
///
/// One forward STFT is performed per channel per hop, and the resulting spectra
/// are copied into each voice's private band state.  Every voice keeps its own
/// frequency map, peak targets, phase predictions, formant-corrected energies
/// and overlap-add history, so its output is numerically identical to a
/// standalone SignalsmithStretch configured with the same settings.
template<typename Sample=float>
struct SignalsmithHarmonizer {
	using Complex = std::complex<Sample>;
	static constexpr Sample noiseFloor{1e-15};
	static constexpr Sample maxCleanStretch{2};

	SignalsmithHarmonizer() : randomEngine(std::random_device{}()) {}
	SignalsmithHarmonizer(long seed) : randomEngine(seed) {}

	int blockSamples() const {
		return int(stft.blockSamples());
	}
	int intervalSamples() const {
		return int(stft.defaultInterval());
	}
	int inputLatency() const {
		return int(stft.analysisLatency());
	}
	int outputLatency() const {
		return int(stft.synthesisLatency() + (_splitComputation ? stft.defaultInterval() : 0));
	}

	void configure(int nChannels, int nVoices, int blockSamples_, int intervalSamples_, bool splitComputation=false) {
		_splitComputation = splitComputation;
		channels = nChannels;
		voices = nVoices;
		stft.configure(channels, voices*channels, blockSamples_, intervalSamples_ + 1);
		stft.setInterval(intervalSamples_, stft.kaiser);
		stft.reset(0.1);
		stashedInput = stft.input;
		stashedOutput = stft.output;
		tmpBuffer.resize(blockSamples_ + intervalSamples_);

		bands = int(stft.bands());
		voiceStates.resize(voices);
		for (auto &v : voiceStates) v.allocate(bands, channels, randomEngine);

		blockProcess = {};
		blockProcess.samplesSinceLast = std::numeric_limits<size_t>::max();
		silenceCounter = 0;
		silenceFirst = true;
		prevInputOffset = -1;
		didSeek = false;
		seekTimeFactor = 1;
		pendingFormantBase = 0;
	}

	/// Updates one voice's pending settings.  Takes effect at the start of the
	/// next synthesis hop; other voices (and this voice's phase/tail history)
	/// are untouched.
	void setVoice(int index, Sample transposeFactor, Sample formantFactor, bool compensatePitch) {
		VoiceState &v = voiceStates[size_t(index)];
		v.pendingFreqMultiplier = transposeFactor;
		v.pendingFreqTonalityLimit = 1;
		v.pendingFormantMultiplier = formantFactor;
		v.pendingInvFormantMultiplier = 1/formantFactor;
		v.pendingFormantCompensation = compensatePitch;
	}

	/// Sets the formant base frequency shared by all voices.  Latched at the
	/// start of the next synthesis hop.
	void setFormantBase(Sample baseFreq) {
		pendingFormantBase = baseFreq;
	}

	void reset() {
		stft.reset(0.1);
		stashedInput = stft.input;
		stashedOutput = stft.output;

		prevInputOffset = -1;
		for (auto &v : voiceStates) v.resetBands();
		silenceCounter = 0;
		silenceFirst = true;
		didSeek = false;
		blockProcess = {};
		blockProcess.samplesSinceLast = std::numeric_limits<size_t>::max();
	}

	/// Provide previous input ("pre-roll"), without affecting the speed
	/// calculation.  The shared input advances exactly once.
	template<class Inputs>
	void seek(Inputs &&inputs, int inputSamples, double playbackRate) {
		int tmpSize = int(stft.blockSamples() + stft.defaultInterval());

		int startIndex = std::max<int>(0, inputSamples - tmpSize); // start position in input
		int padStart = tmpSize + startIndex - inputSamples; // start position in tmpBuffer

		Sample totalEnergy = 0;
		for (int c = 0; c < channels; ++c) {
			auto &&inputChannel = inputs[c];
			for (int i = startIndex; i < inputSamples; ++i) {
				Sample s = inputChannel[i];
				totalEnergy += s*s;
				tmpBuffer[i - startIndex + padStart] = s;
			}
			stft.writeInput(c, tmpSize, tmpBuffer.data());
		}
		stft.moveInput(tmpSize);
		if (totalEnergy >= noiseFloor) {
			silenceCounter = 0;
			silenceFirst = true;
		}
		didSeek = true;
		seekTimeFactor = (playbackRate*stft.defaultInterval() > 1) ? Sample(1/playbackRate) : Sample(stft.defaultInterval());
	}

	template<class Inputs, class Outputs>
	void process(Inputs &&inputs, int inputSamples, Outputs &&outputs, int outputSamples) {
		int prevCopiedInput = 0;
		auto copyInput = [&](int toIndex) {
			int length = std::min<int>(int(stft.blockSamples() + stft.defaultInterval()), toIndex - prevCopiedInput);
			int offset = toIndex - length;
			for (int c = 0; c < channels; ++c) {
				auto &&inputBuffer = inputs[c];
				for (int i = 0; i < length; ++i) {
					tmpBuffer[i] = inputBuffer[i + offset];
				}
				stft.writeInput(c, length, tmpBuffer.data());
			}
			stft.moveInput(length);
			prevCopiedInput = toIndex;
		};

		Sample totalEnergy = 0;
		for (int c = 0; c < channels; ++c) {
			auto &&inputChannel = inputs[c];
			for (int i = 0; i < inputSamples; ++i) {
				Sample s = inputChannel[i];
				totalEnergy += s*s;
			}
		}

		if (totalEnergy < noiseFloor) {
			if (silenceCounter >= 2*stft.blockSamples()) {
				if (silenceFirst) { // first block of silence processing
					silenceFirst = false;
					blockProcess = {};
					for (auto &v : voiceStates) {
						for (auto &b : v.channelBands) {
							b.input = b.prevInput = b.output = 0;
							b.inputEnergy = 0;
						}
					}
				}

				if (inputSamples > 0) {
					// copy from the input, wrapping around if needed, into every voice
					for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
						int inputIndex = outputIndex%inputSamples;
						for (int v = 0; v < voices; ++v) {
							for (int c = 0; c < channels; ++c) {
								outputs[v*channels + c][outputIndex] = inputs[c][inputIndex];
							}
						}
					}
				} else {
					for (int v = 0; v < voices; ++v) {
						for (int c = 0; c < channels; ++c) {
							auto &&outputChannel = outputs[v*channels + c];
							for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
								outputChannel[outputIndex] = 0;
							}
						}
					}
				}

				// Store input in the shared history buffer once
				copyInput(inputSamples);
				return;
			} else {
				silenceCounter += inputSamples;
			}
		} else {
			silenceCounter = 0;
			silenceFirst = true;
		}

		for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
			bool newBlock = blockProcess.samplesSinceLast >= stft.defaultInterval();
			if (newBlock) {
				blockProcess.step = 0;
				blockProcess.samplesSinceLast = 0;

				// Time to process a spectrum!  Where should it come from in the input?
				int inputOffset = std::round(outputIndex*Sample(inputSamples)/outputSamples);
				int inputInterval = inputOffset - prevInputOffset;
				prevInputOffset = inputOffset;

				copyInput(inputOffset);
				stashedInput = stft.input; // save the input state, since that's what we'll analyse later
				if (_splitComputation) {
					stashedOutput = stft.output; // save the current output, and read from it
					stft.moveOutput(stft.defaultInterval()); // the actual input jumps forward in time by one interval, ready for the synthesis
				}

				blockProcess.newSpectrum = didSeek || (inputInterval > 0);
				blockProcess.reanalysePrev = didSeek || std::abs(inputInterval - int(stft.defaultInterval())) > 1;
				blockProcess.timeFactor = didSeek ? seekTimeFactor : Sample(stft.defaultInterval())/std::max<Sample>(1, Sample(inputInterval));
				didSeek = false;

				// Latch all voice settings once, for the whole hop
				activeFormantBase = pendingFormantBase;
				for (auto &v : voiceStates) v.latch();

				// Lay out the global step schedule
				size_t cursor = 0;
				blockProcess.analysisSteps = 0;
				if (blockProcess.newSpectrum) {
					if (blockProcess.reanalysePrev) blockProcess.analysisSteps += stft.analyseSteps() + 1;
					blockProcess.analysisSteps += stft.analyseSteps() + 1;
				}
				cursor += blockProcess.analysisSteps;
				for (auto &v : voiceStates) {
					v.procStart = cursor;
					v.procSteps = processSpectrumSteps(v);
					cursor += v.procSteps;
				}
				blockProcess.copyStep = cursor++;
				blockProcess.synthStart = cursor;
				blockProcess.steps = cursor + stft.synthesiseSteps();
			}

			size_t processToStep = newBlock ? blockProcess.steps : 0;
			if (_splitComputation) {
				Sample processRatio = Sample(blockProcess.samplesSinceLast + 1)/stft.defaultInterval();
				// Match the original processor's float literal exactly
				processToStep = std::min<size_t>(blockProcess.steps, size_t(Sample(blockProcess.steps + Sample(0.999f))*processRatio));
			}

			while (blockProcess.step < processToStep) {
				size_t step = blockProcess.step++;
				runStep(step);
			}

			++blockProcess.samplesSinceLast;
			if (_splitComputation) stashedOutput.swap(stft.output);
			for (int v = 0; v < voices; ++v) {
				for (int c = 0; c < channels; ++c) {
					auto &&outputChannel = outputs[v*channels + c];
					Sample value = 0;
					stft.readOutput(v*channels + c, 1, &value);
					outputChannel[outputIndex] = value;
				}
			}
			stft.moveOutput(1);
			if (_splitComputation) stashedOutput.swap(stft.output);
		}

		copyInput(inputSamples);
		prevInputOffset -= inputSamples;
	}

	// Read the remaining output, providing no further input.  `outputSamples`
	// should ideally be at least `.outputLatency()`.
	template<class Outputs>
	void flush(Outputs &&outputs, int outputSamples) {
		int plainOutput = std::min<int>(outputSamples, int(stft.blockSamples()));
		int foldedBackOutput = std::min<int>(outputSamples, int(stft.blockSamples()) - plainOutput);
		stft.finishOutput(1);
		for (int v = 0; v < voices; ++v) {
			for (int c = 0; c < channels; ++c) {
				int sc = v*channels + c;
				auto &&outputChannel = outputs[sc];
				stft.readOutput(sc, plainOutput, tmpBuffer.data());
				for (int i = 0; i < plainOutput; ++i) {
					outputChannel[i] = tmpBuffer[i];
				}
				stft.readOutput(sc, plainOutput, foldedBackOutput, tmpBuffer.data());
				for (int i = 0; i < foldedBackOutput; ++i) {
					outputChannel[outputSamples - 1 - i] -= tmpBuffer[i];
				}
			}
		}
		stft.reset(0.1);

		// Reset the phase-vocoder state of every voice, so the next block gets a fresh start
		for (auto &v : voiceStates) {
			for (int c = 0; c < channels; ++c) {
				Band *bins = v.bandsForChannel(c, bands);
				for (int b = 0; b < bands; ++b) {
					bins[b].prevInput = bins[b].output = 0;
				}
			}
		}
	}

private:
	using STFT = signalsmith::linear::DynamicSTFT<Sample, false, true>;
	using RandomEngineImpl = std::default_random_engine;

	bool _splitComputation = false;
	int channels = 0, voices = 0, bands = 0;

	struct BlockProcess {
		size_t samplesSinceLast = std::numeric_limits<size_t>::max();
		size_t steps = 0;
		size_t step = 0;

		bool newSpectrum = false;
		bool reanalysePrev = false;
		Sample timeFactor = 1;

		size_t analysisSteps = 0;
		size_t copyStep = 0;
		size_t synthStart = 0;
	} blockProcess;

	size_t silenceCounter = 0;
	bool silenceFirst = true;

	int prevInputOffset = -1;
	bool didSeek = false;
	Sample seekTimeFactor = 1;
	Sample pendingFormantBase = 0, activeFormantBase = 0;

	STFT stft;
	typename STFT::Input stashedInput;
	typename STFT::Output stashedOutput;

	// Used only to give each voice the same seeded engine state as a standalone instance
	RandomEngineImpl randomEngine;

	std::vector<Sample> tmpBuffer;

	struct Band {
		Complex input, prevInput{0};
		Complex output{0};
		Sample inputEnergy = 0;
	};
	struct Peak {
		Sample input, output;
	};
	struct PitchMapPoint {
		Sample inputBin, freqGrad;
	};
	struct Prediction {
		Sample energy = 0;
		Complex input;

		Complex makeOutput(Complex phase) {
			Sample phaseNorm = _harmImpl::norm(phase);
			if (phaseNorm <= noiseFloor) {
				phase = input; // prediction is too weak, fall back to the input
				phaseNorm = _harmImpl::norm(input) + noiseFloor;
			}
			return phase*std::sqrt(energy/phaseNorm);
		}
	};

	struct VoiceState {
		// Pending settings (written by setVoice/setFormantBase, latched at hop start)
		Sample pendingFreqMultiplier = 1, pendingFreqTonalityLimit = 1;
		Sample pendingFormantMultiplier = 1, pendingInvFormantMultiplier = 1;
		bool pendingFormantCompensation = false;

		// Latched settings for the hop currently being processed
		Sample freqMultiplier = 1, freqTonalityLimit = 1;
		Sample formantMultiplier = 1, invFormantMultiplier = 1;
		bool formantCompensation = false;
		bool mappedFrequencies = false;
		bool processFormants = false;

		// Step schedule position within the harmonizer's global step sequence
		size_t procStart = 0, procSteps = 0;

		std::vector<Band> channelBands;
		std::vector<Peak> peaks;
		int peakCount = 0;
		std::vector<Sample> energy, smoothedEnergy;
		std::vector<PitchMapPoint> outputMap;
		std::vector<Prediction> channelPredictions;
		std::vector<Sample> formantMetric;
		Sample smoothEnergyState = 0;
		Sample freqEstimateWeighted = 0;
		Sample freqEstimateWeight = 0;
		Sample freqEstimate = 0;
		RandomEngineImpl randomEngine;

		VoiceState() = default;

		void allocate(int bandCount, int channelCount, RandomEngineImpl &engine) {
			// Match a standalone instance's seeded engine state
			randomEngine = engine;
			// Every voice starts at transpose 1, formant 1, compensation off
			pendingFreqMultiplier = freqMultiplier = 1;
			pendingFreqTonalityLimit = freqTonalityLimit = 1;
			pendingFormantMultiplier = formantMultiplier = 1;
			pendingInvFormantMultiplier = invFormantMultiplier = 1;
			pendingFormantCompensation = formantCompensation = false;
			mappedFrequencies = processFormants = false;
			procStart = procSteps = 0;
			channelBands.assign(bandCount*channelCount, Band());
			// Fixed-size storage (no allocation during processing); peakCount tracks the used prefix
			peaks.resize(bandCount + 8);
			peakCount = 0;
			energy.resize(bandCount);
			smoothedEnergy.resize(bandCount);
			outputMap.resize(bandCount);
			channelPredictions.resize(bandCount*channelCount);
			formantMetric.resize(bandCount + 2);
			smoothEnergyState = freqEstimateWeighted = freqEstimateWeight = freqEstimate = 0;
		}

		void resetBands() {
			std::fill(channelBands.begin(), channelBands.end(), Band());
			peakCount = 0;
			freqEstimateWeighted = freqEstimateWeight = 0;
		}

		void latch() {
			freqMultiplier = pendingFreqMultiplier;
			freqTonalityLimit = pendingFreqTonalityLimit;
			formantMultiplier = pendingFormantMultiplier;
			invFormantMultiplier = pendingInvFormantMultiplier;
			formantCompensation = pendingFormantCompensation;
			mappedFrequencies = (freqMultiplier != 1);
			processFormants = formantMultiplier != 1 || (formantCompensation && mappedFrequencies);
		}

		Band * bandsForChannel(int channel, int bandCount) {
			return channelBands.data() + channel*bandCount;
		}
	};

	std::vector<VoiceState> voiceStates;

	// Runs one global processing step (analysis is shared, the rest is per-voice).
	void runStep(size_t step) {
		if (step < blockProcess.analysisSteps) {
			runAnalysisStep(step);
			return;
		}
		for (int v = 0; v < voices; ++v) {
			VoiceState &voice = voiceStates[size_t(v)];
			if (step >= voice.procStart && step < voice.procStart + voice.procSteps) {
				processSpectrum(voice, step - voice.procStart);
				return;
			}
		}
		if (step == blockProcess.copyStep) {
			for (int v = 0; v < voices; ++v) {
				VoiceState &voice = voiceStates[size_t(v)];
				for (int c = 0; c < channels; ++c) {
					Band *bins = voice.bandsForChannel(c, bands);
					Complex *spectrumBands = stft.spectrum(v*channels + c);
					for (int b = 0; b < bands; ++b) {
						spectrumBands[b] = bins[b].output;
					}
				}
			}
			return;
		}
		size_t synthStep = step - blockProcess.synthStart;
		stft.synthesiseStep(synthStep);
	}

	// Shared forward analysis: each forward FFT runs once per channel, then the
	// spectrum is copied into every voice's private band storage.
	void runAnalysisStep(size_t step) {
		size_t analyseSteps = stft.analyseSteps(); // == channels (non-split STFT)
		bool doingPrev = blockProcess.reanalysePrev;
		size_t prevSection = doingPrev ? analyseSteps + 1 : 0;

		if (doingPrev && step < prevSection) {
			if (step < analyseSteps) {
				stashedInput.swap(stft.input);
				stft.analyseStep(step, stft.defaultInterval());
				stashedInput.swap(stft.input);
				return;
			}
			// Copy the re-analysed spectrum into every voice's prevInput
			for (int v = 0; v < voices; ++v) {
				VoiceState &voice = voiceStates[size_t(v)];
				for (int c = 0; c < channels; ++c) {
					Band *bins = voice.bandsForChannel(c, bands);
					Complex *spectrumBands = stft.spectrum(c);
					for (int b = 0; b < bands; ++b) {
						bins[b].prevInput = spectrumBands[b];
					}
				}
			}
			return;
		}

		size_t local = step - prevSection;
		if (local < analyseSteps) {
			stashedInput.swap(stft.input);
			stft.analyseStep(local);
			stashedInput.swap(stft.input);
			return;
		}
		// Copy the analysed spectrum into every voice's input bands
		for (int v = 0; v < voices; ++v) {
			VoiceState &voice = voiceStates[size_t(v)];
			for (int c = 0; c < channels; ++c) {
				Band *bins = voice.bandsForChannel(c, bands);
				Complex *spectrumBands = stft.spectrum(c);
				for (int b = 0; b < bands; ++b) {
					bins[b].input = spectrumBands[b];
				}
			}
		}
	}

	Sample bandToFreq(Sample b) const {
		return stft.binToFreq(b);
	}
	Sample freqToBand(Sample f) const {
		return stft.freqToBin(f);
	}

	template<Complex Band::*member>
	Complex getBand(const VoiceState &voice, int channel, int index) const {
		if (index < 0 || index >= bands) return 0;
		return voice.channelBands[size_t(index + channel*bands)].*member;
	}
	template<Complex Band::*member>
	Complex getFractional(const VoiceState &voice, int channel, int lowIndex, Sample fractional) const {
		Complex low = getBand<member>(voice, channel, lowIndex);
		Complex high = getBand<member>(voice, channel, lowIndex + 1);
		return low + (high - low)*fractional;
	}
	template<Complex Band::*member>
	Complex getFractional(const VoiceState &voice, int channel, Sample inputIndex) const {
		int lowIndex = int(std::floor(inputIndex));
		Sample fracIndex = inputIndex - lowIndex;
		return getFractional<member>(voice, channel, lowIndex, fracIndex);
	}
	template<Sample Band::*member>
	Sample getBand(const VoiceState &voice, int channel, int index) const {
		if (index < 0 || index >= bands) return 0;
		return voice.channelBands[size_t(index + channel*bands)].*member;
	}
	template<Sample Band::*member>
	Sample getFractional(const VoiceState &voice, int channel, int lowIndex, Sample fractional) const {
		Sample low = getBand<member>(voice, channel, lowIndex);
		Sample high = getBand<member>(voice, channel, lowIndex + 1);
		return low + (high - low)*fractional;
	}
	template<Sample Band::*member>
	Sample getFractional(const VoiceState &voice, int channel, Sample inputIndex) const {
		int lowIndex = int(std::floor(inputIndex));
		Sample fracIndex = inputIndex - lowIndex;
		return getFractional<member>(voice, channel, lowIndex, fracIndex);
	}

	Prediction * predictionsForChannel(VoiceState &voice, int c) {
		return voice.channelPredictions.data() + c*bands;
	}
	const Prediction * predictionsForChannel(const VoiceState &voice, int c) const {
		return voice.channelPredictions.data() + c*bands;
	}

	static constexpr size_t splitMainPrediction = 8;
	static constexpr size_t smoothEnergySteps = 3;

	size_t processSpectrumSteps(VoiceState &voice) const {
		size_t result = 0;
		if (blockProcess.newSpectrum) result += channels;
		if (voice.mappedFrequencies) {
			result += smoothEnergySteps;
			result += 1; // findPeaks
		}
		result += 1; // updating the output map
		result += channels; // preliminary phase-vocoder prediction
		result += splitMainPrediction;
		if (blockProcess.newSpectrum) result += 1; // .input -> .prevInput
		if (voice.processFormants) result += 3;
		return result;
	}

	Sample mapFreq(const VoiceState &voice, Sample freq) const {
		if (freq > voice.freqTonalityLimit) {
			return freq + (voice.freqMultiplier - 1)*voice.freqTonalityLimit;
		}
		return freq*voice.freqMultiplier;
	}

	Sample invMapFormant(const VoiceState &voice, Sample freq) const {
		if (freq*voice.invFormantMultiplier > voice.freqTonalityLimit) {
			return freq + (1 - voice.formantMultiplier)*voice.freqTonalityLimit;
		}
		return freq*voice.invFormantMultiplier;
	}

	void processSpectrum(VoiceState &voice, size_t step) {
		Sample timeFactor = blockProcess.timeFactor;

		Sample smoothingBins = Sample(stft.fftSamples())/stft.defaultInterval();
		int longVerticalStep = int(std::round(smoothingBins));
		timeFactor = std::max<Sample>(timeFactor, 1/maxCleanStretch);
		bool randomTimeFactor = (timeFactor > maxCleanStretch);
		std::uniform_real_distribution<Sample> timeFactorDist(maxCleanStretch*2*randomTimeFactor - timeFactor, timeFactor);

		if (blockProcess.newSpectrum) {
			if (step < size_t(channels)) {
				int channel = int(step);
				Band *bins = voice.bandsForChannel(channel, bands);

				Complex rot = std::polar(Sample(1), bandToFreq(0)*stft.defaultInterval()*Sample(2*M_PI));
				Sample freqStep = bandToFreq(1) - bandToFreq(0);
				Complex rotStep = std::polar(Sample(1), freqStep*stft.defaultInterval()*Sample(2*M_PI));

				for (int b = 0; b < bands; ++b) {
					auto &bin = bins[b];
					bin.output = _harmImpl::mul(bin.output, rot);
					bin.prevInput = _harmImpl::mul(bin.prevInput, rot);
					rot = _harmImpl::mul(rot, rotStep);
				}
				return;
			}
			step -= channels;
		}
		if (voice.mappedFrequencies) {
			if (step < smoothEnergySteps) {
				smoothEnergy(voice, step, smoothingBins);
				return;
			}
			step -= smoothEnergySteps;
			if (step-- == 0) {
				findPeaks(voice);
				return;
			}
		}
		if (step-- == 0) {
			if (voice.mappedFrequencies) {
				updateOutputMap(voice);
			} else { // we're not pitch-shifting, so no need to find peaks etc.
				for (int c = 0; c < channels; ++c) {
					Band *bins = voice.bandsForChannel(c, bands);
					for (int b = 0; b < bands; ++b) {
						bins[b].inputEnergy = _harmImpl::norm(bins[b].input);
					}
				}

				for (int b = 0; b < bands; ++b) {
					voice.outputMap[b] = {Sample(b), 1};
				}
			}
			return;
		}
		if (voice.processFormants) {
			if (step < 3) {
				updateFormants(voice, step);
				return;
			}
			step -= 3;
		}
		// Preliminary output prediction from phase-vocoder
		if (step < size_t(channels)) {			int c = int(step);
			Band *bins = voice.bandsForChannel(c, bands);
			auto *predictions = predictionsForChannel(voice, c);
			for (int b = 0; b < bands; ++b) {
				auto mapPoint = voice.outputMap[b];
				int lowIndex = int(std::floor(mapPoint.inputBin));
				Sample fracIndex = mapPoint.inputBin - lowIndex;

				Prediction &prediction = predictions[b];
				Sample prevEnergy = prediction.energy;
				prediction.energy = getFractional<&Band::inputEnergy>(voice, c, lowIndex, fracIndex);
				prediction.energy *= std::max<Sample>(0, mapPoint.freqGrad); // scale the energy according to local stretch factor
				prediction.input = getFractional<&Band::input>(voice, c, lowIndex, fracIndex);

				auto &outputBin = bins[b];
				Complex prevInput = getFractional<&Band::prevInput>(voice, c, lowIndex, fracIndex);
				Complex freqTwist = _harmImpl::mul<true>(prediction.input, prevInput);
				Complex phase = _harmImpl::mul(outputBin.output, freqTwist);
				outputBin.output = phase/(std::max(prevEnergy, prediction.energy) + noiseFloor);
			}
			return;
		}
		step -= channels;

		if (step < splitMainPrediction) {
			// Re-predict using phase differences between frequencies
			size_t chunk = step;
			int startB = int(bands*chunk/splitMainPrediction);
			int endB = int(bands*(chunk + 1)/splitMainPrediction);
			for (int b = startB; b < endB; ++b) {
				// Find maximum-energy channel and calculate that
				int maxChannel = 0;
				Sample maxEnergy = predictionsForChannel(voice, 0)[b].energy;
				for (int c = 1; c < channels; ++c) {
					Sample e = predictionsForChannel(voice, c)[b].energy;
					if (e > maxEnergy) {
						maxChannel = c;
						maxEnergy = e;
					}
				}

				auto *predictions = predictionsForChannel(voice, maxChannel);
				auto &prediction = predictions[b];
				auto *bins = voice.bandsForChannel(maxChannel, bands);
				auto &outputBin = bins[b];

				Complex phase = 0;
				auto mapPoint = voice.outputMap[b];

				// Upwards vertical steps
				if (b > 0) {
					Sample binTimeFactor = randomTimeFactor ? timeFactorDist(voice.randomEngine) : timeFactor;
					Complex downInput = getFractional<&Band::input>(voice, maxChannel, mapPoint.inputBin - binTimeFactor);
					Complex shortVerticalTwist = _harmImpl::mul<true>(prediction.input, downInput);

					auto &downBin = bins[b - 1];
					phase += _harmImpl::mul(downBin.output, shortVerticalTwist);

					if (b >= longVerticalStep) {
						Complex longDownInput = getFractional<&Band::input>(voice, maxChannel, mapPoint.inputBin - longVerticalStep*binTimeFactor);
						Complex longVerticalTwist = _harmImpl::mul<true>(prediction.input, longDownInput);

						auto &longDownBin = bins[b - longVerticalStep];
						phase += _harmImpl::mul(longDownBin.output, longVerticalTwist);
					}
				}
				// Downwards vertical steps
				if (b < bands - 1) {
					auto &upPrediction = predictions[b + 1];
					auto &upMapPoint = voice.outputMap[b + 1];

					Sample binTimeFactor = randomTimeFactor ? timeFactorDist(voice.randomEngine) : timeFactor;
					Complex downInput = getFractional<&Band::input>(voice, maxChannel, upMapPoint.inputBin - binTimeFactor);
					Complex shortVerticalTwist = _harmImpl::mul<true>(upPrediction.input, downInput);

					auto &upBin = bins[b + 1];
					phase += _harmImpl::mul<true>(upBin.output, shortVerticalTwist);

					if (b < bands - longVerticalStep) {
						auto &longUpPrediction = predictions[b + longVerticalStep];
						auto &longUpMapPoint = voice.outputMap[b + longVerticalStep];

						Complex longDownInput = getFractional<&Band::input>(voice, maxChannel, longUpMapPoint.inputBin - longVerticalStep*binTimeFactor);
						Complex longVerticalTwist = _harmImpl::mul<true>(longUpPrediction.input, longDownInput);

						auto &longUpBin = bins[b + longVerticalStep];
						phase += _harmImpl::mul<true>(longUpBin.output, longVerticalTwist);
					}
				}

				outputBin.output = prediction.makeOutput(phase);

				// All other bins are locked in phase
				for (int c = 0; c < channels; ++c) {
					if (c != maxChannel) {
						auto &channelBin = voice.bandsForChannel(c, bands)[b];
						auto &channelPrediction = predictionsForChannel(voice, c)[b];

						Complex channelTwist = _harmImpl::mul<true>(channelPrediction.input, prediction.input);
						Complex channelPhase = _harmImpl::mul(outputBin.output, channelTwist);
						channelBin.output = channelPrediction.makeOutput(channelPhase);
					}
				}
			}
			return;
		}
		step -= splitMainPrediction;

		if (blockProcess.newSpectrum) {
			if (step-- == 0) {
				for (auto &bin : voice.channelBands) {
					bin.prevInput = bin.input;
				}
			}
		}
	}

	// Produces smoothed energy across all channels of one voice
	void smoothEnergy(VoiceState &voice, size_t step, Sample smoothingBins) {
		Sample smoothingSlew = 1/(1 + smoothingBins*Sample(0.5));
		if (step-- == 0) {
			for (auto &e : voice.energy) e = 0;
			for (int c = 0; c < channels; ++c) {
				Band *bins = voice.bandsForChannel(c, bands);
				for (int b = 0; b < bands; ++b) {
					Sample e = _harmImpl::norm(bins[b].input);
					bins[b].inputEnergy = e; // Used for interpolating prediction energy
					voice.energy[b] += e;
				}
			}
			for (int b = 0; b < bands; ++b) {
				voice.smoothedEnergy[b] = voice.energy[b];
			}
			voice.smoothEnergyState = 0;
			return;
		}

		// The two other steps are repeated smoothing passes, down and up
		Sample e = voice.smoothEnergyState;
		for (int b = bands - 1; b >= 0; --b) {
			e += (voice.smoothedEnergy[b] - e)*smoothingSlew;
			voice.smoothedEnergy[b] = e;
		}
		for (int b = 0; b < bands; ++b) {
			e += (voice.smoothedEnergy[b] - e)*smoothingSlew;
			voice.smoothedEnergy[b] = e;
		}
		voice.smoothEnergyState = e;
	}

	// Identifies spectral peaks using energy across all channels
	void findPeaks(VoiceState &voice) {
		voice.peakCount = 0;

		int start = 0;
		while (start < bands) {
			if (voice.energy[start] > voice.smoothedEnergy[start]) {
				int end = start;
				Sample bandSum = 0, energySum = 0;
				while (end < bands && voice.energy[end] > voice.smoothedEnergy[end]) {
					bandSum += end*voice.energy[end];
					energySum += voice.energy[end];
					++end;
				}
				Sample avgBand = bandSum/energySum;
				Sample avgFreq = bandToFreq(avgBand);
				voice.peaks[voice.peakCount++] = Peak{avgBand, freqToBand(mapFreq(voice, avgFreq))};
				start = end;
			}
			++start;
		}
	}

	void updateOutputMap(VoiceState &voice) {
		if (voice.peakCount == 0) {
			for (int b = 0; b < bands; ++b) {
				voice.outputMap[b] = {Sample(b), 1};
			}
			return;
		}
		const Peak *peaks = voice.peaks.data();
		Sample bottomOffset = peaks[0].input - peaks[0].output;
		for (int b = 0; b < std::min<int>(bands, int(std::ceil(peaks[0].output))); ++b) {
			voice.outputMap[b] = {b + bottomOffset, 1};
		}
		// Interpolate between points
		for (int p = 1; p < voice.peakCount; ++p) {
			const Peak &prev = peaks[p - 1], &next = peaks[p];
			Sample rangeScale = 1/(next.output - prev.output);
			Sample outOffset = prev.input - prev.output;
			Sample outScale = next.input - next.output - prev.input + prev.output;
			Sample gradScale = outScale*rangeScale;
			int startBin = std::max<int>(0, int(std::ceil(prev.output)));
			int endBin = std::min<int>(bands, int(std::ceil(next.output)));
			for (int b = startBin; b < endBin; ++b) {
				Sample r = (b - prev.output)*rangeScale;
				Sample h = r*r*(3 - 2*r);
				Sample outB = b + outOffset + h*outScale;

				Sample gradH = 6*r*(1 - r);
				Sample gradB = 1 + gradH*gradScale;

				voice.outputMap[b] = {outB, gradB};
			}
		}
		Sample topOffset = peaks[voice.peakCount - 1].input - peaks[voice.peakCount - 1].output;
		for (int b = std::max<int>(0, int(peaks[voice.peakCount - 1].output)); b < bands; ++b) {
			voice.outputMap[b] = {b + topOffset, 1};
		}
	}

	Sample estimateFrequency(VoiceState &voice) {
		// 3 highest peaks in the input
		std::array<int, 3> peakIndices{0, 0, 0};
		for (int b = 1; b < bands - 1; ++b) {
			Sample e = voice.formantMetric[b];
			// local maxima only
			if (e < voice.formantMetric[b - 1] || e <= voice.formantMetric[b + 1]) continue;

			if (e > voice.formantMetric[peakIndices[0]]) {
				if (e > voice.formantMetric[peakIndices[1]]) {
					if (e > voice.formantMetric[peakIndices[2]]) {
						peakIndices = {peakIndices[1], peakIndices[2], b};
					} else {
						peakIndices = {peakIndices[1], b, peakIndices[2]};
					}
				} else {
					peakIndices[0] = b;
				}
			}
		}

		// VERY rough pitch estimation
		int peakEstimate = peakIndices[2];
		if (voice.formantMetric[peakIndices[1]] > voice.formantMetric[peakIndices[2]]*0.1) {
			int diff = std::abs(peakEstimate - peakIndices[1]);
			if (diff > peakEstimate/8 && diff < peakEstimate*7/8) peakEstimate = peakEstimate%diff;
			if (voice.formantMetric[peakIndices[0]] > voice.formantMetric[peakIndices[2]]*0.01) {
				int diff = std::abs(peakEstimate - peakIndices[0]);
				if (diff > peakEstimate/8 && diff < peakEstimate*7/8) peakEstimate = peakEstimate%diff;
			}
		}
		Sample weight = voice.formantMetric[peakIndices[2]];
		// Smooth it out a bit
		voice.freqEstimateWeighted += (peakEstimate*weight - voice.freqEstimateWeighted)*0.25;
		voice.freqEstimateWeight += (weight - voice.freqEstimateWeight)*0.25;

		return voice.freqEstimateWeighted/(voice.freqEstimateWeight + Sample(1e-30));
	}

	void updateFormants(VoiceState &voice, size_t step) {
		if (step-- == 0) {
			for (auto &e : voice.formantMetric) e = 0;
			for (int c = 0; c < channels; ++c) {
				Band *bins = voice.bandsForChannel(c, bands);
				for (int b = 0; b < bands; ++b) {
					voice.formantMetric[b] += bins[b].inputEnergy;
				}
			}

			voice.freqEstimate = freqToBand(activeFormantBase);
			if (activeFormantBase <= 0) voice.freqEstimate = estimateFrequency(voice);

			for (int b = 0; b < bands; ++b) {
				voice.formantMetric[b] = std::sqrt(voice.formantMetric[b]);
			}
		} else if (step-- == 0) {
			Sample slew = 1/(voice.freqEstimate*0.5 + 1);
			Sample e = 0;
			for (size_t repeat = 0; repeat < 2; ++repeat) {
				for (int b = bands - 1; b >= 0; --b) {
					e += (voice.formantMetric[b] - e)*slew;
					voice.formantMetric[b] = e;
				}
				for (int b = 0; b < bands; ++b) {
					e += (voice.formantMetric[b] - e)*slew;
					voice.formantMetric[b] = e;
				}
			}
		} else {
			auto getFormant = [&](Sample band) -> Sample {
				if (band < 0) return 0;
				band = std::min<Sample>(band, bands);
				int floorBand = int(std::floor(band));
				Sample fracBand = band - floorBand;
				Sample low = voice.formantMetric[floorBand], high = voice.formantMetric[floorBand + 1];
				return low + (high - low)*fracBand;
			};

			for (int b = 0; b < bands; ++b) {
				Sample inputF = bandToFreq(b);
				Sample outputF = voice.formantCompensation ? mapFreq(voice, inputF) : inputF;
				outputF = invMapFormant(voice, outputF);

				Sample inputE = voice.formantMetric[b];
				Sample targetE = getFormant(freqToBand(outputF));

				Sample formantRatio = targetE/(inputE + Sample(1e-30));
				Sample energyRatio = formantRatio*formantRatio;

				for (int c = 0; c < channels; ++c) {
					Band *bins = voice.bandsForChannel(c, bands);
					// This is what's used to decide the output energy, so this affects the output
					bins[b].inputEnergy *= energyRatio;
				}
			}
		}
	}
};

}} // namespace
#endif // include guard
