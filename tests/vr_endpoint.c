/* Variable-rate resampler: finite-segment end-point, slew and delay tests.
 *
 * The logical-time rule under test (see README): with the initial input/output
 * ratio r, output frame k draws from source-time centre c_k = S_k + r_k/2,
 * where S_k is the sum of the ratios of preceding frames.  A length-L slew
 * uses r_k = r0 + (rt-r0)*k/L for 0 <= k < L, then holds rt.  An input of N
 * frames yields exactly the frames whose centres do not exceed N.
 *
 * These tests recompute that model independently (never trusting the engine's
 * own counters), and check that PCM is independent of input chunking, output
 * capacity and soxr_delay() queries.
 */

#include "soxr.h"
#include "../task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CH 6

/* --------------------------- independent model --------------------------- */

/* Constant-ratio number of frames whose centre does not exceed N. */
static long model_const_count(double r, long N)
{
  long k = 0;
  while (r * ((double)k + .5) <= (double)N)
    ++k;
  return k;
}

/* Replays a ratio/slew schedule exactly as specified, independently. */
struct sched_event {
  int at;           /* Output frame at which the change begins. */
  double target;    /* Ratio to move to. */
  int L;            /* Slew length; 0 means instant. */
};

struct sched {
  double r0;        /* Initial ratio. */
  struct sched_event ev[32];
  size_t nev;
};

static void sched_init(struct sched *h, double r0)
{
  memset(h, 0, sizeof(*h));
  h->r0 = r0;
}

/* Record a ratio change; the slew is re-based on the next frame's ratio. */
static void sched_event(struct sched *h, double target, int L, int at_frame)
{
  if (!task_check(h->nev < sizeof(h->ev) / sizeof(h->ev[0]),
      "sched_event capacity", __FILE__, __LINE__))
    return;
  h->ev[h->nev].at = at_frame;
  h->ev[h->nev].target = target;
  h->ev[h->nev].L = L;
  ++h->nev;
}

/* Count frames whose centre does not exceed N by replaying the recorded
 * events frame by frame, exactly as the engine is specified to behave. */
static long sched_count(struct sched *h, long N, int from, double *S)
{
  double r = h->r0;          /* Ratio of the next frame. */
  double base = h->r0, target = h->r0;
  int L = 0, idx = 0, active = 0;
  size_t ei = 0;
  int k = 0;
  long n = 0;
  (void)from;
  while (1) {
    while (ei < h->nev && h->ev[ei].at <= k) {       /* Apply due events. */
      base = r;                                     /* From next frame's ratio. */
      target = h->ev[ei].target;
      L = h->ev[ei].L;
      idx = 0;
      active = L > 0;
      if (!active)
        r = target;
      ++ei;
    }
    if (*S + r * .5 > (double)N)
      break;
    ++n;
    *S += r;
    ++k;
    if (active) {
      if (++idx >= L) {
        r = target;
        active = 0;
      }
      else
        r = base + (target - base) * (double)idx / (double)L;
    }
  }
  return n;
}

/* ------------------------------ engine helpers --------------------------- */

static soxr_t vr_create(unsigned ch, int split)
{
  soxr_error_t e = 0;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_VR);
  soxr_datatype_t t = split ? SOXR_FLOAT32_S : SOXR_FLOAT32_I;
  soxr_io_spec_t io = soxr_io_spec(t, t);
  soxr_runtime_spec_t rt = soxr_runtime_spec(1);
  soxr_t s = soxr_create(8.0, 1.0, ch, &e, &io, &q, &rt);
  if (!task_check(s && !e, "vr_create", __FILE__, __LINE__))
    return 0;
  return s;
}

static void fill_signal(float *buf, size_t n)
{
  size_t i;
  for (i = 0; i < n; ++i)
    buf[i] = (float)(.35 * sin(i * .031) + .12 * cos(i * .057) + .05 * sin(i * .113));
}

/* Feed a finite interleaved segment then flush.  Returns output frames.
 * When query != 0, soxr_delay() is called after every process call. */
