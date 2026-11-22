#ifndef FRAME_BITRATE_H_
#define FRAME_BITRATE_H_
/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (c) 2021, Tampere University, ITU/ISO/IEC, project contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names of
 *   its contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ****************************************************************************/

/**
 * \ingroup Control
 * \file
 * \brief Per-frame target bitrate switching.
 *
 * When kvz_config.frame_bitrate is enabled, the target bitrate can be changed
 * from input picture to input picture without restarting the encoder. The
 * effective bitrate is bound to each accepted input picture at the moment it
 * enters the pipeline, so pictures already being encoded never observe a
 * later bitrate change through the shared encoder control.
 *
 * The frame target handed to the existing lambda allocation chain for
 * picture n is
 *
 *   q_n = r_n * framerate_denom / framerate_num
 *   m   = max(0, n - owf)
 *   target_n = max(200, q_n + (E_m - A_m) / 40)
 *
 * where E_m is the earned bit budget of the first m pictures accumulated as
 * an exact rational prefix and A_m is their complete Annex-B output cost in
 * bits, including every header and SEI message.
 */

#include "global.h" // IWYU pragma: keep

#include "kvazaar.h"

#include <stdint.h>

struct encoder_control_t;
struct encoder_state_t;


/**
 * \brief One accepted input picture in the feedback history.
 */
typedef struct kvz_frame_rc_entry_t
{
  /** \brief Effective bitrate in bits/s bound at acceptance. */
  int32_t bitrate;

  /** \brief Complete Annex-B output cost in bits once settled. */
  uint64_t actual_bits;

  /** \brief Prefix of actual costs through this picture, in bits. */
  uint64_t actual_prefix;

  /**
   * \brief Exact rational-prefix earned budget E through this picture,
   *        i.e. floor(sum(r_i * framerate_denom) / framerate_num).
   */
  uint64_t earned;

  /** \brief Encoder state that codes this picture. */
  struct encoder_state_t *state;

  /* Diagnostic snapshot computed together with the frame target. */
  uint64_t feedback_frames;
  uint64_t earned_bits;
  uint64_t actual_feedback_bits;
  double target_bits;
} kvz_frame_rc_entry_t;


/**
 * \brief Per-encoder state for frame bitrate switching.
 */
typedef struct kvz_frame_rc_t
{
  /** \brief Bitrate inherited by zero-valued input pictures. */
  int32_t current_bitrate;

  int32_t framerate_num;
  int32_t framerate_denom;
  int owf;

  /** \brief Number of accepted input pictures. */
  uint64_t accepted;

  /**
   * \brief Exact rational prefix kept as quotient + remainder:
   *        sum(r_i * framerate_denom) == earned_total * framerate_num +
   *                                     earned_rem, 0 <= earned_rem < num.
   */
  uint64_t earned_total;
  uint64_t earned_rem;

  /** \brief Ring of accepted pictures, indexed by picture number. */
  kvz_frame_rc_entry_t *ring;
  unsigned ring_capacity;
} kvz_frame_rc_t;


/**
 * \brief Allocate the per-encoder frame bitrate state.
 *
 * \param ctrl  encoder control, whose config must already pass the
 *              frame_bitrate validation checks
 * \return      state, or NULL on failure
 */
kvz_frame_rc_t *kvz_frame_rc_init(const struct encoder_control_t *ctrl);

/**
 * \brief Free frame bitrate state. NULL is allowed.
 */
void kvz_frame_rc_destroy(kvz_frame_rc_t *rc);

/**
 * \brief Bind an effective bitrate to an input picture.
 *
 * A positive pic->rc_bitrate starts a new rate, zero inherits the rate of
 * the previous accepted picture (or the configured target bitrate for the
 * first one). The effective rate is written back to pic->rc_bitrate and a
 * new feedback history entry is created.
 *
 * \param rc     per-encoder frame bitrate state
 * \param state  encoder state that will code this picture
 * \param pic    accepted input picture
 * \return       1 on success, 0 on failure
 */
int kvz_frame_rc_accept(kvz_frame_rc_t *rc,
                        struct encoder_state_t *state,
                        kvz_picture *pic);

/**
 * \brief Settle the complete Annex-B cost of a coded picture.
 *
 * Called from the bitstream worker once the whole access unit of
 * state->frame->num has been written. Later workers cannot settle their
 * picture ahead of order because bitstream jobs depend on the previous
 * frame's bitstream job.
 *
 * \param state  encoder state of the finished picture
 * \param bits   complete Annex-B output cost in bits
 */
void kvz_frame_rc_settle(const struct encoder_state_t *state, uint64_t bits);

/**
 * \brief Compute the frame target for the picture bound to this state.
 *
 * Waits on the existing bitstream job dependency when the feedback frontier
 * picture has not been completed yet; never busy polls. Also stores the
 * diagnostic snapshot (m, E_m, A_m, target) on the state.
 *
 * \param state  encoder state being prepared for a new picture
 * \return       frame target in bits for the lambda allocation chain
 */
double kvz_frame_rc_compute_target(struct encoder_state_t *state);

/**
 * \brief Copy the diagnostic snapshot of an output picture into frame info.
 */
void kvz_frame_rc_fill_info(const struct encoder_state_t *state,
                            kvz_frame_info *info);

#endif // FRAME_BITRATE_H_
