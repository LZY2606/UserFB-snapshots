/*
SoLoud sidechain ducker library-level example.

This example runs entirely on the NULLDRIVER: no audio device is opened and
no assets are loaded. Two generated busses are mixed offline - a "narration"
detector and two "music" targets - and Soloud::setBusDucker() makes the music
duck sample by sample whenever the narration signal exceeds the threshold.
The same narration bus drives both targets, showing that one detector can
control several music busses without being consumed twice.

Build from the repository root with:
  c++ -std=c++17 -O1 -DWITH_NULL -Iinclude examples/bus_sidechain.cpp \
      src/core/soloud_core.cpp src/backend/null/soloud_null.cpp -pthread
(or simply compile every file in src/core together with the null backend.)
*/

#include "soloud.h"
#include "soloud_bus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace
{
	using namespace SoLoud;

	const unsigned int SAMPLE_RATE = 48000;
	const unsigned int FRAMES = 1024;
	// Center pan (the play() default) applies equal gain to both channels.
	const double PAN_GAIN = std::cos(M_PI / 4.0);

	// Generated, deterministic audio source. No files or devices needed.
	class GeneratedSource : public AudioSource
	{
	public:
		struct Instance : public AudioSourceInstance
		{
			GeneratedSource &mOwner;
			unsigned int mPosition;

			explicit Instance(GeneratedSource &aOwner) : mOwner(aOwner), mPosition(0) {}

			unsigned int getAudio(float *aBuffer, unsigned int aSamples, unsigned int aStride) override
			{
				unsigned int n = std::min(aSamples, mOwner.mLength - mPosition);
				for (unsigned int ch = 0; ch < mChannels; ch++)
				{
					for (unsigned int i = 0; i < n; i++)
						aBuffer[ch * aStride + i] = (float)mOwner.mSignal(ch, mPosition + i);
					for (unsigned int i = n; i < aSamples; i++)
						aBuffer[ch * aStride + i] = 0.0f;
				}
				mPosition += n;
				mOwner.mFramesRead += n;
				return n;
			}

			bool hasEnded() override { return mPosition >= mOwner.mLength; }
		};

		unsigned int mLength;
		unsigned long long mFramesRead;
		std::function<double(unsigned int, unsigned int)> mSignal;

		GeneratedSource(unsigned int aChannels, unsigned int aLength,
			std::function<double(unsigned int, unsigned int)> aSignal)
			: mLength(aLength), mFramesRead(0), mSignal(std::move(aSignal))
		{
			mBaseSamplerate = (float)SAMPLE_RATE;
			mChannels = aChannels;
		}

		AudioSourceInstance *createInstance() override { return new Instance(*this); }
	};

	Bus *createBus()
	{
		Bus *bus = new Bus();
		bus->mBaseSamplerate = (float)SAMPLE_RATE;
		bus->setResampler(Soloud::RESAMPLER_POINT);
		bus->setChannels(2);
		return bus;
	}

	// Independent envelope, evaluated in double precision.
	double referenceEnvelope(double aPeak, double aThreshold, double aRatio,
		double aAttackCoeff, double aReleaseCoeff, double &aLogGain)
	{
		double targetGain = 1.0;
		if (aPeak > aThreshold)
			targetGain = std::pow(aThreshold / aPeak, 1.0 - 1.0 / aRatio);
		double targetLog = std::log(targetGain);
		double coeff = targetLog < aLogGain ? aAttackCoeff : aReleaseCoeff;
		aLogGain = coeff * aLogGain + (1.0 - coeff) * targetLog;
		return std::exp(aLogGain);
	}
}

int main()
{
	Soloud soloud;
	if (soloud.init(0, Soloud::NULLDRIVER, SAMPLE_RATE, 2048, 2) != 0)
	{
		std::printf("init failed\n");
		return 1;
	}
	soloud.setMainResampler(Soloud::RESAMPLER_POINT);
	soloud.setPostClipScaler(1.0f);
	soloud.setGlobalVolume(1.0f);

	Bus *narration = createBus();
	Bus *musicA = createBus();
	Bus *musicB = createBus();

	// Mute the detector at the source level before play(): its control signal
	// is still produced, but the bus itself is inaudible at the root.
	narration->setVolume(0.0f);

	handle narrationHandle = soloud.play(*narration);
	handle musicAHandle = soloud.play(*musicA);
	handle musicBHandle = soloud.play(*musicB);

	// Narration: a single loud stereo pulse at frame 200 (right channel wins).
	GeneratedSource voice(2, FRAMES, [](unsigned int ch, unsigned int frame) {
		if (frame != 200) return 0.0;
		return ch == 0 ? 0.5 : 0.7;
	});
	// Two constant music beds at different levels.
	GeneratedSource bedA(2, FRAMES, [](unsigned int, unsigned int) { return 0.3; });
	GeneratedSource bedB(2, FRAMES, [](unsigned int, unsigned int) { return 0.2; });

	narration->play(voice);
	musicA->play(bedA);
	musicB->play(bedB);

	const float threshold = 0.2f;
	const float ratio = 4.0f;
	const float attack = 0.0f;   // instant attack
	const float release = 0.02f; // smooth release

	if (soloud.setBusDucker(musicAHandle, narrationHandle, threshold, ratio, attack, release) != 0)
	{
		std::printf("musicA ducker configuration failed\n");
		return 1;
	}
	// The same detector controls a second target.
	if (soloud.setBusDucker(musicBHandle, narrationHandle, threshold, ratio, attack, release) != 0)
	{
		std::printf("musicB ducker configuration failed\n");
		return 1;
	}

	float output[FRAMES * 2];
	soloud.mix(output, FRAMES);

	// Independent reference. The detector sample point is after the child's
	// pan gain, so the pulse peak is 0.7 * PAN_GAIN. Each music bed crosses two
	// center-pan stages, i.e. PAN_GAIN^2 = 0.5, before reaching the root.
	const double detectorPeak = 0.7 * PAN_GAIN;
	const double bedLevel = (0.3 + 0.2) * PAN_GAIN * PAN_GAIN; // 0.25
	const double attackCoeff = 0.0;
	const double releaseCoeff = std::exp(-1.0 / ((double)SAMPLE_RATE * release));
	double logGain = 0.0;
	double maxError = 0.0;
	for (unsigned int i = 0; i < FRAMES; i++)
	{
		double peak = (i == 200) ? detectorPeak : 0.0;
		double gain = referenceEnvelope(peak, threshold, ratio, attackCoeff, releaseCoeff, logGain);
		double expected = bedLevel * gain;
		maxError = std::max(maxError, std::fabs(output[i * 2] - expected));
		maxError = std::max(maxError, std::fabs(output[i * 2 + 1] - expected));
	}

	std::printf("SIDECHAIN_EXAMPLE frames=%u pulse_frame=200 max_error=%.3g\n",
		FRAMES, maxError);
	std::printf("output[199]=%.6f output[200]=%.6f output[201]=%.6f\n",
		output[199 * 2], output[200 * 2], output[201 * 2]);
	std::printf("detector_frames_read=%llu (expected %u, no double consumption)\n",
		voice.mFramesRead, FRAMES);

	soloud.deinit();
	delete narration;
	delete musicA;
	delete musicB;

	if (maxError > 1e-5)
	{
		std::printf("reference mismatch\n");
		return 1;
	}
	std::printf("SIDECHAIN_EXAMPLE completed=1\n");
	return 0;
}
