/* Self-contained true-peak regression test.
 *
 * It enters only through the public add_frames_short/int/float/double and
 * sample_peak/prev_sample_peak/true_peak/prev_true_peak APIs, and compares
 * them against the independent scalar implementation in
 * true_peak_reference.c.  No external audio files are used.
 *
 * Observed library semantics that this test documents:
 *  - add_frames() resets prev_* at the start of every call.  A call with
 *    frames == 0 therefore leaves prev_* at zero while the global peaks are
 *    unchanged.  No tail is flushed automatically; submit explicit zero
 *    frames if the ringing tail must be observed.
 *  - A single add_frames() call may be split internally at 400 ms/100 ms
 *    boundaries, but prev_* still describes that whole call; it is not reset
 *    between internal pieces.
 *  - Queries only read state: they neither advance the interpolator nor copy
 *    another channel's peak.
 */

#include "ebur128.h"
#include "true_peak_reference.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TP_TAIL_FRAMES 64
#define TP_LABEL_SIZE 256

/* The library stores the interpolation input and its output as float.  A
 * 49-tap convolution can accumulate roughly 49 float-rounding errors, so a
 * few ulps of headroom above the pure double reference is required.  On this
 * machine the observed worst difference is about 4e-8; 1e-6 absolute plus
 * 1e-6 relative leaves ample room for different libm/float paths while still
 * separating the deliberate degradations (whose smallest observed difference
 * is about 1e-2).  The observed worst difference is reported at the end. */
#define TP_TOL_ABS 1.0e-6
#define TP_TOL_REL 1.0e-6
#define TP_SAMPLE_TOL 1.0e-12

enum material {
  MATERIAL_HF_PHASE = 1,
  MATERIAL_NEG_IMPULSE = 2,
  MATERIAL_TWO_BURSTS = 3
};

static int g_failures = 0;
static int g_assertions = 0;
static int g_cases = 0;
static double g_max_true_peak_diff = 0.0;

static size_t type_size(tp_ref_type type) {
  switch (type) {
    case TP_REF_SHORT:
      return sizeof(short);
    case TP_REF_INT:
      return sizeof(int);
    case TP_REF_FLOAT:
      return sizeof(float);
    case TP_REF_DOUBLE:
      return sizeof(double);
  }
  return 0;
}

static const char* type_name(tp_ref_type type) {
  switch (type) {
    case TP_REF_SHORT:
      return "short";
    case TP_REF_INT:
      return "int";
    case TP_REF_FLOAT:
      return "float";
    case TP_REF_DOUBLE:
      return "double";
  }
  return "?";
}

static void check_close(const char* label, double got, double want,
                        double tolerance) {
  double diff;
  ++g_assertions;
  diff = fabs(got - want);
  if (diff > g_max_true_peak_diff && isfinite(got) && isfinite(want)) {
    g_max_true_peak_diff = diff;
  }
  if (!(diff <= tolerance)) {
    ++g_failures;
    fprintf(stderr,
            "ASSERT_FAIL %s: got=%.17g ref=%.17g diff=%.9g tol=%.9g\n", label,
            got, want, diff, tolerance);
  }
}

static void check_exact(const char* label, double got, double want) {
  double diff;
  ++g_assertions;
  diff = fabs(got - want);
  if (!(diff <= TP_SAMPLE_TOL)) {
    ++g_failures;
    fprintf(stderr, "ASSERT_FAIL %s: got=%.17g ref=%.17g diff=%.9g\n", label,
            got, want, diff);
  }
}

static void check_bool(const char* label, int condition) {
  ++g_assertions;
  if (!condition) {
    ++g_failures;
    fprintf(stderr, "ASSERT_FAIL %s: condition is false\n", label);
  }
}

static void check_rc(const char* label, int got, int want) {
  ++g_assertions;
  if (got != want) {
    ++g_failures;
    fprintf(stderr, "ASSERT_FAIL %s: rc=%d expected=%d\n", label, got, want);
  }
}

static double raised_cos(size_t n, size_t length) {
  if (length < 2) {
    return 1.0;
  }
  return 0.5 * (1.0 - cos(2.0 * M_PI * (double) n / (double) length));
}

