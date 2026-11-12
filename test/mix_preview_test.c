/* See COPYING file for copyright and license details. */

/* Tests for EBUR128_MODE_MIX_PREVIEW / ebur128_loudness_mix().
 *
 * Four cases: covariance, gating, history, compatibility.
 * Each case prints "MIX_PREVIEW_CASE <name> PASS" when all its assertions
 * hold.
 *
 * Independent references are computed by explicitly mixing the PCM with the
 * matrix and feeding the result into a plain EBUR128_MODE_I instance; the
 * new query API is never used to generate expected values.
 *
 * Numerical tolerances:
 *   MIX_TOL    1e-6 LU  - preview query vs. explicitly mixed reference.
 *                         Both paths run the same K-weighting filter and the
 *                         same gating; only the summation order differs.
 *   EXACT_TOL  1e-12 LU - paths that must be bit-identical (same input,
 *                         same code path).
 */

#include "ebur128.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000UL
#define MIX_TOL 1e-6
#define EXACT_TOL 1e-12

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

/* Reference: mix pcm (interleaved, channels) with matrix (row-major, rows x
 * channels), scale each output row r by sqrt(weights[r]) so that the energy
 * of row r is weighted by weights[r], and measure the integrated loudness
 * with a plain EBUR128_MODE_I instance. */
static double ref_mix_loudness(const double* pcm,
                               size_t frames,
                               unsigned int channels,
                               const double* matrix,
                               size_t rows,
                               const double* weights) {
  double* mixed = (double*) malloc(frames * rows * sizeof(double));
  ebur128_state* st;
  double out = 0.0;
  size_t i, r;
  unsigned int c;

  CHECK(mixed != NULL);
  for (i = 0; i < frames; ++i) {
    for (r = 0; r < rows; ++r) {
      double acc = 0.0;
      for (c = 0; c < channels; ++c) {
        acc += matrix[r * channels + c] * pcm[i * channels + c];
      }
      mixed[i * rows + r] = sqrt(weights[r]) * acc;
    }
  }
  st = ebur128_init((unsigned int) rows, SR, EBUR128_MODE_I);
  CHECK(st != NULL);
  for (r = 0; r < rows; ++r) {
    CHECK(ebur128_set_channel(st, (unsigned int) r, EBUR128_LEFT) ==
          EBUR128_SUCCESS);
  }
  CHECK(ebur128_add_frames_double(st, mixed, frames) == EBUR128_SUCCESS);
  CHECK(ebur128_loudness_global(st, &out) == EBUR128_SUCCESS);
  ebur128_destroy(&st);
  free(mixed);
  return out;
}

static double query_mix(ebur128_state* st,
                        const double* matrix,
                        size_t rows,
                        const double* weights) {
  double out = 0.0;
  CHECK(ebur128_loudness_mix(st, matrix, rows, weights, &out) ==
        EBUR128_SUCCESS);
  return out;
}

/* deterministic pseudo-random noise in [-1, 1) */
static double noise(unsigned long* state) {
  *state = *state * 6364136223846793005UL + 1442695040888963407UL;
  return (double) (*state >> 11) / (double) (1UL << 52) - 1.0;
}

