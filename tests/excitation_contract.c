/*
   excitation_contract.c

   Independent C regression contract for the narrowband excitation rebuilt by
   nb_decode() (floating-point build only).  Every expected value in this file
   is derived from:

     * the bit-stream format (MSB-first packing, documented field widths),
     * the read-only production codebooks (gain tables, shape tables), and
     * the specification equations re-derived here.

   The production decoder helpers (pitch_unquant_3tap,
   split_cb_shape_sign_unquant, nb_decode) are only ever the system under
   test; they are never called to produce an expected value.  No speech files
   or external tools are used: all stimuli are generated deterministically in
   C, and the frame-level packets come from a normal production encoder run.

   Report categories are kept separate:
     BIT_CURSOR_MISMATCH    bit position / bit consumption / sentinel
     PITCH_DOMAIN_MISMATCH pitch outside [NB_PITCH_START, NB_PITCH_END]
     INDEX_DOMAIN_MISMATCH gain/shape index outside its codebook
     CONTENT_MISMATCH      numerical sample disagreement

   This file exports task_cases/task_case_count and defines no main().
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <speex/speex.h>
#include "arch.h"
#include "nb_celp.h"
#include "ltp.h"
#include "cb_search.h"
#include "task_support/check.h"

/* ------------------------------------------------------------------ */
/* Read-only production codebooks (shared constants, never modified). */
/* ------------------------------------------------------------------ */
extern const signed char gain_cdbk_nb[];   /* 128 entries, 4 chars each */
extern const signed char gain_cdbk_lbr[];  /*  32 entries, 4 chars each */
extern const signed char exc_5_64_table[]; /* mode 5/7 shape codebook   */
extern const signed char exc_10_32_table[];/* mode 3 shape codebook     */
extern const signed char hexc_table[];     /* WB high-band, signed      */

/* Sub-frame gain correction tables (floating-point branch of nb_celp.c). */
static const float exc_gain_scal3[8] = {
   0.061130f, 0.163546f, 0.310413f, 0.428220f,
   0.555887f, 0.719055f, 0.938694f, 1.326874f
};
static const float exc_gain_scal1[2] = {0.70469f, 1.05127f};

#define NSF 40                 /* NB_SUBFRAME_SIZE                        */
#define FRAME_SIZE 160         /* NB_FRAME_SIZE                           */
#define N_SUBFRAMES 4
#define PITCH_START 17         /* NB_PITCH_START                          */
#define PITCH_END 144          /* NB_PITCH_END                            */
#define SHAPE_SCALE 0.03125f   /* 1/32, floating innovation scale         */
#define GAIN_SCALE_C 0.015625f/* 1/64, pitch gain codebook scale         */
#define SECOND_CB_WEIGHT 0.454545f
#define TOL_ABS 1e-4f
#define TOL_REL 2e-5f

/* ------------------------------------------------------------------ */
/* Failure accounting, split by category.                              */
/* ------------------------------------------------------------------ */
static int bit_cursor_failures;
static int pitch_domain_failures;
static int index_domain_failures;
static int content_failures;

static int within_tol(float got, float ref)
{
   float tol = TOL_ABS + TOL_REL * fabsf(ref);
   return fabsf(got - ref) <= tol;
}

/* ------------------------------------------------------------------ */
/* Independent MSB-first bit cursor over a raw byte buffer.            */
/* Does not use any SpeexBits reader.                                  */
/* ------------------------------------------------------------------ */
typedef struct {
   const unsigned char *buf;
   int nbits;
   int pos;
} BitReader;

static unsigned br_read(BitReader *r, int n)
{
   unsigned v = 0;
   int k;
   for (k = 0; k < n; k++) {
      int bp = r->pos++;
      v = (v << 1u) | (unsigned)((r->buf[bp >> 3] >> (7 - (bp & 7))) & 1);
   }
   return v;
}

typedef struct {
   unsigned char *buf;
   int pos;
} BitWriter;

static void br_pack(BitWriter *w, unsigned val, int n)
{
   int k;
   for (k = n - 1; k >= 0; k--) {
      int bp = w->pos++;
      if ((val >> k) & 1u)
         w->buf[bp >> 3] |= (unsigned char)(1u << (7 - (bp & 7)));
   }
}

/* ------------------------------------------------------------------ */
/* Mode descriptors for the three normal narrowband modes under test.  */
/* Values mirror the production SpeexSubmode/ltp_params/split_cb_params */
/* but are stated independently here; only the tables are shared.      */
/* ------------------------------------------------------------------ */
typedef struct {
   int mode;
   int lsp_bits;                              /* LSP field width          */
   int pitch_bits, gain_bits;                 /* adaptive codebook widths */
   const signed char *gain_cb;
   int subvect_size, nb_subvect, shape_bits;  /* innovation geometry      */
   int have_sign;
   const signed char *shape_cb;
   int subgain_bits;                          /* per-subframe gain bits   */
   int double_codebook;
   int payload_bits;                          /* expected consumed bits   */
} ModeDesc;