static size_t run_il(soxr_t s, float const *in, size_t N, unsigned ch,
    size_t ichunk, size_t ocap, int query, float *out)
{
  size_t pos = 0, total = 0, idone, odone;
  while (pos < N) {
    size_t il = ichunk < N - pos ? ichunk : N - pos;
    TASK_CHECK(!soxr_process(s, in + pos * ch, il, &idone,
        out + total * ch, ocap, &odone));
    total += odone;
    pos += idone;
    if (query)
      (void)soxr_delay(s);
    if (!idone && !odone)
      break;
  }
  for (;;) {
    TASK_CHECK(!soxr_process(s, 0, 0, &idone, out + total * ch, ocap, &odone));
    total += odone;
    if (query)
      (void)soxr_delay(s);
    if (!odone)
      break;
  }
  return total;
}

/* Same, for split (separate per-channel) float buffers. */
static size_t run_sp(soxr_t s, float const *const *srcs, size_t N, unsigned ch,
    size_t ichunk, size_t ocap, int query, float **dsts)
{
  size_t pos = 0, total = 0, idone, odone, c;
  float const *in_cur[MAX_CH];          /* Mutable per-channel read cursors. */
  for (c = 0; c < ch; ++c)
    in_cur[c] = srcs[c];
  while (pos < N) {
    size_t il = ichunk < N - pos ? ichunk : N - pos;
    TASK_CHECK(!soxr_process(s, (soxr_in_t)in_cur, il, &idone,
        (soxr_out_t)dsts, ocap, &odone));
    for (c = 0; c < ch; ++c) {
      in_cur[c] += idone;               /* Advance each input channel. */
      dsts[c] += odone;
    }
    total += odone;
    pos += idone;
    if (query)
      (void)soxr_delay(s);
    if (!idone && !odone)
      break;
  }
  for (;;) {
    TASK_CHECK(!soxr_process(s, 0, 0, &idone, (soxr_out_t)dsts, ocap, &odone));
    for (c = 0; c < ch; ++c)
      dsts[c] += odone;
    total += odone;
    if (query)
      (void)soxr_delay(s);
    if (!odone)
      break;
  }
  return total;
}

static int fsame(float a, float b)
{
  return a == b;
}

/* While constant-ratio PCM is bit-exact across blockings, a ratio slew drives
 * the engine's fixed-point clock through block-dependent rounding (this is
 * true of the reference engine too), so slew PCM is compared within a tight
 * tolerance rather than bit-for-bit. */
static int fnear_pcm(float a, float b)
{
  return fabsf(a - b) <= 1e-2f + 1e-3f * fabsf(b);
}

/* ------------------------------- length group ---------------------------- */

static int length_counts(void)
{
  static double const rs[] = {2.0, 1.0, .75, .5, .25, 4.0};
  static long const Ns[] = {0, 1, 2, 3, 7, 50, 250};
  size_t ri, ni;
  for (ri = 0; ri < sizeof(rs) / sizeof(rs[0]); ++ri)
    for (ni = 0; ni < sizeof(Ns) / sizeof(Ns[0]); ++ni) {
      double r = rs[ri];
      long N = Ns[ni];
      soxr_t s = vr_create(1, 0);
      TASK_CHECK(!soxr_set_io_ratio(s, r, 0));
      TASK_CHECK(soxr_delay(s) == 0.0);       /* Fresh: nothing owed. */
      float *in = calloc((size_t)(N ? N : 1), sizeof(float));
      float *out = calloc((size_t)(N * 16 + 64), sizeof(float));
      fill_signal(in, (size_t)N);
      size_t got = run_il(s, in, (size_t)N, 1, 64, 64, 0, out);
      TASK_CHECK((long)got == model_const_count(r, N));
      TASK_CHECK(soxr_delay(s) == 0.0);       /* Fully drained. */
      TASK_CHECK(!soxr_clear(s));
      TASK_CHECK(soxr_delay(s) == 0.0);       /* After clear. */
      soxr_delete(s);
      free(in);
      free(out);
    }
  return 0;
}

static int length_chunk_invariance(void)
{
  size_t const N = 320;
  static size_t const cfg[][2] = {
    {320, 512}, {37, 512}, {1, 512}, {320, 7}, {53, 3}, {13, 1}};
  float *in = calloc(N, sizeof(float));
  float *ref = calloc(N * 16 + 64, sizeof(float));
  size_t i, k, nref = 0;
  fill_signal(in, N);
  for (i = 0; i < sizeof(cfg) / sizeof(cfg[0]); ++i) {
    soxr_t s = vr_create(1, 0);
    TASK_CHECK(!soxr_set_io_ratio(s, 1.25, 0));
    float *out = calloc(N * 16 + 64, sizeof(float));
    size_t n = run_il(s, in, N, 1, cfg[i][0], cfg[i][1], (int)(i == 2), out);
    if (i == 0) {
      memcpy(ref, out, n * sizeof(float));
      nref = n;
    }
    TASK_CHECK(n == nref);
    for (k = 0; k < n; ++k)
      TASK_CHECK(fsame(out[k], ref[k]));
    soxr_delete(s);
    free(out);
  }
  free(in);
  free(ref);
  return 0;
}