static void case_covariance(void) {
  const size_t frames = 2 * SR; /* 2 s -> 17 gating blocks */
  double* pcm = (double*) malloc(frames * 3 * sizeof(double));
  size_t i;
  double m_mono_sum[2] = { 1.0, 1.0 };
  double m_left[2] = { 1.0, 0.0 };
  double m_ident2[4] = { 1.0, 0.0, 0.0, 1.0 };
  double w1[1] = { 1.0 };
  double w11[2] = { 1.0, 1.0 };
  ebur128_state* st;
  double out, ref, global;

  CHECK(pcm != NULL);

  /* --- anti-phase stereo: L = s, R = -s --- */
  for (i = 0; i < frames; ++i) {
    double s = 0.5 * sin(2.0 * M_PI * 997.0 * (double) i / (double) SR);
    pcm[2 * i] = s;
    pcm[2 * i + 1] = -s;
  }
  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  CHECK(st != NULL);
  CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);

  /* single channel query matches the explicit reference */
  out = query_mix(st, m_left, 1, w1);
  ref = ref_mix_loudness(pcm, frames, 2, m_left, 1, w1);
  CHECK(isfinite(out) && fabs(out - ref) < MIX_TOL);

  /* identity matrix matches the classic integrated loudness */
  out = query_mix(st, m_ident2, 2, w11);
  CHECK(ebur128_loudness_global(st, &global) == EBUR128_SUCCESS);
  CHECK(isfinite(out) && fabs(out - global) < MIX_TOL);
  ref = ref_mix_loudness(pcm, frames, 2, m_ident2, 2, w11);
  CHECK(fabs(out - ref) < MIX_TOL);

  /* mono sum of an anti-phase pair cancels exactly: no block passes the
   * absolute gate. An implementation that only keeps per-channel diagonal
   * energies would report a finite value (~3 LU above the single channel)
   * here and is caught by this check. */
  out = query_mix(st, m_mono_sum, 1, w1);
  CHECK(out == -HUGE_VAL);
  ref = ref_mix_loudness(pcm, frames, 2, m_mono_sum, 1, w1);
  CHECK(ref == -HUGE_VAL);
  ebur128_destroy(&st);

  /* --- in-phase stereo: mono sum is exactly +6.02 LU over one channel --- */
  for (i = 0; i < frames; ++i) {
    double s = 0.5 * sin(2.0 * M_PI * 997.0 * (double) i / (double) SR);
    pcm[2 * i] = s;
    pcm[2 * i + 1] = s;
  }
  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  CHECK(st != NULL);
  CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);
  {
    double sum = query_mix(st, m_mono_sum, 1, w1);
    double one = query_mix(st, m_left, 1, w1);
    CHECK(isfinite(sum) && isfinite(one));
    CHECK(fabs((sum - one) - 10.0 * log10(4.0)) < MIX_TOL);
    ref = ref_mix_loudness(pcm, frames, 2, m_mono_sum, 1, w1);
    CHECK(fabs(sum - ref) < MIX_TOL);
  }
  ebur128_destroy(&st);

  /* --- three-channel cross mix with negative coefficients --- */
  {
    unsigned long rng = 12345;
    double m_cross[6] = { 0.5, -0.25, 0.75, 1.0, 1.0, -1.0 };
    double w_cross[2] = { 1.0, 1.41 };
    double w_cross2[2] = { 2.0, 0.0 };
    for (i = 0; i < frames; ++i) {
      pcm[3 * i] = 0.4 * sin(2.0 * M_PI * 440.0 * (double) i / (double) SR);
      pcm[3 * i + 1] =
          0.3 * sin(2.0 * M_PI * 1330.0 * (double) i / (double) SR + 1.0);
      pcm[3 * i + 2] = 0.2 * noise(&rng);
    }
    st = ebur128_init(3, SR, EBUR128_MODE_MIX_PREVIEW);
    CHECK(st != NULL);
    CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);
    out = query_mix(st, m_cross, 2, w_cross);
    ref = ref_mix_loudness(pcm, frames, 3, m_cross, 2, w_cross);
    CHECK(isfinite(out) && fabs(out - ref) < MIX_TOL);
    /* weight 2.0 on one output, 0.0 on the other */
    out = query_mix(st, m_cross, 2, w_cross2);
    ref = ref_mix_loudness(pcm, frames, 3, m_cross, 2, w_cross2);
    CHECK(isfinite(out) && fabs(out - ref) < MIX_TOL);
    ebur128_destroy(&st);
  }

  /* --- input mapped to EBUR128_UNUSED is still available for mixing --- */
  {
    double m_third[3] = { 0.0, 0.0, 1.0 };
    double m_ident3[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    double w111[3] = { 1.0, 1.0, 1.0 };
    unsigned long rng = 6789;
    for (i = 0; i < frames; ++i) {
      pcm[3 * i] = 0.4 * sin(2.0 * M_PI * 440.0 * (double) i / (double) SR);
      pcm[3 * i + 1] =
          0.3 * sin(2.0 * M_PI * 1330.0 * (double) i / (double) SR + 1.0);
      pcm[3 * i + 2] = 0.2 * noise(&rng);
    }
    st = ebur128_init(3, SR, EBUR128_MODE_MIX_PREVIEW);
    CHECK(st != NULL);
    CHECK(ebur128_set_channel(st, 2, EBUR128_UNUSED) == EBUR128_SUCCESS);
    CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);

    /* the UNUSED input alone */
    out = query_mix(st, m_third, 1, w1);
    ref = ref_mix_loudness(pcm, frames, 3, m_third, 1, w1);
    CHECK(isfinite(out) && fabs(out - ref) < MIX_TOL);

    /* all three physical inputs, ignoring the channel map */
    out = query_mix(st, m_ident3, 3, w111);
    ref = ref_mix_loudness(pcm, frames, 3, m_ident3, 3, w111);
    CHECK(isfinite(out) && fabs(out - ref) < MIX_TOL);

    /* the classic global loudness must NOT include the UNUSED channel and
     * therefore differs from the identity mix */
    CHECK(ebur128_loudness_global(st, &global) == EBUR128_SUCCESS);
    CHECK(isfinite(global) && fabs(global - out) > 0.1);
    ebur128_destroy(&st);
  }

  free(pcm);
  printf("MIX_PREVIEW_CASE covariance PASS\n");
}

