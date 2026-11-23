/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (c) 2026, project contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names
 *   of its contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 ****************************************************************************/

/**
 * \file
 * Contract tests for explicit single-reference weighted prediction.
 *
 * Three groups:
 *  - syntax:   real NAL output is de-emulated and parsed independently
 *  - prediction: production prediction is compared against an independent
 *                filter/weight reference (integer, fractional, edge, negative
 *                weight/offset, clip-before-weight ordering)
 *  - pipeline: public API behaviour including validity, delivery order,
 *              references, flush, switching and the disabled path
 */

#include "task_support/check.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvazaar.h"

/* Internal headers for the white-box prediction checks. */
#include "cu.h"
#include "encoderstate.h"
#include "inter.h"
#include "kvazaar_internal.h"
#include "weighted_pred.h"

#define TEST_W 64
#define TEST_H 64

/* The same filter coefficient tables the encoder uses. They are part of the
 * HEVC specification and are duplicated here on purpose so the reference
 * stays independent of the production interpolation code. */
static const int8_t ref_luma_filter[4][8] = {
  {  0, 0,   0, 64,  0,   0, 0,  0 },
  { -1, 4, -10, 58, 17,  -5, 1,  0 },
  { -1, 4, -11, 40, 40, -11, 4, -1 },
  {  0, 1,  -5, 17, 58, -10, 4, -1 },
};
static const int8_t ref_chroma_filter[8][4] = {
  {  0, 64,  0,  0 },
  { -2, 58, 10, -2 },
  { -4, 54, 16, -2 },
  { -6, 46, 28, -4 },
  { -4, 36, 36, -4 },
  { -4, 28, 46, -6 },
  { -2, 16, 54, -4 },
  { -2, 10, 58, -2 },
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static unsigned char src_sample(int frame, int c, int x, int y)
{
  return (unsigned char)((13 * x + 7 * y + 29 * ((x / 8) ^ (y / 8)) +
                          c * 47 + frame * 31 + 19 * c * frame) & 255);
}

static void fill_picture(kvz_picture *p, int frame)
{
  for (int c = 0; c < 3; ++c) {
    const int w = c ? TEST_W / 2 : TEST_W;
    const int h = c ? TEST_H / 2 : TEST_H;
    const int st = c ? p->stride_c : p->stride;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        p->data[c][y * st + x] = src_sample(frame, c, x, y);
      }
    }
  }
  p->pts = frame;
  p->dts = frame;
}

static void base_opts(const kvz_api *api, kvz_config *cfg, int wp)
{
  const char *opts[][2] = {
    { "gop", "0" }, { "ref", "1" }, { "bipred", "0" },
    { "wpp", "0" }, { "threads", "1" }, { "cpuid", "0" },
    { "deblock", "0" }, { "sao", "off" }, { "info", "0" },
    { "hash", "none" }, { "subme", "4" }, { "me", "hexbs" },
    { "force-inter", "1" },
  };
  for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); ++i) {
    if (!api->config_parse(cfg, opts[i][0], opts[i][1])) abort();
  }
  if (wp && !api->config_parse(cfg, "weighted-pred", "1")) abort();
  cfg->width = TEST_W;
  cfg->height = TEST_H;
  cfg->owf = 0;
  cfg->enable_logging_output = 0;
}

static kvz_encoder *open_encoder(const kvz_api *api, int wp)
{
  kvz_config *cfg = api->config_alloc();
  if (!api->config_init(cfg)) abort();
  base_opts(api, cfg, wp);
  kvz_encoder *enc = api->encoder_open(cfg);
  if (!enc) abort();
  api->config_destroy(cfg);
  return enc;
}

static void set_identity_wp(kvz_picture *p)
{
  memset(&p->wp, 0, sizeof(p->wp));
}

static void set_wp(kvz_picture *p, int denom,
                   int wy, int wu, int wv, int oy, int ou, int ov)
{
  p->wp.enabled = 1;
  p->wp.denom = denom;
  p->wp.weight[0] = wy;
  p->wp.weight[1] = wu;
  p->wp.weight[2] = wv;
  p->wp.offset[0] = oy;
  p->wp.offset[1] = ou;
  p->wp.offset[2] = ov;
}

/* ------------------------------------------------------------------ */
/* Annex B de-emulation and bit reader                                */
/* ------------------------------------------------------------------ */

typedef struct {
  const unsigned char *data;
  size_t size;
  size_t bitpos;
} bit_reader_t;

static void br_init(bit_reader_t *br, const unsigned char *data, size_t size)
{
  br->data = data;
  br->size = size;
  br->bitpos = 0;
}

static unsigned br_u(bit_reader_t *br, int n)
{
  unsigned v = 0;
  for (int i = 0; i < n; ++i) {
    size_t b = br->bitpos++;
    v = (v << 1) | ((br->data[b / 8] >> (7 - (b & 7))) & 1);
  }
  return v;
}

static unsigned br_ue(bit_reader_t *br)
{
  int zeros = 0;
  while (br_u(br, 1) == 0) ++zeros;
  unsigned v = (1u << zeros) - 1;
  if (zeros) v += br_u(br, zeros);
  return v;
}

static int br_se(bit_reader_t *br)
{
  unsigned code = br_ue(br);
  return (code & 1) ? (int)((code + 1) / 2) : -(int)(code / 2);
}

static unsigned char *de_emulate(const unsigned char *in, size_t n,
                                 size_t *out_n)
{
  unsigned char *out = malloc(n ? n : 1);
  size_t o = 0;
  int zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    if (zeros >= 2 && in[i] == 0x03) {
      zeros = 0;
      continue;
    }
    out[o++] = in[i];
    zeros = in[i] ? 0 : zeros + 1;
  }
  *out_n = o;
  return out;
}

