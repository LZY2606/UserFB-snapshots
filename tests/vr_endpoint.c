/* Finite-fragment endpoint handling for the variable-rate (SOXR_VR) engine.
 *
 * Logical-time rule under test (independent of the library internals):
 *   - initial input/output ratio r; the source-time centre of the next output
 *     frame is r/2;
 *   - after each frame, accumulate that frame's ratio; with accumulated sum S,
 *     the centre of the next frame using ratio r_k is S + r_k/2;
 *   - a slew of length L from r0 to rt uses r0 + (rt-r0)*k/L, k = 0..L-1,
 *     then holds rt; an L=0 change applies immediately;
 *   - for N real input frames, only frames whose centre is <= N are delivered.
 *
 * This file recomputes those centres itself and checks length, slewing, the
 * input-callback endpoint, chunk/capacity independence, tail impulses,
 * multi-channel sound and that soxr_delay() never alters the PCM.
 */

#include "soxr.h"
#include "../task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATIO_MIN (1. / 8)
#define RATIO_MAX 8.

/* ------------------------------------------------------------------ helpers */

static soxr_t make_vr(unsigned ch, int split)
{
  soxr_error_t e = 0;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_VR);
  soxr_io_spec_t io = soxr_io_spec(
      split ? SOXR_FLOAT32_S : SOXR_FLOAT32_I,
      split ? SOXR_FLOAT32_S : SOXR_FLOAT32_I);
  soxr_runtime_spec_t rts = soxr_runtime_spec(1);
  soxr_t s = soxr_create(RATIO_MAX, 1, ch, &e, &io, &q, &rts);
  if (!s || e)
    return 0;
  return s;
}

static float sample_at(size_t i, int tone)
{
  return tone ? (float)(.31 * sin(i * .0713) + .17 * cos(i * .0377))
              : (float)(.25 * sin(i * .131));
}

/* Independent constant-ratio logical length: count centres <= N. */
static size_t logical_constant(double r, size_t N)
{
  double S = 0;
  size_t k = 0;
  for (;;) {
    if (S + r * .5 > (double)N)
      return k;
    S += r;
    ++k;
  }
}

/* A ratio-change event scheduled at output frame index `frame'. */
typedef struct { size_t frame; double target; size_t len; } ratio_event_t;

/* Build the per-frame ratio sequence from an event list, then count how many
 * frames have source-time centre <= N.  This mirrors the specification, not
 * the engine's bookkeeping. */
static size_t logical_events(double r0, size_t N,
    ratio_event_t const * ev, size_t nev, size_t cap)
{
  double * ratio = malloc(cap * sizeof(*ratio));
  double S = 0;
  size_t k, count = 0;
  TASK_CHECK(ratio != 0);
  for (k = 0; k < cap; ++k)
    ratio[k] = r0;
  for (size_t e = 0; e < nev; ++e) {
    double start = ratio[ev[e].frame];
    if (ev[e].len == 0) {
      for (k = ev[e].frame; k < cap; ++k)
        ratio[k] = ev[e].target;
    } else {
      for (size_t j = 0; j < ev[e].len && ev[e].frame + j < cap; ++j)
        ratio[ev[e].frame + j] = start +
            (ev[e].target - start) * (double)j / (double)ev[e].len;
      for (k = ev[e].frame + ev[e].len; k < cap; ++k)
        ratio[k] = ev[e].target;
    }
  }
  for (k = 0; k < cap; ++k) {
    if (S + ratio[k] * .5 > (double)N)
      break;
    S += ratio[k];
    ++count;
  }
  free(ratio);
  return count;
}