/* Fills an interleaved ideal (double) signal. */
static void material_fill(int material, unsigned long samplerate,
                          unsigned int channels, size_t frames, double* x) {
  size_t n;
  size_t fade;
  size_t small_start;
  size_t small_len;
  unsigned int c;

  memset(x, 0, frames * channels * sizeof(double));

  if (material == MATERIAL_HF_PHASE) {
    /* Finite high-frequency burst at fs/4 with phase-staggered channels.
     * At phase pi/4 the samples sit at +/-0.707 of the continuous peak, so
     * the interpolated true peak is strictly larger than the sample peak. */
    fade = frames / 8;
    if (fade < 2) {
      fade = 2;
    }
    for (n = 0; n < frames; ++n) {
      double env = 1.0;
      double arg = 0.5 * M_PI * (double) n;
      if (n < fade) {
        env = raised_cos(n, fade);
      } else if (n + fade >= frames) {
        env = raised_cos(frames - 1 - n, fade);
      }
      x[n * channels + 0] = 0.90 * env * sin(arg + M_PI / 4.0);
      if (channels > 1) {
        x[n * channels + 1] = 0.60 * env * sin(arg + 3.0 * M_PI / 4.0);
      }
      for (c = 2; c < channels; ++c) {
        x[n * channels + c] = 0.0;
      }
    }
    return;
  }

  if (material == MATERIAL_NEG_IMPULSE) {
    /* Channel 0: one negative pulse.  Channel 1: a small independent signal
     * at a different position, so an accidental channel alias is visible. */
    size_t impulse = frames / 3;
    if (impulse >= frames) {
      impulse = frames - 1;
    }
    x[impulse * channels + 0] = -0.95;
    if (channels > 1) {
      small_start = (frames * 2) / 3;
      small_len = frames / 8;
      if (small_len < 2) {
        small_len = 2;
      }
      if (small_start + small_len > frames) {
        small_len = frames - small_start;
      }
      for (n = 0; n < small_len; ++n) {
        double env = raised_cos(n, small_len);
        double arg = 2.0 * M_PI * 3000.0 * (double) n / (double) samplerate;
        x[(small_start + n) * channels + 1] =
            0.12 * env * sin(arg + M_PI / 6.0);
      }
    }
    return;
  }

  if (material == MATERIAL_TWO_BURSTS) {
    /* Burst A, then a real silence gap, then a short burst B.  The second
     * add_frames() call starts two frames before burst B's strongest point,
     * so B has non-zero interpolation history at the call boundary on the
     * call-1 side and its strongest peak on the call-2 side. */
    size_t a_end = (frames * 28) / 100;
    size_t b_peak = (frames * 54) / 100;
    size_t b_start = (b_peak > 8) ? b_peak - 8 : 0;
    size_t b_end = b_peak + 200;
    size_t a_fade = (a_end / 8 > 2) ? a_end / 8 : 2;
    double sigma = 6.0;

    if (b_end > frames) {
      b_end = frames;
    }
    for (n = 0; n < a_end; ++n) {
      double env = 1.0;
      if (n < a_fade) {
        env = raised_cos(n, a_fade);
      } else if (n + a_fade >= a_end) {
        env = raised_cos(a_end - 1 - n, a_fade);
      }
      x[n * channels + 0] = 0.60 * env * sin(0.5 * M_PI * (double) n +
                                             M_PI / 4.0);
    }
    if (b_start < a_end) {
      b_start = a_end;
    }
    for (n = b_start; n < b_end; ++n) {
      double d = ((double) n - (double) b_peak) / sigma;
      double env = exp(-0.5 * d * d);
      x[n * channels + 0] =
          0.90 * env * sin(0.5 * M_PI * (double) n + M_PI / 4.0);
    }
    return;
  }
}

static size_t material_split(int material, size_t frames) {
  if (material == MATERIAL_TWO_BURSTS) {
    size_t b_peak = (frames * 54) / 100;
    return (b_peak > 2) ? b_peak - 2 : frames / 2;
  }
  return 0; /* a single call */
}

