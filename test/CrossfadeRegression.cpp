// Regression test for fftconvolver::CrossfadeConvolver.
//
// The expected output is computed by an independent plain time-domain
// convolution plus an explicit linear cross-fade model. The same input stream
// is processed with two different processing-call chunkings (which must give
// identical results), in-place and out-of-place.
//
// Global operator new/delete are overridden so that every process() call can
// be checked for heap allocations/frees, including the call in which a
// cross-fade ends exactly.
#include "../CrossfadeConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

// -----------------------------------------------------------------------
// Heap allocation counting
// -----------------------------------------------------------------------
long g_allocs = 0;
long g_frees = 0;

void* operator new(std::size_t size) throw(std::bad_alloc)
{
  if (size == 0) size = 1;
  void* p = std::malloc(size);
  if (!p) throw std::bad_alloc();
  ++g_allocs;
  return p;
}

void* operator new[](std::size_t size) throw(std::bad_alloc)
{
  if (size == 0) size = 1;
  void* p = std::malloc(size);
  if (!p) throw std::bad_alloc();
  ++g_allocs;
  return p;
}

void operator delete(void* p) throw()
{
  if (p) { std::free(p); ++g_frees; }
}

void operator delete[](void* p) throw()
{
  if (p) { std::free(p); ++g_frees; }
}

void* operator new(std::size_t size, const std::nothrow_t&) throw()
{
  if (size == 0) size = 1;
  void* p = std::malloc(size);
  if (p) ++g_allocs;
  return p;
}

void* operator new[](std::size_t size, const std::nothrow_t&) throw()
{
  if (size == 0) size = 1;
  void* p = std::malloc(size);
  if (p) ++g_allocs;
  return p;
}

void operator delete(void* p, const std::nothrow_t&) throw()
{
  if (p) { std::free(p); ++g_frees; }
}

void operator delete[](void* p, const std::nothrow_t&) throw()
{
  if (p) { std::free(p); ++g_frees; }
}

