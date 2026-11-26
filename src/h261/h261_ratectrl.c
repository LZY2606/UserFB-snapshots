/*
Copyright (c) 2013, Ralf Willenbacher
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include "h261_decl.h"

#define H261_RATECTRL_BITS 6
#define H261_RATECTRL_ONE ( 1 << H261_RATECTRL_BITS )
#define H261_RATECTRL_HALF ( 1 << ( H261_RATECTRL_BITS - 1 ) )
/* 29.97 */
#define H261_RATECTRL_TIME_SCALE 1918

/* pow( 2.0, f/5 ), f = -31 to 31 */
static Int32 h261_ratectrl_qscale[ 63 ] = {
	0, 1, 1, 1, 1, 1, 2, 2, 2, 3, /* -31 - -22 */
	3, 4, 4, 5, 6, 6, 8, 9, 10, 12, /* -21 - -12 */
	13, 16, 18, 21, 24, 27, 32, 36, 42, 48, 55,  /* -11 - -1 */
	64, /* 0 */
	73, 84, 97, 111, 128, 147, 168, 194, 222, 256, 294, /* 1 - 11 */
	337, 388, 445, 512, 588, 675, 776, 891, 1024, 1176, /* 12 - 21 */
	1351, 1552, 1782, 2048, 2352, 2702, 3104, 3565, 4096, 4705 /* 22 - 31 */
};


Int32 h261_ratectrl_mul( Int32 m1, Int32 m2 )
{
	Int64 m;

	m = ( ( Int64 ) m1 ) * ( ( Int64 ) m2 );

	return ( Int32 ) ( m >> H261_RATECTRL_BITS );
}


Int32 h261_ratectrl_div( Int32 i_div1, Int32 i_div2 )
{
	Int64 d;

	d = ( ( ( Int64 ) i_div1 ) << H261_RATECTRL_BITS ) / i_div2;

	return ( Int32 ) d;
}


Int32 h261_ratectrl_int_to_fp( Int32 i_int )
{
	return i_int << H261_RATECTRL_BITS;
}


Int32 h261_ratectrl_fp_to_int( Int32 i_fp )
{
	return i_fp >> H261_RATECTRL_BITS;
}


Int32 h261_ratectrl_qscale_for_quant( Int32 i_quant )
{
	i_quant = MIN( MAX( i_quant, 0 ), 31 );
	i_quant += 31;

	return h261_ratectrl_qscale[ i_quant ];
}


Int32 h261_ratectrl_quant_for_qscale( Int32 i_qscale )
{
	Int32 i_pos, i_step, i_lower_idx, i_upper_idx;

	i_pos = 31;
	i_step = 16;

	while( i_step )
	{
		if( h261_ratectrl_qscale[ i_pos ] < i_qscale )
		{
			i_pos += i_step;
		}
		else
		{
			i_pos -= i_step;
		}
		i_step = i_step >> 1;
	}

	if( h261_ratectrl_qscale[ i_pos ] < i_qscale )
	{
		i_lower_idx = i_pos;
		i_upper_idx = MIN( i_pos + 1, 62 );
	}
	else
	{
		i_lower_idx = MAX( 0, i_pos - 1 );
		i_upper_idx = i_pos;
	}

	if( i_qscale - h261_ratectrl_qscale[ i_lower_idx ] < h261_ratectrl_qscale[ i_upper_idx ] - i_qscale )
	{
		i_pos = i_lower_idx;
	}
	else
	{
		i_pos = i_upper_idx;
	}

	return i_pos - 31;
}


Void h261_init_ratectrl( h261_context_t *ps_ctx, Int32 i_quant, Int32 i_bitrate, Int32 i_vbv_size, Int32 i_mode )
{
	Int32 i_idx;

	h261_ratectrl_t *ps_ratectrl;

	/* Re-initialising the old rate controller switches GOB budget mode off. */
	h261_gob_budget_destroy( ps_ctx );
	ps_ctx->i_gob_encode_entries = 0;

	ps_ratectrl = &ps_ctx->s_ratectrl;

	ps_ratectrl->i_ratectrl_mode = i_mode;
	ps_ratectrl->i_current_quant = i_quant;

	ps_ratectrl->f_bitrate = i_bitrate;
	ps_ratectrl->f_framerate = 1.0 / 29.97;

	ps_ratectrl->f_actual_bits = i_bitrate * ps_ratectrl->f_framerate;
	ps_ratectrl->f_wanted_bits = i_bitrate * ps_ratectrl->f_framerate;
	ps_ratectrl->f_bits_quant = ps_ratectrl->f_actual_bits * ps_ratectrl->i_current_quant;

}


Void h261_ratectrl_finish( h261_context_t *ps_ctx )
{
	h261_ratectrl_t *ps_ratectrl;

	ps_ratectrl = &ps_ctx->s_ratectrl;

	fprintf( stderr, "ratectrl final quantizer: %f\n", ps_ratectrl->f_bits_quant / ps_ratectrl->f_wanted_bits );

}