static void* make_typed(tp_ref_type type, const double* ideal, size_t values) {
  size_t i;
  size_t bytes = values * type_size(type);
  void* out = malloc(bytes ? bytes : 1);
  if (!out) {
    return NULL;
  }
  for (i = 0; i < values; ++i) {
    double v = ideal[i];
    switch (type) {
      case TP_REF_SHORT: {
        long q;
        if (v > 1.0) {
          v = 1.0;
        }
        if (v < -1.0) {
          v = -1.0;
        }
        q = lrint(v * 32768.0);
        if (q > 32767) {
          q = 32767;
        }
        if (q < -32768) {
          q = -32768;
        }
        ((short*) out)[i] = (short) q;
        break;
      }
      case TP_REF_INT: {
        double q;
        if (v > 1.0) {
          v = 1.0;
        }
        if (v < -1.0) {
          v = -1.0;
        }
        q = v * 2147483648.0;
        if (q > 2147483647.0) {
          q = 2147483647.0;
        }
        if (q < -2147483648.0) {
          q = -2147483648.0;
        }
        ((int*) out)[i] = (int) llrint(q);
        break;
      }
      case TP_REF_FLOAT:
        ((float*) out)[i] = (float) v;
        break;
      case TP_REF_DOUBLE:
        ((double*) out)[i] = v;
        break;
    }
  }
  return out;
}

static int lib_add_frames(ebur128_state* st, tp_ref_type type,
                          const void* src, size_t frames) {
  switch (type) {
    case TP_REF_SHORT:
      return ebur128_add_frames_short(st, (const short*) src, frames);
    case TP_REF_INT:
      return ebur128_add_frames_int(st, (const int*) src, frames);
    case TP_REF_FLOAT:
      return ebur128_add_frames_float(st, (const float*) src, frames);
    case TP_REF_DOUBLE:
      return ebur128_add_frames_double(st, (const double*) src, frames);
  }
  return -1;
}

static double tolerance_for(double reference) {
  return TP_TOL_ABS + TP_TOL_REL * fabs(reference);
}

/* Compares every public peak value right after one add_frames() call. */
static void compare_after_call(ebur128_state* st, tp_reference* ref,
                               unsigned int channels, const char* tag) {
  unsigned int c;
  for (c = 0; c < channels; ++c) {
    double lib_prev_sp = 0.0;
    double lib_prev_tp = 0.0;
    double lib_sp = 0.0;
    double lib_tp = 0.0;
    double ref_prev_sp = 0.0;
    double ref_prev_tp = 0.0;
    double ref_sp = 0.0;
    double ref_tp = 0.0;
    char label[TP_LABEL_SIZE];

    check_rc("prev_sample_peak_rc",
             ebur128_prev_sample_peak(st, c, &lib_prev_sp),
             EBUR128_SUCCESS);
    check_rc("prev_true_peak_rc",
             ebur128_prev_true_peak(st, c, &lib_prev_tp), EBUR128_SUCCESS);
    check_rc("sample_peak_rc", ebur128_sample_peak(st, c, &lib_sp),
             EBUR128_SUCCESS);
    check_rc("true_peak_rc", ebur128_true_peak(st, c, &lib_tp),
             EBUR128_SUCCESS);

    tp_reference_prev_sample_peak(ref, c, &ref_prev_sp);
    tp_reference_prev_true_peak(ref, c, &ref_prev_tp);
    tp_reference_sample_peak(ref, c, &ref_sp);
    tp_reference_true_peak(ref, c, &ref_tp);

    snprintf(label, sizeof(label), "%s_prev_sample_ch%u", tag, c);
    check_exact(label, lib_prev_sp, ref_prev_sp);
    snprintf(label, sizeof(label), "%s_prev_true_ch%u", tag, c);
    check_close(label, lib_prev_tp, ref_prev_tp,
                tolerance_for(ref_prev_tp));
    snprintf(label, sizeof(label), "%s_global_sample_ch%u", tag, c);
    check_exact(label, lib_sp, ref_sp);
    snprintf(label, sizeof(label), "%s_global_true_ch%u", tag, c);
    check_close(label, lib_tp, ref_tp, tolerance_for(ref_tp));
  }
}

