/*
 * GOB budget contract tests.
 *
 * These cases encode real QCIF/CIF pictures with the local flat / texture /
 * moving generators and independently:
 *   - enumerate all 31 base GQUANT candidates with the old h261_encode_gob
 *     core from a byte-identical predecessor state,
 *   - compute Y/Cb/Cr SSE and the required ordering / buffer decision,
 *   - parse GOB numbers and GQUANT values out of the emitted bitstream,
 *   - settle the exact floor() cumulative budget clock,
 *   - verify committed reconstruction / reference state across P frames.
 *
 * The file intentionally has no main(): task_support/driver.c provides it
 * and locates the cases through task_cases / task_case_count.
 */

#include "task_support/check.h"
#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

#define GB_TRIALS 31
#define GB_MAX_RECS 512

typedef void (*gb_fill_fn)( h261_context_t *ps_ctx, int i_frame );

static int gb_gob_count( int i_format )
{
	return i_format == H261_SOURCE_FORMAT_CIF ? 12 : 3;
}

static void gb_gob_geom( int i_format, int i_idx, int *pi_gn, int *pi_ox, int *pi_oy )
{
	static const int ai_qcif[ 3 ][ 3 ] = { { 1, 0, 0 }, { 3, 0, 48 }, { 5, 0, 96 } };
	static const int ai_cif[ 12 ][ 3 ] = {
		{ 1, 0, 0 }, { 2, 176, 0 }, { 3, 0, 48 }, { 4, 176, 48 },
		{ 5, 0, 96 }, { 6, 176, 96 }, { 7, 0, 144 }, { 8, 176, 144 },
		{ 9, 0, 192 }, { 10, 176, 192 }, { 11, 0, 240 }, { 12, 176, 240 }
	};
	if( i_format == H261_SOURCE_FORMAT_CIF )
	{
		*pi_gn = ai_cif[ i_idx ][ 0 ];
		*pi_ox = ai_cif[ i_idx ][ 1 ];
		*pi_oy = ai_cif[ i_idx ][ 2 ];
	}
	else
	{
		*pi_gn = ai_qcif[ i_idx ][ 0 ];
		*pi_ox = ai_qcif[ i_idx ][ 1 ];
		*pi_oy = ai_qcif[ i_idx ][ 2 ];
	}
}

/* ---------------- local picture generators ---------------- */

static void gb_fill_flat( h261_context_t *ps_ctx, int i_frame )
{
	h261_frame_t *f = &ps_ctx->s_current_frame;
	int y, x;
	(void) i_frame;
	for( y = 0; y < f->i_height; y++ )
		for( x = 0; x < f->i_width; x++ )
			f->pui8_Y[ y * f->i_stride_y + x ] = 128;
	for( y = 0; y < f->i_height / 2; y++ )
		for( x = 0; x < f->i_width / 2; x++ )
		{
			f->pui8_Cb[ y * f->i_stride_c + x ] = 128;
			f->pui8_Cr[ y * f->i_stride_c + x ] = 128;
		}
}

static void gb_fill_texture( h261_context_t *ps_ctx, int i_frame )
{
	h261_frame_t *f = &ps_ctx->s_current_frame;
	int y, x;
	for( y = 0; y < f->i_height; y++ )
		for( x = 0; x < f->i_width; x++ )
		{
			int v = 36 + ( ( ( x + i_frame * 5 ) / 4 * 29 + ( y / 4 ) * 17 + ( ( x ^ y ) & 7 ) * 3 ) % 184 );
			if( ( ( x / 8 + y / 8 ) & 1 ) ) v = 255 - v;
			f->pui8_Y[ y * f->i_stride_y + x ] = ( UInt8 ) v;
		}
	for( y = 0; y < f->i_height / 2; y++ )
		for( x = 0; x < f->i_width / 2; x++ )
		{
			f->pui8_Cb[ y * f->i_stride_c + x ] = ( UInt8 )( 96 + ( ( x / 4 + i_frame ) * 11 + y * 5 ) % 64 );
			f->pui8_Cr[ y * f->i_stride_c + x ] = ( UInt8 )( 110 + ( ( y / 4 + i_frame ) * 13 + x * 7 ) % 64 );
		}
}

static void gb_fill_moving( h261_context_t *ps_ctx, int i_frame )
{
	h261_frame_t *f = &ps_ctx->s_current_frame;
	int y, x;
	int bx = 24 + ( i_frame * 26 ) % ( f->i_width - 96 );
	int by = 36 + ( i_frame * 11 ) % ( f->i_height - 120 );
	for( y = 0; y < f->i_height; y++ )
		for( x = 0; x < f->i_width; x++ )
		{
			int v = 42 + ( x / 16 + y / 16 ) * 9;
			if( ( ( x / 16 ) & 1 ) != ( ( y / 16 ) & 1 ) ) v += 26;
			if( x >= bx && x < bx + 56 && y >= by && y < by + 44 ) v = 224;
			if( x >= bx + 10 && x < bx + 30 && y >= by + 8 && y < by + 20 ) v = 30;
			f->pui8_Y[ y * f->i_stride_y + x ] = ( UInt8 )( v & 255 );
		}
	for( y = 0; y < f->i_height / 2; y++ )
		for( x = 0; x < f->i_width / 2; x++ )
		{
			f->pui8_Cb[ y * f->i_stride_c + x ] = ( UInt8 )( 120 + ( x / 8 ) * 6 );
			f->pui8_Cr[ y * f->i_stride_c + x ] = ( UInt8 )( 128 + ( y / 8 ) * 6 );
		}
}

/* ---------------- observe collector ---------------- */

typedef struct
{
	h261_gob_budget_stats_t recs[ GB_MAX_RECS ];
	Int64 ai64_entry_delta[ GB_MAX_RECS ];
	int i_count;
	h261_context_t *ps_ctx;
	Int64 i64_last_entries;
} gb_collector_t;

