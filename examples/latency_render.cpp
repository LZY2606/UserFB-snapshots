/*
Latency compensation render example.

Builds a small offline graph under the NULL driver, enables filter latency
compensation, renders interleaved float PCM to the file given as the only
command line argument, then reads the file straight back and verifies that the
independently predicted pulse positions are present. No audio device is
opened and no external assets are used.

File layout: a small fixed header followed by interleaved 32-bit float frames.
*/

#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_filter.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{
	constexpr unsigned kSampleRate = 48000;
	constexpr unsigned kChannels = 2;
	constexpr unsigned kWarmFrames = 512;
	constexpr unsigned kRenderFrames = 2048;
	constexpr unsigned kPulseFrame = 1024;
	constexpr unsigned kBusDelay = 120;
	constexpr unsigned kSourceDelay = 300;
	constexpr unsigned kExpectedTotalLatency = 300;
	constexpr char kMagic[4] = {'L', 'P', 'C', 'M'};

	struct Header
	{
		char magic[4];
		std::uint32_t sampleRate;
		std::uint32_t channels;
		std::uint32_t warmFrames;
		std::uint32_t renderFrames;
	};

	// One pulse at a fixed source frame, silence before and after.
	struct PulseInstance : SoLoud::AudioSourceInstance
	{
		unsigned mPos = 0;
		unsigned getAudio(float *aBuffer, unsigned aSamples, unsigned aStride) override
		{
			for (unsigned c = 0; c < mChannels; ++c)
				for (unsigned i = 0; i < aSamples; ++i)
					aBuffer[c * aStride + i] = (mPos + i == kPulseFrame) ? (0.1f * (c + 1)) : 0.f;
			mPos += aSamples;
			return aSamples;
		}
		bool hasEnded() override { return false; }
	};

	struct PulseSource : SoLoud::AudioSource
	{
		PulseSource()
		{
			mChannels = kChannels;
			mBaseSamplerate = (float)kSampleRate;
		}
		SoLoud::AudioSourceInstance *createInstance() override { return new PulseInstance(); }
	};

	// Per-channel pure delay; reports its latency so the planner can use it.
	struct DelayInstance : SoLoud::FilterInstance
	{
		unsigned mDelay;
		unsigned mPos = 0;
		std::vector<float> mRing;
		explicit DelayInstance(unsigned aDelay) : mDelay(aDelay), mRing(aDelay * kChannels, 0.f) {}
		void filter(float *aBuffer, unsigned aSamples, unsigned aStride, unsigned aChannels, float, SoLoud::time) override
		{
			if (mDelay == 0) return;
			for (unsigned i = 0; i < aSamples; ++i)
			{
				for (unsigned c = 0; c < aChannels; ++c)
				{
					float &slot = mRing[c * mDelay + mPos];
					float incoming = aBuffer[c * aStride + i];
					aBuffer[c * aStride + i] = slot;
					slot = incoming;
				}
				mPos = (mPos + 1) % mDelay;
			}
		}
		unsigned getLatencyFrames() const override { return mDelay; }
	};

	struct DelayFilter : SoLoud::Filter
	{
		unsigned mDelay;
		explicit DelayFilter(unsigned aDelay) : mDelay(aDelay) {}
		SoLoud::FilterInstance *createInstance() override { return new DelayInstance(mDelay); }
	};
}