static void run_case(const char* tag, int material, tp_ref_type type,
                     unsigned long samplerate, unsigned int channels,
                     size_t frames, int one_frame_at_a_time, int zero_tail,
                     int extra_queries) {
  size_t total_frames = frames + (zero_tail ? TP_TAIL_FRAMES : 0);
  size_t values = total_frames * channels;
  double* ideal;
  void* typed;
  ebur128_state* st;
  tp_reference* ref;
  size_t split;
  char call_tag[TP_LABEL_SIZE];

  ideal = (double*) calloc(values ? values : 1, sizeof(double));
  if (!ideal) {
    check_bool("allocation_ideal", 0);
    return;
  }
  material_fill(material, samplerate, channels, frames, ideal);
  typed = make_typed(type, ideal, values);
  if (!typed) {
    free(ideal);
    check_bool("allocation_typed", 0);
    return;
  }

  st = ebur128_init(channels, samplerate,
                    EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_M);
  ref = tp_reference_create(samplerate, channels);
  check_bool("library_state_created", st != NULL);
  check_bool("reference_created", ref != NULL);
  if (!st || !ref) {
    ebur128_destroy(&st);
    tp_reference_destroy(&ref);
    free(typed);
    free(ideal);
    return;
  }

  ++g_cases;
  split = material_split(material, frames);

  if (one_frame_at_a_time) {
    size_t n;
    for (n = 0; n < frames; ++n) {
      const char* p = (const char*) typed + n * channels * type_size(type);
      check_rc("one_frame_lib_add", lib_add_frames(st, type, p, 1),
               EBUR128_SUCCESS);
      check_rc("one_frame_ref_add",
               tp_reference_add_frames(ref, type, p, 1), 0);
      snprintf(call_tag, sizeof(call_tag), "%s_frame%zu", tag, n);
      compare_after_call(st, ref, channels, call_tag);
    }
  } else if (split > 0 && split < frames) {
    check_rc("call1_lib_add",
             lib_add_frames(st, type, typed, split), EBUR128_SUCCESS);
    check_rc("call1_ref_add", tp_reference_add_frames(ref, type, typed, split),
             0);
    if (extra_queries) {
      unsigned int c;
      double tmp;
      for (c = 0; c < channels; ++c) {
        int q;
        for (q = 0; q < 4; ++q) {
          ebur128_true_peak(st, c, &tmp);
          ebur128_prev_true_peak(st, c, &tmp);
          ebur128_sample_peak(st, c, &tmp);
          ebur128_prev_sample_peak(st, c, &tmp);
        }
      }
    }
    snprintf(call_tag, sizeof(call_tag), "%s_call1", tag);
    compare_after_call(st, ref, channels, call_tag);

    check_rc("call2_lib_add",
             lib_add_frames(st, type,
                            (const char*) typed + split * channels *
                                                   type_size(type),
                            frames - split),
             EBUR128_SUCCESS);
    check_rc("call2_ref_add",
             tp_reference_add_frames(ref, type,
                                     (const char*) typed + split * channels *
                                                            type_size(type),
                                     frames - split),
             0);
    snprintf(call_tag, sizeof(call_tag), "%s_call2", tag);
    compare_after_call(st, ref, channels, call_tag);
    if (zero_tail) {
      const char* p = (const char*) typed +
                      frames * channels * type_size(type);
      check_rc("tail_lib_add",
               lib_add_frames(st, type, p, TP_TAIL_FRAMES), EBUR128_SUCCESS);
      check_rc("tail_ref_add",
               tp_reference_add_frames(ref, type, p, TP_TAIL_FRAMES), 0);
      snprintf(call_tag, sizeof(call_tag), "%s_tail", tag);
      compare_after_call(st, ref, channels, call_tag);
    }
  } else {
    check_rc("whole_lib_add", lib_add_frames(st, type, typed, frames),
             EBUR128_SUCCESS);
    check_rc("whole_ref_add",
             tp_reference_add_frames(ref, type, typed, frames), 0);
    snprintf(call_tag, sizeof(call_tag), "%s_call1", tag);
    compare_after_call(st, ref, channels, call_tag);
    if (zero_tail) {
      const char* p = (const char*) typed +
                      frames * channels * type_size(type);
      check_rc("tail_lib_add",
               lib_add_frames(st, type, p, TP_TAIL_FRAMES), EBUR128_SUCCESS);
      check_rc("tail_ref_add",
               tp_reference_add_frames(ref, type, p, TP_TAIL_FRAMES), 0);
      snprintf(call_tag, sizeof(call_tag), "%s_tail", tag);
      compare_after_call(st, ref, channels, call_tag);
    }
  }

  /* Material-specific properties: these make the three degradations
   * independently observable, not merely indirectly different. */
  if (!one_frame_at_a_time && !zero_tail && material == MATERIAL_HF_PHASE &&
      samplerate < 192000) {
    double prev_sp = 0.0;
    double prev_tp = 0.0;
    double sp = 0.0;
    double tp = 0.0;
    char label[TP_LABEL_SIZE];
    ebur128_prev_sample_peak(st, 0, &prev_sp);
    ebur128_prev_true_peak(st, 0, &prev_tp);
    ebur128_sample_peak(st, 0, &sp);
    ebur128_true_peak(st, 0, &tp);
    snprintf(label, sizeof(label), "%s_cat1_prev_true_exceeds_sample", tag);
    check_bool(label, prev_tp > prev_sp + 1.0e-3);
    snprintf(label, sizeof(label), "%s_cat1_true_exceeds_sample", tag);
    check_bool(label, tp > sp + 1.0e-3);
  }

  if (!one_frame_at_a_time && !zero_tail &&
      material == MATERIAL_NEG_IMPULSE && channels > 1) {
    double ch0 = 0.0;
    double ch1 = 0.0;
    double g0 = 0.0;
    double g1 = 0.0;
    char label[TP_LABEL_SIZE];
    ebur128_prev_true_peak(st, 0, &ch0);
    ebur128_prev_true_peak(st, 1, &ch1);
    ebur128_true_peak(st, 0, &g0);
    ebur128_true_peak(st, 1, &g1);
    snprintf(label, sizeof(label), "%s_cat2_channel_isolation_prev", tag);
    check_bool(label, ch0 > 0.80 && ch1 < 0.50 * ch0);
    snprintf(label, sizeof(label), "%s_cat2_channel_isolation_global", tag);
    check_bool(label, g0 > 0.80 && g1 < 0.50 * g0);
  }

  ebur128_destroy(&st);
  tp_reference_destroy(&ref);
  free(typed);
  free(ideal);
}

