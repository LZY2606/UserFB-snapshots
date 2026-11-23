/*****************************************************************************
 * ref_feedback.c: exact reference-picture loss feedback
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

#include "common/common.h"
#include "ref_feedback.h"

/* Test build hook; NULL on every production code path. */
x264_reffb_mb_commit_hook_t x264_reffb_mb_commit_hook = NULL;

#define REF_FEEDBACK_MAX 16

int x264_ref_feedback_validate_open( x264_t *h )
{
    if( !h->param.b_reference_feedback )
        return 0;

    /* The single-thread requirement is enforced up-front in
     * validate_parameters, before thread-count clamping. */
    if( h->param.i_bitdepth != 8 )
    {
        x264_log( h, X264_LOG_ERROR,
                  "b_reference_feedback is only supported at bit depth 8\n" );
        return -1;
    }
    if( PARAM_INTERLACED || h->param.b_fake_interlaced )
    {
        x264_log( h, X264_LOG_ERROR,
                  "b_reference_feedback is only supported for progressive frames\n" );
        return -1;
    }
    if( (h->param.i_csp & X264_CSP_MASK) != X264_CSP_I420 )
    {
        x264_log( h, X264_LOG_ERROR,
                  "b_reference_feedback is only supported for I420 input\n" );
        return -1;
    }
    if( h->param.b_intra_refresh )
    {
        x264_log( h, X264_LOG_ERROR,
                  "b_reference_feedback is not compatible with intra refresh\n" );
        return -1;
    }
    return 0;
}

void x264_ref_feedback_filter_lists( x264_t *h )
{
    if( !h->ref_feedback.b_active || h->sh.i_type == SLICE_TYPE_I )
        return;

    /* Drop every frame that belongs to an invalidation closure.  A weighted
     * prediction duplicate is the same picture as its ->orig, so it is lost
     * with it. */
    #define FRAME_IS_LOST(f) ( (f)->b_ref_lost || ( (f)->b_duplicate && (f)->orig && (f)->orig->b_ref_lost ) )
    for( int list = 0; list < 2; list++ )
    {
        int original = h->i_ref[list];
        int n = 0;
        for( int i = 0; i < original; i++ )
            if( !FRAME_IS_LOST( h->fref[list][i] ) )
                h->fref[list][n++] = h->fref[list][i];
        for( int i = n; i < original; i++ )
            h->fref[list][i] = NULL;
        h->i_ref[list] = n;
        h->mb.pic.i_fref[list] = n;
        /* Removing entries changes the list relative to the decoder's standard
         * default, so the in-stream reordering is mandatory even when only one
         * entry remains (the pairwise check cannot express a dropped index). */
        if( n < original )
            h->b_ref_reorder[list] = 1;
    }

    /* A temporally ordered list must stay legal: if one side is empty,
     * borrow clean references from the other side.  Reuse the surviving
     * list's distance order so prediction stays sensible.  The per-list caps
     * match the mvr/p_fref allocation limits, so analysis can never index an
     * unallocated reference slot. */
    int list_cap[2] = { h->frames.i_max_ref0, h->frames.i_max_ref1 };
    for( int empty = 0; empty < 2; empty++ )
    {
        int donor = empty ^ 1;
        if( h->i_ref[empty] != 0 || h->i_ref[donor] == 0 )
            continue;
        int n = X264_MIN( h->i_ref[donor], list_cap[empty] );
        for( int i = 0; i < n; i++ )
            h->fref[empty][i] = h->fref[donor][i];
        h->i_ref[empty] = n;
        h->mb.pic.i_fref[empty] = n;
        h->b_ref_reorder[empty] = 1;
    }
    #undef FRAME_IS_LOST

    /* The lists changed after reference_build_list's own check, so recompute
     * the in-stream reorder flags against the filtered, legalized lists. */
    reference_check_reorder( h );
}

/* Insert a PTS into a strictly increasing, deduplicated array. */
static void ref_used_insert( int64_t *pts, int *count, int64_t value )
{
    int n = *count;
    for( int i = 0; i < n; i++ )
    {
        if( pts[i] == value )
            return;
        if( pts[i] > value )
        {
            for( int j = n; j > i; j-- )
                pts[j] = pts[j-1];
            pts[i] = value;
            *count = n + 1;
            return;
        }
    }
    if( n < REF_FEEDBACK_MAX )
    {
        pts[n] = value;
        *count = n + 1;
    }
}

void x264_ref_feedback_record( x264_t *h )
{
    if( !h->ref_feedback.b_active )
        return;

    x264_frame_t *fdec = h->fdec;
    fdec->i_ref_used = 0;
    if( h->sh.i_type == SLICE_TYPE_I )
        return;

    /* The final per-macroblock reference indices were saved into fdec->ref[]
     * at each macroblock commit, so skip, direct, bipred and BREF frames are
     * all covered while candidates that were only searched never reached this
     * array.  Intra macroblocks carry -1.  The four entries per macroblock are
     * the 8x8 luma positions. */
    int nlists = h->sh.i_type == SLICE_TYPE_B ? 2 : 1;
    for( int list = 0; list < nlists; list++ )
        for( int mb = 0; mb < h->mb.i_mb_count; mb++ )
            for( int k = 0; k < 4; k++ )
            {
                int idx = fdec->ref[list][4 * mb + k];
                if( idx < 0 || idx >= h->i_ref[list] )
                    continue;
                x264_frame_t *ref = h->fref[list][idx];
                if( ref )
                    ref_used_insert( fdec->ref_used_pts, &fdec->i_ref_used, ref->i_pts );
            }
}

