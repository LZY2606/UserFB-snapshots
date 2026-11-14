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

#define H261_GOB_BUDGET_NUM_QUANTS 31
#define H261_GOB_BUDGET_BITSTREAM_CAP ( 1 << 20 )


/* A frozen copy of everything a candidate produced: the writer (in its own
 * buffer, preserving the sub-byte remainder), the GOB coding state, the
 * motion search cache and the local reconstruction planes. */
typedef struct {
	h261_bitstream_t s_bitstream;
	UInt8 *pui8_bitstream_buffer;
	h261_gob_t s_gob;
	h261_mb_cache_t *ps_cache;
	UInt8 *pui8_recon_y;
	UInt8 *pui8_recon_cb;
	UInt8 *pui8_recon_cr;
	Int32 i_offset_y;   /* luma row where this GOB begins */
} h261_gob_snapshot_t;


/* Per-context scratch storage. It only ever holds the candidates and the
 * best result of the GOB currently being coded, so its size is independent
 * of the number of frames already encoded (no per-frame history, and no 31-way
 * full-frame storage is kept). The reconstruction scratch covers only the
 * rows of the current GOB, never the whole frame. */
typedef struct {
	Int32 i_stride_y;
	Int32 i_stride_c;
	size_t sz_gob_y;   /* Y rows touched by one GOB (frame stride * 48) */
	size_t sz_gob_c;   /* chroma rows touched by one GOB (stride * 24) */
	size_t sz_cache;

	h261_bitstream_t s_scratch_bitstream;
	UInt8 *pui8_scratch_bitstream_buffer;

	h261_gob_t s_saved_gob;                 /* committed GOB state */
	h261_mb_cache_t *ps_saved_cache;        /* committed motion cache */
	UInt8 *pui8_saved_recon_y;
	UInt8 *pui8_saved_recon_cb;
	UInt8 *pui8_saved_recon_cr;

	h261_gob_snapshot_t s_fit_snapshot;     /* best buffer-fitting candidate */
	h261_gob_snapshot_t s_overflow_snapshot;/* least-bit overflow candidate */

	Int32 i_allocated;
} h261_gob_budget_workspace_t;


typedef struct {
	Int32 i_gquant;
	Int64 i_bits;
	Int64 i_sse;
} h261_gob_candidate_t;


Int32 h261_gob_budget_active( h261_context_t *ps_ctx )
{
	return ps_ctx->s_gob_budget.i_enabled != 0;
}


static Void h261_gob_budget_copy_recon( h261_context_t *ps_ctx, h261_gob_budget_workspace_t *ps_ws,
	UInt8 *pui8_dst_y, UInt8 *pui8_dst_cb, UInt8 *pui8_dst_cr, Int32 i_offset_y )
{
	h261_frame_t *ps_recon;

	ps_recon = &ps_ctx->s_reconstructed_frame;
	memcpy( pui8_dst_y, ps_recon->pui8_Y + i_offset_y * ps_ws->i_stride_y, ps_ws->sz_gob_y );
	memcpy( pui8_dst_cb, ps_recon->pui8_Cb + ( i_offset_y / 2 ) * ps_ws->i_stride_c, ps_ws->sz_gob_c );
	memcpy( pui8_dst_cr, ps_recon->pui8_Cr + ( i_offset_y / 2 ) * ps_ws->i_stride_c, ps_ws->sz_gob_c );
}


static Void h261_gob_budget_restore_recon( h261_context_t *ps_ctx, h261_gob_budget_workspace_t *ps_ws,
	UInt8 *pui8_src_y, UInt8 *pui8_src_cb, UInt8 *pui8_src_cr, Int32 i_offset_y )
{
	h261_frame_t *ps_recon;

	ps_recon = &ps_ctx->s_reconstructed_frame;
	memcpy( ps_recon->pui8_Y + i_offset_y * ps_ws->i_stride_y, pui8_src_y, ps_ws->sz_gob_y );
	memcpy( ps_recon->pui8_Cb + ( i_offset_y / 2 ) * ps_ws->i_stride_c, pui8_src_cb, ps_ws->sz_gob_c );
	memcpy( ps_recon->pui8_Cr + ( i_offset_y / 2 ) * ps_ws->i_stride_c, pui8_src_cr, ps_ws->sz_gob_c );
}