/* Zero-frame calls: prev_* become zero, global peaks are untouched. */
static void run_zero_frame_check(unsigned long samplerate, unsigned int channels,
                                 tp_ref_type type) {
  size_t frames = 300;
  size_t values = frames * channels;
  double* ideal = (double*) calloc(values, sizeof(double));
  void* typed;
  ebur128_state* st;
  double before_tp = 0.0;
  double before_sp = 0.0;
  double after_tp = 0.0;
  double after_sp = 0.0;
  double prev_tp = 1.0;
  double prev_sp = 1.0;
  unsigned int c;

  if (!ideal) {
    check_bool("zero_frame_allocation", 0);
    return;
  }
  material_fill(MATERIAL_HF_PHASE, samplerate, channels, frames, ideal);
  typed = make_typed(type, ideal, values);
  st = ebur128_init(channels, samplerate,
                    EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_M);
  check_bool("zero_frame_state_created", st != NULL && typed != NULL);
  if (!st || !typed) {
    ebur128_destroy(&st);
    free(typed);
    free(ideal);
    return;
  }

  ++g_cases;
  check_rc("zero_frame_initial_add", lib_add_frames(st, type, typed, frames),
           EBUR128_SUCCESS);

  for (c = 0; c < channels; ++c) {
    char label[TP_LABEL_SIZE];
    ebur128_true_peak(st, c, &before_tp);
    ebur128_sample_peak(st, c, &before_sp);
    check_rc("zero_frame_add_rc",
             lib_add_frames(st, type, typed, 0), EBUR128_SUCCESS);
    ebur128_prev_true_peak(st, c, &prev_tp);
    ebur128_prev_sample_peak(st, c, &prev_sp);
    ebur128_true_peak(st, c, &after_tp);
    ebur128_sample_peak(st, c, &after_sp);

    snprintf(label, sizeof(label), "zero_frame_prev_true_reset_ch%u", c);
    check_exact(label, prev_tp, 0.0);
    snprintf(label, sizeof(label), "zero_frame_prev_sample_reset_ch%u", c);
    check_exact(label, prev_sp, 0.0);
    snprintf(label, sizeof(label), "zero_frame_global_true_stable_ch%u", c);
    check_exact(label, after_tp, before_tp);
    snprintf(label, sizeof(label), "zero_frame_global_sample_stable_ch%u", c);
    check_exact(label, after_sp, before_sp);
  }

  ebur128_destroy(&st);
  free(typed);
  free(ideal);
}

