/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.
 */

/*
 * codebook_contract.c
 *
 * Independent regression contract for the fixed-point iLBC adaptive
 * codebook.  The reference model in this file re-derives every expected
 * value from the bitstream fields and the published constant tables only.
 * It never calls the production vector construction, interpolation,
 * filtering, gain dequantization or codebook construction routines to
 * compute expectations; the only production algorithm reused by contract
 * is the scalar start-state constructor WebRtcIlbcfix_StateConstruct()
 * (explicitly allowed for the residual group).  LSF decoding /
 * interpolation are reused only in the stream group, on a decoder state
 * that is separate from the decoder under test.
 *
 * Numeric conventions (the production build uses -fwrapv):
 *  - 16x16 multiplies are signed int32 products, magnitude < 2^30, so
 *    they cannot wrap.
 *  - ">> n" on a signed integer is an arithmetic shift rounding toward
 *    minus infinity; sar32() below emulates that without relying on
 *    implementation-defined behaviour, including for negative dividends.
 *  - 32-bit accumulators wrap as two's complement; add_wrap32() makes
 *    the wrap explicit.
 *  - Truncation to int16 keeps the low 16 bits and re-signs; wrap16()
 *    makes that explicit (this is a wrap, not a saturation).
 *  - The only saturations in the modelled paths are the MA filter
 *    clamps in ref_ma_q12(); they mirror WebRtcSpl_FilterMAFastQ12().
 *
 * This file defines no main(); task_support/driver.c drives the cases
 * through task_cases / task_case_count.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "task_support/check.h"
#include "ilbc/interface/ilbc.h"
#include "ilbc/defines.h"
#include "ilbc/constants.h"
#include "ilbc/state_construct.h"
#include "ilbc/decode_residual.h"
#include "ilbc/unpack_bits.h"
#include "ilbc/swap_bytes.h"
#include "ilbc/simple_lsf_dequant.h"
#include "ilbc/lsf_check.h"
#include "ilbc/decoder_interpolate_lsf.h"
#include "ilbc/index_conv_dec.h"
#include "ilbc/create_augmented_vec.h"
#include "ilbc/filtered_cb_vecs.h"
#include "ilbc/get_cd_vec.h"
#include "ilbc/gain_dequant.h"
#include "ilbc/cb_construct.h"

/* ------------------------------------------------------------------ */
/* Fixed-point primitives (independent, wrap/round semantics written  */
/* out explicitly).                                                    */
/* ------------------------------------------------------------------ */

/* Low-16 truncation with re-sign: the int16_t cast used in production. */
static int16_t wrap16(int32_t x) {
  uint32_t u = (uint32_t)x & 0xFFFFu;
  return u >= 0x8000u ? (int16_t)(u - 0x10000u) : (int16_t)u;
}

/* Signed 16x16 -> 32 multiply (always fits, no wrap). */
static int32_t mul16(int16_t a, int16_t b) {
  return (int32_t)a * (int32_t)b;
}

/* Arithmetic right shift, rounding toward -infinity for negatives. */
static int32_t sar32(int32_t x, int n) {
  if (x >= 0) {
    return x >> n;
  }
  /* -1-x is the bitwise complement and never overflows. */
  return -1 - ((-1 - x) >> n);
}

/* Two's-complement 32-bit addition, matching -fwrapv accumulation. */
static int32_t add_wrap32(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a + (uint32_t)b);
}

/* WebRtcSpl_SatW32ToW16 style clamp is not used by the modelled paths;
 * abs16 mirrors WEBRTC_SPL_ABS_W16, including the -32768 wrap. */
static int16_t abs16(int16_t a) {
  return a >= 0 ? a : wrap16(-(int32_t)a);
}

/* Signed Q12 MA tap: one product, already in 32-bit range. */
static int32_t ma_tap(int16_t coef, int16_t sample) {
  return mul16(coef, sample);
}

/*
 * Q12 MA filter output for one sample.
 * x[-4..3] relative to position p are supplied explicitly; positions
 * outside the codebook memory are zero by contract (zero extension).
 * Mirrors WebRtcSpl_FilterMAFastQ12: sum B[j]*x[p-j], saturate to
 * [-2^27, 2^27-2049], then round with +2048 and arithmetic shift 12.
 */
static int16_t ref_ma_q12(const int16_t x[CB_FILTERLEN]) {
  int32_t o = 0;
  int j;
  for (j = 0; j < CB_FILTERLEN; ++j) {
    o = add_wrap32(o, ma_tap(WebRtcIlbcfix_kCbFiltersRev[j], x[j]));
  }
  if (o > 134215679) {
    o = 134215679;
  } else if (o < -134217728) {
    o = -134217728;
  }
  return wrap16(sar32(o + 2048, 12));
}

/* Bounded memory read with explicit zero extension on both sides. */
static int16_t mem_at(const int16_t *mem, int lMem, int k) {
  if (k < 0 || k >= lMem) {
    return 0;
  }
  return mem[k];
}

/* ------------------------------------------------------------------ */
/* Independent gain dequantization (mirror of WebRtcIlbcfix_GainDequant). */
/* ------------------------------------------------------------------ */

static int16_t ref_gain_dequant(int16_t index, int16_t maxIn, int stage) {
  int16_t scale = abs16(maxIn);
  if (scale < 1638) {          /* floor at 0.1 in Q14 */
    scale = 1638;
  }
  /* Constant table only; no production gain function is called. */
  int32_t prod = mul16(scale, WebRtcIlbcfix_kGain[stage][index]);
  return wrap16(sar32(prod + 8192, 14));
}

/* ------------------------------------------------------------------ */
/* Independent augmented (short-lag) vector for a 40-sample section.   */
/*                                                                      */
/* The four-sample overlap keeps TWO truncations: each scaled product  */
/* is shifted to int16 on its own, and only then are the two int16      */
/* halves added (with int16 wrap).  Merging the products before one     */
/* truncation is the "merged" mutation and must produce different       */
/* content on crafted memory.                                           */
/* ------------------------------------------------------------------ */

static void ref_augmented_vec40(const int16_t *bufferEnd, int16_t lag,
                                int16_t *out) {
  int k, ilow;
  const int16_t *ppo, *ppi;

  ilow = lag - 4;

  /* First non-interpolated part: plain time-order copy. */
  ppo = bufferEnd - lag;
  for (k = 0; k < lag; ++k) {
    out[k] = ppo[k];
  }

  /* Four-point overlap, two signed scalings and two truncations. */
  ppo = bufferEnd - 4;
  ppi = bufferEnd - lag - 4;
  for (k = 0; k < 4; ++k) {
    int32_t left = sar32(mul16(ppi[k], WebRtcIlbcfix_kAlpha[k]), 15);
    int32_t right = sar32(mul16(ppo[k], WebRtcIlbcfix_kAlpha[3 - k]), 15);
    out[ilow + k] = wrap16(left + right);
  }

  /* Second non-interpolated part. */
  ppo = bufferEnd - lag;
  for (k = 0; k < SUBL - lag; ++k) {
    out[lag + k] = ppo[k];
  }
}

/* ------------------------------------------------------------------ */
/* Independent filtered codebook memory (mirror of                      */
/* WebRtcIlbcfix_FilteredCbVecs).  The convolution is a finite sum      */
/* over CB_FILTERLEN taps; everything outside mem[0..lMem-1] is an      */
/* explicit zero, including the four stuffed positions on each side.    */
/* ------------------------------------------------------------------ */

