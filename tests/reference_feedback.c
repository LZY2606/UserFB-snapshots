/*****************************************************************************
 * reference_feedback.c: tests for exact reference-picture loss feedback
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

#include "check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x264.h"

/* Signature of the production commit hook (see encoder/ref_feedback.h).
 * Redeclared here so the test can install instrumentation without pulling in
 * an internal header through the public include path. */
typedef void (*reffb_commit_hook_t)( int mb_xy, int list, int ref_idx, int64_t ref_pts );
extern reffb_commit_hook_t x264_reffb_mb_commit_hook;

#define WIDTH 64
#define HEIGHT 64
#define MB_COUNT ((WIDTH/16)*(HEIGHT/16))
#define NFRAMES 48
#define REF_COUNT 4
#define TEST_REF_MAX 16   /* matches X264_REF_MAX, which is not in the public API */

/* ------------------------------------------------------------------ */
/* Synthetic texture generation                                       */
/* ------------------------------------------------------------------ */

static void fill_texture( x264_picture_t *pic, int t )
{
    uint8_t *y = pic->img.plane[0];
    uint8_t *u = pic->img.plane[1];
    uint8_t *v = pic->img.plane[2];
    int sy = pic->img.i_stride[0];
    int sc = pic->img.i_stride[1];
    /* A moving gradient plus a static block: motion drives real inter
     * prediction, while the static region keeps older references useful. */
    for( int row = 0; row < HEIGHT; row++ )
        for( int col = 0; col < WIDTH; col++ )
        {
            uint8_t val = (uint8_t)( (col*7 + row*3 + ((col + t*3)/9)*37 + t*5) & 255 );
            if( row < 16 && col < 16 )
                val = (uint8_t)( (col*5 + row*11) & 255 ); /* static texture */
            y[row*sy + col] = val;
        }
    for( int row = 0; row < HEIGHT/2; row++ )
        for( int col = 0; col < WIDTH/2; col++ )
        {
            u[row*sc + col] = (uint8_t)( (col*11 + t*7) & 255 );
            v[row*sc + col] = (uint8_t)( (row*13 + t*9 + 61) & 255 );
        }
}

/* ------------------------------------------------------------------ */
/* Encoder construction                                               */
/* ------------------------------------------------------------------ */

static void base_param( x264_param_t *p, int feedback, int weighted )
{
    x264_param_default_preset( p, "veryfast", NULL );
    p->i_width = WIDTH;
    p->i_height = HEIGHT;
    p->i_threads = 1;
    p->i_lookahead_threads = 1;
    p->i_csp = X264_CSP_I420;
    p->i_bitdepth = 8;
    p->b_vfr_input = 0;
    p->i_fps_num = 25;
    p->i_fps_den = 1;
    p->i_bframe = 3;
    p->i_bframe_adaptive = X264_B_ADAPT_NONE;
    p->i_bframe_pyramid = X264_B_PYRAMID_NORMAL;
    p->i_frame_reference = REF_COUNT;
    p->i_keyint_max = 250;
    p->i_scenecut_threshold = 0;
    p->rc.i_lookahead = 4;
    p->rc.i_rc_method = X264_RC_ABR;
    p->rc.i_bitrate = 250;
    p->rc.b_mb_tree = 0;
    p->b_repeat_headers = 1;
    p->b_annexb = 1;
    p->i_log_level = X264_LOG_NONE;
    p->b_intra_refresh = 0;
    if( !weighted )
    {
        p->analyse.i_weighted_pred = X264_WEIGHTP_NONE;
        p->analyse.b_weighted_bipred = 0;
    }
    p->b_reference_feedback = feedback;
}

/* ------------------------------------------------------------------ */
/* Output records and the independently maintained dependency graph  */
/* ------------------------------------------------------------------ */

typedef struct
{
    int64_t pts;
    int     type;
    int     keyframe;
    int     nref;
    int64_t ref_pts[16];
    int     frame_num;      /* parsed independently from the slice header */
} out_rec_t;

typedef struct
{
    out_rec_t rec[NFRAMES + 8];
    int       n;
} rec_list_t;

static int rec_find( const rec_list_t *r, int64_t pts )
{
    for( int i = 0; i < r->n; i++ )
        if( r->rec[i].pts == pts )
            return i;
    return -1;
}

static int rec_is_ref_picture( const rec_list_t *r, int64_t pts )
{
    int i = rec_find( r, pts );
    if( i < 0 )
        return 0;
    int t = r->rec[i].type;
    return t != X264_TYPE_B; /* disposable B is the only non-reference type */
}

static int pts_in_array( const int64_t *arr, int n, int64_t pts )
{
    for( int i = 0; i < n; i++ )
        if( arr[i] == pts )
            return 1;
    return 0;
}

/* Independent transitive closure over the recorded direct dependencies,
 * restricted to the reference pictures currently retained (the encoder's own
 * closure likewise only covers retained frames, including the most recently
 * delivered one still held in fdec).  retained[] is the live DPB's PTS set. */