/* Invalid mode and out-of-range channel must fail without writing *out. */
static void run_error_checks(void) {
  ebur128_state* st;
  double sentinel = 12345.0;
  double tmp = 0.0;

  ++g_cases;

  st = ebur128_init(1, 48000, EBUR128_MODE_M);
  check_bool("error_mode_state_created", st != NULL);
  if (st) {
    check_rc("error_mode_add", ebur128_add_frames_double(st, &tmp, 1),
             EBUR128_SUCCESS);
    sentinel = 12345.0;
    check_rc("error_mode_true_peak_rc",
             ebur128_true_peak(st, 0, &sentinel),
             EBUR128_ERROR_INVALID_MODE);
    check_exact("error_mode_true_peak_out_untouched", sentinel, 12345.0);
    sentinel = 12345.0;
    check_rc("error_mode_prev_true_peak_rc",
             ebur128_prev_true_peak(st, 0, &sentinel),
             EBUR128_ERROR_INVALID_MODE);
    check_exact("error_mode_prev_true_peak_out_untouched", sentinel, 12345.0);
    ebur128_destroy(&st);
  }

  st = ebur128_init(1, 48000, EBUR128_MODE_SAMPLE_PEAK);
  check_bool("sample_only_mode_state_created", st != NULL);
  if (st) {
    check_rc("sample_only_add", ebur128_add_frames_double(st, &tmp, 1),
             EBUR128_SUCCESS);
    sentinel = 12345.0;
    check_rc("sample_only_true_peak_rc", ebur128_true_peak(st, 0, &sentinel),
             EBUR128_ERROR_INVALID_MODE);
    check_exact("sample_only_true_peak_out_untouched", sentinel, 12345.0);
    check_rc("sample_only_sample_peak_rc",
             ebur128_sample_peak(st, 0, &tmp), EBUR128_SUCCESS);
    ebur128_destroy(&st);
  }

  st = ebur128_init(2, 48000, EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_M);
  check_bool("range_state_created", st != NULL);
  if (st) {
    check_rc("range_add", ebur128_add_frames_double(st, &tmp, 1),
             EBUR128_SUCCESS);
    sentinel = 12345.0;
    check_rc("range_true_peak_rc", ebur128_true_peak(st, 2, &sentinel),
             EBUR128_ERROR_INVALID_CHANNEL_INDEX);
    check_exact("range_true_peak_out_untouched", sentinel, 12345.0);
    sentinel = 12345.0;
    check_rc("range_prev_true_peak_rc", ebur128_prev_true_peak(st, 2, &sentinel),
             EBUR128_ERROR_INVALID_CHANNEL_INDEX);
    check_exact("range_prev_true_peak_out_untouched", sentinel, 12345.0);
    sentinel = 12345.0;
    check_rc("range_sample_peak_rc", ebur128_sample_peak(st, 2, &sentinel),
             EBUR128_ERROR_INVALID_CHANNEL_INDEX);
    check_exact("range_sample_peak_out_untouched", sentinel, 12345.0);
    sentinel = 12345.0;
    check_rc("range_huge_true_peak_rc",
             ebur128_true_peak(st, (unsigned int) -1, &sentinel),
             EBUR128_ERROR_INVALID_CHANNEL_INDEX);
    check_exact("range_huge_true_peak_out_untouched", sentinel, 12345.0);
    ebur128_destroy(&st);
  }
}