static void ref_filtered_memory(int16_t *filtered, const int16_t *mem,
                                int lMem, int samples) {
  int n, j;
  int first = lMem - samples;

  for (n = 0; n < first; ++n) {
    filtered[n] = 0;
  }
  for (n = 0; n < samples; ++n) {
    int16_t window[CB_FILTERLEN];
    int center = CB_HALFFILTERLEN + first + n;
    for (j = 0; j < CB_FILTERLEN; ++j) {
      /* FilterMA reads in[center-j] for j=0..7. */
      window[j] = mem_at(mem, lMem, center - j);
    }
    filtered[first + n] = ref_ma_q12(window);
  }
}

/* ------------------------------------------------------------------ */
/* Independent codebook vector construction (mirror of                  */
/* WebRtcIlbcfix_GetCbVec).  Four sections for a 40-sample vector:      */
/*   0 .. lMem-veclen       plain unfiltered window                     */
/*   .. base_size-1         augmented unfiltered short-lag vector       */
/*   .. +(lMem-veclen)      plain filtered window                       */
/*   .. 2*base_size-1       augmented filtered short-lag vector         */
/* ------------------------------------------------------------------ */

static void ref_get_cbvec(int16_t *cbvec, const int16_t *mem, int16_t index,
                          int16_t lMem, int16_t veclen) {
  int16_t base_size = (int16_t)(lMem - veclen + 1);
  int k;

  if (veclen == SUBL) {
    base_size = (int16_t)(base_size + veclen / 2);
  }

  if (index < lMem - veclen + 1) {
    /* Section 1, plain non-interpolated window. */
    k = index + veclen;
    for (int i = 0; i < veclen; ++i) {
      cbvec[i] = mem[lMem - k + i];
    }
    return;
  }

  if (index < base_size) {
    /* Section 1, augmented.  Positive lag, ordinary integer division. */
    int k2 = 2 * (index - (lMem - veclen + 1)) + veclen;
    int16_t lag = (int16_t)(k2 / 2);
    ref_augmented_vec40(mem + lMem, lag, cbvec);
    return;
  }

  /* Filtered section. */
  int16_t d = (int16_t)(index - base_size);

  if (d < lMem - veclen + 1) {
    /* Section 2, plain filtered window; zero-stuffed outside memory. */
    int memIndTest = lMem - (d + veclen);
    for (int n = 0; n < veclen; ++n) {
      int16_t window[CB_FILTERLEN];
      int center = memIndTest + CB_HALFFILTERLEN + n;
      for (int j = 0; j < CB_FILTERLEN; ++j) {
        window[j] = mem_at(mem, lMem, center - j);
      }
      cbvec[n] = ref_ma_q12(window);
    }
    return;
  }

  /* Section 2, augmented filtered vector. */
  int memIndTest = lMem - veclen - CB_FILTERLEN;
  int16_t tempbuff2[SUBL + 5];
  for (int n = 0; n < veclen + 5; ++n) {
    int16_t window[CB_FILTERLEN];
    int center = memIndTest + 7 + n;
    for (int j = 0; j < CB_FILTERLEN; ++j) {
      window[j] = mem_at(mem, lMem, center - j);
    }
    tempbuff2[n] = ref_ma_q12(window);
  }
  int16_t lag =
      (int16_t)((veclen << 1) - 20 + index - base_size - lMem - 1);
  ref_augmented_vec40(tempbuff2 + SUBL + 5, lag, cbvec);
}

/* ------------------------------------------------------------------ */
/* Independent three-stage gain-shape combination (mirror of            */
/* WebRtcIlbcfix_CbConstruct).  Gains recurse from the previous         */
/* stage's already quantized magnitude with the 1638 floor.            */
/* ------------------------------------------------------------------ */

static void ref_cb_construct(int16_t *decvector, const int16_t *index,
                             const int16_t *gain_index, const int16_t *mem,
                             int16_t lMem, int16_t veclen) {
  int16_t gain[CB_NSTAGES];
  int16_t cbvec[CB_NSTAGES][SUBL];
  int j;

  gain[0] = ref_gain_dequant(gain_index[0], 16384, 0);
  gain[1] = ref_gain_dequant(gain_index[1], gain[0], 1);
  gain[2] = ref_gain_dequant(gain_index[2], gain[1], 2);

  ref_get_cbvec(cbvec[0], mem, index[0], lMem, veclen);
  ref_get_cbvec(cbvec[1], mem, index[1], lMem, veclen);
  ref_get_cbvec(cbvec[2], mem, index[2], lMem, veclen);

  for (j = 0; j < veclen; ++j) {
    /* 32-bit two's-complement accumulation; final rounded >>14 and
     * low-16 truncation exactly as in the production code. */
    int32_t a32 = mul16(gain[0], cbvec[0][j]);
    a32 = add_wrap32(a32, mul16(gain[1], cbvec[1][j]));
    a32 = add_wrap32(a32, mul16(gain[2], cbvec[2][j]));
    decvector[j] = wrap16(sar32(a32 + 8192, 14));
  }
}

/* ------------------------------------------------------------------ */
/* TASK_CHECK plumbing: every content failure prints CONTENT_MISMATCH  */
/* before TASK_CHECK(0) makes the driver return 4.                      */
/* ------------------------------------------------------------------ */

static const char *cur_group;
static const char *cur_case;

static int eq16(const char *what, int idx, int16_t got, int16_t want) {
  if (got == want) {
    return 1;
  }
  fprintf(stderr,
          "CONTENT_MISMATCH group=%s case=%s what=%s idx=%d got=%d want=%d\n",
          cur_group, cur_case, what, idx, (int)got, (int)want);
  return 0;
}

static int vec_eq(const char *what, const int16_t *got,
                  const int16_t *want, int n) {
  int i;
  for (i = 0; i < n; ++i) {
    int match = (got[i] == want[i]);
    if (!match) {
      eq16(what, i, got[i], want[i]);   /* prints CONTENT_MISMATCH */
    }
    /* Every sample passes through TASK_CHECK, so a single differing
     * sample yields TASK_CHECK(0) and the driver exit code 4. */
    if (!task_check(match, what, __FILE__, __LINE__)) {
      return 0;
    }
  }
  return 1;
}

#define CK_16(what, idx, got, want) TASK_CHECK(eq16(what, idx, got, want))
#define CK_VEC(what, got, want, n) TASK_CHECK(vec_eq(what, got, want, n))
#define CK(cond) TASK_CHECK(cond)

/* ================================================================== */
/* vectors group: bounded, time-ordered codebook memory fixtures       */
/* ================================================================== */

/*
 * Memory fixture in ordinary time order (oldest sample at index 0).
 * Values stay well inside int16 and mix signs; seed changes the shape.
 */
static void make_memory(int16_t *mem, int lMem, uint32_t seed) {
  uint32_t rng = seed;
  int i;
  for (i = 0; i < lMem; ++i) {
    rng = rng * 1664525u + 1013904223u;
    int32_t noise = (int32_t)(rng >> 16) - 32768;       /* +/- 32768 */
    int32_t tone = (int32_t)(i * 37 + (int)seed) % 211 - 105;
    int32_t v = noise / 3 + tone * 64;
    if (i % 17 == 0) {
      v = -v;                                  /* force negative pockets */
    }
    mem[i] = wrap16(v);
  }
}

/* All-negative fixture so sign handling and negative arithmetic shifts
 * are exercised on their own. */
static void make_negative_memory(int16_t *mem, int lMem, uint32_t seed) {
  uint32_t rng = seed;
  int i;
  for (i = 0; i < lMem; ++i) {
    rng = rng * 1103515245u + 12345u;
    int32_t v = -1 - (int32_t)(rng % 28000u);
    mem[i] = (int16_t)v;
  }
}

