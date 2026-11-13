// Continuous size mode test groups for MVerb.
//
// delay     : fractional delay, allpass and post-write taps checked against a
//             standalone time-ordered history (the new delay implementation
//             is never used by the reference).
// tank      : the public dual-tank network re-derived independently and run
//             with asymmetric input and a decaying tail, including amplitude
//             scaling over the full size range at 48 kHz.
// automation: mid-trajectory redirects, immediate switches, invalid commands,
//             zero-length blocks, in-place processing and 1/7/113 frame
//             chunking.
#include "checks.h"
#include "MVerb.h"

#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double PI = 3.141592654;

bool closeEnough(double got, double ref)
{
    return std::abs(got - ref) <= 1e-8 + 1e-7 * std::abs(ref);
}

// ---------------------------------------------------------------------------
// Reference history: samples kept in actual write-time order, with a finite
// capacity matching the ring buffers. Reads of negative or forgotten times
// return zero; fractional positions use linear interpolation.
// ---------------------------------------------------------------------------
class RefHistory
{
    int capacity = 0;
    long long writeTime = 0;
    std::deque<double> hist; // oldest first, at most capacity entries

public:
    RefHistory() = default;
    explicit RefHistory(int cap) : capacity(cap) {}

    void clear() { writeTime = 0; hist.clear(); }

    long long timeOfNextWrite() const { return writeTime; }

    double at(long long t) const
    {
        if(t < 0 || hist.empty()) return 0.0;
        long long oldest = writeTime - static_cast<long long>(hist.size());
        if(t < oldest || t >= writeTime) return 0.0;
        return hist[static_cast<std::size_t>(t - oldest)];
    }

    double age(double a) const
    {
        if(hist.empty()) return 0.0;
        double baseT = static_cast<double>(writeTime - 1) - a;
        long long t0 = static_cast<long long>(std::floor(baseT));
        double frac = baseT - static_cast<double>(t0);
        double v0 = at(t0);
        double v1 = at(t0 + 1);
        return v0 + frac * (v1 - v0);
    }

    void write(double v)
    {
        hist.push_back(v);
        if(static_cast<int>(hist.size()) > capacity) hist.pop_front();
        ++writeTime;
    }
};

// Integer ring buffer replicating the legacy Allpass / StaticDelayLine
// semantics (lengths are fixed once configured in these tests).
class RefRing
{
    std::vector<double> buf;
    int index = 0;
    int length = 0;

public:
    void configure(int len)
    {
        length = len;
        buf.assign(len + 1, 0.0);
        index = 0;
    }
    double read() const { return buf[static_cast<std::size_t>(index)]; }
    void write(double v)
    {
        buf[static_cast<std::size_t>(index)] = v;
        if(++index >= length) index = 0;
    }
};

// Independent state-variable filter, transcribed from StateVariable<T,4>.
class RefSVF
{
    double f = 0.0;
    double low = 0.0, high = 0.0, band = 0.0, notch = 0.0;

public:
    void frequency(double frequency, double sampleRate)
    {
        f = 2.0 * std::sinf(static_cast<float>(PI * frequency / (sampleRate * 4.0)));
    }
    void reset() { low = high = band = notch = 0.0; }
    double operator()(double input)
    {
        for(int i = 0; i < 4; ++i) {
            low += f * band + 1e-25;
            high = input - low - 2.0 * band;
            band += f * high;
            notch = low + high;
        }
        return low;
    }
};

