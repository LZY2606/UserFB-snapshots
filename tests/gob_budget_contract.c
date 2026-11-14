/*
 * GOB budget mode contract tests.
 *
 * The tests drive the real encoder on locally generated flat, textured and
 * moving QCIF/CIF content, independently parse the produced GOB headers, and
 * re-enumerate every base quantizer through the legacy core from the same
 * committed pre-GOB state. SSE, budget accounting and the selection ordering
 * are all recomputed independently and compared against the encoder.
 */

#include "check.h"
#include "fixture.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------ */
/* local content generators                                            */
/* ------------------------------------------------------------------ */

static void fill_flat( h261_context_t *c, int frame )
{
	h261_frame_t *f = &c->s_current_frame;
	( void ) frame;
	for( int y = 0; y < f->i_height; y++ )
		for( int x = 0; x < f->i_width; x++ )
			f->pui8_Y[ y * f->i_stride_y + x ] = ( UInt8 ) ( 40 + ( ( x / 16 ) * 17 + ( y / 16 ) * 29 ) % 180 );
	for( int y = 0; y < f->i_height / 2; y++ )
		for( int x = 0; x < f->i_width / 2; x++ ) {
			f->pui8_Cb[ y * f->i_stride_c + x ] = ( UInt8 ) ( 100 + ( x / 8 ) % 40 );
			f->pui8_Cr[ y * f->i_stride_c + x ] = ( UInt8 ) ( 120 + ( y / 8 ) % 40 );
		}
}

static void fill_texture( h261_context_t *c, int frame )
{
	h261_frame_t *f = &c->s_current_frame;
	for( int y = 0; y < f->i_height; y++ )
		for( int x = 0; x < f->i_width; x++ ) {
			unsigned v = ( unsigned ) ( ( x * 73 + y * 131 + frame * 19 ) ^ ( ( x * y + frame ) & 0xff ) );
			f->pui8_Y[ y * f->i_stride_y + x ] = ( UInt8 ) ( v & 0xff );
		}
	for( int y = 0; y < f->i_height / 2; y++ )
		for( int x = 0; x < f->i_width / 2; x++ ) {
			f->pui8_Cb[ y * f->i_stride_c + x ] = ( UInt8 ) ( ( x * 29 + y * 57 + frame ) & 0xff );
			f->pui8_Cr[ y * f->i_stride_c + x ] = ( UInt8 ) ( ( x * 47 + y * 31 + frame * 3 ) & 0xff );
		}
}

static void fill_motion( h261_context_t *c, int frame )
{
	h261_frame_t *f = &c->s_current_frame;
	int shift = frame * 7;
	for( int y = 0; y < f->i_height; y++ )
		for( int x = 0; x < f->i_width; x++ ) {
			int xs = ( x + shift ) % f->i_width;
			int v = ( ( ( xs / 8 ) * 31 + ( y / 8 ) * 53 ) ^ ( xs * 5 + y * 3 ) ) & 0xff;
			f->pui8_Y[ y * f->i_stride_y + x ] = ( UInt8 ) v;
		}
	for( int y = 0; y < f->i_height / 2; y++ )
		for( int x = 0; x < f->i_width / 2; x++ ) {
			int xs = ( x + shift / 2 ) % ( f->i_width / 2 );
			f->pui8_Cb[ y * f->i_stride_c + x ] = ( UInt8 ) ( ( xs * 13 + y * 7 + 64 ) & 0xff );
			f->pui8_Cr[ y * f->i_stride_c + x ] = ( UInt8 ) ( ( xs * 11 + y * 17 + 96 ) & 0xff );
		}
}

typedef void ( *fill_fn )( h261_context_t *, int );


/* ------------------------------------------------------------------ */
/* geometry and small helpers                                          */
/* ------------------------------------------------------------------ */

typedef struct { int num, ox, oy; } gob_geom_t;

static const gob_geom_t qcif_geom[ 3 ] = { { 1, 0, 0 }, { 3, 0, 48 }, { 5, 0, 96 } };
static const gob_geom_t cif_geom[ 12 ] = {
	{ 1, 0, 0 }, { 2, 176, 0 }, { 3, 0, 48 }, { 4, 176, 48 },
	{ 5, 0, 96 }, { 6, 176, 96 }, { 7, 0, 144 }, { 8, 176, 144 },
	{ 9, 0, 192 }, { 10, 176, 192 }, { 11, 0, 240 }, { 12, 176, 240 }
};

static int gobs_for( int format ) { return format == H261_SOURCE_FORMAT_CIF ? 12 : 3; }

