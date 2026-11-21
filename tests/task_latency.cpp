/*
Filter latency compensation task tests.

The cases below are registered in three groups:
  graph  - planner arithmetic, topology coverage and configuration validation
  pcm    - independently predicted pulse arrival frames and amplitudes in real
           mixer output (1, 2 and 6 channels, nested buses, >512 frame delays)
  blocks - output invariance across 1/7/257-frame chunks and one-shot mixes

Every expected arrival frame and amplitude is calculated on the test side,
either from a small independent graph model or from explicit latency sums;
the engine's own compensation numbers are never used as the reference.
*/

#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_filter.h"
#include "task_support/check.h"

#include <vector>
#include <cmath>
#include <cstdint>

namespace
{
	// ---- Test fixtures -----------------------------------------------------

	// Persistent pulse source: one pulse per channel at frame mPulseFrame,
	// silence (including after the material) forever afterwards.
	struct PulseInstance : SoLoud::AudioSourceInstance
	{
		unsigned mPos = 0;
		unsigned mPulseFrame;
		float mBase;
		PulseInstance(unsigned aPulseFrame, float aBase) : mPulseFrame(aPulseFrame), mBase(aBase) {}
		unsigned getAudio(float *aBuffer, unsigned aSamples, unsigned aStride) override
		{
			for (unsigned c = 0; c < mChannels; ++c)
				for (unsigned i = 0; i < aSamples; ++i)
					aBuffer[c * aStride + i] = (mPos + i == mPulseFrame) ? (mBase * (c + 1)) : 0.f;
			mPos += aSamples;
			return aSamples;
		}
		bool hasEnded() override { return false; }
	};

	struct PulseSource : SoLoud::AudioSource
	{
		unsigned mPulseFrame;
		float mBase;
		PulseSource(unsigned aPulseFrame, float aBase, unsigned aChannels)
			: mPulseFrame(aPulseFrame), mBase(aBase)
		{
			mChannels = aChannels;
			mBaseSamplerate = 48000;
		}
		SoLoud::AudioSourceInstance *createInstance() override
		{
			return new PulseInstance(mPulseFrame, mBase);
		}
	};

	// Finite material followed by persistent zeros: the meaningful material
	// ends at mMaterialFrames, but the instance keeps returning silence and
	// never reports EOF (the only supported source shape for compensation;
	// auto-stop is switched off by the callers).
	struct FiniteInstance : SoLoud::AudioSourceInstance
	{
		unsigned mPos = 0;
		unsigned mPulseFrame;
		unsigned mMaterialFrames;
		float mBase;
		FiniteInstance(unsigned aPulseFrame, unsigned aMaterialFrames, float aBase)
			: mPulseFrame(aPulseFrame), mMaterialFrames(aMaterialFrames), mBase(aBase) {}
		unsigned getAudio(float *aBuffer, unsigned aSamples, unsigned aStride) override
		{
			for (unsigned c = 0; c < mChannels; ++c)
				for (unsigned i = 0; i < aSamples; ++i)
					aBuffer[c * aStride + i] = (mPos + i == mPulseFrame) ? (mBase * (c + 1)) : 0.f;
			mPos += aSamples;
			return aSamples;
		}
		bool hasEnded() override { return false; }
	};

	struct FiniteSource : SoLoud::AudioSource
	{
		unsigned mPulseFrame;
		unsigned mMaterialFrames;
		float mBase;
		FiniteSource(unsigned aPulseFrame, unsigned aMaterialFrames, float aBase, unsigned aChannels)
			: mPulseFrame(aPulseFrame), mMaterialFrames(aMaterialFrames), mBase(aBase)
		{
			mChannels = aChannels;
			mBaseSamplerate = 48000;
		}
		SoLoud::AudioSourceInstance *createInstance() override
		{
			return new FiniteInstance(mPulseFrame, mMaterialFrames, mBase);
		}
	};

	// Pure per-channel delay line filter; position advances per frame.
	struct DelayInstance : SoLoud::FilterInstance
	{
		unsigned mDelay;
		unsigned mPos = 0;
		std::vector<float> mRing;
		explicit DelayInstance(unsigned aDelay) : mDelay(aDelay), mRing(aDelay * 8, 0.f) {}
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

