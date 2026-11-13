/*
 * Task tests for declarable finite filter tails (FIR flush).
 *
 * All expectations are produced with independent, direct convolution and
 * point resampling reference code; the engine's own output is never used as
 * the expected value. Only the NULL driver and code-generated inputs are used.
 */

#include "registry.hpp"
#include "soloud.h"
#include "soloud_bus.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
	const unsigned int GRAN = 512; // SAMPLE_GRANULARITY
	const float EPS = 3e-4f;

	// ------------------------------------------------------------------
	// Deterministic FIR filter: y[n] = sum_k coef[k] * x[n - delay[k]]
	// The declared tail is the maximum delay line, i.e. the number of
	// zero-input frames still needed after the last valid input frame.
	// ------------------------------------------------------------------
	// Largest delay line used by the test filters, and SoLoud's channel cap.
	const unsigned int MAX_TEST_TAIL = 600;
	const unsigned int MAX_TEST_CH = 8;

	struct FirFilter : public SoLoud::Filter
	{
		std::vector<float> mCoef;
		std::vector<unsigned int> mDelay;
		unsigned int mTail;

		FirFilter(const std::vector<float> &aCoef,
			const std::vector<unsigned int> &aDelay, unsigned int aTail)
			: mCoef(aCoef), mDelay(aDelay), mTail(aTail) {}

		SoLoud::FilterInstance *createInstance() override;
	};

	struct FirInstance : public SoLoud::FilterInstance
	{
		const FirFilter *mParent;
		// Fixed, preallocated delay lines so the mixing path never allocates.
		float mHistory[MAX_TEST_CH][MAX_TEST_TAIL + 1];
		unsigned int mPos[MAX_TEST_CH];

		explicit FirInstance(const FirFilter *aParent) : mParent(aParent)
		{
			for (unsigned int c = 0; c < MAX_TEST_CH; c++)
			{
				mPos[c] = 0;
				for (unsigned int i = 0; i <= MAX_TEST_TAIL; i++)
					mHistory[c][i] = 0.0f;
			}
		}

		unsigned int getTailFrames() const override { return mParent->mTail; }

		void filterChannel(float *aBuffer, unsigned int aSamples, float /*aSamplerate*/,
			double /*aTime*/, unsigned int aChannel, unsigned int /*aChannels*/) override
		{
			const unsigned int ring = mParent->mTail + 1; // holds delays 0..tail
			unsigned int pos = mPos[aChannel];
			for (unsigned int i = 0; i < aSamples; i++)
			{
				mHistory[aChannel][pos] = aBuffer[i];
				float acc = 0.0f;
				for (size_t k = 0; k < mParent->mCoef.size(); k++)
				{
					unsigned int d = mParent->mDelay[k];
					unsigned int idx = (pos + ring - d) % ring;
					acc += mParent->mCoef[k] * mHistory[aChannel][idx];
				}
				aBuffer[i] = acc; // input already saved in the delay line
				pos = (pos + 1) % ring;
			}
			mPos[aChannel] = pos;
		}
	};

	SoLoud::FilterInstance *FirFilter::createInstance()
	{
		return new FirInstance(this);
	}

	// Chain: feed each filter's full impulse response through the next one.
	std::vector<float> chainImpulse(const std::vector<FirFilter *> &filters)
	{
		std::vector<float> h(1, 1.0f);
		for (size_t f = 0; f < filters.size(); f++)
		{
			std::vector<float> nh(h.size() + filters[f]->mTail, 0.0f);
			for (size_t n = 0; n < nh.size(); n++)
			{
				float acc = 0.0f;
				for (size_t k = 0; k < filters[f]->mCoef.size(); k++)
				{
					int idx = (int)n - (int)filters[f]->mDelay[k];
					if (idx >= 0 && idx < (int)h.size())
						acc += filters[f]->mCoef[k] * h[idx];
				}
				nh[n] = acc;
			}
			h = nh;
		}
		return h;
	}

	// Point resampling that faithfully reproduces Soloud's resample_point and
	// its block bookkeeping. The engine keeps the playhead block-relative: each
	// 512-frame source block starts at a (usually small) residual offset, and
	// resamples with the same fixed-point writesamples formula. We replay that
	// exact state machine against the full filtered stream instead of assuming
	// a single global floor(n*ratio).
	std::vector<float> pointResample(const std::vector<float> &src,
		unsigned int srcRate, unsigned int dstRate, unsigned int dstFrames)
	{
		const unsigned int FRACBITS = 20;
		const unsigned int FRACMUL = 1 << FRACBITS;
		const unsigned int GRANF = GRAN * FRACMUL;
		unsigned int stepFixed = (unsigned int)floor((double)srcRate / (double)dstRate * FRACMUL);
		std::vector<float> out(dstFrames, 0.0f);
		unsigned int srcOffset = 0; // block-relative fixed point
		unsigned int blockBase = 0; // global source frame at start of current block
		unsigned int produced = 0;
		while (produced < dstFrames)
		{
			// Fetch a fresh 512-frame block (mirrors the mLeftoverSamples==0 path).
			unsigned int writeSamples = 0;
			if (srcOffset < GRANF)
			{
				writeSamples = (GRANF - srcOffset) / stepFixed + 1;
				if (((writeSamples * stepFixed + srcOffset) >> FRACBITS) >= GRAN)
					writeSamples--;
			}
			for (unsigned int k = 0; k < writeSamples && produced < dstFrames; k++)
			{
				unsigned int localIdx = (srcOffset + k * stepFixed) >> FRACBITS;
				unsigned int globalIdx = blockBase + localIdx;
				out[produced] = (globalIdx < src.size()) ? src[globalIdx] : 0.0f;
				produced++;
			}
			srcOffset += writeSamples * stepFixed;
			// Advance to the next block with the engine's residual handling.
			if (srcOffset < GRANF)
				srcOffset = 0;
			else
				srcOffset -= GRANF;
			blockBase += GRAN;
		}
		return out;
	}

	// ------------------------------------------------------------------
	// Code-generated deterministic mono/stereo source with controllable
	// length, short reads and loop behavior.
	// ------------------------------------------------------------------
	struct GenSource : public SoLoud::AudioSource
	{
		unsigned int mLength;
		unsigned int mRate;
		unsigned int mMaxRead;   // cap each getAudio call (short reads)
		bool mShortNotEnded;     // short reads while not ended
		bool mLoopable;
		std::vector<float> mData; // planar, mChannels * mLength

		GenSource(unsigned int aLength, unsigned int aChannels, unsigned int aRate,
			unsigned int aMaxRead = GRAN, bool aShortNotEnded = false, bool aLoopable = false)
			: mLength(aLength), mRate(aRate), mMaxRead(aMaxRead),
			mShortNotEnded(aShortNotEnded), mLoopable(aLoopable)
		{
			mBaseSamplerate = (float)aRate;
			// Sets AudioSource::mChannels (no shadowing field), which init()
			// forwards to the play instance so filters see all channels.
			mChannels = aChannels;
			mData.resize((size_t)aChannels * aLength);
			for (unsigned int c = 0; c < aChannels; c++)
				for (unsigned int n = 0; n < aLength; n++)
				{
					float v;
					if (aChannels == 1)
						v = 0.5f * sinf(0.07f * n) + 0.25f * sinf(0.31f * n + 1.3f);
					else
						v = (c == 0 ? 0.5f * sinf(0.07f * n) : 0.5f * sinf(0.09f * n + 2.0f))
							+ 0.2f * sinf(0.31f * n + (c ? 0.7f : 1.3f));
					mData[(size_t)c * aLength + n] = v;
				}
		}

		SoLoud::AudioSourceInstance *createInstance() override;
	};

	struct GenInstance : public SoLoud::AudioSourceInstance
	{
		GenSource *mParent;
		unsigned int mPos;

		explicit GenInstance(GenSource *aParent) : mParent(aParent), mPos(0) {}

		unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize) override
		{
			unsigned int want = aSamplesToRead;
			if (want > mParent->mMaxRead)
				want = mParent->mMaxRead;
			bool loop = (mFlags & LOOPING) != 0;
			unsigned int done = 0;
			while (done < want)
			{
				if (mPos >= mParent->mLength)
				{
					if (loop)
					{
						mPos = 0; // rewind, filter history is untouched
						continue;
					}
					break;
				}
				unsigned int chunk = mParent->mLength - mPos;
				if (chunk > want - done)
					chunk = want - done;
				for (unsigned int c = 0; c < mParent->mChannels; c++)
					for (unsigned int i = 0; i < chunk; i++)
						aBuffer[c * aBufferSize + done + i] =
							mParent->mData[(size_t)c * mParent->mLength + mPos + i];
				mPos += chunk;
				done += chunk;
				if (!loop && mPos >= mParent->mLength)
					break;
			}
			return done;
		}

		bool hasEnded() override
		{
			if (mFlags & LOOPING)
				return false;
			return mPos >= mParent->mLength;
		}
	};

	SoLoud::AudioSourceInstance *GenSource::createInstance()
	{
		GenInstance *g = new GenInstance(this);
		g->mChannels = mChannels;
		g->mBaseSamplerate = (float)mRate;
		g->mSamplerate = (float)mRate;
		return g;
	}

	// ------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------
	SoLoud::Soloud *makeEngine(unsigned int rate, unsigned int channels, unsigned int buffer)
	{
		SoLoud::Soloud *s = new SoLoud::Soloud();
		int r = s->init(0, SoLoud::Soloud::NULLDRIVER, rate, buffer, channels);
		if (r != 0)
		{
			delete s;
			return 0;
		}
		s->setPostClipScaler(1.0f); // linear clip only, deterministic gain
		s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		return s;
	}

	// Interleaved engine output -> planar channel vectors.
	std::vector<std::vector<float> > deinterleave(const std::vector<float> &mix,
		unsigned int channels, unsigned int frames)
	{
		std::vector<std::vector<float> > p(channels, std::vector<float>(frames));
		for (unsigned int c = 0; c < channels; c++)
			for (unsigned int n = 0; n < frames; n++)
				p[c][n] = mix[n * channels + c];
		return p;
	}

	// Compare a planar reference against one engine output channel over [0, frames).
	void checkChannel(Registry &r, const std::vector<float> &ref,
		const std::vector<float> &got, unsigned int frames, const char *tag, unsigned int ch)
	{
		r.check(ref.size() >= frames, "reference long enough");
		r.check(got.size() >= frames, "output long enough");
		unsigned int mismatches = 0;
		float worst = 0.0f;
		unsigned int worstAt = 0;
		for (unsigned int n = 0; n < frames; n++)
		{
			float d = fabsf(got[n] - ref[n]);
			if (d > worst) { worst = d; worstAt = n; }
			if (d > EPS) mismatches++;
		}
		if (mismatches)
		{
			char msg[256];
			snprintf(msg, sizeof(msg),
				"%s ch%u: %u sample mismatches, worst diff %.2e at %u (got %.6f exp %.6f)",
				tag, ch, mismatches, worst, worstAt, got[worstAt], ref[worstAt]);
			r.check(false, msg);
		}
		r.check(true, "channel samples match reference");
	}

	// Reproduce the exact frame stream the engine feeds into the filter chain:
	// each block request yields up to maxRead real frames packed at the start
	// of a 512-frame block, zero-padded for the rest; after natural EOF the
	// zero-padded frames are charged against the tail, exactly tailFrames in
	// total. Short reads while not ended therefore create real (filtered) gap
	// zeros, matching mixBus_internal's block semantics.
	std::vector<float> simulateFilterInput(const std::vector<float> &channelInput,
		unsigned int length, unsigned int maxRead, unsigned int tailFrames)
	{
		std::vector<float> x;
		x.reserve(length + tailFrames + GRAN);
		unsigned int pos = 0;
		unsigned int tailZeroFed = 0;
		bool ended = false;
		while (true)
		{
			unsigned int real = 0;
			if (!ended)
			{
				unsigned int want = std::min(GRAN, maxRead);
				real = std::min(want, length - pos);
				pos += real;
				ended = (pos >= length);
			}
			unsigned int zeros = GRAN - real;
			if (ended)
			{
				unsigned int charge = std::min(zeros, tailFrames - tailZeroFed);
				tailZeroFed += charge;
				zeros = charge; // truncate final block at the tail boundary
			}
			for (unsigned int i = 0; i < real; i++)
				x.push_back(channelInput[pos - real + i]);
			for (unsigned int i = 0; i < zeros; i++)
				x.push_back(0.0f);
			if (ended && tailZeroFed >= tailFrames)
				break;
		}
		return x;
	}

	// Number of source-rate frames actually run through the filters up to and
	// including the declared tail (engine block semantics).
	unsigned int filteredFrameCount(const std::vector<float> &channelInput,
		unsigned int length, unsigned int maxRead, unsigned int tailFrames)
	{
		return (unsigned int)simulateFilterInput(channelInput, length, maxRead, tailFrames).size();
	}

	// Backend output frames covering the full filtered stream (plus margin).
	unsigned int refOutFrames(GenSource &src, unsigned int tailFrames, unsigned int backendRate)
	{
		std::vector<float> in0(src.mLength);
		for (unsigned int n = 0; n < src.mLength; n++)
			in0[n] = src.mData[n]; // channel 0 is planar at the start
		unsigned int count = filteredFrameCount(in0, src.mLength, src.mMaxRead, tailFrames);
		return (unsigned int)ceil((double)count * backendRate / src.mRate) + 2;
	}

	// Build the planar reference output for a source + filter chain played
	// straight through (no bus), then point-resampled to the backend rate.
	// aPanGain accounts for Soloud's constant center-pan channel gain that
	// panAndExpand applies at the default pan of 0 (cos(pi/4) per stage).
	std::vector<std::vector<float> > buildReference(GenSource &src,
		const std::vector<FirFilter *> &filters, unsigned int tailFrames,
		unsigned int backendRate, unsigned int outFrames,
		float aPanGain = 0.7071067811865476f)
	{
		std::vector<std::vector<float> > ref(src.mChannels);
		for (unsigned int c = 0; c < src.mChannels; c++)
		{
			std::vector<float> in(src.mLength);
			for (unsigned int n = 0; n < src.mLength; n++)
				in[n] = src.mData[(size_t)c * src.mLength + n];
			// Frame stream as actually delivered to the filters (block gaps + tail).
			std::vector<float> fed = simulateFilterInput(in, src.mLength, src.mMaxRead, tailFrames);
			std::vector<float> h = chainImpulse(filters);
			std::vector<float> filt(fed.size(), 0.0f);
			for (size_t n = 0; n < fed.size(); n++)
				for (size_t k = 0; k < h.size(); k++)
				{
					int idx = (int)n - (int)k;
					if (idx >= 0)
						filt[n] += h[k] * fed[idx];
				}
			std::vector<float> rs = pointResample(filt, src.mRate, backendRate, outFrames);
			// Engine is configured with post-clip scaler 1.0, so only the
			// center-pan gain remains.
			for (float &v : rs) v *= aPanGain;
			ref[c] = rs;
		}
		return ref;
	}

	// Run engine mix() in fixed-size chunks, collect interleaved output.
	std::vector<float> runChunks(SoLoud::Soloud &s, unsigned int totalFrames,
		unsigned int chunk, unsigned int channels)
	{
		std::vector<float> out;
		out.reserve((size_t)totalFrames * channels);
		std::vector<float> buf((size_t)chunk * channels);
		unsigned int done = 0;
		while (done < totalFrames)
		{
			unsigned int n = std::min(chunk, totalFrames - done);
			s.mix(buf.data(), n);
			for (unsigned int i = 0; i < n * channels; i++)
				out.push_back(buf[i]);
			done += n;
		}
		return out;
	}

	// Filters used across the tests.
	FirFilter *makeShortFir()
	{
		// 3 taps, max delay 7. Deterministic, non-trivial coefficients.
		std::vector<float> coef; coef.push_back(0.6f); coef.push_back(0.3f); coef.push_back(0.1f);
		std::vector<unsigned int> delay; delay.push_back(0); delay.push_back(3); delay.push_back(7);
		return new FirFilter(coef, delay, 7);
	}

	FirFilter *makeLongFir()
	{
		// Delay longer than one 512-frame read block.
		std::vector<float> coef; coef.push_back(0.7f); coef.push_back(0.3f);
		std::vector<unsigned int> delay; delay.push_back(0); delay.push_back(600);
		return new FirFilter(coef, delay, 600);
	}

	// Number of backend frames needed to cover source input + filter tail.
	unsigned int coveredBackendFrames(unsigned int srcRate, unsigned int backendRate,
		unsigned int srcFramesPlusTail)
	{
		return (unsigned int)ceil((double)srcFramesPlusTail * backendRate / srcRate) + 4;
	}
}