static Int64 region_sse( h261_context_t *c, int ox, int oy )
{
	h261_frame_t *s = &c->s_current_frame, *r = &c->s_reconstructed_frame;
	Int64 acc = 0;
	for( int p = 0; p < 3; p++ ) {
		UInt8 *src, *rec;
		int w, h, ss, sr, x0, y0;
		if( p == 0 ) {
			src = s->pui8_Y; rec = r->pui8_Y; w = 176; h = 48;
			ss = s->i_stride_y; sr = r->i_stride_y; x0 = ox; y0 = oy;
		} else {
			src = ( p == 1 ) ? s->pui8_Cb : s->pui8_Cr;
			rec = ( p == 1 ) ? r->pui8_Cb : r->pui8_Cr;
			w = 88; h = 24; ss = s->i_stride_c; sr = r->i_stride_c;
			x0 = ox / 2; y0 = oy / 2;
		}
		for( int y = 0; y < h; y++ )
			for( int x = 0; x < w; x++ ) {
				int d = ( int ) src[ ( y + y0 ) * ss + x0 + x ] - ( int ) rec[ ( y + y0 ) * sr + x0 + x ];
				acc += ( Int64 ) d * d;
			}
	}
	return acc;
}

static void copy_ref_from_recon( h261_context_t *c )
{
	h261_frame_t *r = &c->s_reconstructed_frame, *ref = &c->s_reference_frame;
	size_t py = ( size_t ) r->i_stride_y * r->i_height;
	size_t pc = ( size_t ) r->i_stride_c * ( r->i_height / 2 );
	memcpy( ref->pui8_Y, r->pui8_Y, py );
	memcpy( ref->pui8_Cb, r->pui8_Cb, pc );
	memcpy( ref->pui8_Cr, r->pui8_Cr, pc );
}

static int planes_equal( h261_context_t *a, h261_context_t *b )
{
	h261_frame_t *fa = &a->s_reconstructed_frame, *fb = &b->s_reconstructed_frame;
	size_t py = ( size_t ) fa->i_stride_y * fa->i_height;
	size_t pc = ( size_t ) fa->i_stride_c * ( fa->i_height / 2 );
	return memcmp( fa->pui8_Y, fb->pui8_Y, py ) == 0 &&
		memcmp( fa->pui8_Cb, fb->pui8_Cb, pc ) == 0 &&
		memcmp( fa->pui8_Cr, fb->pui8_Cr, pc ) == 0;
}

static unsigned long long fnv( const unsigned char *p, size_t n )
{
	unsigned long long h = 1469598103934665603ULL;
	for( size_t i = 0; i < n; i++ ) { h ^= p[ i ]; h *= 1099511628211ULL; }
	return h;
}


/* ------------------------------------------------------------------ */
/* independent bitstream parser                                        */
/* ------------------------------------------------------------------ */

typedef struct { const UInt8 *p; int nbits, pos; } br_t;

static UInt32 br_get( br_t *b, int n )
{
	UInt32 v = 0;
	for( int i = 0; i < n; i++ ) {
		int bi = b->pos >> 3, sh = 7 - ( b->pos & 7 );
		UInt32 bit = ( bi * 8 < b->nbits ) ? ( ( b->p[ bi ] >> sh ) & 1u ) : 0u;
		v = ( v << 1 ) | bit;
		b->pos++;
	}
	return v;
}

typedef struct { int gob_num; int gquant; long bitpos; } parsed_gob_t;

/* extract picture and GOB header fields straight from the byte stream */
static int parse_stream( const UInt8 *buf, long nbytes, parsed_gob_t *out, int maxg, int *npic, int *ngob )
{
	br_t b; b.p = buf; b.nbits = ( int ) nbytes * 8; b.pos = 0;
	*npic = 0; *ngob = 0;
	while( b.pos + 25 <= b.nbits ) {
		long save = b.pos;
		if( br_get( &b, 20 ) == 0x10 ) {
			( *npic )++;
			br_get( &b, 5 ); br_get( &b, 6 ); br_get( &b, 1 );
			continue;
		}
		b.pos = save;
		if( br_get( &b, 16 ) == 0x1 ) {
			UInt32 num = br_get( &b, 4 );
			UInt32 q = br_get( &b, 5 );
			br_get( &b, 1 );
			if( *ngob < maxg ) {
				out[ *ngob ].gob_num = ( int ) num;
				out[ *ngob ].gquant = ( int ) q;
				out[ *ngob ].bitpos = save;
			}
			( *ngob )++;
			continue;
		}
		b.pos = save + 1;
	}
	return 0;
}


/* ------------------------------------------------------------------ */
/* observer record storage                                             */
/* ------------------------------------------------------------------ */

#define MAX_RECORDS 256

typedef struct {
	int tag;
	h261_gob_budget_stats_t st;
} record_t;

typedef struct {
	int tag;
	int count;
	record_t rec[ MAX_RECORDS ];
} observer_bag_t;

static void observe_cb( const h261_gob_budget_stats_t *s, void *op )
{
	observer_bag_t *bag = ( observer_bag_t * ) op;
	if( bag->count >= MAX_RECORDS ) {
		/* record buffer exhausted: flag via the tag field so tests fail */
		bag->tag = -1;
		return;
	}
	bag->rec[ bag->count ].tag = bag->tag;
	bag->rec[ bag->count ].st = *s;
	bag->count++;
}


