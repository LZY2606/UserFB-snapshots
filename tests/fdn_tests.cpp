// Regression tests for signalsmith::feedback::Reverb4 - see tests/run_fdn.sh
#include "include/signalsmith-dsp/feedback.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

// ---------------------------------------------------------------- heap traffic counting
static long g_allocCount = 0;
static long g_freeCount = 0;
void *operator new(std::size_t size) {
	++g_allocCount;
	void *p = std::malloc(size ? size : 1);
	if (!p) throw std::bad_alloc();
	return p;
}
void *operator new[](std::size_t size) {
	++g_allocCount;
	void *p = std::malloc(size ? size : 1);
	if (!p) throw std::bad_alloc();
	return p;
}
void operator delete(void *p) noexcept {
	if (p) ++g_freeCount;
	std::free(p);
}
void operator delete[](void *p) noexcept {
	if (p) ++g_freeCount;
	std::free(p);
}

// ---------------------------------------------------------------- test state
static int g_failures = 0;
static double g_maxErrDouble = 0;   // Reverb4<double> vs reference
static double g_maxErrFloat = 0;    // Reverb4<float> vs reference
static double g_maxEnergyErr = 0;   // storedEnergy() vs reference, relative
static double g_maxFreezeDrift = 0; // relative stored-energy drift while frozen
static long g_countedAllocs = -1;   // heap traffic during process/parameter calls
static long g_countedFrees = -1;

static bool check(bool ok, const char *message) {
	if (!ok) {
		++g_failures;
		std::printf("FAIL: %s\n", message);
	}
	return ok;
}

// ---------------------------------------------------------------- reference
// Independent reference model: four short ring arrays (exact length, no
// capacity rounding) driven by a plain per-sample recursion.
struct RefReverb4 {
	double sampleRate = 0, t60 = 0;
	int delays[4] = {1, 1, 1, 1};
	double gains[4] = {0, 0, 0, 0};
	std::vector<double> lines[4];
	int pos[4] = {0, 0, 0, 0};
	bool frozen = false;

	void updateGains() {
		for (int c = 0; c < 4; ++c) {
			gains[c] = std::pow(10.0, -3.0*delays[c]/(sampleRate*t60));
		}
	}
	void prepare(double sr, const int d[4], double t) {
		sampleRate = sr;
		t60 = t;
		for (int c = 0; c < 4; ++c) {
			delays[c] = d[c];
			lines[c].assign(d[c], 0.0);
			pos[c] = 0;
		}
		updateGains();
		frozen = false;
	}
	void setDecay(double t) {
		t60 = t;
		updateGains();
	}
	void reset() {
		for (int c = 0; c < 4; ++c) {
			std::fill(lines[c].begin(), lines[c].end(), 0.0);
			pos[c] = 0;
		}
		frozen = false;
	}
	void frame(const double in[4], double out[4]) {
		double d[4];
		for (int c = 0; c < 4; ++c) d[c] = lines[c][pos[c]];
		for (int c = 0; c < 4; ++c) out[c] = d[c];
		double s = -0.5*(d[0] + d[1] + d[2] + d[3]); // 4x4 Householder
		double h[4];
		for (int c = 0; c < 4; ++c) h[c] = d[c] + s;
		for (int c = 0; c < 4; ++c) {
			lines[c][pos[c]] = frozen ? h[c] : in[c] + gains[c]*h[c];
		}
		for (int c = 0; c < 4; ++c) pos[c] = (pos[c] + 1) % delays[c];
	}
	double energy() const {
		double e = 0;
		for (int c = 0; c < 4; ++c) {
			for (int k = 0; k < delays[c]; ++k) e += lines[c][k]*lines[c][k];
		}
		return e;
	}
};

// Deterministic pseudo-random values in [-1, 1)
struct Lcg {
	uint64_t state;
	explicit Lcg(uint64_t seed) : state(seed) {}
	double next() {
		state = state*6364136223846793005ULL + 1442695040888963407ULL;
		return double(state >> 33)/double(1u << 31) - 1.0;
	}
};

// Runs a Reverb4<double> and the reference in lockstep, comparing every frame.
struct DoubleRun {
	signalsmith::feedback::Reverb4<double> rev;
	RefReverb4 ref;