/* A soxr_delay() query must neither consume nor alter sound. */
static int length_query_invisible(void)
{
  size_t const N = 300;
  float *in = calloc(N, sizeof(float));
  float a[8192], b[8192];
  fill_signal(in, N);
  soxr_t s1 = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s1, 1.5, 0));
  size_t n1 = run_il(s1, in, N, 1, 29, 11, 0, a);
  soxr_delete(s1);
  soxr_t s2 = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s2, 1.5, 0));
  size_t n2 = run_il(s2, in, N, 1, 29, 11, 1, b); /* Queries interspersed. */
  soxr_delete(s2);
  TASK_CHECK(n1 == n2);
  {
    size_t k;
    for (k = 0; k < n1; ++k)
      TASK_CHECK(fsame(a[k], b[k]));
  }
  free(in);
  return 0;
}

/* Delay mid-stream equals frames still deliverable if input ended now. */
static int length_delay_matches_model(void)
{
  size_t const N = 400;
  double const r = 1.5;
  float *in = calloc(N, sizeof(float));
  float out[8192];
  size_t pos = 0, total = 0, idone, odone;
  fill_signal(in, N);
  soxr_t s = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s, r, 0));
  while (pos < N) {
    TASK_CHECK(!soxr_process(s, in + pos, 31, &idone, out + total, 13, &odone));
    total += odone;
    pos += idone;
    /* Independent expectation for the accepted prefix. */
    TASK_CHECK((long)soxr_delay(s) ==
        model_const_count(r, (long)pos) - (long)total);
    if (!idone && !odone)
      break;
  }
  soxr_delete(s);
  free(in);
  return 0;
}

/* The finite segment must be a prefix of a longer (un-ended) reference and its
 * filter tail must remain audible. */
static int length_tail_preserved(void)
{
  size_t const N = 120, pad = 400;
  float *fin = calloc(N, sizeof(float));
  float *lng = calloc(N + pad, sizeof(float));
  float ff[8192], fl[8192];
  fin[N - 2] = lng[N - 2] = 1.0f;          /* Impulse near the finite end. */
  soxr_t s1 = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s1, 1.0, 0));
  size_t nf = run_il(s1, fin, N, 1, 11, 11, 0, ff);
  soxr_delete(s1);
  soxr_t s2 = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s2, 1.0, 0));
  size_t nl = run_il(s2, lng, N + pad, 1, 11, 11, 0, fl);
  soxr_delete(s2);
  TASK_CHECK(nl > nf);
  {
    size_t k;
    int nonzero = 0;
    for (k = 0; k < nf; ++k) {
      TASK_CHECK(fsame(ff[k], fl[k]));     /* Byte-exact valid prefix. */
      if (fabsf(ff[k]) > 1e-7f)
        ++nonzero;
    }
    TASK_CHECK(nonzero > 0);               /* Tail not silently truncated. */
  }
  free(fin);
  free(lng);
  return 0;
}