/* Drive one interleaved, mono resampler to completion; returns frames out. */
static size_t drive_mono(soxr_t s, float const * in, size_t N,
    size_t ichunk, size_t ocap, float * out, size_t outcap,
    ratio_event_t const * ev, size_t nev)
{
  size_t fed = 0, made = 0, got, used, ei = 0;
  while (fed < N) {
    size_t n = N - fed;
    if (ichunk && n > ichunk)
      n = ichunk;
    while (ei < nev && ev[ei].frame <= made) {
      TASK_CHECK(soxr_set_io_ratio(s, ev[ei].target, ev[ei].len) == 0);
      ++ei;
    }
    TASK_CHECK(soxr_process(s, (void *)(in + fed), n, &used,
        out + made, ocap < outcap - made ? ocap : outcap - made, &got) == 0);
    fed += used;
    made += got;
  }
  for (;;) {
    TASK_CHECK(soxr_process(s, 0, 0, 0, out + made, outcap - made, &got) == 0);
    made += got;
    if (!got)
      break;
  }
  return made;
}

static void fill_input(float * in, size_t N, int tone, int tail_impulse)
{
  for (size_t i = 0; i < N; ++i)
    in[i] = sample_at(i, tone);
  if (tail_impulse && N)
    in[N - 1] += 1.0f; /* impulse at the real input endpoint */
}

/* ------------------------------------------------------------- length group */

static int length_fresh_and_clear(void)
{
  soxr_t s = make_vr(1, 0);
  TASK_CHECK(s != 0);
  TASK_CHECK(soxr_delay(s) == 0);            /* freshly created */
  float in[64], out[256];
  size_t got;
  fill_input(in, 64, 1, 0);
  TASK_CHECK(soxr_process(s, in, 64, 0, out, 256, &got) == 0);
  TASK_CHECK(soxr_process(s, 0, 0, 0, out, 256, &got) == 0);
  do TASK_CHECK(soxr_process(s, 0, 0, 0, out, 256, &got) == 0);
  while (got);
  TASK_CHECK(soxr_delay(s) == 0);            /* fully drained */

  TASK_CHECK(soxr_clear(s) == 0);
  TASK_CHECK(soxr_delay(s) == 0);            /* after clear */
  /* reusable after clear */
  TASK_CHECK(soxr_process(s, in, 64, 0, out, 256, &got) == 0);
  do TASK_CHECK(soxr_process(s, 0, 0, 0, out, 256, &got) == 0);
  while (got);
  TASK_CHECK(soxr_delay(s) == 0);
  soxr_delete(s);
  return 0;
}

static int length_empty_fragment(void)
{
  double rs[] = {RATIO_MIN, .5, 1., 2., RATIO_MAX};
  for (unsigned i = 0; i < sizeof(rs) / sizeof(rs[0]); ++i) {
    soxr_t s = make_vr(1, 0);
    TASK_CHECK(s != 0);
    float out[64];
    size_t got;
    TASK_CHECK(soxr_set_io_ratio(s, rs[i], 0) == 0);
    TASK_CHECK(soxr_delay(s) == 0);
    TASK_CHECK(soxr_process(s, 0, 0, 0, out, 64, &got) == 0); /* immediate EOF */
    TASK_CHECK(got == 0);                                     /* empty: no out */
    TASK_CHECK(soxr_delay(s) == 0);
    soxr_delete(s);
  }
  return 0;
}

static int length_constant_rule(void)
{
  double down[] = {RATIO_MIN, .25, .5, .75, 1.};  /* exact physical length */
  double up[] = {1.5, 2., 3., 4., RATIO_MAX};     /* at most one frame short */
  size_t Ns[] = {2, 5, 17, 64, 200, 500};
  for (unsigned t = 0; t < 2; ++t) {
    double const * rs = t ? up : down;
    unsigned nr = t ? sizeof(up) / sizeof(up[0]) : sizeof(down) / sizeof(down[0]);
    for (unsigned a = 0; a < nr; ++a) for (unsigned b = 0; b < sizeof(Ns)/sizeof(Ns[0]); ++b) {
      size_t N = Ns[b], cap = N * 10 + 64;
      float * in = malloc(N * sizeof(float));
      float * out = malloc(cap * sizeof(float));
      soxr_t s = make_vr(1, 0);
      TASK_CHECK(s != 0);
      size_t model, made;
      TASK_CHECK(in && out);
      TASK_CHECK(soxr_set_io_ratio(s, rs[a], 0) == 0);
      fill_input(in, N, (a + b) & 1, 0);
      model = logical_constant(rs[a], N);
      made = drive_mono(s, in, N, 37, 53, out, cap, 0, 0);
      TASK_CHECK(made <= model);                 /* never past the endpoint */
      if (!t)
        TASK_CHECK(made == model);               /* down/unity ratios: exact */
      else
        TASK_CHECK(made + 1 >= model);           /* up ratios: <=1 short */
      TASK_CHECK(soxr_delay(s) == 0);
      soxr_delete(s);
      free(in);
      free(out);
    }
  }
  return 0;
}