	void prepare(double sr, std::array<int, 4> delays, double t60) {
		check(rev.prepare(sr, delays, t60), "prepare(valid) returned false");
		int d[4] = {delays[0], delays[1], delays[2], delays[3]};
		ref.prepare(sr, d, t60);
	}
	// Processes `frames` interleaved frames in blocks, comparing per-frame
	// against the reference.  Optionally queries storedEnergy() between
	// blocks (which must not affect the output).
	std::vector<double> run(const double *in, std::size_t frames,
			const std::size_t *blocks, std::size_t nBlocks, bool queryEnergy=false) {
		std::vector<double> out(frames*4);
		std::size_t done = 0, b = 0;
		double maxErr = 0;
		while (done < frames) {
			std::size_t n = blocks[b++ % nBlocks];
			if (n > frames - done) n = frames - done;
			rev.process(in + done*4, out.data() + done*4, n);
			if (queryEnergy) {
				double e = rev.storedEnergy();
				check(e == e, "storedEnergy() returned NaN");
			}
			for (std::size_t i = 0; i < n; ++i) {
				double ro[4];
				ref.frame(in + (done + i)*4, ro);
				for (int c = 0; c < 4; ++c) {
					double err = std::fabs(out[(done + i)*4 + c] - ro[c]);
					if (err > maxErr) maxErr = err;
				}
			}
			done += n;
		}
		if (maxErr > g_maxErrDouble) g_maxErrDouble = maxErr;
		check(maxErr < 1e-12, "double output mismatch vs reference");
		double e0 = ref.energy(), e1 = rev.storedEnergy();
		double rel = std::fabs(e1 - e0)/(std::fabs(e0) + 1e-300);
		if (rel > g_maxEnergyErr) g_maxEnergyErr = rel;
		check(rel < 1e-9, "storedEnergy() mismatch vs reference");
		return out;
	}
};

// ---------------------------------------------------------------- scenarios

// Unequal delays {1,2,3,5}: impulse recursion, block-size invariance
static void scenarioImpulseEchoes() {
	std::printf("scenario: impulse echoes with delays {1,2,3,5}\n");
	const std::size_t frames = 200;
	std::vector<double> in(frames*4, 0.0);
	in[0] = 1.0; // single impulse on channel 0

	std::array<int, 4> delays {{1, 2, 3, 5}};
	DoubleRun a, b, c;
	a.prepare(48000, delays, 1.0);
	b.prepare(48000, delays, 1.0);
	c.prepare(48000, delays, 1.0);

	const std::size_t frameByFrame[] = {1};
	const std::size_t irregular[] = {7, 1, 13, 64, 3};
	const std::size_t chunked[] = {64, 1, 135};
	std::vector<double> outA = a.run(in.data(), frames, frameByFrame, 1);
	std::vector<double> outB = b.run(in.data(), frames, irregular, 5);
	std::vector<double> outC = c.run(in.data(), frames, chunked, 3, true);

	check(outA == outB, "frame-by-frame and irregular blocks must agree exactly");
	check(outA == outC, "storedEnergy() queries must not affect the output");

	int first[4] = {-1, -1, -1, -1};
	for (std::size_t i = 0; i < frames; ++i) {
		for (int ch = 0; ch < 4; ++ch) {
			if (first[ch] < 0 && outA[i*4 + ch] != 0.0) first[ch] = (int)i;
		}
	}
	check(first[0] == 1, "channel 0 (delay 1) first echo should be frame 1");
	check(first[1] == 3, "channel 1 (delay 2) first echo should be frame 3");
	check(first[2] == 4, "channel 2 (delay 3) first echo should be frame 4");
	check(first[3] == 6, "channel 3 (delay 5) first echo should be frame 6");
	check(outA[1*4 + 0] == 1.0, "channel 0 first echo should be the impulse itself");
}

// Repeated delays, live t60 changes, freeze with non-zero input, unfreeze, reset
static void scenarioRandomOps() {
	std::printf("scenario: repeated delays {4,4,7,7}, live t60/freeze/reset\n");
	const std::size_t frames = 1600;
	Lcg lcg(12345);
	std::vector<double> in(frames*4);
	for (std::size_t i = 0; i < in.size(); ++i) in[i] = lcg.next();
	const std::size_t blocks[] = {1, 5, 64, 3, 128, 1};

	DoubleRun run;
	run.prepare(48000, {{4, 4, 7, 7}}, 1.2);
	std::size_t at = 0;
	auto step = [&](std::size_t n) {
		run.run(in.data() + at*4, n, blocks, 6);
		at += n;
	};
	step(300);
	check(run.rev.setDecay(0.7), "setDecay(0.7) mid-run");
	run.ref.setDecay(0.7);
	step(200);
	run.rev.setFreeze(true);
	run.ref.frozen = true;
	step(300); // frozen, but non-zero input is still being fed (and ignored)
	check(run.rev.setDecay(2.0), "setDecay() during freeze");
	run.ref.setDecay(2.0); // only affects the decay used after unfreezing
	step(100);
	run.rev.setFreeze(false);
	run.ref.frozen = false;
	step(300);
	// Invalid decays: rejected, previous t60 kept
	check(!run.rev.setDecay(0), "setDecay(0) should fail");
	check(!run.rev.setDecay(-1), "setDecay(-1) should fail");
	check(!run.rev.setDecay(std::numeric_limits<double>::quiet_NaN()), "setDecay(NaN) should fail");
	check(!run.rev.setDecay(std::numeric_limits<double>::infinity()), "setDecay(inf) should fail");
	step(100);
	// reset: history cleared, freeze off, parameters (incl. t60 = 2.0) kept
	run.rev.reset();
	run.ref.reset();
	step(300);
}

