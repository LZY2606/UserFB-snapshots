/* Peak-window streaming observer tests.
 *
 * Three groups, registered through task_support/check.h:
 *   interpolation - independent offline formula for every quarter-frame
 *                   phase, overshoot, ties, normalised integer-output scale;
 *   streaming     - whole/1/7/113-frame inputs, small output capacities,
 *                   split/interleaved/callback snapshots, eviction, short
 *                   streams, starvation and repeated draining;
 *   lifecycle     - configuration rejection, clear/reuse, legacy PCM
 *                   compatibility, formats and channel counts.
 */

#include "soxr.h"
#include "peak.h"
#include "../task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEST_N 300u
#define TEST_W 64u
#define N_TARGET 3u

static const uint64_t snap_targets[N_TARGET] = {37, 100, 250};

struct ref_peak {
  int valid;
  double magnitude;
  uint64_t frame;
  unsigned phase;
  unsigned channel;
};

/* ------------------------- Independent reference ------------------------- */

static double ref_sinc(double t)
{
  return t == 0.? 1. : sin(M_PI * t) / (M_PI * t);
}

static void ref_weights(double w[3][SOXR_PEAK_TAPS])
{
  int p, j;
  for (p = 1; p <= 3; ++p) {
    double sum = 0.;
    for (j = 0; j < SOXR_PEAK_TAPS; ++j) {
      double t = (double)p / 4. - ((double)j - 15.);
      w[p - 1][j] = ref_sinc(t) * (1. + cos(M_PI * t / 16.)) / 2.;
      sum += w[p - 1][j];
    }
    for (j = 0; j < SOXR_PEAK_TAPS; ++j)
      w[p - 1][j] /= sum;
  }
}

/* Value of phase p (0..3) of unit k; only frames [0, delivered) exist,
 * everything else is zero. */
static double ref_phase_value(double const * x, uint64_t delivered,
    int64_t k, unsigned p, double const w[3][SOXR_PEAK_TAPS])
{
  if (p == 0)
    return (k >= 0 && (uint64_t)k < delivered)? x[k] : 0.;
  else {
    int j;
    double v = 0.;
    for (j = 0; j < SOXR_PEAK_TAPS; ++j) {
      int64_t idx = k + (int64_t)j - 15;
      double s = (idx >= 0 && (uint64_t)idx < delivered)? x[idx] : 0.;
      v += w[p - 1][j] * s;
    }
    return v;
  }
}

static void ref_peak_window(double const * const * x, uint64_t delivered,
    unsigned nch, uint64_t completed, unsigned window, unsigned channel,
    struct ref_peak * r)
{
  double weights[3][SOXR_PEAK_TAPS];
  uint64_t lo, k;
  unsigned c, first, last, p;
  ref_weights(weights);
  lo = completed > window? completed - window : 0;
  r->valid = 0;
  r->magnitude = 0.;
  r->frame = 0;
  r->phase = 0;
  r->channel = 0;
  first = channel < nch? channel : 0;
  last = channel < nch? channel : nch - 1;
  for (c = first; c <= last; ++c)
    for (k = lo; k < completed; ++k)
      for (p = 0; p < 4; ++p) {
        double m = fabs(ref_phase_value(x[c], delivered,
            (int64_t)k, p, weights));
        if (!r->valid || m > r->magnitude ||
            (m == r->magnitude && (k < r->frame ||
                (k == r->frame && (p < r->phase ||
                    (p == r->phase && c < r->channel)))))) {
          r->valid = 1;
          r->magnitude = m;
          r->frame = k;
          r->phase = p;
          r->channel = c;
        }
      }
}

/* ------------------------------- Test data ------------------------------- */

/* Two-sample blocks of alternating sign at amplitude a: every integer
 * sample stays below 1 while the quarter-frame interpolants overshoot. */
static float signal_sample(unsigned channel, unsigned frame)
{
  static double const amp[6] = {.95, .9, .85, .8, .75, .7};
  double a = amp[channel % 6];
  int sign = ((frame / 2 + channel) & 1)? -1 : 1;
  return (float)(sign * a);
}

static float * build_interleaved(unsigned nch, unsigned n)
{
  unsigned i, c;
  float * s = malloc((size_t)n * nch * sizeof(float));
  for (i = 0; i < n; ++i)
    for (c = 0; c < nch; ++c)
      s[(size_t)i * nch + c] = signal_sample(c, i);
  return s;
}

static float ** build_split(unsigned nch, unsigned n)
{
  unsigned i, c;
  float ** s = malloc(nch * sizeof(*s));
  for (c = 0; c < nch; ++c) {
    s[c] = malloc(n * sizeof(float));
    for (i = 0; i < n; ++i)
      s[c][i] = signal_sample(c, i);
  }
  return s;
}

static void free_split(float ** s, unsigned nch)
{
  unsigned c;
  for (c = 0; c < nch; ++c) free(s[c]);
  free(s);
}

/* ------------------------------ Run machinery ---------------------------- */

struct snapshots {
  soxr_peak_info_t all[N_TARGET + 1];
  soxr_peak_info_t * ch;           /* (N_TARGET+1) * nch entries. */
  unsigned got[N_TARGET];
};

struct run_result {
  float * pcm;
  size_t cap, n;
  struct snapshots snaps;
};

struct variant {
  int split;                       /* Split buffers vs interleaved. */
  int callback;                    /* Drive via soxr_output input fn. */
  size_t in_chunk;
  size_t out_cap;
};

struct cb_state {
  float const * signal;
  unsigned nch, n, pos;
};

static size_t cb_input(void * state, soxr_in_t * data, size_t requested)
{
  struct cb_state * s = state;
  size_t avail = s->n - s->pos;
  size_t len = requested < avail? requested : avail;
  *data = (soxr_in_t)(s->signal + (size_t)s->pos * s->nch);
  s->pos += (unsigned)len;
  return len;
}

