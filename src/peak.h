/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Streaming peak observer: records the largest normalised output magnitude
 * (integer samples plus 3 interpolated sub-sample phases) inside a sliding
 * window of completed frame units.  Self-contained and independent of the
 * resampling engines; a NULL (disabled) observer is accepted by every
 * function and costs no interpolation work. */

#if !defined soxr_peak_included
#define soxr_peak_included

#include "soxr.h"

#define SOXR_PEAK_WINDOW_MIN 1u
#define SOXR_PEAK_WINDOW_MAX 8192u
#define SOXR_PEAK_GUARD      16  /* A unit completes once its 16 future frames exist. */
#define SOXR_PEAK_TAPS       32  /* j = -15..16 per interpolated phase. */

typedef struct soxr_peak_observer soxr_peak_observer_t;

/* Allocate an observer for the given channel count.  Returns NULL on failure;
 * a NULL observer is a valid 'disabled' argument to every function below. */
soxr_peak_observer_t * _soxr_peak_create(unsigned num_channels);
void _soxr_peak_delete(soxr_peak_observer_t *);

/* (Re)allocate storage for the configured window and reset every counter;
 * O(num_channels x (window + 32)).  `output_gain' is the engine's output
 * full-scale factor (1 for float targets); it is divided out so integer
 * targets still report normalised magnitudes.  0 on allocation failure. */
int _soxr_peak_set_storage(soxr_peak_observer_t *, unsigned window,
    double output_gain);

/* Drop every buffered frame and all counters, retaining the window size and
 * the channel count. */
void _soxr_peak_clear(soxr_peak_observer_t *);

/* Store `n' frames of one channel's engine output; the values must be the
 * normalised output after io_spec.scale but before rounding, saturation and
 * dithering.  Every channel of a block is stored first, then the block is
 * published once with _soxr_peak_commit. */
void _soxr_peak_store_channel_f(soxr_peak_observer_t *, unsigned channel,
    float const * src, size_t n);
void _soxr_peak_store_channel_d(soxr_peak_observer_t *, unsigned channel,
    double const * src, size_t n);
void _soxr_peak_commit(soxr_peak_observer_t *, size_t n);

/* Signal the true end of the stream: complete the final guard units using
 * the implicit trailing zeros.  Repeated calls do not advance the state. */
void _soxr_peak_finish(soxr_peak_observer_t *);

/* Window length in output frames; 0 disables the observer. */
unsigned _soxr_peak_get_window(soxr_peak_observer_t const *);

/* Number of output frames delivered to the observer so far. */
uint64_t _soxr_peak_delivered(soxr_peak_observer_t const *);

/* Number of frame units whose four phases have been fully observed. */
uint64_t _soxr_peak_completed(soxr_peak_observer_t const *);

/* Fill the output arguments with the peak of channel `channel' (or every
 * channel merged when channel == num_channels) over the current window.
 * Returns 0 if the window contains no completed unit, zeroing the outputs. */
int _soxr_peak_query(soxr_peak_observer_t const *, unsigned channel,
    double * magnitude, unsigned * phase, uint64_t * frame,
    unsigned * winner_channel);

#endif
