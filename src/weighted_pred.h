#ifndef WEIGHTED_PRED_H_
#define WEIGHTED_PRED_H_
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
 * \ingroup Reconstruction
 * \file
 * Explicit single-reference weighted prediction for P slices.
 *
 * All weighting is performed at the HEVC internal sample precision
 * (intermediate filter output, i.e. the reference sample shifted up by 6 for
 * integer positions) and only the final weighted, rounded and offset sample
 * is clipped to the 8-bit range.
 */

#include "global.h" // IWYU pragma: keep
#include "image.h"
#include "kvazaar.h"

struct bitstream_t;
struct encoder_state_t;

/**
 * \brief Check the per-picture weighted prediction parameters.
 *
 * \return 1 if the parameters are valid (or weighting is disabled),
 *         0 otherwise
 */
int kvz_wp_validate_picture(const kvz_wp_params_t *wp);

/**
 * \brief Whether weighted prediction is applied to the current frame.
 *
 * Requires the config switch to be on, a P slice and an enabled per-picture
 * parameter set. The first (I) picture is never weighted.
 */
int kvz_wp_frame_active(const struct encoder_state_t *state);

/**
 * \brief Get the effective weight parameters of the current frame.
 *
 * \return pointer to the parameters, or NULL when weighting is not active
 */
const kvz_wp_params_t *kvz_wp_get_frame_params(const struct encoder_state_t *state);

/**
 * \brief Build a weighted uniprediction block for one PU.
 *
 * Runs the standard HEVC fractional-pixel filtering (or integer-pixel
 * extension at the frame borders) at internal precision and applies the
 * per-component weight, rounding and offset before clipping to 0..255.
 *
 * \param state            encoder state
 * \param ref              reference picture
 * \param pu_x,pu_y        PU position in tile coordinates (luma pixels)
 * \param pu_w,pu_h        PU size (luma pixels)
 * \param out_stride_luma  stride of the destination luma plane
 * \param mv_param         motion vector in quarter-luma precision
 * \param yuv_px           destination buffers
 * \param predict_luma     generate luma
 * \param predict_chroma   generate chroma
 */
void kvz_wp_predict_unipred(const struct encoder_state_t *state,
                            const kvz_picture *ref,
                            int32_t pu_x,
                            int32_t pu_y,
                            int32_t pu_w,
                            int32_t pu_h,
                            int32_t out_stride_luma,
                            const int16_t mv_param[2],
                            yuv_t *yuv_px,
                            bool predict_luma,
                            bool predict_chroma);

/**
 * \brief Weighted integer-motion SAD between a source block and the
 *        weighted reference block.
 *
 * Border extension matches the normal inter prediction path. Luma only.
 */
unsigned kvz_wp_calc_sad(const struct encoder_state_t *state,
                         const kvz_picture *ref,
                         int pic_x,
                         int pic_y,
                         int ref_x,
                         int ref_y,
                         int block_width,
                         int block_height);

/**
 * \brief Weighted SATD at an integer motion-vector position. Luma only.
 */
unsigned kvz_wp_calc_satd_int(const struct encoder_state_t *state,
                              const kvz_picture *ref,
                              int pic_x,
                              int pic_y,
                              int ref_x,
                              int ref_y,
                              int block_width,
                              int block_height);

/**
 * \brief Weighted fractional-pixel SATD for one quarter-pel position.
 *
 * \param fx,fy  fractional part of the MV in 0..3 (quarter luma)
 */
unsigned kvz_wp_calc_satd_frac(const struct encoder_state_t *state,
                               const kvz_picture *ref,
                               int origin_x,
                               int origin_y,
                               int int_mv_x,
                               int int_mv_y,
                               int block_width,
                               int block_height,
                               int fx,
                               int fy);

/**
 * \brief Write pred_weight_table() for one L0 reference.
 */
void kvz_wp_write_pred_weight_table(struct bitstream_t *stream,
                                    const kvz_wp_params_t *wp);

#endif // WEIGHTED_PRED_H_