static int capture_snapshot(soxr_t p, unsigned nch, struct snapshots * s,
    unsigned slot)
{
  unsigned c;
  TASK_CHECK(!soxr_peak_query_all(p, &s->all[slot]));
  for (c = 0; c < nch; ++c)
    TASK_CHECK(!soxr_peak_query(p, c, &s->ch[(size_t)slot * nch + c]));
  return 0;
}

static int maybe_capture(soxr_t p, unsigned nch, struct snapshots * s)
{
  soxr_peak_info_t info;
  unsigned t;
  TASK_CHECK(!soxr_peak_query_all(p, &info));
  for (t = 0; t < N_TARGET; ++t)
    if (!s->got[t] && info.delivered >= snap_targets[t]) {
      s->got[t] = 1;
      TASK_CHECK(!capture_snapshot(p, nch, s, t));
    }
  return 0;
}

static int run_variant(struct variant const * v, soxr_io_spec_t const * io,
    soxr_quality_spec_t const * q, double irate, double orate, unsigned nch,
    unsigned n, unsigned window, struct run_result * rr)
{
  float * interleaved = build_interleaved(nch, n);
  float ** split = build_split(nch, n);
  soxr_t p;
  soxr_error_t e;
  size_t in_pos = 0;
  struct cb_state cb;
  unsigned t;

  memset(rr, 0, sizeof(*rr));
  rr->cap = n + 64;
  rr->pcm = malloc((size_t)rr->cap * nch * sizeof(float));
  rr->snaps.ch = calloc((N_TARGET + 1) * nch, sizeof(soxr_peak_info_t));

  p = soxr_create(irate, orate, nch, &e, io, q, 0);
  TASK_CHECK(p && !e);
  e = soxr_set_peak_window(p, window);
  TASK_CHECK(!e);

  if (v->callback) {
    cb.signal = interleaved; cb.nch = nch; cb.n = n; cb.pos = 0;
    TASK_CHECK(!soxr_set_input_fn(p, cb_input, &cb, v->in_chunk));
  }

  for (;;) {
    size_t idone = 0, odone = 0;
    size_t space = rr->cap - rr->n;
    size_t olen = v->out_cap < space? v->out_cap : space;
    if (v->callback) {
      if (!olen) break;
      odone = soxr_output(p, rr->pcm + (size_t)rr->n * nch, olen);
      TASK_CHECK(!soxr_error(p));
      rr->n += odone;
      if (cb.pos >= n && !odone) break;
    }
    else if (v->split) {
      float * tmp[6];
      float * in_ptrs[6];
      size_t ilen = n - in_pos, i;
      unsigned c2u;
      if (ilen > v->in_chunk) ilen = v->in_chunk;
      for (c2u = 0; c2u < nch; ++c2u) {
        tmp[c2u] = malloc((olen? olen : 1) * sizeof(float));
        in_ptrs[c2u] = split[c2u] + in_pos;
      }
      e = soxr_process(p, ilen? (soxr_in_t)in_ptrs : 0, ilen, &idone,
          tmp, olen, &odone);
      TASK_CHECK(!e);
      for (i = 0; i < odone; ++i)
        for (c2u = 0; c2u < nch; ++c2u)
          rr->pcm[((size_t)rr->n + i) * nch + c2u] = tmp[c2u][i];
      for (c2u = 0; c2u < nch; ++c2u) free(tmp[c2u]);
      in_pos += idone;
      rr->n += odone;
      if (!ilen && !odone) break;
    }
    else {
      size_t ilen = n - in_pos;
      if (ilen > v->in_chunk) ilen = v->in_chunk;
      e = soxr_process(p, ilen?
          (soxr_in_t)(interleaved + (size_t)in_pos * nch) : 0,
          ilen, &idone, rr->pcm + (size_t)rr->n * nch, olen, &odone);
      TASK_CHECK(!e);
      in_pos += idone;
      rr->n += odone;
      if (!ilen && !odone) break;
    }
    TASK_CHECK(!maybe_capture(p, nch, &rr->snaps));
  }
  capture_snapshot(p, nch, &rr->snaps, N_TARGET);
  for (t = 0; t < N_TARGET; ++t)
    TASK_CHECK(rr->snaps.got[t]);

  soxr_delete(p);
  free(interleaved);
  free_split(split, nch);
  return 0;
}

static void free_run(struct run_result * rr)
{
  free(rr->pcm);
  free(rr->snaps.ch);
}

/* Unity-rate helper: feed all `n' interleaved input frames, then drain the
 * engine until exactly `n' output frames are in `out'.  Always supplies a
 * valid output buffer (a NULL output pointer is not accepted by the
 * interleaved path even at zero capacity). */
static int run_unity(soxr_t p, float const * in, size_t n, float * out)
{
  size_t idone = 0, odone = 0, total = 0;
  TASK_CHECK(!soxr_process(p, in, n, &idone, out, n, &odone));
  TASK_CHECK(idone == n);
  total += odone;
  while (total < n) {
    TASK_CHECK(!soxr_process(p, 0, 0, 0, out + total, n - total, &odone));
    TASK_CHECK(odone);
    total += odone;
  }
  TASK_CHECK(total == n);
  TASK_CHECK(!soxr_process(p, 0, 0, 0, out, 0, &odone));
  TASK_CHECK(odone == 0);
  return 0;
}

/* Returns 0 when equal; TASK_CHECK inside returns 1 on the first mismatch. */
static int info_equal(soxr_peak_info_t const * a, soxr_peak_info_t const * b,
    double tol)
{
  TASK_CHECK(a->valid == b->valid);
  TASK_CHECK(a->delivered == b->delivered);
  TASK_CHECK(a->observed == b->observed);
  TASK_CHECK(a->window_start == b->window_start);
  TASK_CHECK(a->frame == b->frame);
  TASK_CHECK(a->phase == b->phase);
  TASK_CHECK(a->channel == b->channel);
  TASK_CHECK(fabs(a->magnitude - b->magnitude) <= tol);
  return 0;
}

/* Compare every snapshot against the offline reference evaluated over the
 * captured output PCM, truncated at each snapshot's delivered count. */