/* vectors: short (22/23 sample) codebook over 85-sample memory. */
static int vectors_short(void) {
  cur_group = "vectors";
  cur_case = "short23_memory85";

  int16_t mem[85];
  int16_t filtered[85];
  const int16_t lMem = 85;
  const int16_t veclen = 23;
  const int16_t base_size = lMem - veclen + 1;     /* 63, no augmented half */

  make_memory(mem, lMem, 11u);
  ref_filtered_memory(filtered, mem, lMem, 58);    /* search range block 0 */

  /* Production counterpart for the actual values. */
  int16_t prod_buf[85 + 8];
  int16_t prod_filt[85];
  int16_t prod_vec[SUBL];
  memset(prod_buf, 0, sizeof(prod_buf));
  memcpy(prod_buf + CB_HALFFILTERLEN, mem, sizeof(mem));
  WebRtcIlbcfix_FilteredCbVecs(prod_filt, prod_buf + CB_HALFFILTERLEN,
                               lMem, 58);
  CK_VEC("short_filtered_memory", prod_filt, filtered, lMem);

  /* Boundary indexes of both sections plus interior samples. */
  const int16_t probes[] = {0, 1, 7, 31, base_size - 1,
                            base_size, base_size + 1, base_size + 31,
                            2 * base_size - 1};
  for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); ++p) {
    int16_t want[SUBL];
    ref_get_cbvec(want, mem, probes[p], lMem, veclen);
    WebRtcIlbcfix_GetCbVec(prod_vec, prod_buf + CB_HALFFILTERLEN,
                           probes[p], lMem, veclen);
    char label[48];
    snprintf(label, sizeof(label), "short_getcb_idx%d", (int)probes[p]);
    CK_VEC(label, prod_vec, want, veclen);
  }

  /* Negative-only memory: every shift path sees negative dividends. */
  int16_t negmem[85], negfilt[85];
  make_negative_memory(negmem, lMem, 91u);
  ref_filtered_memory(negfilt, negmem, lMem, 58);
  memset(prod_buf, 0, sizeof(prod_buf));
  memcpy(prod_buf + CB_HALFFILTERLEN, negmem, sizeof(negmem));
  memset(prod_filt, 0, sizeof(prod_filt));
  WebRtcIlbcfix_FilteredCbVecs(prod_filt, prod_buf + CB_HALFFILTERLEN,
                               lMem, 58);
  CK_VEC("short_negative_filtered", prod_filt, negfilt, lMem);
  int16_t want[SUBL], got[SUBL];
  ref_get_cbvec(want, negmem, 44, lMem, veclen);
  WebRtcIlbcfix_GetCbVec(got, prod_buf + CB_HALFFILTERLEN, 44, lMem, veclen);
  CK_VEC("short_negative_getcb", got, want, veclen);
  return 0;
}

/* vectors: 40-sample vectors, all four codebook sections. */
static int vectors_long40(void) {
  cur_group = "vectors";
  cur_case = "long40_sections";

  const int16_t lMem = CB_MEML;      /* 147 */
  const int16_t base_size = lMem - SUBL + 1 + SUBL / 2;   /* 128 */
  CK(base_size == 128);
  int16_t mem[CB_MEML], filtered[CB_MEML];
  make_memory(mem, lMem, 23u);
  ref_filtered_memory(filtered, mem, lMem, 147);

  int16_t prod_buf[CB_MEML + 8];
  int16_t prod_filt[CB_MEML];
  int16_t got[SUBL], want[SUBL];
  memset(prod_buf, 0, sizeof(prod_buf));
  memcpy(prod_buf + CB_HALFFILTERLEN, mem, sizeof(mem));
  WebRtcIlbcfix_FilteredCbVecs(prod_filt, prod_buf + CB_HALFFILTERLEN,
                               lMem, 147);
  CK_VEC("long_filtered_memory", prod_filt, filtered, lMem);

  /* One probe at each section edge and several interiors: 0/107 plain,
   * 108/127 augmented, 128/235 filtered plain, 236/255 filtered aug. */
  const int16_t probes[] = {0, 53, 107, 108, 117, 127,
                            128, 180, 235, 236, 245, 255};
  for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); ++p) {
    ref_get_cbvec(want, mem, probes[p], lMem, SUBL);
    WebRtcIlbcfix_GetCbVec(got, prod_buf + CB_HALFFILTERLEN, probes[p],
                           lMem, SUBL);
    char label[48];
    snprintf(label, sizeof(label), "long_getcb_idx%d", (int)probes[p]);
    CK_VEC(label, got, want, SUBL);
  }

  /* The four conceptual segments against the same time-ordered memory:
   * unfiltered window, filtered window, augmented unfiltered, augmented
   * filtered.  Only lags 20..39 are reachable from a GetCbVec index
   * (108..127), so those are the legal augmented-vector inputs. */
  for (int16_t lag = 20; lag <= 39; ++lag) {
    int16_t aug_mem[SUBL], aug_filt[SUBL];
    int16_t prod_aug[SUBL];
    ref_augmented_vec40(mem + lMem, lag, aug_mem);
    ref_augmented_vec40(filtered + lMem, lag, aug_filt);
    WebRtcIlbcfix_CreateAugmentedVec(lag, prod_buf + CB_HALFFILTERLEN + lMem,
                                     prod_aug);
    char label[48];
    snprintf(label, sizeof(label), "aug_mem_lag%d", (int)lag);
    CK_VEC(label, prod_aug, aug_mem, SUBL);

    int16_t filt_buf[CB_MEML + 8];
    memset(filt_buf, 0, sizeof(filt_buf));
    memcpy(filt_buf + CB_HALFFILTERLEN, filtered, sizeof(filtered));
    WebRtcIlbcfix_CreateAugmentedVec(lag, filt_buf + CB_HALFFILTERLEN + lMem,
                                     prod_aug);
    snprintf(label, sizeof(label), "aug_filt_lag%d", (int)lag);
    CK_VEC(label, prod_aug, aug_filt, SUBL);
  }
  return 0;
}

/*
 * vectors: the four-sample overlap must remain two separate truncations.
 * Memory is crafted so that for at least one overlap sample the merged
 * single-truncation result differs from the two-truncation contract;
 * the case asserts that the discriminating sample really exists, then
 * checks every product and every combined sample against production.
 */
static int vectors_overlap_truncation(void) {
  cur_group = "vectors";
  cur_case = "overlap_two_truncations";

  int16_t mem[CB_MEML];
  make_memory(mem, CB_MEML, 47u);
  int discriminating = 0;

  /* Legal augmented lags are 20..39; all reads stay inside the memory
   * plus its contractually zero stuffed positions. */
  for (int16_t lag = 20; lag <= 39; ++lag) {
    const int16_t *ppo = mem + CB_MEML - 4;
    const int16_t *ppi = mem + CB_MEML - lag - 4;
    int16_t want[SUBL], got[SUBL];
    ref_augmented_vec40(mem + CB_MEML, lag, want);
    WebRtcIlbcfix_CreateAugmentedVec(
        lag, mem + CB_MEML, got);   /* production buffer needs no padding
                                       for the unfiltered augmented path */
    char label[48];
    snprintf(label, sizeof(label), "overlap_vec_lag%d", (int)lag);
    CK_VEC(label, got, want, SUBL);

    for (int k = 0; k < 4; ++k) {
      /* Two independently truncated int16 halves. */
      int16_t left16 =
          wrap16(sar32(mul16(ppi[k], WebRtcIlbcfix_kAlpha[k]), 15));
      int16_t right16 =
          wrap16(sar32(mul16(ppo[k], WebRtcIlbcfix_kAlpha[3 - k]), 15));
      int16_t two_step = wrap16((int32_t)left16 + (int32_t)right16);
      /* The mutation under test: sum the 32-bit products first, then a
       * single truncation. */
      int16_t merged =
          wrap16(sar32(mul16(ppi[k], WebRtcIlbcfix_kAlpha[k]) +
                           mul16(ppo[k], WebRtcIlbcfix_kAlpha[3 - k]),
                       15));
      /* Production output must follow the two-truncation contract. */
      CK_16("overlap_combined", k, got[lag - 4 + k], two_step);
      if (merged != two_step && merged != got[lag - 4 + k]) {
        /* A sample where the single-truncation result diverges from the
         * production two-truncation value makes the merged mutation
         * observable through this very content comparison. */
        discriminating = 1;
      }
    }
  }
  /* The fixture must actually tell one merged truncation apart from the
   * contractual two truncations. */
  CK(discriminating);
  return 0;
}