static void gb_on_observe( const h261_gob_budget_stats_t *ps_stats, void *pv_opaque )
{
	gb_collector_t *pc = ( gb_collector_t * ) pv_opaque;
	if( pc->i_count < GB_MAX_RECS )
	{
		pc->recs[ pc->i_count ] = *ps_stats; /* copy: record is borrowed only */
		if( pc->ps_ctx != NULL )
		{
			Int64 i64_now = pc->ps_ctx->i_gob_encode_entries;
			pc->ai64_entry_delta[ pc->i_count ] = i64_now - pc->i64_last_entries;
			pc->i64_last_entries = i64_now;
		}
		else
		{
			pc->ai64_entry_delta[ pc->i_count ] = -1;
		}
		pc->i_count++;
	}
}

/* ---------------- independent bit reader ---------------- */

static UInt32 gb_bits_at( const UInt8 *p, size_t z_bits, size_t z_pos, int i_n )
{
	UInt32 v = 0;
	int i;
	(void) z_bits;
	for( i = 0; i < i_n; i++ )
	{
		size_t z_bp = z_pos + ( size_t ) i;
		UInt8 bit = ( UInt8 )( ( p[ z_bp >> 3 ] >> ( 7 - ( int )( z_bp & 7 ) ) ) & 1 );
		v = ( v << 1 ) | bit;
	}
	return v;
}

/* ---------------- independent SSE ---------------- */

static Int64 gb_indep_sse( h261_context_t *ps_ctx, int i_ox, int i_oy )
{
	h261_frame_t *s = &ps_ctx->s_current_frame;
	h261_frame_t *r = &ps_ctx->s_reconstructed_frame;
	Int64 sse = 0;
	int y, x, d;
	for( y = 0; y < 48; y++ )
	{
		const UInt8 *psrc = s->pui8_Y + ( i_oy + y ) * s->i_stride_y + i_ox;
		const UInt8 *prec = r->pui8_Y + ( i_oy + y ) * r->i_stride_y + i_ox;
		for( x = 0; x < 176; x++ ) { d = ( int ) prec[ x ] - ( int ) psrc[ x ]; sse += ( Int64 ) d * d; }
	}
	for( y = 0; y < 24; y++ )
	{
		const UInt8 *scb = s->pui8_Cb + ( ( i_oy >> 1 ) + y ) * s->i_stride_c + ( i_ox >> 1 );
		const UInt8 *rcb = r->pui8_Cb + ( ( i_oy >> 1 ) + y ) * r->i_stride_c + ( i_ox >> 1 );
		const UInt8 *scr = s->pui8_Cr + ( ( i_oy >> 1 ) + y ) * s->i_stride_c + ( i_ox >> 1 );
		const UInt8 *rcr = r->pui8_Cr + ( ( i_oy >> 1 ) + y ) * r->i_stride_c + ( i_ox >> 1 );
		for( x = 0; x < 88; x++ )
		{
			d = ( int ) rcb[ x ] - ( int ) scb[ x ]; sse += ( Int64 ) d * d;
			d = ( int ) rcr[ x ] - ( int ) scr[ x ]; sse += ( Int64 ) d * d;
		}
	}
	return sse;
}

/* ---------------- oracle ---------------- */

typedef struct
{
	h261_context_t s_o;
	h261_context_t s_snap_ctx;
	h261_mb_cache_t *ps_snap_cache;
	int i_cache_n;
	UInt8 *p_snap_y, *p_snap_cb, *p_snap_cr;
	UInt8 *p_snap_bs;
	int i_frame_w, i_frame_h;

	Int64 i64_rem, i64_target, i64_debt, i64_coded;
	int i_ph_bits;
} gb_oracle_t;

static int gb_oracle_init( gb_oracle_t *po, int i_format )
{
	int w = i_format == H261_SOURCE_FORMAT_CIF ? 352 : 176;
	int h = i_format == H261_SOURCE_FORMAT_CIF ? 288 : 144;
	memset( po, 0, sizeof( *po ) );
	if( !fixture_init( &po->s_o, i_format, H261_RATECTRL_MODE_QUANT ) ) return 0;
	po->i_frame_w = w; po->i_frame_h = h;
	po->i_cache_n = ( w / 16 ) * ( h / 16 );
	po->ps_snap_cache = calloc( ( size_t ) po->i_cache_n, sizeof( h261_mb_cache_t ) );
	po->p_snap_y = malloc( ( size_t ) w * h );
	po->p_snap_cb = malloc( ( size_t ) w * h / 4 );
	po->p_snap_cr = malloc( ( size_t ) w * h / 4 );
	po->p_snap_bs = malloc( po->s_o.s_bitstream.i_length );
	if( !po->ps_snap_cache || !po->p_snap_y || !po->p_snap_cb || !po->p_snap_cr || !po->p_snap_bs ) return 0;
	return 1;
}

static void gb_oracle_free( gb_oracle_t *po )
{
	free( po->ps_snap_cache ); free( po->p_snap_y ); free( po->p_snap_cb );
	free( po->p_snap_cr ); free( po->p_snap_bs );
	fixture_free( &po->s_o );
}

static void gb_oracle_save( gb_oracle_t *po )
{
	h261_context_t *o = &po->s_o;
	int w = po->i_frame_w, h = po->i_frame_h;
	Int32 n;
	po->s_snap_ctx = *o;
	if( o->ps_mb_cache )
		memcpy( po->ps_snap_cache, o->ps_mb_cache, sizeof( h261_mb_cache_t ) * ( size_t ) po->i_cache_n );
	memcpy( po->p_snap_y, o->s_reconstructed_frame.pui8_Y, ( size_t ) w * h );
	memcpy( po->p_snap_cb, o->s_reconstructed_frame.pui8_Cb, ( size_t ) w * h / 4 );
	memcpy( po->p_snap_cr, o->s_reconstructed_frame.pui8_Cr, ( size_t ) w * h / 4 );
	n = ( Int32 )( o->s_bitstream.pui8_codeword_ptr - o->s_bitstream.pui8_bitstream );
	memcpy( po->p_snap_bs, o->s_bitstream.pui8_bitstream, ( size_t ) n );
}

