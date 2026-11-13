// Task tests for SignalsmithHarmonizer.
//
// Registered through task_support/registry.hpp in four groups:
//   equivalence / sharing / automation / stream
// This TU deliberately has no main() - task_support/main.cpp drives it.

// The analysis hook must be defined before any Linear STFT header is included.
#include <cstddef>
static long g_hookCurrent = 0, g_hookLookback = 0;
#define SIGNALSMITH_TASK_ANALYSIS(channel, samplesInPast) \
	do { if ((samplesInPast) == 0) { ++g_hookCurrent; } else { ++g_hookLookback; } } while (0)

#include "signalsmith-harmonizer.h"
#include "signalsmith-stretch.h"
#include "task_support/registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <new>

// ---------------------------------------------------------------------------
// Heap-allocation counters.  configure() may allocate; processing must not.
// ---------------------------------------------------------------------------
static std::size_t g_allocCount = 0, g_freeCount = 0;
void *operator new(std::size_t n) {
	void *p = std::malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	++g_allocCount;
	return p;
}
void *operator new[](std::size_t n) {
	void *p = std::malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	++g_allocCount;
	return p;
}
void operator delete(void *p) noexcept {
	if (p) { ++g_freeCount; std::free(p); }
}
void operator delete[](void *p) noexcept {
	if (p) { ++g_freeCount; std::free(p); }
}