static Void h261_gob_budget_free_snapshot( h261_gob_snapshot_t *ps_snap )
{
	h261_bitstream_deinit( &ps_snap->s_bitstream );
	free( ps_snap->pui8_bitstream_buffer );
	free( ps_snap->ps_cache );
	free( ps_snap->pui8_recon_y );
	free( ps_snap->pui8_recon_cb );
	free( ps_snap->pui8_recon_cr );
	memset( ps_snap, 0, sizeof( *ps_snap ) );
}


Void h261_gob_budget_deinit( h261_context_t *ps_ctx )
{
	h261_gob_budget_workspace_t *ps_ws;

	ps_ws = ( h261_gob_budget_workspace_t * ) ps_ctx->s_gob_budget.p_workspace;
	if( ps_ws == NULL )
	{
		return;
	}

	h261_bitstream_deinit( &ps_ws->s_scratch_bitstream );
	free( ps_ws->pui8_scratch_bitstream_buffer );

	free( ps_ws->ps_saved_cache );
	free( ps_ws->pui8_saved_recon_y );
	free( ps_ws->pui8_saved_recon_cb );
	free( ps_ws->pui8_saved_recon_cr );

	h261_gob_budget_free_snapshot( &ps_ws->s_fit_snapshot );
	h261_gob_budget_free_snapshot( &ps_ws->s_overflow_snapshot );

	free( ps_ws );
	ps_ctx->s_gob_budget.p_workspace = NULL;
}


static Int32 h261_gob_budget_alloc_snapshot( h261_gob_snapshot_t *ps_snap,
	size_t sz_cache, size_t sz_gob_y, size_t sz_gob_c )
{
	memset( ps_snap, 0, sizeof( *ps_snap ) );
	h261_bitstream_init( &ps_snap->s_bitstream, H261_GOB_BUDGET_BITSTREAM_CAP );
	ps_snap->pui8_bitstream_buffer = ps_snap->s_bitstream.pui8_bitstream;
	ps_snap->ps_cache = ( h261_mb_cache_t * ) malloc( sz_cache );
	ps_snap->pui8_recon_y = ( UInt8 * ) malloc( sz_gob_y );
	ps_snap->pui8_recon_cb = ( UInt8 * ) malloc( sz_gob_c );
	ps_snap->pui8_recon_cr = ( UInt8 * ) malloc( sz_gob_c );

	if( ps_snap->ps_cache == NULL || ps_snap->pui8_recon_y == NULL ||
		ps_snap->pui8_recon_cb == NULL || ps_snap->pui8_recon_cr == NULL )
	{
		h261_gob_budget_free_snapshot( ps_snap );
		return 0;
	}
	return 1;
}


