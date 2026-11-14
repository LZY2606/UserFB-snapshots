#ifndef CORE_PHASE_LOCK_H
#define CORE_PHASE_LOCK_H

#include <complex>
#include <cstddef>


namespace al {

/* Number of bins produced by a 1024-point real FFT (DC through Nyquist). */
constexpr size_t PhaseLockBinCount{513};

/* One analysis bin: magnitude, wrapped phase in radians, and instantaneous
 * frequency expressed in FFT bins.
 */
struct PhaseLockBin {
    float magnitude;
    float phase;
    float frequency;
};


/* Phase-locked spectrum mapper for the 1024-sample, 128-sample-hop STFT used
 * by the pitch shifter. All storage is fixed-size; process() never allocates.
 */
struct PhaseLock1024 {
    static constexpr size_t BinCount{PhaseLockBinCount};
    static constexpr size_t InnerFirst{1};
    static constexpr size_t InnerLast{BinCount - 2};
    static constexpr size_t InnerCount{BinCount - 2};
    static constexpr size_t WindowSize{1024};
    static constexpr size_t HopSize{128};
    static constexpr int MaxPeaks{InnerCount};

    struct Peak {
        int bin;
        float frequency;
        float phase;
        double synthPhase;
    };

    Peak mPeaks[MaxPeaks];
    int mPeakCount{0};
    float mLastMagnitudes[InnerCount]{};
    float mLastMagnitudeSum{0.0f};
    bool mHasHistory{false};

    void reset() noexcept;

    /* Maps the analysis bins to a 513-bin complex synthesis spectrum. DC and
     * Nyquist are always zero. ratio is the quantized pitch ratio in [0.5,2],
     * threshold is the spectral-flux transient threshold (0 disables it).
     */
    void process(const PhaseLockBin *bins, float ratio, float threshold,
        std::complex<float> *output) noexcept;
};

} // namespace al

#endif /* CORE_PHASE_LOCK_H */