/* ------------------------------------------------------------------ */
/* independent reference oracle                                        */
/* ------------------------------------------------------------------ */

typedef struct {
	h261_context_t ctx;
	h261_bitstream_t scratch;
	UInt8 *scratch_buf;
	h261_gob_t saved_gob;
	h261_mb_cache_t *saved_cache;
	UInt8 *saved_y, *saved_cb, *saved_cr;
	size_t cache_sz, plane_y, plane_c;
	Int64 debt, coded, target, remain;
	Int64 K;
} oracle_t;

static int oracle_init( oracle_t *o, int format )
{
	memset( o, 0, sizeof( *o ) );
	TASK_CHECK( fixture_init( &o->ctx, format, H261_RATECTRL_MODE_QUANT ) );
	h261_frame_t *f = &o->ctx.s_reconstructed_frame;
	o->plane_y = ( size_t ) f->i_stride_y * f->i_height;
	o->plane_c = ( size_t ) f->i_stride_c * ( f->i_height / 2 );
	o->cache_sz = ( size_t ) rgi16_mb_dim[ format ][ 0 ] * rgi16_mb_dim[ format ][ 1 ] * sizeof( h261_mb_cache_t );
	h261_bitstream_init( &o->scratch, 1 << 20 );
	o->scratch_buf = o->scratch.pui8_bitstream;
	o->saved_cache = ( h261_mb_cache_t * ) malloc( o->cache_sz );
	o->saved_y = ( UInt8 * ) malloc( o->plane_y );
	o->saved_cb = ( UInt8 * ) malloc( o->plane_c );
	o->saved_cr = ( UInt8 * ) malloc( o->plane_c );
	TASK_CHECK( o->saved_cache && o->saved_y && o->saved_cb && o->saved_cr );
	return 0;
}

static void oracle_free( oracle_t *o )
{
	h261_bitstream_deinit( &o->scratch );
	free( o->saved_cache ); free( o->saved_y ); free( o->saved_cb ); free( o->saved_cr );
	fixture_free( &o->ctx );
}

typedef struct {
	int gquant;
	Int64 bits, sse;
	int fits;
} trial_t;

static void oracle_snapshot( oracle_t *o )
{
	h261_context_t *c = &o->ctx;
	o->saved_gob = c->s_picture.s_groups_of_blocks;
	memcpy( o->saved_cache, c->ps_mb_cache, o->cache_sz );
	memcpy( o->saved_y, c->s_reconstructed_frame.pui8_Y, o->plane_y );
	memcpy( o->saved_cb, c->s_reconstructed_frame.pui8_Cb, o->plane_c );
	memcpy( o->saved_cr, c->s_reconstructed_frame.pui8_Cr, o->plane_c );
}

static void oracle_restore( oracle_t *o, const h261_bitstream_t *committed )
{
	h261_context_t *c = &o->ctx;
	c->s_bitstream = *committed;
	c->s_picture.s_groups_of_blocks = o->saved_gob;
	memcpy( c->ps_mb_cache, o->saved_cache, o->cache_sz );
	memcpy( c->s_reconstructed_frame.pui8_Y, o->saved_y, o->plane_y );
	memcpy( c->s_reconstructed_frame.pui8_Cb, o->saved_cb, o->plane_c );
	memcpy( c->s_reconstructed_frame.pui8_Cr, o->saved_cr, o->plane_c );
}

/* enumerate all 31 base quantizers through the legacy core, independently */
static void oracle_trials( oracle_t *o, const gob_geom_t *g, int header_bits,
	Int64 quota, int buffer_bits, trial_t out[ 31 ] )
{
	h261_context_t *c = &o->ctx;
	h261_bitstream_t committed = c->s_bitstream;
	oracle_snapshot( o );

	for( int q = 1; q <= 31; q++ ) {
		h261_bitstream_t sc;
		Int32 off = ( Int32 )( committed.pui8_codeword_ptr - committed.pui8_bitstream );
		oracle_restore( o, &committed );
		sc = committed;
		sc.pui8_bitstream = o->scratch_buf;
		sc.pui8_codeword_ptr = o->scratch_buf + off;
		c->s_bitstream = sc;
		c->s_picture.i_quantiser = q;

		int start = h261_bitstream_bits( &c->s_bitstream );
		h261_encode_gob( c, g->num, g->ox, g->oy );
		int end = h261_bitstream_bits( &c->s_bitstream );

		trial_t *t = &out[ q - 1 ];
		t->gquant = q;
		t->bits = ( Int64 )( end - start ) + header_bits;
		t->sse = region_sse( c, g->ox, g->oy );
		t->fits = ( o->debt + t->bits - quota <= ( Int64 ) buffer_bits );
	}
	oracle_restore( o, &committed );
}

