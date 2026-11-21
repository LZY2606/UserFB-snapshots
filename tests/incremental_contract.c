/*
Contract tests for the incremental (pausable, macroblock-stepping)
H.261 encoding interface in src/h261/h261_incremental.h.

Every scenario encodes real QCIF/CIF I/P sequences from locally
generated non-uniform still and moving pictures and is checked against
an independent whole-frame run of the classic path (fixture_encode):
identical bytes, identical reconstructed planes, identical reference
planes and identical rate-control state.
*/

#include "check.h"
#include "fixture.h"
#include "h261_incremental.h"

#define NFRAME_QCIF 4
#define NFRAME_CIF 3
#define MAXFRAME 4

/* ---------------------------------------------------------------- */
/* growing byte buffer                                              */

typedef struct {
	UInt8 *pui8;
	UInt32 ui_len;
	UInt32 ui_cap;
} sbuf_t;

static void sbuf_put( sbuf_t *ps_buf, const UInt8 *pui8_data, UInt32 ui_len )
{
	if( ps_buf->ui_len + ui_len > ps_buf->ui_cap )
	{
		ps_buf->ui_cap = ( ps_buf->ui_len + ui_len ) * 2 + 64;
		ps_buf->pui8 = ( UInt8 * ) realloc( ps_buf->pui8, ps_buf->ui_cap );
	}
	memcpy( ps_buf->pui8 + ps_buf->ui_len, pui8_data, ui_len );
	ps_buf->ui_len += ui_len;
}

static void sbuf_free( sbuf_t *ps_buf )
{
	free( ps_buf->pui8 );
	ps_buf->pui8 = 0;
	ps_buf->ui_len = ps_buf->ui_cap = 0;
}

/* ---------------------------------------------------------------- */
/* plane snapshots                                                  */

typedef struct {
	UInt8 *pui8_y;
	UInt8 *pui8_cb;
	UInt8 *pui8_cr;
	size_t i_luma;
	size_t i_chroma;
} planes_t;

static void planes_snap( planes_t *ps_planes, const h261_frame_t *ps_frame )
{
	ps_planes->i_luma = ( size_t ) ps_frame->i_width * ps_frame->i_height;
	ps_planes->i_chroma = ps_planes->i_luma / 4;
	ps_planes->pui8_y = ( UInt8 * ) malloc( ps_planes->i_luma );
	ps_planes->pui8_cb = ( UInt8 * ) malloc( ps_planes->i_chroma );
	ps_planes->pui8_cr = ( UInt8 * ) malloc( ps_planes->i_chroma );
	memcpy( ps_planes->pui8_y, ps_frame->pui8_Y, ps_planes->i_luma );
	memcpy( ps_planes->pui8_cb, ps_frame->pui8_Cb, ps_planes->i_chroma );
	memcpy( ps_planes->pui8_cr, ps_frame->pui8_Cr, ps_planes->i_chroma );
}

static int planes_match( const planes_t *ps_planes, const h261_frame_t *ps_frame )
{
	return !memcmp( ps_planes->pui8_y, ps_frame->pui8_Y, ps_planes->i_luma ) &&
	       !memcmp( ps_planes->pui8_cb, ps_frame->pui8_Cb, ps_planes->i_chroma ) &&
	       !memcmp( ps_planes->pui8_cr, ps_frame->pui8_Cr, ps_planes->i_chroma );
}

static void planes_release( planes_t *ps_planes )
{
	free( ps_planes->pui8_y );
	free( ps_planes->pui8_cb );
	free( ps_planes->pui8_cr );
	ps_planes->pui8_y = ps_planes->pui8_cb = ps_planes->pui8_cr = 0;
}

/* ---------------------------------------------------------------- */
/* test material: non-uniform pictures, still and moving            */
/* frames 0..1 identical (still), frame 2 shifted (motion),         */
/* frame 3 identical to frame 2 (still again)                       */

static void fill_frame( h261_context_t *ps_ctx, int i_frame )
{
	h261_frame_t *ps_frame = &ps_ctx->s_current_frame;
	int i_shift_x = i_frame >= 2 ? 6 : 0;
	int i_shift_y = i_frame >= 2 ? 3 : 0;
	int x, y;

	for( y = 0; y < ps_frame->i_height; y++ )
	{
		for( x = 0; x < ps_frame->i_width; x++ )
		{
			int sx = x + i_shift_x;
			int sy = y + i_shift_y;
			int v;
			if( sx >= ps_frame->i_width ) sx = 2 * ps_frame->i_width - 1 - sx;
			if( sy >= ps_frame->i_height ) sy = 2 * ps_frame->i_height - 1 - sy;
			/* non-uniform: per-block plateaus plus a mild ramp */
			v = 32 + ( ( sx / 16 ) * 13 + ( sy / 16 ) * 7 + ( sx % 8 ) ) % 192;
			ps_frame->pui8_Y[ y * ps_frame->i_stride_y + x ] = ( UInt8 ) v;
		}
	}
	for( y = 0; y < ps_frame->i_height / 2; y++ )
	{
		for( x = 0; x < ps_frame->i_width / 2; x++ )
		{
			int sx = x + i_shift_x / 2;
			int sy = y + i_shift_y / 2;
			if( sx >= ps_frame->i_width / 2 ) sx = ps_frame->i_width - 1 - sx;
			if( sy >= ps_frame->i_height / 2 ) sy = ps_frame->i_height - 1 - sy;
			ps_frame->pui8_Cb[ y * ps_frame->i_stride_c + x ] =
				( UInt8 )( 96 + ( ( sx / 8 ) * 5 + ( sy / 8 ) * 3 ) % 48 );
			ps_frame->pui8_Cr[ y * ps_frame->i_stride_c + x ] =
				( UInt8 )( 112 + ( ( sx / 8 ) * 7 + ( sy / 8 ) * 11 ) % 40 );
		}
	}
}