static Int32 h261_gob_budget_alloc( h261_context_t *ps_ctx )
{
	h261_gob_budget_workspace_t *ps_ws;
	h261_frame_t *ps_frame;

	ps_ws = ( h261_gob_budget_workspace_t * ) calloc( 1, sizeof( h261_gob_budget_workspace_t ) );
	if( ps_ws == NULL )
	{
		return 0;
	}

	ps_frame = &ps_ctx->s_reconstructed_frame;
	ps_ws->i_stride_y = ps_frame->i_stride_y;
	ps_ws->i_stride_c = ps_frame->i_stride_c;
	/* one GOB spans 48 luma rows / 24 chroma rows, full frame stride */
	ps_ws->sz_gob_y = ( size_t ) ps_frame->i_stride_y * H261_GOB_HEIGHT;
	ps_ws->sz_gob_c = ( size_t ) ps_frame->i_stride_c * ( H261_GOB_HEIGHT / 2 );
	ps_ws->sz_cache = ( size_t ) rgi16_mb_dim[ ps_ctx->i_source_format ][ 0 ] *
		rgi16_mb_dim[ ps_ctx->i_source_format ][ 1 ] * sizeof( h261_mb_cache_t );

	h261_bitstream_init( &ps_ws->s_scratch_bitstream, H261_GOB_BUDGET_BITSTREAM_CAP );
	ps_ws->pui8_scratch_bitstream_buffer = ps_ws->s_scratch_bitstream.pui8_bitstream;

	ps_ws->ps_saved_cache = ( h261_mb_cache_t * ) malloc( ps_ws->sz_cache );
	ps_ws->pui8_saved_recon_y = ( UInt8 * ) malloc( ps_ws->sz_gob_y );
	ps_ws->pui8_saved_recon_cb = ( UInt8 * ) malloc( ps_ws->sz_gob_c );
	ps_ws->pui8_saved_recon_cr = ( UInt8 * ) malloc( ps_ws->sz_gob_c );

	if( ps_ws->ps_saved_cache == NULL || ps_ws->pui8_saved_recon_y == NULL ||
		ps_ws->pui8_saved_recon_cb == NULL || ps_ws->pui8_saved_recon_cr == NULL ||
		!h261_gob_budget_alloc_snapshot( &ps_ws->s_fit_snapshot, ps_ws->sz_cache,
			ps_ws->sz_gob_y, ps_ws->sz_gob_c ) ||
		!h261_gob_budget_alloc_snapshot( &ps_ws->s_overflow_snapshot, ps_ws->sz_cache,
			ps_ws->sz_gob_y, ps_ws->sz_gob_c ) )
	{
		h261_gob_budget_deinit( ps_ctx );
		return 0;
	}

	ps_ws->i_allocated = 1;
	ps_ctx->s_gob_budget.p_workspace = ps_ws;
	return 1;
}


Void h261_gob_budget_init_picture( h261_context_t *ps_ctx, h261_picture_parameters_t *ps_picture )
{
	h261_gob_budget_state_t *ps_state;

	ps_state = &ps_ctx->s_gob_budget;
	if( !ps_state->i_enabled )
	{
		return;
	}

	if( ps_state->p_workspace == NULL && !h261_gob_budget_alloc( ps_ctx ) )
	{
		/* scratch storage unavailable: drop back to the legacy GOB path */
		ps_state->i_enabled = 0;
		ps_ctx->s_ratectrl.i_ratectrl_mode = H261_RATECTRL_MODE_QUANT;
		return;
	}

	ps_state->i_frame_num = ps_picture->i_frame_num;
	ps_state->i_gob_index_in_frame = 0;
	/* writer position just before the picture header; its bits are billed to
	 * the first GOB of the picture */
	ps_state->i_frame_start_bits = h261_bitstream_bits( &ps_ctx->s_bitstream );

	/* the picture level quantiser is irrelevant: every GOB carries its own
	 * GQUANT chosen by the budget engine */
	ps_picture->i_quantiser = 1;
}


/* Point a private writer at its own buffer while inheriting the exact bit
 * position (including the sub-byte remainder) of the committed stream. */
static Void h261_gob_budget_attach_bitstream( h261_bitstream_t *ps_target,
	UInt8 *pui8_target_buffer, const h261_bitstream_t *ps_template )
{
	Int32 i_offset;

	i_offset = ( Int32 )( ps_template->pui8_codeword_ptr - ps_template->pui8_bitstream );

	ps_target->pui8_bitstream = pui8_target_buffer;
	ps_target->i_length = H261_GOB_BUDGET_BITSTREAM_CAP;
	ps_target->i_byte_count = ps_template->i_byte_count;
	ps_target->i_next_bit = ps_template->i_next_bit;
	ps_target->pui8_codeword_ptr = pui8_target_buffer + i_offset;
	ps_target->ui_codeword = ps_template->ui_codeword;
	ps_target->i_codeword_fill = ps_template->i_codeword_fill;
}