Void h261_ratectrl_init_picture( h261_context_t *ps_ctx, h261_picture_parameters_t *ps_picture )
{
	h261_ratectrl_t *ps_ratectrl;

	ps_ratectrl = &ps_ctx->s_ratectrl;

	h261_gob_budget_capture_frame_num( ps_ctx, ps_picture->i_frame_num );

	if( ps_ratectrl->i_ratectrl_mode == H261_RATECTRL_MODE_QUANT )
	{
		ps_picture->i_quantiser = ps_ratectrl->i_current_quant;
	}
	else if( ps_ratectrl->i_ratectrl_mode == H261_RATECTRL_MODE_VBR )
	{
		float f_quantizer, f_quantizer_adjust;

		f_quantizer = ps_ratectrl->f_bits_quant / ps_ratectrl->f_actual_bits;
		f_quantizer_adjust = MIN( 2.0, MAX( 0.5, 1.0 + ( ( ps_ratectrl->f_actual_bits - ps_ratectrl->f_wanted_bits ) / ps_ratectrl->f_bitrate ) ) );
		f_quantizer = f_quantizer * f_quantizer_adjust;

		ps_ratectrl->i_current_quant = MIN( 31, MAX( 0, ( Int32 ) floorf( f_quantizer + 0.5f ) ) );
		ps_picture->i_quantiser = ps_ratectrl->i_current_quant;
	}
}


Void h261_ratectrl_update_picture( h261_context_t *ps_ctx, Int32 i_bitstream_bytes )
{
	h261_ratectrl_t *ps_ratectrl;

	ps_ratectrl = &ps_ctx->s_ratectrl;

	if( ps_ratectrl->i_ratectrl_mode == H261_RATECTRL_MODE_VBR )
	{
		ps_ratectrl->f_actual_bits += i_bitstream_bytes * 8;
		ps_ratectrl->f_wanted_bits += ps_ratectrl->f_bitrate * ps_ratectrl->f_framerate;
		ps_ratectrl->f_bits_quant += i_bitstream_bytes * 8 * ps_ratectrl->i_current_quant;
	}
}


/* ====================================================================== */
/* Optional GOB budget mode                                                */
/* ====================================================================== */

/* Every H.261 GOB covers 11 macroblocks horizontally by 3 vertically. */
#define H261_GB_TRIALS		31
#define H261_GB_LUMA_W		176
#define H261_GB_LUMA_H		48
#define H261_GB_CHROMA_W	88
#define H261_GB_CHROMA_H	24
#define H261_GB_DEFAULT_BS_CAP 0x100000

typedef struct h261_gob_budget_state_s
{
	/* configuration copy (the instance owns observer and state) */
	Int32 i_bitrate;
	Int32 i_fps_num;
	Int32 i_fps_den;
	Int32 i_buffer_bits;
	h261_gob_budget_observe_fn pf_observe;
	Void *pv_opaque;

	/* exact settlement clock */
	Int64 i_gob_index;
	Int64 i_clock_remainder;
	Int64 i_target_total;
	Int64 i_coded_total;
	Int64 i_debt;

	/* picture header attribution */
	Int32 i_frame_num;
	Int32 i_header_pending;
	Int32 i_header_bits;

	/* reusable, single-GOB-only workspace */
	Int32 i_cache_count;
	Int32 i_scratch_cap;

	h261_mb_cache_t *ps_cache_base;
	h261_mb_cache_t *ps_cache_fit;
	h261_mb_cache_t *ps_cache_of;

	UInt8 *pui8_y_base;
	UInt8 *pui8_cb_base;
	UInt8 *pui8_cr_base;
	UInt8 *pui8_y_fit;
	UInt8 *pui8_cb_fit;
	UInt8 *pui8_cr_fit;
	UInt8 *pui8_y_of;
	UInt8 *pui8_cb_of;
	UInt8 *pui8_cr_of;

	UInt8 *pui8_scratch_bits;
	UInt8 *pui8_bits_fit;
	UInt8 *pui8_bits_of;

	Int32 i_fit_nbytes;
	Int32 i_of_nbytes;
	Int32 i_have_fit;
	Int32 i_have_of;

	Int32 i_stash_ox;
	Int32 i_stash_oy;

	Int32 i_fit_q;
	Int64 i_fit_bits;
	Int64 i_fit_sse;
	Int32 i_of_q;
	Int64 i_of_bits;
	Int64 i_of_sse;

	h261_context_t s_fit_ctx;
	h261_context_t s_of_ctx;

	h261_gob_budget_stats_t s_stats;
} h261_gob_budget_state_t;