/*
 * vectors: three-stage gain recursion plus the final weighted vector,
 * including the 1638 floor binding, negative gains and 16/32-bit wrap.
 */
static int vectors_gain_chain(void) {
  cur_group = "vectors";
  cur_case = "gain_chain_weighted";

  int16_t mem[CB_MEML];
  make_memory(mem, CB_MEML, 59u);
  int16_t prod_buf[CB_MEML + 8];
  memset(prod_buf, 0, sizeof(prod_buf));
  memcpy(prod_buf + CB_HALFFILTERLEN, mem, sizeof(mem));

  /* Stage tables: 32 / 16 / 8 entries.  Include extremes. */
  int16_t gain_index[CB_NSTAGES] = {31, 16, 8};
  int16_t cb_index[CB_NSTAGES] = {7, 144, 250};

  int16_t g[CB_NSTAGES];
  g[0] = ref_gain_dequant(gain_index[0], 16384, 0);
  g[1] = ref_gain_dequant(gain_index[1], g[0], 1);
  g[2] = ref_gain_dequant(gain_index[2], g[1], 2);

  int16_t pg[CB_NSTAGES];
  pg[0] = WebRtcIlbcfix_GainDequant(gain_index[0], 16384, 0);
  pg[1] = WebRtcIlbcfix_GainDequant(gain_index[1], pg[0], 1);
  pg[2] = WebRtcIlbcfix_GainDequant(gain_index[2], pg[1], 2);
  CK_VEC("gain_stages_extremes", pg, g, CB_NSTAGES);

  /* Floor-binding case: tiny previous gain must be replaced by 1638. */
  int16_t tiny = ref_gain_dequant(3, 0, 0);      /* maxIn 0 -> floor */
  CK_16("gain_floor_binds", 0, tiny,
        wrap16(sar32(mul16(1638, WebRtcIlbcfix_kGain[0][3]) + 8192, 14)));
  int16_t next = ref_gain_dequant(0, tiny, 1);
  CK_16("gain_floor_next", 0, next,
        WebRtcIlbcfix_GainDequant(0, WebRtcIlbcfix_GainDequant(3, 0, 0), 1));

  /* Full weighted 40-sample vector against production CbConstruct. */
  int16_t want[SUBL], got[SUBL];
  ref_cb_construct(want, cb_index, gain_index,
                   prod_buf + CB_HALFFILTERLEN, CB_MEML, SUBL);
  WebRtcIlbcfix_CbConstruct(got, cb_index, gain_index,
                            prod_buf + CB_HALFFILTERLEN, CB_MEML, SUBL);
  CK_VEC("weighted_vec_extremes", got, want, SUBL);

  /* A second, milder triple over a short 23-sample start-state vector. */
  int16_t gi2[CB_NSTAGES] = {5, 2, 1};
  int16_t ci2[CB_NSTAGES] = {12, 40, 61};
  int16_t short_mem[85], short_prod[85 + 8];
  make_negative_memory(short_mem, 85, 73u);
  memset(short_prod, 0, sizeof(short_prod));
  memcpy(short_prod + CB_HALFFILTERLEN, short_mem, sizeof(short_mem));
  int16_t sw[23], sg[23];
  ref_cb_construct(sw, ci2, gi2, short_prod + CB_HALFFILTERLEN, 85, 23);
  WebRtcIlbcfix_CbConstruct(sg, ci2, gi2, short_prod + CB_HALFFILTERLEN,
                            85, 23);
  CK_VEC("weighted_short_negative", sg, sw, 23);

  int16_t gg[CB_NSTAGES];
  gg[0] = ref_gain_dequant(gi2[0], 16384, 0);
  gg[1] = ref_gain_dequant(gi2[1], gg[0], 1);
  gg[2] = ref_gain_dequant(gi2[2], gg[1], 2);
  int16_t gp[CB_NSTAGES];
  gp[0] = WebRtcIlbcfix_GainDequant(gi2[0], 16384, 0);
  gp[1] = WebRtcIlbcfix_GainDequant(gi2[1], gp[0], 1);
  gp[2] = WebRtcIlbcfix_GainDequant(gi2[2], gp[1], 2);
  CK_VEC("gain_stages_mild", gp, gg, CB_NSTAGES);
  return 0;
}

/* vectors: finite convolution with explicit zero extension, across the
 * full set of production filter lengths. */
static int vectors_filter_convolution(void) {
  cur_group = "vectors";
  cur_case = "filter_zero_extension";

  const int samples_tbl[] = {63, 85, 125, 147};
  for (size_t t = 0; t < sizeof(samples_tbl) / sizeof(samples_tbl[0]); ++t) {
    int samples = samples_tbl[t];
    int16_t mem[CB_MEML], filtered[CB_MEML];
    int16_t prod_buf[CB_MEML + 8], prod_filt[CB_MEML];
    make_memory(mem, CB_MEML, (uint32_t)(101u + t * 13));
    ref_filtered_memory(filtered, mem, CB_MEML, samples);

    memset(prod_buf, 0, sizeof(prod_buf));
    memcpy(prod_buf + CB_HALFFILTERLEN, mem, sizeof(mem));
    memset(prod_filt, 0, sizeof(prod_filt));
    WebRtcIlbcfix_FilteredCbVecs(prod_filt,
                                 prod_buf + CB_HALFFILTERLEN, CB_MEML,
                                 (int16_t)samples);
    char label[48];
    snprintf(label, sizeof(label), "conv_samples%d", samples);
    CK_VEC(label, prod_filt, filtered, CB_MEML);

    /* The leading lMem-samples positions must be exactly zero. */
    for (int z = 0; z < CB_MEML - samples; ++z) {
      CK_16("conv_zero_prefix", z, filtered[z], 0);
    }
  }
  return 0;
}

/* ================================================================== */
/* Shared deterministic stream fixture                                 */
/* ================================================================== */

#define STREAM_FRAMES 240

struct frame_rec {
  int16_t bits[25];                 /* packet bytes as handed to Decode */
  iLBC_bits fields;                 /* unpacked fields */
  iLBC_bits conv;                   /* fields with codebook indexes
                                       converted to lag-domain form */
  int startIdx;
  int state_first;
};

struct mode_fixture {
  int mode;
  int samples;
  int bytes;
  int frames;
  struct frame_rec rec[STREAM_FRAMES];
  unsigned cell[8][2];              /* observed (startIdx,state_first) */
  unsigned section[4];              /* subframe GetCbVec section counts */
  unsigned state_section[4];        /* start-state triple section counts
                                       (only sections 0 and 2 are legal) */
  unsigned converted;               /* indexes changed by IndexConvDec */
};

/* Deterministic, non-silent PCM: a narrow energy burst whose centre
 * scans every sample position, plus bounded LCG noise.  The generator is
 * fully specified here, needs no files and runs offline. */