// Fully in-place processing must match out-of-place exactly
static void scenarioInPlace() {
	std::printf("scenario: in-place processing\n");
	const std::size_t frames = 500;
	Lcg lcg(999);
	std::vector<double> in(frames*4);
	for (std::size_t i = 0; i < in.size(); ++i) in[i] = lcg.next();
	const std::size_t blocks[] = {13, 1, 100};

	DoubleRun a;
	a.prepare(44100, {{5, 11, 17, 23}}, 0.8);
	std::vector<double> out = a.run(in.data(), frames, blocks, 3);

	signalsmith::feedback::Reverb4<double> rev;
	check(rev.prepare(44100, {{5, 11, 17, 23}}, 0.8), "prepare for in-place run");
	std::vector<double> buf = in;
	std::size_t done = 0, b = 0;
	while (done < frames) {
		std::size_t n = blocks[b++ % 3];
		if (n > frames - done) n = frames - done;
		rev.process(buf.data() + done*4, buf.data() + done*4, n);
		done += n;
	}
	check(buf == out, "in-place output differs from out-of-place");
}

// Reverb4<float> against the double reference: abs 1e-5 + rel 1e-4
static void scenarioFloat() {
	std::printf("scenario: float vs double reference\n");
	const std::size_t frames = 2000;
	Lcg lcg(777);
	std::vector<float> in(frames*4);
	std::vector<double> inD(frames*4);
	for (std::size_t i = 0; i < frames*4; ++i) {
		in[i] = (float)lcg.next();
		inD[i] = in[i]; // the reference gets exactly the float input values
	}
	signalsmith::feedback::Reverb4<float> rev;
	check(rev.prepare(44100, {{101, 203, 307, 401}}, 0.8), "prepare(float)");
	RefReverb4 ref;
	int d[4] = {101, 203, 307, 401};
	ref.prepare(44100, d, 0.8);

	const std::size_t blocks[] = {1, 250, 7};
	std::vector<float> out(frames*4);
	std::size_t done = 0, b = 0;
	double maxRatio = 0;
	while (done < frames) {
		std::size_t n = blocks[b++ % 3];
		if (n > frames - done) n = frames - done;
		rev.process(in.data() + done*4, out.data() + done*4, n);
		for (std::size_t i = 0; i < n; ++i) {
			double ro[4];
			ref.frame(inD.data() + (done + i)*4, ro);
			for (int c = 0; c < 4; ++c) {
				double err = std::fabs(out[(done + i)*4 + c] - ro[c]);
				double tol = 1e-5 + 1e-4*std::fabs(ro[c]);
				if (err > g_maxErrFloat) g_maxErrFloat = err;
				if (err/tol > maxRatio) maxRatio = err/tol;
			}
		}
		done += n;
	}
	check(maxRatio <= 1.0, "float output outside abs 1e-5 + rel 1e-4 tolerance");
}

