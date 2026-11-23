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
 * Explicit single-reference weighted prediction for P slices.
 *
 * The prediction is always generated at the HEVC internal sample precision
 * (reference samples multiplied by 64) so that the weight can be applied
 * before any clipping to 8-bit. Fractional positions use the standard luma
 * 8-tap and chroma 4-tap interpolation filters; border positions use the
 * same edge extension as the normal inter path.
 */

#include "weighted_pred.h"

#include <stdlib.h>
#include <string.h>

#include "bitstream.h"
#include "encoderstate.h"
#include "image.h"
#include "strategies/strategies-ipol.h"
#include "strategies/strategies-picture.h"

extern int8_t kvz_g_luma_filter[4][8];
extern int8_t kvz_g_chroma_filter[8][4];

/** \brief Number of fractional bits carried by the internal precision. */
#define WP_INTERNAL_SHIFT 6
#define WP_INTERNAL_SCALE (1 << WP_INTERNAL_SHIFT)
#define WP_ROUND_OFFSET (1 << (WP_INTERNAL_SHIFT - 1))


int kvz_wp_validate_picture(const kvz_wp_params_t *wp)
{
  if (!wp || !wp->enabled) {
    // Disabled weighting is always valid and maps to the identity transform.
    return 1;
  }

  if (wp->denom < 0 || wp->denom > 7) return 0;

  const int base = 1 << wp->denom;

  for (int c = 0; c < 3; ++c) {
    const int delta_weight = wp->weight[c] - base;
    if (delta_weight < -128 || delta_weight > 127) return 0;
    if (wp->offset[c] < -128 || wp->offset[c] > 127) return 0;
  }

  // Additional chroma range from HEVC 7.4.7.2 (weighted sample prediction).
  for (int c = 1; c < 3; ++c) {
    // floor(128 * weight / 2^denom), computed without floating point and
    // truncation toward zero.
    int product = 128 * wp->weight[c];
    int q = product / base;
    if (product < 0 && product % base != 0) q -= 1;

    const int of = wp->offset[c] - 128 + q;
    if (of < -512 || of > 511) return 0;
  }

  return 1;
}

int kvz_wp_frame_active(const encoder_state_t *state)
{
  return state->frame->wp_active;
}

const kvz_wp_params_t *kvz_wp_get_frame_params(const encoder_state_t *state)
{
  return state->frame->wp_active ? &state->frame->wp : NULL;
}

/**
 * \brief Apply the weight, rounding and offset to one internal-precision
 *        sample and clip it to the 8-bit output range.
 */
static INLINE kvz_pixel wp_weight_sample(int internal_sample,
                                         int weight,
                                         int denom,
                                         int offset)
{
  int v = internal_sample * weight;
  v = (v + WP_ROUND_OFFSET) >> denom;
  v += offset;
  return (kvz_pixel)CLIP(0, 255, v);
}

static void wp_weight_plane(const kvz_pixel_im *src,
                            int src_stride,
                            kvz_pixel *dst,
                            int dst_stride,
                            int width,
                            int height,
                            int weight,
                            int denom,
                            int offset)
{
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      dst[y * dst_stride + x] =
        wp_weight_sample(src[y * src_stride + x], weight, denom, offset);
    }
  }
}

/**
 * \brief Copy an integer-position luma block at internal precision, extending
 *        the frame borders by repetition.
 */
static void wp_extended_block_luma_hi(const kvz_picture *ref,
                                      int blk_x, int blk_y,
                                      int width, int height,
                                      kvz_pixel_im *dst, int dst_stride)
{
  for (int y = 0; y < height; ++y) {
    const int sy = CLIP(0, ref->height - 1, blk_y + y);
    for (int x = 0; x < width; ++x) {
      const int sx = CLIP(0, ref->width - 1, blk_x + x);
      dst[y * dst_stride + x] = (kvz_pixel_im)ref->y[sy * ref->stride + sx]
                                << WP_INTERNAL_SHIFT;
    }
  }
}

/**
 * \brief Copy an integer-position chroma block at internal precision,
 *        extending the frame borders by repetition.
 */
static void wp_extended_block_chroma_hi(const kvz_pixel *plane,
                                        int plane_stride,
                                        int plane_w, int plane_h,
                                        int blk_x, int blk_y,
                                        int width, int height,
                                        kvz_pixel_im *dst, int dst_stride)
{
  for (int y = 0; y < height; ++y) {
    const int sy = CLIP(0, plane_h - 1, blk_y + y);
    for (int x = 0; x < width; ++x) {
      const int sx = CLIP(0, plane_w - 1, blk_x + x);
      dst[y * dst_stride + x] = (kvz_pixel_im)plane[sy * plane_stride + sx]
                                << WP_INTERNAL_SHIFT;
    }
  }
}

