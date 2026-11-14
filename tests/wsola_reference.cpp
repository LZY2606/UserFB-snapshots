////////////////////////////////////////////////////////////////////////////////
///
/// Independent numerical regression for the floating-point scalar WSOLA
/// time-stretch implementation (soundtouch::TDStretch).
///
/// The reference used by every check below is a standalone, natural-frame-
/// order double-precision model that:
///   - re-accumulates the full all-channel inner product and candidate energy
///     for every seek candidate with direct sums,
///   - applies the centre-preference weighting exactly as defined by the
///     current fixed source (candidate 0 is entered with the raw normalized
///     correlation; candidates 1..seekLength-1 get the heuristic factor),
///   - rebuilds the linear cross-fade of the previous tail window and the new
///     window, the pass-through middle and the retained next tail window,
///   - maintains the fractional nominalSkip clock independently in double.
///
/// The model never calls the production search, overlap or correlation
/// accumulation routines and does not include TDStretch.cpp. Production code
/// is exercised only as the system under test; a test-only derived class is
/// used solely to load or observe protected state.
///
////////////////////////////////////////////////////////////////////////////////

#include "task_support/check.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <vector>

#include "SoundTouch.h"
#include "TDStretch.h"

using namespace soundtouch;

namespace {

static const double kPi = 3.14159265358979323846;

// --- fixed processing geometry (ms values pinned for every case) ------------
static const int kSequenceMs = 40;
static const int kSeekWindowMs = 15;
static const int kOverlapMs = 8;

// --- numerical tolerances ---------------------------------------------------
// The cross-fade is the only float-vs-double reordered summation path;
// observed divergence stays below ~6e-6 on the chosen inputs. A wrong window
// choice moves the output by O(1e-1), so this bound is tight while not
// depending on the host machine.
static const double kBlendTol = 1.0e-5;
// Scores on the deterministic inputs are separated by far more than this
// after the first (zero-mid) window, so no check depends on float ties.
static const double kMinScoreMargin = 1.0e-5;

// ---------------------------------------------------------------------------
// Deterministic pseudo-random source (LCG), independent of the library.
// ---------------------------------------------------------------------------
struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed ? seed : 1u) {}
    float next()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int32_t>(state >> 8)) / 8388608.0f;
    }
};

static std::vector<float> makeNoiseFrames(int frames, int channels, uint32_t seed, float amplitude)
{
    std::vector<float> v(static_cast<size_t>(frames) * channels);
    Rng r(seed);
    for (float &x : v) x = r.next() * amplitude;
    return v;
}

// ---------------------------------------------------------------------------
// Failure detail helper: TASK_CHECK still performs the registered assertion;
// the detail line names the earliest concrete divergence.
// ---------------------------------------------------------------------------
static bool expectTrue(bool cond, const char *msg)
{
    if (!cond) std::fprintf(stderr, "  FIRST_DIFF %s\n", msg);
    return cond;
}

static bool expectTrueF(bool cond, const char *fmt, ...)
{
    if (!cond) {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "  FIRST_DIFF %s\n", buf);
    }
    return cond;
}

// ---------------------------------------------------------------------------
// Production observer: a test-only derived class that may load/inspect the
// protected search state. It contains no reference math.
// ---------------------------------------------------------------------------
class ObservableTDStretch : public TDStretch
{
public:
    void configure(int numChannels, int sampleRate, float tempoValue)
    {
        setChannels(numChannels);
        setParameters(sampleRate, kSequenceMs, kSeekWindowMs, kOverlapMs);
        setTempo(tempoValue);
        enableQuickSeek(FALSE);
    }

    int overlapLengthValue() const { return overlapLength; }
    int seekLengthValue() const { return seekLength; }
    int windowLengthValue() const { return seekWindowLength; }
    int sampleReqValue() const { return sampleReq; }
    double nominalSkipValue() const { return static_cast<double>(nominalSkip); }
    double skipFractionValue() const { return static_cast<double>(skipFract); }

    void loadMidBuffer(const std::vector<float> &values)
    {
        for (size_t i = 0; i < values.size(); ++i) pMidBuffer[i] = values[i];
    }

    float midBufferValue(int index) const { return pMidBuffer[index]; }

    bool midBufferIsZero() const
    {
        const int count = channels * overlapLength;
        for (int i = 0; i < count; ++i)
            if (pMidBuffer[i] != 0.0f) return false;
        return true;
    }

    void loadInput(const std::vector<float> &frames)
    {
        getInput()->putSamples(frames.data(), static_cast<uint>(frames.size() / channels));
    }

    int runFullSeek()
    {
        return seekBestOverlapPositionFull(getInput()->ptrBegin());
    }

    const std::vector<int> &recordedOffsets() const { return offsets; }
    int inputFrames() { return static_cast<int>(getInput()->numSamples()); }

    // Per-round integer skips reconstructed purely from observable FIFO
    // bookkeeping: at seek k the cumulative consumption is (puts - input
    // level); consecutive differences are the rounds' integer skips.
    std::vector<int> buildSkipHistory()
    {
        std::vector<int> result;
        const int finalConsumed = cumulativePut - inputFrames();
        for (size_t k = 1; k < consumedAtSeek.size(); ++k)
            result.push_back(consumedAtSeek[k] - consumedAtSeek[k - 1]);
        if (!consumedAtSeek.empty())
            result.push_back(finalConsumed - consumedAtSeek.back());
        return result;
    }

    void putSamples(const SAMPLETYPE *samples, uint nSamples) override
    {
        cumulativePut += static_cast<int>(nSamples);
        TDStretch::putSamples(samples, nSamples);
    }

    // Production clear() drops the audio FIFOs while keeping skipFract; the
    // observer's own per-run bookkeeping must restart so a post-clear stream is
    // compared against only the post-clear reference rounds.
    void clear() override
    {
        TDStretch::clear();
        offsets.clear();
        consumedAtSeek.clear();
        cumulativePut = 0;
    }

protected:
    int seekBestOverlapPositionFull(const SAMPLETYPE *refPos) override
    {
        const int offset = TDStretch::seekBestOverlapPositionFull(refPos);
        offsets.push_back(offset);
        consumedAtSeek.push_back(cumulativePut - inputFrames());
        return offset;
    }

private:
    std::vector<int> offsets;
    std::vector<int> consumedAtSeek;
    int cumulativePut = 0;
};