static void case_gating(void) {
  const size_t frames_a = 1 * SR;
  const size_t frames_b = 1 * SR;
  const size_t frames = frames_a + frames_b;
  double* pcm = (double*) malloc(frames * 2 * sizeof(double));
  size_t i;
  double m_mono[2] = { 1.0, 1.0 };
  double w1[1] = { 1.0 };
  ebur128_state* st;
  double out, ref, loud_only;

  CHECK(pcm != NULL);

  /* Segment A: in-phase stereo sine, amplitude 1.2e-3 (clearly above the
   * -70 LUFS absolute gate). Segment B: same signal at 2.83e-4, so the
   * original stereo energy per block is ~9e-8 * g^2, i.e. below the absolute
   * gate, while the mono sum [1 1] roughly quadruples the energy and pushes
   * these blocks above the gate. The correct order (gate the mixed block
   * energies) therefore keeps segment B; gating the original channels first
   * and mixing afterwards would drop it. */
  for (i = 0; i < frames_a; ++i) {
    double s = 1.2e-3 * sin(2.0 * M_PI * 1000.0 * (double) i / (double) SR);
    pcm[2 * i] = s;
    pcm[2 * i + 1] = s;
  }
  for (i = frames_a; i < frames; ++i) {
    double s = 2.83e-4 * sin(2.0 * M_PI * 1000.0 * (double) i / (double) SR);
    pcm[2 * i] = s;
    pcm[2 * i + 1] = s;
  }

  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  CHECK(st != NULL);
  CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);
  out = query_mix(st, m_mono, 1, w1);
  ref = ref_mix_loudness(pcm, frames, 2, m_mono, 1, w1);
  CHECK(isfinite(out) && fabs(out - ref) < MIX_TOL);

  /* loudness of segment A alone, measured the same way */
  {
    ebur128_state* st_a = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
    CHECK(st_a != NULL);
    CHECK(ebur128_add_frames_double(st_a, pcm, frames_a) == EBUR128_SUCCESS);
    loud_only = query_mix(st_a, m_mono, 1, w1);
    ebur128_destroy(&st_a);
  }
  CHECK(isfinite(loud_only));
  /* the quiet segment must take part in the mixed gating: it drags the
   * integrated loudness down by more than 1 LU */
  CHECK(out < loud_only - 1.0);
  ebur128_destroy(&st);

  /* all silence: no block passes the absolute gate */
  memset(pcm, 0, frames * 2 * sizeof(double));
  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  CHECK(st != NULL);
  CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);
  CHECK(query_mix(st, m_mono, 1, w1) == -HUGE_VAL);
  ebur128_destroy(&st);

  /* no frames at all, and less than 400ms: no complete block yet */
  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  CHECK(st != NULL);
  CHECK(query_mix(st, m_mono, 1, w1) == -HUGE_VAL);
  CHECK(ebur128_add_frames_double(st, pcm, SR / 10) == EBUR128_SUCCESS);
  CHECK(query_mix(st, m_mono, 1, w1) == -HUGE_VAL);
  ebur128_destroy(&st);

  /* --- error handling --- */
  {
    double m_ident2[4] = { 1.0, 0.0, 0.0, 1.0 };
    double m_bad[4] = { 1.0, 0.0, 0.0, 1.0 };
    double w_ok[2] = { 1.0, 1.41 };
    double w_bad[2] = { 1.0, 0.7 };
    double sentinel;
    ebur128_state* plain = ebur128_init(2, SR, EBUR128_MODE_I);
    CHECK(plain != NULL);

    /* querying without the mode returns INVALID_MODE */
    sentinel = 777.0;
    CHECK(ebur128_loudness_mix(plain, m_ident2, 2, w_ok, &sentinel) ==
          EBUR128_ERROR_INVALID_MODE);
    CHECK(sentinel == 777.0);
    ebur128_destroy(&plain);

    /* MIX_PREVIEW cannot be combined with HISTOGRAM */
    st = ebur128_init(2, SR,
                      EBUR128_MODE_MIX_PREVIEW | EBUR128_MODE_HISTOGRAM);
    CHECK(st == NULL);

    st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
    CHECK(st != NULL);

    /* invalid row counts */
    sentinel = 777.0;
    CHECK(ebur128_loudness_mix(st, m_ident2, 0, w_ok, &sentinel) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    CHECK(ebur128_loudness_mix(st, m_ident2, 33, w_ok, &sentinel) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    CHECK(sentinel == 777.0);

    /* NULL pointers */
    CHECK(ebur128_loudness_mix(st, NULL, 2, w_ok, &sentinel) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    CHECK(ebur128_loudness_mix(st, m_ident2, 2, NULL, &sentinel) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    CHECK(ebur128_loudness_mix(st, m_ident2, 2, w_ok, NULL) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    CHECK(sentinel == 777.0);

    /* non-finite matrix coefficients */
    m_bad[1] = NAN;
    CHECK(ebur128_loudness_mix(st, m_bad, 2, w_ok, &sentinel) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    m_bad[1] = INFINITY;
    CHECK(ebur128_loudness_mix(st, m_bad, 2, w_ok, &sentinel) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    m_bad[1] = -INFINITY;
    CHECK(ebur128_loudness_mix(st, m_bad, 2, w_ok, &sentinel) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    CHECK(sentinel == 777.0);

    /* invalid weight */
    CHECK(ebur128_loudness_mix(st, m_ident2, 2, w_bad, &sentinel) ==
          EBUR128_ERROR_INVALID_ARGUMENT);
    CHECK(sentinel == 777.0);

    /* all allowed weights are accepted; negative coefficients are fine */
    {
      double m_neg[4] = { 1.0, -1.0, -0.5, 0.25 };
      double w_all[4] = { 0.0, 1.0, 1.41, 2.0 };
      double m_rows4[8] = { 1.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.5, -0.5 };
      CHECK(ebur128_loudness_mix(st, m_ident2, 2, w_ok, &sentinel) ==
            EBUR128_SUCCESS);
      CHECK(ebur128_loudness_mix(st, m_neg, 2, w_ok, &sentinel) ==
            EBUR128_SUCCESS);
      CHECK(ebur128_loudness_mix(st, m_rows4, 4, w_all, &sentinel) ==
            EBUR128_SUCCESS);
    }
    ebur128_destroy(&st);
  }

  free(pcm);
  printf("MIX_PREVIEW_CASE gating PASS\n");
}

static void fill_loud(double* pcm, size_t frames) {
  size_t i;
  for (i = 0; i < frames; ++i) {
    double s = 0.5 * sin(2.0 * M_PI * 440.0 * (double) i / (double) SR);
    pcm[2 * i] = s;
    pcm[2 * i + 1] = s;
  }
}

static void case_history(void) {
  double m_mono[2] = { 1.0, 1.0 };
  double w1[1] = { 1.0 };
  double* loud = (double*) malloc(2 * SR * 2 * sizeof(double));
  double* silence = (double*) calloc(2 * SR * 2, sizeof(double));
  ebur128_state* st;
  double global;

  CHECK(loud != NULL && silence != NULL);
  fill_loud(loud, 2 * SR);

  /* history of 1000ms keeps the last 10 complete blocks; silent blocks
   * count towards the history and evict older loud blocks, while the
   * classic block list (mode I) is not fed by sub-gate blocks and keeps
   * the loud ones. */
  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  CHECK(st != NULL);
  CHECK(ebur128_set_max_history(st, 1000) == EBUR128_SUCCESS);
  CHECK(ebur128_add_frames_double(st, loud, 2 * SR) == EBUR128_SUCCESS);
  CHECK(isfinite(query_mix(st, m_mono, 1, w1)));
  CHECK(ebur128_add_frames_double(st, silence, 2 * SR) == EBUR128_SUCCESS);
  CHECK(query_mix(st, m_mono, 1, w1) == -HUGE_VAL);
  /* old interface behavior is unchanged: still finite */
  CHECK(ebur128_loudness_global(st, &global) == EBUR128_SUCCESS);
  CHECK(isfinite(global));
  ebur128_destroy(&st);

  /* shortening the history evicts old preview blocks immediately, and
   * following silence advances the eviction */
  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  CHECK(st != NULL);
  CHECK(ebur128_add_frames_double(st, loud, 2 * SR) == EBUR128_SUCCESS);
  CHECK(isfinite(query_mix(st, m_mono, 1, w1)));
  CHECK(ebur128_set_max_history(st, 400) == EBUR128_SUCCESS);
  /* the 4 newest blocks are still loud */
  CHECK(isfinite(query_mix(st, m_mono, 1, w1)));
  CHECK(ebur128_loudness_global(st, &global) == EBUR128_SUCCESS);
  CHECK(isfinite(global));
  /* 1 s of silence produces 7+ silent blocks, evicting all loud ones */
  CHECK(ebur128_add_frames_double(st, silence, 1 * SR) == EBUR128_SUCCESS);
  CHECK(query_mix(st, m_mono, 1, w1) == -HUGE_VAL);
  /* the classic block list only holds above-gate blocks, so the loud
   * blocks survive the silence there */
  CHECK(ebur128_loudness_global(st, &global) == EBUR128_SUCCESS);
  CHECK(isfinite(global));
  ebur128_destroy(&st);

  /* the history minimum is clamped to 400ms (4 blocks), not 100ms */
  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  CHECK(st != NULL);
  CHECK(ebur128_set_max_history(st, 100) == EBUR128_SUCCESS);
  CHECK(ebur128_add_frames_double(st, loud, 2 * SR) == EBUR128_SUCCESS);
  /* 400ms of silence: the 4 kept blocks are the ones completed during the
   * transition; the loudest of them is still above the gate. With a 100ms
   * history (1 block) only the newest, fully silent block would remain. */
  CHECK(ebur128_add_frames_double(st, silence, (SR / 10) * 4) ==
        EBUR128_SUCCESS);
  CHECK(isfinite(query_mix(st, m_mono, 1, w1)));
  /* another 600ms of silence: all kept blocks are fully silent */
  CHECK(ebur128_add_frames_double(st, silence, (SR / 10) * 6) ==
        EBUR128_SUCCESS);
  CHECK(query_mix(st, m_mono, 1, w1) == -HUGE_VAL);
  ebur128_destroy(&st);

  free(loud);
  free(silence);
  printf("MIX_PREVIEW_CASE history PASS\n");
}

static void case_compatibility(void) {
  const size_t frames = 2 * SR;
  double* pcm = (double*) malloc(frames * 2 * sizeof(double));
  size_t i;
  double m_ident2[4] = { 1.0, 0.0, 0.0, 1.0 };
  double m_mono[2] = { 1.0, 1.0 };
  double m_swap[4] = { 0.0, 1.0, 1.0, 0.0 };
  double w11[2] = { 1.0, 1.0 };
  double w1[1] = { 1.0 };
  ebur128_state *st, *plain;
  double g_preview, g_plain, out, q1, q2;
  unsigned long rng = 4242;

  CHECK(pcm != NULL);
  for (i = 0; i < frames; ++i) {
    pcm[2 * i] = 0.4 * sin(2.0 * M_PI * 550.0 * (double) i / (double) SR);
    pcm[2 * i + 1] = 0.3 * noise(&rng);
  }

  /* the classic global loudness of a MIX_PREVIEW state matches a plain
   * EBUR128_MODE_I state fed the same audio */
  st = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
  plain = ebur128_init(2, SR, EBUR128_MODE_I);
  CHECK(st != NULL && plain != NULL);
  CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);
  CHECK(ebur128_add_frames_double(plain, pcm, frames) == EBUR128_SUCCESS);
  CHECK(ebur128_loudness_global(st, &g_preview) == EBUR128_SUCCESS);
  CHECK(ebur128_loudness_global(plain, &g_plain) == EBUR128_SUCCESS);
  CHECK(isfinite(g_preview) && fabs(g_preview - g_plain) < EXACT_TOL);

  /* identity matrix query matches the classic global loudness */
  out = query_mix(st, m_ident2, 2, w11);
  CHECK(isfinite(out) && fabs(out - g_preview) < MIX_TOL);

  /* alternating queries do not disturb the state or each other */
  q1 = query_mix(st, m_mono, 1, w1);
  (void) query_mix(st, m_swap, 2, w11);
  (void) query_mix(st, m_ident2, 2, w11);
  q2 = query_mix(st, m_mono, 1, w1);
  CHECK(q1 == q2);

  /* feeding more audio after the queries is unaffected by them */
  for (i = 0; i < frames; ++i) {
    pcm[2 * i] = 0.2 * sin(2.0 * M_PI * 700.0 * (double) i / (double) SR);
    pcm[2 * i + 1] = pcm[2 * i];
  }
  CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);
  CHECK(ebur128_add_frames_double(plain, pcm, frames) == EBUR128_SUCCESS);
  out = query_mix(st, m_ident2, 2, w11);
  CHECK(ebur128_loudness_global(plain, &g_plain) == EBUR128_SUCCESS);
  CHECK(isfinite(out) && fabs(out - g_plain) < MIX_TOL);
  ebur128_destroy(&plain);

  /* change_parameters: identical parameters -> NO_CHANGE; a real change
   * clears only the preview history */
  CHECK(ebur128_change_parameters(st, 2, SR) == EBUR128_ERROR_NO_CHANGE);
  CHECK(isfinite(query_mix(st, m_ident2, 2, w11)));
  CHECK(ebur128_change_parameters(st, 2, 44100) == EBUR128_SUCCESS);
  /* preview history was cleared: no complete block at the new rate yet */
  CHECK(query_mix(st, m_ident2, 2, w11) == -HUGE_VAL);
  {
    const size_t frames44 = 44100;
    double* pcm44 = (double*) malloc(frames44 * 2 * sizeof(double));
    double ref;
    CHECK(pcm44 != NULL);
    for (i = 0; i < frames44; ++i) {
      double s = 0.4 * sin(2.0 * M_PI * 550.0 * (double) i / 44100.0);
      pcm44[2 * i] = s;
      pcm44[2 * i + 1] = s;
    }
    CHECK(ebur128_add_frames_double(st, pcm44, frames44) == EBUR128_SUCCESS);
    out = query_mix(st, m_mono, 1, w1);
    {
      /* reference at 44100 Hz */
      double* mixed = (double*) malloc(frames44 * sizeof(double));
      ebur128_state* rst;
      CHECK(mixed != NULL);
      for (i = 0; i < frames44; ++i) {
        mixed[i] = pcm44[2 * i] + pcm44[2 * i + 1];
      }
      rst = ebur128_init(1, 44100, EBUR128_MODE_I);
      CHECK(rst != NULL);
      CHECK(ebur128_add_frames_double(rst, mixed, frames44) ==
            EBUR128_SUCCESS);
      CHECK(ebur128_loudness_global(rst, &ref) == EBUR128_SUCCESS);
      ebur128_destroy(&rst);
      free(mixed);
    }
    CHECK(isfinite(out) && fabs(out - ref) < MIX_TOL);
    free(pcm44);
  }
  ebur128_destroy(&st);

  /* all four add_frames variants see the same normalized input */
  {
    const size_t n = SR;
    short* ps = (short*) malloc(n * 2 * sizeof(short));
    int* pi = (int*) malloc(n * 2 * sizeof(int));
    float* pf = (float*) malloc(n * 2 * sizeof(float));
    double* pd = (double*) malloc(n * 2 * sizeof(double));
    ebur128_state *ss, *si, *sf, *sd;
    double vs, vi, vf, vd;
    CHECK(ps != NULL && pi != NULL && pf != NULL && pd != NULL);
    for (i = 0; i < n; ++i) {
      double t = (double) i / (double) SR;
      short l = (short) lrint(0.4 * 32767.0 *
                              sin(2.0 * M_PI * 440.0 * t));
      short r = (short) lrint(0.3 * 32767.0 *
                              sin(2.0 * M_PI * 990.0 * t + 0.5));
      ps[2 * i] = l;
      ps[2 * i + 1] = r;
      /* int scaling: (v << 16) / 2^31 == v / 2^15, same normalized value */
      pi[2 * i] = (int) l << 16;
      pi[2 * i + 1] = (int) r << 16;
      /* l / 32768.0 is exactly representable as float and double */
      pf[2 * i] = (float) (l / 32768.0);
      pf[2 * i + 1] = (float) (r / 32768.0);
      pd[2 * i] = l / 32768.0;
      pd[2 * i + 1] = r / 32768.0;
    }
    ss = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
    si = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
    sf = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
    sd = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
    CHECK(ss != NULL && si != NULL && sf != NULL && sd != NULL);
    CHECK(ebur128_add_frames_short(ss, ps, n) == EBUR128_SUCCESS);
    CHECK(ebur128_add_frames_int(si, pi, n) == EBUR128_SUCCESS);
    CHECK(ebur128_add_frames_float(sf, pf, n) == EBUR128_SUCCESS);
    CHECK(ebur128_add_frames_double(sd, pd, n) == EBUR128_SUCCESS);
    vs = query_mix(ss, m_ident2, 2, w11);
    vi = query_mix(si, m_ident2, 2, w11);
    vf = query_mix(sf, m_ident2, 2, w11);
    vd = query_mix(sd, m_ident2, 2, w11);
    CHECK(isfinite(vs) && isfinite(vi) && isfinite(vf) && isfinite(vd));
    CHECK(fabs(vs - vd) < EXACT_TOL && fabs(vi - vd) < EXACT_TOL &&
          fabs(vf - vd) < EXACT_TOL);
    ebur128_destroy(&ss);
    ebur128_destroy(&si);
    ebur128_destroy(&sf);
    ebur128_destroy(&sd);
    free(ps);
    free(pi);
    free(pf);
    free(pd);
  }

  /* splitting the input into odd-sized chunks does not change the preview */
  {
    const size_t n = frames + SR / 2;
    double* sig = (double*) malloc(n * 2 * sizeof(double));
    ebur128_state *whole, *chunked;
    double qw, qc;
    size_t pos;
    const size_t chunks[] = { 1, 7, 100, 4800, 13333, 9600 };
    size_t ci;
    CHECK(sig != NULL);
    rng = 777;
    for (i = 0; i < n; ++i) {
      sig[2 * i] = 0.3 * noise(&rng);
      sig[2 * i + 1] = 0.3 * noise(&rng);
    }
    whole = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
    chunked = ebur128_init(2, SR, EBUR128_MODE_MIX_PREVIEW);
    CHECK(whole != NULL && chunked != NULL);
    CHECK(ebur128_add_frames_double(whole, sig, n) == EBUR128_SUCCESS);
    pos = 0;
    for (ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]) && pos < n; ++ci) {
      size_t take = chunks[ci];
      if (take > n - pos) {
        take = n - pos;
      }
      CHECK(ebur128_add_frames_double(chunked, sig + 2 * pos, take) ==
            EBUR128_SUCCESS);
      pos += take;
    }
    if (pos < n) {
      CHECK(ebur128_add_frames_double(chunked, sig + 2 * pos, n - pos) ==
            EBUR128_SUCCESS);
    }
    qw = query_mix(whole, m_ident2, 2, w11);
    qc = query_mix(chunked, m_ident2, 2, w11);
    CHECK(isfinite(qw) && fabs(qw - qc) < EXACT_TOL);
    ebur128_destroy(&whole);
    ebur128_destroy(&chunked);
    free(sig);
  }

  free(pcm);
  printf("MIX_PREVIEW_CASE compatibility PASS\n");
}

int main(void) {
  case_covariance();
  case_gating();
  case_history();
  case_compatibility();
  return 0;
}