void x264_ref_feedback_commit_hook( x264_t *h, int mb_xy )
{
    if( !x264_reffb_mb_commit_hook || h->sh.i_type == SLICE_TYPE_I )
        return;

    static const int scan_idx[4] = { 0, 4, 8, 12 };
    int nlists = h->sh.i_type == SLICE_TYPE_B ? 2 : 1;
    for( int list = 0; list < nlists; list++ )
        for( int k = 0; k < 4; k++ )
        {
            int idx = h->mb.cache.ref[list][x264_scan8[scan_idx[k]]];
            if( idx < 0 || idx >= h->i_ref[list] || !h->fref[list][idx] )
                continue;
            x264_reffb_mb_commit_hook( mb_xy, list, idx, h->fref[list][idx]->i_pts );
        }
}

/* Find a still-retained reference frame by PTS. */
static x264_frame_t *find_retained( x264_t *h, int64_t pts )
{
    for( int i = 0; h->frames.reference[i]; i++ )
        if( h->frames.reference[i]->i_pts == pts )
            return h->frames.reference[i];
    /* The most recently encoded frame is still in fdec until the next call's
     * reference_update moves it into the reference array. */
    if( h->fdec->b_kept_as_ref && h->fdec->i_pts == pts )
        return h->fdec;
    return NULL;
}

/* Snapshot every currently retained reference: the DPB array plus the frame
 * most recently encoded, which has been delivered to the caller but not yet
 * pushed into the array.  Returns the count. */
static int snapshot_retained( x264_t *h, x264_frame_t **out, int cap )
{
    int n = 0;
    for( int i = 0; h->frames.reference[i]; i++ )
        if( n < cap )
            out[n++] = h->frames.reference[i];
    if( h->fdec->b_kept_as_ref )
    {
        int present = 0;
        for( int i = 0; i < n; i++ )
            if( out[i] == h->fdec )
            {
                present = 1;
                break;
            }
        if( !present && n < cap )
            out[n++] = h->fdec;
    }
    return n;
}

/* Compute the invalidation closure: the target plus every retained reference
 * whose recorded dependencies reach it, transitively.  Frames no longer in
 * the DPB cannot be reported and cannot extend the closure. */
static int build_closure( x264_t *h, x264_frame_t *target, x264_frame_t **closure, int max )
{
    x264_frame_t *retained[X264_REF_MAX + 2];
    int n_retained = snapshot_retained( h, retained, X264_REF_MAX + 2 );

    int n = 0;
    closure[n++] = target;

    for( int progress = 1; progress; )
    {
        progress = 0;
        for( int i = 0; i < n_retained; i++ )
        {
            x264_frame_t *f = retained[i];
            int already = 0;
            for( int j = 0; j < n; j++ )
                if( closure[j] == f )
                {
                    already = 1;
                    break;
                }
            if( already )
                continue;
            for( int d = 0; d < f->i_ref_used; d++ )
            {
                int64_t dep_pts = f->ref_used_pts[d];
                int reaches = 0;
                for( int j = 0; j < n; j++ )
                    if( closure[j]->i_pts == dep_pts )
                    {
                        reaches = 1;
                        break;
                    }
                if( reaches )
                {
                    if( n >= max )
                        return -1;
                    closure[n++] = f;
                    progress = 1;
                    break;
                }
            }
        }
    }
    return n;
}

static int in_closure( x264_frame_t **closure, int n, x264_frame_t *f )
{
    for( int i = 0; i < n; i++ )
        if( closure[i] == f )
            return 1;
    return 0;
}

int x264_encoder_invalidate_reference_exact( x264_t *h, int64_t pts )
{
    if( !h->param.b_reference_feedback )
        return -1;

    x264_frame_t *target = find_retained( h, pts );
    if( !target || !target->b_kept_as_ref )
        return -1;

    /* Repeated reports of the same picture are allowed: once it is already
     * lost the closure is a no-op and succeeds. */
    if( target->b_ref_lost )
        return 0;

    x264_frame_t *closure[X264_REF_MAX + 2];
    int n_closure = build_closure( h, target, closure, X264_REF_MAX + 2 );
    if( n_closure < 0 )
        return -1;

    /* The request must never leave the encoder without a single clean
     * reference for the pictures still pending.  Every retained frame,
     * including the one still in fdec, is a potential anchor, so the decision
     * considers the full retained set.  Reject atomically first. */
    x264_frame_t *retained[X264_REF_MAX + 2];
    int n_retained = snapshot_retained( h, retained, X264_REF_MAX + 2 );
    int clean = 0;
    for( int i = 0; i < n_retained; i++ )
        if( !in_closure( closure, n_closure, retained[i] ) )
        {
            clean = 1;
            break;
        }
    if( !clean )
        return -1;

    for( int i = 0; i < n_closure; i++ )
        closure[i]->b_ref_lost = 1;
    return 0;
}