/* Freeze the candidate currently held in the scratch writer / context. */
static Void h261_gob_budget_capture_snapshot( h261_context_t *ps_ctx,
	h261_gob_budget_workspace_t *ps_ws, h261_gob_snapshot_t *ps_snap,
	const h261_bitstream_t *ps_start, const h261_bitstream_t *ps_scratch, Int32 i_offset_y )
{
	Int32 i_start_offset, i_end_offset;

	i_start_offset = ( Int32 )( ps_start->pui8_codeword_ptr - ps_start->pui8_bitstream );
	i_end_offset = ( Int32 )( ps_scratch->pui8_codeword_ptr - ps_scratch->pui8_bitstream );

	/* duplicate the produced bytes into the snapshot's own buffer */
	memcpy( ps_snap->pui8_bitstream_buffer + i_start_offset,
		ps_ws->pui8_scratch_bitstream_buffer + i_start_offset,
		( size_t )( i_end_offset - i_start_offset ) );

	ps_snap->s_bitstream = *ps_scratch;
	ps_snap->s_bitstream.pui8_bitstream = ps_snap->pui8_bitstream_buffer;
	ps_snap->s_bitstream.pui8_codeword_ptr = ps_snap->pui8_bitstream_buffer + i_end_offset;

	ps_snap->s_gob = ps_ctx->s_picture.s_groups_of_blocks;
	ps_snap->i_offset_y = i_offset_y;
	memcpy( ps_snap->ps_cache, ps_ctx->ps_mb_cache, ps_ws->sz_cache );
	h261_gob_budget_copy_recon( ps_ctx, ps_ws,
		ps_snap->pui8_recon_y, ps_snap->pui8_recon_cb, ps_snap->pui8_recon_cr, i_offset_y );
}


/* Install a frozen snapshot as the committed state without re-encoding. */
static Void h261_gob_budget_commit_snapshot( h261_context_t *ps_ctx,
	h261_gob_budget_workspace_t *ps_ws, const h261_gob_snapshot_t *ps_snap,
	const h261_bitstream_t *ps_start )
{
	h261_bitstream_t *ps_committed;
	Int32 i_start_offset, i_end_offset;

	ps_committed = &ps_ctx->s_bitstream;
	i_start_offset = ( Int32 )( ps_start->pui8_codeword_ptr - ps_start->pui8_bitstream );
	i_end_offset = ( Int32 )( ps_snap->s_bitstream.pui8_codeword_ptr - ps_snap->s_bitstream.pui8_bitstream );

	memcpy( ps_committed->pui8_bitstream + i_start_offset,
		ps_snap->pui8_bitstream_buffer + i_start_offset,
		( size_t )( i_end_offset - i_start_offset ) );

	ps_committed->pui8_codeword_ptr = ps_committed->pui8_bitstream + i_end_offset;
	ps_committed->ui_codeword = ps_snap->s_bitstream.ui_codeword;
	ps_committed->i_codeword_fill = ps_snap->s_bitstream.i_codeword_fill;
	ps_committed->i_byte_count = ps_snap->s_bitstream.i_byte_count;
	ps_committed->i_next_bit = ps_snap->s_bitstream.i_next_bit;

	ps_ctx->s_picture.s_groups_of_blocks = ps_snap->s_gob;
	memcpy( ps_ctx->ps_mb_cache, ps_snap->ps_cache, ps_ws->sz_cache );
	h261_gob_budget_restore_recon( ps_ctx, ps_ws,
		ps_snap->pui8_recon_y, ps_snap->pui8_recon_cb, ps_snap->pui8_recon_cr, ps_snap->i_offset_y );
}