int main(int aArgc, char **aArgv)
{
	if (aArgc != 2)
	{
		std::fprintf(stderr, "usage: %s <output-pcm-path>\n", aArgv[0]);
		return 2;
	}
	const char *outputPath = aArgv[1];

	SoLoud::Soloud engine;
	if (engine.init(0, SoLoud::Soloud::NULLDRIVER, kSampleRate, 1024, kChannels) != SoLoud::SO_NO_ERROR)
	{
		std::fprintf(stderr, "null driver init failed\n");
		return 1;
	}
	engine.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	engine.setPostClipScaler(1);

	// Branch A: bus carrying a 120-frame delay filter.
	// Branch B: source with a 300-frame delay filter played straight to root.
	// Branch C: unfiltered source at root (the bypass that must be delayed to match).
	DelayFilter busDelay(kBusDelay);
	DelayFilter sourceDelay(kSourceDelay);
	SoLoud::Bus bus;
	bus.setChannels(kChannels);
	bus.mBaseSamplerate = (float)kSampleRate;
	bus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
	bus.setFilter(0, &busDelay);

	PulseSource busSource, delayedSource, cleanSource;
	delayedSource.setFilter(0, &sourceDelay);

	SoLoud::handle hBus = engine.playBackground(bus);
	SoLoud::handle hBusSource = bus.play(busSource);
	SoLoud::handle hDelayed = engine.playBackground(delayedSource);
	SoLoud::handle hClean = engine.playBackground(cleanSource);
	for (SoLoud::handle h : {hBus, hBusSource, hDelayed, hClean})
	{
		for (unsigned c = 0; c < kChannels; ++c)
			engine.setChannelVolume(h, c, 1.0f);
		engine.setAutoStop(h, false);
	}

	if (engine.prepareFilterLatencyCompensation() != SoLoud::SO_NO_ERROR)
	{
		std::fprintf(stderr, "prepareFilterLatencyCompensation failed\n");
		return 1;
	}
	const unsigned total = engine.getFilterLatencyCompensationFrames();
	if (total != kExpectedTotalLatency)
	{
		std::fprintf(stderr, "unexpected total latency %u (want %u)\n", total, kExpectedTotalLatency);
		return 1;
	}

	// Warm up the engine's volume smoothing with silence so render results are
	// independent of the mix block sizes.
	std::vector<float> warm(kWarmFrames * kChannels, 0.f);
	engine.mix(warm.data(), kWarmFrames);

	std::vector<float> rendered(kRenderFrames * kChannels, 0.f);
	for (unsigned done = 0; done < kRenderFrames;)
	{
		unsigned n = kRenderFrames - done;
		if (n > 257) n = 257;
		engine.mix(rendered.data() + done * kChannels, n);
		done += n;
	}

	Header header;
	std::memcpy(header.magic, kMagic, 4);
	header.sampleRate = kSampleRate;
	header.channels = kChannels;
	header.warmFrames = kWarmFrames;
	header.renderFrames = kRenderFrames;

	std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
	if (!out)
	{
		std::fprintf(stderr, "cannot open %s for writing\n", outputPath);
		return 1;
	}
	out.write(reinterpret_cast<const char *>(&header), sizeof(header));
	out.write(reinterpret_cast<const char *>(rendered.data()),
			  (std::streamsize)(rendered.size() * sizeof(float)));
	if (!out)
	{
		std::fprintf(stderr, "write failed\n");
		return 1;
	}
	out.close();

	// Read the file back and verify the predicted pulse.
	std::ifstream in(outputPath, std::ios::binary);
	if (!in)
	{
		std::fprintf(stderr, "cannot reopen %s\n", outputPath);
		return 1;
	}
	Header readHeader;
	in.read(reinterpret_cast<char *>(&readHeader), sizeof(readHeader));
	if (!in || std::memcmp(readHeader.magic, kMagic, 4) != 0 ||
		readHeader.sampleRate != kSampleRate || readHeader.channels != kChannels ||
		readHeader.renderFrames != kRenderFrames)
	{
		std::fprintf(stderr, "bad PCM header\n");
		return 1;
	}
	std::vector<float> pcm(kRenderFrames * kChannels);
	in.read(reinterpret_cast<char *>(pcm.data()), (std::streamsize)(pcm.size() * sizeof(float)));
	if (!in)
	{
		std::fprintf(stderr, "short PCM payload\n");
		return 1;
	}

	// Independent prediction: every branch arrives kTotalLatency frames after
	// the source pulse; subtract the warm-up to get the position inside the
	// rendered window.
	const unsigned expectedLocal = kPulseFrame + total - kWarmFrames;
	std::vector<unsigned> pulseFrames;
	for (unsigned f = 0; f < kRenderFrames; ++f)
	{
		if (std::fabs(pcm[f * kChannels]) > 1e-7f)
			pulseFrames.push_back(f);
	}
	if (pulseFrames.size() != 1 || pulseFrames[0] != expectedLocal)
	{
		std::fprintf(stderr, "pulse position mismatch: got %zu hits, want single frame %u\n",
					 pulseFrames.size(), expectedLocal);
		return 1;
	}
	// Three branches contribute 0.1*(c+1) each, all aligned at the same frame.
	for (unsigned c = 0; c < kChannels; ++c)
	{
		float expected = 3.0f * 0.1f * (c + 1);
		float got = pcm[expectedLocal * kChannels + c];
		if (std::fabs(got - expected) > 1e-5f)
		{
			std::fprintf(stderr, "channel %u amplitude %g differs from %g\n", c, got, expected);
			return 1;
		}
		if (std::fabs(pcm[(expectedLocal - 1) * kChannels + c]) > 1e-9f ||
			std::fabs(pcm[(expectedLocal + 1) * kChannels + c]) > 1e-9f)
		{
			std::fprintf(stderr, "pulse smeared into neighboring frames\n");
			return 1;
		}
	}

	std::printf("latency_render OK: %u frames, total latency %u, pulse at local frame %u -> %s\n",
				kRenderFrames, total, expectedLocal, outputPath);
	return 0;
}