int h261_ratectrl_init_gob_budget( h261_context_t *ps_ctx, const h261_gob_budget_config_t *ps_config )
{
	h261_gob_budget_state_t *ps_state;

	if( ps_ctx == NULL || ps_config == NULL )
	{
		return 0;
	}
	if( ps_config->bitrate < 1 || ps_config->bitrate > 100000000 )
	{
		return 0;
	}
	if( ps_config->fps_num < 1 || ps_config->fps_num > 1000000 )
	{
		return 0;
	}
	if( ps_config->fps_den < 1 || ps_config->fps_den > 1000000 )
	{
		return 0;
	}
	if( ps_config->buffer_bits < 1 || ps_config->buffer_bits > 1000000000 )
	{
		return 0;
	}
	/* 1 <= fps_num / fps_den <= 120, checked without floating point. */
	if( ( Int64 ) ps_config->fps_num < ( Int64 ) ps_config->fps_den )
	{
		return 0;
	}
	if( ( Int64 ) ps_config->fps_num > 120LL * ( Int64 ) ps_config->fps_den )
	{
		return 0;
	}

	if( ps_ctx->ps_gob_budget == NULL )
	{
		ps_state = ( h261_gob_budget_state_t * ) calloc( 1, sizeof( h261_gob_budget_state_t ) );
		if( ps_state == NULL )
		{
			return 0;
		}
		ps_ctx->ps_gob_budget = ps_state;
	}
	else
	{
		ps_state = ps_ctx->ps_gob_budget;
	}

	ps_state->i_bitrate = ps_config->bitrate;
	ps_state->i_fps_num = ps_config->fps_num;
	ps_state->i_fps_den = ps_config->fps_den;
	ps_state->i_buffer_bits = ps_config->buffer_bits;
	ps_state->pf_observe = ps_config->observe;
	ps_state->pv_opaque = ps_config->opaque;

	/* successful initialisation restarts the budget history */
	ps_state->i_gob_index = 0;
	ps_state->i_clock_remainder = 0;
	ps_state->i_target_total = 0;
	ps_state->i_coded_total = 0;
	ps_state->i_debt = 0;
	ps_state->i_frame_num = 0;
	ps_state->i_header_pending = 0;
	ps_state->i_header_bits = 0;
	ps_state->i_have_fit = 0;
	ps_state->i_have_of = 0;

	memset( &ps_state->s_stats, 0, sizeof( ps_state->s_stats ) );

	return 1;
}


Void h261_gob_budget_destroy( h261_context_t *ps_ctx )
{
	h261_gob_budget_state_t *ps_state;

	if( ps_ctx == NULL || ps_ctx->ps_gob_budget == NULL )
	{
		return;
	}
	ps_state = ps_ctx->ps_gob_budget;

	free( ps_state->ps_cache_base );
	free( ps_state->ps_cache_fit );
	free( ps_state->ps_cache_of );
	free( ps_state->pui8_y_base );
	free( ps_state->pui8_cb_base );
	free( ps_state->pui8_cr_base );
	free( ps_state->pui8_y_fit );
	free( ps_state->pui8_cb_fit );
	free( ps_state->pui8_cr_fit );
	free( ps_state->pui8_y_of );
	free( ps_state->pui8_cb_of );
	free( ps_state->pui8_cr_of );
	free( ps_state->pui8_scratch_bits );
	free( ps_state->pui8_bits_fit );
	free( ps_state->pui8_bits_of );
	free( ps_state );

	ps_ctx->ps_gob_budget = NULL;
}


Void h261_gob_budget_capture_frame_num( h261_context_t *ps_ctx, Int32 i_frame_num )
{
	if( ps_ctx != NULL && ps_ctx->ps_gob_budget != NULL )
	{
		ps_ctx->ps_gob_budget->i_frame_num = i_frame_num;
	}
}


Void h261_gob_budget_record_picture_header( h261_context_t *ps_ctx, Int32 i_before_bits, Int32 i_after_bits )
{
	h261_gob_budget_state_t *ps_state;

	if( ps_ctx == NULL || ps_ctx->ps_gob_budget == NULL )
	{
		return;
	}
	ps_state = ps_ctx->ps_gob_budget;
	ps_state->i_header_bits = i_after_bits - i_before_bits;
	ps_state->i_header_pending = 1;
}


static Int32 h261_gob_budget_gob_count( h261_context_t *ps_ctx )
{
	return ps_ctx->i_source_format == H261_SOURCE_FORMAT_CIF ? 12 : 3;
}


/* Exact one-step cumulative budget clock: target_K = floor(K*N/D). */
static Int64 h261_gob_budget_clock_step_raw( h261_gob_budget_state_t *ps_state, Int32 i_gobs_per_frame, Int64 *pi64_quota )
{
	Int64 i64_numerator = ( Int64 ) ps_state->i_bitrate * ( Int64 ) ps_state->i_fps_den;
	Int64 i64_denominator = ( Int64 ) ps_state->i_fps_num * ( Int64 ) i_gobs_per_frame;
	Int64 i64_value = ps_state->i_clock_remainder + i64_numerator;
	Int64 i64_quota = i64_value / i64_denominator;

	ps_state->i_clock_remainder = i64_value - i64_quota * i64_denominator;
	ps_state->i_target_total += i64_quota;
	ps_state->i_gob_index++;

	if( pi64_quota != NULL )
	{
		*pi64_quota = i64_quota;
	}
	return ps_state->i_target_total;
}


