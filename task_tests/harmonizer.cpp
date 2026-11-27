// Task tests for SignalsmithHarmonizer: equivalence, shared analysis,
// automation latching and streaming behaviour.  No main() here - the fixed
// driver in task_support/main.cpp owns registration and completion.
#include "task_support/registry.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>
#include <cmath>
#include <string>

// The hook must be visible the first time the vendored STFT header is seen.
// It sits at the real forward-FFFT completion point inside DynamicSTFT.
namespace task_harm {
struct AnalysisCounters { long current = 0; long lookback = 0; } analysisCounters;
}
#define SIGNALSMITH_TASK_ANALYSIS(channel, samplesInPast) \
	do { if ((samplesInPast) == 0) ++::task_harm::analysisCounters.current; \
	     else ++::task_harm::analysisCounters.lookback; } while (0)

#include "signalsmith-harmonizer.h"
#include "signalsmith-stretch.h"

using namespace signalsmith::stretch;

// ---- Heap accounting: fixed-size workspaces mean none of this may move during audio ----
namespace task_harm {
long heapNew = 0, heapDelete = 0;
}
void* operator new(std::size_t n) {
	++task_harm::heapNew;
	void *p = std::malloc(n);
	if (!p) throw std::bad_alloc();
	return p;
}
void* operator new[](std::size_t n) {
	++task_harm::heapNew;
	void *p = std::malloc(n);
	if (!p) throw std::bad_alloc();
	return p;
}
void operator delete(void *p) noexcept { if (p) ++task_harm::heapDelete; std::free(p); }
void operator delete[](void *p) noexcept { if (p) ++task_harm::heapDelete; std::free(p); }
void operator delete(void *p, std::size_t) noexcept { if (p) ++task_harm::heapDelete; std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { if (p) ++task_harm::heapDelete; std::free(p); }

namespace {

using std::vector;

// Generated material: multi-tone, periodic pulses and two distinct channels.
template<class Sample>
struct Material {
	int channels, total;
	vector<vector<Sample>> data;
	Material(int ch, int length) : channels(ch), total(length), data(ch, vector<Sample>(length, Sample(0))) {
		for (int c = 0; c < ch; ++c) {
			for (int n = 0; n < length; ++n) {
				double v = 0.22*std::sin(0.071*n + c*1.3)
				         + 0.13*std::sin(0.0293*n*(1 + 0.25*c) + 0.7)
				         + 0.06*std::cos(0.173*n + c);
				if (n % 113 == 7) v += (c ? -0.35 : 0.4);      // pulse
				if (n % 67 == 3) v += (c ? 0.18 : -0.22);      // shorter pulse
				data[c][n] = Sample(v);
			}
		}
	}
	// A variant with a long silent gap in the middle, for silence recovery.
	static Material withSilence(int ch, int n, int gapStart, int gapLen) {
		Material m(ch, n);
		for (int c = 0; c < ch; ++c)
			for (int i = gapStart; i < std::min(n, gapStart + gapLen); ++i) m.data[c][i] = 0;
		return m;
	}
};

template<class Sample>
struct VoiceSettings { Sample pitch, formant; bool comp; };

// Builds matching standalone reference processors, one per voice.
template<class Sample>
struct References {
	vector<SignalsmithStretch<Sample>*> s;
	References(int voices, int ch, int B, int I, bool split, long seed,
	           const vector<VoiceSettings<Sample>> &cfg, Sample formantBase) {
		for (int v = 0; v < voices; ++v) {
			auto *p = new SignalsmithStretch<Sample>(seed);
			p->configure(ch, B, I, split);
			p->setTransposeFactor(cfg[v].pitch);
			p->setFormantFactor(cfg[v].formant, cfg[v].comp);
			p->setFormantBase(formantBase);
			s.push_back(p);
		}
	}
	~References() { for (auto *p : s) delete p; }
};

template<class Sample>
vector<VoiceSettings<Sample>> defaultSettings(int voices) {
	// Covers transpose 0.5..2, formant 0.8..1.25 and compensation on/off.
	static const double pitchTable[4] = {0.5, 1.25, 2.0, 0.8};
	static const double formTable[4] = {1.0, 0.8, 1.25, 0.95};
	vector<VoiceSettings<Sample>> out;
	for (int v = 0; v < voices; ++v) {
		out.push_back({Sample(pitchTable[v % 4]), Sample(formTable[v % 4]), (v % 2) == 1});
	}
	return out;
}

// Drives harmonizer and per-voice references through identical call sequences
// (streaming process calls, then an optional flush) and returns the largest
// absolute per-sample difference across every voice/channel.
template<class Sample>
Sample staticEquivalence(int ch, int voices, int B, int I, bool split, long seed,
                         const vector<VoiceSettings<Sample>> &cfg,
                         int inChunk, int outChunk, int total,
                         int flushLen, int zeroPad, Sample formantBase) {
	SignalsmithHarmonizer<Sample> h(seed);
	h.configure(ch, voices, B, I, split);
	for (int v = 0; v < voices; ++v) h.setVoice(v, cfg[v].pitch, cfg[v].formant, cfg[v].comp);
	h.setFormantBase(formantBase);
	References<Sample> refs(voices, ch, B, I, split, seed, cfg, formantBase);

	Material<Sample> mat(ch, total);
	vector<vector<Sample>> hOut(voices*ch, vector<Sample>(outChunk));
	vector<vector<Sample>> rOut(voices*ch, vector<Sample>(outChunk));
	vector<const Sample*> inPtr(ch);
	vector<Sample*> hPtr(voices*ch), rPtr(voices*ch);

	Sample worst = 0;
	auto compare = [&](int length) {
		for (int k = 0; k < voices*ch; ++k)
			for (int i = 0; i < length; ++i)
				worst = std::max(worst, Sample(std::fabs(double(hOut[k][i]) - double(rOut[k][i]))));
	};
	auto zeroCall = [&](int length) {
		for (int c = 0; c < ch; ++c) inPtr[c] = nullptr;
		vector<Sample> zeros(std::max(length, 1), Sample(0));
		for (int c = 0; c < ch; ++c) inPtr[c] = zeros.data();
		for (int k = 0; k < voices*ch; ++k) hPtr[k] = hOut[k].data();
		h.process(inPtr, length, hPtr, outChunk);
		for (int v = 0; v < voices; ++v) {
			for (int c = 0; c < ch; ++c) rPtr[c] = rOut[v*ch + c].data();
			refs.s[v]->process(inPtr, length, rPtr, outChunk);
		}
		compare(outChunk);
	};

	// Optional inputLatency worth of zero input, exactly like the original usage.
	int padded = 0;
	while (padded < zeroPad) {
		int length = std::min(inChunk, zeroPad - padded);
		zeroCall(length);
		padded += length;
	}

	for (int pos = 0; pos < total; ) {
		int nIn = std::min(inChunk, total - pos);
		for (int c = 0; c < ch; ++c) inPtr[c] = mat.data[c].data() + pos;
		for (int k = 0; k < voices*ch; ++k) hPtr[k] = hOut[k].data();
		h.process(inPtr, nIn, hPtr, outChunk);
		for (int v = 0; v < voices; ++v) {
			for (int c = 0; c < ch; ++c) rPtr[c] = rOut[v*ch + c].data();
			refs.s[v]->process(inPtr, nIn, rPtr, outChunk);
		}
		compare(outChunk);
		pos += nIn;
	}

	// Tail: no more than blockSamples, using the original flush trimming rule.
	int fLen = flushLen > 0 ? std::min(flushLen, B) : 0;
	if (fLen > 0) {
		vector<vector<Sample>> hf(voices*ch, vector<Sample>(fLen));
		vector<vector<Sample>> rf(voices*ch, vector<Sample>(fLen));
		vector<Sample*> hfp(voices*ch), rfp(voices*ch);
		for (int k = 0; k < voices*ch; ++k) hfp[k] = hf[k].data();
		h.flush(hfp, fLen);
		for (int v = 0; v < voices; ++v) {
			for (int c = 0; c < ch; ++c) rfp[c] = rf[v*ch + c].data();
			refs.s[v]->flush(rfp, fLen);
		}
		for (int k = 0; k < voices*ch; ++k)
			for (int i = 0; i < fLen; ++i)
				worst = std::max(worst, Sample(std::fabs(double(hf[k][i]) - double(rf[k][i]))));
	}
	return worst;
}

// ============================ equivalence ============================

TASK_CASE("equivalence", eq_static_double) {
	const long seed = 12345;
	int blocks[2][2] = {{512, 128}, {1024, 256}};
	int chunkings[4][2] = {{128, 128}, {96, 128}, {200, 150}, {192, 128}}; // ratios 1.0, 0.75, ~1.33, 1.5
	for (int bi = 0; bi < 2; ++bi) {
		int B = blocks[bi][0], I = blocks[bi][1];
		for (int split = 0; split <= 1; ++split) {
			for (int ch : {1, 2}) {
				for (int voices : {1, 2, 4}) {
					auto cfg = defaultSettings<double>(voices);
					for (auto &ck : chunkings) {
						double e = staticEquivalence<double>(ch, voices, B, I, split, seed, cfg,
						                                     ck[0], ck[1], 3072, B, 0, 0.01);
						t.check(e <= 1e-9);
					}
				}
			}
		}
	}
}

TASK_CASE("equivalence", eq_static_float) {
	const long seed = 777;
	int blocks[2][2] = {{512, 128}, {1024, 256}};
	int chunkings[2][2] = {{128, 128}, {100, 150}};
	for (int bi = 0; bi < 2; ++bi) {
		int B = blocks[bi][0], I = blocks[bi][1];
		for (int split = 0; split <= 1; ++split) {
			for (int ch : {1, 2}) {
				for (int voices : {1, 2, 4}) {
					auto cfg = defaultSettings<float>(voices);
					for (auto &ck : chunkings) {
						float e = staticEquivalence<float>(ch, voices, B, I, split, seed, cfg,
						                                   ck[0], ck[1], 2560, B, 0, 0.01f);
						t.check(e <= 2e-5f);
					}
				}
			}
		}
	}
}

TASK_CASE("equivalence", eq_finite_zero_padded_tail) {
	// Finite material handled exactly the original way: pad inputLatency zero
	// inputs, process, then read a tail no longer than blockSamples.
	for (int split = 0; split <= 1; ++split) {
		double e = staticEquivalence<double>(2, 3, 512, 128, split, 99,
		                                     defaultSettings<double>(3),
		                                     128, 128, 2048, 512, 256, 0.01);
		t.check(e <= 1e-9);
		double e2 = staticEquivalence<double>(1, 2, 1024, 256, split, 100,
		                                      defaultSettings<double>(2),
		                                      200, 150, 2200, 300, 512, 0.01); // short flush
		t.check(e2 <= 1e-9);
	}
}

TASK_CASE("equivalence", eq_latency_matches_original) {
	for (int split = 0; split <= 1; ++split) {
		SignalsmithHarmonizer<double> h(5);
		h.configure(2, 4, 512, 128, split);
		SignalsmithStretch<double> ref(5);
		ref.configure(2, 512, 128, split);
		t.check(h.inputLatency() == ref.inputLatency());
		t.check(h.outputLatency() == ref.outputLatency());
		t.check(h.blockSamples() == 512 && h.intervalSamples() == 128);
		// split mode keeps the extra one-interval output delay
		t.check(h.outputLatency() == ref.outputLatency() + 0);
	}
	SignalsmithHarmonizer<float> hf(1);
	hf.configure(1, 1, 1024, 256, true);
	SignalsmithStretch<float> rf(1);
	rf.configure(1, 1024, 256, true);
	t.check(hf.inputLatency() == rf.inputLatency() && hf.outputLatency() == rf.outputLatency());
	t.check(hf.outputLatency() - hf.inputLatency() == 256);
}

// ============================ sharing ============================

template<class Sample>
void feedForSharing(SignalsmithHarmonizer<Sample> &h, int ch, int voices,
                    int inChunk, int outChunk, int total, bool doSeek) {
	Material<Sample> mat(ch, total);
	vector<const Sample*> ip(ch);
	vector<vector<Sample>> out(voices*ch, vector<Sample>(outChunk));
	vector<Sample*> op(voices*ch);
	for (int k = 0; k < voices*ch; ++k) op[k] = out[k].data();
	if (doSeek) {
		for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data();
		h.seek(ip, 256, Sample(1.0));
	}
	for (int pos = 0; pos < total; ) {
		int nIn = std::min(inChunk, total - pos);
		for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data() + pos;
		h.process(ip, nIn, op, outChunk);
		pos += nIn;
	}
}

TASK_CASE("sharing", share_forward_fft_counts) {
	const int B = 512, I = 128;
	long cur[3] = {0, 0, 0}, back[3] = {0, 0, 0};
	int voiceOpts[3] = {1, 2, 4};
	for (int k = 0; k < 3; ++k) {
		int voices = voiceOpts[k];
		task_harm::analysisCounters.current = 0;
		task_harm::analysisCounters.lookback = 0;
		SignalsmithHarmonizer<double> h(11);
		h.configure(2, voices, B, I, false);
		auto cfg = defaultSettings<double>(voices);
		for (int v = 0; v < voices; ++v) h.setVoice(v, cfg[v].pitch, cfg[v].formant, cfg[v].comp);
		h.setFormantBase(0.01);
		feedForSharing(h, 2, voices, 96, 128, 3072, true); // seek forces the lookback path
		cur[k] = task_harm::analysisCounters.current;
		back[k] = task_harm::analysisCounters.lookback;
	}
	// Both paths must actually run, and counts must not grow with voice count.
	for (int k = 0; k < 3; ++k) {
		t.check(cur[k] > 0 && back[k] > 0);
		t.check(cur[k] == cur[0] && back[k] == back[0]);
	}
	// A single standalone instance on the same input arrangement matches.
	task_harm::analysisCounters.current = 0;
	task_harm::analysisCounters.lookback = 0;
	SignalsmithStretch<double> ref(11);
	ref.configure(2, B, I, false);
	auto cfg1 = defaultSettings<double>(1);
	ref.setTransposeFactor(cfg1[0].pitch);
	ref.setFormantFactor(cfg1[0].formant, cfg1[0].comp);
	ref.setFormantBase(0.01);
	Material<double> mat(2, 3072);
	{
		vector<const double*> sp(2); sp[0] = mat.data[0].data(); sp[1] = mat.data[1].data();
		ref.seek(sp, 256, 1.0);
	}
	{
		vector<double> o(128); vector<double*> op(2, o.data());
		vector<const double*> ip(2);
		for (int pos = 0; pos < 3072; ) {
			int nIn = std::min(96, 3072 - pos);
			ip[0] = mat.data[0].data() + pos; ip[1] = mat.data[1].data() + pos;
			ref.process(ip, nIn, op, 128);
			pos += nIn;
		}
	}
	t.check(task_harm::analysisCounters.current == cur[0]);
	t.check(task_harm::analysisCounters.lookback == back[0]);
}

TASK_CASE("sharing", share_counts_unaffected_by_settings) {
	// Heavier per-voice settings must not add any shared forward FFTs.
	const int B = 512, I = 128;
	long baselineCur = 0, baselineBack = 0;
	for (int pass = 0; pass < 2; ++pass) {
		task_harm::analysisCounters.current = 0;
		task_harm::analysisCounters.lookback = 0;
		SignalsmithHarmonizer<double> h(21);
		h.configure(2, 4, B, I, true);
		for (int v = 0; v < 4; ++v) {
			if (pass == 0) h.setVoice(v, 1.0, 1.0, false);
			else h.setVoice(v, 0.5 + 0.5*v, 0.8 + 0.15*v, (v & 1));
		}
		h.setFormantBase(pass ? 0.02 : 0.0);
		feedForSharing(h, 2, 4, 128, 128, 2560, false);
		if (pass == 0) { baselineCur = task_harm::analysisCounters.current; baselineBack = task_harm::analysisCounters.lookback; }
		else {
			t.check(task_harm::analysisCounters.current == baselineCur);
			t.check(task_harm::analysisCounters.lookback == baselineBack);
			t.check(baselineCur > 0);
		}
	}
}

// ============================ automation ============================

TASK_CASE("automation", auto_hop_boundary_equivalence) {
	// Output chunk == interval, so every process call starts on a hop boundary.
	// Settings change between calls on both implementations; latch points align.
	const int ch = 2, voices = 3, B = 512, I = 128, total = 2560, split = 1;
	SignalsmithHarmonizer<double> h(31);
	h.configure(ch, voices, B, I, split);
	auto cfg = defaultSettings<double>(voices);
	for (int v = 0; v < voices; ++v) h.setVoice(v, cfg[v].pitch, cfg[v].formant, cfg[v].comp);
	h.setFormantBase(0.01);
	References<double> refs(voices, ch, B, I, split, 31, cfg, 0.01);

	Material<double> mat(ch, total);
	vector<vector<double>> hOut(voices*ch, vector<double>(I));
	vector<vector<double>> rOut(voices*ch, vector<double>(I));
	vector<const double*> ip(ch);
	vector<double*> hp(voices*ch), rp(ch);
	double worst = 0;

	auto applySettings = [&](int call) {
		for (int v = 0; v < voices; ++v) {
			double pitch = (call % 3 == 0) ? cfg[v].pitch : (0.6 + 0.4*((call + v) % 3));
			double form = (call % 2) ? 1.15 : 0.85;
			bool comp = ((call + v) & 1);
			h.setVoice(v, pitch, form, comp);
			refs.s[v]->setTransposeFactor(pitch);
			refs.s[v]->setFormantFactor(form, comp);
		}
		double base = call % 2 ? 0.02 : 0.0;
		h.setFormantBase(base);
		for (int v = 0; v < voices; ++v) refs.s[v]->setFormantBase(base);
	};

	int call = 0;
	for (int pos = 0; pos < total; pos += I) {
		applySettings(call++); // between process calls; latched on the next hop
		for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data() + pos;
		for (int k = 0; k < voices*ch; ++k) hp[k] = hOut[k].data();
		h.process(ip, I, hp, I);
		for (int v = 0; v < voices; ++v) {
			for (int c = 0; c < ch; ++c) rp[c] = rOut[v*ch + c].data();
			refs.s[v]->process(ip, I, rp, I);
		}
		for (int k = 0; k < voices*ch; ++k)
			for (int i = 0; i < I; ++i)
				worst = std::max(worst, std::fabs(hOut[k][i] - rOut[k][i]));
	}
	t.check(worst <= 1e-9);
}

TASK_CASE("automation", auto_half_hop_uses_old_settings) {
	// Output chunk is half an interval.  Change settings after the first half of
	// a hop: the in-flight hop must finish with the latched (old) settings, and
	// only the next hop picks up the new ones.  Both sides change at equivalent
	// hop boundaries, so the half-hop output must still match.
	const int ch = 1, voices = 2, B = 512, I = 128, half = I/2, total = 2048;
	for (int split = 0; split <= 1; ++split) {
		SignalsmithHarmonizer<double> h(41);
		h.configure(ch, voices, B, I, split);
		auto cfg = defaultSettings<double>(voices);
		for (int v = 0; v < voices; ++v) h.setVoice(v, cfg[v].pitch, cfg[v].formant, cfg[v].comp);
		h.setFormantBase(0.01);
		References<double> refs(voices, ch, B, I, split, 41, cfg, 0.01);

		Material<double> mat(ch, total);
		vector<vector<double>> hOut(voices*ch, vector<double>(half));
		vector<vector<double>> rOut(voices*ch, vector<double>(half));
		vector<const double*> ip(ch);
		vector<double*> hp(voices*ch), rp(ch);
		double worst = 0;
		int calls = total/half;
		int changeCall = (calls/2) | 1; // force odd: lands in the middle of a hop
		bool changed = false, refPending = false;
		for (int call = 0; call < calls; ++call) {
			int pos = call*half;
			ip[0] = mat.data[0].data() + pos;
			for (int k = 0; k < voices*ch; ++k) hp[k] = hOut[k].data();
			// The harmonizer changes mid-hop (odd call).  It must latch and keep
			// using the old settings for the rest of this hop.
			if (call == changeCall && !changed) {
				changed = true;
				t.check((changeCall % 2) == 1);
				h.setVoice(0, 1.6, 1.2, true);
				h.setVoice(1, 0.7, 0.85, false);
				h.setFormantBase(0.015);
				refPending = true;
			}
			// The reference only picks up the change at the next hop boundary
			// (the following, even call), matching where the latched harmonizer
			// applies it.  This proves the in-flight hop kept the old settings.
			else if (refPending) {
				refPending = false;
				t.check((call % 2) == 0);
				for (int v = 0; v < voices; ++v) {
					refs.s[v]->setTransposeFactor(v == 0 ? 1.6 : 0.7);
					refs.s[v]->setFormantFactor(v == 0 ? 1.2 : 0.85, v == 0);
					refs.s[v]->setFormantBase(0.015);
				}
			}
			h.process(ip, half, hp, half);
			for (int v = 0; v < voices; ++v) {
				rp[0] = rOut[v].data();
				refs.s[v]->process(ip, half, rp, half);
			}
			for (int k = 0; k < voices; ++k)
				for (int i = 0; i < half; ++i)
					worst = std::max(worst, std::fabs(hOut[k][i] - rOut[k][i]));
		}
		t.check(changed);
		t.check(worst <= 1e-9);
	}
}

TASK_CASE("automation", auto_voice_isolation_and_persistence) {
	// Changing one voice must not reset or alter another voice, and must not
	// clear the changed voice's own phase/tail history.
	const int ch = 2, voices = 2, B = 512, I = 128, total = 1536;
	SignalsmithHarmonizer<double> h(51);
	h.configure(ch, voices, B, I, true);
	h.setVoice(0, 0.8, 1.0, false);
	h.setVoice(1, 1.25, 0.9, true);
	h.setFormantBase(0.01);

	SignalsmithStretch<double> refFixed(51);   // voice 1: fixed throughout
	refFixed.configure(ch, B, I, true);
	refFixed.setTransposeFactor(1.25);
	refFixed.setFormantFactor(0.9, true);
	refFixed.setFormantBase(0.01);
	SignalsmithStretch<double> refMoving(51);  // voice 0: changes once
	refMoving.configure(ch, B, I, true);
	refMoving.setTransposeFactor(0.8);
	refMoving.setFormantFactor(1.0, false);
	refMoving.setFormantBase(0.01);

	Material<double> mat(ch, total);
	vector<vector<double>> hOut(voices*ch, vector<double>(128));
	vector<vector<double>> fOut(ch, vector<double>(128)), mOut(ch, vector<double>(128));
	vector<const double*> ip(ch);
	vector<double*> hp(voices*ch), fp(ch), mp(ch);
	double worstFixed = 0, worstMoving = 0;
	int call = 0;
	for (int pos = 0; pos < total; pos += 128) {
		if (call == 4) { // change only voice 0, leave voice 1 alone
			h.setVoice(0, 1.5, 1.2, true);
			refMoving.setTransposeFactor(1.5);
			refMoving.setFormantFactor(1.2, true);
		}
		for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data() + pos;
		for (int k = 0; k < voices*ch; ++k) hp[k] = hOut[k].data();
		h.process(ip, 128, hp, 128);
		for (int c = 0; c < ch; ++c) { fp[c] = fOut[c].data(); mp[c] = mOut[c].data(); }
		refFixed.process(ip, 128, fp, 128);
		refMoving.process(ip, 128, mp, 128);
		for (int c = 0; c < ch; ++c)
			for (int i = 0; i < 128; ++i) {
				worstFixed = std::max(worstFixed, std::fabs(hOut[ch + c][i] - fOut[c][i]));
				worstMoving = std::max(worstMoving, std::fabs(hOut[c][i] - mOut[c][i]));
			}
		++call;
	}
	t.check(worstFixed <= 1e-9);  // untouched voice stays identical
	t.check(worstMoving <= 1e-9); // changed voice matches, with its history kept
}

// ============================ stream ============================

TASK_CASE("stream", stream_irregular_chunks_and_silence) {
	// Fragmented, uneven input/output chunks, a long silence gap and recovery.
	const int ch = 2, voices = 4, B = 512, I = 128, total = 4096;
	for (int split = 0; split <= 1; ++split) {
		auto cfg = defaultSettings<double>(voices);
		SignalsmithHarmonizer<double> h(61);
		h.configure(ch, voices, B, I, split);
		for (int v = 0; v < voices; ++v) h.setVoice(v, cfg[v].pitch, cfg[v].formant, cfg[v].comp);
		h.setFormantBase(0.01);
		References<double> refs(voices, ch, B, I, split, 61, cfg, 0.01);

		Material<double> mat = Material<double>::withSilence(ch, total, 1400, 1400);
		vector<vector<double>> hOut(voices*ch, vector<double>(150));
		vector<vector<double>> rOut(voices*ch, vector<double>(150));
		vector<const double*> ip(ch);
		vector<double*> hp(voices*ch), rp(ch);
		int inChunks[4] = {96, 128, 64, 110};
		double worst = 0;
		int k = 0;
		for (int pos = 0; pos < total; ) {
			int nIn = std::min(inChunks[k % 4], total - pos);
			for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data() + pos;
			for (int x = 0; x < voices*ch; ++x) hp[x] = hOut[x].data();
			h.process(ip, nIn, hp, 150);
			for (int v = 0; v < voices; ++v) {
				for (int c = 0; c < ch; ++c) rp[c] = rOut[v*ch + c].data();
				refs.s[v]->process(ip, nIn, rp, 150);
			}
			for (int x = 0; x < voices*ch; ++x)
				for (int i = 0; i < 150; ++i)
					worst = std::max(worst, std::fabs(hOut[x][i] - rOut[x][i]));
			pos += nIn;
			++k;
		}
		t.check(worst <= 1e-9);
	}
}

TASK_CASE("stream", stream_seek_flush_reset) {
	// seek pre-roll, normal streaming, flush tail, then reset and confirm a
	// fresh run reproduces from-zero output of an identically built processor.
	const int ch = 2, voices = 3, B = 512, I = 128, total = 2560;
	for (int split = 0; split <= 1; ++split) {
		auto cfg = defaultSettings<double>(voices);
		SignalsmithHarmonizer<double> h(71);
		h.configure(ch, voices, B, I, split);
		for (int v = 0; v < voices; ++v) h.setVoice(v, cfg[v].pitch, cfg[v].formant, cfg[v].comp);
		h.setFormantBase(0.01);
		References<double> refs(voices, ch, B, I, split, 71, cfg, 0.01);

		Material<double> mat(ch, total);
		vector<const double*> ip(ch);
		for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data();
		h.seek(ip, 256, 1.0);
		for (int v = 0; v < voices; ++v) refs.s[v]->seek(ip, 256, 1.0);

		vector<vector<double>> hOut(voices*ch, vector<double>(128));
		vector<vector<double>> rOut(voices*ch, vector<double>(128));
		vector<double*> hp(voices*ch), rp(ch);
		double worst = 0;
		for (int pos = 0; pos < total; pos += 128) {
			for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data() + pos;
			for (int x = 0; x < voices*ch; ++x) hp[x] = hOut[x].data();
			h.process(ip, 128, hp, 128);
			for (int v = 0; v < voices; ++v) {
				for (int c = 0; c < ch; ++c) rp[c] = rOut[v*ch + c].data();
				refs.s[v]->process(ip, 128, rp, 128);
			}
			for (int x = 0; x < voices*ch; ++x)
				for (int i = 0; i < 128; ++i)
					worst = std::max(worst, std::fabs(hOut[x][i] - rOut[x][i]));
		}
		vector<vector<double>> hf(voices*ch, vector<double>(300));
		vector<vector<double>> rf(voices*ch, vector<double>(300));
		vector<double*> hfp(voices*ch), rfp(ch);
		for (int x = 0; x < voices*ch; ++x) hfp[x] = hf[x].data();
		h.flush(hfp, 300);
		for (int v = 0; v < voices; ++v) {
			for (int c = 0; c < ch; ++c) rfp[c] = rf[v*ch + c].data();
			refs.s[v]->flush(rfp, 300);
		}
		for (int x = 0; x < voices*ch; ++x)
			for (int i = 0; i < 300; ++i)
				worst = std::max(worst, std::fabs(hf[x][i] - rf[x][i]));
		t.check(worst <= 1e-9);

		// reset keeps configuration/settings; compare against a fresh processor.
		h.reset();
		SignalsmithHarmonizer<double> h2(71);
		h2.configure(ch, voices, B, I, split);
		for (int v = 0; v < voices; ++v) h2.setVoice(v, cfg[v].pitch, cfg[v].formant, cfg[v].comp);
		h2.setFormantBase(0.01);
		vector<vector<double>> aOut(voices*ch, vector<double>(128));
		vector<vector<double>> bOut(voices*ch, vector<double>(128));
		vector<double*> ap(voices*ch), bp(voices*ch);
		double resetWorst = 0;
		for (int pos = 0; pos < 1024; pos += 128) {
			for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data() + pos;
			for (int x = 0; x < voices*ch; ++x) { ap[x] = aOut[x].data(); bp[x] = bOut[x].data(); }
			h.process(ip, 128, ap, 128);
			h2.process(ip, 128, bp, 128);
			for (int x = 0; x < voices*ch; ++x)
				for (int i = 0; i < 128; ++i)
					resetWorst = std::max(resetWorst, std::fabs(aOut[x][i] - bOut[x][i]));
		}
		t.check(resetWorst <= 1e-9);
	}
}

TASK_CASE("stream", stream_no_heap_during_audio) {
	// After configuration, process/seek/flush/reset must not touch the heap,
	// regardless of audio length.  All scaffolding is allocated before marking.
	const int ch = 2, voices = 4, B = 512, I = 128, total = 3072;
	SignalsmithHarmonizer<double> h(81);
	h.configure(ch, voices, B, I, true);
	auto cfg = defaultSettings<double>(voices);
	for (int v = 0; v < voices; ++v) h.setVoice(v, cfg[v].pitch, cfg[v].formant, cfg[v].comp);
	h.setFormantBase(0.01);

	Material<double> mat(ch, total);
	vector<vector<double>> out(voices*ch, vector<double>(128));
	vector<vector<double>> tail(voices*ch, vector<double>(B));
	vector<const double*> ip(ch);
	vector<double*> op(voices*ch), fp(voices*ch);
	for (int x = 0; x < voices*ch; ++x) { op[x] = out[x].data(); fp[x] = tail[x].data(); }

	long a0 = task_harm::heapNew, d0 = task_harm::heapDelete;
	for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data();
	h.seek(ip, 256, 1.0);
	t.check(task_harm::heapNew == a0 && task_harm::heapDelete == d0);

	int chunks[4] = {96, 128, 77, 128};
	int pos = 0, k = 0;
	while (pos < total) {
		int nIn = std::min(chunks[k % 4], total - pos);
		for (int c = 0; c < ch; ++c) ip[c] = mat.data[c].data() + pos;
		h.process(ip, nIn, op, 128);
		pos += nIn;
		++k;
	}
	t.check(task_harm::heapNew == a0 && task_harm::heapDelete == d0);

	h.flush(fp, B);
	t.check(task_harm::heapNew == a0 && task_harm::heapDelete == d0);

	h.reset();
	t.check(task_harm::heapNew == a0 && task_harm::heapDelete == d0);
}

} // anonymous namespace