void kvz_wp_predict_unipred(const encoder_state_t *state,
                            const kvz_picture *ref,
                            int32_t pu_x,
                            int32_t pu_y,
                            int32_t pu_w,
                            int32_t pu_h,
                            int32_t out_stride_luma,
                            const int16_t mv_param[2],
                            yuv_t *yuv_px,
                            bool predict_luma,
                            bool predict_chroma)
{
  const kvz_wp_params_t *wp = kvz_wp_get_frame_params(state);
  assert(wp);

  const int shift_w = SHIFT_W;
  const int shift_h = SHIFT_H;
  const int frac_luma_x = mv_param[0] & 3;
  const int frac_luma_y = mv_param[1] & 3;
  const bool fractional_luma = frac_luma_x || frac_luma_y;

  // The generic interpolation keeps its intermediate results in int16_t.
  kvz_pixel_im hi_buf[LCU_LUMA_SIZE];

  if (predict_luma) {
    if (fractional_luma) {
      // Standard quarter-pel luma interpolation at internal precision,
      // including border extension.
      kvz_pixel ext_buffer[KVZ_IPOL_MAX_INPUT_SIZE_LUMA_SIMD];
      kvz_pixel *ext = NULL;
      kvz_pixel *ext_origin = NULL;
      int ext_s = 0;
      kvz_epol_args epol_args = {
        .src = ref->y,
        .src_w = ref->width,
        .src_h = ref->height,
        .src_s = ref->stride,
        .blk_x = state->tile->offset_x + pu_x + (mv_param[0] >> 2),
        .blk_y = state->tile->offset_y + pu_y + (mv_param[1] >> 2),
        .blk_w = pu_w,
        .blk_h = pu_h,
        .pad_l = KVZ_LUMA_FILTER_OFFSET,
        .pad_r = KVZ_EXT_PADDING_LUMA - KVZ_LUMA_FILTER_OFFSET,
        .pad_t = KVZ_LUMA_FILTER_OFFSET,
        .pad_b = KVZ_EXT_PADDING_LUMA - KVZ_LUMA_FILTER_OFFSET,
        .pad_b_simd = 1,
      };
      epol_args.buf = ext_buffer;
      epol_args.ext = &ext;
      epol_args.ext_origin = &ext_origin;
      epol_args.ext_s = &ext_s;

      kvz_get_extended_block(&epol_args);
      kvz_sample_quarterpel_luma_hi(state->encoder_control,
                                    ext_origin, ext_s,
                                    pu_w, pu_h,
                                    hi_buf, pu_w,
                                    frac_luma_x, frac_luma_y, mv_param);
    } else {
      const int blk_x = state->tile->offset_x + pu_x + (mv_param[0] >> 2);
      const int blk_y = state->tile->offset_y + pu_y + (mv_param[1] >> 2);
      wp_extended_block_luma_hi(ref, blk_x, blk_y, pu_w, pu_h,
                                hi_buf, pu_w);
    }

    wp_weight_plane(hi_buf, pu_w, yuv_px->y, out_stride_luma,
                    pu_w, pu_h, wp->weight[0], wp->denom, wp->offset[0]);
  }

  if (!predict_chroma) return;

  const unsigned out_stride_c = out_stride_luma >> shift_w;
  const int pb_w = pu_w >> shift_w;
  const int pb_h = pu_h >> shift_h;

  // With 4:2:0, odd integer luma coordinates need chroma interpolation even
  // when the luma MV is integer.
  const int mv_mask_x = shift_w ? 7 : 3;
  const int mv_mask_y = shift_h ? 7 : 3;
  const int mv_shift_x = shift_w ? 3 : 2;
  const int mv_shift_y = shift_h ? 3 : 2;
  const int frac_cx = mv_param[0] & mv_mask_x;
  const int frac_cy = mv_param[1] & mv_mask_y;
  const bool fractional_chroma = fractional_luma || frac_cx || frac_cy;

  const kvz_pixel *chroma_plane[2] = { ref->u, ref->v };
  kvz_pixel *dst_plane[2] = { yuv_px->u, yuv_px->v };
  for (int c = 0; c < 2; ++c) {
    if (fractional_chroma) {
      kvz_pixel ext_buffer[KVZ_IPOL_MAX_INPUT_SIZE_LUMA_SIMD];
      kvz_pixel *ext = NULL;
      kvz_pixel *ext_origin = NULL;
      int ext_s = 0;
      kvz_epol_args epol_args = {
        .src = (kvz_pixel *)chroma_plane[c],
        .src_w = ref->width_c,
        .src_h = ref->height_c,
        .src_s = ref->stride_c,
        .blk_x = ((state->tile->offset_x + pu_x) >> shift_w)
                 + (mv_param[0] >> mv_shift_x),
        .blk_y = ((state->tile->offset_y + pu_y) >> shift_h)
                 + (mv_param[1] >> mv_shift_y),
        .blk_w = pb_w,
        .blk_h = pb_h,
        .pad_l = KVZ_CHROMA_FILTER_OFFSET,
        .pad_r = KVZ_EXT_PADDING_CHROMA - KVZ_CHROMA_FILTER_OFFSET,
        .pad_t = KVZ_CHROMA_FILTER_OFFSET,
        .pad_b = KVZ_EXT_PADDING_CHROMA - KVZ_CHROMA_FILTER_OFFSET,
        .pad_b_simd = 3,
      };
      epol_args.buf = ext_buffer;
      epol_args.ext = &ext;
      epol_args.ext_origin = &ext_origin;
      epol_args.ext_s = &ext_s;

      kvz_get_extended_block(&epol_args);
      kvz_sample_octpel_chroma_hi(state->encoder_control,
                                  ext_origin, ext_s,
                                  pb_w, pb_h,
                                  hi_buf, pb_w,
                                  frac_cx, frac_cy, mv_param);
    } else {
      const int blk_x = ((state->tile->offset_x + pu_x) >> shift_w)
                        + (mv_param[0] >> mv_shift_x);
      const int blk_y = ((state->tile->offset_y + pu_y) >> shift_h)
                        + (mv_param[1] >> mv_shift_y);
      wp_extended_block_chroma_hi(chroma_plane[c], ref->stride_c,
                                  ref->width_c, ref->height_c,
                                  blk_x, blk_y, pb_w, pb_h,
                                  hi_buf, pb_w);
    }

    wp_weight_plane(hi_buf, pb_w, dst_plane[c], out_stride_c,
                    pb_w, pb_h,
                    wp->weight[c + 1], wp->denom, wp->offset[c + 1]);
  }
}

