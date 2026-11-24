/*****************************************************************************
 * This file is part of uvg266 VVC encoder.
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
 * INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * INCLUDING NEGLIGENCE OR OTHERWISE ARISING IN ANY WAY OUT OF THE USE OF THIS
 ****************************************************************************/

#include "input_frame_buffer.h"

#include "encoder.h"
#include "encoderstate.h"
#include "image.h"


void uvg_init_input_frame_buffer(input_frame_buffer_t *input_buffer)
{
  FILL(input_buffer->pic_buffer, 0);
  FILL(input_buffer->pts_buffer, 0);
  input_buffer->num_in = 0;
  input_buffer->num_out = 0;
  input_buffer->delay = 0;
  input_buffer->gop_skipped = 0;
  input_buffer->last_force_idx = 0;
  input_buffer->force_drain_until = -1;
  input_buffer->epoch_hier_pos = 0;
  input_buffer->display_order_last_dts = 0;
  input_buffer->display_order_dts_valid = false;
}

/**
 * \brief Pass an input frame to the encoder state.
 *
 * Returns the image that should be encoded next if there is a suitable
 * image available.
 *
 * The caller must not modify img_in after calling this function.
 *
 * \param buf         an input frame buffer
 * \param state       a main encoder state
 * \param img_in      input frame or NULL
 * \param first_done  whether the first frame has been done,
 *                    needed for the OBA rc
 * \return        pointer to the next picture, or NULL if no picture is
 *                available
 */