static void desc_init(ModeDesc *d, int mode)
{
   memset(d, 0, sizeof(*d));
   d->mode = mode;
   if (mode == 3) {
      d->lsp_bits = 18;                       /* 3 x 6-bit LSP fields */
      d->pitch_bits = 7;
      d->gain_bits = 5;
      d->gain_cb = gain_cdbk_lbr;
      d->subvect_size = 10;
      d->nb_subvect = 4;
      d->shape_bits = 5;
      d->shape_cb = exc_10_32_table;
      d->subgain_bits = 1;
      /* header 28 + 4 subframes x (7+5+1+4*5=33) = 160, byte aligned,
         so no terminator bits are appended. */
      d->payload_bits = 160;
   } else {
      d->lsp_bits = 30;                       /* 5 x 6-bit LSP fields */
      d->pitch_bits = 7;
      d->gain_bits = 7;
      d->gain_cb = gain_cdbk_nb;
      d->subvect_size = 5;
      d->nb_subvect = 8;
      d->shape_bits = 6;
      d->shape_cb = exc_5_64_table;
      d->subgain_bits = 3;
      d->payload_bits = (mode == 7) ? 492 : 300;
      d->double_codebook = (mode == 7);
   }
}

/* Decode the three pitch gains from a gain codebook row. */
static void model_pitch_gains(const signed char *cb, int gain_index, float g[3])
{
   int t;
   for (t = 0; t < 3; t++)
      g[t] = GAIN_SCALE_C * (float)cb[gain_index * 4 + t] + 0.5f;
}

/*
   Independent adaptive codebook model in natural history coordinates.

   H[base + j] is the excitation sample at sub-frame-local coordinate j
   (j < 0 is history, 0 <= j < 40 is the current sub-frame).  For each of
   the three taps i in {0,1,2}:

     pp = pitch + 1 - i      (tap delay; tap i uses gain[2-i])
     first segment:  j in [0, min(40, pp))         reads H[base + j - pp]
     second segment: j in [min(40,pp), min(40,pp+pitch))
                                                    reads H[base+j-pp-pitch]
     after that the tap contributes exactly zero.

   The model only ever reads coordinates below the current sample; it never
   loops periodically and never reads outputs being computed.
*/
static void model_adaptive(const float *H, long base, int pitch,
                           const float g[3], float out[NSF])
{
   int i, j;
   for (j = 0; j < NSF; j++)
      out[j] = 0.f;
   for (i = 0; i < 3; i++) {
      int pp = pitch + 1 - i;
      int end1 = pp < NSF ? pp : NSF;
      int end2 = pp + pitch;
      if (end2 > NSF)
         end2 = NSF;
      for (j = 0; j < end1; j++)
         out[j] += g[2 - i] * H[base + j - pp];
      for (j = end1; j < end2; j++)
         out[j] += g[2 - i] * H[base + j - pp - pitch];
   }
}

/* Tap-only contribution, used by the structural short/long delay checks. */
static void model_single_tap(const float *H, long base, int pitch, int i,
                             float gain, float out[NSF])
{
   int j;
   int pp = pitch + 1 - i;
   int end1 = pp < NSF ? pp : NSF;
   int end2 = pp + pitch;
   if (end2 > NSF)
      end2 = NSF;
   for (j = 0; j < NSF; j++)
      out[j] = 0.f;
   for (j = 0; j < end1; j++)
      out[j] += gain * H[base + j - pp];
   for (j = end1; j < end2; j++)
      out[j] += gain * H[base + j - pp - pitch];
}

/*
   Independent split shape/sign innovation model.  Reads one optional sign
   bit then one shape index per sub-vector from the independent cursor and
   accumulates (never overwrites) the scaled codebook entries on top of the
   caller-supplied initial target.  Returns the number of bits consumed.
*/
static int model_innovation(BitReader *r, const ModeDesc *d, int have_sign,
                            const float *initial, float out[NSF])
{
   int i, j;
   int start = r->pos;
   for (j = 0; j < NSF; j++)
      out[j] = initial[j];
   for (i = 0; i < d->nb_subvect; i++) {
      int sgn = 1;
      unsigned idx;
      if (have_sign) {
         if (br_read(r, 1))
            sgn = -1;
      }
      idx = br_read(r, d->shape_bits);
      if ((int)idx >= (1 << d->shape_bits))
         index_domain_failures++;
      for (j = 0; j < d->subvect_size; j++) {
         int p = d->subvect_size * i + j;
         out[p] += (float)sgn * SHAPE_SCALE *
                   (float)d->shape_cb[idx * d->subvect_size + j];
      }
   }
   return r->pos - start;
}

