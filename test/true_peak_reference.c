/* Independent scalar reference implementation of the true-peak chain.
 *
 * The prototype is a 49-tap Hann-windowed sinc.  Instead of the library's
 * polyphase table, this file explicitly inserts factor-1 zeros between input
 * samples and convolves the resulting high-rate stream with the prototype in
 * the time domain.  The interpolation history starts as zeros, contains only
 * samples actually submitted through tp_reference_add_frames(), and never
 * pretends that a call has flushed its tail: callers must submit explicit
 * zero frames to observe the tail.  Only the delay line used by the
 * convolution is kept; the library's coefficient table, ring layout and
 * internal functions are not used.
 *
 * All arithmetic here is double.  The library stores interpolation input and
 * output as float, so the regression tolerances include that rounding.
 */

#include "true_peak_reference.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TP_REF_TAPS 49

struct tp_reference {
  unsigned long samplerate;
  unsigned int channels;
  unsigned int factor; /* 4, 2, or 0 (no interpolation) */
  unsigned int taps;
  double* coeff; /* prototype, taps entries */
  /* Explicitly zero-stuffed high-rate delay line, one per channel. */
  double* ring; /* channels * taps */
  unsigned int ri; /* high-rate ring write index */
  double* frame; /* scratch: one input frame, per channel */
  double* prev_interp;
  double* prev_sample;
  double* global_interp;
  double* global_sample;
};

static double tp_ref_value(tp_ref_type type, const void* src, size_t index) {
  switch (type) {
    case TP_REF_SHORT:
      return (double) ((const short*) src)[index] / 32768.0;
    case TP_REF_INT:
      return (double) ((const int*) src)[index] / 2147483648.0;
    case TP_REF_FLOAT:
      return (double) ((const float*) src)[index];
    case TP_REF_DOUBLE:
      return ((const double*) src)[index];
    default:
      return 0.0;
  }
}

tp_reference* tp_reference_create(unsigned long samplerate,
                                  unsigned int channels) {
  tp_reference* ref;
  unsigned int j;

  if (channels == 0) {
    return NULL;
  }
  ref = (tp_reference*) calloc(1, sizeof(*ref));
  if (!ref) {
    return NULL;
  }
  ref->samplerate = samplerate;
  ref->channels = channels;
  if (samplerate < 96000) {
    ref->factor = 4;
  } else if (samplerate < 192000) {
    ref->factor = 2;
  } else {
    ref->factor = 0;
  }
  ref->taps = TP_REF_TAPS;

  ref->coeff = (double*) calloc(ref->taps, sizeof(double));
  ref->ring = (double*) calloc((size_t) channels * ref->taps, sizeof(double));
  ref->frame = (double*) calloc(channels, sizeof(double));
  ref->prev_interp = (double*) calloc(channels, sizeof(double));
  ref->prev_sample = (double*) calloc(channels, sizeof(double));
  ref->global_interp = (double*) calloc(channels, sizeof(double));
  ref->global_sample = (double*) calloc(channels, sizeof(double));
  if (!ref->coeff || !ref->ring || !ref->frame || !ref->prev_interp ||
      !ref->prev_sample || !ref->global_interp || !ref->global_sample) {
    tp_reference_destroy(&ref);
    return NULL;
  }

  /* Hann-windowed sinc, 49 taps, computed independently of the library. */
  for (j = 0; j < ref->taps; ++j) {
    double m = (double) j - (double) (ref->taps - 1) / 2.0;
    double c;
    if (ref->factor == 0) {
      c = (j == (ref->taps - 1) / 2) ? 1.0 : 0.0;
    } else if (fabs(m) > 1e-6) {
      c = sin(m * M_PI / (double) ref->factor) /
          (m * M_PI / (double) ref->factor);
    } else {
      c = 1.0;
    }
    c *= 0.5 * (1.0 - cos(2.0 * M_PI * (double) j /
                          (double) (ref->taps - 1)));
    ref->coeff[j] = c;
  }

  return ref;
}