// ---------------------------------------------------------------------------
// delay group
// ---------------------------------------------------------------------------
void runDelayGroup()
{
    constexpr int CAP = 32;
    constexpr int FRAMES = 240;

    // Plain fractional delay line against the independent history.
    MorphLine<double, CAP> line;
    RefHistory refLine(CAP);
    line.Clear();
    double maxDelayErr = 0.0;
    double maxTapErr = 0.0;
    for(int n = 0; n < FRAMES; ++n) {
        double L = 1.5 + 1.2 * std::sin(0.13 * n);
        double x = 0.4 * std::sin(0.21 * n) + (n % 17 == 0 ? 0.5 : 0.0);
        double got = line.process(x, L);
        double expect = refLine.age(L - 1.0);
        maxDelayErr = std::max(maxDelayErr, std::abs(got - expect));
        refLine.write(x);
        // Post-write tap at age L - q - 1, observed after the write.
        double q = 0.3 + 0.4 * std::sin(0.07 * n);
        double tapGot = line.tap(L, q);
        double tapExpect = refLine.age(L - q - 1.0);
        maxTapErr = std::max(maxTapErr, std::abs(tapGot - tapExpect));
    }
    TASK_CHECK(closeEnough(maxDelayErr, 0.0));
    TASK_CHECK(closeEnough(maxTapErr, 0.0));

    // A line whose delay exceeds the stored history must read zero, and once
    // the buffer has wrapped many times old samples must be forgotten exactly
    // as the finite-capacity reference forgets them.
    MorphLine<double, CAP> deep;
    RefHistory refDeep(CAP);
    deep.Clear();
    TASK_CHECK(deep.process(0.9, 20.0) == 0.0);
    refDeep.write(0.9);
    double maxDeepErr = 0.0;
    for(int n = 1; n < FRAMES; ++n) {
        double L = 20.0 + 10.0 * std::sin(0.11 * n);
        double x = 0.6 * std::cos(0.17 * n) + (n % 23 == 0 ? -0.4 : 0.0);
        double got = deep.process(x, L);
        double expect = refDeep.age(L - 1.0);
        maxDeepErr = std::max(maxDeepErr, std::abs(got - expect));
        refDeep.write(x);
    }
    TASK_CHECK(closeEnough(maxDeepErr, 0.0));

    // Allpass: y = d - g*x, stored value x + g*y, plus post-write taps.
    MorphAllpassFourTap<double, CAP> ap;
    RefHistory refAp(CAP);
    ap.Clear();
    ap.SetFeedback(0.4);
    double g = 0.4;
    double maxApErr = 0.0;
    double maxApTapErr = 0.0;
    for(int n = 0; n < FRAMES; ++n) {
        double L = 2.2 + 0.9 * std::sin(0.09 * n + 1.0);
        double x = 0.5 * std::sin(0.29 * n + 0.5) + (n % 13 == 0 ? 0.7 : 0.0);
        double got = ap.process(x, L);
        double d = refAp.age(L - 1.0);
        double y = d - g * x;
        maxApErr = std::max(maxApErr, std::abs(got - y));
        refAp.write(x + g * y);
        const double qs[3] = {0.0, 0.5, 1.25};
        for(double q : qs) {
            double tapGot = ap.tap(L, q);
            double tapExpect = refAp.age(L - q - 1.0);
            maxApTapErr = std::max(maxApTapErr, std::abs(tapGot - tapExpect));
        }
    }
    TASK_CHECK(closeEnough(maxApErr, 0.0));
    TASK_CHECK(closeEnough(maxApTapErr, 0.0));
}

// ---------------------------------------------------------------------------
// tank group: full independent reimplementation of the MVerb signal path in
// continuous-size mode, using only the reference histories above.
// ---------------------------------------------------------------------------
class RefMVerb
{
public:
    static constexpr int TANK_CAP = 96000;

    double sampleRate;
    double size;
    // Smoothed (pinned) parameter values used per frame.
    double bandwidthFreq, dampingFreq, predelayLength;
    double decaySmooth, density1;
    double mix, earlyMix, gain;

    RefSVF bw[2];
    RefSVF damp[2];
    RefRing predelay;
    RefRing inputAp[4];
    RefHistory er[2]{RefHistory(TANK_CAP), RefHistory(TANK_CAP)};
    RefHistory tankAp[4]{RefHistory(TANK_CAP), RefHistory(TANK_CAP),
                         RefHistory(TANK_CAP), RefHistory(TANK_CAP)};
    RefHistory tankDly[4]{RefHistory(TANK_CAP), RefHistory(TANK_CAP),
                          RefHistory(TANK_CAP), RefHistory(TANK_CAP)};
    double prevL = 0.0, prevR = 0.0;

    int controlRate, controlRateCounter = 0;

    int erLength[2];
    int erTap[2][6];

    RefMVerb(double sr, double normalizedSize,
             double density, double decay, double predelayParam,
             double dampingParam, double bandwidthParam,
             double mixValue, double earlyMixValue, double gainValue)
        : sampleRate(sr), size(0.05 + 0.95 * normalizedSize)
    {
        bandwidthFreq = bandwidthParam * 18400.0 + 100.0;
        dampingFreq = (1.0 - dampingParam) * 18400.0 + 100.0;
        predelayLength = predelayParam * 200.0 * (sr / 1000.0);
        decaySmooth = 0.7995 * decay + 0.005;
        density1 = density;
        mix = mixValue;
        earlyMix = earlyMixValue;
        gain = gainValue;

        for(int c = 0; c < 2; ++c) {
            // StateVariable constructs at 1000 Hz; MVerb only pushes the
            // smoothed frequency once per control-rate block.
            bw[c].frequency(1000.0, sr);
            damp[c].frequency(1000.0, sr);
        }
        controlRate = static_cast<int>(sr / 1000);
        predelay.configure(static_cast<int>(predelayLength));
        const double apSec[4] = {0.0048, 0.0036, 0.0127, 0.0093};
        for(int j = 0; j < 4; ++j)
            inputAp[j].configure(static_cast<int>(apSec[j] * sr));

        erLength[0] = static_cast<int>(0.089 * sr);
        erLength[1] = static_cast<int>(0.069 * sr);
        // GetIndex(2..7) reads SetIndex slots index3..index8: the first
        // configured tap is dead and the last slot is position 0, so its age
        // equals the main output's Length - 1.
        const double er0Sec[5] = {0.0219, 0.0354, 0.0389, 0.0414, 0.0692};
        const double er1Sec[5] = {0.011, 0.0182, 0.0189, 0.0213, 0.0431};
        for(int k = 0; k < 5; ++k) {
            erTap[0][k] = static_cast<int>(er0Sec[k] * sr);
            erTap[1][k] = static_cast<int>(er1Sec[k] * sr);
        }
    }

