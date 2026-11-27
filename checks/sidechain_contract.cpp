/*
SoLoud sidechain ducker contract tests.

The test sources are fully deterministic, and every expected envelope and mix
result is computed by independent C++ math in this file; engine output is only
ever compared against, never reused as, the reference.
*/

#include "contract.hpp"
#include "soloud.h"
#include "soloud_bus.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace
{
	const unsigned SR = 48000;
	const float ABS_TOL = 1e-5f;
	const float REL_TOL = 1e-5f;

	// Deterministic generated source. fn(channel, frame) gives the sample value.
	struct GenSource : public SoLoud::AudioSource
	{
		struct Instance : public SoLoud::AudioSourceInstance
		{
			GenSource &mParent;
			unsigned mPos;
			explicit Instance(GenSource &aParent) : mParent(aParent), mPos(0) {}
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

		GenSource(unsigned int aChannels, unsigned int aLength,
			std::function<float(unsigned int, unsigned int)> aFn)
			: mLength(aLength), mReadFrames(0), mFn(std::move(aFn))
		{
			mBaseSamplerate = (float)SR;
			mChannels = aChannels;
		}
		SoLoud::AudioSourceInstance *createInstance() override
		{
			return new Instance(*this);
		}
	};

	// Deterministic bus filter: adds a constant offset on every channel.
	struct OffsetFilter : public SoLoud::Filter
	{
		struct Instance : public SoLoud::FilterInstance
		{
			float mOffset;
			explicit Instance(float aOffset) : mOffset(aOffset) { initParams(1); }
			void filter(float *aBuffer, unsigned int aSamples, unsigned int aBufferSize,
				unsigned int aChannels, float /*aSamplerate*/, SoLoud::time /*aTime*/) override
			{
				for (unsigned int ch = 0; ch < aChannels; ch++)
					for (unsigned int i = 0; i < aSamples; i++)
						aBuffer[ch * aBufferSize + i] += mOffset;
			}
		};
		float mOffset;
		explicit OffsetFilter(float aOffset) : mOffset(aOffset) {}
		SoLoud::FilterInstance *createInstance() override { return new Instance(mOffset); }
	};

	struct EnvParams
	{
		float threshold, ratio, attack, release;
	};

	// Independent reference for the per-sample log-domain envelope.
	// aDetectorPeak[i] is the max absolute detector sample at frame i.
	std::vector<float> referenceEnvelope(const std::vector<float> &aDetectorPeak,
		const EnvParams &aParams)
	{
		const float attackCoeff = aParams.attack == 0.0f ? 0.0f :
			std::exp(-1.0f / ((float)SR * aParams.attack));
		const float releaseCoeff = aParams.release == 0.0f ? 0.0f :
			std::exp(-1.0f / ((float)SR * aParams.release));
		const float exponent = 1.0f - 1.0f / aParams.ratio;

		std::vector<float> gain;
		gain.reserve(aDetectorPeak.size());
		float logGain = 0.0f;
		for (float peak : aDetectorPeak)
		{
			float targetLogGain = 0.0f;
			if (peak > aParams.threshold)
				targetLogGain = (float)std::log((double)aParams.threshold / (double)peak) * exponent;
			float coeff = (targetLogGain < logGain) ? attackCoeff : releaseCoeff;
			logGain = coeff * logGain + (1.0f - coeff) * targetLogGain;
			gain.push_back((float)std::exp((double)logGain));
		}
		return gain;
	}

	void expectClose(const std::vector<float> &aGot, const std::vector<float> &aExpected,
		const char *aReason)
	{
		contract_expect(aGot.size() == aExpected.size(), aReason);
		for (size_t i = 0; i < aGot.size(); i++)
		{
			float tolerance = ABS_TOL + REL_TOL * std::max(std::fabs(aGot[i]), std::fabs(aExpected[i]));
			contract_expect(std::fabs(aGot[i] - aExpected[i]) <= tolerance, aReason);
		}
	}

	void expectClose(float aGot, float aExpected, const char *aReason)
	{
		float tolerance = ABS_TOL + REL_TOL * std::max(std::fabs(aGot), std::fabs(aExpected));
		contract_expect(std::fabs(aGot - aExpected) <= tolerance, aReason);
	}

	struct Engine
	{
		SoLoud::Soloud mSoloud;
		explicit Engine(unsigned int aChannels = 1)
		{
			SoLoud::result r = mSoloud.init(0, SoLoud::Soloud::NULLDRIVER, SR, 2048, aChannels);
			contract_expect(r == SoLoud::SO_NO_ERROR, "null driver init");
			mSoloud.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
			mSoloud.setPostClipScaler(1.0f);
		}
	};

	// Fix volume and pan at play time: while paused, setVolume() refreshes the
	// per-channel ramp state (mCurrentChannelVolume) to the final value, so the
	// mix never performs a startup volume ramp. Deterministic across chunk sizes.
	void stabilizeVoice(SoLoud::Soloud &aSoloud, SoLoud::handle aHandle, float aVolume = 1.0f)
	{
		aSoloud.setPause(aHandle, true);
		aSoloud.setPanAbsolute(aHandle, 1.0f, 1.0f);
		aSoloud.setVolume(aHandle, aVolume);
		aSoloud.setPause(aHandle, false);
	}

	SoLoud::handle makeBus(SoLoud::Soloud &aSoloud, SoLoud::Bus &aBus, unsigned int aChannels)
	{
		aBus.setChannels(aChannels);
		aBus.mBaseSamplerate = (float)SR;
		aBus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
		SoLoud::handle h = aSoloud.play(aBus);
		stabilizeVoice(aSoloud, h);
		return h;
	}

	SoLoud::handle playSource(SoLoud::Soloud &aSoloud, SoLoud::Bus &aBus, GenSource &aSource)
	{
		SoLoud::handle h = aBus.play(aSource);
		stabilizeVoice(aSoloud, h);
		return h;
	}

	SoLoud::handle playRoot(SoLoud::Soloud &aSoloud, GenSource &aSource)
	{
		SoLoud::handle h = aSoloud.play(aSource);
		stabilizeVoice(aSoloud, h);
		return h;
	}

	std::vector<float> mixWhole(SoLoud::Soloud &aSoloud, unsigned int aFrames, unsigned int aChannels)
	{
		std::vector<float> out((size_t)aFrames * aChannels);
		aSoloud.mix(out.data(), aFrames);
		return out;
	}

	std::vector<float> mixChunks(SoLoud::Soloud &aSoloud, const std::vector<unsigned int> &aChunks,
		unsigned int aChannels)
	{
		std::vector<float> out;
		for (unsigned int n : aChunks)
		{
			std::vector<float> tmp((size_t)n * aChannels);
			aSoloud.mix(tmp.data(), n);
			out.insert(out.end(), tmp.begin(), tmp.end());
		}
		return out;
	}

	// Scene used by the clock tests: one detector bus, one target bus, both mono.
	struct MonoScene
	{
		Engine mEngine;
		SoLoud::Bus mDetectorBus, mTargetBus;
		SoLoud::handle mDetectorHandle, mTargetHandle;
		std::unique_ptr<GenSource> mDetectorSource, mTargetSource;

		MonoScene(unsigned int aChannels, std::function<float(unsigned int, unsigned int)> aDetectorFn,
			float aTone, const EnvParams *aParams)
			: mEngine(aChannels)
		{
			mDetectorHandle = makeBus(mEngine.mSoloud, mDetectorBus, aChannels);
			mTargetHandle = makeBus(mEngine.mSoloud, mTargetBus, aChannels);
			mDetectorSource.reset(new GenSource(aChannels, 8192, std::move(aDetectorFn)));
			mTargetSource.reset(new GenSource(aChannels, 8192,
				[=](unsigned int, unsigned int) { return aTone; }));
			playSource(mEngine.mSoloud, mDetectorBus, *mDetectorSource);
			playSource(mEngine.mSoloud, mTargetBus, *mTargetSource);
			if (aParams)
			{
				SoLoud::result r = mEngine.mSoloud.setBusDucker(mTargetHandle, mDetectorHandle,
					aParams->threshold, aParams->ratio, aParams->attack, aParams->release);
				contract_expect(r == SoLoud::SO_NO_ERROR, "setBusDucker in mono scene");
			}
		}
	};

	// Build the interleaved expected output for a mono detector + mono target.
	std::vector<float> expectedMono(const std::function<float(unsigned int)> &aDetectorFn,
		const std::vector<float> &aGain, float aTone, unsigned int aFrames,
		float aDetectorAudibleScale = 1.0f)
	{
		std::vector<float> out(aFrames);
		for (unsigned int i = 0; i < aFrames; i++)
			out[i] = aDetectorFn(i) * aDetectorAudibleScale + aTone * aGain[i];
		return out;
	}

	std::vector<float> detectorPeakMono(const std::function<float(unsigned int)> &aFn, unsigned int aFrames)
	{
		std::vector<float> peak(aFrames);
		for (unsigned int i = 0; i < aFrames; i++)
			peak[i] = std::fabs(aFn(i));
		return peak;
	}
}

void register_contract()
{
	// ------------------------------------------------------------------ envelope
	contract_add("envelope", "pulse_timing_and_gain", [] {
		const EnvParams params{ 0.3f, 4.0f, 0.002f, 0.005f };
		auto detFn = [](unsigned int i) { return (i >= 600 && i < 620) ? 0.5f : 0.0f; };
		MonoScene scene(1, [](unsigned int, unsigned int i) { return (i >= 600 && i < 620) ? 0.5f : 0.0f; },
			0.25f, &params);
		const unsigned frames = 1024;
		std::vector<float> got = mixWhole(scene.mEngine.mSoloud, frames, 1);
		std::vector<float> peak = detectorPeakMono(detFn, frames);
		std::vector<float> gain = referenceEnvelope(peak, params);
		std::vector<float> expected = expectedMono(detFn, gain, 0.25f, frames);
		expectClose(got, expected, "pulse envelope mismatch");
		// Before the pulse the target stays at unity gain.
		expectClose(got[0], 0.25f, "unity gain before pulse");
		expectClose(got[599], 0.25f, "unity gain on frame before pulse");
		// The pulse itself is audible through the detector bus.
		contract_expect(gain[600] < 1.0f, "ducking starts on the exact pulse frame");
		// The source was consumed once, in 512-frame blocks.
		contract_expect(scene.mDetectorSource->mReadFrames == 1024, "detector source read once");
		contract_expect(scene.mTargetSource->mReadFrames == 1024, "target source read once");
	});

	contract_add("envelope", "stereo_peak_uses_both_channels", [] {
		const EnvParams params{ 0.3f, 8.0f, 0.0f, 0.0f };
		auto detFn = [](unsigned int ch, unsigned int i) {
			if (ch == 0) return i >= 100 ? 0.45f : 0.0f;
			return 0.15f;
		};
		MonoScene scene(2, detFn, 0.35f, &params);
		const unsigned frames = 512;
		std::vector<float> got = mixWhole(scene.mEngine.mSoloud, frames, 2);
		std::vector<float> peak(frames);
		for (unsigned i = 0; i < frames; i++)
			peak[i] = std::max(std::fabs(detFn(0, i)), std::fabs(detFn(1, i)));
		std::vector<float> gain = referenceEnvelope(peak, params);
		std::vector<float> expected((size_t)frames * 2);
		for (unsigned i = 0; i < frames; i++)
			for (unsigned ch = 0; ch < 2; ch++)
				expected[i * 2 + ch] = detFn(ch, i) + 0.35f * gain[i];
		expectClose(got, expected, "stereo envelope mismatch");
		expectClose(got[50 * 2], 0.35f, "left unity before step");
		expectClose(got[50 * 2 + 1], 0.5f, "right unity before step");
		contract_expect(gain[200] < 1.0f, "stereo max peak drives compression");
	});

	contract_add("envelope", "target_filter_runs_before_compression", [] {
		const EnvParams params{ 0.25f, 2.0f, 0.0f, 0.0f };
		auto detFn = [](unsigned int i) { return i == 300 ? 0.5f : 0.0f; };
		Engine engine(1);
		SoLoud::Bus detectorBus, targetBus;
		SoLoud::handle dh = makeBus(engine.mSoloud, detectorBus, 1);
		SoLoud::handle th = makeBus(engine.mSoloud, targetBus, 1);
		OffsetFilter filter(0.1f);
		targetBus.setFilter(0, &filter);
		GenSource det(1, 1024, [](unsigned int, unsigned int i) { return i == 300 ? 0.5f : 0.0f; });
		GenSource tgt(1, 1024, [](unsigned int, unsigned int) { return 0.3f; });
		playSource(engine.mSoloud, detectorBus, det);
		playSource(engine.mSoloud, targetBus, tgt);
		contract_expect(engine.mSoloud.setBusDucker(th, dh, params.threshold, params.ratio,
			params.attack, params.release) == SoLoud::SO_NO_ERROR, "ducker with filtered target");
		const unsigned frames = 512;
		std::vector<float> got = mixWhole(engine.mSoloud, frames, 1);
		std::vector<float> gain = referenceEnvelope(detectorPeakMono(detFn, frames), params);
		std::vector<float> expected = expectedMono(detFn, gain, 0.3f + 0.1f, frames);
		expectClose(got, expected, "filter-before-gain ordering mismatch");
		expectClose(got[0], 0.4f, "filtered target at unity gain");
	});

	contract_add("envelope", "release_towards_unity_after_silence", [] {
		const EnvParams params{ 0.3f, 4.0f, 0.0f, 0.001f };
		auto detFn = [](unsigned int i) { return i < 256 ? 0.5f : 0.0f; };
		MonoScene scene(1, [](unsigned int, unsigned int i) { return i < 256 ? 0.5f : 0.0f; },
			0.25f, &params);
		const unsigned frames = 1024;
		std::vector<float> got = mixWhole(scene.mEngine.mSoloud, frames, 1);
		std::vector<float> gain = referenceEnvelope(detectorPeakMono(detFn, frames), params);
		std::vector<float> expected = expectedMono(detFn, gain, 0.25f, frames);
		expectClose(got, expected, "release envelope mismatch");
		contract_expect(gain[255] < gain[400], "gain recovers during release");
		expectClose(gain[1023], 1.0f, "fully released well after silence");
	});

	// --------------------------------------------------------------------- graph
	contract_add("graph", "invalid_handles_and_parameters", [] {
		Engine engine(1);
		SoLoud::Bus a, b;
		SoLoud::handle ah = makeBus(engine.mSoloud, a, 1);
		SoLoud::handle bh = makeBus(engine.mSoloud, b, 1);
		GenSource src(1, 512, [](unsigned int, unsigned int) { return 0.1f; });
		SoLoud::handle voice = playRoot(engine.mSoloud, src);
		SoLoud::handle group = engine.mSoloud.createVoiceGroup();
		engine.mSoloud.stop(voice);
		// A separate, still-live plain source is not a bus and cannot take part.
		GenSource live(1, 512, [](unsigned int, unsigned int) { return 0.1f; });
		SoLoud::handle liveVoice = playRoot(engine.mSoloud, live);
		contract_expect(engine.mSoloud.isValidVoiceHandle(liveVoice), "plain voice is live");
		contract_expect(engine.mSoloud.setBusDucker(liveVoice, bh, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "plain source target");
		contract_expect(engine.mSoloud.setBusDucker(ah, liveVoice, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "plain source detector");
		contract_expect(engine.mSoloud.setBusDucker(voice, bh, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "stale handle");
		contract_expect(engine.mSoloud.setBusDucker(ah, voice, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "stale detector");
		contract_expect(engine.mSoloud.setBusDucker(group, bh, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "voice group target");
		contract_expect(engine.mSoloud.setBusDucker(ah, group, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "voice group detector");
		contract_expect(engine.mSoloud.setBusDucker(0, bh, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "zero target");
		contract_expect(engine.mSoloud.setBusDucker(ah, ah, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "self listening");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, 0.0f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "zero threshold");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, -0.1f, 2, 0, 0) == SoLoud::INVALID_PARAMETER, "negative threshold");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, std::numeric_limits<float>::infinity(), 2, 0, 0) == SoLoud::INVALID_PARAMETER, "infinite threshold");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, std::nanf(""), 2, 0, 0) == SoLoud::INVALID_PARAMETER, "nan threshold");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, 0.3f, 0.5f, 0, 0) == SoLoud::INVALID_PARAMETER, "ratio below one");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, 0.3f, std::nanf(""), 0, 0) == SoLoud::INVALID_PARAMETER, "nan ratio");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, 0.3f, 2, -0.1f, 0) == SoLoud::INVALID_PARAMETER, "negative attack");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, 0.3f, 2, 0, std::nanf("")) == SoLoud::INVALID_PARAMETER, "nan release");
		contract_expect(engine.mSoloud.setBusDucker(ah, bh, 0.3f, 1.0f, 0.0f, 0.0f) == SoLoud::SO_NO_ERROR, "ratio one and zero times accepted");
	});

	contract_add("graph", "nested_containment_cycle_rejected", [] {
		Engine engine(1);
		SoLoud::Bus parent, child, detector;
		SoLoud::handle ph = makeBus(engine.mSoloud, parent, 1);
		child.setChannels(1);
		child.mBaseSamplerate = (float)SR;
		child.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
		SoLoud::handle ch = parent.play(child);
		stabilizeVoice(engine.mSoloud, ch);
		SoLoud::handle dh = makeBus(engine.mSoloud, detector, 1);
		// child listening to its own parent bus closes a containment cycle.
		contract_expect(engine.mSoloud.setBusDucker(ch, ph, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER,
			"child->parent sidechain is a cycle");
		// A valid connection on the child must survive the failed reconfiguration.
		contract_expect(engine.mSoloud.setBusDucker(ch, dh, 0.3f, 2, 0, 0) == SoLoud::SO_NO_ERROR,
			"valid replacement connection");
		contract_expect(engine.mSoloud.setBusDucker(ch, ph, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER,
			"cycle still rejected");
		GenSource pulse(1, 1024, [](unsigned int, unsigned int i) { return i == 10 ? 0.5f : 0.0f; });
		GenSource tone(1, 1024, [](unsigned int, unsigned int) { return 0.3f; });
		playSource(engine.mSoloud, detector, pulse);
		playSource(engine.mSoloud, child, tone);
		std::vector<float> got = mixWhole(engine.mSoloud, 256, 1);
		// The retained detector connection still compresses the child target.
		// Subtract the audible detector pulse to inspect the target contribution.
		contract_expect((got[10] - 0.5f) < got[0], "retained connection still ducks");
	});

	contract_add("graph", "sidechain_edges_alone_are_not_enough", [] {
		Engine engine(1);
		SoLoud::Bus parent, child, d;
		SoLoud::handle ph = makeBus(engine.mSoloud, parent, 1);
		child.setChannels(1);
		child.mBaseSamplerate = (float)SR;
		child.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
		SoLoud::handle ch = parent.play(child);
		stabilizeVoice(engine.mSoloud, ch);
		SoLoud::handle dh = makeBus(engine.mSoloud, d, 1);
		// Sidechain-only edges child->d and d->parent would be acyclic; combined
		// with the containment edge parent->child they close parent->child->d->parent.
		contract_expect(engine.mSoloud.setBusDucker(ch, dh, 0.3f, 2, 0, 0) == SoLoud::SO_NO_ERROR,
			"child listening to external bus is acyclic");
		contract_expect(engine.mSoloud.setBusDucker(dh, ph, 0.3f, 2, 0, 0) == SoLoud::INVALID_PARAMETER,
			"combined containment and sidechain cycle rejected");
	});

	contract_add("graph", "sidechain_through_nested_busses", [] {
		const EnvParams params{ 0.3f, 4.0f, 0.0f, 0.0f };
		auto detFn = [](unsigned int i) { return (i >= 300 && i < 320) ? 0.5f : 0.0f; };
		const unsigned frames = 768;
		auto run = [&](bool chunked) {
			Engine engine(1);
			SoLoud::Bus parent, child, detector;
			makeBus(engine.mSoloud, parent, 1);
			child.setChannels(1);
			child.mBaseSamplerate = (float)SR;
			child.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
			SoLoud::handle ch = parent.play(child);
			stabilizeVoice(engine.mSoloud, ch);
			SoLoud::handle dh = makeBus(engine.mSoloud, detector, 1);
			contract_expect(engine.mSoloud.setBusDucker(ch, dh, params.threshold, params.ratio,
				params.attack, params.release) == SoLoud::SO_NO_ERROR, "nested target accepts ducker");
			GenSource ds(1, 4096, [](unsigned int, unsigned int i) { return (i >= 300 && i < 320) ? 0.5f : 0.0f; });
			GenSource ts(1, 4096, [](unsigned int, unsigned int) { return 0.2f; });
			playSource(engine.mSoloud, detector, ds);
			playSource(engine.mSoloud, child, ts);
			std::vector<float> out = chunked
				? mixChunks(engine.mSoloud, { 257u, 257u, 254u }, 1)
				: mixWhole(engine.mSoloud, frames, 1);
			contract_expect(ds.mReadFrames == 1024 && ts.mReadFrames == 1024,
				"nested sources read once per block");
			return out;
		};
		std::vector<float> whole = run(false);
		std::vector<float> chunked = run(true);
		expectClose(chunked, whole, "nested sidechain chunk mismatch");
		std::vector<float> gain = referenceEnvelope(detectorPeakMono(detFn, frames), params);
		std::vector<float> expected = expectedMono(detFn, gain, 0.2f, frames);
		expectClose(whole, expected, "nested sidechain numeric reference mismatch");
	});

	contract_add("graph", "cascaded_and_shared_detector", [] {
		const EnvParams p1{ 0.3f, 4.0f, 0.0f, 0.0f };
		const EnvParams p2{ 0.2f, 2.0f, 0.0f, 0.0f };
		auto build = [&] {
			struct Scene { Engine mEngine{1}; SoLoud::Bus d, t1, t2; };
			auto s = std::make_shared<Scene>();
			SoLoud::handle dh = makeBus(s->mEngine.mSoloud, s->d, 1);
			SoLoud::handle t1h = makeBus(s->mEngine.mSoloud, s->t1, 1);
			SoLoud::handle t2h = makeBus(s->mEngine.mSoloud, s->t2, 1);
			contract_expect(s->mEngine.mSoloud.setBusDucker(t1h, dh, p1.threshold, p1.ratio, p1.attack, p1.release) == SoLoud::SO_NO_ERROR, "cascade edge one");
			contract_expect(s->mEngine.mSoloud.setBusDucker(t2h, t1h, p2.threshold, p2.ratio, p2.attack, p2.release) == SoLoud::SO_NO_ERROR, "cascade edge two");
			return s;
		};
		auto detFn = [](unsigned int i) { return (i >= 550 && i < 580) ? 0.5f : 0.0f; };
		const unsigned frames = 768;
		auto run = [&](bool chunked) {
			auto s = build();
			GenSource ds(1, 4096, [](unsigned int, unsigned int i) { return (i >= 550 && i < 580) ? 0.5f : 0.0f; });
			GenSource s1(1, 4096, [](unsigned int, unsigned int) { return 0.3f; });
			GenSource s2(1, 4096, [](unsigned int, unsigned int) { return 0.15f; });
			playSource(s->mEngine.mSoloud, s->d, ds);
			playSource(s->mEngine.mSoloud, s->t1, s1);
			playSource(s->mEngine.mSoloud, s->t2, s2);
			std::vector<float> out = chunked
				? mixChunks(s->mEngine.mSoloud, { 1u, 7u, 257u, 503u }, 1)
				: mixWhole(s->mEngine.mSoloud, frames, 1);
			contract_expect(ds.mReadFrames == 1024 && s1.mReadFrames == 1024 && s2.mReadFrames == 1024,
				"cascade sources each advanced once per block");
			return out;
		};
		std::vector<float> whole = run(false);
		std::vector<float> chunks = run(true);
		expectClose(chunks, whole, "cascade chunking changes output");
		std::vector<float> peak1 = detectorPeakMono(detFn, frames);
		std::vector<float> g1 = referenceEnvelope(peak1, p1);
		std::vector<float> peak2(frames);
		for (unsigned i = 0; i < frames; i++) peak2[i] = 0.3f * g1[i];
		std::vector<float> g2 = referenceEnvelope(peak2, p2);
		std::vector<float> expected(frames);
		for (unsigned i = 0; i < frames; i++)
			expected[i] = detFn(i) + 0.3f * g1[i] + 0.15f * g2[i];
		expectClose(whole, expected, "cascade numeric reference mismatch");

		// One detector driving two targets shares the same control signal.
		Engine engine(1);
		SoLoud::Bus d, t1, t2;
		SoLoud::handle dh = makeBus(engine.mSoloud, d, 1);
		SoLoud::handle t1h = makeBus(engine.mSoloud, t1, 1);
		SoLoud::handle t2h = makeBus(engine.mSoloud, t2, 1);
		contract_expect(engine.mSoloud.setBusDucker(t1h, dh, 0.3f, 4, 0, 0) == SoLoud::SO_NO_ERROR, "shared edge one");
		contract_expect(engine.mSoloud.setBusDucker(t2h, dh, 0.3f, 4, 0, 0) == SoLoud::SO_NO_ERROR, "shared edge two");
		GenSource ds(1, 2048, [](unsigned int, unsigned int i) { return i == 33 ? 0.5f : 0.0f; });
		GenSource s1(1, 2048, [](unsigned int, unsigned int) { return 0.3f; });
		GenSource s2(1, 2048, [](unsigned int, unsigned int) { return 0.15f; });
		playSource(engine.mSoloud, d, ds);
		playSource(engine.mSoloud, t1, s1);
		playSource(engine.mSoloud, t2, s2);
		std::vector<float> got = mixWhole(engine.mSoloud, 128, 1);
		contract_expect(ds.mReadFrames == 512, "shared detector source read only once");
		std::vector<float> gain = referenceEnvelope(detectorPeakMono([](unsigned int i) { return i == 33 ? 0.5f : 0.0f; }, 128),
			EnvParams{ 0.3f, 4.0f, 0.0f, 0.0f });
		std::vector<float> expectedShared(128);
		for (unsigned i = 0; i < 128; i++)
			expectedShared[i] = (i == 33 ? 0.5f : 0.0f) + 0.3f * gain[i] + 0.15f * gain[i];
		expectClose(got, expectedShared, "shared detector reference mismatch");
	});

	// --------------------------------------------------------------------- clock
	contract_add("clock", "whole_block_matches_1_7_257_chunks", [] {
		const EnvParams params{ 0.3f, 4.0f, 0.002f, 0.006f };
		auto detFn = [](unsigned int i) { return (i >= 600 && i < 620) ? 0.5f : 0.0f; };
		const unsigned frames = 2048;
		auto makeOut = [&](bool chunked) {
			MonoScene scene(1, [](unsigned int, unsigned int i) { return (i >= 600 && i < 620) ? 0.5f : 0.0f; },
				0.25f, &params);
			std::vector<float> out = chunked
				? mixChunks(scene.mEngine.mSoloud, { 1u, 7u, 257u, 512u, 1024u, 247u }, 1)
				: mixWhole(scene.mEngine.mSoloud, frames, 1);
			contract_expect(scene.mDetectorSource->mReadFrames == frames, "chunked detector consumption");
			return out;
		};
		std::vector<float> whole = makeOut(false);
		std::vector<float> chunks = makeOut(true);
		expectClose(chunks, whole, "chunked and whole mixes differ");
		std::vector<float> gain = referenceEnvelope(detectorPeakMono(detFn, frames), params);
		std::vector<float> expected = expectedMono(detFn, gain, 0.25f, frames);
		expectClose(whole, expected, "independent numeric reference mismatch");
	});

	contract_add("clock", "pulse_straddling_512_boundary", [] {
		const EnvParams params{ 0.3f, 4.0f, 0.0f, 0.0f };
		auto detFn = [](unsigned int i) { return (i >= 505 && i <= 520) ? 0.5f : 0.0f; };
		const unsigned frames = 1024;
		MonoScene wholeScene(1, [](unsigned int, unsigned int i) { return (i >= 505 && i <= 520) ? 0.5f : 0.0f; },
			0.25f, &params);
		MonoScene chunkScene(1, [](unsigned int, unsigned int i) { return (i >= 505 && i <= 520) ? 0.5f : 0.0f; },
			0.25f, &params);
		std::vector<float> whole = mixWhole(wholeScene.mEngine.mSoloud, frames, 1);
		std::vector<float> chunked = mixChunks(chunkScene.mEngine.mSoloud, { 500u, 100u, 424u }, 1);
		expectClose(chunked, whole, "boundary straddle chunk mismatch");
		std::vector<float> gain = referenceEnvelope(detectorPeakMono(detFn, frames), params);
		expectClose(whole, expectedMono(detFn, gain, 0.25f, frames), "boundary straddle reference mismatch");
		expectClose(gain[504], 1.0f, "no ducking before straddling pulse");
		contract_expect(gain[505] < 1.0f, "ducking begins at the exact first pulse frame");
	});

	contract_add("clock", "zero_volume_detector_still_controls", [] {
		const EnvParams params{ 0.3f, 4.0f, 0.0f, 0.0f };
		Engine engine(1);
		SoLoud::Bus detectorBus, targetBus;
		SoLoud::handle dh = makeBus(engine.mSoloud, detectorBus, 1);
		SoLoud::handle th = makeBus(engine.mSoloud, targetBus, 1);
		detectorBus.setVisualizationEnable(false);
		// Mute at play time without a startup volume ramp.
		stabilizeVoice(engine.mSoloud, dh, 0.0f);
		contract_expect(engine.mSoloud.setBusDucker(th, dh, 0.3f, 4, 0, 0) == SoLoud::SO_NO_ERROR,
			"muted bus accepted as detector");
		GenSource det(1, 1024, [](unsigned int, unsigned int i) { return i == 77 ? 0.5f : 0.0f; });
		GenSource tgt(1, 1024, [](unsigned int, unsigned int) { return 0.25f; });
		playSource(engine.mSoloud, detectorBus, det);
		playSource(engine.mSoloud, targetBus, tgt);
		std::vector<float> got = mixWhole(engine.mSoloud, 256, 1);
		std::vector<float> gain = referenceEnvelope(
			detectorPeakMono([](unsigned int i) { return i == 77 ? 0.5f : 0.0f; }, 256), params);
		std::vector<float> expected = expectedMono([](unsigned int) { return 0.0f; }, gain, 0.25f, 256);
		expectClose(got, expected, "muted detector control mismatch");
		expectClose(got[0], 0.25f, "muted detector is inaudible");
		contract_expect(got[77] < 0.25f, "muted detector still ducks the target");
	});

	contract_add("clock", "no_sidechain_scene_unchanged", [] {
		auto fn = [](unsigned int, unsigned int i) { return 0.25f + 0.01f * (i % 7); };
		Engine e1(1), e2(1);
		SoLoud::Bus b1, b2;
		makeBus(e1.mSoloud, b1, 1);
		makeBus(e2.mSoloud, b2, 1);
		GenSource s1(1, 2048, fn), s2(1, 2048, fn);
		playSource(e1.mSoloud, b1, s1);
		playSource(e2.mSoloud, b2, s2);
		std::vector<float> whole = mixWhole(e1.mSoloud, 777, 1);
		std::vector<float> chunks = mixChunks(e2.mSoloud, { 1u, 7u, 257u, 512u }, 1);
		expectClose(chunks, whole, "plain bus chunking changed behavior");
		expectClose(whole[0], fn(0, 0), "no added latency on first frame");
	});

	// ------------------------------------------------------------------ lifetime
	contract_add("lifetime", "detector_stop_releases_from_silence", [] {
		const EnvParams params{ 0.3f, 4.0f, 0.001f, 0.004f };
		Engine engine(1);
		SoLoud::Bus detectorBus, targetBus;
		SoLoud::handle dh = makeBus(engine.mSoloud, detectorBus, 1);
		SoLoud::handle th = makeBus(engine.mSoloud, targetBus, 1);
		contract_expect(engine.mSoloud.setBusDucker(th, dh, 0.3f, 4, 0.001f, 0.004f) == SoLoud::SO_NO_ERROR,
			"lifetime ducker configured");
		GenSource det(1, 4096, [](unsigned int, unsigned int) { return 0.5f; });
		GenSource tgt(1, 4096, [](unsigned int, unsigned int) { return 0.25f; });
		playSource(engine.mSoloud, detectorBus, det);
		playSource(engine.mSoloud, targetBus, tgt);
		std::vector<float> first = mixWhole(engine.mSoloud, 512, 1);
		engine.mSoloud.stop(dh);
		contract_expect(!engine.mSoloud.isValidVoiceHandle(dh), "detector bus stopped at block boundary");
		std::vector<float> second = mixWhole(engine.mSoloud, 512, 1);
		std::vector<float> peak(1024, 0.5f);
		for (unsigned i = 512; i < 1024; i++) peak[i] = 0.0f;
		std::vector<float> gain = referenceEnvelope(peak, params);
		std::vector<float> expected(1024);
		for (unsigned i = 0; i < 512; i++) expected[i] = 0.5f + 0.25f * gain[i];
		for (unsigned i = 512; i < 1024; i++) expected[i] = 0.25f * gain[i];
		std::vector<float> got;
		got.insert(got.end(), first.begin(), first.end());
		got.insert(got.end(), second.begin(), second.end());
		expectClose(got, expected, "post-stop release mismatch");
		contract_expect(gain[600] > gain[511], "envelope releases after detector stop");
	});

	contract_add("lifetime", "stopped_slot_reused_by_plain_source", [] {
		Engine engine(1);
		SoLoud::Bus detectorBus, targetBus;
		SoLoud::handle dh = makeBus(engine.mSoloud, detectorBus, 1);
		SoLoud::handle th = makeBus(engine.mSoloud, targetBus, 1);
		contract_expect(engine.mSoloud.setBusDucker(th, dh, 0.3f, 4, 0, 0) == SoLoud::SO_NO_ERROR, "ducker set");
		GenSource det(1, 1024, [](unsigned int, unsigned int) { return 0.5f; });
		GenSource tgt(1, 4096, [](unsigned int, unsigned int) { return 0.25f; });
		playSource(engine.mSoloud, detectorBus, det);
		playSource(engine.mSoloud, targetBus, tgt);
		mixWhole(engine.mSoloud, 512, 1);
		engine.mSoloud.stop(dh);
		// A plain source reuses voice slots; it must not become the detector.
		GenSource loud(1, 4096, [](unsigned int, unsigned int) { return 0.5f; });
		SoLoud::handle lh = playRoot(engine.mSoloud, loud);
		contract_expect(lh != 0, "replacement voice started");
		std::vector<float> got = mixWhole(engine.mSoloud, 512, 1);
		for (unsigned i = 0; i < 512; i++)
			expectClose(got[i], 0.25f + 0.5f, "reused slot does not feed the sidechain");
	});

	contract_add("lifetime", "stopping_one_target_keeps_other_listener", [] {
		Engine engine(1);
		SoLoud::Bus d, t1, t2;
		SoLoud::handle dh = makeBus(engine.mSoloud, d, 1);
		SoLoud::handle t1h = makeBus(engine.mSoloud, t1, 1);
		SoLoud::handle t2h = makeBus(engine.mSoloud, t2, 1);
		contract_expect(engine.mSoloud.setBusDucker(t1h, dh, 0.3f, 4, 0, 0) == SoLoud::SO_NO_ERROR, "listener one");
		contract_expect(engine.mSoloud.setBusDucker(t2h, dh, 0.3f, 4, 0, 0) == SoLoud::SO_NO_ERROR, "listener two");
		GenSource ds(1, 4096, [](unsigned int, unsigned int) { return 0.5f; });
		GenSource s1(1, 4096, [](unsigned int, unsigned int) { return 0.25f; });
		GenSource s2(1, 4096, [](unsigned int, unsigned int) { return 0.15f; });
		playSource(engine.mSoloud, d, ds);
		playSource(engine.mSoloud, t1, s1);
		playSource(engine.mSoloud, t2, s2);
		mixWhole(engine.mSoloud, 512, 1);
		engine.mSoloud.stop(t1h);
		std::vector<float> got = mixWhole(engine.mSoloud, 256, 1);
		std::vector<float> gain = referenceEnvelope(std::vector<float>(256, 0.5f), EnvParams{ 0.3f, 4.0f, 0.0f, 0.0f });
		for (unsigned i = 0; i < 256; i++)
			expectClose(got[i], 0.5f + 0.15f * gain[i], "remaining listener still ducked");
		contract_expect(ds.mReadFrames == 1024, "detector source not re-read for missing target");
	});

	contract_add("lifetime", "reconfigure_replaces_and_resets", [] {
		Engine engine(1);
		SoLoud::Bus d1, d2, target;
		SoLoud::handle d1h = makeBus(engine.mSoloud, d1, 1);
		SoLoud::handle d2h = makeBus(engine.mSoloud, d2, 1);
		SoLoud::handle th = makeBus(engine.mSoloud, target, 1);
		contract_expect(engine.mSoloud.setBusDucker(th, d1h, 0.3f, 4, 0, 0) == SoLoud::SO_NO_ERROR, "first connection");
		contract_expect(engine.mSoloud.setBusDucker(th, d2h, 0.3f, 4, 0, 0) == SoLoud::SO_NO_ERROR, "replacement connection");
		GenSource s1(1, 512, [](unsigned int, unsigned int) { return 0.5f; });
		GenSource s2(1, 512, [](unsigned int, unsigned int i) { return i == 5 ? 0.5f : 0.0f; });
		GenSource tgt(1, 512, [](unsigned int, unsigned int) { return 0.25f; });
		playSource(engine.mSoloud, d1, s1);
		playSource(engine.mSoloud, d2, s2);
		playSource(engine.mSoloud, target, tgt);
		std::vector<float> got = mixWhole(engine.mSoloud, 64, 1);
		// d1 is loud the whole time but no longer connected; its bus stays audible
		// though, so inspect the target contribution with the detector pulses removed.
		expectClose(got[0], 0.5f + 0.25f, "old detector no longer ducks at unity frames");
		float targetAtPulse = got[5] - 0.5f - 0.5f;
		contract_expect(targetAtPulse < 0.25f, "new detector drives the target");
		// Configuration is frozen once mixing has started.
		contract_expect(engine.mSoloud.setBusDucker(th, d1h, 0.3f, 4, 0, 0) == SoLoud::INVALID_PARAMETER,
			"post-mix configuration rejected");
	});
}

