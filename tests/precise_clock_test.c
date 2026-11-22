/* Precise rational clock tests for the SpeexDSP resampler.
 *
 * Registered with task_support/check.h in three groups: arithmetic, audio
 * and streaming.  Pure C, no external multi-precision dependency: the
 * independent rational reference accumulates 2/3 and 5/7 on their common
 * denominator 21, deliberately not reusing the production clock recurrence.
 */

#include "speex_resampler.h"
#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATIO_A_NUM 2u
#define RATIO_A_DEN 3u
#define RATIO_B_NUM 5u
#define RATIO_B_DEN 7u
#define RATE_IN 32000u
#define RATE_OUT 48000u
#define QUALITY 5

/* ----------------------------------------------------------------------- */
/* Independent reference arithmetic (no production clock code reused).     */
/* ----------------------------------------------------------------------- */

/* floor(num * 2^64 / den) for 32-bit num, den, computed without __int128 so
   the reference does not even rely on the same compiler extension. */
static uint64_t ref_frac64(uint32_t num, uint32_t den)
{
   /* num*2^64/den = num * (2^64/den); split into two 32-bit products. */
   uint64_t hi = ((uint64_t)num << 32) / den;
   uint64_t rem = ((uint64_t)num << 32) % den;
   uint64_t lo = (rem << 32) / den;
   return (hi << 32) | (uint32_t)lo;
}

struct ref_clock {
   /* U = counter21 / 21 input frames.  2/3 contributes 14, 5/7 contributes 15. */
   int64_t counter21;
};

static void ref_init(struct ref_clock *r) { r->counter21 = 0; }

static void ref_advance(struct ref_clock *r, int ratio_b)
{
   r->counter21 += ratio_b ? 15 : 14;
}

static void ref_get(const struct ref_clock *r, uint64_t *whole, uint64_t *frac64)
{
   int64_t q = r->counter21 / 21;
   int64_t rem = r->counter21 - q * 21;
   *whole = (uint64_t)q;
   *frac64 = ref_frac64((uint32_t)rem, 21u);
}

/* WORD2INT rule from arch.h for the floating-point build. */
static spx_int16_t quantize(float x)
{
   float v = floorf(0.5f + x);
   if (v < -32767.5f) return -32768;
   if (v > 32766.5f) return 32767;
   return (spx_int16_t)v;
}

/* Each clock step stores floor(num*2^64/den), so a position built from several
   steps can differ from the exact rational fraction by a few 2^-64 ULPs.
   Allow a tight margin while staying vastly inside the 2^-40 (= 2^24 ULP)
   contractual bound. */
#define FRAC_ULP_TOLERANCE UINT64_C(64)

static int frac_close(uint64_t got, uint64_t want)
{
   uint64_t d = got > want ? got - want : want - got;
   return d <= FRAC_ULP_TOLERANCE;
}

static SpeexResamplerState *make_precise(unsigned channels, int quality,
                                         uint32_t num, uint32_t den)
{
   int err = 0;
   SpeexResamplerState *st =
      speex_resampler_init_frac(channels, num, den, RATE_IN, RATE_OUT, quality, &err);
   if (!st || err != RESAMPLER_ERR_SUCCESS)
      return NULL;
   if (speex_resampler_enable_precise_clock(st) != RESAMPLER_ERR_SUCCESS)
   {
      speex_resampler_destroy(st);
      return NULL;
   }
   return st;
}

static int cursor_get(SpeexResamplerState *st, uint32_t ch,
                      uint64_t *w, uint64_t *f)
{
   return speex_resampler_get_precise_clock(st, ch, w, f);
}

/* ======================================================================= */
/* arithmetic group                                                        */
/* ======================================================================= */