/* ---------------------------------------------------------------- */
/* independent whole-frame reference run (classic path)             */

static void old_run( h261_context_t *ps_ctx, int i_frames, sbuf_t *ps_stream,
                     planes_t *ps_recon, planes_t *ps_ref, UInt32 *pui_frame_bytes,
                     h261_ratectrl_t *ps_rc )
{
	FILE *pf = tmpfile();
	int i_frame;

	for( i_frame = 0; i_frame < i_frames; i_frame++ )
	{
		long i_before;
		fill_frame( ps_ctx, i_frame );
		i_before = ftell( pf );
		fixture_encode( ps_ctx, i_frame, pf );
		pui_frame_bytes[ i_frame ] = ( UInt32 )( ftell( pf ) - i_before );
		planes_snap( &ps_recon[ i_frame ], &ps_ctx->s_reconstructed_frame );
		planes_snap( &ps_ref[ i_frame ], &ps_ctx->s_reference_frame );
		if( ps_rc )
		{
			ps_rc[ i_frame ] = ps_ctx->s_ratectrl;
		}
	}
	{
		UInt8 *pui8_tail;
		UInt32 ui_tail;
		long i_total;
		h261_bitstream_flush( &ps_ctx->s_bitstream, &pui8_tail, &ui_tail );
		fwrite( pui8_tail, 1, ui_tail, pf );
		fflush( pf );
		i_total = ftell( pf );
		rewind( pf );
		ps_stream->pui8 = ( UInt8 * ) malloc( ( size_t ) i_total );
		ps_stream->ui_len = ( UInt32 ) fread( ps_stream->pui8, 1, ( size_t ) i_total, pf );
		ps_stream->ui_cap = ps_stream->ui_len;
	}
	fclose( pf );
}

/* ---------------------------------------------------------------- */
/* incremental helpers                                              */

static int inc_begin_frame( h261_incremental_t *ps_inc, h261_context_t *ps_ctx, int i_frame )
{
	h261_picture_parameters_t s_params;
	s_params.i_frame_num = i_frame;
	s_params.i_source_format = ps_ctx->i_source_format;
	s_params.i_frame_type = i_frame ? H261_FRAME_TYPE_INTER : H261_FRAME_TYPE_INTRA;
	s_params.i_quantiser = 10;
	return h261inc_begin_picture( ps_inc, &s_params );
}

static void pull_all( h261_incremental_t *ps_inc, sbuf_t *ps_stream )
{
	UInt8 rgui8[ 512 ];
	UInt32 ui_got;
	while( ( ui_got = h261inc_pull( ps_inc, rgui8, sizeof( rgui8 ) ) ) > 0 )
	{
		sbuf_put( ps_stream, rgui8, ui_got );
	}
}

static int stream_equal( const sbuf_t *ps_a, const sbuf_t *ps_b )
{
	return ps_a->ui_len == ps_b->ui_len &&
	       ( ps_a->ui_len == 0 || !memcmp( ps_a->pui8, ps_b->pui8, ps_a->ui_len ) );
}

/* ================================================================ */
/* group: cursor                                                    */

/* budget-1 stepping, one byte pulled per step, QCIF I/P sequence */
static int case_cursor_budget_one_qcif( void )
{
	h261_context_t s_inc_ctx, s_old_ctx;
	sbuf_t s_inc_stream = { 0, 0, 0 }, s_old_stream = { 0, 0, 0 };
	planes_t ps_recon_old[ MAXFRAME ], ps_ref_old[ MAXFRAME ], s_ref_snap;
	UInt32 rgui_frame_bytes[ MAXFRAME ];
	h261_incremental_t *ps_inc;
	int i_frame, i_saw_carry;

	TASK_CHECK( fixture_init( &s_inc_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	TASK_CHECK( fixture_init( &s_old_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	old_run( &s_old_ctx, NFRAME_QCIF, &s_old_stream, ps_recon_old, ps_ref_old, rgui_frame_bytes, 0 );

	ps_inc = h261inc_create( &s_inc_ctx );
	TASK_CHECK( ps_inc != 0 );
	TASK_CHECK( h261inc_busy( ps_inc ) == 0 );

	i_saw_carry = 0;
	for( i_frame = 0; i_frame < NFRAME_QCIF; i_frame++ )
	{
		h261inc_step_t s_step;
		int i_expected;

		fill_frame( &s_inc_ctx, i_frame );
		planes_snap( &s_ref_snap, &s_inc_ctx.s_reference_frame );

		TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame ) == 0 );
		TASK_CHECK( h261inc_busy( ps_inc ) == 1 );
		TASK_CHECK( h261inc_committed( ps_inc ) == 0 );

		/* busy: a second begin fails and changes nothing */
		TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame ) != 0 );
		TASK_CHECK( h261inc_committed( ps_inc ) == 0 );
		TASK_CHECK( h261inc_busy( ps_inc ) == 1 );

		/* zero budget: no progress */
		TASK_CHECK( h261inc_step( ps_inc, 0, &s_step ) == 0 );
		TASK_CHECK( s_step.i_committed == 0 );
		TASK_CHECK( s_step.i_picture_done == 0 );
		TASK_CHECK( h261inc_committed( ps_inc ) == 0 );

		i_expected = 0;
		for( ;; )
		{
			UInt8 ui8;
			UInt32 ui_got;

			TASK_CHECK( h261inc_step( ps_inc, 1, &s_step ) == 0 );
			TASK_CHECK( s_step.i_committed == 0 || s_step.i_committed == 1 );
			i_expected += s_step.i_committed;
			/* real cursor, not a replay of return values */
			TASK_CHECK( h261inc_committed( ps_inc ) == i_expected );
			if( !s_step.i_picture_done )
			{
				/* reference plane frozen in the middle of a picture */
				TASK_CHECK( planes_match( &s_ref_snap, &s_inc_ctx.s_reference_frame ) );
			}
			ui_got = h261inc_pull( ps_inc, &ui8, 1 );
			TASK_CHECK( ui_got <= 1 );
			if( ui_got )
			{
				sbuf_put( &s_inc_stream, &ui8, 1 );
			}
			if( s_step.i_picture_done )
			{
				break;
			}
		}
		TASK_CHECK( i_expected == 99 ); /* 3 GOBs x 33 macroblocks */
		TASK_CHECK( h261inc_committed( ps_inc ) == 99 );
		if( h261_bitstream_bits( &s_inc_ctx.s_bitstream ) % 8 )
		{
			i_saw_carry = 1; /* residual bits cross the picture boundary */
		}
		pull_all( ps_inc, &s_inc_stream );
		TASK_CHECK( h261inc_pending( ps_inc ) == 0 );
		TASK_CHECK( planes_match( &ps_recon_old[ i_frame ], &s_inc_ctx.s_reconstructed_frame ) );
		TASK_CHECK( planes_match( &ps_ref_old[ i_frame ], &s_inc_ctx.s_reference_frame ) );
		planes_release( &s_ref_snap );
	}
	TASK_CHECK( i_saw_carry );

	TASK_CHECK( h261inc_end( ps_inc ) == 0 );
	pull_all( ps_inc, &s_inc_stream );
	{
		UInt8 ui8;
		TASK_CHECK( h261inc_pull( ps_inc, &ui8, 1 ) == 0 ); /* drained stays drained */
	}
	TASK_CHECK( stream_equal( &s_inc_stream, &s_old_stream ) );

	h261inc_destroy( ps_inc );
	sbuf_free( &s_inc_stream );
	sbuf_free( &s_old_stream );
	for( i_frame = 0; i_frame < NFRAME_QCIF; i_frame++ )
	{
		planes_release( &ps_recon_old[ i_frame ] );
		planes_release( &ps_ref_old[ i_frame ] );
	}
	fixture_free( &s_inc_ctx );
	fixture_free( &s_old_ctx );
	return 0;
}