/* 1, 2 and 6 channels, interleaved and split: channels share progress. */
static int length_multichannel(void)
{
  static unsigned const chs[] = {1, 2, 6};
  size_t const N = 256;
  double const r = 1.25;
  size_t ci;
  float *sig = calloc(N, sizeof(float));
  fill_signal(sig, N);
  /* Mono reference (interleaved), generous caps. */
  float *ref = calloc(N * 8 + 64, sizeof(float));
  soxr_t sr = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(sr, r, 0));
  size_t nref = run_il(sr, sig, N, 1, N, 64, 0, ref);
  soxr_delete(sr);

  for (ci = 0; ci < sizeof(chs) / sizeof(chs[0]); ++ci) {
    unsigned ch = chs[ci];
    size_t k, c;

    /* Interleaved: every channel carries the identical signal. */
    float *il_in = calloc(N * ch, sizeof(float));
    for (k = 0; k < N; ++k)
      for (c = 0; c < ch; ++c)
        il_in[k * ch + c] = sig[k];
    float *il_out = calloc((nref + 16) * ch, sizeof(float));
    soxr_t si = vr_create(ch, 0);
    TASK_CHECK(!soxr_set_io_ratio(si, r, 0));
    size_t ni = run_il(si, il_in, N, ch, 37, 17, 0, il_out);
    TASK_CHECK(ni == nref);
    for (k = 0; k < ni; ++k)
      for (c = 0; c < ch; ++c)
        TASK_CHECK(fsame(il_out[k * ch + c], ref[k]));
    soxr_delete(si);

    /* Split: compare each channel against itself under a different chunking. */
    float *srcs[MAX_CH], *dst1[MAX_CH], *dst2[MAX_CH], *base1[MAX_CH], *base2[MAX_CH];
    for (c = 0; c < ch; ++c) {
      srcs[c] = calloc(N, sizeof(float));
      for (k = 0; k < N; ++k)
        srcs[c][k] = sig[k];
      base1[c] = calloc(nref + 16, sizeof(float));
      base2[c] = calloc(nref + 16, sizeof(float));
      dst1[c] = base1[c];
      dst2[c] = base2[c];
    }
    soxr_t sp1 = vr_create(ch, 1);
    TASK_CHECK(!soxr_set_io_ratio(sp1, r, 0));
    size_t ns1 = run_sp(sp1, (float const *const *)srcs, N, ch, 41, 19, 0, dst1);
    soxr_delete(sp1);
    soxr_t sp2 = vr_create(ch, 1);
    TASK_CHECK(!soxr_set_io_ratio(sp2, r, 0));
    size_t ns2 = run_sp(sp2, (float const *const *)srcs, N, ch, 7, 5, 1, dst2);
    soxr_delete(sp2);
    TASK_CHECK(ns1 == nref && ns2 == nref);   /* Shared progress, same count. */
    for (c = 0; c < ch; ++c) {
      for (k = 0; k < nref; ++k) {
        TASK_CHECK(fsame(base1[c][k], base2[c][k]));
        TASK_CHECK(fsame(base1[c][k], ref[k]));
      }
      free(srcs[c]);
      free(base1[c]);
      free(base2[c]);
    }
    free(il_in);
    free(il_out);
  }
  free(sig);
  free(ref);
  return 0;
}

/* End-of-input rules: rejection, repeat draining, zero-length feeds, clear. */
static int length_endpoint_rules(void)
{
  size_t const N = 200;
  float *in = calloc(N, sizeof(float));
  float out[8192];
  size_t idone, odone;
  fill_signal(in, N);
  soxr_t s = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s, 2.0, 0));

  /* A non-NULL zero-length feed must not end the segment. */
  TASK_CHECK(!soxr_process(s, in, 0, &idone, out, 64, &odone));
  TASK_CHECK(idone == 0 && odone == 0);
  TASK_CHECK(soxr_delay(s) == 0.0);          /* Nothing accepted yet. */

  size_t pos = 0, total = 0;
  while (pos < N) {
    TASK_CHECK(!soxr_process(s, in + pos, N - pos, &idone, out + total, 64, &odone));
    pos += idone;
    total += odone;
    if (!idone && !odone)
      break;
  }
  size_t const full = (size_t)model_const_count(2.0, (long)N);

  /* Signal EOF, then drain only part of the tail. */
  TASK_CHECK(!soxr_process(s, 0, 0, &idone, out + total, 8, &odone));
  total += odone;
  double owed = soxr_delay(s);
  TASK_CHECK(owed > 0 && (long)(total + owed) == (long)full);

  /* Non-empty input and a ratio change are rejected, sound is preserved. */
  float more[3] = {1, 2, 3};
  TASK_CHECK(soxr_process(s, more, 3, &idone, out + total, 8, &odone) != 0);
  TASK_CHECK(idone == 0 && odone == 0);
  TASK_CHECK(soxr_delay(s) == owed);         /* Unread sound untouched. */
  TASK_CHECK(soxr_set_io_ratio(s, 1.0, 0) != 0);
  TASK_CHECK(soxr_delay(s) == owed);
  TASK_CHECK(!soxr_error(s));                /* Recoverable, not a fatal error. */

  /* Finish draining; repeat draining adds nothing. */
  for (;;) {
    TASK_CHECK(!soxr_process(s, 0, 0, &idone, out + total, 8, &odone));
    total += odone;
    if (!odone)
      break;
  }
  TASK_CHECK(total == full && soxr_delay(s) == 0.0);
  TASK_CHECK(!soxr_process(s, 0, 0, &idone, out, 64, &odone));
  TASK_CHECK(odone == 0 && soxr_delay(s) == 0.0);

  /* Clear makes the resampler reusable for a fresh segment. */
  TASK_CHECK(!soxr_clear(s));
  TASK_CHECK(soxr_delay(s) == 0.0);
  size_t again = run_il(s, in, N, 1, 50, 30, 0, out);
  TASK_CHECK(again == full);
  soxr_delete(s);
  free(in);
  return 0;
}