static int length_chunk_invariance(void)
{
  double r = .75;
  size_t N = 233, cap = N * 4 + 64;
  size_t chunks[] = {1, 3, 19, 71, N};
  size_t caps[] = {1, 7, 64, 4096};
  float * in = malloc(N * sizeof(float));
  float ref[4096];
  size_t reflen = 0;
  TASK_CHECK(in);
  fill_input(in, N, 1, 1); /* tail impulse exercises the filter flush tail */
  for (unsigned ci = 0; ci < sizeof(chunks)/sizeof(chunks[0]); ++ci)
    for (unsigned k = 0; k < sizeof(caps)/sizeof(caps[0]); ++k) {
      soxr_t s = make_vr(1, 0);
      TASK_CHECK(s != 0);
      float * out = malloc(cap * sizeof(float));
      size_t made;
      TASK_CHECK(soxr_set_io_ratio(s, r, 0) == 0);
      made = drive_mono(s, in, N, chunks[ci], caps[k], out, cap, 0, 0);
      if (!(ci | k)) {
        reflen = made;
        memcpy(ref, out, made * sizeof(float));
      } else {
        TASK_CHECK(made == reflen);
        TASK_CHECK(memcmp(ref, out, made * sizeof(float)) == 0);
      }
      soxr_delete(s);
      free(out);
    }
  TASK_CHECK(reflen == logical_constant(r, N));
  free(in);
  return 0;
}

static int length_tail_impulse(void)
{
  /* Energy from an impulse at the last input frame must survive the tail:
   * the later output samples cannot all be zero, and the length is intact. */
  double rs[] = {.5, .75, 1.};
  size_t Ns[] = {64, 150};
  for (unsigned a = 0; a < 3; ++a) for (unsigned b = 0; b < 2; ++b) {
    size_t N = Ns[b], cap = N * 4 + 64;
    float * in = calloc(N, sizeof(float));
    float * out = calloc(cap, sizeof(float));
    soxr_t s = make_vr(1, 0);
    TASK_CHECK(s != 0);
    size_t made, nonzero = 0;
    TASK_CHECK(in && out);
    in[N - 1] = 1.0f;
    TASK_CHECK(soxr_set_io_ratio(s, rs[a], 0) == 0);
    made = drive_mono(s, in, N, 11, 17, out, cap, 0, 0);
    TASK_CHECK(made == logical_constant(rs[a], N));
    for (size_t i = made / 2; i < made; ++i)
      nonzero += out[i] != 0.0f;
    TASK_CHECK(nonzero > 0);                 /* normal tail sound, not trimmed */
    soxr_delete(s);
    free(in);
    free(out);
  }
  return 0;
}