/* independent ordering, returns selected index 0..30 and overflow flag */
static int oracle_select( trial_t t[ 31 ], int *overflow )
{
	int fit = -1, ovf = -1;
	for( int i = 0; i < 31; i++ ) {
		if( t[ i ].fits ) {
			if( fit < 0 || t[ i ].sse < t[ fit ].sse ||
				( t[ i ].sse == t[ fit ].sse && t[ i ].bits < t[ fit ].bits ) ||
				( t[ i ].sse == t[ fit ].sse && t[ i ].bits == t[ fit ].bits && t[ i ].gquant < t[ fit ].gquant ) )
				fit = i;
		} else {
			if( ovf < 0 || t[ i ].bits < t[ ovf ].bits ||
				( t[ i ].bits == t[ ovf ].bits && t[ i ].sse < t[ ovf ].sse ) ||
				( t[ i ].bits == t[ ovf ].bits && t[ i ].sse == t[ ovf ].sse && t[ i ].gquant < t[ ovf ].gquant ) )
				ovf = i;
		}
	}
	if( fit >= 0 ) { *overflow = 0; return fit; }
	*overflow = 1;
	return ovf;
}

/* commit the selected candidate on the oracle through a clean legacy encode */
static void oracle_commit( oracle_t *o, const gob_geom_t *g, int q )
{
	h261_context_t *c = &o->ctx;
	c->s_picture.i_quantiser = q;
	h261_encode_gob( c, g->num, g->ox, g->oy );
}

static void oracle_picture_start( oracle_t *o, int frame, int format )
{
	h261_picture_parameters_t p;
	p.i_frame_num = frame; p.i_source_format = format;
	p.i_frame_type = frame ? H261_FRAME_TYPE_INTER : H261_FRAME_TYPE_INTRA;
	p.i_quantiser = 1;
	h261_init_picture( &o->ctx, &p );
	h261_write_picture_header( &o->ctx );
}


/* ------------------------------------------------------------------ */
/* shared library-context encoder drive                                */
/* ------------------------------------------------------------------ */

static void lib_picture_start( h261_context_t *c, int frame, int format )
{
	h261_picture_parameters_t p;
	p.i_frame_num = frame; p.i_source_format = format;
	p.i_frame_type = frame ? H261_FRAME_TYPE_INTER : H261_FRAME_TYPE_INTRA;
	p.i_quantiser = 10;
	h261_ratectrl_init_picture( c, &p );
	h261_init_picture( c, &p );
	h261_write_picture_header( c );
}

static void make_config( h261_gob_budget_config_t *cfg, int bitrate, int fps_num, int fps_den, int buffer )
{
	cfg->i_bitrate = bitrate;
	cfg->i_fps_num = fps_num;
	cfg->i_fps_den = fps_den;
	cfg->i_buffer_bits = buffer;
}


/* Run library and oracle in lockstep over several frames and reconcile every
 * observable. Drives fit, overflow, within-frame quant changes, real bit
 * counts, SSE, selection ordering, trials and the full budget clock. */