unsigned kvz_wp_calc_sad(const encoder_state_t *state,
                         const kvz_picture *ref,
                         int pic_x,
                         int pic_y,
                         int ref_x,
                         int ref_y,
                         int block_width,
                         int block_height)
{
  const kvz_wp_params_t *wp = kvz_wp_get_frame_params(state);
  assert(wp);
  const kvz_picture *src = state->tile->frame->source;

  const int w = wp->weight[0];
  const int d = wp->denom;
  const int o = wp->offset[0];

  unsigned sad = 0;
  for (int y = 0; y < block_height; ++y) {
    const int sy = CLIP(0, ref->height - 1, ref_y + y);
    for (int x = 0; x < block_width; ++x) {
      const int sx = CLIP(0, ref->width - 1, ref_x + x);
      const int internal = (int)ref->y[sy * ref->stride + sx]
                           << WP_INTERNAL_SHIFT;
      const kvz_pixel pred = wp_weight_sample(internal, w, d, o);
      sad += abs((int)src->y[(pic_y + y) * src->stride + pic_x + x]
                 - (int)pred);
    }
  }
  return sad;
}

unsigned kvz_wp_calc_satd_int(const encoder_state_t *state,
                              const kvz_picture *ref,
                              int pic_x,
                              int pic_y,
                              int ref_x,
                              int ref_y,
                              int block_width,
                              int block_height)
{
  const kvz_wp_params_t *wp = kvz_wp_get_frame_params(state);
  assert(wp);
  const kvz_picture *src = state->tile->frame->source;

  kvz_pixel pred[LCU_LUMA_SIZE];

  for (int y = 0; y < block_height; ++y) {
    const int sy = CLIP(0, ref->height - 1, ref_y + y);
    for (int x = 0; x < block_width; ++x) {
      const int sx = CLIP(0, ref->width - 1, ref_x + x);
      const int internal = (int)ref->y[sy * ref->stride + sx]
                           << WP_INTERNAL_SHIFT;
      pred[y * block_width + x] =
        wp_weight_sample(internal, wp->weight[0], wp->denom, wp->offset[0]);
    }
  }

  return kvz_satd_any_size(block_width, block_height,
                           pred, block_width,
                           &src->y[pic_y * src->stride + pic_x],
                           src->stride);
}