Int64 h261_gob_budget_debug_clock_tick( h261_context_t *ps_ctx, Int64 *pi64_quota )
{
	if( pi64_quota != NULL )
	{
		*pi64_quota = 0;
	}
	if( ps_ctx == NULL || ps_ctx->ps_gob_budget == NULL )
	{
		return 0;
	}
	return h261_gob_budget_clock_step_raw( ps_ctx->ps_gob_budget, h261_gob_budget_gob_count( ps_ctx ), pi64_quota );
}


static Int32 h261_gob_budget_ensure_workspace( h261_context_t *ps_ctx, h261_gob_budget_state_t *ps_state )
{
	Int32 i_width = ps_ctx->s_current_frame.i_width;
	Int32 i_height = ps_ctx->s_current_frame.i_height;
	Int32 i_cache_count;
	Int32 i_cap;

	if( i_width <= 0 || i_height <= 0 )
	{
		i_width = ps_ctx->i_source_format == H261_SOURCE_FORMAT_CIF ? 352 : 176;
		i_height = ps_ctx->i_source_format == H261_SOURCE_FORMAT_CIF ? 288 : 144;
	}
	i_cache_count = ( i_width >> 4 ) * ( i_height >> 4 );
	i_cap = ps_ctx->s_bitstream.i_length > 0 ? ps_ctx->s_bitstream.i_length : H261_GB_DEFAULT_BS_CAP;

	if( ps_state->ps_cache_base != NULL &&
		ps_state->i_cache_count == i_cache_count &&
		ps_state->i_scratch_cap == i_cap )
	{
		return 1;
	}

	free( ps_state->ps_cache_base );
	free( ps_state->ps_cache_fit );
	free( ps_state->ps_cache_of );
	free( ps_state->pui8_y_base );
	free( ps_state->pui8_cb_base );
	free( ps_state->pui8_cr_base );
	free( ps_state->pui8_y_fit );
	free( ps_state->pui8_cb_fit );
	free( ps_state->pui8_cr_fit );
	free( ps_state->pui8_y_of );
	free( ps_state->pui8_cb_of );
	free( ps_state->pui8_cr_of );
	free( ps_state->pui8_scratch_bits );
	free( ps_state->pui8_bits_fit );
	free( ps_state->pui8_bits_of );

	ps_state->i_cache_count = i_cache_count;
	ps_state->i_scratch_cap = i_cap;

	ps_state->ps_cache_base = ( h261_mb_cache_t * ) calloc( ( size_t ) i_cache_count, sizeof( h261_mb_cache_t ) );
	ps_state->ps_cache_fit = ( h261_mb_cache_t * ) calloc( ( size_t ) i_cache_count, sizeof( h261_mb_cache_t ) );
	ps_state->ps_cache_of = ( h261_mb_cache_t * ) calloc( ( size_t ) i_cache_count, sizeof( h261_mb_cache_t ) );

	ps_state->pui8_y_base = ( UInt8 * ) malloc( H261_GB_LUMA_W * H261_GB_LUMA_H );
	ps_state->pui8_cb_base = ( UInt8 * ) malloc( H261_GB_CHROMA_W * H261_GB_CHROMA_H );
	ps_state->pui8_cr_base = ( UInt8 * ) malloc( H261_GB_CHROMA_W * H261_GB_CHROMA_H );
	ps_state->pui8_y_fit = ( UInt8 * ) malloc( H261_GB_LUMA_W * H261_GB_LUMA_H );
	ps_state->pui8_cb_fit = ( UInt8 * ) malloc( H261_GB_CHROMA_W * H261_GB_CHROMA_H );
	ps_state->pui8_cr_fit = ( UInt8 * ) malloc( H261_GB_CHROMA_W * H261_GB_CHROMA_H );
	ps_state->pui8_y_of = ( UInt8 * ) malloc( H261_GB_LUMA_W * H261_GB_LUMA_H );
	ps_state->pui8_cb_of = ( UInt8 * ) malloc( H261_GB_CHROMA_W * H261_GB_CHROMA_H );
	ps_state->pui8_cr_of = ( UInt8 * ) malloc( H261_GB_CHROMA_W * H261_GB_CHROMA_H );

	ps_state->pui8_scratch_bits = ( UInt8 * ) malloc( ( size_t ) i_cap );
	ps_state->pui8_bits_fit = ( UInt8 * ) malloc( ( size_t ) i_cap );
	ps_state->pui8_bits_of = ( UInt8 * ) malloc( ( size_t ) i_cap );

	if( !ps_state->ps_cache_base || !ps_state->ps_cache_fit || !ps_state->ps_cache_of ||
		!ps_state->pui8_y_base || !ps_state->pui8_cb_base || !ps_state->pui8_cr_base ||
		!ps_state->pui8_y_fit || !ps_state->pui8_cb_fit || !ps_state->pui8_cr_fit ||
		!ps_state->pui8_y_of || !ps_state->pui8_cb_of || !ps_state->pui8_cr_of ||
		!ps_state->pui8_scratch_bits || !ps_state->pui8_bits_fit || !ps_state->pui8_bits_of )
	{
		h261_gob_budget_destroy( ps_ctx );
		return 0;
	}

	return 1;
}