/* irregular budgets on CIF, exact stops on the GOB boundary,
   skipped macroblocks and nonzero motion observed through the context */
static int case_cursor_irregular_budget_cif( void )
{
	static const int rgi_budgets[] = { 0, 1, 33, 7, 34, 100, 2, 396, 5, 13 };
	static const UInt32 rgui_caps[] = { 0, 1, 3, 4096, 17, 256, 91 };
	h261_context_t s_inc_ctx, s_old_ctx;
	sbuf_t s_inc_stream = { 0, 0, 0 }, s_old_stream = { 0, 0, 0 };
	planes_t ps_recon_old[ MAXFRAME ], ps_ref_old[ MAXFRAME ];
	UInt32 rgui_frame_bytes[ MAXFRAME ];
	h261_incremental_t *ps_inc;
	int i_frame, i_boundary_stop;

	TASK_CHECK( fixture_init( &s_inc_ctx, H261_SOURCE_FORMAT_CIF, H261_RATECTRL_MODE_QUANT ) );
	TASK_CHECK( fixture_init( &s_old_ctx, H261_SOURCE_FORMAT_CIF, H261_RATECTRL_MODE_QUANT ) );
	old_run( &s_old_ctx, NFRAME_CIF, &s_old_stream, ps_recon_old, ps_ref_old, rgui_frame_bytes, 0 );

	ps_inc = h261inc_create( &s_inc_ctx );
	TASK_CHECK( ps_inc != 0 );

	i_boundary_stop = 0;
	for( i_frame = 0; i_frame < NFRAME_CIF; i_frame++ )
	{
		h261inc_step_t s_step;
		int i_expected, i_bi, i_ci, i_skips, i_max_mv, i_done;

		fill_frame( &s_inc_ctx, i_frame );
		TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame ) == 0 );

		i_expected = 0;
		i_bi = 0;
		i_ci = 0;
		i_skips = 0;
		i_max_mv = 0;
		i_done = 0;

		/* land exactly on a 33rd-macroblock / GOB-header boundary */
		TASK_CHECK( h261inc_step( ps_inc, 1, &s_step ) == 0 );
		i_expected += s_step.i_committed;
		pull_all( ps_inc, &s_inc_stream );
		while( !s_step.i_picture_done && ( h261inc_committed( ps_inc ) % 33 ) != 0 )
		{
			int i_need = 33 - ( h261inc_committed( ps_inc ) % 33 );
			TASK_CHECK( h261inc_step( ps_inc, i_need, &s_step ) == 0 );
			i_expected += s_step.i_committed;
			pull_all( ps_inc, &s_inc_stream );
		}
		TASK_CHECK( h261inc_committed( ps_inc ) == i_expected );
		if( !s_step.i_picture_done )
		{
			TASK_CHECK( h261inc_committed( ps_inc ) % 33 == 0 );
			TASK_CHECK( h261inc_committed( ps_inc ) > 0 );
			i_boundary_stop = 1;
			/* a zero-budget pause on the boundary changes nothing */
			TASK_CHECK( h261inc_step( ps_inc, 0, &s_step ) == 0 );
			TASK_CHECK( s_step.i_committed == 0 );
		}

		while( !i_done )
		{
			int i_budget = rgi_budgets[ i_bi++ % 10 ];
			UInt32 ui_cap;
			UInt8 rgui8[ 4096 ];
			UInt32 ui_got;

			if( i_frame >= 1 )
			{
				i_budget = 1; /* sample every macroblock of the P frames */
			}
			TASK_CHECK( h261inc_step( ps_inc, i_budget, &s_step ) == 0 );
			TASK_CHECK( s_step.i_committed >= 0 && s_step.i_committed <= i_budget );
			if( i_budget == 0 )
			{
				TASK_CHECK( s_step.i_committed == 0 );
			}
			i_expected += s_step.i_committed;
			TASK_CHECK( h261inc_committed( ps_inc ) == i_expected );
			TASK_CHECK( i_expected <= 396 );

			if( s_step.i_committed > 0 )
			{
				/* observe the just-committed macroblock through the context */
				h261_macroblock_t *ps_mb = &s_inc_ctx.s_picture.s_groups_of_blocks.s_macroblock;
				if( i_frame == 1 && ps_mb->i_macroblock_type_flags == 0 )
				{
					i_skips++;
				}
				if( i_frame == 2 )
				{
					int i_amv = H261_ABS( ps_mb->rgi_mv[ 0 ] ) + H261_ABS( ps_mb->rgi_mv[ 1 ] );
					if( i_amv > i_max_mv )
					{
						i_max_mv = i_amv;
					}
				}
			}

			ui_cap = rgui_caps[ i_ci++ % 7 ];
			ui_got = h261inc_pull( ps_inc, rgui8, ui_cap );
			TASK_CHECK( ui_got <= ui_cap );
			sbuf_put( &s_inc_stream, rgui8, ui_got );

			if( s_step.i_picture_done )
			{
				i_done = 1;
			}
		}
		TASK_CHECK( i_expected == 396 ); /* 12 GOBs x 33 macroblocks */
		TASK_CHECK( h261inc_committed( ps_inc ) == 396 );
		if( i_frame == 1 )
		{
			TASK_CHECK( i_skips > 0 ); /* still picture really skipped blocks */
		}
		if( i_frame == 2 )
		{
			TASK_CHECK( i_max_mv > 0 ); /* moving picture really has motion */
		}
		pull_all( ps_inc, &s_inc_stream );
		TASK_CHECK( planes_match( &ps_recon_old[ i_frame ], &s_inc_ctx.s_reconstructed_frame ) );
		TASK_CHECK( planes_match( &ps_ref_old[ i_frame ], &s_inc_ctx.s_reference_frame ) );
	}
	TASK_CHECK( i_boundary_stop );

	TASK_CHECK( h261inc_end( ps_inc ) == 0 );
	pull_all( ps_inc, &s_inc_stream );
	TASK_CHECK( stream_equal( &s_inc_stream, &s_old_stream ) );

	h261inc_destroy( ps_inc );
	sbuf_free( &s_inc_stream );
	sbuf_free( &s_old_stream );
	for( i_frame = 0; i_frame < NFRAME_CIF; i_frame++ )
	{
		planes_release( &ps_recon_old[ i_frame ] );
		planes_release( &ps_ref_old[ i_frame ] );
	}
	fixture_free( &s_inc_ctx );
	fixture_free( &s_old_ctx );
	return 0;
}