static Int64 h261_gob_budget_plane_sse( UInt8 *pui8_src, Int32 i_src_stride,
	UInt8 *pui8_recon, Int32 i_recon_stride, Int32 i_width, Int32 i_height,
	Int32 i_offset_x, Int32 i_offset_y )
{
	Int32 i_x, i_y;
	Int64 i_sse;

	i_sse = 0;
	for( i_y = 0; i_y < i_height; i_y++ )
	{
		UInt8 *pui8_s = pui8_src + ( i_y + i_offset_y ) * i_src_stride + i_offset_x;
		UInt8 *pui8_r = pui8_recon + ( i_y + i_offset_y ) * i_recon_stride + i_offset_x;
		for( i_x = 0; i_x < i_width; i_x++ )
		{
			Int32 i_delta;
			i_delta = ( Int32 ) pui8_s[ i_x ] - ( Int32 ) pui8_r[ i_x ];
			i_sse += ( Int64 )( i_delta * i_delta );
		}
	}
	return i_sse;
}


static Int64 h261_gob_budget_gob_sse( h261_context_t *ps_ctx, Int32 i_offset_x, Int32 i_offset_y )
{
	h261_frame_t *ps_src, *ps_recon;
	Int64 i_sse;

	ps_src = &ps_ctx->s_current_frame;
	ps_recon = &ps_ctx->s_reconstructed_frame;

	i_sse = h261_gob_budget_plane_sse( ps_src->pui8_Y, ps_src->i_stride_y,
		ps_recon->pui8_Y, ps_recon->i_stride_y, H261_GOB_WIDTH, H261_GOB_HEIGHT,
		i_offset_x, i_offset_y );
	i_sse += h261_gob_budget_plane_sse( ps_src->pui8_Cb, ps_src->i_stride_c,
		ps_recon->pui8_Cb, ps_recon->i_stride_c, H261_GOB_WIDTH / 2, H261_GOB_HEIGHT / 2,
		i_offset_x / 2, i_offset_y / 2 );
	i_sse += h261_gob_budget_plane_sse( ps_src->pui8_Cr, ps_src->i_stride_c,
		ps_recon->pui8_Cr, ps_recon->i_stride_c, H261_GOB_WIDTH / 2, H261_GOB_HEIGHT / 2,
		i_offset_x / 2, i_offset_y / 2 );

	return i_sse;
}


/* Full legacy GOB pipeline (header, mode search, variance adjustment,
 * trellis, reconstruction) for one fixed base GQUANT. This is the single
 * real candidate encoding entry; the context writer and reconstruction
 * plane are prepared by the caller and restored afterwards. */
