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
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names of its
 *   contributors may be used to endorse or promote products derived from
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

#include "frame_bitrate.h"

#include <stdlib.h>

#include "encoder.h"
#include "encoderstate.h"
#include "threadqueue.h"


/**
 * \brief Minimum frame target in bits, matching the HM-style floor used by
 *        the existing lambda allocator.
 */
#define KVZ_FRAME_RC_MIN_TARGET_BITS 200.0


kvz_frame_rc_t *kvz_frame_rc_init(const encoder_control_t *ctrl)
{
  kvz_frame_rc_t *rc = calloc(1, sizeof(kvz_frame_rc_t));
  if (!rc) return NULL;

  rc->current_bitrate = ctrl->cfg.target_bitrate;
  rc->framerate_num = ctrl->cfg.framerate_num;
  rc->framerate_denom = ctrl->cfg.framerate_denom;
  rc->owf = ctrl->cfg.owf;
  rc->accepted = 0;
  rc->earned_total = 0;
  rc->earned_rem = 0;

  // The state ring has owf+1 entries and a picture is output before the same
  // slot could be accepted again, but acceptance happens slightly before the
  // matching output in the same encoder_encode call, so keep one extra slot.
  rc->ring_capacity = (unsigned)ctrl->cfg.owf + 2;
  rc->ring = calloc(rc->ring_capacity, sizeof(kvz_frame_rc_entry_t));
  if (!rc->ring) {
    free(rc);
    return NULL;
  }

  return rc;
}


void kvz_frame_rc_destroy(kvz_frame_rc_t *rc)
{
  if (!rc) return;
  free(rc->ring);
  free(rc);
}


int kvz_frame_rc_accept(kvz_frame_rc_t *rc,
                        encoder_state_t *state,
                        kvz_picture *pic)
{
  if (!rc || !pic) return 0;

  // Negative requests are rejected by the caller and must never reach here.
  if (pic->rc_bitrate < 0) return 0;

  if (pic->rc_bitrate > 0) {
    rc->current_bitrate = pic->rc_bitrate;
  }
  // rc_bitrate == 0 inherits the rate of the previous accepted picture;
  // current_bitrate is initialized from target_bitrate for the first one.

  const int32_t effective = rc->current_bitrate;
  if (effective <= 0) return 0;

  const uint64_t n = rc->accepted;
  kvz_frame_rc_entry_t *entry = &rc->ring[n % rc->ring_capacity];

  // Exact rational prefix accumulation in plain 64-bit arithmetic:
  //   floor((S + t) / k) = floor(S / k) + floor((S mod k + t) / k).
  // r <= INT32_MAX and denom <= INT32_MAX, so the product fits in uint64.
  const uint64_t term =
    (uint64_t)effective * (uint64_t)rc->framerate_denom;
  const uint64_t rem_sum = rc->earned_rem + term;
  rc->earned_total += rem_sum / (uint64_t)rc->framerate_num;
  rc->earned_rem = rem_sum % (uint64_t)rc->framerate_num;

  entry->bitrate = effective;
  entry->actual_bits = 0;
  entry->actual_prefix = 0;
  entry->earned = rc->earned_total;
  entry->state = state;
  entry->feedback_frames = 0;
  entry->earned_bits = 0;
  entry->actual_feedback_bits = 0;
  entry->target_bits = 0.0;

  // The effective rate is bound to this picture through the ring entry
  // above; the caller's input structure is not modified.

  rc->accepted += 1;
  return 1;
}


void kvz_frame_rc_settle(const encoder_state_t *state, uint64_t bits)
{
  const encoder_control_t *ctrl = state->encoder_control;
  if (!ctrl->frame_rc) return;

  kvz_frame_rc_t *rc = ctrl->frame_rc;
  const int64_t n = state->frame->num;
  if (n < 0) return;

  kvz_frame_rc_entry_t *entry = &rc->ring[(uint64_t)n % rc->ring_capacity];

  // Bitstream jobs are chained on the previous frame's bitstream job, so
  // pictures settle in input order and every earlier entry is settled too.
  entry->actual_bits = bits;
  if ((uint64_t)n == 0) {
    entry->actual_prefix = bits;
  } else {
    const kvz_frame_rc_entry_t *prev =
      &rc->ring[((uint64_t)n - 1) % rc->ring_capacity];
    entry->actual_prefix = prev->actual_prefix + bits;
  }
}


double kvz_frame_rc_compute_target(encoder_state_t *state)
{
  const encoder_control_t *ctrl = state->encoder_control;
  kvz_frame_rc_t *rc = ctrl->frame_rc;

  const int64_t n = state->frame->num;
  kvz_frame_rc_entry_t *entry = &rc->ring[(uint64_t)n % rc->ring_capacity];
  const int32_t r_n = entry->bitrate;

  const double q_n = (double)r_n * (double)rc->framerate_denom /
                     (double)rc->framerate_num;

  // Fixed feedback frontier: only the first m completed pictures count.
  const uint64_t m = (n > rc->owf) ? (uint64_t)(n - rc->owf) : 0;

  uint64_t e_m = 0;
  uint64_t a_m = 0;

  if (m > 0) {
    const kvz_frame_rc_entry_t *frontier =
      &rc->ring[(m - 1) % rc->ring_capacity];

    // Wait for the frontier picture's complete Annex-B output using the
    // existing bitstream job instead of polling. The job has already been
    // consumed when the frontier is the current state (owf=0).
    if (frontier->state != state &&
        frontier->state->tqj_bitstream_written != NULL) {
      kvz_threadqueue_waitfor(ctrl->threadqueue,
                              frontier->state->tqj_bitstream_written);
    }

    e_m = frontier->earned;
    a_m = frontier->actual_prefix;
  }

  const double target =
    MAX(KVZ_FRAME_RC_MIN_TARGET_BITS, q_n + ((double)e_m - (double)a_m) / 40.0);

  entry->feedback_frames = m;
  entry->earned_bits = e_m;
  entry->actual_feedback_bits = a_m;
  entry->target_bits = target;

  return target;
}


void kvz_frame_rc_fill_info(const encoder_state_t *state,
                            kvz_frame_info *info)
{
  const encoder_control_t *ctrl = state->encoder_control;
  if (!ctrl->frame_rc) return;

  const kvz_frame_rc_t *rc = ctrl->frame_rc;
  const int64_t n = state->frame->num;
  if (n < 0) return;

  const kvz_frame_rc_entry_t *entry =
    &rc->ring[(uint64_t)n % rc->ring_capacity];

  info->rc_bitrate = entry->bitrate;
  info->rc_feedback_frames = entry->feedback_frames;
  info->rc_earned_bits = entry->earned_bits;
  info->rc_actual_bits = entry->actual_feedback_bits;
  info->rc_target_bits = entry->target_bits;
}