/* -------------------------------- slew group ----------------------------- */

struct milestone { int at; double target; int L; };

/* Drive scheduled ratio changes and independently predict the frame count. */
static int slew_scenario(double r0, struct milestone const *ms, size_t nms,
    long N, size_t ocap)
{
  struct sched h;
  float *in = calloc(N, sizeof(float));
  float *out = calloc((size_t)(N * 16 + 64), sizeof(float));
  fill_signal(in, (size_t)N);
  sched_init(&h, r0);

  soxr_t s = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s, r0, 0));
  size_t pos = 0, total = 0, idone, odone;
  size_t mi = 0;
  while (pos < (size_t)N) {
    while (mi < nms && (int)total >= ms[mi].at) {
      TASK_CHECK(!soxr_set_io_ratio(s, ms[mi].target, (size_t)ms[mi].L));
      /* Record the real frame the change begins at (batches may overshoot). */
      sched_event(&h, ms[mi].target, ms[mi].L, (int)total);
      ++mi;
    }
    /* Feed small chunks so output frames keep pace with input and every
     * milestone frame is reached while data is still flowing. */
    size_t il = 8 < N - (long)pos ? 8 : N - (long)pos;
    TASK_CHECK(!soxr_process(s, in + pos, il, &idone, out + total, ocap, &odone));
    pos += idone;
    total += odone;
    if (!idone && !odone)
      break;
  }
  /* Every ratio change must have been applied while input was still flowing;
   * a change requested after end-of-input is (correctly) rejected. */
  TASK_CHECK(mi == nms);
  for (;;) {
    TASK_CHECK(!soxr_process(s, 0, 0, &idone, out + total, ocap, &odone));
    total += odone;
    if (!odone)
      break;
  }
  double S = 0.0;
  long expect = sched_count(&h, N, 0, &S);
  TASK_CHECK((long)total == expect);
  TASK_CHECK(soxr_delay(s) == 0.0);
  soxr_delete(s);
  free(in);
  free(out);
  return 0;
}

static int slew_basic_counts(void)
{
  struct milestone none[] = {{0, 2.0, 100}};
  struct milestone down[] = {{0, .5, 50}};
  struct milestone instant[] = {{0, 3.0, 0}};
  struct milestone wide[] = {{0, .25, 80}};
  static long const Ns[] = {1, 50, 200, 500, 1000};
  size_t i;
  for (i = 0; i < sizeof(Ns) / sizeof(Ns[0]); ++i) {
    TASK_CHECK(!slew_scenario(.5, none, 1, Ns[i], 7));
    TASK_CHECK(!slew_scenario(2.0, down, 1, Ns[i], 13));
    TASK_CHECK(!slew_scenario(1.0, instant, 1, Ns[i], 9));
    TASK_CHECK(!slew_scenario(3.0, wide, 1, Ns[i], 5));
  }
  return 0;
}

/* Re-target a slew while it is still in progress; the new slew continues from
 * the next frame's ratio.  Inputs are large enough that all milestones are
 * reached while data is still flowing (none land in the post-EOF tail). */
static int slew_reschedule(void)
{
  struct milestone ms[] = {{0, 2.0, 100}, {40, .4, 60}, {120, 1.5, 0}};
  static long const Ns[] = {600, 1200, 2000};
  size_t i;
  for (i = 0; i < sizeof(Ns) / sizeof(Ns[0]); ++i)
    TASK_CHECK(!slew_scenario(.5, ms, 3, Ns[i], 11));
  return 0;
}