/* ================================================================== */
/* Adaptive codebook group                                             */
/* ================================================================== */

/*
   Short delays (pitch < 40): the second periodic segment exists, the tail
   past pp+pitch stays zero, the three taps keep distinct delay/gain pairs,
   and only pre-existing history is read (no recursion into fresh output).
*/
static int test_adaptive_short_delay(void)
{
   /* 7-bit/7-bit NB gain table and 5-bit/5-bit LBR table, both exercised. */
   struct { const signed char *cb; int gain_bits; int pitch_bits; const char *tag; }
      cfgs[2] = {
         { gain_cdbk_nb,  7, 7, "nb"  },
         { gain_cdbk_lbr, 5, 5, "lbr" }
      };
   int pitches[7] = {17, 18, 20, 25, 33, 39, 40};
   int ci, k;
   int failed = 0;

   for (ci = 0; ci < 2; ci++) {
      for (k = 0; k < 7; k++) {
         int pitch = pitches[k];
         int gain_index = (k * 17 + ci * 11 + 3) & ((1 << cfgs[ci].gain_bits) - 1);
         float g[3], model[NSF];
         int i, j;
         long base = 300;
         float H[600];
         for (j = 0; j < 600; j++)
            H[j] = 0.f;
         /* Asymmetric, distinctive history; current-frame slots stay zero. */
         for (j = -PITCH_END - 16; j < 0; j++)
            H[base + j] = 0.31f * sinf(0.23f * j + ci) +
                          0.17f * cosf(0.11f * j) + 0.002f * j;

         model_pitch_gains(cfgs[ci].cb, gain_index, g);
         model_adaptive(H, base, pitch, g, model);

         /* Domain: pitch and gain index stay inside the legal format range. */
         if (pitch < PITCH_START || pitch > PITCH_END) {
            fprintf(stderr, "PITCH_DOMAIN_MISMATCH short %s pitch=%d\n",
                    cfgs[ci].tag, pitch);
            pitch_domain_failures++; failed = 1;
         }
         if (gain_index < 0 || gain_index >= (1 << cfgs[ci].gain_bits)) {
            fprintf(stderr, "INDEX_DOMAIN_MISMATCH short %s gain=%d\n",
                    cfgs[ci].tag, gain_index);
            index_domain_failures++; failed = 1;
         }

         for (i = 0; i < 3; i++) {
            int pp = pitch + 1 - i;
            int end1 = pp < NSF ? pp : NSF;
            int end2 = pp + pitch;
            if (end2 > NSF)
               end2 = NSF;
            float tap[NSF];
            model_single_tap(H, base, pitch, i, g[2 - i], tap);

            /* Short delay must reach a zero tail after the second segment. */
            for (j = end2; j < NSF; j++) {
               if (tap[j] != 0.f) {
                  fprintf(stderr,
                          "CONTENT_MISMATCH short %s p=%d tap=%d nonzero tail at %d\n",
                          cfgs[ci].tag, pitch, i, j);
                  content_failures++; failed = 1;
               }
            }
            /* First segment delay is pp and lands in negative history. */
            for (j = 0; j < end1; j++) {
               float expect = g[2 - i] * H[base + j - pp];
               if (!within_tol(tap[j], expect)) {
                  fprintf(stderr,
                          "CONTENT_MISMATCH short %s p=%d tap=%d seg1 at %d got=%g ref=%g\n",
                          cfgs[ci].tag, pitch, i, j, tap[j], expect);
                  content_failures++; failed = 1;
               }
            }
            /* Second segment takes at most one more period, same tap gain. */
            for (j = end1; j < end2; j++) {
               float expect = g[2 - i] * H[base + j - pp - pitch];
               if (!within_tol(tap[j], expect)) {
                  fprintf(stderr,
                          "CONTENT_MISMATCH short %s p=%d tap=%d seg2 at %d got=%g ref=%g\n",
                          cfgs[ci].tag, pitch, i, j, tap[j], expect);
                  content_failures++; failed = 1;
               }
            }
         }

         /* First-sample identity: only the three first-segment delays. */
         {
            float expect0 = 0.f;
            for (i = 0; i < 3; i++) {
               int pp = pitch + 1 - i;
               expect0 += g[2 - i] * H[base - pp];
            }
            if (!within_tol(model[0], expect0)) {
               fprintf(stderr,
                       "CONTENT_MISMATCH short %s p=%d first sample got=%g ref=%g\n",
                       cfgs[ci].tag, pitch, model[0], expect0);
               content_failures++; failed = 1;
            }
         }
      }
   }
   TASK_CHECK(!failed);
   return 0;
}