    void processFrame(double inL, double inR, double &outL, double &outR)
    {
        double density2 = decaySmooth + 0.15;
        if(density2 > 0.5) density2 = 0.5;
        if(density2 < 0.25) density2 = 0.25;
        const double gAp[4] = {density1, density2, density1, density2};
        const double gInput[4] = {0.75, 0.75, 0.625, 0.625};

        if(controlRateCounter >= controlRate) {
            controlRateCounter = 0;
            bw[0].frequency(bandwidthFreq, sampleRate);
            bw[1].frequency(bandwidthFreq, sampleRate);
            damp[0].frequency(dampingFreq, sampleRate);
            damp[1].frequency(dampingFreq, sampleRate);
        }
        ++controlRateCounter;

        double bwl = bw[0](inL);
        double bwr = bw[1](inR);

        // Early reflections: the main output is read before the write, while
        // the multi-taps are observed afterwards (GetIndex after operator()).
        double erMain[2] = {
            er[0].age(static_cast<double>(erLength[0] - 1)),
            er[1].age(static_cast<double>(erLength[1] - 1))
        };
        er[0].write(bwl * 0.5 + bwr * 0.3);
        er[1].write(bwl * 0.3 + bwr * 0.5);
        double erTapSum[2] = {0.0, 0.0};
        const double erCoeff[5] = {0.6, 0.4, 0.3, 0.3, 0.1};
        for(int c = 0; c < 2; ++c) {
            for(int k = 0; k < 5; ++k) {
                double age = static_cast<double>(erLength[c] - erTap[c][k] - 1);
                erTapSum[c] += erCoeff[k] * er[c].age(age);
            }
            // GetIndex(7) reads index8, which stays at write position 0.
            erTapSum[c] += 0.1 * er[c].age(static_cast<double>(erLength[c] - 1));
        }
        double erl = erMain[0] + erTapSum[0] + (bwl * 0.4 + bwr * 0.2) * 0.5;
        double err = erMain[1] + erTapSum[1] + (bwl * 0.2 + bwr * 0.4) * 0.5;

        // Predelay and input diffusion.
        double preOut = predelay.read();
        predelay.write((bwr + bwl) * 0.5);
        double smeared = preOut;
        for(int j = 0; j < 4; ++j) {
            double d = inputAp[j].read();
            double y = d - gInput[j] * smeared;
            inputAp[j].write(smeared + gInput[j] * y);
            smeared = y;
        }

        // Fractional tank geometry from the reset() constants.
        const double apSec[4] = {0.020, 0.060, 0.030, 0.089};
        const double dlySec[4] = {0.15, 0.12, 0.14, 0.11};
        const double apTapSec[4][3] = {
            {0.0, 0.0, 0.0},
            {0.006, 0.041, 0.0},
            {0.0, 0.0, 0.0},
            {0.031, 0.011, 0.0}
        };
        const double dlyTapSec[4][3] = {
            {0.067, 0.011, 0.121},
            {0.036, 0.089, 0.0},
            {0.0089, 0.099, 0.0},
            {0.067, 0.0041, 0.0}
        };
        double La[4], Ld[4], qa[4][3], qd[4][3];
        for(int j = 0; j < 4; ++j) {
            La[j] = apSec[j] * sampleRate * size;
            Ld[j] = dlySec[j] * sampleRate * size;
            for(int k = 0; k < 3; ++k) {
                qa[j][k] = apTapSec[j][k] * sampleRate * size;
                qd[j][k] = dlyTapSec[j][k] * sampleRate * size;
            }
        }

        auto apStep = [&](int j, double x) {
            double d = tankAp[j].age(La[j] - 1.0);
            double y = d - gAp[j] * x;
            tankAp[j].write(x + gAp[j] * y);
            return y;
        };
        auto dlyStep = [&](int j, double x) {
            double y = tankDly[j].age(Ld[j] - 1.0);
            tankDly[j].write(x);
            return y;
        };

        double leftTank = apStep(0, smeared + prevR);
        leftTank = dlyStep(0, leftTank);
        leftTank = damp[0](leftTank);
        leftTank = apStep(1, leftTank);
        leftTank = dlyStep(1, leftTank);
        double rightTank = apStep(2, smeared + prevL);
        rightTank = dlyStep(2, rightTank);
        rightTank = damp[1](rightTank);
        rightTank = apStep(3, rightTank);
        rightTank = dlyStep(3, rightTank);
        prevL = leftTank * decaySmooth;
        prevR = rightTank * decaySmooth;

        double accL =
            0.6 * tankDly[2].age(Ld[2] - qd[2][0] - 1.0) +
            0.6 * tankDly[2].age(Ld[2] - qd[2][1] - 1.0) -
            0.6 * tankAp[3].age(La[3] - qa[3][0] - 1.0) +
            0.6 * tankDly[3].age(Ld[3] - qd[3][0] - 1.0) -
            0.6 * tankDly[0].age(Ld[0] - qd[0][0] - 1.0) -
            0.6 * tankAp[1].age(La[1] - qa[1][0] - 1.0) -
            0.6 * tankDly[1].age(Ld[1] - qd[1][0] - 1.0);
        double accR =
            0.6 * tankDly[0].age(Ld[0] - qd[0][1] - 1.0) +
            0.6 * tankDly[0].age(Ld[0] - qd[0][2] - 1.0) -
            0.6 * tankAp[1].age(La[1] - qa[1][1] - 1.0) +
            0.6 * tankDly[1].age(Ld[1] - qd[1][1] - 1.0) -
            0.6 * tankDly[2].age(Ld[2] - qd[2][2] - 1.0) -
            0.6 * tankAp[3].age(La[3] - qa[3][1] - 1.0) -
            0.6 * tankDly[3].age(Ld[3] - qd[3][1] - 1.0);

        accL = accL * earlyMix + (1.0 - earlyMix) * erl;
        accR = accR * earlyMix + (1.0 - earlyMix) * err;
        outL = (inL + mix * (accL - inL)) * gain;
        outR = (inR + mix * (accR - inR)) * gain;
    }
};