static void gb_oracle_restore( gb_oracle_t *po )
{
	h261_context_t *o = &po->s_o;
	int w = po->i_frame_w, h = po->i_frame_h;
	Int32 n;
	*o = po->s_snap_ctx;
	if( o->ps_mb_cache )
		memcpy( o->ps_mb_cache, po->ps_snap_cache, sizeof( h261_mb_cache_t ) * ( size_t ) po->i_cache_n );
	memcpy( o->s_reconstructed_frame.pui8_Y, po->p_snap_y, ( size_t ) w * h );
	memcpy( o->s_reconstructed_frame.pui8_Cb, po->p_snap_cb, ( size_t ) w * h / 4 );
	memcpy( o->s_reconstructed_frame.pui8_Cr, po->p_snap_cr, ( size_t ) w * h / 4 );
	n = ( Int32 )( o->s_bitstream.pui8_codeword_ptr - o->s_bitstream.pui8_bitstream );
	memcpy( o->s_bitstream.pui8_bitstream, po->p_snap_bs, ( size_t ) n );
}

static void gb_oracle_begin_frame( gb_oracle_t *po, int i_frame )
{
	h261_context_t *o = &po->s_o;
	h261_picture_parameters_t p;
	int b0, b1;
	memset( &p, 0, sizeof( p ) );
	p.i_frame_num = i_frame;
	p.i_source_format = o->i_source_format;
	p.i_frame_type = i_frame ? H261_FRAME_TYPE_INTER : H261_FRAME_TYPE_INTRA;
	p.i_quantiser = 10;
	o->i_frame_counter = i_frame;
	h261_ratectrl_init_picture( o, &p );
	h261_init_picture( o, &p );
	b0 = h261_bitstream_bits( &o->s_bitstream );
	h261_write_picture_header( o );
	b1 = h261_bitstream_bits( &o->s_bitstream );
	po->i_ph_bits = b1 - b0;
}

static void gb_oracle_end_frame( gb_oracle_t *po )
{
	h261_context_t *o = &po->s_o;
	UInt8 *bytes; UInt32 n;
	h261_bitstream_get( &o->s_bitstream, &bytes, &n );
	h261_bitstream_advance( &o->s_bitstream );
	h261_ratectrl_update_picture( o, ( Int32 ) n );
	memcpy( o->s_reference_frame.pui8_Y, o->s_reconstructed_frame.pui8_Y,
		( size_t ) o->s_reconstructed_frame.i_width * o->s_reconstructed_frame.i_height );
	memcpy( o->s_reference_frame.pui8_Cb, o->s_reconstructed_frame.pui8_Cb,
		( size_t ) o->s_reconstructed_frame.i_width * o->s_reconstructed_frame.i_height / 4 );
	memcpy( o->s_reference_frame.pui8_Cr, o->s_reconstructed_frame.pui8_Cr,
		( size_t ) o->s_reconstructed_frame.i_width * o->s_reconstructed_frame.i_height / 4 );
}

/*
 * Enumerate one GOB independently, choose the winner, compare against the
 * observed stats record, and leave the oracle committed to the winner.
 * Returns 0 on full agreement.
 */
static int gb_oracle_gob( gb_oracle_t *po, int i_format, int i_gidx,
	int i_bitrate, int i_fps_num, int i_fps_den, int i_buffer_bits,
	const h261_gob_budget_stats_t *w )
{
	h261_context_t *o = &po->s_o;
	int gn, ox, oy, q;
	int i_g = gb_gob_count( i_format );
	Int64 N = ( Int64 ) i_bitrate * i_fps_den;
	Int64 D = ( Int64 ) i_fps_num * i_g;
	Int64 v = po->i64_rem + N;
	Int64 quota = v / D;
	Int64 newrem = v - quota * D;
	Int64 dbefore = po->i64_debt;

	int have_fit = 0, have_of = 0;
	int fit_q = 0, of_q = 0;
	Int64 fit_bits = 0, fit_sse = 0, of_bits = 0, of_sse = 0;
	Int64 win_bits, win_sse;
	int win_q, win_ovf;

	gb_gob_geom( i_format, i_gidx, &gn, &ox, &oy );

	gb_oracle_save( po );

	for( q = 1; q <= GB_TRIALS; q++ )
	{
		int b0, b1;
		Int64 gob_bits, bits, sse;

		gb_oracle_restore( po );
		o->s_picture.i_quantiser = q;

		b0 = h261_bitstream_bits( &o->s_bitstream );
		h261_encode_gob( o, gn, ox, oy );
		b1 = h261_bitstream_bits( &o->s_bitstream );
		gob_bits = ( Int64 )( b1 - b0 );
		bits = gob_bits + ( i_gidx == 0 ? po->i_ph_bits : 0 );
		sse = gb_indep_sse( o, ox, oy );

		if( dbefore + bits - quota <= ( Int64 ) i_buffer_bits )
		{
			if( !have_fit || sse < fit_sse ||
				( sse == fit_sse && ( bits < fit_bits || ( bits == fit_bits && q < fit_q ) ) ) )
			{
				have_fit = 1; fit_q = q; fit_bits = bits; fit_sse = sse;
			}
		}
		else
		{
			if( !have_of || bits < of_bits ||
				( bits == of_bits && ( sse < of_sse || ( sse == of_sse && q < of_q ) ) ) )
			{
				have_of = 1; of_q = q; of_bits = bits; of_sse = sse;
			}
		}
	}

	if( have_fit )
	{
		win_q = fit_q; win_bits = fit_bits; win_sse = fit_sse; win_ovf = 0;
	}
	else
	{
		win_q = of_q; win_bits = of_bits; win_sse = of_sse; win_ovf = 1;
	}

	/* compare against implementation record */
	if( w->gob_number != gn ) { fprintf( stderr, "gob number %d != %d\n", w->gob_number, gn ); return 1; }
	if( w->gquant != win_q ) { fprintf( stderr, "gquant gob%d impl%d oracle%d\n", gn, w->gquant, win_q ); return 2; }
	if( w->overflow != win_ovf ) { fprintf( stderr, "overflow mismatch gob%d\n", gn ); return 3; }
	if( w->bits != win_bits ) { fprintf( stderr, "bits gob%d impl%lld oracle%lld\n", gn, (long long)w->bits, (long long)win_bits ); return 4; }
	if( w->sse != win_sse ) { fprintf( stderr, "sse gob%d impl%lld oracle%lld\n", gn, (long long)w->sse, (long long)win_sse ); return 5; }
	if( w->quota != quota ) { fprintf( stderr, "quota gob%d\n", gn ); return 6; }

	/* commit oracle winner through the old core (test-side reference) */
	gb_oracle_restore( po );
	o->s_picture.i_quantiser = win_q;
	h261_encode_gob( o, gn, ox, oy );

	po->i64_rem = newrem;
	po->i64_target += quota;
	po->i64_debt += win_bits - quota;
	po->i64_coded += win_bits;

	if( w->target_total != po->i64_target ) return 7;
	if( w->debt_after != po->i64_debt ) return 8;
	if( w->coded_total != po->i64_coded ) return 9;

	return 0;
}

