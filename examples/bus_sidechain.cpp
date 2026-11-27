/*
SoLoud sample-accurate bus sidechain example.

Library-level, headless demo: it uses only the NULLDRIVER, generates every
signal in code (no assets, no audio device), and prints a few values so the
behavior can be checked without listening.

Routing:
  voiceBus   (the narration)        -> root
  musicBusA  (ducked by narration)  -> root
  musicBusB  (ducked by narration, and itself ducking musicBusC)
  musicBusC  (ducked by musicBusB)  -> root

The same narration bus therefore controls several music busses, and one music
bus is reused as a detector for another sidechain (a cascaded ducker).
*/

#include "soloud.h"
#include "soloud_bus.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

namespace
{
	// A tiny deterministic source: fn(channel, frame) gives each sample.
	class GeneratedSource : public SoLoud::AudioSource
	{
	public:
		struct Instance : public SoLoud::AudioSourceInstance
		{
			GeneratedSource &mParent;
			unsigned int mPos;
			explicit Instance(GeneratedSource &aParent) : mParent(aParent), mPos(0) {}
			unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize) override
			{
				unsigned int n = std::min(aSamplesToRead, mParent.mLength - mPos);
				for (unsigned int ch = 0; ch < mChannels; ch++)
					for (unsigned int i = 0; i < n; i++)
						aBuffer[ch * aBufferSize + i] = mParent.mFn(ch, mPos + i);
				mPos += n;
				mParent.mReadFrames += n;
				return n;
			}
			bool hasEnded() override { return mPos >= mParent.mLength; }
		};

		unsigned int mLength;
		unsigned long long mReadFrames;
		std::function<float(unsigned int, unsigned int)> mFn;

		GeneratedSource(unsigned int aChannels, unsigned int aLength,
			std::function<float(unsigned int, unsigned int)> aFn)
			: mLength(aLength), mReadFrames(0), mFn(std::move(aFn))
		{
			mBaseSamplerate = 48000.0f;
			mChannels = aChannels;
		}
		SoLoud::AudioSourceInstance *createInstance() override { return new Instance(*this); }
	};

	// Fix volume/pan at play time so no startup ramp affects the numbers.
	void stabilize(SoLoud::Soloud &aSoloud, SoLoud::handle aHandle)
	{
		aSoloud.setPause(aHandle, true);
		aSoloud.setPanAbsolute(aHandle, 1.0f, 1.0f);
		aSoloud.setVolume(aHandle, 1.0f);
		aSoloud.setPause(aHandle, false);
	}

	SoLoud::handle startBus(SoLoud::Soloud &aSoloud, SoLoud::Bus &aBus)
	{
		aBus.setChannels(1);
		aBus.mBaseSamplerate = 48000.0f;
		aBus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
		SoLoud::handle h = aSoloud.play(aBus);
		stabilize(aSoloud, h);
		return h;
	}
}

int main()
{
	SoLoud::Soloud soloud;
	if (soloud.init(0, SoLoud::Soloud::NULLDRIVER, 48000, 2048, 1) != SoLoud::SO_NO_ERROR)
	{
		std::printf("init failed\n");
		return 1;
	}
	soloud.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
	soloud.setPostClipScaler(1.0f);

	SoLoud::Bus voiceBus, musicBusA, musicBusB, musicBusC;
	SoLoud::handle voice = startBus(soloud, voiceBus);
	SoLoud::handle musicA = startBus(soloud, musicBusA);
	SoLoud::handle musicB = startBus(soloud, musicBusB);
	SoLoud::handle musicC = startBus(soloud, musicBusC);

	// One narration controls two music busses; musicBusB in turn controls C.
	// threshold, ratio, attack seconds, release seconds.
	if (soloud.setBusDucker(musicA, voice, 0.3f, 4.0f, 0.002f, 0.005f) != SoLoud::SO_NO_ERROR ||
		soloud.setBusDucker(musicB, voice, 0.3f, 4.0f, 0.002f, 0.005f) != SoLoud::SO_NO_ERROR ||
		soloud.setBusDucker(musicC, musicB, 0.2f, 2.0f, 0.001f, 0.004f) != SoLoud::SO_NO_ERROR)
	{
		std::printf("setBusDucker failed\n");
		return 1;
	}

	// A short narration pulse in the middle of the first 512-frame block.
	GeneratedSource narration(1, 2048,
		[](unsigned int, unsigned int i) { return (i >= 200 && i < 220) ? 0.5f : 0.0f; });
	GeneratedSource toneA(1, 2048, [](unsigned int, unsigned int) { return 0.20f; });
	GeneratedSource toneB(1, 2048, [](unsigned int, unsigned int) { return 0.15f; });
	GeneratedSource toneC(1, 2048, [](unsigned int, unsigned int) { return 0.10f; });

	SoLoud::handle nh = voiceBus.play(narration);
	SoLoud::handle ah = musicBusA.play(toneA);
	SoLoud::handle bh = musicBusB.play(toneB);
	SoLoud::handle ch = musicBusC.play(toneC);
	for (SoLoud::handle h : { nh, ah, bh, ch }) stabilize(soloud, h);

	const unsigned int frames = 512;
	std::vector<float> output(frames);
	soloud.mix(output.data(), frames);

	std::printf("frame 0   (silence)  output=%.6f\n", output[0]);
	std::printf("frame 200 (narration) output=%.6f\n", output[200]);
	std::printf("frame 219 (narration) output=%.6f\n", output[219]);
	std::printf("frame 240 (release)   output=%.6f\n", output[240]);
	std::printf("source frames consumed: narration=%llu A=%llu B=%llu C=%llu\n",
		narration.mReadFrames, toneA.mReadFrames, toneB.mReadFrames, toneC.mReadFrames);
	std::printf("SIDECHAIN_EXAMPLE done\n");
	return 0;
}