/* ================================================================ */
/* group: output                                                    */

/* pull semantics: zero capacity, single bytes, drained stays drained,
   bytes really stream out before the picture completes */
static int case_output_pull_capacity_patterns( void )
{
	static const int rgi_budgets[] = { 2, 9, 1, 40 };
	h261_context_t s_inc_ctx, s_old_ctx;
	sbuf_t s_inc_stream = { 0, 0, 0 }, s_old_stream = { 0, 0, 0 };
	planes_t ps_recon_old[ MAXFRAME ], ps_ref_old[ MAXFRAME ];
	UInt32 rgui_frame_bytes[ MAXFRAME ];
	h261_incremental_t *ps_inc;
	int i_frame, i_bi;
	UInt32 ui_streamed_before_first_done;

	TASK_CHECK( fixture_init( &s_inc_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	TASK_CHECK( fixture_init( &s_old_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	old_run( &s_old_ctx, NFRAME_QCIF, &s_old_stream, ps_recon_old, ps_ref_old, rgui_frame_bytes, 0 );

	ps_inc = h261inc_create( &s_inc_ctx );
	TASK_CHECK( ps_inc != 0 );

	i_bi = 0;
	ui_streamed_before_first_done = 0;
	for( i_frame = 0; i_frame < NFRAME_QCIF; i_frame++ )
	{
		h261inc_step_t s_step;
		int i_done = 0;

		fill_frame( &s_inc_ctx, i_frame );
		TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame ) == 0 );

		while( !i_done )
		{
			UInt8 rgui8[ 4 ];
			UInt32 ui_pending_before, ui_got;

			TASK_CHECK( h261inc_step( ps_inc, rgi_budgets[ i_bi++ % 4 ], &s_step ) == 0 );

			/* zero capacity consumes nothing */
			ui_pending_before = h261inc_pending( ps_inc );
			TASK_CHECK( h261inc_pull( ps_inc, rgui8, 0 ) == 0 );
			TASK_CHECK( h261inc_pending( ps_inc ) == ui_pending_before );

			/* one byte at a time, in encoding order */
			ui_got = h261inc_pull( ps_inc, rgui8, 1 );
			TASK_CHECK( ui_got <= 1 );
			TASK_CHECK( h261inc_pending( ps_inc ) == ui_pending_before - ui_got );
			sbuf_put( &s_inc_stream, rgui8, ui_got );

			if( s_step.i_picture_done )
			{
				if( i_frame == 0 )
				{
					ui_streamed_before_first_done = s_inc_stream.ui_len;
				}
				i_done = 1;
			}
		}
		pull_all( ps_inc, &s_inc_stream );
		TASK_CHECK( planes_match( &ps_recon_old[ i_frame ], &s_inc_ctx.s_reconstructed_frame ) );
	}
	/* output really streamed out before the first picture completed */
	TASK_CHECK( ui_streamed_before_first_done > 0 );

	TASK_CHECK( h261inc_end( ps_inc ) == 0 );
	pull_all( ps_inc, &s_inc_stream );
	{
		UInt8 rgui8[ 8 ];
		/* repeated pulls of an empty buffer deliver nothing */
		TASK_CHECK( h261inc_pull( ps_inc, rgui8, sizeof( rgui8 ) ) == 0 );
		TASK_CHECK( h261inc_pull( ps_inc, rgui8, sizeof( rgui8 ) ) == 0 );
		/* repeated end produces no further data */
		TASK_CHECK( h261inc_end( ps_inc ) == 0 );
		TASK_CHECK( h261inc_pull( ps_inc, rgui8, sizeof( rgui8 ) ) == 0 );
		/* begin is rejected after end */
		TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, 0 ) != 0 );
		TASK_CHECK( h261inc_ended( ps_inc ) == 1 );
	}
	TASK_CHECK( stream_equal( &s_inc_stream, &s_old_stream ) );

	h261inc_destroy( ps_inc );
	sbuf_free( &s_inc_stream );
	sbuf_free( &s_old_stream );
	for( i_frame = 0; i_frame < NFRAME_QCIF; i_frame++ )
	{
		planes_release( &ps_recon_old[ i_frame ] );
		planes_release( &ps_ref_old[ i_frame ] );
	}
	fixture_free( &s_inc_ctx );
	fixture_free( &s_old_ctx );
	return 0;
}