	// ---- Independent reference model --------------------------------------
	//
	// A node is either a source (leaf, own latency = serial filter sum) or a
	// bus junction (own latency = the bus' own filter chain). The reference
	// computes the same J(b)/compensation arithmetic as the production planner
	// without touching any engine state.
	struct RefNode
	{
		bool mIsBus;
		unsigned mOwnLatency;
		std::vector<RefNode *> mChildren;
		unsigned mJunction = 0;       // J(b) for buses
		unsigned mCompensation = 0;   // C(node) at its parent junction
		unsigned mArrival = 0;        // T(node): own latency (+ J for buses)
		RefNode(bool aIsBus, unsigned aOwnLatency) : mIsBus(aIsBus), mOwnLatency(aOwnLatency) {}
	};

	unsigned refCompute(RefNode *aBus)
	{
		unsigned longest = 0;
		for (RefNode *child : aBus->mChildren)
		{
			if (child->mIsBus)
				child->mJunction = refCompute(child);
			child->mArrival = child->mOwnLatency + (child->mIsBus ? child->mJunction : 0);
			if (child->mArrival > longest) longest = child->mArrival;
		}
		for (RefNode *child : aBus->mChildren)
			child->mCompensation = longest - child->mArrival;
		aBus->mJunction = longest;
		return longest;
	}

	// ---- Engine helpers ----------------------------------------------------

	SoLoud::Soloud *makeEngine(unsigned aChannels)
	{
		SoLoud::Soloud *e = new SoLoud::Soloud();
		if (e->init(0, SoLoud::Soloud::NULLDRIVER, 48000, 1024, aChannels) != SoLoud::SO_NO_ERROR)
		{
			delete e;
			return nullptr;
		}
		e->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		e->setPostClipScaler(1);
		return e;
	}

	void configureBus(SoLoud::Bus &aBus, unsigned aChannels)
	{
		aBus.setChannels(aChannels);
		aBus.mBaseSamplerate = 48000;
		aBus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
	}

	void unityVoice(SoLoud::Soloud *aEngine, SoLoud::handle aHandle, unsigned aChannels)
	{
		for (unsigned c = 0; c < aChannels; ++c)
			aEngine->setChannelVolume(aHandle, c, 1.0f);
		aEngine->setAutoStop(aHandle, false);
	}

	// Render aEngine after a silence warm-up that settles the pan gain ramp.
	// Output is interleaved float, aFrames long, produced in aChunk blocks.
	std::vector<float> render(SoLoud::Soloud *aEngine, unsigned aWarm, unsigned aFrames, unsigned aChunk, unsigned aChannels)
	{
		std::vector<float> warm(aWarm * aChannels, 0.f);
		unsigned done = 0;
		while (done < aWarm)
		{
			unsigned n = aWarm - done;
			if (n > aChunk) n = aChunk;
			aEngine->mix(warm.data() + done * aChannels, n);
			done += n;
		}
		std::vector<float> out(aFrames * aChannels, 0.f);
		done = 0;
		while (done < aFrames)
		{
			unsigned n = aFrames - done;
			if (n > aChunk) n = aChunk;
			aEngine->mix(out.data() + done * aChannels, n);
			done += n;
		}
		return out;
	}

	std::vector<unsigned> pulseFrames(const std::vector<float> &aOut, unsigned aChannels, unsigned aChannel, float aThreshold = 1e-7f)
	{
		std::vector<unsigned> hits;
		for (unsigned f = 0; f < (unsigned)(aOut.size() / aChannels); ++f)
			if (std::fabs(aOut[f * aChannels + aChannel]) > aThreshold)
				hits.push_back(f);
		return hits;
	}

	// ---- graph cases -------------------------------------------------------

	int graph_serial_and_bypass()
	{
		SoLoud::Soloud *e = makeEngine(2);
		TASK_CHECK(e != nullptr);
		DelayFilter f30(30), f20(20);
		PulseSource a(1000, 0.05f, 2), b(1000, 0.07f, 2);
		a.setFilter(0, &f30);
		a.setFilter(1, &f20); // serial: 30 + 20 = 50
		SoLoud::handle ha = e->playBackground(a);
		SoLoud::handle hb = e->playBackground(b);
		unityVoice(e, ha, 2);
		unityVoice(e, hb, 2);

		RefNode root{true, 0};
		RefNode ra{false, 50}, rb{false, 0};
		root.mChildren = {&ra, &rb};
		unsigned total = refCompute(&root);
		TASK_CHECK(total == 50);

		TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(e->getFilterLatencyCompensationFrames() == total);
		int ca = e->getVoiceFromHandle_internal(ha);
		int cb = e->getVoiceFromHandle_internal(hb);
		TASK_CHECK(ca >= 0 && cb >= 0);
		TASK_CHECK(e->mVoice[ca]->mLatencyCompensation == ra.mCompensation);
		TASK_CHECK(e->mVoice[cb]->mLatencyCompensation == rb.mCompensation);
		TASK_CHECK(ra.mCompensation == 0 && rb.mCompensation == 50);
		delete e;
		return 0;
	}

