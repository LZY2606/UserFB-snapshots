
#include "config.h"

#include "phase_lock.h"

#include <algorithm>
#include <cmath>

#include "alnumbers.h"


namespace al {

namespace {

constexpr double Pi{al::numbers::pi};
constexpr double Tau{Pi * 2.0};

/* Phase advance of one hop for a peak at FFT-bin frequency f with pitch ratio
 * r: 2*pi * hop/window * f * r. The persistent peak phase is kept in double so
 * long renderings do not accumulate single-precision phase drift.
 */
constexpr double HopPhaseFactor{Tau * (double(PhaseLock1024::HopSize)
    / double(PhaseLock1024::WindowSize))};

/* Relative peak floor, matching distance and instantaneous-frequency distance. */
constexpr float PeakFloor{1e-4f};
constexpr int MaxBinDistance{3};
constexpr float MaxFreqDistance{1.5f};
constexpr float FluxDenominatorEps{1e-12f};


inline double WrapPhase(double phase) noexcept
{
    /* Wrap into [-pi, pi). */
    phase = std::fmod(phase + Pi, Tau);
    if(phase < 0.0) phase += Tau;
    return phase - Pi;
}

} // namespace


void PhaseLock1024::reset() noexcept
{
    mPeakCount = 0;
    std::fill(std::begin(mLastMagnitudes), std::end(mLastMagnitudes), 0.0f);
    mLastMagnitudeSum = 0.0f;
    mHasHistory = false;
}


void PhaseLock1024::process(const PhaseLockBin *bins, float ratio, float threshold,
    std::complex<float> *output) noexcept
{
    /* DC and Nyquist are always synthesized as zero. */
    output[0] = std::complex<float>{};
    output[BinCount-1] = std::complex<float>{};
    for(size_t i{1};i < BinCount-1;++i)
        output[i] = std::complex<float>{};

    /* Magnitude statistics over the inner bins 1..511. */
    float maxMagnitude{0.0f};
    float magnitudeSum{0.0f};
    float positiveFlux{0.0f};
    for(size_t k{InnerFirst};k <= InnerLast;++k)
    {
        const float magnitude{bins[k].magnitude};
        maxMagnitude = std::max(maxMagnitude, magnitude);
        magnitudeSum += magnitude;
        if(mHasHistory)
        {
            const float delta{magnitude - mLastMagnitudes[k-1]};
            if(delta > 0.0f) positiveFlux += delta;
        }
    }

    /* The previous frame's magnitudes always get replaced, silence included. */
    for(size_t k{InnerFirst};k <= InnerLast;++k)
        mLastMagnitudes[k-1] = bins[k].magnitude;
    const float previousSum{mLastMagnitudeSum};
    mLastMagnitudeSum = magnitudeSum;
    mHasHistory = true;

    /* A silent frame produces no peaks and clears the peak trajectories. */
    if(!(maxMagnitude > 0.0f))
    {
        mPeakCount = 0;
        return;
    }

    /* Detect peaks: at or above the floor, strictly above the left neighbor,
     * and at or above the right neighbor.
     */
    const float floor{maxMagnitude * PeakFloor};
    Peak current[MaxPeaks];
    int peakCount{0};
    for(size_t k{InnerFirst};k <= InnerLast;++k)
    {
        const float magnitude{bins[k].magnitude};
        if(magnitude < floor) continue;
        if(!(magnitude > bins[k-1].magnitude)) continue;
        if(magnitude < bins[k+1].magnitude) continue;

        current[peakCount].bin = static_cast<int>(k);
        current[peakCount].frequency = bins[k].frequency;
        current[peakCount].phase = bins[k].phase;
        current[peakCount].synthPhase = 0.0;
        ++peakCount;
    }

    if(peakCount == 0)
    {
        mPeakCount = 0;
        return;
    }

    /* Spectral-flux transient detection, evaluated before mapping/synthesis
     * and sharing this frame's analysis data with peak matching.
     */
    bool transient{false};
    if(threshold > 0.0f)
    {
        const float fluxRatio{positiveFlux / (previousSum + FluxDenominatorEps)};
        if(fluxRatio > threshold)
            transient = true;
    }

    /* Match current peaks (in ascending peak number) against still-unused
     * previous-frame peaks: bin distance at most 3, instantaneous-frequency
     * distance at most 1.5 bins; smallest frequency distance wins, ties go to
     * the lower previous peak number.
     */
    bool usedOld[MaxPeaks]{};
    for(int i{0};i < peakCount;++i)
    {
        int best{-1};
        float bestFreqDistance{MaxFreqDistance};
        for(int j{0};j < mPeakCount;++j)
        {
            if(usedOld[j]) continue;
            const int binDistance{std::abs(current[i].bin - mPeaks[j].bin)};
            if(binDistance > MaxBinDistance) continue;
            const float freqDistance{std::abs(current[i].frequency - mPeaks[j].frequency)};
            if(!(freqDistance <= MaxFreqDistance)) continue;
            if(best < 0 || freqDistance < bestFreqDistance
                || (freqDistance == bestFreqDistance && j < best))
            {
                best = j;
                bestFreqDistance = freqDistance;
            }
        }
        if(best >= 0)
        {
            usedOld[best] = true;
            /* Continue the matched synthesis peak phase; transients and the
             * first voiced frame restart from the current input phase instead.
             */
            if(!transient)
            {
                const double advance{HopPhaseFactor * static_cast<double>(current[i].frequency)
                    * static_cast<double>(ratio)};
                current[i].synthPhase = WrapPhase(mPeaks[best].synthPhase + advance);
            }
            else
                current[i].synthPhase = WrapPhase(static_cast<double>(current[i].phase));
        }
        else
        {
            /* Newly born peaks start at the current input peak phase. */
            current[i].synthPhase = WrapPhase(static_cast<double>(current[i].phase));
        }
    }

    /* Assign every above-floor inner bin to its nearest peak, with ties going
     * to the lower bin number. Then synthesize each bin's complex contribution
     * from the peak's synthesis phase plus the bin's own phase offset, and
     * accumulate it onto floor(k*ratio+0.5). Bins below the floor contribute
     * nothing.
     */
    int cursor{0};
    for(size_t k{InnerFirst};k <= InnerLast;++k)
    {
        if(bins[k].magnitude < floor) continue;

        while(cursor < peakCount && current[cursor].bin < static_cast<int>(k))
            ++cursor;
        int chosen;
        if(cursor == 0)
            chosen = 0;
        else if(cursor == peakCount)
            chosen = peakCount - 1;
        else
        {
            const int rightDistance{current[cursor].bin - static_cast<int>(k)};
            const int leftDistance{static_cast<int>(k) - current[cursor-1].bin};
            chosen = (leftDistance <= rightDistance) ? cursor-1 : cursor;
        }

        const double phaseOffset{WrapPhase(static_cast<double>(bins[k].phase)
            - static_cast<double>(current[chosen].phase))};
        const double synthPhase{WrapPhase(current[chosen].synthPhase + phaseOffset)};
        const std::complex<float> contribution{std::polar(bins[k].magnitude,
            static_cast<float>(synthPhase))};

        const int target{static_cast<int>(std::floor(static_cast<double>(k)
            * static_cast<double>(ratio) + 0.5))};
        if(target >= static_cast<int>(InnerFirst) && target <= static_cast<int>(InnerLast))
            output[target] += contribution;
    }

    /* Peak numbers are per-frame identities: persist the measurements, not
     * the indices. */
    for(int i{0};i < peakCount;++i)
        mPeaks[i] = current[i];
    mPeakCount = peakCount;
}

} // namespace al