/* ---------------- whole scenario with oracle + stream ---------------- */

typedef struct
{
	gb_collector_t col;
	UInt8 *p_stream;
	size_t z_stream_len;
	Int64 i64_coded_bits;
} gb_scenario_t;

static int gb_run_scenario( gb_scenario_t *psc, int i_format, gb_fill_fn pf_fill, int i_frames,
	int i_bitrate, int i_fps_num, int i_fps_den, int i_buffer_bits, int i_use_oracle )
{
	h261_context_t c;
	gb_oracle_t oracle;
	h261_gob_budget_config_t cfg;
	FILE *pf;
	int f, g, rc = 0;
	int i_g = gb_gob_count( i_format );
	long z_size;

	memset( psc, 0, sizeof( *psc ) );
	memset( &cfg, 0, sizeof( cfg ) );

	if( !fixture_init( &c, i_format, H261_RATECTRL_MODE_QUANT ) ) return 100;

	psc->col.ps_ctx = &c;
	psc->col.i64_last_entries = 0;
	cfg.bitrate = i_bitrate;
	cfg.fps_num = i_fps_num;
	cfg.fps_den = i_fps_den;
	cfg.buffer_bits = i_buffer_bits;
	cfg.observe = gb_on_observe;
	cfg.opaque = &psc->col;
	if( !h261_ratectrl_init_gob_budget( &c, &cfg ) ) { fixture_free( &c ); return 101; }

	if( i_use_oracle && !gb_oracle_init( &oracle, i_format ) ) { fixture_free( &c ); return 102; }

	pf = tmpfile();
	if( !pf ) { fixture_free( &c ); if( i_use_oracle ) gb_oracle_free( &oracle ); return 103; }

	for( f = 0; f < i_frames; f++ )
	{
		unsigned ui_frame_bits;

		pf_fill( &c, f );
		if( i_use_oracle ) pf_fill( &oracle.s_o, f );

		ui_frame_bits = fixture_encode( &c, f, pf );

		if( i_use_oracle )
		{
			gb_oracle_begin_frame( &oracle, f );
			for( g = 0; g < i_g; g++ )
			{
				const h261_gob_budget_stats_t *w = &psc->col.recs[ f * i_g + g ];
				if( f * i_g + g >= psc->col.i_count ) { rc = 104; break; }
				rc = gb_oracle_gob( &oracle, i_format, g, i_bitrate, i_fps_num, i_fps_den, i_buffer_bits, w );
				if( rc ) break;
			}
			if( !rc ) gb_oracle_end_frame( &oracle );

			/* P-frame pollution: committed reconstruction must match oracle. */
			if( !rc )
			{
				size_t ysz = ( size_t ) c.s_reconstructed_frame.i_width * c.s_reconstructed_frame.i_height;
				if( memcmp( c.s_reconstructed_frame.pui8_Y, oracle.s_o.s_reconstructed_frame.pui8_Y, ysz ) != 0 ||
					memcmp( c.s_reconstructed_frame.pui8_Cb, oracle.s_o.s_reconstructed_frame.pui8_Cb, ysz / 4 ) != 0 ||
					memcmp( c.s_reconstructed_frame.pui8_Cr, oracle.s_o.s_reconstructed_frame.pui8_Cr, ysz / 4 ) != 0 )
				{
					fprintf( stderr, "recon divergence frame %d\n", f );
					rc = 105;
				}
			}
			if( rc ) break;
		}

		/* frame bits must equal the sum of this frame's stats bits */
		{
			Int64 sum = 0;
			for( g = 0; g < i_g; g++ ) sum += psc->col.recs[ f * i_g + g ].bits;
			if( sum != ( Int64 ) ui_frame_bits )
			{
				fprintf( stderr, "frame %d stats bits %lld != fixture %u\n", f, (long long)sum, ui_frame_bits );
				rc = 106; break;
			}
		}
	}

	/* final padding byte is emitted but never counted */
	if( !rc )
	{
		UInt8 *tail; UInt32 tlen;
		h261_bitstream_flush( &c.s_bitstream, &tail, &tlen );
		if( tlen ) fwrite( tail, 1, tlen, pf );
	}

	if( !rc )
	{
		fflush( pf );
		z_size = ftell( pf );
		rewind( pf );
		psc->p_stream = malloc( ( size_t ) z_size );
		if( psc->p_stream && fread( psc->p_stream, 1, ( size_t ) z_size, pf ) == ( size_t ) z_size )
		{
			psc->z_stream_len = ( size_t ) z_size;
		}
		else rc = 107;
	}
	fclose( pf );

	if( !rc && psc->col.i_count )
	{
		psc->i64_coded_bits = psc->col.recs[ psc->col.i_count - 1 ].coded_total;
		if( ( Int64 ) psc->z_stream_len != ( psc->i64_coded_bits + 7 ) / 8 )
		{
			fprintf( stderr, "file bytes %zu vs coded bits %lld -> %lld\n",
				psc->z_stream_len, (long long)psc->i64_coded_bits,
				(long long)( ( psc->i64_coded_bits + 7 ) / 8 ) );
			rc = 108;
		}
	}

	if( i_use_oracle ) gb_oracle_free( &oracle );
	fixture_free( &c );
	return rc;
}