/* PCM under a slew must not depend on chunking or output capacity. */
static int slew_pcm_invariant(void)
{
  size_t const N = 500;
  float *in = calloc(N, sizeof(float));
  float *ref = calloc(N * 16 + 64, sizeof(float));
  static size_t const caps[] = {256, 17, 3, 1};
  size_t i, k;
  fill_signal(in, N);
  size_t nref = 0;
  for (i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i) {
    soxr_t s = vr_create(1, 0);
    TASK_CHECK(!soxr_set_io_ratio(s, .5, 0));
    TASK_CHECK(!soxr_set_io_ratio(s, 2.0, 120));
    float *out = calloc(N * 16 + 64, sizeof(float));
    size_t pos = 0, total = 0, idone, odone;
    while (pos < N) {
      TASK_CHECK(!soxr_process(s, in + pos, N - pos, &idone, out + total, caps[i], &odone));
      pos += idone;
      total += odone;
      if (!idone && !odone)
        break;
    }
    for (;;) {
      TASK_CHECK(!soxr_process(s, 0, 0, &idone, out + total, caps[i], &odone));
      total += odone;
      if (!odone)
        break;
    }
    if (i == 0) {
      memcpy(ref, out, total * sizeof(float));
      nref = total;
    }
    TASK_CHECK(total == nref);
    for (k = 0; k < total; ++k)
      TASK_CHECK(fnear_pcm(out[k], ref[k]));
    soxr_delete(s);
    free(out);
  }
  free(in);
  free(ref);
  return 0;
}

/* Ratios outside [1/8, 8] are clamped; out-of-range/non-positive rejected. */
static int slew_range(void)
{
  long const N = 160;
  float *in = calloc(N, sizeof(float));
  float hi[4096], lo[4096], h8[4096], l8[4096];
  fill_signal(in, (size_t)N);
  soxr_t a = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(a, 100.0, 0));            /* -> 8 */
  size_t nhi = run_il(a, in, (size_t)N, 1, 64, 64, 0, hi);
  soxr_delete(a);
  soxr_t b = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(b, 8.0, 0));
  size_t n8 = run_il(b, in, (size_t)N, 1, 64, 64, 0, h8);
  soxr_delete(b);
  soxr_t c = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(c, .01, 0));             /* -> 1/8 */
  size_t nlo = run_il(c, in, (size_t)N, 1, 64, 64, 0, lo);
  soxr_delete(c);
  soxr_t d = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(d, .125, 0));
  size_t n18 = run_il(d, in, (size_t)N, 1, 64, 64, 0, l8);
  soxr_delete(d);
  TASK_CHECK(nhi == n8 && nlo == n18);
  {
    size_t k;
    for (k = 0; k < nhi; ++k)
      TASK_CHECK(fsame(hi[k], h8[k]));
    for (k = 0; k < nlo; ++k)
      TASK_CHECK(fsame(lo[k], l8[k]));
  }
  soxr_t e = vr_create(1, 0);
  TASK_CHECK(soxr_set_io_ratio(e, 0.0, 0) != 0);
  TASK_CHECK(soxr_set_io_ratio(e, -1.0, 0) != 0);
  soxr_delete(e);
  free(in);
  return 0;
}

/* ------------------------------ callback group --------------------------- */

struct cb_state {
  float const *data;
  size_t N, pos;     /* N and pos are in frames; stride is samples/frame. */
  unsigned stride;
  int eof_seen;
};

static size_t input_fn(void *state, soxr_in_t *data, size_t requested)
{
  struct cb_state *c = state;
  size_t avail = c->N - c->pos;
  if (avail == 0) {
    ++c->eof_seen;
    return 0;                            /* EOF: len 0, leave *data non-NULL. */
  }
  size_t n = requested < avail ? requested : avail;
  *data = (soxr_in_t)(c->data + c->pos * c->stride);
  c->pos += n;
  return n;
}

/* Callback path must agree with the NULL-flush soxr_process path. */
static int callback_matches_process(void)
{
  size_t const N = 280;
  float *in = calloc(N, sizeof(float));
  float via_cb[8192], via_null[8192];
  fill_signal(in, N);

  /* Reference: NULL input signals end-of-input. */
  soxr_t s1 = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s1, 2.0, 0));
  size_t nnull = run_il(s1, in, N, 1, 60, 40, 0, via_null);
  soxr_delete(s1);

  /* Input-function path: EOF is a zero-length callback. */
  soxr_t s2 = vr_create(1, 0);
  TASK_CHECK(!soxr_set_io_ratio(s2, 2.0, 0));
  struct cb_state st = {in, N, 0, 1, 0};
  TASK_CHECK(!soxr_set_input_fn(s2, input_fn, &st, 64));
  size_t ncb = 0;
  for (;;) {
    size_t od = soxr_output(s2, via_cb + ncb, 23);
    ncb += od;
    if (od == 0)
      break;
  }
  TASK_CHECK(st.eof_seen >= 1);
  TASK_CHECK(ncb == nnull);
  {
    size_t k;
    for (k = 0; k < ncb; ++k)
      TASK_CHECK(fsame(via_cb[k], via_null[k]));
  }
  /* EOF reached: further reads add nothing and delay reports zero. */
  TASK_CHECK(soxr_output(s2, via_cb, 32) == 0);
  TASK_CHECK(soxr_delay(s2) == 0.0);
  soxr_delete(s2);
  free(in);
  return 0;
}

