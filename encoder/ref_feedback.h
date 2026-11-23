/*****************************************************************************
 * ref_feedback.h: exact reference-picture loss feedback
 *****************************************************************************
 * Copyright (C) 2024 x264 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#ifndef X264_ENCODER_REF_FEEDBACK_H
#define X264_ENCODER_REF_FEEDBACK_H

/* Reject unsupported parameter combinations when b_reference_feedback is on. */
#define x264_ref_feedback_validate_open x264_template(ref_feedback_validate_open)
int  x264_ref_feedback_validate_open( x264_t *h );

/* Remove invalidation-closure frames from the freshly built L0/L1 lists and
 * legalize a temporally emptied list with clean references from the other one. */
#define x264_ref_feedback_filter_lists x264_template(ref_feedback_filter_lists)
void x264_ref_feedback_filter_lists( x264_t *h );

/* Recompute b_ref_reorder for the (possibly filtered) current lists.
 * Defined in encoder.c. */
void reference_check_reorder( x264_t *h );

/* Record the references actually committed by the final macroblock decisions
 * of the frame just encoded, sorted by PTS and deduplicated. */
#define x264_ref_feedback_record x264_template(ref_feedback_record)
void x264_ref_feedback_record( x264_t *h );

/* Test-build instrumentation: if non-NULL, invoked at the final macroblock
 * commit point with the resolved PTS of every reference index the committed
 * macroblock uses.  Production code leaves it NULL. */
typedef void (*x264_reffb_mb_commit_hook_t)( int mb_xy, int list, int ref_idx, int64_t ref_pts );
extern x264_reffb_mb_commit_hook_t x264_reffb_mb_commit_hook;
#define x264_ref_feedback_commit_hook x264_template(ref_feedback_commit_hook)
void x264_ref_feedback_commit_hook( x264_t *h, int mb_xy );

#endif