unsigned kvz_wp_calc_satd_frac(const encoder_state_t *state,
                               const kvz_picture *ref,
                               int origin_x,
                               int origin_y,
                               int int_mv_x,
                               int int_mv_y,
                               int block_width,
                               int block_height,
                               int fx,
                               int fy)
{
  const kvz_wp_params_t *wp = kvz_wp_get_frame_params(state);
  assert(wp);
  const kvz_picture *src = state->tile->frame->source;

  kvz_pixel_im hi_buf[LCU_LUMA_SIZE];
  kvz_pixel pred[LCU_LUMA_SIZE];

  if (fx == 0 && fy == 0) {
    const int blk_x = state->tile->offset_x + origin_x + int_mv_x;
    const int blk_y = state->tile->offset_y + origin_y + int_mv_y;
    wp_extended_block_luma_hi(ref, blk_x, blk_y,
                              block_width, block_height, hi_buf, block_width);
  } else {
    kvz_pixel ext_buffer[KVZ_IPOL_MAX_INPUT_SIZE_LUMA_SIMD];
    kvz_pixel *ext = NULL;
    kvz_pixel *ext_origin = NULL;
    int ext_s = 0;
    const int16_t mv[2] = { (int16_t)(fx), (int16_t)(fy) };
    kvz_epol_args epol_args = {
      .src = ref->y,
      .src_w = ref->width,
      .src_h = ref->height,
      .src_s = ref->stride,
      .blk_x = state->tile->offset_x + origin_x + int_mv_x,
      .blk_y = state->tile->offset_y + origin_y + int_mv_y,
      .blk_w = block_width,
      .blk_h = block_height,
      .pad_l = KVZ_LUMA_FILTER_OFFSET,
      .pad_r = KVZ_EXT_PADDING_LUMA - KVZ_LUMA_FILTER_OFFSET,
      .pad_t = KVZ_LUMA_FILTER_OFFSET,
      .pad_b = KVZ_EXT_PADDING_LUMA - KVZ_LUMA_FILTER_OFFSET,
      .pad_b_simd = 1,
    };
    epol_args.buf = ext_buffer;
    epol_args.ext = &ext;
    epol_args.ext_origin = &ext_origin;
    epol_args.ext_s = &ext_s;

    kvz_get_extended_block(&epol_args);
    kvz_sample_quarterpel_luma_hi(state->encoder_control,
                                  ext_origin, ext_s,
                                  block_width, block_height,
                                  hi_buf, block_width,
                                  fx, fy, mv);
  }

  wp_weight_plane(hi_buf, block_width, pred, block_width,
                  block_width, block_height,
                  wp->weight[0], wp->denom, wp->offset[0]);

  return kvz_satd_any_size(block_width, block_height,
                           pred, block_width,
                           &src->y[origin_y * src->stride + origin_x],
                           src->stride);
}

void kvz_wp_write_pred_weight_table(bitstream_t *stream,
                                    const kvz_wp_params_t *wp)
{
  // One L0 entry, P slices only (num_ref_idx_l0_active_minus1 == 0).
  WRITE_UE(stream, wp->denom, "luma_log2_weight_denom");

  // Chroma uses the same denominator in this implementation.
  WRITE_SE(stream, 0, "delta_chroma_log2_weight_denom");

  for (int ref = 0; ref < 1; ++ref) {
    (void)ref;
    // Luma.
    const bool luma_flag = !(wp->weight[0] == (1 << wp->denom) &&
                             wp->offset[0] == 0);
    WRITE_U(stream, luma_flag, 1, "luma_weight_l0_flag");
    if (luma_flag) {
      WRITE_SE(stream, wp->weight[0] - (1 << wp->denom),
               "delta_luma_weight_l0");
      WRITE_SE(stream, wp->offset[0], "luma_offset_l0");
    }

    // The shared chroma flag may only be zero when both chroma components
    // are the identity transform. When the flag is one, both entries must
    // be written.
    const bool chroma_identity =
      wp->weight[1] == (1 << wp->denom) && wp->offset[1] == 0 &&
      wp->weight[2] == (1 << wp->denom) && wp->offset[2] == 0;
    const bool chroma_flag = !chroma_identity;
    WRITE_U(stream, chroma_flag, 1, "chroma_weight_l0_flag");
    if (chroma_flag) {
      for (int c = 0; c < 2; ++c) {
        WRITE_SE(stream, wp->weight[c + 1] - (1 << wp->denom),
                 "delta_chroma_weight_l0");
        // The API offset is the final effective offset; the syntax element
        // carries a difference against the inherited 128 midpoint.
        WRITE_SE(stream, wp->offset[c + 1] - 128,
                 "delta_chroma_offset_l0");
      }
    }
  }
}
