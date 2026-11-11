#ifndef MAXI_SYNCOSC_H
#define MAXI_SYNCOSC_H

#include <cstddef>

/**
 * Native hard-sync sawtooth oscillator.
 *
 * A master and a slave phase both start at zero and advance with the
 * per-sample frequencies supplied to play()/process().  The unfiltered
 * slave waveform is 2*phase-1.  Whenever the master phase wraps past 1
 * the slave phase is reset to 0 at the true crossing time inside the
 * sample interval, and keeps advancing for the remainder of the
 * interval.  Every discontinuity (master reset or the slave's own
 * wrap) is band-limited with a PolyBLEP correction; a reset that lands
 * on the slave's natural wrap is emitted as a single jump.
 *
 * The output is delayed by a fixed integer number of samples reported
 * by latency() (the tail of the band-limited step corrections); the
 * delay never changes with frequency.
 */
class maxiSyncOsc {
public:
    /*! Constructs the oscillator using maxiSettings::sampleRate. */
    maxiSyncOsc();

    /*!
     * Updates the sample rate and resets every phase and all filter
     * history.  Throws std::invalid_argument for non-finite or
     * non-positive values, leaving the object untouched.
     */
    void setSampleRate(double sampleRate);

    /*! Resets both phases and all band-limited jump history. */
    void reset();

    /*!
     * Produces one output sample.  Frequencies are held constant for
     * this sample interval.  An invalid control pair (non-finite or
     * outside [0, 0.45*sampleRate]) outputs zero and does not advance
     * the state.
     */
    double play(double masterHz, double slaveHz);

    /*!
     * Block form, sample-for-sample identical to repeated play()
     * calls.  count == 0 touches none of the pointers.
     */
    void process(const double *masterHz, const double *slaveHz,
                 double *out, size_t count);

    /*! Fixed output latency in samples (does not vary with frequency). */
    size_t latency() const;

private:
    double sampleRate;
    double masterPhase;
    double slavePhase;
    // Ring buffer of band-limited step (PolyBLEP) corrections.
    static const size_t BLEP_TAPS = 2;
    double blepBuffer[BLEP_TAPS];
    size_t blepPos;

    void clearHistory();
    void addJump(double offsetFromNow, double magnitude);
    double step(double masterHz, double slaveHz);
};

#endif // MAXI_SYNCOSC_H
