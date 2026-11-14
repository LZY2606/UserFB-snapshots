/* Contract tests for the OpenAL Soft pitch shifter phase-lock extension.
 *
 * The spectral and tracking groups drive the production al::PhaseLock1024
 * kernel and compare every output bin against an independent double-precision
 * reference implemented in this file. The render group runs the real effect
 * through a 48 kHz stereo float loopback device and compares the wet PCM
 * against a from-scratch double STFT reference using the same analysis window,
 * hop, FFT, overlap-add and output scaling.
 *
 * No reference code calls the production kernel.
 */

#include "task_support/check.h"

#define AL_ALEXT_PROTOTYPES

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <vector>

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "AL/efx.h"

#include "common/alnumeric.h"
#include "core/phase_lock.h"


namespace {

constexpr double Pi{3.141592653589793238462643383279502884};
constexpr double Tau{2.0 * Pi};
constexpr int StftSize{1024};
constexpr int StftHop{128};
constexpr int BinCount{513};
constexpr int InnerLast{511};

using cd = std::complex<double>;


/* ------------------------------------------------------------------ */
/* Independent double-precision helpers.                              */
/* ------------------------------------------------------------------ */

void fftDouble(std::vector<cd> &a, int sign)
{
    const int n{static_cast<int>(a.size())};
    for(int i{1}, j{0}; i < n; ++i)
    {
        int bit{n >> 1};
        for(; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if(i < j) std::swap(a[i], a[j]);
    }
    for(int len{2}; len <= n; len <<= 1)
    {
        const double ang{Pi * sign / (len >> 1)};
        const cd wn{std::cos(ang), std::sin(ang)};
        for(int i{0}; i < n; i += len)
        {
            cd w{1.0, 0.0};
            for(int k{0}; k < len/2; ++k)
            {
                const cd u{a[i+k]};
                const cd v{a[i+k+len/2] * w};
                a[i+k] = u + v;
                a[i+k+len/2] = u - v;
                w *= wn;
            }
        }
    }
}

double wrapPi(double phase)
{
    phase = std::fmod(phase + Pi, Tau);
    if(phase < 0.0) phase += Tau;
    return phase - Pi;
}

float wrapPiF(double phase)
{ return static_cast<float>(wrapPi(phase)); }

/* Independent double-precision re-implementation of the phase-lock contract. */
struct RefKernel {
    struct Peak {
        int bin;
        double frequency;
        double phase;
        double synthPhase;
    };

    std::vector<Peak> mOld;
    std::vector<double> mLastMagnitude{std::vector<double>(511, 0.0)};
    double mLastSum{0.0};
    bool mHasHistory{false};

    void reset()
    {
        mOld.clear();
        std::fill(mLastMagnitude.begin(), mLastMagnitude.end(), 0.0);
        mLastSum = 0.0;
        mHasHistory = false;
    }

    void process(const std::vector<cd> &bins, const std::vector<double> &frequencies,
        double ratio, double threshold, std::vector<cd> &output)
    {
        output.assign(BinCount, cd{0.0, 0.0});

        double maxMagnitude{0.0}, magnitudeSum{0.0}, positiveFlux{0.0};
        for(int k{1}; k <= InnerLast; ++k)
        {
            const double magnitude{std::abs(bins[k])};
            maxMagnitude = std::max(maxMagnitude, magnitude);
            magnitudeSum += magnitude;
            if(mHasHistory)
            {
                const double delta{magnitude - mLastMagnitude[k-1]};
                if(delta > 0.0) positiveFlux += delta;
            }
        }
        for(int k{1}; k <= InnerLast; ++k)
            mLastMagnitude[k-1] = std::abs(bins[k]);
        const double previousSum{mLastSum};
        mLastSum = magnitudeSum;
        mHasHistory = true;

        if(!(maxMagnitude > 0.0)) { mOld.clear(); return; }

        const double floorMagnitude{maxMagnitude * 1e-4};
        std::vector<Peak> current;
        for(int k{1}; k <= InnerLast; ++k)
        {
            const double magnitude{std::abs(bins[k])};
            if(magnitude < floorMagnitude) continue;
            if(!(magnitude > std::abs(bins[k-1]))) continue;
            if(magnitude < std::abs(bins[k+1])) continue;
            current.push_back(Peak{k, frequencies[k], std::arg(bins[k]), 0.0});
        }
        if(current.empty()) { mOld.clear(); return; }

        bool transient{false};
        if(threshold > 0.0 && positiveFlux / (previousSum + 1e-12) > threshold)
            transient = true;

        std::vector<char> used(mOld.size(), 0);
        for(size_t i{0}; i < current.size(); ++i)
        {
            int best{-1};
            double bestFreqDistance{1.5};
            for(size_t j{0}; j < mOld.size(); ++j)
            {
                if(used[j]) continue;
                if(std::abs(current[i].bin - mOld[j].bin) > 3) continue;
                const double freqDistance{std::abs(current[i].frequency - mOld[j].frequency)};
                if(!(freqDistance <= 1.5)) continue;
                if(best < 0 || freqDistance < bestFreqDistance
                    || (freqDistance == bestFreqDistance && static_cast<int>(j) < best))
                {
                    best = static_cast<int>(j);
                    bestFreqDistance = freqDistance;
                }
            }
            if(best >= 0)
            {
                used[best] = 1;
                if(!transient)
                    current[i].synthPhase = wrapPi(mOld[best].synthPhase
                        + Tau * double(StftHop) / double(StftSize)
                        * current[i].frequency * ratio);
                else
                    current[i].synthPhase = wrapPi(current[i].phase);
            }
            else
                current[i].synthPhase = wrapPi(current[i].phase);
        }

        int cursor{0};
        for(int k{1}; k <= InnerLast; ++k)
        {
            const double magnitude{std::abs(bins[k])};
            if(magnitude < floorMagnitude) continue;
            while(cursor < static_cast<int>(current.size()) && current[cursor].bin < k)
                ++cursor;
            int chosen;
            if(cursor == 0) chosen = 0;
            else if(cursor == static_cast<int>(current.size())) chosen = static_cast<int>(current.size())-1;
            else
            {
                const int leftDistance{k - current[cursor-1].bin};
                const int rightDistance{current[cursor].bin - k};
                chosen = (leftDistance <= rightDistance) ? cursor-1 : cursor;
            }
            const double phaseOffset{wrapPi(std::arg(bins[k]) - current[chosen].phase)};
            const double synthPhase{wrapPi(current[chosen].synthPhase + phaseOffset)};
            const int target{static_cast<int>(std::floor(k * ratio + 0.5))};
            if(target >= 1 && target <= InnerLast)
                output[target] += std::polar(magnitude, synthPhase);
        }

        mOld = current;
    }
};

/* The library's quantized pitch ratio: 16-bit fractional, truncated, clamped
 * to [0.5, 2]. Matches PshifterState::update on this build.
 */
float quantizedRatio(int coarse, int fine)
{
    constexpr int fracBits{16};
    constexpr int fracOne{1 << fracBits};
    constexpr int fracHalf{fracOne >> 1};
    const int tune{coarse * 100 + fine};
    const float pitch{std::pow(2.0f, static_cast<float>(tune) / 1200.0f)};
    const unsigned int q{clampu(fastf2u(pitch * fracOne),
        static_cast<unsigned int>(fracHalf), static_cast<unsigned int>(fracOne * 2))};
    return static_cast<float>(q) / static_cast<float>(fracOne);
}

/* All-float independent mirror of the legacy pitch-shifter pipeline. The
 * production legacy state (analysis/output phases, magnitudes, FIFO, OLA) is
 * entirely single precision, so a bit-faithful comparison has to stay in
 * float; the algorithm and FFT are still re-implemented here independently.
 */
void fftFloat(std::vector<std::complex<float>> &a, int sign)
{
    const int n{static_cast<int>(a.size())};
    for(int i{1}, j{0}; i < n; ++i)
    {
        int bit{n >> 1};
        for(; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if(i < j) std::swap(a[i], a[j]);
    }
    for(int len{2}; len <= n; len <<= 1)
    {
        const float ang{static_cast<float>(Pi) * sign / (len >> 1)};
        const std::complex<float> wn{std::cos(ang), std::sin(ang)};
        for(int i{0}; i < n; i += len)
        {
            std::complex<float> w{1.0f, 0.0f};
            for(int k{0}; k < len/2; ++k)
            {
                const auto u{a[i+k]};
                const auto v{a[i+k+len/2] * w};
                a[i+k] = u + v;
                a[i+k+len/2] = u - v;
                w *= wn;
            }
        }
    }
}

std::vector<float> referenceRenderLegacyFloat(const std::vector<float> &input, int coarse,
    int fine)
{
    const float ratio{quantizedRatio(coarse, fine)};
    std::vector<float> window(StftSize);
    for(int i{0}; i < StftSize/2; ++i)
    {
        const float v{std::sin((static_cast<float>(i) + 0.5f)
            * static_cast<float>(Pi) / StftSize)};
        window[i] = window[StftSize-1-i] = v * v;
    }

    std::vector<float> fifo(StftSize, 0.0f);
    std::vector<float> accum(StftSize, 0.0f);
    std::vector<float> lastPhase(BinCount, 0.0f);
    std::vector<float> sumPhase(BinCount, 0.0f);
    std::vector<std::complex<float>> fftBuffer(StftSize);
    std::vector<float> synMag(BinCount), synFreq(BinCount);
    int pos{StftSize - StftHop};

    const int hops{static_cast<int>(input.size()) / StftHop};
    std::vector<float> output;
    output.reserve(static_cast<size_t>(hops) * StftHop);

    constexpr float expectedCycles{static_cast<float>(Tau / 8.0)};
    constexpr float invPi{static_cast<float>(1.0 / Pi)};

    for(int hop{0}; hop < hops; ++hop)
    {
        for(int t{0}; t < StftHop; ++t)
            output.push_back(fifo[(pos+t) & (StftSize-1)]);
        for(int t{0}; t < StftHop; ++t)
            fifo[(pos+t) & (StftSize-1)] = input[hop*StftHop + t];
        pos = (pos + StftHop) & (StftSize-1);

        for(int k{0}; k < StftSize; ++k)
            fftBuffer[k] = std::complex<float>{fifo[(pos+k) & (StftSize-1)] * window[k], 0.0f};
        fftFloat(fftBuffer, -1);

        std::vector<float> freqBin(BinCount);
        for(int k{0}; k < BinCount; ++k)
        {
            const float magnitude{std::abs(fftBuffer[k])};
            const float phase{std::arg(fftBuffer[k])};
            float tmp{(phase - lastPhase[k]) - static_cast<float>(k % 8) * expectedCycles};
            lastPhase[k] = phase;
            tmp *= invPi;
            const int qpd{static_cast<int>(tmp)};
            tmp -= static_cast<float>(qpd + (qpd % 2));
            tmp *= 4.0f;
            freqBin[k] = static_cast<float>(k) + tmp;
            (void)magnitude;
        }

        std::fill(synMag.begin(), synMag.end(), 0.0f);
        std::fill(synFreq.begin(), synFreq.end(), 0.0f);
        for(int k{0}; k < BinCount; ++k)
        {
            const int target{static_cast<int>(std::floor(k * ratio + 0.5f))};
            if(target > InnerLast) continue;
            const float magnitude{std::abs(fftBuffer[k])};
            if(magnitude > synMag[target])
                synFreq[target] = freqBin[k] * ratio;
            synMag[target] += magnitude;
        }
        for(int k{0}; k < BinCount; ++k)
        {
            float tmp{sumPhase[k] + synFreq[k] * expectedCycles};
            tmp *= invPi;
            const int qpd{static_cast<int>(tmp)};
            tmp -= static_cast<float>(qpd + (qpd % 2));
            sumPhase[k] = tmp * static_cast<float>(Pi);
            fftBuffer[k] = std::polar(synMag[k], sumPhase[k]);
        }
        for(int k{BinCount}; k < StftSize; ++k)
            fftBuffer[k] = std::conj(fftBuffer[StftSize-k]);
        fftFloat(fftBuffer, 1);

        const float scale{3.0f / 8.0f / StftSize};
        for(int k{0}; k < StftSize; ++k)
            accum[(pos+k) & (StftSize-1)] += window[k] * fftBuffer[k].real() * scale;
        for(int t{0}; t < StftHop; ++t)
        {
            const int idx{(pos+t) & (StftSize-1)};
            fifo[idx] = accum[idx];
            accum[idx] = 0.0f;
        }
    }
    return output;
}

/* Double-precision STFT/OLA mirror of the production phase-locked pitch
 * shifter. The production kernel keeps its persistent peak phase in double,
 * so a double reference matches the wet PCM to a few parts in 1e-6.
 */
std::vector<float> referenceRender(const std::vector<float> &input, int coarse, int fine,
    double threshold)
{
    const float ratio{quantizedRatio(coarse, fine)};
    std::vector<double> window(StftSize);
    for(int i{0}; i < StftSize/2; ++i)
    {
        const double v{std::sin((static_cast<double>(i) + 0.5) * Pi / StftSize)};
        window[i] = window[StftSize-1-i] = v * v;
    }

    std::vector<double> fifo(StftSize, 0.0);
    std::vector<double> accum(StftSize, 0.0);
    std::vector<double> lastPhase(BinCount, 0.0);
    int pos{StftSize - StftHop};

    const int hops{static_cast<int>(input.size()) / StftHop};
    std::vector<float> output;
    output.reserve(static_cast<size_t>(hops) * StftHop);
    RefKernel kernel;

    for(int hop{0}; hop < hops; ++hop)
    {
        for(int t{0}; t < StftHop; ++t)
            output.push_back(static_cast<float>(fifo[(pos+t) & (StftSize-1)]));
        for(int t{0}; t < StftHop; ++t)
            fifo[(pos+t) & (StftSize-1)] = input[hop*StftHop + t];
        pos = (pos + StftHop) & (StftSize-1);

        std::vector<cd> frame(StftSize);
        for(int k{0}; k < StftSize; ++k)
            frame[k] = fifo[(pos+k) & (StftSize-1)] * window[k];
        fftDouble(frame, -1);

        std::vector<double> magnitude(BinCount), freqBin(BinCount), phase(BinCount);
        for(int k{0}; k < BinCount; ++k)
        {
            magnitude[k] = std::abs(frame[k]);
            phase[k] = std::arg(frame[k]);
            double tmp{(phase[k] - lastPhase[k]) - (k % 8) * (Tau / 8.0)};
            lastPhase[k] = phase[k];
            tmp /= Pi;
            const int q{static_cast<int>(tmp)};
            tmp -= q + (q % 2);
            tmp *= 4.0;
            freqBin[k] = k + tmp;
        }

        std::vector<cd> spectrum(BinCount, cd{0.0, 0.0});
        {
            std::vector<cd> analysis(BinCount);
            for(int k{0}; k < BinCount; ++k)
                analysis[k] = std::polar(magnitude[k], phase[k]);
            kernel.process(analysis, freqBin, ratio, threshold, spectrum);
        }

        for(int k{0}; k < BinCount; ++k) frame[k] = spectrum[k];
        for(int k{BinCount}; k < StftSize; ++k)
            frame[k] = std::conj(frame[StftSize-k]);
        fftDouble(frame, 1);

        const double scale{3.0 / 8.0 / StftSize};
        for(int k{0}; k < StftSize; ++k)
            accum[(pos+k) & (StftSize-1)] += window[k] * frame[k].real() * scale;
        for(int t{0}; t < StftHop; ++t)
        {
            const int idx{(pos+t) & (StftSize-1)};
            fifo[idx] = accum[idx];
            accum[idx] = 0.0;
        }
    }
    return output;
}

/* Stereo N3D decoder gain for a front-center (W-only ambisonic) wet signal:
 * row coefficient 0.5 on W plus 0.0552305643 on the sqrt(3) X component.
 */
constexpr double DecoderGain{0.5 + 0.0552305643 * 1.7320508075688772};

bool closeComplex(cd got, cd expected)
{
    const double distance{std::abs(got - expected)};
    return distance <= 2e-4 + 2e-5 * std::abs(expected);
}

cd toDouble(const std::complex<float> &v)
{ return cd{v.real(), v.imag()}; }


/* ------------------------------------------------------------------ */
/* Spectral group: per-bin contract against the double reference.     */
/* ------------------------------------------------------------------ */

int spectralSilenceAndEndpoints()
{
    al::PhaseLock1024 kernel;
    kernel.reset();
    std::array<al::PhaseLockBin, BinCount> bins{};
    std::array<std::complex<float>, BinCount> out{};
    /* A completely silent frame clears trajectories and outputs zero. */
    kernel.process(bins.data(), 1.0f, 0.5f, out.data());
    for(int k{0}; k < BinCount; ++k)
        TASK_CHECK(out[k].real() == 0.0f && out[k].imag() == 0.0f);
    return 0;
}

int spectralNeighborhoodPhase()
{
    al::PhaseLock1024 kernel;
    kernel.reset();
    RefKernel reference;

    /* Two frames of a single broad peak around bin 40. */
    for(int frame{0}; frame < 2; ++frame)
    {
        std::array<al::PhaseLockBin, BinCount> bins{};
        std::vector<cd> refBins(BinCount, cd{0.0, 0.0});
        std::vector<double> refFreq(BinCount, 0.0);
        for(int k{35}; k <= 45; ++k)
        {
            const float magnitude{1.0f - 0.08f * std::abs(k - 40)};
            const float phase{static_cast<float>(wrapPi(0.37 * k + frame * 0.11))};
            bins[k] = al::PhaseLockBin{magnitude, phase, 40.0f + 0.1f * frame};
            refBins[k] = std::polar(static_cast<double>(magnitude), static_cast<double>(phase));
            refFreq[k] = 40.0 + 0.1 * frame;
        }
        std::array<std::complex<float>, BinCount> out{};
        kernel.process(bins.data(), 1.0f, 0.0f, out.data());

        std::vector<cd> refOut;
        reference.process(refBins, refFreq, 1.0, 0.0, refOut);
        for(int k{0}; k < BinCount; ++k)
            TASK_CHECK(closeComplex(toDouble(out[k]), refOut[k]));

        /* Explicit neighborhood check: every bin keeps the peak's phase
         * relationship, and DC/Nyquist stay zero.
         */
        TASK_CHECK(out[0] == std::complex<float>{});
        TASK_CHECK(out[InnerLast+1] == std::complex<float>{});
        const float peakInputPhase{bins[40].phase};
        for(int k{35}; k <= 45; ++k)
        {
            const float expectedPhase{wrapPiF(std::arg(out[40])
                + wrapPi(bins[k].phase - peakInputPhase))};
            TASK_CHECK(std::abs(wrapPi(std::arg(out[k]) - expectedPhase)) <= 2e-6f);
            TASK_CHECK(std::abs(std::abs(out[k]) - bins[k].magnitude) <= 2e-6f);
        }
    }
    return 0;
}

int spectralComplexCollisionAndCancellation()
{
    /* The -5-semitone ratio (~0.749) packs more than one source bin onto a
     * target bin. Pick an actual isolated colliding pair at runtime.
     */
    const float ratio{quantizedRatio(-5, 0)};
    TASK_CHECK(ratio > 0.74f && ratio < 0.76f);
    int k1{-1}, k2{-1}, target{-1};
    for(int k{20}; k < 180; ++k)
    {
        const int t{static_cast<int>(std::floor(k * ratio + 0.5))};
        if(static_cast<int>(std::floor((k+1) * ratio + 0.5)) == t
            && static_cast<int>(std::floor((k-1) * ratio + 0.5)) != t
            && static_cast<int>(std::floor((k+2) * ratio + 0.5)) != t)
        { k1 = k; k2 = k+1; target = t; break; }
    }
    TASK_CHECK(k1 > 0 && k2 > 0 && target > 0);

    al::PhaseLock1024 kernel;
    kernel.reset();
    std::array<al::PhaseLockBin, BinCount> bins{};
    /* One strict peak far away, then a 0.5 plateau holding the colliding pair.
     * The plateau's only peak is its leftmost bin, so on this newborn frame
     * every plateau bin synthesizes at its own input phase.
     */
    bins[9]  = al::PhaseLockBin{0.30f, 0.20f, 9.0f};
    bins[10] = al::PhaseLockBin{1.00f, 0.30f, 10.0f};
    bins[11] = al::PhaseLockBin{0.30f, 0.40f, 11.0f};
    bins[k1-1] = al::PhaseLockBin{0.40f, 0.10f, static_cast<float>(k1-1)};
    bins[k1]   = al::PhaseLockBin{0.50f, 0.60f, static_cast<float>(k1)};
    bins[k2]   = al::PhaseLockBin{0.50f, wrapPiF(0.60 + Pi - 0.01),
        static_cast<float>(k2)};
    std::array<std::complex<float>, BinCount> out{};
    kernel.process(bins.data(), ratio, 0.0f, out.data());

    /* Hand-computed complex sum: only k1 and k2 land on the collision target,
     * and newborn-bin synthesis phases equal the input phases.
     */
    const cd c1{std::polar(0.5, 0.6)};
    const cd c2{std::polar(0.5, wrapPi(0.6 + Pi - 0.01))};
    const cd expected{c1 + c2};
    TASK_CHECK(std::abs(toDouble(out[target]) - expected) <= 2e-4 + 2e-5*std::abs(expected));
    /* Near-cancellation leaves a small complex residual, never the 1.0 that
     * legacy magnitude accumulation would produce.
     */
    TASK_CHECK(std::abs(out[target]) < 0.05f);
    TASK_CHECK(std::abs(std::abs(out[target]) - 1.0f) > 0.5f);
    /* The dominant-frequency overwrite rule cannot represent two contributions
     * either: the target must retain both magnitudes' complex energy.
     */
    TASK_CHECK(std::abs(toDouble(out[target]) - c1) > 0.2);
    return 0;
}

int spectralFloorAssignment()
{
    /* Bins below the 1e-4 floor contribute nothing; above-floor bins attach to
     * the nearest peak, ties going to the lower peak.
     */
    al::PhaseLock1024 kernel;
    kernel.reset();
    RefKernel reference;
    std::array<al::PhaseLockBin, BinCount> bins{};
    std::vector<cd> refBins(BinCount, cd{0.0, 0.0});
    std::vector<double> refFreq(BinCount, 0.0);
    for(int peak : {20, 30})
    {
        bins[peak-1] = al::PhaseLockBin{0.60f, wrapPiF(0.2f*peak - 0.1f),
            static_cast<float>(peak)};
        bins[peak]   = al::PhaseLockBin{1.00f, wrapPiF(0.2f*peak),
            static_cast<float>(peak)};
        bins[peak+1] = al::PhaseLockBin{0.60f, wrapPiF(0.2f*peak + 0.1f),
            static_cast<float>(peak)};
        refBins[peak-1] = std::polar(0.6, 0.2*peak - 0.1);
        refBins[peak]   = std::polar(1.0, 0.2*peak);
        refBins[peak+1] = std::polar(0.6, 0.2*peak + 0.1);
        for(int k : {peak-1, peak, peak+1}) refFreq[k] = static_cast<double>(peak);
    }
    /* Midpoint bin 25 is equidistant from peaks 20 and 30: tie goes low. */
    bins[25] = al::PhaseLockBin{0.5f, 1.1f, 25.0f};
    refBins[25] = std::polar(0.5, 1.1);
    refFreq[25] = 25.0;
    /* Below-floor bin is ignored. */
    bins[27] = al::PhaseLockBin{5e-6f, 0.4f, 27.0f};

    std::array<std::complex<float>, BinCount> out{};
    kernel.process(bins.data(), 1.0f, 0.0f, out.data());
    std::vector<cd> refOut;
    reference.process(refBins, refFreq, 1.0, 0.0, refOut);
    for(int k{0}; k < BinCount; ++k)
        TASK_CHECK(closeComplex(toDouble(out[k]), refOut[k]));
    return 0;
}


/* ------------------------------------------------------------------ */
/* Tracking group: peak identities, resets and rebirth.               */
/* ------------------------------------------------------------------ */

void makeSinglePeak(std::array<al::PhaseLockBin, BinCount> &bins,
    std::vector<cd> &refBins, std::vector<double> &refFreq, int center, double phaseSeed,
    double frequency)
{
    bins.fill(al::PhaseLockBin{});
    std::fill(refBins.begin(), refBins.end(), cd{0.0, 0.0});
    std::fill(refFreq.begin(), refFreq.end(), 0.0);
    for(int k{center-2}; k <= center+2; ++k)
    {
        const float magnitude{1.0f - 0.1f * std::abs(k-center)};
        const float phase{static_cast<float>(wrapPi(phaseSeed + 0.13 * (k-center)))};
        bins[k] = al::PhaseLockBin{magnitude, phase, static_cast<float>(frequency)};
        refBins[k] = std::polar(static_cast<double>(magnitude), static_cast<double>(phase));
        refFreq[k] = frequency;
    }
}

int trackingMovingPeakInheritance()
{
    al::PhaseLock1024 kernel;
    kernel.reset();
    RefKernel reference;
    const float ratio{1.0f};

    /* The peak walks one bin per frame; its instantaneous frequency moves
     * with it, so consecutive peaks keep matching and the synthesis phase
     * advances instead of restarting.
     */
    for(int frame{0}; frame < 8; ++frame)
    {
        const int center{30 + frame};
        std::array<al::PhaseLockBin, BinCount> bins{};
        std::vector<cd> refBins(BinCount, cd{0.0, 0.0});
        std::vector<double> refFreq(BinCount, 0.0);
        makeSinglePeak(bins, refBins, refFreq, center, 0.5 + frame * 0.07,
            static_cast<double>(center) + 0.25);

        std::array<std::complex<float>, BinCount> out{};
        kernel.process(bins.data(), ratio, 0.0f, out.data());
        std::vector<cd> refOut;
        reference.process(refBins, refFreq, ratio, 0.0, refOut);
        for(int k{0}; k < BinCount; ++k)
            TASK_CHECK(closeComplex(toDouble(out[k]), refOut[k]));
    }

    /* After the first frame every later frame is an inherited peak: its
     * synthesis phase leads the input peak phase by the seven accumulated
     * advances, not the newborn value of (0.5 + 7*0.07).
     */
    TASK_CHECK(kernel.mPeakCount == 1);
    TASK_CHECK(kernel.mPeaks[0].bin == 37);
    double expectedPhase{0.5};
    for(int frame{1}; frame < 8; ++frame)
        expectedPhase = wrapPi(expectedPhase
            + Tau * StftHop / StftSize * (30.0 + frame + 0.25));
    TASK_CHECK(std::abs(wrapPi(kernel.mPeaks[0].synthPhase - expectedPhase)) <= 2e-6);
    TASK_CHECK(std::abs(wrapPi(kernel.mPeaks[0].synthPhase - (0.5 + 7*0.07))) > 0.01);
    return 0;
}

int trackingOneToOne()
{
    al::PhaseLock1024 kernel;
    kernel.reset();
    RefKernel reference;

    /* Frame 1: two peaks. Frame 2: one peak within range of both old peaks;
     * only the closer (in instantaneous frequency) old peak may match.
     */
    for(int frame{0}; frame < 2; ++frame)
    {
        std::array<al::PhaseLockBin, BinCount> bins{};
        std::vector<cd> refBins(BinCount, cd{0.0, 0.0});
        std::vector<double> refFreq(BinCount, 0.0);
        if(frame == 0)
        {
            for(int peak : {40, 44})
            {
                bins[peak-1] = al::PhaseLockBin{0.4f, 0.2f, static_cast<float>(peak)};
                bins[peak]   = al::PhaseLockBin{1.0f, 0.3f, static_cast<float>(peak)};
                bins[peak+1] = al::PhaseLockBin{0.4f, 0.2f, static_cast<float>(peak)};
                refBins[peak-1] = std::polar(0.4, 0.2);
                refBins[peak]   = std::polar(1.0, 0.3);
                refBins[peak+1] = std::polar(0.4, 0.2);
                for(int k : {peak-1, peak, peak+1})
                    refFreq[k] = static_cast<double>(peak);
            }
        }
        else
        {
            /* New single peak at 42: bin distance 2 from both old peaks.
             * Frequency 42.6 is 1.4 bins from old peak 44 but 2.6 from old
             * peak 40, so only peak 44 is an admissible match.
             */
            const float lobe[5]{0.2f, 0.4f, 1.0f, 0.4f, 0.2f};
            for(int k{40}; k <= 44; ++k)
            {
                const float magnitude{lobe[k-40]};
                bins[k] = al::PhaseLockBin{magnitude, 0.9f, 42.6f};
                refBins[k] = std::polar(static_cast<double>(magnitude), 0.9);
                refFreq[k] = 42.6;
            }
        }
        std::array<std::complex<float>, BinCount> out{};
        kernel.process(bins.data(), 1.0f, 0.0f, out.data());
        std::vector<cd> refOut;
        reference.process(refBins, refFreq, 1.0, 0.0, refOut);
        for(int k{0}; k < BinCount; ++k)
            TASK_CHECK(closeComplex(toDouble(out[k]), refOut[k]));
    }
    TASK_CHECK(kernel.mPeakCount == 1);
    /* Matched against old peak 44 (frequency distance 1.4 vs 2.6 for peak 40,
     * which fails the 1.5 limit outright), so the synthesis phase continues
     * from the old peak's newborn phase instead of restarting at 0.9.
     */
    const double expected{wrapPi(0.3 + Tau * StftHop / StftSize * 42.6)};
    TASK_CHECK(std::abs(wrapPi(kernel.mPeaks[0].synthPhase - expected)) <= 2e-6);
    return 0;
}

int trackingFluxReset()
{
    al::PhaseLock1024 kernel;
    kernel.reset();

    /* Establish a quiet steady peak, then a loud onset on the next frame. The
     * positive spectral flux exceeds the threshold and every peak restarts at
     * its current input phase.
     */
    std::array<al::PhaseLockBin, BinCount> bins{};
    const float quietLobe[5]{0.04f, 0.07f, 0.10f, 0.07f, 0.04f};
    for(int k{48}; k <= 52; ++k)
        bins[k] = al::PhaseLockBin{quietLobe[k-48], 0.4f, 50.0f};
    std::array<std::complex<float>, BinCount> out{};
    kernel.process(bins.data(), 1.0f, 0.5f, out.data());
    TASK_CHECK(kernel.mPeakCount == 1);
    const double continued{wrapPi(kernel.mPeaks[0].synthPhase
        + Tau * StftHop / StftSize * 50.0)};

    const float loudLobe[5]{0.4f, 0.7f, 1.0f, 0.7f, 0.4f};
    for(int k{48}; k <= 52; ++k)
        bins[k] = al::PhaseLockBin{loudLobe[k-48], 1.7f, 50.0f};
    kernel.process(bins.data(), 1.0f, 0.5f, out.data());
    TASK_CHECK(kernel.mPeakCount == 1);
    /* Transient: restarted at the input phase, not the continued phase. */
    TASK_CHECK(std::abs(wrapPi(kernel.mPeaks[0].synthPhase - 1.7)) <= 2e-6);
    TASK_CHECK(std::abs(wrapPi(kernel.mPeaks[0].synthPhase - continued)) > 1e-3);

    /* With the transient threshold disabled the same onset would continue. */
    kernel.reset();
    for(int k{48}; k <= 52; ++k)
        bins[k] = al::PhaseLockBin{quietLobe[k-48], 0.4f, 50.0f};
    kernel.process(bins.data(), 1.0f, 0.0f, out.data());
    for(int k{48}; k <= 52; ++k)
        bins[k] = al::PhaseLockBin{loudLobe[k-48], 1.7f, 50.0f};
    kernel.process(bins.data(), 1.0f, 0.0f, out.data());
    TASK_CHECK(std::abs(wrapPi(kernel.mPeaks[0].synthPhase - continued)) <= 2e-6);
    return 0;
}

int trackingSilenceRebirth()
{
    al::PhaseLock1024 kernel;
    kernel.reset();

    std::array<al::PhaseLockBin, BinCount> bins{};
    const float lobe[5]{0.4f, 0.7f, 1.0f, 0.7f, 0.4f};
    for(int k{60}; k <= 64; ++k)
        bins[k] = al::PhaseLockBin{lobe[k-60], 0.8f, 62.0f};
    std::array<std::complex<float>, BinCount> out{};
    kernel.process(bins.data(), 1.0f, 0.0f, out.data());
    TASK_CHECK(kernel.mPeakCount == 1);

    /* Silent frames clear the trajectories. */
    bins.fill(al::PhaseLockBin{});
    for(int silence{0}; silence < 3; ++silence)
    {
        kernel.process(bins.data(), 1.0f, 0.0f, out.data());
        TASK_CHECK(kernel.mPeakCount == 0);
        for(int k{0}; k < BinCount; ++k)
            TASK_CHECK(out[k] == std::complex<float>{});
    }

    /* First voiced frame after silence is a newborn: synthesis phase equals
     * the current input phase even though the peak sits at the same bin.
     */
    for(int k{60}; k <= 64; ++k)
        bins[k] = al::PhaseLockBin{lobe[k-60], 2.1f, 62.0f};
    kernel.process(bins.data(), 1.0f, 0.0f, out.data());
    TASK_CHECK(kernel.mPeakCount == 1);
    TASK_CHECK(std::abs(wrapPi(kernel.mPeaks[0].synthPhase - 2.1)) <= 2e-6);

    /* Fresh object and a reset object behave identically. */
    al::PhaseLock1024 fresh;
    al::PhaseLock1024 reset;
    reset.reset();
    std::array<std::complex<float>, BinCount> outFresh{}, outReset{};
    fresh.process(bins.data(), 1.0f, 0.0f, outFresh.data());
    reset.process(bins.data(), 1.0f, 0.0f, outReset.data());
    for(int k{0}; k < BinCount; ++k)
        TASK_CHECK(outFresh[k] == outReset[k]);
    return 0;
}

int trackingPhaseWrapped()
{
    al::PhaseLock1024 kernel;
    kernel.reset();
    std::array<al::PhaseLockBin, BinCount> bins{};
    std::array<std::complex<float>, BinCount> out{};
    /* Many frames force repeated wrapping of the synthesis phase. */
    for(int frame{0}; frame < 400; ++frame)
    {
        for(int k{10}; k <= 14; ++k)
            bins[k] = al::PhaseLockBin{1.0f, wrapPiF(0.01f * frame + 0.05f*k), 12.7f};
        kernel.process(bins.data(), 1.0f, 0.0f, out.data());
        for(int i{0}; i < kernel.mPeakCount; ++i)
        {
            TASK_CHECK(kernel.mPeaks[i].synthPhase >= -Pi);
            TASK_CHECK(kernel.mPeaks[i].synthPhase < Pi);
        }
        for(int k{0}; k < BinCount; ++k)
        {
            const float p{std::arg(out[k])};
            if(std::abs(out[k]) > 0.0f)
            {
                TASK_CHECK(p >= -static_cast<float>(Pi));
                TASK_CHECK(p < static_cast<float>(Pi));
            }
        }
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* Render group: real AL effect through a loopback device.            */
/* ------------------------------------------------------------------ */

void expectAl(bool condition, const char *message)
{
    if(!condition)
    {
        std::fprintf(stderr, "AL failure: %s (AL=%x ALC=%x)\n", message,
            alGetError(), alcGetError(nullptr));
        std::exit(1);
    }
}

struct Loopback {
    ALCdevice *device{nullptr};
    ALCcontext *context{nullptr};
    ALuint buffer{0}, source{0}, effect{0}, slot{0}, filter{0};

    void open()
    {
        device = alcLoopbackOpenDeviceSOFT(nullptr);
        expectAl(device != nullptr, "loopback open");
        const ALCint attrs[] = {
            ALC_FREQUENCY, 48000,
            ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
            ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
            ALC_OUTPUT_LIMITER_SOFT, ALC_FALSE,
            ALC_HRTF_SOFT, ALC_FALSE,
            0};
        context = alcCreateContext(device, attrs);
        expectAl(context != nullptr && alcMakeContextCurrent(context), "context");
        alDistanceModel(AL_NONE);
        alGenBuffers(1, &buffer);
        alGenSources(1, &source);
        alGenEffects(1, &effect);
        alGenAuxiliaryEffectSlots(1, &slot);
        alGenFilters(1, &filter);
        alFilteri(filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        alFilterf(filter, AL_LOWPASS_GAIN, 0.0f);
        alFilterf(filter, AL_LOWPASS_GAINHF, 1.0f);
        expectAl(alGetError() == AL_NO_ERROR, "object setup");
    }

    void configure(int coarse, int fine, int phaseLock, float threshold) const
    {
        alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_PITCH_SHIFTER);
        alEffecti(effect, AL_PITCH_SHIFTER_COARSE_TUNE, coarse);
        alEffecti(effect, AL_PITCH_SHIFTER_FINE_TUNE, fine);
        alEffecti(effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, phaseLock);
        alEffectf(effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, threshold);
        expectAl(alGetError() == AL_NO_ERROR, "effect configure");
        alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, effect);
        expectAl(alGetError() == AL_NO_ERROR, "slot attach");
    }

    void play(const std::vector<float> &input) const
    {
        alBufferData(buffer, AL_FORMAT_MONO_FLOAT32, input.data(),
            static_cast<ALsizei>(input.size() * sizeof(float)), 48000);
        alSourcei(source, AL_BUFFER, buffer);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSourcei(source, AL_DIRECT_FILTER, filter);
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, slot, 0, AL_FILTER_NULL);
        alSourcePlay(source);
        expectAl(alGetError() == AL_NO_ERROR, "source play");
    }

    void close()
    {
        alDeleteSources(1, &source);
        alDeleteBuffers(1, &buffer);
        alDeleteAuxiliaryEffectSlots(1, &slot);
        alDeleteEffects(1, &effect);
        alDeleteFilters(1, &filter);
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
        expectAl(alcCloseDevice(device), "close");
    }
};

std::vector<float> renderWet(const std::vector<float> &input, int coarse, int fine,
    int phaseLock, float threshold, const int *chunkSizes = nullptr)
{
    Loopback lb;
    lb.open();
    lb.configure(coarse, fine, phaseLock, threshold);
    lb.play(input);

    std::vector<float> interleaved(input.size() * 2, 0.0f);
    size_t offset{0};
    int chunkIndex{0};
    while(offset < input.size())
    {
        size_t count{input.size() - offset};
        if(chunkSizes)
            count = std::min(count, static_cast<size_t>(chunkSizes[chunkIndex++ % 4]));
        alcRenderSamplesSOFT(lb.device, interleaved.data() + offset*2,
            static_cast<ALCsizei>(count));
        offset += count;
    }
    expectAl(alGetError() == AL_NO_ERROR && alcGetError(lb.device) == ALC_NO_ERROR,
        "render");
    std::vector<float> wet(input.size());
    for(size_t n{0}; n < input.size(); ++n)
        wet[n] = interleaved[n*2];
    lb.close();
    return wet;
}

std::vector<float> makeHarmonics(int length)
{
    std::vector<float> input(length);
    for(int n{0}; n < length; ++n)
        input[n] = 0.08f * std::sin(Tau * 337.3 * n / 48000.0)
            + 0.05f * std::sin(Tau * 751.7 * n / 48000.0);
    return input;
}

std::vector<float> makeGlide(int length)
{
    std::vector<float> input(length);
    double phase{0.0};
    for(int n{0}; n < length; ++n)
    {
        const double frequency{400.0 + 600.0 * n / length};
        phase += Tau * frequency / 48000.0;
        input[n] = 0.10f * std::sin(phase) + 0.04f * std::sin(2.0 * phase);
    }
    return input;
}

std::vector<float> makePulses(int length)
{
    std::vector<float> input(length, 0.0f);
    for(int pulse{0}; pulse < 4; ++pulse)
    {
        const int start{256 + pulse * 3840};
        double phase{0.0};
        for(int n{start}; n < start + 1500 && n < length; ++n)
        {
            const double envelope{std::sin(Pi * (n-start) / 1500.0)};
            const double frequency{500.0 + 33.7 * (n-start)};
            phase += Tau * frequency / 48000.0;
            input[n] = static_cast<float>(0.12 * envelope * std::sin(phase));
        }
    }
    return input;
}

int compareStable(const std::vector<float> &wet, const std::vector<float> &reference,
    double tolerance, const char *tag)
{
    TASK_CHECK(wet.size() == reference.size());
    double maxError{0.0};
    size_t worstFrame{0};
    /* The first 3072 output frames cover the FIFO fill and OLA buildup; the
     * final partial window is still draining. Everything in between is
     * steady-state.
     */
    for(size_t n{3072}; n + 128 <= wet.size(); ++n)
    {
        const double error{std::abs(static_cast<double>(wet[n]) - DecoderGain * reference[n])};
        if(error > maxError)
        {
            maxError = error;
            worstFrame = n;
        }
    }
    std::printf("  [%s] stable max_error=%.3g at %zu\n", tag, maxError, worstFrame);
    TASK_CHECK(maxError <= tolerance);
    return 0;
}

int renderHarmonics()
{
    const int length{24576};
    const auto input{makeHarmonics(length)};
    const auto wet{renderWet(input, 0, 0, 1, 0.0f)};
    const auto reference{referenceRender(input, 0, 0, 0.0)};
    return compareStable(wet, reference, 3e-4, "harmonics");
}

int renderGlide()
{
    /* Threshold zero: the transient decision itself is covered by the
     * tracking group on exact kernel inputs, so render comparisons avoid
     * float/double flux thresholds straddling a boundary.
     */
    const int length{24576};
    const auto input{makeGlide(length)};
    const auto wet{renderWet(input, 7, 0, 1, 0.0f)};
    const auto reference{referenceRender(input, 7, 0, 0.0)};
    return compareStable(wet, reference, 3e-4, "glide");
}

int renderPulses()
{
    const int length{24576};
    const auto input{makePulses(length)};
    const auto wet{renderWet(input, -5, 0, 1, 0.0f)};
    const auto reference{referenceRender(input, -5, 0, 0.0)};
    return compareStable(wet, reference, 3e-4, "pulses");
}

int renderLegacyBaseline()
{
    /* Phase lock disabled must keep the legacy Bernsee spectrum, including
     * its magnitude-accumulation collisions. The legacy path is entirely
     * single precision, so its independent mirror is too.
     */
    const int length{24576};
    const auto input{makeGlide(length)};
    const auto wet{renderWet(input, 7, 0, 0, 0.0f)};
    const auto legacy{referenceRenderLegacyFloat(input, 7, 0)};
    if(compareStable(wet, legacy, 3e-4, "legacy") != 0) return 1;

    /* The phase-locked and legacy wet signals must actually differ on this
     * moving spectrum: independent per-bin phases versus coherent peaks.
     */
    const auto locked{renderWet(input, 7, 0, 1, 0.0f)};
    double maxDifference{0.0};
    for(size_t n{3072}; n + 128 <= wet.size(); ++n)
        maxDifference = std::max(maxDifference, std::abs(double(locked[n] - wet[n])));
    TASK_CHECK(maxDifference > 1e-3);
    return 0;
}

int renderChunkContinuity()
{
    /* The mixer's gain ramp counter depends on the chunk length for its first
     * 512 samples, so chunk-invariance is checked once the gains have settled
     * (the initial mixing gain is accounted for separately in the references).
     */
    const int length{24576};
    const auto input{makeGlide(length)};
    const int chunks[4]{1, 7, 257, 1025};
    const auto whole{renderWet(input, 7, 0, 1, 0.0f)};
    const auto split{renderWet(input, 7, 0, 1, 0.0f, chunks)};
    TASK_CHECK(whole.size() == split.size());
    double maxDifference{0.0};
    size_t worstFrame{0};
    for(size_t n{2048}; n < whole.size(); ++n)
    {
        const double difference{std::abs(double(whole[n] - split[n]))};
        if(difference > maxDifference) { maxDifference = difference; worstFrame = n; }
        TASK_CHECK(std::isfinite(whole[n]) && std::isfinite(split[n]));
    }
    std::printf("  chunk max_difference=%.3g at %zu (post-ramp)\n", maxDifference, worstFrame);
    TASK_CHECK(maxDifference <= 2e-5);
    return 0;
}

int renderSwitchAtHopInterior()
{
    /* The setting changes on a non-hop boundary. One-shot and chunked renders
     * must publish identical samples at the same absolute output frame once
     * the mixer's chunk-dependent initial gain ramp has settled (the same
     * ramp the existing pitch baseline avoids by comparing past frame 8192).
     */
    const int length{24576};
    const auto input{makePulses(length)};
    const int switchFrame{4000}; /* 4000/128 is not an integer hop boundary. */
    TASK_CHECK((switchFrame % StftHop) != 0);
    const int chunks[4]{1, 7, 257, 1025};

    auto run = [&](const int *chunkSizes) {
        Loopback lb;
        lb.open();
        lb.configure(-5, 0, 1, 0.5f);
        lb.play(input);
        std::vector<float> wet(input.size());
        int chunkIndex{0};
        auto renderBlock = [&](size_t count) {
            std::vector<float> block(count * 2);
            alcRenderSamplesSOFT(lb.device, block.data(), static_cast<ALCsizei>(count));
            return block;
        };
        /* Phase 1: render exactly up to the switch frame, optionally split
         * into the irregular chunk pattern.
         */
        size_t offset{0};
        while(offset < static_cast<size_t>(switchFrame))
        {
            size_t count{static_cast<size_t>(switchFrame) - offset};
            if(chunkSizes)
                count = std::min(count, static_cast<size_t>(chunks[chunkIndex++ % 4]));
            const auto block{renderBlock(count)};
            for(size_t i{0}; i < count; ++i) wet[offset+i] = block[i*2];
            offset += count;
        }
        /* Re-attach the same slot with a new threshold at a sample position
         * inside an unfinished hop (2000 is not a multiple of the 128 hop).
         */
        alEffectf(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, 0.0f);
        expectAl(alGetError() == AL_NO_ERROR, "threshold switch");
        alAuxiliaryEffectSloti(lb.slot, AL_EFFECTSLOT_EFFECT, lb.effect);
        expectAl(alGetError() == AL_NO_ERROR, "reattach");

        /* Phase 2: drain the rest, keeping the same chunk pattern. */
        while(offset < input.size())
        {
            size_t count{input.size() - offset};
            if(chunkSizes)
                count = std::min(count, static_cast<size_t>(chunks[chunkIndex++ % 4]));
            const auto block{renderBlock(count)};
            for(size_t i{0}; i < count; ++i) wet[offset+i] = block[i*2];
            offset += count;
        }
        lb.close();
        return wet;
    };

    const auto oneShot{run(nullptr)};
    const auto chunked{run(chunks)};
    double switchMaxDifference{0.0};
    for(size_t n{2048}; n < oneShot.size(); ++n)
    {
        switchMaxDifference = std::max(switchMaxDifference,
            std::abs(double(oneShot[n] - chunked[n])));
        TASK_CHECK(std::abs(double(oneShot[n] - chunked[n])) <= 2e-5);
    }
    std::printf("  switch max_difference=%.3g (post-ramp)\n", switchMaxDifference);

    /* Before the switch, output must be identical to a run that never
     * switches: unfinished hops cannot mix two settings. Both renders issue
     * only >=1024-frame loopback calls here, so their initial ramps match too.
     */
    const auto control{renderWet(input, -5, 0, 1, 0.5f)};
    for(int n{0}; n < switchFrame; ++n)
        TASK_CHECK(std::abs(double(oneShot[n] - control[n])) <= 2e-6);
    return 0;
}

int renderParameterValidation()
{
    Loopback lb;
    lb.open();

    TASK_CHECK(alGetEnumValue("AL_PITCH_SHIFTER_PHASE_LOCK_SOFT") == 0x0003);
    TASK_CHECK(alGetEnumValue("AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT") == 0x0004);

    alEffecti(lb.effect, AL_EFFECT_TYPE, AL_EFFECT_PITCH_SHIFTER);
    int lockValue{-99};
    float thresholdValue{-99.0f};
    alGetEffecti(lb.effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, &lockValue);
    alGetEffectf(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, &thresholdValue);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    TASK_CHECK(lockValue == 0);
    TASK_CHECK(std::abs(thresholdValue - 0.5f) < 1e-7f);

    /* Valid scalar and vector setters/getters, both integer and float. */
    alEffecti(lb.effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, 1);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    const int one{1};
    alEffectiv(lb.effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, &one);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alGetEffectiv(lb.effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, &lockValue);
    TASK_CHECK(lockValue == 1);

    alEffectf(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, 1.25f);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    const float tv{0.75f};
    alEffectfv(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, &tv);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alGetEffectfv(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, &thresholdValue);
    TASK_CHECK(std::abs(thresholdValue - 0.75f) < 1e-7f);

    /* Out-of-range integer values: AL_INVALID_VALUE, last good value kept. */
    for(int bad : {-1, 2, 100})
    {
        alEffecti(lb.effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, bad);
        TASK_CHECK(alGetError() == AL_INVALID_VALUE);
        alGetEffecti(lb.effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, &lockValue);
        TASK_CHECK(lockValue == 1);
    }
    /* Out-of-range or non-finite floats: AL_INVALID_VALUE. */
    for(float bad : {-0.1f, 2.1f, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()})
    {
        alEffectf(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, bad);
        TASK_CHECK(alGetError() == AL_INVALID_VALUE);
        alGetEffectf(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, &thresholdValue);
        TASK_CHECK(std::abs(thresholdValue - 0.75f) < 1e-7f);
    }
    /* Zero threshold is valid and disables transient resets. */
    const float zero{0.0f};
    alEffectfv(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, &zero);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alGetEffectf(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, &thresholdValue);
    TASK_CHECK(thresholdValue == 0.0f);

    /* Wrong type for the parameter: AL_INVALID_ENUM. */
    alEffectf(lb.effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, 1.0f);
    TASK_CHECK(alGetError() == AL_INVALID_ENUM);
    alEffecti(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, 1);
    TASK_CHECK(alGetError() == AL_INVALID_ENUM);
    float asFloat{-99.0f};
    int asInt{-99};
    alGetEffectf(lb.effect, AL_PITCH_SHIFTER_PHASE_LOCK_SOFT, &asFloat);
    TASK_CHECK(alGetError() == AL_INVALID_ENUM);
    alGetEffecti(lb.effect, AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT, &asInt);
    TASK_CHECK(alGetError() == AL_INVALID_ENUM);

    lb.close();
    return 0;
}


/* ------------------------------------------------------------------ */
/* Registration.                                                       */
/* ------------------------------------------------------------------ */

} // namespace

const task_case task_cases[] = {
    {"spectral", "silence_and_endpoints",    spectralSilenceAndEndpoints},
    {"spectral", "neighborhood_phase",       spectralNeighborhoodPhase},
    {"spectral", "complex_collision",        spectralComplexCollisionAndCancellation},
    {"spectral", "floor_assignment",         spectralFloorAssignment},

    {"tracking", "moving_peak_inheritance",  trackingMovingPeakInheritance},
    {"tracking", "one_to_one_matching",      trackingOneToOne},
    {"tracking", "spectral_flux_reset",      trackingFluxReset},
    {"tracking", "silence_rebirth",          trackingSilenceRebirth},
    {"tracking", "phase_wrapping",           trackingPhaseWrapped},

    {"render", "harmonics_pcm",              renderHarmonics},
    {"render", "glide_pcm",                  renderGlide},
    {"render", "pulses_pcm",                 renderPulses},
    {"render", "legacy_baseline",            renderLegacyBaseline},
    {"render", "chunk_continuity",           renderChunkContinuity},
    {"render", "switch_inside_hop",          renderSwitchAtHopInterior},
    {"render", "parameter_validation",       renderParameterValidation},
};
const size_t task_case_count{sizeof(task_cases) / sizeof(task_cases[0])};
