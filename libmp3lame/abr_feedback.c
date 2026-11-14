/*
 *      Optional closed-loop feedback for ABR encoding
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "lame.h"
#include "machine.h"
#include "encoder.h"
#include "util.h"
#include "tables.h"
#include "abr_feedback.h"

/*
 * The error controller amortises the accumulated error over a few frames
 * so that a change of the requested target does not produce a single
 * extreme frame.  Corrections are bounded to a sane fraction of the
 * nominal quantization budget; bit rate selection itself stays with the
 * regular ABR reservoir logic.
 */
#define ABR_FEEDBACK_AMORTIZE   8.0
#define ABR_FEEDBACK_MAX_CORR   0.50
/* the quantiser follows a changed budget with some slack, so a correction
 * of the same order as the shortfall is allowed; the amortisation and the
 * backlog clamp keep it from becoming a burst */
#define ABR_FEEDBACK_MAX_BOOST  0.50
/* do not chase a backlog longer than this many frames; a long silent
 * passage must not force a long burst of overspending */
#define ABR_FEEDBACK_MAX_BACKLOG 24.0

static int
abr_kbps_in_range(lame_internal_flags const *gfc, int kbps)
{
    SessionConfig_t const *const cfg = &gfc->cfg;
    int const lo = bitrate_table[cfg->version][cfg->vbr_min_bitrate_index];
    int const hi = bitrate_table[cfg->version][cfg->vbr_max_bitrate_index];
    return lo <= kbps && kbps <= hi;
}

void
abr_feedback_init(lame_internal_flags * gfc, int target_kbps)
{
    AbrFeedbackState_t *const fb = &gfc->abr_feedback;
    fb->enabled = 1;
    fb->finished = 0;
    fb->target_kbps = target_kbps;
    fb->pending_kbps = -1;
    fb->current_frame_kbps = target_kbps;
    fb->frames = 0;
    fb->encoded_bits = 0;
    fb->budget_bit_samples = 0;
}

void
abr_feedback_mark_finished(lame_internal_flags * gfc)
{
    if (gfc->abr_feedback.enabled)
        gfc->abr_feedback.finished = 1;
}

/*
 * Called once for each frame that is about to enter the encoder.
 * A target which was set since the previous frame becomes effective
 * with this frame; several settings before the same frame collapse
 * into the last one because they all overwrite pending_kbps.
 */
void
abr_feedback_frame_begin(lame_internal_flags * gfc)
{
    AbrFeedbackState_t *const fb = &gfc->abr_feedback;
    if (!fb->enabled)
        return;
    if (fb->pending_kbps >= 0) {
        fb->target_kbps = fb->pending_kbps;
        fb->pending_kbps = -1;
    }
    fb->current_frame_kbps = fb->target_kbps;
}

/*
 * Called after a frame has been formatted into the bitstream.  frame_bits
 * is the complete physical size of that frame (header, CRC, side info,
 * padding), and is independent of the delayed delivery of bytes through
 * the bit reservoir.
 */
void
abr_feedback_frame_end(lame_internal_flags * gfc, int frame_bits)
{
    AbrFeedbackState_t *const fb = &gfc->abr_feedback;
    SessionConfig_t const *const cfg = &gfc->cfg;
    int const frame_samples = 576 * cfg->mode_gr;
    if (!fb->enabled)
        return;
    fb->encoded_bits += (uint64_t) (unsigned int) frame_bits;
    fb->budget_bit_samples += (uint64_t) fb->current_frame_kbps * 1000u *
        (uint64_t) frame_samples;
    fb->frames++;
}

/*
 * Multiplier for the quantisation budget.  A positive accumulated error
 * (more bits spent than budgeted, scaled by the output samplerate) lowers
 * the budget, a negative one raises it.  The error is converted into bits
 * by dividing by the output samplerate and spread over a few frames.
 */
double
abr_feedback_quantizer_gain(lame_internal_flags const *gfc)
{
    AbrFeedbackState_t const *const fb = &gfc->abr_feedback;
    SessionConfig_t const *const cfg = &gfc->cfg;
    double  frame_samples, nominal_bits, err_bits, corr;

    if (!fb->enabled || fb->frames == 0)
        return 1.0;

    frame_samples = 576.0 * cfg->mode_gr;
    nominal_bits = (double) fb->target_kbps * 1000.0 * frame_samples /
        (double) cfg->samplerate_out;
    if (nominal_bits <= 0.0)
        return 1.0;

    err_bits = (double) fb->encoded_bits -
        (double) fb->budget_bit_samples / (double) cfg->samplerate_out;

    if (err_bits > ABR_FEEDBACK_MAX_BACKLOG * nominal_bits)
        err_bits = ABR_FEEDBACK_MAX_BACKLOG * nominal_bits;
    if (err_bits < -ABR_FEEDBACK_MAX_BACKLOG * nominal_bits)
        err_bits = -ABR_FEEDBACK_MAX_BACKLOG * nominal_bits;

    corr = -err_bits / (ABR_FEEDBACK_AMORTIZE * nominal_bits);
    if (corr > ABR_FEEDBACK_MAX_BOOST)
        corr = ABR_FEEDBACK_MAX_BOOST;
    if (corr < -ABR_FEEDBACK_MAX_CORR)
        corr = -ABR_FEEDBACK_MAX_CORR;
    return 1.0 + corr;
}


/********************************************************************
 *  public API
 ********************************************************************/

int CDECL
lame_set_abr_feedback(lame_global_flags * gfp, int enable)
{
    if (!is_lame_global_flags_valid(gfp))
        return -1;
    /* only meaningful before a successful lame_init_params(); if that call
     * failed the application may correct the switch and try again */
    if (gfp->internal_flags != NULL &&
        gfp->internal_flags->lame_init_params_successful > 0)
        return -1;
    if (enable != 0 && enable != 1)
        return -1;
    gfp->abr_feedback = enable;
    return 0;
}

int CDECL
lame_set_abr_target(lame_global_flags * gfp, int kbps)
{
    lame_internal_flags *gfc;
    AbrFeedbackState_t *fb;

    if (!is_lame_global_flags_valid(gfp))
        return -1;
    gfc = gfp->internal_flags;
    if (!is_lame_internal_flags_valid(gfc))
        return -1;
    fb = &gfc->abr_feedback;
    if (!fb->enabled || fb->finished)
        return -1;
    if (!abr_kbps_in_range(gfc, kbps))
        return -1;

    /* effective from the next audio frame; last setting wins */
    fb->pending_kbps = kbps;
    return 0;
}

int CDECL
lame_get_abr_feedback_stats(lame_global_flags * gfp, lame_abr_feedback_stats * stats)
{
    lame_internal_flags const *gfc;
    AbrFeedbackState_t const *fb;

    if (!is_lame_global_flags_valid(gfp) || stats == NULL)
        return -1;
    gfc = gfp->internal_flags;
    if (!is_lame_internal_flags_valid(gfc))
        return -1;
    fb = &gfc->abr_feedback;
    if (!fb->enabled)
        return -1;

    stats->frames = fb->frames;
    stats->encoded_bits = fb->encoded_bits;
    stats->budget_bit_samples = fb->budget_bit_samples;
    stats->target_kbps = fb->target_kbps;
    return 0;
}