static int length_query_does_not_change_pcm(void)
{
  double rs[] = {.5, .75, 1., 2.};
  size_t Ns[] = {50, 173};
  for (unsigned a = 0; a < 4; ++a) for (unsigned b = 0; b < 2; ++b) {
    size_t N = Ns[b], cap = N * 9 + 64;
    float * in = malloc(N * sizeof(float));
    float * oq = malloc(cap * sizeof(float));
    float * on = malloc(cap * sizeof(float));
    soxr_t sq = make_vr(1, 0), sn = make_vr(1, 0);
    TASK_CHECK(sq != 0 && sn != 0);
    size_t mq = 0, mn = 0, got, fed = 0;
    double predicted = 0;
    TASK_CHECK(in && oq && on);
    fill_input(in, N, 1, 0);
    TASK_CHECK(soxr_set_io_ratio(sq, rs[a], 0) == 0);
    TASK_CHECK(soxr_set_io_ratio(sn, rs[a], 0) == 0);
    while (fed < N) {
      size_t n = N - fed; if (n > 23) n = 23;
      TASK_CHECK(soxr_process(sq, in + fed, n, 0, oq + mq, 31, &got) == 0); mq += got;
      TASK_CHECK(soxr_process(sn, in + fed, n, 0, on + mn, 31, &got) == 0); mn += got;
      fed += n;
    }
    /* many queries on one resampler only; must be a non-negative integer and
     * must not consume sound or move the ratio gradient. */
    for (int q = 0; q < 8; ++q)
      predicted = soxr_delay(sq);
    TASK_CHECK(predicted >= 0 && predicted == floor(predicted));
    for (;;) {
      size_t gq, gn;
      TASK_CHECK(soxr_process(sq, 0, 0, 0, oq + mq, cap - mq, &gq) == 0);
      TASK_CHECK(soxr_process(sn, 0, 0, 0, on + mn, cap - mn, &gn) == 0);
      mq += gq; mn += gn;
      if (!gq && !gn) break;
    }
    TASK_CHECK(mq == mn);
    TASK_CHECK(memcmp(oq, on, mq * sizeof(float)) == 0);
    TASK_CHECK(soxr_delay(sq) == 0 && soxr_delay(sn) == 0);
    soxr_delete(sq);
    soxr_delete(sn);
    free(in); free(oq); free(on);
  }
  return 0;
}

/* --------------------------------------------------------------- slew group */

static int slew_schedule_length(void)
{
  struct { double r0, rt; size_t L, N; } exact_cases[] = {
    {1., 2., 100, 300}, {.75, 3., 120, 400}, {.5, 8., 200, 600},
    {RATIO_MIN, RATIO_MAX, 300, 500}, {RATIO_MAX, RATIO_MIN, 300, 500},
  };
  for (unsigned i = 0; i < sizeof(exact_cases)/sizeof(exact_cases[0]); ++i) {
    size_t N = exact_cases[i].N, cap = N * 10 + 128;
    ratio_event_t ev = {0, exact_cases[i].rt, exact_cases[i].L};
    float * in = malloc(N * sizeof(float));
    float * out = malloc(cap * sizeof(float));
    soxr_t s = make_vr(1, 0);
    TASK_CHECK(s != 0);
    size_t model, made;
    TASK_CHECK(in && out);
    fill_input(in, N, 1, 0);
    TASK_CHECK(soxr_set_io_ratio(s, exact_cases[i].r0, 0) == 0);
    model = logical_events(exact_cases[i].r0, N, &ev, 1, cap);
    made = drive_mono(s, in, N, 29, 41, out, cap, &ev, 1);
    TASK_CHECK(made == model);
    TASK_CHECK(soxr_delay(s) == 0);
    soxr_delete(s);
    free(in); free(out);
  }
  return 0;
}

static int slew_never_overshoots(void)
{
  struct { double r0, rt; size_t L, N; } cases[] = {
    {2., .5, 80, 300}, {3., .75, 60, 400}, {2., 2., 50, 257},
    {1., 1.5, 1, 200}, {1.5, 1., 0, 200},
  };
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
    size_t N = cases[i].N, cap = N * 10 + 128;
    ratio_event_t ev = {0, cases[i].rt, cases[i].L};
    float * in = malloc(N * sizeof(float));
    float * out = malloc(cap * sizeof(float));
    soxr_t s = make_vr(1, 0);
    TASK_CHECK(s != 0);
    size_t model, made;
    TASK_CHECK(in && out);
    fill_input(in, N, 0, 1);
    TASK_CHECK(soxr_set_io_ratio(s, cases[i].r0, 0) == 0);
    model = logical_events(cases[i].r0, N, &ev, 1, cap);
    made = drive_mono(s, in, N, 13, 23, out, cap, &ev, 1);
    TASK_CHECK(made <= model);
    TASK_CHECK(made + 2 >= model);      /* filter physics: only a frame or two */
    TASK_CHECK(soxr_delay(s) == 0);
    soxr_delete(s);
    free(in); free(out);
  }
  return 0;
}

