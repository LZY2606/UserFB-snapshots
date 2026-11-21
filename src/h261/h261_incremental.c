/*
Incremental (pausable, macroblock-stepping) H.261 encoding interface.

See h261_incremental.h for the contract.  This file only re-sequences
the existing whole-frame encode loop (h261_write_picture_header,
h261_encode_gob bodies, h261_bitstream_flush) into a resumable cursor;
the macroblock encode/reconstruct/write primitives themselves are the
untouched production ones, which is what makes the output bit-identical
to the whole-frame path.
*/

#include "h261_incremental.h"

/* Free staging space required before emitting a GOB header (26 bits)
   or one macroblock (worst case well under 1024 bytes: 6 blocks of
   64 escape-coded coefficients plus headers). */
#define H261INC_HDR_SAFE 16
#define H261INC_MB_SAFE 1536

struct h261_incremental_s {
	h261_context_t *ps_ctx;
	Int32 i_saved_bs_length;    /* buffer length of the swapped-out writer */

	h261_picture_parameters_t s_parameters; /* copied at begin */

	Int32 i_busy;               /* picture in progress */
	Int32 i_ended;              /* sequence ended */

	/* resumable cursor, mirrors h261_encode_gobs / h261_encode_gob */
	Int32 i_gob_idx;            /* index into the GOB layout table */
	Int32 i_mb_idx;             /* 1..33 inside the current GOB */
	Int32 i_gob_started;        /* GOB header already emitted */
	Int32 i_mb_addr;            /* skipped-macroblock address increment */
	Int32 i_committed;          /* macroblocks committed this picture */

	UInt32 ui_pulled_this_picture; /* complete bytes pulled since begin */
	UInt32 ui_peak;             /* high-water mark of pending bytes */
};

/* GOB layout: { group number, x offset in GOB widths, y offset in GOB
   heights }, identical to the tables in h261_encode_gobs. */
static const Int32 rgi_gob_layout[ 2 ][ 12 ][ 3 ] = {
	{	/* H261_SOURCE_FORMAT_QCIF */
		{ 1, 0, 0 },
		{ 3, 0, 1 },
		{ 5, 0, 2 }
	},
	{	/* H261_SOURCE_FORMAT_CIF */
		{ 1, 0, 0 },
		{ 2, 1, 0 },
		{ 3, 0, 1 },
		{ 4, 1, 1 },
		{ 5, 0, 2 },
		{ 6, 1, 2 },
		{ 7, 0, 3 },
		{ 8, 1, 3 },
		{ 9, 0, 4 },
		{ 10, 1, 4 },
		{ 11, 0, 5 },
		{ 12, 1, 5 }
	}
};

static const Int32 rgi_num_gobs[ 2 ] = { 3, 12 };

static UInt32 h261inc_pending_bytes( const h261_incremental_t *ps_inc )
{
	const h261_bitstream_t *ps_bitstream = &ps_inc->ps_ctx->s_bitstream;
	return ( UInt32 )( ps_bitstream->pui8_codeword_ptr - ps_bitstream->pui8_bitstream );
}

static UInt32 h261inc_free_bytes( const h261_incremental_t *ps_inc )
{
	const h261_bitstream_t *ps_bitstream = &ps_inc->ps_ctx->s_bitstream;
	return ( UInt32 )( ps_bitstream->i_length - ( ps_bitstream->pui8_codeword_ptr - ps_bitstream->pui8_bitstream ) );
}

static Void h261inc_note_peak( h261_incremental_t *ps_inc )
{
	UInt32 ui_pending = h261inc_pending_bytes( ps_inc );
	if( ui_pending > ps_inc->ui_peak )
	{
		ps_inc->ui_peak = ui_pending;
	}
}