static void run_rate_and_type_matrix(void) {
  static const unsigned long rates[4] = {44100, 48000, 96000, 192000};
  static const tp_ref_type types[4] = {TP_REF_SHORT, TP_REF_INT,
                                       TP_REF_FLOAT, TP_REF_DOUBLE};
  static const int materials[3] = {MATERIAL_HF_PHASE, MATERIAL_NEG_IMPULSE,
                                   MATERIAL_TWO_BURSTS};
  int r;
  int t;
  int m;
  tp_ref_type type;

  /* Every material and every public entry at an interleaved stereo rate of
   * 44.1, 48, 96 and 192 kHz.  192 kHz exercises the library's sample-peak
   * true-peak path (no interpolation). */
  for (r = 0; r < 4; ++r) {
    for (t = 0; t < 4; ++t) {
      type = types[t];
      for (m = 0; m < 3; ++m) {
        size_t frames;
        char tag[TP_LABEL_SIZE];
        if (materials[m] == MATERIAL_HF_PHASE) {
          frames = (size_t) (rates[r] * 200 / 1000); /* 200 ms */
        } else if (materials[m] == MATERIAL_NEG_IMPULSE) {
          frames = (size_t) (rates[r] * 100 / 1000); /* 100 ms */
        } else {
          frames = (size_t) (rates[r] * 600 / 1000); /* 600 ms */
        }
        if (materials[m] == MATERIAL_TWO_BURSTS) {
          snprintf(tag, sizeof(tag), "cat3_two_calls_%lu_%s", rates[r],
                   type_name(type));
        } else {
          snprintf(tag, sizeof(tag), "%s_stereo_%lu_%s",
                   materials[m] == MATERIAL_HF_PHASE ? "cat1" : "cat2",
                   rates[r], type_name(type));
        }
        run_case(tag, materials[m], type, rates[r], 2, frames, 0, 0,
                 materials[m] == MATERIAL_TWO_BURSTS);
      }
    }
  }

  /* Mono coverage at 48 kHz for all materials and all public entries. */
  for (t = 0; t < 4; ++t) {
    char tag[TP_LABEL_SIZE];
    size_t cat1_frames = 48000 * 200 / 1000;
    size_t cat2_frames = 48000 * 100 / 1000;
    size_t cat3_frames = 48000 * 600 / 1000;
    type = types[t];
    snprintf(tag, sizeof(tag), "cat1_mono_48k_%s", type_name(type));
    run_case(tag, MATERIAL_HF_PHASE, type, 48000, 1, cat1_frames, 0, 0, 0);
    snprintf(tag, sizeof(tag), "cat2_mono_48k_%s", type_name(type));
    run_case(tag, MATERIAL_NEG_IMPULSE, type, 48000, 1, cat2_frames, 0, 0, 0);
    snprintf(tag, sizeof(tag), "cat3_mono_48k_%s", type_name(type));
    run_case(tag, MATERIAL_TWO_BURSTS, type, 48000, 1, cat3_frames, 0, 0, 0);
  }
}

static void run_stereo_special_cases(void) {
  static const tp_ref_type types[4] = {TP_REF_SHORT, TP_REF_INT,
                                       TP_REF_FLOAT, TP_REF_DOUBLE};
  int t;
  tp_ref_type type;
  size_t cat1_frames = 48000 * 200 / 1000;
  size_t cat3_frames = 48000 * 600 / 1000;

  for (t = 0; t < 4; ++t) {
    char tag[TP_LABEL_SIZE];
    type = types[t];

    /* One big call crosses internal 400 ms/100 ms boundaries; prev_* must
     * still describe the call as one call. */
    snprintf(tag, sizeof(tag), "cat3_single_call_internal_split_%s",
             type_name(type));
    run_case(tag, MATERIAL_TWO_BURSTS, type, 48000, 2, cat3_frames, 0, 0, 0);

    /* Explicit tail zeros, not an implied API flush. */
    snprintf(tag, sizeof(tag), "cat1_tail_zeros_%s", type_name(type));
    run_case(tag, MATERIAL_HF_PHASE, type, 48000, 2, cat1_frames, 0, 1, 0);
  }
}

static void run_one_frame_matrix(void) {
  static const tp_ref_type types[4] = {TP_REF_SHORT, TP_REF_INT,
                                       TP_REF_FLOAT, TP_REF_DOUBLE};
  int t;
  tp_ref_type type;
  for (t = 0; t < 4; ++t) {
    char tag[TP_LABEL_SIZE];
    type = types[t];
    snprintf(tag, sizeof(tag), "cat1_one_frame_calls_%s", type_name(type));
    run_case(tag, MATERIAL_HF_PHASE, type, 48000, 2, 360, 1, 0, 0);
  }
}

static void run_zero_frame_matrix(void) {
  static const tp_ref_type types[4] = {TP_REF_SHORT, TP_REF_INT,
                                       TP_REF_FLOAT, TP_REF_DOUBLE};
  int t;
  for (t = 0; t < 4; ++t) {
    run_zero_frame_check(48000, 2, types[t]);
  }
}

int main(void) {
  run_rate_and_type_matrix();
  run_stereo_special_cases();
  run_one_frame_matrix();
  run_zero_frame_matrix();
  run_error_checks();

  printf("CASES_RUN=%d ASSERTIONS_RUN=%d FAILURES=%d MAX_TRUE_PEAK_DIFF=%.9g\n",
         g_cases, g_assertions, g_failures, g_max_true_peak_diff);
  if (g_failures) {
    return 1;
  }
  return 0;
}