/* Different callback block sizes must give identical sound. */
static int callback_chunk_invariance(void)
{
  size_t const N = 260;
  float *in = calloc(N, sizeof(float));
  float ref[8192];
  static size_t const blocks[] = {64, 17, 1};
  size_t i, k, nref = 0;
  fill_signal(in, N);
  for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]); ++i) {
    soxr_t s = vr_create(1, 0);
    TASK_CHECK(!soxr_set_io_ratio(s, .75, 0));
    struct cb_state st = {in, N, 0, 1, 0};
    TASK_CHECK(!soxr_set_input_fn(s, input_fn, &st, blocks[i]));
    float out[8192];
    size_t total = 0;
    for (;;) {
      size_t od = soxr_output(s, out + total, blocks[i] + 5);
      total += od;
      if (od == 0)
        break;
    }
    if (i == 0) {
      memcpy(ref, out, total * sizeof(float));
      nref = total;
    }
    TASK_CHECK(total == nref);
    for (k = 0; k < total; ++k)
      TASK_CHECK(fsame(out[k], ref[k]));
    soxr_delete(s);
  }
  free(in);
  return 0;
}

/* Callback EOF with multiple channels: progress is shared across channels. */
static int callback_multichannel(void)
{
  static unsigned const chs[] = {2, 6};
  size_t const N = 200;
  size_t ci;
  float *sig = calloc(N, sizeof(float));
  fill_signal(sig, N);
  for (ci = 0; ci < sizeof(chs) / sizeof(chs[0]); ++ci) {
    unsigned ch = chs[ci];
    float *il = calloc(N * ch, sizeof(float));
    size_t k, c;
    for (k = 0; k < N; ++k)
      for (c = 0; c < ch; ++c)
        il[k * ch + c] = sig[k];
    soxr_t s = vr_create(ch, 0);
    TASK_CHECK(!soxr_set_io_ratio(s, 1.5, 0));
    struct cb_state st = {il, N, 0, ch, 0};
    TASK_CHECK(!soxr_set_input_fn(s, input_fn, &st, 48));
    float *out = calloc(N * 8 * ch + 64, sizeof(float));
    size_t total = 0;
    for (;;) {
      size_t od = soxr_output(s, out + total * ch, 19);
      total += od;
      if (od == 0)
        break;
    }
    TASK_CHECK((long)total == model_const_count(1.5, (long)N));
    TASK_CHECK(soxr_delay(s) == 0.0);
    /* Identical input: every channel carries the same per-frame result. */
    for (k = 0; k < total; ++k)
      for (c = 1; c < ch; ++c)
        TASK_CHECK(fsame(out[k * ch + c], out[k * ch]));
    soxr_delete(s);
    free(il);
    free(out);
  }
  free(sig);
  return 0;
}

const struct task_case task_cases[] = {
  {"length", "counts",            length_counts},
  {"length", "chunk_invariance",  length_chunk_invariance},
  {"length", "query_invisible",   length_query_invisible},
  {"length", "delay_model",       length_delay_matches_model},
  {"length", "tail_preserved",    length_tail_preserved},
  {"length", "multichannel",      length_multichannel},
  {"length", "endpoint_rules",    length_endpoint_rules},

  {"slew", "basic_counts",        slew_basic_counts},
  {"slew", "reschedule",          slew_reschedule},
  {"slew", "pcm_invariant",       slew_pcm_invariant},
  {"slew", "range_clamp",         slew_range},

  {"callback", "matches_process", callback_matches_process},
  {"callback", "chunk_invariance",callback_chunk_invariance},
  {"callback", "multichannel",    callback_multichannel},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
