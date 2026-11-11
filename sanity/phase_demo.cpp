//
//  phase_demo.cpp
//  Per-sample phase modulation sanity checks for TableLookupOsc / SineWave.
//
//  Reference: independent double accumulator + linear-interpolated table lookup.
//  Run via sanity/phase_demo.sh. Prints PHASE_MODULATION_OK when all checks pass.
//

#include "Tonic.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace Tonic;
using namespace Tonic::Tonic_;

static const unsigned BS = kSynthesisBlockSize;
static const float SR = 44100.f;

// ---------------------------------------------------------------------------
// Test generators
// ---------------------------------------------------------------------------

// Per-sample sequence generator, optionally stereo (L/R vectors).
class SequenceGen_ : public Generator_{
public:
  std::vector<float> left;
  std::vector<float> right;
  bool stereo_;
  unsigned long pos;
  SequenceGen_() : stereo_(false), pos(0) {}
  void makeStereo(bool s){ stereo_ = s; setIsStereoOutput(s); }
protected:
  void computeSynthesisBlock(const SynthesisContext_ &){
    for (unsigned i = 0; i < BS; i++){
      float lv = left[pos % left.size()];
      if (stereo_){
        float rv = right[pos % right.size()];
        outputFrames_(i, 0) = lv;
        outputFrames_(i, 1) = rv;
      }
      else{
        outputFrames_[i] = lv;
      }
      pos++;
    }
  }
};

class SequenceGen : public TemplatedGenerator<SequenceGen_>{
public:
  SequenceGen & setValues(std::vector<float> v){ gen()->left = v; return *this; }
  SequenceGen & setStereo(std::vector<float> l, std::vector<float> r){
    gen()->left = l; gen()->right = r; gen()->makeStereo(true); return *this;
  }
};

// Stateful generator counting how often its block is actually computed.
// Emits its 1-based block id as the raw sample value on every channel.
class CountGen_ : public Generator_{
public:
  long computeCount;
  CountGen_() : computeCount(0) {}
protected:
  void computeSynthesisBlock(const SynthesisContext_ &){
    ++computeCount;
    float v = (float)computeCount;
    for (unsigned i = 0; i < BS; i++) outputFrames_[i] = v;
  }
};

class CountGen : public TemplatedGenerator<CountGen_>{
public:
  long count(){ return gen()->computeCount; }
};

// Stateful control generator: one new value per control block.
class BlockCtrl_ : public ControlGenerator_{
public:
  long computeCount;
  BlockCtrl_() : computeCount(0) { output_.value = -1.f; }
protected:
  void computeOutput(const SynthesisContext_ &){
    ++computeCount;
    output_.value = (float)computeCount;
    output_.triggered = true;
  }
};

class BlockCtrl : public TemplatedControlGenerator<BlockCtrl_>{
public:
  long count(){ return gen()->computeCount; }
};

// Synth with the limiter disabled, for deterministic PCM comparisons.
class PhaseDemoSynth : public Synth{
public:
  PhaseDemoSynth(Generator out){
    setOutputGen(out);
    setLimitOutput(false);
  }
};

// ---------------------------------------------------------------------------
// Reference model: independent double carrier accumulator, linear lookup
// ---------------------------------------------------------------------------

static std::vector<float> makeSineReferenceTable(){
  const unsigned long N = 4096;
  std::vector<float> t(N + 1);
  for (unsigned long i = 0; i <= N; i++) t[i] = sinf(TWO_PI * (float)i / (float)N);
  return t;
}

static std::vector<float> referenceOsc(const std::vector<float> &table, unsigned long N,
                                       const std::vector<float> &freq,
                                       const std::vector<float> &off){
  std::vector<float> out;
  out.reserve(freq.size());
  double phasePos = 0.0;                              // table positions
  float rateConstant = (float)N / SR;
  for (size_t n = 0; n < freq.size(); n++){
    double o = std::isfinite(off[n]) ? (double)off[n] : 0.0;   // NaN/Inf -> 0
    double readPos = phasePos + o * (double)N;
    double ip = floor(readPos);
    long idx = (long)ip % (long)N;
    if (idx < 0) idx += N;
    double frac = readPos - ip;
    out.push_back((float)(table[idx] + frac * (table[idx + 1] - table[idx])));
    // carrier advances by frequency only, in the same float precision Tonic uses
    phasePos += (double)((float)freq[n] * rateConstant);
  }
  return out;
}