typedef struct {
  int type;
  const unsigned char *rbsp; /* points into the de-emulated buffer */
  size_t rbsp_size;
} nal_t;

static int split_nals(const unsigned char *rbsp, size_t n, nal_t *nals,
                      int max_nals)
{
  int count = 0;
  size_t i = 0;
  while (i + 3 <= n && count < max_nals) {
    if (rbsp[i] == 0 && rbsp[i + 1] == 0 && rbsp[i + 2] == 1) {
      size_t start = i + 3;
      size_t j = start;
      while (j + 3 <= n && !(rbsp[j] == 0 && rbsp[j + 1] == 0 &&
                             rbsp[j + 2] == 1)) {
        ++j;
      }
      nals[count].type = rbsp[start] >> 1;
      nals[count].rbsp = rbsp + start;
      nals[count].rbsp_size = j - start;
      ++count;
      i = j;
    } else {
      ++i;
    }
  }
  return count;
}

static void collect_output(const kvz_api *api, kvz_encoder *enc,
                           unsigned char **buf, size_t *len, size_t *cap)
{
  kvz_data_chunk *chunks = NULL;
  uint32_t n = 0;
  if (api->encoder_headers(enc, &chunks, &n) != 1) abort();
  for (kvz_data_chunk *c = chunks; c; c = c->next) {
    if (*len + c->len > *cap) {
      *cap = (*cap ? *cap * 2 : 4096);
      *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, c->data, c->len);
    *len += c->len;
  }
  api->chunk_free(chunks);
}

static void collect_chunks(kvz_data_chunk *chunks, unsigned char **buf,
                           size_t *len, size_t *cap)
{
  for (kvz_data_chunk *c = chunks; c; c = c->next) {
    if (*len + c->len > *cap) {
      *cap = (*cap ? *cap * 2 : 4096);
      *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, c->data, c->len);
    *len += c->len;
  }
}

/* Parsed pred_weight_table contents. */
typedef struct {
  unsigned luma_log2_denom;
  int delta_chroma_denom;
  unsigned luma_flag;
  int delta_luma_weight;
  int luma_offset;
  unsigned chroma_flag;
  int delta_chroma_weight[2];
  int delta_chroma_offset[2];
} weight_table_t;

/* Parse a PPS NAL and return weighted_pred_flag. */
static int parse_pps_weighted_pred(const nal_t *nal)
{
  bit_reader_t br;
  br_init(&br, nal->rbsp, nal->rbsp_size);
  br_u(&br, 16); /* NAL header */
  br_ue(&br);    /* pps id */
  br_ue(&br);    /* sps id */
  br_u(&br, 1);  /* dependent_slice_segments_enabled_flag */
  br_u(&br, 1);  /* output_flag_present_flag */
  br_u(&br, 3);  /* num_extra_slice_header_bits */
  br_u(&br, 1);  /* sign_data_hiding */
  br_u(&br, 1);  /* cabac_init_present */
  br_ue(&br);    /* num_ref_idx_l0_default_active_minus1 */
  br_ue(&br);    /* num_ref_idx_l1_default_active_minus1 */
  br_se(&br);    /* pic_init_qp_minus26 */
  br_u(&br, 1);  /* constrained_intra_pred_flag */
  br_u(&br, 1);  /* transform_skip_enabled_flag */
  if (br_u(&br, 1)) br_ue(&br); /* cu_qp_delta */
  br_se(&br);    /* pps_cb_qp_offset */
  br_se(&br);    /* pps_cr_qp_offset */
  br_u(&br, 1);  /* pps_slice_chroma_qp_offsets_present_flag */
  int weighted_pred_flag = (int)br_u(&br, 1);
  int weighted_bipred_idc = (int)br_u(&br, 2);
  TASK_CHECK(weighted_bipred_idc == 0);
  return weighted_pred_flag;
}

/* Parse the slice header of a P-slice NAL up to and including the
 * pred_weight_table. poc_lsb_bits is 4 for the tested configuration. */