static int run_selection( int format, fill_fn fill, int bitrate, int buffer, int frames,
	observer_bag_t *bag, int require_variation, int require_overflow, int require_fit )
{
	h261_context_t lib;
	oracle_t o;
	TASK_CHECK( fixture_init( &lib, format, H261_RATECTRL_MODE_QUANT ) == 1 );
	TASK_CHECK( oracle_init( &o, format ) == 0 );

	h261_gob_budget_config_t cfg;
	make_config( &cfg, bitrate, 30, 1, buffer );
	cfg.pf_observe = observe_cb;
	cfg.p_opaque = bag;
	TASK_CHECK( h261_ratectrl_init_gob_budget( &lib, &cfg ) == 1 );
	TASK_CHECK( h261_gob_budget_active( &lib ) );

	const gob_geom_t *geom = format == H261_SOURCE_FORMAT_CIF ? cif_geom : qcif_geom;
	int G = gobs_for( format );
	Int64 num = ( Int64 ) bitrate;            /* bitrate * fps_den, den=1 */
	Int64 den = ( Int64 ) 30 * G;

	int saw_variation = 0, saw_overflow = 0, saw_fit = 0;

	for( int f = 0; f < frames; f++ ) {
		fill( &lib, f );
		fill( &o.ctx, f );

		/* capture the writer position before the picture header so the
		 * header bits are billed to the first GOB */
		int lib_frame_start = h261_bitstream_bits( &lib.s_bitstream );
		int orc_frame_start = h261_bitstream_bits( &o.ctx.s_bitstream );
		lib_picture_start( &lib, f, format );
		oracle_picture_start( &o, f, format );

		Int32 prev_q = -1;

		for( int gi = 0; gi < G; gi++ ) {
			const gob_geom_t *g = &geom[ gi ];
			int header_bits = ( gi == 0 ) ? ( h261_bitstream_bits( &lib.s_bitstream ) - lib_frame_start ) : 0;
			int orc_header = ( gi == 0 ) ? ( h261_bitstream_bits( &o.ctx.s_bitstream ) - orc_frame_start ) : 0;
			TASK_CHECK( header_bits == orc_header );

			/* independent clock */
			Int64 quota = ( o.remain + num ) / den;
			Int64 new_remain = o.remain + num - quota * den;
			Int64 target = o.target + quota;

			trial_t trials[ 31 ];
			oracle_trials( &o, g, header_bits, quota, buffer, trials );
			int overflow;
			int sel = oracle_select( trials, &overflow );

			int before_records = bag->count;
			h261_gob_budget_encode_gob( &lib, g->num, g->ox, g->oy );
			TASK_CHECK( bag->count == before_records + 1 );
			h261_gob_budget_stats_t *st = &bag->rec[ bag->count - 1 ].st;

			/* selection */
			TASK_CHECK( st->i_gquant == trials[ sel ].gquant );
			TASK_CHECK( st->i_overflow == overflow );
			TASK_CHECK( st->i_trials == 31 );
			TASK_CHECK( st->i_bits == trials[ sel ].bits );
			TASK_CHECK( st->i_sse == trials[ sel ].sse );
			TASK_CHECK( st->i_gob_number == g->num );
			TASK_CHECK( st->i_frame_num == f );

			if( overflow ) saw_overflow = 1; else saw_fit = 1;
			if( prev_q >= 0 && st->i_gquant != prev_q ) saw_variation = 1;
			prev_q = st->i_gquant;

			/* clock reconciliation against the documented exact formula */
			o.K++;
			Int64 direct = ( o.K * num ) / den;
			TASK_CHECK( target == direct );
			TASK_CHECK( st->i_quota == quota );
			TASK_CHECK( st->i_target_total == target );
			o.remain = new_remain;
			TASK_CHECK( o.remain >= 0 && o.remain < den );
			o.target = target;
			o.coded += trials[ sel ].bits;
			o.debt += trials[ sel ].bits - quota;
			TASK_CHECK( st->i_coded_total == o.coded );
			TASK_CHECK( st->i_debt_after == o.debt );
			TASK_CHECK( o.debt == o.coded - o.target );

			/* advance the oracle with the chosen candidate */
			oracle_commit( &o, g, trials[ sel ].gquant );
		}

		/* stream bookkeeping identical to the normal frame flow */
		UInt8 *lb; UInt32 ln;
		h261_bitstream_get( &lib.s_bitstream, &lb, &ln );
		h261_bitstream_advance( &lib.s_bitstream );
		h261_ratectrl_update_picture( &lib, ln );
		h261_bitstream_advance( &o.ctx.s_bitstream );

		copy_ref_from_recon( &lib );
		copy_ref_from_recon( &o.ctx );

		/* rejected candidates must not leak into the next picture's basis */
		TASK_CHECK( planes_equal( &lib, &o.ctx ) );
	}

	if( require_variation ) TASK_CHECK( saw_variation );
	if( require_overflow ) TASK_CHECK( saw_overflow );
	if( require_fit ) TASK_CHECK( saw_fit );

	oracle_free( &o );
	fixture_free( &lib );
	return 0;
}


/* ------------------------------------------------------------------ */
/* clock group                                                         */
/* ------------------------------------------------------------------ */

static int clock_exact_formula( void )
{
	observer_bag_t bag; memset( &bag, 0, sizeof( bag ) ); bag.tag = 1;
	/* QCIF and CIF, flat and textured content, multiple frames */
	TASK_CHECK( run_selection( H261_SOURCE_FORMAT_QCIF, fill_texture, 400000, 30000, 4, &bag, 0, 0, 1 ) == 0 );
	TASK_CHECK( bag.count == 4 * 3 );
	observer_bag_t bag2; memset( &bag2, 0, sizeof( bag2 ) ); bag2.tag = 2;
	TASK_CHECK( run_selection( H261_SOURCE_FORMAT_CIF, fill_flat, 1000000, 200000, 2, &bag2, 0, 0, 1 ) == 0 );
	TASK_CHECK( bag2.count == 2 * 12 );

	/* cumulative budget must equal floor(K*bitrate*fps_den/(fps_num*G)) */
	for( int i = 0; i < bag.count; i++ ) {
		h261_gob_budget_stats_t *s = &bag.rec[ i ].st;
		Int64 K = i + 1;
		Int64 expect = ( K * ( Int64 ) 400000 ) / ( ( Int64 ) 30 * 3 );
		TASK_CHECK( s->i_target_total == expect );
	}
	return 0;
}