static int slew_chunk_invariance(void)
{
  double r0 = 1., rt = 2.;
  size_t L = 100, N = 300, cap = N * 4 + 128;
  size_t chunks[] = {1, 17, 53, N};
  size_t caps[] = {1, 11, 97};
  ratio_event_t ev = {0, rt, L};
  float * in = malloc(N * sizeof(float));
  float ref[2048];
  size_t reflen = 0;
  TASK_CHECK(in);
  fill_input(in, N, 1, 0);
  for (unsigned ci = 0; ci < 4; ++ci) for (unsigned k = 0; k < 3; ++k) {
    soxr_t s = make_vr(1, 0);
    TASK_CHECK(s != 0);
    float * out = malloc(cap * sizeof(float));
    size_t made;
    TASK_CHECK(soxr_set_io_ratio(s, r0, 0) == 0);
    made = drive_mono(s, in, N, chunks[ci], caps[k], out, cap, &ev, 1);
    if (!(ci | k)) {
      reflen = made;
      memcpy(ref, out, made * sizeof(float));
    } else {
      TASK_CHECK(made == reflen);
      TASK_CHECK(memcmp(ref, out, made * sizeof(float)) == 0);
    }
    soxr_delete(s);
    free(out);
  }
  free(in);
  return 0;
}

static int slew_mid_reset_and_instant(void)
{
  size_t N = 400, cap = N * 10 + 128;
  ratio_event_t ev1[2] = {{0, 2., 100}, {40, .5, 60}};
  ratio_event_t ev2[2] = {{0, 2., 100}, {30, 1., 0}};
  float * in = malloc(N * sizeof(float));
  TASK_CHECK(in);
  fill_input(in, N, 1, 0);
  for (int variant = 0; variant < 2; ++variant) {
    ratio_event_t const * ev = variant ? ev2 : ev1;
    size_t model = logical_events(1., N, ev, 2, cap);
    float ref[4096];
    size_t reflen = 0;
    /* A mid-stream ratio change takes effect between process calls, so drive
     * both runs with identical call geometry: this checks the continuation
     * rule's endpoint and determinism (not cross-chunk invariance, which the
     * frame-0 slew and constant-ratio cases cover). */
    for (int rep = 0; rep < 2; ++rep) {
      soxr_t s = make_vr(1, 0);
      TASK_CHECK(s != 0);
      float * out = malloc(cap * sizeof(float));
      size_t made;
      TASK_CHECK(soxr_set_io_ratio(s, 1., 0) == 0);
      made = drive_mono(s, in, N, 19, 37, out, cap, ev, 2);
      TASK_CHECK(made <= model);               /* continuation rule endpoint */
      if (!rep) {
        reflen = made;
        memcpy(ref, out, made * sizeof(float));
      } else {
        TASK_CHECK(made == reflen);
        TASK_CHECK(memcmp(ref, out, made * sizeof(float)) == 0);
      }
      TASK_CHECK(soxr_delay(s) == 0);
      soxr_delete(s);
      free(out);
    }
  }
  free(in);
  return 0;
}

static int slew_query_invariant(void)
{
  size_t N = 300, cap = N * 4 + 128;
  ratio_event_t ev = {0, 2., 100};
  float * in = malloc(N * sizeof(float));
  float * oq = malloc(cap * sizeof(float));
  float * on = malloc(cap * sizeof(float));
  soxr_t sq = make_vr(1, 0), sn = make_vr(1, 0);
  TASK_CHECK(sq != 0 && sn != 0);
  size_t fed = 0, got, mq = 0, mn = 0;
  TASK_CHECK(in && oq && on);
  fill_input(in, N, 0, 1);
  TASK_CHECK(soxr_set_io_ratio(sq, 1., 0) == 0);
  TASK_CHECK(soxr_set_io_ratio(sn, 1., 0) == 0);
  TASK_CHECK(soxr_set_io_ratio(sq, ev.target, ev.len) == 0);
  TASK_CHECK(soxr_set_io_ratio(sn, ev.target, ev.len) == 0);
  while (fed < N) {
    size_t n = N - fed; if (n > 23) n = 23;
    TASK_CHECK(soxr_process(sq, in + fed, n, 0, oq + mq, 43, &got) == 0); mq += got;
    TASK_CHECK(soxr_process(sn, in + fed, n, 0, on + mn, 43, &got) == 0); mn += got;
    (void)soxr_delay(sq); /* queries only on the first resampler */
    fed += n;
  }
  for (;;) {
    size_t gq, gn;
    TASK_CHECK(soxr_process(sq, 0, 0, 0, oq + mq, cap - mq, &gq) == 0);
    TASK_CHECK(soxr_process(sn, 0, 0, 0, on + mn, cap - mn, &gn) == 0);
    mq += gq; mn += gn;
    if (!gq && !gn) break;
  }
  TASK_CHECK(mq == mn);
  TASK_CHECK(memcmp(oq, on, mq * sizeof(float)) == 0);
  soxr_delete(sq); soxr_delete(sn);
  free(in); free(oq); free(on);
  return 0;
}

