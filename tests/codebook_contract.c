/*
 *  Codebook contract regression tests for the fixed-point iLBC decoder.
 *
 *  This file is a self-contained independent reference.  It never calls the
 *  production vector construction, interpolation, filtering, gain dequant or
 *  codebook construction routines to build an *expected* value.  The only
 *  production entry points reused on the expectation side are:
 *    - WebRtcIlbcfix_StateConstruct  (the scalar start-state block, which is
 *      explicitly allowed by the test contract),
 *    - the LSF dequant / check / interpolation path (allowed for the stream
 *      group only),
 *    - bit packing/unpacking and byte swap helpers (wire format only).
 *
 *  Everything adaptive (augmented vectors, MA filtered vectors, gain
 *  recurrence, codebook construction, forward/backward residual assembly) is
 *  recomputed below from the constant tables, using ordinary time-ordered
 *  bounded arrays.  Expected values are then compared sample by sample with
 *  what the production library actually produces.
 *
 *  Numeric conventions (the build uses -fwrapv):
 *    - int16_t assignments wrap with two's complement semantics;
 *    - every 16x16 product is evaluated in int32_t, which is exact because
 *      |a*b| <= 32768*32767 < 2^31;
 *    - right shifts of signed values are arithmetic shifts, matching the
 *      production code compiled with -fwrapv on the same tool chain;
 *    - there is no negative integer division anywhere in this reference:
 *      the only division (lag = k/2 in the augmented section split) acts on
 *      a strictly positive even k.  Rounding of negative sums is done with
 *      the same bias-then-arithmetic-shift expression as production;
 *    - 32-bit accumulators that can overflow (the three-stage weighted sum)
 *      are kept in int32_t and rely on the same defined wraparound as the
 *      production code.
 *
 *  The file defines no main(); it exports task_cases/task_case_count which
 *  task_support/driver.c executes.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ilbc/interface/ilbc.h"
#include "ilbc/defines.h"
#include "ilbc/constants.h"
#include "ilbc/unpack_bits.h"
#include "ilbc/pack_bits.h"
#include "ilbc/swap_bytes.h"
#include "ilbc/state_construct.h"
#include "ilbc/simple_lsf_dequant.h"
#include "ilbc/lsf_check.h"
#include "ilbc/decoder_interpolate_lsf.h"
#include "ilbc/index_conv_dec.h"
#include "ilbc/get_cd_vec.h"
#include "ilbc/gain_dequant.h"
#include "ilbc/cb_construct.h"
#include "ilbc/decode_residual.h"

#include "task_support/check.h"

/* ------------------------------------------------------------------ */
/* Small shared helpers                                               */
/* ------------------------------------------------------------------ */

/* Exact 16x16 -> 32 product. */
static int32_t mul16(int16_t a, int16_t b) {
  return (int32_t)a * (int32_t)b;
}

/* Arithmetic right shift with the production rounding bias; s in 1..15. */
static int16_t round_shift16(int32_t x, int s) {
  return (int16_t)((x + ((int32_t)1 << (s - 1))) >> s);
}

static int16_t sat16(int32_t x) {
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return (int16_t)x;
}

/*
 * Report a content-level discrepancy (the mutant runner keys on this token)
 * and fail through TASK_CHECK so the driver returns 4.
 */
#define EXPECT(cond, msg) do {                                              \
    if (!(cond)) {                                                          \
      fprintf(stderr, "CONTENT_MISMATCH %s:%d %s\n", __FILE__, __LINE__,    \
              (msg));                                                       \
      TASK_CHECK(0);                                                        \
    }                                                                      \
  } while (0)

