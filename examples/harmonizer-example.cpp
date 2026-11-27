// Small end-to-end example for SignalsmithHarmonizer.
//
// Generates a synthetic voice-like signal, runs three harmonised voices from a
// single shared analysis, then mixes the un-normalised voices with per-voice
// gains and pan positions into a stereo 16-bit WAV.  No external audio files.
//
// Build:
//   c++ -std=c++11 -O2 -I. -Ivendor/linear/include examples/harmonizer-example.cpp -o harmonizer-example
// Run:
//   ./harmonizer-example harmonizer.wav
#include "signalsmith-harmonizer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using signalsmith::stretch::SignalsmithHarmonizer;

static void writeStereoWav(const char *path, const std::vector<double> &interleaved, int sampleRate) {
	std::ofstream f(path, std::ios::binary);
	const uint32_t dataBytes = uint32_t(interleaved.size()*2);
	const uint32_t fileBytes = 36 + dataBytes;
	auto w32 = [&](uint32_t v) { char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; f.write(b, 4); };
	auto w16 = [&](uint16_t v) { char b[2] = {char(v), char(v >> 8)}; f.write(b, 2); };
	w32(0x46464952); w32(fileBytes); w32(0x45564157); // RIFF ... WAVE
	w32(0x20746d66); w32(16); w16(1); w16(2);          // fmt chunk, PCM, stereo
	w32(sampleRate); w32(sampleRate*4); w16(4); w16(16);
	w32(0x61746164); w32(dataBytes);                   // data chunk
	for (double s : interleaved) {
		double v = s*32767.0;
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		w16(uint16_t(int16_t(v)));
	}
}

int main(int argc, char **argv) {
	const char *outPath = argc > 1 ? argv[1] : "harmonizer.wav";
	const int sampleRate = 48000;
	const int block = 512, interval = 128, voices = 3;
	const long seed = 2024;

	// One input channel; three independent voices from the shared analysis.
	SignalsmithHarmonizer<double> h(seed);
	h.configure(1, voices, block, interval, /*splitComputation=*/true);
	h.setVoice(0, 1.0,  1.0,  false); // unison
	h.setVoice(1, 1.25, 1.05, true);  // major third-ish up, slight formant shift
	h.setVoice(2, 0.8,  0.95, false); // minor third-ish down
	h.setFormantBase(120.0/sampleRate);

	// Per-voice gain and equal-power pan (0 = left, 1 = right).
	const double gain[voices] = {0.55, 0.4, 0.4};
	const double pan[voices]  = {0.25, 0.75, 0.5};

	const int total = sampleRate/2; // half a second of material
	std::vector<double> input(total);
	for (int n = 0; n < total; ++n) {
		double t = double(n)/sampleRate;
		// Multi-harmonic voice-like tone with a gentle vibrato and a few pulses.
		double f = 220.0*(1.0 + 0.01*std::sin(2*M_PI*5.5*t));
		double s = 0.5*std::sin(2*M_PI*f*t) + 0.25*std::sin(2*M_PI*2*f*t) + 0.12*std::sin(2*M_PI*3*f*t);
		if (n % 4096 == 0) s += 0.3;
		input[n] = 0.3*s;
	}

	const int chunk = interval;
	std::vector<std::vector<double>> voiceOut(voices, std::vector<double>(chunk));
	std::vector<double> stereo;
	stereo.reserve(size_t((total + h.outputLatency() + block)*2));

	auto mixChunk = [&](int length) {
		for (int i = 0; i < length; ++i) {
			double left = 0, right = 0;
			for (int v = 0; v < voices; ++v) {
				double angle = pan[v]*(M_PI/2);
				left += gain[v]*std::cos(angle)*voiceOut[v][i];
				right += gain[v]*std::sin(angle)*voiceOut[v][i];
			}
			stereo.push_back(left);
			stereo.push_back(right);
		}
	};

	const double *inPlane[1];
	std::vector<double*> outPlane(voices);

	// Finite material, the same calling convention as a standalone stretch:
	// pre-roll inputLatency zero samples, then feed the signal, then flush.
	std::vector<double> zeros(chunk, 0.0);
	for (int done = 0; done < h.inputLatency(); done += chunk) {
		int length = std::min(chunk, h.inputLatency() - done);
		inPlane[0] = zeros.data();
		for (int v = 0; v < voices; ++v) outPlane[v] = voiceOut[v].data();
		h.process(inPlane, length, outPlane.data(), chunk);
		mixChunk(chunk);
	}
	for (int pos = 0; pos < total; pos += chunk) {
		int length = std::min(chunk, total - pos);
		inPlane[0] = input.data() + pos;
		for (int v = 0; v < voices; ++v) outPlane[v] = voiceOut[v].data();
		h.process(inPlane, length, outPlane.data(), chunk);
		mixChunk(chunk);
	}
	std::vector<std::vector<double>> tail(voices, std::vector<double>(block));
	for (int v = 0; v < voices; ++v) outPlane[v] = tail[v].data();
	h.flush(outPlane.data(), block);
	for (int i = 0; i < block; ++i) {
		double left = 0, right = 0;
		for (int v = 0; v < voices; ++v) {
			double angle = pan[v]*(M_PI/2);
			left += gain[v]*std::cos(angle)*tail[v][i];
			right += gain[v]*std::sin(angle)*tail[v][i];
		}
		stereo.push_back(left);
		stereo.push_back(right);
	}

	writeStereoWav(outPath, stereo, sampleRate);
	std::printf("wrote %s: %zu stereo frames (input latency %d, output latency %d)\n",
	            outPath, stereo.size()/2, h.inputLatency(), h.outputLatency());
	return 0;
}
