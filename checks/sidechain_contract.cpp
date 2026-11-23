/*
SoLoud sidechain ducker contract tests.

Built together with contract_main.cpp (which supplies main()). The cases
exercise the per-sample bus sidechain implemented behind Soloud::setBusDucker:
envelope math, graph validation, sample-clock alignment and voice lifetime.
All expected values are generated independently from the engine; every
generated source counts the frames it actually produced so double consumption
of a monitored bus would show up as a mismatch.
*/

#include "contract.hpp"
#include "soloud.h"
#include "soloud_bus.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace
{
	using SoLoud::handle;
	using SoLoud::result;

	const unsigned int SR = 48000;
	const unsigned int BLOCK = 512;

	// Deterministic generated source. The same generator function is used by
	// the independent reference calculation.
	struct GenSource : SoLoud::AudioSource
	{
		struct Instance : SoLoud::AudioSourceInstance
		{
			GenSource &mOwner;
			unsigned int mPos;
			Instance(GenSource &aOwner) : mOwner(aOwner), mPos(0) {}

			unsigned int getAudio(float *aBuffer, unsigned int aSamples, unsigned int aStride) override
			{
				unsigned int n = std::min(aSamples, mOwner.mLength - mPos);
				for (unsigned int ch = 0; ch < mChannels; ch++)
				{
					for (unsigned int i = 0; i < n; i++)
						aBuffer[ch * aStride + i] = (float)mOwner.mSignal(ch, mPos + i);
					for (unsigned int i = n; i < aSamples; i++)
						aBuffer[ch * aStride + i] = 0.0f;
				}
				mPos += n;
				mOwner.mFramesRead += n;
				return n;
			}

			bool hasEnded() override { return mPos >= mOwner.mLength; }
		};

		unsigned int mLength;
		unsigned long long mFramesRead;
		std::function<double(unsigned int, unsigned int)> mSignal;

		GenSource(unsigned int aChannels, unsigned int aLength,
			std::function<double(unsigned int, unsigned int)> aSignal)
			: mLength(aLength), mFramesRead(0), mSignal(std::move(aSignal))
		{
			mBaseSamplerate = (float)SR;
			mChannels = aChannels;
		}

		SoLoud::AudioSourceInstance *createInstance() override
		{
			return new Instance(*this);
		}
	};

	SoLoud::Bus *makeBus(unsigned int aChannels = 2)
	{
		SoLoud::Bus *bus = new SoLoud::Bus();
		bus->mBaseSamplerate = (float)SR;
		bus->setResampler(SoLoud::Soloud::RESAMPLER_POINT);
		bus->setChannels(aChannels);
		return bus;
	}

	// SoLoud ramps channel volumes across each mix quantum. play() snaps the
	// ramp start to the configured volume; later setPanAbsolute/setVolume calls
	// do not, which would make the result depend on the mix chunk size. Snap
	// again after configuring so the fixed volume/pan is quantum-independent.
	void syncVoiceLevels(SoLoud::Soloud &aEngine, handle aVoice)
	{
		int v = aEngine.getVoiceFromHandle_internal(aVoice);
		contract_expect(v >= 0, "voice exists for level sync");
		if (v < 0) return;
		SoLoud::AudioSourceInstance *voice = aEngine.mVoice[v];
		for (unsigned int c = 0; c < MAX_CHANNELS; c++)
			voice->mCurrentChannelVolume[c] = voice->mChannelVolume[c] * voice->mOverallVolume;
	}

	struct Engine
	{
		SoLoud::Soloud mSoloud;
		std::vector<SoLoud::Bus *> mBuses;
		std::vector<GenSource *> mSources;

		Engine()
		{
			contract_expect(mSoloud.init(0, SoLoud::Soloud::NULLDRIVER, SR, 2048, 2) == 0, "null driver init");
			mSoloud.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
			mSoloud.setPostClipScaler(1.0f);
			mSoloud.setGlobalVolume(1.0f);
		}

		~Engine()
		{
			// Release sources and busses while the engine is still alive; the
			// Soloud destructor then stops any remaining voices and frees the
			// sidechain state.
			for (auto s : mSources) delete s;
			for (auto b : mBuses) delete b;
		}

		handle playBus(SoLoud::Bus *aBus, float aVolume = 1.0f)
		{
			handle h = mSoloud.play(*aBus);
			// Pin the bus' channel handle even if it never receives a child:
			// BusInstance teardown otherwise targets the root bus (handle 0).
			aBus->findBusHandle();
			mSoloud.setPanAbsolute(h, 1.0f, 1.0f);
			mSoloud.setVolume(h, aVolume);
			syncVoiceLevels(mSoloud, h);
			return h;
		}

		handle playOn(SoLoud::Bus &aBus, GenSource *aSource, float aVolume = 1.0f)
		{
			handle h = aBus.play(*aSource);
			mSoloud.setPanAbsolute(h, 1.0f, 1.0f);
			mSoloud.setVolume(h, aVolume);
			syncVoiceLevels(mSoloud, h);
			return h;
		}

		SoLoud::Bus *addBus(unsigned int aChannels = 2)
		{
			SoLoud::Bus *b = makeBus(aChannels);
			mBuses.push_back(b);
			return b;
		}

		GenSource *addSource(unsigned int aChannels, unsigned int aLength,
			std::function<double(unsigned int, unsigned int)> aSignal)
		{
			GenSource *s = new GenSource(aChannels, aLength, std::move(aSignal));
			mSources.push_back(s);
			return s;
		}

		// Interleaved stereo output for one mix call.
		std::vector<float> mix(unsigned int aFrames)
		{
			std::vector<float> out(aFrames * 2, 0.0f);
			mSoloud.mix(out.data(), aFrames);
			return out;
		}
	};

	bool nearValue(double a, double b)
	{
		return std::fabs(a - b) <= 1e-5 + 1e-5 * std::max(std::fabs(a), std::fabs(b));
	}

	void expectBuffer(const std::vector<float> &aGot, const std::vector<double> &aWant, const char *aReason)
	{
		contract_expect(aGot.size() == aWant.size(), "buffer size");
		for (size_t i = 0; i < aWant.size(); i++)
			contract_expect(nearValue(aGot[i], aWant[i]), aReason);
	}

	double duckerCoeff(double aSeconds)
	{
		return aSeconds <= 0.0 ? 0.0 : std::exp(-1.0 / ((double)SR * aSeconds));
	}

	// Independent per-sample envelope, computed in double precision.
	struct EnvelopeRef
	{
		double mThreshold;
		double mRatio;
		double mAttack;
		double mRelease;
		double mLogGain;

		EnvelopeRef(double aThreshold, double aRatio, double aAttack, double aRelease)
			: mThreshold(aThreshold), mRatio(aRatio),
			mAttack(duckerCoeff(aAttack)), mRelease(duckerCoeff(aRelease)), mLogGain(0.0) {}

		double step(double aPeak)
		{
			double targetGain = 1.0;
			if (aPeak > mThreshold)
				targetGain = std::pow(mThreshold / aPeak, 1.0 - 1.0 / mRatio);
			double targetLog = std::log(targetGain);
			double coeff = targetLog < mLogGain ? mAttack : mRelease;
			mLogGain = coeff * mLogGain + (1.0 - coeff) * targetLog;
			return std::exp(mLogGain);
		}
	};

	double peakOf(GenSource *aSource, unsigned int aFrame)
	{
		double peak = 0.0;
		for (unsigned int ch = 0; ch < aSource->mChannels; ch++)
			peak = std::max(peak, std::fabs(aSource->mSignal(ch, aFrame)));
		return peak;
	}

	// Full envelope table for a detector source over aFrames.
	std::vector<double> envelopeTable(GenSource *aDetector, unsigned int aFrames,
		double aThreshold, double aRatio, double aAttack, double aRelease)
	{
		EnvelopeRef env(aThreshold, aRatio, aAttack, aRelease);
		std::vector<double> table(aFrames);
		for (unsigned int i = 0; i < aFrames; i++)
			table[i] = env.step(peakOf(aDetector, i));
		return table;
	}

	// ---------------------------------------------------------------- envelope
	void envelopePulseZeroTime()
	{
		Engine e;
		SoLoud::Bus *det = e.addBus();
		SoLoud::Bus *music = e.addBus();
		handle dh = e.playBus(det, 0.0f); // muted but still monitored
		handle mh = e.playBus(music, 1.0f);

		const unsigned int N = 1024;
		GenSource *voice = e.addSource(2, N, [](unsigned int ch, unsigned int frame) {
			if (frame != 300) return 0.0;
			return ch == 0 ? 0.5 : 0.7; // right channel wins the per-frame max
		});
		GenSource *tone = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.4; });
		e.playOn(*det, voice);
		e.playOn(*music, tone);

		const double threshold = 0.2, ratio = 4.0;
		contract_expect(e.mSoloud.setBusDucker(mh, dh, threshold, ratio, 0.0, 0.0) == 0, "configure ducker");

		std::vector<float> got = e.mix(N);
		std::vector<double> env = envelopeTable(voice, N, threshold, ratio, 0.0, 0.0);
		std::vector<double> want(N * 2);
		for (unsigned int i = 0; i < N; i++)
		{
			want[i * 2] = 0.4 * env[i];
			want[i * 2 + 1] = 0.4 * env[i];
		}
		expectBuffer(got, want, "zero-time pulse envelope");

		// Instant attack/release: only frame 300 ducks, using the stereo max 0.7.
		contract_expect(env[299] == 1.0, "no duck before pulse");
		contract_expect(nearValue(env[300], std::pow(threshold / 0.7, 1.0 - 1.0 / ratio)), "stereo max drives gain");
		contract_expect(env[301] == 1.0, "instant release after pulse");
		contract_expect(nearValue(got[300 * 2], 0.4 * env[300]), "engine duck at pulse frame");
		contract_expect(nearValue(got[299 * 2], 0.4), "engine unity before pulse");
		contract_expect(voice->mFramesRead == N, "detector source consumed once per frame");
	}

	void envelopeStepRamps()
	{
		Engine e;
		SoLoud::Bus *det = e.addBus(1);
		SoLoud::Bus *music = e.addBus();
		handle dh = e.playBus(det, 0.0f);
		handle mh = e.playBus(music, 1.0f);

		const unsigned int N = 1024;
		GenSource *voice = e.addSource(1, N, [](unsigned int, unsigned int frame) {
			return frame >= 256 ? 0.8 : 0.0; // mono step
		});
		GenSource *tone = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.25; });
		e.playOn(*det, voice);
		e.playOn(*music, tone);
		contract_expect(det->mChannels == 1, "detector bus mono");

		const double threshold = 0.1, ratio = 2.0, attack = 0.002, release = 0.01;
		contract_expect(e.mSoloud.setBusDucker(mh, dh, threshold, ratio, attack, release) == 0, "configure ramp ducker");

		std::vector<float> got = e.mix(N);
		std::vector<double> env = envelopeTable(voice, N, threshold, ratio, attack, release);
		std::vector<double> want(N * 2);
		for (unsigned int i = 0; i < N; i++)
		{
			// Mono detector signal is expanded to both output channels.
			want[i * 2] = 0.25 * env[i];
			want[i * 2 + 1] = 0.25 * env[i];
		}
		expectBuffer(got, want, "smoothed step envelope");

		// Attack must move gradually toward the static gain, then hold.
		double staticGain = std::pow(threshold / 0.8, 1.0 - 1.0 / ratio);
		contract_expect(env[256] < 1.0 && env[256] > staticGain, "attack starts on step frame");
		EnvelopeRef probe(threshold, ratio, attack, release);
		double first = probe.step(0.8);
		contract_expect(first > staticGain, "attack coefficient smooths first step");
		contract_expect(duckerCoeff(release) > duckerCoeff(attack), "release coefficient matches longer time");
	}

	// Deterministic fixed-gain filter: no internal state, so its output does
	// not depend on when the audio callback runs.
	struct GainFilterInstance : SoLoud::FilterInstance
	{
		float mGain;
		explicit GainFilterInstance(float aGain) : mGain(aGain) {}
		void filter(float *aBuffer, unsigned int aSamples, unsigned int aBufferSize,
			unsigned int aChannels, float, SoLoud::time) override
		{
			for (unsigned int c = 0; c < aChannels; c++)
				for (unsigned int i = 0; i < aSamples; i++)
					aBuffer[c * aBufferSize + i] *= mGain;
		}
	};
	struct GainFilter : SoLoud::Filter
	{
		float mGain;
		explicit GainFilter(float aGain) : mGain(aGain) {}
		SoLoud::FilterInstance *createInstance() override { return new GainFilterInstance(mGain); }
	};

	void envelopeFiltersBeforeSidechain()
	{
		const unsigned int N = 1024;
		Engine e;
		SoLoud::Bus *det = e.addBus(), *music = e.addBus();
		handle dh = e.playBus(det, 0.0f);
		handle mh = e.playBus(music, 1.0f);

		GenSource *voice = e.addSource(2, N, [](unsigned int, unsigned int f) { return f >= 200 ? 0.6 : 0.0; });
		GenSource *tone = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.4; });
		e.playOn(*det, voice);
		e.playOn(*music, tone);

		GainFilter detHalf(0.5f); // detector PCM becomes 0.3 before the sample point
		GainFilter musicQuarter(0.5f); // target PCM becomes 0.2 before compression
		det->setFilter(0, &detHalf);
		music->setFilter(0, &musicQuarter);

		const double threshold = 0.2, ratio = 4.0, atk = 0.001, rel = 0.005;
		contract_expect(e.mSoloud.setBusDucker(mh, dh, threshold, ratio, atk, rel) == 0, "ducker with filtered busses");

		std::vector<float> got = e.mix(N);

		// Detector reference uses the filtered PCM (0.6 * 0.5 = 0.3).
		EnvelopeRef env(threshold, ratio, atk, rel);
		for (unsigned int i = 0; i < N; i++)
		{
			double peak = i >= 200 ? 0.3 : 0.0;
			double gain = env.step(peak);
			contract_expect(nearValue(got[i * 2], 0.2 * gain), "target filter then ducker left");
			contract_expect(nearValue(got[i * 2 + 1], 0.2 * gain), "target filter then ducker right");
		}

		// The pre-filter level 0.6 would duck harder; prove the filter is seen.
		EnvelopeRef unfiltered(threshold, ratio, atk, rel);
		double gainNoFilter = 0.0;
		for (unsigned int i = 0; i <= 200; i++)
			gainNoFilter = unfiltered.step(i >= 200 ? 0.6 : 0.0);
		EnvelopeRef filtered(threshold, ratio, atk, rel);
		double gainFiltered = 0.0;
		for (unsigned int i = 0; i <= 200; i++)
			gainFiltered = filtered.step(i >= 200 ? 0.3 : 0.0);
		contract_expect(gainFiltered > gainNoFilter, "detector filter changes the envelope");
	}

	// ------------------------------------------------------------------- graph
	void graphValidation()
	{
		Engine e;
		SoLoud::Bus *a = e.addBus(), *b = e.addBus(), *c = e.addBus(), *d = e.addBus();
		handle ah = e.playBus(a), bh = e.playBus(b), ch = e.playBus(c), dh = e.playBus(d);

		GenSource *src = e.addSource(2, 4096, [](unsigned int, unsigned int) { return 0.3; });
		handle sh = e.mSoloud.play(*src);
		handle group = e.mSoloud.createVoiceGroup();

		const float inf = std::numeric_limits<float>::infinity();
		const float nan = std::numeric_limits<float>::quiet_NaN();
		contract_expect(e.mSoloud.setBusDucker(ah, bh, nan, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "nan threshold");
		contract_expect(e.mSoloud.setBusDucker(ah, bh, 0.0f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "zero threshold");
		contract_expect(e.mSoloud.setBusDucker(ah, bh, -1.0f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "negative threshold");
		contract_expect(e.mSoloud.setBusDucker(ah, bh, 0.2f, inf, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "infinite ratio");
		contract_expect(e.mSoloud.setBusDucker(ah, bh, 0.2f, 0.5f, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "ratio below one");
		contract_expect(e.mSoloud.setBusDucker(ah, bh, 0.2f, 2, inf, 0.01f) == SoLoud::INVALID_PARAMETER, "infinite attack");
		contract_expect(e.mSoloud.setBusDucker(ah, bh, 0.2f, 2, -0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "negative attack");
		contract_expect(e.mSoloud.setBusDucker(ah, bh, 0.2f, 2, 0.01f, -0.01f) == SoLoud::INVALID_PARAMETER, "negative release");

		contract_expect(e.mSoloud.setBusDucker(sh, bh, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "regular source target");
		contract_expect(e.mSoloud.setBusDucker(ah, sh, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "regular source detector");
		contract_expect(e.mSoloud.setBusDucker(group, bh, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "voice group target");
		contract_expect(e.mSoloud.setBusDucker(ah, group, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "voice group detector");
		contract_expect(e.mSoloud.setBusDucker(ah, ah, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "self listen");
		contract_expect(e.mSoloud.setBusDucker(ah, 0, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "null detector handle");

		e.mSoloud.stop(dh);
		contract_expect(!e.mSoloud.isValidVoiceHandle(dh), "stopped handle expired");
		contract_expect(e.mSoloud.setBusDucker(ah, dh, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "expired detector handle");

		// Boundary values: ratio == 1 and zero times are accepted.
		contract_expect(e.mSoloud.setBusDucker(ah, bh, 0.2f, 1.0f, 0.0f, 0.0f) == 0, "ratio one and zero times accepted");
		// Reconfiguring the same target replaces the detector without error.
		contract_expect(e.mSoloud.setBusDucker(ah, ch, 0.2f, 2.0f, 0.01f, 0.01f) == 0, "same target replacement accepted");

		// A pure sidechain two-cycle across two targets is rejected and the
		// existing link for the target is left untouched.
		contract_expect(e.mSoloud.setBusDucker(bh, ah, 0.2f, 2.0f, 0.01f, 0.01f) == 0, "first link of the pair");
		contract_expect(e.mSoloud.setBusDucker(ah, bh, 0.2f, 2.0f, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "sidechain cycle rejected");
		// The rejected replacement kept a <- c, so re-applying it still works.
		contract_expect(e.mSoloud.setBusDucker(ah, ch, 0.2f, 2.0f, 0.01f, 0.01f) == 0, "preserved link still configurable");
	}

	void graphContainmentCycles()
	{
		Engine e;
		SoLoud::Bus *parent = e.addBus(), *child = e.addBus(), *grand = e.addBus();
		handle ph = e.playBus(parent);
		handle chh = parent->play(*child);
		e.mSoloud.setPanAbsolute(chh, 1, 1);
		syncVoiceLevels(e.mSoloud, chh);
		handle gh = child->play(*grand);
		e.mSoloud.setPanAbsolute(gh, 1, 1);
		syncVoiceLevels(e.mSoloud, gh);

		// Child listening to its parent closes the containment dependency.
		contract_expect(e.mSoloud.setBusDucker(chh, ph, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "parent->child sidechain cycles");
		// Grandchild listening to an ancestor likewise.
		contract_expect(e.mSoloud.setBusDucker(gh, ph, 0.2f, 2, 0.01f, 0.01f) == SoLoud::INVALID_PARAMETER, "ancestor sidechain cycles");

		// Sibling sidechains are fine even inside a nested bus tree.
		SoLoud::Bus *sib = e.addBus();
		handle sibh = parent->play(*sib);
		e.mSoloud.setPanAbsolute(sibh, 1, 1);
		syncVoiceLevels(e.mSoloud, sibh);
		contract_expect(e.mSoloud.setBusDucker(sibh, chh, 0.2f, 2, 0.01f, 0.01f) == 0, "nested sibling sidechain accepted");
	}

	void graphReplacementAndCascade()
	{
		const unsigned int N = 1024;
		Engine e;
		SoLoud::Bus *d1 = e.addBus(), *d2 = e.addBus(), *m1 = e.addBus(), *m2 = e.addBus();
		handle d1h = e.playBus(d1, 0.0f), d2h = e.playBus(d2, 0.0f);
		handle m1h = e.playBus(m1, 0.0f), m2h = e.playBus(m2, 1.0f);

		GenSource *v1 = e.addSource(2, N, [](unsigned int, unsigned int f) { return f == 100 ? 0.6 : 0.0; });
		GenSource *v2 = e.addSource(2, N, [](unsigned int, unsigned int f) { return f == 200 ? 0.6 : 0.0; });
		GenSource *tone1 = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.3; });
		GenSource *tone2 = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.2; });
		e.playOn(*d1, v1);
		e.playOn(*d2, v2);
		e.playOn(*m1, tone1);
		e.playOn(*m2, tone2);

		const double thr = 0.2, ratio = 4.0, atk = 0.001, rel = 0.005;
		contract_expect(e.mSoloud.setBusDucker(m1h, d1h, thr, ratio, atk, rel) == 0, "first link");
		// Replace the detector on the same target; unity gain restarts.
		contract_expect(e.mSoloud.setBusDucker(m1h, d2h, thr, ratio, atk, rel) == 0, "replacement link");
		// m2 listens to m1, whose published PCM is already ducked.
		contract_expect(e.mSoloud.setBusDucker(m2h, m1h, thr, ratio, atk, rel) == 0, "cascade link");

		std::vector<float> got = e.mix(N);

		EnvelopeRef env1(thr, ratio, atk, rel);
		EnvelopeRef env2(thr, ratio, atk, rel);
		std::vector<double> want(N * 2, 0.0);
		for (unsigned int i = 0; i < N; i++)
		{
			double g1 = env1.step(peakOf(v2, i)); // replacement: only d2 controls m1
			double m1pcm = 0.3 * g1;
			double g2 = env2.step(std::fabs(m1pcm));
			want[i * 2] = 0.2 * g2; // d1, d2 and m1 are muted at the root
			want[i * 2 + 1] = 0.2 * g2;
		}
		expectBuffer(got, want, "replacement and cascade envelope");

		// The replaced detector's pulse at frame 100 must have no effect.
		EnvelopeRef probe(thr, ratio, atk, rel);
		double gAt100 = 0, gAt200 = 0;
		for (unsigned int i = 0; i <= 200; i++)
		{
			double g = probe.step(peakOf(v2, i));
			if (i == 100) gAt100 = g;
			if (i == 200) gAt200 = g;
		}
		contract_expect(gAt100 == 1.0, "old detector ignored");
		contract_expect(gAt200 < 1.0, "new detector active");
	}

	void graphSharedDetector()
	{
		const unsigned int N = 768;
		Engine e;
		SoLoud::Bus *det = e.addBus(), *m1 = e.addBus(), *m2 = e.addBus();
		handle dh = e.playBus(det, 0.0f);
		handle m1h = e.playBus(m1), m2h = e.playBus(m2);

		GenSource *voice = e.addSource(2, N, [](unsigned int ch, unsigned int f) {
			if (f < 256) return 0.0;
			return ch == 0 ? 0.45 : 0.55;
		});
		GenSource *t1 = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.3; });
		GenSource *t2 = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.2; });
		e.playOn(*det, voice);
		e.playOn(*m1, t1);
		e.playOn(*m2, t2);

		contract_expect(e.mSoloud.setBusDucker(m1h, dh, 0.2, 3, 0.002, 0.01) == 0, "first target");
		contract_expect(e.mSoloud.setBusDucker(m2h, dh, 0.2, 3, 0.002, 0.01) == 0, "second target shares detector");

		std::vector<float> got = e.mix(N);
		std::vector<double> env = envelopeTable(voice, N, 0.2, 3, 0.002, 0.01);
		std::vector<double> want(N * 2);
		for (unsigned int i = 0; i < N; i++)
		{
			want[i * 2] = (0.3 + 0.2) * env[i];
			want[i * 2 + 1] = (0.3 + 0.2) * env[i];
		}
		expectBuffer(got, want, "shared detector envelope");
		contract_expect(voice->mFramesRead == N, "shared detector source read once");
	}

	void graphRejectedAfterMixAndPreserved()
	{
		const unsigned int N = 512;
		Engine e;
		SoLoud::Bus *det = e.addBus(), *other = e.addBus(), *music = e.addBus();
		handle dh = e.playBus(det, 0.0f), oh = e.playBus(other, 0.0f), mh = e.playBus(music);
		GenSource *voice = e.addSource(2, N, [](unsigned int, unsigned int f) { return f == 50 ? 0.7 : 0.0; });
		GenSource *silence = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.0; });
		GenSource *tone = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.4; });
		e.playOn(*det, voice);
		e.playOn(*other, silence);
		e.playOn(*music, tone);

		contract_expect(e.mSoloud.setBusDucker(mh, dh, 0.2, 4, 0.0, 0.0) == 0, "original link");
		e.mix(BLOCK); // freezes configuration
		contract_expect(e.mSoloud.setBusDucker(mh, oh, 0.2, 4, 0.0, 0.0) == SoLoud::INVALID_PARAMETER, "post-mix configuration rejected");

		// The failed replacement must leave the original link working.
		std::vector<float> got = e.mix(N - BLOCK);
		std::vector<double> env = envelopeTable(voice, N, 0.2, 4, 0.0, 0.0);
		for (unsigned int i = BLOCK; i < N; i++)
		{
			contract_expect(nearValue(got[(i - BLOCK) * 2], 0.4 * env[i]), "original link preserved");
			contract_expect(nearValue(got[(i - BLOCK) * 2 + 1], 0.4 * env[i]), "original link preserved right");
		}
	}

	// ------------------------------------------------------------------- clock
	void clockChunkEquivalence()
	{
		const unsigned int N = 2048;
		auto build = [](Engine &e, handle &dh, handle &mh, GenSource *&voice, GenSource *&tone) {
			SoLoud::Bus *det = e.addBus(), *music = e.addBus();
			dh = e.playBus(det, 0.0f);
			mh = e.playBus(music, 1.0f);
			voice = e.addSource(2, N, [](unsigned int ch, unsigned int f) {
				if (f == 700) return ch == 0 ? 0.5 : 0.9;     // pulse inside the second 512 block
				if (f >= 1500) return ch == 0 ? 0.3 : 0.4;    // step across the 1536 boundary
				return 0.0;
			});
			tone = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.35; });
			e.playOn(*det, voice);
			e.playOn(*music, tone);
			contract_expect(e.mSoloud.setBusDucker(mh, dh, 0.2, 3, 0.002, 0.01) == 0, "clock ducker");
		};

		Engine whole;
		handle dh1, mh1;
		GenSource *v1, *t1;
		build(whole, dh1, mh1, v1, t1);
		std::vector<float> wholeOut = whole.mix(N);

		Engine chunked;
		handle dh2, mh2;
		GenSource *v2, *t2;
		build(chunked, dh2, mh2, v2, t2);
		std::vector<float> chunkOut;
		unsigned int chunks[] = {1, 7, 257};
		unsigned int made = 0, ci = 0;
		while (made < N)
		{
			unsigned int n;
			if (ci < 3) n = chunks[ci++];
			else n = std::min(BLOCK, N - made);
			std::vector<float> part = chunked.mix(n);
			chunkOut.insert(chunkOut.end(), part.begin(), part.end());
			made += n;
		}
		contract_expect(made == N && chunkOut.size() == N * 2, "chunk total length");

		for (unsigned int i = 0; i < N * 2; i++)
			contract_expect(nearValue(chunkOut[i], wholeOut[i]), "chunked and whole mixes match");

		// Independent reference, no engine output reused.
		std::vector<double> env = envelopeTable(v1, N, 0.2, 3, 0.002, 0.01);
		for (unsigned int i = 0; i < N; i++)
		{
			contract_expect(nearValue(wholeOut[i * 2], 0.35 * env[i]), "independent reference left");
			contract_expect(nearValue(wholeOut[i * 2 + 1], 0.35 * env[i]), "independent reference right");
		}

		// The pulse lives in the block starting at 512: no one-block latency.
		contract_expect(nearValue(wholeOut[699 * 2], 0.35), "unity one frame before pulse");
		contract_expect(wholeOut[700 * 2] < 0.35 - 1e-4, "duck arrives on the pulse frame");

		// Identical source consumption: monitored bus is never read twice.
		contract_expect(v1->mFramesRead == v2->mFramesRead, "detector read count match");
		contract_expect(t1->mFramesRead == t2->mFramesRead, "music read count match");
		contract_expect(v1->mFramesRead == N, "detector read exactly once per output frame");
		contract_expect(t1->mFramesRead == N, "music read exactly once per output frame");
	}

	void clockNoSidechainUnchanged()
	{
		const unsigned int N = 2048;
		auto build = [](Engine &e) {
			SoLoud::Bus *bus = e.addBus();
			e.playBus(bus, 1.0f);
			GenSource *src = e.addSource(2, N, [](unsigned int ch, unsigned int f) {
				return ch == 0 ? 0.3 : (f < 1024 ? -0.2 : 0.25);
			});
			e.playOn(*bus, src);
		};

		Engine whole;
		build(whole);
		std::vector<float> a = whole.mix(N);

		Engine chunked;
		build(chunked);
		std::vector<float> b;
		unsigned int made = 0;
		while (made < N)
		{
			unsigned int n = std::min(257u, N - made);
			std::vector<float> part = chunked.mix(n);
			b.insert(b.end(), part.begin(), part.end());
			made += n;
		}
		for (unsigned int i = 0; i < N * 2; i++)
			contract_expect(nearValue(a[i], b[i]), "no-sidechain chunk parity");

		// Hand-computed path: point resampler and fixed unity gains are identity.
		for (unsigned int i = 0; i < N; i++)
		{
			contract_expect(nearValue(a[i * 2], 0.3), "no-sidechain left identity");
			contract_expect(nearValue(a[i * 2 + 1], i < 1024 ? -0.2 : 0.25), "no-sidechain right identity");
		}
	}

	void clockNestedBuses()
	{
		const unsigned int N = 1536;
		Engine e;
		SoLoud::Bus *outer = e.addBus(), *det = e.addBus(), *inner = e.addBus();
		handle oh = e.playBus(outer, 1.0f);
		handle dh = e.playBus(det, 0.0f);
		handle ih = outer->play(*inner);
		e.mSoloud.setPanAbsolute(ih, 1, 1);
		syncVoiceLevels(e.mSoloud, ih);

		GenSource *voice = e.addSource(2, N, [](unsigned int, unsigned int f) { return f >= 300 ? 0.6 : 0.0; });
		GenSource *tone = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.2; });
		e.playOn(*det, voice);
		e.playOn(*inner, tone);

		contract_expect(e.mSoloud.setBusDucker(oh, dh, 0.2, 2.5, 0.001, 0.008) == 0, "duck a nested target from a root detector");

		std::vector<float> got = e.mix(N);
		std::vector<double> env = envelopeTable(voice, N, 0.2, 2.5, 0.001, 0.008);
		for (unsigned int i = 0; i < N; i++)
		{
			contract_expect(nearValue(got[i * 2], 0.2 * env[i]), "nested bus left follows envelope");
			contract_expect(nearValue(got[i * 2 + 1], 0.2 * env[i]), "nested bus right follows envelope");
		}
		contract_expect(voice->mFramesRead == N && tone->mFramesRead == N, "nested sources read once");
	}

	void clockNestedDetector()
	{
		const unsigned int N = 1536;
		Engine e;
		SoLoud::Bus *outer = e.addBus(), *det = e.addBus(), *music = e.addBus();
		// Play the target before the detector subtree so the raw voice-slot
		// order would schedule it first; the sidechain ordering must fix that.
		handle mh = e.playBus(music, 1.0f);
		handle oh = e.playBus(outer, 0.0f);   // detector subtree is inaudible at root
		handle dh = outer->play(*det);
		syncVoiceLevels(e.mSoloud, dh);

		GenSource *voice = e.addSource(2, N, [](unsigned int ch, unsigned int f) {
			if (f < 600) return 0.0;
			return ch == 0 ? 0.4 : 0.6; // nested detector pulse train
		});
		GenSource *tone = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.2; });
		e.playOn(*det, voice);
		e.playOn(*music, tone);

		contract_expect(e.mSoloud.setBusDucker(mh, dh, 0.2, 3, 0.001, 0.006) == 0, "root target listens to nested detector");

		// Whole-quantum and fragmented runs must agree and match the reference.
		std::vector<float> whole = e.mix(N);

		std::vector<double> env = envelopeTable(voice, N, 0.2, 3, 0.001, 0.006);
		for (unsigned int i = 0; i < N; i++)
		{
			contract_expect(nearValue(whole[i * 2], 0.2 * env[i]), "nested detector envelope left");
			contract_expect(nearValue(whole[i * 2 + 1], 0.2 * env[i]), "nested detector envelope right");
		}
		contract_expect(voice->mFramesRead == N && tone->mFramesRead == N, "nested detector sources read once");

		// Re-run the same graph in tiny chunks and compare against the whole run.
		Engine e2;
		SoLoud::Bus *outer2 = e2.addBus(), *det2 = e2.addBus(), *music2 = e2.addBus();
		handle mh2 = e2.playBus(music2, 1.0f);
		handle oh2 = e2.playBus(outer2, 0.0f);
		handle dh2 = outer2->play(*det2);
		syncVoiceLevels(e2.mSoloud, dh2);
		GenSource *voice2 = e2.addSource(2, N, [](unsigned int ch, unsigned int f) {
			if (f < 600) return 0.0;
			return ch == 0 ? 0.4 : 0.6;
		});
		GenSource *tone2 = e2.addSource(2, N, [](unsigned int, unsigned int) { return 0.2; });
		e2.playOn(*det2, voice2);
		e2.playOn(*music2, tone2);
		contract_expect(e2.mSoloud.setBusDucker(mh2, dh2, 0.2, 3, 0.001, 0.006) == 0, "chunked nested ducker");
		std::vector<float> chunked;
		unsigned made = 0;
		const unsigned int chunks[] = {3, 11, 257, 512};
		unsigned ci = 0;
		while (made < N)
		{
			unsigned int n = chunks[ci % 4];
			ci++;
			if (n > N - made) n = N - made;
			std::vector<float> part = e2.mix(n);
			chunked.insert(chunked.end(), part.begin(), part.end());
			made += n;
		}
		for (unsigned int i = 0; i < N * 2; i++)
			contract_expect(nearValue(chunked[i], whole[i]), "nested detector chunk parity");
		(void)oh; (void)oh2;
	}

	// ---------------------------------------------------------------- lifetime
	void lifetimeDetectorStopAndReuse()
	{
		const unsigned int N = 1024;
		Engine e;
		SoLoud::Bus *det = e.addBus(), *m1 = e.addBus(), *m2 = e.addBus();
		handle dh = e.playBus(det, 0.0f);
		handle m1h = e.playBus(m1), m2h = e.playBus(m2);

		GenSource *voice = e.addSource(2, N, [](unsigned int, unsigned int f) { return f < 256 ? 0.7 : 0.0; });
		GenSource *t1 = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.3; });
		GenSource *t2 = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.15; });
		e.playOn(*det, voice);
		e.playOn(*m1, t1);
		e.playOn(*m2, t2);

		const double thr = 0.2, ratio = 3.0, atk = 0.001, rel = 0.002;
		contract_expect(e.mSoloud.setBusDucker(m1h, dh, thr, ratio, atk, rel) == 0, "target one");
		contract_expect(e.mSoloud.setBusDucker(m2h, dh, thr, ratio, atk, rel) == 0, "target two");

		e.mix(BLOCK); // deliver an exact 512-frame boundary

		// Stop the detector on the boundary; remember the slot it used.
		int oldSlot = (dh & 0xfff) - 1;
		e.mSoloud.stop(dh);
		contract_expect(!e.mSoloud.isValidVoiceHandle(dh), "detector handle expired after stop");

		// A plain source reusing the slot must not become the detector.
		GenSource *loud = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.5; });
		handle reuse = e.mSoloud.play(*loud);
		e.mSoloud.setPanAbsolute(reuse, 1.0f, 1.0f);
		syncVoiceLevels(e.mSoloud, reuse);
		contract_expect((int)((reuse & 0xfff) - 1) == oldSlot, "slot reused by plain source");
		contract_expect(!e.mSoloud.isValidVoiceHandle(dh), "old handle still invalid after reuse");

		std::vector<float> got = e.mix(N - BLOCK);

		// Reference: the detector goes silent after frame 511, both envelopes
		// release; the loud source only contributes its own audible signal.
		EnvelopeRef env1(thr, ratio, atk, rel), env2(thr, ratio, atk, rel);
		std::vector<double> e1(N), e2(N);
		for (unsigned int i = 0; i < N; i++)
		{
			double peak = i < BLOCK ? peakOf(voice, i) : 0.0;
			e1[i] = env1.step(peak);
			e2[i] = env2.step(peak);
		}
		for (unsigned int i = BLOCK; i < N; i++)
		{
			double left = 0.3 * e1[i] + 0.15 * e2[i] + 0.5;
			contract_expect(nearValue(got[(i - BLOCK) * 2], left), "release with reused slot left");
			contract_expect(nearValue(got[(i - BLOCK) * 2 + 1], left), "release with reused slot right");
		}
		// Release must converge back to unity.
		contract_expect(e1[N - 1] > 0.999 && e2[N - 1] > 0.999, "envelopes release to unity");
	}

	void lifetimeTargetStop()
	{
		const unsigned int N = 1024;
		Engine e;
		SoLoud::Bus *det = e.addBus(), *m1 = e.addBus(), *m2 = e.addBus();
		handle dh = e.playBus(det, 0.0f);
		handle m1h = e.playBus(m1, 0.0f), m2h = e.playBus(m2, 1.0f);

		GenSource *voice = e.addSource(2, N, [](unsigned int, unsigned int f) { return f >= 100 ? 0.6 : 0.0; });
		GenSource *t1 = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.3; });
		GenSource *t2 = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.25; });
		e.playOn(*det, voice);
		e.playOn(*m1, t1);
		e.playOn(*m2, t2);

		contract_expect(e.mSoloud.setBusDucker(m1h, dh, 0.2, 3, 0.002, 0.01) == 0, "target one link");
		contract_expect(e.mSoloud.setBusDucker(m2h, dh, 0.2, 3, 0.002, 0.01) == 0, "target two link");

		e.mix(BLOCK);
		e.mSoloud.stop(m1h); // stopping one target must not disturb the other
		std::vector<float> got = e.mix(N - BLOCK);

		std::vector<double> env = envelopeTable(voice, N, 0.2, 3, 0.002, 0.01);
		for (unsigned int i = BLOCK; i < N; i++)
		{
			contract_expect(nearValue(got[(i - BLOCK) * 2], 0.25 * env[i]), "surviving target keeps ducking left");
			contract_expect(nearValue(got[(i - BLOCK) * 2 + 1], 0.25 * env[i]), "surviving target keeps ducking right");
		}
		contract_expect(voice->mFramesRead == N, "detector still consumed once after target stop");
	}

	void lifetimeSilentMonitoredBus()
	{
		const unsigned int N = 768;
		Engine e;
		SoLoud::Bus *det = e.addBus(), *music = e.addBus();
		handle dh = e.playBus(det, 0.0f); // inaudible at the root
		handle mh = e.playBus(music, 1.0f);
		// Visualization stays off, as required for the sidechain path.
		contract_expect(!(det->mFlags & SoLoud::AudioSource::VISUALIZATION_DATA), "visualization disabled");

		GenSource *voice = e.addSource(2, N, [](unsigned int, unsigned int f) { return f >= 128 ? 0.5 : 0.0; });
		GenSource *tone = e.addSource(2, N, [](unsigned int, unsigned int) { return 0.4; });
		e.playOn(*det, voice);
		e.playOn(*music, tone);

		contract_expect(e.mSoloud.setBusDucker(mh, dh, 0.2, 4, 0.0, 0.02) == 0, "ducker on muted detector");
		std::vector<float> got = e.mix(N);
		std::vector<double> env = envelopeTable(voice, N, 0.2, 4, 0.0, 0.02);
		for (unsigned int i = 0; i < N; i++)
		{
			contract_expect(nearValue(got[i * 2], 0.4 * env[i]), "muted detector still controls left");
			contract_expect(nearValue(got[i * 2 + 1], 0.4 * env[i]), "muted detector still controls right");
		}
	}
}

void register_contract()
{
	contract_add("envelope", "pulse_zero_time", envelopePulseZeroTime);
	contract_add("envelope", "step_ramps", envelopeStepRamps);
	contract_add("envelope", "filters_before_sidechain", envelopeFiltersBeforeSidechain);

	contract_add("graph", "validation", graphValidation);
	contract_add("graph", "containment_cycles", graphContainmentCycles);
	contract_add("graph", "replacement_cascade", graphReplacementAndCascade);
	contract_add("graph", "shared_detector", graphSharedDetector);
	contract_add("graph", "frozen_after_mix", graphRejectedAfterMixAndPreserved);

	contract_add("clock", "chunk_equivalence", clockChunkEquivalence);
	contract_add("clock", "no_sidechain_unchanged", clockNoSidechainUnchanged);
	contract_add("clock", "nested_buses", clockNestedBuses);
	contract_add("clock", "nested_detector", clockNestedDetector);

	contract_add("lifetime", "detector_stop_reuse", lifetimeDetectorStopAndReuse);
	contract_add("lifetime", "target_stop", lifetimeTargetStop);
	contract_add("lifetime", "silent_monitored_bus", lifetimeSilentMonitoredBus);
}