static Void h261_gob_budget_save_region( h261_frame_t *ps_frame, Int32 i_ox, Int32 i_oy,
	UInt8 *pui8_y, UInt8 *pui8_cb, UInt8 *pui8_cr )
{
	Int32 i_y;

	for( i_y = 0; i_y < H261_GB_LUMA_H; i_y++ )
	{
		memcpy( pui8_y + i_y * H261_GB_LUMA_W,
			ps_frame->pui8_Y + ( i_oy + i_y ) * ps_frame->i_stride_y + i_ox,
			H261_GB_LUMA_W );
	}
	for( i_y = 0; i_y < H261_GB_CHROMA_H; i_y++ )
	{
		memcpy( pui8_cb + i_y * H261_GB_CHROMA_W,
			ps_frame->pui8_Cb + ( ( i_oy >> 1 ) + i_y ) * ps_frame->i_stride_c + ( i_ox >> 1 ),
			H261_GB_CHROMA_W );
		memcpy( pui8_cr + i_y * H261_GB_CHROMA_W,
			ps_frame->pui8_Cr + ( ( i_oy >> 1 ) + i_y ) * ps_frame->i_stride_c + ( i_ox >> 1 ),
			H261_GB_CHROMA_W );
	}
}


static Void h261_gob_budget_restore_region( h261_frame_t *ps_frame, Int32 i_ox, Int32 i_oy,
	const UInt8 *pui8_y, const UInt8 *pui8_cb, const UInt8 *pui8_cr )
{
	Int32 i_y;

	for( i_y = 0; i_y < H261_GB_LUMA_H; i_y++ )
	{
		memcpy( ps_frame->pui8_Y + ( i_oy + i_y ) * ps_frame->i_stride_y + i_ox,
			pui8_y + i_y * H261_GB_LUMA_W, H261_GB_LUMA_W );
	}
	for( i_y = 0; i_y < H261_GB_CHROMA_H; i_y++ )
	{
		memcpy( ps_frame->pui8_Cb + ( ( i_oy >> 1 ) + i_y ) * ps_frame->i_stride_c + ( i_ox >> 1 ),
			pui8_cb + i_y * H261_GB_CHROMA_W, H261_GB_CHROMA_W );
		memcpy( ps_frame->pui8_Cr + ( ( i_oy >> 1 ) + i_y ) * ps_frame->i_stride_c + ( i_ox >> 1 ),
			pui8_cr + i_y * H261_GB_CHROMA_W, H261_GB_CHROMA_W );
	}
}


static Int64 h261_gob_budget_region_sse( h261_context_t *ps_ctx, Int32 i_ox, Int32 i_oy )
{
	h261_frame_t *ps_src = &ps_ctx->s_current_frame;
	h261_frame_t *ps_rec = &ps_ctx->s_reconstructed_frame;
	Int64 i64_sse = 0;
	Int32 i_y, i_x;
	Int32 i_d;

	for( i_y = 0; i_y < H261_GB_LUMA_H; i_y++ )
	{
		const UInt8 *p_src = ps_src->pui8_Y + ( i_oy + i_y ) * ps_src->i_stride_y + i_ox;
		const UInt8 *p_rec = ps_rec->pui8_Y + ( i_oy + i_y ) * ps_rec->i_stride_y + i_ox;
		for( i_x = 0; i_x < H261_GB_LUMA_W; i_x++ )
		{
			i_d = ( Int32 ) p_rec[ i_x ] - ( Int32 ) p_src[ i_x ];
			i64_sse += ( Int64 ) i_d * i_d;
		}
	}
	for( i_y = 0; i_y < H261_GB_CHROMA_H; i_y++ )
	{
		const UInt8 *p_src_cb = ps_src->pui8_Cb + ( ( i_oy >> 1 ) + i_y ) * ps_src->i_stride_c + ( i_ox >> 1 );
		const UInt8 *p_rec_cb = ps_rec->pui8_Cb + ( ( i_oy >> 1 ) + i_y ) * ps_rec->i_stride_c + ( i_ox >> 1 );
		const UInt8 *p_src_cr = ps_src->pui8_Cr + ( ( i_oy >> 1 ) + i_y ) * ps_src->i_stride_c + ( i_ox >> 1 );
		const UInt8 *p_rec_cr = ps_rec->pui8_Cr + ( ( i_oy >> 1 ) + i_y ) * ps_rec->i_stride_c + ( i_ox >> 1 );
		for( i_x = 0; i_x < H261_GB_CHROMA_W; i_x++ )
		{
			i_d = ( Int32 ) p_rec_cb[ i_x ] - ( Int32 ) p_src_cb[ i_x ];
			i64_sse += ( Int64 ) i_d * i_d;
			i_d = ( Int32 ) p_rec_cr[ i_x ] - ( Int32 ) p_src_cr[ i_x ];
			i64_sse += ( Int64 ) i_d * i_d;
		}
	}
	return i64_sse;
}