static int clock_long_run( void )
{
	/* pure budget clock, well over one million GOBs, a few rates */
	const int rates[ ] = { 1, 64000, 100000000 };
	for( int ri = 0; ri < 3; ri++ ) {
		Int64 num = ( Int64 ) rates[ ri ];      /* fps 30/1 */
		Int64 den = ( Int64 ) 30 * 3;           /* QCIF */
		Int64 remain = 0, B = 0, coded = 0, debt = 0;
		const Int64 N = 1000003;                /* > 1,000,000 GOBs */
		for( Int64 K = 1; K <= N; K++ ) {
			Int64 quota = ( remain + num ) / den;
			remain = remain + num - quota * den;
			B += quota;
			Int64 bits = ( K * 37 ) % 5000;     /* deterministic pseudo cost */
			coded += bits;
			debt += bits - quota;
			TASK_CHECK( remain >= 0 && remain < den );
			if( K % 99991 == 0 || K == N ) {
				TASK_CHECK( B == ( K * num ) / den );
				TASK_CHECK( debt == coded - B );  /* never reset or clamped */
			}
		}
	}
	return 0;
}

static int clock_interleaved_instances( void )
{
	h261_context_t a, b;
	observer_bag_t ba, bb;
	memset( &ba, 0, sizeof( ba ) ); ba.tag = 7;
	memset( &bb, 0, sizeof( bb ) ); bb.tag = 9;
	TASK_CHECK( fixture_init( &a, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	TASK_CHECK( fixture_init( &b, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	h261_gob_budget_config_t ca, cb;
	make_config( &ca, 300000, 30, 1, 25000 ); ca.pf_observe = observe_cb; ca.p_opaque = &ba;
	make_config( &cb, 120000, 25, 1, 9000 );  cb.pf_observe = observe_cb; cb.p_opaque = &bb;
	TASK_CHECK( h261_ratectrl_init_gob_budget( &a, &ca ) == 1 );
	TASK_CHECK( h261_ratectrl_init_gob_budget( &b, &cb ) == 1 );

	for( int f = 0; f < 4; f++ ) {
		fill_texture( &a, f );
		fill_motion( &b, f );
		lib_picture_start( &a, f, H261_SOURCE_FORMAT_QCIF );
		lib_picture_start( &b, f, H261_SOURCE_FORMAT_QCIF );
		for( int gi = 0; gi < 3; gi++ ) {
			h261_gob_budget_encode_gob( &a, qcif_geom[ gi ].num, qcif_geom[ gi ].ox, qcif_geom[ gi ].oy );
			h261_gob_budget_encode_gob( &b, qcif_geom[ gi ].num, qcif_geom[ gi ].ox, qcif_geom[ gi ].oy );
		}
		UInt8 *bytes; UInt32 n;
		h261_bitstream_get( &a.s_bitstream, &bytes, &n ); h261_bitstream_advance( &a.s_bitstream );
		h261_ratectrl_update_picture( &a, n );
		h261_bitstream_get( &b.s_bitstream, &bytes, &n ); h261_bitstream_advance( &b.s_bitstream );
		h261_ratectrl_update_picture( &b, n );
		copy_ref_from_recon( &a );
		copy_ref_from_recon( &b );
	}

	TASK_CHECK( ba.count == 12 && bb.count == 12 );
	/* independent clocks: each bag keeps its own instance and frame/gob ids */
	for( int i = 0; i < 12; i++ ) {
		TASK_CHECK( ba.rec[ i ].tag == 7 && bb.rec[ i ].tag == 9 );
		TASK_CHECK( ba.rec[ i ].st.i_frame_num == i / 3 );
		TASK_CHECK( bb.rec[ i ].st.i_frame_num == i / 3 );
		TASK_CHECK( ba.rec[ i ].st.i_gob_number == qcif_geom[ i % 3 ].num );
		TASK_CHECK( bb.rec[ i ].st.i_gob_number == qcif_geom[ i % 3 ].num );
	}
	/* the two budgets must diverge (different rate/buffer) so isolation matters */
	TASK_CHECK( ba.rec[ 11 ].st.i_target_total != bb.rec[ 11 ].st.i_target_total );
	fixture_free( &a );
	fixture_free( &b );
	return 0;
}


/* ------------------------------------------------------------------ */
/* selection group                                                     */
/* ------------------------------------------------------------------ */

static int selection_qcif_textured( void )
{
	observer_bag_t bag; memset( &bag, 0, sizeof( bag ) ); bag.tag = 3;
	/* tight-ish textured content: within-frame quant changes, both outcomes */
	TASK_CHECK( run_selection( H261_SOURCE_FORMAT_QCIF, fill_texture, 2000000, 30000, 4,
		&bag, 1, 1, 1 ) == 0 );
	TASK_CHECK( bag.count == 12 );
	return 0;
}

static int selection_qcif_motion_fit( void )
{
	observer_bag_t bag; memset( &bag, 0, sizeof( bag ) ); bag.tag = 4;
	/* generous buffer: moving P frames, all fit, quant still adapts per GOB */
	TASK_CHECK( run_selection( H261_SOURCE_FORMAT_QCIF, fill_motion, 4000000, 4000000, 4,
		&bag, 0, 0, 1 ) == 0 );
	for( int i = 0; i < bag.count; i++ ) TASK_CHECK( bag.rec[ i ].st.i_overflow == 0 );
	return 0;
}

static int selection_cif_all_overflow( void )
{
	observer_bag_t bag; memset( &bag, 0, sizeof( bag ) ); bag.tag = 5;
	/* tiny buffer on CIF texture: every GOB overflows but stays legal */
	TASK_CHECK( run_selection( H261_SOURCE_FORMAT_CIF, fill_texture, 50000, 1, 2,
		&bag, 0, 1, 0 ) == 0 );
	TASK_CHECK( bag.count == 24 );
	for( int i = 0; i < bag.count; i++ ) {
		TASK_CHECK( bag.rec[ i ].st.i_overflow == 1 );
		TASK_CHECK( bag.rec[ i ].st.i_trials == 31 );
	}
	return 0;
}

static int selection_invalid_and_disable( void )
{
	h261_context_t c;
	TASK_CHECK( fixture_init( &c, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	h261_gob_budget_config_t cfg;
	make_config( &cfg, 64000, 30, 1, 20000 ); cfg.pf_observe = NULL; cfg.p_opaque = NULL;

	/* NULL and every out-of-range value are rejected and leave state untouched */
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, NULL ) == 0 );
	TASK_CHECK( h261_gob_budget_active( &c ) == 0 );

	h261_gob_budget_config_t bad;
	bad = cfg; bad.i_bitrate = 0; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = cfg; bad.i_bitrate = 100000001; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = cfg; bad.i_buffer_bits = 0; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = cfg; bad.i_buffer_bits = 1000000001; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = cfg; bad.i_fps_num = 0; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = cfg; bad.i_fps_den = 0; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = cfg; bad.i_fps_num = 1000001; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = cfg; bad.i_fps_num = 1; bad.i_fps_den = 2;   /* ratio 0.5 < 1 */
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	bad = cfg; bad.i_fps_num = 121; bad.i_fps_den = 1; /* ratio 121 > 120 */
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 0 );
	TASK_CHECK( h261_gob_budget_active( &c ) == 0 );

	/* exact boundary values are accepted */
	bad = cfg; bad.i_bitrate = 1; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 1 );
	bad = cfg; bad.i_bitrate = 100000000; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 1 );
	bad = cfg; bad.i_buffer_bits = 1; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 1 );
	bad = cfg; bad.i_buffer_bits = 1000000000; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 1 );
	bad = cfg; bad.i_fps_num = 1; bad.i_fps_den = 1; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 1 );
	bad = cfg; bad.i_fps_num = 120; bad.i_fps_den = 1; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 1 );
	bad = cfg; bad.i_fps_num = 1000000; bad.i_fps_den = 1000000; TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &bad ) == 1 );

	/* a valid enable works, then the legacy entry point disables the mode */
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &cfg ) == 1 );
	TASK_CHECK( h261_gob_budget_active( &c ) != 0 );
	h261_init_ratectrl( &c, 10, 64000, 64000, H261_RATECTRL_MODE_QUANT );
	TASK_CHECK( h261_gob_budget_active( &c ) == 0 );
	TASK_CHECK( c.s_ratectrl.i_ratectrl_mode == H261_RATECTRL_MODE_QUANT );
	fixture_free( &c );
	return 0;
}