/* VBR sequence with irregular budgets and pull capacities; a picture
   only starts after the previous output is fully drained */
static int case_output_irregular_pulls_vbr( void )
{
	static const int rgi_budgets[] = { 3, 0, 11, 1, 50, 6 };
	static const UInt32 rgui_caps[] = { 1, 0, 7, 64, 300, 2, 13 };
	h261_context_t s_inc_ctx, s_old_ctx;
	sbuf_t s_inc_stream = { 0, 0, 0 }, s_old_stream = { 0, 0, 0 };
	planes_t ps_recon_old[ MAXFRAME ], ps_ref_old[ MAXFRAME ];
	UInt32 rgui_frame_bytes[ MAXFRAME ];
	h261_ratectrl_t ps_rc_old[ MAXFRAME ];
	h261_incremental_t *ps_inc;
	int i_frame, i_bi, i_ci;

	TASK_CHECK( fixture_init( &s_inc_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_VBR ) );
	TASK_CHECK( fixture_init( &s_old_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_VBR ) );
	old_run( &s_old_ctx, NFRAME_QCIF, &s_old_stream, ps_recon_old, ps_ref_old, rgui_frame_bytes, ps_rc_old );

	ps_inc = h261inc_create( &s_inc_ctx );
	TASK_CHECK( ps_inc != 0 );

	i_bi = 0;
	i_ci = 0;
	for( i_frame = 0; i_frame < NFRAME_QCIF; i_frame++ )
	{
		h261inc_step_t s_step;
		int i_done = 0;

		fill_frame( &s_inc_ctx, i_frame );
		TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame ) == 0 );

		while( !i_done )
		{
			UInt8 rgui8[ 320 ];
			UInt32 ui_cap, ui_got;

			TASK_CHECK( h261inc_step( ps_inc, rgi_budgets[ i_bi++ % 6 ], &s_step ) == 0 );
			ui_cap = rgui_caps[ i_ci++ % 7 ];
			ui_got = h261inc_pull( ps_inc, rgui8, ui_cap );
			TASK_CHECK( ui_got <= ui_cap );
			sbuf_put( &s_inc_stream, rgui8, ui_got );
			if( s_step.i_picture_done )
			{
				i_done = 1;
			}
		}

		/* end is rejected while a picture is in progress? no: picture
		   is done here, but unconsumed output must block the next begin */
		{
			UInt8 rgui8[ 5 ];
			UInt32 ui_got = h261inc_pull( ps_inc, rgui8, sizeof( rgui8 ) );
			UInt32 ui_left;
			sbuf_put( &s_inc_stream, rgui8, ui_got );
			ui_left = h261inc_pending( ps_inc );
			if( ui_left > 0 )
			{
				TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame + 1 ) != 0 );
				TASK_CHECK( h261inc_busy( ps_inc ) == 0 );
				TASK_CHECK( h261inc_committed( ps_inc ) == 99 );
				TASK_CHECK( h261inc_pending( ps_inc ) == ui_left );
			}
			pull_all( ps_inc, &s_inc_stream );
		}
		TASK_CHECK( h261inc_pending( ps_inc ) == 0 );

		/* rate control marched in lockstep with the classic path */
		TASK_CHECK( s_inc_ctx.s_ratectrl.i_current_quant == ps_rc_old[ i_frame ].i_current_quant );
		TASK_CHECK( s_inc_ctx.s_ratectrl.f_actual_bits == ps_rc_old[ i_frame ].f_actual_bits );
		TASK_CHECK( s_inc_ctx.s_ratectrl.f_wanted_bits == ps_rc_old[ i_frame ].f_wanted_bits );
		TASK_CHECK( s_inc_ctx.s_ratectrl.f_bits_quant == ps_rc_old[ i_frame ].f_bits_quant );
		TASK_CHECK( planes_match( &ps_recon_old[ i_frame ], &s_inc_ctx.s_reconstructed_frame ) );
		TASK_CHECK( planes_match( &ps_ref_old[ i_frame ], &s_inc_ctx.s_reference_frame ) );
	}

	TASK_CHECK( h261inc_end( ps_inc ) == 0 );
	pull_all( ps_inc, &s_inc_stream );
	TASK_CHECK( stream_equal( &s_inc_stream, &s_old_stream ) );

	h261inc_destroy( ps_inc );
	sbuf_free( &s_inc_stream );
	sbuf_free( &s_old_stream );
	for( i_frame = 0; i_frame < NFRAME_QCIF; i_frame++ )
	{
		planes_release( &ps_recon_old[ i_frame ] );
		planes_release( &ps_ref_old[ i_frame ] );
	}
	fixture_free( &s_inc_ctx );
	fixture_free( &s_old_ctx );
	return 0;
}

/* ================================================================ */
/* group: commit                                                    */

/* the reference plane and the VBR update move exactly once per
   picture: at the commit of its last macroblock */