	int graph_three_level_buses()
	{
		SoLoud::Soloud *e = makeEngine(2);
		TASK_CHECK(e != nullptr);
		// Root children: bus A (filter 100), bus B (filter 200).
		// B contains bus C (filter 50); C contains a filtered source s3 (400).
		// A contains s1 with serial 30+20 and clean s2. Root also has s5 (60), s6 (0).
		DelayFilter f100(100), f200(200), f50(50), f400(400), f60(60), f30(30), f20(20);
		SoLoud::Bus A, B, C;
		configureBus(A, 2);
		configureBus(B, 2);
		configureBus(C, 2);
		A.setFilter(0, &f100);
		B.setFilter(0, &f200);
		C.setFilter(0, &f50);
		SoLoud::handle hA = e->playBackground(A);
		SoLoud::handle hB = e->playBackground(B);
		SoLoud::handle hC = e->playBackground(C);
		B.annexSound(hC);

		PulseSource s1(1200, 0.01f, 2), s2(1400, 0.01f, 2), s3(1600, 0.01f, 2),
			s4(1800, 0.01f, 2), s5(2000, 0.01f, 2), s6(2200, 0.01f, 2);
		s1.setFilter(0, &f30);
		s1.setFilter(1, &f20);
		s3.setFilter(0, &f400);
		s5.setFilter(0, &f60);
		SoLoud::handle k1 = A.play(s1), k2 = A.play(s2), k3 = C.play(s3), k4 = C.play(s4);
		SoLoud::handle k5 = e->playBackground(s5), k6 = e->playBackground(s6);
		for (SoLoud::handle h : {hA, hB, hC, k1, k2, k3, k4, k5, k6}) unityVoice(e, h, 2);

		// Independent reference graph.
		RefNode rA{true, 100}, rB{true, 200}, rC{true, 50};
		RefNode n1{false, 50}, n2{false, 0}, n3{false, 400}, n4{false, 0}, n5{false, 60}, n6{false, 0};
		rC.mChildren = {&n3, &n4};
		rB.mChildren = {&rC};
		rA.mChildren = {&n1, &n2};
		RefNode root{true, 0};
		root.mChildren = {&rA, &rB, &n5, &n6};
		unsigned total = refCompute(&root);
		// Hand check of the same arithmetic: J(C)=400, T(C)=450, J(B)=450,
		// T(B)=650; J(A)=50, T(A)=150; root J = max(150,650,60,0) = 650.
		TASK_CHECK(rC.mJunction == 400 && rB.mJunction == 450 && rA.mJunction == 50);
		TASK_CHECK(total == 650);

		TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(e->getFilterLatencyCompensationFrames() == 650);

		// Every production voice's compensation matches the reference node.
		SoLoud::handle handles[] = {hA, hB, hC, k1, k2, k3, k4, k5, k6};
		RefNode *refs[] = {&rA, &rB, &rC, &n1, &n2, &n3, &n4, &n5, &n6};
		for (unsigned i = 0; i < 9; ++i)
		{
			int ch = e->getVoiceFromHandle_internal(handles[i]);
			TASK_CHECK(ch >= 0);
			TASK_CHECK(e->mVoice[ch]->mLatencyCompensation == refs[i]->mCompensation);
			TASK_CHECK((refs[i]->mCompensation == 0) || (e->mVoice[ch]->mLatencyRing != 0));
		}
		delete e;
		return 0;
	}