static int check_run_against_reference(struct run_result const * rr,
    unsigned nch, unsigned window, double tol)
{
  double ** ref;
  unsigned c, slot;
  ref = malloc(nch * sizeof(*ref));
  for (c = 0; c < nch; ++c) {
    size_t i;
    ref[c] = malloc(rr->n * sizeof(double));
    for (i = 0; i < rr->n; ++i)
      ref[c][i] = rr->pcm[i * nch + c];
  }

  for (slot = 0; slot <= N_TARGET; ++slot) {
    soxr_peak_info_t const * all = &rr->snaps.all[slot];
    struct ref_peak expect;
    TASK_CHECK(all->valid);
    TASK_CHECK(all->observed <= all->delivered);
    TASK_CHECK(all->window_start == (all->observed > window?
        all->observed - window : 0));
    ref_peak_window((double const * const *)ref, all->delivered, nch,
        all->observed, window, nch, &expect);
    TASK_CHECK(expect.valid);
    TASK_CHECK(all->frame == expect.frame);
    TASK_CHECK(all->phase == expect.phase);
    TASK_CHECK(all->channel == expect.channel);
    TASK_CHECK(fabs(all->magnitude - expect.magnitude) <= tol);
    for (c = 0; c < nch; ++c) {
      soxr_peak_info_t const * info = &rr->snaps.ch[(size_t)slot * nch + c];
      ref_peak_window((double const * const *)ref, info->delivered, nch,
          info->observed, window, c, &expect);
      TASK_CHECK(expect.valid);
      TASK_CHECK(info->frame == expect.frame);
      TASK_CHECK(info->phase == expect.phase);
      TASK_CHECK(info->channel == c);
      TASK_CHECK(fabs(info->magnitude - expect.magnitude) <= tol);
    }
  }
  for (c = 0; c < nch; ++c) free(ref[c]);
  free(ref);
  return 0;
}

/* --------------------------- interpolation group ------------------------- */

/* Drive the observer module directly with a known sequence and isolate one
 * unit at a time through a W=1 window; every quarter-frame phase is checked
 * against the independent formula. */
static int case_module_phase_formula(void)
{
  double weights[3][SOXR_PEAK_TAPS];
  double x[96];
  unsigned n = 96, k;
  ref_weights(weights);
  for (k = 0; k < n; ++k)
    x[k] = signal_sample(0, k) + .05 * sin(M_PI * k / 7.);

  for (k = 4; k < 70; ++k) {
    soxr_peak_observer_t * q = _soxr_peak_create(1);
    struct ref_peak expect;
    double const * refs[1];
    double mag;
    unsigned phase, channel;
    uint64_t frame;
    TASK_CHECK(q);
    TASK_CHECK(_soxr_peak_set_storage(q, 1, 1.));
    _soxr_peak_store_channel_d(q, 0, x, (size_t)k + 17);
    _soxr_peak_commit(q, (size_t)k + 17);
    TASK_CHECK(_soxr_peak_completed(q) == (uint64_t)k + 1);
    refs[0] = x;
    ref_peak_window(refs, (uint64_t)k + 17, 1, (uint64_t)k + 1, 1, 0,
        &expect);
    TASK_CHECK(expect.valid && expect.frame == k);
    TASK_CHECK(_soxr_peak_query(q, 0, &mag, &phase, &frame, &channel));
    TASK_CHECK(frame == k);
    TASK_CHECK(phase == expect.phase);
    TASK_CHECK(channel == 0);
    TASK_CHECK(fabs(mag - expect.magnitude) < 1e-12);
    _soxr_peak_delete(q);
  }
  return 0;
}

/* Integer samples all below 1 while a quarter-frame interpolant exceeds 1;
 * the end-to-end resampler path must report the same peak as the formula. */
static int case_overshoot_and_phase_values(void)
{
  double weights[3][SOXR_PEAK_TAPS];
  double x[80];
  double const * refs[1];
  struct ref_peak r;
  unsigned n = 80, k, p, overshoot = 0;
  int all_below_one = 1;
  ref_weights(weights);
  for (k = 0; k < n; ++k)
    x[k] = signal_sample(0, k);
  for (k = 0; k < n; ++k)
    if (fabs(x[k]) >= 1.) all_below_one = 0;
  TASK_CHECK(all_below_one);
  for (k = 16; k + 16 < n; ++k)
    for (p = 1; p < 4; ++p) {
      double v = fabs(ref_phase_value(x, n, (int64_t)k, p, weights));
      if (v > 1.) ++overshoot;
    }
  TASK_CHECK(overshoot > 10);

  {
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t q = soxr_quality_spec(SOXR_QQ, 0);
    struct run_result rr;
    struct variant v = {0, 0, TEST_N, 64};
    float const * pcm;
    size_t i;
    double * refx;
    TASK_CHECK(!run_variant(&v, &io, &q, 48000, 48000, 1, TEST_N, TEST_W, &rr));
    TASK_CHECK(rr.n == TEST_N);
    pcm = rr.pcm;
    for (i = 0; i < rr.n; ++i)
      TASK_CHECK(fabsf(pcm[i]) < 1.);
    refx = malloc(rr.n * sizeof(double));
    for (i = 0; i < rr.n; ++i) refx[i] = pcm[i];
    refs[0] = refx;
    ref_peak_window(refs, rr.n, 1, rr.n, TEST_W, 0, &r);
    TASK_CHECK(r.valid && r.phase != 0 && r.magnitude > 1.);
    TASK_CHECK(rr.snaps.all[N_TARGET].phase == r.phase);
    TASK_CHECK(rr.snaps.all[N_TARGET].frame == r.frame);
    TASK_CHECK(fabs(rr.snaps.all[N_TARGET].magnitude - r.magnitude) < 1e-10);
    free(refx);
    free_run(&rr);
  }
  return 0;
}

/* Equal magnitudes resolve to the earlier position, then the smaller
 * channel number; the window floor must drop expired entries.  Queried in
 * the plateau interior (no finish, so no trailing-zero edge effects). */