static void gb_scenario_free( gb_scenario_t *psc )
{
	free( psc->p_stream );
	psc->p_stream = NULL;
}

/*
 * Independent stream parse: walk every observed record using its real bit
 * count to locate the header, then read GOB number / GQUANT straight from
 * the bytes.  Also scan for all start codes and check the exact sequence.
 */
static int gb_parse_stream( const gb_scenario_t *psc, int i_format, int i_frames )
{
	int i_g = gb_gob_count( i_format );
	size_t bits = ( size_t ) psc->i64_coded_bits;
	const UInt8 *p = psc->p_stream;
	int f, g, recidx = 0;
	size_t pos = 0;
	int total_events = 0;

	if( psc->z_stream_len < ( bits + 7 ) / 8 ) return 1;

	/* whole-stream start code scan */
	for( size_t sp = 0; sp + 20 <= bits; sp++ )
	{
		UInt32 u16 = gb_bits_at( p, bits, sp, 16 );
		UInt32 u4;
		if( u16 != 1 ) continue;
		u4 = gb_bits_at( p, bits, sp + 16, 4 );
		if( u4 == 0 || ( i_format == H261_SOURCE_FORMAT_CIF ? ( u4 >= 1 && u4 <= 12 )
		                                                     : ( u4 == 1 || u4 == 3 || u4 == 5 ) ) )
		{
			total_events++;
		}
	}
	if( total_events != i_frames * ( 1 + i_g ) )
	{
		fprintf( stderr, "start code events %d expected %d\n", total_events, i_frames * ( 1 + i_g ) );
		return 2;
	}

	for( f = 0; f < i_frames; f++ )
	{
		/* picture start code: 20 bits value 16 */
		if( gb_bits_at( p, bits, pos, 20 ) != 16 ) { fprintf( stderr, "no PSC frame %d\n", f ); return 3; }
		/* temporal reference follows and must keep its own meaning */
		{
			UInt32 tr = gb_bits_at( p, bits, pos + 20, 5 );
			if( tr != ( UInt32 )( f & 31 ) ) { fprintf( stderr, "TR %u f%d\n", tr, f ); return 4; }
		}
		for( g = 0; g < i_g; g++ )
		{
			const h261_gob_budget_stats_t *w = &psc->col.recs[ recidx ];
			size_t gobpos;
			UInt32 gn, gq;
			int eg_n, ex, ey;
			(void) ex; (void) ey;

			if( g == 0 )
			{
				/* picture header is exactly 32 bits in this encoder */
				gobpos = pos + 32;
			}
			else
			{
				gobpos = pos; /* pos already at this GOB start */
			}

			if( gb_bits_at( p, bits, gobpos, 16 ) != 1 ) return 5;
			gn = gb_bits_at( p, bits, gobpos + 16, 4 );
			gq = gb_bits_at( p, bits, gobpos + 20, 5 );
			gb_gob_geom( i_format, g, &eg_n, &ex, &ey );
			if( ( int ) gn != eg_n ) { fprintf( stderr, "parse gn %u != %d\n", gn, eg_n ); return 6; }
			if( ( int ) gn != w->gob_number ) return 7;
			if( ( int ) gq != w->gquant ) { fprintf( stderr, "parse gq %u != stats %d\n", gq, w->gquant ); return 8; }
			if( gq < 1 || gq > 31 ) return 9;

			/* advance using the independently observed real bit count */
			if( g == 0 )
			{
				pos = gobpos + ( size_t )( w->bits - 32 );
			}
			else
			{
				pos = gobpos + ( size_t ) w->bits;
			}
			recidx++;
		}
		/* next picture starts exactly here (or stream ends) */
		if( f + 1 < i_frames )
		{
			if( gb_bits_at( p, bits, pos, 20 ) != 16 ) { fprintf( stderr, "next PSC missing f%d pos%zu\n", f, pos ); return 10; }
		}
	}
	if( pos != bits ) { fprintf( stderr, "end pos %zu != bits %zu\n", pos, bits ); return 11; }
	return 0;
}

/* ================================================================== */
/* clock group                                                         */
/* ================================================================== */

static Int64 gb_ref_target( Int64 K, Int64 br, Int64 fn, Int64 fd, Int64 G )
{
	__int128 num = ( __int128 ) K * br * fd;
	__int128 den = ( __int128 ) fn * G;
	return ( Int64 )( num / den );
}

static int case_clock_qcif_million( void )
{
	h261_context_t c;
	h261_gob_budget_config_t cfg;
	Int64 K, mism = 0, lastq = 0;
	const Int64 br = 1234567, fn = 30000, fd = 1001, G = 3;
	TASK_CHECK( fixture_init( &c, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	memset( &cfg, 0, sizeof( cfg ) );
	cfg.bitrate = ( Int32 ) br; cfg.fps_num = ( Int32 ) fn; cfg.fps_den = ( Int32 ) fd; cfg.buffer_bits = 50000;
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &cfg ) == 1 );
	for( K = 1; K <= 1000037; K++ )
	{
		Int64 q;
		Int64 t = h261_gob_budget_debug_clock_tick( &c, &q );
		Int64 ref = gb_ref_target( K, br, fn, fd, G );
		if( t != ref || q != ref - gb_ref_target( K - 1, br, fn, fd, G ) ) mism++;
		lastq = q;
	}
	TASK_CHECK( mism == 0 );
	TASK_CHECK( lastq >= 0 );
	fixture_free( &c );
	return 0;
}

static int case_clock_cif_million( void )
{
	h261_context_t c;
	h261_gob_budget_config_t cfg;
	Int64 K, mism = 0;
	/* extreme but legal values exercise overflow-safe arithmetic */
	const Int64 br = 100000000, fn = 120, fd = 1, G = 12;
	TASK_CHECK( fixture_init( &c, H261_SOURCE_FORMAT_CIF, H261_RATECTRL_MODE_QUANT ) );
	memset( &cfg, 0, sizeof( cfg ) );
	cfg.bitrate = 100000000; cfg.fps_num = 120; cfg.fps_den = 1; cfg.buffer_bits = 1000000000;
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &cfg ) == 1 );
	for( K = 1; K <= 1000013; K++ )
	{
		Int64 t = h261_gob_budget_debug_clock_tick( &c, NULL );
		if( t != gb_ref_target( K, br, fn, fd, G ) ) mism++;
	}
	TASK_CHECK( mism == 0 );
	fixture_free( &c );
	return 0;
}