static void pcm_frame(int16_t *out, int samples, int f, uint32_t *rng) {
  int center = f % samples;
  int width = 8 + (f % 6) * 6;
  int amp = 12000 + (f % 4) * 4000;
  int i;
  for (i = 0; i < samples; ++i) {
    *rng = *rng * 1664525u + 1013904223u;
    int dist = i - center;
    if (dist < 0) {
      dist = -dist;
    }
    int32_t v = (dist < width ? amp / (1 + dist) : 0) +
                ((int32_t)(*rng >> 16) - 32768) / 40;
    if (v > 32767) {
      v = 32767;
    } else if (v < -32768) {
      v = -32768;
    }
    out[i] = (int16_t)v;
  }
}

/* Classify a converted codebook index into the four GetCbVec sections. */
static int cb_section(int16_t index, int16_t lMem, int16_t veclen) {
  int16_t base_size = (int16_t)(lMem - veclen + 1);
  if (veclen == SUBL) {
    base_size = (int16_t)(base_size + veclen / 2);
  }
  if (index < lMem - veclen + 1) {
    return 0;                        /* plain unfiltered */
  }
  if (index < base_size) {
    return 1;                        /* augmented unfiltered */
  }
  if (index - base_size < lMem - veclen + 1) {
    return 2;                        /* plain filtered */
  }
  return 3;                          /* augmented filtered */
}

static struct mode_fixture fixture20;
static struct mode_fixture fixture30;
static int fixtures_built;

static int build_fixture(struct mode_fixture *fx, int mode) {
  iLBC_encinst_t *enc = NULL;
  int f;
  uint32_t rng = 2024u;

  fx->mode = mode;
  fx->samples = mode * 8;
  fx->bytes = (mode == 20) ? 38 : 50;
  fx->frames = STREAM_FRAMES;
  memset(fx->cell, 0, sizeof(fx->cell));
  memset(fx->section, 0, sizeof(fx->section));
  memset(fx->state_section, 0, sizeof(fx->state_section));
  fx->converted = 0;

  CK(WebRtcIlbcfix_EncoderCreate(&enc) == 0);
  CK(WebRtcIlbcfix_EncoderInit(enc, mode) == 0);

  int nsub = mode == 20 ? NSUB_20MS : NSUB_30MS;
  for (f = 0; f < STREAM_FRAMES; ++f) {
    struct frame_rec *r = &fx->rec[f];
    int16_t pcm[BLOCKL_MAX];
    uint16_t swapped[NO_OF_WORDS_30MS];

    pcm_frame(pcm, fx->samples, f, &rng);
    int len = WebRtcIlbcfix_Encode(enc, pcm, (int16_t)fx->samples,
                                   r->bits);
    CK(len == fx->bytes);

    WebRtcIlbcfix_SwapBytes((uint16_t *)r->bits, (int16_t)(len / 2),
                            swapped);
    CK(WebRtcIlbcfix_UnpackBits(swapped, &r->fields, (int16_t)mode) == 0);
    r->conv = r->fields;
    WebRtcIlbcfix_IndexConvDec(r->conv.cb_index);
    r->startIdx = r->fields.startIdx;
    r->state_first = r->fields.state_first;
    fx->cell[r->startIdx][r->state_first]++;

    /* Record real codebook-section coverage and index conversions.
     * The four GetCbVec sections only exist for 40-sample vectors, so
     * subframe triples (t >= 3) are classified into four sections while
     * triple 0 builds the 22/23-sample start-state completion, whose
     * codebook has only the plain and filtered halves. */
    int short_len = STATE_LEN -
        (mode == 20 ? STATE_SHORT_LEN_20MS : STATE_SHORT_LEN_30MS);
    int short_base = ST_MEM_L_TBL - short_len + 1;
    int t;
    for (t = 0; t < nsub * CB_NSTAGES; ++t) {
      if (r->conv.cb_index[t] != r->fields.cb_index[t]) {
        fx->converted++;
      }
      if (t >= CB_NSTAGES) {
        fx->section[cb_section(r->conv.cb_index[t], CB_MEML, SUBL)]++;
      } else if (r->conv.cb_index[t] < short_base) {
        fx->state_section[0]++;     /* plain unfiltered short window */
      } else {
        fx->state_section[2]++;     /* plain filtered short window */
      }
    }
  }

  WebRtcIlbcfix_EncoderFree(enc);
  return 0;
}

static int ensure_fixtures(void) {
  if (fixtures_built) {
    return 0;
  }
  CK(build_fixture(&fixture20, 20) == 0);
  CK(build_fixture(&fixture30, 30) == 0);
  fixtures_built = 1;
  return 0;
}

static const struct frame_rec *find_rec(const struct mode_fixture *fx,
                                        int startIdx, int state_first) {
  int f;
  for (f = 0; f < fx->frames; ++f) {
    if (fx->rec[f].startIdx == startIdx &&
        fx->rec[f].state_first == state_first) {
      return &fx->rec[f];
    }
  }
  return NULL;
}

/* ================================================================== */
/* Independent whole-frame residual model.                             */
/*                                                                      */
/* All positions below are ABSOLUTE sample coordinates inside the      */
/* frame (0 = oldest).  The model keeps its own codebook memory        */
/* mem[0..CB_MEML-1] in ordinary time order, mirroring the layout of   */
/* decoder prevResidual+CB_HALFFILTERLEN, instead of replaying the     */
/* production pointer/memmove loops.                                   */
/*                                                                      */
/* Reverse-time consumption order of (memory, cb index, gain index)    */
/* triples, exactly as packetized:                                     */
/*   triple 0          : start-state adaptive completion (diff samp.)  */
/*   triples 1..Nfor   : forward subframes, oldest->newest             */
/*   triples 1+Nfor..  : backward subframes, consumed from the start   */
/*                      state outward; each produced vector is stored  */
/*                      in reverse time and flipped at the end.        */
/* ================================================================== */