/* Feasible ordering: min SSE, then min bits, then min base quant. */
static Int32 h261_gob_budget_better_fit( Int64 i_sse, Int64 i_bits, Int32 i_q, const h261_gob_budget_state_t *ps_state )
{
	if( i_sse != ps_state->i_fit_sse ) return i_sse < ps_state->i_fit_sse;
	if( i_bits != ps_state->i_fit_bits ) return i_bits < ps_state->i_fit_bits;
	return i_q < ps_state->i_fit_q;
}


/* Overflow ordering: min bits, then min SSE, then min base quant. */
static Int32 h261_gob_budget_better_of( Int64 i_sse, Int64 i_bits, Int32 i_q, const h261_gob_budget_state_t *ps_state )
{
	if( i_bits != ps_state->i_of_bits ) return i_bits < ps_state->i_of_bits;
	if( i_sse != ps_state->i_of_sse ) return i_sse < ps_state->i_of_sse;
	return i_q < ps_state->i_of_q;
}


static Void h261_gob_budget_stash( h261_context_t *ps_ctx, h261_gob_budget_state_t *ps_state, Int32 i_fit )
{
	Int32 i_nbytes;

	i_nbytes = ( Int32 )( ps_ctx->s_bitstream.pui8_codeword_ptr - ps_state->pui8_scratch_bits );

	if( i_fit )
	{
		ps_state->s_fit_ctx = *ps_ctx;
		if( ps_ctx->ps_mb_cache != NULL )
		{
			memcpy( ps_state->ps_cache_fit, ps_ctx->ps_mb_cache, sizeof( h261_mb_cache_t ) * ( size_t ) ps_state->i_cache_count );
		}
		h261_gob_budget_save_region( &ps_ctx->s_reconstructed_frame, ps_state->i_stash_ox, ps_state->i_stash_oy,
			ps_state->pui8_y_fit, ps_state->pui8_cb_fit, ps_state->pui8_cr_fit );
		memcpy( ps_state->pui8_bits_fit, ps_state->pui8_scratch_bits, ( size_t ) i_nbytes );
		ps_state->i_fit_nbytes = i_nbytes;
	}
	else
	{
		ps_state->s_of_ctx = *ps_ctx;
		if( ps_ctx->ps_mb_cache != NULL )
		{
			memcpy( ps_state->ps_cache_of, ps_ctx->ps_mb_cache, sizeof( h261_mb_cache_t ) * ( size_t ) ps_state->i_cache_count );
		}
		h261_gob_budget_save_region( &ps_ctx->s_reconstructed_frame, ps_state->i_stash_ox, ps_state->i_stash_oy,
			ps_state->pui8_y_of, ps_state->pui8_cb_of, ps_state->pui8_cr_of );
		memcpy( ps_state->pui8_bits_of, ps_state->pui8_scratch_bits, ( size_t ) i_nbytes );
		ps_state->i_of_nbytes = i_nbytes;
	}
}