namespace {

using fftconvolver::Sample;
using fftconvolver::CrossfadeConvolver;

static int g_failures = 0;

void check(bool condition, const char* message)
{
  if (!condition)
  {
    std::printf("FAILED: %s\n", message);
    ++g_failures;
  }
}

// Independent time-domain causal convolution of the whole signal with one IR.
std::vector<double> referenceConvolution(const std::vector<Sample>& input,
                                         const std::vector<Sample>& ir)
{
  const size_t n = input.size();
  std::vector<double> result(n, 0.0);
  for (size_t i = 0; i < n; ++i)
  {
    double acc = 0.0;
    const size_t taps = std::min(ir.size(), i + 1);
    for (size_t k = 0; k < taps; ++k)
    {
      acc += static_cast<double>(ir[k]) *
             static_cast<double>(input[i - k]);
    }
    result[i] = acc;
  }
  return result;
}

struct Switch
{
  size_t n0;                 // First output sample of the new region
  std::vector<Sample> ir;    // New impulse response (empty => mute filter)
  size_t fade;               // Cross-fade length F (0 => hard switch)
};

// Builds the complete expected output from the switch schedule.
// convTable[k] holds the full convolution with switches[k-1]'s IR
// (convTable[0] is the initial IR), so the math never reuses an engine.
std::vector<double> referenceOutput(
    const std::vector<Sample>& input,
    const std::vector<Sample>& initialIR,
    const std::vector<Switch>& switches,
    std::vector<std::vector<double> >& convTable)
{
  const size_t n = input.size();
  convTable.clear();
  convTable.push_back(referenceConvolution(input, initialIR));
  for (size_t s = 0; s < switches.size(); ++s)
  {
    convTable.push_back(referenceConvolution(input, switches[s].ir));
  }

  std::vector<double> output(n, 0.0);
  for (size_t i = 0; i < n; ++i)
  {
    // Latest switch whose new region already covers sample i.
    int s = -1;
    for (size_t k = 0; k < switches.size(); ++k)
    {
      if (switches[k].n0 <= i) s = static_cast<int>(k);
    }

    if (s < 0)
    {
      output[i] = convTable[0][i];
      continue;
    }

    const size_t oldIndex = static_cast<size_t>(s);       // previous IR
    const size_t newIndex = static_cast<size_t>(s + 1);   // new IR
    const Switch& sw = switches[static_cast<size_t>(s)];
    const size_t j = i - sw.n0;

    if (sw.fade == 0 || j >= sw.fade)
    {
      output[i] = convTable[newIndex][i];
    }
    else
    {
      const double a =
          (static_cast<double>(j) + 1.0) / static_cast<double>(sw.fade);
      output[i] = (1.0 - a) * convTable[oldIndex][i] +
                  a * convTable[newIndex][i];
    }
  }
  return output;
}

bool withinTolerance(double actual, double expected)
{
  return std::fabs(actual - expected) <= 1e-5 + 1e-4 * std::fabs(expected);
}

struct RunResult
{
  std::vector<Sample> output;
  long processAllocs;
  long processFrees;
  long fadeEndCallAllocs;
  long fadeEndCallFrees;
};

// Runs the complete scenario with a cyclic chunking pattern. Switch points and
// the rejection point are stream positions, independent of the chunking.
RunResult runScenario(const std::vector<Sample>& input,
                      const std::vector<Sample>& initialIR,
                      const std::vector<Sample>& hMid,
                      const std::vector<Switch>& switches,
                      const size_t* cycle,
                      size_t cycleLen,
                      bool inPlace,
                      size_t blockSize,
                      size_t maxIRLen,
                      size_t rejectAt,
                      size_t fadeEndSample)
{
  const size_t n = input.size();
  RunResult result;
  result.output.assign(n, Sample(0.0));
  result.processAllocs = 0;
  result.processFrees = 0;
  result.fadeEndCallAllocs = -1;
  result.fadeEndCallFrees = -1;

  CrossfadeConvolver conv;
  bool ok = conv.init(blockSize, maxIRLen, initialIR.data(), initialIR.size());
  check(ok, "init() failed");

  // All positions at which something happens between process() calls.
  std::vector<size_t> boundaries;
  boundaries.push_back(rejectAt);
  for (size_t s = 0; s < switches.size(); ++s)
  {
    boundaries.push_back(switches[s].n0);
  }
  boundaries.push_back(n);
  std::sort(boundaries.begin(), boundaries.end());

  std::vector<Sample> work;
  if (inPlace)
  {
    work = input; // In-place: one buffer serves as input and output.
  }

  size_t pos = 0;
  size_t cyclePos = 0;
  size_t boundaryIdx = 0;
  size_t switchIdx = 0;
  bool rejectDone = false;

  while (pos < n)
  {
    // Zero-length call with null pointers must not advance any state.
    const long a0 = g_allocs;
    const long f0 = g_frees;
    conv.process(0, 0, 0);
    result.processAllocs += g_allocs - a0;
    result.processFrees += g_frees - f0;

    const size_t nextBoundary = boundaries[boundaryIdx];
    size_t step = cycle[cyclePos % cycleLen];
    cyclePos++;
    if (step > nextBoundary - pos) step = nextBoundary - pos;

    const size_t prevPos = pos;
    const long beforeA = g_allocs;
    const long beforeF = g_frees;
    if (inPlace)
    {
      conv.process(work.data() + pos, work.data() + pos, step);
      std::memcpy(result.output.data() + pos, work.data() + pos,
                  step * sizeof(Sample));
    }
    else
    {
      conv.process(input.data() + pos, result.output.data() + pos, step);
    }
    const long da = g_allocs - beforeA;
    const long df = g_frees - beforeF;
    result.processAllocs += da;
    result.processFrees += df;

    pos += step;

    // Capture the call within which the fade ends (its last faded sample).
    if (result.fadeEndCallAllocs < 0 &&
        prevPos < fadeEndSample && fadeEndSample <= pos)
    {
      result.fadeEndCallAllocs = da;
      result.fadeEndCallFrees = df;
    }

    if (pos == rejectAt && !rejectDone)
    {
      rejectDone = true;

      // Valid update while a fade is active: must be rejected.
      check(!conv.setImpulse(hMid.data(), hMid.size(), 4),
            "setImpulse() during fade must return false");

      // Invalid requests must fail as well and leave everything untouched.
      std::vector<Sample> tooLong(maxIRLen + 1, Sample(0.5));
      check(!conv.setImpulse(tooLong.data(), tooLong.size(), 4),
            "IR longer than maxIRLen must be rejected");
      check(!conv.setImpulse(0, 3, 4),
            "null IR with non-zero length must be rejected");
      std::vector<Sample> nanIR(4, Sample(0.5));
      nanIR[2] = std::nanf("");
      check(!conv.setImpulse(nanIR.data(), nanIR.size(), 4),
            "non-finite (NaN) IR must be rejected");
      std::vector<Sample> infIR(4, Sample(0.5));
      infIR[1] = std::numeric_limits<Sample>::infinity();
      check(!conv.setImpulse(infIR.data(), infIR.size(), 4),
            "non-finite (Inf) IR must be rejected");
    }

    if (switchIdx < switches.size() && pos == switches[switchIdx].n0)
    {
      const Switch& sw = switches[switchIdx];
      std::vector<Sample> owned = sw.ir; // mutable caller-side copy
      const bool applied =
          conv.setImpulse(owned.empty() ? 0 : owned.data(),
                          owned.size(), sw.fade);
      check(applied, "setImpulse() of a valid response must succeed");

      // The class holds its own copy: corrupting the caller's array must
      // not change the sound.
      for (size_t k = 0; k < owned.size(); ++k) owned[k] = Sample(99.0);

      ++switchIdx;
    }

    if (pos == nextBoundary)
    {
      ++boundaryIdx;
    }
  }

  return result;
}

} // namespace