static int model_residual(const struct mode_fixture *fx,
                           const struct frame_rec *r,
                           iLBC_Dec_Inst_t *lsf_state,
                           const int16_t *prev_residual,
                           int16_t *decresidual, int *cap_hit) {
  const int mode = fx->mode;
  const int nsub = mode == 20 ? NSUB_20MS : NSUB_30MS;
  const int ssl = mode == 20 ? STATE_SHORT_LEN_20MS : STATE_SHORT_LEN_30MS;
  const int diff = STATE_LEN - ssl;
  const int startIdx = r->startIdx;
  int start_pos;
  int16_t memVec[BLOCKL_MAX];
  int16_t reverse[BLOCKL_MAX];
  int16_t *mem;
  int k, sub;

  /* LSF decode and interpolation on the caller's independent decoder
   * state: this advances the lsfdeqold history exactly as production
   * does, keeping the reference on the same trajectory as the decoder
   * under test.  Only the LSF/LSP path is reused here. */
  int16_t lsfdeq[LPC_FILTERORDER * LPC_N_MAX];
  int16_t lsf_idx[LSF_NSPLIT * LPC_N_MAX];
  int16_t syntdenum[NSUB_MAX * (LPC_FILTERORDER + 1)];
  int16_t weightdenum[NSUB_MAX * (LPC_FILTERORDER + 1)];
  memcpy(lsf_idx, r->conv.lsf, sizeof(lsf_idx));
  WebRtcIlbcfix_SimpleLsfDeQ(lsfdeq, lsf_idx, lsf_state->lpc_n);
  WebRtcIlbcfix_LsfCheck(lsfdeq, LPC_FILTERORDER, lsf_state->lpc_n);
  WebRtcIlbcfix_DecoderInterpolateLsp(syntdenum, weightdenum, lsfdeq,
                                      LPC_FILTERORDER, lsf_state);

  memset(decresidual, 0, sizeof(int16_t) * (nsub * SUBL));
  memset(reverse, 0, sizeof(reverse));

  /* Carry the previous frame's residual exactly like the decoder:
   * memVec is prevResidual, codebook memory starts at offset +4. */
  memset(memVec, 0, sizeof(memVec));
  memcpy(memVec, prev_residual, sizeof(int16_t) * (nsub * SUBL));
  mem = memVec + CB_HALFFILTERLEN;

  if (r->state_first == 1) {
    start_pos = (startIdx - 1) * SUBL;
  } else {
    start_pos = (startIdx - 1) * SUBL + diff;
  }

  /* Scalar start block: the one production constructor reused.  The
   * production signature takes non-const pointers, so hand it copies. */
  int16_t idx_vec[STATE_SHORT_LEN_30MS];
  int16_t synt_den[LPC_FILTERORDER + 1];
  memcpy(idx_vec, r->fields.idxVec, sizeof(int16_t) * ssl);
  memcpy(synt_den,
         &syntdenum[(startIdx - 1) * (LPC_FILTERORDER + 1)],
         sizeof(synt_den));
  WebRtcIlbcfix_StateConstruct(r->fields.idxForMax, idx_vec, synt_den,
                               &decresidual[start_pos], ssl);

  if (r->state_first == 1) {
    /* Adaptive completion appended after the scalar block. */
    for (k = 0; k < CB_MEML - ssl; ++k) {
      mem[k] = 0;
    }
    for (k = 0; k < ssl; ++k) {
      mem[CB_MEML - ssl + k] = decresidual[start_pos + k];
    }
    ref_cb_construct(&decresidual[start_pos + ssl], r->conv.cb_index,
                     r->conv.gain_index, mem + CB_MEML - ST_MEM_L_TBL,
                     ST_MEM_L_TBL, (int16_t)diff);
  } else {
    /* Adaptive completion built backwards, in front of the scalar block. */
    for (k = 0; k < ssl; ++k) {
      mem[CB_MEML - 1 - k] = decresidual[start_pos + k];
    }
    for (k = 0; k < CB_MEML - ssl; ++k) {
      mem[k] = 0;
    }
    ref_cb_construct(reverse, r->conv.cb_index, r->conv.gain_index,
                     mem + CB_MEML - ST_MEM_L_TBL, ST_MEM_L_TBL,
                     (int16_t)diff);
    for (k = 0; k < diff; ++k) {
      decresidual[start_pos - 1 - k] = reverse[k];
    }
  }

  /* Forward subframes: absolute positions (startIdx+1+sub)*40. */
  int Nfor = nsub - startIdx - 1;
  if (Nfor > 0) {
    for (k = 0; k < CB_MEML - STATE_LEN; ++k) {
      mem[k] = 0;
    }
    for (k = 0; k < STATE_LEN; ++k) {
      mem[CB_MEML - STATE_LEN + k] =
          decresidual[(startIdx - 1) * SUBL + k];
    }
    for (sub = 0; sub < Nfor; ++sub) {
      int triple = 1 + sub;
      ref_cb_construct(&decresidual[(startIdx + 1 + sub) * SUBL],
                       r->conv.cb_index + triple * CB_NSTAGES,
                       r->conv.gain_index + triple * CB_NSTAGES,
                       mem, MEM_LF_TBL, SUBL);
      for (k = 0; k < CB_MEML - SUBL; ++k) {
        mem[k] = mem[k + SUBL];
      }
      for (k = 0; k < SUBL; ++k) {
        mem[CB_MEML - SUBL + k] =
            decresidual[(startIdx + 1 + sub) * SUBL + k];
      }
    }
  }

  /* Backward subframes: memory is filled in reverse time order. */
  int Nback = startIdx - 1;
  if (Nback > 0) {
    int gotten = SUBL * (nsub + 1 - startIdx);
    if (gotten > CB_MEML) {
      gotten = CB_MEML;
      *cap_hit = 1;
    }
    for (k = 0; k < gotten; ++k) {
      mem[CB_MEML - 1 - k] = decresidual[(startIdx - 1) * SUBL + k];
    }
    for (k = 0; k < CB_MEML - gotten; ++k) {
      mem[k] = 0;
    }
    for (sub = 0; sub < Nback; ++sub) {
      int triple = 1 + Nfor + sub;
      ref_cb_construct(&reverse[sub * SUBL],
                       r->conv.cb_index + triple * CB_NSTAGES,
                       r->conv.gain_index + triple * CB_NSTAGES,
                       mem, MEM_LF_TBL, SUBL);
      for (k = 0; k < CB_MEML - SUBL; ++k) {
        mem[k] = mem[k + SUBL];
      }
      for (k = 0; k < SUBL; ++k) {
        mem[CB_MEML - SUBL + k] = reverse[sub * SUBL + k];
      }
    }
    /* Flip the whole backward region into forward time. */
    for (k = 0; k < SUBL * Nback; ++k) {
      decresidual[SUBL * Nback - 1 - k] = reverse[k];
    }
  }
  return 0;
}

/* Decode one frame on a fresh decoder and return its prevResidual. */
static int actual_residual_fresh(const struct mode_fixture *fx,
                                  const struct frame_rec *r,
                                  int16_t *prev_out) {
  iLBC_decinst_t *dec = NULL;
  int16_t speech[BLOCKL_MAX];
  int16_t type = -1;
  CK(WebRtcIlbcfix_DecoderCreate(&dec) == 0);
  CK(WebRtcIlbcfix_DecoderInit(dec, fx->mode) == 0);
  int len = WebRtcIlbcfix_Decode(dec, r->bits, (int16_t)fx->bytes, speech,
                                 &type);
  CK(len == fx->samples);
  CK(type == ILBC_SPEECH);
  memcpy(prev_out, ((iLBC_Dec_Inst_t *)dec)->prevResidual,
         sizeof(int16_t) * fx->samples);
  WebRtcIlbcfix_DecoderFree(dec);
  return 0;
}

/* Run one full-frame residual comparison on a zero-history decoder. */
static int residual_cell(const char *label, int mode, int startIdx,
                         int state_first, int require_cap) {
  cur_group = "residual";
  cur_case = label;

  CK(ensure_fixtures() == 0);
  const struct mode_fixture *fx = mode == 20 ? &fixture20 : &fixture30;
  const struct frame_rec *r = find_rec(fx, startIdx, state_first);
  CK(r != NULL);

  int16_t zero_prev[BLOCKL_MAX];
  int16_t model[BLOCKL_MAX];
  int16_t actual[BLOCKL_MAX];
  int cap_hit = 0;
  memset(zero_prev, 0, sizeof(zero_prev));

  /* Independent LSF state with the same mean initialization as a fresh
   * decoder, so model and production interpolate from identical history. */
  iLBC_decinst_t *lsf_opaque = NULL;
  CK(WebRtcIlbcfix_DecoderCreate(&lsf_opaque) == 0);
  CK(WebRtcIlbcfix_DecoderInit(lsf_opaque, mode) == 0);
  CK(model_residual(fx, r, (iLBC_Dec_Inst_t *)lsf_opaque, zero_prev,
                    model, &cap_hit) == 0);
  CK(actual_residual_fresh(fx, r, actual) == 0);
  WebRtcIlbcfix_DecoderFree(lsf_opaque);

  if (require_cap) {
    CK(cap_hit);                 /* backward memory really clipped at 147 */
  }
  CK_VEC("frame_residual", actual, model, fx->samples);
  return 0;
}