void configureVerb(MVerb<double> &v, double normalizedSize)
{
    v.setSampleRate(48000);
    v.setParameter(MVerb<double>::PREDELAY, 0.01);
    v.setParameter(MVerb<double>::DENSITY, 0.5);
    v.setParameter(MVerb<double>::DECAY, 0.8);
    v.setParameter(MVerb<double>::DAMPINGFREQ, 0.9);
    v.setParameter(MVerb<double>::BANDWIDTHFREQ, 1.0);
    v.setParameter(MVerb<double>::SIZE, normalizedSize);
    v.setParameter(MVerb<double>::GAIN, 1.0);
    v.setParameter(MVerb<double>::MIX, 1.0);
    v.setParameter(MVerb<double>::EARLYMIX, 0.75);
}

void runTankComparison(double normalizedSize, int driven, int tail,
                       double amplitudeScale, double &worstErr)
{
    auto verbStorage = std::make_unique<MVerb<double>>();
    MVerb<double> &verb = *verbStorage;
    configureVerb(verb, normalizedSize);
    TASK_CHECK(verb.enableSizeMorph(true));
    auto refStorage = std::make_unique<RefMVerb>(48000, normalizedSize, 0.5, 0.8,
                                                 0.01, 0.9, 1.0, 1.0, 0.75, 1.0);
    RefMVerb &ref = *refStorage;

    int total = driven + tail;
    for(int n = 0; n < total; ++n) {
        double inL = 0.0, inR = 0.0;
        if(n < driven) {
            inL = (n == 5 ? 1.0 : 0.0) + 0.3 * std::sin(0.031 * n);
            inR = (n == 20 ? -0.7 : 0.0) + 0.2 * std::sin(0.047 * n + 1.0);
        }
        double refL = 0.0, refR = 0.0;
        ref.processFrame(inL, inR, refL, refR);

        double scaledIn[2] = {amplitudeScale * inL, amplitudeScale * inR};
        double out[2] = {0.0, 0.0};
        double *inPtrs[2] = {scaledIn, scaledIn + 1};
        double *outPtrs[2] = {out, out + 1};
        verb.process(inPtrs, outPtrs, 1);

        TASK_CHECK(closeEnough(out[0], amplitudeScale * refL));
        TASK_CHECK(closeEnough(out[1], amplitudeScale * refR));
        worstErr = std::max(worstErr,
                            std::max(std::abs(out[0] - amplitudeScale * refL),
                                     std::abs(out[1] - amplitudeScale * refR)));
    }
}

void runTankGroup()
{
    double worstErr = 0.0;
    // Asymmetric excitation, then a zero-input tail; full size range.
    runTankComparison(0.6, 4000, 3000, 1.0, worstErr);
    runTankComparison(0.0, 1500, 1000, 1.0, worstErr);
    runTankComparison(1.0, 1500, 1000, 1.0, worstErr);
    // Amplitude scaling must hold for the whole response, tail included.
    runTankComparison(0.6, 2000, 2000, 2.5, worstErr);
    runTankComparison(0.35, 1500, 1500, -1.0, worstErr);
    TASK_CHECK(std::isfinite(worstErr));
}