int main()
{
  // ----- Synthetic dry signal: sparse impulses plus a smooth waveform. -----
  const size_t N = 130;
  std::vector<Sample> input(N);
  for (size_t i = 0; i < N; ++i)
  {
    input[i] = Sample(0.4 * std::sin(0.31 * static_cast<double>(i)) +
                      0.25 * std::cos(0.13 * static_cast<double>(i)));
  }
  input[3] += Sample(1.0);
  input[17] += Sample(-0.8);
  input[41] += Sample(0.9);
  input[88] += Sample(0.7);

  // ----- Sparse impulse responses (block size 8 => partitions of 8). -------
  const Sample shortTaps[5] = {Sample(0.9), Sample(-0.4), Sample(0.0),
                               Sample(0.25), Sample(-0.15)};
  std::vector<Sample> hShort(shortTaps, shortTaps + 5);

  std::vector<Sample> hLong(37, Sample(0.0));
  hLong[0] = Sample(1.0);
  hLong[1] = Sample(0.3);
  hLong[4] = Sample(-0.6);
  hLong[9] = Sample(0.45);
  hLong[20] = Sample(-0.4);
  hLong[33] = Sample(0.55); // late echo of pre-switch input
  hLong[36] = Sample(0.2);

  std::vector<Sample> hMid(13, Sample(0.0));
  hMid[0] = Sample(0.8);
  hMid[2] = Sample(0.5);
  hMid[6] = Sample(-0.35);
  hMid[12] = Sample(0.3);

  std::vector<Sample> hEmpty; // mute filter

  const size_t blockSize = 8;
  const size_t maxIRLen = 40;

  // ----- Switch schedule (all points fall inside FFT partitions). ----------
  std::vector<Switch> switches;
  switches.push_back((Switch){23, hLong, 7});    // short -> long, spans a partition
  switches.push_back((Switch){48, hEmpty, 1});   // long -> mute, F = 1
  switches.push_back((Switch){70, hLong, 0});    // mute -> long, hard switch F = 0
  switches.push_back((Switch){97, hShort, 13});  // long -> short, fade ends at 110

  std::vector<std::vector<double> > convTable;
  const std::vector<double> expected =
      referenceOutput(input, hShort, switches, convTable);

  // Two deliberately different chunkings, both including lengths 1, 3, 17.
  const size_t patternA[] = {17, 3, 1, 7};
  const size_t patternB[] = {1, 3, 17, 5};
  const size_t rejectAt = 27; // inside the first fade (23 .. 29)

  const RunResult outOfPlace =
      runScenario(input, hShort, hMid, switches, patternA, 4, false,
                  blockSize, maxIRLen, rejectAt, 110);
  const RunResult otherChunking =
      runScenario(input, hShort, hMid, switches, patternB, 4, false,
                  blockSize, maxIRLen, rejectAt, 110);
  const RunResult inPlace =
      runScenario(input, hShort, hMid, switches, patternA, 4, true,
                  blockSize, maxIRLen, rejectAt, 110);

  // ----- Numeric checks against the time-domain reference. -----------------
  double maxError = 0.0;
  double maxChunkDiff = 0.0;
  double maxInPlaceDiff = 0.0;
  for (size_t i = 0; i < N; ++i)
  {
    const double actual = static_cast<double>(outOfPlace.output[i]);
    const double err = std::fabs(actual - expected[i]);
    maxError = std::max(maxError, err);
    if (!withinTolerance(actual, expected[i]))
    {
      std::printf("FAILED: reference mismatch at sample %d: got %.9g "
                  "expected %.9g\n",
                  static_cast<int>(i), actual, expected[i]);
      ++g_failures;
    }
    maxChunkDiff = std::max(
        maxChunkDiff,
        std::fabs(actual -
                  static_cast<double>(otherChunking.output[i])));
    maxInPlaceDiff = std::max(
        maxInPlaceDiff,
        std::fabs(actual - static_cast<double>(inPlace.output[i])));
  }
  check(maxChunkDiff <= 1e-6, "output must be independent of chunking");
  check(maxInPlaceDiff <= 1e-6, "in-place processing must match out-of-place");

  // Explicit history check: the impulse at sample 41 must produce the late
  // hLong tap-33 echo at sample 74, i.e. after the mute phase and the switch
  // back at 70 (a cold-started engine would output silence here).
  check(std::fabs(static_cast<double>(outOfPlace.output[74])) > 0.05,
        "pre-switch input must be audible through the new long response");

  // ----- process() must never allocate or free heap memory. ----------------
  check(outOfPlace.processAllocs == 0, "process() performed allocations");
  check(outOfPlace.processFrees == 0, "process() performed frees");
  check(otherChunking.processAllocs == 0,
        "process() performed allocations (pattern B)");
  check(otherChunking.processFrees == 0,
        "process() performed frees (pattern B)");
  check(inPlace.processAllocs == 0,
        "process() performed allocations (in-place)");
  check(inPlace.processFrees == 0,
        "process() performed frees (in-place)");
  check(outOfPlace.fadeEndCallAllocs == 0 &&
        outOfPlace.fadeEndCallFrees == 0,
        "the process() call ending a fade must be allocation/free-free");

  // ----- Uninitialized convolver outputs silence. --------------------------
  {
    CrossfadeConvolver uninitialized;
    Sample zeros[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uninitialized.process(zeros, zeros, 8);
    for (size_t i = 0; i < 8; ++i)
    {
      check(zeros[i] == Sample(0.0), "uninitialized process() must zero");
    }
    uninitialized.process(0, 0, 0); // must be harmless
  }

  // ----- reset() discards response and history; re-init starts at zero. ----
  {
    CrossfadeConvolver conv;
    check(conv.init(blockSize, maxIRLen, hLong.data(), hLong.size()),
          "init before reset");
    std::vector<Sample> buf(16, Sample(0.0));
    conv.process(input.data(), buf.data(), 16);
    conv.reset();
    std::vector<Sample> silence(16, Sample(1.0));
    conv.process(input.data(), silence.data(), 16);
    for (size_t i = 0; i < 16; ++i)
    {
      check(silence[i] == Sample(0.0), "process after reset must be silent");
    }

    check(conv.init(blockSize, maxIRLen, hShort.data(), hShort.size()),
          "re-init after reset");
    std::vector<Sample> fresh(N, Sample(0.0));
    size_t pos = 0;
    const size_t chunks[] = {1, 3, 17};
    size_t c = 0;
    while (pos < N)
    {
      size_t step = std::min(chunks[c % 3], N - pos);
      conv.process(input.data() + pos, fresh.data() + pos, step);
      pos += step;
      ++c;
    }
    const std::vector<double> shortConv =
        referenceConvolution(input, hShort);
    for (size_t i = 0; i < N; ++i)
    {
      if (!withinTolerance(static_cast<double>(fresh[i]), shortConv[i]))
      {
        std::printf("FAILED: post-reset stream mismatch at %d\n",
                    static_cast<int>(i));
        ++g_failures;
      }
    }
  }

  // ----- init()/setImpulse() argument validation outside any fade. ---------
  {
    CrossfadeConvolver conv;
    check(!conv.init(0, maxIRLen, hShort.data(), hShort.size()),
          "block size 0 must be rejected");
    std::vector<Sample> tooLong(maxIRLen + 1, Sample(0.5));
    check(!conv.init(blockSize, maxIRLen, tooLong.data(), tooLong.size()),
          "init with over-long IR must be rejected");
    check(!conv.init(blockSize, maxIRLen, 0, 2),
          "init with null IR and non-zero length must be rejected");
    std::vector<Sample> bad(3, Sample(0.5));
    bad[1] = std::nanf("");
    check(!conv.init(blockSize, maxIRLen, bad.data(), bad.size()),
          "init with non-finite IR must be rejected");
    check(conv.init(blockSize, maxIRLen, hShort.data(), hShort.size()),
          "valid init must succeed");
    check(!conv.setImpulse(0, 1, 0),
          "setImpulse with null IR and non-zero length must be rejected");
    check(!conv.setImpulse(tooLong.data(), tooLong.size(), 0),
          "setImpulse with over-long IR must be rejected");
    bad[1] = std::numeric_limits<Sample>::infinity();
    check(!conv.setImpulse(bad.data(), bad.size(), 0),
          "setImpulse with non-finite IR must be rejected");
    // Rejected calls must leave the sound untouched.
    Sample chunk[5] = {Sample(1), Sample(0), Sample(0), Sample(0), Sample(0)};
    Sample got[5];
    conv.process(chunk, got, 5);
    for (size_t i = 0; i < 5; ++i)
    {
      check(withinTolerance(static_cast<double>(got[i]),
                            static_cast<double>(hShort[i])),
            "rejected setImpulse must not change the sound");
    }
  }

  // ----- Repeated swapping must not grow memory usage. ---------------------
  {
    CrossfadeConvolver conv;
    check(conv.init(blockSize, maxIRLen, hShort.data(), hShort.size()),
          "init for repeated-swap run");
    Sample chunk[4] = {Sample(0.1), Sample(-0.2), Sample(0.3), Sample(0.05)};
    Sample out[4];

    // One complete swap pair brings both engines into existence: from here
    // on the footprint has reached its steady state.
    check(conv.setImpulse(hLong.data(), hLong.size(), 2), "swap to long");
    conv.process(chunk, out, 4);
    conv.process(chunk, out, 4);
    check(conv.setImpulse(hShort.data(), hShort.size(), 2),
          "swap back to short");
    conv.process(chunk, out, 4);
    conv.process(chunk, out, 4);
    const long steady = g_allocs - g_frees;

    for (int round = 0; round < 25; ++round)
    {
      check(conv.setImpulse(hLong.data(), hLong.size(), 2), "swap to long");
      conv.process(chunk, out, 4);
      conv.process(chunk, out, 4); // let the fade finish
      check(conv.setImpulse(hShort.data(), hShort.size(), 2),
            "swap back to short");
      conv.process(chunk, out, 4);
      conv.process(chunk, out, 4);
    }
    const long outstanding = g_allocs - g_frees;
    check(outstanding == steady,
          "memory footprint must not grow with the number of swaps");
  }

  // ----- process() lengths larger than maxIRLen and a mid-call switch. -----
  {
    CrossfadeConvolver conv;
    check(conv.init(blockSize, maxIRLen, hShort.data(), hShort.size()),
          "init for oversized process() run");

    std::vector<Sample> big(3 * maxIRLen + 5, Sample(0.0));
    for (size_t i = 0; i < big.size(); ++i)
    {
      big[i] = input[i % N] - Sample(0.5) * input[(i * 7) % N];
    }

    // Reference: initial short convolution, hard switch to long at 2*maxIRLen.
    const size_t switchPoint = 2 * maxIRLen;
    std::vector<double> refShort = referenceConvolution(big, hShort);
    std::vector<double> refLong = referenceConvolution(big, hLong);

    std::vector<Sample> got(big.size(), Sample(0.0));
    conv.process(big.data(), got.data(), switchPoint); // one oversized call
    check(conv.setImpulse(hLong.data(), hLong.size(), 0),
          "hard switch for oversized process() run");
    conv.process(big.data() + switchPoint, got.data() + switchPoint,
                 big.size() - switchPoint);

    for (size_t i = 0; i < big.size(); ++i)
    {
      const double ref = (i < switchPoint) ? refShort[i] : refLong[i];
      if (!withinTolerance(static_cast<double>(got[i]), ref))
      {
        std::printf("FAILED: oversized process() mismatch at %d\n",
                    static_cast<int>(i));
        ++g_failures;
      }
    }
  }

  // ----- maxIRLen == 0: only mute filters are admissible. ------------------
  {
    CrossfadeConvolver conv;
    check(conv.init(blockSize, 0, 0, 0), "init with maxIRLen 0 and empty IR");
    check(!conv.setImpulse(hShort.data(), hShort.size(), 0),
          "non-empty IR must exceed maxIRLen 0");
    check(conv.setImpulse(0, 0, 3), "empty IR must be allowed");
    std::vector<Sample> got(20, Sample(1.0));
    conv.process(input.data(), got.data(), 20);
    for (size_t i = 0; i < 20; ++i)
    {
      check(got[i] == Sample(0.0), "mute filter with maxIRLen 0 must zero");
    }
    conv.reset();
  }

  // ----- CSV output around the switch points. ------------------------------
  FILE* csv = std::fopen("build/crossfade.csv", "w");
  check(csv != 0, "cannot open build/crossfade.csv");
  if (csv)
  {
    std::fprintf(csv, "sample,actual,reference\n");
    for (size_t i = 0; i < N; ++i)
    {
      std::fprintf(csv, "%d,%.9g,%.9g\n", static_cast<int>(i),
                   static_cast<double>(outOfPlace.output[i]), expected[i]);
    }
    std::fclose(csv);
  }

  std::printf("max_error=%.9g\n", maxError);
  std::printf("max_chunking_diff=%.9g max_inplace_diff=%.9g\n",
              maxChunkDiff, maxInPlaceDiff);
  std::printf("process_allocations=%ld process_frees=%ld\n",
              outOfPlace.processAllocs + otherChunking.processAllocs +
                  inPlace.processAllocs,
              outOfPlace.processFrees + otherChunking.processFrees +
                  inPlace.processFrees);
  std::printf("fade_ending_call_allocations=%ld fade_ending_call_frees=%ld\n",
              outOfPlace.fadeEndCallAllocs, outOfPlace.fadeEndCallFrees);

  if (g_failures == 0)
  {
    std::printf("CrossfadeConvolver regression: ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("CrossfadeConvolver regression: %d CHECK(S) FAILED\n",
              g_failures);
  return 1;
}