static int case_tie_and_eviction_rules(void)
{
  enum { N = 64, W = 8 };
  soxr_peak_observer_t * q;
  double ch0[N], ch1[N];
  double * refs[2];
  struct ref_peak r;
  double weights[3][SOXR_PEAK_TAPS];
  unsigned i;
  double mag;
  unsigned phase, channel, p;
  uint64_t frame;

  ref_weights(weights);
  /* Period-3 pattern on ch0: frames congruent 0 mod 3 carry the high value,
   * so units 42 and 45 see identical 32-tap neighbourhoods (a genuine tie).
   * ch1 is a constant plateau. */
  for (i = 0; i < N; ++i) {
    double pat[3] = {.6, .1, .1};
    ch0[i] = pat[i % 3];
    ch1[i] = .5;
  }

  q = _soxr_peak_create(2);
  TASK_CHECK(q);
  TASK_CHECK(_soxr_peak_set_storage(q, W, 1.));
  _soxr_peak_store_channel_d(q, 0, ch0, N);
  _soxr_peak_store_channel_d(q, 1, ch1, N);
  _soxr_peak_commit(q, N);
  /* No finish: completed = N - 16 = 48, window [40, 48), every unit there is
   * surrounded by in-bounds samples (plateau interior). */
  TASK_CHECK(_soxr_peak_completed(q) == 48);
  refs[0] = ch0; refs[1] = ch1;

  /* ch0: a genuine tie between units 42 and 45; the earlier one must win. */
  {
    double m42 = 0., m45 = 0.;
    for (p = 0; p < 4; ++p) {
      double a = fabs(ref_phase_value(ch0, N, 42, p, weights));
      double b = fabs(ref_phase_value(ch0, N, 45, p, weights));
      if (a > m42) m42 = a;
      if (b > m45) m45 = b;
    }
    TASK_CHECK(fabs(m42 - m45) < 1e-14 && m42 > .5);
  }
  ref_peak_window((double const * const *)refs, N, 2, 48, W, 0, &r);
  TASK_CHECK(r.valid && r.frame == 42 && r.channel == 0);
  TASK_CHECK(_soxr_peak_query(q, 0, &mag, &phase, &frame, &channel));
  TASK_CHECK(frame == 42 && channel == 0);
  TASK_CHECK(fabs(mag - r.magnitude) < 1e-14);

  /* Merged: ch0's .6 pattern beats ch1's .5 plateau; same tie outcome. */
  ref_peak_window((double const * const *)refs, N, 2, 48, W, 2, &r);
  TASK_CHECK(r.valid && r.frame == 42 && r.channel == 0);
  TASK_CHECK(_soxr_peak_query(q, 2, &mag, &phase, &frame, &channel));
  TASK_CHECK(frame == 42 && channel == 0);
  TASK_CHECK(fabs(mag - r.magnitude) < 1e-14);

  /* ch1 alone: constant plateau, every cell ties -> window start, phase 0. */
  ref_peak_window((double const * const *)refs, N, 2, 48, W, 1, &r);
  TASK_CHECK(r.valid && r.frame == 40 && r.phase == 0 && r.channel == 1);
  TASK_CHECK(fabs(r.magnitude - .5) < 1e-15);
  TASK_CHECK(_soxr_peak_query(q, 1, &mag, &phase, &frame, &channel));
  TASK_CHECK(frame == 40 && phase == 0 && channel == 1);
  TASK_CHECK(fabs(mag - .5) < 1e-15);
  _soxr_peak_delete(q);

  /* Eviction: a strong early pulse disappears once it leaves the window. */
  {
    soxr_peak_observer_t * e = _soxr_peak_create(1);
    double s[N];
    for (i = 0; i < N; ++i) s[i] = .2;
    s[10] = .9;
    TASK_CHECK(_soxr_peak_set_storage(e, W, 1.));
    _soxr_peak_store_channel_d(e, 0, s, N);
    _soxr_peak_commit(e, N);
    TASK_CHECK(_soxr_peak_completed(e) == 48);
    TASK_CHECK(_soxr_peak_query(e, 0, &mag, &phase, &frame, &channel));
    TASK_CHECK(frame >= 40);                  /* Pulse at 10 long evicted. */
    TASK_CHECK(fabs(mag - .2) < 1e-14);       /* Plateau value remains. */
    _soxr_peak_delete(e);
  }
  return 0;
}

/* Integer output types report pre-saturation, normalised magnitudes that
 * match the floating-point engine's observer reading. */
static int case_integer_output_normalised(void)
{
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_QQ, SOXR_NO_DITHER);
  struct run_result ref_run;
  struct variant vf = {0, 0, TEST_N, 64};
  soxr_io_spec_t io_f = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_datatype_t types[2] = {SOXR_INT16_I, SOXR_INT32_I};
  unsigned t;
  TASK_CHECK(!run_variant(&vf, &io_f, &q, 48000, 48000, 1,
      TEST_N, TEST_W, &ref_run));
  for (t = 0; t < 2; ++t) {
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, types[t]);
    soxr_t p;
    soxr_error_t e;
    soxr_peak_info_t info;
    unsigned i;
    float * in = build_interleaved(1, TEST_N);
    size_t bytes = soxr_datatype_size(types[t]);
    void * out = malloc((size_t)TEST_N * bytes);
    size_t idone = 0, odone = 0, total = 0;
    p = soxr_create(48000, 48000, 1, &e, &io, &q, 0);
    TASK_CHECK(p && !e);
    TASK_CHECK(!soxr_set_peak_window(p, TEST_W));
    e = soxr_process(p, in, TEST_N, &idone, out, TEST_N, &odone);
    TASK_CHECK(!e && idone == TEST_N);
    total += odone;
    while (total < TEST_N) {                 /* Drain engine latency. */
      e = soxr_process(p, 0, 0, 0, (char *)out + total * bytes,
          TEST_N - total, &odone);
      TASK_CHECK(!e && odone);
      total += odone;
    }
    TASK_CHECK(total == TEST_N);
    e = soxr_process(p, 0, 0, 0, out, 0, &odone);
    TASK_CHECK(!e && odone == 0);
    TASK_CHECK(!soxr_peak_query_all(p, &info));
    TASK_CHECK(info.valid && info.observed == TEST_N);
    TASK_CHECK(info.magnitude > 1.);
    TASK_CHECK(fabs(info.magnitude -
        ref_run.snaps.all[N_TARGET].magnitude) < 1e-9);
    for (i = 0; i < TEST_N; ++i) {
      if (types[t] == SOXR_INT16_I)
        TASK_CHECK(abs((int)((int16_t *)out)[i]) <= 32767);
      else
        TASK_CHECK(abs((int)((int32_t *)out)[i]) <= 2147483647);
    }
    soxr_delete(p);
    free(in); free(out);
  }
  free_run(&ref_run);
  return 0;
}