// ---------------------------------------------------------------------------
// automation group
// ---------------------------------------------------------------------------
struct Event
{
    unsigned at;
    double normalized;
    unsigned frames;
};

void makeInput(std::vector<double> &l, std::vector<double> &r, int n)
{
    l.resize(static_cast<std::size_t>(n));
    r.resize(static_cast<std::size_t>(n));
    for(int i = 0; i < n; ++i) {
        l[static_cast<std::size_t>(i)] =
            (i == 7 ? 0.8 : 0.0) + 0.25 * std::sin(0.037 * i);
        r[static_cast<std::size_t>(i)] =
            (i == 31 ? -0.6 : 0.0) + 0.18 * std::sin(0.051 * i + 0.4);
    }
}

// Runs the same event script and input, splitting blocks at event boundaries
// so every command is issued at the same global frame regardless of chunking.
// In-place mode passes the very same pointer as input and output.
void runScript(MVerb<double> &verb, const std::vector<Event> &events,
               const std::vector<double> &inL, const std::vector<double> &inR,
               std::vector<double> &outL, std::vector<double> &outR,
               int chunk, bool inPlace)
{
    int total = static_cast<int>(inL.size());
    std::vector<double> workL, workR;
    const std::vector<double> *srcL = &inL;
    const std::vector<double> *srcR = &inR;
    if(inPlace) {
        workL = inL;
        workR = inR;
        srcL = &workL;
        srcR = &workR;
    }
    outL.assign(static_cast<std::size_t>(total), 0.0);
    outR.assign(static_cast<std::size_t>(total), 0.0);
    double *lBuf = outL.data();
    double *rBuf = outR.data();
    if(inPlace) {
        lBuf = workL.data();
        rBuf = workR.data();
    }
    std::size_t eventIdx = 0;
    int pos = 0;
    while(pos < total) {
        while(eventIdx < events.size() &&
              static_cast<int>(events[eventIdx].at) == pos) {
            const Event &e = events[eventIdx];
            TASK_CHECK(verb.morphSize(e.normalized, e.frames));
            ++eventIdx;
        }
        int block = chunk;
        if(eventIdx < events.size())
            block = std::min(block, static_cast<int>(events[eventIdx].at) - pos);
        block = std::min(block, total - pos);
        TASK_CHECK(block > 0);
        double *inPtrs[2] = {
            const_cast<double *>(srcL->data()) + pos,
            const_cast<double *>(srcR->data()) + pos
        };
        double *outPtrs[2] = {lBuf + pos, rBuf + pos};
        if(inPlace) {
            // Genuinely identical pointers for every channel.
            TASK_CHECK(inPtrs[0] == outPtrs[0] && inPtrs[1] == outPtrs[1]);
        }
        verb.process(inPtrs, outPtrs, block);
        pos += block;
    }
    if(inPlace) {
        outL = std::move(workL);
        outR = std::move(workR);
    }
}