// Drive a generator block by block with a normal context and collect PCM.
static std::vector<float> runBlocks(Generator osc, unsigned blocks){
  TonicFrames frames(BS, 1);
  SynthesisContext_ ctx;
  std::vector<float> out;
  out.reserve(blocks * BS);
  for (unsigned b = 0; b < blocks; b++){
    osc.tick(frames, ctx);
    for (unsigned i = 0; i < BS; i++) out.push_back(frames[i]);
    ctx.tick();
  }
  return out;
}

static float maxAbsDiff(const std::vector<float> &a, const std::vector<float> &b){
  assert(a.size() == b.size());
  float m = 0.f;
  for (size_t i = 0; i < a.size(); i++) m = std::max(m, fabsf(a[i] - b[i]));
  return m;
}

static void check(bool cond, const char *msg){
  if (!cond){
    printf("FAIL: %s\n", msg);
    assert(false);
  }
}

// Build the real five-sample table [0,1,0,-1,0] (N = 4 positions).
static SampleTable makeFiveTable(){
  SampleTable table(5, 1);
  const float values[] = {0, 1, 0, -1, 0};
  for (unsigned i = 0; i < 5; i++) table.dataPointer()[i] = values[i];
  return table;
}

// Build an independently-instantiated phase-modulated sine chain.
static Generator makePhaseChain(const std::vector<float> &pattern){
  return SineWave()
    .freq(SequenceGen().setValues(std::vector<float>(pattern.size(), 196.f)))
    .phaseOffset(SequenceGen().setValues(pattern));
}

