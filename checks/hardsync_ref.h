#ifndef HARDSYNC_REF_H
#define HARDSYNC_REF_H

#include <cstddef>
#include <vector>

// Independent reference for the hard-sync sawtooth.  It shares no code
// with maxiSyncOsc and depends only on the C++ standard library.
namespace hardsync_ref {

// Number of output samples in the common master/slave period.  Returns
// 0 when no period aligned to the sample grid can be found.
size_t periodSamples(double sampleRate, double masterHz, double slaveHz);

// Band-limited periodic reference: the ideal continuous hard-sync
// waveform integrated over one period, keeping only harmonics strictly
// below Nyquist.  The true DC component is retained; no gain, phase or
// extra-delay fitting is performed.  out[i] is evaluated at time
// (skip + i) / sampleRate.
void bandlimited(double sampleRate, double masterHz, double slaveHz,
                 size_t skip, size_t count, double *out);

// The naive model that resets the slave only on output sample
// boundaries, with no band limiting.
void naiveHardReset(double sampleRate, double masterHz, double slaveHz,
                    size_t skip, size_t count, double *out);

// The ideal continuous waveform evaluated on a fine sub-sample grid and
// box-car averaged per output interval.  Used as a loose target for
// non-periodic (non-integer ratio) control signals.
void fineAveraged(double sampleRate, double masterHz, double slaveHz,
                  size_t skip, size_t count, int oversample, double *out);

} // namespace hardsync_ref

#endif // HARDSYNC_REF_H