	int graph_invalid_configs()
	{
		// Paused voice.
		{
			SoLoud::Soloud *e = makeEngine(2);
			TASK_CHECK(e != nullptr);
			PulseSource a(100, 0.05f, 2);
			SoLoud::handle h = e->play(a, 1, 0, true);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::INVALID_PARAMETER);
			TASK_CHECK(e->getFilterLatencyCompensationFrames() == 0);
			int ch = e->getVoiceFromHandle_internal(h);
			TASK_CHECK(ch >= 0 && e->mVoice[ch]->mLatencyRing == 0);
			delete e;
		}
		// Prepare after mixing started.
		{
			SoLoud::Soloud *e = makeEngine(2);
			TASK_CHECK(e != nullptr);
			PulseSource a(100, 0.05f, 2);
			SoLoud::handle h = e->playBackground(a);
			unityVoice(e, h, 2);
			std::vector<float> scratch(64 * 2);
			e->mix(scratch.data(), 64);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::INVALID_PARAMETER);
			delete e;
		}
		// Global filter.
		{
			SoLoud::Soloud *e = makeEngine(2);
			TASK_CHECK(e != nullptr);
			DelayFilter g(10);
			e->setGlobalFilter(0, &g);
			PulseSource a(100, 0.05f, 2);
			SoLoud::handle h = e->playBackground(a);
			unityVoice(e, h, 2);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::INVALID_PARAMETER);
			e->setGlobalFilter(0, 0);
			delete e;
		}
		// Delayed start.
		{
			SoLoud::Soloud *e = makeEngine(2);
			TASK_CHECK(e != nullptr);
			PulseSource a(100, 0.05f, 2);
			SoLoud::handle h = e->playBackground(a);
			unityVoice(e, h, 2);
			e->setDelaySamples(h, 16);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::INVALID_PARAMETER);
			delete e;
		}
		// Unsupported sample rate.
		{
			SoLoud::Soloud *e = new SoLoud::Soloud();
			TASK_CHECK(e->init(0, SoLoud::Soloud::NULLDRIVER, 44100, 1024, 2) == SoLoud::SO_NO_ERROR);
			e->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
			PulseSource a(100, 0.05f, 2);
			SoLoud::handle h = e->playBackground(a);
			unityVoice(e, h, 2);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::INVALID_PARAMETER);
			delete e;
		}
		// Unsupported channel count (quad is outside the 1/2/6 subset).
		{
			SoLoud::Soloud *e = makeEngine(4);
			TASK_CHECK(e != nullptr);
			PulseSource a(100, 0.05f, 4);
			SoLoud::handle h = e->playBackground(a);
			e->setAutoStop(h, false);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::INVALID_PARAMETER);
			delete e;
		}
		// Double prepare leaves the first plan intact and rejects the second.
		{
			SoLoud::Soloud *e = makeEngine(2);
			TASK_CHECK(e != nullptr);
			DelayFilter f(10);
			PulseSource a(100, 0.05f, 2), b(100, 0.05f, 2);
			a.setFilter(0, &f);
			SoLoud::handle ha = e->playBackground(a), hb = e->playBackground(b);
			unityVoice(e, ha, 2);
			unityVoice(e, hb, 2);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::INVALID_PARAMETER);
			TASK_CHECK(e->getFilterLatencyCompensationFrames() == 10);
			delete e;
		}
		return 0;
	}

	int graph_channel_counts()
	{
		for (unsigned channels : {1u, 2u, 6u})
		{
			SoLoud::Soloud *e = makeEngine(channels);
			TASK_CHECK(e != nullptr);
			DelayFilter f(33);
			PulseSource a(400, 0.02f, channels), b(400, 0.02f, channels);
			a.setFilter(0, &f);
			SoLoud::handle ha = e->playBackground(a), hb = e->playBackground(b);
			unityVoice(e, ha, channels);
			unityVoice(e, hb, channels);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
			TASK_CHECK(e->getFilterLatencyCompensationFrames() == 33);
			delete e;
		}
		return 0;
	}

	int graph_reinit_reuse()
	{
		SoLoud::Soloud e;
		TASK_CHECK(e.init(0, SoLoud::Soloud::NULLDRIVER, 48000, 1024, 2) == SoLoud::SO_NO_ERROR);
		e.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		DelayFilter f100(100);
		PulseSource a(400, 0.05f, 2), b(400, 0.05f, 2);
		a.setFilter(0, &f100);
		SoLoud::handle ha = e.playBackground(a), hb = e.playBackground(b);
		unityVoice(&e, ha, 2);
		unityVoice(&e, hb, 2);
		TASK_CHECK(e.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(e.getFilterLatencyCompensationFrames() == 100);
		e.deinit();

		// Re-init on the same object: a new graph without prepare must behave
		// as stock SoLoud, with no stale plan on the reused slots.
		TASK_CHECK(e.init(0, SoLoud::Soloud::NULLDRIVER, 48000, 1024, 2) == SoLoud::SO_NO_ERROR);
		e.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		DelayFilter f200(200);
		PulseSource c(400, 0.05f, 2), g(400, 0.05f, 2);
		c.setFilter(0, &f200);
		SoLoud::handle hc = e.playBackground(c), hg = e.playBackground(g);
		unityVoice(&e, hc, 2);
		unityVoice(&e, hg, 2);
		TASK_CHECK(e.getFilterLatencyCompensationFrames() == 0);
		int slot = e.getVoiceFromHandle_internal(hc);
		TASK_CHECK(slot >= 0);
		TASK_CHECK(e.mVoice[slot]->mLatencyRing == 0 && e.mVoice[slot]->mLatencyCompensation == 0);
		e.deinit();

		// A fresh prepare on reused slots produces a clean, correct plan.
		TASK_CHECK(e.init(0, SoLoud::Soloud::NULLDRIVER, 48000, 1024, 2) == SoLoud::SO_NO_ERROR);
		e.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		DelayFilter f200b(200);
		PulseSource c2(400, 0.05f, 2), g2(400, 0.05f, 2);
		c2.setFilter(0, &f200b);
		SoLoud::handle hc2 = e.playBackground(c2), hg2 = e.playBackground(g2);
		unityVoice(&e, hc2, 2);
		unityVoice(&e, hg2, 2);
		TASK_CHECK(e.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(e.getFilterLatencyCompensationFrames() == 200);
		e.deinit();
		return 0;
	}

	// ---- pcm cases ---------------------------------------------------------

	int pcm_stereo_alignment()
	{
		const unsigned channels = 2, warm = 1024, frames = 2048, pulse = 1500, delay = 237;
		SoLoud::Soloud *e = makeEngine(channels);
		TASK_CHECK(e != nullptr);
		DelayFilter f(delay);
		// Distinct left/right content: channel c carries base*(c+1).
		PulseSource filtered(pulse, 0.10f, channels), clean(pulse, 0.07f, channels);
		filtered.setFilter(0, &f);
		SoLoud::handle hf = e->playBackground(filtered), hc = e->playBackground(clean);
		unityVoice(e, hf, channels);
		unityVoice(e, hc, channels);
		TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(e->getFilterLatencyCompensationFrames() == delay);

		std::vector<float> out = render(e, warm, frames, 257, channels);
		// Independent prediction: every aligned branch arrives at pulse+delay;
		// amplitude per channel is the sum of the two branches.
		unsigned expectedLocal = pulse + delay - warm;
		for (unsigned c = 0; c < channels; ++c)
		{
			std::vector<unsigned> hits = pulseFrames(out, channels, c);
			TASK_CHECK(hits.size() == 1);
			TASK_CHECK(hits[0] == expectedLocal);
			float expected = (0.10f + 0.07f) * (c + 1);
			TASK_CHECK(std::fabs(out[hits[0] * channels + c] - expected) <= 1e-5f);
			TASK_CHECK(std::fabs(out[(hits[0] - 1) * channels + c]) < 1e-9f);
		}

		// Without a plan the two branches must arrive at different, independently
		// predicted frames (pulse and pulse+delay).
		delete e;
		e = makeEngine(channels);
		TASK_CHECK(e != nullptr);
		DelayFilter f2(delay);
		PulseSource filtered2(pulse, 0.10f, channels), clean2(pulse, 0.07f, channels);
		filtered2.setFilter(0, &f2);
		SoLoud::handle hf2 = e->playBackground(filtered2), hc2 = e->playBackground(clean2);
		unityVoice(e, hf2, channels);
		unityVoice(e, hc2, channels);
		std::vector<float> raw = render(e, warm, frames, 257, channels);
		std::vector<unsigned> hits = pulseFrames(raw, channels, 0);
		TASK_CHECK(hits.size() == 2);
		TASK_CHECK(hits[0] == pulse - warm);
		TASK_CHECK(hits[1] == pulse + delay - warm);
		delete e;
		return 0;
	}

	int pcm_three_level()
	{
		const unsigned channels = 2, warm = 1024, frames = 3072;
		SoLoud::Soloud *e = makeEngine(channels);
		TASK_CHECK(e != nullptr);
		DelayFilter f100(100), f200(200), f50(50), f400(400), f60(60), f30(30), f20(20);
		SoLoud::Bus A, B, C;
		configureBus(A, channels);
		configureBus(B, channels);
		configureBus(C, channels);
		A.setFilter(0, &f100);
		B.setFilter(0, &f200);
		C.setFilter(0, &f50);
		SoLoud::handle hA = e->playBackground(A), hB = e->playBackground(B), hC = e->playBackground(C);
		B.annexSound(hC);

		const unsigned p1 = 1200, p2 = 1400, p3 = 1600, p4 = 1800, p5 = 2000, p6 = 2200;
		PulseSource s1(p1, 0.01f, 2), s2(p2, 0.02f, 2), s3(p3, 0.03f, 2),
			s4(p4, 0.04f, 2), s5(p5, 0.05f, 2), s6(p6, 0.06f, 2);
		s1.setFilter(0, &f30);
		s1.setFilter(1, &f20);
		s3.setFilter(0, &f400);
		s5.setFilter(0, &f60);
		SoLoud::handle k1 = A.play(s1), k2 = A.play(s2), k3 = C.play(s3), k4 = C.play(s4);
		SoLoud::handle k5 = e->playBackground(s5), k6 = e->playBackground(s6);
		for (SoLoud::handle h : {hA, hB, hC, k1, k2, k3, k4, k5, k6}) unityVoice(e, h, channels);

		// Reference model, built independently of the engine calls above.
		RefNode rA{true, 100}, rB{true, 200}, rC{true, 50};
		RefNode n1{false, 50}, n2{false, 0}, n3{false, 400}, n4{false, 0}, n5{false, 60}, n6{false, 0};
		rC.mChildren = {&n3, &n4};
		rB.mChildren = {&rC};
		rA.mChildren = {&n1, &n2};
		RefNode root{true, 0};
		root.mChildren = {&rA, &rB, &n5, &n6};
		unsigned total = refCompute(&root);
		TASK_CHECK(total == 650);
		TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(e->getFilterLatencyCompensationFrames() == total);

		std::vector<float> out = render(e, warm, frames, 7, channels);
		struct Item { unsigned pulse; float base; };
		Item items[] = {{p1, 0.01f}, {p2, 0.02f}, {p3, 0.03f}, {p4, 0.04f}, {p5, 0.05f}, {p6, 0.06f}};
		for (const Item &it : items)
		{
			unsigned expectedLocal = it.pulse + total - warm;
			for (unsigned c = 0; c < channels; ++c)
			{
				std::vector<unsigned> hits = pulseFrames(out, channels, c);
				bool found = false;
				for (unsigned h : hits)
					if (h == expectedLocal) found = true;
				TASK_CHECK(found);
				float expected = it.base * (c + 1);
				TASK_CHECK(std::fabs(out[expectedLocal * channels + c] - expected) <= 1e-5f);
			}
		}
		delete e;
		return 0;
	}

	int pcm_six_channel()
	{
		const unsigned channels = 6, warm = 1024, frames = 1600, pulse = 1300, delay = 311;
		SoLoud::Soloud *e = makeEngine(channels);
		TASK_CHECK(e != nullptr);
		DelayFilter f(delay);
		PulseSource a(pulse, 0.01f, channels), b(pulse, 0.02f, channels);
		a.setFilter(0, &f);
		SoLoud::handle ha = e->playBackground(a), hb = e->playBackground(b);
		unityVoice(e, ha, channels);
		unityVoice(e, hb, channels);
		TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);

		std::vector<float> out = render(e, warm, frames, 257, channels);
		unsigned expectedLocal = pulse + delay - warm;
		for (unsigned c = 0; c < channels; ++c)
		{
			std::vector<unsigned> hits = pulseFrames(out, channels, c);
			TASK_CHECK(hits.size() == 1 && hits[0] == expectedLocal);
			// Independent per-channel amplitude: (0.01 + 0.02) * (c + 1).
			float expected = 0.03f * (c + 1);
			TASK_CHECK(std::fabs(out[hits[0] * channels + c] - expected) <= 1e-5f);
		}
		delete e;
		return 0;
	}

	int pcm_mono()
	{
		const unsigned channels = 1, warm = 512, frames = 1200, pulse = 900, delay = 77;
		SoLoud::Soloud *e = makeEngine(channels);
		TASK_CHECK(e != nullptr);
		DelayFilter f(delay);
		PulseSource a(pulse, 0.10f, channels), b(pulse, 0.20f, channels);
		a.setFilter(0, &f);
		SoLoud::handle ha = e->playBackground(a), hb = e->playBackground(b);
		unityVoice(e, ha, channels);
		unityVoice(e, hb, channels);
		TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		std::vector<float> out = render(e, warm, frames, 1, channels);
		unsigned expectedLocal = pulse + delay - warm;
		std::vector<unsigned> hits = pulseFrames(out, channels, 0);
		TASK_CHECK(hits.size() == 1 && hits[0] == expectedLocal);
		TASK_CHECK(std::fabs(out[hits[0]] - 0.30f) <= 1e-5f);
		delete e;
		return 0;
	}

	int pcm_large_delay_over_512()
	{
		const unsigned channels = 2, warm = 1024, frames = 3000, pulse = 2000;
		const unsigned delayA = 701, delayB = 600; // both beyond SAMPLE_GRANULARITY (512)
		SoLoud::Soloud *e = makeEngine(channels);
		TASK_CHECK(e != nullptr);
		DelayFilter fa(delayA), fb(delayB);
		PulseSource a(pulse, 0.03f, channels), b(pulse, 0.05f, channels);
		a.setFilter(0, &fa);
		b.setFilter(0, &fb);
		SoLoud::handle ha = e->playBackground(a), hb = e->playBackground(b);
		unityVoice(e, ha, channels);
		unityVoice(e, hb, channels);
		TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		// Junction alignment point is the longer path.
		TASK_CHECK(e->getFilterLatencyCompensationFrames() == delayA);
		std::vector<float> out = render(e, warm, frames, 257, channels);
		unsigned expectedLocal = pulse + delayA - warm;
		for (unsigned c = 0; c < channels; ++c)
		{
			std::vector<unsigned> hits = pulseFrames(out, channels, c);
			TASK_CHECK(hits.size() == 1 && hits[0] == expectedLocal);
			float expected = 0.08f * (c + 1);
			TASK_CHECK(std::fabs(out[hits[0] * channels + c] - expected) <= 1e-5f);
		}
		delete e;
		return 0;
	}

	int pcm_finite_autostop()
	{
		// The finite-material branch is the short one (no filter); after its
		// material it keeps returning zeros while its compensation ring carries
		// its already-filtered PCM until the long branch catches up. Auto-stop
		// is switched off for both persistent voices.
		const unsigned channels = 2, warm = 256, frames = 1400, pulse = 400, material = 700, delay = 300;
		SoLoud::Soloud *e = makeEngine(channels);
		TASK_CHECK(e != nullptr);
		DelayFilter f(delay);
		FiniteSource finite(pulse, material, 0.05f, channels);
		PulseSource eternal(pulse, 0.05f, channels);
		eternal.setFilter(0, &f);
		SoLoud::handle hf = e->playBackground(finite), he = e->playBackground(eternal);
		e->setAutoStop(hf, false);
		e->setAutoStop(he, false);
		for (unsigned c = 0; c < channels; ++c)
		{
			e->setChannelVolume(hf, c, 1.0f);
			e->setChannelVolume(he, c, 1.0f);
		}
		TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(e->getFilterLatencyCompensationFrames() == delay);
		std::vector<float> out = render(e, warm, frames, 257, channels);
		unsigned expectedLocal = pulse + delay - warm;
		for (unsigned c = 0; c < channels; ++c)
		{
			std::vector<unsigned> hits = pulseFrames(out, channels, c);
			TASK_CHECK(hits.size() == 1 && hits[0] == expectedLocal);
			// Both branches contribute while the finite voice is still alive.
			float expected = 0.10f * (c + 1);
			TASK_CHECK(std::fabs(out[hits[0] * channels + c] - expected) <= 1e-5f);
		}
		delete e;
		return 0;
	}

	// ---- blocks cases ------------------------------------------------------

	int blocks_root_sources()
	{
		const unsigned channels = 2, warm = 1024, frames = 2048, pulse = 1500, delay = 237;
		auto build = [&](SoLoud::Soloud *e) -> int {
			DelayFilter *f = new DelayFilter(delay);
			PulseSource *a = new PulseSource(pulse, 0.10f, channels);
			PulseSource *b = new PulseSource(pulse, 0.07f, channels);
			a->setFilter(0, f);
			SoLoud::handle ha = e->playBackground(*a);
			SoLoud::handle hb = e->playBackground(*b);
			unityVoice(e, ha, channels);
			unityVoice(e, hb, channels);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
			return 0;
		};
		std::vector<float> ref;
		for (unsigned chunk : {1u, 7u, 257u, frames})
		{
			SoLoud::Soloud *e = makeEngine(channels);
			TASK_CHECK(e != nullptr);
			TASK_CHECK(build(e) == 0);
			std::vector<float> out = render(e, warm, frames, chunk, channels);
			if (ref.empty()) ref = out;
			double worst = 0;
			for (size_t i = 0; i < out.size(); ++i)
				worst = std::fmax(worst, std::fabs((double)out[i] - ref[i]));
			TASK_CHECK(worst <= 1e-5f);
			delete e;
		}
		return 0;
	}

	int blocks_three_level_buses()
	{
		const unsigned channels = 2, warm = 1024, frames = 3072;
		auto build = [&](SoLoud::Soloud *e) -> int {
			DelayFilter *f100 = new DelayFilter(100), *f200 = new DelayFilter(200), *f50 = new DelayFilter(50),
				*f400 = new DelayFilter(400), *f60 = new DelayFilter(60);
			SoLoud::Bus *A = new SoLoud::Bus(), *B = new SoLoud::Bus(), *C = new SoLoud::Bus();
			configureBus(*A, channels);
			configureBus(*B, channels);
			configureBus(*C, channels);
			A->setFilter(0, f100);
			B->setFilter(0, f200);
			C->setFilter(0, f50);
			SoLoud::handle hA = e->playBackground(*A), hB = e->playBackground(*B), hC = e->playBackground(*C);
			B->annexSound(hC);
			PulseSource *s3 = new PulseSource(1600, 0.03f, channels), *s4 = new PulseSource(1800, 0.04f, channels);
			s3->setFilter(0, f400);
			SoLoud::handle k3 = C->play(*s3), k4 = C->play(*s4);
			PulseSource *s5 = new PulseSource(2000, 0.05f, channels);
			s5->setFilter(0, f60);
			SoLoud::handle k5 = e->playBackground(*s5);
			for (SoLoud::handle h : {hA, hB, hC, k3, k4, k5}) unityVoice(e, h, channels);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
			TASK_CHECK(e->getFilterLatencyCompensationFrames() == 650);
			return 0;
		};
		std::vector<float> ref;
		for (unsigned chunk : {1u, 7u, 257u, frames})
		{
			SoLoud::Soloud *e = makeEngine(channels);
			TASK_CHECK(e != nullptr);
			TASK_CHECK(build(e) == 0);
			std::vector<float> out = render(e, warm, frames, chunk, channels);
			if (ref.empty()) ref = out;
			double worst = 0;
			for (size_t i = 0; i < out.size(); ++i)
				worst = std::fmax(worst, std::fabs((double)out[i] - ref[i]));
			TASK_CHECK(worst <= 1e-5f);
			delete e;
		}
		return 0;
	}

	int blocks_six_channel()
	{
		const unsigned channels = 6, warm = 1024, frames = 1600, pulse = 1300, delay = 311;
		auto build = [&](SoLoud::Soloud *e) -> int {
			DelayFilter *f = new DelayFilter(delay);
			PulseSource *a = new PulseSource(pulse, 0.01f, channels);
			PulseSource *b = new PulseSource(pulse, 0.02f, channels);
			a->setFilter(0, f);
			SoLoud::handle ha = e->playBackground(*a), hb = e->playBackground(*b);
			unityVoice(e, ha, channels);
			unityVoice(e, hb, channels);
			TASK_CHECK(e->prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
			return 0;
		};
		std::vector<float> ref;
		for (unsigned chunk : {1u, 7u, 257u, frames})
		{
			SoLoud::Soloud *e = makeEngine(channels);
			TASK_CHECK(e != nullptr);
			TASK_CHECK(build(e) == 0);
			std::vector<float> out = render(e, warm, frames, chunk, channels);
			if (ref.empty()) ref = out;
			double worst = 0;
			for (size_t i = 0; i < out.size(); ++i)
				worst = std::fmax(worst, std::fabs((double)out[i] - ref[i]));
			TASK_CHECK(worst <= 1e-5f);
			delete e;
		}
		return 0;
	}
}

const struct task_case task_cases[] = {
	{"graph", "serial_and_bypass", graph_serial_and_bypass},
	{"graph", "three_level_buses", graph_three_level_buses},
	{"graph", "invalid_configs", graph_invalid_configs},
	{"graph", "channel_counts", graph_channel_counts},
	{"graph", "reinit_reuse", graph_reinit_reuse},
	{"pcm", "stereo_alignment", pcm_stereo_alignment},
	{"pcm", "three_level", pcm_three_level},
	{"pcm", "six_channel", pcm_six_channel},
	{"pcm", "mono", pcm_mono},
	{"pcm", "large_delay_over_512", pcm_large_delay_over_512},
	{"pcm", "finite_autostop", pcm_finite_autostop},
	{"blocks", "root_sources", blocks_root_sources},
	{"blocks", "three_level_buses", blocks_three_level_buses},
	{"blocks", "six_channel", blocks_six_channel},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