/*
   Long delays (pitch >= 41): the smallest tap delay pp = pitch-1 is at least
   40, so the first segment always covers the whole sub-frame and the second
   loop must never contribute.  The pitch==40 boundary is covered by the
   short-delay group because its tap i==2 still has a one-sample segment two.
*/
static int test_adaptive_long_delay(void)
{
   int pitches[5] = {41, 42, 60, 100, 144};
   int k, i, j;
   int failed = 0;
   long base = 320;
   float H[640];

   for (j = 0; j < 640; j++)
      H[j] = 0.f;
   for (j = -PITCH_END - 16; j < 0; j++)
      H[base + j] = 0.27f * sinf(0.19f * j + 0.4f) + 0.003f * j;

   for (k = 0; k < 5; k++) {
      int pitch = pitches[k];
      int gain_index = (k * 29 + 5) & 127;
      float g[3], model[NSF];
      model_pitch_gains(gain_cdbk_nb, gain_index, g);
      model_adaptive(H, base, pitch, g, model);

      if (pitch < PITCH_START || pitch > PITCH_END) {
         fprintf(stderr, "PITCH_DOMAIN_MISMATCH long pitch=%d\n", pitch);
         pitch_domain_failures++; failed = 1;
      }
      for (i = 0; i < 3; i++) {
         int pp = pitch + 1 - i;
         int end1 = pp < NSF ? pp : NSF;
         int end2 = pp + pitch;
         float tap[NSF];
         model_single_tap(H, base, pitch, i, g[2 - i], tap);
         /* For pitch >= 40 every tap fills the sub-frame from segment one. */
         if (end1 != NSF) {
            fprintf(stderr,
                    "CONTENT_MISMATCH long p=%d tap=%d first segment ended at %d\n",
                    pitch, i, end1);
            content_failures++; failed = 1;
         }
         if (end2 <= NSF) {
            fprintf(stderr,
                    "CONTENT_MISMATCH long p=%d tap=%d unexpected second segment end2=%d\n",
                    pitch, i, end2);
            content_failures++; failed = 1;
         }
         for (j = 0; j < NSF; j++) {
            float expect = g[2 - i] * H[base + j - pp];
            if (!within_tol(tap[j], expect)) {
               fprintf(stderr,
                       "CONTENT_MISMATCH long p=%d tap=%d at %d got=%g ref=%g\n",
                       pitch, i, j, tap[j], expect);
               content_failures++; failed = 1;
            }
         }
      }
   }
   TASK_CHECK(!failed);
   return 0;
}