/* ------------------------------------------------------------------ */
/* stream group                                                        */
/* ------------------------------------------------------------------ */

static int stream_parse_headers( void )
{
	h261_context_t c;
	observer_bag_t bag; memset( &bag, 0, sizeof( bag ) ); bag.tag = 6;
	TASK_CHECK( fixture_init( &c, H261_SOURCE_FORMAT_QCIF, H261_RATECTRL_MODE_QUANT ) );
	h261_gob_budget_config_t cfg;
	make_config( &cfg, 2000000, 30, 1, 30000 ); cfg.pf_observe = observe_cb; cfg.p_opaque = &bag;
	TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &cfg ) == 1 );

	FILE *out = tmpfile(); TASK_CHECK( out != NULL );
	int frames = 4;
	for( int f = 0; f < frames; f++ ) {
		fill_texture( &c, f );
		lib_picture_start( &c, f, H261_SOURCE_FORMAT_QCIF );
		h261_encode_gobs( &c );
		UInt8 *bytes; UInt32 n;
		h261_bitstream_get( &c.s_bitstream, &bytes, &n );
		fwrite( bytes, 1, n, out );
		h261_bitstream_advance( &c.s_bitstream );
		h261_ratectrl_update_picture( &c, n );
		copy_ref_from_recon( &c );
	}
	UInt8 *tail; UInt32 tlen;
	h261_bitstream_flush( &c.s_bitstream, &tail, &tlen );
	fwrite( tail, 1, tlen, out );
	fflush( out );

	long sz = ftell( out ); rewind( out );
	UInt8 *buf = ( UInt8 * ) malloc( ( size_t ) sz );
	TASK_CHECK( fread( buf, 1, ( size_t ) sz, out ) == ( size_t ) sz );

	parsed_gob_t parsed[ 64 ]; int npic, ngob;
	parse_stream( buf, sz, parsed, 64, &npic, &ngob );
	TASK_CHECK( npic == frames );
	TASK_CHECK( ngob == frames * 3 );
	TASK_CHECK( bag.count == ngob );
	for( int i = 0; i < ngob; i++ ) {
		TASK_CHECK( parsed[ i ].gob_num == bag.rec[ i ].st.i_gob_number );
		TASK_CHECK( parsed[ i ].gquant == bag.rec[ i ].st.i_gquant );
		TASK_CHECK( parsed[ i ].gquant >= 1 && parsed[ i ].gquant <= 31 );
	}
	free( buf ); fclose( out ); fixture_free( &c );
	return 0;
}