static int case_clock_config_validation( void )
{
	h261_context_t c;
	h261_gob_budget_config_t good, bad;
	Int64 q1, q2;

	TASK_CHECK( fixture_init( &c, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	memset( &good, 0, sizeof( good ) );
	good.bitrate = 800000; good.fps_num = 25; good.fps_den = 1; good.buffer_bits = 40000;
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, NULL ) == 0 );
	TASK_CHECK( c.ps_gob_budget == NULL );
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &good ) == 1 );
	(void) h261_gob_budget_debug_clock_tick( &c, &q1 );

	bad = good; bad.bitrate = 0;          TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = good; bad.bitrate = 100000001;  TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = good; bad.fps_num = 0;          TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = good; bad.fps_num = 1000001;    TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = good; bad.fps_den = 0;          TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = good; bad.fps_den = 1000001;    TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = good; bad.buffer_bits = 0;      TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = good; bad.buffer_bits = 1000000001; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = good; bad.fps_num = 1; bad.fps_den = 2; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 ); /* ratio < 1 */
	bad = good; bad.fps_num = 1000000; bad.fps_den = 1; /* ratio 1e6 > 120 */
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );

	/* failed re-init leaves the previous control state and clock config. */
	q2 = 0;
	(void) h261_gob_budget_debug_clock_tick( &c, &q2 );
	{
		Int64 expect_q = gb_ref_target( 2, 800000, 25, 1, 3 ) - gb_ref_target( 1, 800000, 25, 1, 3 );
		TASK_CHECK( q2 == expect_q );
	}

	/* old ratectrl init switches budget mode off */
	h261_init_ratectrl( &c, 10, 64000, 64000, H261_RATECTRL_MODE_QUANT );
	TASK_CHECK( c.ps_gob_budget == NULL );
	fixture_free( &c );
	return 0;
}

/* ================================================================== */
/* selection group                                                     */
/* ================================================================== */

static int check_entries_and_trials( const gb_scenario_t *psc )
{
	int i;
	for( i = 0; i < psc->col.i_count; i++ )
	{
		if( psc->col.recs[ i ].trials != 31 ) return 1;
		if( psc->col.ai64_entry_delta[ i ] != 31 )
		{
			fprintf( stderr, "entry delta rec%d = %lld\n", i, (long long)psc->col.ai64_entry_delta[ i ] );
			return 2;
		}
	}
	return 0;
}

static int case_select_flat_qcif_ties( void )
{
	gb_scenario_t sc;
	int rc, f, g, distinct_one = 0;
	/* generous buffer: flat intra ties SSE and bits over q=1..14 -> smallest q */
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_QCIF, gb_fill_flat, 2, 400000, 30, 1, 60000, 1 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( sc.col.i_count == 6 );
	TASK_CHECK( check_entries_and_trials( &sc ) == 0 );
	for( f = 0; f < 2; f++ )
	{
		int qs[ 32 ] = { 0 }, nq = 0;
		for( g = 0; g < 3; g++ )
		{
			const h261_gob_budget_stats_t *w = &sc.col.recs[ f * 3 + g ];
			int k, seen = 0;
			TASK_CHECK( w->overflow == 0 );
			TASK_CHECK( w->gquant >= 1 && w->gquant <= 31 );
			for( k = 0; k < nq; k++ ) if( qs[ k ] == w->gquant ) seen = 1;
			if( !seen ) qs[ nq++ ] = w->gquant;
		}
		if( nq >= 2 ) distinct_one = 1;
	}
	/* flat tie rule must pick the smallest base quant somewhere */
	TASK_CHECK( sc.col.recs[ 0 ].gquant == 1 );
	(void) distinct_one;
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_QCIF, 2 ) == 0 );
	gb_scenario_free( &sc );
	return 0;
}

static int case_select_texture_qcif_variation( void )
{
	gb_scenario_t sc;
	int rc, f, g, b_variation = 0, b_feasible = 0;
	/* the supplied procedural block texture produces real in-frame changes */
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_QCIF, fixture_fill, 3, 100000, 30, 1, 4000, 1 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( sc.col.i_count == 9 );
	TASK_CHECK( check_entries_and_trials( &sc ) == 0 );
	for( f = 0; f < 3; f++ )
	{
		int qs[ 32 ] = { 0 }, nq = 0;
		for( g = 0; g < 3; g++ )
		{
			const h261_gob_budget_stats_t *w = &sc.col.recs[ f * 3 + g ];
			int k, seen = 0;
			if( !w->overflow ) b_feasible = 1;
			for( k = 0; k < nq; k++ ) if( qs[ k ] == w->gquant ) seen = 1;
			if( !seen ) qs[ nq++ ] = w->gquant;
		}
		if( nq >= 2 ) b_variation = 1;
	}
	TASK_CHECK( b_feasible );
	TASK_CHECK( b_variation ); /* same-frame quant change must really happen */
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_QCIF, 3 ) == 0 );
	gb_scenario_free( &sc );
	return 0;
}

static int case_select_overflow_all( void )
{
	gb_scenario_t sc;
	int rc, i;
	/* bitrate 1 bit/s, one-bit buffer: no real GOB can fit. */
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_QCIF, gb_fill_flat, 2, 1, 1, 1, 1, 1 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( sc.col.i_count == 6 );
	for( i = 0; i < sc.col.i_count; i++ )
	{
		TASK_CHECK( sc.col.recs[ i ].overflow == 1 );
		TASK_CHECK( sc.col.recs[ i ].gquant >= 1 && sc.col.recs[ i ].gquant <= 31 );
		TASK_CHECK( sc.col.recs[ i ].bits > 0 );
	}
	TASK_CHECK( check_entries_and_trials( &sc ) == 0 );
	/* complete legal stream despite every GOB overflowing */
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_QCIF, 2 ) == 0 );
	gb_scenario_free( &sc );

	/* textured overflow ordering checked independently too */
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_QCIF, gb_fill_texture, 1, 1, 1, 1, 1, 1 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( sc.col.recs[ 0 ].overflow == 1 );
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_QCIF, 1 ) == 0 );
	gb_scenario_free( &sc );
	return 0;
}