static int case_commit_updates_once_vbr( void )
{
	h261_context_t s_inc_ctx, s_old_ctx;
	sbuf_t s_inc_stream = { 0, 0, 0 }, s_old_stream = { 0, 0, 0 };
	planes_t ps_recon_old[ MAXFRAME ], ps_ref_old[ MAXFRAME ], s_ref_snap;
	UInt32 rgui_frame_bytes[ MAXFRAME ];
	h261_incremental_t *ps_inc;
	int i_frame;

	TASK_CHECK( fixture_init( &s_inc_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_VBR ) );
	TASK_CHECK( fixture_init( &s_old_ctx, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_VBR ) );
	old_run( &s_old_ctx, NFRAME_QCIF, &s_old_stream, ps_recon_old, ps_ref_old, rgui_frame_bytes, 0 );

	ps_inc = h261inc_create( &s_inc_ctx );
	TASK_CHECK( ps_inc != 0 );

	for( i_frame = 0; i_frame < NFRAME_QCIF; i_frame++ )
	{
		h261inc_step_t s_step;
		float f_wanted_pre, f_actual_pre, f_bits_quant_pre;
		float f_wanted_begin, f_actual_begin, f_bits_quant_begin;
		int i_done = 0;

		fill_frame( &s_inc_ctx, i_frame );
		planes_snap( &s_ref_snap, &s_inc_ctx.s_reference_frame );
		f_wanted_pre = s_inc_ctx.s_ratectrl.f_wanted_bits;
		f_actual_pre = s_inc_ctx.s_ratectrl.f_actual_bits;
		f_bits_quant_pre = s_inc_ctx.s_ratectrl.f_bits_quant;

		TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame ) == 0 );
		f_wanted_begin = s_inc_ctx.s_ratectrl.f_wanted_bits;
		f_actual_begin = s_inc_ctx.s_ratectrl.f_actual_bits;
		f_bits_quant_begin = s_inc_ctx.s_ratectrl.f_bits_quant;

		while( !i_done )
		{
			UInt8 rgui8[ 50 ];
			UInt32 ui_got;

			TASK_CHECK( h261inc_step( ps_inc, 13, &s_step ) == 0 );
			ui_got = h261inc_pull( ps_inc, rgui8, sizeof( rgui8 ) );
			sbuf_put( &s_inc_stream, rgui8, ui_got );

			if( !s_step.i_picture_done )
			{
				/* neither the reference plane nor the VBR state may
				   move before the last macroblock commits */
				TASK_CHECK( planes_match( &s_ref_snap, &s_inc_ctx.s_reference_frame ) );
				TASK_CHECK( s_inc_ctx.s_ratectrl.f_wanted_bits == f_wanted_begin );
				TASK_CHECK( s_inc_ctx.s_ratectrl.f_actual_bits == f_actual_begin );
				TASK_CHECK( s_inc_ctx.s_ratectrl.f_bits_quant == f_bits_quant_begin );
			}
			else
			{
				i_done = 1;
			}
		}

		/* exactly one reference update: reference is now the
		   reconstructed picture, and it matches the classic path */
		TASK_CHECK( planes_match( &ps_ref_old[ i_frame ], &s_inc_ctx.s_reference_frame ) );
		TASK_CHECK( planes_match( &ps_recon_old[ i_frame ], &s_inc_ctx.s_reconstructed_frame ) );

		/* exactly one VBR update, with the same byte count the classic
		   path would have reported for this picture */
		TASK_CHECK( s_inc_ctx.s_ratectrl.f_wanted_bits ==
			f_wanted_pre + s_inc_ctx.s_ratectrl.f_bitrate * s_inc_ctx.s_ratectrl.f_framerate );
		TASK_CHECK( s_inc_ctx.s_ratectrl.f_actual_bits ==
			f_actual_pre + ( float )( rgui_frame_bytes[ i_frame ] * 8 ) );
		TASK_CHECK( s_inc_ctx.s_ratectrl.f_bits_quant ==
			f_bits_quant_pre + ( float )( rgui_frame_bytes[ i_frame ] * 8 ) * s_inc_ctx.s_ratectrl.i_current_quant );

		/* after the commit, pulling and rejected calls move nothing */
		{
			float f_wanted_done = s_inc_ctx.s_ratectrl.f_wanted_bits;
			float f_actual_done = s_inc_ctx.s_ratectrl.f_actual_bits;
			planes_t s_ref_done;
			planes_snap( &s_ref_done, &s_inc_ctx.s_reference_frame );

			/* step with no picture in progress is rejected */
			TASK_CHECK( h261inc_step( ps_inc, 1, &s_step ) != 0 );
			/* while output is still buffered, begin is rejected too */
			if( h261inc_pending( ps_inc ) > 0 )
			{
				TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame + 1 ) != 0 );
				TASK_CHECK( h261inc_busy( ps_inc ) == 0 );
			}
			pull_all( ps_inc, &s_inc_stream );
			TASK_CHECK( planes_match( &s_ref_done, &s_inc_ctx.s_reference_frame ) );
			TASK_CHECK( s_inc_ctx.s_ratectrl.f_wanted_bits == f_wanted_done );
			TASK_CHECK( s_inc_ctx.s_ratectrl.f_actual_bits == f_actual_done );
			planes_release( &s_ref_done );
		}
		planes_release( &s_ref_snap );
	}

	TASK_CHECK( h261inc_end( ps_inc ) == 0 );
	pull_all( ps_inc, &s_inc_stream );
	TASK_CHECK( stream_equal( &s_inc_stream, &s_old_stream ) );

	h261inc_destroy( ps_inc );
	sbuf_free( &s_inc_stream );
	sbuf_free( &s_old_stream );
	for( i_frame = 0; i_frame < NFRAME_QCIF; i_frame++ )
	{
		planes_release( &ps_recon_old[ i_frame ] );
		planes_release( &ps_ref_old[ i_frame ] );
	}
	fixture_free( &s_inc_ctx );
	fixture_free( &s_old_ctx );
	return 0;
}