// ===========================================================================
// Independent double-precision WSOLA reference
// ===========================================================================
namespace reference {

// One fully evaluated processing round, expressed in input frame coordinates.
struct Round {
    int offset = 0;                 // chosen seek candidate
    int skip = 0;                   // integer frames consumed this round
    std::vector<float> pcm;         // win-ovl output frames (cross-fade + middle)
    std::vector<double> nextMid;    // ovl retained tail frames for next round
};

// Direct-sum normalized cross correlation of one candidate window against the
// retained mid window, summed over all channels independently of the
// production accumulation order. Returns (corr, energy).
static void candidateSums(const std::deque<float> &ring, int ringChannels,
                          const std::vector<double> &mid,
                          int candidate, int ovl,
                          double &corr, double &energy)
{
    corr = 0.0;
    energy = 0.0;
    for (int t = 0; t < ovl; ++t) {
        for (int c = 0; c < ringChannels; ++c) {
            const double a = static_cast<double>(ring[static_cast<size_t>(candidate + t) * ringChannels + c]);
            const double b = mid[static_cast<size_t>(t) * ringChannels + c];
            corr += a * b;
            energy += a * a;
        }
    }
}

// Centre-preference factor used by the fixed source for candidates >= 1.
static double centreWeight(int candidate, int seek)
{
    const double tmp = static_cast<double>(2 * candidate - seek) / static_cast<double>(seek);
    return 1.0 - 0.25 * tmp * tmp;
}

// Full natural-order search. Candidate 0 is deliberately scored WITHOUT the
// heuristic (matching seekBestOverlapPositionFull, which stores the candidate-0
// correlation straight into bestCorr); every later candidate is weighted.
// Selection uses a strict greater-than comparison, so exact ties keep the
// lower index. With all-zero energy the weighted zero scores of candidates >= 1
// still peak at the centre candidate, which is therefore the deterministic
// result rather than an arbitrary index.
static int seekFull(const std::deque<float> &ring, int channels,
                    const std::vector<double> &mid,
                    int ovl, int seek, std::vector<double> *scoresOut = nullptr)
{
    int best = 0;
    double bestScore = 0.0;
    if (scoresOut) scoresOut->assign(seek, 0.0);

    for (int candidate = 0; candidate < seek; ++candidate) {
        double corr, energy;
        candidateSums(ring, channels, mid, candidate, ovl, corr, energy);
        const double normalized = corr / std::sqrt(energy < 1e-9 ? 1.0 : energy);

        double score = normalized;
        if (candidate != 0) score = (normalized + 0.1) * centreWeight(candidate, seek);

        if (scoresOut) (*scoresOut)[candidate] = score;
        if (candidate == 0) {
            bestScore = score;
        } else if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

// Same direct sums restricted to the first 'usedChannels' channels, used to
// prove that a single-channel listener would choose different positions.
static int seekFullRestricted(const std::deque<float> &ring, int channels,
                              const std::vector<double> &mid,
                              int ovl, int seek, int usedChannels)
{
    int best = 0;
    double bestScore = 0.0;
    for (int candidate = 0; candidate < seek; ++candidate) {
        double corr = 0.0, energy = 0.0;
        for (int t = 0; t < ovl; ++t) {
            for (int c = 0; c < usedChannels; ++c) {
                const double a = static_cast<double>(ring[static_cast<size_t>(candidate + t) * channels + c]);
                const double b = mid[static_cast<size_t>(t) * channels + c];
                corr += a * b;
                energy += a * a;
            }
        }
        double score = corr / std::sqrt(energy < 1e-9 ? 1.0 : energy);
        if (candidate != 0) score = (score + 0.1) * centreWeight(candidate, seek);
        if (candidate == 0) bestScore = score;
        else if (score > bestScore) { bestScore = score; best = candidate; }
    }
    return best;
}

class Model
{
public:
    void configure(int numChannels, int ovl, int window, int seek, int req, double skip)
    {
        ch = numChannels;
        overlap = ovl;
        windowLength = window;
        seekLength = seek;
        sampleReq = req;
        nominalSkip = skip;
        ring.clear();
        mid.assign(static_cast<size_t>(ch) * overlap, 0.0);
        skipFract = 0.0;
        rounds.clear();
        output.clear();
        minMarginAfterFirst = 1.0e300;
    }

    // clear() in the fixed source drops the audio history (input, mid and
    // output FIFO) but leaves skipFract untouched.
    void clearAudio()
    {
        ring.clear();
        std::fill(mid.begin(), mid.end(), 0.0);
        rounds.clear();
        output.clear();
    }

    void appendFrames(const float *frames, int frameCount)
    {
        for (int i = 0; i < frameCount * ch; ++i) ring.push_back(frames[i]);
    }

    void setMid(const std::vector<float> &values)
    {
        mid.assign(values.begin(), values.end());
    }

    // Run every processing round currently enabled by the input level.
    void step()
    {
        while (static_cast<int>(ring.size()) / ch >= sampleReq) {
            std::vector<double> scores;
            const int offset = seekFull(ring, ch, mid, overlap, seekLength, &scores);

            double second = -1.0e300;
            for (int i = 0; i < seekLength; ++i) {
                if (i != offset && scores[i] > second) second = scores[i];
            }
            if (!rounds.empty()) minMarginAfterFirst = std::min(minMarginAfterFirst, scores[offset] - second);

            Round round;
            round.offset = offset;
            const int middleLength = windowLength - 2 * overlap;
            round.pcm.resize(static_cast<size_t>(windowLength - overlap) * ch);

            // Linear cross-fade of the retained tail (weight 1 -> 0) and the
            // newly chosen window (weight 0 -> 1).
            for (int t = 0; t < overlap; ++t) {
                const double wNew = static_cast<double>(t) / overlap;
                const double wOld = static_cast<double>(overlap - t) / overlap;
                for (int c = 0; c < ch; ++c) {
                    const double a = ring[static_cast<size_t>(offset + t) * ch + c];
                    const double b = mid[static_cast<size_t>(t) * ch + c];
                    round.pcm[static_cast<size_t>(t) * ch + c] = static_cast<float>(a * wNew + b * wOld);
                }
            }
            // Pass-through middle, copied verbatim by the production code.
            for (int i = 0; i < middleLength * ch; ++i) {
                round.pcm[static_cast<size_t>(overlap) * ch + i] =
                    ring[static_cast<size_t>(offset + overlap) * ch + i];
            }
            // Retained tail window for the next round.
            round.nextMid.resize(static_cast<size_t>(overlap) * ch);
            for (int i = 0; i < overlap * ch; ++i) {
                round.nextMid[i] =
                    static_cast<double>(ring[static_cast<size_t>(offset + middleLength + overlap) * ch + i]);
            }

            // Independent fractional skip clock (double accumulation).
            skipFract += nominalSkip;
            const int integerSkip = static_cast<int>(skipFract);
            skipFract -= integerSkip;
            round.skip = integerSkip;

            for (int i = 0; i < integerSkip * ch; ++i) ring.pop_front();
            mid = round.nextMid;

            output.insert(output.end(), round.pcm.begin(), round.pcm.end());
            rounds.push_back(std::move(round));
        }
    }

    int ringFrames() const { return static_cast<int>(ring.size()) / ch; }
    double fraction() const { return skipFract; }

    int ch = 0, overlap = 0, windowLength = 0, seekLength = 0, sampleReq = 0;
    double nominalSkip = 0.0, skipFract = 0.0;
    std::deque<float> ring;
    std::vector<double> mid;
    std::vector<Round> rounds;
    std::vector<float> output;
    double minMarginAfterFirst = 1.0e300;
};

} // namespace reference

// ---------------------------------------------------------------------------
// Shared comparison helpers
// ---------------------------------------------------------------------------
static long firstFloatMismatch(const std::vector<float> &a, const std::vector<float> &b, double tol)
{
    const long n = static_cast<long>(std::min(a.size(), b.size()));
    for (long i = 0; i < n; ++i) {
        if (std::abs(static_cast<double>(a[i]) - b[i]) > tol) return i;
    }
    return a.size() == b.size() ? -1 : n;
}

static long firstIntMismatch(const std::vector<int> &a, const std::vector<int> &b)
{
    const long n = static_cast<long>(std::min(a.size(), b.size()));
    for (long i = 0; i < n; ++i) if (a[i] != b[i]) return i;
    return a.size() == b.size() ? -1 : n;
}

static bool allFinite(const std::vector<float> &v)
{
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

// ===========================================================================
// search group: independent full-search position and scoring behaviour
// ===========================================================================

static int search_random_position()
{
    ObservableTDStretch td;
    td.configure(2, 48000, 1.0f);
    const int ch = 2, ovl = td.overlapLengthValue(), seek = td.seekLengthValue(), win = td.windowLengthValue();

    const std::vector<float> mid = makeNoiseFrames(ovl, ch, /*seed*/ 114001u, 0.5f);
    const std::vector<float> input = makeNoiseFrames(win + seek, ch, 114002u, 0.5f);
    td.loadMidBuffer(mid);
    td.loadInput(input);

    std::deque<float> ring(input.begin(), input.end());
    std::vector<double> refMid(mid.begin(), mid.end());
    std::vector<double> scores;
    const int refPosition = reference::seekFull(ring, ch, refMid, ovl, seek, &scores);
    const int productionPosition = td.runFullSeek();

    TASK_CHECK(expectTrueF(productionPosition == refPosition,
                           "random stereo position production=%d reference=%d",
                           productionPosition, refPosition));

    double second = -1.0e300;
    for (int i = 0; i < seek; ++i) if (i != refPosition) second = std::max(second, scores[i]);
    const double margin = scores[refPosition] - second;
    TASK_CHECK(expectTrueF(margin > kMinScoreMargin, "random stereo score margin too small %.3e", margin));

    // A first-channel-only listener must reach a different decision on this
    // multi-channel non-periodic material.
    const int firstChannelOnly = reference::seekFullRestricted(ring, ch, refMid, ovl, seek, 1);
    TASK_CHECK(expectTrueF(firstChannelOnly != refPosition,
                           "channel-0-only position %d equals joint position %d",
                           firstChannelOnly, refPosition));
    return 0;
}

static int search_six_channel_joint()
{
    ObservableTDStretch td;
    td.configure(6, 44100, 1.3f);
    const int ch = 6, ovl = td.overlapLengthValue(), seek = td.seekLengthValue(), win = td.windowLengthValue();

    const std::vector<float> mid = makeNoiseFrames(ovl, ch, 224001u, 0.5f);
    const std::vector<float> input = makeNoiseFrames(win + seek, ch, 224002u, 0.5f);
    td.loadMidBuffer(mid);
    td.loadInput(input);

    std::deque<float> ring(input.begin(), input.end());
    std::vector<double> refMid(mid.begin(), mid.end());
    std::vector<double> scores;
    const int refPosition = reference::seekFull(ring, ch, refMid, ovl, seek, &scores);
    const int productionPosition = td.runFullSeek();

    TASK_CHECK(expectTrueF(productionPosition == refPosition,
                           "six-channel position production=%d reference=%d",
                           productionPosition, refPosition));
    double second = -1.0e300;
    for (int i = 0; i < seek; ++i) if (i != refPosition) second = std::max(second, scores[i]);
    TASK_CHECK(expectTrueF(scores[refPosition] - second > kMinScoreMargin,
                           "six-channel margin %.3e", scores[refPosition] - second));

    const int firstChannelOnly = reference::seekFullRestricted(ring, ch, refMid, ovl, seek, 1);
    TASK_CHECK(expectTrue(firstChannelOnly != refPosition, "six-channel channel-0 decision not independent"));
    return 0;
}

// All-zero energy has a deterministic outcome even though no signal exists:
// candidate 0 keeps its raw normalized correlation (0.0), while every later
// candidate is scored as (0.0 + 0.1) * centreWeight, whose strict maximum sits
// at the centre candidate seek/2. Both the fixed source and the reference must
// therefore converge on seek/2 rather than an implementation-dependent value.
static int search_zero_energy()
{
    ObservableTDStretch td;
    td.configure(2, 48000, 1.0f);
    const int ch = 2, ovl = td.overlapLengthValue(), seek = td.seekLengthValue(), win = td.windowLengthValue();

    const std::vector<float> mid(static_cast<size_t>(ovl) * ch, 0.0f);
    const std::vector<float> input(static_cast<size_t>(win + seek) * ch, 0.0f);
    td.loadMidBuffer(mid);
    td.loadInput(input);

    std::deque<float> ring(input.begin(), input.end());
    std::vector<double> refMid(mid.begin(), mid.end());
    const int refPosition = reference::seekFull(ring, ch, refMid, ovl, seek);
    const int productionPosition = td.runFullSeek();

    TASK_CHECK(expectTrueF(refPosition == seek / 2,
                           "reference zero-energy position %d != centre %d", refPosition, seek / 2));
    TASK_CHECK(expectTrueF(productionPosition == seek / 2,
                           "production zero-energy position %d != centre %d", productionPosition, seek / 2));
    return 0;
}

// Candidate 0 uses the raw normalized correlation while later candidates are
// weighted. Build a case where weighting candidate 0 as well would flip the
// decision, proving the reference follows the fixed source rather than a
// generic "weight everything" rule.
static int search_candidate_zero_quirk()
{
    ObservableTDStretch td;
    td.configure(2, 48000, 1.0f);
    const int ch = 2, ovl = td.overlapLengthValue(), seek = td.seekLengthValue(), win = td.windowLengthValue();

    Rng r(99u);
    std::vector<float> mid(static_cast<size_t>(ovl) * ch);
    for (float &x : mid) x = r.next() * 0.8f;

    std::vector<float> input(static_cast<size_t>(win + seek) * ch);
    for (float &x : input) x = r.next() * 0.03f;
    // Exact match at candidate 0 ...
    for (int i = 0; i < ovl * ch; ++i) input[i] = mid[i];
    // ... and a noisier, lower-normalized-correlation copy at the centre, which
    // the centre preference would promote if candidate 0 were weighted too.
    const int planted = seek / 2;
    for (int t = 0; t < ovl; ++t) {
        for (int c = 0; c < ch; ++c) {
            input[static_cast<size_t>(planted + t) * ch + c] =
                mid[static_cast<size_t>(t) * ch + c] + 0.5f * r.next();
        }
    }
    td.loadMidBuffer(mid);
    td.loadInput(input);

    std::deque<float> ring(input.begin(), input.end());
    std::vector<double> refMid(mid.begin(), mid.end());
    const int fixedRulePosition = reference::seekFull(ring, ch, refMid, ovl, seek);

    // Alternative (wrong) rule: apply the heuristic to candidate 0 as well.
    int weightedAllPosition = 0;
    double weightedAllBest = 0.0;
    for (int candidate = 0; candidate < seek; ++candidate) {
        double corr, energy;
        reference::candidateSums(ring, ch, refMid, candidate, ovl, corr, energy);
        const double normalized = corr / std::sqrt(energy < 1e-9 ? 1.0 : energy);
        const double score = (normalized + 0.1) * reference::centreWeight(candidate, seek);
        if (candidate == 0) weightedAllBest = score;
        else if (score > weightedAllBest) { weightedAllBest = score; weightedAllPosition = candidate; }
    }
    const int productionPosition = td.runFullSeek();

    TASK_CHECK(expectTrue(fixedRulePosition == 0, "fixed-rule candidate-0 quirk position not 0"));
    TASK_CHECK(expectTrueF(weightedAllPosition != 0,
                           "weight-all rule also picked 0, quirk case not discriminating (picked %d)",
                           weightedAllPosition));
    TASK_CHECK(expectTrueF(productionPosition == fixedRulePosition,
                           "production %d diverges from fixed rule %d",
                           productionPosition, fixedRulePosition));
    return 0;
}

// ===========================================================================
// overlap group: independently chosen position drives a sample-exact rebuild
// ===========================================================================

struct OverlapFixture {
    int sampleRate;
    float tempo;
    int ch;
    int ovl, win, seek, req;
    std::vector<float> mid;
    std::vector<float> input;
};

static OverlapFixture makeFixture(int channels, int sampleRate, float tempo, uint32_t seed, bool zeroMid)
{
    ObservableTDStretch probe;
    probe.configure(channels, sampleRate, tempo);
    OverlapFixture f;
    f.sampleRate = sampleRate;
    f.tempo = tempo;
    f.ch = channels;
    f.ovl = probe.overlapLengthValue();
    f.win = probe.windowLengthValue();
    f.seek = probe.seekLengthValue();
    f.req = probe.sampleReqValue();
    f.mid = zeroMid ? std::vector<float>(static_cast<size_t>(f.ovl) * channels, 0.0f)
                    : makeNoiseFrames(f.ovl, channels, seed, 0.5f);
    f.input = makeNoiseFrames(f.req, channels, seed + 1u, 0.5f);
    return f;
}

static int overlap_case(const OverlapFixture &f)
{
    ObservableTDStretch td;
    td.configure(f.ch, f.sampleRate, f.tempo);
    TASK_CHECK(expectTrue(td.overlapLengthValue() == f.ovl && td.seekLengthValue() == f.seek &&
                              td.windowLengthValue() == f.win && td.sampleReqValue() == f.req,
                          "fixture geometry mismatch"));
    td.loadMidBuffer(f.mid);
    // Go through the real entry point so processSamples() runs the search,
    // overlap and fractional-skip bookkeeping rather than merely filling the
    // input FIFO.
    td.putSamples(f.input.data(), f.req);

    reference::Model model;
    model.configure(f.ch, f.ovl, f.win, f.seek, f.req, td.nominalSkipValue());
    model.setMid(f.mid);
    model.appendFrames(f.input.data(), f.req);
    model.step();

    TASK_CHECK(expectTrue(model.rounds.size() == 1, "reference did not produce exactly one round"));
    const reference::Round &round = model.rounds.front();

    // Production must choose the independently selected position.
    TASK_CHECK(expectTrue(td.recordedOffsets().size() == 1, "production did not run one search"));
    TASK_CHECK(expectTrueF(td.recordedOffsets()[0] == round.offset,
                           "overlap offset production=%d reference=%d",
                           td.recordedOffsets()[0], round.offset));

    // Read the actual production PCM for the batch (win - ovl frames).
    std::vector<float> productionPcm(static_cast<size_t>(f.win - f.ovl) * f.ch);
    const uint received = td.receiveSamples(productionPcm.data(), f.win - f.ovl);
    TASK_CHECK(expectTrueF(received == static_cast<uint>(f.win - f.ovl),
                           "production batch size %u != %d", received, f.win - f.ovl));

    // Cross-fade region: tight float tolerance.
    long firstBad = -1;
    for (int i = 0; i < f.ovl * f.ch; ++i) {
        if (std::abs(static_cast<double>(productionPcm[i]) - round.pcm[i]) > kBlendTol) { firstBad = i; break; }
    }
    TASK_CHECK(expectTrueF(firstBad < 0, "cross-fade sample mismatch at index %ld", firstBad));

    // Pass-through middle must be bit-identical copies of the input frames.
    const int middleLength = f.win - 2 * f.ovl;
    firstBad = -1;
    for (int i = 0; i < middleLength * f.ch; ++i) {
        const float expected = f.input[static_cast<size_t>(round.offset + f.ovl) * f.ch + i];
        if (productionPcm[static_cast<size_t>(f.ovl) * f.ch + i] != expected) { firstBad = i; break; }
    }
    TASK_CHECK(expectTrueF(firstBad < 0, "pass-through middle mismatch at index %ld", firstBad));

    // Retained tail window must be bit-identical to the referenced input tail.
    firstBad = -1;
    for (int i = 0; i < f.ovl * f.ch; ++i) {
        const float expected = f.input[static_cast<size_t>(round.offset + middleLength + f.ovl) * f.ch + i];
        if (td.midBufferValue(i) != expected) { firstBad = i; break; }
    }
    TASK_CHECK(expectTrueF(firstBad < 0, "retained tail mismatch at index %ld", firstBad));

    // Consumption must match the independent fractional clock for one round.
    TASK_CHECK(expectTrueF(td.inputFrames() == f.req - round.skip,
                           "remaining input %d != %d", td.inputFrames(), f.req - round.skip));
    return 0;
}

static int overlap_first_round_all_channel_counts()
{
    // First round starts from a zeroed mid buffer and exercises the mono,
    // stereo and multi-channel overlap routines with the same reference model.
    for (int ch : {1, 2, 6}) {
        OverlapFixture f = makeFixture(ch, 48000, 1.0f, 301000u + ch * 97u, /*zeroMid*/ true);
        TASK_CHECK(overlap_case(f) == 0);
    }
    return 0;
}

static int overlap_stereo_dirty_mid()
{
    OverlapFixture f = makeFixture(2, 48000, 1.37f, 302000u, /*zeroMid*/ false);
    TASK_CHECK(overlap_case(f) == 0);
    return 0;
}

static int overlap_six_channel_dirty_mid()
{
    OverlapFixture f = makeFixture(6, 44100, 0.7f, 303000u, /*zeroMid*/ false);
    TASK_CHECK(overlap_case(f) == 0);
    return 0;
}

// ===========================================================================
// stream group: putSamples/receiveSamples against the independently clocked
// model, including fragmentation, clear() semantics and public API wiring.
// ===========================================================================

struct StreamConfig {
    int sampleRate;
    int channels;
    float tempo;
    uint32_t seed;
};

struct StreamRun {
    std::vector<float> output;
    std::vector<int> offsets;
    std::vector<int> skips;
    std::vector<int> consumedAfterPut;
    int finalInputRemaining = 0;
    double finalFraction = 0.0;
};

static void runModelStream(const StreamConfig &cfg, const std::vector<float> &input,
                           const int *putPattern, int patternLength,
                           reference::Model &model, StreamRun &run)
{
    int total = static_cast<int>(input.size()) / cfg.channels;
    int pos = 0, putIndex = 0;
    while (pos < total) {
        const int chunk = std::min(putPattern[putIndex % patternLength], total - pos);
        ++putIndex;
        model.appendFrames(input.data() + static_cast<size_t>(pos) * cfg.channels, chunk);
        pos += chunk;
        model.step();
        run.consumedAfterPut.push_back(pos - model.ringFrames());
    }
    run.output = model.output;
    run.finalInputRemaining = model.ringFrames();
    run.finalFraction = model.fraction();
    for (const reference::Round &r : model.rounds) {
        run.offsets.push_back(r.offset);
        run.skips.push_back(r.skip);
    }
}

static void runProductionStream(const StreamConfig &cfg, const std::vector<float> &input,
                                const int *putPattern, int putPatternLength,
                                const int *readPattern, int readPatternLength,
                                ObservableTDStretch &td, StreamRun &run)
{
    int total = static_cast<int>(input.size()) / cfg.channels;
    int pos = 0, putIndex = 0, readIndex = 0;
    std::vector<float> buf(8192 * cfg.channels);

    while (pos < total) {
        const int chunk = std::min(putPattern[putIndex % putPatternLength], total - pos);
        ++putIndex;
        td.putSamples(input.data() + static_cast<size_t>(pos) * cfg.channels, chunk);
        pos += chunk;

        // Alternate the read capacity; drain everything currently available.
        for (;;) {
            const int capacity = readPattern[readIndex % readPatternLength];
            ++readIndex;
            const uint got = td.receiveSamples(buf.data(), capacity);
            if (got == 0) break;
            run.output.insert(run.output.end(), buf.begin(), buf.begin() + got * cfg.channels);
        }
        run.consumedAfterPut.push_back(pos - td.inputFrames());
    }
    // Final drain without flush: no further input means no further rounds.
    for (;;) {
        const int capacity = readPattern[readIndex % readPatternLength];
        ++readIndex;
        const uint got = td.receiveSamples(buf.data(), capacity);
        if (got == 0) break;
        run.output.insert(run.output.end(), buf.begin(), buf.begin() + got * cfg.channels);
    }
    run.offsets = td.recordedOffsets();
    run.skips = td.buildSkipHistory();
    run.finalInputRemaining = td.inputFrames();
    run.finalFraction = td.skipFractionValue();
}

static int compareStreamRun(const char *tag, const StreamRun &production, const StreamRun &model)
{
    TASK_CHECK(expectTrueF(production.output.size() == model.output.size(),
                           "%s output size production=%zu reference=%zu",
                           tag, production.output.size(), model.output.size()));

    // Check the discrete window/clock histories before the bulk PCM scan so a
    // broken skip clock names the consumption divergence directly.
    const long offsetIndex = firstIntMismatch(production.offsets, model.offsets);
    TASK_CHECK(expectTrueF(offsetIndex < 0, "%s window offset mismatch at round %ld", tag, offsetIndex));

    const long skipIndex = firstIntMismatch(production.skips, model.skips);
    TASK_CHECK(expectTrueF(skipIndex < 0, "%s consumption skip mismatch at round %ld", tag, skipIndex));

    const long consumedIndex = firstIntMismatch(production.consumedAfterPut, model.consumedAfterPut);
    TASK_CHECK(expectTrueF(consumedIndex < 0, "%s consumed-history mismatch after put %ld", tag, consumedIndex));

    // With the chosen window and clock confirmed, verify every output sample.
    const long pcmIndex = firstFloatMismatch(production.output, model.output, kBlendTol);
    TASK_CHECK(expectTrueF(pcmIndex < 0, "%s PCM mismatch at sample %ld", tag, pcmIndex));
    TASK_CHECK(expectTrue(allFinite(production.output), "production output contains non-finite sample"));

    TASK_CHECK(expectTrueF(production.finalInputRemaining == model.finalInputRemaining,
                           "%s final input remaining %d != %d", tag,
                           production.finalInputRemaining, model.finalInputRemaining));
    TASK_CHECK(expectTrueF(std::abs(production.finalFraction - model.finalFraction) < 1.0e-9,
                           "%s final skip fraction %.9g != %.9g", tag,
                           production.finalFraction, model.finalFraction));
    return 0;
}

static int stream_configuration(const StreamConfig &cfg)
{
    ObservableTDStretch geometry;
    geometry.configure(cfg.channels, cfg.sampleRate, cfg.tempo);
    const int ovl = geometry.overlapLengthValue(), win = geometry.windowLengthValue();
    const int seek = geometry.seekLengthValue(), req = geometry.sampleReqValue();
    const double nominal = geometry.nominalSkipValue();

    // Long enough for well over 64 processing windows at every tempo.
    const int frames = 200000;
    const std::vector<float> input = makeNoiseFrames(frames, cfg.channels, cfg.seed, 0.6f);

    const int fragmentedPuts[3] = {1, 7, 113};
    const int blockPuts[1] = {frames};
    const int alternatingReads[5] = {5, 64, 1000, 37, 2048};
    const int singleReads[1] = {4096};

    reference::Model fragmentedModel;
    fragmentedModel.configure(cfg.channels, ovl, win, seek, req, nominal);
    StreamRun fragmentedModelRun;
    runModelStream(cfg, input, fragmentedPuts, 3, fragmentedModel, fragmentedModelRun);

    ObservableTDStretch fragmentedTd;
    fragmentedTd.configure(cfg.channels, cfg.sampleRate, cfg.tempo);
    StreamRun fragmentedProductionRun;
    runProductionStream(cfg, input, fragmentedPuts, 3, alternatingReads, 5,
                        fragmentedTd, fragmentedProductionRun);

    TASK_CHECK(expectTrueF(fragmentedModel.rounds.size() >= 64,
                           "only %zu processing windows, need >= 64", fragmentedModel.rounds.size()));
    TASK_CHECK(expectTrue(fragmentedModel.minMarginAfterFirst > kMinScoreMargin,
                          "score margin too small: checks would depend on float ties"));
    TASK_CHECK(compareStreamRun("fragmented", fragmentedProductionRun, fragmentedModelRun) == 0);

    // The same stream delivered in one block with fixed-size reads must give
    // exactly the same PCM as the fragmented delivery.
    reference::Model blockModel;
    blockModel.configure(cfg.channels, ovl, win, seek, req, nominal);
    StreamRun blockModelRun;
    runModelStream(cfg, input, blockPuts, 1, blockModel, blockModelRun);

    ObservableTDStretch blockTd;
    blockTd.configure(cfg.channels, cfg.sampleRate, cfg.tempo);
    StreamRun blockProductionRun;
    runProductionStream(cfg, input, blockPuts, 1, singleReads, 1, blockTd, blockProductionRun);
    TASK_CHECK(compareStreamRun("block", blockProductionRun, blockModelRun) == 0);

    const long blockVsFragmented = firstFloatMismatch(blockProductionRun.output,
                                                      fragmentedProductionRun.output, 0.0);
    TASK_CHECK(expectTrueF(blockVsFragmented < 0,
                           "block vs fragmented PCM mismatch at sample %ld", blockVsFragmented));
    return 0;
}

static int stream_tempo_slow_stereo()
{
    return stream_configuration({48000, 2, 0.70f, 401001u}); // nominalSkip 1075.2, fractional
}

static int stream_tempo_fast_stereo()
{
    return stream_configuration({48000, 2, 1.37f, 401002u}); // nominalSkip 1996.8, fractional
}

static int stream_tempo_nominal()
{
    return stream_configuration({48000, 2, 1.00f, 401003u}); // integer nominalSkip
}

static int stream_tempo_slow_six_channel()
{
    return stream_configuration({44100, 6, 0.70f, 401009u});
}

static int stream_tempo_fast_six_channel()
{
    return stream_configuration({44100, 6, 1.37f, 401005u});
}

static int stream_tempo_slowish_six_channel()
{
    return stream_configuration({48000, 6, 0.90f, 401006u});
}

// clear() drops audio history but preserves the fractional skip residue; the
// reference keeps the same clock and must still predict every later sample.
static int stream_clear_keeps_fraction()
{
    const int ch = 2, sampleRate = 48000;
    const float tempo = 0.7f;
    ObservableTDStretch geometry;
    geometry.configure(ch, sampleRate, tempo);
    const int ovl = geometry.overlapLengthValue(), win = geometry.windowLengthValue();
    const int seek = geometry.seekLengthValue(), req = geometry.sampleReqValue();
    const double nominal = geometry.nominalSkipValue();

    const std::vector<float> first = makeNoiseFrames(30000, ch, 402001u, 0.6f);
    const std::vector<float> second = makeNoiseFrames(40000, ch, 402002u, 0.6f);
    const int puts[3] = {1, 7, 113};
    const StreamConfig cfg{sampleRate, ch, tempo, 0};

    ObservableTDStretch td;
    td.configure(ch, sampleRate, tempo);
    reference::Model model;
    model.configure(ch, ovl, win, seek, req, nominal);

    StreamRun beforeRun, modelBeforeRun;
    runProductionStream(cfg, first, puts, 3, puts, 3, td, beforeRun);
    runModelStream(cfg, first, puts, 3, model, modelBeforeRun);
    TASK_CHECK(compareStreamRun("pre-clear", beforeRun, modelBeforeRun) == 0);

    const double retainedFraction = td.skipFractionValue();
    TASK_CHECK(expectTrue(retainedFraction > 0.0, "fractional residue was zero before clear()"));

    td.clear();
    model.clearAudio();

    TASK_CHECK(expectTrue(td.inputFrames() == 0, "production clear() left input frames"));
    TASK_CHECK(expectTrue(td.numSamples() == 0, "production clear() left output frames"));
    TASK_CHECK(expectTrue(td.midBufferIsZero(), "production clear() did not zero the mid window"));
    TASK_CHECK(expectTrue(model.ringFrames() == 0, "reference clear left input frames"));
    TASK_CHECK(expectTrueF(std::abs(td.skipFractionValue() - retainedFraction) < 1.0e-9,
                           "clear() changed skip fraction %.9g -> %.9g",
                           retainedFraction, td.skipFractionValue()));
    TASK_CHECK(expectTrueF(std::abs(model.fraction() - retainedFraction) < 1.0e-9,
                           "reference fraction %.9g != retained %.9g", model.fraction(), retainedFraction));

    StreamRun afterRun, modelAfterRun;
    runProductionStream(cfg, second, puts, 3, puts, 3, td, afterRun);
    runModelStream(cfg, second, puts, 3, model, modelAfterRun);
    TASK_CHECK(compareStreamRun("post-clear", afterRun, modelAfterRun) == 0);
    return 0;
}

// At a zero-residue point, a cleared instance must behave identically to a
// freshly constructed instance fed the same continuation.
static int stream_clear_at_zero_residue_matches_new_instance()
{
    const int ch = 2, sampleRate = 48000;
    const float tempo = 1.0f; // integer nominalSkip => residue is zero after every round
    ObservableTDStretch geometry;
    geometry.configure(ch, sampleRate, tempo);
    const int ovl = geometry.overlapLengthValue(), win = geometry.windowLengthValue();
    const int seek = geometry.seekLengthValue(), req = geometry.sampleReqValue();
    const double nominal = geometry.nominalSkipValue();

    const std::vector<float> warmup = makeNoiseFrames(req, ch, 403001u, 0.6f);
    const std::vector<float> continuation = makeNoiseFrames(60000, ch, 403002u, 0.6f);
    const int puts[3] = {1, 7, 113};
    const StreamConfig cfg{sampleRate, ch, tempo, 0};

    ObservableTDStretch cleared;
    cleared.configure(ch, sampleRate, tempo);
    cleared.putSamples(warmup.data(), req);
    std::vector<float> discard(8192 * ch);
    while (cleared.receiveSamples(discard.data(), 8192)) {}
    TASK_CHECK(expectTrueF(std::abs(cleared.skipFractionValue()) < 1.0e-9,
                           "warmup residue %.9g not zero", cleared.skipFractionValue()));
    cleared.clear();

    ObservableTDStretch fresh;
    fresh.configure(ch, sampleRate, tempo);

    reference::Model clearedModel, freshModel;
    clearedModel.configure(ch, ovl, win, seek, req, nominal);
    freshModel.configure(ch, ovl, win, seek, req, nominal);
    clearedModel.appendFrames(warmup.data(), req);
    clearedModel.step();
    clearedModel.clearAudio();

    StreamRun clearedRun, freshRun, clearedModelRun, freshModelRun;
    runProductionStream(cfg, continuation, puts, 3, puts, 3, cleared, clearedRun);
    runProductionStream(cfg, continuation, puts, 3, puts, 3, fresh, freshRun);
    runModelStream(cfg, continuation, puts, 3, clearedModel, clearedModelRun);
    runModelStream(cfg, continuation, puts, 3, freshModel, freshModelRun);

    TASK_CHECK(compareStreamRun("cleared-zero-residue", clearedRun, clearedModelRun) == 0);
    TASK_CHECK(compareStreamRun("fresh-instance", freshRun, freshModelRun) == 0);
    const long index = firstFloatMismatch(clearedRun.output, freshRun.output, 0.0);
    TASK_CHECK(expectTrueF(index < 0, "cleared vs fresh instance mismatch at sample %ld", index));
    return 0;
}

// Empty puts and empty reads must not advance any clock, buffer or output.
static int stream_empty_operations_no_advance()
{
    const int ch = 2, sampleRate = 48000;
    const float tempo = 1.37f;
    ObservableTDStretch td;
    td.configure(ch, sampleRate, tempo);
    reference::Model model;
    model.configure(ch, td.overlapLengthValue(), td.windowLengthValue(), td.seekLengthValue(),
                    td.sampleReqValue(), td.nominalSkipValue());

    std::vector<float> sink(64 * ch);
    TASK_CHECK(expectTrue(td.receiveSamples(sink.data(), 64) == 0, "read from empty TDStretch returned samples"));
    td.putSamples(sink.data(), 0);
    TASK_CHECK(expectTrue(td.inputFrames() == 0, "zero-sample put advanced input"));
    TASK_CHECK(expectTrue(td.numSamples() == 0, "zero-sample put produced output"));
    TASK_CHECK(expectTrue(td.skipFractionValue() == 0.0, "zero-sample put moved the skip clock"));

    const std::vector<float> block = makeNoiseFrames(td.sampleReqValue(), ch, 404001u, 0.6f);
    td.putSamples(block.data(), td.sampleReqValue());
    model.appendFrames(block.data(), td.sampleReqValue());
    model.step();
    const int inputsAfterRound = td.inputFrames();
    const int outputsAfterRound = static_cast<int>(td.numSamples());
    const double fractionAfterRound = td.skipFractionValue();

    td.putSamples(sink.data(), 0);
    TASK_CHECK(expectTrue(td.inputFrames() == inputsAfterRound, "empty put changed buffered input"));
    TASK_CHECK(expectTrueF(td.skipFractionValue() == fractionAfterRound,
                           "empty put changed fraction %.9g -> %.9g",
                           fractionAfterRound, td.skipFractionValue()));
    TASK_CHECK(expectTrue(td.receiveSamples(sink.data(), 0) == 0, "zero-capacity read returned samples"));
    TASK_CHECK(expectTrue(static_cast<int>(td.numSamples()) == outputsAfterRound, "zero-capacity read consumed output"));
    TASK_CHECK(expectTrue(model.ringFrames() == inputsAfterRound, "reference bookkeeping diverged"));
    return 0;
}

// Public SoundTouch API wiring check only: non-empty, finite, preserves the
// stereo channel relation and lands on a plausible stretched duration. It does
// not replace the content reference above.
static int stream_public_soundtouch_wiring()
{
    const int channels = 2, sampleRate = 48000;
    const int frames = 48000;
    std::vector<float> input(static_cast<size_t>(frames) * channels);
    Rng r(405001u);
    for (int i = 0; i < frames; ++i) {
        const float v = 0.4f * r.next() +
                        0.3f * static_cast<float>(std::sin(2.0 * kPi * 120.0 * i / sampleRate)) +
                        0.2f * static_cast<float>(std::sin(2.0 * kPi * 331.0 * i / sampleRate));
        input[i * 2] = v;
        input[i * 2 + 1] = -v; // exact anti-phase relation
    }

    SoundTouch st;
    st.setSampleRate(sampleRate);
    st.setChannels(channels);
    st.setTempo(1.25f);
    st.setSetting(SETTING_USE_QUICKSEEK, 0);
    st.setSetting(SETTING_SEQUENCE_MS, kSequenceMs);
    st.setSetting(SETTING_SEEKWINDOW_MS, kSeekWindowMs);
    st.setSetting(SETTING_OVERLAP_MS, kOverlapMs);

    std::vector<float> output;
    std::vector<float> buf(8192 * channels);
    for (int pos = 0; pos < frames; pos += 3001) {
        const int chunk = std::min(3001, frames - pos);
        st.putSamples(input.data() + static_cast<size_t>(pos) * channels, chunk);
        uint got;
        while ((got = st.receiveSamples(buf.data(), 8192)) > 0)
            output.insert(output.end(), buf.begin(), buf.begin() + got * channels);
    }
    st.flush();
    uint got;
    while ((got = st.receiveSamples(buf.data(), 8192)) > 0)
        output.insert(output.end(), buf.begin(), buf.begin() + got * channels);

    TASK_CHECK(expectTrue(!output.empty(), "public API produced no output"));
    TASK_CHECK(expectTrue(allFinite(output), "public API output not finite"));

    double maxAnti = 0.0, maxAmplitude = 0.0;
    for (size_t i = 0; i < output.size(); i += 2) {
        maxAnti = std::max(maxAnti, std::abs(static_cast<double>(output[i] + output[i + 1])));
        maxAmplitude = std::max(maxAmplitude, std::abs(static_cast<double>(output[i])));
    }
    TASK_CHECK(expectTrueF(maxAnti < 2.0e-6, "anti-phase relation broken, max L+R %.3e", maxAnti));
    TASK_CHECK(expectTrue(maxAmplitude > 0.05, "output collapsed to silence"));

    const double nominalOutputFrames = frames / 1.25;
    const long outputFrames = static_cast<long>(output.size()) / channels;
    TASK_CHECK(expectTrueF(outputFrames > 0.8 * nominalOutputFrames &&
                               outputFrames < 1.2 * nominalOutputFrames,
                           "implausible duration %ld frames vs nominal %.0f",
                           outputFrames, nominalOutputFrames));
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Task registration consumed by task_support/driver.c
// ---------------------------------------------------------------------------
const struct task_case task_cases[] = {
    {"search", "random_stereo_position", search_random_position},
    {"search", "six_channel_joint_position", search_six_channel_joint},
    {"search", "zero_energy_deterministic", search_zero_energy},
    {"search", "candidate_zero_unweighted_quirk", search_candidate_zero_quirk},

    {"overlap", "first_round_mono_stereo_six", overlap_first_round_all_channel_counts},
    {"overlap", "stereo_dirty_mid_rebuild", overlap_stereo_dirty_mid},
    {"overlap", "six_channel_dirty_mid_rebuild", overlap_six_channel_dirty_mid},

    {"stream", "tempo_0p70_stereo_48k", stream_tempo_slow_stereo},
    {"stream", "tempo_1p37_stereo_48k", stream_tempo_fast_stereo},
    {"stream", "tempo_1p00_stereo_48k", stream_tempo_nominal},
    {"stream", "tempo_0p70_six_channel_44k1", stream_tempo_slow_six_channel},
    {"stream", "tempo_1p37_six_channel_44k1", stream_tempo_fast_six_channel},
    {"stream", "tempo_0p90_six_channel_48k", stream_tempo_slowish_six_channel},
    {"stream", "clear_preserves_skip_fraction", stream_clear_keeps_fraction},
    {"stream", "clear_zero_residue_matches_new_instance", stream_clear_at_zero_residue_matches_new_instance},
    {"stream", "empty_put_and_read_no_advance", stream_empty_operations_no_advance},
    {"stream", "public_soundtouch_wiring", stream_public_soundtouch_wiring},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