/* ----------------------------- streaming group --------------------------- */

static int case_chunk_snapshots(void)
{
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_QQ, 0);
  struct variant variants[4] = {
    {0, 0, TEST_N, 128},   /* Whole block. */
    {0, 0, 1, 7},          /* One input frame at a time, tiny output cap. */
    {0, 0, 7, 3},
    {0, 0, 113, 5},
  };
  struct run_result rr[4];
  unsigned v, c;
  for (v = 0; v < 4; ++v) {
    TASK_CHECK(!run_variant(&variants[v], &io, &q, 48000, 48000, 2,
        TEST_N, TEST_W, &rr[v]));
    TASK_CHECK(rr[v].n == TEST_N);
    TASK_CHECK(!check_run_against_reference(&rr[v], 2, TEST_W, 1e-10));
  }
  for (v = 1; v < 4; ++v) {
    TASK_CHECK(rr[v].n == rr[0].n);
    TASK_CHECK(!memcmp(rr[v].pcm, rr[0].pcm,
        rr[0].n * 2 * sizeof(float)));     /* Bit-identical PCM. */
    /* Intermediate snapshots are taken at the first call past each target,
     * so their delivered counts can differ by chunk shape; each one is
     * independently checked against the reference above.  Only the final
     * state (same fully-drained stream) must be identical. */
    TASK_CHECK(!info_equal(&rr[v].snaps.all[N_TARGET],
        &rr[0].snaps.all[N_TARGET], 1e-12));
    for (c = 0; c < 2; ++c)
      TASK_CHECK(!info_equal(
          &rr[v].snaps.ch[(size_t)N_TARGET * 2 + c],
          &rr[0].snaps.ch[(size_t)N_TARGET * 2 + c], 1e-12));
  }
  for (v = 0; v < 4; ++v) free_run(&rr[v]);
  return 0;
}

static int case_split_interleaved_callback(void)
{
  soxr_io_spec_t io_i = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_io_spec_t io_s = soxr_io_spec(SOXR_FLOAT32_S, SOXR_FLOAT32_S);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, 0);
  struct variant v_i = {0, 0, 7, 11};
  struct variant v_s = {1, 0, 7, 11};
  struct variant v_cb = {0, 1, 7, 11};
  struct run_result a, b, cres;
  unsigned slot;
  TASK_CHECK(!run_variant(&v_i, &io_i, &q, 48000, 44100, 2,
      TEST_N, TEST_W, &a));
  TASK_CHECK(!run_variant(&v_s, &io_s, &q, 48000, 44100, 2,
      TEST_N, TEST_W, &b));
  TASK_CHECK(!run_variant(&v_cb, &io_i, &q, 48000, 44100, 2,
      TEST_N, TEST_W, &cres));
  TASK_CHECK(a.n == b.n && b.n == cres.n && a.n > 0);
  TASK_CHECK(!memcmp(a.pcm, b.pcm, a.n * 2 * sizeof(float)));
  TASK_CHECK(!memcmp(a.pcm, cres.pcm, a.n * 2 * sizeof(float)));
  TASK_CHECK(!check_run_against_reference(&a, 2, TEST_W, 1e-10));
  TASK_CHECK(!check_run_against_reference(&b, 2, TEST_W, 1e-10));
  for (slot = 0; slot <= N_TARGET; ++slot)
    TASK_CHECK(!info_equal(&cres.snaps.all[slot], &a.snaps.all[slot], 1e-12));
  free_run(&a); free_run(&b); free_run(&cres);
  return 0;
}

static int case_window_eviction(void)
{
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_QQ, 0);
  soxr_t p;
  soxr_error_t e;
  soxr_peak_info_t info;
  float * in = calloc(80, sizeof(float));
  float * out = calloc(96, sizeof(float));
  size_t idone, odone;
  unsigned i;
  in[6] = .9f;                        /* Unique early pulse. */
  for (i = 20; i < 80; ++i) in[i] = .2f;  /* Quiet plateau. */

  p = soxr_create(48000, 48000, 1, &e, &io, &q, 0);
  TASK_CHECK(p && !e);
  TASK_CHECK(!soxr_set_peak_window(p, 8));
  TASK_CHECK(soxr_peak_query(p, 0, 0));          /* NULL info is an error. */
  {
    size_t total = 0;
    e = soxr_process(p, in, 80, &idone, out, 80, &odone);
    TASK_CHECK(!e && idone == 80);
    total += odone;
    while (total < 80) {
      e = soxr_process(p, 0, 0, 0, out + total, 80 - total, &odone);
      TASK_CHECK(!e && odone);
      total += odone;
    }
    TASK_CHECK(total == 80);
  }
  e = soxr_process(p, 0, 0, 0, out, 0, &odone);
  TASK_CHECK(!e && odone == 0);
  TASK_CHECK(!soxr_peak_query_all(p, &info));
  TASK_CHECK(info.valid && info.observed == 80);
  TASK_CHECK(info.window_start == 72);
  TASK_CHECK(info.frame >= 72);               /* Pulse at 6 long forgotten. */
  TASK_CHECK(info.magnitude < .5);            /* Well below the evicted .9. */
  {
    double * ref = malloc(80 * sizeof(double));
    double * refs[1];
    struct ref_peak r;
    unsigned k;
    for (k = 0; k < 80; ++k) ref[k] = out[k];
    refs[0] = ref;
    ref_peak_window((double const * const *)refs, 80, 1, 80, 8, 0, &r);
    TASK_CHECK(r.valid && r.frame == info.frame && r.phase == info.phase);
    TASK_CHECK(fabs(r.magnitude - info.magnitude) < 1e-11);
    free(ref);
  }
  soxr_delete(p);
  free(in); free(out);
  return 0;
}