static Void h261_gob_budget_encode_one( h261_context_t *ps_ctx, Int32 i_gob_index,
	Int32 i_offset_x, Int32 i_offset_y, Int32 i_base_quant )
{
	static const Int32 rgi_macroblock_offsets[ 33 ][ 2 ] = {
		{ 0, 0 }, { 16, 0 }, { 32, 0 }, { 48, 0 }, { 64, 0 }, { 80, 0 },
		{ 96, 0 }, { 112, 0 }, { 128, 0 }, { 144, 0 }, { 160, 0 },
		{ 0, 16 }, { 16, 16 }, { 32, 16 }, { 48, 16 }, { 64, 16 }, { 80, 16 },
		{ 96, 16 }, { 112, 16 }, { 128, 16 }, { 144, 16 }, { 160, 16 },
		{ 0, 32 }, { 16, 32 }, { 32, 32 }, { 48, 32 }, { 64, 32 }, { 80, 32 },
		{ 96, 32 }, { 112, 32 }, { 128, 32 }, { 144, 32 }, { 160, 32 }
	};

	Int32 i_mb_stride, i_mb_height, i_mb_idx, i_mb_addr, i_mb_x, i_mb_y, i_coded;
	h261_picture_t *ps_picture;
	h261_gob_t *ps_gob;
	h261_macroblock_t *ps_mb;
	h261_mb_coding_vars_t *ps_coding_vars;

	ps_picture = &ps_ctx->s_picture;
	ps_gob = &ps_picture->s_groups_of_blocks;
	ps_mb = &ps_gob->s_macroblock;
	ps_coding_vars = &ps_gob->s_coding_vars;

	ps_gob->i_group_of_blocks_start_code = 0x0001;
	ps_gob->i_group_number = i_gob_index;
	ps_gob->i_quantiser = i_base_quant;
	ps_gob->i_num_extra_insertion_information = 0;

	ps_coding_vars->i_quantiser = i_base_quant;
	ps_coding_vars->i_last_mv_valid = 0;

	h261_write_gob_header( ps_ctx );

	i_mb_addr = 0;
	for( i_mb_idx = 1; i_mb_idx < 34; i_mb_idx++ )
	{
		i_mb_addr++;

		i_mb_x = rgi_macroblock_offsets[ i_mb_idx - 1 ][ 0 ] + i_offset_x;
		i_mb_y = rgi_macroblock_offsets[ i_mb_idx - 1 ][ 1 ] + i_offset_y;

		if( i_mb_idx == 1 || i_mb_idx == 12 || i_mb_idx == 23 )
		{
			ps_coding_vars->i_last_mv_valid = 0;
		}

		ps_mb->i_mb_x = i_mb_x;
		ps_mb->i_mb_y = i_mb_y;

		i_mb_x >>= 4;
		i_mb_y >>= 4;
		i_mb_stride = rgi16_mb_dim[ ps_ctx->i_source_format ][ 0 ];
		i_mb_height = rgi16_mb_dim[ ps_ctx->i_source_format ][ 1 ];

		ps_mb->ps_mb_cache = &ps_ctx->ps_mb_cache[ i_mb_x + i_mb_y * i_mb_stride ];

		memset( ps_mb->rgps_neighbours, 0, sizeof( ps_mb->rgps_neighbours ) );
		if( i_mb_x - 1 >= 0 )
		{
			ps_mb->rgps_neighbours[ H261_LEFT ] = ps_mb->ps_mb_cache - 1;
		}
		if( i_mb_x + 1 < i_mb_stride )
		{
			ps_mb->rgps_neighbours[ H261_RIGHT ] = ps_mb->ps_mb_cache + 1;
		}
		if( i_mb_y - 1 >= 0 )
		{
			ps_mb->rgps_neighbours[ H261_TOP ] = ps_mb->ps_mb_cache - i_mb_stride;
		}
		if( i_mb_y + 1 < i_mb_height )
		{
			ps_mb->rgps_neighbours[ H261_BOTTOM ] = ps_mb->ps_mb_cache + i_mb_stride;
		}

		i_coded = h261_encode_macroblock( ps_ctx );
		if( i_coded )
		{
			h261_write_macroblock( ps_ctx, i_mb_addr );
			i_mb_addr = 0;

			if( ps_mb->i_macroblock_type_flags & H261_MB_COEFF )
			{
				ps_coding_vars->i_quantiser = ps_mb->i_macroblock_quant;
			}

			if( ps_mb->i_macroblock_type_flags & H261_MB_MC )
			{
				ps_coding_vars->i_last_mv_x = ps_mb->rgi_mv[ 0 ];
				ps_coding_vars->i_last_mv_y = ps_mb->rgi_mv[ 1 ];
				ps_coding_vars->i_last_mv_valid = 1;
			}
			else
			{
				ps_coding_vars->i_last_mv_valid = 0;
			}
		}
		else
		{
			ps_coding_vars->i_last_mv_valid = 0;
		}
		h261_reconstruct_macroblock( ps_ctx );
	}
}


static Int32 h261_gob_budget_fit_better( const h261_gob_candidate_t *ps_best,
	const h261_gob_candidate_t *ps_new )
{
	/* fitting candidates: minimum SSE, then fewer bits, then smaller quant */
	if( ps_new->i_sse != ps_best->i_sse )
	{
		return ps_new->i_sse < ps_best->i_sse;
	}
	if( ps_new->i_bits != ps_best->i_bits )
	{
		return ps_new->i_bits < ps_best->i_bits;
	}
	return ps_new->i_gquant < ps_best->i_gquant;
}