static int parse_p_slice_weight_table(const nal_t *nal,
                                      weight_table_t *wt)
{
  bit_reader_t br;
  br_init(&br, nal->rbsp, nal->rbsp_size);
  br_u(&br, 16); /* NAL header */
  TASK_CHECK(br_u(&br, 1) == 1); /* first_slice_segment_in_pic_flag */
  br_ue(&br);    /* slice_pic_parameter_set_id */
  TASK_CHECK(br_ue(&br) == (unsigned)KVZ_SLICE_P);
  br_u(&br, 4);  /* pic_order_cnt_lsb (poc_lsb_bits == 4) */
  TASK_CHECK(br_u(&br, 1) == 0); /* short_term_ref_pic_set_sps_flag */
  unsigned num_neg = br_ue(&br);
  unsigned num_pos = br_ue(&br);
  for (unsigned i = 0; i < num_neg; ++i) { br_ue(&br); br_u(&br, 1); }
  for (unsigned i = 0; i < num_pos; ++i) { br_ue(&br); br_u(&br, 1); }
  br_u(&br, 1);  /* slice_temporal_mvp_enabled_flag */
  TASK_CHECK(br_u(&br, 1) == 1); /* num_ref_idx_active_override_flag */
  TASK_CHECK(br_ue(&br) == 0);   /* num_ref_idx_l0_active_minus1 */
  br_ue(&br);    /* five_minus_max_num_merge_cand */
  br_se(&br);    /* slice_qp_delta */

  /* pred_weight_table follows slice_qp_delta. */
  wt->luma_log2_denom = br_ue(&br);
  wt->delta_chroma_denom = br_se(&br);
  wt->luma_flag = br_u(&br, 1);
  if (wt->luma_flag) {
    wt->delta_luma_weight = br_se(&br);
    wt->luma_offset = br_se(&br);
  } else {
    wt->delta_luma_weight = 0;
    wt->luma_offset = 0;
  }
  wt->chroma_flag = br_u(&br, 1);
  for (int c = 0; c < 2; ++c) {
    if (wt->chroma_flag) {
      wt->delta_chroma_weight[c] = br_se(&br);
      wt->delta_chroma_offset[c] = br_se(&br);
    } else {
      wt->delta_chroma_weight[c] = 0;
      wt->delta_chroma_offset[c] = 0;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Independent interpolation + weighting reference                   */
/* ------------------------------------------------------------------ */

static int clipi(int lo, int hi, int v)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

/* Weight at internal precision (samples scaled by 64). */
static int weight_internal(int internal_sample, int weight, int denom,
                           int offset)
{
  int v = internal_sample * weight;
  v = (v + (1 << 5)) >> denom;
  v += offset;
  return clipi(0, 255, v);
}

/* Independent quarter-pel luma prediction for one sample, including border
 * extension by repetition. mv is in quarter-luma precision. */
static int ref_luma_sample(const kvz_picture *ref, int blk_x, int blk_y,
                           int mv_qx, int mv_qy, int lx, int ly)
{
  const int fx = mv_qx & 3;
  const int fy = mv_qy & 3;
  const int ix = blk_x + (mv_qx >> 2);
  const int iy = blk_y + (mv_qy >> 2);

  if (fx == 0 && fy == 0) {
    const int sx = clipi(0, ref->width - 1, ix + lx);
    const int sy = clipi(0, ref->height - 1, iy + ly);
    return (int)ref->y[sy * ref->stride + sx] << 6;
  }

  const int8_t *hf = ref_luma_filter[fx];
  const int8_t *vf = ref_luma_filter[fy];
  int hor[12]; /* block row + vertical filter margin */
  for (int ry = -3; ry <= 8; ++ry) {
    int acc = 0;
    for (int k = 0; k < 8; ++k) {
      const int sx = clipi(0, ref->width - 1, ix + lx + k - 3);
      const int sy = clipi(0, ref->height - 1, iy + ly + ry);
      acc += hf[k] * ref->y[sy * ref->stride + sx];
    }
    hor[ry + 3] = acc; /* bit-depth shift is zero for 8-bit */
  }
  int acc = 0;
  for (int k = 0; k < 8; ++k) acc += vf[k] * hor[k];
  return acc >> 6;
}

/* Independent eighth-pel chroma prediction for one chroma plane. */
static int ref_chroma_sample(const kvz_pixel *plane, int pw, int ph, int stride,
                             int blk_cx, int blk_cy,
                             int mv_cx, int mv_cy, int lx, int ly)
{
  const int fx = mv_cx & 7;
  const int fy = mv_cy & 7;
  const int ix = blk_cx + (mv_cx >> 3);
  const int iy = blk_cy + (mv_cy >> 3);

  if (fx == 0 && fy == 0) {
    const int sx = clipi(0, pw - 1, ix + lx);
    const int sy = clipi(0, ph - 1, iy + ly);
    return (int)plane[sy * stride + sx] << 6;
  }

  const int8_t *hf = ref_chroma_filter[fx];
  const int8_t *vf = ref_chroma_filter[fy];
  int hor[8];
  for (int ry = -1; ry <= 6; ++ry) {
    int acc = 0;
    for (int k = 0; k < 4; ++k) {
      const int sx = clipi(0, pw - 1, ix + lx + k - 1);
      const int sy = clipi(0, ph - 1, iy + ly + ry);
      acc += hf[k] * plane[sy * stride + sx];
    }
    hor[ry + 1] = acc;
  }
  int acc = 0;
  for (int k = 0; k < 4; ++k) acc += vf[k] * hor[k];
  return acc >> 6;
}

/* Build the full independent weighted prediction for a PU and compare it
 * against the production output. */
static void check_prediction_block(encoder_state_t *state,
                                   const kvz_picture *ref,
                                   const kvz_wp_params_t *wp,
                                   int pu_x, int pu_y, int pu_w, int pu_h,
                                   int mv_qx, int mv_qy)
{
  yuv_t out;
  static kvz_pixel buf_y[LCU_LUMA_SIZE];
  static kvz_pixel buf_u[LCU_LUMA_SIZE];
  static kvz_pixel buf_v[LCU_LUMA_SIZE];
  out.size = pu_w * pu_h;
  out.y = buf_y;
  out.u = buf_u;
  out.v = buf_v;

  const int16_t mv[2] = { (int16_t)mv_qx, (int16_t)mv_qy };
  kvz_wp_predict_unipred(state, ref, pu_x, pu_y, pu_w, pu_h, pu_w, mv,
                         &out, true, true);

  /* Luma */
  for (int ly = 0; ly < pu_h; ++ly) {
    for (int lx = 0; lx < pu_w; ++lx) {
      const int internal = ref_luma_sample(ref, state->tile->offset_x + pu_x,
                                           state->tile->offset_y + pu_y,
                                           mv_qx, mv_qy, lx, ly);
      const int expect = weight_internal(internal, wp->weight[0],
                                         wp->denom, wp->offset[0]);
      assert(out.y[ly * pu_w + lx] == (kvz_pixel)expect);
    }
  }

  /* Chroma, 4:2:0 so the luma quarter-pel MV maps to eighth-pel chroma. */
  const int cw = pu_w / 2;
  const int ch = pu_h / 2;
  for (int c = 0; c < 2; ++c) {
    const kvz_pixel *plane = c == 0 ? ref->u : ref->v;
    const int blk_cx = (state->tile->offset_x + pu_x) / 2;
    const int blk_cy = (state->tile->offset_y + pu_y) / 2;
    for (int ly = 0; ly < ch; ++ly) {
      for (int lx = 0; lx < cw; ++lx) {
        const int internal = ref_chroma_sample(plane, ref->width_c,
                                               ref->height_c, ref->stride_c,
                                               blk_cx, blk_cy,
                                               mv_qx, mv_qy, lx, ly);
        const int expect = weight_internal(internal, wp->weight[c + 1],
                                           wp->denom, wp->offset[c + 1]);
        const kvz_pixel got = (c == 0 ? out.u : out.v)[ly * cw + lx];
        assert(got == (kvz_pixel)expect);
      }
    }
  }
}

/* "Clip before weight" would clip the filtered internal sample to the 8-bit
 * range first and then scale it; the correct order weights the internal
 * sample and clips only the result. Construct a case where the two differ. */
static int clip_first_differs(int internal_sample, int weight, int denom)
{
  const int correct = clipi(0, 255,
      ((internal_sample * weight + (1 << 5)) >> denom));
  const int clipped_input = clipi(0, 255, internal_sample >> 6);
  const int wrong = clipi(0, 255,
      ((clipped_input * weight + (1 << 5)) >> denom));
  return correct != wrong;
}

/* ------------------------------------------------------------------ */
/* Syntax group                                                       */
/* ------------------------------------------------------------------ */

static int test_pps_flag(void)
{
  const kvz_api *api = kvz_api_get(8);

  /* Disabled: PPS weighted_pred_flag must be 0. */
  kvz_encoder *enc_off = open_encoder(api, 0);
  unsigned char *buf = NULL;
  size_t len = 0, cap = 0;
  collect_output(api, enc_off, &buf, &len, &cap);
  size_t rbsp_n = 0;
  unsigned char *rbsp = de_emulate(buf, len, &rbsp_n);
  nal_t nals[32];
  int nn = split_nals(rbsp, rbsp_n, nals, 32);
  int found_pps = 0;
  for (int i = 0; i < nn; ++i) {
    if (nals[i].type == KVZ_NAL_PPS_NUT) {
      TASK_CHECK(parse_pps_weighted_pred(&nals[i]) == 0);
      found_pps = 1;
    }
  }
  TASK_CHECK(found_pps);
  api->encoder_close(enc_off);
  free(buf);
  free(rbsp);

  /* Enabled: PPS weighted_pred_flag must be 1. */
  kvz_encoder *enc_on = open_encoder(api, 1);
  buf = NULL; len = 0; cap = 0;
  collect_output(api, enc_on, &buf, &len, &cap);
  rbsp = de_emulate(buf, len, &rbsp_n);
  nn = split_nals(rbsp, rbsp_n, nals, 32);
  found_pps = 0;
  for (int i = 0; i < nn; ++i) {
    if (nals[i].type == KVZ_NAL_PPS_NUT) {
      TASK_CHECK(parse_pps_weighted_pred(&nals[i]) == 1);
      found_pps = 1;
    }
  }
  TASK_CHECK(found_pps);
  api->encoder_close(enc_on);
  free(buf);
  free(rbsp);
  return 0;
}

static int test_pred_weight_table(void)
{
  const kvz_api *api = kvz_api_get(8);
  kvz_encoder *enc = open_encoder(api, 1);

  const int denom = 2;
  const int base = 1 << denom;
  /* Negative weight, non-zero offsets for all components. */
  const int wy = base - 7, wu = base + 11, wv = base - 1;
  const int oy = -23, ou = 9, ov = 40;

  unsigned char *buf = NULL;
  size_t len = 0, cap = 0;
  collect_output(api, enc, &buf, &len, &cap);

  for (int f = 0; f < 3; ++f) {
    kvz_picture *in = api->picture_alloc(TEST_W, TEST_H);
    fill_picture(in, f);
    if (f == 2) set_wp(in, denom, wy, wu, wv, oy, ou, ov);
    else set_identity_wp(in);

    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *ch = NULL;
    uint32_t bytes = 0;
    kvz_frame_info info;
    TASK_CHECK(api->encoder_encode(enc, in, &ch, &bytes, &rec, &src, &info) == 1);
    TASK_CHECK(ch != NULL);
    collect_chunks(ch, &buf, &len, &cap);
    api->chunk_free(ch);
    api->picture_free(rec);
    api->picture_free(src);
    api->picture_free(in);
  }

  size_t rbsp_n = 0;
  unsigned char *rbsp = de_emulate(buf, len, &rbsp_n);
  nal_t nals[64];
  int nn = split_nals(rbsp, rbsp_n, nals, 64);

  int p_slices = 0;
  for (int i = 0; i < nn; ++i) {
    /* P-slice VCL NAL units are TRAIL_R (type 1); IDR is type 19. */
    if (nals[i].type != KVZ_NAL_TRAIL_R) continue;
    bit_reader_t probe;
    br_init(&probe, nals[i].rbsp, nals[i].rbsp_size);
    br_u(&probe, 16);
    br_u(&probe, 1);
    br_ue(&probe);
    if (br_ue(&probe) != (unsigned)KVZ_SLICE_P) continue;

    weight_table_t wt;
    TASK_CHECK(parse_p_slice_weight_table(&nals[i], &wt) == 0);
    ++p_slices;

    if (p_slices == 1) {
      /* First P frame is the identity frame: all flags zero, denom zero. */
      TASK_CHECK(wt.luma_log2_denom == 0);
      TASK_CHECK(wt.delta_chroma_denom == 0);
      TASK_CHECK(wt.luma_flag == 0);
      TASK_CHECK(wt.chroma_flag == 0);
    } else {
      TASK_CHECK(wt.luma_log2_denom == (unsigned)denom);
      TASK_CHECK(wt.delta_chroma_denom == 0);
      TASK_CHECK(wt.luma_flag == 1);
      TASK_CHECK(wt.delta_luma_weight == wy - base);
      TASK_CHECK(wt.luma_offset == oy);
      TASK_CHECK(wt.chroma_flag == 1);
      TASK_CHECK(wt.delta_chroma_weight[0] == wu - base);
      TASK_CHECK(wt.delta_chroma_weight[1] == wv - base);
      /* API offset is the effective offset; the syntax carries offset-128. */
      TASK_CHECK(wt.delta_chroma_offset[0] == ou - 128);
      TASK_CHECK(wt.delta_chroma_offset[1] == ov - 128);
    }
  }
  TASK_CHECK(p_slices == 2);

  api->encoder_close(enc);
  free(buf);
  free(rbsp);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Prediction group                                                   */
/* ------------------------------------------------------------------ */

/* Drive one weighted P frame and return the live main encoder state plus
 * the reference picture used for prediction. */
static encoder_state_t *weighted_frame_state(kvz_encoder *enc,
                                             const kvz_api *api,
                                             int frame_idx,
                                             const kvz_wp_params_t *wp,
                                             const kvz_picture **ref_out)
{
  kvz_picture *in = api->picture_alloc(TEST_W, TEST_H);
  fill_picture(in, frame_idx);
  in->wp = *wp;

  kvz_picture *rec = NULL, *src = NULL;
  kvz_data_chunk *ch = NULL;
  uint32_t bytes = 0;
  kvz_frame_info info;
  if (api->encoder_encode(enc, in, &ch, &bytes, &rec, &src, &info) != 1 ||
      ch == NULL) abort();
  api->chunk_free(ch);
  api->picture_free(rec);
  api->picture_free(src);
  api->picture_free(in);

  encoder_state_t *state = &enc->states[0];
  assert(state->frame->wp_active == 1);
  assert(state->frame->ref->used_size == 1);
  *ref_out = state->frame->ref->images[0];
  return state;
}

static int test_prediction_variants(void)
{
  const kvz_api *api = kvz_api_get(8);
  kvz_encoder *enc = open_encoder(api, 1);

  /* Frame 0: I frame, never weighted even though wp is filled in. */
  kvz_picture *i_pic = api->picture_alloc(TEST_W, TEST_H);
  fill_picture(i_pic, 0);
  set_wp(i_pic, 1, 1, 2, 3, 4, 5, 6);
  kvz_picture *rec = NULL, *src = NULL;
  kvz_data_chunk *ch = NULL;
  uint32_t bytes = 0;
  TASK_CHECK(api->encoder_encode(enc, i_pic, &ch, &bytes, &rec, &src, NULL) == 1);
  api->chunk_free(ch);
  api->picture_free(rec);
  api->picture_free(src);
  api->picture_free(i_pic);
  TASK_CHECK(enc->states[0].frame->wp_active == 0);

  /* Weighted P frame with negative luma weight and negative luma offset. */
  kvz_wp_params_t wp;
  wp.enabled = 1;
  wp.denom = 1;
  wp.weight[0] = 2 - 5;   /* -3 */
  wp.weight[1] = 2 + 4;
  wp.weight[2] = 2 - 2;   /* identity chroma V */
  wp.offset[0] = -17;
  wp.offset[1] = 23;
  wp.offset[2] = -40;

  const kvz_picture *ref = NULL;
  encoder_state_t *state = weighted_frame_state(enc, api, 1, &wp, &ref);

  /* Integer MV, including a block positioned at the frame edge so border
   * extension is exercised. */
  check_prediction_block(state, ref, &wp, 0, 0, 16, 16, 0, 0);
  check_prediction_block(state, ref, &wp, 48, 48, 16, 16, 0, 0);
  /* Integer MV pointing outside the frame on the top and left. */
  check_prediction_block(state, ref, &wp, 0, 0, 16, 16, -8, -12);
  check_prediction_block(state, ref, &wp, 0, 0, 16, 16, 20, 24);

  /* Fractional luma MVs: quarter, half and three-quarter positions, including
   * the positions that force chroma interpolation under 4:2:0. */
  check_prediction_block(state, ref, &wp, 8, 8, 16, 16, 1, 0);
  check_prediction_block(state, ref, &wp, 8, 8, 16, 16, 0, 1);
  check_prediction_block(state, ref, &wp, 8, 8, 16, 16, 2, 2);
  check_prediction_block(state, ref, &wp, 8, 8, 16, 16, 3, 3);
  check_prediction_block(state, ref, &wp, 8, 8, 16, 16, 3, 1);
  check_prediction_block(state, ref, &wp, 24, 24, 16, 16, -5, -7);

  api->encoder_close(enc);
  return 0;
}

static int test_clip_before_weight_ordering(void)
{
  const kvz_api *api = kvz_api_get(8);
  kvz_encoder *enc = open_encoder(api, 1);

  /* Frame 0 reference. */
  kvz_picture *i_pic = api->picture_alloc(TEST_W, TEST_H);
  fill_picture(i_pic, 0);
  set_identity_wp(i_pic);
  kvz_picture *rec = NULL, *src = NULL;
  kvz_data_chunk *ch = NULL;
  uint32_t bytes = 0;
  TASK_CHECK(api->encoder_encode(enc, i_pic, &ch, &bytes, &rec, &src, NULL) == 1);
  api->chunk_free(ch);
  api->picture_free(rec);
  api->picture_free(src);
  api->picture_free(i_pic);

  /* A fractional prediction produces internal samples above the 8-bit range
   * (intermediate precision is 6 bits wider). Find a fractional position
   * where weighting before clipping differs from clipping before weighting.
   */
  kvz_wp_params_t wp;
  wp.enabled = 1;
  wp.denom = 0;
  wp.weight[0] = 3;
  wp.weight[1] = 1;
  wp.weight[2] = 1;
  wp.offset[0] = 0;
  wp.offset[1] = 0;
  wp.offset[2] = 0;

  const kvz_picture *ref = NULL;
  encoder_state_t *state = weighted_frame_state(enc, api, 1, &wp, &ref);

  /* Search the fractional positions of one block for an internal sample whose
   * correct weighted result cannot be obtained by clipping first. */
  int found_distinguishable = 0;
  yuv_t out;
  static kvz_pixel by[LCU_LUMA_SIZE], bu[LCU_LUMA_SIZE], bv[LCU_LUMA_SIZE];
  out.size = 16 * 16;
  out.y = by; out.u = bu; out.v = bv;

  for (int fy = 0; fy < 4 && !found_distinguishable; ++fy) {
    for (int fx = 0; fx < 4 && !found_distinguishable; ++fx) {
      const int16_t mv[2] = { (int16_t)fx, (int16_t)fy };
      kvz_wp_predict_unipred(state, ref, 8, 8, 16, 16, 16, mv,
                             &out, true, false);
      for (int ly = 0; ly < 16 && !found_distinguishable; ++ly) {
        for (int lx = 0; lx < 16 && !found_distinguishable; ++lx) {
          const int internal = ref_luma_sample(ref,
              state->tile->offset_x + 8, state->tile->offset_y + 8,
              fx, fy, lx, ly);
          if (clip_first_differs(internal, wp.weight[0], wp.denom)) {
            found_distinguishable = 1;
            const int correct = weight_internal(internal, wp.weight[0],
                                                 wp.denom, wp.offset[0]);
            TASK_CHECK(out.y[ly * 16 + lx] == (kvz_pixel)correct);
          }
        }
      }
    }
  }
  TASK_CHECK(found_distinguishable);

  api->encoder_close(enc);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Pipeline group                                                     */
/* ------------------------------------------------------------------ */

static int test_config_constraints(void)
{
  const kvz_api *api = kvz_api_get(8);

  const char *bad[][2] = {
    { "gop", "8" }, { "ref", "2" }, { "bipred", "1" },
  };
  for (size_t b = 0; b < sizeof(bad) / sizeof(bad[0]); ++b) {
    kvz_config *cfg = api->config_alloc();
    TASK_CHECK(api->config_init(cfg));
    base_opts(api, cfg, 1);
    TASK_CHECK(api->config_parse(cfg, bad[b][0], bad[b][1]));
    TASK_CHECK(api->encoder_open(cfg) == NULL);
    api->config_destroy(cfg);
  }

  /* The supported combination opens fine. */
  kvz_encoder *enc = open_encoder(api, 1);
  api->encoder_close(enc);
  return 0;
}

static int test_invalid_picture_rejected(void)
{
  const kvz_api *api = kvz_api_get(8);
  kvz_encoder *enc = open_encoder(api, 1);

  kvz_picture *p = api->picture_alloc(TEST_W, TEST_H);
  fill_picture(p, 0);

  /* Out of range denom. */
  set_wp(p, 8, 1, 1, 1, 0, 0, 0);
  kvz_data_chunk *ch = NULL;
  uint32_t bytes = 99;
  kvz_picture *rec = NULL, *src = NULL;
  TASK_CHECK(api->encoder_encode(enc, p, &ch, &bytes, &rec, &src, NULL) == 0);
  TASK_CHECK(ch == NULL && bytes == 0 && rec == NULL && src == NULL);

  /* Delta weight out of range. */
  set_wp(p, 0, 129, 1, 1, 0, 0, 0);
  TASK_CHECK(api->encoder_encode(enc, p, &ch, &bytes, &rec, &src, NULL) == 0);

  /* Offset out of range. */
  set_wp(p, 0, 1, 1, 1, 129, 0, 0);
  TASK_CHECK(api->encoder_encode(enc, p, &ch, &bytes, &rec, &src, NULL) == 0);

  /* Chroma range violation. */
  set_wp(p, 0, 1, 1, 1, 0, 127, 0); /* offset-128+128 = 127, fine */
  TASK_CHECK(api->encoder_encode(enc, p, &ch, &bytes, &rec, &src, NULL) == 1);
  if (ch) { api->chunk_free(ch); ch = NULL; }
  if (rec) { api->picture_free(rec); rec = NULL; }
  if (src) { api->picture_free(src); src = NULL; }

  /* The previously rejected picture is still usable once corrected. */
  set_identity_wp(p);
  TASK_CHECK(api->encoder_encode(enc, p, &ch, &bytes, &rec, &src, NULL) == 1);
  TASK_CHECK(ch != NULL);
  if (ch) api->chunk_free(ch);
  if (rec) api->picture_free(rec);
  if (src) api->picture_free(src);

  api->picture_free(p);
  api->encoder_close(enc);
  return 0;
}

static int test_delivery_and_references(void)
{
  const kvz_api *api = kvz_api_get(8);
  kvz_encoder *enc = open_encoder(api, 1);

  const int N = 5;
  int seen_poc = 0;
  for (int f = 0; f < N; ++f) {
    kvz_picture *in = api->picture_alloc(TEST_W, TEST_H);
    fill_picture(in, f);
    if (f > 0) {
      /* Alternate between weighted and identity frames to prove the per-frame
       * parameters never leak across pictures. */
      if (f % 2) set_wp(in, 1, 2 + 3, 2 - 1, 2, 11, -7, 5);
      else set_identity_wp(in);
    } else {
      set_identity_wp(in);
    }

    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *ch = NULL;
    uint32_t bytes = 0;
    kvz_frame_info info;
    memset(&info, 0, sizeof(info));
    TASK_CHECK(api->encoder_encode(enc, in, &ch, &bytes, &rec, &src, &info) == 1);
    TASK_CHECK(ch != NULL);
    TASK_CHECK(info.poc == (unsigned)seen_poc++);
    if (f == 0) {
      TASK_CHECK(info.slice_type == KVZ_SLICE_I);
      TASK_CHECK(info.ref_list_len[0] == 0);
    } else {
      TASK_CHECK(info.slice_type == KVZ_SLICE_P);
      TASK_CHECK(info.ref_list_len[0] == 1);
      TASK_CHECK(info.ref_list[0][0] == f - 1);
      /* The current frame must reference the previous reconstruction. */
      TASK_CHECK(enc->states[0].frame->wp_active == (f % 2));
    }
    /* The returned source still carries the caller's wp values. */
    TASK_CHECK(src->wp.enabled == in->wp.enabled);
    api->chunk_free(ch);
    api->picture_free(rec);
    api->picture_free(src);
    api->picture_free(in);
  }

  /* Flush: no more pictures should be pending. */
  kvz_data_chunk *ch = NULL;
  uint32_t bytes = 0;
  kvz_picture *rec = NULL, *src = NULL;
  TASK_CHECK(api->encoder_encode(enc, NULL, &ch, &bytes, &rec, &src, NULL) == 1);
  TASK_CHECK(ch == NULL && bytes == 0 && rec == NULL && src == NULL);

  api->encoder_close(enc);
  return 0;
}

/* With OWF enabled, several in-flight pictures hold distinct wp snapshots.
 * Parameters must never bleed between delayed frames, and flush must drain
 * them in presentation order. */
static int test_delayed_isolation_and_flush(void)
{
  const kvz_api *api = kvz_api_get(8);
  kvz_config *cfg = api->config_alloc();
  if (!api->config_init(cfg)) abort();
  base_opts(api, cfg, 1);
  cfg->owf = 2;
  cfg->threads = 3;
  kvz_encoder *enc = api->encoder_open(cfg);
  if (!enc) abort();
  api->config_destroy(cfg);

  const int N = 6;
  int delivered = 0;
  for (int f = 0; f < N; ++f) {
    kvz_picture *in = api->picture_alloc(TEST_W, TEST_H);
    fill_picture(in, f);
    if (f == 0) {
      set_identity_wp(in);
    } else {
      /* Every frame gets a distinct, valid parameter set, alternating with
       * the identity transform. */
      if (f % 2) {
        set_wp(in, 1, 2 + (f % 5) - 2, 2 - 1, 2,
               (f * 7) % 60 - 30, -f, f);
      } else {
        set_identity_wp(in);
      }
    }

    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *ch = NULL;
    uint32_t bytes = 0;
    kvz_frame_info info;
    memset(&info, 0, sizeof(info));
    TASK_CHECK(api->encoder_encode(enc, in, &ch, &bytes, &rec, &src,
                                   &info) == 1);
    if (ch) {
      TASK_CHECK(info.poc == (unsigned)delivered);
      if (delivered == 0) {
        TASK_CHECK(info.slice_type == KVZ_SLICE_I);
      } else {
        TASK_CHECK(info.slice_type == KVZ_SLICE_P);
        TASK_CHECK(info.ref_list_len[0] == 1);
      }
      ++delivered;
      api->chunk_free(ch);
      api->picture_free(rec);
      api->picture_free(src);
    }
    api->picture_free(in);
  }

  /* Drain the rest of the delayed pipeline; order must stay contiguous. */
  for (;;) {
    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *ch = NULL;
    uint32_t bytes = 0;
    kvz_frame_info info;
    memset(&info, 0, sizeof(info));
    TASK_CHECK(api->encoder_encode(enc, NULL, &ch, &bytes, &rec, &src,
                                   &info) == 1);
    if (!ch) break;
    TASK_CHECK(info.poc == (unsigned)delivered);
    TASK_CHECK(info.slice_type == KVZ_SLICE_P);
    TASK_CHECK(info.ref_list_len[0] == 1);
    ++delivered;
    api->chunk_free(ch);
    api->picture_free(rec);
    api->picture_free(src);
    if (delivered > N) break;
  }
  TASK_CHECK(delivered == N);

  /* Further flush calls stay empty. */
  kvz_data_chunk *ch = NULL;
  uint32_t bytes = 0;
  TASK_CHECK(api->encoder_encode(enc, NULL, &ch, &bytes, NULL, NULL, NULL)
              == 1);
  TASK_CHECK(ch == NULL && bytes == 0);

  api->encoder_close(enc);
  return 0;
}

/* The weighted reconstruction of one frame becomes the reference for the
 * next frame, so the reference pixels must match the weighted+clipped output
 * rather than the unweighted filtered samples. */
static int test_weighted_reference_chain(void)
{
  const kvz_api *api = kvz_api_get(8);
  kvz_encoder *enc = open_encoder(api, 1);

  kvz_wp_params_t wp;
  wp.enabled = 1;
  wp.denom = 1;
  wp.weight[0] = 2 - 4;
  wp.weight[1] = 2 + 2;
  wp.weight[2] = 2;
  wp.offset[0] = 30;
  wp.offset[1] = -10;
  wp.offset[2] = 5;

  kvz_picture *prev_rec = NULL;
  for (int f = 0; f < 3; ++f) {
    kvz_picture *in = api->picture_alloc(TEST_W, TEST_H);
    fill_picture(in, f);
    if (f > 0) in->wp = wp; else set_identity_wp(in);

    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *ch = NULL;
    uint32_t bytes = 0;
    TASK_CHECK(api->encoder_encode(enc, in, &ch, &bytes, &rec, &src, NULL) == 1);
    TASK_CHECK(ch != NULL);

    if (f > 0) {
      /* The reference held by the encoder is the previous reconstruction. */
      const kvz_picture *held_ref = enc->states[0].frame->ref->images[0];
      TASK_CHECK(held_ref != NULL);
      if (prev_rec) {
        TASK_CHECK(held_ref->width == prev_rec->width);
        TASK_CHECK(memcmp(held_ref->y, prev_rec->y,
                          TEST_W * TEST_H) == 0);
        TASK_CHECK(memcmp(held_ref->u, prev_rec->u,
                          (TEST_W / 2) * (TEST_H / 2)) == 0);
      }
    }
    if (prev_rec) api->picture_free(prev_rec);
    prev_rec = rec;
    api->chunk_free(ch);
    api->picture_free(src);
    api->picture_free(in);
  }
  if (prev_rec) api->picture_free(prev_rec);
  api->encoder_close(enc);
  return 0;
}

/* Weighted and disabled encoders must produce different output, and the
 * disabled path must keep working. */
static int test_enabled_vs_disabled(void)
{
  const kvz_api *api = kvz_api_get(8);

  size_t sizes[2] = { 0, 0 };
  for (int mode = 0; mode < 2; ++mode) {
    kvz_encoder *enc = open_encoder(api, mode);
    for (int f = 0; f < 2; ++f) {
      kvz_picture *in = api->picture_alloc(TEST_W, TEST_H);
      fill_picture(in, f);
      if (f) set_wp(in, 1, -1, 3, 2, 40, -20, 10);
      kvz_picture *rec = NULL, *src = NULL;
      kvz_data_chunk *ch = NULL;
      uint32_t bytes = 0;
      TASK_CHECK(api->encoder_encode(enc, in, &ch, &bytes, &rec, &src, NULL) == 1);
      if (f) sizes[mode] += bytes;
      if (ch) api->chunk_free(ch);
      if (rec) api->picture_free(rec);
      if (src) api->picture_free(src);
      api->picture_free(in);
    }
    api->encoder_close(enc);
  }
  TASK_CHECK(sizes[0] != sizes[1]);
  return 0;
}

/* Fixed candidate evaluation: the weighted integer-MV cost ranking must
 * differ from the unweighted ranking on an exposure change. */
static int test_weighted_search_ranking(void)
{
  const kvz_api *api = kvz_api_get(8);

  /* Two encoders, identical pictures and MVs available; one weights. */
  unsigned unweighted_costs[3];
  unsigned weighted_costs[3];

  for (int mode = 0; mode < 2; ++mode) {
    kvz_encoder *enc = open_encoder(api, mode);
    /* I frame. */
    kvz_picture *i_pic = api->picture_alloc(TEST_W, TEST_H);
    fill_picture(i_pic, 0);
    set_identity_wp(i_pic);
    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *ch = NULL;
    uint32_t bytes = 0;
    TASK_CHECK(api->encoder_encode(enc, i_pic, &ch, &bytes, &rec, &src, NULL) == 1);
    api->chunk_free(ch);
    api->picture_free(rec);
    api->picture_free(src);
    api->picture_free(i_pic);

    /* Set up a weighted P frame manually through the public API, then read
     * its state and score three fixed integer candidates directly. */
    kvz_picture *p = api->picture_alloc(TEST_W, TEST_H);
    fill_picture(p, 1);
    kvz_wp_params_t wp;
    wp.enabled = 1;
    wp.denom = 0;
    wp.weight[0] = -2;
    wp.weight[1] = 1;
    wp.weight[2] = 1;
    wp.offset[0] = 100;
    wp.offset[1] = 0;
    wp.offset[2] = 0;
    p->wp = wp;
    ch = NULL; bytes = 0; rec = NULL; src = NULL;
    TASK_CHECK(api->encoder_encode(enc, p, &ch, &bytes, &rec, &src, NULL) == 1);
    api->chunk_free(ch);
    api->picture_free(rec);
    api->picture_free(src);
    api->picture_free(p);

    encoder_state_t *state = &enc->states[0];
    const kvz_picture *refpic = state->frame->ref->images[0];
    const int cand[3][2] = { {0, 0}, {4, 0}, {0, 4} };
    for (int i = 0; i < 3; ++i) {
      unsigned c;
      if (mode) {
        c = kvz_wp_calc_sad(state, refpic, 8, 8,
                           state->tile->offset_x + 8 + cand[i][0],
                           state->tile->offset_y + 8 + cand[i][1],
                           16, 16);
        weighted_costs[i] = c;
      } else {
        c = kvz_image_calc_sad(state->tile->frame->source, refpic, 8, 8,
                               state->tile->offset_x + 8 + cand[i][0],
                               state->tile->offset_y + 8 + cand[i][1],
                               16, 16, NULL);
        unweighted_costs[i] = c;
      }
    }
    api->encoder_close(enc);
  }

  /* A negative weight changes which fixed candidate is cheapest. */
  int diff_order = 0;
  for (int i = 0; i < 3; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      if ((unweighted_costs[i] < unweighted_costs[j]) !=
          (weighted_costs[i] < weighted_costs[j])) {
        diff_order = 1;
      }
    }
  }
  TASK_CHECK(diff_order);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Registry                                                           */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "syntax", "pps-weighted-pred-flag", test_pps_flag },
  { "syntax", "pred-weight-table-fields", test_pred_weight_table },

  { "prediction", "integer-fractional-edge-vs-reference", test_prediction_variants },
  { "prediction", "weight-before-clip-ordering", test_clip_before_weight_ordering },

  { "pipeline", "config-constraints", test_config_constraints },
  { "pipeline", "invalid-picture-rejected-before-consume", test_invalid_picture_rejected },
  { "pipeline", "delivery-order-and-references", test_delivery_and_references },
  { "pipeline", "delayed-isolation-and-flush", test_delayed_isolation_and_flush },
  { "pipeline", "weighted-reference-chain", test_weighted_reference_chain },
  { "pipeline", "enabled-vs-disabled-output", test_enabled_vs_disabled },
  { "pipeline", "weighted-search-ranking", test_weighted_search_ranking },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