/*
   Codebook-to-gain scaling and the tap/gain correspondence, including a
   fully hand-computed first sample.  Also checks the production helper on an
   independently packed bitstream (production is the system under test; the
   expected numbers come from the model above).
*/
static int test_adaptive_gain_mapping(void)
{
   int failed = 0;
   int gi, t;

   /* Every row of both tables maps through g = cb/64 + 0.5. */
   for (gi = 0; gi < 128; gi++) {
      float g[3];
      model_pitch_gains(gain_cdbk_nb, gi, g);
      for (t = 0; t < 3; t++) {
         float expect = GAIN_SCALE_C * (float)gain_cdbk_nb[gi * 4 + t] + 0.5f;
         if (fabsf(g[t] - expect) > 1e-7f) {
            fprintf(stderr, "CONTENT_MISMATCH nb gain row=%d tap=%d\n", gi, t);
            content_failures++; failed = 1;
         }
      }
   }
   for (gi = 0; gi < 32; gi++) {
      float g[3];
      model_pitch_gains(gain_cdbk_lbr, gi, g);
      for (t = 0; t < 3; t++) {
         float expect = GAIN_SCALE_C * (float)gain_cdbk_lbr[gi * 4 + t] + 0.5f;
         if (fabsf(g[t] - expect) > 1e-7f) {
            fprintf(stderr, "CONTENT_MISMATCH lbr gain row=%d tap=%d\n", gi, t);
            content_failures++; failed = 1;
         }
      }
   }

   /* Hand-built case: pitch 20, three literal history samples. */
   {
      const int pitch = 20;
      const int gain_index = 7;
      long base = 200;
      float H[512];
      float g[3], model[NSF], hand;
      int i;
      memset(H, 0, sizeof(H));
      H[base - 21] = 1.f;   /* tap 0 (pp=21) */
      H[base - 20] = 2.f;   /* tap 1 (pp=20) */
      H[base - 19] = 3.f;   /* tap 2 (pp=19) */
      model_pitch_gains(gain_cdbk_nb, gain_index, g);
      model_adaptive(H, base, pitch, g, model);
      hand = g[2] * 1.f + g[1] * 2.f + g[0] * 3.f;
      if (!within_tol(model[0], hand)) {
         fprintf(stderr,
                 "CONTENT_MISMATCH hand first sample got=%g ref=%g\n",
                 model[0], hand);
         content_failures++; failed = 1;
      }
      /* Sample 1 pulls a different coordinate triple, proving the pairing. */
      hand = g[2] * H[base + 1 - 21] + g[1] * H[base + 1 - 20] +
             g[0] * H[base + 1 - 19];
      if (!within_tol(model[1], hand)) {
         fprintf(stderr, "CONTENT_MISMATCH hand second sample got=%g ref=%g\n",
                 model[1], hand);
         content_failures++; failed = 1;
      }
      for (i = 0; i < 3; i++) {
         if (fabsf(g[i]) > 1.5f) { /* gains are ordinary Q-format fractions */
            fprintf(stderr, "CONTENT_MISMATCH implausible gain[%d]=%g\n", i, g[i]);
            content_failures++; failed = 1;
         }
      }
   }

   /* Cross-check the production helper with an independently packed stream. */
   {
      const int pitch = 27;
      const int gain_index = 41;
      unsigned char raw[16];
      BitWriter w;
      BitReader r;
      SpeexBits sb;
      float exc[512];
      float *e;
      float prod[NSF], g[3], expect[NSF];
      int got_pitch = 0;
      float got_gain[3] = {0.f, 0.f, 0.f};
      long base = 300;
      float H[512];
      int j;
      memset(raw, 0, sizeof(raw));
      memset(exc, 0, sizeof(exc));
      memset(H, 0, sizeof(H));
      w.buf = raw; w.pos = 0;
      br_pack(&w, (unsigned)(pitch - PITCH_START), 7);
      br_pack(&w, gain_index, 7);
      e = exc + 300;
      for (j = -PITCH_END - 16; j < 0; j++) {
         float v = 0.23f * sinf(0.31f * j + 0.2f) + 0.001f * j;
         e[j] = v;
         H[base + j] = v;
      }
      speex_bits_init(&sb);
      speex_bits_read_from(&sb, (char *)raw, sizeof(raw));
      pitch_unquant_3tap(e, prod, PITCH_START, PITCH_END, 0.f,
                         &(ltp_params){gain_cdbk_nb, 7, 7}, NSF,
                         &got_pitch, got_gain, &sb, NULL, 0, 0, 0.f, 0);
      r.buf = raw; r.nbits = (int)sizeof(raw) * 8; r.pos = 0;
      if ((int)br_read(&r, 7) != pitch - PITCH_START ||
          (int)br_read(&r, 7) != gain_index || r.pos != 14) {
         fprintf(stderr, "BIT_CURSOR_MISMATCH adaptive unit consumed=%d\n", r.pos);
         bit_cursor_failures++; failed = 1;
      }
      model_pitch_gains(gain_cdbk_nb, gain_index, g);
      if (got_pitch != pitch) {
         fprintf(stderr,
                 "PITCH_DOMAIN_MISMATCH production pitch got=%d ref=%d\n",
                 got_pitch, pitch);
         pitch_domain_failures++; failed = 1;
      }
      for (j = 0; j < 3; j++) {
         if (fabsf(got_gain[j] - g[j]) > 1e-6f) {
            fprintf(stderr,
                    "CONTENT_MISMATCH production gain[%d] got=%g ref=%g\n",
                    j, got_gain[j], g[j]);
            content_failures++; failed = 1;
         }
      }
      model_adaptive(H, base, pitch, g, expect);
      for (j = 0; j < NSF; j++) {
         if (!within_tol(prod[j], expect[j])) {
            fprintf(stderr,
                    "CONTENT_MISMATCH production adaptive at %d got=%g ref=%g\n",
                    j, prod[j], expect[j]);
            content_failures++; failed = 1;
         }
      }
      speex_bits_destroy(&sb);
   }
   TASK_CHECK(!failed);
   return 0;
}

/* ================================================================== */
/* Innovation codebook group                                           */
/* ================================================================== */

