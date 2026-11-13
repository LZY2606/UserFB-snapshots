// Small example for SignalsmithHarmonizer.
//
// Generates a synthetic voice-like signal, runs it through three harmonised
// voices (different pitch/formant settings), and mixes them with different
// gains/pan positions into a stereo 16-bit WAV.  No external audio files.
//
// Build (uses the vendored Linear sources, no downloaded FFT backend):
//   c++ -std=c++11 -O2 -I. -Ivendor/linear/include \
//       examples/harmonizer-example.cpp -o harmonizer-example
//   ./harmonizer-example .task-build/harmonizer-example.wav

#include "signalsmith-harmonizer.h"
#include "cmd/util/wav.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
	using Sample = float;
	const std::string outPath = (argc > 1) ? argv[1] : ".task-build/harmonizer-example.wav";

	const int sampleRate = 48000;
	const int channels = 1, voices = 3;
	const int block = 512, interval = 128;
	const long seed = 2024;

	signalsmith::stretch::SignalsmithHarmonizer<Sample> harmonizer(seed);
	harmonizer.configure(channels, voices, block, interval, /*splitComputation=*/true);
	// Lower fifth, unison, upper octave-ish - each with its own formant touch.
	harmonizer.setVoice(0, Sample(0.75), Sample(0.95), /*compensatePitch=*/false);
	harmonizer.setVoice(1, Sample(1.0),  Sample(1.0),  false);
	harmonizer.setVoice(2, Sample(1.5),  Sample(1.08), true);
	harmonizer.setFormantBase(Sample(180.0/sampleRate));

	// Per-voice mix gains and equal-power pan positions (-1 left .. +1 right).
	const Sample gain[voices] = {Sample(0.45), Sample(0.6), Sample(0.4)};
	const Sample pan[voices]  = {Sample(-0.7), Sample(0.0), Sample(0.7)};

	const long totalSamples = sampleRate * 2; // two seconds
	const int chunk = interval;

	std::vector<Sample> input(chunk);
	std::vector<Sample*> inputPtr(channels);
	inputPtr[0] = input.data();

	Wav wav;
	wav.sampleRate = sampleRate;
	wav.channels = 2;
	wav.samples.reserve(size_t(totalSamples)*2);

	const double twoPi = 6.283185307179586;
	long cursor = 0;
	long produced = 0;

	// Mixes one block of per-voice planar output into the interleaved stereo WAV.
	auto mixBlock = [&](const std::vector<std::vector<Sample>> &outPlanes, int n) {
		for (int i = 0; i < n; ++i) {
			Sample left = 0, right = 0;
			for (int v = 0; v < voices; ++v) {
				Sample angle = pan[v]*Sample(0.25*twoPi);
				Sample panL = std::cos(angle), panR = std::sin(angle);
				Sample x = gain[v]*outPlanes[v][i]; // raw voice level: no limiting/normalisation
				left += panL*x;
				right += panR*x;
			}
			wav.samples.push_back(left);
			wav.samples.push_back(right);
			++produced;
		}
	};

	while (cursor < totalSamples) {
		int n = int(std::min<long>(chunk, totalSamples - cursor));
		for (int i = 0; i < n; ++i) {
			long k = cursor + i;
			// Multi-tone material with a little vibrato and occasional impulses.
			double vibrato = 1 + 0.01*std::sin(twoPi*5.0*k/sampleRate);
			double s = 0.3*std::sin(twoPi*220.0*vibrato*k/sampleRate)
			         + 0.15*std::sin(twoPi*330.0*vibrato*k/sampleRate)
			         + 0.1*std::sin(twoPi*440.0*vibrato*k/sampleRate);
			if (k % (sampleRate/2) == 0) s += 0.4;
			input[i] = Sample(s);
		}

		std::vector<std::vector<Sample>> outPlanes(voices, std::vector<Sample>(n));
		std::vector<Sample*> outPtr(voices);
		for (int v = 0; v < voices; ++v) outPtr[v] = outPlanes[v].data();
		harmonizer.process(inputPtr, n, outPtr, n);
		mixBlock(outPlanes, n);
		cursor += n;
	}

	// Finite-sound ending: feed inputLatency() of silence, then read the tail
	// with flush() (no more than one block of output), exactly like the
	// single-voice API.
	int padded = 0;
	while (padded < harmonizer.inputLatency()) {
		int n = std::min(chunk, harmonizer.inputLatency() - padded);
		for (int i = 0; i < n; ++i) input[i] = 0;
		std::vector<std::vector<Sample>> outPlanes(voices, std::vector<Sample>(n));
		std::vector<Sample*> outPtr(voices);
		for (int v = 0; v < voices; ++v) outPtr[v] = outPlanes[v].data();
		harmonizer.process(inputPtr, n, outPtr, n);
		mixBlock(outPlanes, n);
		padded += n;
	}
	{
		const int tail = harmonizer.blockSamples();
		std::vector<std::vector<Sample>> outPlanes(voices, std::vector<Sample>(tail));
		std::vector<Sample*> outPtr(voices);
		for (int v = 0; v < voices; ++v) outPtr[v] = outPlanes[v].data();
		harmonizer.flush(outPtr, tail);
		mixBlock(outPlanes, tail);
	}

	if (!wav.write(outPath)) {
		std::fprintf(stderr, "failed to write %s\n", outPath.c_str());
		return 1;
	}
	std::printf("wrote %s (%ld stereo frames, %d voices)\n", outPath.c_str(), produced, voices);
	std::printf("inputLatency=%d outputLatency=%d (split mode adds one interval)\n",
		harmonizer.inputLatency(), harmonizer.outputLatency());
	return 0;
}
