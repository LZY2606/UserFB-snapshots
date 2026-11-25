// Task: optional filter latency compensation for SoLoud.
//
// Registers three groups via task_cases:
//   graph - planner arithmetic, API state/validation, independent latency tree
//   pcm   - real NULLDRIVER mixes; every pulse frame/amplitude computed independently
//   blocks- one big mix vs 1/7/257-frame fragmented mixes
//
// No main(): task_support/driver.c provides it.

#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_filter.h"
#include "soloud_latency.h"
#include "task_support/check.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
	constexpr unsigned int SR = 48000;
	constexpr unsigned int WARM = 128; // identical silence/zero warm-up in every render

	// ---------------------------------------------------------------------
	// Test filters: exact pure delay D with declared latency D.
	// Cursor advances once per frame; channels use independent ring blocks.
	// ---------------------------------------------------------------------
	class DelayInstance : public SoLoud::FilterInstance
	{
	public:
		unsigned int mDelay;
		unsigned int mPos;
		std::vector<float> mRing;

		explicit DelayInstance(unsigned int aDelay)
		    : mDelay(aDelay), mPos(0),
		      mRing((size_t)(aDelay ? aDelay : 1) * MAX_CHANNELS, 0.0f)
		{
		}

		unsigned int getLatencyFrames() const override { return mDelay; }

		void filter(float *aBuffer, unsigned int aSamples, unsigned int aStride,
		            unsigned int aChannels, float, SoLoud::time) override
		{
			if (mDelay == 0)
				return;
			for (unsigned int i = 0; i < aSamples; ++i)
			{
				for (unsigned int c = 0; c < aChannels; ++c)
				{
					float &slot = mRing[(size_t)c * mDelay + mPos];
					float x = aBuffer[(size_t)c * aStride + i];
					aBuffer[(size_t)c * aStride + i] = slot;
					slot = x;
				}
				++mPos;
				if (mPos == mDelay)
					mPos = 0;
			}
		}
	};

	class DelayFilter : public SoLoud::Filter
	{
	public:
		unsigned int mDelay;
		explicit DelayFilter(unsigned int aDelay) : mDelay(aDelay) {}
		SoLoud::FilterInstance *createInstance() override
		{
			return new DelayInstance(mDelay);
		}
	};

	// ---------------------------------------------------------------------
	// Pulse source: per-channel lists of absolute-frame pulses.
	// After the described material it simply keeps returning zeros and
	// hasEnded() is always false (auto-stop is disabled by the cases).
	// ---------------------------------------------------------------------
	struct PulseSpec
	{
		static constexpr int MAXP = 4;
		struct PE { unsigned int at; float amp; };
		PE mPulse[MAX_CHANNELS][MAXP];
		int mCount[MAX_CHANNELS];

		PulseSpec()
		{
			for (int c = 0; c < MAX_CHANNELS; ++c)
			{
				mCount[c] = 0;
				for (int k = 0; k < MAXP; ++k)
				{
					mPulse[c][k].at = 0;
					mPulse[c][k].amp = 0.0f;
				}
			}
		}

		void add(unsigned int aChannel, unsigned int aFrame, float aAmp)
		{
			if (aChannel < MAX_CHANNELS && mCount[aChannel] < MAXP)
			{
				mPulse[aChannel][mCount[aChannel]].at = aFrame;
				mPulse[aChannel][mCount[aChannel]].amp = aAmp;
				++mCount[aChannel];
			}
		}
	};

	class PulseInstance : public SoLoud::AudioSourceInstance
	{
	public:
		unsigned int mPos = 0;
		PulseSpec mSpec;

		unsigned int getAudio(float *aBuffer, unsigned int aSamples,
		                      unsigned int aStride) override
		{
			for (unsigned int c = 0; c < mChannels; ++c)
			{
				for (unsigned int i = 0; i < aSamples; ++i)
				{
					unsigned int frame = mPos + i;
					float v = 0.0f;
					for (int k = 0; k < mSpec.mCount[c]; ++k)
					{
						if (mSpec.mPulse[c][k].at == frame)
							v = mSpec.mPulse[c][k].amp;
					}
					aBuffer[(size_t)c * aStride + i] = v;
				}
			}
			mPos += aSamples;
			return aSamples;
		}

		bool hasEnded() override { return false; }
	};

	class Pulse : public SoLoud::AudioSource
	{
	public:
		PulseSpec mSpec;
		explicit Pulse(unsigned int aChannels)
		{
			mChannels = aChannels;
			mBaseSamplerate = (float)SR;
		}
		SoLoud::AudioSourceInstance *createInstance() override
		{
			PulseInstance *p = new PulseInstance();
			p->mSpec = mSpec;
			return p;
		}
	};

	// ---------------------------------------------------------------------
	// Rig owns the engine and every graph object. Soloud is the first
	// member so it is destroyed last; sources/buses/filters die first.
	// ---------------------------------------------------------------------
	struct Rig
	{
		SoLoud::Soloud mSol;
		std::vector<std::unique_ptr<SoLoud::Filter> > mFilters;
		std::vector<std::unique_ptr<Pulse> > mSources;
		std::vector<std::unique_ptr<SoLoud::Bus> > mBuses;
		unsigned int mChannels = 2;

		void init(unsigned int aChannels)
		{
			mChannels = aChannels;
			mSol.init(0, SoLoud::Soloud::NULLDRIVER, SR, 1024, aChannels);
			mSol.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
			mSol.setPostClipScaler(1.0f);
			mSol.setMaxActiveVoiceCount(128);
		}

		SoLoud::Filter *delay(unsigned int aFrames)
		{
			mFilters.emplace_back(new DelayFilter(aFrames));
			return mFilters.back().get();
		}

		Pulse *source(unsigned int aChannels, const PulseSpec &aSpec)
		{
			Pulse *p = new Pulse(aChannels);
			p->mSpec = aSpec;
			mSources.emplace_back(p);
			return p;
		}

		SoLoud::Bus *bus(unsigned int aChannels, int aFilterDelay = -1)
		{
			SoLoud::Bus *b = new SoLoud::Bus();
			mBuses.emplace_back(b);
			b->setChannels(aChannels);
			b->mBaseSamplerate = (float)SR;
			b->setResampler(SoLoud::Soloud::RESAMPLER_POINT);
			if (aFilterDelay >= 0)
				b->setFilter(0, delay((unsigned int)aFilterDelay));
			return b;
		}

		void unity(SoLoud::handle h) { mSol.setPanAbsolute(h, 1.0f, 1.0f); }
	};

	// ---------------------------------------------------------------------
	// Render helpers. Every render performs the same WARM-frame mix first
	// so pan gain smoothing has converged to the fixed targets.
	// ---------------------------------------------------------------------
	struct TagExpect { unsigned int mFrame; unsigned int mCh; float mAmp; };

	int checkPulse(const std::vector<float> &aOut, unsigned int aChannels,
	               unsigned int aChannel, unsigned int aExpectedFrame,
	               float aExpectedAmp);

	std::vector<float> doWarm(Rig &r)
	{
		std::vector<float> w((size_t)WARM * r.mChannels, 0.0f);
		r.mSol.mix(w.data(), WARM);
		return w;
	}

	std::vector<float> renderWhole(Rig &r, unsigned int aFrames)
	{
		doWarm(r);
		std::vector<float> out((size_t)aFrames * r.mChannels, 0.0f);
		r.mSol.mix(out.data(), aFrames);
		return out;
	}

	std::vector<float> renderChunked(Rig &r, unsigned int aFrames)
	{
		doWarm(r);
		static const unsigned int SIZES[3] = {1, 7, 257};
		std::vector<float> out;
		out.reserve((size_t)aFrames * r.mChannels);
		unsigned int done = 0, qi = 0;
		while (done < aFrames)
		{
			unsigned int n = SIZES[qi % 3];
			++qi;
			if (n > aFrames - done)
				n = aFrames - done;
			std::vector<float> tmp((size_t)n * r.mChannels, 0.0f);
			r.mSol.mix(tmp.data(), n);
			out.insert(out.end(), tmp.begin(), tmp.end());
			done += n;
		}
		return out;
	}

	// Render using one fixed fragment size for every mix() call.
	std::vector<float> renderFixedChunks(Rig &r, unsigned int aFrames,
	                                     unsigned int aChunk)
	{
		doWarm(r);
		std::vector<float> out;
		out.reserve((size_t)aFrames * r.mChannels);
		unsigned int done = 0;
		while (done < aFrames)
		{
			unsigned int n = aChunk;
			if (n > aFrames - done)
				n = aFrames - done;
			std::vector<float> tmp((size_t)n * r.mChannels, 0.0f);
			r.mSol.mix(tmp.data(), n);
			out.insert(out.end(), tmp.begin(), tmp.end());
			done += n;
		}
		return out;
	}

	// Compare one whole-buffer render against the cycling 1/7/257 pattern and
	// against uniform 1-, 7- and 257-frame fragments. Every variant uses its
	// own freshly built engine. Also re-checks pulse oracles on each variant.
	template <class BuildFn>
	int blockCompare(BuildFn aBuild, unsigned int aFrames,
	                 unsigned int aChannels, const char *aLabel)
	{
		Rig wholeRig;
		std::vector<TagExpect> wholeTags;
		TASK_CHECK(aBuild(wholeRig, wholeTags) == 0);
		std::vector<float> whole = renderWhole(wholeRig, aFrames);

		struct Mode { const char *mName; unsigned int mFixed; bool mCycle; };
		Mode modes[4] = {
		    {"cycle-1-7-257", 0, true},
		    {"fixed-1", 1, false},
		    {"fixed-7", 7, false},
		    {"fixed-257", 257, false},
		};
		for (unsigned int mi = 0; mi < 4; ++mi)
		{
			Rig cr;
			std::vector<TagExpect> tags;
			TASK_CHECK(aBuild(cr, tags) == 0);
			std::vector<float> chunk = modes[mi].mCycle
			    ? renderChunked(cr, aFrames)
			    : renderFixedChunks(cr, aFrames, modes[mi].mFixed);
			TASK_CHECK(chunk.size() == whole.size());
			float md = 0.0f;
			for (size_t i = 0; i < whole.size(); ++i)
				md = std::max(md, std::fabs(whole[i] - chunk[i]));
			std::printf("  [blocks] %s %s maxDiff=%g\n", aLabel, modes[mi].mName, md);
			TASK_CHECK(md <= 1e-5f);
			TASK_CHECK(tags.size() == wholeTags.size());
			for (size_t i = 0; i < tags.size(); ++i)
			{
				TASK_CHECK(tags[i].mCh < aChannels);
				TASK_CHECK(checkPulse(chunk, aChannels, tags[i].mCh,
				                      tags[i].mFrame, tags[i].mAmp) == 0);
			}
		}
		return 0;
	}

	float sampleAt(const std::vector<float> &aOut, unsigned int aChannels,
	               unsigned int aFrame, unsigned int aChannel)
	{
		return aOut[(size_t)aFrame * aChannels + aChannel];
	}

	// Frame of the largest-magnitude sample of one channel in [f0,f1).
	unsigned int peakFrame(const std::vector<float> &aOut, unsigned int aChannels,
	                       unsigned int aChannel, unsigned int f0, unsigned int f1,
	                       float &aValue)
	{
		unsigned int total = (unsigned int)(aOut.size() / aChannels);
		if (f1 > total)
			f1 = total;
		unsigned int best = f0;
		float bestMag = -1.0f;
		for (unsigned int f = f0; f < f1; ++f)
		{
			float m = std::fabs(sampleAt(aOut, aChannels, f, aChannel));
			if (m > bestMag)
			{
				bestMag = m;
				best = f;
			}
		}
		aValue = sampleAt(aOut, aChannels, best, aChannel);
		return best;
	}

	bool nearFloat(float a, float b, float eps = 1e-5f)
	{
		return std::fabs(a - b) <= eps;
	}

	// ---------------------------------------------------------------------
	// Independent test-side latency tree. Plain delay data, separate from
	// the production planner. calc() computes merge maxima and each node's
	// slack; pathLatency() sums own delays and slacks along a leaf->root.
	// ---------------------------------------------------------------------
	struct TNode
	{
		std::string mName;
		unsigned int mOwn;
		std::vector<std::unique_ptr<TNode> > mKids;
		unsigned int mMerge = 0;
		unsigned int mSub = 0;
		unsigned int mSlack = 0;

		TNode(const char *aName, unsigned int aOwn) : mName(aName), mOwn(aOwn) {}

		TNode *add(const char *aName, unsigned int aOwn)
		{
			mKids.emplace_back(new TNode(aName, aOwn));
			return mKids.back().get();
		}
	};

	unsigned int calcLatency(TNode &n)
	{
		unsigned int worst = 0;
		for (size_t i = 0; i < n.mKids.size(); ++i)
		{
			unsigned int v = calcLatency(*n.mKids[i]);
			if (v > worst)
				worst = v;
		}
		n.mMerge = worst;
		n.mSub = n.mOwn + worst;
		for (size_t i = 0; i < n.mKids.size(); ++i)
			n.mKids[i]->mSlack = worst - n.mKids[i]->mSub;
		return n.mSub;
	}

	unsigned int pathLatency(std::initializer_list<TNode *> aChain)
	{
		unsigned int total = 0;
		for (TNode *n : aChain)
			total += n->mOwn + n->mSlack;
		return total;
	}

	// Verify one isolated pulse: independently predicted frame/amplitude,
	// actual peak location, amplitude and single-sample shape.
	int checkPulse(const std::vector<float> &aOut, unsigned int aChannels,
	               unsigned int aChannel, unsigned int aExpectedFrame,
	               float aExpectedAmp)
	{
		unsigned int total = (unsigned int)(aOut.size() / aChannels);
		TASK_CHECK(aExpectedFrame < total);
		float pv = 0.0f;
		unsigned int pf = peakFrame(aOut, aChannels, aChannel,
		                            aExpectedFrame > 24 ? aExpectedFrame - 24 : 0,
		                            aExpectedFrame + 24, pv);
		TASK_CHECK(pf == aExpectedFrame);
		TASK_CHECK(nearFloat(pv, aExpectedAmp));
		TASK_CHECK(nearFloat(sampleAt(aOut, aChannels, aExpectedFrame, aChannel),
		                     aExpectedAmp));
		if (aExpectedFrame > 0)
			TASK_CHECK(std::fabs(sampleAt(aOut, aChannels, aExpectedFrame - 1,
			                              aChannel)) < 1e-5f);
		TASK_CHECK(std::fabs(sampleAt(aOut, aChannels, aExpectedFrame + 1,
		                              aChannel)) < 1e-5f);
		return 0;
	}

	// =====================================================================
	// graph group
	// =====================================================================

	int g_default_virtual(void)
	{
		SoLoud::FilterInstance fi;
		TASK_CHECK(fi.getLatencyFrames() == 0);
		return 0;
	}

	int g_prepare_empty(void)
	{
		Rig r;
		r.init(2);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == 0);
		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == 0);
		return 0;
	}

	int g_serial_sum(void)
	{
		Rig r;
		r.init(2);
		PulseSpec ps;
		ps.add(0, 500, 0.2f);
		Pulse *p = r.source(2, ps);
		p->setFilter(0, r.delay(11));
		p->setFilter(1, r.delay(22));
		SoLoud::handle h = r.mSol.play(*p);
		r.unity(h);
		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == 33);
		return 0;
	}

	// Build the canonical combo graph and its independent latency tree.
	// Delays: A serial 13+29 (42), bypass B-source 0, Bus filter 17,
	// root source C filter 7. Root total = max(42,0)+17 = 59 vs 7 -> 59.
	struct ComboGraph
	{
		static constexpr unsigned int DA1 = 13, DA2 = 29, DBUS = 17, DC = 7;
		static constexpr unsigned int ROOT = 59;
	};

	void buildCombo(Rig &r, SoLoud::handle &hA, SoLoud::handle &hB,
	                SoLoud::handle &hC)
	{
		r.init(2);
		SoLoud::Bus *bus = r.bus(2, ComboGraph::DBUS);
		SoLoud::handle bh = r.mSol.play(*bus);
		r.unity(bh);

		PulseSpec sa;
		Pulse *A = r.source(2, sa);
		A->setFilter(0, r.delay(ComboGraph::DA1));
		A->setFilter(1, r.delay(ComboGraph::DA2));
		hA = bus->play(*A);
		r.unity(hA);
		r.mSol.setAutoStop(hA, false);

		PulseSpec sb;
		Pulse *B = r.source(2, sb); // explicit bypass branch
		hB = bus->play(*B);
		r.unity(hB);
		r.mSol.setAutoStop(hB, false);

		PulseSpec sc;
		Pulse *C = r.source(2, sc);
		C->setFilter(0, r.delay(ComboGraph::DC));
		hC = r.mSol.play(*C);
		r.unity(hC);
		r.mSol.setAutoStop(hC, false);
		// Distinct fixed channel gains on path C.
		r.mSol.setChannelVolume(hC, 0, 0.5f);
		r.mSol.setChannelVolume(hC, 1, 0.25f);
	}

	int g_combo_oracle(void)
	{
		Rig r;
		SoLoud::handle hA, hB, hC;
		buildCombo(r, hA, hB, hC);

		TNode root("root", 0);
		TNode *bus = root.add("bus", ComboGraph::DBUS);
		TNode *A = bus->add("A", ComboGraph::DA1 + ComboGraph::DA2);
		TNode *B = bus->add("B", 0);
		TNode *C = root.add("C", ComboGraph::DC);
		TASK_CHECK(calcLatency(root) == ComboGraph::ROOT);
		TASK_CHECK(pathLatency({A, bus}) == ComboGraph::ROOT);
		TASK_CHECK(pathLatency({B, bus}) == ComboGraph::ROOT);
		TASK_CHECK(pathLatency({C}) == ComboGraph::ROOT);

		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == ComboGraph::ROOT);
		return 0;
	}

	int g_long3_oracle(void)
	{
		// Three-level bus arithmetic (610 frames total, > SAMPLE_GRANULARITY).
		// Sa 200+150=350, Sd bypass on B3; B3 own 120 -> 470.
		// Sb bypass on B2, B2 own 80 -> 550.
		// Sc bypass on B1, B1 own 60 -> 610.
		TNode root("root", 0);
		TNode *b1 = root.add("B1", 60);
		TNode *b2 = b1->add("B2", 80);
		TNode *b3 = b2->add("B3", 120);
		TNode *sa = b3->add("Sa", 350);
		TNode *sd = b3->add("Sd", 0);
		TNode *sb = b2->add("Sb", 0);
		TNode *sc = b1->add("Sc", 0);
		TASK_CHECK(calcLatency(root) == 610);
		TASK_CHECK(pathLatency({sa, b3, b2, b1}) == 610);
		TASK_CHECK(pathLatency({sd, b3, b2, b1}) == 610);
		TASK_CHECK(pathLatency({sb, b2, b1}) == 610);
		TASK_CHECK(pathLatency({sc, b1}) == 610);
		return 0;
	}

	int g_prepare_uninit_invalid(void)
	{
		SoLoud::Soloud s;
		TASK_CHECK(s.prepareFilterLatencyCompensation() ==
		           SoLoud::INVALID_PARAMETER);
		TASK_CHECK(s.getFilterLatencyCompensationFrames() == 0);
		return 0;
	}

	int g_prepare_after_mix_invalid(void)
	{
		Rig r;
		SoLoud::handle hA, hB, hC;
		buildCombo(r, hA, hB, hC);
		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == ComboGraph::ROOT);
		std::vector<float> z(2, 0.0f);
		r.mSol.mix(z.data(), 1);
		// Mixing has started: prepare must be rejected and the committed
		// plan/query must be unchanged (no half replacement).
		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() ==
		           SoLoud::INVALID_PARAMETER);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == ComboGraph::ROOT);
		return 0;
	}

	// =====================================================================
	// pcm group
	// =====================================================================

	// Full combo render data. Tag pulses are path-unique; one shared align
	// pulse lets the branches sum at one frame.
	struct ComboData
	{
		static constexpr unsigned int PA = 900;
		static constexpr unsigned int PB = 1100;
		static constexpr unsigned int PC = 1300;
		static constexpr unsigned int ALIGN = 1500;
		static constexpr unsigned int FRAMES = 1800;
	};

	// Build combo with pulses. Returns per-path tag expectations.
	std::vector<TagExpect> buildComboPcm(Rig &r, bool withPlan = true)
	{
		r.init(2);
		SoLoud::Bus *bus = r.bus(2, ComboGraph::DBUS);
		SoLoud::handle bh = r.mSol.play(*bus);
		r.unity(bh);

		PulseSpec sa;
		for (unsigned int c = 0; c < 2; ++c)
		{
			sa.add(c, ComboData::PA + c * 3, 0.10f * (c + 1));
			sa.add(c, ComboData::ALIGN, 0.05f);
		}
		Pulse *A = r.source(2, sa);
		A->setFilter(0, r.delay(ComboGraph::DA1));
		A->setFilter(1, r.delay(ComboGraph::DA2));
		SoLoud::handle hA = bus->play(*A);
		r.unity(hA);
		r.mSol.setAutoStop(hA, false);

		PulseSpec sb;
		for (unsigned int c = 0; c < 2; ++c)
		{
			sb.add(c, ComboData::PB + c * 3, 0.15f * (c + 1));
			sb.add(c, ComboData::ALIGN, 0.05f);
		}
		Pulse *B = r.source(2, sb);
		SoLoud::handle hB = bus->play(*B);
		r.unity(hB);
		r.mSol.setAutoStop(hB, false);

		PulseSpec sc;
		static const float GAIN[2] = {0.5f, 0.25f};
		for (unsigned int c = 0; c < 2; ++c)
		{
			sc.add(c, ComboData::PC + c * 3, 0.08f * (c + 1));
			sc.add(c, ComboData::ALIGN, 0.05f);
		}
		Pulse *C = r.source(2, sc);
		C->setFilter(0, r.delay(ComboGraph::DC));
		SoLoud::handle hC = r.mSol.play(*C);
		r.unity(hC);
		r.mSol.setAutoStop(hC, false);
		r.mSol.setChannelVolume(hC, 0, GAIN[0]);
		r.mSol.setChannelVolume(hC, 1, GAIN[1]);

		if (withPlan)
			r.mSol.prepareFilterLatencyCompensation();

		std::vector<TagExpect> tags;
		for (unsigned int c = 0; c < 2; ++c)
		{
			// All three paths independently sum to ROOT latency.
			unsigned int latA = (ComboGraph::DA1 + ComboGraph::DA2) + 0 +
			                    ComboGraph::DBUS + 0;
			unsigned int latB = 0 + (ComboGraph::DA1 + ComboGraph::DA2) +
			                    ComboGraph::DBUS + 0; // bypass slack 42
			unsigned int latC = ComboGraph::DC +
			                    (ComboGraph::ROOT - ComboGraph::DC);
			tags.push_back({ComboData::PA + c * 3 + latA - WARM, c,
			                0.10f * (c + 1)});
			tags.push_back({ComboData::PB + c * 3 + latB - WARM, c,
			                0.15f * (c + 1)});
			tags.push_back({ComboData::PC + c * 3 + latC - WARM, c,
			                0.08f * (c + 1) * GAIN[c]});
		}
		return tags;
	}

	int p_combo_pulses(void)
	{
		Rig r;
		std::vector<TagExpect> tags = buildComboPcm(r);
		std::vector<float> out = renderWhole(r, ComboData::FRAMES);
		for (size_t i = 0; i < tags.size(); ++i)
		{
			TASK_CHECK(checkPulse(out, 2, tags[i].mCh, tags[i].mFrame,
			                      tags[i].mAmp) == 0);
		}
		// Shared alignment pulse: all three branches at one frame.
		static const float GAIN[2] = {0.5f, 0.25f};
		unsigned int af = ComboData::ALIGN + ComboGraph::ROOT - WARM;
		for (unsigned int c = 0; c < 2; ++c)
		{
			float want = 0.05f + 0.05f + 0.05f * GAIN[c];
			TASK_CHECK(checkPulse(out, 2, c, af, want) == 0);
		}
		return 0;
	}

	int buildComboTags(Rig &r, std::vector<TagExpect> &tags)
	{
		tags = buildComboPcm(r);
		return 0;
	}

	int b_combo_blocks(void)
	{
		return blockCompare(buildComboTags, ComboData::FRAMES, 2, "combo");
	}

	// ---------------------------------------------------------------------
	// Three-level bus, total latency 610 (>512).
	// ---------------------------------------------------------------------
	struct LongData
	{
		static constexpr unsigned int L_SA = 350; // 200+150
		static constexpr unsigned int L_B3 = 120;
		static constexpr unsigned int L_B2 = 80;
		static constexpr unsigned int L_B1 = 60;
		static constexpr unsigned int ROOT = 610;
		static constexpr unsigned int FRAMES = 2500;
	};

	int buildLong3(Rig &r, std::vector<TagExpect> &tags)
	{
		r.init(2);
		SoLoud::Bus *b1 = r.bus(2, LongData::L_B1);
		SoLoud::handle h1 = r.mSol.play(*b1);
		r.unity(h1);
		SoLoud::Bus *b2 = r.bus(2, LongData::L_B2);
		SoLoud::handle h2 = b1->play(*b2);
		r.unity(h2);
		SoLoud::Bus *b3 = r.bus(2, LongData::L_B3);
		SoLoud::handle h3 = b2->play(*b3);
		r.unity(h3);

		struct Item { SoLoud::Bus *parent; unsigned int at; float amp; unsigned int own; };
		std::vector<Item> items;
		items.push_back({b3, 900, 0.10f, LongData::L_SA});
		items.push_back({b3, 1050, 0.12f, 0});
		items.push_back({b2, 1200, 0.14f, 0});
		items.push_back({b1, 1350, 0.16f, 0});

		for (size_t k = 0; k < items.size(); ++k)
		{
			PulseSpec ps;
			for (unsigned int c = 0; c < 2; ++c)
				ps.add(c, items[k].at + c * 5, items[k].amp * (c + 1) * 0.5f);
			Pulse *p = r.source(2, ps);
			if (items[k].own == LongData::L_SA)
			{
				p->setFilter(0, r.delay(200));
				p->setFilter(1, r.delay(150));
			}
			SoLoud::handle h = items[k].parent->play(*p);
			r.unity(h);
			r.mSol.setAutoStop(h, false);
			for (unsigned int c = 0; c < 2; ++c)
			{
				tags.push_back({items[k].at + c * 5 + LongData::ROOT - WARM, c,
				                items[k].amp * (c + 1) * 0.5f});
			}
		}
		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == LongData::ROOT);
		return 0;
	}

	int p_long3_pulses(void)
	{
		Rig r;
		std::vector<TagExpect> tags;
		TASK_CHECK(buildLong3(r, tags) == 0);
		std::vector<float> out = renderWhole(r, LongData::FRAMES);
		for (size_t i = 0; i < tags.size(); ++i)
			TASK_CHECK(checkPulse(out, 2, tags[i].mCh, tags[i].mFrame,
			                      tags[i].mAmp) == 0);
		return 0;
	}

	int b_long3_blocks(void)
	{
		return blockCompare(buildLong3, LongData::FRAMES, 2, "long3");
	}

	// ---------------------------------------------------------------------
	// Explicit left/right different signals.
	// ---------------------------------------------------------------------
	int p_lr_distinct(void)
	{
		Rig r;
		r.init(2);
		PulseSpec ps;
		ps.add(0, 800, 0.20f);  // left event early
		ps.add(1, 1500, 0.35f); // right event much later
		Pulse *p = r.source(2, ps);
		p->setFilter(0, r.delay(31));
		SoLoud::handle h = r.mSol.play(*p);
		r.unity(h);
		r.mSol.setAutoStop(h, false);
		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == 31);
		std::vector<float> out = renderWhole(r, 2200);
		TASK_CHECK(checkPulse(out, 2, 0, 800 + 31 - WARM, 0.20f) == 0);
		TASK_CHECK(checkPulse(out, 2, 1, 1500 + 31 - WARM, 0.35f) == 0);
		// The opposite channel must stay silent at each event frame.
		TASK_CHECK(std::fabs(sampleAt(out, 2, 800 + 31 - WARM, 1)) < 1e-5f);
		TASK_CHECK(std::fabs(sampleAt(out, 2, 1500 + 31 - WARM, 0)) < 1e-5f);
		return 0;
	}

	// ---------------------------------------------------------------------
	// Six-channel graph with independent per-channel pulse positions.
	// X delay 40, Y bypass on 6ch bus (own 23); Z root delay 5.
	// root total = 40+23 = 63; Z gets 58 frames of 6ch root slack.
	// ---------------------------------------------------------------------
	struct SixData
	{
		static constexpr unsigned int ROOT = 63;
		static constexpr unsigned int FRAMES = 2100;
	};

	int buildSix(Rig &r, std::vector<TagExpect> &tags)
	{
		r.init(6);
		SoLoud::Bus *bus = r.bus(6, 23);
		SoLoud::handle bh = r.mSol.play(*bus);
		r.unity(bh);

		PulseSpec px;
		for (unsigned int c = 0; c < 6; ++c)
			px.add(c, 900 + c * 11, 0.05f * (c + 1));
		Pulse *X = r.source(6, px);
		X->setFilter(0, r.delay(40));
		SoLoud::handle hx = bus->play(*X);
		r.unity(hx);
		r.mSol.setAutoStop(hx, false);

		PulseSpec py;
		for (unsigned int c = 0; c < 6; ++c)
			py.add(c, 1200 + c * 13, 0.04f * (c + 1));
		Pulse *Y = r.source(6, py); // bypass, gets 40 frames slack in bus
		SoLoud::handle hy = bus->play(*Y);
		r.unity(hy);
		r.mSol.setAutoStop(hy, false);

		PulseSpec pz;
		for (unsigned int c = 0; c < 6; ++c)
			pz.add(c, 1500 + c * 7, 0.03f * (c + 1));
		Pulse *Z = r.source(6, pz);
		Z->setFilter(0, r.delay(5)); // gets 58 frames slack at root
		SoLoud::handle hz = r.mSol.play(*Z);
		r.unity(hz);
		r.mSol.setAutoStop(hz, false);

		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == SixData::ROOT);

		for (unsigned int c = 0; c < 6; ++c)
		{
			tags.push_back({900 + c * 11 + SixData::ROOT - WARM, c,
			                0.05f * (c + 1)});
			tags.push_back({1200 + c * 13 + SixData::ROOT - WARM, c,
			                0.04f * (c + 1)});
			tags.push_back({1500 + c * 7 + SixData::ROOT - WARM, c,
			                0.03f * (c + 1)});
		}
		return 0;
	}

	int p_six_channel(void)
	{
		Rig r;
		std::vector<TagExpect> tags;
		TASK_CHECK(buildSix(r, tags) == 0);
		std::vector<float> out = renderWhole(r, SixData::FRAMES);
		for (size_t i = 0; i < tags.size(); ++i)
			TASK_CHECK(checkPulse(out, 6, tags[i].mCh, tags[i].mFrame,
			                      tags[i].mAmp) == 0);
		return 0;
	}

	int b_six_blocks(void)
	{
		return blockCompare(buildSix, SixData::FRAMES, 6, "six");
	}

	// ---------------------------------------------------------------------
	// Inverse topology: the longest root child is a direct source, so the
	// nested Bus voice itself must receive the root-level slack (88).
	// ---------------------------------------------------------------------
	int p_root_slack_onto_bus(void)
	{
		Rig r;
		r.init(2);
		SoLoud::Bus *bus = r.bus(2, 12);
		SoLoud::handle bh = r.mSol.play(*bus);
		r.unity(bh);

		PulseSpec pbSpec;
		pbSpec.add(0, 1000, 0.21f);
		pbSpec.add(1, 1000, 0.21f);
		Pulse *pb = r.source(2, pbSpec);
		SoLoud::handle hb = bus->play(*pb);
		r.unity(hb);
		r.mSol.setAutoStop(hb, false);

		PulseSpec prSpec;
		prSpec.add(0, 1000, 0.22f);
		prSpec.add(1, 1000, 0.22f);
		Pulse *pr = r.source(2, prSpec);
		pr->setFilter(0, r.delay(100));
		SoLoud::handle hr = r.mSol.play(*pr);
		r.unity(hr);
		r.mSol.setAutoStop(hr, false);

		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == 100);
		std::vector<float> out = renderWhole(r, 1400);
		unsigned int ef = 1000 + 100 - WARM;
		// Both paths align and sum (0.21 + 0.22).
		TASK_CHECK(checkPulse(out, 2, 0, ef, 0.43f) == 0);
		TASK_CHECK(checkPulse(out, 2, 1, ef, 0.43f) == 0);
		return 0;
	}

	// ---------------------------------------------------------------------
	// Mono support.
	// ---------------------------------------------------------------------
	int p_mono(void)
	{
		Rig r;
		r.init(1);
		PulseSpec p1;
		p1.add(0, 800, 0.2f);
		Pulse *a = r.source(1, p1);
		a->setFilter(0, r.delay(19));
		SoLoud::handle ha = r.mSol.play(*a);
		r.unity(ha);
		r.mSol.setAutoStop(ha, false);

		PulseSpec p2;
		p2.add(0, 1100, 0.15f);
		Pulse *b = r.source(1, p2); // bypass sibling gets 19 slack
		SoLoud::handle hb = r.mSol.play(*b);
		r.unity(hb);
		r.mSol.setAutoStop(hb, false);

		TASK_CHECK(r.mSol.prepareFilterLatencyCompensation() == SoLoud::SO_NO_ERROR);
		TASK_CHECK(r.mSol.getFilterLatencyCompensationFrames() == 19);
		std::vector<float> out = renderWhole(r, 1700);
		TASK_CHECK(checkPulse(out, 1, 0, 800 + 19 - WARM, 0.2f) == 0);
		TASK_CHECK(checkPulse(out, 1, 0, 1100 + 19 - WARM, 0.15f) == 0);
		return 0;
	}

	// ---------------------------------------------------------------------
	// deinit -> re-init -> play -> prepare must not reuse stale plan state.
	// ---------------------------------------------------------------------
	int p_reinit_clean(void)
	{
		SoLoud::Soloud sol;
		{
			// First render with a large latency plan.
			DelayFilter big(321);
			PulseSpec ps;
			ps.add(0, 700, 0.2f);
			ps.add(1, 703, 0.3f);
			Pulse src(2);
			src.mSpec = ps;
			src.setFilter(0, &big);
			TASK_CHECK(sol.init(0, SoLoud::Soloud::NULLDRIVER, SR, 1024, 2) ==
			           SoLoud::SO_NO_ERROR);
			sol.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
			sol.setPostClipScaler(1.0f);
			SoLoud::handle h = sol.play(src);
			sol.setPanAbsolute(h, 1, 1);
			sol.setAutoStop(h, false);
			TASK_CHECK(sol.prepareFilterLatencyCompensation() ==
			           SoLoud::SO_NO_ERROR);
			TASK_CHECK(sol.getFilterLatencyCompensationFrames() == 321);
			std::vector<float> w((size_t)WARM * 2, 0.f);
			sol.mix(w.data(), WARM);
			std::vector<float> o(1400 * 2, 0.f);
			sol.mix(o.data(), 1400);
			TASK_CHECK(nearFloat(sampleAt(o, 2, 700 + 321 - WARM, 0), 0.2f));
			sol.deinit();
			TASK_CHECK(sol.getFilterLatencyCompensationFrames() == 0);
		}
		{
			// Re-init the same engine object with a tiny plan. Reused voice
			// slots must not carry old delay rings.
			DelayFilter small(5);
			PulseSpec ps;
			ps.add(0, 700, 0.2f);
			ps.add(1, 703, 0.3f);
			Pulse src(2);
			src.mSpec = ps;
			src.setFilter(0, &small);
			TASK_CHECK(sol.init(0, SoLoud::Soloud::NULLDRIVER, SR, 1024, 2) ==
			           SoLoud::SO_NO_ERROR);
			sol.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
			sol.setPostClipScaler(1.0f);
			SoLoud::handle h = sol.play(src);
			sol.setPanAbsolute(h, 1, 1);
			sol.setAutoStop(h, false);
			TASK_CHECK(sol.prepareFilterLatencyCompensation() ==
			           SoLoud::SO_NO_ERROR);
			TASK_CHECK(sol.getFilterLatencyCompensationFrames() == 5);
			std::vector<float> w((size_t)WARM * 2, 0.f);
			sol.mix(w.data(), WARM);
			std::vector<float> o(1400 * 2, 0.f);
			sol.mix(o.data(), 1400);
			TASK_CHECK(checkPulse(o, 2, 0, 700 + 5 - WARM, 0.2f) == 0);
			TASK_CHECK(checkPulse(o, 2, 1, 703 + 5 - WARM, 0.3f) == 0);
			// Old 321-frame compensation must be absent.
			TASK_CHECK(std::fabs(sampleAt(o, 2, 700 + 321 - WARM, 0)) < 1e-5f);
			sol.deinit();
		}
		return 0;
	}
}