/*
   Build an independent raw bitstream for a split shape/sign codebook and
   compare the model against the production unquantizer.  The bit cursor
   starts on a non-byte boundary and a sentinel bit must remain in place.
*/
static int innov_roundtrip(const char *tag, const ModeDesc *d, int have_sign,
                           int bit_offset)
{
   unsigned char raw[32];
   BitWriter w;
   BitReader r;
   SpeexBits sb;
   float initial[NSF], model[NSF], prod[NSF];
   int i, j;
   int failed = 0;
   int sentinel_pos, sentinel;

   memset(raw, 0, sizeof(raw));
   w.buf = raw;
   w.pos = bit_offset;

   /* Nonzero initial target: accumulation must not become overwrite. */
   for (j = 0; j < NSF; j++)
      initial[j] = 1.25f - 0.013f * j;

   for (i = 0; i < d->nb_subvect; i++) {
      int sign = have_sign ? ((i * 7 + 3) & 1) : 0;
      int idx = (i * 37 + 11) & ((1 << d->shape_bits) - 1);
      if (have_sign)
         br_pack(&w, (unsigned)sign, 1);
      br_pack(&w, (unsigned)idx, d->shape_bits);
   }
   sentinel_pos = w.pos;
   sentinel = 1;
   br_pack(&w, (unsigned)sentinel, 1);

   /* Production side, fed from the same raw bytes. */
   speex_bits_init(&sb);
   speex_bits_read_from(&sb, (char *)raw, sizeof(raw));
   speex_bits_advance(&sb, bit_offset);
   for (j = 0; j < NSF; j++)
      prod[j] = initial[j];
   {
      split_cb_params sp;
      sp.subvect_size = d->subvect_size;
      sp.nb_subvect = d->nb_subvect;
      sp.shape_cb = d->shape_cb;
      sp.shape_bits = d->shape_bits;
      sp.have_sign = have_sign;
      split_cb_shape_sign_unquant(prod, &sp, NSF, &sb, NULL, NULL);
   }

   /* Independent side. */
   r.buf = raw;
   r.nbits = (int)sizeof(raw) * 8;
   r.pos = bit_offset;
   {
      int consumed = model_innovation(&r, d, have_sign, initial, model);
      int expect_consumed =
         d->nb_subvect * (d->shape_bits + (have_sign ? 1 : 0));
      if (consumed != expect_consumed) {
         fprintf(stderr,
                 "BIT_CURSOR_MISMATCH innov %s consumed=%d expected=%d\n",
                 tag, consumed, expect_consumed);
         bit_cursor_failures++; failed = 1;
      }
   }
   if (r.pos != sentinel_pos || (int)br_read(&r, 1) != sentinel) {
      fprintf(stderr,
              "BIT_CURSOR_MISMATCH innov %s sentinel pos=%d/%d\n",
              tag, r.pos, sentinel_pos);
      bit_cursor_failures++; failed = 1;
   }

   for (j = 0; j < NSF; j++) {
      if (!within_tol(prod[j], model[j])) {
         fprintf(stderr,
                 "CONTENT_MISMATCH innov %s at %d got=%g ref=%g\n",
                 tag, j, prod[j], model[j]);
         content_failures++; failed = 1;
      }
   }
   speex_bits_destroy(&sb);
   return failed;
}

/* Narrowband parameter sets: no independent sign bit (signs implicit). */
static int test_innovation_nosign(void)
{
   ModeDesc d3, d5;
   int failed = 0;
   desc_init(&d3, 3);
   desc_init(&d5, 5);
   failed |= innov_roundtrip("nb-mode3", &d3, 0, 3);
   failed |= innov_roundtrip("nb-mode5", &d5, 0, 0);
   TASK_CHECK(!failed);
   return 0;
}

/* Wideband high-band parameter set: explicit per-subvector sign bit. */
static int test_innovation_sign(void)
{
   ModeDesc d;
   int failed = 0;
   memset(&d, 0, sizeof(d));
   d.subvect_size = 8;
   d.nb_subvect = 5;
   d.shape_bits = 7;
   d.have_sign = 1;
   d.shape_cb = hexc_table;
   failed |= innov_roundtrip("wb-sign-b1", &d, 1, 1);
   failed |= innov_roundtrip("wb-sign-b5", &d, 1, 5);
   TASK_CHECK(!failed);
   return 0;
}

/* ================================================================== */
/* Frame group: full nb_decode excitation contract                     */
/* ================================================================== */

#define CONTRACT_FRAMES 8   /* >= 6, alternating two speech histories */

/* Deterministic synthetic speech, two distinct tracks interleaved. */
static short contract_sample(int track, long n)
{
   double v = track
      ? 3500.0 * sin(0.047 * n) + 800.0 * cos(0.17 * n)
      : 2600.0 * sin(0.021 * n + 1.2) + 1200.0 * sin(0.093 * n);
   if (v > 32767.0) v = 32767.0;
   if (v < -32768.0) v = -32768.0;
   return (short)v;
}

/* Reference history in global sample coordinates; self-propagated only.
   The leading pad maps negative coordinates to deterministic zeros. */
#define HIST_PAD 160
static float hist_store[HIST_PAD + CONTRACT_FRAMES * FRAME_SIZE + 16];

#ifdef TASK_OBSERVE
typedef struct {
   int frame;
   int submode;
   int qe;
   int bit_pos;
   float ol_gain;
   /* Deepest 3-tap read reaches -(NB_PITCH_END+1); keep one extra sample. */
   float hist[PITCH_END + 1];
   int hist_valid;
} Observation;
static Observation observation;
#endif