void runAutomationGroup()
{
    constexpr int N = 4000;
    std::vector<double> inL, inR;
    makeInput(inL, inR, N);

    // Redirects land while earlier trajectories are still running, and one
    // command is an immediate (zero-frame) switch.
    const std::vector<Event> events = {
        {100,  0.9,  500},
        {300,  0.15, 800},   // redirect from the frame-300 current size
        {900,  0.7,  1},     // single-frame ramp
        {1500, 0.05, 0},     // immediate
        {1600, 1.0,  1200},
        {2000, 0.4,  300},   // another mid-flight redirect
        {3200, 0.6,  400}
    };

    struct Result { std::vector<double> l, r; };
    Result ref;
    {
        auto storage = std::make_unique<MVerb<double>>();
        MVerb<double> &verb = *storage;
        configureVerb(verb, 0.6);
        TASK_CHECK(verb.enableSizeMorph(true));
        runScript(verb, events, inL, inR, ref.l, ref.r, 1, false);
    }

    const int chunks[3] = {1, 7, 113};
    for(int chunk : chunks) {
        auto storage = std::make_unique<MVerb<double>>();
        MVerb<double> &verb = *storage;
        configureVerb(verb, 0.6);
        TASK_CHECK(verb.enableSizeMorph(true));
        Result got;
        runScript(verb, events, inL, inR, got.l, got.r, chunk, false);
        double worst = 0.0;
        for(int n = 0; n < N; ++n)
            worst = std::max(worst,
                             std::max(std::abs(got.l[static_cast<std::size_t>(n)] -
                                               ref.l[static_cast<std::size_t>(n)]),
                                      std::abs(got.r[static_cast<std::size_t>(n)] -
                                               ref.r[static_cast<std::size_t>(n)])));
        TASK_CHECK(closeEnough(worst, 0.0));
    }

    // Fully in-place processing must match separate input/output buffers.
    {
        auto storage = std::make_unique<MVerb<double>>();
        MVerb<double> &verb = *storage;
        configureVerb(verb, 0.6);
        TASK_CHECK(verb.enableSizeMorph(true));
        Result got;
        runScript(verb, events, inL, inR, got.l, got.r, 113, true);
        double worst = 0.0;
        for(int n = 0; n < N; ++n)
            worst = std::max(worst,
                             std::max(std::abs(got.l[static_cast<std::size_t>(n)] -
                                               ref.l[static_cast<std::size_t>(n)]),
                                      std::abs(got.r[static_cast<std::size_t>(n)] -
                                               ref.r[static_cast<std::size_t>(n)])));
        TASK_CHECK(closeEnough(worst, 0.0));
    }

    // Zero-length blocks neither advance the trajectory nor touch the buffers.
    {
        auto s1 = std::make_unique<MVerb<double>>();
        auto s2 = std::make_unique<MVerb<double>>();
        MVerb<double> &verb = *s1;
        MVerb<double> &probe = *s2;
        configureVerb(verb, 0.6);
        configureVerb(probe, 0.6);
        TASK_CHECK(verb.enableSizeMorph(true));
        TASK_CHECK(probe.enableSizeMorph(true));
        double sentinel[2] = {1234.5678, -8765.4321};
        double guard[2] = {sentinel[0], sentinel[1]};
        double *inPtrs[2] = {sentinel, sentinel + 1};
        double *outPtrs[2] = {guard, guard + 1};
        verb.process(inPtrs, outPtrs, 0);
        TASK_CHECK(guard[0] == sentinel[0] && guard[1] == sentinel[1]);

        for(int n = 0; n < 600; ++n) {
            if(n == 120) {
                TASK_CHECK(verb.morphSize(0.15, 300));
                TASK_CHECK(probe.morphSize(0.15, 300));
            }
            if(n == 200 || n == 201)
                verb.process(inPtrs, outPtrs, 0); // must be a no-op
            double in[2] = {inL[static_cast<std::size_t>(n)],
                            inR[static_cast<std::size_t>(n)]};
            double o1[2], o2[2];
            double *ip[2] = {in, in + 1};
            double *op1[2] = {o1, o1 + 1};
            double *op2[2] = {o2, o2 + 1};
            verb.process(ip, op1, 1);
            probe.process(ip, op2, 1);
            TASK_CHECK(o1[0] == o2[0] && o1[1] == o2[1]);
        }
    }

    // Invalid commands fail and leave audio and modulation progress untouched.
    {
        auto s1 = std::make_unique<MVerb<double>>();
        auto s2 = std::make_unique<MVerb<double>>();
        MVerb<double> &verb = *s1;
        MVerb<double> &probe = *s2;
        configureVerb(verb, 0.6);
        configureVerb(probe, 0.6);
        TASK_CHECK(verb.enableSizeMorph(true));
        TASK_CHECK(probe.enableSizeMorph(true));
        TASK_CHECK(!verb.morphSize(std::numeric_limits<double>::infinity(), 10));
        TASK_CHECK(!verb.morphSize(-std::numeric_limits<double>::infinity(), 10));
        TASK_CHECK(!verb.morphSize(std::numeric_limits<double>::quiet_NaN(), 10));
        TASK_CHECK(!verb.morphSize(-0.0001, 10));
        TASK_CHECK(!verb.morphSize(1.0001, 10));
        for(int n = 0; n < 900; ++n) {
            if(n == 100) {
                TASK_CHECK(verb.morphSize(0.2, 400));
                TASK_CHECK(probe.morphSize(0.2, 400));
            }
            if(n == 250) {
                // Illegal redirect while the trajectory is running.
                TASK_CHECK(!verb.morphSize(std::numeric_limits<double>::quiet_NaN(), 5));
                TASK_CHECK(!verb.morphSize(2.0, 5));
            }
            double in[2] = {inL[static_cast<std::size_t>(n)],
                            inR[static_cast<std::size_t>(n)]};
            double o1[2], o2[2];
            double *ip[2] = {in, in + 1};
            double *op1[2] = {o1, o1 + 1};
            double *op2[2] = {o2, o2 + 1};
            verb.process(ip, op1, 1);
            probe.process(ip, op2, 1);
            TASK_CHECK(o1[0] == o2[0] && o1[1] == o2[1]);
        }
    }

    // Mode gating: toggles only after reset and before any non-empty process.
    {
        auto storage = std::make_unique<MVerb<double>>();
        MVerb<double> &verb = *storage;
        configureVerb(verb, 0.6);
        double zero[2] = {0.0, 0.0};
        double out[2];
        double *ip[2] = {zero, zero + 1};
        double *op[2] = {out, out + 1};
        verb.process(ip, op, 0); // zero blocks do not consume the reset window
        TASK_CHECK(verb.enableSizeMorph(true));
        verb.process(ip, op, 1);
        TASK_CHECK(!verb.enableSizeMorph(false));
        TASK_CHECK(!verb.enableSizeMorph(true));
        // Rejected toggles leave the enabled mode working.
        TASK_CHECK(verb.morphSize(0.5, 10));
        verb.reset();
        TASK_CHECK(verb.enableSizeMorph(false));
        TASK_CHECK(!verb.morphSize(0.5, 10));
        // Legacy SIZE setter still clears the tanks when the mode is off.
        auto laStorage = std::make_unique<MVerb<double>>();
        auto lbStorage = std::make_unique<MVerb<double>>();
        MVerb<double> &legacyA = *laStorage;
        MVerb<double> &legacyB = *lbStorage;
        configureVerb(legacyA, 0.6);
        configureVerb(legacyB, 0.6);
        for(int n = 0; n < 500; ++n) {
            double in[2] = {inL[static_cast<std::size_t>(n)],
                            inR[static_cast<std::size_t>(n)]};
            double oa[2], ob[2];
            double *iptr[2] = {in, in + 1};
            double *pa[2] = {oa, oa + 1};
            double *pb[2] = {ob, ob + 1};
            legacyA.process(iptr, pa, 1);
            legacyB.process(iptr, pb, 1);
            TASK_CHECK(oa[0] == ob[0] && oa[1] == ob[1]);
        }
        legacyA.setParameter(MVerb<double>::SIZE, 0.2);
        legacyB.setParameter(MVerb<double>::SIZE, 0.2);
        for(int n = 0; n < 200; ++n) {
            double in[2] = {0.0, 0.0};
            double oa[2], ob[2];
            double *iptr[2] = {in, in + 1};
            double *pa[2] = {oa, oa + 1};
            double *pb[2] = {ob, ob + 1};
            legacyA.process(iptr, pa, 1);
            legacyB.process(iptr, pb, 1);
            TASK_CHECK(oa[0] == ob[0] && oa[1] == ob[1]);
        }
    }

    // A size moved while enabled, then the mode disabled before any audio:
    // the integer tanks must line up with a verb configured at that size.
    {
        auto sA = std::make_unique<MVerb<double>>();
        auto sB = std::make_unique<MVerb<double>>();
        MVerb<double> &morphed = *sA;
        MVerb<double> &direct = *sB;
        configureVerb(morphed, 0.6);
        configureVerb(direct, 0.2); // integer lines already at size 0.2
        TASK_CHECK(morphed.enableSizeMorph(true));
        TASK_CHECK(morphed.morphSize(0.2, 0));
        TASK_CHECK(morphed.enableSizeMorph(false));
        for(int n = 0; n < 700; ++n) {
            double in[2] = {inL[static_cast<std::size_t>(n)],
                            inR[static_cast<std::size_t>(n)]};
            double oa[2], ob[2];
            double *ip[2] = {in, in + 1};
            double *pa[2] = {oa, oa + 1};
            double *pb[2] = {ob, ob + 1};
            morphed.process(ip, pa, 1);
            direct.process(ip, pb, 1);
            TASK_CHECK(oa[0] == ob[0] && oa[1] == ob[1]);
        }
    }

    // Enabling starts at the current SIZE, and the SIZE setter in morph mode
    // behaves as a zero-frame command without clearing in-flight sound.
    {
        auto storage = std::make_unique<MVerb<double>>();
        MVerb<double> &verb = *storage;
        configureVerb(verb, 0.4);
        TASK_CHECK(verb.enableSizeMorph(true));
        // A long ramp redirected by an immediate setter command.
        TASK_CHECK(verb.morphSize(0.9, 1000));
        for(int n = 0; n < 200; ++n) {
            double in[2] = {inL[static_cast<std::size_t>(n)],
                            inR[static_cast<std::size_t>(n)]};
            double *ip[2] = {in, in + 1};
            double o[2];
            double *op[2] = {o, o + 1};
            verb.process(ip, op, 1);
        }
        verb.setParameter(MVerb<double>::SIZE, 0.1); // normalized 0.1
        auto probeStorage = std::make_unique<MVerb<double>>();
        MVerb<double> &probe = *probeStorage;
        configureVerb(probe, 0.4);
        TASK_CHECK(probe.enableSizeMorph(true));
        TASK_CHECK(probe.morphSize(0.9, 1000));
        for(int n = 0; n < 200; ++n) {
            double in[2] = {inL[static_cast<std::size_t>(n)],
                            inR[static_cast<std::size_t>(n)]};
            double *ip[2] = {in, in + 1};
            double o[2];
            double *op[2] = {o, o + 1};
            probe.process(ip, op, 1);
        }
        TASK_CHECK(probe.morphSize(0.1, 0));
        double worst = 0.0;
        for(int n = 200; n < 800; ++n) {
            double in[2] = {inL[static_cast<std::size_t>(n)],
                            inR[static_cast<std::size_t>(n)]};
            double o1[2], o2[2];
            double *ip[2] = {in, in + 1};
            double *op1[2] = {o1, o1 + 1};
            double *op2[2] = {o2, o2 + 1};
            verb.process(ip, op1, 1);
            probe.process(ip, op2, 1);
            worst = std::max(worst, std::max(std::abs(o1[0] - o2[0]),
                                             std::abs(o1[1] - o2[1])));
        }
        TASK_CHECK(closeEnough(worst, 0.0));
    }

    // Frame-level trajectory arithmetic, re-derived from the specification
    // without reading any MVerb internals.
    struct Scheduler
    {
        double current; // size the next output frame will use
        double target = 0.0;
        double step = 0.0;
        unsigned remaining = 0;

        explicit Scheduler(double start) : current(start) {}

        void command(double normalized, unsigned frames)
        {
            target = normalized;
            if(frames == 0) {
                current = normalized;
                step = 0.0;
                remaining = 0;
            } else {
                step = (normalized - current) / static_cast<double>(frames);
                remaining = frames;
                current += step;
            }
        }
        double frameSize()
        {
            double used = current;
            if(remaining > 0) {
                --remaining;
                if(remaining == 0) {
                    current = target;
                    step = 0.0;
                } else {
                    current += step;
                }
            }
            return used;
        }
    };

    {
        // Equal steps, exact arrival on the final frame, then hold.
        Scheduler s(0.6);
        s.command(0.0, 4);
        const double expect1[4] = {0.45, 0.3, 0.15, 0.0};
        for(double e : expect1) TASK_CHECK(std::abs(s.frameSize() - e) < 1e-15);
        TASK_CHECK(std::abs(s.frameSize() - 0.0) < 1e-15);
        // Single-frame ramp lands immediately on the target.
        s.command(1.0, 1);
        TASK_CHECK(std::abs(s.frameSize() - 1.0) < 1e-15);
        TASK_CHECK(std::abs(s.frameSize() - 1.0) < 1e-15);
        // A redirect restarts at the size the next frame was going to use.
        Scheduler r(0.0);
        r.command(0.8, 8); // step 0.1
        TASK_CHECK(std::abs(r.frameSize() - 0.1) < 1e-15);
        TASK_CHECK(std::abs(r.frameSize() - 0.2) < 1e-15);
        TASK_CHECK(std::abs(r.frameSize() - 0.3) < 1e-15);
        // Before frame 4 current == 0.4; redirect to 0 over four frames.
        r.command(0.0, 4);
        const double expect2[4] = {0.3, 0.2, 0.1, 0.0};
        for(double e : expect2) TASK_CHECK(std::abs(r.frameSize() - e) < 1e-15);
    }

    // End to end: one verb receives multi-frame commands and redirects, the
    // other is driven frame by frame with zero-frame commands taken from the
    // independent scheduler; their audio must agree.
    {
        auto sA = std::make_unique<MVerb<double>>();
        auto sB = std::make_unique<MVerb<double>>();
        MVerb<double> &cmdVerb = *sA;
        MVerb<double> &frameVerb = *sB;
        configureVerb(cmdVerb, 0.6);
        configureVerb(frameVerb, 0.6);
        TASK_CHECK(cmdVerb.enableSizeMorph(true));
        TASK_CHECK(frameVerb.enableSizeMorph(true));

        const std::vector<Event> script = {
            {100,  0.9,  500},
            {300,  0.15, 800},
            {900,  0.7,  1},
            {1500, 0.05, 0},
            {1600, 1.0,  1200},
            {2000, 0.4,  300},
            {3200, 0.6,  400}
        };
        Scheduler scheduler(0.6);
        std::size_t idx = 0;
        double worst = 0.0;
        for(int n = 0; n < N; ++n) {
            while(idx < script.size() && script[idx].at == static_cast<unsigned>(n)) {
                TASK_CHECK(cmdVerb.morphSize(script[idx].normalized, script[idx].frames));
                scheduler.command(script[idx].normalized, script[idx].frames);
                ++idx;
            }
            double frameNorm = scheduler.frameSize();
            TASK_CHECK(frameVerb.morphSize(frameNorm, 0));
            double in[2] = {inL[static_cast<std::size_t>(n)],
                            inR[static_cast<std::size_t>(n)]};
            double oa[2], ob[2];
            double *ip[2] = {in, in + 1};
            double *pa[2] = {oa, oa + 1};
            double *pb[2] = {ob, ob + 1};
            cmdVerb.process(ip, pa, 1);
            frameVerb.process(ip, pb, 1);
            worst = std::max(worst, std::max(std::abs(oa[0] - ob[0]),
                                             std::abs(oa[1] - ob[1])));
        }
        TASK_CHECK(closeEnough(worst, 0.0));
    }
}

} // namespace

void task_run(const char *group)
{
    std::string g = group;
    if(g == "delay")
        runDelayGroup();
    else if(g == "tank")
        runTankGroup();
    else if(g == "automation")
        runAutomationGroup();
}