static int case_short_stream_and_tail_pulse(void)
{
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_QQ, 0);
  soxr_t p;
  soxr_error_t e;
  soxr_peak_info_t info;
  float in[10], out[32];
  double ref[10], * refs[1];
  struct ref_peak r;
  size_t idone = 0, odone = 0, total = 0;
  unsigned i, drains = 0;
  for (i = 0; i < 9; ++i) in[i] = .1f;
  in[9] = .8f;                          /* Pulse on the final frame. */

  p = soxr_create(48000, 48000, 1, &e, &io, &q, 0);
  TASK_CHECK(p && !e);
  TASK_CHECK(!soxr_set_peak_window(p, 64));
  e = soxr_process(p, in, 10, &idone, out, 10, &odone);
  TASK_CHECK(!e && idone == 10);
  total += odone;
  TASK_CHECK(!soxr_peak_query_all(p, &info));
  TASK_CHECK(info.delivered == total && info.observed == 0 && !info.valid);

  /* Drain through repeated, small/zero-capacity calls until quiescent. */
  do {
    e = soxr_process(p, 0, 0, 0, out, 4, &odone);
    TASK_CHECK(!e);
    total += odone;
    ++drains;
  } while (odone && drains < 10);
  e = soxr_process(p, 0, 0, 0, out, 0, &odone);
  TASK_CHECK(!e && odone == 0);
  TASK_CHECK(total == 10);
  TASK_CHECK(!soxr_peak_query_all(p, &info));
  TASK_CHECK(info.valid && info.observed == 10);  /* All units completed. */
  TASK_CHECK(info.frame < 10);                     /* Nothing past the end. */

  for (i = 0; i < 10; ++i) ref[i] = out[i];
  refs[0] = ref;
  ref_peak_window((double const * const *)refs, 10, 1, 10, 64, 0, &r);
  TASK_CHECK(r.valid && r.frame == info.frame);
  TASK_CHECK(r.phase == info.phase);
  TASK_CHECK(fabs(r.magnitude - info.magnitude) < 1e-11);
  TASK_CHECK(info.frame >= 8);                     /* Tail pulse dominates. */
  soxr_delete(p);
  return 0;
}

static int case_starvation_repeat_finish(void)
{
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_QQ, 0);
  soxr_t p;
  soxr_error_t e;
  soxr_peak_info_t before, after;
  float in[64], out[64];
  size_t idone = 0, odone = 0;
  unsigned i;
  for (i = 0; i < 64; ++i) in[i] = signal_sample(0, i);

  p = soxr_create(48000, 48000, 1, &e, &io, &q, 0);
  TASK_CHECK(p && !e);
  TASK_CHECK(!soxr_set_peak_window(p, 16));
  e = soxr_process(p, in, 20, &idone, out, 20, &odone);
  TASK_CHECK(!e);
  TASK_CHECK(!soxr_peak_query_all(p, &before));

  /* Starvation: zero-length, non-NULL input and zero output capacity. */
  for (i = 0; i < 3; ++i) {
    e = soxr_process(p, in, 0, &idone, out, 0, &odone);
    TASK_CHECK(!e && idone == 0 && odone == 0);
  }
  TASK_CHECK(!soxr_peak_query_all(p, &after));
  TASK_CHECK(after.delivered == before.delivered);
  TASK_CHECK(after.observed == before.observed);
  TASK_CHECK(after.frame == before.frame);

  /* Feed more; the stream continues coherently. */
  e = soxr_process(p, in + 20, 44, &idone, out, 64, &odone);
  TASK_CHECK(!e && idone == 44);
  TASK_CHECK(!soxr_peak_query_all(p, &after));
  TASK_CHECK(after.delivered == 64);

  /* Repeated draining never advances past the true end. */
  e = soxr_process(p, 0, 0, 0, out, 64, &odone);
  TASK_CHECK(!e && odone == 0);
  TASK_CHECK(!soxr_peak_query_all(p, &before));
  TASK_CHECK(before.observed == 64);
  for (i = 0; i < 3; ++i) {
    e = soxr_process(p, 0, 0, 0, out, 64, &odone);
    TASK_CHECK(!e && odone == 0);
    TASK_CHECK(!soxr_peak_query_all(p, &after));
    TASK_CHECK(after.delivered == before.delivered);
    TASK_CHECK(after.observed == before.observed);
    TASK_CHECK(after.frame == before.frame);
    TASK_CHECK(after.phase == before.phase);
    TASK_CHECK(after.magnitude == before.magnitude);
  }
  soxr_delete(p);
  return 0;
}

/* ----------------------------- lifecycle group --------------------------- */