/* ----------------------------------------------------------- callback group */

typedef struct { float const * buf; size_t len, pos; } cb_ctx_t;

static size_t cb_input(void * state, soxr_cbuf_t * data, size_t requested)
{
  cb_ctx_t * c = state;
  size_t n = c->len - c->pos;
  (void)requested;
  *data = (soxr_cbuf_t)(c->buf + c->pos);
  if (n) {
    c->pos += n;
    return n;
  }
  return 0; /* EOF: data pointer valid, length zero */
}

static int callback_matches_process(void)
{
  double rs[] = {.5, .75, 1., 2.};
  size_t N = 210, cap = N * 9 + 64;
  float * in = malloc(N * sizeof(float));
  TASK_CHECK(in);
  fill_input(in, N, 1, 1);
  for (unsigned a = 0; a < 4; ++a) {
    size_t plen, clen = 0, got;
    float * po = malloc(cap * sizeof(float));
    float * co = malloc(cap * sizeof(float));
    soxr_t sp = make_vr(1, 0), sc = make_vr(1, 0);
    TASK_CHECK(sp != 0 && sc != 0);
    cb_ctx_t ctx = {in, N, 0};
    TASK_CHECK(po && co);
    TASK_CHECK(soxr_set_io_ratio(sp, rs[a], 0) == 0);
    TASK_CHECK(soxr_set_io_ratio(sc, rs[a], 0) == 0);
    /* process/NULL endpoint */
    TASK_CHECK(soxr_process(sp, in, N, 0, po, cap, &got) == 0);
    plen = got;
    for (;;) {
      TASK_CHECK(soxr_process(sp, 0, 0, 0, po + plen, cap - plen, &got) == 0);
      if (!got)
        break;
      plen += got;
    }
    /* callback EOF endpoint */
    TASK_CHECK(soxr_set_input_fn(sc, (soxr_input_fn_t)cb_input, &ctx, 47) == 0);
    for (;;) {
      got = soxr_output(sc, co + clen, 31);
      if (!got)
        break;
      clen += got;
    }
    TASK_CHECK(clen == plen);
    TASK_CHECK(memcmp(po, co, plen * sizeof(float)) == 0);
    TASK_CHECK(soxr_delay(sc) == 0);
    soxr_delete(sp); soxr_delete(sc);
    free(po); free(co);
  }
  free(in);
  return 0;
}