static int test_cursor_basics(void)
{
   SpeexResamplerState *st = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   uint64_t w = 99, f = 99;
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == 0 && f == 0);

   /* One produced frame at 2/3: U must be exactly floor(2^64*2/3)/2^64. */
   float in[256], out[16];
   for (int i = 0; i < 256; i++) in[i] = 0.25f * sinf(0.07f * i);
   spx_uint32_t il = 256, ol = 1;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(ol == 1);
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == 0 && f == ref_frac64(2, 3));

   /* 1/1 gives whole frames while the old 2/3 fraction persists. */
   TASK_CHECK(speex_resampler_set_rate_frac(st, 1, 1, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
   il = 256; ol = 16;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == 16 && f == ref_frac64(2, 3));

   /* 5/7: U = 16 + 2/3 + 5/7 = 17 + 8/21. */
   TASK_CHECK(speex_resampler_set_rate_frac(st, 5, 7, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
   il = 256; ol = 1;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == 17 && frac_close(f, ref_frac64(8, 21)));

   speex_resampler_destroy(st);
   return 0;
}

static int test_long_accumulation(void)
{
   /* At least 2^20 real advances, alternating both ratios, compared with the
      common-denominator-21 reference.  Error budget: 2^-40 input frames. */
   enum { TARGET = 1 << 20, BLOCK = 8192 };
   SpeexResamplerState *st = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   speex_resampler_skip_zeros(st);

   size_t cap = (size_t)TARGET * 2 + 256;
   float *in = (float *)malloc(cap * sizeof(float));
   float *out = (float *)malloc(BLOCK * sizeof(float));
   TASK_CHECK(in != NULL && out != NULL);
   for (size_t i = 0; i < cap; i++) in[i] = 0.3f * sinf(0.013f * (float)i);

   struct ref_clock ref;
   ref_init(&ref);
   long produced = 0;
   size_t in_pos = 0;
   int use_b = 0;
   while (produced < TARGET)
   {
      spx_uint32_t want = (spx_uint32_t)((TARGET - produced) < BLOCK ? (TARGET - produced) : BLOCK);
      TASK_CHECK(speex_resampler_set_rate_frac(st, use_b ? RATIO_B_NUM : RATIO_A_NUM,
                  use_b ? RATIO_B_DEN : RATIO_A_DEN, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
      spx_uint32_t il = (spx_uint32_t)(cap - in_pos);
      spx_uint32_t ol = want;
      TASK_CHECK(speex_resampler_process_float(st, 0, in + in_pos, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(ol == want);
      in_pos += il;
      for (spx_uint32_t k = 0; k < ol; k++)
         ref_advance(&ref, use_b);
      produced += ol;
      use_b = !use_b;
   }
   TASK_CHECK(produced >= TARGET);

   uint64_t w, f, rw, rf;
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   ref_get(&ref, &rw, &rf);
   TASK_CHECK(w == rw);
   uint64_t diff = f > rf ? f - rf : rf - f;
   /* 2^-40 in 2^-64 fraction units is 2^24.  In addition, compare the full
      128-bit cursor so the bound also covers a carry into the whole part. */
   TASK_CHECK(diff <= (UINT64_C(1) << 24));
#if defined(__SIZEOF_INT128__)
   {
      unsigned __int128 got = ((unsigned __int128)w << 64) | f;
      unsigned __int128 want = ((unsigned __int128)rw << 64) | rf;
      unsigned __int128 d128 = got > want ? got - want : want - got;
      TASK_CHECK(d128 <= ((unsigned __int128)1 << 24));
      printf("    %d-frame cursor error = %.3e input frames (bound 2^-40 = %.3e)\n",
             TARGET, ldexp((double)(uint64_t)d128, -64), ldexp(1.0, -40));
   }
#else
   printf("    %d-frame cursor fraction error = %llu ULPs (bound 2^24)\n",
          TARGET, (unsigned long long)diff);
#endif

   free(in);
   free(out);
   speex_resampler_destroy(st);
   return 0;
}

static int test_roundtrip_no_output(void)
{
   SpeexResamplerState *st = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   float in[512], out[64];
   for (int i = 0; i < 512; i++) in[i] = 0.2f * sinf(0.05f * i);

   /* Generate a few frames so a real fractional position exists. */
   spx_uint32_t il = 512, ol = 32;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(ol == 32);
   uint64_t w0, f0;
   TASK_CHECK(cursor_get(st, 0, &w0, &f0) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(f0 != 0);

   /* Repeated A -> B -> A trips with zero output capacity must not move U. */
   for (int k = 0; k < 8; k++)
   {
      spx_uint32_t zero = 0;
      TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &zero) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(zero == 0);
      TASK_CHECK(speex_resampler_set_rate_frac(st, 5, 7, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(speex_resampler_set_rate_frac(st, 2, 3, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
      uint64_t w, f;
      TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(w == w0 && f == f0);
   }

   /* Empty / NULL feeding also produces nothing and does not advance. */
   spx_uint32_t none = 0;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &none, out, &none) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_process_float(st, 0, NULL, &none, out, &none) == RESAMPLER_ERR_SUCCESS);
   uint64_t w, f;
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == w0 && f == f0);

   /* Querying never advances either. */
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == w0 && f == f0);

   speex_resampler_destroy(st);
   return 0;
}

static int test_ratio_validation(void)
{
   SpeexResamplerState *st = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   spx_uint32_t num, den;
   speex_resampler_get_ratio(st, &num, &den);
   TASK_CHECK(num == 2 && den == 3);

   /* Out of [1/2, 2], oversized terms and zeros are rejected. */
   TASK_CHECK(speex_resampler_set_rate_frac(st, 3, 1, RATE_IN, RATE_OUT) == RESAMPLER_ERR_INVALID_ARG);
   TASK_CHECK(speex_resampler_set_rate_frac(st, 1, 3, RATE_IN, RATE_OUT) == RESAMPLER_ERR_INVALID_ARG);
   TASK_CHECK(speex_resampler_set_rate_frac(st, 1000001, 1000000, RATE_IN, RATE_OUT) == RESAMPLER_ERR_INVALID_ARG);
   TASK_CHECK(speex_resampler_set_rate_frac(st, 1000000, 1000001, RATE_IN, RATE_OUT) == RESAMPLER_ERR_INVALID_ARG);
   TASK_CHECK(speex_resampler_set_rate_frac(st, 0, 1, RATE_IN, RATE_OUT) == RESAMPLER_ERR_INVALID_ARG);
   TASK_CHECK(speex_resampler_set_rate_frac(st, 1, 0, RATE_IN, RATE_OUT) == RESAMPLER_ERR_INVALID_ARG);
   /* Unreduced oversized fraction reduces inside the limit and is accepted. */
   TASK_CHECK(speex_resampler_set_rate_frac(st, 2000000, 2000000, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
   speex_resampler_get_ratio(st, &num, &den);
   TASK_CHECK(num == 1 && den == 1);

   /* Boundaries 1/2 and 2/1 are accepted; the earlier rejections left the
      live ratio intact at each step. */
   TASK_CHECK(speex_resampler_set_rate_frac(st, 1, 2, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
   speex_resampler_get_ratio(st, &num, &den);
   TASK_CHECK(num == 1 && den == 2);
   TASK_CHECK(speex_resampler_set_rate_frac(st, 2, 1, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
   speex_resampler_get_ratio(st, &num, &den);
   TASK_CHECK(num == 2 && den == 1);
   TASK_CHECK(speex_resampler_set_rate_frac(st, 2, 3, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);

   /* A rejection after processing preserves the cursor. */
   float in[256], out[32];
   for (int i = 0; i < 256; i++) in[i] = 0.1f;
   spx_uint32_t il = 256, ol = 32;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   uint64_t w0, f0;
   TASK_CHECK(cursor_get(st, 0, &w0, &f0) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_set_rate_frac(st, 9, 1, RATE_IN, RATE_OUT) == RESAMPLER_ERR_INVALID_ARG);
   uint64_t w1, f1;
   TASK_CHECK(cursor_get(st, 0, &w1, &f1) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w0 == w1 && f0 == f1);
   speex_resampler_destroy(st);

   /* Precise mode cannot be enabled on a state whose initial ratio is outside
      the supported range. */
   int eerr = 0;
   SpeexResamplerState *wide = speex_resampler_init_frac(1, 3, 1, RATE_IN, RATE_OUT, QUALITY, &eerr);
   TASK_CHECK(wide != NULL);
   TASK_CHECK(speex_resampler_enable_precise_clock(wide) == RESAMPLER_ERR_INVALID_ARG);
   /* It stays a working legacy resampler after the rejection. */
   spx_uint32_t wil = 64, wol = 32;
   float wout[32];
   TASK_CHECK(speex_resampler_process_float(wide, 0, in, &wil, wout, &wol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(wol > 0);
   TASK_CHECK(speex_resampler_get_precise_clock(wide, 0, &w0, &f0) == RESAMPLER_ERR_BAD_STATE);
   speex_resampler_destroy(wide);
   return 0;
}

static int test_quality_and_enable_states(void)
{
   int err = 0;
   SpeexResamplerState *st = speex_resampler_init_frac(1, 2, 3, RATE_IN, RATE_OUT, QUALITY, &err);
   TASK_CHECK(st != NULL);
   /* Enabling twice is harmless and still cannot be disabled (no disable API
      exists); a fresh instance accepts it. */
   TASK_CHECK(speex_resampler_enable_precise_clock(st) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_enable_precise_clock(st) == RESAMPLER_ERR_SUCCESS);

   float in[256], out[32];
   for (int i = 0; i < 256; i++) in[i] = 0.1f * sinf(0.04f * i);
   spx_uint32_t il = 256, ol = 32;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);

   /* Quality changes mid-stream are rejected, including out-of-range values. */
   TASK_CHECK(speex_resampler_set_quality(st, 8) == RESAMPLER_ERR_BAD_STATE);
   TASK_CHECK(speex_resampler_set_quality(st, 11) == RESAMPLER_ERR_INVALID_ARG);
   int got_q = -1;
   speex_resampler_get_quality(st, &got_q);
   TASK_CHECK(got_q == QUALITY);

   /* After reset_mem quality may be re-selected. */
   TASK_CHECK(speex_resampler_reset_mem(st) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_set_quality(st, 9) == RESAMPLER_ERR_SUCCESS);
   speex_resampler_get_quality(st, &got_q);
   TASK_CHECK(got_q == 9);

   speex_resampler_destroy(st);

   /* Precise mode cannot be enabled once input has been processed. */
   SpeexResamplerState *late = speex_resampler_init_frac(1, 2, 3, RATE_IN, RATE_OUT, QUALITY, &err);
   TASK_CHECK(late != NULL);
   il = 64; ol = 8;
   TASK_CHECK(speex_resampler_process_float(late, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_enable_precise_clock(late) == RESAMPLER_ERR_BAD_STATE);
   TASK_CHECK(speex_resampler_reset_mem(late) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_enable_precise_clock(late) == RESAMPLER_ERR_SUCCESS);
   speex_resampler_destroy(late);
   return 0;
}

static int test_query_errors(void)
{
   SpeexResamplerState *st = make_precise(2, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   uint64_t w, f;
   TASK_CHECK(speex_resampler_get_precise_clock(st, 2, &w, &f) == RESAMPLER_ERR_INVALID_ARG);
   TASK_CHECK(speex_resampler_get_precise_clock(st, 0, NULL, &f) == RESAMPLER_ERR_INVALID_ARG);
   TASK_CHECK(speex_resampler_get_precise_clock(st, 0, &w, NULL) == RESAMPLER_ERR_INVALID_ARG);
   TASK_CHECK(speex_resampler_get_precise_clock(NULL, 0, &w, &f) == RESAMPLER_ERR_BAD_STATE);

   /* A non-enabled state reports BAD_STATE. */
   int err = 0;
   SpeexResamplerState *plain = speex_resampler_init_frac(1, 2, 3, RATE_IN, RATE_OUT, QUALITY, &err);
   TASK_CHECK(plain != NULL);
   TASK_CHECK(speex_resampler_get_precise_clock(plain, 0, &w, &f) == RESAMPLER_ERR_BAD_STATE);
   speex_resampler_destroy(plain);
   speex_resampler_destroy(st);
   return 0;
}

/* ======================================================================= */
/* audio group                                                             */
/* ======================================================================= */

/* Alternate 2/3 and 5/7 one output frame at a time and check the actual FIR
   output against a low-frequency sine evaluated at the reference cursor. */
static int test_sine_phase(void)
{
   enum { NFRAMES = 2000, NIN = 8192 };
   const double freq = 0.01;
   float in[NIN];
   for (int i = 0; i < NIN; i++) in[i] = (float)sin(2 * M_PI * freq * i);

   SpeexResamplerState *st = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   TASK_CHECK(speex_resampler_skip_zeros(st) == RESAMPLER_ERR_SUCCESS);

   struct ref_clock ref;
   ref_init(&ref);
   double maxerr = 0.0;
   spx_uint32_t pos = 0;
   for (int k = 0; k < NFRAMES; k++)
   {
      int use_b = k & 1;
      TASK_CHECK(speex_resampler_set_rate_frac(st, use_b ? 5 : 2, use_b ? 7 : 3,
                  RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
      float y = 0.0f;
      spx_uint32_t il = (spx_uint32_t)(NIN - pos), ol = 1;
      TASK_CHECK(speex_resampler_process_float(st, 0, in + pos, &il, &y, &ol) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(ol == 1);
      pos += il;
      if (k >= 256 && k < NFRAMES - 256)
      {
         /* Frame k is read at U measured just before this frame's advance. */
         uint64_t rw, rf;
         ref_get(&ref, &rw, &rf);
         double u = (double)rw + ldexp((double)rf, -64);
         double expect = sin(2 * M_PI * freq * u);
         double e = fabs((double)y - expect);
         if (e > maxerr) maxerr = e;
      }
      ref_advance(&ref, use_b);
   }
   TASK_CHECK(maxerr <= 2e-4);
   printf("    sine phase max abs error = %.3e\n", maxerr);

   /* The nominal cursor excludes startup filter delay, which is reported
      separately as latency. */
   SpeexResamplerState *fresh = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(fresh != NULL);
   TASK_CHECK(speex_resampler_skip_zeros(fresh) == RESAMPLER_ERR_SUCCESS);
   float y;
   spx_uint32_t il = 256, ol = 1;
   TASK_CHECK(speex_resampler_process_float(fresh, 0, in, &il, &y, &ol) == RESAMPLER_ERR_SUCCESS);
   uint64_t w, f;
   TASK_CHECK(cursor_get(fresh, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == 0 && f == ref_frac64(2, 3));
   TASK_CHECK(speex_resampler_get_input_latency(fresh) > 0);
   speex_resampler_destroy(fresh);
   speex_resampler_destroy(st);
   return 0;
}

/* Impulses and an inverted second channel across 1x and filter-window
   growth/shrink; magic samples must be produced and drained without losing
   or repeating sound. */
static int test_pulses_magic_inversion(void)
{
   enum { CH = 2, NIN = 6000, MAXOUT = 16384 };
   SpeexResamplerState *st = make_precise(CH, 8, 2, 3);
   SpeexResamplerState *ref = make_precise(CH, 8, 2, 3);
   TASK_CHECK(st != NULL && ref != NULL);

   float planar[CH][NIN];
   for (int i = 0; i < NIN; i++)
   {
      float s = (i % 47 == 0) ? 0.9f : (0.05f * sinf(0.03f * i));
      planar[0][i] = s;
      planar[1][i] = -s;
   }

   /* 2/3 -> 3/2 grows the window, 3/2 -> 2/3 shrinks it and creates magic
      samples, later growth drains them again. */
   const spx_uint32_t schedule[][2] = {
      {2,3}, {3,2}, {2,3}, {5,7}, {3,2}, {1,1}, {2,3}
   };
   float out0[MAXOUT], out1[MAXOUT], r0[MAXOUT], r1[MAXOUT];
   long total = 0, rtotal = 0;
   spx_uint32_t pos = 0, rpos = 0;
   int saw_nonzero_after_shrink = 0;

   for (unsigned step = 0; step < sizeof(schedule)/sizeof(schedule[0]); step++)
   {
      TASK_CHECK(speex_resampler_set_rate_frac(st, schedule[step][0], schedule[step][1],
                  RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(speex_resampler_set_rate_frac(ref, schedule[step][0], schedule[step][1],
                  RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
      spx_uint32_t il = (spx_uint32_t)(NIN - pos), ol = 900;
      TASK_CHECK(speex_resampler_process_float(st, 0, planar[0] + pos, &il, out0 + total, &ol) == RESAMPLER_ERR_SUCCESS);
      spx_uint32_t il1 = (spx_uint32_t)(NIN - pos), ol1 = ol;
      TASK_CHECK(speex_resampler_process_float(st, 1, planar[1] + pos, &il1, out1 + total, &ol1) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(il == il1 && ol == ol1);
      /* Reference instance: one output frame per call, same scheduling. */
      for (spx_uint32_t k = 0; k < ol; k++)
      {
         spx_uint32_t ci = (spx_uint32_t)(NIN - rpos), co = 1;
         TASK_CHECK(speex_resampler_process_float(ref, 0, planar[0] + rpos, &ci, r0 + rtotal, &co) == RESAMPLER_ERR_SUCCESS);
         spx_uint32_t ci1 = (spx_uint32_t)(NIN - rpos), co1 = 1;
         TASK_CHECK(speex_resampler_process_float(ref, 1, planar[1] + rpos, &ci1, r1 + rtotal, &co1) == RESAMPLER_ERR_SUCCESS);
         TASK_CHECK(ci == ci1 && co == 1 && co1 == 1);
         rpos += ci;
         rtotal++;
      }
      pos += il;
      for (spx_uint32_t k = 0; k < ol; k++)
      {
         TASK_CHECK(isfinite(out0[total + k]) && isfinite(out1[total + k]));
         /* Exact inversion must survive window scaling and magic handling. */
         TASK_CHECK(out1[total + k] == -out0[total + k]);
      }
      if (step >= 2)
      {
         for (spx_uint32_t k = 0; k < ol; k++)
            if (out0[total + k] != 0.0f) saw_nonzero_after_shrink = 1;
      }
      total += ol;
   }
   TASK_CHECK(total == rtotal);
   TASK_CHECK(saw_nonzero_after_shrink == 1);
   float pk = 0.0f;
   for (long k = 0; k < total; k++)
   {
      float d = fabsf(out0[k] - r0[k]);
      if (d > pk) pk = d;
   }
   TASK_CHECK(pk <= 2e-6f);

   /* Drain with silence: magic tail must come out and decay to zero. */
   float zeros[256];
   memset(zeros, 0, sizeof(zeros));
   int tail_nonzero = 0;
   for (int round = 0; round < 6; round++)
   {
      spx_uint32_t il = 256, ol = 256;
      TASK_CHECK(speex_resampler_process_float(st, 0, zeros, &il, out0 + total, &ol) == RESAMPLER_ERR_SUCCESS);
      spx_uint32_t il1 = 256, ol1 = ol;
      TASK_CHECK(speex_resampler_process_float(st, 1, zeros, &il1, out1 + total, &ol1) == RESAMPLER_ERR_SUCCESS);
      for (spx_uint32_t k = 0; k < ol; k++)
      {
         TASK_CHECK(isfinite(out0[total + k]) && isfinite(out1[total + k]));
         if (out0[total + k] != 0.0f) tail_nonzero = 1;
      }
      total += ol;
   }
   TASK_CHECK(tail_nonzero == 1);
   float tail_peak = 0.0f;
   for (long k = total - 64; k < total; k++)
   {
      float a = fabsf(out0[k]);
      if (a > tail_peak) tail_peak = a;
   }
   TASK_CHECK(tail_peak < 1e-6f);

   speex_resampler_destroy(st);
   speex_resampler_destroy(ref);
   return 0;
}

/* An A -> B -> A sequence with no generated frame in between is exactly
   equivalent to never changing the ratio, including the subsequent audio. */
static int test_rate_roundtrip_audio(void)
{
   enum { NIN = 4096, MAXOUT = 1024 };
   float in[NIN];
   for (int i = 0; i < NIN; i++) in[i] = 0.4f * sinf(2 * M_PI * 0.017 * i);

   SpeexResamplerState *ref = make_precise(1, 7, 2, 3);
   SpeexResamplerState *trip = make_precise(1, 7, 2, 3);
   TASK_CHECK(ref != NULL && trip != NULL);
   speex_resampler_skip_zeros(ref);
   speex_resampler_skip_zeros(trip);

   /* First chunk at A on both, identical. */
   float ro[MAXOUT], to[MAXOUT];
   spx_uint32_t ril = 2048, rol = MAXOUT, til = 2048, tol = MAXOUT;
   TASK_CHECK(speex_resampler_process_float(ref, 0, in, &ril, ro, &rol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_process_float(trip, 0, in, &til, to, &tol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(ril == til && rol == tol);
   for (spx_uint32_t k = 0; k < rol; k++) TASK_CHECK(ro[k] == to[k]);

   /* On trip only: B then A with no frame generated in between. */
   TASK_CHECK(speex_resampler_set_rate_frac(trip, 5, 7, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_set_rate_frac(trip, 2, 3, RATE_IN, RATE_OUT) == RESAMPLER_ERR_SUCCESS);

   /* Subsequent sound, several chunks, must stay bit identical. */
   size_t pos = ril;
   for (int round = 0; round < 3; round++)
   {
      spx_uint32_t give = 600;
      ril = give; til = give; rol = MAXOUT; tol = MAXOUT;
      TASK_CHECK(speex_resampler_process_float(ref, 0, in + pos, &ril, ro, &rol) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(speex_resampler_process_float(trip, 0, in + pos, &til, to, &tol) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(ril == til && rol == tol);
      for (spx_uint32_t k = 0; k < rol; k++) TASK_CHECK(ro[k] == to[k]);
      pos += ril;
   }
   /* Cursors end up identical too. */
   uint64_t rw, rf, tw, tf;
   TASK_CHECK(cursor_get(ref, 0, &rw, &rf) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(cursor_get(trip, 0, &tw, &tf) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(rw == tw && rf == tf);

   speex_resampler_destroy(ref);
   speex_resampler_destroy(trip);
   return 0;
}

/* Every supported quality 0..10 runs in precise mode and advances the clock
   by the ratio for each produced frame. */
static int test_quality_sweep(void)
{
   enum { NIN = 4096, MAXOUT = 4096 };
   float in[NIN], out[MAXOUT];
   for (int i = 0; i < NIN; i++) in[i] = 0.3f * sinf(0.02f * i);

   for (int q = 0; q <= 10; q++)
   {
      SpeexResamplerState *st = make_precise(1, q, 2, 3);
      TASK_CHECK(st != NULL);
      spx_uint32_t il = NIN, ol = MAXOUT;
      TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(ol > 0);
      for (spx_uint32_t k = 0; k < ol; k++)
         TASK_CHECK(isfinite(out[k]));
      uint64_t w, f;
      TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
      /* U advances by 2/3 per produced frame; whole part is exact. */
      TASK_CHECK(w == (uint64_t)ol * 2 / 3);
      speex_resampler_destroy(st);
   }
   return 0;
}

/* Integer output follows the same processing as float under the WORD2INT
   quantization rule. */
static int test_int_matches_float(void)
{
   enum { NIN = 2048, MAXOUT = 2048 };
   SpeexResamplerState *fs = make_precise(1, 6, 2, 3);
   SpeexResamplerState *is = make_precise(1, 6, 2, 3);
   TASK_CHECK(fs != NULL && is != NULL);
   speex_resampler_skip_zeros(fs);
   speex_resampler_skip_zeros(is);

   spx_int16_t iv[NIN];
   float fv[NIN];
   for (int i = 0; i < NIN; i++)
   {
      iv[i] = (spx_int16_t)((i % 101) - 50);
      fv[i] = (float)iv[i];
   }
   float fout[MAXOUT];
   spx_int16_t iout[MAXOUT];
   spx_uint32_t fil = NIN, fol = MAXOUT;
   spx_uint32_t iil = NIN, iol = MAXOUT;
   TASK_CHECK(speex_resampler_process_float(fs, 0, fv, &fil, fout, &fol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_process_int(is, 0, iv, &iil, iout, &iol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(fol == iol && fil == iil);
   for (spx_uint32_t k = 0; k < fol; k++)
      TASK_CHECK(iout[k] == quantize(fout[k]));

   speex_resampler_destroy(fs);
   speex_resampler_destroy(is);

   /* Both interleaved entries, two channels. */
   enum { ICH = 2, IIN = 1024, IOUT = 1024 };
   SpeexResamplerState *fms = make_precise(ICH, 6, 5, 7);
   SpeexResamplerState *ims = make_precise(ICH, 6, 5, 7);
   TASK_CHECK(fms != NULL && ims != NULL);
   spx_int16_t iiv[IIN * ICH];
   float fiv[IIN * ICH];
   for (int i = 0; i < IIN * ICH; i++)
   {
      iiv[i] = (spx_int16_t)(((i * 7) % 201) - 100);
      fiv[i] = (float)iiv[i];
   }
   float fmo[IOUT * ICH];
   spx_int16_t imo[IOUT * ICH];
   spx_uint32_t fmil = IIN, fmol = IOUT, imil = IIN, imol = IOUT;
   TASK_CHECK(speex_resampler_process_interleaved_float(fms, fiv, &fmil, fmo, &fmol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_process_interleaved_int(ims, iiv, &imil, imo, &imol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(fmol == imol && fmil == imil);
   for (spx_uint32_t k = 0; k < imol * ICH; k++)
      TASK_CHECK(imo[k] == quantize(fmo[k]));
   /* Both channels advanced together. */
   for (unsigned c = 0; c < ICH; c++)
   {
      uint64_t fw, ff, iw, imf;
      TASK_CHECK(cursor_get(fms, c, &fw, &ff) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(cursor_get(ims, c, &iw, &imf) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(fw == iw && ff == imf);
   }
   speex_resampler_destroy(fms);
   speex_resampler_destroy(ims);
   return 0;
}

static int test_channel_counts(void)
{
   const unsigned counts[] = {1, 2, 6};
   for (unsigned ci = 0; ci < sizeof(counts)/sizeof(counts[0]); ci++)
   {
      unsigned ch = counts[ci];
      enum { NIN = 512, MAXOUT = 512 };
      SpeexResamplerState *planar = make_precise(ch, QUALITY, 2, 3);
      SpeexResamplerState *inter = make_precise(ch, QUALITY, 2, 3);
      TASK_CHECK(planar != NULL && inter != NULL);

      float *pin = malloc((size_t)ch * NIN * sizeof(float));
      float *iin = malloc((size_t)ch * NIN * sizeof(float));
      float *pout = malloc((size_t)ch * MAXOUT * sizeof(float));
      float *iout = malloc((size_t)ch * MAXOUT * sizeof(float));
      TASK_CHECK(pin && iin && pout && iout);
      for (unsigned c = 0; c < ch; c++)
         for (int i = 0; i < NIN; i++)
         {
            float v = 0.2f * sinf(0.05f * i + c);
            pin[c * NIN + i] = v;
            iin[i * ch + c] = v;
         }

      spx_uint32_t frames = 0;
      spx_uint32_t planar_used = 0;
      for (unsigned c = 0; c < ch; c++)
      {
         spx_uint32_t il = NIN, ol = MAXOUT;
         TASK_CHECK(speex_resampler_process_float(planar, c, pin + c * NIN, &il,
                     pout + c * MAXOUT, &ol) == RESAMPLER_ERR_SUCCESS);
         if (c == 0) { frames = ol; planar_used = il; }
         else TASK_CHECK(ol == frames && il == planar_used);
      }
      spx_uint32_t iil = NIN, iol = MAXOUT;
      TASK_CHECK(speex_resampler_process_interleaved_float(inter, iin, &iil, iout, &iol) == RESAMPLER_ERR_SUCCESS);
      TASK_CHECK(iol == frames && iil == planar_used);

      float pk = 0.0f;
      for (unsigned c = 0; c < ch; c++)
         for (spx_uint32_t k = 0; k < frames; k++)
         {
            float d = fabsf(pout[c * MAXOUT + k] - iout[k * ch + c]);
            if (d > pk) pk = d;
         }
      TASK_CHECK(pk <= 2e-6f);

      /* Every channel cursor advanced together; the two states match. */
      for (unsigned c = 0; c < ch; c++)
      {
         uint64_t pw, pf, iw, imf;
         TASK_CHECK(cursor_get(planar, c, &pw, &pf) == RESAMPLER_ERR_SUCCESS);
         TASK_CHECK(cursor_get(inter, c, &iw, &imf) == RESAMPLER_ERR_SUCCESS);
         TASK_CHECK(pw == iw && pf == imf);
         if (c > 0)
         {
            uint64_t w0, f0;
            TASK_CHECK(cursor_get(planar, 0, &w0, &f0) == RESAMPLER_ERR_SUCCESS);
            TASK_CHECK(pw == w0 && pf == f0);
         }
      }
      free(pin); free(iin); free(pout); free(iout);
      speex_resampler_destroy(planar);
      speex_resampler_destroy(inter);
   }
   return 0;
}

/* ======================================================================= */
/* streaming group                                                          */
/* ======================================================================= */

struct schedule_pt { long frame; uint32_t num, den; };

/* Drive one state through a per-output-frame ratio schedule using the given
   input and output chunk patterns.  Planar (per-channel) or interleaved I/O. */
struct stream_result {
   float *out;        /* planar, ch * cap */
   long frames;
   size_t in_used;
   uint64_t cursor_whole;
   uint64_t cursor_frac;
};

static void apply_schedule(SpeexResamplerState *st, long produced,
                           const struct schedule_pt *sched, unsigned nseg)
{
   /* Segments are frame boundaries; pick the last segment whose start frame
      has been reached so ratios follow produced frames, not call counts. */
   const struct schedule_pt *chosen = &sched[0];
   for (unsigned i = 0; i < nseg; i++)
      if (produced >= sched[i].frame)
         chosen = &sched[i];
   speex_resampler_set_rate_frac(st, chosen->num, chosen->den, RATE_IN, RATE_OUT);
}

/* Number of output frames from produced up to (but not crossing) the next
   ratio boundary.  Capping every call at this keeps one ratio per call while
   still exercising arbitrary chunking inside a segment. */
static long frames_until_boundary(long produced, long target,
                                  const struct schedule_pt *sched, unsigned nseg)
{
   long best = target - produced;
   for (unsigned i = 0; i < nseg; i++)
   {
      long d = sched[i].frame - produced;
      if (d > 0 && d < best)
         best = d;
   }
   return best;
}

/* Every variant gets the identical bulk warmup, so the filter memory is
   fully populated before the arbitrary chunking begins.  Without this, a
   one-sample first call would emit zero-padded startup outputs that a bulk
   call never produces (a priming artifact unrelated to the clock).  Must
   stay inside the first schedule segment. */
#define STREAM_WARMUP_FRAMES 320
#define STREAM_WARMUP_INPUT  512

static int run_stream(unsigned ch, int interleaved,
                      const int *in_pattern, unsigned n_inpat,
                      const int *out_pattern, unsigned n_outpat,
                      const float *planar_in, size_t total_in,
                      const struct schedule_pt *sched, unsigned nseg,
                      long target, struct stream_result *res)
{
   SpeexResamplerState *st = make_precise(ch, QUALITY, 2, 3);
   if (!st) return -1;
   res->out = (float *)malloc((size_t)ch * (size_t)target * sizeof(float));
   if (!res->out) { speex_resampler_destroy(st); return -1; }
   res->frames = 0;
   res->in_used = 0;
   /* Planar processing reads channel c out of the interleaved input array. */
   if (!interleaved)
      speex_resampler_set_input_stride(st, ch);

   size_t pin = 0;
   unsigned ipi = 0, opi = 0;

   /* Identical bulk warmup for every variant. */
   apply_schedule(st, res->frames, sched, nseg);
   if (interleaved)
   {
      spx_uint32_t il = STREAM_WARMUP_INPUT, ol = STREAM_WARMUP_FRAMES;
      if (speex_resampler_process_interleaved_float(st, planar_in, &il,
              res->out, &ol) != RESAMPLER_ERR_SUCCESS || ol != STREAM_WARMUP_FRAMES)
      { speex_resampler_destroy(st); return -1; }
      pin = il;
      res->frames = ol;
   } else {
      spx_uint32_t warm_ol = 0;
      for (unsigned c = 0; c < ch; c++)
      {
         spx_uint32_t il = STREAM_WARMUP_INPUT, ol = STREAM_WARMUP_FRAMES;
         if (speex_resampler_process_float(st, c, planar_in + c, &il,
                 res->out + c * target, &ol) != RESAMPLER_ERR_SUCCESS
             || ol != STREAM_WARMUP_FRAMES)
         { speex_resampler_destroy(st); return -1; }
         if (c == 0) { pin = il; warm_ol = ol; }
         else if (il != (spx_uint32_t)pin) { speex_resampler_destroy(st); return -1; }
      }
      res->frames = warm_ol;
   }

   while (res->frames < target)
   {
      apply_schedule(st, res->frames, sched, nseg);
      spx_uint32_t want_out = (spx_uint32_t)frames_until_boundary(res->frames, target, sched, nseg);
      if (out_pattern)
      {
         int p = out_pattern[opi++ % n_outpat];
         if ((long)p < (long)want_out) want_out = (spx_uint32_t)p;
      }
      spx_uint32_t avail_in = (spx_uint32_t)(total_in - pin);
      spx_uint32_t give_in = avail_in;
      if (in_pattern)
      {
         int p = in_pattern[ipi++ % n_inpat];
         if ((spx_uint32_t)p < give_in) give_in = (spx_uint32_t)p;
      }

      if (interleaved)
      {
         spx_uint32_t il = give_in, ol = want_out;
         int rc = speex_resampler_process_interleaved_float(st,
                  give_in ? planar_in + pin * ch : NULL, &il,
                  res->out + res->frames * ch, &ol);
         if (rc != RESAMPLER_ERR_SUCCESS) { speex_resampler_destroy(st); return -1; }
         pin += il;
         res->frames += ol;
      } else {
         spx_uint32_t common_ol = 0;
         spx_uint32_t base_il = 0;
         for (unsigned c = 0; c < ch; c++)
         {
            spx_uint32_t il = give_in, ol = want_out;
            /* All channels read from the same base offset; pin must not move
               until every channel has consumed its chunk. */
            int rc = speex_resampler_process_float(st, c,
                     give_in ? planar_in + pin * ch + c : NULL, &il,
                     res->out + c * target + res->frames, &ol);
            if (rc != RESAMPLER_ERR_SUCCESS) { speex_resampler_destroy(st); return -1; }
            if (c == 0) { base_il = il; common_ol = ol; }
            else if (ol != common_ol || il != base_il) { speex_resampler_destroy(st); return -1; }
         }
         pin += base_il;
         res->frames += common_ol;
      }
   }
   res->in_used = pin;
   /* The actual read position is the deterministic quantity; the number of
      input frames handed to the API also depends on the buffering chunk
      size. */
   if (speex_resampler_get_precise_clock(st, 0, &res->cursor_whole,
                                         &res->cursor_frac) != RESAMPLER_ERR_SUCCESS)
   {
      speex_resampler_destroy(st);
      return -1;
   }
   speex_resampler_destroy(st);
   return 0;
}

static int test_chunking_equivalence(void)
{
   enum { CH = 2, TARGET = 3000 };
   const size_t total_in = (size_t)TARGET * 2 + 512;
   float *planar_in = malloc(total_in * CH * sizeof(float));
   TASK_CHECK(planar_in != NULL);
   for (size_t i = 0; i < total_in; i++)
      for (unsigned c = 0; c < CH; c++)
         planar_in[i * CH + c] = 0.3f * sinf(0.017f * (float)i + 0.5f * c);

   /* Ratios are scheduled on output-frame boundaries, never on call count. */
   const struct schedule_pt sched[] = {
      {0, 2, 3}, {1000, 5, 7}, {2000, 1, 1}, {2500, 2, 3}
   };
   const int pat1[] = {1};
   const int pat7[] = {7};
   const int pat113[] = {113};
   const int pat_mix[] = {1, 7, 113};
   const int pat_out_small[] = {3, 11};

   struct stream_result whole, in1, in7, in113, outmix, planar_mix, inter_mix;
   TASK_CHECK(run_stream(CH, 1, NULL, 0, NULL, 0, planar_in, total_in, sched, 4, TARGET, &whole) == 0);
   TASK_CHECK(run_stream(CH, 1, pat1, 1, NULL, 0, planar_in, total_in, sched, 4, TARGET, &in1) == 0);
   TASK_CHECK(run_stream(CH, 1, pat7, 1, NULL, 0, planar_in, total_in, sched, 4, TARGET, &in7) == 0);
   TASK_CHECK(run_stream(CH, 1, pat113, 1, NULL, 0, planar_in, total_in, sched, 4, TARGET, &in113) == 0);
   TASK_CHECK(run_stream(CH, 1, NULL, 0, pat_out_small, 2, planar_in, total_in, sched, 4, TARGET, &outmix) == 0);
   TASK_CHECK(run_stream(CH, 0, pat_mix, 3, pat_mix, 3, planar_in, total_in, sched, 4, TARGET, &planar_mix) == 0);
   TASK_CHECK(run_stream(CH, 1, pat_mix, 3, pat_mix, 3, planar_in, total_in, sched, 4, TARGET, &inter_mix) == 0);

   struct stream_result *all[] = {&whole, &in1, &in7, &in113, &outmix, &planar_mix, &inter_mix};
   for (unsigned i = 0; i < sizeof(all)/sizeof(all[0]); i++)
   {
      TASK_CHECK(all[i]->frames == TARGET);
      /* The read cursor must be identical regardless of chunking. */
      TASK_CHECK(all[i]->cursor_whole == whole.cursor_whole
                 && all[i]->cursor_frac == whole.cursor_frac);
   }

   /* Interleaved and planar use different output layouts; normalize planar to
      interleaved before comparing. */
   float *ref_inter = malloc((size_t)TARGET * CH * sizeof(float));
   TASK_CHECK(ref_inter != NULL);
   for (long k = 0; k < TARGET; k++)
      for (unsigned c = 0; c < CH; c++)
         ref_inter[k * CH + c] = whole.out[k * CH + c];

   const char *names[] = {"whole","in1","in7","in113","outmix","planar_mix","inter_mix"};
   for (unsigned i = 0; i < sizeof(all)/sizeof(all[0]); i++)
   {
      float pk = 0.0f;
      for (long k = 0; k < TARGET; k++)
         for (unsigned c = 0; c < CH; c++)
         {
            float a, b;
            if (i == 5) /* planar_mix layout */
               a = all[i]->out[c * TARGET + k];
            else
               a = all[i]->out[k * CH + c];
            b = ref_inter[k * CH + c];
            float d = fabsf(a - b);
            if (d > pk) pk = d;
         }
      printf("    chunking %-10s max diff = %.3e\n", names[i], pk);
      TASK_CHECK(pk <= 2e-6f);
   }

   for (unsigned i = 0; i < sizeof(all)/sizeof(all[0]); i++)
      free(all[i]->out);
   free(ref_inter);
   free(planar_in);
   return 0;
}

static int test_stride_sentinels(void)
{
   enum { NIN = 600, MAXOUT = 600 };
   SpeexResamplerState *packed = make_precise(1, QUALITY, 2, 3);
   SpeexResamplerState *strided = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(packed != NULL && strided != NULL);
   speex_resampler_set_input_stride(strided, 2);
   speex_resampler_set_output_stride(strided, 2);

   float pin[NIN], sin[2 * NIN];
   float pout[MAXOUT], sout[2 * MAXOUT];
   for (int i = 0; i < NIN; i++)
   {
      pin[i] = 0.25f * sinf(0.04f * i);
      sin[2 * i] = pin[i];
      sin[2 * i + 1] = 777.0f; /* sentinel */
   }
   for (int i = 0; i < 2 * MAXOUT; i++) sout[i] = -888.0f;

   spx_uint32_t pil = NIN, pol = MAXOUT;
   spx_uint32_t sil = NIN, sol = MAXOUT;
   TASK_CHECK(speex_resampler_process_float(packed, 0, pin, &pil, pout, &pol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_process_float(strided, 0, sin, &sil, sout, &sol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(pol == sol && pil == sil);
   for (spx_uint32_t k = 0; k < sol; k++)
   {
      TASK_CHECK(fabsf(sout[2 * k] - pout[k]) <= 2e-6f);
      TASK_CHECK(sout[2 * k + 1] == -888.0f); /* output gap untouched */
   }
   for (int i = 0; i < NIN; i++)
      TASK_CHECK(sin[2 * i + 1] == 777.0f); /* input gap untouched */

   speex_resampler_destroy(packed);
   speex_resampler_destroy(strided);
   return 0;
}

static int test_zero_capacity_and_empty(void)
{
   SpeexResamplerState *st = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   float in[256], out[32];
   for (int i = 0; i < 256; i++) in[i] = 0.1f;

   spx_uint32_t il = 256, ol = 16;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   uint64_t w0, f0;
   TASK_CHECK(cursor_get(st, 0, &w0, &f0) == RESAMPLER_ERR_SUCCESS);

   spx_uint32_t zero = 0;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &zero) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(zero == 0);
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &zero, out, &zero) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_process_float(st, 0, NULL, &zero, out, &zero) == RESAMPLER_ERR_SUCCESS);
   uint64_t w, f;
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == w0 && f == f0);

   /* Interleaved zero capacity behaves the same. */
   SpeexResamplerState *ms = make_precise(2, QUALITY, 2, 3);
   TASK_CHECK(ms != NULL);
   float mi[64], mo[64];
   spx_uint32_t mz = 0;
   TASK_CHECK(speex_resampler_process_interleaved_float(ms, mi, &mz, mo, &mz) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(cursor_get(ms, 1, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == 0 && f == 0);
   speex_resampler_destroy(ms);
   speex_resampler_destroy(st);
   return 0;
}

static int test_reset_reuse(void)
{
   SpeexResamplerState *st = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   float in[512], out[128];
   for (int i = 0; i < 512; i++) in[i] = 0.2f * sinf(0.05f * i);
   spx_uint32_t il = 512, ol = 64;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(ol == 64);

   TASK_CHECK(speex_resampler_reset_mem(st) == RESAMPLER_ERR_SUCCESS);
   uint64_t w, f;
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == 0 && f == 0);

   /* History is gone: silence in gives zero-valued outputs, but the clock
      still advances because real frames are produced. */
   float zeros[512];
   memset(zeros, 0, sizeof(zeros));
   il = 512; ol = 128;
   TASK_CHECK(speex_resampler_process_float(st, 0, zeros, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(ol > 0);
   float pk = 0.0f;
   for (spx_uint32_t k = 0; k < ol; k++) pk = fabsf(out[k]) > pk ? fabsf(out[k]) : pk;
   TASK_CHECK(pk == 0.0f);

   /* Enabled state and ratio survive the reset. */
   spx_uint32_t num, den;
   speex_resampler_get_ratio(st, &num, &den);
   TASK_CHECK(num == 2 && den == 3);
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   /* U equals ol * 2/3.  Each 64.64 step truncates by under one ULP, so the
      integer part matches the rational value exactly and the fraction drifts
      by at most one ULP per produced frame (far inside the 2^-40 budget). */
   {
      uint64_t exp_whole = (uint64_t)ol * 2 / 3;
      uint64_t exp_frac = ref_frac64((uint32_t)((uint64_t)ol * 2 % 3), 3);
      uint64_t tol = (uint64_t)ol + 1;
      uint64_t d = f > exp_frac ? f - exp_frac : exp_frac - f;
      TASK_CHECK(w == exp_whole && d <= tol);
   }

   /* Re-reset so the comparison below starts from U = 0 with empty history. */
   TASK_CHECK(speex_resampler_reset_mem(st) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(cursor_get(st, 0, &w, &f) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w == 0 && f == 0);

   /* The reset state is reusable and produces the same stream as a brand new
      precise instance fed the same signal. */
   SpeexResamplerState *fresh = make_precise(1, QUALITY, 2, 3);
   TASK_CHECK(fresh != NULL);
   float a[128], b[128];
   il = 512; spx_uint32_t il2 = 512; ol = 128; spx_uint32_t ol2 = 128;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, a, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(speex_resampler_process_float(fresh, 0, in, &il2, b, &ol2) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(ol == ol2 && il == il2);
   float dpk = 0.0f;
   for (spx_uint32_t k = 0; k < ol; k++)
   {
      float d = fabsf(a[k] - b[k]);
      if (d > dpk) dpk = d;
   }
   TASK_CHECK(dpk <= 2e-6f);
   speex_resampler_destroy(fresh);
   speex_resampler_destroy(st);
   return 0;
}

static int test_independent_channel_advance(void)
{
   SpeexResamplerState *st = make_precise(2, QUALITY, 2, 3);
   TASK_CHECK(st != NULL);
   float in[256], out[64];
   for (int i = 0; i < 256; i++) in[i] = 0.15f;

   /* Only channel 0 is processed. */
   spx_uint32_t il = 256, ol = 32;
   TASK_CHECK(speex_resampler_process_float(st, 0, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(ol == 32);
   uint64_t w0, f0, w1, f1;
   TASK_CHECK(cursor_get(st, 0, &w0, &f0) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(cursor_get(st, 1, &w1, &f1) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK((w0 != 0 || f0 != 0) && w1 == 0 && f1 == 0);

   /* Channel 1 now starts from zero independently. */
   /* Channel 1 now starts from zero independently: 8 frames at 2/3 = 5+1/3. */
   il = 256; ol = 8;
   TASK_CHECK(speex_resampler_process_float(st, 1, in, &il, out, &ol) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(cursor_get(st, 1, &w1, &f1) == RESAMPLER_ERR_SUCCESS);
   TASK_CHECK(w1 == 5 && frac_close(f1, ref_frac64(1, 3)));
   speex_resampler_destroy(st);
   return 0;
}

/* ======================================================================= */
/* registration                                                            */
/* ======================================================================= */

const struct task_case task_cases[] = {
   {"arithmetic", "cursor_basics", test_cursor_basics},
   {"arithmetic", "long_accumulation_2_pow_20", test_long_accumulation},
   {"arithmetic", "roundtrip_without_output", test_roundtrip_no_output},
   {"arithmetic", "ratio_validation_preserves_state", test_ratio_validation},
   {"arithmetic", "quality_and_enable_lifecycle", test_quality_and_enable_states},
   {"arithmetic", "query_argument_validation", test_query_errors},

   {"audio", "sine_phase_tracking", test_sine_phase},
   {"audio", "rate_roundtrip_audio_equivalence", test_rate_roundtrip_audio},
   {"audio", "pulses_magic_and_inversion", test_pulses_magic_inversion},
   {"audio", "quality_0_to_10_sweep", test_quality_sweep},
   {"audio", "int_matches_float_quantization", test_int_matches_float},
   {"audio", "channel_counts_1_2_6", test_channel_counts},

   {"streaming", "chunking_equivalence", test_chunking_equivalence},
   {"streaming", "stride_sentinels", test_stride_sentinels},
   {"streaming", "zero_capacity_and_empty_feed", test_zero_capacity_and_empty},
   {"streaming", "reset_mem_reuse", test_reset_reuse},
   {"streaming", "independent_channel_cursors", test_independent_channel_advance},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