h261_incremental_t *h261inc_create( h261_context_t *ps_ctx )
{
	h261_incremental_t *ps_inc;

	if( !ps_ctx )
	{
		return NULL;
	}

	ps_inc = ( h261_incremental_t * ) calloc( 1, sizeof( h261_incremental_t ) );
	if( !ps_inc )
	{
		return NULL;
	}

	ps_inc->ps_ctx = ps_ctx;

	/* Swap the context's bitstream writer for the bounded staging
	   buffer; the original writer is re-created in h261inc_destroy. */
	ps_inc->i_saved_bs_length = ps_ctx->s_bitstream.i_length;
	h261_bitstream_deinit( &ps_ctx->s_bitstream );
	h261_bitstream_init( &ps_ctx->s_bitstream, H261INC_STORAGE_CAPACITY );

	return ps_inc;
}

Void h261inc_destroy( h261_incremental_t *ps_inc )
{
	h261_context_t *ps_ctx;

	if( !ps_inc )
	{
		return;
	}

	ps_ctx = ps_inc->ps_ctx;
	h261_bitstream_deinit( &ps_ctx->s_bitstream );
	h261_bitstream_init( &ps_ctx->s_bitstream, ps_inc->i_saved_bs_length );

	free( ps_inc );
}

Int32 h261inc_begin_picture( h261_incremental_t *ps_inc, const h261_picture_parameters_t *ps_parameters )
{
	h261_context_t *ps_ctx;

	if( !ps_inc || !ps_parameters )
	{
		return -1;
	}
	if( ps_inc->i_busy || ps_inc->i_ended )
	{
		return -1;
	}
	if( h261inc_pending_bytes( ps_inc ) != 0 )
	{
		return -1;
	}

	ps_ctx = ps_inc->ps_ctx;

	ps_inc->s_parameters = *ps_parameters;

	/* identical call sequence to the whole-frame path */
	ps_ctx->i_frame_counter = ps_inc->s_parameters.i_frame_num;
	h261_ratectrl_init_picture( ps_ctx, &ps_inc->s_parameters );
	h261_init_picture( ps_ctx, &ps_inc->s_parameters );
	h261_write_picture_header( ps_ctx );

	ps_inc->i_gob_idx = 0;
	ps_inc->i_mb_idx = 1;
	ps_inc->i_gob_started = 0;
	ps_inc->i_mb_addr = 0;
	ps_inc->i_committed = 0;
	ps_inc->ui_pulled_this_picture = 0;

	ps_inc->i_busy = 1;
	h261inc_note_peak( ps_inc );

	return 0;
}

/* GOB prologue, identical to the start of h261_encode_gob. */
static Void h261inc_start_gob( h261_incremental_t *ps_inc )
{
	h261_context_t *ps_ctx = ps_inc->ps_ctx;
	h261_picture_t *ps_picture = &ps_ctx->s_picture;
	h261_gob_t *ps_gob = &ps_picture->s_groups_of_blocks;
	h261_mb_coding_vars_t *ps_coding_vars = &ps_gob->s_coding_vars;

	ps_gob->i_group_of_blocks_start_code = 0x0001;
	ps_gob->i_group_number = rgi_gob_layout[ ps_ctx->i_source_format ][ ps_inc->i_gob_idx ][ 0 ];
	ps_gob->i_quantiser = ps_picture->i_quantiser;
	ps_gob->i_num_extra_insertion_information = 0;

	ps_coding_vars->i_quantiser = ps_gob->i_quantiser;
	ps_coding_vars->i_last_mv_valid = 0;

	h261_write_gob_header( ps_ctx );

	ps_inc->i_mb_addr = 0;
	ps_inc->i_gob_started = 1;
}

/* One macroblock, identical to one iteration of the h261_encode_gob
   loop; the cursor lives in the instance instead of on the stack. */