static int callback_query_invariant(void)
{
  size_t N = 180, cap = N * 9 + 64;
  float * in = malloc(N * sizeof(float));
  float * oq = malloc(cap * sizeof(float));
  float * on = malloc(cap * sizeof(float));
  cb_ctx_t cq = {in, N, 0}, cn = {in, N, 0};
  soxr_t sq = make_vr(1, 0), sn = make_vr(1, 0);
  TASK_CHECK(sq != 0 && sn != 0);
  size_t lq = 0, ln = 0;
  TASK_CHECK(in && oq && on);
  fill_input(in, N, 0, 0);
  TASK_CHECK(soxr_set_io_ratio(sq, .75, 0) == 0);
  TASK_CHECK(soxr_set_io_ratio(sn, .75, 0) == 0);
  TASK_CHECK(soxr_set_input_fn(sq, (soxr_input_fn_t)cb_input, &cq, 33) == 0);
  TASK_CHECK(soxr_set_input_fn(sn, (soxr_input_fn_t)cb_input, &cn, 33) == 0);
  for (;;) {
    size_t gq = soxr_output(sq, oq + lq, 29);
    size_t gn = soxr_output(sn, on + ln, 29);
    (void)soxr_delay(sq);
    lq += gq; ln += gn;
    if (!gq && !gn) break;
  }
  TASK_CHECK(lq == ln);
  TASK_CHECK(memcmp(oq, on, lq * sizeof(float)) == 0);
  TASK_CHECK(soxr_delay(sq) == 0 && soxr_delay(sn) == 0);
  soxr_delete(sq); soxr_delete(sn);
  free(in); free(oq); free(on);
  return 0;
}

static int callback_multichannel(void)
{
  unsigned chs[] = {1, 2, 6};
  size_t N = 120, cap = N * 4 + 64;
  size_t expect = logical_constant(2., N);
  for (unsigned ci = 0; ci < 3; ++ci) for (int split = 0; split <= 1; ++split) {
    unsigned ch = chs[ci];
    soxr_t s = make_vr(ch, split);
    TASK_CHECK(s != 0);
    TASK_CHECK(soxr_set_io_ratio(s, 2., 0) == 0);
    size_t made = 0, got;

    if (!split) {
      float * il = malloc(N * ch * sizeof(float));
      float * ol = calloc(cap * ch, sizeof(float));
      TASK_CHECK(il && ol);
      for (size_t i = 0; i < N; ++i)
        for (unsigned c = 0; c < ch; ++c)
          il[i * ch + c] = sample_at(i, 1);
      TASK_CHECK(soxr_process(s, il, N, 0, ol, cap, &got) == 0);
      made = got;
      for (;;) {
        TASK_CHECK(soxr_process(s, 0, 0, 0, ol + made * ch, cap - made, &got) == 0);
        if (!got)
          break;
        made += got;
      }
      TASK_CHECK(made <= expect && made + 1 >= expect);
      TASK_CHECK(soxr_delay(s) == 0);
      free(il); free(ol);
    } else {
      float ** sp = malloc(ch * sizeof(float*));
      float ** so = malloc(ch * sizeof(float*));
      float ** op = malloc(ch * sizeof(float*)); /* moving output cursors */
      TASK_CHECK(sp && so && op);
      for (unsigned c = 0; c < ch; ++c) {
        sp[c] = malloc(N * sizeof(float));
        so[c] = calloc(cap, sizeof(float));
        op[c] = so[c];
        for (size_t i = 0; i < N; ++i)
          sp[c][i] = sample_at(i, 1); /* identical per-channel content */
      }
      TASK_CHECK(soxr_process(s, sp, N, 0, op, cap, &got) == 0);
      made = got;
      for (unsigned c = 0; c < ch; ++c)
        op[c] = so[c] + made;
      for (;;) {
        TASK_CHECK(soxr_process(s, 0, 0, 0, op, cap - made, &got) == 0);
        if (!got)
          break;
        made += got;
        for (unsigned c = 0; c < ch; ++c)
          op[c] = so[c] + made;
      }
      TASK_CHECK(made <= expect && made + 1 >= expect);
      /* all channels share one progress, hence identical content */
      for (size_t i = 0; i < made; ++i)
        for (unsigned c = 1; c < ch; ++c)
          TASK_CHECK(so[c][i] == so[0][i]);
      TASK_CHECK(soxr_delay(s) == 0);
      for (unsigned c = 0; c < ch; ++c) { free(sp[c]); free(so[c]); }
      free(sp); free(so); free(op);
    }
    soxr_delete(s);
  }
  return 0;
}

/* ----------------------------------------------------------- common rules */