static int case_config_rejection(void)
{
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_QQ, 0);
  soxr_quality_spec_t qvr = soxr_quality_spec(SOXR_QQ, SOXR_VR);
  soxr_t p, pv;
  soxr_error_t e;
  soxr_peak_info_t info;
  float in[40], out[64];
  size_t idone = 0, odone = 0;
  unsigned i;
  for (i = 0; i < 40; ++i) in[i] = signal_sample(0, i);

  TASK_CHECK(soxr_set_peak_window(0, 8));
  p = soxr_create(48000, 48000, 1, &e, &io, &q, 0);
  TASK_CHECK(p && !e);
  TASK_CHECK(soxr_set_peak_window(p, 0));
  TASK_CHECK(soxr_set_peak_window(p, SOXR_PEAK_WINDOW_MAX + 1));
  TASK_CHECK(soxr_set_peak_window(p, 100000));
  TASK_CHECK(soxr_peak_query(p, 0, &info));       /* Not configured. */
  TASK_CHECK(!soxr_error(p));                     /* Scene preserved. */

  /* Inclusive boundary windows are accepted while still idle. */
  {
    soxr_t edge = soxr_create(48000, 48000, 1, &e, &io, &q, 0);
    TASK_CHECK(edge && !e);
    TASK_CHECK(!soxr_set_peak_window(edge, SOXR_PEAK_WINDOW_MIN));
    TASK_CHECK(!soxr_set_peak_window(edge, SOXR_PEAK_WINDOW_MAX));
    TASK_CHECK(!soxr_peak_query_all(edge, &info));
    TASK_CHECK(!info.valid && info.delivered == 0);
    soxr_delete(edge);
  }

  TASK_CHECK(!soxr_set_peak_window(p, 16));
  {
    size_t total = 0;
    e = soxr_process(p, in, 40, &idone, out, 40, &odone);
    TASK_CHECK(!e && idone == 40);
    total += odone;
    while (total < 40) {
      e = soxr_process(p, 0, 0, 0, out + total, 40 - total, &odone);
      TASK_CHECK(!e && odone);
      total += odone;
    }
    TASK_CHECK(total == 40);
  }
  TASK_CHECK(soxr_set_peak_window(p, 32));        /* Reconfigure mid-stream. */
  TASK_CHECK(!soxr_peak_query_all(p, &info));
  TASK_CHECK(info.delivered == 40);               /* Old window still live. */
  TASK_CHECK(soxr_set_peak_window(p, 0));
  TASK_CHECK(soxr_set_peak_window(p, 8193));
  TASK_CHECK(!soxr_error(p));

  /* Variable rate is rejected but the resampler itself stays usable. */
  pv = soxr_create(48000, 48000, 1, &e, &io, &qvr, 0);
  TASK_CHECK(pv && !e);
  TASK_CHECK(soxr_set_peak_window(pv, 16));
  TASK_CHECK(!soxr_error(pv));
  e = soxr_process(pv, in, 40, &idone, out, 64, &odone);
  TASK_CHECK(!e && idone == 40);
  TASK_CHECK(soxr_peak_query_all(pv, &info));     /* Never enabled. */
  soxr_delete(pv);

  /* Bad channel arguments. */
  TASK_CHECK(soxr_peak_query(p, 5, &info));
  TASK_CHECK(soxr_peak_query(p, 1, 0));
  soxr_delete(p);
  return 0;
}

static int case_clear_reuse(void)
{
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_QQ, 0);
  soxr_t p;
  soxr_error_t e;
  soxr_peak_info_t first, second;
  float * in = build_interleaved(1, TEST_N);
  float * out1 = malloc(TEST_N * sizeof(float));
  float * out2 = malloc(TEST_N * sizeof(float));

  p = soxr_create(48000, 48000, 1, &e, &io, &q, 0);
  TASK_CHECK(p && !e);
  TASK_CHECK(!soxr_set_peak_window(p, TEST_W));

  TASK_CHECK(!run_unity(p, in, TEST_N, out1));
  TASK_CHECK(!soxr_peak_query_all(p, &first));
  TASK_CHECK(first.observed == TEST_N);

  TASK_CHECK(!soxr_clear(p));
  TASK_CHECK(!soxr_peak_query_all(p, &second));   /* Config retained. */
  TASK_CHECK(!second.valid && second.delivered == 0 && second.observed == 0);
  TASK_CHECK(!soxr_set_peak_window(p, 32));       /* Reconfigure allowed. */

  TASK_CHECK(!run_unity(p, in, TEST_N, out2));
  TASK_CHECK(!memcmp(out1, out2, TEST_N * sizeof(float)));
  TASK_CHECK(!soxr_peak_query_all(p, &second));
  TASK_CHECK(second.observed == TEST_N);
  TASK_CHECK(second.window_start == TEST_N - 32);
  soxr_delete(p);
  free(in); free(out1); free(out2);
  return 0;
}