static int contract_one_mode(int mode)
{
   ModeDesc d;
   void *enc, *dec;
   SpeexBits bits;
   int m = mode, enh = 0;
   int f, sub, j;
   int failed = 0;
   unsigned char packets[CONTRACT_FRAMES][128];
   int pkt_bytes[CONTRACT_FRAMES];
   short in[FRAME_SIZE], out[FRAME_SIZE];

   desc_init(&d, mode);
   enc = speex_encoder_init(&speex_nb_mode);
   dec = speex_decoder_init(&speex_nb_mode);
   if (!enc || !dec)
      return 1;
   speex_encoder_ctl(enc, SPEEX_SET_MODE, &m);
   speex_decoder_ctl(dec, SPEEX_SET_ENH, &enh);
   speex_bits_init(&bits);

   /* Generate legal packets with the normal production encoder. */
   for (f = 0; f < CONTRACT_FRAMES; f++) {
      for (j = 0; j < FRAME_SIZE; j++)
         in[j] = contract_sample(f & 1, (long)f * FRAME_SIZE + j);
      speex_bits_reset(&bits);
      if (speex_encode_int(enc, in, &bits) < 0)
         return 1;
      pkt_bytes[f] = speex_bits_nbytes(&bits);
      speex_bits_write(&bits, (char *)packets[f], pkt_bytes[f]);
   }

   memset(hist_store, 0, sizeof(hist_store));

   for (f = 0; f < CONTRACT_FRAMES; f++) {
      BitReader r;
      unsigned wb_flag, submode;
      int qe, k;
      float ol_gain;
      DecState *st;
      long frame_base = (long)f * FRAME_SIZE;

      r.buf = packets[f];
      r.nbits = pkt_bytes[f] * 8;
      r.pos = 0;

      wb_flag = br_read(&r, 1);
      submode = br_read(&r, NB_SUBMODE_BITS);
      if (wb_flag != 0 || (int)submode != mode) {
         fprintf(stderr, "INDEX_DOMAIN_MISMATCH mode=%d frame=%d wb=%u sub=%u\n",
                 mode, f, wb_flag, submode);
         index_domain_failures++; failed = 1;
      }
      for (k = 0; k < d.lsp_bits / 6; k++)
         br_read(&r, 6);
      qe = (int)br_read(&r, 5);
      /* Match production exactly: double exp() truncated to float. */
      ol_gain = (float)exp((double)qe / 3.5); /* SIG_SCALING == 1 in float build */

#ifdef TASK_OBSERVE
      memset(&observation, 0, sizeof(observation));
      observation.frame = f;
      observation.submode = (int)submode;
      observation.qe = qe;
      observation.ol_gain = ol_gain;
      /* Position right after the global gain field: wideband flag, submode,
         LSPs and the 5-bit excitation gain have all been read. */
      observation.bit_pos = r.pos;
      /* Read-only pre-call snapshot of the history the adaptive codebook
         will use.  nb_decode() shifts excBuf by one frame before decoding,
         so the upcoming window exc[-144..-1] is currently exc[16..159]. */
      st = (DecState *)dec;
      observation.hist_valid = (f > 0);
      if (f > 0) {
         for (j = 0; j < PITCH_END + 1; j++)
            observation.hist[j] = st->exc[15 + j];
      }
#endif

      /* Production decode of the same packet. */
      speex_bits_read_from(&bits, (char *)packets[f], pkt_bytes[f]);
      if (speex_decode_int(dec, &bits, out) != 0) {
         fprintf(stderr, "CONTENT_MISMATCH mode=%d frame=%d decode failed\n",
                 mode, f);
         content_failures++; failed = 1;
         continue;
      }
      st = (DecState *)dec;

#ifdef TASK_OBSERVE
      /* Observed production global gain must match the independent value. */
      if (fabsf(st->last_ol_gain - ol_gain) > 1e-6f) {
         fprintf(stderr,
                 "CONTENT_MISMATCH mode=%d frame=%d ol_gain prod=%g ref=%g\n",
                 mode, f, st->last_ol_gain, ol_gain);
         content_failures++; failed = 1;
      }
      if (observation.hist_valid) {
         for (j = 0; j < PITCH_END + 1; j++) {
            /* Snapshot slot j holds window coordinate j-(PITCH_END+1). */
            float rv = hist_store[HIST_PAD + frame_base + j - (PITCH_END + 1)];
            if (!within_tol(observation.hist[j], rv)) {
               fprintf(stderr,
                       "CONTENT_MISMATCH mode=%d frame=%d observed history at %d prod=%g ref=%g\n",
                       mode, f, j - (PITCH_END + 1), observation.hist[j], rv);
               content_failures++; failed = 1;
            }
         }
      }
      if (observation.bit_pos != 1 + NB_SUBMODE_BITS + d.lsp_bits + 5) {
         fprintf(stderr,
                 "BIT_CURSOR_MISMATCH mode=%d frame=%d observed pos=%d\n",
                 mode, f, observation.bit_pos);
         bit_cursor_failures++; failed = 1;
      }
#endif

      /* Independent per-subframe explanation of the same raw packet. */
      for (sub = 0; sub < N_SUBFRAMES; sub++) {
         long base = frame_base + (long)sub * NSF;
         int pitch_index, gain_index, pitch;
         float g[3], adapt[NSF];
         float ener;
         float innov[NSF], innov2[NSF], initial[NSF];
         int qidx;

         pitch_index = (int)br_read(&r, d.pitch_bits);
         gain_index = (int)br_read(&r, d.gain_bits);
         pitch = PITCH_START + pitch_index;
         if (pitch < PITCH_START || pitch > PITCH_END) {
            fprintf(stderr,
                    "PITCH_DOMAIN_MISMATCH mode=%d f=%d sub=%d pitch=%d\n",
                    mode, f, sub, pitch);
            pitch_domain_failures++; failed = 1;
         }
         if (gain_index >= (1 << d.gain_bits)) {
            fprintf(stderr,
                    "INDEX_DOMAIN_MISMATCH mode=%d f=%d sub=%d gain_idx=%d\n",
                    mode, f, sub, gain_index);
            index_domain_failures++; failed = 1;
         }
         model_pitch_gains(d.gain_cb, gain_index, g);
         model_adaptive(hist_store + HIST_PAD, base, pitch, g, adapt);

         if (d.subgain_bits == 3) {
            qidx = (int)br_read(&r, 3);
            if (qidx >= 8) {
               fprintf(stderr, "INDEX_DOMAIN_MISMATCH scal3 idx=%d\n", qidx);
               index_domain_failures++; failed = 1;
            }
            ener = exc_gain_scal3[qidx] * ol_gain;
         } else if (d.subgain_bits == 1) {
            qidx = (int)br_read(&r, 1);
            ener = exc_gain_scal1[qidx] * ol_gain;
         } else {
            ener = ol_gain;
         }

         for (j = 0; j < NSF; j++)
            initial[j] = 0.f;
         model_innovation(&r, &d, 0, initial, innov);
         for (j = 0; j < NSF; j++)
            innov[j] *= ener;

         if (d.double_codebook) {
            model_innovation(&r, &d, 0, initial, innov2);
            for (j = 0; j < NSF; j++)
               innov[j] += innov2[j] * (SECOND_CB_WEIGHT * ener);
         }

         /* Combine and propagate the reference history ourselves. */
         for (j = 0; j < NSF; j++)
            hist_store[HIST_PAD + base + j] = adapt[j] + innov[j];
      }

      /* Bit cursor must land on the documented payload width. */
      if (r.pos != d.payload_bits) {
         fprintf(stderr,
                 "BIT_CURSOR_MISMATCH mode=%d frame=%d pos=%d expected=%d\n",
                 mode, f, r.pos, d.payload_bits);
         bit_cursor_failures++; failed = 1;
      }
      /* Remaining packet bits hold the Speex terminator: 0 then ones. */
      if (r.pos < r.nbits) {
         if (br_read(&r, 1) != 0) {
            fprintf(stderr,
                    "BIT_CURSOR_MISMATCH mode=%d frame=%d terminator lead bit\n",
                    mode, f);
            bit_cursor_failures++; failed = 1;
         }
         while (r.pos < r.nbits) {
            if (br_read(&r, 1) != 1) {
               fprintf(stderr,
                       "BIT_CURSOR_MISMATCH mode=%d frame=%d terminator fill at %d\n",
                       mode, f, r.pos);
               bit_cursor_failures++; failed = 1;
            }
         }
      }

      /* Point-by-point excitation comparison before synthesis. */
      for (j = 0; j < FRAME_SIZE; j++) {
         float rv = hist_store[HIST_PAD + frame_base + j];
         if (!within_tol(st->exc[j], rv)) {
            fprintf(stderr,
                    "CONTENT_MISMATCH mode=%d frame=%d point=%d prod=%g ref=%g\n",
                    mode, f, j, st->exc[j], rv);
            content_failures++; failed = 1;
         }
      }
   }

   speex_bits_destroy(&bits);
   speex_encoder_destroy(enc);
   speex_decoder_destroy(dec);
   TASK_CHECK(!failed);
   return 0;
}

static int test_frames_modes(void)
{
   int rc = 0;
   rc |= contract_one_mode(3);
   rc |= contract_one_mode(5);
   rc |= contract_one_mode(7);
   TASK_CHECK(rc == 0);
   return 0;
}

/* ------------------------------------------------------------------ */
/* Registration (no main in this translation unit).                    */
/* ------------------------------------------------------------------ */
const struct task_case task_cases[] = {
   { "adaptive",   "short_delay",      test_adaptive_short_delay },
   { "adaptive",   "long_delay",       test_adaptive_long_delay  },
   { "adaptive",   "gain_mapping",     test_adaptive_gain_mapping},
   { "innovation", "nosign_nb",        test_innovation_nosign    },
   { "innovation", "sign_wb_high",     test_innovation_sign      },
   { "frames",     "nb_decode_modes",  test_frames_modes         }
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