// Freeze with a non-zero tail: stored energy conserved to 1e-10 (relative)
// over 12000 frames, and no heap traffic from process/setDecay/setFreeze.
static void scenarioFreezeEnergy() {
	std::printf("scenario: freeze energy conservation + no heap traffic\n");
	signalsmith::feedback::Reverb4<double> rev;
	RefReverb4 ref;
	std::array<int, 4> delays {{211, 223, 227, 233}};
	int d[4] = {211, 223, 227, 233};
	check(rev.prepare(44100, delays, 0.9), "prepare for freeze test");
	ref.prepare(44100, d, 0.9);

	Lcg lcg(424242);
	// Build up a non-zero tail
	{
		std::vector<double> in(3000*4), out(3000*4);
		for (std::size_t i = 0; i < in.size(); ++i) in[i] = lcg.next();
		rev.process(in.data(), out.data(), 3000);
		for (std::size_t i = 0; i < 3000; ++i) {
			double ro[4];
			ref.frame(in.data() + i*4, ro);
		}
	}
	rev.setFreeze(true);
	ref.frozen = true;
	const double eFrozen = rev.storedEnergy();
	check(eFrozen > 1e-6, "tail energy should be non-zero before freeze");

	const std::size_t frames = 12000;
	std::vector<double> in(frames*4), out(frames*4);
	for (std::size_t i = 0; i < in.size(); ++i) in[i] = lcg.next(); // ignored while frozen

	long allocsBefore = g_allocCount, freesBefore = g_freeCount;
	const std::size_t blocks[] = {1, 17, 256, 3, 1000};
	std::size_t done = 0, b = 0;
	double maxErr = 0;
	while (done < frames) {
		std::size_t n = blocks[b++ % 5];
		if (n > frames - done) n = frames - done;
		rev.process(in.data() + done*4, out.data() + done*4, n);
		for (std::size_t i = 0; i < n; ++i) {
			double ro[4];
			ref.frame(in.data() + (done + i)*4, ro);
			for (int c = 0; c < 4; ++c) {
				double err = std::fabs(out[(done + i)*4 + c] - ro[c]);
				if (err > maxErr) maxErr = err;
			}
		}
		// Parameter updates are allowed during a freeze, and must not allocate
		check(rev.setDecay(1.7), "setDecay() while frozen");
		rev.setFreeze(true);
		double drift = std::fabs(rev.storedEnergy() - eFrozen)/eFrozen;
		if (drift > g_maxFreezeDrift) g_maxFreezeDrift = drift;
		done += n;
	}
	g_countedAllocs = g_allocCount - allocsBefore;
	g_countedFrees = g_freeCount - freesBefore;
	check(g_countedAllocs == 0, "heap allocation in process/setDecay/setFreeze/storedEnergy");
	check(g_countedFrees == 0, "heap free in process/setDecay/setFreeze/storedEnergy");
	check(maxErr < 1e-12, "frozen output mismatch vs reference");
	check(g_maxFreezeDrift <= 1e-10, "frozen energy drift above 1e-10");
}

// Unprepared/invalid-argument behaviour, 0-frame calls, reset() allocation-free
static void scenarioEdgeCases() {
	std::printf("scenario: edge cases and invalid arguments\n");
	signalsmith::feedback::Reverb4<double> rev;
	std::vector<double> in(40, 1.0), out(40, -1.0);

	// process() before prepare() outputs zeros
	rev.process(in.data(), out.data(), 10);
	bool allZero = true;
	for (std::size_t i = 0; i < out.size(); ++i) if (out[i] != 0) allZero = false;
	check(allZero, "unprepared process() should output zeros");
	check(rev.storedEnergy() == 0, "unprepared storedEnergy() should be 0");
	check(!rev.setDecay(1.0), "setDecay() before prepare() should fail");
	rev.setFreeze(true); // harmless before prepare
	rev.reset(); // harmless before prepare
	rev.process(nullptr, nullptr, 0); // must not touch the pointers

	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	std::array<int, 4> good {{1, 2, 3, 5}};
	check(!rev.prepare(0, good, 1.0), "prepare(sr=0) should fail");
	check(!rev.prepare(-48000, good, 1.0), "prepare(sr<0) should fail");
	check(!rev.prepare(nan, good, 1.0), "prepare(sr=NaN) should fail");
	check(!rev.prepare(inf, good, 1.0), "prepare(sr=inf) should fail");
	check(!rev.prepare(48000, good, 0), "prepare(t60=0) should fail");
	check(!rev.prepare(48000, good, -1), "prepare(t60<0) should fail");
	check(!rev.prepare(48000, good, nan), "prepare(t60=NaN) should fail");
	check(!rev.prepare(48000, good, inf), "prepare(t60=inf) should fail");
	check(!rev.prepare(48000, {{0, 2, 3, 5}}, 1.0), "prepare(delay=0) should fail");
	check(!rev.prepare(48000, {{1, -2, 3, 5}}, 1.0), "prepare(delay<0) should fail");
	rev.process(in.data(), out.data(), 10);
	allZero = true;
	for (std::size_t i = 0; i < out.size(); ++i) if (out[i] != 0) allZero = false;
	check(allZero, "failed prepare() must leave the previous (unprepared) state");

	// Valid prepare, then build up some state
	check(rev.prepare(48000, good, 1.0), "prepare(valid)");
	RefReverb4 ref;
	int d[4] = {1, 2, 3, 5};
	ref.prepare(48000, d, 1.0);
	Lcg lcg(5);
	std::vector<double> in2(60*4);
	for (std::size_t i = 0; i < in2.size(); ++i) in2[i] = lcg.next();
	std::vector<double> out2(60*4);
	rev.process(in2.data(), out2.data(), 50);
	for (int i = 0; i < 50; ++i) {
		double ro[4];
		ref.frame(in2.data() + i*4, ro);
	}
	const double e = rev.storedEnergy();

	// Invalid calls must be rejected and preserve the state
	check(!rev.prepare(nan, good, 1.0), "invalid re-prepare should fail");
	check(!rev.prepare(48000, {{1, 2, 3, -5}}, 1.0), "invalid re-prepare should fail");
	check(!rev.setDecay(nan), "setDecay(NaN) should fail");
	check(!rev.setDecay(-0.5), "setDecay(-0.5) should fail");
	check(rev.storedEnergy() == e, "invalid calls must preserve the state");

	// frames == 0 with null pointers: no-op, does not advance
	rev.process(nullptr, nullptr, 0);
	check(rev.storedEnergy() == e, "0-frame process() must not advance the state");

	rev.process(in2.data() + 50*4, out2.data() + 50*4, 10);
	double maxErr = 0;
	for (int i = 0; i < 10; ++i) {
		double ro[4];
		ref.frame(in2.data() + (50 + i)*4, ro);
		for (int c = 0; c < 4; ++c) {
			maxErr = std::max(maxErr, std::fabs(out2[(50 + i)*4 + c] - ro[c]));
		}
	}
	check(maxErr < 1e-12, "state not preserved across invalid/0-frame calls");

	// reset() must not allocate, and behaves like a fresh instance with the
	// same parameters (including the last valid t60)
	long allocsBefore = g_allocCount, freesBefore = g_freeCount;
	rev.reset();
	check(g_allocCount == allocsBefore && g_freeCount == freesBefore,
		"reset() must not allocate/free");
	signalsmith::feedback::Reverb4<double> fresh;
	check(fresh.prepare(48000, good, 1.0), "fresh prepare");
	std::vector<double> o1(40*4), o2(40*4);
	rev.process(in2.data(), o1.data(), 40);
	fresh.process(in2.data(), o2.data(), 40);
	check(o1 == o2, "reset() should behave like a fresh instance with kept params");
}