static int case_legacy_pcm_compatibility(void)
{
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, 0);
  soxr_t plain, watched;
  soxr_error_t e;
  soxr_peak_info_t info;
  float * in = build_interleaved(2, 200);
  float * o1 = calloc(256, 2 * sizeof(float));
  float * o2 = calloc(256, 2 * sizeof(float));
  size_t pos, id1, od1, id2, od2;
  double delay1, delay2;

  plain = soxr_create(48000, 44100, 2, &e, &io, &q, 0);
  TASK_CHECK(plain && !e);
  watched = soxr_create(48000, 44100, 2, &e, &io, &q, 0);
  TASK_CHECK(watched && !e);
  TASK_CHECK(!soxr_set_peak_window(watched, 32));

  pos = 0;
  for (;;) {
    size_t chunk = 200 - pos;
    if (chunk > 13) chunk = 13;
    e = soxr_process(plain, chunk? in + pos * 2 : 0, chunk, &id1,
        o1, 64, &od1);
    TASK_CHECK(!e);
    e = soxr_process(watched, chunk? in + pos * 2 : 0, chunk, &id2,
        o2, 64, &od2);
    TASK_CHECK(!e);
    TASK_CHECK(id1 == id2 && od1 == od2);
    delay1 = soxr_delay(plain);
    delay2 = soxr_delay(watched);
    TASK_CHECK(delay1 == delay2);
    pos += id1;
    if (!chunk && !od1) break;
  }
  TASK_CHECK(!memcmp(o1, o2, 256 * 2 * sizeof(float)));
  TASK_CHECK(*soxr_num_clips(plain) == *soxr_num_clips(watched));
  TASK_CHECK(!soxr_peak_query_all(watched, &info));
  TASK_CHECK(info.valid);

  /* Integer clip counters match with dither disabled. */
  {
    soxr_io_spec_t ioi = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    soxr_quality_spec_t qi = soxr_quality_spec(SOXR_QQ, SOXR_NO_DITHER);
    soxr_t a = soxr_create(48000, 48000, 1, &e, &ioi, &qi, 0);
    soxr_t b = soxr_create(48000, 48000, 1, &e, &ioi, &qi, 0);
    int16_t * z1 = calloc(200, sizeof(int16_t));
    int16_t * z2 = calloc(200, sizeof(int16_t));
    unsigned i;
    TASK_CHECK(a && b);
    TASK_CHECK(!soxr_set_peak_window(b, 16));
    for (i = 0; i < 200; ++i) in[i * 2] = 1.3f * signal_sample(0, i);
    {
      size_t ta = 0, tb = 0;
      TASK_CHECK(!soxr_process(a, in, 200, &id1, z1, 200, &od1));
      TASK_CHECK(!soxr_process(b, in, 200, &id2, z2, 200, &od2));
      ta += od1; tb += od2;
      while (ta < 200) {
        TASK_CHECK(!soxr_process(a, 0, 0, 0, z1 + ta, 200 - ta, &od1));
        TASK_CHECK(od1); ta += od1;
      }
      while (tb < 200) {
        TASK_CHECK(!soxr_process(b, 0, 0, 0, z2 + tb, 200 - tb, &od2));
        TASK_CHECK(od2); tb += od2;
      }
      od1 = ta; od2 = tb;
    }
    TASK_CHECK(od1 == od2 && od1 == 200);
    /* Separate engine instances already differ at rounding boundaries even
     * with no observer attached (a control run of two plain instances shows
     * the same spread, up to 2 LSB); the observer must not add anything
     * beyond that natural variation.  The float path is bit-identical. */
    {
      unsigned k, lsb_diffs = 0;
      for (k = 0; k < 200; ++k) {
        int d = (int)z1[k] - (int)z2[k];
        if (d < 0) d = -d;
        TASK_CHECK(d <= 2);
        if (d) ++lsb_diffs;
      }
      TASK_CHECK(lsb_diffs < 200);      /* The streams are still the same. */
    }
    TASK_CHECK(*soxr_num_clips(a) == *soxr_num_clips(b));
    TASK_CHECK(*soxr_num_clips(b) > 0);
    TASK_CHECK(!soxr_peak_query_all(b, &info));
    TASK_CHECK(info.magnitude > 1.);
    soxr_delete(a); soxr_delete(b);
    free(z1); free(z2);
  }

  /* oneshot remains available and behaves as before. */
  {
    size_t oid = 0, ood = 0;
    e = soxr_oneshot(48000, 44100, 2, in, 200, &oid, o1, 256, &ood,
        &io, &q, 0);
    TASK_CHECK(!e && oid == 200 && ood > 170 && ood < 200);
  }
  soxr_delete(plain);
  soxr_delete(watched);
  free(in); free(o1); free(o2);
  return 0;
}

static int case_formats_and_channels(void)
{
  soxr_datatype_t types[4] = {
    SOXR_FLOAT32_I, SOXR_FLOAT64_I, SOXR_INT16_I, SOXR_INT32_I};
  unsigned channels[3] = {1, 2, 6};
  unsigned ti, ci;
  for (ci = 0; ci < 3; ++ci)
    for (ti = 0; ti < 4; ++ti) {
      soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, types[ti]);
      soxr_quality_spec_t q = soxr_quality_spec(
          ti == 1? SOXR_VHQ : SOXR_QQ, SOXR_NO_DITHER);
      soxr_t p;
      soxr_error_t e;
      soxr_peak_info_t info, per;
      unsigned nch = channels[ci], n = 96, c;
      float * in = build_interleaved(nch, n);
      size_t bytes = soxr_datatype_size(types[ti]);
      void * out = malloc((size_t)n * nch * bytes);
      size_t idone = 0, odone = 0, total = 0;
      p = soxr_create(48000, 48000, nch, &e, &io, &q, 0);
      TASK_CHECK(p && !e);
      TASK_CHECK(!soxr_set_peak_window(p, 24));
      e = soxr_process(p, in, n, &idone, out, n, &odone);
      TASK_CHECK(!e && idone == n);
      total += odone;
      while (total < n) {
        e = soxr_process(p, 0, 0, 0,
            (char *)out + total * nch * bytes, n - total, &odone);
        TASK_CHECK(!e && odone);
        total += odone;
      }
      TASK_CHECK(total == n);
      e = soxr_process(p, 0, 0, 0, out, 0, &odone);
      TASK_CHECK(!e && odone == 0);
      TASK_CHECK(!soxr_peak_query_all(p, &info));
      TASK_CHECK(info.valid && info.observed == n);
      TASK_CHECK(info.channel < nch);
      TASK_CHECK(info.magnitude > .5);
      for (c = 0; c < nch; ++c) {
        TASK_CHECK(!soxr_peak_query(p, c, &per));
        TASK_CHECK(per.valid && per.channel == c);
        TASK_CHECK(per.magnitude <= info.magnitude + 1e-12);
      }
      soxr_delete(p);
      free(in); free(out);
    }
  return 0;
}

/* ------------------------------- Registration ---------------------------- */

struct task_case const task_cases[] = {
  {"interpolation", "module_phase_formula", case_module_phase_formula},
  {"interpolation", "overshoot_and_phase_values",
      case_overshoot_and_phase_values},
  {"interpolation", "tie_and_eviction_rules", case_tie_and_eviction_rules},
  {"interpolation", "integer_output_normalised",
      case_integer_output_normalised},
  {"streaming", "chunk_snapshots", case_chunk_snapshots},
  {"streaming", "split_interleaved_callback",
      case_split_interleaved_callback},
  {"streaming", "window_eviction", case_window_eviction},
  {"streaming", "short_stream_and_tail_pulse",
      case_short_stream_and_tail_pulse},
  {"streaming", "starvation_repeat_finish",
      case_starvation_repeat_finish},
  {"lifecycle", "config_rejection", case_config_rejection},
  {"lifecycle", "clear_reuse", case_clear_reuse},
  {"lifecycle", "legacy_pcm_compatibility", case_legacy_pcm_compatibility},
  {"lifecycle", "formats_and_channels", case_formats_and_channels},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
