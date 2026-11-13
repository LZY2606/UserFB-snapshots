#ifndef SIGNALSMITH_HARMONIZER_H
#define SIGNALSMITH_HARMONIZER_H

#include "signalsmith-linear/stft.h" // https://github.com/Signalsmith-Audio/linear

#include <vector>
#include <array>
#include <algorithm>
#include <functional>
#include <random>
#include <limits>
#include <type_traits>
#include <cmath>

namespace signalsmith { namespace stretch {

namespace harmonizer_impl {
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

/// Multi-voice pitch/formant stretcher with a single shared analysis stage.
///
/// One input spectrum (and, when needed, one re-analysis of the previous
/// position) is computed per channel per synthesis hop, and then mapped and
/// re-synthesised independently by every voice.
template<typename Sample=float, class RandomEngine=void>
struct SignalsmithHarmonizer {
	static constexpr size_t version[3] = {0, 1, 0};

	SignalsmithHarmonizer() : randomEngine(std::random_device{}()) {}
	SignalsmithHarmonizer(long seed) : randomEngine(seed) {}

	int blockSamples() const {
		return int(analysisStft.blockSamples());
	}
	int intervalSamples() const {
		return int(analysisStft.defaultInterval());
	}
	int inputLatency() const {
		return int(analysisStft.analysisLatency());
	}
	int outputLatency() const {
		// Every voice uses the same STFT geometry, so any of them reports the same latency.
		return int(analysisStft.synthesisLatency() + _splitComputation*analysisStft.defaultInterval());
	}

	void reset() {
		analysisStft.reset(0.1);
		stashedInput = analysisStft.input;
		stashedOutput = analysisStft.output;

		prevInputOffset = -1;
		for (auto &voice : voices) voice.resetState();
		silenceCounter = 0;
		blockProcess = {};
		didSeek = false;
	}

	/// Manual setup.  All working memory is allocated here; processing never allocates.
	void configure(int nChannels, int nVoices, int blockSamples, int intervalSamples, bool splitComputation=false) {
		_splitComputation = splitComputation;
		channels = nChannels;
		voicesCount = nVoices;

		analysisStft.configure(channels, channels, blockSamples, intervalSamples + 1);
		analysisStft.setInterval(intervalSamples, analysisStft.kaiser);
		analysisStft.reset(0.1);
		stashedInput = analysisStft.input;
		stashedOutput = analysisStft.output;

		bands = int(analysisStft.bands());
		tmpBuffer.resize(blockSamples + intervalSamples);

		// Every voice starts at transpose = formant = 1 with compensation off,
		// and the shared formant base starts unset (0).
		pendingSettings.assign(voicesCount, VoiceSettings{});
		formantBaseFreq = 0;
		pendingFormantBase = 0;

		voices.clear();
		voices.reserve(voicesCount);
		for (int v = 0; v < voicesCount; ++v) {
			voices.emplace_back(bands, channels, blockSamples, intervalSamples, randomEngine);
		}
		voicePlan.resize(voicesCount);

		blockProcess = {};
	}

	/// Updates one voice.  Takes effect at the start of the next synthesis hop.
	void setVoice(int index, Sample transposeFactor, Sample formantFactor, bool compensatePitch) {
		VoiceSettings &s = pendingSettings[index];
		s.transposeFactor = transposeFactor;
		s.formantFactor = formantFactor;
		s.invFormantFactor = 1/formantFactor;
		s.compensatePitch = compensatePitch;
	}

	/// Sets the formant base frequency (relative to Nyquist), shared by all voices.
	void setFormantBase(Sample baseFreq=0) {
		formantBaseFreq = baseFreq;
		pendingFormantBase = baseFreq;
	}

	// Provide previous input ("pre-roll"), without affecting the speed calculation.
	// The shared input advances exactly once, regardless of the voice count.
	template<class Inputs>
	void seek(Inputs &&inputs, int inputSamples, double playbackRate) {
		int tmpSize = int(analysisStft.blockSamples() + analysisStft.defaultInterval());
		tmpBuffer.resize(tmpSize);

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
			analysisStft.writeInput(c, tmpBuffer.size(), tmpBuffer.data());
		}
		analysisStft.moveInput(tmpBuffer.size());
		if (totalEnergy >= noiseFloor) {
			silenceCounter = 0;
			silenceFirst = true;
		}
		didSeek = true;
		seekTimeFactor = (playbackRate*analysisStft.defaultInterval() > 1) ? 1/playbackRate : analysisStft.defaultInterval();
	}