static Void h261inc_encode_mb( h261_incremental_t *ps_inc )
{
	h261_context_t *ps_ctx = ps_inc->ps_ctx;
	h261_picture_t *ps_picture = &ps_ctx->s_picture;
	h261_gob_t *ps_gob = &ps_picture->s_groups_of_blocks;
	h261_macroblock_t *ps_mb = &ps_gob->s_macroblock;
	h261_mb_coding_vars_t *ps_coding_vars = &ps_gob->s_coding_vars;

	Int32 i_mb_stride, i_mb_height, i_mb_x, i_mb_y, i_coded;
	Int32 i_offset_x, i_offset_y;

	static const Int32 rgi_macroblock_offsets[ 33 ][ 2 ] = {
		{ 0, 0 }, { 16, 0 }, { 32, 0 }, { 48, 0 }, { 64, 0 }, { 80, 0 },
		{ 96, 0 }, { 112, 0 }, { 128, 0 }, { 144, 0 }, { 160, 0 },
		{ 0, 16 }, { 16, 16 }, { 32, 16 }, { 48, 16 }, { 64, 16 }, { 80, 16 },
		{ 96, 16 }, { 112, 16 }, { 128, 16 }, { 144, 16 }, { 160, 16 },
		{ 0, 32 }, { 16, 32 }, { 32, 32 }, { 48, 32 }, { 64, 32 }, { 80, 32 },
		{ 96, 32 }, { 112, 32 }, { 128, 32 }, { 144, 32 }, { 160, 32 },
	};

	i_offset_x = rgi_gob_layout[ ps_ctx->i_source_format ][ ps_inc->i_gob_idx ][ 1 ] * H261_GOB_WIDTH;
	i_offset_y = rgi_gob_layout[ ps_ctx->i_source_format ][ ps_inc->i_gob_idx ][ 2 ] * H261_GOB_HEIGHT;

	ps_inc->i_mb_addr++;

	i_mb_x = rgi_macroblock_offsets[ ps_inc->i_mb_idx - 1 ][ 0 ] + i_offset_x;
	i_mb_y = rgi_macroblock_offsets[ ps_inc->i_mb_idx - 1 ][ 1 ] + i_offset_y;

	if( ps_inc->i_mb_idx == 1 || ps_inc->i_mb_idx == 12 || ps_inc->i_mb_idx == 23 )
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
		h261_write_macroblock( ps_ctx, ps_inc->i_mb_addr );
		ps_inc->i_mb_addr = 0;

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

	/* advance the cursor */
	ps_inc->i_committed++;
	ps_inc->i_mb_idx++;
	if( ps_inc->i_mb_idx > 33 )
	{
		ps_inc->i_mb_idx = 1;
		ps_inc->i_gob_idx++;
		ps_inc->i_gob_started = 0;
	}
}

/* Runs exactly once per picture, at the commit of its last macroblock:
   the reference plane update and the rate-control (VBR) update. */
static Void h261inc_finish_picture( h261_incremental_t *ps_inc )
{
	h261_context_t *ps_ctx = ps_inc->ps_ctx;
	h261_frame_t *ps_recon = &ps_ctx->s_reconstructed_frame;
	h261_frame_t *ps_ref = &ps_ctx->s_reference_frame;
	UInt32 ui_length;

	/* same byte count the whole-frame path would hand to the rate
	   control: every complete byte of this picture, pulled or not */
	ui_length = ps_inc->ui_pulled_this_picture + h261inc_pending_bytes( ps_inc );
	h261_ratectrl_update_picture( ps_ctx, ui_length );

	memcpy( ps_ref->pui8_Y, ps_recon->pui8_Y, ( size_t ) ps_recon->i_width * ps_recon->i_height );
	memcpy( ps_ref->pui8_Cb, ps_recon->pui8_Cb, ( size_t ) ps_recon->i_width * ps_recon->i_height / 4 );
	memcpy( ps_ref->pui8_Cr, ps_recon->pui8_Cr, ( size_t ) ps_recon->i_width * ps_recon->i_height / 4 );

	ps_inc->i_busy = 0;
}