static int endpoint_rejections(void)
{
  size_t N = 120, cap = N * 4 + 64;
  float * in = malloc(N * sizeof(float));
  float extra[4] = {1, 2, 3, 4};
  float * out = malloc(cap * sizeof(float));
  size_t got;
  soxr_t s = make_vr(1, 0);
  TASK_CHECK(s != 0);
  TASK_CHECK(in && out);
  fill_input(in, N, 1, 0);
  TASK_CHECK(soxr_set_io_ratio(s, .75, 0) == 0);
  TASK_CHECK(soxr_process(s, in, N, 0, out, cap, &got) == 0);
  TASK_CHECK(soxr_process(s, 0, 0, 0, out, cap, &got) == 0); /* EOF */
  /* non-empty input after EOF: rejected, sound already made stays readable */
  TASK_CHECK(soxr_process(s, extra, 4, 0, out, cap, &got) != 0);
  TASK_CHECK(got == 0);
  /* ratio change after EOF: rejected */
  TASK_CHECK(soxr_set_io_ratio(s, 1.5, 0) != 0);
  /* repeated drain adds nothing */
  TASK_CHECK(soxr_process(s, 0, 0, 0, out, cap, &got) == 0);
  TASK_CHECK(got == 0);
  TASK_CHECK(soxr_delay(s) == 0);
  soxr_delete(s);
  free(in); free(out);
  return 0;
}

static int zero_length_does_not_end(void)
{
  size_t N = 80, cap = N * 4 + 64;
  float * in = malloc(N * sizeof(float));
  float * out = malloc(cap * sizeof(float));
  size_t got;
  soxr_t s = make_vr(1, 0);
  TASK_CHECK(s != 0);
  TASK_CHECK(in && out);
  fill_input(in, N, 1, 0);
  TASK_CHECK(soxr_set_io_ratio(s, 1., 0) == 0);
  TASK_CHECK(soxr_process(s, in, 40, 0, out, cap, &got) == 0);
  TASK_CHECK(soxr_process(s, in, 0, 0, out, cap, &got) == 0); /* non-NULL, 0 */
  TASK_CHECK(soxr_process(s, in + 40, 40, 0, out, cap, &got) == 0); /* accepted */
  do TASK_CHECK(soxr_process(s, 0, 0, 0, out, cap, &got) == 0);
  while (got);
  TASK_CHECK(soxr_delay(s) == 0);
  soxr_delete(s);
  free(in); free(out);
  return 0;
}

static int ratio_range_enforced(void)
{
  soxr_t s = make_vr(1, 0);
  TASK_CHECK(s != 0);
  TASK_CHECK(soxr_set_io_ratio(s, RATIO_MIN, 0) == 0);
  TASK_CHECK(soxr_set_io_ratio(s, RATIO_MAX, 0) == 0);
  TASK_CHECK(soxr_set_io_ratio(s, RATIO_MIN / 2, 0) != 0);
  TASK_CHECK(soxr_set_io_ratio(s, RATIO_MAX * 2, 0) != 0);
  /* rejected changes leave the engine usable at the previous ratio */
  float in[32], out[128];
  size_t got;
  fill_input(in, 32, 0, 0);
  TASK_CHECK(soxr_process(s, in, 32, 0, out, 128, &got) == 0);
  soxr_delete(s);
  return 0;
}

/* ----------------------------------------------------------------- registry */

#define CASE(g, n) { #g, #n, n }

const struct task_case task_cases[] = {
  CASE(length, length_fresh_and_clear),
  CASE(length, length_empty_fragment),
  CASE(length, length_constant_rule),
  CASE(length, length_chunk_invariance),
  CASE(length, length_tail_impulse),
  CASE(length, length_query_does_not_change_pcm),
  CASE(length, endpoint_rejections),
  CASE(length, zero_length_does_not_end),
  CASE(length, ratio_range_enforced),

  CASE(slew, slew_schedule_length),
  CASE(slew, slew_never_overshoots),
  CASE(slew, slew_chunk_invariance),
  CASE(slew, slew_mid_reset_and_instant),
  CASE(slew, slew_query_invariant),

  CASE(callback, callback_matches_process),
  CASE(callback, callback_query_invariant),
  CASE(callback, callback_multichannel),
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