	template<class Inputs, class Outputs>
	void process(Inputs &&inputs, int inputSamples, Outputs &&outputs, int outputSamples) {
		int prevCopiedInput = 0;
		auto copyInput = [&](int toIndex) {
			int length = std::min<int>(int(analysisStft.blockSamples() + analysisStft.defaultInterval()), toIndex - prevCopiedInput);
			tmpBuffer.resize(length);
			int offset = toIndex - length;
			for (int c = 0; c < channels; ++c) {
				auto &&inputBuffer = inputs[c];
				for (int i = 0; i < length; ++i) {
					tmpBuffer[i] = inputBuffer[i + offset];
				}
				analysisStft.writeInput(c, length, tmpBuffer.data());
			}
			analysisStft.moveInput(length);
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
			if (silenceCounter >= 2*int(analysisStft.blockSamples())) {
				if (silenceFirst) { // first block of silence processing
					silenceFirst = false;
					blockProcess = {};
					for (auto &voice : voices) voice.clearBands();
				}

				// Every voice passes the input through unchanged, just like the single-voice API.
				if (inputSamples > 0) {
					for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
						int inputIndex = outputIndex%inputSamples;
						for (int v = 0; v < voicesCount; ++v) {
							for (int c = 0; c < channels; ++c) {
								outputs[v*channels + c][outputIndex] = inputs[c][inputIndex];
							}
						}
					}
				} else {
					for (int v = 0; v < voicesCount; ++v) {
						for (int c = 0; c < channels; ++c) {
							auto &&outputChannel = outputs[v*channels + c];
							for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
								outputChannel[outputIndex] = 0;
							}
						}
					}
				}

				// Store input in the shared history buffer, once.
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
			bool newBlock = blockProcess.samplesSinceLast >= analysisStft.defaultInterval();
			if (newBlock) {
				blockProcess.samplesSinceLast = 0;

				// Time to process a spectrum!  Where should it come from in the input?
				int inputOffset = std::round(outputIndex*Sample(inputSamples)/outputSamples);
				int inputInterval = inputOffset - prevInputOffset;
				prevInputOffset = inputOffset;

				copyInput(inputOffset);
				stashedInput = analysisStft.input; // save the input state, since that's what we'll analyse later
				if (_splitComputation) {
					// Copy (not swap): both states must start the hop identical;
					// the vectors keep their configure()-time capacity, so equal-size
					// assign() calls overwrite in place and never touch the heap.
					stashedOutput = analysisStft.output;
					analysisStft.moveOutput(analysisStft.defaultInterval()); // the output jumps forward, ready for synthesis
					// Every voice's synthesis output gets the same one-interval
					// advance, preserving the extra interval of split latency.
					for (auto &voice : voices) {
						voice.stashedOutput = voice.stft.output;
						voice.stft.moveOutput(analysisStft.defaultInterval());
					}
				}

				blockProcess.newSpectrum = didSeek || (inputInterval > 0);
				blockProcess.reanalysePrev = blockProcess.newSpectrum && (didSeek || std::abs(inputInterval - int(analysisStft.defaultInterval())) > 1);

				blockProcess.timeFactor = didSeek ? seekTimeFactor : analysisStft.defaultInterval()/std::max<Sample>(1, inputInterval);
				didSeek = false;

				// Latch every voice's settings once, at the start of the synthesis hop.
				for (int v = 0; v < voicesCount; ++v) {
					voices[v].latch(pendingSettings[v], pendingFormantBase);
				}

				// Shared analysis steps (identical for every voice).
				size_t analysisSteps = 0;
				if (blockProcess.newSpectrum) {
					if (blockProcess.reanalysePrev) analysisSteps += analysisStft.analyseSteps() + 1;
					analysisSteps += analysisStft.analyseSteps() + 1;
				}
				analysisTotalSteps = analysisSteps;
				analysisStep = 0;

				// Each voice keeps the same unified step numbering a single-voice
				// instance would use (shared analysis + private processing + synth),
				// so the split-computation ratio lands on exactly the same steps.
				for (int v = 0; v < voicesCount; ++v) {
					Voice &voice = voices[v];
					voicePlan[v].steps = analysisSteps
						+ processSpectrumStepCount(voice, blockProcess.newSpectrum)
						+ 1 // copy band objects into the spectrum
						+ voice.stft.synthesiseSteps();
					voicePlan[v].step = 0;
					voicePlan[v].targetStep = 0;
				}
			}

			// Every voice computes its own target with the original single-voice
			// formula, based on its own total step count.
			for (int v = 0; v < voicesCount; ++v) {
				size_t target = newBlock ? voicePlan[v].steps : 0;
				if (_splitComputation) {
					Sample processRatio = Sample(blockProcess.samplesSinceLast + 1)/analysisStft.defaultInterval();
					target = std::min<size_t>(voicePlan[v].steps, (voicePlan[v].steps + 0.999f)*processRatio);
				}
				voicePlan[v].targetStep = target;
			}

			// The shared analysis only needs to run as far as the neediest voice.
			// Like everything else it is spread across the interval in split mode.
			size_t analysisNeeded = 0;
			for (int v = 0; v < voicesCount; ++v) {
				analysisNeeded = std::max(analysisNeeded, std::min(analysisTotalSteps, voicePlan[v].targetStep));
			}
			while (analysisStep < analysisNeeded) {
				runAnalysisStep(analysisStep++);
			}

			// Per-voice private processing and synthesis, each on its own cursor.
			for (int v = 0; v < voicesCount; ++v) {
				while (voicePlan[v].step < voicePlan[v].targetStep) {
					size_t step = voicePlan[v].step++;
					if (step < analysisTotalSteps) continue; // shared analysis, already done once
					runVoiceStep(v, step - analysisTotalSteps);
				}
			}

			++blockProcess.samplesSinceLast;
			if (_splitComputation) stashedOutput.swap(analysisStft.output);
			for (int v = 0; v < voicesCount; ++v) {
				Voice &voice = voices[v];
				if (_splitComputation) voice.stashedOutput.swap(voice.stft.output);
				for (int c = 0; c < channels; ++c) {
					Sample value = 0;
					voice.stft.readOutput(c, 1, &value);
					outputs[v*channels + c][outputIndex] = value;
				}
				voice.stft.moveOutput(1);
				if (_splitComputation) voice.stashedOutput.swap(voice.stft.output);
			}
			if (_splitComputation) stashedOutput.swap(analysisStft.output);
		}