Int32 h261inc_step( h261_incremental_t *ps_inc, Int32 i_mb_budget, h261inc_step_t *ps_out )
{
	Int32 i_committed = 0;
	Int32 i_num_gobs;

	if( !ps_inc || i_mb_budget < 0 )
	{
		return -1;
	}
	if( !ps_inc->i_busy )
	{
		if( ps_out )
		{
			ps_out->i_committed = 0;
			ps_out->i_output_pending = ( h261inc_pending_bytes( ps_inc ) > 0 );
			ps_out->i_picture_done = 1;
		}
		return -1;
	}

	i_num_gobs = rgi_num_gobs[ ps_inc->ps_ctx->i_source_format ];

	while( i_committed < i_mb_budget && ps_inc->i_busy )
	{
		if( !ps_inc->i_gob_started )
		{
			if( h261inc_free_bytes( ps_inc ) < H261INC_HDR_SAFE )
			{
				break; /* staging full: pause, lose nothing */
			}
			h261inc_start_gob( ps_inc );
		}

		if( h261inc_free_bytes( ps_inc ) < H261INC_MB_SAFE )
		{
			break; /* staging full: pause, lose nothing */
		}

		h261inc_encode_mb( ps_inc );
		i_committed++;
		h261inc_note_peak( ps_inc );

		if( ps_inc->i_gob_idx >= i_num_gobs )
		{
			h261inc_finish_picture( ps_inc );
		}
	}

	if( ps_out )
	{
		ps_out->i_committed = i_committed;
		ps_out->i_output_pending = ( h261inc_pending_bytes( ps_inc ) > 0 );
		ps_out->i_picture_done = !ps_inc->i_busy;
	}

	return 0;
}

UInt32 h261inc_pull( h261_incremental_t *ps_inc, UInt8 *pui8_dst, UInt32 ui_capacity )
{
	h261_bitstream_t *ps_bitstream;
	UInt32 ui_pending, ui_move;

	if( !ps_inc || ui_capacity == 0 )
	{
		return 0;
	}
	if( !pui8_dst )
	{
		return 0;
	}

	ps_bitstream = &ps_inc->ps_ctx->s_bitstream;
	ui_pending = ( UInt32 )( ps_bitstream->pui8_codeword_ptr - ps_bitstream->pui8_bitstream );
	ui_move = MIN( ui_capacity, ui_pending );

	if( ui_move )
	{
		memcpy( pui8_dst, ps_bitstream->pui8_bitstream, ui_move );
		memmove( ps_bitstream->pui8_bitstream, ps_bitstream->pui8_bitstream + ui_move, ui_pending - ui_move );
		ps_bitstream->pui8_codeword_ptr -= ui_move;
		if( ps_inc->i_busy )
		{
			ps_inc->ui_pulled_this_picture += ui_move;
		}
	}

	return ui_move;
}

Int32 h261inc_end( h261_incremental_t *ps_inc )
{
	h261_bitstream_t *ps_bitstream;

	if( !ps_inc )
	{
		return -1;
	}
	if( ps_inc->i_busy )
	{
		return -1;
	}
	if( ps_inc->i_ended )
	{
		return 0; /* repeated end: no further data */
	}

	/* pad out at most one final byte, like h261_bitstream_flush */
	ps_bitstream = &ps_inc->ps_ctx->s_bitstream;
	if( ps_bitstream->i_codeword_fill )
	{
		*( ps_bitstream->pui8_codeword_ptr++ ) = ( ps_bitstream->ui_codeword >> 24 );
		ps_bitstream->ui_codeword <<= 8;
		ps_bitstream->i_codeword_fill = 0;
	}

	ps_inc->i_ended = 1;
	h261inc_note_peak( ps_inc );

	return 0;
}

Int32 h261inc_committed( const h261_incremental_t *ps_inc )
{
	return ps_inc ? ps_inc->i_committed : 0;
}

UInt32 h261inc_pending( const h261_incremental_t *ps_inc )
{
	return ps_inc ? h261inc_pending_bytes( ps_inc ) : 0;
}

UInt32 h261inc_capacity( const h261_incremental_t *ps_inc )
{
	return ps_inc ? H261INC_STORAGE_CAPACITY : 0;
}

UInt32 h261inc_peak( const h261_incremental_t *ps_inc )
{
	return ps_inc ? ps_inc->ui_peak : 0;
}

Int32 h261inc_busy( const h261_incremental_t *ps_inc )
{
	return ps_inc ? ps_inc->i_busy : 0;
}

Int32 h261inc_ended( const h261_incremental_t *ps_inc )
{
	return ps_inc ? ps_inc->i_ended : 0;
}