static unsigned long long legacy_hash( int mode, int format )
{
	h261_context_t c;
	TASK_CHECK( fixture_init( &c, format, mode ) );
	FILE *out = tmpfile();
	unsigned long long h = 1469598103934665603ULL;
	for( int f = 0; f < 3; f++ ) {
		fill_flat( &c, f );
		fixture_encode( &c, f, out );
	}
	UInt8 *tail; UInt32 len;
	h261_bitstream_flush( &c.s_bitstream, &tail, &len );
	fwrite( tail, 1, len, out );
	fflush( out ); rewind( out );
	int ch;
	while( ( ch = fgetc( out ) ) != EOF ) {
		unsigned char b = ( unsigned char ) ch;
		h = fnv( &b, 1 );
	}
	fclose( out ); fixture_free( &c );
	return h;
}

static int stream_legacy_byte_compat( void )
{
	/* QUANT and VBR output must be identical whether or not budget mode was
	 * ever configured and then switched off again */
	for( int mode = H261_RATECTRL_MODE_QUANT; mode <= H261_RATECTRL_MODE_VBR; mode++ ) {
		unsigned long long h_plain = legacy_hash( mode, H261_SOURCE_FORMAT_QCIF );

		h261_context_t c;
		TASK_CHECK( fixture_init( &c, H261_SOURCE_FORMAT_QCIF, mode ) );
		h261_gob_budget_config_t cfg;
		make_config( &cfg, 64000, 30, 1, 20000 ); cfg.pf_observe = NULL; cfg.p_opaque = NULL;
		TASK_CHECK( h261_ratectrl_init_gob_budget( &c, &cfg ) == 1 );
		h261_init_ratectrl( &c, 10, 64000, 64000, mode );
		TASK_CHECK( h261_gob_budget_active( &c ) == 0 );
		FILE *out = tmpfile();
		unsigned long long h = 1469598103934665603ULL;
		for( int f = 0; f < 3; f++ ) {
			fill_flat( &c, f );
			fixture_encode( &c, f, out );
		}
		UInt8 *tail; UInt32 len;
		h261_bitstream_flush( &c.s_bitstream, &tail, &len );
		fwrite( tail, 1, len, out );
		fflush( out ); rewind( out );
		int ch;
		while( ( ch = fgetc( out ) ) != EOF ) { unsigned char b = ( unsigned char ) ch; h = fnv( &b, 1 ); }
		fclose( out ); fixture_free( &c );
		TASK_CHECK( h == h_plain );
	}
	return 0;
}

static int stream_pframe_no_pollution( void )
{
	/* library recon after each P frame must equal the clean oracle recon,
	 * proving rejected candidates never pollute motion cache or planes.
	 * Fit/overflow mixing on textured content is covered separately; here the
	 * moving inter frames must all stay legal and vary their per-GOB quant. */
	observer_bag_t bag; memset( &bag, 0, sizeof( bag ) ); bag.tag = 8;
	TASK_CHECK( run_selection( H261_SOURCE_FORMAT_QCIF, fill_motion, 2000000, 4000000, 5,
		&bag, 0, 0, 1 ) == 0 );
	/* at least one P frame was covered and every record used real trials */
	TASK_CHECK( bag.count == 15 );
	for( int i = 3; i < bag.count; i++ ) TASK_CHECK( bag.rec[ i ].st.i_frame_num >= 1 );
	return 0;
}


/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[ ] = {
	{ "clock", "clock_exact_formula", clock_exact_formula },
	{ "clock", "clock_long_run", clock_long_run },
	{ "clock", "clock_interleaved_instances", clock_interleaved_instances },

	{ "selection", "selection_qcif_textured", selection_qcif_textured },
	{ "selection", "selection_qcif_motion_fit", selection_qcif_motion_fit },
	{ "selection", "selection_cif_all_overflow", selection_cif_all_overflow },
	{ "selection", "selection_invalid_and_disable", selection_invalid_and_disable },

	{ "stream", "stream_parse_headers", stream_parse_headers },
	{ "stream", "stream_legacy_byte_compat", stream_legacy_byte_compat },
	{ "stream", "stream_pframe_no_pollution", stream_pframe_no_pollution },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[ 0 ] );