		copyInput(inputSamples);
		prevInputOffset -= inputSamples;
	}

	// Read the remaining output from every voice, providing no further input.
	// `outputSamples` should ideally be at least `.outputLatency()`.
	template<class Outputs>
	void flush(Outputs &&outputs, int outputSamples) {
		int plainOutput = std::min<int>(outputSamples, int(analysisStft.blockSamples()));
		int foldedBackOutput = std::min<int>(outputSamples, int(analysisStft.blockSamples()) - plainOutput);

		// The single-voice API calls stft.reset() here on its combined STFT.
		// Mirror it on both roles: reset the shared analysis STFT (input ring and
		// spectrum) and, below, every voice's synthesis STFT.  The stashed states
		// are deliberately left untouched - they are re-assigned from the reset
		// STFTs at the start of the next hop, exactly as in the single-voice class.
		analysisStft.reset(0.1);

		for (int v = 0; v < voicesCount; ++v) {
			Voice &voice = voices[v];
			voice.stft.finishOutput(1);
			for (int c = 0; c < channels; ++c) {
				tmpBuffer.resize(plainOutput);
				voice.stft.readOutput(c, plainOutput, tmpBuffer.data());
				auto &&outputChannel = outputs[v*channels + c];
				for (int i = 0; i < plainOutput; ++i) {
					outputChannel[i] = tmpBuffer[i];
				}
				tmpBuffer.resize(foldedBackOutput);
				voice.stft.readOutput(c, plainOutput, foldedBackOutput, tmpBuffer.data());
				for (int i = 0; i < foldedBackOutput; ++i) {
					outputChannel[outputSamples - 1 - i] -= tmpBuffer[i];
				}
			}
			voice.stft.reset(0.1);

			// Reset the phase-vocoder state, so the next block gets a fresh start.
			for (int c = 0; c < channels; ++c) {
				Band *channelBands = voice.bandsForChannel(c);
				for (int b = 0; b < bands; ++b) {
					channelBands[b].prevInput = channelBands[b].output = 0;
				}
			}
		}
	}

private:
	using Complex = std::complex<Sample>;
	using STFT = signalsmith::linear::DynamicSTFT<Sample, false, true>;
	static constexpr Sample noiseFloor{1e-15};
	static constexpr Sample maxCleanStretch{2}; // time-stretch ratio before we start randomising phases