namespace {

using namespace signalsmith::stretch;

struct Combo {
	double transpose, formant;
	bool compensate;
};
const Combo combos[] = {
	{1.0,  1.0,  false},
	{1.25, 1.0,  false},
	{0.8,  1.15, true},
	{2.0,  0.8,  false},
	{0.5,  1.25, true},
	{1.5,  0.9,  true},
	{1.0,  1.2,  false},
};
const int comboCount = int(sizeof(combos)/sizeof(combos[0]));

// Generated material: multi-tones, impulses, a long silent stretch, two
// distinct channels.  No external audio files.
template<class Sample>
Sample genSample(long n, int channel) {
	const double twoPi = 6.283185307179586;
	double s = 0.25*std::sin(twoPi*0.0217*n) + 0.15*std::sin(twoPi*0.00731*n + 1.3);
	if ((n % 503) == 0) s += 0.5;
	if (n >= 2048 && n < 4608) s = 0; // long silence (>= 2 * 1024-block)
	if (channel == 1) {
		s = -0.7*s + 0.12*std::cos(twoPi*0.0311*n);
		if (n >= 2048 && n < 4608) s = 0;
	}
	return Sample(s);
}

template<class Sample>
double toleranceFor();
template<> inline double toleranceFor<float>() { return 2e-5; }
template<> inline double toleranceFor<double>() { return 1e-9; }

// One reference voice.
template<class Sample>
struct RefVoice {
	SignalsmithStretch<Sample> stretch;
	explicit RefVoice(long seed) : stretch(seed) {}
};

template<class Sample>
struct PlanarPointers {
	std::vector<std::vector<Sample>> storage;
	std::vector<Sample*> pointers;
	PlanarPointers() {}
	PlanarPointers(int planes, int length) { resize(planes, length); }
	void resize(int planes, int length) {
		storage.resize(planes);
		pointers.resize(planes);
		for (int p = 0; p < planes; ++p) {
			storage[p].assign(length, Sample(0));
			pointers[p] = storage[p].data();
		}
	}
};

// Fills the first `length` samples of each plane with generated input.
template<class Sample>
void fillInput(PlanarPointers<Sample> &buf, int channels, int length, long startIndex) {
	buf.resize(channels, length);
	for (int c = 0; c < channels; ++c) {
		for (int i = 0; i < length; ++i) {
			buf.storage[c][i] = genSample<Sample>(startIndex + i, c);
		}
	}
}

struct DiffReport {
	double maxAbs = 0;
	long atIndex = -1;
	int atPlane = -1;
	long samples = 0;
};
template<class Sample>
DiffReport comparePlanes(const PlanarPointers<Sample> &a, const PlanarPointers<Sample> &b, int planes, int length) {
	DiffReport d;
	d.samples = long(planes)*long(length);
	for (int p = 0; p < planes; ++p) {
		for (int i = 0; i < length; ++i) {
			double diff = std::fabs(double(a.storage[p][i]) - double(b.storage[p][i]));
			if (diff > d.maxAbs) {
				d.maxAbs = diff;
				d.atIndex = i;
				d.atPlane = p;
			}
		}
	}
	return d;
}

template<class Sample>
void configureRefs(std::vector<RefVoice<Sample>*> &refs, int channels, int voices, int block, int interval,
		bool split, long seed, double formantBase) {
	for (int v = 0; v < voices; ++v) {
		auto *r = new RefVoice<Sample>(seed);
		r->stretch.configure(channels, block, interval, split);
		const Combo &cm = combos[v % comboCount];
		r->stretch.setTransposeFactor(Sample(cm.transpose));
		r->stretch.setFormantFactor(Sample(cm.formant), cm.compensate);
		r->stretch.setFormantBase(Sample(formantBase));
		refs.push_back(r);
	}
}
template<class Sample>
void configureHarmonizer(SignalsmithHarmonizer<Sample> &h, int channels, int voices, int block, int interval,
		bool split, long seed, double formantBase) {
	h.configure(channels, voices, block, interval, split);
	for (int v = 0; v < voices; ++v) {
		const Combo &cm = combos[v % comboCount];
		h.setVoice(v, Sample(cm.transpose), Sample(cm.formant), cm.compensate);
	}
	h.setFormantBase(Sample(formantBase));
}

// ---------------------------------------------------------------------------
// equivalence
// ---------------------------------------------------------------------------
template<class Sample>
void runStaticEquiv(task::Case &t, int channels, int voices, int block, int interval, bool split,
		int inChunk, int outChunk, int chunks) {
	const long seed = 123;
	const double formantBase = 0.01;
	const double tol = toleranceFor<Sample>();

	std::vector<RefVoice<Sample>*> refs;
	configureRefs(refs, channels, voices, block, interval, split, seed, formantBase);
	SignalsmithHarmonizer<Sample> h(seed);
	configureHarmonizer(h, channels, voices, block, interval, split, seed, formantBase);

	t.check(h.inputLatency() == refs[0]->stretch.inputLatency());
	t.check(h.outputLatency() == refs[0]->stretch.outputLatency());
	t.check(h.blockSamples() == block && h.intervalSamples() == interval);

	long inputCursor = 0;
	for (int chunk = 0; chunk < chunks; ++chunk) {
		PlanarPointers<Sample> in(channels, inChunk);
		fillInput(in, channels, inChunk, inputCursor);
		inputCursor += inChunk;

		PlanarPointers<Sample> hOut(voices*channels, outChunk);
		h.process(in.pointers, inChunk, hOut.pointers, outChunk);

		for (int v = 0; v < voices; ++v) {
			PlanarPointers<Sample> rOut(channels, outChunk);
			refs[v]->stretch.process(in.pointers, inChunk, rOut.pointers, outChunk);
			for (int c = 0; c < channels; ++c) {
				DiffReport d;
				d.samples = outChunk;
				for (int i = 0; i < outChunk; ++i) {
					double diff = std::fabs(double(hOut.storage[v*channels + c][i]) - double(rOut.storage[c][i]));
					if (diff > d.maxAbs) { d.maxAbs = diff; d.atIndex = i; d.atPlane = c; }
					t.check(std::isfinite(double(hOut.storage[v*channels + c][i])));
				}
				if (!(d.maxAbs <= tol)) {
					std::printf("EQUIV mismatch type=%s ch=%d voices=%d block=%d split=%d v=%d c=%d chunk=%d max=%.3g at=%ld\n",
						sizeof(Sample) == 4 ? "f" : "d", channels, voices, block, int(split), v, c, chunk, d.maxAbs, d.atIndex);
				}
				t.check(d.maxAbs <= tol);
			}
		}
	}
	for (auto *r : refs) delete r;
}

TASK_CASE("equivalence", equivalence_static) {
	// Covers float/double, 1/2 channels, 1/2/4 voices, both window/interval
	// combinations, both split modes, and several time-stretch chunk ratios.
	runStaticEquiv<float>(t, 2, 4, 512, 128, false, 128, 128, 30);
	runStaticEquiv<float>(t, 2, 4, 512, 128, true, 128, 128, 30);
	runStaticEquiv<double>(t, 1, 2, 512, 128, false, 96, 128, 40);
	runStaticEquiv<double>(t, 2, 1, 512, 128, true, 128, 96, 40);
	runStaticEquiv<float>(t, 1, 4, 1024, 256, false, 256, 256, 24);
	runStaticEquiv<double>(t, 2, 2, 1024, 256, true, 192, 256, 24);
	runStaticEquiv<double>(t, 1, 1, 1024, 256, false, 256, 192, 24);
	runStaticEquiv<float>(t, 2, 4, 512, 128, false, 170, 128, 24);
	runStaticEquiv<double>(t, 2, 2, 512, 128, true, 96, 128, 40);
	// Time-stretch at the supported 0.75 and 1.5 ratio bounds.
	runStaticEquiv<double>(t, 2, 4, 512, 128, false, 96, 128, 40);  // 0.75
	runStaticEquiv<float>(t, 1, 2, 512, 128, true, 192, 128, 24);   // 1.5
	runStaticEquiv<double>(t, 2, 2, 1024, 256, false, 384, 256, 18); // 1.5
}

// Finite sound: trailing inputLatency() of zeros, then flush() with no more
// than blockSamples of tail, exactly as the single-voice API is driven.
template<class Sample>
void runFiniteEquiv(task::Case &t, bool split) {
	const int channels = 2, voices = 3, block = 512, interval = 128;
	const long seed = 991;
	const double tol = toleranceFor<Sample>();

	std::vector<RefVoice<Sample>*> refs;
	configureRefs(refs, channels, voices, block, interval, split, seed, 0.02);
	SignalsmithHarmonizer<Sample> h(seed);
	configureHarmonizer(h, channels, voices, block, interval, split, seed, 0.02);

	const int soundSamples = 2400;
	long cursor = 0;
	while (cursor < soundSamples) {
		int n = std::min<int>(177, soundSamples - cursor);
		PlanarPointers<Sample> in(channels, n);
		fillInput(in, channels, n, cursor);
		cursor += n;
		PlanarPointers<Sample> hOut(voices*channels, n);
		h.process(in.pointers, n, hOut.pointers, n);
		for (int v = 0; v < voices; ++v) {
			PlanarPointers<Sample> rOut(channels, n);
			refs[v]->stretch.process(in.pointers, n, rOut.pointers, n);
			for (int c = 0; c < channels; ++c) {
				double maxDiff = 0;
				for (int i = 0; i < n; ++i) {
					maxDiff = std::max(maxDiff, std::fabs(double(hOut.storage[v*channels + c][i]) - double(rOut.storage[c][i])));
				}
				t.check(maxDiff <= tol);
			}
		}
	}

	// Tail of silence, one inputLatency() worth, in arbitrary small chunks.
	int padding = 0;
	while (padding < h.inputLatency()) {
		int n = std::min<int>(53, h.inputLatency() - padding);
		PlanarPointers<Sample> in(channels, n); // zero-filled
		PlanarPointers<Sample> hOut(voices*channels, n);
		h.process(in.pointers, n, hOut.pointers, n);
		for (int v = 0; v < voices; ++v) {
			PlanarPointers<Sample> rOut(channels, n);
			refs[v]->stretch.process(in.pointers, n, rOut.pointers, n);
			for (int c = 0; c < channels; ++c) {
				double maxDiff = 0;
				for (int i = 0; i < n; ++i) {
					maxDiff = std::max(maxDiff, std::fabs(double(hOut.storage[v*channels + c][i]) - double(rOut.storage[c][i])));
				}
				t.check(maxDiff <= tol);
			}
		}
		padding += n;
	}

	// flush(): read blockSamples of tail, compare everything including folding.
	const int flushSamples = block;
	PlanarPointers<Sample> hFlush(voices*channels, flushSamples);
	h.flush(hFlush.pointers, flushSamples);
	for (int v = 0; v < voices; ++v) {
		PlanarPointers<Sample> rFlush(channels, flushSamples);
		refs[v]->stretch.flush(rFlush.pointers, flushSamples);
		for (int c = 0; c < channels; ++c) {
			double maxDiff = 0;
			for (int i = 0; i < flushSamples; ++i) {
				t.check(std::isfinite(double(hFlush.storage[v*channels + c][i])));
				maxDiff = std::max(maxDiff, std::fabs(double(hFlush.storage[v*channels + c][i]) - double(rFlush.storage[c][i])));
			}
			t.check(maxDiff <= tol);
		}
	}
	for (auto *r : refs) delete r;
}

TASK_CASE("equivalence", equivalence_finite_flush) {
	runFiniteEquiv<float>(t, false);
	runFiniteEquiv<double>(t, true);
}

// ---------------------------------------------------------------------------
// sharing
// ---------------------------------------------------------------------------
struct HookCounts {
	long current = 0, lookback = 0;
};
template<class Sample>
HookCounts runHarmonizerWithHooks(int voices, bool split, int chunks) {
	const int channels = 2, block = 512, interval = 128;
	SignalsmithHarmonizer<Sample> h(4321);
	configureHarmonizer(h, channels, voices, block, interval, split, 4321, 0.015);

	long beforeC = g_hookCurrent, beforeL = g_hookLookback;

	// seek() forces a re-analysis of the previous position (non-zero lookback path).
	PlanarPointers<Sample> preroll(channels, block + interval);
	fillInput(preroll, channels, block + interval, -1000);
	h.seek(preroll.pointers, block + interval, 1.0);

	long cursor = 0;
	for (int k = 0; k < chunks; ++k) {
		PlanarPointers<Sample> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		PlanarPointers<Sample> out(voices*channels, interval);
		h.process(in.pointers, interval, out.pointers, interval);
	}
	HookCounts result;
	result.current = g_hookCurrent - beforeC;
	result.lookback = g_hookLookback - beforeL;
	return result;
}

template<class Sample>
HookCounts runReferencesWithHooks(int voices, bool split, int chunks) {
	const int channels = 2, block = 512, interval = 128;
	std::vector<RefVoice<Sample>*> refs;
	configureRefs(refs, channels, voices, block, interval, split, 4321, 0.015);

	long beforeC = g_hookCurrent, beforeL = g_hookLookback;
	PlanarPointers<Sample> preroll(channels, block + interval);
	fillInput(preroll, channels, block + interval, -1000);
	for (auto *r : refs) r->stretch.seek(preroll.pointers, block + interval, 1.0);

	long cursor = 0;
	for (int k = 0; k < chunks; ++k) {
		PlanarPointers<Sample> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		for (auto *r : refs) {
			PlanarPointers<Sample> out(channels, interval);
			r->stretch.process(in.pointers, interval, out.pointers, interval);
		}
	}
	HookCounts result;
	result.current = g_hookCurrent - beforeC;
	result.lookback = g_hookLookback - beforeL;
	for (auto *r : refs) delete r;
	return result;
}

TASK_CASE("sharing", sharing_analysis_counts) {
	const int chunks = 12;
	for (int split = 0; split < 2; ++split) {
		HookCounts h1 = runHarmonizerWithHooks<double>(1, bool(split), chunks);
		HookCounts h2 = runHarmonizerWithHooks<double>(2, bool(split), chunks);
		HookCounts h4 = runHarmonizerWithHooks<double>(4, bool(split), chunks);

		// Both paths must actually be exercised.
		t.check(h1.current > 0 && h2.current > 0 && h4.current > 0);
		t.check(h1.lookback > 0 && h2.lookback > 0 && h4.lookback > 0);

		// Measured analysis work is independent of the voice count.
		t.check(h1.current == h2.current && h2.current == h4.current);
		t.check(h1.lookback == h2.lookback && h2.lookback == h4.lookback);

		// Independent single-voice instances do the work per voice: measured,
		// not derived from the voice count.
		HookCounts r1 = runReferencesWithHooks<double>(1, bool(split), chunks);
		HookCounts r2 = runReferencesWithHooks<double>(2, bool(split), chunks);
		HookCounts r4 = runReferencesWithHooks<double>(4, bool(split), chunks);
		t.check(r1.current == h1.current && r1.lookback == h1.lookback);
		t.check(r2.current == 2*h1.current && r2.lookback == 2*h1.lookback);
		t.check(r4.current == 4*h1.current && r4.lookback == 4*h1.lookback);
	}
}

TASK_CASE("sharing", sharing_voice_isolation) {
	// Changing one voice must not touch the others, and voices must not behave
	// like extra channels of one stretcher.
	const int channels = 2, voices = 4, block = 512, interval = 128;
	const double tol = toleranceFor<double>();

	SignalsmithHarmonizer<double> a(77), b(77);
	configureHarmonizer(a, channels, voices, block, interval, true, 77, 0.01);
	configureHarmonizer(b, channels, voices, block, interval, true, 77, 0.01);

	// B gets a different setting on voice 1 (and 3) between process calls.
	long cursor = 0;
	for (int k = 0; k < 16; ++k) {
		PlanarPointers<double> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		PlanarPointers<double> outA(voices*channels, interval), outB(voices*channels, interval);
		if (k == 5) {
			b.setVoice(1, 1.5, 1.1, true);
			b.setVoice(3, 0.6, 0.9, false);
		}
		a.process(in.pointers, interval, outA.pointers, interval);
		b.process(in.pointers, interval, outB.pointers, interval);

		for (int v = 0; v < voices; ++v) {
			if (v == 1 || v == 3) continue;
			for (int c = 0; c < channels; ++c) {
				double maxDiff = 0;
				for (int i = 0; i < interval; ++i) {
					maxDiff = std::max(maxDiff, std::fabs(outA.storage[v*channels + c][i] - outB.storage[v*channels + c][i]));
				}
				t.check(maxDiff <= tol);
			}
		}
	}

	// The changed voices must actually differ from the untouched harmonizer,
	// and the planar layout keeps each voice/channel pair independent.
	bool changedDiffers = false;
	// Re-run a short segment with static settings for a direct reference check.
	SignalsmithStretch<double> ref(77);
	ref.configure(channels, block, interval, true);
	ref.setTransposeFactor(1.5);
	ref.setFormantFactor(1.1, true);
	ref.setFormantBase(0.01);
	SignalsmithHarmonizer<double> h(77);
	h.configure(channels, 2, block, interval, true);
	h.setVoice(0, 1.0, 1.0, false);
	h.setVoice(1, 1.5, 1.1, true);
	h.setFormantBase(0.01);
	cursor = 0;
	double maxVoice1 = 0;
	for (int k = 0; k < 16; ++k) {
		PlanarPointers<double> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		PlanarPointers<double> out(2*channels, interval);
		h.process(in.pointers, interval, out.pointers, interval);
		PlanarPointers<double> rOut(channels, interval);
		ref.process(in.pointers, interval, rOut.pointers, interval);
		for (int c = 0; c < channels; ++c) {
			for (int i = 0; i < interval; ++i) {
				maxVoice1 = std::max(maxVoice1, std::fabs(out.storage[channels + c][i] - rOut.storage[c][i]));
				if (std::fabs(out.storage[c][i] - out.storage[channels + c][i]) > 1e-12) changedDiffers = true;
			}
		}
	}
	t.check(maxVoice1 <= toleranceFor<double>());
	t.check(changedDiffers);
}

// ---------------------------------------------------------------------------
// automation
// ---------------------------------------------------------------------------
TASK_CASE("automation", automation_latch) {
	// Split mode, half-hop process calls.  Settings change between calls that
	// land in the middle of a synthesis hop.
	const int channels = 2, voices = 2, block = 512, interval = 128;
	const int half = interval/2;
	const double tol = toleranceFor<double>();

	SignalsmithHarmonizer<double> hFixed(55), hAuto(55);
	configureHarmonizer(hFixed, channels, voices, block, interval, true, 55, 0.01);
	configureHarmonizer(hAuto, channels, voices, block, interval, true, 55, 0.01);

	// References use the original latching semantics: parameters are read at
	// the start of each block, so changing them between calls is equivalent.
	std::vector<RefVoice<double>*> refOld, refNew;
	configureRefs(refOld, channels, voices, block, interval, true, 55, 0.01);
	configureRefs(refNew, channels, voices, block, interval, true, 55, 0.01);

	long cursor = 0;
	long outputCursor = 0;
	bool applied = false;
	for (int k = 0; k < 32; ++k) {
		PlanarPointers<double> in(channels, half);
		fillInput(in, channels, half, cursor);
		cursor += half;

		// k == 7 means the next output sample (7*64 = 448) is mid-hop
		// (hops start every 128 samples).
		if (k == 7) {
			t.check(outputCursor % interval != 0);
			hAuto.setVoice(0, 1.4, 1.12, true);
			hAuto.setVoice(1, 0.7, 0.85, false);
			hAuto.setFormantBase(0.03);
			refNew[0]->stretch.setTransposeFactor(1.4);
			refNew[0]->stretch.setFormantFactor(1.12, true);
			refNew[0]->stretch.setFormantBase(0.03);
			refNew[1]->stretch.setTransposeFactor(0.7);
			refNew[1]->stretch.setFormantFactor(0.85, false);
			refNew[1]->stretch.setFormantBase(0.03);
			applied = true;
		}

		PlanarPointers<double> fixedOut(voices*channels, half), autoOut(voices*channels, half);
		hFixed.process(in.pointers, half, fixedOut.pointers, half);
		hAuto.process(in.pointers, half, autoOut.pointers, half);

		for (int v = 0; v < voices; ++v) {
			PlanarPointers<double> oldOut(channels, half), newOut(channels, half);
			refOld[v]->stretch.process(in.pointers, half, oldOut.pointers, half);
			refNew[v]->stretch.process(in.pointers, half, newOut.pointers, half);
			for (int c = 0; c < channels; ++c) {
				for (int i = 0; i < half; ++i) {
					long globalIndex = outputCursor + i;
					double hv = autoOut.storage[v*channels + c][i];
					if (globalIndex < 512) {
						// Samples belonging to the hop in progress when the
						// change was made keep the old settings end-to-end.
						t.check(std::fabs(hv - fixedOut.storage[v*channels + c][i]) <= tol);
						t.check(std::fabs(hv - oldOut.storage[c][i]) <= tol);
					} else {
						t.check(std::fabs(hv - newOut.storage[c][i]) <= tol);
					}
				}
			}
		}
		outputCursor += half;
	}
	t.check(applied);

	// Rapid repeated changes before the next hop: the last one wins.
	SignalsmithHarmonizer<double> hLast(55);
	configureHarmonizer(hLast, channels, 1, block, interval, true, 55, 0.01);
	SignalsmithStretch<double> refLast(55);
	refLast.configure(channels, block, interval, true);
	refLast.setTransposeFactor(combos[0].transpose);
	refLast.setFormantFactor(combos[0].formant, combos[0].compensate);
	refLast.setFormantBase(0.01);
	cursor = 0;
	for (int k = 0; k < 16; ++k) {
		PlanarPointers<double> in(channels, half);
		fillInput(in, channels, half, cursor);
		cursor += half;
		if (k == 3) {
			hLast.setVoice(0, 1.9, 1.0, false);
			hLast.setVoice(0, 0.55, 1.25, true);
			hLast.setVoice(0, 1.3, 0.9, false); // last write before the next hop start
			refLast.setTransposeFactor(1.3);
			refLast.setFormantFactor(0.9, false);
		}
		PlanarPointers<double> out(channels, half), rOut(channels, half);
		hLast.process(in.pointers, half, out.pointers, half);
		refLast.process(in.pointers, half, rOut.pointers, half);
		for (int c = 0; c < channels; ++c) {
			double maxDiff = 0;
			for (int i = 0; i < half; ++i) {
				maxDiff = std::max(maxDiff, std::fabs(out.storage[c][i] - rOut.storage[c][i]));
			}
			t.check(maxDiff <= tol);
		}
	}
	for (auto *r : refOld) delete r;
	for (auto *r : refNew) delete r;
}

TASK_CASE("automation", automation_voice_isolation) {
	// Changing a voice keeps its own phase/tail and leaves the others intact.
	const int channels = 1, voices = 3, block = 512, interval = 128;
	const double tol = toleranceFor<double>();

	SignalsmithHarmonizer<double> a(91), b(91);
	configureHarmonizer(a, channels, voices, block, interval, false, 91, 0.0);
	configureHarmonizer(b, channels, voices, block, interval, false, 91, 0.0);

	long cursor = 0;
	for (int k = 0; k < 20; ++k) {
		PlanarPointers<double> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		if (k == 8) b.setVoice(2, 1.6, 1.1, true);
		PlanarPointers<double> outA(voices, interval), outB(voices, interval);
		a.process(in.pointers, interval, outA.pointers, interval);
		b.process(in.pointers, interval, outB.pointers, interval);
		for (int v = 0; v < voices; ++v) {
			if (v == 2) continue;
			double maxDiff = 0;
			for (int i = 0; i < interval; ++i) {
				maxDiff = std::max(maxDiff, std::fabs(outA.storage[v][i] - outB.storage[v][i]));
			}
			t.check(maxDiff <= tol);
		}
	}

	// The changed voice still matches a reference stretcher switched at the
	// same point: no reset of its phase history happened.
	SignalsmithStretch<double> ref(91);
	ref.configure(channels, block, interval, false);
	ref.setTransposeFactor(combos[2 % comboCount].transpose);
	ref.setFormantFactor(combos[2 % comboCount].formant, combos[2 % comboCount].compensate);
	SignalsmithHarmonizer<double> h(91);
	h.configure(channels, 1, block, interval, false);
	h.setVoice(0, combos[2 % comboCount].transpose, combos[2 % comboCount].formant, combos[2 % comboCount].compensate);
	cursor = 0;
	double maxDiff = 0;
	for (int k = 0; k < 20; ++k) {
		PlanarPointers<double> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		if (k == 8) {
			h.setVoice(0, 1.6, 1.1, true);
			ref.setTransposeFactor(1.6);
			ref.setFormantFactor(1.1, true);
		}
		PlanarPointers<double> out(1, interval), rOut(1, interval);
		h.process(in.pointers, interval, out.pointers, interval);
		ref.process(in.pointers, interval, rOut.pointers, interval);
		for (int i = 0; i < interval; ++i) {
			maxDiff = std::max(maxDiff, std::fabs(out.storage[0][i] - rOut.storage[0][i]));
		}
	}
	t.check(maxDiff <= tol);
}

// ---------------------------------------------------------------------------
// stream
// ---------------------------------------------------------------------------
TASK_CASE("stream", stream_lifecycle) {
	const int channels = 2, voices = 2, block = 512, interval = 128;
	const double tol = toleranceFor<double>();

	SignalsmithHarmonizer<double> h(314);
	configureHarmonizer(h, channels, voices, block, interval, true, 314, 0.012);
	std::vector<RefVoice<double>*> refs;
	configureRefs(refs, channels, voices, block, interval, true, 314, 0.012);

	auto checkAgainstRefs = [&](PlanarPointers<double> &hOut, int n) {
		for (int v = 0; v < voices; ++v) {
			for (int c = 0; c < channels; ++c) {
				for (int i = 0; i < n; ++i) {
					t.check(std::isfinite(hOut.storage[v*channels + c][i]));
				}
			}
		}
	};

	// Arbitrary, mismatched chunk sizes (every non-zero ratio stays within the
	// supported 0.75..1.5 time-stretch range), including zero-input calls.
	const int inSizes[] = {96, 0, 128, 170, 110, 96, 150, 128};
	const int outSizes[] = {128, 128, 96, 128, 128, 110, 128, 96};
	long cursor = 0;
	for (int k = 0; k < 24; ++k) {
		int inN = inSizes[k % 8], outN = outSizes[k % 8];
		PlanarPointers<double> in(channels, inN);
		fillInput(in, channels, inN, cursor);
		cursor += inN;
		PlanarPointers<double> hOut(voices*channels, outN);
		h.process(in.pointers, inN, hOut.pointers, outN);
		for (int v = 0; v < voices; ++v) {
			PlanarPointers<double> rOut(channels, outN);
			refs[v]->stretch.process(in.pointers, inN, rOut.pointers, outN);
			for (int c = 0; c < channels; ++c) {
				double maxDiff = 0;
				for (int i = 0; i < outN; ++i) {
					maxDiff = std::max(maxDiff, std::fabs(hOut.storage[v*channels + c][i] - rOut.storage[c][i]));
				}
				t.check(maxDiff <= tol);
			}
		}
		checkAgainstRefs(hOut, outN);
	}

	// seek() mid-stream: shared input advances once, every voice matches its ref.
	PlanarPointers<double> preroll(channels, block + interval);
	fillInput(preroll, channels, block + interval, 5000);
	h.seek(preroll.pointers, block + interval, 1.2);
	for (auto *r : refs) r->stretch.seek(preroll.pointers, block + interval, 1.2);
	cursor = 0;
	for (int k = 0; k < 8; ++k) {
		PlanarPointers<double> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		PlanarPointers<double> hOut(voices*channels, interval);
		h.process(in.pointers, interval, hOut.pointers, interval);
		for (int v = 0; v < voices; ++v) {
			PlanarPointers<double> rOut(channels, interval);
			refs[v]->stretch.process(in.pointers, interval, rOut.pointers, interval);
			for (int c = 0; c < channels; ++c) {
				double maxDiff = 0;
				for (int i = 0; i < interval; ++i) {
					maxDiff = std::max(maxDiff, std::fabs(hOut.storage[v*channels + c][i] - rOut.storage[c][i]));
				}
				t.check(maxDiff <= tol);
			}
		}
	}

	// flush() then keep going, like the single-voice API.
	{
		PlanarPointers<double> hFlush(voices*channels, block);
		h.flush(hFlush.pointers, block);
		for (int v = 0; v < voices; ++v) {
			PlanarPointers<double> rFlush(channels, block);
			refs[v]->stretch.flush(rFlush.pointers, block);
			for (int c = 0; c < channels; ++c) {
				double maxDiff = 0;
				for (int i = 0; i < block; ++i) {
					maxDiff = std::max(maxDiff, std::fabs(hFlush.storage[v*channels + c][i] - rFlush.storage[c][i]));
				}
				t.check(maxDiff <= tol);
			}
		}
	}
	cursor = 9000;
	for (int k = 0; k < 6; ++k) {
		PlanarPointers<double> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		PlanarPointers<double> hOut(voices*channels, interval);
		h.process(in.pointers, interval, hOut.pointers, interval);
		for (int v = 0; v < voices; ++v) {
			PlanarPointers<double> rOut(channels, interval);
			refs[v]->stretch.process(in.pointers, interval, rOut.pointers, interval);
			for (int c = 0; c < channels; ++c) {
				double maxDiff = 0;
				for (int i = 0; i < interval; ++i) {
					maxDiff = std::max(maxDiff, std::fabs(hOut.storage[v*channels + c][i] - rOut.storage[c][i]));
				}
				t.check(maxDiff <= tol);
			}
		}
	}

	// reset() clears history but keeps configuration and settings.
	h.reset();
	std::vector<RefVoice<double>*> fresh;
	configureRefs(fresh, channels, voices, block, interval, true, 314, 0.012);
	for (auto *r : fresh) r->stretch.reset();
	cursor = 0;
	for (int k = 0; k < 10; ++k) {
		PlanarPointers<double> in(channels, interval);
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		PlanarPointers<double> hOut(voices*channels, interval);
		h.process(in.pointers, interval, hOut.pointers, interval);
		for (int v = 0; v < voices; ++v) {
			PlanarPointers<double> rOut(channels, interval);
			fresh[v]->stretch.process(in.pointers, interval, rOut.pointers, interval);
			for (int c = 0; c < channels; ++c) {
				double maxDiff = 0;
				for (int i = 0; i < interval; ++i) {
					maxDiff = std::max(maxDiff, std::fabs(hOut.storage[v*channels + c][i] - rOut.storage[c][i]));
				}
				t.check(maxDiff <= tol);
			}
		}
	}

	// Silence recovery: the generated material contains a long silent stretch.
	SignalsmithHarmonizer<double> h2(314);
	configureHarmonizer(h2, 1, 2, block, interval, false, 314, 0.0);
	SignalsmithStretch<double> r0(314), r1(314);
	r0.configure(1, block, interval, false);
	r1.configure(1, block, interval, false);
	r0.setTransposeFactor(combos[0].transpose);
	r1.setTransposeFactor(combos[1].transpose);
	cursor = 0;
	for (int k = 0; k < 40; ++k) {
		PlanarPointers<double> in(1, interval);
		fillInput(in, 1, interval, cursor);
		cursor += interval;
		PlanarPointers<double> out(2, interval);
		h2.process(in.pointers, interval, out.pointers, interval);
		PlanarPointers<double> o0(1, interval), o1(1, interval);
		r0.process(in.pointers, interval, o0.pointers, interval);
		r1.process(in.pointers, interval, o1.pointers, interval);
		double d0 = 0, d1 = 0;
		for (int i = 0; i < interval; ++i) {
			d0 = std::max(d0, std::fabs(out.storage[0][i] - o0.storage[0][i]));
			d1 = std::max(d1, std::fabs(out.storage[1][i] - o1.storage[0][i]));
		}
		t.check(d0 <= tol && d1 <= tol);
	}

	for (auto *r : refs) delete r;
	for (auto *r : fresh) delete r;
}

TASK_CASE("stream", stream_memory) {
	// After configure(), none of process/seek/flush/reset may touch the heap.
	const int channels = 2, voices = 4, block = 512, interval = 128;
	SignalsmithHarmonizer<double> h(2718);
	h.configure(channels, voices, block, interval, true);
	for (int v = 0; v < voices; ++v) {
		h.setVoice(v, double(combos[v % comboCount].transpose), double(combos[v % comboCount].formant),
			combos[v % comboCount].compensate);
	}
	h.setFormantBase(0.01);

	// All test-side buffers exist (at their final sizes) before the snapshot,
	// so any measured allocation can only come from the harmonizer itself.
	PlanarPointers<double> in(channels, interval);
	PlanarPointers<double> smallIn(channels, 96);
	PlanarPointers<double> out(voices*channels, interval);
	PlanarPointers<double> preroll(channels, block + interval);
	PlanarPointers<double> flushOut(voices*channels, block);

	// Warm up so every code path has been touched before the snapshot.
	fillInput(in, channels, interval, 0);
	h.process(in.pointers, interval, out.pointers, interval);
	g_allocCount = g_freeCount = 0;

	long cursor = interval;
	for (int k = 0; k < 12; ++k) {
		bool small = (k % 3 == 0);
		int inN = small ? 96 : interval;
		if (small) {
			fillInput(smallIn, channels, inN, cursor);
			h.process(smallIn.pointers, inN, out.pointers, interval);
		} else {
			fillInput(in, channels, inN, cursor);
			h.process(in.pointers, inN, out.pointers, interval);
		}
		cursor += inN;
	}
	fillInput(preroll, channels, block + interval, 7000);
	h.seek(preroll.pointers, block + interval, 0.9);
	for (int k = 0; k < 4; ++k) {
		fillInput(in, channels, interval, cursor);
		cursor += interval;
		h.process(in.pointers, interval, out.pointers, interval);
	}
	h.flush(flushOut.pointers, block);
	h.reset();
	fillInput(in, channels, interval, 0);
	h.process(in.pointers, interval, out.pointers, interval);

	t.check(g_allocCount == 0);
	t.check(g_freeCount == 0);
}

} // namespace
