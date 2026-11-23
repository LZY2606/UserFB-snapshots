/*****************************************************************************
 * example_ref_feedback.c: exact reference-picture loss feedback example
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

/* This example synthesizes a moving I420 texture, encodes it with a B-pyramid,
 * and reports one lost reference picture through the exact feedback API.
 * It prints the reference list that every output picture actually used, so the
 * effect of the loss (the closure and the legalized L0/L1 lists) can be seen.
 *
 * Timing note: x264_encoder_invalidate_reference_exact() must be called between
 * x264_encoder_encode() calls (never from a callback inside one), and only with
 * a PTS that has already been returned on an output picture and is still kept
 * as a reference.  Input PTS must be strictly increasing and unique. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <x264.h>

#define WIDTH 64
#define HEIGHT 64
#define NFRAMES 40
#define LOST_PTS 160   /* PTS of the picture we pretend the receiver lost */

static void fill_frame( x264_picture_t *pic, int t )
{
    uint8_t *y = pic->img.plane[0];
    uint8_t *u = pic->img.plane[1];
    uint8_t *v = pic->img.plane[2];
    int sy = pic->img.i_stride[0];
    int sc = pic->img.i_stride[1];
    for( int row = 0; row < HEIGHT; row++ )
        for( int col = 0; col < WIDTH; col++ )
            y[row*sy + col] = (uint8_t)( (col*7 + row*3 + (col + t*3)/9 * 37 + t*5) & 255 );
    for( int row = 0; row < HEIGHT/2; row++ )
        for( int col = 0; col < WIDTH/2; col++ )
        {
            u[row*sc + col] = (uint8_t)( (col*11 + t*7) & 255 );
            v[row*sc + col] = (uint8_t)( (row*13 + t*9 + 61) & 255 );
        }
}

int main( void )
{
    x264_param_t param;
    x264_picture_t in, out;
    x264_nal_t *nal;
    int nn;

    if( x264_param_default_preset( &param, "veryfast", NULL ) < 0 )
        return 2;
    param.i_width = WIDTH;
    param.i_height = HEIGHT;
    param.i_threads = 1;
    param.i_lookahead_threads = 1;
    param.i_csp = X264_CSP_I420;
    param.i_bitdepth = 8;
    param.b_vfr_input = 0;
    param.i_fps_num = 25;
    param.i_fps_den = 1;
    param.i_bframe = 3;
    param.i_bframe_adaptive = X264_B_ADAPT_NONE;
    param.i_bframe_pyramid = X264_B_PYRAMID_NORMAL;
    param.i_frame_reference = 4;
    param.i_keyint_max = 100;
    param.i_scenecut_threshold = 0;
    param.rc.i_rc_method = X264_RC_ABR;
    param.rc.i_bitrate = 200;
    param.rc.b_mb_tree = 0;
    param.b_repeat_headers = 1;
    param.b_annexb = 1;
    param.i_log_level = X264_LOG_NONE;
    param.b_intra_refresh = 0;
    param.b_reference_feedback = 1;

    x264_t *h = x264_encoder_open( &param );
    if( !h )
    {
        fprintf( stderr, "x264_encoder_open failed\n" );
        return 3;
    }
    if( x264_picture_alloc( &in, X264_CSP_I420, WIDTH, HEIGHT ) < 0 )
        return 4;

    int reported = 0;
    for( int t = 0; t < NFRAMES || x264_encoder_delayed_frames( h ); t++ )
    {
        if( t < NFRAMES )
        {
            fill_frame( &in, t );
            in.i_pts = t * 40;
            in.i_type = X264_TYPE_AUTO;
        }
        int size = x264_encoder_encode( h, &nal, &nn, t < NFRAMES ? &in : NULL, &out );
        if( size < 0 )
            return 5;
        if( !size )
            continue;

        printf( "out pts=%4lld type=%s refs=",
                (long long)out.i_pts,
                out.i_type == X264_TYPE_IDR || out.i_type == X264_TYPE_I ? "I" :
                out.i_type == X264_TYPE_P ? "P" :
                out.i_type == X264_TYPE_BREF ? "BREF" : "B" );
        for( int i = 0; i < out.prop.i_ref_used; i++ )
            printf( "%s%lld", i ? "," : "", (long long)out.prop.ref_used_pts[i] );
        if( !out.prop.i_ref_used )
            printf( "-" );
        printf( "\n" );

        /* Report the loss only after that picture has been delivered and while
         * it is still retained.  A negative return means the request was
         * rejected atomically; here a first successful report is enough. */
        if( !reported && out.i_pts >= LOST_PTS )
        {
            int rc = x264_encoder_invalidate_reference_exact( h, LOST_PTS );
            printf( "invalidate pts=%d -> %s\n", LOST_PTS, rc == 0 ? "ok" : "rejected" );
            if( rc == 0 )
                reported = 1;
        }
    }

    x264_picture_clean( &in );
    x264_encoder_close( h );
    printf( "reference feedback example complete\n" );
    return 0;
}