void tp_reference_destroy(tp_reference** ref) {
  if (!ref || !*ref) {
    return;
  }
  free((*ref)->coeff);
  free((*ref)->ring);
  free((*ref)->frame);
  free((*ref)->prev_interp);
  free((*ref)->prev_sample);
  free((*ref)->global_interp);
  free((*ref)->global_sample);
  free(*ref);
  *ref = NULL;
}

int tp_reference_add_frames(tp_reference* ref, tp_ref_type type,
                            const void* src, size_t frames) {
  unsigned int c, f;
  size_t n;
  unsigned int taps;

  if (!ref) {
    return -1;
  }

  /* A call resets the per-call statistics even when it contains no frames;
   * the accumulated global values therefore stay unchanged. */
  for (c = 0; c < ref->channels; ++c) {
    ref->prev_interp[c] = 0.0;
    ref->prev_sample[c] = 0.0;
  }
  if (frames == 0) {
    return 0;
  }

  taps = ref->taps;
  for (n = 0; n < frames; ++n) {
    /* Raw samples first; these are the samples the library also sees. */
    for (c = 0; c < ref->channels; ++c) {
      double x = tp_ref_value(type, src, n * ref->channels + c);
      ref->frame[c] = x;
      if (fabs(x) > ref->prev_sample[c]) {
        ref->prev_sample[c] = fabs(x);
      }
    }

    if (ref->factor == 0) {
      /* 192 kHz and above: the library uses the sample-peak path. */
      continue;
    }

    /* Explicit zero insertion: emit x, 0, 0, ... then convolve each emitted
     * high-rate sample with the whole prototype.  samples outside this call
     * are either previously submitted history or the initial zeros. */
    for (f = 0; f < ref->factor; ++f) {
      for (c = 0; c < ref->channels; ++c) {
        double acc = 0.0;
        unsigned int idx;
        unsigned int j;
        double u = (f == 0) ? ref->frame[c] : 0.0;
        double* ring = ref->ring + (size_t) c * taps;

        ring[ref->ri] = u;
        idx = ref->ri;
        for (j = 0; j < taps; ++j) {
          acc += ref->coeff[j] * ring[idx];
          if (idx == 0) {
            idx = taps - 1;
          } else {
            --idx;
          }
        }
        if (fabs(acc) > ref->prev_interp[c]) {
          ref->prev_interp[c] = fabs(acc);
        }
      }
      ref->ri += 1;
      if (ref->ri == taps) {
        ref->ri = 0;
      }
    }
  }

  for (c = 0; c < ref->channels; ++c) {
    double prev_true = ref->prev_interp[c];
    if (ref->prev_sample[c] > prev_true) {
      prev_true = ref->prev_sample[c];
    }
    if (prev_true > ref->global_interp[c]) {
      ref->global_interp[c] = prev_true;
    }
    if (ref->prev_sample[c] > ref->global_sample[c]) {
      ref->global_sample[c] = ref->prev_sample[c];
    }
  }

  return 0;
}

static void tp_ref_get(const double* values, unsigned int channels,
                       unsigned int channel, double* out) {
  if (!out) {
    return;
  }
  *out = (channel < channels) ? values[channel] : 0.0;
}

void tp_reference_prev_true_peak(const tp_reference* ref, unsigned int channel,
                                 double* out) {
  if (!ref || channel >= ref->channels) {
    return;
  }
  *out = ref->prev_interp[channel];
  if (ref->prev_sample[channel] > *out) {
    *out = ref->prev_sample[channel];
  }
}

void tp_reference_prev_sample_peak(const tp_reference* ref,
                                   unsigned int channel, double* out) {
  if (!ref) {
    return;
  }
  tp_ref_get(ref->prev_sample, ref->channels, channel, out);
}

void tp_reference_true_peak(const tp_reference* ref, unsigned int channel,
                            double* out) {
  if (!ref || channel >= ref->channels) {
    return;
  }
  *out = ref->global_interp[channel];
  if (ref->global_sample[channel] > *out) {
    *out = ref->global_sample[channel];
  }
}

void tp_reference_sample_peak(const tp_reference* ref, unsigned int channel,
                              double* out) {
  if (!ref) {
    return;
  }
  tp_ref_get(ref->global_sample, ref->channels, channel, out);
}