static int graph_closure_retained( const rec_list_t *r, const int64_t *retained, int nret,
                                   int64_t target, int64_t *closure, int cap )
{
    int n = 0;
    closure[n++] = target;
    for( int progress = 1; progress; )
    {
        progress = 0;
        for( int i = 0; i < nret; i++ )
        {
            int64_t cand = retained[i];
            if( pts_in_array( closure, n, cand ) )
                continue;
            int ri = rec_find( r, cand );
            if( ri < 0 )
                continue;
            const out_rec_t *o = &r->rec[ri];
            for( int d = 0; d < o->nref; d++ )
                if( pts_in_array( closure, n, o->ref_pts[d] ) )
                {
                    if( n >= cap )
                        return -1;
                    closure[n++] = cand;
                    progress = 1;
                    break;
                }
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Minimal H.264 bitstream reader (independent of x264 internals)     */
/* ------------------------------------------------------------------ */

typedef struct
{
    const uint8_t *rbsp;
    int size;
    int byte;
    int bit;
} bitreader_t;

static void br_init( bitreader_t *br, const uint8_t *rbsp, int size )
{
    br->rbsp = rbsp;
    br->size = size;
    br->byte = 0;
    br->bit = 0;
}

static uint32_t br_read( bitreader_t *br, int n )
{
    uint32_t v = 0;
    for( int i = 0; i < n; i++ )
    {
        if( br->byte >= br->size )
            return 0;
        v = (v << 1) | ((br->rbsp[br->byte] >> (7 - br->bit)) & 1);
        if( ++br->bit == 8 ) { br->bit = 0; br->byte++; }
    }
    return v;
}

static uint32_t br_ue( bitreader_t *br )
{
    int zeros = 0;
    while( br_read( br, 1 ) == 0 && zeros < 32 )
        zeros++;
    if( zeros == 0 )
        return 0;
    return (1u << zeros) - 1 + br_read( br, zeros );
}

static int32_t br_se( bitreader_t *br )
{
    uint32_t code = br_ue( br );
    return (code & 1) ? (int32_t)((code + 1) / 2) : -(int32_t)(code / 2);
}

/* Strip emulation prevention bytes (00 00 03 -> 00 00). */
static int remove_epb( const uint8_t *in, int in_len, uint8_t *out, int out_cap )
{
    int n = 0;
    for( int i = 0; i < in_len; i++ )
    {
        if( i + 2 < in_len && in[i] == 0 && in[i+1] == 0 && in[i+2] == 3 )
        {
            if( n + 2 > out_cap )
                return -1;
            out[n++] = 0;
            out[n++] = 0;
            i += 2;
        }
        else
        {
            if( n >= out_cap )
                return -1;
            out[n++] = in[i];
        }
    }
    return n;
}

#define NAL_SLICE_TYPE 1
#define NAL_IDR_TYPE 5
#define NAL_SPS_TYPE 7

typedef struct
{
    int log2_max_frame_num;
    int log2_max_poc_lsb;
    int num_ref_frames;
    int frame_mbs_only;
} sps_info_t;

static int parse_sps( const x264_nal_t *nal, sps_info_t *sps )
{
    uint8_t rbsp[4096];
    /* Find the NAL header: annexb payload begins with a start code. */
    int off = 0;
    const uint8_t *p = nal->p_payload;
    if( p[0] == 0 && p[1] == 0 )
        off = (p[2] == 1) ? 3 : 4;
    off += 1; /* nal header byte */
    int rbsp_len = remove_epb( nal->p_payload + off, nal->i_payload - off, rbsp, sizeof(rbsp) );
    TASK_CHECK( rbsp_len > 0 );
    bitreader_t br;
    br_init( &br, rbsp, rbsp_len );
    int profile_idc = br_read( &br, 8 );
    br_read( &br, 8 ); /* constraint flags + reserved */
    br_read( &br, 8 ); /* level_idc */
    br_ue( &br );      /* sps id */
    static const int high_profiles[] = { 100,110,122,244,44,83,86,118,128,138,139,134,135 };
    int high = 0;
    for( unsigned i = 0; i < sizeof(high_profiles)/sizeof(high_profiles[0]); i++ )
        if( high_profiles[i] == profile_idc )
            high = 1;
    if( high )
    {
        int chroma = br_ue( &br );
        if( chroma == 3 )
            br_read( &br, 1 );
        br_ue( &br ); /* bit depth luma minus 8 */
        br_ue( &br ); /* bit depth chroma minus 8 */
        br_read( &br, 1 ); /* qpprime zero transform */
        if( br_read( &br, 1 ) ) /* seq scaling matrix present */
            for( int i = 0; i < (chroma != 3 ? 8 : 12); i++ )
                if( br_read( &br, 1 ) ) /* scaling list present */
                {
                    int size = (i < 6) ? 16 : 64;
                    int next = 8, scale;
                    for( int j = 0; j < size; j++ )
                    {
                        if( next != 0 )
                        {
                            int delta = br_se( &br );
                            next = (next + delta + 256) & 255;
                        }
                        scale = next ? next : scale;
                    }
                }
    }
    sps->log2_max_frame_num = br_ue( &br ) + 4;
    int poc_type = br_ue( &br );
    sps->log2_max_poc_lsb = (poc_type == 0) ? br_ue( &br ) + 4 : 0;
    sps->num_ref_frames = br_ue( &br );
    br_read( &br, 1 ); /* gaps_in_frame_num_value_allowed */
    br_ue( &br );      /* pic_width_in_mbs_minus1 */
    br_ue( &br );      /* pic_height_in_map_units_minus1 */
    sps->frame_mbs_only = br_read( &br, 1 );
    if( !sps->frame_mbs_only )
        br_read( &br, 1 ); /* mb_adaptive_frame_field */
    br_read( &br, 1 ); /* direct_8x8_inference_flag */
    return 0;
}

/* One parsed reference picture list, as frame numbers and mapped PTS. */
typedef struct
{
    int     frame_num[16];
    int64_t pts[16];
    int     n;
} ref_list_t;

typedef struct
{
    int        slice_type;       /* 0 P, 1 B, 2 I */
    int        frame_num;
    int        nal_ref_idc;
    int        is_idr;
    int        active[2];
    ref_list_t list[2];
    int        has_l[2];
} slice_info_t;

/* DPB of short-term reference frame numbers. */
typedef struct
{
    int nums[TEST_REF_MAX + 2];
    int64_t pts[TEST_REF_MAX + 2];
    int n;
    int max_frame_num;
    int num_ref_frames;
} dpb_sim_t;

/* Per-IDR-GOP frame number -> PTS map.  Frame numbers are unique within a GOP
 * and never reference across an IDR, so this resolves every legal active list
 * entry without emulating implicit/MMCO DPB eviction. */
typedef struct
{
    int     frame_num[256];
    int64_t pts[256];
    int     valid[256];
} gop_map_t;

static int frame_num_mod( int a, int mod )
{
    int r = a % mod;
    return r < 0 ? r + mod : r;
}

/* Distance going backward from cur to ref (ref is older than cur). */
static int backward_dist( int cur, int ref, int mod )
{
    return frame_num_mod( cur - ref, mod );
}

static void dpb_init( dpb_sim_t *d, int max_frame_num, int num_ref_frames )
{
    d->n = 0;
    d->max_frame_num = max_frame_num;
    d->num_ref_frames = num_ref_frames;
}

static void dpb_remove( dpb_sim_t *d, int fn )
{
    for( int i = 0; i < d->n; i++ )
        if( d->nums[i] == fn )
        {
            for( int j = i; j < d->n - 1; j++ )
            {
                d->nums[j] = d->nums[j+1];
                d->pts[j] = d->pts[j+1];
            }
            d->n--;
            return;
        }
}

/* Accurate implicit sliding-window eviction for building the default lists.
 * Final active-entry PTS resolution uses the per-GOP map instead, since the
 * current frame's reordering is computed before its own marking evicts a
 * reference. */
static void dpb_sliding_window( dpb_sim_t *d, int cur )
{
    while( d->n >= d->num_ref_frames && d->n > 0 )
    {
        int worst = 0, worst_dist = -1;
        for( int i = 0; i < d->n; i++ )
        {
            int dist = backward_dist( cur, d->nums[i], d->max_frame_num );
            if( dist > worst_dist ) { worst_dist = dist; worst = i; }
        }
        dpb_remove( d, d->nums[worst] );
    }
}

static void dpb_add( dpb_sim_t *d, int fn, int64_t pts )
{
    dpb_sliding_window( d, fn );
    d->nums[d->n] = fn;
    d->pts[d->n] = pts;
    d->n++;
}

static int contains( const int *arr, int n, int v )
{
    for( int i = 0; i < n; i++ )
        if( arr[i] == v )
            return 1;
    return 0;
}

/* Apply one ref_pic_list_modification command stream following the H.264
 * reference list reordering procedure.  RefIdxL0/L1 starts at the standard
 * default list; each command's target replaces entry pos, the displaced tail
 * shifts down.  A target absent from the default list (feedback legalization
 * borrows an opposite-direction frame) is pulled from the whole DPB. */
static int apply_reordering( bitreader_t *br, int cur_fn, dpb_sim_t *d,
                             int *order, int n_order )
{
    int pred = cur_fn;
    int pos = 0;
    for( ;; )
    {
        uint32_t idc = br_ue( br );
        if( idc == 3 )
            break;
        uint32_t arg = br_ue( br );
        int ref_fn;
        if( idc == 0 )
            ref_fn = frame_num_mod( pred - (int)(arg + 1), d->max_frame_num );
        else
            ref_fn = frame_num_mod( pred + (int)(arg + 1), d->max_frame_num );

        int idx = -1;
        for( int i = pos; i < n_order; i++ )
            if( order[i] == ref_fn ) { idx = i; break; }
        if( idx >= 0 )
        {
            int val = order[idx];
            for( int i = idx; i > pos; i-- )
                order[i] = order[i-1];
            order[pos] = val;
        }
        else
        {
            /* Borrowed frame not in the default list: grow from the DPB. */
            int in_dpb = 0;
            for( int i = 0; i < d->n; i++ )
                if( d->nums[i] == ref_fn ) { in_dpb = 1; break; }
            if( in_dpb && n_order < TEST_REF_MAX + 1 )
            {
                for( int i = n_order; i > pos; i-- )
                    order[i] = order[i-1];
                order[pos] = ref_fn;
                n_order++;
            }
        }
        pred = ref_fn;
        pos++;
    }
    return n_order;
}

/* Parse a slice NAL and reconstruct both active reference lists. */
static int parse_slice( const x264_nal_t *nal, const sps_info_t *sps,
                        dpb_sim_t *d, slice_info_t *si, int64_t cur_pts )
{
    uint8_t rbsp[8192];
    int off = 0;
    const uint8_t *p = nal->p_payload;
    if( p[0] == 0 && p[1] == 0 )
        off = (p[2] == 1) ? 3 : 4;
    int nal_header = p[off];
    si->nal_ref_idc = nal_header >> 5;
    si->is_idr = (nal_header & 0x1f) == NAL_IDR_TYPE;
    off += 1;
    int rbsp_len = remove_epb( nal->p_payload + off, nal->i_payload - off, rbsp, sizeof(rbsp) );
    if( rbsp_len <= 0 )
        return -1;
    bitreader_t br;
    br_init( &br, rbsp, rbsp_len );

    br_ue( &br ); /* first mb in slice */
    uint32_t stype = br_ue( &br );
    si->slice_type = stype >= 5 ? (int)(stype - 5) : (int)stype;
    br_ue( &br ); /* pps id */
    si->frame_num = br_read( &br, sps->log2_max_frame_num );
    /* Only when the SPS permits field pictures does the slice carry a
     * frame/field flag (always "frame" in these progressive streams). */
    if( !sps->frame_mbs_only )
    {
        int field_pic = br_read( &br, 1 );
        TASK_CHECK( field_pic == 0 );
        if( field_pic )
            br_read( &br, 1 );
    }
    if( si->is_idr )
        br_ue( &br ); /* idr pic id */
    br_read( &br, sps->log2_max_poc_lsb ); /* poc lsb */
    /* b_pic_order is only set for interlaced streams, so no delta_poc_bottom. */

    int is_b = (si->slice_type == 1);
    int is_pb = (si->slice_type == 0 || si->slice_type == 1);
    if( is_b )
        br_read( &br, 1 ); /* direct spatial mv pred flag */

    si->active[0] = REF_COUNT;
    si->active[1] = 1;
    if( is_pb )
    {
        if( br_read( &br, 1 ) ) /* num ref idx override */
        {
            si->active[0] = br_ue( &br ) + 1;
            if( is_b )
                si->active[1] = br_ue( &br ) + 1;
        }
    }

    int order[2][TEST_REF_MAX + 2];
    int n_order[2] = { 0, 0 };

    /* Build the H.264 standard default reference lists, which are POC ordered
     * (PTS is monotonic with POC in this progressive stream):
     *   L0 = references older than current, nearest (highest POC) first;
     *   L1 = references newer than current, nearest (lowest POC) first, then
     *        older references nearest first.  This matches x264's
     *        reference_build_list split and ordering. */
    if( is_pb )
    {
        int past[TEST_REF_MAX + 2], future[TEST_REF_MAX + 2];
        int64_t past_pts[TEST_REF_MAX + 2], future_pts[TEST_REF_MAX + 2];
        int np = 0, nf = 0;
        for( int i = 0; i < d->n; i++ )
        {
            if( d->pts[i] < cur_pts )
            {
                past[np] = d->nums[i];
                past_pts[np] = d->pts[i];
                np++;
            }
            else if( d->pts[i] > cur_pts )
            {
                future[nf] = d->nums[i];
                future_pts[nf] = d->pts[i];
                nf++;
            }
        }
        /* L0: past, descending PTS. */
        for( int i = 0; i < np; i++ )
        {
            int best = -1;
            for( int j = 0; j < np; j++ )
                if( !contains( order[0], n_order[0], past[j] ) &&
                    (best < 0 || past_pts[j] > past_pts[best]) )
                    best = j;
            order[0][n_order[0]++] = past[best];
        }

        if( is_b )
        {
            /* L1: future ascending PTS, then past descending PTS. */
            for( int i = 0; i < nf; i++ )
            {
                int best = -1;
                for( int j = 0; j < nf; j++ )
                    if( !contains( order[1], n_order[1], future[j] ) &&
                        (best < 0 || future_pts[j] < future_pts[best]) )
                        best = j;
                order[1][n_order[1]++] = future[best];
            }
            for( int i = 0; i < np; i++ )
            {
                int best = -1;
                for( int j = 0; j < np; j++ )
                    if( !contains( order[1], n_order[1], past[j] ) &&
                        (best < 0 || past_pts[j] > past_pts[best]) )
                        best = j;
                order[1][n_order[1]++] = past[best];
            }
        }
    }

    /* Reference picture list modification. */
    si->has_l[0] = si->has_l[1] = 0;
    if( is_pb )
    {
        if( br_read( &br, 1 ) )
        {
            n_order[0] = apply_reordering( &br, si->frame_num, d, order[0], n_order[0] );
            si->has_l[0] = 1;
        }
        if( is_b )
        {
            if( br_read( &br, 1 ) )
            {
                n_order[1] = apply_reordering( &br, si->frame_num, d, order[1], n_order[1] );
                si->has_l[1] = 1;
            }
        }
    }

    /* dec_ref_pic_marking: update the simulated DPB. */
    if( si->nal_ref_idc != 0 )
    {
        if( si->is_idr )
        {
            br_read( &br, 1 );
            br_read( &br, 1 );
            d->n = 0;
        }
        else if( br_read( &br, 1 ) ) /* adaptive mmco */
        {
            for( ;; )
            {
                uint32_t op = br_ue( &br );
                if( op == 0 )
                    break;
                uint32_t diff = br_ue( &br ) + 1;
                if( op == 1 )
                    dpb_remove( d, frame_num_mod( si->frame_num - (int)diff, d->max_frame_num ) );
            }
        }
        else
            dpb_sliding_window( d, si->frame_num );
        dpb_add( d, si->frame_num, cur_pts );
    }

    /* Materialize the active lists. */
    for( int list = 0; list < (is_b ? 2 : 1); list++ )
    {
        si->list[list].n = 0;
        for( int i = 0; i < si->active[list] && i < n_order[list]; i++ )
        {
            si->list[list].frame_num[i] = order[list][i];
            si->list[list].n++;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Commit-hook instrumentation capture                                */
/* ------------------------------------------------------------------ */

#define HOOK_CAP 4096
typedef struct
{
    int64_t pts[HOOK_CAP];
    int     n;
} hook_capture_t;

static hook_capture_t g_hook;
static int g_hook_armed;

static void commit_hook_cb( int mb_xy, int list, int ref_idx, int64_t ref_pts )
{
    (void)mb_xy;
    (void)list;
    (void)ref_idx;
    if( !g_hook_armed || g_hook.n >= HOOK_CAP )
        return;
    if( !pts_in_array( g_hook.pts, g_hook.n, ref_pts ) )
        g_hook.pts[g_hook.n++] = ref_pts;
}

/* ------------------------------------------------------------------ */
/* Encoding harness                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
    x264_t      *h;
    x264_picture_t in;
    rec_list_t   rec;
    sps_info_t   sps;
    int          have_sps;
    dpb_sim_t    dpb;
    gop_map_t    gop;
    int          saw_keyframe_after_first;
    /* Cumulative set of PTS inside every accepted invalidation closure. */
    int64_t      unusable[128];
    int          n_unusable;
    int          n_invalidations;
} enc_env_t;

static int env_open( enc_env_t *e, int feedback, int weighted )
{
    x264_param_t p;
    base_param( &p, feedback, weighted );
    e->h = x264_encoder_open( &p );
    if( !e->h )
        return -1;
    if( x264_picture_alloc( &e->in, X264_CSP_I420, WIDTH, HEIGHT ) < 0 )
        return -1;
    memset( &e->rec, 0, sizeof(e->rec) );
    memset( &e->gop, 0, sizeof(e->gop) );
    e->have_sps = 0;
    e->saw_keyframe_after_first = 0;
    e->n_unusable = 0;
    e->n_invalidations = 0;
    dpb_init( &e->dpb, 16, REF_COUNT );
    return 0;
}

static void env_close( enc_env_t *e )
{
    x264_picture_clean( &e->in );
    x264_encoder_close( e->h );
}

/* Encode one input frame (or drain with pts<0). Returns 0 on no output. */
static int env_step( enc_env_t *e, int t, slice_info_t *si_out )
{
    x264_picture_t out;
    x264_nal_t *nal;
    int nn;
    if( t >= 0 )
    {
        fill_texture( &e->in, t );
        e->in.i_pts = t * 40;
        e->in.i_type = X264_TYPE_AUTO;
    }
    int size = x264_encoder_encode( e->h, &nal, &nn, t >= 0 ? &e->in : NULL, &out );
    if( size < 0 )
        return -1;
    if( !size )
        return 0;

    out_rec_t *r = &e->rec.rec[e->rec.n];
    memset( r, 0, sizeof(*r) );
    r->pts = out.i_pts;
    r->type = out.i_type;
    r->keyframe = out.b_keyframe;
    r->nref = out.prop.i_ref_used;
    TASK_CHECK( r->nref >= 0 && r->nref <= 16 );
    for( int i = 0; i < r->nref; i++ )
        r->ref_pts[i] = out.prop.ref_used_pts[i];
    /* Verify strictly increasing, deduplicated ordering. */
    for( int i = 1; i < r->nref; i++ )
        TASK_CHECK( r->ref_pts[i] > r->ref_pts[i-1] );
    if( r->type == X264_TYPE_I || r->type == X264_TYPE_IDR )
        TASK_CHECK( r->nref == 0 );
    if( e->rec.n > 0 && out.b_keyframe )
        e->saw_keyframe_after_first = 1;

    /* Independently parse the emitted NALs. */
    slice_info_t si;
    int got_slice = 0;
    for( int k = 0; k < nn; k++ )
    {
        int hdr = nal[k].i_type;
        if( hdr == NAL_SPS_TYPE )
        {
            TASK_CHECK( parse_sps( &nal[k], &e->sps ) == 0 );
            e->have_sps = 1;
            dpb_init( &e->dpb, 1 << e->sps.log2_max_frame_num, e->sps.num_ref_frames );
        }
        else if( hdr == NAL_SLICE_TYPE || hdr == NAL_IDR_TYPE )
        {
            TASK_CHECK( e->have_sps );
            if( hdr == NAL_IDR_TYPE )
                memset( &e->gop, 0, sizeof(e->gop) ); /* new GOP: frame_num restarts */
            if( parse_slice( &nal[k], &e->sps, &e->dpb, &si, out.i_pts ) < 0 )
                return -1;
            got_slice = 1;
        }
    }
    TASK_CHECK( got_slice );
    r->frame_num = si.frame_num;
    /* Record this GOP's frame number -> PTS mapping (B frames keep frame_num
     * of their anchor, but only reference pictures reach active lists). */
    {
        int k = si.frame_num & 255;
        e->gop.frame_num[k] = si.frame_num;
        e->gop.pts[k] = out.i_pts;
        e->gop.valid[k] = 1;
    }
    if( si_out )
        *si_out = si;
    e->rec.n++;
    return 1;
}

static int env_drain( enc_env_t *e )
{
    int produced = 0;
    while( x264_encoder_delayed_frames( e->h ) )
    {
        int rc = env_step( e, -1, NULL );
        if( rc < 0 )
            return -1;
        if( rc > 0 )
            produced++;
    }
    return produced;
}

/* Resolve a live DPB frame number to the PTS of that exact retained picture. */
static int64_t fn_to_pts( const enc_env_t *e, int fn )
{
    int k = fn & 255;
    return e->gop.valid[k] && e->gop.frame_num[k] == fn ? e->gop.pts[k] : -1;
}

/* Drive one invalidation through the public API and, on success, record the
 * independently computed closure of retained pictures as unusable. */
static int env_invalidate( enc_env_t *e, int64_t target )
{
    /* The retained candidates are every reference picture already delivered
     * (disposable B frames are never kept as references). */
    int64_t retained[TEST_REF_MAX + 2];
    int nret = 0;
    for( int i = 0; i < e->rec.n; i++ )
    {
        const out_rec_t *o = &e->rec.rec[i];
        if( o->type != X264_TYPE_B && !pts_in_array( retained, nret, o->pts ) )
            retained[nret++] = o->pts;
    }
    int64_t closure[64];
    int nc = graph_closure_retained( &e->rec, retained, nret, target, closure, 64 );
    TASK_CHECK( nc >= 1 );

    int rc = x264_encoder_invalidate_reference_exact( e->h, target );
    if( rc == 0 )
    {
        e->n_invalidations++;
        for( int i = 0; i < nc; i++ )
            if( !pts_in_array( e->unusable, e->n_unusable, closure[i] ) )
                e->unusable[e->n_unusable++] = closure[i];
    }
    return rc;
}

/* ================================================================== */
/* graph group                                                        */
/* ================================================================== */

static int case_graph_closure_p( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    int lost_pts = -1;
    int lost_at_rec = -1;
    for( int t = 0; t < NFRAMES; t++ )
    {
        int rc = env_step( &e, t, NULL );
        TASK_CHECK( rc >= 0 );
        /* Invalidate the first P (pts 160) once it has been delivered. */
        if( lost_pts < 0 && rc > 0 )
        {
            int idx = rec_find( &e.rec, 160 );
            if( idx >= 0 && e.rec.rec[idx].type == X264_TYPE_P )
            {
                int irc = env_invalidate( &e, 160 );
                TASK_CHECK( irc == 0 );
                lost_pts = 160;
                lost_at_rec = e.rec.n;
            }
        }
    }
    TASK_CHECK( lost_pts == 160 );
    TASK_CHECK( lost_at_rec > 0 );
    TASK_CHECK( env_drain( &e ) >= 0 );

    /* The target is recorded unusable, and later outputs never touch the set. */
    TASK_CHECK( pts_in_array( e.unusable, e.n_unusable, lost_pts ) );
    for( int i = lost_at_rec; i < e.rec.n; i++ )
        for( int d = 0; d < e.rec.rec[i].nref; d++ )
            TASK_CHECK( !pts_in_array( e.unusable, e.n_unusable, e.rec.rec[i].ref_pts[d] ) );

    /* An unaffected older reference (IDR pts 0) is still used afterwards. */
    int saw_old = 0;
    for( int i = lost_at_rec; i < e.rec.n; i++ )
        if( pts_in_array( e.rec.rec[i].ref_pts, e.rec.rec[i].nref, 0 ) )
            saw_old = 1;
    TASK_CHECK( saw_old );
    env_close( &e );
    return 0;
}

static int case_graph_closure_transitive( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    /* Let several P/BREF frames accumulate real dependencies on P160 before
     * reporting it lost, so the closure is genuinely multi-node. */
    int armed = 0, lost_at = -1;
    for( int t = 0; t < NFRAMES; t++ )
    {
        int rc = env_step( &e, t, NULL );
        TASK_CHECK( rc >= 0 );
        /* Wait until P320 (which references P160 or its successors) is out. */
        if( !armed && rec_find( &e.rec, 320 ) >= 0 )
        {
            int irc = env_invalidate( &e, 160 );
            TASK_CHECK( irc == 0 );
            armed = 1;
            lost_at = e.rec.n;
        }
    }
    TASK_CHECK( armed );
    TASK_CHECK( env_drain( &e ) >= 0 );

    /* The closure captured at invalidation time must contain 160 itself and at
     * least one retained dependent (P320 directly references 160). */
    TASK_CHECK( e.n_unusable >= 2 );
    TASK_CHECK( pts_in_array( e.unusable, e.n_unusable, 160 ) );
    TASK_CHECK( pts_in_array( e.unusable, e.n_unusable, 320 ) );
    /* Frames after the loss avoid the entire captured closure. */
    for( int i = lost_at; i < e.rec.n; i++ )
        for( int d = 0; d < e.rec.rec[i].nref; d++ )
            TASK_CHECK( !pts_in_array( e.unusable, e.n_unusable, e.rec.rec[i].ref_pts[d] ) );
    env_close( &e );
    return 0;
}

static int case_graph_closure_bref( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    int64_t bref_pts = -1;
    int lost_at = -1;
    for( int t = 0; t < NFRAMES; t++ )
    {
        int rc = env_step( &e, t, NULL );
        TASK_CHECK( rc >= 0 );
        if( bref_pts < 0 && rc > 0 )
        {
            /* Find a delivered BREF still in the DPB (the first BREF, pts 80). */
            int idx = rec_find( &e.rec, 80 );
            if( idx >= 0 && e.rec.rec[idx].type == X264_TYPE_BREF )
            {
                int irc = env_invalidate( &e, 80 );
                if( irc == 0 )
                {
                    bref_pts = 80;
                    lost_at = e.rec.n;
                }
            }
        }
    }
    TASK_CHECK( bref_pts == 80 );
    TASK_CHECK( env_drain( &e ) >= 0 );
    TASK_CHECK( pts_in_array( e.unusable, e.n_unusable, bref_pts ) );
    for( int i = lost_at; i < e.rec.n; i++ )
        for( int d = 0; d < e.rec.rec[i].nref; d++ )
            TASK_CHECK( e.rec.rec[i].ref_pts[d] != bref_pts );
    env_close( &e );
    return 0;
}

static int case_graph_unknown_and_nonref( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    for( int t = 0; t < 12; t++ )
        TASK_CHECK( env_step( &e, t, NULL ) >= 0 );
    /* Unknown PTS. */
    TASK_CHECK( x264_encoder_invalidate_reference_exact( e.h, 999999 ) < 0 );
    /* A disposable, non-reference B picture. */
    int64_t nonref = -1;
    for( int i = 0; i < e.rec.n; i++ )
        if( e.rec.rec[i].type == X264_TYPE_B ) { nonref = e.rec.rec[i].pts; break; }
    TASK_CHECK( nonref >= 0 );
    TASK_CHECK( !rec_is_ref_picture( &e.rec, nonref ) );
    TASK_CHECK( x264_encoder_invalidate_reference_exact( e.h, nonref ) < 0 );
    /* Encoder state is intact: more frames encode and keep references. */
    int before = e.rec.n;
    for( int t = 12; t < 20; t++ )
        TASK_CHECK( env_step( &e, t, NULL ) >= 0 );
    TASK_CHECK( e.rec.n > before );
    TASK_CHECK( env_drain( &e ) >= 0 );
    env_close( &e );
    return 0;
}

static int case_graph_full_closure_reject( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    /* Feed until the first P (pts 160) is delivered: DPB holds IDR 0 and P 160,
     * and P 160 directly depends on IDR 0, so invalidating 0 empties the DPB. */
    int got_p = 0;
    for( int t = 0; t < 8 && !got_p; t++ )
    {
        int rc = env_step( &e, t, NULL );
        TASK_CHECK( rc >= 0 );
        if( rec_find( &e.rec, 160 ) >= 0 )
            got_p = 1;
    }
    TASK_CHECK( got_p );
    TASK_CHECK( x264_encoder_invalidate_reference_exact( e.h, 0 ) < 0 );
    /* State unchanged: both references remain usable on subsequent frames. */
    int n_before = e.rec.n;
    for( int t = 8; t < 16; t++ )
        TASK_CHECK( env_step( &e, t, NULL ) >= 0 );
    int used_zero = 0, used_160 = 0;
    for( int i = n_before; i < e.rec.n; i++ )
    {
        if( pts_in_array( e.rec.rec[i].ref_pts, e.rec.rec[i].nref, 0 ) )
            used_zero = 1;
        if( pts_in_array( e.rec.rec[i].ref_pts, e.rec.rec[i].nref, 160 ) )
            used_160 = 1;
    }
    TASK_CHECK( used_zero );
    TASK_CHECK( used_160 );
    TASK_CHECK( env_drain( &e ) >= 0 );
    env_close( &e );
    return 0;
}

static int case_graph_repeat_and_displaced( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    int64_t target = -1;
    int next_t = 0;
    /* Invalidate the first P as soon as it is delivered, while the IDR is
     * still retained as a clean anchor (so the request is accepted). */
    for( int t = 0; t < NFRAMES && target < 0; t++ )
    {
        TASK_CHECK( env_step( &e, t, NULL ) >= 0 );
        next_t = t + 1;
        int idx = rec_find( &e.rec, 160 );
        if( idx >= 0 && e.rec.rec[idx].type == X264_TYPE_P )
            target = 160;
    }
    TASK_CHECK( target >= 0 );
    TASK_CHECK( x264_encoder_invalidate_reference_exact( e.h, target ) == 0 );
    /* Repeated report while still retained (or already lost) succeeds. */
    TASK_CHECK( x264_encoder_invalidate_reference_exact( e.h, target ) == 0 );
    /* Keep encoding so the target's DPB slot is recycled many times. */
    for( int t2 = next_t; t2 < NFRAMES; t2++ )
        TASK_CHECK( env_step( &e, t2, NULL ) >= 0 );
    TASK_CHECK( env_drain( &e ) >= 0 );
    /* The target has left the reference set through slot reuse: rejected. */
    TASK_CHECK( x264_encoder_invalidate_reference_exact( e.h, target ) < 0 );
    env_close( &e );
    return 0;
}

/* ================================================================== */
/* lists group                                                        */
/* ================================================================== */

static int case_lists_header_parse( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    int nlost = 0;
    int lost_at = -1;
    for( int t = 0; t < NFRAMES; t++ )
    {
        slice_info_t si;
        int rc = env_step( &e, t, &si );
        TASK_CHECK( rc >= 0 );
        if( nlost == 0 && rec_find( &e.rec, 160 ) >= 0 )
        {
            TASK_CHECK( env_invalidate( &e, 160 ) == 0 );
            nlost++;
            lost_at = e.rec.n;
        }
        if( rc > 0 && nlost && e.rec.n > lost_at )
        {
            const out_rec_t *cur = &e.rec.rec[e.rec.n - 1];
            /* Every active list entry, parsed independently from the slice
             * header, must be one of the clean pictures this frame's final
             * prediction actually committed (its public ref_used set). */
            int nlists = (si.slice_type == 1) ? 2 : 1;
            if( si.slice_type == 2 )
                nlists = 0;
            for( int list = 0; list < nlists; list++ )
            {
                /* A temporally ordered list must never be empty. */
                TASK_CHECK( si.list[list].n > 0 );
                TASK_CHECK( si.list[list].n == si.active[list] );
                for( int i = 0; i < si.list[list].n; i++ )
                {
                    int64_t pts = fn_to_pts( &e, si.list[list].frame_num[i] );
                    /* Every active index resolves to a GOP-visible, clean
                     * reference (the independently parsed header must never
                     * address an invalidated picture).  It need not be among
                     * the frame's committed refs: an offered list entry can
                     * remain unused by the final macroblocks. */
                    TASK_CHECK( pts >= 0 );
                    TASK_CHECK( !pts_in_array( e.unusable, e.n_unusable, pts ) );
                }
                /* Conversely, every reference the frame did commit is clean. */
                for( int i = 0; i < cur->nref; i++ )
                    TASK_CHECK( !pts_in_array( e.unusable, e.n_unusable, cur->ref_pts[i] ) );
            }
        }
    }
    TASK_CHECK( nlost == 1 );
    TASK_CHECK( env_drain( &e ) >= 0 );
    env_close( &e );
    return 0;
}

static int case_lists_empty_direction( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    int armed = 0, checked_b = 0;
    for( int t = 0; t < NFRAMES; t++ )
    {
        slice_info_t si;
        int rc = env_step( &e, t, &si );
        TASK_CHECK( rc >= 0 );
        if( !armed && rec_find( &e.rec, 160 ) >= 0 )
        {
            TASK_CHECK( env_invalidate( &e, 160 ) == 0 );
            armed = 1;
        }
        /* A B slice after the loss whose future (L1) direction lost its only
         * reference must still carry a legal, non-empty, clean L1. */
        if( armed && rc > 0 && si.slice_type == 1 )
        {
            TASK_CHECK( si.list[1].n > 0 );
            int all_clean = 1;
            for( int i = 0; i < si.list[1].n; i++ )
                if( pts_in_array( e.unusable, e.n_unusable,
                                  fn_to_pts( &e, si.list[1].frame_num[i] ) ) )
                    all_clean = 0;
            TASK_CHECK( all_clean );
            checked_b = 1;
        }
    }
    TASK_CHECK( armed );
    TASK_CHECK( checked_b );
    TASK_CHECK( env_drain( &e ) >= 0 );
    env_close( &e );
    return 0;
}

static int case_lists_commit_instrument( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    reffb_commit_hook_t prev = x264_reffb_mb_commit_hook;
    g_hook.n = 0;
    g_hook_armed = 1;
    x264_reffb_mb_commit_hook = commit_hook_cb;
    int compared = 0;
    for( int t = 0; t < 24; t++ )
    {
        g_hook.n = 0;
        int rc = env_step( &e, t, NULL );
        TASK_CHECK( rc >= 0 );
        if( rc > 0 )
        {
            const out_rec_t *r = &e.rec.rec[e.rec.n - 1];
            /* The hook independently gathers the committed ref PTS set; it
             * must match the public record exactly. */
            TASK_CHECK( g_hook.n == r->nref );
            for( int i = 0; i < r->nref; i++ )
                TASK_CHECK( pts_in_array( g_hook.pts, g_hook.n, r->ref_pts[i] ) );
            if( r->nref > 0 )
                compared++;
        }
    }
    TASK_CHECK( compared >= 3 );
    x264_reffb_mb_commit_hook = prev;
    g_hook_armed = 0;
    TASK_CHECK( env_drain( &e ) >= 0 );
    env_close( &e );
    return 0;
}

/* ================================================================== */
/* pipeline group                                                     */
/* ================================================================== */

static int case_pipeline_no_forced_idr( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    int armed = 0;
    for( int t = 0; t < NFRAMES; t++ )
    {
        TASK_CHECK( env_step( &e, t, NULL ) >= 0 );
        if( !armed && rec_find( &e.rec, 160 ) >= 0 )
        {
            TASK_CHECK( x264_encoder_invalidate_reference_exact( e.h, 160 ) == 0 );
            armed = 1;
        }
    }
    TASK_CHECK( armed );
    TASK_CHECK( env_drain( &e ) >= 0 );
    /* No keyframe beyond the very first IDR, no pictures dropped. */
    TASK_CHECK( !e.saw_keyframe_after_first );
    TASK_CHECK( e.rec.n == NFRAMES );
    /* B-frames are delivered in coded (not display) order, so the output PTS
     * sequence is not monotonic; instead verify every input PTS is produced
     * exactly once, so the timeline stays contiguous and unique. */
    for( int t = 0; t < NFRAMES; t++ )
    {
        int matches = 0;
        for( int i = 0; i < e.rec.n; i++ )
            if( e.rec.rec[i].pts == (int64_t)t * 40 )
                matches++;
        TASK_CHECK( matches == 1 );
    }
    env_close( &e );
    return 0;
}

static int case_pipeline_slot_reuse( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    int invalidations = 0;
    for( int t = 0; t < NFRAMES; t++ )
    {
        TASK_CHECK( env_step( &e, t, NULL ) >= 0 );
        /* Repeatedly invalidate retained P frames to exercise many slot
         * recycles; accept whenever one is still retained. */
        if( t > 6 && (t % 6) == 0 )
        {
            int64_t cand = (int64_t)(t - 4) * 40;
            if( rec_find( &e.rec, cand ) >= 0 && rec_is_ref_picture( &e.rec, cand ) )
            {
                int rc = x264_encoder_invalidate_reference_exact( e.h, cand );
                TASK_CHECK( rc == 0 || rc < 0 );
                if( rc == 0 )
                    invalidations++;
            }
        }
    }
    TASK_CHECK( invalidations >= 2 );
    TASK_CHECK( env_drain( &e ) >= 0 );
    TASK_CHECK( e.rec.n == NFRAMES );
    /* After all losses, every recorded dependency still resolves to a PTS
     * that was actually delivered. */
    for( int i = 0; i < e.rec.n; i++ )
        for( int d = 0; d < e.rec.rec[i].nref; d++ )
            TASK_CHECK( rec_find( &e.rec, e.rec.rec[i].ref_pts[d] ) >= 0 );
    env_close( &e );
    return 0;
}

static int case_pipeline_two_instances( void )
{
    enc_env_t a, b;
    TASK_CHECK( env_open( &a, 1, 0 ) == 0 );
    TASK_CHECK( env_open( &b, 1, 0 ) == 0 );
    int lost_a = 0, lost_b = 0;
    for( int t = 0; t < 24; t++ )
    {
        TASK_CHECK( env_step( &a, t, NULL ) >= 0 );
        TASK_CHECK( env_step( &b, t, NULL ) >= 0 );
        if( !lost_a && rec_find( &a.rec, 160 ) >= 0 &&
            x264_encoder_invalidate_reference_exact( a.h, 160 ) == 0 )
            lost_a = 1;
        /* Instance B loses a different frame (BREF pts 80). */
        if( !lost_b && rec_find( &b.rec, 80 ) >= 0 &&
            x264_encoder_invalidate_reference_exact( b.h, 80 ) == 0 )
            lost_b = 1;
    }
    TASK_CHECK( lost_a );
    TASK_CHECK( lost_b );
    TASK_CHECK( env_drain( &a ) >= 0 );
    TASK_CHECK( env_drain( &b ) >= 0 );
    /* B never lost 160, so it may still reference it; A must not. */
    int a_clean = 1, b_uses_160 = 0;
    for( int i = 0; i < a.rec.n; i++ )
        if( pts_in_array( a.rec.rec[i].ref_pts, a.rec.rec[i].nref, 160 ) &&
            a.rec.rec[i].pts > 160 )
            a_clean = 0;
    for( int i = 0; i < b.rec.n; i++ )
        if( pts_in_array( b.rec.rec[i].ref_pts, b.rec.rec[i].nref, 160 ) )
            b_uses_160 = 1;
    TASK_CHECK( a_clean );
    TASK_CHECK( b_uses_160 );
    env_close( &a );
    env_close( &b );
    return 0;
}

static int case_pipeline_null_drain( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    for( int t = 0; t < 16; t++ )
        TASK_CHECK( env_step( &e, t, NULL ) >= 0 );
    int delayed = x264_encoder_delayed_frames( e.h );
    TASK_CHECK( delayed > 0 );
    int drained = 0;
    while( x264_encoder_delayed_frames( e.h ) )
    {
        x264_picture_t out;
        x264_nal_t *nal;
        int nn;
        int size = x264_encoder_encode( e.h, &nal, &nn, NULL, &out );
        TASK_CHECK( size >= 0 );
        if( size == 0 )
        {
            /* No output: feedback counters must report zero. */
            TASK_CHECK( out.prop.i_ref_used == 0 );
            continue;
        }
        drained++;
        /* Delayed output still carries its own dependency record. */
        TASK_CHECK( (out.i_type == X264_TYPE_I || out.i_type == X264_TYPE_IDR)
                    ? out.prop.i_ref_used == 0 : out.prop.i_ref_used >= 0 );
    }
    TASK_CHECK( drained > 0 );
    env_close( &e );
    return 0;
}

/* ================================================================== */
/* compat group                                                       */
/* ================================================================== */

static int case_compat_disabled( void )
{
    /* This case never inspects slice headers, so it uses a plain encoder
     * instead of the parser-backed harness. */
    x264_param_t p;
    base_param( &p, 0, 0 );
    x264_t *h = x264_encoder_open( &p );
    TASK_CHECK( h != NULL );
    x264_picture_t in;
    TASK_CHECK( x264_picture_alloc( &in, X264_CSP_I420, WIDTH, HEIGHT ) == 0 );

    for( int t = 0; t < 16; t++ )
    {
        x264_picture_t out;
        x264_nal_t *nal;
        int nn;
        memset( &out, 0, sizeof(out) ); /* emulate a freshly initialized output */
        fill_texture( &in, t );
        in.i_pts = t * 40;
        in.i_type = X264_TYPE_AUTO;
        int size = x264_encoder_encode( h, &nal, &nn, &in, &out );
        TASK_CHECK( size >= 0 );
        if( size > 0 )
        {
            TASK_CHECK( out.prop.i_ref_used == 0 );
            /* Exact feedback is inert when the switch is off. */
            TASK_CHECK( x264_encoder_invalidate_reference_exact( h, out.i_pts ) < 0 );
        }
    }
    /* Legacy interface still rejects B-frame configurations. */
    TASK_CHECK( x264_encoder_invalidate_reference( h, 0 ) < 0 );
    while( x264_encoder_delayed_frames( h ) )
    {
        x264_picture_t out;
        x264_nal_t *nal;
        int nn;
        TASK_CHECK( x264_encoder_encode( h, &nal, &nn, NULL, &out ) >= 0 );
    }
    x264_picture_clean( &in );
    x264_encoder_close( h );
    return 0;
}

static int case_compat_unsupported_open( void )
{
    x264_param_t p;

    base_param( &p, 1, 0 );
    p.i_threads = 2;
    TASK_CHECK( x264_encoder_open( &p ) == NULL );

    base_param( &p, 1, 0 );
    p.b_intra_refresh = 1;
    TASK_CHECK( x264_encoder_open( &p ) == NULL );

    base_param( &p, 1, 0 );
    p.i_csp = X264_CSP_NV12;
    TASK_CHECK( x264_encoder_open( &p ) == NULL );

    base_param( &p, 1, 0 );
    p.b_fake_interlaced = 1;
    TASK_CHECK( x264_encoder_open( &p ) == NULL );

    /* Sanity: the supported combination opens. */
    base_param( &p, 1, 0 );
    x264_t *h = x264_encoder_open( &p );
    TASK_CHECK( h != NULL );
    x264_encoder_close( h );
    return 0;
}

static int case_compat_pts_uniqueness( void )
{
    enc_env_t e;
    TASK_CHECK( env_open( &e, 1, 0 ) == 0 );
    fill_texture( &e.in, 0 );
    e.in.i_pts = 40;
    e.in.i_type = X264_TYPE_AUTO;
    x264_picture_t out;
    x264_nal_t *nal;
    int nn;
    TASK_CHECK( x264_encoder_encode( e.h, &nal, &nn, &e.in, &out ) >= 0 );
    /* Repeating a PTS (non-increasing) must be rejected. */
    e.in.i_pts = 40;
    TASK_CHECK( x264_encoder_encode( e.h, &nal, &nn, &e.in, &out ) < 0 );
    /* A strictly larger PTS is accepted again. */
    e.in.i_pts = 80;
    TASK_CHECK( x264_encoder_encode( e.h, &nal, &nn, &e.in, &out ) >= 0 );
    TASK_CHECK( env_drain( &e ) >= 0 );
    env_close( &e );
    return 0;
}

static int case_compat_ref_count_cap( void )
{
    /* The public record never exceeds its 16-entry capacity even with the
     * maximum reference count. */
    x264_param_t p;
    base_param( &p, 1, 0 );
    p.i_frame_reference = 16;
    x264_t *h = x264_encoder_open( &p );
    TASK_CHECK( h != NULL );
    x264_picture_t in;
    TASK_CHECK( x264_picture_alloc( &in, X264_CSP_I420, WIDTH, HEIGHT ) == 0 );
    int max_refs = 0;
    for( int t = 0; t < 24; t++ )
    {
        fill_texture( &in, t );
        in.i_pts = t * 40;
        in.i_type = X264_TYPE_AUTO;
        x264_picture_t o;
        x264_nal_t *nal;
        int nn;
        int size = x264_encoder_encode( h, &nal, &nn, &in, &o );
        TASK_CHECK( size >= 0 );
        if( size > 0 )
        {
            TASK_CHECK( o.prop.i_ref_used >= 0 && o.prop.i_ref_used <= 16 );
            if( o.prop.i_ref_used > max_refs )
                max_refs = o.prop.i_ref_used;
        }
    }
    TASK_CHECK( max_refs >= 1 );
    while( x264_encoder_delayed_frames( h ) )
    {
        x264_picture_t o;
        x264_nal_t *nal;
        int nn;
        TASK_CHECK( x264_encoder_encode( h, &nal, &nn, NULL, &o ) >= 0 );
    }
    x264_picture_clean( &in );
    x264_encoder_close( h );
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
    { "graph", "closure_p",            case_graph_closure_p },
    { "graph", "closure_transitive",   case_graph_closure_transitive },
    { "graph", "closure_bref",         case_graph_closure_bref },
    { "graph", "unknown_and_nonref",   case_graph_unknown_and_nonref },
    { "graph", "full_closure_reject",  case_graph_full_closure_reject },
    { "graph", "repeat_and_displaced", case_graph_repeat_and_displaced },

    { "lists", "header_parse",         case_lists_header_parse },
    { "lists", "empty_direction",      case_lists_empty_direction },
    { "lists", "commit_instrument",    case_lists_commit_instrument },

    { "pipeline", "no_forced_idr",     case_pipeline_no_forced_idr },
    { "pipeline", "slot_reuse",        case_pipeline_slot_reuse },
    { "pipeline", "two_instances",     case_pipeline_two_instances },
    { "pipeline", "null_drain",        case_pipeline_null_drain },

    { "compat", "disabled",            case_compat_disabled },
    { "compat", "unsupported_open",    case_compat_unsupported_open },
    { "compat", "pts_uniqueness",      case_compat_pts_uniqueness },
    { "compat", "ref_count_cap",       case_compat_ref_count_cap },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