static Int32 h261_gob_budget_overflow_better( const h261_gob_candidate_t *ps_best,
	const h261_gob_candidate_t *ps_new )
{
	/* nothing fits: fewest bits, then minimum SSE, then smaller quant */
	if( ps_new->i_bits != ps_best->i_bits )
	{
		return ps_new->i_bits < ps_best->i_bits;
	}
	if( ps_new->i_sse != ps_best->i_sse )
	{
		return ps_new->i_sse < ps_best->i_sse;
	}
	return ps_new->i_gquant < ps_best->i_gquant;
}


Void h261_gob_budget_encode_gob( h261_context_t *ps_ctx, Int32 i_gob_index,
	Int32 i_offset_x, Int32 i_offset_y )
{
	h261_gob_budget_state_t *ps_state;
	h261_gob_budget_workspace_t *ps_ws;
	h261_bitstream_t s_committed_bitstream;
	h261_bitstream_t s_start_bitstream;
	h261_bitstream_t *ps_scratch;
	h261_gob_t *ps_gob;
	h261_gob_candidate_t s_candidate, s_best_fit, s_best_overflow;
	Int32 i_quant, i_have_fit, i_have_overflow, i_overflow, i_start_bits, i_end_bits;
	Int32 i_header_bits, i_real_start_bits;
	Int64 i_quota, i_target_total, i_gob_bits, i_gob_sse, i_debt_after;
	Int64 i_num, i_den, i_step, i_new_remain;

	ps_state = &ps_ctx->s_gob_budget;
	ps_ws = ( h261_gob_budget_workspace_t * ) ps_state->p_workspace;
	ps_gob = &ps_ctx->s_picture.s_groups_of_blocks;
	ps_scratch = &ps_ws->s_scratch_bitstream;

	/* snapshot the committed state every candidate starts from */
	s_committed_bitstream = ps_ctx->s_bitstream;
	s_start_bitstream = ps_ctx->s_bitstream;
	ps_ws->s_saved_gob = *ps_gob;
	memcpy( ps_ws->ps_saved_cache, ps_ctx->ps_mb_cache, ps_ws->sz_cache );
	h261_gob_budget_copy_recon( ps_ctx, ps_ws,
		ps_ws->pui8_saved_recon_y, ps_ws->pui8_saved_recon_cb, ps_ws->pui8_saved_recon_cr, i_offset_y );

	i_real_start_bits = h261_bitstream_bits( &ps_ctx->s_bitstream );
	i_header_bits = ( ps_state->i_gob_index_in_frame == 0 )
		? ( i_real_start_bits - ps_state->i_frame_start_bits ) : 0;

	/* exact integer budget clock:
	 * B(K) = floor(K*num/den), quota q(K) = floor((r(K-1)+num)/den),
	 * r(K) = r(K-1) + num - q(K)*den, remainder carried forever */
	i_num = ps_state->i_budget_num;
	i_den = ps_state->i_budget_den;
	i_step = ( ps_state->i_budget_remain + i_num ) / i_den;
	i_quota = i_step;
	i_target_total = ps_state->i_target_total + i_quota;
	i_new_remain = ps_state->i_budget_remain + i_num - i_quota * i_den;

	i_have_fit = 0;
	i_have_overflow = 0;
	ps_state->i_trials = 0;

	for( i_quant = 1; i_quant <= H261_GOB_BUDGET_NUM_QUANTS; i_quant++ )
	{
		/* every candidate restarts from the identical committed state so a
		 * previous trial cannot pollute the motion cache, quantiser
		 * inheritance, bitstream remainder or reconstruction */
		ps_ctx->s_bitstream = s_committed_bitstream;
		*ps_gob = ps_ws->s_saved_gob;
		memcpy( ps_ctx->ps_mb_cache, ps_ws->ps_saved_cache, ps_ws->sz_cache );
		h261_gob_budget_restore_recon( ps_ctx, ps_ws,
			ps_ws->pui8_saved_recon_y, ps_ws->pui8_saved_recon_cb, ps_ws->pui8_saved_recon_cr, i_offset_y );

		h261_gob_budget_attach_bitstream( ps_scratch,
			ps_ws->pui8_scratch_bitstream_buffer, &s_start_bitstream );
		/* route the real writer through the scratch buffer for this trial */
		ps_ctx->s_bitstream = *ps_scratch;
		i_start_bits = h261_bitstream_bits( &ps_ctx->s_bitstream );

		ps_state->i_trials++;
		h261_gob_budget_encode_one( ps_ctx, i_gob_index, i_offset_x, i_offset_y, i_quant );

		i_end_bits = h261_bitstream_bits( &ps_ctx->s_bitstream );
		/* effective bits: this GOB's own bits, plus the picture header that
		 * is billed to the first GOB of the picture */
		i_gob_bits = ( Int64 )( i_end_bits - i_start_bits ) + ( Int64 ) i_header_bits;
		i_gob_sse = h261_gob_budget_gob_sse( ps_ctx, i_offset_x, i_offset_y );

		s_candidate.i_gquant = i_quant;
		s_candidate.i_bits = i_gob_bits;
		s_candidate.i_sse = i_gob_sse;

		if( ps_state->i_debt + i_gob_bits - i_quota <= ( Int64 ) ps_state->i_buffer_bits )
		{
			if( !i_have_fit || h261_gob_budget_fit_better( &s_best_fit, &s_candidate ) )
			{
				s_best_fit = s_candidate;
				i_have_fit = 1;
				h261_gob_budget_capture_snapshot( ps_ctx, ps_ws,
					&ps_ws->s_fit_snapshot, &s_start_bitstream, &ps_ctx->s_bitstream, i_offset_y );
			}
		}
		else
		{
			if( !i_have_overflow || h261_gob_budget_overflow_better( &s_best_overflow, &s_candidate ) )
			{
				s_best_overflow = s_candidate;
				i_have_overflow = 1;
				h261_gob_budget_capture_snapshot( ps_ctx, ps_ws,
					&ps_ws->s_overflow_snapshot, &s_start_bitstream, &ps_ctx->s_bitstream, i_offset_y );
			}
		}
	}

	if( i_have_fit )
	{
		s_candidate = s_best_fit;
		i_overflow = 0;
	}
	else
	{
		s_candidate = s_best_overflow;
		i_overflow = 1;
	}

	/* settle the budget using the committed candidate; trials never moved
	 * the official budget and no callback fired for them */
	ps_state->i_gob_counter += 1;
	ps_state->i_coded_total += s_candidate.i_bits;
	ps_state->i_target_total = i_target_total;
	ps_state->i_budget_remain = i_new_remain;
	ps_state->i_debt += s_candidate.i_bits - i_quota;
	i_debt_after = ps_state->i_debt;

	/* install the already generated content/state of the chosen candidate
	 * only; it is not encoded a second time */
	ps_ctx->s_bitstream = s_committed_bitstream;
	if( !i_overflow )
	{
		h261_gob_budget_commit_snapshot( ps_ctx, ps_ws,
			&ps_ws->s_fit_snapshot, &s_start_bitstream );
	}
	else
	{
		/* still emit a complete, legal GOB: commit the least-bit candidate */
		h261_gob_budget_commit_snapshot( ps_ctx, ps_ws,
			&ps_ws->s_overflow_snapshot, &s_start_bitstream );
	}

	ps_state->i_gob_number = i_gob_index;
	ps_state->i_gob_index_in_frame += 1;

	if( ps_state->pf_observe != NULL )
	{
		h261_gob_budget_stats_t s_stats;

		s_stats.i_frame_num = ps_state->i_frame_num;
		s_stats.i_gob_number = i_gob_index;
		s_stats.i_gquant = s_candidate.i_gquant;
		s_stats.i_overflow = i_overflow;
		s_stats.i_trials = ps_state->i_trials;
		s_stats.i_bits = s_candidate.i_bits;
		s_stats.i_quota = i_quota;
		s_stats.i_coded_total = ps_state->i_coded_total;
		s_stats.i_target_total = i_target_total;
		s_stats.i_debt_after = i_debt_after;
		s_stats.i_sse = s_candidate.i_sse;

		ps_state->pf_observe( &s_stats, ps_state->p_opaque );
	}
}