// Writes build/fdn_trace.csv: frame, 4 outputs, storedEnergy, freeze flag
static void scenarioTrace(const char *path) {
	std::printf("scenario: writing trace CSV to %s\n", path);
	signalsmith::feedback::Reverb4<double> rev;
	check(rev.prepare(48000, {{1, 2, 3, 5}}, 0.5), "prepare for trace");
	FILE *f = std::fopen(path, "w");
	if (!check(f != nullptr, "could not open trace CSV for writing")) return;
	std::fprintf(f, "frame,out0,out1,out2,out3,storedEnergy,freeze\n");
	const std::size_t frames = 192;
	int freeze = 0;
	for (std::size_t i = 0; i < frames; ++i) {
		if (i == 64) { freeze = 1; rev.setFreeze(true); }
		if (i == 128) { freeze = 0; rev.setFreeze(false); }
		double in[4] = {0, 0, 0, 0};
		if (i == 0) in[0] = 1.0;
		double out[4];
		rev.process(in, out, 1);
		std::fprintf(f, "%zu,%.17g,%.17g,%.17g,%.17g,%.17g,%d\n",
			i, out[0], out[1], out[2], out[3], rev.storedEnergy(), freeze);
	}
	std::fclose(f);
}

int main(int argc, char **argv) {
	const char *tracePath = (argc > 1) ? argv[1] : "build/fdn_trace.csv";
	scenarioImpulseEchoes();
	scenarioRandomOps();
	scenarioInPlace();
	scenarioFloat();
	scenarioFreezeEnergy();
	scenarioEdgeCases();
	scenarioTrace(tracePath);

	std::printf("max double error vs reference: %.3g\n", g_maxErrDouble);
	std::printf("max float error vs reference:  %.3g (limit abs 1e-5 + rel 1e-4)\n", g_maxErrFloat);
	std::printf("max storedEnergy rel. error:   %.3g\n", g_maxEnergyErr);
	std::printf("max freeze energy rel. drift:  %.3g (limit 1e-10)\n", g_maxFreezeDrift);
	std::printf("heap allocs/frees in process+param calls: %ld/%ld\n", g_countedAllocs, g_countedFrees);
	if (g_failures) {
		std::printf("FAILED: %d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