static Void h261_gob_budget_run_gob( h261_context_t *ps_ctx, Int32 i_gob_number, Int32 i_offset_x, Int32 i_offset_y, Int32 i_first )
{
	h261_gob_budget_state_t *ps_state = ps_ctx->ps_gob_budget;
	h261_context_t s_base;
	h261_bitstream_t s_trial_bs;

	Int64 i64_quota = 0;
	Int64 i64_d_before;
	Int64 i64_entries;
	Int32 i_q;
	Int32 i_header_extra;

	if( !h261_gob_budget_ensure_workspace( ps_ctx, ps_state ) )
	{
		/* allocation failure fallback: encode once through the plain core */
		h261_encode_gob( ps_ctx, i_gob_number, i_offset_x, i_offset_y );
		return;
	}

	/* quota for the group about to be settled (does not mutate history yet) */
	{
		Int32 i_g = h261_gob_budget_gob_count( ps_ctx );
		Int64 i64_n = ( Int64 ) ps_state->i_bitrate * ( Int64 ) ps_state->i_fps_den;
		Int64 i64_d = ( Int64 ) ps_state->i_fps_num * ( Int64 ) i_g;
		Int64 i64_v = ps_state->i_clock_remainder + i64_n;
		i64_quota = i64_v / i64_d;
	}

	i64_d_before = ps_state->i_debt;
	i_header_extra = ( i_first && ps_state->i_header_pending ) ? ps_state->i_header_bits : 0;
	if( i_first )
	{
		ps_state->i_header_pending = 0;
	}

	s_base = *ps_ctx;
	i64_entries = ps_ctx->i_gob_encode_entries;

	if( ps_ctx->ps_mb_cache != NULL )
	{
		memcpy( ps_state->ps_cache_base, ps_ctx->ps_mb_cache, sizeof( h261_mb_cache_t ) * ( size_t ) ps_state->i_cache_count );
	}
	h261_gob_budget_save_region( &ps_ctx->s_reconstructed_frame, i_offset_x, i_offset_y,
		ps_state->pui8_y_base, ps_state->pui8_cb_base, ps_state->pui8_cr_base );

	ps_state->i_have_fit = 0;
	ps_state->i_have_of = 0;
	ps_state->i_stash_ox = i_offset_x;
	ps_state->i_stash_oy = i_offset_y;

	for( i_q = 1; i_q <= H261_GB_TRIALS; i_q++ )
	{
		Int32 i_bits_start, i_bits_end;
		Int64 i64_gob_bits, i64_bits, i64_sse;

		/* every candidate starts from the same committed state */
		*ps_ctx = s_base;
		ps_ctx->i_gob_encode_entries = i64_entries;

		if( ps_ctx->ps_mb_cache != NULL )
		{
			memcpy( ps_ctx->ps_mb_cache, ps_state->ps_cache_base, sizeof( h261_mb_cache_t ) * ( size_t ) ps_state->i_cache_count );
		}
		h261_gob_budget_restore_region( &ps_ctx->s_reconstructed_frame, i_offset_x, i_offset_y,
			ps_state->pui8_y_base, ps_state->pui8_cb_base, ps_state->pui8_cr_base );

		/* private trial writer: same residue, isolated byte buffer */
		s_trial_bs = s_base.s_bitstream;
		s_trial_bs.pui8_bitstream = ps_state->pui8_scratch_bits;
		s_trial_bs.pui8_codeword_ptr = ps_state->pui8_scratch_bits;
		ps_ctx->s_bitstream = s_trial_bs;

		ps_ctx->s_picture.i_quantiser = i_q;

		i_bits_start = h261_bitstream_bits( &ps_ctx->s_bitstream );
		h261_encode_gob( ps_ctx, i_gob_number, i_offset_x, i_offset_y );
		i_bits_end = h261_bitstream_bits( &ps_ctx->s_bitstream );

		i64_entries = ps_ctx->i_gob_encode_entries;

		i64_gob_bits = ( Int64 )( i_bits_end - i_bits_start );
		i64_bits = i64_gob_bits + i_header_extra;
		i64_sse = h261_gob_budget_region_sse( ps_ctx, i_offset_x, i_offset_y );

		if( i64_d_before + i64_bits - i64_quota <= ( Int64 ) ps_state->i_buffer_bits )
		{
			if( !ps_state->i_have_fit ||
				h261_gob_budget_better_fit( i64_sse, i64_bits, i_q, ps_state ) )
			{
				ps_state->i_have_fit = 1;
				ps_state->i_fit_q = i_q;
				ps_state->i_fit_bits = i64_bits;
				ps_state->i_fit_sse = i64_sse;
				h261_gob_budget_stash( ps_ctx, ps_state, 1 );
			}
		}
		else
		{
			if( !ps_state->i_have_of ||
				h261_gob_budget_better_of( i64_sse, i64_bits, i_q, ps_state ) )
			{
				ps_state->i_have_of = 1;
				ps_state->i_of_q = i_q;
				ps_state->i_of_bits = i64_bits;
				ps_state->i_of_sse = i64_sse;
				h261_gob_budget_stash( ps_ctx, ps_state, 0 );
			}
		}
	}

	/* commit the already generated winner: no second real encoding */
	if( ps_state->i_have_fit )
	{
		UInt8 *pui8_real_ptr = s_base.s_bitstream.pui8_codeword_ptr;

		*ps_ctx = ps_state->s_fit_ctx;
		ps_ctx->i_gob_encode_entries = i64_entries;

		ps_ctx->s_bitstream.pui8_bitstream = s_base.s_bitstream.pui8_bitstream;
		ps_ctx->s_bitstream.i_length = s_base.s_bitstream.i_length;
		memcpy( pui8_real_ptr, ps_state->pui8_bits_fit, ( size_t ) ps_state->i_fit_nbytes );
		ps_ctx->s_bitstream.pui8_codeword_ptr = pui8_real_ptr + ps_state->i_fit_nbytes;

		if( ps_ctx->ps_mb_cache != NULL )
		{
			memcpy( ps_ctx->ps_mb_cache, ps_state->ps_cache_fit, sizeof( h261_mb_cache_t ) * ( size_t ) ps_state->i_cache_count );
		}
		h261_gob_budget_restore_region( &ps_ctx->s_reconstructed_frame, i_offset_x, i_offset_y,
			ps_state->pui8_y_fit, ps_state->pui8_cb_fit, ps_state->pui8_cr_fit );

		ps_state->s_stats.overflow = 0;
		ps_state->s_stats.gquant = ps_state->i_fit_q;
		ps_state->s_stats.bits = ps_state->i_fit_bits;
		ps_state->s_stats.sse = ps_state->i_fit_sse;
	}
	else
	{
		UInt8 *pui8_real_ptr = s_base.s_bitstream.pui8_codeword_ptr;

		*ps_ctx = ps_state->s_of_ctx;
		ps_ctx->i_gob_encode_entries = i64_entries;

		ps_ctx->s_bitstream.pui8_bitstream = s_base.s_bitstream.pui8_bitstream;
		ps_ctx->s_bitstream.i_length = s_base.s_bitstream.i_length;
		memcpy( pui8_real_ptr, ps_state->pui8_bits_of, ( size_t ) ps_state->i_of_nbytes );
		ps_ctx->s_bitstream.pui8_codeword_ptr = pui8_real_ptr + ps_state->i_of_nbytes;

		if( ps_ctx->ps_mb_cache != NULL )
		{
			memcpy( ps_ctx->ps_mb_cache, ps_state->ps_cache_of, sizeof( h261_mb_cache_t ) * ( size_t ) ps_state->i_cache_count );
		}
		h261_gob_budget_restore_region( &ps_ctx->s_reconstructed_frame, i_offset_x, i_offset_y,
			ps_state->pui8_y_of, ps_state->pui8_cb_of, ps_state->pui8_cr_of );

		ps_state->s_stats.overflow = 1;
		ps_state->s_stats.gquant = ps_state->i_of_q;
		ps_state->s_stats.bits = ps_state->i_of_bits;
		ps_state->s_stats.sse = ps_state->i_of_sse;
	}

	/* formal settlement happens once, after the candidate is committed */
	{
		Int32 i_g = h261_gob_budget_gob_count( ps_ctx );
		Int64 i64_n = ( Int64 ) ps_state->i_bitrate * ( Int64 ) ps_state->i_fps_den;
		Int64 i64_d = ( Int64 ) ps_state->i_fps_num * ( Int64 ) i_g;
		Int64 i64_v = ps_state->i_clock_remainder + i64_n;

		i64_quota = i64_v / i64_d;
		ps_state->i_clock_remainder = i64_v - i64_quota * i64_d;
		ps_state->i_target_total += i64_quota;
		ps_state->i_gob_index++;
	}

	ps_state->i_coded_total += ps_state->s_stats.bits;
	ps_state->i_debt += ps_state->s_stats.bits - i64_quota;

	ps_state->s_stats.frame_num = ps_state->i_frame_num;
	ps_state->s_stats.gob_number = i_gob_number;
	ps_state->s_stats.trials = H261_GB_TRIALS;
	ps_state->s_stats.quota = i64_quota;
	ps_state->s_stats.coded_total = ps_state->i_coded_total;
	ps_state->s_stats.target_total = ps_state->i_target_total;
	ps_state->s_stats.debt_after = ps_state->i_debt;

	if( ps_state->pf_observe != NULL )
	{
		ps_state->pf_observe( &ps_state->s_stats, ps_state->pv_opaque );
	}
}