// ---------------------------------------------------------------------------
// Registration consumed by task_support/driver.c (compiled as C++).
// ---------------------------------------------------------------------------
const struct task_case task_cases[] = {
	{"graph", "default_filter_instance_zero_latency", g_default_virtual},
	{"graph", "prepare_empty_graph", g_prepare_empty},
	{"graph", "serial_filter_latency_sum", g_serial_sum},
	{"graph", "combo_graph_oracle_59", g_combo_oracle},
	{"graph", "three_bus_graph_oracle_610", g_long3_oracle},
	{"graph", "prepare_uninitialized_rejected", g_prepare_uninit_invalid},
	{"graph", "prepare_after_mix_rejected", g_prepare_after_mix_invalid},

	{"pcm", "combo_serial_bus_bypass", p_combo_pulses},
	{"pcm", "three_level_bus_long_delay", p_long3_pulses},
	{"pcm", "left_right_distinct_signals", p_lr_distinct},
	{"pcm", "six_channels", p_six_channel},
	{"pcm", "root_slack_onto_bus", p_root_slack_onto_bus},
	{"pcm", "mono_channel", p_mono},
	{"pcm", "deinit_reinit_clean_state", p_reinit_clean},

	{"blocks", "combo_whole_vs_1_7_257", b_combo_blocks},
	{"blocks", "long_three_bus_whole_vs_blocks", b_long3_blocks},
	{"blocks", "six_channel_whole_vs_blocks", b_six_blocks},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