/* two independent instances interleaved step by step */
static int case_commit_two_instances( void )
{
	h261_context_t s_ctx_a, s_old_a, s_ctx_b, s_old_b;
	sbuf_t s_stream_a = { 0, 0, 0 }, s_ref_stream_a = { 0, 0, 0 };
	sbuf_t s_stream_b = { 0, 0, 0 }, s_ref_stream_b = { 0, 0, 0 };
	planes_t ps_recon_a[ MAXFRAME ], ps_ref_a[ MAXFRAME ];
	planes_t ps_recon_b[ MAXFRAME ], ps_ref_b[ MAXFRAME ];
	UInt32 rgui_bytes_a[ MAXFRAME ], rgui_bytes_b[ MAXFRAME ];
	h261_incremental_t *ps_inc_a, *ps_inc_b;
	int i_frame;

	TASK_CHECK( fixture_init( &s_ctx_a, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	TASK_CHECK( fixture_init( &s_old_a, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	TASK_CHECK( fixture_init( &s_ctx_b, H261_SOURCE_FORMAT_CIF, H261_RATECTRL_MODE_VBR ) );
	TASK_CHECK( fixture_init( &s_old_b, H261_SOURCE_FORMAT_CIF, H261_RATECTRL_MODE_VBR ) );
	old_run( &s_old_a, NFRAME_CIF, &s_ref_stream_a, ps_recon_a, ps_ref_a, rgui_bytes_a, 0 );
	old_run( &s_old_b, NFRAME_CIF, &s_ref_stream_b, ps_recon_b, ps_ref_b, rgui_bytes_b, 0 );

	ps_inc_a = h261inc_create( &s_ctx_a );
	ps_inc_b = h261inc_create( &s_ctx_b );
	TASK_CHECK( ps_inc_a != 0 && ps_inc_b != 0 );

	for( i_frame = 0; i_frame < NFRAME_CIF; i_frame++ )
	{
		h261inc_step_t s_step_a, s_step_b;
		int i_done_a = 0, i_done_b = 0;
		int i_expected_a = 0, i_expected_b = 0;

		fill_frame( &s_ctx_a, i_frame );
		fill_frame( &s_ctx_b, i_frame );
		TASK_CHECK( inc_begin_frame( ps_inc_a, &s_ctx_a, i_frame ) == 0 );
		TASK_CHECK( inc_begin_frame( ps_inc_b, &s_ctx_b, i_frame ) == 0 );

		while( !i_done_a || !i_done_b )
		{
			UInt8 rgui8[ 128 ];
			UInt32 ui_got;

			if( !i_done_a )
			{
				TASK_CHECK( h261inc_step( ps_inc_a, 7, &s_step_a ) == 0 );
				i_expected_a += s_step_a.i_committed;
				TASK_CHECK( h261inc_committed( ps_inc_a ) == i_expected_a );
				ui_got = h261inc_pull( ps_inc_a, rgui8, 13 );
				TASK_CHECK( ui_got <= 13 );
				sbuf_put( &s_stream_a, rgui8, ui_got );
				i_done_a = s_step_a.i_picture_done;
			}
			if( !i_done_b )
			{
				TASK_CHECK( h261inc_step( ps_inc_b, 5, &s_step_b ) == 0 );
				i_expected_b += s_step_b.i_committed;
				TASK_CHECK( h261inc_committed( ps_inc_b ) == i_expected_b );
				ui_got = h261inc_pull( ps_inc_b, rgui8, 100 );
				TASK_CHECK( ui_got <= 100 );
				sbuf_put( &s_stream_b, rgui8, ui_got );
				i_done_b = s_step_b.i_picture_done;
			}
			/* the instances do not share cursors */
			TASK_CHECK( h261inc_committed( ps_inc_a ) == i_expected_a );
			TASK_CHECK( h261inc_committed( ps_inc_b ) == i_expected_b );
		}
		TASK_CHECK( i_expected_a == 99 );
		TASK_CHECK( i_expected_b == 396 );
		pull_all( ps_inc_a, &s_stream_a );
		pull_all( ps_inc_b, &s_stream_b );
		TASK_CHECK( planes_match( &ps_recon_a[ i_frame ], &s_ctx_a.s_reconstructed_frame ) );
		TASK_CHECK( planes_match( &ps_recon_b[ i_frame ], &s_ctx_b.s_reconstructed_frame ) );
		TASK_CHECK( planes_match( &ps_ref_a[ i_frame ], &s_ctx_a.s_reference_frame ) );
		TASK_CHECK( planes_match( &ps_ref_b[ i_frame ], &s_ctx_b.s_reference_frame ) );
	}

	TASK_CHECK( h261inc_end( ps_inc_a ) == 0 );
	TASK_CHECK( h261inc_end( ps_inc_b ) == 0 );
	pull_all( ps_inc_a, &s_stream_a );
	pull_all( ps_inc_b, &s_stream_b );
	TASK_CHECK( stream_equal( &s_stream_a, &s_ref_stream_a ) );
	TASK_CHECK( stream_equal( &s_stream_b, &s_ref_stream_b ) );
	TASK_CHECK( s_ctx_b.s_ratectrl.f_actual_bits == s_old_b.s_ratectrl.f_actual_bits );
	TASK_CHECK( s_ctx_b.s_ratectrl.f_wanted_bits == s_old_b.s_ratectrl.f_wanted_bits );

	h261inc_destroy( ps_inc_a );
	h261inc_destroy( ps_inc_b );
	sbuf_free( &s_stream_a );
	sbuf_free( &s_stream_b );
	sbuf_free( &s_ref_stream_a );
	sbuf_free( &s_ref_stream_b );
	for( i_frame = 0; i_frame < NFRAME_CIF; i_frame++ )
	{
		planes_release( &ps_recon_a[ i_frame ] );
		planes_release( &ps_ref_a[ i_frame ] );
		planes_release( &ps_recon_b[ i_frame ] );
		planes_release( &ps_ref_b[ i_frame ] );
	}
	fixture_free( &s_ctx_a );
	fixture_free( &s_old_a );
	fixture_free( &s_ctx_b );
	fixture_free( &s_old_b );
	return 0;
}

/* bounded staging: capacity, observable peak, stall-and-resume
   without losing codewords, writer restored on destroy */
static int case_commit_memory_bound( void )
{
	h261_context_t s_inc_ctx, s_old_ctx;
	sbuf_t s_inc_stream = { 0, 0, 0 }, s_old_stream = { 0, 0, 0 };
	planes_t ps_recon_old[ MAXFRAME ], ps_ref_old[ MAXFRAME ];
	UInt32 rgui_frame_bytes[ MAXFRAME ];
	h261_incremental_t *ps_inc;
	int i_frame, i_stall_seen;
	unsigned ui_bits_inc, ui_bits_old;

	TASK_CHECK( fixture_init( &s_inc_ctx, H261_SOURCE_FORMAT_CIF, H261_RATECTRL_MODE_QUANT ) );
	TASK_CHECK( fixture_init( &s_old_ctx, H261_SOURCE_FORMAT_CIF, H261_RATECTRL_MODE_QUANT ) );
	old_run( &s_old_ctx, NFRAME_CIF, &s_old_stream, ps_recon_old, ps_ref_old, rgui_frame_bytes, 0 );

	TASK_CHECK( s_inc_ctx.s_bitstream.i_length == 0x100000 );
	ps_inc = h261inc_create( &s_inc_ctx );
	TASK_CHECK( ps_inc != 0 );

	/* the whole compressed storage is the bounded staging buffer */
	TASK_CHECK( h261inc_capacity( ps_inc ) <= 8192 );
	TASK_CHECK( s_inc_ctx.s_bitstream.i_length == ( Int32 ) h261inc_capacity( ps_inc ) );

	i_stall_seen = 0;
	for( i_frame = 0; i_frame < NFRAME_CIF; i_frame++ )
	{
		h261inc_step_t s_step;
		int i_done = 0;

		fill_frame( &s_inc_ctx, i_frame );
		TASK_CHECK( inc_begin_frame( ps_inc, &s_inc_ctx, i_frame ) == 0 );

		/* never pull until the bounded staging stalls */
		while( !i_done )
		{
			TASK_CHECK( h261inc_step( ps_inc, 400, &s_step ) == 0 );
			TASK_CHECK( h261inc_pending( ps_inc ) <= h261inc_capacity( ps_inc ) );
			if( s_step.i_committed == 0 && s_step.i_output_pending && !s_step.i_picture_done )
			{
				i_stall_seen = 1; /* buffer full: paused, nothing lost */
				pull_all( ps_inc, &s_inc_stream );
			}
			if( s_step.i_picture_done )
			{
				i_done = 1;
			}
		}
		pull_all( ps_inc, &s_inc_stream );
		TASK_CHECK( h261inc_peak( ps_inc ) <= h261inc_capacity( ps_inc ) );
		TASK_CHECK( planes_match( &ps_recon_old[ i_frame ], &s_inc_ctx.s_reconstructed_frame ) );
		TASK_CHECK( planes_match( &ps_ref_old[ i_frame ], &s_inc_ctx.s_reference_frame ) );
	}
	TASK_CHECK( i_stall_seen );
	/* the staging buffer really was the limiting resource */
	TASK_CHECK( h261inc_peak( ps_inc ) > 2048 );
	TASK_CHECK( h261inc_peak( ps_inc ) <= 8192 );

	TASK_CHECK( h261inc_end( ps_inc ) == 0 );
	pull_all( ps_inc, &s_inc_stream );
	TASK_CHECK( stream_equal( &s_inc_stream, &s_old_stream ) );

	h261inc_destroy( ps_inc );
	/* the original writer is restored and the classic path works on */
	TASK_CHECK( s_inc_ctx.s_bitstream.i_length == 0x100000 );
	fill_frame( &s_inc_ctx, NFRAME_CIF );
	fill_frame( &s_old_ctx, NFRAME_CIF );
	ui_bits_inc = fixture_encode( &s_inc_ctx, NFRAME_CIF, NULL );
	ui_bits_old = fixture_encode( &s_old_ctx, NFRAME_CIF, NULL );
	TASK_CHECK( ui_bits_inc > 0 );
	TASK_CHECK( ui_bits_inc == ui_bits_old );
	{
		planes_t s_recon_old3;
		planes_snap( &s_recon_old3, &s_old_ctx.s_reconstructed_frame );
		TASK_CHECK( planes_match( &s_recon_old3, &s_inc_ctx.s_reconstructed_frame ) );
		planes_release( &s_recon_old3 );
	}

	sbuf_free( &s_inc_stream );
	sbuf_free( &s_old_stream );
	for( i_frame = 0; i_frame < NFRAME_CIF; i_frame++ )
	{
		planes_release( &ps_recon_old[ i_frame ] );
		planes_release( &ps_ref_old[ i_frame ] );
	}
	fixture_free( &s_inc_ctx );
	fixture_free( &s_old_ctx );
	return 0;
}

/* ================================================================ */

const struct task_case task_cases[] = {
	{ "cursor", "budget_one_qcif", case_cursor_budget_one_qcif },
	{ "cursor", "irregular_budget_cif", case_cursor_irregular_budget_cif },
	{ "output", "pull_capacity_patterns", case_output_pull_capacity_patterns },
	{ "output", "irregular_pulls_vbr", case_output_irregular_pulls_vbr },
	{ "commit", "updates_once_vbr", case_commit_updates_once_vbr },
	{ "commit", "two_instances", case_commit_two_instances },
	{ "commit", "memory_bound", case_commit_memory_bound },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[ 0 ] );