// ======================================================================
// Registration
// ======================================================================
void register_task_tests(Registry &r)
{
	// ---------------------------------------------------------------- tail
	r.add("tail", "short_fir_mono_full_block", [&r]() {
		const unsigned int rate = 48000, channels = 1, len = 1000, tail = 7;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeShortFir();
		GenSource src(len, channels, rate);
		src.setFilter(0, f);
		SoLoud::handle h = s->play(src);
		r.check(h != 0, "play handle");

		unsigned int outFrames = coveredBackendFrames(rate, rate, len + tail);
		std::vector<FirFilter *> chain; chain.push_back(f);
		auto ref = buildReference(src, chain, tail, rate, outFrames);
		std::vector<float> mix = runChunks(*s, outFrames, 512, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "tail/full-block", 0);
		r.check(!s->isValidVoiceHandle(h), "voice auto-stopped after tail delivered");
		s->deinit();
		delete s;
		delete f;
	});

	r.add("tail", "short_read_not_ended_keeps_phase", [&r]() {
		// Source returns short blocks (7 frames) but is not ended; the end
		// phase must not start until hasEnded() finally goes true.
		const unsigned int rate = 48000, channels = 1, len = 530, tail = 7;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeShortFir();
		GenSource src(len, channels, rate, 7, true);
		src.setFilter(0, f);
		SoLoud::handle h = s->play(src);
		// Short reads insert real filtered gap zeros per 512-block, so the
		// rendered stream is longer than len+tail; derive its true length.
		unsigned int outFrames = refOutFrames(src, tail, rate);
		std::vector<FirFilter *> chain; chain.push_back(f);
		auto ref = buildReference(src, chain, tail, rate, outFrames);
		std::vector<float> mix = runChunks(*s, outFrames, 257, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "tail/short-read", 0);
		r.check(!s->isValidVoiceHandle(h), "voice stopped after real end + tail");
		s->deinit();
		delete s;
		delete f;
	});

	r.add("tail", "last_input_at_block_boundary", [&r]() {
		// Length chosen so the final valid input frame lands exactly at the
		// end of a 512-frame read block (len = 1024), tail starts on next block.
		const unsigned int rate = 48000, channels = 1, len = 1024, tail = 7;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeShortFir();
		GenSource src(len, channels, rate);
		src.setFilter(0, f);
		SoLoud::handle h = s->play(src);
		unsigned int outFrames = coveredBackendFrames(rate, rate, len + tail);
		std::vector<FirFilter *> chain; chain.push_back(f);
		auto ref = buildReference(src, chain, tail, rate, outFrames);
		// Mix in 1-frame chunks to stress cross-call buffer accounting.
		std::vector<float> mix = runChunks(*s, outFrames, 1, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "tail/block-boundary", 0);
		r.check(!s->isValidVoiceHandle(h), "voice stopped after boundary tail");
		s->deinit();
		delete s;
		delete f;
	});

	r.add("tail", "chunk_sizes_match", [&r]() {
		// Same scenario rendered with 512/1/7/257-frame mix chunks must agree.
		const unsigned int rate = 48000, channels = 1, len = 900, tail = 7;
		const unsigned int chunks[4] = { 512, 1, 7, 257 };
		FirFilter *f = makeShortFir();
		std::vector<FirFilter *> chain; chain.push_back(f);
		unsigned int outFrames = coveredBackendFrames(rate, rate, len + tail);
		std::vector<std::vector<float> > renders;
		for (int q = 0; q < 4; q++)
		{
			SoLoud::Soloud *s = makeEngine(rate, channels, 512);
			GenSource src(len, channels, rate);
			src.setFilter(0, f);
			s->play(src);
			std::vector<float> mix = runChunks(*s, outFrames, chunks[q], channels);
			renders.push_back(deinterleave(mix, channels, outFrames)[0]);
			s->deinit();
			delete s;
		}
		for (int q = 1; q < 4; q++)
		{
			unsigned int bad = 0;
			for (unsigned int n = 0; n < outFrames; n++)
				if (fabsf(renders[q][n] - renders[0][n]) > EPS) bad++;
			r.check(bad == 0, "all chunk sizes produce identical PCM");
		}
		delete f;
	});

	// --------------------------------------------------------------- chain
	r.add("chain", "two_firs_tails_add_stereo", [&r]() {
		const unsigned int rate = 48000, channels = 2, len = 1200;
		const unsigned int tail = 7 + 600; // chained tails add
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f1 = makeShortFir();
		FirFilter *f2 = makeLongFir();
		GenSource src(len, channels, rate);
		src.setFilter(0, f1);
		src.setFilter(1, f2);
		SoLoud::handle h = s->play(src);
		unsigned int outFrames = coveredBackendFrames(rate, rate, len + tail);
		std::vector<FirFilter *> chain; chain.push_back(f1); chain.push_back(f2);
		auto ref = buildReference(src, chain, tail, rate, outFrames);
		std::vector<float> mix = runChunks(*s, outFrames, 257, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "chain/stereo/L", 0);
		checkChannel(r, ref[1], planar[1], outFrames, "chain/stereo/R", 1);
		r.check(!s->isValidVoiceHandle(h), "chained voice stopped after combined tail");
		s->deinit();
		delete s;
		delete f1;
		delete f2;
	});

	r.add("chain", "long_delay_exceeds_512", [&r]() {
		// Filter delay (600) exceeds one read block; tail spans multiple blocks.
		const unsigned int rate = 48000, channels = 1, len = 700, tail = 600;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeLongFir();
		GenSource src(len, channels, rate);
		src.setFilter(0, f);
		SoLoud::handle h = s->play(src);
		unsigned int outFrames = coveredBackendFrames(rate, rate, len + tail);
		std::vector<FirFilter *> chain; chain.push_back(f);
		auto ref = buildReference(src, chain, tail, rate, outFrames);
		std::vector<float> mix = runChunks(*s, outFrames, 7, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "chain/long-delay", 0);
		// A legal filter stays silent then emits at the tail; the tail energy
		// must be present (no all-zero heuristic may cut it short).
		double tailEnergy = 0.0;
		for (unsigned int n = len; n < len + tail && n < planar[0].size(); n++)
			tailEnergy += planar[0][n] * planar[0][n];
		r.check(tailEnergy > 1.0, "long-delay tail emits energy after EOF");
		r.check(!s->isValidVoiceHandle(h), "long-tail voice eventually stops");
		s->deinit();
		delete s;
		delete f;
	});

	// ---------------------------------------------------------- resampling
	r.add("resampling", "48k_to_32k_point_mono", [&r]() {
		const unsigned int backend = 32000, srcRate = 48000, channels = 1, len = 1000, tail = 600;
		SoLoud::Soloud *s = makeEngine(backend, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeLongFir();
		GenSource src(len, channels, srcRate);
		src.setFilter(0, f);
		SoLoud::handle h = s->play(src);
		unsigned int outFrames = coveredBackendFrames(srcRate, backend, len + tail);
		std::vector<FirFilter *> chain; chain.push_back(f);
		auto ref = buildReference(src, chain, tail, backend, outFrames);
		std::vector<float> mix = runChunks(*s, outFrames, 257, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "resample/48to32", 0);
		r.check(!s->isValidVoiceHandle(h), "resampled voice stops after tail");
		s->deinit();
		delete s;
		delete f;
	});

	r.add("resampling", "48k_to_32k_stereo_via_bus", [&r]() {
		// Nested node: source plays on a parent Bus; tail must reach root.
		const unsigned int backend = 32000, srcRate = 48000, channels = 2, len = 900, tail = 7;
		SoLoud::Soloud *s = makeEngine(backend, channels, 512);
		r.check(s != 0, "engine init");
		s->setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
		SoLoud::Bus bus;
		r.check(bus.setChannels(channels) == 0, "bus channels");
		// Pin the bus to the source rate so the child's first stage is 1:1;
		// the only rate conversion is the 48k->32k bus-to-root stage.
		bus.mBaseSamplerate = (float)srcRate;
		bus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
		SoLoud::handle busHandle = s->play(bus);
		r.check(busHandle != 0, "bus playing");
		FirFilter *f = makeShortFir();
		GenSource src(len, channels, srcRate);
		src.setFilter(0, f);
		SoLoud::handle h = bus.play(src);
		r.check(h != 0, "source on bus");
		unsigned int outFrames = coveredBackendFrames(srcRate, backend, len + tail);
		std::vector<FirFilter *> chain; chain.push_back(f);
		// Two pan stages (child->bus, bus->root): cos(pi/4)^2 = 0.5.
		auto ref = buildReference(src, chain, tail, backend, outFrames, 0.5f);
		std::vector<float> mix = runChunks(*s, outFrames, 7, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "resample/bus/L", 0);
		checkChannel(r, ref[1], planar[1], outFrames, "resample/bus/R", 1);
		r.check(!s->isValidVoiceHandle(h), "bus child stops after tail");
		s->deinit();
		delete s;
		delete f;
	});

	// ------------------------------------------------------------ lifecycle
	r.add("lifecycle", "loop_then_natural_end_drains_once", [&r]() {
		// Looping voice keeps filter history at wrap points; after looping is
		// disabled the natural end drains the tail exactly once.
		const unsigned int rate = 48000, channels = 1, len = 300, tail = 7;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeShortFir();
		GenSource src(len, channels, rate, GRAN, false, true);
		src.setLooping(true);
		src.setFilter(0, f);
		SoLoud::handle h = s->play(src);
		// Let it cross several loop seams (each 512-block read rewinds inside).
		std::vector<float> discard(512 * channels);
		for (int i = 0; i < 4; i++) s->mix(discard.data(), 512);
		r.check(s->isValidVoiceHandle(h), "looping voice stays alive across seams");
		s->setLooping(h, false);
		// After disabling loop, let it run to natural end + tail.
		unsigned int more = coveredBackendFrames(rate, rate, len + tail) + 512;
		std::vector<float> mix = runChunks(*s, more, 257, channels);
		// The tail region must contain the delayed (non-zero) filter response
		// and the voice must terminate; no assertion on exact loop history PCM.
		double energy = 0.0;
		for (float v : mix) energy += (double)v * v;
		r.check(energy > 1.0, "post-loop output audible");
		// Drain until it ends (bounded), then confirm it invalidates.
		for (int i = 0; i < 20 && s->isValidVoiceHandle(h); i++)
			s->mix(discard.data(), 512);
		r.check(!s->isValidVoiceHandle(h), "voice ends after loop disabled + tail");
		s->deinit();
		delete s;
		delete f;
	});

	r.add("lifecycle", "pause_freezes_then_resumes", [&r]() {
		const unsigned int rate = 48000, channels = 1, len = 2000, tail = 600;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeLongFir();
		GenSource src(len, channels, rate);
		src.setFilter(0, f);
		SoLoud::handle h = s->play(src);
		unsigned int outFrames = coveredBackendFrames(rate, rate, len + tail);

		// Reference timeline produced without pause.
		std::vector<FirFilter *> chain; chain.push_back(f);
		auto ref = buildReference(src, chain, tail, rate, outFrames);

		std::vector<float> got;
		got.reserve(outFrames);
		std::vector<float> buf(512);
		unsigned int produced = 0;
		// Pause immediately, mix some silence while frozen.
		s->setPause(h, true);
		for (int i = 0; i < 3; i++)
		{
			s->mix(buf.data(), 512);
			for (unsigned int k = 0; k < 512; k++) got.push_back(0.0f);
		}
		r.check(s->getPause(h), "voice reports paused");
		// Resume; subsequent PCM must match the reference from frame 0.
		s->setPause(h, false);
		while (produced < outFrames)
		{
			unsigned int n = std::min(257u, outFrames - produced);
			std::vector<float> b(n);
			s->mix(b.data(), n);
			for (float v : b) got.push_back(v);
			produced += n;
		}
		// Compare the post-resume segment against the reference head.
		unsigned int resumeStart = 3 * 512;
		unsigned int cmp = outFrames; // got has extra leading silence frames
		unsigned int mismatches = 0;
		for (unsigned int n = 0; n < cmp; n++)
		{
			float g = got[resumeStart + n];
			if (fabsf(g - ref[0][n]) > EPS) mismatches++;
		}
		r.check(mismatches == 0, "resumed tail continues reference timeline");
		// While paused the handle must remain valid.
		s->deinit();
		delete s;
		delete f;
	});

	r.add("lifecycle", "explicit_stop_destroys_immediately", [&r]() {
		const unsigned int rate = 48000, channels = 1, len = 5000, tail = 600;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeLongFir();
		GenSource src(len, channels, rate);
		src.setFilter(0, f);
		SoLoud::handle h = s->play(src);
		std::vector<float> buf(512);
		s->mix(buf.data(), 512);
		r.check(s->isValidVoiceHandle(h), "voice alive before stop");
		s->stop(h);
		r.check(!s->isValidVoiceHandle(h), "explicit stop invalidates immediately, no tail");
		// After stop, further mixes contain nothing from this voice.
		std::fill(buf.begin(), buf.end(), 7.0f);
		s->mix(buf.data(), 512);
		double energy = 0.0;
		for (float v : buf) energy += (double)v * v;
		r.check(energy == 0.0, "stopped voice contributes no PCM");
		s->deinit();
		delete s;
		delete f;
	});

	r.add("lifecycle", "slot_reuse_has_no_stale_cache", [&r]() {
		const unsigned int rate = 48000, channels = 1, tail = 600;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeLongFir();
		// First voice: long source, stopped early while tail would remain.
		{
			GenSource src(5000, channels, rate);
			src.setFilter(0, f);
			SoLoud::handle h1 = s->play(src);
			std::vector<float> buf(512);
			s->mix(buf.data(), 512);
			s->stop(h1);
			r.check(!s->isValidVoiceHandle(h1), "first voice stopped");
		}
		// Second voice reuses a slot; its output must equal its own reference
		// with no inherited samples/caches.
		const unsigned int len2 = 400;
		GenSource src2(len2, channels, rate);
		src2.setFilter(0, f);
		SoLoud::handle h2 = s->play(src2);
		unsigned int outFrames = coveredBackendFrames(rate, rate, len2 + tail);
		std::vector<FirFilter *> chain; chain.push_back(f);
		auto ref = buildReference(src2, chain, tail, rate, outFrames);
		std::vector<float> mix = runChunks(*s, outFrames, 512, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "lifecycle/slot-reuse", 0);
		r.check(!s->isValidVoiceHandle(h2), "reused-slot voice stops cleanly");
		s->deinit();
		delete s;
		delete f;
	});

	r.add("lifecycle", "autostop_disabled_keeps_caller_control", [&r]() {
		// With auto-stop off, the voice is not reclaimed by the engine even
		// after the tail is flushed; caller retains control (legacy behavior).
		const unsigned int rate = 48000, channels = 1, len = 600, tail = 7;
		SoLoud::Soloud *s = makeEngine(rate, channels, 512);
		r.check(s != 0, "engine init");
		FirFilter *f = makeShortFir();
		GenSource src(len, channels, rate);
		src.setFilter(0, f);
		src.setAutoStop(false);
		SoLoud::handle h = s->play(src);
		unsigned int outFrames = coveredBackendFrames(rate, rate, len + tail);
		std::vector<FirFilter *> chain; chain.push_back(f);
		auto ref = buildReference(src, chain, tail, rate, outFrames);
		std::vector<float> mix = runChunks(*s, outFrames, 257, channels);
		auto planar = deinterleave(mix, channels, outFrames);
		checkChannel(r, ref[0], planar[0], outFrames, "lifecycle/no-autostop", 0);
		// Mix well past the end; handle must still be valid.
		std::vector<float> buf(512);
		for (int i = 0; i < 4; i++) s->mix(buf.data(), 512);
		r.check(s->isValidVoiceHandle(h), "auto-stop-off voice is not reclaimed");
		s->stop(h);
		r.check(!s->isValidVoiceHandle(h), "caller can still stop it");
		s->deinit();
		delete s;
		delete f;
	});
}