Void h261_gob_budget_encode_gobs( h261_context_t *ps_ctx )
{
	Int32 i_idx;

	if( ps_ctx->ps_gob_budget == NULL )
	{
		h261_encode_gobs_plain( ps_ctx );
		return;
	}

	if( ps_ctx->i_source_format == H261_SOURCE_FORMAT_CIF )
	{
		const Int32 rgi_cif[ 12 ][ 3 ] = {
			{ 1, 0, 0 }, { 2, 176, 0 }, { 3, 0, 48 }, { 4, 176, 48 },
			{ 5, 0, 96 }, { 6, 176, 96 }, { 7, 0, 144 }, { 8, 176, 144 },
			{ 9, 0, 192 }, { 10, 176, 192 }, { 11, 0, 240 }, { 12, 176, 240 }
		};
		for( i_idx = 0; i_idx < 12; i_idx++ )
		{
			if( ps_ctx->ps_gob_budget == NULL )
				h261_encode_gob( ps_ctx, rgi_cif[ i_idx ][ 0 ], rgi_cif[ i_idx ][ 1 ], rgi_cif[ i_idx ][ 2 ] );
			else
				h261_gob_budget_run_gob( ps_ctx, rgi_cif[ i_idx ][ 0 ], rgi_cif[ i_idx ][ 1 ], rgi_cif[ i_idx ][ 2 ], i_idx == 0 );
		}
	}
	else
	{
		const Int32 rgi_qcif[ 3 ][ 3 ] = {
			{ 1, 0, 0 }, { 3, 0, 48 }, { 5, 0, 96 }
		};
		for( i_idx = 0; i_idx < 3; i_idx++ )
		{
			if( ps_ctx->ps_gob_budget == NULL )
				h261_encode_gob( ps_ctx, rgi_qcif[ i_idx ][ 0 ], rgi_qcif[ i_idx ][ 1 ], rgi_qcif[ i_idx ][ 2 ] );
			else
				h261_gob_budget_run_gob( ps_ctx, rgi_qcif[ i_idx ][ 0 ], rgi_qcif[ i_idx ][ 1 ], rgi_qcif[ i_idx ][ 2 ], i_idx == 0 );
		}
	}
}