static int case_select_moving_cif_oracle( void )
{
	gb_scenario_t sc;
	int rc;
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_CIF, gb_fill_moving, 2, 900000, 30, 1, 60000, 1 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( sc.col.i_count == 24 );
	TASK_CHECK( check_entries_and_trials( &sc ) == 0 );
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_CIF, 2 ) == 0 );
	gb_scenario_free( &sc );
	return 0;
}

/* ================================================================== */
/* stream group                                                        */
/* ================================================================== */

static int case_stream_parse_texture_qcif( void )
{
	gb_scenario_t sc;
	int rc;
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_QCIF, gb_fill_texture, 4, 220000, 25, 1, 30000, 0 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( sc.col.i_count == 12 );
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_QCIF, 4 ) == 0 );
	gb_scenario_free( &sc );
	return 0;
}

static int case_stream_parse_texture_cif( void )
{
	gb_scenario_t sc;
	int rc;
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_CIF, gb_fill_texture, 2, 1400000, 25, 1, 120000, 0 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( sc.col.i_count == 24 );
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_CIF, 2 ) == 0 );
	gb_scenario_free( &sc );
	return 0;
}

static int case_stream_cross_frame_residue( void )
{
	gb_scenario_t sc;
	int rc, f, b_found_residue = 0;
	/* odd rational fps and modest budget make non-byte-aligned ends likely */
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_QCIF, fixture_fill, 5, 137000, 30000, 1001, 12000, 1 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_QCIF, 5 ) == 0 );
	/* look for a frame boundary whose cumulative effective bits leave 1..7 residue */
	for( f = 0; f < 5; f++ )
	{
		Int64 coded = sc.col.recs[ f * 3 + 2 ].coded_total;
		if( coded % 8 != 0 ) b_found_residue = 1;
	}
	TASK_CHECK( b_found_residue );
	gb_scenario_free( &sc );
	return 0;
}

static int case_stream_pframe_unpolluted( void )
{
	gb_scenario_t sc;
	int rc;
	/* I frame plus four P frames; oracle byte-compares recon after each frame */
	rc = gb_run_scenario( &sc, H261_SOURCE_FORMAT_QCIF, gb_fill_moving, 5, 160000, 30, 1, 6000, 1 );
	TASK_CHECK( rc == 0 );
	TASK_CHECK( sc.col.i_count == 15 );
	TASK_CHECK( gb_parse_stream( &sc, H261_SOURCE_FORMAT_QCIF, 5 ) == 0 );
	gb_scenario_free( &sc );
	return 0;
}

static int gb_run_collect_only( int fmt, gb_fill_fn fill, int frames, int br, int fn, int fd, int buf,
	gb_collector_t *col, UInt8 **pp_stream, size_t *pz_len )
{
	h261_context_t c;
	h261_gob_budget_config_t cfg;
	FILE *pf = tmpfile();
	int f, rc = 0;
	long z;
	if( !fixture_init( &c, fmt, H261_RATECTRL_MODE_QUANT ) ) return 1;
	memset( col, 0, sizeof( *col ) );
	col->ps_ctx = &c;
	memset( &cfg, 0, sizeof( cfg ) );
	cfg.bitrate = br; cfg.fps_num = fn; cfg.fps_den = fd; cfg.buffer_bits = buf;
	cfg.observe = gb_on_observe; cfg.opaque = col;
	if( !h261_ratectrl_init_gob_budget( &c, &cfg ) ) { fixture_free( &c ); fclose( pf ); return 2; }
	if( !pf ) { fixture_free( &c ); return 3; }
	for( f = 0; f < frames; f++ )
	{
		fill( &c, f );
		fixture_encode( &c, f, pf );
	}
	{
		UInt8 *tail; UInt32 tl;
		h261_bitstream_flush( &c.s_bitstream, &tail, &tl );
		if( tl ) fwrite( tail, 1, tl, pf );
	}
	fflush( pf ); z = ftell( pf ); rewind( pf );
	*pp_stream = malloc( ( size_t ) z );
	if( fread( *pp_stream, 1, ( size_t ) z, pf ) != ( size_t ) z ) rc = 4;
	*pz_len = ( size_t ) z;
	fclose( pf );
	fixture_free( &c );
	return rc;
}