uvg_picture* uvg_encoder_feed_frame(input_frame_buffer_t *buf,
                                    encoder_state_t *const state,
                                    uvg_picture *const img_in, 
                                    int first_done)
{
  const encoder_control_t* const encoder = state->encoder_control;
  const uvg_config* const cfg = &encoder->cfg;

  const int gop_buf_size = 3 * cfg->gop_len;

  bool is_closed_gop = false;

  // Check for closed gop, we need an extra frame in the buffer in this case
  if (!cfg->open_gop && cfg->intra_period > 0 && cfg->gop_len > 0) is_closed_gop = true;

  if (cfg->gop_len == 0 || cfg->gop_lowdelay) {
    // No reordering of output pictures necessary.

    if (img_in == NULL) return NULL;

    // The marked picture is coded immediately, so it becomes the new epoch
    // anchor. The first picture is an IDR anyway, so nothing changes there.
    if (img_in->force_idr && buf->num_in > 0) {
      buf->last_force_idx = (int64_t)buf->num_in;
    }

    img_in->dts = img_in->pts;
    state->frame->gop_offset = 0;
    if (cfg->gop_len > 0 && !img_in->force_idr) {
      // Using a low delay GOP structure. The forced IDR is the anchor of a
      // new epoch, so its GOP offset stays zero.
      uint64_t frame_num = buf->num_out - buf->last_force_idx;
      if (cfg->intra_period) {
        frame_num %= cfg->intra_period;
      }
      state->frame->gop_offset = (frame_num + cfg->gop_len - 1) % cfg->gop_len;
    }
    buf->num_in++;
    buf->num_out++;
    // The current frame is the num_out-th input (0-based), so its display
    // position inside the epoch is num_out relative to the last anchor.
    state->frame->epoch_display_pos =
      (int32_t)((int64_t)buf->num_out - 1 - buf->last_force_idx);
    state->frame->display_order_epoch = false;
    state->frame->forced_epoch_active = buf->last_force_idx > 0;
    state->frame->epoch_coding_pos = (int32_t)state->frame->epoch_display_pos;
    return uvg_image_copy_ref(img_in);
  }
  
  if (img_in != NULL) {
    // Index of the next input picture, in range [-1, +inf). Values
    // i and j refer to the same indices in buf->pic_buffer iff
    // i === j (mod gop_buf_size).
    int64_t idx_in = buf->num_in - 1;

    // Index in buf->pic_buffer and buf->pts_buffer.
    int buf_idx = (idx_in + gop_buf_size) % gop_buf_size;

    // Save the input image in the buffer.
    assert(buf_idx >= 0 && buf_idx < gop_buf_size);
    assert(buf->pic_buffer[buf_idx] == NULL);
    buf->pic_buffer[buf_idx] = uvg_image_copy_ref(img_in);
    buf->pts_buffer[buf_idx] = img_in->pts;
    buf->num_in++;

    if (buf->num_in < cfg->gop_len + is_closed_gop ? 1 : 0) {
      // Not enough frames to start output.
      return 0;

    } else if (buf->num_in == cfg->gop_len + is_closed_gop ? 1 : 0) {
      // Now we known the PTSs that are needed to compute the delay.
      buf->delay = buf->pts_buffer[gop_buf_size - 1] - img_in->pts;
    }
  }

  if (buf->num_out == buf->num_in) {
    // All frames returned.
    return NULL;
  }

  if (img_in == NULL && buf->num_in < cfg->gop_len + is_closed_gop ? 1 : 0) {
    // End of the sequence but we have less than a single GOP of frames. Use
    // the difference between the PTSs of the first and the last frame as the
    // delay.
    int first_pic_idx = gop_buf_size - 1;
    int last_pic_idx  = (buf->num_in - 2 + gop_buf_size) % gop_buf_size;
    buf->delay = buf->pts_buffer[first_pic_idx] - buf->pts_buffer[last_pic_idx];
  }

  // Register a pending forced IDR. The scan follows the input order, so the
  // request stays attached to the exact source picture even while it waits
  // in the reordering buffer. Display frame f is stored in ring slot
  // (f - 1 + gop_buf_size) mod gop_buf_size (frame 0 uses the last slot).
  if (buf->force_drain_until < 0) {
    for (int64_t f = (int64_t)buf->num_out; f < (int64_t)buf->num_in; f++) {
      // A request on the very first picture needs no special handling: it is
      // an IDR anyway and must not change the regular output order.
      if (f == 0) continue;
      int slot = (int)((f - 1 + gop_buf_size) % gop_buf_size);
      uvg_picture *candidate = buf->pic_buffer[slot];
      if (candidate != NULL && candidate->force_idr) {
        buf->force_drain_until = f;
        break;
      }
    }
  } else if (buf->force_drain_until < (int64_t)buf->num_out) {
    // The marked picture was consumed as a regular output (e.g. the very
    // first frame is always an IDR), so the stale request can be dropped.
    buf->force_drain_until = -1;
  }

  // Index of the next output picture, in range [-1, +inf). Values
  // i and j refer to the same indices in buf->pic_buffer iff
  // i === j (mod gop_buf_size).
  int64_t idx_out;

  // DTS of the output picture.
  int64_t dts_out;

  // Number of the next output picture in the GOP.
  int gop_offset;

  // Display position of the selected picture inside the current epoch.
  int64_t epoch_display_pos;

  // Set for the pictures that must be coded as display-order plain P
  // frames: the truncated prefix before a cut and the IDR anchor itself.
  bool use_display_order;

  if (buf->num_out == 0) {
    // Output the first frame.
    idx_out = -1;
    dts_out = buf->pts_buffer[gop_buf_size - 1] + buf->delay;
    gop_offset = 0; // highest quality picture
    epoch_display_pos = 0;
    use_display_order = false;

  } else if(first_done) {
    const int64_t output_slot = (int64_t)buf->num_out - 1;

    if (buf->force_drain_until >= 0) {
      // Prefix drain / anchor: emit the smallest buffered display frame so
      // nothing is reordered across the pending boundary.
      int64_t frame = -1;
      for (int64_t f = 0; f < (int64_t)buf->num_in; ++f) {
        const int s = (int)((f - 1 + gop_buf_size) % gop_buf_size);
        if (buf->pic_buffer[s] != NULL) { frame = f; break; }
      }
      assert(frame >= 0);
      idx_out = frame - 1;
      use_display_order = true;

      if (frame == buf->force_drain_until) {
        // The marked picture opens the new epoch.
        gop_offset = 0;
        epoch_display_pos = 0;
        buf->last_force_idx = buf->force_drain_until;
        buf->force_drain_until = -1;
        buf->gop_skipped = 0;
        buf->epoch_hier_pos = 0;
      } else {
        // Truncated-prefix P frame: only earlier pictures may be referenced.
        gop_offset = 0;
        epoch_display_pos = frame;
      }
    } else if (buf->last_force_idx > 0) {
      // Hierarchical GOP inside the new epoch, counted relative to the IDR
      // anchor. Mirrors the legacy slot arithmetic but on epoch positions.
      const int64_t rel_slot = buf->epoch_hier_pos;
      gop_offset = (int)(rel_slot % cfg->gop_len);

      if (!cfg->open_gop && cfg->intra_period > 0) {
        int32_t num_extra = (int32_t)(rel_slot / (cfg->intra_period + 1));
        gop_offset = (int)((rel_slot - num_extra) % cfg->gop_len);
      }

      const int64_t rel_gop_start = rel_slot - gop_offset;
      gop_offset += buf->gop_skipped;

      int64_t display_frame;
      for (;;) {
        assert(gop_offset < cfg->gop_len + (is_closed_gop ? 1 : 0));
        display_frame = buf->last_force_idx + rel_gop_start +
                        cfg->gop[gop_offset].poc_offset;
        if (display_frame < (int64_t)buf->num_in) break;
        // Truncated final GOP: fall back to an available buffered anchor.
        buf->gop_skipped++;
        gop_offset++;
      }
      idx_out = display_frame - 1;
      epoch_display_pos = display_frame - buf->last_force_idx;
      use_display_order = false;
      state->frame->forced_epoch_active = true;
      state->frame->epoch_coding_pos = (int32_t)(buf->epoch_hier_pos + 1);
      buf->epoch_hier_pos++;
    } else {
    // Legacy hierarchical path, unchanged from the original behavior.
    const int64_t epoch_slot = output_slot;
    gop_offset = epoch_slot % cfg->gop_len;

    // For closed gop, calculate the gop_offset again
    if (!cfg->open_gop && cfg->intra_period > 0) {
      // Offset the GOP position for each extra I-frame added to the structure
      // in closed gop case
      int32_t num_extra_frames = (int32_t)(epoch_slot / (cfg->intra_period + 1));
      gop_offset = (epoch_slot - num_extra_frames) % cfg->gop_len;
    }

    // Index of the first picture in the GOP that is being output.
    int32_t gop_start_idx = (int32_t)(epoch_slot - gop_offset);

    // Skip pictures until we find an available one.
    gop_offset += buf->gop_skipped;

    // Every closed-gop IRAP handled here
    if (is_closed_gop && (!cfg->open_gop && (epoch_slot % (cfg->intra_period + 1)) == cfg->intra_period)) {
      idx_out = gop_start_idx;
    } else {
      for (;;) {
        assert(gop_offset < cfg->gop_len + is_closed_gop ? 1 : 0);
        idx_out = gop_start_idx + cfg->gop[gop_offset].poc_offset - 1;
        if (idx_out < (int64_t)buf->num_in - 1) {
          // An available picture found.
          break;
        }
        buf->gop_skipped++;
        gop_offset++;
      }
    }
    // No cut has happened, so display and epoch positions coincide.
    epoch_display_pos = idx_out + 1;
    use_display_order = false;
    }

    if (use_display_order || buf->last_force_idx > 0) {
      // Pictures around a cut use a monotonic DTS timeline. After the anchor
      // the hierarchy resumes but its legacy delay formula would not be
      // monotonic across the boundary, so keep the separate timeline.
      const int64_t current_pts =
        buf->pts_buffer[(idx_out + gop_buf_size) % gop_buf_size];
      if (!buf->display_order_dts_valid) {
        dts_out = current_pts;
        buf->display_order_dts_valid = true;
      } else {
        dts_out = MAX(current_pts, buf->display_order_last_dts + 1);
      }
      buf->display_order_last_dts = dts_out;
    } else if (buf->num_out < cfg->gop_len - 1) {
      // This picture needs a DTS that is less than the PTS of the first
      // frame so the delay must be applied.
      int32_t dts_idx = (int32_t)(buf->num_out - 1);
      dts_out = buf->pts_buffer[dts_idx % gop_buf_size] + buf->delay;
    } else {
      int32_t dts_idx = (int32_t)(buf->num_out - (cfg->gop_len - 1));
      dts_out = buf->pts_buffer[dts_idx % gop_buf_size] - 1;
    }
  }
  else {
    return NULL;
  }

  // Index in buf->pic_buffer and buf->pts_buffer.
  int buf_idx = (idx_out + gop_buf_size) % gop_buf_size;

  uvg_picture* next_pic = buf->pic_buffer[buf_idx];
  assert(next_pic != NULL);
  next_pic->dts = dts_out;
  buf->pic_buffer[buf_idx] = NULL;
  state->frame->gop_offset = gop_offset;
  state->frame->epoch_display_pos = (int32_t)epoch_display_pos;
  state->frame->display_order_epoch = use_display_order;
  if (use_display_order) {
    // Prefix P frames are not part of the new epoch yet; only the anchor and
    // later pictures carry the active-epoch flag.
    state->frame->forced_epoch_active = (buf->last_force_idx > 0);
    if (state->frame->forced_epoch_active) {
      state->frame->epoch_coding_pos = 0;
    }
  }

  buf->num_out++;
  return next_pic;
}