static int residual_20_early(void) {
  /* startIdx=1: no backward subframes; both state_first layouts. */
  CK(residual_cell("20ms_start1_first0", 20, 1, 0, 0) == 0);
  CK(residual_cell("20ms_start1_first1", 20, 1, 1, 0) == 0);
  return 0;
}

static int residual_20_middle(void) {
  /* startIdx=2: one forward and one backward subframe. */
  CK(residual_cell("20ms_start2_first0", 20, 2, 0, 0) == 0);
  CK(residual_cell("20ms_start2_first1", 20, 2, 1, 0) == 0);
  return 0;
}

static int residual_20_late(void) {
  /* startIdx=3: backward-only region. */
  CK(residual_cell("20ms_start3_first0", 20, 3, 0, 0) == 0);
  CK(residual_cell("20ms_start3_first1", 20, 3, 1, 0) == 0);
  return 0;
}

static int residual_30_early(void) {
  CK(residual_cell("30ms_start1_first0", 30, 1, 0, 0) == 0);
  CK(residual_cell("30ms_start1_first1", 30, 1, 1, 0) == 0);
  return 0;
}

static int residual_30_middle_cap147(void) {
  /* startIdx=3: Nfor=2, Nback=2, raw backward memory is 160 samples and
   * is clipped to the 147-sample ceiling. */
  CK(residual_cell("30ms_start3_first0_cap", 30, 3, 0, 1) == 0);
  CK(residual_cell("30ms_start3_first1_cap", 30, 3, 1, 1) == 0);
  return 0;
}

static int residual_30_late(void) {
  CK(residual_cell("30ms_start5_first0", 30, 5, 0, 0) == 0);
  CK(residual_cell("30ms_start5_first1", 30, 5, 1, 0) == 0);
  return 0;
}

/* ================================================================== */
/* stream group: public Decode driven end-to-end checks                 */
/* ================================================================== */

/* Continuous legal stream; the model carries its own previous-residual
 * state and is compared to the decoder's prevResidual every frame. */
static int stream_continuous(int mode) {
  cur_group = "stream";
  cur_case = mode == 20 ? "continuous_20ms" : "continuous_30ms";

  CK(ensure_fixtures() == 0);
  const struct mode_fixture *fx = mode == 20 ? &fixture20 : &fixture30;

  iLBC_decinst_t *dec = NULL;
  iLBC_decinst_t *lsf_opaque = NULL;   /* model's independent LSF state */
  CK(WebRtcIlbcfix_DecoderCreate(&dec) == 0);
  CK(WebRtcIlbcfix_DecoderCreate(&lsf_opaque) == 0);
  CK(WebRtcIlbcfix_DecoderInit(dec, mode) == 0);
  CK(WebRtcIlbcfix_DecoderInit(lsf_opaque, mode) == 0);

  int16_t model_prev[BLOCKL_MAX];
  int16_t model[BLOCKL_MAX];
  int16_t actual[BLOCKL_MAX];
  int16_t speech[BLOCKL_MAX];
  memset(model_prev, 0, sizeof(model_prev));

  int cap_frames = 0;
  for (int f = 0; f < fx->frames; ++f) {
    const struct frame_rec *r = &fx->rec[f];
    int cap_hit = 0;
    CK(model_residual(fx, r, (iLBC_Dec_Inst_t *)lsf_opaque, model_prev,
                      model, &cap_hit) == 0);
    cap_frames += cap_hit;

    int16_t type = -1;
    int len = WebRtcIlbcfix_Decode(dec, r->bits, (int16_t)fx->bytes,
                                   speech, &type);
    CK(len == fx->samples);
    CK(type == ILBC_SPEECH);
    memcpy(actual, ((iLBC_Dec_Inst_t *)dec)->prevResidual,
           sizeof(int16_t) * fx->samples);

    char label[40];
    snprintf(label, sizeof(label), "stream_frame%d", f);
    CK_VEC(label, actual, model, fx->samples);

    /* Valid frames: PLC copies decresidual straight into prevResidual. */
    memcpy(model_prev, model, sizeof(int16_t) * fx->samples);
  }
  /* 30 ms startIdx=3 frames ask for 160 backward samples and hit the
   * 147-sample ceiling inside the continuous stream. */
  if (mode == 30) {
    CK(cap_frames > 0);
  }
  WebRtcIlbcfix_DecoderFree(dec);
  WebRtcIlbcfix_DecoderFree(lsf_opaque);
  return 0;
}

static int stream_continuous_20(void) {
  return stream_continuous(20);
}

static int stream_continuous_30(void) {
  return stream_continuous(30);
}

/* Reset-and-reuse: the same legal packet decoded on two brand-new
 * decoder instances must yield identical residuals; then a reset
 * instance continues a fresh stream correctly. */
static int stream_reset_reuse(void) {
  cur_group = "stream";
  cur_case = "reset_and_reuse";

  CK(ensure_fixtures() == 0);
  const struct mode_fixture *fx = &fixture30;
  const struct frame_rec *r = &fx->rec[7];

  int16_t a[BLOCKL_MAX], b[BLOCKL_MAX], speech[BLOCKL_MAX];
  int16_t type;
  iLBC_decinst_t *d1 = NULL, *d2 = NULL;

  CK(WebRtcIlbcfix_DecoderCreate(&d1) == 0);
  CK(WebRtcIlbcfix_DecoderCreate(&d2) == 0);
  CK(WebRtcIlbcfix_DecoderInit(d1, 30) == 0);
  CK(WebRtcIlbcfix_DecoderInit(d2, 30) == 0);
  type = -1;
  CK(WebRtcIlbcfix_Decode(d1, r->bits, fx->bytes, speech, &type) ==
     fx->samples);
  CK(type == ILBC_SPEECH);
  memcpy(a, ((iLBC_Dec_Inst_t *)d1)->prevResidual, sizeof(a));
  type = -1;
  CK(WebRtcIlbcfix_Decode(d2, r->bits, fx->bytes, speech, &type) ==
     fx->samples);
  CK(type == ILBC_SPEECH);
  memcpy(b, ((iLBC_Dec_Inst_t *)d2)->prevResidual, sizeof(b));
  CK_VEC("reset_same_packet", b, a, fx->samples);

  /* Re-init d1 and replay the first 8 frames; output must match a
   * third fresh instance following the same sequence. */
  CK(WebRtcIlbcfix_DecoderInit(d1, 30) == 0);
  iLBC_decinst_t *d3 = NULL;
  CK(WebRtcIlbcfix_DecoderCreate(&d3) == 0);
  CK(WebRtcIlbcfix_DecoderInit(d3, 30) == 0);
  for (int f = 0; f < 8; ++f) {
    int16_t s1[BLOCKL_MAX], s3[BLOCKL_MAX], t1 = -1, t3 = -1;
    CK(WebRtcIlbcfix_Decode(d1, fx->rec[f].bits, fx->bytes, s1, &t1) ==
       fx->samples);
    CK(WebRtcIlbcfix_Decode(d3, fx->rec[f].bits, fx->bytes, s3, &t3) ==
       fx->samples);
    CK(t1 == ILBC_SPEECH && t3 == ILBC_SPEECH);
    int16_t p1[BLOCKL_MAX], p3[BLOCKL_MAX];
    memcpy(p1, ((iLBC_Dec_Inst_t *)d1)->prevResidual, sizeof(p1));
    memcpy(p3, ((iLBC_Dec_Inst_t *)d3)->prevResidual, sizeof(p3));
    char label[32];
    snprintf(label, sizeof(label), "reuse_frame%d", f);
    CK_VEC(label, p3, p1, fx->samples);
  }
  WebRtcIlbcfix_DecoderFree(d1);
  WebRtcIlbcfix_DecoderFree(d2);
  WebRtcIlbcfix_DecoderFree(d3);
  return 0;
}