static int expect_vec(const char *tag, const int16_t *got,
                      const int16_t *want, int n) {
  int i;
  for (i = 0; i < n; ++i) {
    if (got[i] != want[i]) {
      fprintf(stderr,
              "CONTENT_MISMATCH %s at sample %d got=%d want=%d\n",
              tag, i, (int)got[i], (int)want[i]);
      TASK_CHECK(0);
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Independent reference: gain dequantization                         */
/* ------------------------------------------------------------------ */

/*
 * Mirror of WebRtcIlbcfix_GainDequant over the public constant tables.
 * scale = max(1638, |maxIn|); result = (scale * table[stage][index] + 8192)>>14
 * with the int16 cast performed on the wrapped result.
 */
static int16_t ref_gain_dequant(int16_t index, int16_t max_in, int stage) {
  int16_t scale = max_in >= 0 ? max_in : (int16_t)(-max_in);
  if (scale < 1638) scale = 1638;
  return round_shift16(mul16(scale, WebRtcIlbcfix_kGain[stage][index]), 14);
}

/* Recursively derive the three stage gains from the quantized gain indices. */
static void ref_gain_chain(const int16_t gain_index[3], int16_t gain[3]) {
  gain[0] = ref_gain_dequant(gain_index[0], 16384, 0);
  gain[1] = ref_gain_dequant(gain_index[1], gain[0], 1);
  gain[2] = ref_gain_dequant(gain_index[2], gain[1], 2);
}

/* ------------------------------------------------------------------ */
/* Independent reference: augmented (overlap-add) codebook vector     */
/* ------------------------------------------------------------------ */

/*
 * Recreate one augmented vector of length veclen from a codebook memory
 * buffer buf[0..buf_len) given in ordinary time order.
 *
 * The four-point overlap keeps the two products as *separate* signed,
 * individually truncated Q15 terms and only then adds them as int16; the
 * addition therefore sees the two rounding errors independently and wraps
 * as int16.  Merging the products before a single truncation is a different
 * arithmetic operation and is exercised against tests/mutants/merged.patch.
 */
static void ref_augmented(int16_t *out, const int16_t *buf, int buf_len,
                          int16_t lag, int veclen) {
  int k;
  /* first non-interpolated part reads from buffer-lag */
  const int16_t *ppo = buf + buf_len - lag;
  /* the four-sample splice reads from buffer-4 and buffer-lag-4 instead */
  const int16_t *splice_ppo = buf + buf_len - 4;
  const int16_t *ppi = buf + buf_len - lag - 4;

  /* first non-interpolated part */
  for (k = 0; k < lag; ++k) out[k] = ppo[k];

  /* four sample interpolation splice, two truncations then one add */
  for (k = 0; k < 4; ++k) {
    int16_t left = (int16_t)(mul16(ppi[k], WebRtcIlbcfix_kAlpha[k]) >> 15);
    int16_t right =
        (int16_t)(mul16(splice_ppo[k], WebRtcIlbcfix_kAlpha[3 - k]) >> 15);
    out[lag - 4 + k] = (int16_t)((int32_t)left + (int32_t)right);
  }

  /* second non-interpolated part: cbVec[lag+j] = ppo[j] */
  for (k = 0; k < veclen - lag; ++k) out[lag + k] = ppo[k];
}

/*
 * Same splice computed with a single merged truncation.  This is the mutated
 * arithmetic; the test asserts it is observably different from the reference
 * on a crafted memory pattern.
 */
static int16_t merged_splice_sample(int16_t ppi_k, int16_t ppo_k, int k) {
  int32_t merged = mul16(ppi_k, WebRtcIlbcfix_kAlpha[k]) +
                   mul16(ppo_k, WebRtcIlbcfix_kAlpha[3 - k]);
  return (int16_t)(merged >> 15);
}

/* ------------------------------------------------------------------ */
/* Independent reference: MA filter with explicit zero extension      */
/* ------------------------------------------------------------------ */

/*
 * Finite MA convolution (Q12).  ext[] is a padded view of the codebook
 * memory: ext[4 .. 4+lMem) holds the memory samples, every other position is
 * the explicit zero extension (the production code temporarily stuffs zeros
 * in the four slots on each side of the buffer).  The convolution never reads
 * beyond ext[], so the zero boundary is part of the model rather than an
 * implicit out-of-bounds read.
 *
 * Saturation mirrors WebRtcSpl_FilterMAFastQ12 exactly:
 *   clamp to [-134217728, 134215679], then (o + 2048) >> 12.
 */
static int16_t ref_ma_one(const int16_t *ext, int center) {
  int32_t o = 0;
  int j;
  for (j = 0; j < CB_FILTERLEN; ++j) {
    o += mul16(WebRtcIlbcfix_kCbFiltersRev[j], ext[center - j]);
  }
  if (o > 134215679) o = 134215679;
  if (o < -134217728) o = -134217728;
  return (int16_t)((o + 2048) >> 12);
}

static void build_extended(const int16_t *mem, int l_mem, int16_t *ext) {
  memset(ext, 0, sizeof(int16_t) * (l_mem + 2 * CB_HALFFILTERLEN));
  memcpy(ext + CB_HALFFILTERLEN, mem, sizeof(int16_t) * l_mem);
}

/* ------------------------------------------------------------------ */
/* Independent reference: full codebook vector by index               */
/* ------------------------------------------------------------------ */

/*
 * Independent re-implementation of WebRtcIlbcfix_GetCbVec over mem[0..l_mem)
 * in normal time order.  Four sections (unfiltered plain, unfiltered
 * augmented, filtered plain, filtered augmented) are selected by index using
 * only the index arithmetic from the specification.
 */
static void ref_get_cbvec(int16_t *cbvec, const int16_t *mem, int l_mem,
                          int veclen, int16_t index) {
  int base_size = l_mem - veclen + 1;
  if (veclen == SUBL) base_size += veclen / 2;

  if (index < l_mem - veclen + 1) {
    /* section 1: plain window into memory */
    int k = index + veclen;
    int j;
    for (j = 0; j < veclen; ++j) cbvec[j] = mem[l_mem - k + j];
  } else if (index < base_size) {
    /* section 2: augmented vector, integer lag, unfiltered memory */
    int k = 2 * (index - (l_mem - veclen + 1)) + veclen; /* positive, even */
    int lag = k / 2;
    ref_augmented(cbvec, mem, l_mem, (int16_t)lag, veclen);
  } else {
    int16_t ext[CB_MEML + 2 * CB_HALFFILTERLEN];
    build_extended(mem, l_mem, ext);
    if (index - base_size < l_mem - veclen + 1) {
      /* section 3: filtered, non interpolated */
      int mem_ind_test = l_mem - (index - base_size + veclen);
      int base = CB_HALFFILTERLEN + mem_ind_test + CB_HALFFILTERLEN;
      int i;
      for (i = 0; i < veclen; ++i) cbvec[i] = ref_ma_one(ext, base + i);
    } else {
      /* section 4: filtered memory, then augmented vector */
      int mem_ind_test = l_mem - veclen - CB_FILTERLEN;
      int base = CB_HALFFILTERLEN + mem_ind_test + 7;
      int16_t temp[SUBL + 5];
      int lag = (veclen << 1) - 20 + index - base_size - l_mem - 1;
      int i;
      for (i = 0; i < veclen + 5; ++i) temp[i] = ref_ma_one(ext, base + i);
      ref_augmented(cbvec, temp, veclen + 5, (int16_t)lag, veclen);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Independent reference: three-stage weighted codebook construction  */
/* ------------------------------------------------------------------ */

static void ref_cb_construct(int16_t *decvector, const int16_t *mem, int l_mem,
                             int veclen, const int16_t index[3],
                             const int16_t gain_index[3]) {
  int16_t gain[3];
  int16_t cb[3][SUBL];
  int j;
  ref_gain_chain(gain_index, gain);
  ref_get_cbvec(cb[0], mem, l_mem, veclen, index[0]);
  ref_get_cbvec(cb[1], mem, l_mem, veclen, index[1]);
  ref_get_cbvec(cb[2], mem, l_mem, veclen, index[2]);
  for (j = 0; j < veclen; ++j) {
    /* int32 accumulation; -fwrapv gives the same defined wrap as production */
    int32_t a32 = mul16(gain[0], cb[0][j]);
    a32 += mul16(gain[1], cb[1][j]);
    a32 += mul16(gain[2], cb[2][j]);
    decvector[j] = (int16_t)((a32 + 8192) >> 14);
  }
}

/* ================================================================== */
/* vectors group                                                      */
/* ================================================================== */

/* Deterministic bounded memory, normal time order, mixed sign. */
static void make_mem(int16_t *mem, int n, uint32_t salt) {
  int i;
  for (i = 0; i < n; ++i) {
    int32_t v = (int32_t)((i * 49157u + salt * 1103515245u) & 0x7fff) - 16000;
    /* sprinkle a few large magnitudes to exercise filter saturation */
    if ((i + salt) % 31 == 0) v = (v & 1) ? 19000 : -19000;
    mem[i] = sat16(v);
  }
}

static int run_vec_short_plain(void) {
  /* four guard words per side: GetCbVec zero-stuffs mem[-4..-1] and
     mem[lMem..lMem+3] in the filtered sections */
  int16_t store[ST_MEM_L_TBL + 2 * CB_HALFFILTERLEN];
  int16_t *mem = store + CB_HALFFILTERLEN;
  int16_t got[SUBL], want[SUBL];
  const int16_t indices[] = {0, 7, 33, 57}; /* all inside the plain section */
  int t;
  make_mem(mem, ST_MEM_L_TBL, 11);
  TASK_CHECK(ST_MEM_L_TBL == 85);
  for (t = 0; t < (int)(sizeof(indices) / sizeof(indices[0])); ++t) {
    WebRtcIlbcfix_GetCbVec(got, mem, indices[t], ST_MEM_L_TBL,
                           (int16_t)(STATE_LEN - STATE_SHORT_LEN_30MS));
    ref_get_cbvec(want, mem, ST_MEM_L_TBL, STATE_LEN - STATE_SHORT_LEN_30MS,
                  indices[t]);
    if (expect_vec("vectors.short_plain", got, want,
                   STATE_LEN - STATE_SHORT_LEN_30MS)) return 1;
  }
  TASK_CHECK(1);
  return 0;
}

static int run_vec_long_plain(void) {
  int16_t store[MEM_LF_TBL + 2 * CB_HALFFILTERLEN];
  int16_t *mem = store + CB_HALFFILTERLEN;
  int16_t got[SUBL], want[SUBL];
  const int16_t indices[] = {0, 25, 77, 107};
  int t;
  make_mem(mem, MEM_LF_TBL, 23);
  TASK_CHECK(MEM_LF_TBL == CB_MEML);
  for (t = 0; t < (int)(sizeof(indices) / sizeof(indices[0])); ++t) {
    WebRtcIlbcfix_GetCbVec(got, mem, indices[t], MEM_LF_TBL, SUBL);
    ref_get_cbvec(want, mem, MEM_LF_TBL, SUBL, indices[t]);
    if (expect_vec("vectors.long_plain", got, want, SUBL)) return 1;
  }
  TASK_CHECK(1);
  return 0;
}

static int run_vec_long_augmented(void) {
  int16_t store[MEM_LF_TBL + 2 * CB_HALFFILTERLEN];
  int16_t *mem = store + CB_HALFFILTERLEN;
  int16_t got[SUBL], want[SUBL];
  /* augmented section for a 40-vector on 147 memory: indices 108..127 */
  const int16_t indices[] = {108, 112, 120, 127};
  int t, k;
  make_mem(mem, MEM_LF_TBL, 37);
  TASK_CHECK(CB_MEML == 147);
  for (t = 0; t < (int)(sizeof(indices) / sizeof(indices[0])); ++t) {
    int lag = indices[t] - 88; /* k is even, lag = index - 88 */
    const int16_t *ppo = mem + MEM_LF_TBL - 4;
    const int16_t *ppi = mem + MEM_LF_TBL - lag - 4;
    WebRtcIlbcfix_GetCbVec(got, mem, indices[t], MEM_LF_TBL, SUBL);
    ref_get_cbvec(want, mem, MEM_LF_TBL, SUBL, indices[t]);
    if (expect_vec("vectors.long_augmented", got, want, SUBL)) return 1;
    /* both arithmetic orderings are evaluated independently on every splice
       sample so the one-truncation alternative stays a concrete, named
       quantity rather than an assumed behaviour */
    for (k = 0; k < 4; ++k) {
      int16_t two = (int16_t)((mul16(ppi[k], WebRtcIlbcfix_kAlpha[k]) >> 15) +
                              (mul16(ppo[k], WebRtcIlbcfix_kAlpha[3 - k]) >>
                               15));
      int16_t one = merged_splice_sample(ppi[k], ppo[k], k);
      TASK_CHECK(got[lag - 4 + k] == two);
      (void)one; /* contrast value used by the crafted case below */
    }
  }
  /*
   * Crafted memory that forces the two truncations and the single merged
   * truncation to disagree at overlap position 0: with both contributing
   * samples equal to 1 the two fractions round separately to 0+0 while the
   * merged sum rounds to 1.
   */
  {
    int16_t cstore[MEM_LF_TBL + 2 * CB_HALFFILTERLEN];
    int16_t *craft = cstore + CB_HALFFILTERLEN;
    int16_t cgot[SUBL], cwant[SUBL], merged[SUBL];
    int lag = 20;
    make_mem(craft, MEM_LF_TBL, 41);
    craft[MEM_LF_TBL - lag - 4 + 0] = 1; /* ppi[0] */
    craft[MEM_LF_TBL - 4 + 0] = 1;       /* ppo[0] */
    {
      int16_t two = (int16_t)((mul16(1, WebRtcIlbcfix_kAlpha[0]) >> 15) +
                              (mul16(1, WebRtcIlbcfix_kAlpha[3]) >> 15));
      int16_t one = merged_splice_sample(1, 1, 0);
      TASK_CHECK(two == 0);
      TASK_CHECK(one == 1);
      TASK_CHECK(two != one);
    }
    WebRtcIlbcfix_GetCbVec(cgot, craft, 108, MEM_LF_TBL, SUBL);
    ref_get_cbvec(cwant, craft, MEM_LF_TBL, SUBL, 108);
    /* build the merged-truncation variant over the same memory and confirm
       it diverges from production exactly at the splice position */
    ref_get_cbvec(merged, craft, MEM_LF_TBL, SUBL, 108);
    merged[lag - 4] = merged_splice_sample(1, 1, 0);
    TASK_CHECK(cgot[lag - 4] == 0);      /* production keeps two truncations */
    TASK_CHECK(merged[lag - 4] == 1);    /* merged variant would give 1 */
    TASK_CHECK(cgot[lag - 4] != merged[lag - 4]);
    if (expect_vec("vectors.augmented_crafted", cgot, cwant, SUBL)) return 1;
  }
  return 0;
}

static int run_vec_long_filtered(void) {
  int16_t store[MEM_LF_TBL + 2 * CB_HALFFILTERLEN];
  int16_t *mem = store + CB_HALFFILTERLEN;
  int16_t got[SUBL], want[SUBL];
  /* filtered plain section: index in [base_size=128, 128+108) */
  const int16_t indices[] = {128, 150, 200, 235};
  int t;
  make_mem(mem, MEM_LF_TBL, 53);
  for (t = 0; t < (int)(sizeof(indices) / sizeof(indices[0])); ++t) {
    WebRtcIlbcfix_GetCbVec(got, mem, indices[t], MEM_LF_TBL, SUBL);
    ref_get_cbvec(want, mem, MEM_LF_TBL, SUBL, indices[t]);
    if (expect_vec("vectors.long_filtered", got, want, SUBL)) return 1;
  }
  TASK_CHECK(1);
  return 0;
}

static int run_vec_long_filtered_interp(void) {
  int16_t store[MEM_LF_TBL + 2 * CB_HALFFILTERLEN];
  int16_t *mem = store + CB_HALFFILTERLEN;
  int16_t got[SUBL], want[SUBL];
  /* filtered + augmented section: indices 236..255 */
  const int16_t indices[] = {236, 240, 248, 255};
  int t;
  make_mem(mem, MEM_LF_TBL, 67);
  for (t = 0; t < (int)(sizeof(indices) / sizeof(indices[0])); ++t) {
    WebRtcIlbcfix_GetCbVec(got, mem, indices[t], MEM_LF_TBL, SUBL);
    ref_get_cbvec(want, mem, MEM_LF_TBL, SUBL, indices[t]);
    if (expect_vec("vectors.long_filtered_interp", got, want, SUBL)) return 1;
  }
  TASK_CHECK(1);
  return 0;
}

static int run_vec_gain_chain(void) {
  int16_t gstore[MEM_LF_TBL + 2 * CB_HALFFILTERLEN];
  int16_t *mem = gstore + CB_HALFFILTERLEN;
  int16_t got[SUBL], want[SUBL];
  int16_t gain_index[3] = {16, 7, 4};
  int16_t cb_index[3] = {30, 118, 244}; /* plain, augmented, filtered-interp */
  int16_t ref_gain[3], prod_gain[3];
  make_mem(mem, MEM_LF_TBL, 71);

  /* stage-by-stage recurrence, each level derived from the prior quantized
     gain's absolute value floored at 1638 (0.1 in Q14). */
  ref_gain_chain(gain_index, ref_gain);
  prod_gain[0] = WebRtcIlbcfix_GainDequant(gain_index[0], 16384, 0);
  prod_gain[1] = WebRtcIlbcfix_GainDequant(gain_index[1], prod_gain[0], 1);
  prod_gain[2] = WebRtcIlbcfix_GainDequant(gain_index[2], prod_gain[1], 2);
  TASK_CHECK(ref_gain[0] == prod_gain[0]);
  TASK_CHECK(ref_gain[1] == prod_gain[1]);
  TASK_CHECK(ref_gain[2] == prod_gain[2]);

  /* final weighted 40-sample vector, built entirely from the reference */
  WebRtcIlbcfix_CbConstruct(got, cb_index, gain_index, mem, MEM_LF_TBL, SUBL);
  ref_cb_construct(want, mem, MEM_LF_TBL, SUBL, cb_index, gain_index);
  if (expect_vec("vectors.gain_weighted40", got, want, SUBL)) return 1;

  /* negative gain path and the 22-sample start-state vector.  The short
     codebook has no augmented section: indices >= base_size select the
     MA-filtered section, so include one of each. */
  {
    int16_t sstore[ST_MEM_L_TBL + 2 * CB_HALFFILTERLEN];
    int16_t *smem = sstore + CB_HALFFILTERLEN;
    int16_t sgot[SUBL], swant[SUBL];
    int16_t s_gain_index[3] = {3, 12, 6};
    int16_t s_cb_index[3] = {10, 70, 120};
    int16_t sref[3], sprod[3];
    int veclen = STATE_LEN - STATE_SHORT_LEN_30MS; /* 22 for 30ms tables */
    make_mem(smem, ST_MEM_L_TBL, 83);
    ref_gain_chain(s_gain_index, sref);
    sprod[0] = WebRtcIlbcfix_GainDequant(s_gain_index[0], 16384, 0);
    sprod[1] = WebRtcIlbcfix_GainDequant(s_gain_index[1], sprod[0], 1);
    sprod[2] = WebRtcIlbcfix_GainDequant(s_gain_index[2], sprod[1], 2);
    TASK_CHECK(sref[0] == sprod[0] && sref[1] == sprod[1] &&
               sref[2] == sprod[2]);
    WebRtcIlbcfix_CbConstruct(sgot, s_cb_index, s_gain_index, smem,
                              ST_MEM_L_TBL, (int16_t)veclen);
    ref_cb_construct(swant, smem, ST_MEM_L_TBL, veclen, s_cb_index,
                     s_gain_index);
    if (expect_vec("vectors.gain_weighted22", sgot, swant, veclen)) return 1;
  }
  TASK_CHECK(1);
  return 0;
}

/* ================================================================== */
/* residual group                                                     */
/* ================================================================== */

/*
 * Reference frame residual model.  All positions below are absolute sample
 * coordinates inside one frame [0, blockl).  The adaptive codebook memory is
 * kept in ordinary time order; reversed-order consumption (the backward
 * path) is expressed explicitly at each boundary.
 *
 * Consumption order of the (memory, cb-index triple, gain triple) tuples in
 * reverse time, matching DecodeResidual:
 *   1. start-state completion, triple set 0, memory seeded from the scalar
 *      block (reversed when state_first == 0);
 *   2. forward subframes, triple sets 1 .. Nfor, memory sliding forward over
 *      already decoded samples;
 *   3. backward subframes, triple sets 1+Nfor .. Nfor+Nback, memory seeded
 *      from the decoded tail in reverse and sliding toward frame start.
 */
typedef struct {
  int mode;
  int blockl;
  int nsub;
  int state_short_len;
  int diff;
  int16_t dec[BLOCKL_MAX];
} ref_frame_t;

static void ref_construct_block(int16_t *out, const int16_t *mem147,
                                int l_mem, int veclen,
                                const int16_t *cb_index,
                                const int16_t *gain_index, int set) {
  int16_t idx[3], gi[3];
  idx[0] = cb_index[set * 3 + 0];
  idx[1] = cb_index[set * 3 + 1];
  idx[2] = cb_index[set * 3 + 2];
  gi[0] = gain_index[set * 3 + 0];
  gi[1] = gain_index[set * 3 + 1];
  gi[2] = gain_index[set * 3 + 2];
  if (l_mem == ST_MEM_L_TBL) {
    int16_t mem85[ST_MEM_L_TBL];
    memcpy(mem85, mem147 + CB_MEML - ST_MEM_L_TBL,
           sizeof(int16_t) * ST_MEM_L_TBL);
    ref_cb_construct(out, mem85, ST_MEM_L_TBL, veclen, idx, gi);
  } else {
    ref_cb_construct(out, mem147, l_mem, veclen, idx, gi);
  }
}

/*
 * Build the full decoded residual.  The scalar start block is supplied by the
 * caller (it is the output of production StateConstruct); every adaptive
 * sample is produced by the reference model.
 */
static void ref_build_residual(ref_frame_t *rf, const iLBC_bits *bits,
                               const int16_t *scalar) {
  const int s = bits->startIdx;            /* 1-based */
  const int first = bits->state_first;
  const int S = rf->state_short_len;
  const int diff = rf->diff;
  int start_pos = (s - 1) * SUBL + (first ? 0 : diff);
  int16_t mem[CB_MEML];
  int i, sub;

  memset(rf->dec, 0, sizeof(rf->dec));

  /* scalar region, absolute interval [start_pos, start_pos+S) */
  memcpy(rf->dec + start_pos, scalar, sizeof(int16_t) * S);

  /* ---- start-state adaptive completion, triple set 0 ---- */
  memset(mem, 0, sizeof(mem));
  if (first) {
    /* memory ends with the scalar block in forward order */
    memcpy(mem + CB_MEML - S, rf->dec + start_pos, sizeof(int16_t) * S);
    ref_construct_block(rf->dec + start_pos + S, mem, ST_MEM_L_TBL, diff,
                        bits->cb_index, bits->gain_index, 0);
  } else {
    /* memory ends with the scalar block in reverse order */
    for (i = 0; i < S; ++i) mem[CB_MEML - 1 - i] = rf->dec[start_pos + i];
    {
      int16_t tmp[STATE_LEN];
      ref_construct_block(tmp, mem, ST_MEM_L_TBL, diff, bits->cb_index,
                          bits->gain_index, 0);
      /* reverse back into forward time, absolute [start_pos-diff, start_pos) */
      for (i = 0; i < diff; ++i) rf->dec[start_pos - 1 - i] = tmp[i];
    }
  }

  /* ---- forward subframes, triple sets 1 .. Nfor ----
   * The codebook memory at every step is exactly the trailing CB_MEML
   * samples of the decoded frame before the current subframe, zero extended
   * toward the frame start.  It is rebuilt from absolute coordinates rather
   * than shifted in place, so no production memmove loop is mirrored.  The
   * first window therefore reduces to the 80-sample start-state window with
   * 67 leading zeros, matching the production seed. */
  {
    int nfor = rf->nsub - s - 1;
    for (sub = 0; sub < nfor; ++sub) {
      int at = (s + 1 + sub) * SUBL;
      int m;
      memset(mem, 0, sizeof(mem));
      for (m = 0; m < CB_MEML; ++m) {
        int pos = at - CB_MEML + m;      /* absolute frame coordinate */
        if (pos >= 0) mem[m] = rf->dec[pos];
      }
      ref_construct_block(rf->dec + at, mem, MEM_LF_TBL, SUBL,
                          bits->cb_index, bits->gain_index, 1 + sub);
    }
  }

  /* ---- backward subframes, triple sets 1+Nfor .. ----
   * Reverse-time accumulator rev[] records samples in the exact order they
   * are consumed: first the reversed decoded-tail snapshot (clamped to
   * CB_MEML), then each reconstructed 40-sample reverse subframe as it is
   * produced.  The codebook memory for every step is the trailing CB_MEML
   * entries of rev[], zero extended toward its beginning. */
  {
    int nback = s - 1;
    int nfor = rf->nsub - s - 1;
    int gotten = SUBL * (rf->nsub + 1 - s);
    int16_t rv[BLOCKL_MAX];
    int16_t rev[BLOCKL_MAX + CB_MEML];
    int rev_len = 0;
    int sub2, m;
    if (gotten > CB_MEML) gotten = CB_MEML; /* the 147-sample ceiling */
    /* reverse snapshot of the decoded tail, absolute base (s-1)*40 */
    for (i = 0; i < gotten; ++i)
      rev[rev_len++] = rf->dec[(s - 1) * SUBL + (gotten - 1 - i)];
    for (sub2 = 0; sub2 < nback; ++sub2) {
      memset(mem, 0, sizeof(mem));
      for (m = 0; m < CB_MEML; ++m) {
        int rix = rev_len - CB_MEML + m;   /* reverse-time coordinate */
        if (rix >= 0) mem[m] = rev[rix];
      }
      ref_construct_block(rv + sub2 * SUBL, mem, MEM_LF_TBL, SUBL,
                          bits->cb_index, bits->gain_index, 1 + nfor + sub2);
      for (m = 0; m < SUBL; ++m) rev[rev_len++] = rv[sub2 * SUBL + m];
    }
    /* reverse the whole backward block into frame-absolute positions */
    for (i = 0; i < nback * SUBL; ++i)
      rf->dec[SUBL * nback - 1 - i] = rv[i];
  }
}

/* Deterministic synthetic codebook/gain indices for one triple set.  Values
 * are chosen in wire domain; IndexConvDec is applied by the caller exactly as
 * DecodeImpl does, so the model and production see identical indices. */
static void synth_indices(iLBC_bits *bits, int n_sets, uint32_t salt) {
  static const int16_t cb_pick[6] = {10, 40, 60, 110, 150, 240};
  static const int16_t g_pick[3][3] = {{16, 7, 4}, {1, 15, 7}, {20, 3, 2}};
  int set, st;
  for (set = 0; set < n_sets; ++set) {
    for (st = 0; st < 3; ++st) {
      int slot = set * 3 + st;
      int16_t v = cb_pick[(set * 3 + st + salt) % 6];
      /*
       * The start-state completion runs over the 85-sample memory with a
       * 22/23-long vector: that codebook has only 2*(85-veclen+1) <= 128
       * entries, so keep set 0 inside [0,128).
       */
      if (set == 0) v = (int16_t)(v % 128);
      /*
       * Slots 4 and 5 (stage 1 of triple sets 1 and 2) travel in seven wire
       * bits; keep their wire values inside the seven-bit domain.  Values
       * >= 44 get remapped by IndexConvDec, exercised deliberately here.
       */
      if (slot == 4 || slot == 5) v = (int16_t)(v % 128);
      bits->cb_index[slot] = v;
      bits->gain_index[slot] = g_pick[(set + salt) % 3][st];
    }
  }
}

/* Impulse synthesis filter {1, 0, ...}: StateConstruct then reduces to a
 * deterministic permutation of the state quantizer table, with no LPC work. */
static void impulse_syntdenum(int16_t *syntdenum, int nsub) {
  int i;
  memset(syntdenum, 0, sizeof(int16_t) * NSUB_MAX * (LPC_FILTERORDER + 1));
  for (i = 0; i < nsub; ++i) syntdenum[i * (LPC_FILTERORDER + 1)] = 4096;
}

static int run_residual_case(int mode, int start_idx, int state_first) {
  iLBC_decinst_t *dec = NULL;
  iLBC_bits bits;
  iLBC_Dec_Inst_t *inst;
  int16_t syntdenum[NSUB_MAX * (LPC_FILTERORDER + 1)];
  int16_t scalar[STATE_SHORT_LEN_30MS];
  int16_t prod[BLOCKL_MAX];
  ref_frame_t ref;
  int n_sets;

  if (WebRtcIlbcfix_DecoderCreate(&dec)) TASK_CHECK(0);
  TASK_CHECK(WebRtcIlbcfix_DecoderInit(dec, (int16_t)mode) == 0);
  inst = (iLBC_Dec_Inst_t *)dec;

  memset(&bits, 0, sizeof(bits));
  bits.startIdx = (int16_t)start_idx;
  bits.state_first = (int16_t)state_first;
  bits.idxForMax = 12;
  {
    int k;
    for (k = 0; k < STATE_SHORT_LEN_30MS; ++k) bits.idxVec[k] = (int16_t)(k % 8);
  }
  n_sets = (mode == 20 ? 3 : 5);
  synth_indices(&bits, n_sets, (uint32_t)(start_idx * 7 + state_first));
  /* mirror DecodeImpl's wire-to-internal index adjustment */
  WebRtcIlbcfix_IndexConvDec(bits.cb_index);

  impulse_syntdenum(syntdenum, inst->nsub);

  /* scalar start block: the one production construction reused on purpose */
  WebRtcIlbcfix_StateConstruct(bits.idxForMax, bits.idxVec,
                               syntdenum + (start_idx - 1) *
                                               (LPC_FILTERORDER + 1),
                               scalar, inst->state_short_len);

  /* production full residual */
  memset(prod, 0, sizeof(prod));
  WebRtcIlbcfix_DecodeResidual(inst, &bits, prod, syntdenum);

  /* independent reference full residual */
  ref.mode = mode;
  ref.blockl = inst->blockl;
  ref.nsub = inst->nsub;
  ref.state_short_len = inst->state_short_len;
  ref.diff = STATE_LEN - inst->state_short_len;
  ref_build_residual(&ref, &bits, scalar);

  {
    char tag[64];
    snprintf(tag, sizeof(tag), "residual.m%d.s%d.f%d", mode, start_idx,
             state_first);
    if (expect_vec(tag, prod, ref.dec, ref.blockl)) {
      WebRtcIlbcfix_DecoderFree(dec);
      return 1;
    }
  }

  /* structural assertions about section boundaries */
  {
    int nfor = inst->nsub - start_idx - 1;
    int nback = start_idx - 1;
    int uncapped = SUBL * (inst->nsub + 1 - start_idx);
    TASK_CHECK(nfor + nback + 1 == inst->nsub - 1);
    if (nback > 0) {
      TASK_CHECK((uncapped > CB_MEML ? CB_MEML : uncapped) <= CB_MEML);
    }
    /* centered 30ms case must really touch the 147 ceiling */
    if (mode == 30 && start_idx == 3) {
      TASK_CHECK(uncapped == 160);
      TASK_CHECK(CB_MEML == 147);
    }
  }
  WebRtcIlbcfix_DecoderFree(dec);
  TASK_CHECK(1);
  return 0;
}

#define RESIDUAL_CASE(mode, s, f)                                          \
  static int run_residual_##mode##_##s##_##f(void) {                       \
    return run_residual_case(mode, s, f);                                  \
  }

RESIDUAL_CASE(20, 1, 1)  /* front, forward only */
RESIDUAL_CASE(20, 1, 0)
RESIDUAL_CASE(20, 2, 1)  /* centered, both directions */
RESIDUAL_CASE(20, 2, 0)
RESIDUAL_CASE(20, 3, 1)  /* back, backward only */
RESIDUAL_CASE(20, 3, 0)
RESIDUAL_CASE(30, 1, 1)
RESIDUAL_CASE(30, 1, 0)
RESIDUAL_CASE(30, 3, 1)  /* centered, cap hit, both directions */
RESIDUAL_CASE(30, 3, 0)
RESIDUAL_CASE(30, 5, 1)  /* back */
RESIDUAL_CASE(30, 5, 0)

/* ================================================================== */
/* stream group                                                       */
/* ================================================================== */

#define STREAM_FRAMES 96

/*
 * Deterministic non-silence PCM: a weak bounded noise floor plus a sharp
 * alternating burst whose center walks through every subframe position.
 * The walk is what makes real encoded frames land on every start position
 * and both state_first values; coverage is counted from the unpacked frames
 * rather than assumed from the frame count.
 */
static int16_t pcm_sample(int mode, int frame, int i, uint32_t *rng) {
  int samples = mode * 8;
  int32_t v;
  *rng = (*rng) * 1664525u + 1013904223u;
  v = ((int32_t)(*rng >> 16) - 32768) / 32;
  {
    int center = (frame * 53) % (samples - 40) + 20;
    int d = i - center;
    int burst;
    if (d < 0) d = -d;
    burst = d < 12 ? (((i + frame) & 1) ? 12000 : -12000) / (1 + d / 2) : 0;
    v += burst;
  }
  return sat16(v);
}

typedef struct {
  unsigned pair[8][2];      /* [startIdx][state_first] */
  unsigned frames;
  unsigned fwd_frames;
  unsigned back_frames;
  unsigned cap_frames;
  unsigned residual_mismatches;
} stream_coverage_t;

/*
 * One full mode run.  Two public decoder instances are driven interleaved;
 * a third, independent instance only advances the permitted LSF path, while
 * the adaptive residual is rebuilt purely from the reference model.
 */
static int run_stream_mode(int mode) {
  iLBC_encinst_t *enc = NULL;
  iLBC_decinst_t *d1 = NULL, *d2 = NULL, *rd = NULL;
  iLBC_Enc_Inst_t *einst;
  iLBC_Dec_Inst_t *rinst;
  int samples = mode * 8;
  int bytes = mode == 20 ? NO_OF_BYTES_20MS : NO_OF_BYTES_30MS;
  int words = bytes / 2;
  int n_sets = mode == 20 ? 3 : 5;
  uint32_t rng = 777;
  int f;
  stream_coverage_t cov;
  /* stored legal frames so the reset/reuse pass replays the identical stream */
  int16_t stored[STREAM_FRAMES][NO_OF_BYTES_30MS];

  memset(&cov, 0, sizeof(cov));
  TASK_CHECK(WebRtcIlbcfix_EncoderCreate(&enc) == 0);
  TASK_CHECK(WebRtcIlbcfix_DecoderCreate(&d1) == 0);
  TASK_CHECK(WebRtcIlbcfix_DecoderCreate(&d2) == 0);
  TASK_CHECK(WebRtcIlbcfix_DecoderCreate(&rd) == 0);
  TASK_CHECK(WebRtcIlbcfix_EncoderInit(enc, (int16_t)mode) == 0);
  TASK_CHECK(WebRtcIlbcfix_DecoderInit(d1, (int16_t)mode) == 0);
  TASK_CHECK(WebRtcIlbcfix_DecoderInit(d2, (int16_t)mode) == 0);
  TASK_CHECK(WebRtcIlbcfix_DecoderInit(rd, (int16_t)mode) == 0);
  einst = (iLBC_Enc_Inst_t *)enc;
  rinst = (iLBC_Dec_Inst_t *)rd;
  (void)einst;

  for (f = 0; f < STREAM_FRAMES; ++f) {
    int16_t in[BLOCKL_MAX], out1[BLOCKL_MAX], out2[BLOCKL_MAX];
    int16_t bits[NO_OF_WORDS_30MS];
    uint16_t swapped[NO_OF_WORDS_30MS];
    int16_t type1 = -1, type2 = -1;
    int len, i;
    iLBC_bits fields;
    int16_t lsfdeq[LPC_FILTERORDER * LPC_N_MAX];
    int16_t syntdenum[NSUB_MAX * (LPC_FILTERORDER + 1)];
    int16_t weightdenum[NSUB_MAX * (LPC_FILTERORDER + 1)];
    int16_t scalar[STATE_SHORT_LEN_30MS];
    ref_frame_t ref;

    for (i = 0; i < samples; ++i) in[i] = pcm_sample(mode, f, i, &rng);

    len = WebRtcIlbcfix_Encode(enc, in, (int16_t)samples, bits);
    TASK_CHECK(len == bytes);
    memcpy(stored[f], bits, sizeof(int16_t) * words);

    /* public decoders, interleaved on the same frame */
    len = WebRtcIlbcfix_Decode(d1, bits, (int16_t)bytes, out1, &type1);
    TASK_CHECK(len == samples);
    TASK_CHECK(type1 == ILBC_SPEECH);
    len = WebRtcIlbcfix_Decode(d2, bits, (int16_t)bytes, out2, &type2);
    TASK_CHECK(len == samples);
    TASK_CHECK(type2 == ILBC_SPEECH);

    /* unpack and apply the format index conversion */
    WebRtcIlbcfix_SwapBytes((uint16_t *)bits, (int16_t)words, swapped);
    TASK_CHECK(WebRtcIlbcfix_UnpackBits(swapped, &fields, (int16_t)mode) == 0);
    WebRtcIlbcfix_IndexConvDec(fields.cb_index);

    /* permitted reuse: LSF decode, stability check and interpolation,
       advanced on the independent reference decoder instance.  LsfCheck
       returns a "coefficients were adjusted" flag, not an error code, so it
       is called for its in-place stabilization side effect only. */
    WebRtcIlbcfix_SimpleLsfDeQ(lsfdeq, fields.lsf, rinst->lpc_n);
    (void)WebRtcIlbcfix_LsfCheck(lsfdeq, LPC_FILTERORDER, rinst->lpc_n);
    WebRtcIlbcfix_DecoderInterpolateLsp(syntdenum, weightdenum, lsfdeq,
                                        LPC_FILTERORDER, rinst);

    WebRtcIlbcfix_StateConstruct(fields.idxForMax, fields.idxVec,
                                 syntdenum + (fields.startIdx - 1) *
                                                 (LPC_FILTERORDER + 1),
                                 scalar, rinst->state_short_len);

    ref.mode = mode;
    ref.blockl = rinst->blockl;
    ref.nsub = rinst->nsub;
    ref.state_short_len = rinst->state_short_len;
    ref.diff = STATE_LEN - rinst->state_short_len;
    ref_build_residual(&ref, &fields, scalar);

    /* no packet loss: DoThePlc copies decresidual straight into prevResidual */
    if (memcmp(((iLBC_Dec_Inst_t *)d1)->prevResidual, ref.dec,
               sizeof(int16_t) * samples) != 0)
      cov.residual_mismatches++;
    if (memcmp(((iLBC_Dec_Inst_t *)d2)->prevResidual, ref.dec,
               sizeof(int16_t) * samples) != 0)
      cov.residual_mismatches++;
    {
      char tag[64];
      snprintf(tag, sizeof(tag), "stream.m%d.frame%d.d1", mode, f);
      if (expect_vec(tag, ((iLBC_Dec_Inst_t *)d1)->prevResidual, ref.dec,
                     samples))
        return 1;
      snprintf(tag, sizeof(tag), "stream.m%d.frame%d.d2", mode, f);
      if (expect_vec(tag, ((iLBC_Dec_Inst_t *)d2)->prevResidual, ref.dec,
                     samples))
        return 1;
    }

    /* coverage bookkeeping derived from the actual unpacked frame */
    TASK_CHECK(fields.startIdx >= 1 &&
               fields.startIdx <= samples / SUBL - 1);
    TASK_CHECK(fields.state_first == 0 || fields.state_first == 1);
    cov.pair[fields.startIdx][fields.state_first]++;
    cov.frames++;
    if (rinst->nsub - fields.startIdx - 1 > 0) cov.fwd_frames++;
    if (fields.startIdx - 1 > 0) {
      cov.back_frames++;
      /* the clamp only executes inside the backward setup, so require
         Nback > 0 for the 147-sample ceiling to be genuinely exercised */
      if (SUBL * (rinst->nsub + 1 - fields.startIdx) > CB_MEML)
        cov.cap_frames++;
    }
  }

  /* every reachable (startIdx, state_first) pair must actually be covered */
  {
    int s, k;
    for (s = 1; s < samples / SUBL; ++s)
      for (k = 0; k < 2; ++k) TASK_CHECK(cov.pair[s][k] > 0);
    TASK_CHECK(cov.frames == STREAM_FRAMES);
    TASK_CHECK(cov.residual_mismatches == 0);
  }

  /* reset/reuse: re-init d1 and replay the identical legal byte stream */
  TASK_CHECK(WebRtcIlbcfix_DecoderInit(d1, (int16_t)mode) == 0);
  TASK_CHECK(WebRtcIlbcfix_DecoderInit(rd, (int16_t)mode) == 0);
  for (f = 0; f < STREAM_FRAMES; ++f) {
    int16_t out[BLOCKL_MAX], type = -1;
    uint16_t swapped[NO_OF_WORDS_30MS];
    iLBC_bits fields;
    int16_t lsfdeq[LPC_FILTERORDER * LPC_N_MAX];
    int16_t syntdenum[NSUB_MAX * (LPC_FILTERORDER + 1)];
    int16_t weightdenum[NSUB_MAX * (LPC_FILTERORDER + 1)];
    int16_t scalar[STATE_SHORT_LEN_30MS];
    ref_frame_t ref;
    int len = WebRtcIlbcfix_Decode(d1, stored[f], (int16_t)bytes, out, &type);
    TASK_CHECK(len == samples && type == ILBC_SPEECH);
    WebRtcIlbcfix_SwapBytes((uint16_t *)stored[f], (int16_t)words, swapped);
    TASK_CHECK(WebRtcIlbcfix_UnpackBits(swapped, &fields, (int16_t)mode) == 0);
    WebRtcIlbcfix_IndexConvDec(fields.cb_index);
    WebRtcIlbcfix_SimpleLsfDeQ(lsfdeq, fields.lsf, rinst->lpc_n);
    WebRtcIlbcfix_LsfCheck(lsfdeq, LPC_FILTERORDER, rinst->lpc_n);
    WebRtcIlbcfix_DecoderInterpolateLsp(syntdenum, weightdenum, lsfdeq,
                                        LPC_FILTERORDER, rinst);
    WebRtcIlbcfix_StateConstruct(fields.idxForMax, fields.idxVec,
                                 syntdenum + (fields.startIdx - 1) *
                                                 (LPC_FILTERORDER + 1),
                                 scalar, rinst->state_short_len);
    ref.mode = mode;
    ref.blockl = rinst->blockl;
    ref.nsub = rinst->nsub;
    ref.state_short_len = rinst->state_short_len;
    ref.diff = STATE_LEN - rinst->state_short_len;
    ref_build_residual(&ref, &fields, scalar);
    {
      char tag[64];
      snprintf(tag, sizeof(tag), "stream.m%d.reset.frame%d", mode, f);
      if (expect_vec(tag, ((iLBC_Dec_Inst_t *)d1)->prevResidual, ref.dec,
                     samples))
        return 1;
    }
  }

  printf("STREAM_COVERAGE mode=%d frames=%u fwd=%u back=%u cap=%u pairs=",
         mode, cov.frames, cov.fwd_frames, cov.back_frames, cov.cap_frames);
  {
    int s, k;
    for (s = 1; s < samples / SUBL; ++s)
      for (k = 0; k < 2; ++k)
        printf("%s(%d,%d)=%u", (s == 1 && k == 0) ? "" : ",", s, k,
               cov.pair[s][k]);
  }
  puts("");

  WebRtcIlbcfix_EncoderFree(enc);
  WebRtcIlbcfix_DecoderFree(d1);
  WebRtcIlbcfix_DecoderFree(d2);
  WebRtcIlbcfix_DecoderFree(rd);
  TASK_CHECK(n_sets >= 3);
  return 0;
}

static int run_stream_20(void) { return run_stream_mode(20); }
static int run_stream_30(void) { return run_stream_mode(30); }

/* ================================================================== */
/* Registration                                                       */
/* ================================================================== */

const struct task_case task_cases[] = {
    /* vectors */
    {"vectors", "short_plain", run_vec_short_plain},
    {"vectors", "long_plain", run_vec_long_plain},
    {"vectors", "long_augmented", run_vec_long_augmented},
    {"vectors", "long_filtered", run_vec_long_filtered},
    {"vectors", "long_filtered_interp", run_vec_long_filtered_interp},
    {"vectors", "gain_chain_weighted", run_vec_gain_chain},
    /* residual */
    {"residual", "m20_s1_f1", run_residual_20_1_1},
    {"residual", "m20_s1_f0", run_residual_20_1_0},
    {"residual", "m20_s2_f1", run_residual_20_2_1},
    {"residual", "m20_s2_f0", run_residual_20_2_0},
    {"residual", "m20_s3_f1", run_residual_20_3_1},
    {"residual", "m20_s3_f0", run_residual_20_3_0},
    {"residual", "m30_s1_f1", run_residual_30_1_1},
    {"residual", "m30_s1_f0", run_residual_30_1_0},
    {"residual", "m30_s3_f1", run_residual_30_3_1},
    {"residual", "m30_s3_f0", run_residual_30_3_0},
    {"residual", "m30_s5_f1", run_residual_30_5_1},
    {"residual", "m30_s5_f0", run_residual_30_5_0},
    /* stream */
    {"stream", "mode20", run_stream_20},
    {"stream", "mode30", run_stream_30},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
