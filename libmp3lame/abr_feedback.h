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

#ifndef LAME_ABR_FEEDBACK_H
#define LAME_ABR_FEEDBACK_H

#include <stdint.h>

#ifndef lame_internal_flags_defined
#define lame_internal_flags_defined
struct lame_internal_flags;
typedef struct lame_internal_flags lame_internal_flags;
#endif

/*
 * Bookkeeping and control state for the optional ABR feedback loop.
 *
 * frames, encoded_bits and budget_bit_samples are only advanced for audio
 * frames which have actually been submitted by lame_encode_mp3_frame();
 * buffering calls which do not produce a frame leave the ledger untouched.
 */
typedef struct {
    int     enabled;         /* feedback mode was requested and accepted  */
    int     finished;        /* lame_encode_flush has ended the stream    */
    int     target_kbps;     /* target currently in effect                */
    int     pending_kbps;    /* last target set before the next frame    */
    int     current_frame_kbps; /* target used by the frame being encoded */
    uint64_t frames;         /* submitted audio frames                    */
    uint64_t encoded_bits;   /* physical bits of those frames             */
    uint64_t budget_bit_samples; /* sum(target_kbps * 1000 * frame_samples) */
} AbrFeedbackState_t;

void    abr_feedback_init(lame_internal_flags * gfc, int target_kbps);
void    abr_feedback_mark_finished(lame_internal_flags * gfc);
void    abr_feedback_frame_begin(lame_internal_flags * gfc);
void    abr_feedback_frame_end(lame_internal_flags * gfc, int frame_bits);
double  abr_feedback_quantizer_gain(lame_internal_flags const *gfc);
#endif /* LAME_ABR_FEEDBACK_H */