/* Two instances interleaved on disjoint frame subsequences; each model
 * track keeps its own previous-residual history. */
static int stream_interleaved(int mode) {
  cur_group = "stream";
  cur_case = mode == 20 ? "interleaved_20ms" : "interleaved_30ms";

  CK(ensure_fixtures() == 0);
  const struct mode_fixture *fx = mode == 20 ? &fixture20 : &fixture30;

  iLBC_decinst_t *da = NULL, *db = NULL;
  iLBC_decinst_t *la = NULL, *lb = NULL;   /* per-track LSF states */
  CK(WebRtcIlbcfix_DecoderCreate(&da) == 0);
  CK(WebRtcIlbcfix_DecoderCreate(&db) == 0);
  CK(WebRtcIlbcfix_DecoderCreate(&la) == 0);
  CK(WebRtcIlbcfix_DecoderCreate(&lb) == 0);
  CK(WebRtcIlbcfix_DecoderInit(da, mode) == 0);
  CK(WebRtcIlbcfix_DecoderInit(db, mode) == 0);
  CK(WebRtcIlbcfix_DecoderInit(la, mode) == 0);
  CK(WebRtcIlbcfix_DecoderInit(lb, mode) == 0);

  int16_t prev_a[BLOCKL_MAX], prev_b[BLOCKL_MAX];
  int16_t model[BLOCKL_MAX], actual[BLOCKL_MAX], speech[BLOCKL_MAX];
  memset(prev_a, 0, sizeof(prev_a));
  memset(prev_b, 0, sizeof(prev_b));

  for (int f = 0; f + 1 < fx->frames; f += 2) {
    int cap = 0;
    int16_t type = -1;

    CK(model_residual(fx, &fx->rec[f], (iLBC_Dec_Inst_t *)la, prev_a,
                      model, &cap) == 0);
    CK(WebRtcIlbcfix_Decode(da, fx->rec[f].bits, fx->bytes, speech,
                            &type) == fx->samples);
    CK(type == ILBC_SPEECH);
    memcpy(actual, ((iLBC_Dec_Inst_t *)da)->prevResidual,
           sizeof(int16_t) * fx->samples);
    char label[40];
    snprintf(label, sizeof(label), "interleave_A_frame%d", f);
    CK_VEC(label, actual, model, fx->samples);
    memcpy(prev_a, model, sizeof(int16_t) * fx->samples);

    cap = 0;
    type = -1;
    CK(model_residual(fx, &fx->rec[f + 1], (iLBC_Dec_Inst_t *)lb, prev_b,
                      model, &cap) == 0);
    CK(WebRtcIlbcfix_Decode(db, fx->rec[f + 1].bits, fx->bytes, speech,
                            &type) == fx->samples);
    CK(type == ILBC_SPEECH);
    memcpy(actual, ((iLBC_Dec_Inst_t *)db)->prevResidual,
           sizeof(int16_t) * fx->samples);
    snprintf(label, sizeof(label), "interleave_B_frame%d", f + 1);
    CK_VEC(label, actual, model, fx->samples);
    memcpy(prev_b, model, sizeof(int16_t) * fx->samples);
  }
  WebRtcIlbcfix_DecoderFree(da);
  WebRtcIlbcfix_DecoderFree(db);
  WebRtcIlbcfix_DecoderFree(la);
  WebRtcIlbcfix_DecoderFree(lb);
  return 0;
}

static int stream_interleaved_20(void) {
  return stream_interleaved(20);
}

static int stream_interleaved_30(void) {
  return stream_interleaved(30);
}

/* Coverage is asserted from the real encoded frames, not frame counts:
 * every (startIdx, state_first) cell, all four GetCbVec sections, real
 * index conversions, centered frames and the 147-sample cap. */
static int stream_coverage(void) {
  cur_group = "stream";
  cur_case = "coverage_counts";

  CK(ensure_fixtures() == 0);

  for (int m = 0; m < 2; ++m) {
    const struct mode_fixture *fx = m == 0 ? &fixture20 : &fixture30;
    int npos = fx->samples / SUBL - 1;
    unsigned centered = 0, cap_eligible = 0;

    for (int s = 1; s <= npos; ++s) {
      for (int fi = 0; fi < 2; ++fi) {
        if (fx->cell[s][fi] == 0) {
          fprintf(stderr,
                  "CONTENT_MISMATCH group=stream case=coverage_counts "
                  "what=empty_cell mode=%d start=%d first=%d\n",
                  fx->mode, s, fi);
        }
        CK(fx->cell[s][fi] > 0);
      }
    }
    for (int sec = 0; sec < 4; ++sec) {
      CK(fx->section[sec] > 0);
    }
    CK(fx->converted > 0);
    /* The short start-state codebook has no augmented half, so its
     * triples must never land in sections 1 or 3. */
    CK(fx->state_section[1] == 0);
    CK(fx->state_section[3] == 0);
    CK(fx->state_section[0] + fx->state_section[2] > 0);

    for (int f = 0; f < fx->frames; ++f) {
      int s = fx->rec[f].startIdx;
      int nfor = (fx->samples / SUBL) - s - 1;
      int nback = s - 1;
      if (nfor > 0 && nback > 0) {
        centered++;
      }
      int gotten = SUBL * ((fx->samples / SUBL) + 1 - s);
      if (gotten > CB_MEML) {
        cap_eligible++;
      }
    }
    CK(centered > 0);
    if (fx->mode == 30) {
      CK(cap_eligible > 0);
    }

    printf("COVER mode=%d frames=%d converted=%u sections=%u/%u/%u/%u "
           "state_sections=%u/%u/%u/%u centered=%u cap147_eligible=%u cells=",
           fx->mode, fx->frames, fx->converted, fx->section[0],
           fx->section[1], fx->section[2], fx->section[3],
           fx->state_section[0], fx->state_section[1],
           fx->state_section[2], fx->state_section[3], centered,
           cap_eligible);
    for (int s = 1; s <= npos; ++s) {
      printf("%s%u/%u", s == 1 ? "" : ",", fx->cell[s][0],
             fx->cell[s][1]);
    }
    printf("\n");
  }
  return 0;
}

/* ================================================================== */
/* Registration (no main in this translation unit).                    */
/* ================================================================== */

static int vectors_short_case(void) { return vectors_short(); }
static int vectors_long_case(void) { return vectors_long40(); }
static int vectors_overlap_case(void) { return vectors_overlap_truncation(); }
static int vectors_gain_case(void) { return vectors_gain_chain(); }
static int vectors_filter_case(void) { return vectors_filter_convolution(); }

const struct task_case task_cases[] = {
    {"vectors", "short23_memory85", vectors_short_case},
    {"vectors", "long40_sections", vectors_long_case},
    {"vectors", "overlap_two_truncations", vectors_overlap_case},
    {"vectors", "gain_chain_weighted", vectors_gain_case},
    {"vectors", "filter_zero_extension", vectors_filter_case},

    {"residual", "20ms_early", residual_20_early},
    {"residual", "20ms_middle", residual_20_middle},
    {"residual", "20ms_late", residual_20_late},
    {"residual", "30ms_early", residual_30_early},
    {"residual", "30ms_middle_cap147", residual_30_middle_cap147},
    {"residual", "30ms_late", residual_30_late},

    {"stream", "continuous_20ms", stream_continuous_20},
    {"stream", "continuous_30ms", stream_continuous_30},
    {"stream", "reset_and_reuse", stream_reset_reuse},
    {"stream", "interleaved_20ms", stream_interleaved_20},
    {"stream", "interleaved_30ms", stream_interleaved_30},
    {"stream", "coverage_counts", stream_coverage},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