static int case_stream_two_instances_interleaved( void )
{
	/* standalone references */
	gb_collector_t a0, b0;
	UInt8 *sa0, *sb0; size_t la0, lb0;
	/* interleaved pair */
	h261_context_t a, b;
	gb_collector_t ca, cb;
	h261_gob_budget_config_t cfg;
	int f, i, rc;

	TASK_CHECK( gb_run_collect_only( H261_SOURCE_FORMAT_QCIF, gb_fill_texture, 3,
		150000, 30, 1, 9000, &a0, &sa0, &la0 ) == 0 );
	TASK_CHECK( gb_run_collect_only( H261_SOURCE_FORMAT_CIF, gb_fill_flat, 2,
		900000, 30, 1, 80000, &b0, &sb0, &lb0 ) == 0 );

	TASK_CHECK( fixture_init( &a, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	TASK_CHECK( fixture_init( &b, H261_SOURCE_FORMAT_CIF, H261_RATECTRL_MODE_QUANT ) );
	memset( &ca, 0, sizeof( ca ) ); ca.ps_ctx = &a;
	memset( &cb, 0, sizeof( cb ) ); cb.ps_ctx = &b;
	memset( &cfg, 0, sizeof( cfg ) );
	cfg.bitrate = 150000; cfg.fps_num = 30; cfg.fps_den = 1; cfg.buffer_bits = 9000;
	cfg.observe = gb_on_observe; cfg.opaque = &ca;
	TASK_CHECK( h261_ratectrl_init_gob_budget( &a, &cfg ) == 1 );
	cfg.bitrate = 900000; cfg.buffer_bits = 80000; cfg.opaque = &cb;
	TASK_CHECK( h261_ratectrl_init_gob_budget( &b, &cfg ) == 1 );

	for( f = 0; f < 3; f++ )
	{
		gb_fill_texture( &a, f );
		fixture_encode( &a, f, NULL );
		if( f < 2 )
		{
			gb_fill_flat( &b, f );
			fixture_encode( &b, f, NULL );
		}
	}

	TASK_CHECK( ca.i_count == a0.i_count );
	TASK_CHECK( cb.i_count == b0.i_count );
	rc = 0;
	for( i = 0; i < a0.i_count; i++ )
		if( ca.recs[ i ].gquant != a0.recs[ i ].gquant || ca.recs[ i ].bits != a0.recs[ i ].bits ||
			ca.recs[ i ].sse != a0.recs[ i ].sse ) rc = 1;
	for( i = 0; i < b0.i_count; i++ )
		if( cb.recs[ i ].gquant != b0.recs[ i ].gquant || cb.recs[ i ].bits != b0.recs[ i ].bits ||
			cb.recs[ i ].sse != b0.recs[ i ].sse ) rc = 2;
	TASK_CHECK( rc == 0 );

	fixture_free( &a );
	fixture_free( &b );
	free( sa0 ); free( sb0 );
	return 0;
}

/* ---------------- QUANT / VBR byte compatibility ---------------- */

static uint64_t gb_fnv_bytes( uint64_t h, const unsigned char *p, size_t n )
{
	size_t i;
	for( i = 0; i < n; i++ ) { h ^= p[ i ]; h *= UINT64_C( 1099511628211 ); }
	return h;
}

static int case_stream_quant_vbr_compat( void )
{
	/*
	 * Reproduce task_support/baseline exactly (same fixture, same hashing) and
	 * compare against the checked-in baseline.expected vectors.
	 */
	struct { int mode, fmt; unsigned bits; uint64_t stream, recon; } exp[ 3 ] = {
		{ 1, 0, 10212, UINT64_C( 0xe4896ee0be94656c ), UINT64_C( 0x834789da8ba43673 ) },
		{ 2, 0,  9302, UINT64_C( 0xbf9ecc18898463b3 ), UINT64_C( 0x8ebd45d84158d294 ) },
		{ 1, 1, 39214, UINT64_C( 0x5e855ce0198aaa34 ), UINT64_C( 0xb7a34323f87da662 ) }
	};
	int k;
	for( k = 0; k < 3; k++ )
	{
		h261_context_t c;
		FILE *out = tmpfile();
		uint64_t sh = UINT64_C( 1469598103934665603 );
		uint64_t rh = UINT64_C( 1469598103934665603 );
		unsigned total = 0;
		int b;
		long sz;
		unsigned char *buf;
		int f;
		TASK_CHECK( out != NULL );
		TASK_CHECK( fixture_init( &c, exp[ k ].fmt, exp[ k ].mode ) );
		TASK_CHECK( c.ps_gob_budget == NULL );
		for( f = 0; f < 3; f++ )
		{
			size_t pixels;
			fixture_fill( &c, f );
			unsigned bits = fixture_encode( &c, f, out );
			total += bits;
			pixels = ( size_t ) c.s_reconstructed_frame.i_width * c.s_reconstructed_frame.i_height;
			rh = gb_fnv_bytes( rh, c.s_reconstructed_frame.pui8_Y, pixels );
			rh = gb_fnv_bytes( rh, c.s_reconstructed_frame.pui8_Cb, pixels / 4 );
			rh = gb_fnv_bytes( rh, c.s_reconstructed_frame.pui8_Cr, pixels / 4 );
		}
		{
			UInt8 *tail; UInt32 tl;
			h261_bitstream_flush( &c.s_bitstream, &tail, &tl );
			if( tl ) fwrite( tail, 1, tl, out );
		}
		TASK_CHECK( total == exp[ k ].bits );
		sz = ftell( out ); rewind( out );
		buf = malloc( ( size_t ) sz );
		TASK_CHECK( buf != NULL );
		TASK_CHECK( fread( buf, 1, ( size_t ) sz, out ) == ( size_t ) sz );
		for( b = 0; b < sz; b++ ) sh = gb_fnv_bytes( sh, &buf[ b ], 1 );
		TASK_CHECK( sh == exp[ k ].stream );
		TASK_CHECK( rh == exp[ k ].recon );
		free( buf );
		fclose( out );
		fixture_free( &c );
	}
	return 0;
}

/* ================================================================== */
/* registration                                                        */
/* ================================================================== */

const struct task_case task_cases[] = {
	{ "clock",     "clock_qcif_million_floor",        case_clock_qcif_million },
	{ "clock",     "clock_cif_million_floor",         case_clock_cif_million },
	{ "clock",     "clock_config_validation",         case_clock_config_validation },

	{ "selection", "select_flat_qcif_ties",           case_select_flat_qcif_ties },
	{ "selection", "select_texture_qcif_variation",   case_select_texture_qcif_variation },
	{ "selection", "select_overflow_all",             case_select_overflow_all },
	{ "selection", "select_moving_cif_oracle",        case_select_moving_cif_oracle },

	{ "stream",    "stream_parse_texture_qcif",       case_stream_parse_texture_qcif },
	{ "stream",    "stream_parse_texture_cif",        case_stream_parse_texture_cif },
	{ "stream",    "stream_cross_frame_residue",      case_stream_cross_frame_residue },
	{ "stream",    "stream_pframe_unpolluted",        case_stream_pframe_unpolluted },
	{ "stream",    "stream_two_instances_interleaved", case_stream_two_instances_interleaved },
	{ "stream",    "stream_quant_vbr_compat",         case_stream_quant_vbr_compat }
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[ 0 ] );