int main(){

  const std::vector<float> sineTable = makeSineReferenceTable();
  const unsigned long sineN = sineTable.size() - 1;
  SampleTable five = makeFiveTable();
  const unsigned long fiveN = 4;

  const unsigned blocks = 8;
  const size_t total = blocks * BS;

  // =======================================================================
  // 1. No phaseOffset call: output identical to the zero-offset reference.
  // =======================================================================
  {
    std::vector<float> freq(total), off(total, 0.f);
    for (size_t i = 0; i < total; i++) freq[i] = 220.f + 0.25f * (float)i;

    TableLookupOsc osc;
    osc.setLookupTable(five).freq(SequenceGen().setValues(freq));
    std::vector<float> got = runBlocks(osc, blocks);
    std::vector<float> ref = referenceOsc(std::vector<float>(five.dataPointer(),
                                                              five.dataPointer() + 5),
                                          fiveN, freq, off);
    float d = maxAbsDiff(got, ref);
    printf("1. default zero offset              maxDiff=%g\n", d);
    check(d < 1e-6f, "default phase offset must be zero and preserve original output");
  }

  // =======================================================================
  // 2. Quarter-cycle offset: SineWave().freq(0).phaseOffset(0.25) ~ 1,
  //    and on the five-table it lands exactly on sample value 1.
  // =======================================================================
  {
    SineWave sine;
    sine.freq(0.f).phaseOffset(0.25f);
    std::vector<float> got = runBlocks(sine, 2);
    for (size_t i = 0; i < got.size(); i++){
      check(std::isfinite(got[i]), "quarter-cycle sine must be finite");
      check(fabsf(got[i] - 1.f) < 1e-4f, "0.25 cycle offset at freq 0 must read ~1");
    }

    TableLookupOsc osc;
    osc.setLookupTable(five).freq(0.f).phaseOffset(0.25f);
    std::vector<float> fiveGot = runBlocks(osc, 2);
    for (size_t i = 0; i < fiveGot.size(); i++)
      check(fabsf(fiveGot[i] - 1.f) < 1e-6f, "0.25 cycle on five-table must read exactly 1");

    // -0.25 cycle at freq 0 must read -1 (negative offset, no abs folding).
    SineWave neg;
    neg.freq(0.f).phaseOffset(-0.25f);
    std::vector<float> negGot = runBlocks(neg, 1);
    for (size_t i = 0; i < negGot.size(); i++)
      check(fabsf(negGot[i] + 1.f) < 1e-4f, "negative quarter cycle must read ~-1");
  }

  // =======================================================================
  // 3. Offsets differing by whole cycles are identical; large exact values.
  // =======================================================================
  {
    const float big = 1000000.25f;
    check(big == 1000000.25f, "1000000.25 must be exactly representable in float");

    std::vector<float> base(total, 0.25f);
    std::vector<float> shifted(total, big);
    std::vector<float> negShifted(total, -1000000.25f);
    std::vector<float> negBase(total, -0.25f);
    std::vector<float> freq(total, 137.f);

    TableLookupOsc a; a.setLookupTable(five).freq(SequenceGen().setValues(freq)).phaseOffset(SequenceGen().setValues(base));
    TableLookupOsc b; b.setLookupTable(five).freq(SequenceGen().setValues(freq)).phaseOffset(SequenceGen().setValues(shifted));
    TableLookupOsc c; c.setLookupTable(five).freq(SequenceGen().setValues(freq)).phaseOffset(SequenceGen().setValues(negShifted));
    TableLookupOsc d; d.setLookupTable(five).freq(SequenceGen().setValues(freq)).phaseOffset(SequenceGen().setValues(negBase));

    std::vector<float> ga = runBlocks(a, blocks), gb = runBlocks(b, blocks);
    std::vector<float> gc = runBlocks(c, blocks), gd = runBlocks(d, blocks);
    float d1 = maxAbsDiff(ga, gb), d2 = maxAbsDiff(gc, gd);
    printf("3. integer-cycle equivalence        d(+1e6)=%g d(-1e6)=%g\n", d1, d2);
    check(d1 < 1e-6f, "offset 1000000.25 must equal offset 0.25");
    check(d2 < 1e-6f, "offset -1000000.25 must equal offset -0.25");
  }

  // =======================================================================
  // 4. Fast per-sample offset changes, offsets beyond one cycle and
  //    per-sample frequency changes all match the double reference.
  // =======================================================================
  {
    std::vector<float> freq(total), off(total);
    const float wild[] = {0.1f, 0.7f, -0.3f, 1.4f, -2.6f, 3.25f, 0.0f, -1.75f};
    for (size_t i = 0; i < total; i++){
      off[i] = wild[i % 8];
      freq[i] = (i % 3 == 0) ? -550.f : (440.f + 13.f * (float)(i % 11));
    }

    TableLookupOsc osc;
    osc.setLookupTable(five)
       .freq(SequenceGen().setValues(freq))
       .phaseOffset(SequenceGen().setValues(off));
    std::vector<float> got = runBlocks(osc, blocks);
    std::vector<float> ref = referenceOsc(std::vector<float>(five.dataPointer(),
                                                              five.dataPointer() + 5),
                                          fiveN, freq, off);
    float d = maxAbsDiff(got, ref);
    printf("4. wild per-sample modulation       maxDiff=%g\n", d);
    check(d < 1e-6f, "per-sample offsets/frequencies must match the double reference");
  }

  // =======================================================================
  // 5. freq = 0, offset ramps 0 -> 1 across one block: one full cycle scan,
  //    not an accelerating spin. Checked on the sine table.
  // =======================================================================
  {
    std::vector<float> freq(BS, 0.f), off(BS);
    for (unsigned i = 0; i < BS; i++) off[i] = (float)i / (float)BS;

    SineWave sine;
    sine.freq(SequenceGen().setValues(freq)).phaseOffset(SequenceGen().setValues(off));
    std::vector<float> got = runBlocks(sine, 1);
    std::vector<float> ref = referenceOsc(sineTable, sineN, freq, off);
    float d = maxAbsDiff(got, ref);
    printf("5. one-cycle scan at freq 0         maxDiff=%g\n", d);
    check(d < 2e-6f, "linear offset ramp at freq 0 must scan exactly one cycle");
    // monotonic rise over the first quarter of the scan
    for (unsigned i = 1; i < BS / 4; i++)
      check(got[i] > got[i - 1], "ramp scan must move smoothly through the table");
  }

  // =======================================================================
  // 6. Carrier recovery: after an offset stretch returns to 0, the carrier
  //    position matches an oscillator that was never modulated.
  // =======================================================================
  {
    std::vector<float> freq(total, 330.f);
    std::vector<float> modulatedOff(total, 0.f);
    for (size_t i = 0; i < 2 * BS; i++) modulatedOff[i] = 0.25f;   // first two blocks
    // remaining blocks stay at 0

    SineWave modulated, untouched;
    modulated.freq(SequenceGen().setValues(freq)).phaseOffset(SequenceGen().setValues(modulatedOff));
    untouched.freq(SequenceGen().setValues(freq));   // independent, identical freq input

    std::vector<float> gMod = runBlocks(modulated, blocks);
    std::vector<float> gRef = runBlocks(untouched, blocks);

    float duringDiff = 0.f, afterDiff = 0.f;
    for (size_t i = 0; i < total; i++){
      if (i < 2 * BS) duringDiff = std::max(duringDiff, fabsf(gMod[i] - gRef[i]));
      else afterDiff = std::max(afterDiff, fabsf(gMod[i] - gRef[i]));
    }
    printf("6. carrier recovery                 during=%g after=%g\n", duringDiff, afterDiff);
    check(duringDiff > 0.1f, "modulated stretch should differ from the untouched oscillator");
    check(afterDiff == 0.f, "returning offset to 0 must restore the exact carrier position");
  }

  // =======================================================================
  // 7. Negative frequency combined with large / negative offsets.
  // =======================================================================
  {
    std::vector<float> freq(total), off(total);
    for (size_t i = 0; i < total; i++){
      freq[i] = -440.f - 5.f * (float)(i % 7);
      off[i] = (i % 2 == 0) ? (1000000.25f + 0.1f * (float)i)
                            : (-7777.5f - 0.2f * (float)i);
    }

    SineWave sine;
    sine.freq(SequenceGen().setValues(freq)).phaseOffset(SequenceGen().setValues(off));
    std::vector<float> got = runBlocks(sine, blocks);
    std::vector<float> ref = referenceOsc(sineTable, sineN, freq, off);
    float d = maxAbsDiff(got, ref);
    printf("7. negative freq / large offsets    maxDiff=%g\n", d);
    check(d < 2e-6f, "negative frequency and huge offsets must match reference");
  }

  // =======================================================================
  // 8. NaN and infinity offsets affect only their frame; carrier survives.
  // =======================================================================
  {
    const float nanf = std::numeric_limits<float>::quiet_NaN();
    const float inff = std::numeric_limits<float>::infinity();
    std::vector<float> freq(total, 220.f);
    std::vector<float> off(total, 0.f), sanitized(total, 0.f);
    for (unsigned i = 10; i < 20; i++){ off[i] = nanf; }
    for (unsigned i = 30; i < 35; i++){ off[i] = -inff; }
    off[40] = inff; off[41] = nanf;

    SineWave sine, clean;
    sine.freq(SequenceGen().setValues(freq)).phaseOffset(SequenceGen().setValues(off));
    clean.freq(SequenceGen().setValues(freq));   // always offset 0

    std::vector<float> got = runBlocks(sine, blocks);
    std::vector<float> cleanGot = runBlocks(clean, blocks);
    std::vector<float> ref = referenceOsc(sineTable, sineN, freq, sanitized);

    for (size_t i = 0; i < total; i++){
      check(std::isfinite(got[i]), "NaN/Inf offset must never produce NaN output");
      check(got[i] == cleanGot[i], "bad frames treated as zero must leave the carrier untouched");
    }
    float d = maxAbsDiff(got, ref);
    printf("8. NaN/Inf recovery                  maxDiff=%g\n", d);
    check(d == 0.f, "NaN/Inf frames must be handled as zero-offset frames");
  }

  // =======================================================================
  // 9. Stereo modulation input is averaged to mono; oscillator stays mono.
  // =======================================================================
  {
    std::vector<float> freq(total, 176.f);
    std::vector<float> l(total), r(total), monoOff(total);
    for (size_t i = 0; i < total; i++){
      l[i] = 0.1f + 0.01f * (float)(i % 5);
      r[i] = -0.3f + 0.02f * (float)(i % 7);
      monoOff[i] = 0.5f * (l[i] + r[i]);
    }

    SequenceGen stereo;
    stereo.setStereo(l, r);
    SineWave sine;
    sine.freq(SequenceGen().setValues(freq)).phaseOffset(stereo);

    TonicFrames frames(BS, 1);
    SynthesisContext_ ctx;
    std::vector<float> got;
    for (unsigned b = 0; b < blocks; b++){
      sine.tick(frames, ctx);
      check(frames.channels() == 1, "oscillator output must remain mono");
      for (unsigned i = 0; i < BS; i++) got.push_back(frames[i]);
      ctx.tick();
    }
    std::vector<float> ref = referenceOsc(sineTable, sineN, freq, monoOff);
    float d = maxAbsDiff(got, ref);
    printf("9. stereo downmix                    maxDiff=%g\n", d);
    check(d < 2e-6f, "stereo phase input must be averaged to mono");
  }

  // =======================================================================
  // 10. Shared stateful dependency feeds freq, phaseOffset and a side branch
  //     in one context: computed once, all branches see the same raw samples,
  //     and the freq rate scaling must not pollute the other branches.
  // =======================================================================
  {
    CountGen shared;
    TableLookupOsc osc;
    osc.setLookupTable(five).freq(shared).phaseOffset(shared);

    std::vector<float> got;
    std::vector<float> sideSamples;
    TonicFrames frames(BS, 1), sideFrames(BS, 1);
    SynthesisContext_ ctx;
    ctx.tick();                                    // exercise the forceNewOutput=false path
    for (unsigned b = 0; b < blocks; b++){
      osc.tick(frames, ctx);
      shared.tick(sideFrames, ctx);                 // side branch, same context
      check(shared.count() == (long)(b + 1), "shared generator must compute once per block");
      for (unsigned i = 0; i < BS; i++){
        got.push_back(frames[i]);
        sideSamples.push_back(sideFrames[i]);
      }
      ctx.tick();
    }

    // reference: in block b (1-based) every raw sample equals b
    std::vector<float> freq(total), off(total);
    for (unsigned b = 0; b < blocks; b++){
      for (unsigned i = 0; i < BS; i++){
        freq[b * BS + i] = (float)(b + 1);
        off[b * BS + i] = (float)(b + 1);
      }
    }
    std::vector<float> ref = referenceOsc(std::vector<float>(five.dataPointer(),
                                                              five.dataPointer() + 5),
                                          fiveN, freq, off);
    float d = maxAbsDiff(got, ref);
    bool sideRaw = true;
    for (unsigned b = 0; b < blocks; b++)
      for (unsigned i = 0; i < BS; i++)
        if (sideSamples[b * BS + i] != (float)(b + 1)) sideRaw = false;
    printf("10. shared dependency                maxDiff=%g rawSideSamples=%s computes=%ld\n",
           d, sideRaw ? "ok" : "POLLUTED", shared.count());
    check(d < 1e-6f, "all three branches must use the same shared input samples");
    check(sideRaw, "freq rate conversion must not pollute the shared raw samples");
    check(shared.count() == (long)blocks, "shared generator computed the wrong number of times");
  }

  // =======================================================================
  // 11. ControlGenerator phase input updates at control-block granularity
  //     (one value per 64-sample block).
  // =======================================================================
  {
    BlockCtrl ctrl;
    SineWave sine;
    sine.freq(88.f).phaseOffset(ctrl);             // ControlGenerator overload

    TonicFrames frames(BS, 1);
    SynthesisContext_ ctx;
    std::vector<float> got;
    std::vector<float> freq(total), off(total);
    for (unsigned b = 0; b < blocks; b++){
      sine.tick(frames, ctx);
      check(ctrl.count() == (long)(b + 1), "control generator must update once per block");
      for (unsigned i = 0; i < BS; i++){
        got.push_back(frames[i]);
        freq[b * BS + i] = 88.f;
        off[b * BS + i] = (float)(b + 1);         // constant across the whole block
      }
      ctx.tick();
    }
    std::vector<float> ref = referenceOsc(sineTable, sineN, freq, off);
    float d = maxAbsDiff(got, ref);
    printf("11. control-block granularity        maxDiff=%g controlUpdates=%ld\n", d, ctrl.count());
    check(d < 2e-6f, "ControlGenerator phase input must hold for the whole block");
  }

  // =======================================================================
  // 12. float / ControlGenerator / Generator overloads agree, and setters
  //     return the oscillator for chained calls.
  // =======================================================================
  {
    std::vector<float> off(total, 0.3f), freq(total, 110.f);

    SineWave a; a.freq(110.f).phaseOffset(0.3f);
    SineWave b; b.freq(ControlValue(110.f)).phaseOffset(ControlValue(0.3f));
    SineWave c; c.freq(FixedValue(110.f)).phaseOffset(FixedValue(0.3f));

    std::vector<float> ga = runBlocks(a, blocks);
    std::vector<float> gb = runBlocks(b, blocks);
    std::vector<float> gc = runBlocks(c, blocks);
    printf("12. setter overloads                 d(float,ctrl)=%g d(float,gen)=%g\n",
           maxAbsDiff(ga, gb), maxAbsDiff(ga, gc));
    check(maxAbsDiff(ga, gb) == 0.f, "float and ControlGenerator overloads must agree");
    check(maxAbsDiff(ga, gc) == 0.f, "float and Generator overloads must agree");
  }

  // =======================================================================
  // 13. forceNewOutput semantics preserved: DummyContext forces a recompute;
  //     re-ticking the same normal context reuses cached output.
  // =======================================================================
  {
    CountGen g;
    TableLookupOsc osc;
    osc.setLookupTable(five).freq(0.f).phaseOffset(g);

    TonicFrames f1(BS, 1), f2(BS, 1), f3(BS, 1);
    osc.tick(f1, DummyContext);
    osc.tick(f2, DummyContext);
    check(g.count() == 2, "DummyContext must force a fresh computation each time");

    SynthesisContext_ ctx;
    ctx.tick();                                   // forceNewOutput becomes false
    osc.tick(f1, ctx);
    long before = g.count();
    osc.tick(f2, ctx);                              // same context: cached
    check(g.count() == before, "same context must reuse the cached block");
    ctx.tick();
    osc.tick(f3, ctx);
    check(g.count() == before + 1, "a new context frame must compute once");
    printf("13. forceNewOutput semantics         count=%ld\n", g.count());
  }

  // =======================================================================
  // 14. SampleTable swap and Generator-handle copy keep shared relationships.
  // =======================================================================
  {
    SampleTable other(5, 1);
    const float otherVals[] = {0, 2, 0, -2, 0};
    for (unsigned i = 0; i < 5; i++) other.dataPointer()[i] = otherVals[i];

    TableLookupOsc osc;
    osc.setLookupTable(five).freq(0.f).phaseOffset(0.25f);
    std::vector<float> before = runBlocks(osc, 1);
    check(fabsf(before[0] - 1.f) < 1e-6f, "five-table quarter offset reads 1");

    osc.setLookupTable(other);
    std::vector<float> after = runBlocks(osc, 1);
    check(fabsf(after[0] - 2.f) < 1e-6f, "swapped table must be used immediately");

    TableLookupOsc copy = osc;                      // shared internal generator
    SequenceGen off; off.setValues(std::vector<float>(total, 0.1f));
    osc.phaseOffset(off);
    TonicFrames fA(BS, 1), fB(BS, 1);
    SynthesisContext_ ctx;
    osc.tick(fA, ctx);
    copy.tick(fB, ctx);                            // same context, shared cached block
    float d = 0.f;
    for (unsigned i = 0; i < BS; i++) d = std::max(d, fabsf(fA[i] - fB[i]));
    printf("14. table swap / handle sharing      sharedDiff=%g\n", d);
    check(d == 0.f, "copied handles must share the same generator and phase input");
  }

  // =======================================================================
  // 15. Real BufferFiller chain: one large request vs concatenated 1/7/31
  //     frame requests must produce identical PCM (max abs error 2e-5).
  // =======================================================================
  {
    const unsigned totalFrames = 400;
    std::vector<float> loopPattern(totalFrames);
    for (unsigned i = 0; i < totalFrames; i++)
      loopPattern[i] = 0.4f * sinf(0.05f * (float)i) + 0.05f * (float)(i % 3);

    PhaseDemoSynth big(makePhaseChain(loopPattern));
    PhaseDemoSynth chunked(makePhaseChain(loopPattern));

    std::vector<float> pcmBig(totalFrames, 0.f);
    big.fillBufferOfFloats(&pcmBig[0], totalFrames, 1);

    std::vector<float> pcmChunk;
    pcmChunk.reserve(totalFrames);
    const unsigned chunkSizes[3] = {1, 7, 31};
    unsigned produced = 0, k = 0;
    while (produced < totalFrames){
      unsigned n = std::min(chunkSizes[k % 3], totalFrames - produced);
      std::vector<float> piece(n, 0.f);
      chunked.fillBufferOfFloats(&piece[0], n, 1);
      for (unsigned i = 0; i < n; i++) pcmChunk.push_back(piece[i]);
      produced += n;
      k++;
    }

    check(pcmChunk.size() == pcmBig.size(), "chunked fill must produce the same frame count");
    float d = maxAbsDiff(pcmBig, pcmChunk);
    printf("15. BufferFiller chunk equivalence   maxDiff=%g over %u frames\n", d, totalFrames);
    check(d <= 2e-5f, "chunked and one-shot BufferFiller output must match within 2e-5");
  }

  printf("PHASE_MODULATION_OK\n");
  return 0;
}