	bool _splitComputation = false;
	struct {
		size_t samplesSinceLast = std::numeric_limits<size_t>::max();

		bool newSpectrum = false;
		bool reanalysePrev = false;
		Sample timeFactor = 1;
	} blockProcess;

	size_t silenceCounter = 0;
	bool silenceFirst = true;

	int channels = 0, voicesCount = 0, bands = 0;
	int prevInputOffset = -1;
	bool didSeek = false;
	Sample seekTimeFactor = 1;

	// If RandomEngine=void, use std::default_random_engine;
	using RandomEngineImpl = typename std::conditional<
		std::is_void<RandomEngine>::value,
		std::default_random_engine,
		RandomEngine
	>::type;
	RandomEngineImpl randomEngine;

	struct VoiceSettings {
		Sample transposeFactor = 1;
		Sample formantFactor = 1, invFormantFactor = 1;
		bool compensatePitch = false;
	};
	std::vector<VoiceSettings> pendingSettings;
	Sample pendingFormantBase = 0;
	Sample formantBaseFreq = 0;

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
			Sample phaseNorm = harmonizer_impl::norm(phase);
			if (phaseNorm <= noiseFloor) {
				phase = input; // prediction is too weak, fall back to the input
				phaseNorm = harmonizer_impl::norm(input) + noiseFloor;
			}
			return phase*std::sqrt(energy/phaseNorm);
		}
	};

	struct Voice {
		STFT stft;
		typename STFT::Output stashedOutput;
		RandomEngineImpl randomEngine;

		std::vector<Band> channelBands;
		std::vector<Peak> peaks;
		std::vector<Sample> energy, smoothedEnergy;
		std::vector<PitchMapPoint> outputMap;
		std::vector<Prediction> channelPredictions;
		std::vector<Sample> formantMetric;
		Sample smoothEnergyState = 0;
		Sample freqEstimateWeighted = 0;
		Sample freqEstimateWeight = 0;
		Sample freqEstimate = 0;

		// Latched at the start of each synthesis hop.
		VoiceSettings settings;
		Sample formantBaseFreq = 0;
		bool mappedFrequencies = false;
		bool processFormants = false;

		int bands, channels;

		Voice(int nBands, int nChannels, int blockSamples, int intervalSamples, const RandomEngineImpl &engine)
				: randomEngine(engine), bands(nBands), channels(nChannels) {
			stft.configure(channels, channels, blockSamples, intervalSamples + 1);
			stft.setInterval(intervalSamples, stft.kaiser);
			stft.reset(0.1);
			stashedOutput = stft.output;

			channelBands.assign(bands*channels, Band());
			peaks.reserve(bands); // upper bound, so processing never has to grow this
			energy.resize(bands);
			smoothedEnergy.resize(bands);
			outputMap.resize(bands);
			channelPredictions.resize(channels*bands);
			formantMetric.resize(bands + 2);
		}

		void resetState() {
			stft.reset(0.1);
			stashedOutput = stft.output;
			for (auto &b : channelBands) b = Band();
			smoothEnergyState = 0;
			freqEstimateWeighted = freqEstimateWeight = freqEstimate = 0;
		}
		void clearBands() {
			for (auto &b : channelBands) {
				b.input = b.prevInput = b.output = 0;
				b.inputEnergy = 0;
			}
		}
		void latch(const VoiceSettings &pending, Sample baseFreq) {
			settings = pending;
			formantBaseFreq = baseFreq;
			mappedFrequencies = (settings.transposeFactor != 1);
			processFormants = settings.formantFactor != 1 || (settings.compensatePitch && mappedFrequencies);
		}

		Band * bandsForChannel(int channel) {
			return channelBands.data() + channel*bands;
		}
	};

	// The shared analysis STFT and its saved state.
	STFT analysisStft;
	typename STFT::Input stashedInput;
	typename STFT::Output stashedOutput;

	std::vector<Voice> voices;
	struct VoicePlan {
		size_t steps = 0;   // total steps for this voice this hop
		size_t step = 0;    // steps already performed
		size_t targetStep = 0;
	};
	std::vector<VoicePlan> voicePlan;
	// Shared analysis cursor for the current hop (analysis is identical per voice).
	size_t analysisTotalSteps = 0;
	size_t analysisStep = 0;

	std::vector<Sample> tmpBuffer;

	Sample bandToFreq(Sample b) const {
		return analysisStft.binToFreq(b);
	}
	Sample freqToBand(Sample f) const {
		return analysisStft.freqToBin(f);
	}

	// Runs one step of the shared analysis, and distributes the resulting spectra
	// into every voice's private band objects.
	void runAnalysisStep(size_t step) {
		size_t analyseSteps = analysisStft.analyseSteps();

		if (blockProcess.reanalysePrev) {
			// analyse past input
			if (step < analyseSteps) {
				stashedInput.swap(analysisStft.input);
				analysisStft.analyseStep(step, analysisStft.defaultInterval());
				stashedInput.swap(analysisStft.input);
				return;
			}
			step -= analyseSteps;
			if (step < 1) {
				// Copy previous analysis into each voice's band objects
				for (int v = 0; v < voicesCount; ++v) {
					for (int c = 0; c < channels; ++c) {
						Band *channelBands = voices[v].channelBands.data() + c*bands;
						auto *spectrumBands = analysisStft.spectrum(c);
						for (int b = 0; b < bands; ++b) {
							channelBands[b].prevInput = spectrumBands[b];
						}
					}
				}
				return;
			}
			step -= 1;
		}

		// Analyse latest (stashed) input
		if (step < analyseSteps) {
			stashedInput.swap(analysisStft.input);
			analysisStft.analyseStep(step);
			stashedInput.swap(analysisStft.input);
			return;
		}
		step -= analyseSteps;
		if (step < 1) {
			// Copy analysed spectrum into each voice's band objects
			for (int v = 0; v < voicesCount; ++v) {
				for (int c = 0; c < channels; ++c) {
					Band *channelBands = voices[v].channelBands.data() + c*bands;
					auto *spectrumBands = analysisStft.spectrum(c);
					for (int b = 0; b < bands; ++b) {
						channelBands[b].input = spectrumBands[b];
					}
				}
			}
		}
	}

	// Runs one step of one voice's private processing/synthesis pipeline.
	void runVoiceStep(int voiceIndex, size_t step) {
		Voice &voice = voices[voiceIndex];
		size_t processSteps = processSpectrumStepCount(voice, blockProcess.newSpectrum);

		if (step < processSteps) {
			processSpectrum(voice, step);
			return;
		}
		step -= processSteps;

		if (step < 1) {
			// Copy band objects into this voice's synthesis spectrum
			for (int c = 0; c < channels; ++c) {
				Band *channelBands = voice.channelBands.data() + c*bands;
				auto *spectrumBands = voice.stft.spectrum(c);
				for (int b = 0; b < bands; ++b) {
					spectrumBands[b] = channelBands[b].output;
				}
			}
			return;
		}
		step -= 1;

		if (step < voice.stft.synthesiseSteps()) {
			voice.stft.synthesiseStep(step);
		}
	}

	template<Complex Band::*member>
	Complex getBand(Voice &voice, int channel, int index) {
		if (index < 0 || index >= bands) return 0;
		return voice.channelBands[index + channel*bands].*member;
	}
	template<Complex Band::*member>
	Complex getFractional(Voice &voice, int channel, int lowIndex, Sample fractional) {
		Complex low = getBand<member>(voice, channel, lowIndex);
		Complex high = getBand<member>(voice, channel, lowIndex + 1);
		return low + (high - low)*fractional;
	}
	template<Complex Band::*member>
	Complex getFractional(Voice &voice, int channel, Sample inputIndex) {
		int lowIndex = std::floor(inputIndex);
		Sample fracIndex = inputIndex - lowIndex;
		return getFractional<member>(voice, channel, lowIndex, fracIndex);
	}
	template<Sample Band::*member>
	Sample getBand(Voice &voice, int channel, int index) {
		if (index < 0 || index >= bands) return 0;
		return voice.channelBands[index + channel*bands].*member;
	}
	template<Sample Band::*member>
	Sample getFractional(Voice &voice, int channel, int lowIndex, Sample fractional) {
		Sample low = getBand<member>(voice, channel, lowIndex);
		Sample high = getBand<member>(voice, channel, lowIndex + 1);
		return low + (high - low)*fractional;
	}
	template<Sample Band::*member>
	Sample getFractional(Voice &voice, int channel, Sample inputIndex) {
		int lowIndex = std::floor(inputIndex);
		Sample fracIndex = inputIndex - lowIndex;
		return getFractional<member>(voice, channel, lowIndex, fracIndex);
	}

	static constexpr size_t splitMainPrediction = 8; // it's just heavy, since we're blending up to 4 different phase predictions
	static constexpr size_t smoothEnergySteps = 3;

	Sample mapFreq(const Voice &voice, Sample freq) const {
		// No tonality limit is exposed by the harmonizer API, so this is a pure multiplier.
		return freq*voice.settings.transposeFactor;
	}

	size_t processSpectrumStepCount(Voice &voice, bool newSpectrum) {
		size_t steps = 0;
		if (newSpectrum) steps += channels;
		if (voice.mappedFrequencies) {
			steps += smoothEnergySteps;
			steps += 1; // findPeaks
		}
		steps += 1; // updating the output map
		steps += channels; // preliminary phase-vocoder prediction
		steps += splitMainPrediction;
		if (newSpectrum) steps += 1; // .input -> .prevInput
		if (voice.processFormants) steps += 3;
		return steps;
	}

	void processSpectrum(Voice &voice, size_t step) {
		Sample timeFactor = blockProcess.timeFactor;

		Sample smoothingBins = Sample(analysisStft.fftSamples())/analysisStft.defaultInterval();
		int longVerticalStep = std::round(smoothingBins);
		timeFactor = std::max<Sample>(timeFactor, 1/maxCleanStretch);
		bool randomTimeFactor = (timeFactor > maxCleanStretch);
		std::uniform_real_distribution<Sample> timeFactorDist(maxCleanStretch*2*randomTimeFactor - timeFactor, timeFactor);

		if (blockProcess.newSpectrum) {
			if (step < size_t(channels)) {
				int channel = int(step);
				Band *bins = voice.channelBands.data() + channel*bands;

				Complex rot = std::polar(Sample(1), bandToFreq(0)*analysisStft.defaultInterval()*Sample(2*M_PI));
				Sample freqStep = bandToFreq(1) - bandToFreq(0);
				Complex rotStep = std::polar(Sample(1), freqStep*analysisStft.defaultInterval()*Sample(2*M_PI));

				for (int b = 0; b < bands; ++b) {
					auto &bin = bins[b];
					bin.output = harmonizer_impl::mul(bin.output, rot);
					bin.prevInput = harmonizer_impl::mul(bin.prevInput, rot);
					rot = harmonizer_impl::mul(rot, rotStep);
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
					Band *bins = voice.channelBands.data() + c*bands;
					for (int b = 0; b < bands; ++b) {
						bins[b].inputEnergy = harmonizer_impl::norm(bins[b].input);
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
		if (step < size_t(channels)) {
			int c = int(step);
			Band *bins = voice.channelBands.data() + c*bands;
			auto *predictions = voice.channelPredictions.data() + c*bands;
			for (int b = 0; b < bands; ++b) {
				auto mapPoint = voice.outputMap[b];
				int lowIndex = std::floor(mapPoint.inputBin);
				Sample fracIndex = mapPoint.inputBin - lowIndex;

				Prediction &prediction = predictions[b];
				Sample prevEnergy = prediction.energy;
				prediction.energy = getFractional<&Band::inputEnergy>(voice, c, lowIndex, fracIndex);
				prediction.energy *= std::max<Sample>(0, mapPoint.freqGrad); // scale the energy according to local stretch factor
				prediction.input = getFractional<&Band::input>(voice, c, lowIndex, fracIndex);

				auto &outputBin = bins[b];
				Complex prevInput = getFractional<&Band::prevInput>(voice, c, lowIndex, fracIndex);
				Complex freqTwist = harmonizer_impl::mul<true>(prediction.input, prevInput);
				Complex phase = harmonizer_impl::mul(outputBin.output, freqTwist);
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
				Sample maxEnergy = voice.channelPredictions[b].energy;
				for (int c = 1; c < channels; ++c) {
					Sample e = voice.channelPredictions[c*bands + b].energy;
					if (e > maxEnergy) {
						maxChannel = c;
						maxEnergy = e;
					}
				}

				auto *predictions = voice.channelPredictions.data() + maxChannel*bands;
				auto &prediction = predictions[b];
				auto *bins = voice.channelBands.data() + maxChannel*bands;
				auto &outputBin = bins[b];

				Complex phase = 0;
				auto mapPoint = voice.outputMap[b];

				// Upwards vertical steps
				if (b > 0) {
					Sample binTimeFactor = randomTimeFactor ? timeFactorDist(voice.randomEngine) : timeFactor;
					Complex downInput = getFractional<&Band::input>(voice, maxChannel, mapPoint.inputBin - binTimeFactor);
					Complex shortVerticalTwist = harmonizer_impl::mul<true>(prediction.input, downInput);

					auto &downBin = bins[b - 1];
					phase += harmonizer_impl::mul(downBin.output, shortVerticalTwist);

					if (b >= longVerticalStep) {
						Complex longDownInput = getFractional<&Band::input>(voice, maxChannel, mapPoint.inputBin - longVerticalStep*binTimeFactor);
						Complex longVerticalTwist = harmonizer_impl::mul<true>(prediction.input, longDownInput);

						auto &longDownBin = bins[b - longVerticalStep];
						phase += harmonizer_impl::mul(longDownBin.output, longVerticalTwist);
					}
				}
				// Downwards vertical steps
				if (b < bands - 1) {
					auto &upPrediction = predictions[b + 1];
					auto &upMapPoint = voice.outputMap[b + 1];

					Sample binTimeFactor = randomTimeFactor ? timeFactorDist(voice.randomEngine) : timeFactor;
					Complex downInput = getFractional<&Band::input>(voice, maxChannel, upMapPoint.inputBin - binTimeFactor);
					Complex shortVerticalTwist = harmonizer_impl::mul<true>(upPrediction.input, downInput);

					auto &upBin = bins[b + 1];
					phase += harmonizer_impl::mul<true>(upBin.output, shortVerticalTwist);

					if (b < bands - longVerticalStep) {
						auto &longUpPrediction = predictions[b + longVerticalStep];
						auto &longUpMapPoint = voice.outputMap[b + longVerticalStep];

						Complex longDownInput = getFractional<&Band::input>(voice, maxChannel, longUpMapPoint.inputBin - longVerticalStep*binTimeFactor);
						Complex longVerticalTwist = harmonizer_impl::mul<true>(longUpPrediction.input, longDownInput);

						auto &longUpBin = bins[b + longVerticalStep];
						phase += harmonizer_impl::mul<true>(longUpBin.output, longVerticalTwist);
					}
				}

				outputBin.output = prediction.makeOutput(phase);

				// All other bins are locked in phase
				for (int c = 0; c < channels; ++c) {
					if (c != maxChannel) {
						auto &channelBin = voice.channelBands[c*bands + b];
						auto &channelPrediction = voice.channelPredictions[c*bands + b];

						Complex channelTwist = harmonizer_impl::mul<true>(channelPrediction.input, prediction.input);
						Complex channelPhase = harmonizer_impl::mul(outputBin.output, channelTwist);
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

	// Produces smoothed energy across all channels (of one voice)
	void smoothEnergy(Voice &voice, size_t step, Sample smoothingBins) {
		Sample smoothingSlew = 1/(1 + smoothingBins*Sample(0.5));
		if (step-- == 0) {
			for (auto &e : voice.energy) e = 0;
			for (int c = 0; c < channels; ++c) {
				Band *bins = voice.channelBands.data() + c*bands;
				for (int b = 0; b < bands; ++b) {
					Sample e = harmonizer_impl::norm(bins[b].input);
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
	void findPeaks(Voice &voice) {
		voice.peaks.resize(0);

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
				voice.peaks.emplace_back(Peak{avgBand, freqToBand(mapFreq(voice, avgFreq))});

				start = end;
			}
			++start;
		}
	}

	void updateOutputMap(Voice &voice) {
		if (voice.peaks.empty()) {
			for (int b = 0; b < bands; ++b) {
				voice.outputMap[b] = {Sample(b), 1};
			}
			return;
		}
		Sample bottomOffset = voice.peaks[0].input - voice.peaks[0].output;
		for (int b = 0; b < std::min<int>(bands, std::ceil(voice.peaks[0].output)); ++b) {
			voice.outputMap[b] = {b + bottomOffset, 1};
		}
		// Interpolate between points
		for (size_t p = 1; p < voice.peaks.size(); ++p) {
			const Peak &prev = voice.peaks[p - 1], &next = voice.peaks[p];
			Sample rangeScale = 1/(next.output - prev.output);
			Sample outOffset = prev.input - prev.output;
			Sample outScale = next.input - next.output - prev.input + prev.output;
			Sample gradScale = outScale*rangeScale;
			int startBin = std::max<int>(0, std::ceil(prev.output));
			int endBin = std::min<int>(bands, std::ceil(next.output));
			for (int b = startBin; b < endBin; ++b) {
				Sample r = (b - prev.output)*rangeScale;
				Sample h = r*r*(3 - 2*r);
				Sample outB = b + outOffset + h*outScale;

				Sample gradH = 6*r*(1 - r);
				Sample gradB = 1 + gradH*gradScale;

				voice.outputMap[b] = {outB, gradB};
			}
		}
		Sample topOffset = voice.peaks.back().input - voice.peaks.back().output;
		for (int b = std::max<int>(0, voice.peaks.back().output); b < bands; ++b) {
			voice.outputMap[b] = {b + topOffset, 1};
		}
	}

	// If we mapped formants the same way as mapFreq(), this would be the inverse
	Sample invMapFormant(const Voice &voice, Sample freq) const {
		return freq*voice.settings.invFormantFactor;
	}

	Sample estimateFrequency(Voice &voice) {
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

	void updateFormants(Voice &voice, size_t step) {
		if (step-- == 0) {
			for (auto &e : voice.formantMetric) e = 0;
			for (int c = 0; c < channels; ++c) {
				Band *bins = voice.channelBands.data() + c*bands;
				for (int b = 0; b < bands; ++b) {
					voice.formantMetric[b] += bins[b].inputEnergy;
				}
			}

			voice.freqEstimate = freqToBand(voice.formantBaseFreq);
			if (voice.formantBaseFreq <= 0) voice.freqEstimate = estimateFrequency(voice);

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
				int floorBand = std::floor(band);
				Sample fracBand = band - floorBand;
				Sample low = voice.formantMetric[floorBand], high = voice.formantMetric[floorBand + 1];
				return low + (high - low)*fracBand;
			};

			for (int b = 0; b < bands; ++b) {
				Sample inputF = bandToFreq(b);
				Sample outputF = voice.settings.compensatePitch ? mapFreq(voice, inputF) : inputF;
				outputF = invMapFormant(voice, outputF);

				Sample inputE = voice.formantMetric[b];
				Sample targetE = getFormant(freqToBand(outputF));

				Sample formantRatio = targetE/(inputE + Sample(1e-30));
				Sample energyRatio = formantRatio*formantRatio;

				for (int c = 0; c < channels; ++c) {
					Band *bins = voice.channelBands.data() + c*bands;
					// This is what's used to decide the output energy, so this affects the output
					bins[b].inputEnergy *= energyRatio;
				}
			}
		}
	}
};

}} // namespace
#endif // include guard
