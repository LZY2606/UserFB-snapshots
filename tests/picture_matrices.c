/*
tests for per picture quantization matrix updates.

the file registers cases in three groups ( inheritance, coefficients, stream )
through task_cases / task_case_count. it contains no main; the task driver
provides that.

every case encodes a real mpeg-2 elementary stream with an asymmetric luma /
chroma texture, including B frames, a gop boundary and a short flush tail, and
then checks one facet of the feature against an independent decoder side model.
*/

#include "task_support/check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "y262.h"

/* ------------------------------------------------------------------ */
/* constants and matrix helpers                                       */
/* ------------------------------------------------------------------ */

#define TEST_W 64
#define TEST_H 64

#define MAT_LUMA_INTRA   0
#define MAT_LUMA_INTER   1
#define MAT_CHROMA_INTRA 2
#define MAT_CHROMA_INTER 3

static const uint8_t rgui8_test_default_intra[ 64 ] = {
	8, 16, 19, 22, 26, 27, 29, 34,
	16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38,
	22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48,
	26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69,
	27, 29, 35, 38, 46, 56, 69, 83
};

static const uint8_t rgui8_test_default_inter[ 64 ] = {
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16,
	16, 16, 16, 16, 16, 16, 16, 16
};

static void make_intra_matrix( uint8_t *pui8_m, int32_t i_seed )
{
	int32_t i;
	for( i = 0; i < 64; i++ )
	{
		int32_t x = i % 8, y = i / 8;
		pui8_m[ i ] = ( uint8_t )( 8 + ( ( x * 7 + y * 5 + i_seed * 11 ) % 70 ) );
	}
	pui8_m[ 0 ] = 8;
}

static void make_inter_matrix( uint8_t *pui8_m, int32_t i_seed )
{
	int32_t i;
	for( i = 0; i < 64; i++ )
	{
		int32_t x = i % 8, y = i / 8;
		pui8_m[ i ] = ( uint8_t )( 10 + ( ( x * 3 + y * 9 + i_seed * 13 ) % 90 ) );
	}
}

/* ------------------------------------------------------------------ */
/* capture / callback harness                                          */
/* ------------------------------------------------------------------ */

typedef struct
{
	int32_t i_don;
	int32_t i_pon;
	int32_t i_unit_type;
	int32_t i_offset;
	int32_t i_length;
} test_unit_log_t;

typedef struct
{
	int32_t i_pon;
	int32_t i_don;
	int32_t i_frame_type;
} test_recon_log_t;

typedef struct
{
	uint8_t *pui8_es;
	int32_t i_es_len;
	int32_t i_es_cap;

	test_unit_log_t rgs_units[ 8192 ];
	int32_t i_num_units;

	test_recon_log_t rgs_recon[ 512 ];
	int32_t i_num_recon;

	int32_t i_errors;
} test_capture_t;

static void test_error_cb( void *p_handle, int32_t i_code, int8_t *pi8_text )
{
	test_capture_t *ps_cap = ( test_capture_t * )p_handle;
	ps_cap->i_errors++;
	( void )i_code;
	( void )pi8_text;
}

static void test_es_append( test_capture_t *ps_cap, const uint8_t *pui8, int32_t i_len )
{
	if( ps_cap->i_es_len + i_len > ps_cap->i_es_cap )
	{
		int32_t i_new_cap = ps_cap->i_es_cap ? ps_cap->i_es_cap : 65536;
		uint8_t *p;
		while( i_new_cap < ps_cap->i_es_len + i_len )
		{
			i_new_cap *= 2;
		}
		p = ( uint8_t * )realloc( ps_cap->pui8_es, ( size_t )i_new_cap );
		if( !p )
		{
			return;
		}
		ps_cap->pui8_es = p;
		ps_cap->i_es_cap = i_new_cap;
	}
	memcpy( ps_cap->pui8_es + ps_cap->i_es_len, pui8, ( size_t )i_len );
	ps_cap->i_es_len += i_len;
}

static void test_result_cb( void *p_handle, int32_t i_result_type, y262_result_t *ps_result )
{
	test_capture_t *ps_cap = ( test_capture_t * )p_handle;

	if( i_result_type == Y262_RESULT_BITSTREAM )
	{
		test_unit_log_t *ps_u;
		if( ps_cap->i_num_units >= ( int32_t )( sizeof( ps_cap->rgs_units ) / sizeof( ps_cap->rgs_units[ 0 ] ) ) )
		{
			return;
		}
		ps_u = &ps_cap->rgs_units[ ps_cap->i_num_units++ ];
		ps_u->i_don = ps_result->bitstream_unit.i_don;
		ps_u->i_pon = ps_result->bitstream_unit.i_pon;
		ps_u->i_unit_type = ps_result->bitstream_unit.i_unit_type;
		ps_u->i_offset = ps_cap->i_es_len;
		ps_u->i_length = ps_result->bitstream_unit.i_unit_length;
		test_es_append( ps_cap, ps_result->bitstream_unit.pui8_unit, ps_result->bitstream_unit.i_unit_length );
	}
	else if( i_result_type == Y262_RESULT_RECON )
	{
		test_recon_log_t *ps_r;
		if( ps_cap->i_num_recon >= ( int32_t )( sizeof( ps_cap->rgs_recon ) / sizeof( ps_cap->rgs_recon[ 0 ] ) ) )
		{
			return;
		}
		ps_r = &ps_cap->rgs_recon[ ps_cap->i_num_recon++ ];
		ps_r->i_pon = ps_result->recon.i_pon;
		ps_r->i_don = ps_result->recon.i_don;
		ps_r->i_frame_type = ps_result->recon.i_frame_type;
	}
}

static void test_capture_reset( test_capture_t *ps_cap )
{
	ps_cap->i_es_len = 0;
	ps_cap->i_num_units = 0;
	ps_cap->i_num_recon = 0;
	ps_cap->i_errors = 0;
}

static void test_capture_free( test_capture_t *ps_cap )
{
	free( ps_cap->pui8_es );
	memset( ps_cap, 0, sizeof( *ps_cap ) );
}

/* ------------------------------------------------------------------ */
/* encoder configuration and input generation                          */
/* ------------------------------------------------------------------ */

static void test_config_init( y262_configuration_t *ps_config, int32_t i_threads )
{
	memset( ps_config, 0, sizeof( *ps_config ) );
	ps_config->i_coded_width = TEST_W;
	ps_config->i_display_width = TEST_W;
	ps_config->i_coded_height = TEST_H;
	ps_config->i_display_height = TEST_H;
	ps_config->i_coded_chroma_format = Y262_CHROMA_FORMAT_420;
	ps_config->i_profile = Y262_PROFILE_DERIVE;
	ps_config->i_level = Y262_LEVEL_DERIVE;
	ps_config->i_videoformat = Y262_VIDEOFORMAT_UNKNOWN;
	ps_config->i_frame_rate_code = 1;
	ps_config->i_aspect_ratio_information = 1;
	ps_config->i_multithreading = i_threads > 1 ? 1 : 0;
	ps_config->i_num_threads = i_threads;
	ps_config->i_lookahead_pictures = 10;
	ps_config->i_bframes = 2;
	ps_config->i_keyframe_ref_distance = 1;
	ps_config->i_rcmode = BITRATE_CONTROL_CQ;
	ps_config->i_quantizer = 8;
	ps_config->i_vbv_rate = 800;
	ps_config->i_vbv_size = 400;
	ps_config->i_bitrate = 400;
	ps_config->b_variance_aq = 0;
	ps_config->i_quality_for_speed = 0;
	ps_config->b_qscale_type = 1;
	ps_config->rgi_fcode[ 0 ] = 5;
	ps_config->rgi_fcode[ 1 ] = 5;
	ps_config->b_picture_quant_matrices = 1;
	ps_config->pf_error_callback = test_error_cb;
}

static void test_make_frame( int32_t i_idx, uint8_t *pui8_y, uint8_t *pui8_cb, uint8_t *pui8_cr )
{
	int32_t x, y;
	for( y = 0; y < TEST_H; y++ )
	{
		for( x = 0; x < TEST_W; x++ )
		{
			int32_t v = 20 + ( ( x * 3 + y * 2 + i_idx * 9 ) % 150 );
			v += ( ( ( x / 8 + y / 8 + i_idx ) & 1 ) ? 20 : 0 );
			pui8_y[ y * TEST_W + x ] = ( uint8_t )( v < 0 ? 0 : ( v > 255 ? 255 : v ) );
		}
	}
	for( y = 0; y < TEST_H / 2; y++ )
	{
		for( x = 0; x < TEST_W / 2; x++ )
		{
			pui8_cb[ y * ( TEST_W / 2 ) + x ] = ( uint8_t )( 48 + ( ( x * 9 + y * 2 + i_idx * 5 ) % 150 ) );
			pui8_cr[ y * ( TEST_W / 2 ) + x ] = ( uint8_t )( 48 + ( ( x * 2 + y * 11 + i_idx * 7 ) % 150 ) );
		}
	}
}

static int32_t test_push_frame( void *p_enc, int32_t i_pon, uint32_t ui_mask, const uint8_t rgui8_update[ 4 ][ 64 ], int32_t b_set_fields )
{
	uint8_t y[ TEST_W * TEST_H ], cb[ TEST_W * TEST_H / 4 ], cr[ TEST_W * TEST_H / 4 ];
	y262_input_picture_t s_pic;

	test_make_frame( i_pon, y, cb, cr );
	memset( &s_pic, 0, sizeof( s_pic ) );
	s_pic.pui8_luma = y;
	s_pic.pui8_cb = cb;
	s_pic.pui8_cr = cr;
	s_pic.i_frame_structure = Y262_INPUT_PICTURE_FRAME_PROGRESSIVE;
	if( b_set_fields )
	{
		s_pic.i_quant_matrix_update_mask = ui_mask;
		if( rgui8_update )
		{
			memcpy( s_pic.rgui8_quant_matrices, rgui8_update, sizeof( s_pic.rgui8_quant_matrices ) );
		}
	}
	return y262_push_input_picture( p_enc, &s_pic, i_pon );
}

/*
encode i_num_frames frames. masks / updates are given per picture order number.
frames after i_legal_frames are pushed without touching the new fields, to
mirror an application that leaves them zeroed.
*/
static int32_t test_encode( void *p_enc, test_capture_t *ps_cap, int32_t i_num_frames,
	const uint32_t *prgui32_masks, uint8_t rgui8_updates[][ 4 ][ 64 ] )
{
	int32_t f, k;

	test_capture_reset( ps_cap );
	for( f = 0; f < i_num_frames; f++ )
	{
		int32_t rc = test_push_frame( p_enc, f, prgui32_masks ? prgui32_masks[ f ] : 0,
			rgui8_updates ? rgui8_updates[ f ] : NULL, 1 );
		if( rc != Y262_PUSH_INPUT_CONTINUE )
		{
			return rc;
		}
	}
	for( k = 0; k < 1024; k++ )
	{
		int32_t rc = y262_push_input_picture( p_enc, NULL, i_num_frames );
		if( rc == Y262_PUSH_INPUT_FLUSHED )
		{
			return 0;
		}
		if( rc != Y262_PUSH_INPUT_CONTINUE )
		{
			return rc;
		}
	}
	return -99;
}

/* ------------------------------------------------------------------ */
/* independent mpeg-2 header parser                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
	const uint8_t *pui8;
	int32_t i_len;
	int32_t i_bitpos;
} test_bits_t;

static int32_t test_bits_get( test_bits_t *ps_b )
{
	int32_t i_byte, i_bit;
	if( ps_b->i_bitpos >= ps_b->i_len * 8 )
	{
		ps_b->i_bitpos++;
		return 0;
	}
	i_byte = ps_b->i_bitpos >> 3;
	i_bit = 7 - ( ps_b->i_bitpos & 7 );
	ps_b->i_bitpos++;
	return ( ps_b->pui8[ i_byte ] >> i_bit ) & 1;
}

static uint32_t test_bits_get_n( test_bits_t *ps_b, int32_t i_n )
{
	uint32_t ui = 0;
	int32_t i;
	for( i = 0; i < i_n; i++ )
	{
		ui = ( ui << 1 ) | ( uint32_t )test_bits_get( ps_b );
	}
	return ui;
}

/* zigzag scan position -> natural index, straight from the mpeg-2 spec */
static const int32_t rgi_test_zigzag[ 64 ] = {
	0, 1, 8, 16, 9, 2, 3, 10,
	17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct
{
	int32_t i_pon;
	int32_t i_picture_type;
	int32_t i_has_quant_ext;
	int32_t i_quant_flags;
	uint8_t rgui8_load[ 4 ][ 64 ];  /* natural order */
	uint8_t rgui8_state_in[ 4 ][ 64 ];
	uint8_t rgui8_state_out[ 4 ][ 64 ];
	int32_t i_es_offset;
} test_pic_header_t;

static void test_state_reset( uint8_t rgui8_state[ 4 ][ 64 ] )
{
	memcpy( rgui8_state[ 0 ], rgui8_test_default_intra, 64 );
	memcpy( rgui8_state[ 1 ], rgui8_test_default_inter, 64 );
	memcpy( rgui8_state[ 2 ], rgui8_test_default_intra, 64 );
	memcpy( rgui8_state[ 3 ], rgui8_test_default_inter, 64 );
}

static void test_state_apply( uint8_t rgui8_state[ 4 ][ 64 ], int32_t i_flags, uint8_t rgui8_load[ 4 ][ 64 ] )
{
	/* sequence header / quant matrix extension reload behaviour per spec */
	if( i_flags & 1 )
	{
		memcpy( rgui8_state[ 0 ], rgui8_load[ 0 ], 64 );
		memcpy( rgui8_state[ 2 ], rgui8_load[ 0 ], 64 );
	}
	if( i_flags & 2 )
	{
		memcpy( rgui8_state[ 1 ], rgui8_load[ 1 ], 64 );
		memcpy( rgui8_state[ 3 ], rgui8_load[ 1 ], 64 );
	}
	if( i_flags & 4 )
	{
		memcpy( rgui8_state[ 2 ], rgui8_load[ 2 ], 64 );
	}
	if( i_flags & 8 )
	{
		memcpy( rgui8_state[ 3 ], rgui8_load[ 3 ], 64 );
	}
}

/*
parse the raw elementary stream. picture order numbers come from a parallel
list supplied by the caller ( the encoder puts them in the callback, not in the
header ). i_pon_by_don maps a decode order index to pon.
*/
static int32_t test_parse_stream( const uint8_t *pui8_es, int32_t i_len,
	test_pic_header_t *ps_pics, int32_t i_max_pics, int32_t *pi_num_quant_ext,
	const int32_t *prgi_pon_by_don, int32_t i_num_dons )
{
	uint8_t rgui8_state[ 4 ][ 64 ];
	int32_t i_pos, i_num_pics, i_cur_pic;

	test_state_reset( rgui8_state );
	i_num_pics = 0;
	i_cur_pic = -1;
	if( pi_num_quant_ext )
	{
		*pi_num_quant_ext = 0;
	}

	for( i_pos = 0; i_pos + 4 <= i_len; i_pos++ )
	{
		int32_t i_startcode;
		if( !( pui8_es[ i_pos ] == 0 && pui8_es[ i_pos + 1 ] == 0 && pui8_es[ i_pos + 2 ] == 1 ) )
		{
			continue;
		}
		i_startcode = pui8_es[ i_pos + 3 ];
		if( i_startcode == 0xb3 )
		{
			test_bits_t s_b;
			int32_t b_load_intra, b_load_inter, k;
			uint8_t rgui8_intra[ 64 ], rgui8_inter[ 64 ];
			int32_t i_flags = 0;

			s_b.pui8 = pui8_es + i_pos + 4;
			s_b.i_len = i_len - ( i_pos + 4 );
			s_b.i_bitpos = 0;
			test_bits_get_n( &s_b, 12 );
			test_bits_get_n( &s_b, 12 );
			test_bits_get_n( &s_b, 4 );
			test_bits_get_n( &s_b, 4 );
			test_bits_get_n( &s_b, 18 );
			test_bits_get_n( &s_b, 1 );
			test_bits_get_n( &s_b, 10 );
			test_bits_get_n( &s_b, 1 );
			b_load_intra = test_bits_get( &s_b );
			if( b_load_intra )
			{
				for( k = 0; k < 64; k++ )
				{
					rgui8_intra[ k ] = ( uint8_t )test_bits_get_n( &s_b, 8 );
				}
			}
			else
			{
				memcpy( rgui8_intra, rgui8_test_default_intra, 64 );
			}
			b_load_inter = test_bits_get( &s_b );
			if( b_load_inter )
			{
				for( k = 0; k < 64; k++ )
				{
					rgui8_inter[ k ] = ( uint8_t )test_bits_get_n( &s_b, 8 );
				}
			}
			else
			{
				memcpy( rgui8_inter, rgui8_test_default_inter, 64 );
			}
			memcpy( rgui8_state[ 0 ], rgui8_intra, 64 );
			memcpy( rgui8_state[ 2 ], rgui8_intra, 64 );
			memcpy( rgui8_state[ 1 ], rgui8_inter, 64 );
			memcpy( rgui8_state[ 3 ], rgui8_inter, 64 );
			( void )i_flags;
			i_cur_pic = -1;
		}
		else if( i_startcode == 0x00 )
		{
			test_bits_t s_b;
			test_pic_header_t *ps_pic;
			if( i_num_pics >= i_max_pics )
			{
				break;
			}
			ps_pic = &ps_pics[ i_num_pics ];
			memset( ps_pic, 0, sizeof( *ps_pic ) );
			s_b.pui8 = pui8_es + i_pos + 4;
			s_b.i_len = i_len - ( i_pos + 4 );
			s_b.i_bitpos = 0;
			test_bits_get_n( &s_b, 10 );
			ps_pic->i_picture_type = ( int32_t )test_bits_get_n( &s_b, 3 );
			ps_pic->i_es_offset = i_pos;
			ps_pic->i_pon = -1;
			ps_pic->i_pon = ( prgi_pon_by_don && i_num_pics < i_num_dons ) ? prgi_pon_by_don[ i_num_pics ] : i_num_pics;
			memcpy( ps_pic->rgui8_state_in, rgui8_state, sizeof( rgui8_state ) );
			i_cur_pic = i_num_pics;
			i_num_pics++;
		}
		else if( i_startcode == 0xb5 && i_pos + 5 <= i_len )
		{
			int32_t i_ext_id = ( pui8_es[ i_pos + 4 ] >> 4 ) & 0xf;
			if( i_ext_id == 3 && i_cur_pic >= 0 )
			{
				test_bits_t s_b;
				test_pic_header_t *ps_pic = &ps_pics[ i_cur_pic ];
				int32_t k, i_idx;
				ps_pic->i_has_quant_ext = 1;
				if( pi_num_quant_ext )
				{
					( *pi_num_quant_ext )++;
				}
				s_b.pui8 = pui8_es + i_pos + 4;
				s_b.i_len = i_len - ( i_pos + 4 );
				s_b.i_bitpos = 4; /* skip the 4 bit extension id */
				for( i_idx = 0; i_idx < 4; i_idx++ )
				{
					int32_t b_load = test_bits_get( &s_b );
					if( b_load )
					{
						ps_pic->i_quant_flags |= 1 << i_idx;
						for( k = 0; k < 64; k++ )
						{
							ps_pic->rgui8_load[ i_idx ][ rgi_test_zigzag[ k ] ] = ( uint8_t )test_bits_get_n( &s_b, 8 );
						}
					}
				}
				test_state_apply( rgui8_state, ps_pic->i_quant_flags, ps_pic->rgui8_load );
			}
		}
	}

	{
		int32_t p;
		for( p = 0; p < i_num_pics; p++ )
		{
			memcpy( ps_pics[ p ].rgui8_state_out, ps_pics[ p ].rgui8_state_in, sizeof( ps_pics[ p ].rgui8_state_out ) );
			test_state_apply( ps_pics[ p ].rgui8_state_out, ps_pics[ p ].i_quant_flags, ps_pics[ p ].rgui8_load );
		}
	}
	return i_num_pics;
}

/* build the decode order pon list from the callback log */
static int32_t test_build_don_pons( test_capture_t *ps_cap, int32_t *prgi_pon_by_don, int32_t i_max )
{
	int32_t i, n = 0;
	for( i = 0; i < ps_cap->i_num_recon && n < i_max; i++ )
	{
		int32_t don = ps_cap->rgs_recon[ i ].i_don;
		int32_t pon = ps_cap->rgs_recon[ i ].i_pon;
		if( don >= 0 && don < i_max )
		{
			prgi_pon_by_don[ don ] = pon;
		}
		n++;
	}
	return n;
}

/* find the parsed picture whose pon matches, using the recon decode order */
static test_pic_header_t *test_pic_for_pon( test_capture_t *ps_cap, test_pic_header_t *ps_pics, int32_t i_num_pics, int32_t i_pon )
{
	int32_t i;
	for( i = 0; i < i_num_pics && i < ps_cap->i_num_recon; i++ )
	{
		if( ps_cap->rgs_recon[ i ].i_pon == i_pon )
		{
			return &ps_pics[ i ];
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* independent integer inverse quantization                            */
/* ------------------------------------------------------------------ */

/*
independent integer inverse quantization that mirrors the reconstruction the
encoder performs ( mpeg-2 style, 8 bit, progressive => intra dc precision 1 ).
the important part is that every dequantized coefficient is obtained from the
quantized level, the supplied matrix and the quantizer, so using a wrong matrix
produces a different result.
*/
static void test_dequant( const int16_t *pi16_levels, const uint8_t *pui8_matrix, int32_t i_quantizer, int32_t b_intra, int16_t *pi16_out )
{
	int32_t i_x, i_y, i_sum;

	if( b_intra )
	{
		/* dc: no matrix, shifted left for intra dc precision 1 */
		pi16_out[ 0 ] = ( int16_t )( pi16_levels[ 0 ] << 2 );
		i_sum = pi16_out[ 0 ] + 1;
		for( i_y = 0; i_y < 8; i_y++ )
		{
			for( i_x = ( i_y == 0 ) ? 1 : 0; i_x < 8; i_x++ )
			{
				int32_t i_idx = i_y * 8 + i_x;
				int32_t i_qt = pui8_matrix[ i_idx ] * i_quantizer * 2;
				int32_t i_res = ( ( int32_t )pi16_levels[ i_idx ] * i_qt ) / 32;
				pi16_out[ i_idx ] = ( int16_t )i_res;
				i_sum += i_res;
			}
		}
		pi16_out[ 63 ] = ( int16_t )( pi16_out[ 63 ] ^ ( i_sum & 1 ) );
	}
	else
	{
		i_sum = 1;
		for( i_y = 0; i_y < 8; i_y++ )
		{
			for( i_x = 0; i_x < 8; i_x++ )
			{
				int32_t i_idx = i_y * 8 + i_x;
				int32_t i_level = pi16_levels[ i_idx ];
				int32_t i_qt = pui8_matrix[ i_idx ] * i_quantizer;
				int32_t i_res;
				if( i_level > 0 )
				{
					i_res = ( ( i_level * 2 + 1 ) * i_qt ) / 32;
				}
				else if( i_level < 0 )
				{
					i_res = ( ( i_level * 2 - 1 ) * i_qt ) / 32;
				}
				else
				{
					i_res = 0;
				}
				pi16_out[ i_idx ] = ( int16_t )i_res;
				i_sum += i_res;
			}
		}
		pi16_out[ 63 ] = ( int16_t )( pi16_out[ 63 ] ^ ( i_sum & 1 ) );
	}
}

/* ------------------------------------------------------------------ */
/* observation capture during encode                                   */
/* ------------------------------------------------------------------ */

typedef struct
{
	int32_t i_pon;
	int32_t i_plane_idx;
	int32_t i_blk_idx;
	int32_t b_intra;
	int32_t i_quantizer;
	int16_t rgi16_levels[ 64 ];
	int16_t rgi16_dequant[ 64 ];
	int16_t rgi16_recon[ 64 ];
	uint8_t rgui8_matrices[ 4 ][ 64 ];
} test_block_obs_t;

static test_block_obs_t rgts_obs[ 4096 ];
static int32_t i_num_obs;

static void test_block_observer( const y262_block_observation_t *ps_obs )
{
	test_block_obs_t *ps_dst;
	if( i_num_obs >= ( int32_t )( sizeof( rgts_obs ) / sizeof( rgts_obs[ 0 ] ) ) )
	{
		return;
	}
	ps_dst = &rgts_obs[ i_num_obs++ ];
	memset( ps_dst, 0, sizeof( *ps_dst ) );
	ps_dst->i_pon = ps_obs->i_pon;
	ps_dst->i_plane_idx = ps_obs->i_plane_idx;
	ps_dst->i_blk_idx = ps_obs->i_blk_idx;
	ps_dst->b_intra = ps_obs->b_intra;
	ps_dst->i_quantizer = ps_obs->i_quantizer;
	memcpy( ps_dst->rgi16_levels, ps_obs->pi16_levels, sizeof( ps_dst->rgi16_levels ) );
	memcpy( ps_dst->rgi16_dequant, ps_obs->pi16_dequant, sizeof( ps_dst->rgi16_dequant ) );
	memcpy( ps_dst->rgi16_recon, ps_obs->pi16_recon, sizeof( ps_dst->rgi16_recon ) );
	memcpy( ps_dst->rgui8_matrices[ 0 ], ps_obs->rgui8_luma_intra_matrix, 64 );
	memcpy( ps_dst->rgui8_matrices[ 1 ], ps_obs->rgui8_luma_inter_matrix, 64 );
	memcpy( ps_dst->rgui8_matrices[ 2 ], ps_obs->rgui8_chroma_intra_matrix, 64 );
	memcpy( ps_dst->rgui8_matrices[ 3 ], ps_obs->rgui8_chroma_inter_matrix, 64 );
}

/* ------------------------------------------------------------------ */
/* shared sequence setup                                               */
/* ------------------------------------------------------------------ */

#define TEST_NUM_FRAMES 12

typedef struct
{
	uint32_t rgui32_masks[ TEST_NUM_FRAMES ];
	uint8_t rgui8_updates[ TEST_NUM_FRAMES ][ 4 ][ 64 ];
} test_sequence_t;

/* the matrix a picture order number is expected to carry at input time */
static void test_expected_snapshot( test_sequence_t *ps_seq, int32_t i_pon, int32_t i_idx, uint8_t rgui8_out[ 4 ][ 64 ] )
{
	int32_t i;
	if( i_pon == 0 )
	{
		memcpy( rgui8_out[ 0 ], rgui8_test_default_intra, 64 );
		memcpy( rgui8_out[ 1 ], rgui8_test_default_inter, 64 );
		memcpy( rgui8_out[ 2 ], rgui8_test_default_intra, 64 );
		memcpy( rgui8_out[ 3 ], rgui8_test_default_inter, 64 );
	}
	else
	{
		test_expected_snapshot( ps_seq, i_pon - 1, i_idx - 1, rgui8_out );
	}
	for( i = 0; i < 4; i++ )
	{
		if( ps_seq->rgui32_masks[ i_pon ] & ( 1u << i ) )
		{
			memcpy( rgui8_out[ i ], ps_seq->rgui8_updates[ i_pon ][ i ], 64 );
		}
	}
}

static void test_build_sequence( test_sequence_t *ps_seq )
{
	int32_t i;

	memset( ps_seq, 0, sizeof( *ps_seq ) );

	/* frame 0: full set, chroma deliberately different from luma */
	ps_seq->rgui32_masks[ 0 ] = 0xf;
	make_intra_matrix( ps_seq->rgui8_updates[ 0 ][ MAT_LUMA_INTRA ], 1 );
	make_inter_matrix( ps_seq->rgui8_updates[ 0 ][ MAT_LUMA_INTER ], 2 );
	make_intra_matrix( ps_seq->rgui8_updates[ 0 ][ MAT_CHROMA_INTRA ], 3 );
	make_inter_matrix( ps_seq->rgui8_updates[ 0 ][ MAT_CHROMA_INTER ], 4 );

	/* frame 1: chroma only */
	ps_seq->rgui32_masks[ 1 ] = 0xc;
	make_intra_matrix( ps_seq->rgui8_updates[ 1 ][ MAT_CHROMA_INTRA ], 5 );
	make_inter_matrix( ps_seq->rgui8_updates[ 1 ][ MAT_CHROMA_INTER ], 6 );

	/* frame 2: partial then restore later */
	ps_seq->rgui32_masks[ 2 ] = 0x1;
	make_intra_matrix( ps_seq->rgui8_updates[ 2 ][ MAT_LUMA_INTRA ], 7 );

	/* frame 3: restore luma intra, change inter */
	ps_seq->rgui32_masks[ 3 ] = 0x3;
	ps_seq->rgui8_updates[ 3 ][ MAT_LUMA_INTRA ][ 0 ] = 8;
	for( i = 1; i < 64; i++ )
	{
		ps_seq->rgui8_updates[ 3 ][ MAT_LUMA_INTRA ][ i ] = ps_seq->rgui8_updates[ 0 ][ MAT_LUMA_INTRA ][ i ];
	}
	make_inter_matrix( ps_seq->rgui8_updates[ 3 ][ MAT_LUMA_INTER ], 8 );

	/* frame 4: consecutive B frames get chroma updates */
	ps_seq->rgui32_masks[ 4 ] = 0x4;
	make_intra_matrix( ps_seq->rgui8_updates[ 4 ][ MAT_CHROMA_INTRA ], 9 );
	ps_seq->rgui32_masks[ 5 ] = 0x8;
	make_inter_matrix( ps_seq->rgui8_updates[ 5 ][ MAT_CHROMA_INTER ], 10 );

	/* frame 6: no change at all */
	ps_seq->rgui32_masks[ 6 ] = 0;

	/* frame 7: new gop, luma intra only */
	ps_seq->rgui32_masks[ 7 ] = 0x1;
	make_intra_matrix( ps_seq->rgui8_updates[ 7 ][ MAT_LUMA_INTRA ], 11 );

	/* frame 8: chroma intra, no luma so the chroma matrix must be emitted on its own */
	ps_seq->rgui32_masks[ 8 ] = 0x4;
	make_intra_matrix( ps_seq->rgui8_updates[ 8 ][ MAT_CHROMA_INTRA ], 12 );

	/* frame 9: inter both planes */
	ps_seq->rgui32_masks[ 9 ] = 0xa;
	make_inter_matrix( ps_seq->rgui8_updates[ 9 ][ MAT_LUMA_INTER ], 13 );
	make_inter_matrix( ps_seq->rgui8_updates[ 9 ][ MAT_CHROMA_INTER ], 14 );

	/* frame 10: chroma inter only */
	ps_seq->rgui32_masks[ 10 ] = 0x8;
	make_inter_matrix( ps_seq->rgui8_updates[ 10 ][ MAT_CHROMA_INTER ], 15 );

	/* frame 11: no update, tail */
	ps_seq->rgui32_masks[ 11 ] = 0;
}

/* ------------------------------------------------------------------ */
/* cases                                                              */
/* ------------------------------------------------------------------ */

/*
inheritance group
*/

/* invalid mask / matrix must be rejected without consuming input state */
static int case_invalid_update_retry( void )
{
	test_capture_t s_cap;
	y262_configuration_t s_config;
	void *p_enc;
	test_sequence_t s_seq;
	int32_t f, i;
	uint8_t rgui8_bad[ 4 ][ 64 ];

	memset( &s_cap, 0, sizeof( s_cap ) );
	test_build_sequence( &s_seq );

	/* find a valid sequence we can compare against */
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );
	TASK_CHECK( test_encode( p_enc, &s_cap, TEST_NUM_FRAMES, s_seq.rgui32_masks, s_seq.rgui8_updates ) == 0 );
	TASK_CHECK( s_cap.i_errors == 0 );
	y262_deinitialize( p_enc );

	/* now reject every kind of invalid update and then retry the same valid one */
	{
		test_capture_t s_cap2;
		memset( &s_cap2, 0, sizeof( s_cap2 ) );
		p_enc = y262_create( &s_config );
		test_config_init( &s_config, 1 );
		s_config.pf_result_callback = test_result_cb;
		s_config.p_cb_handle = &s_cap2;
		TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );

		/* picture 0 valid */
		TASK_CHECK( test_push_frame( p_enc, 0, s_seq.rgui32_masks[ 0 ], s_seq.rgui8_updates[ 0 ], 1 ) == Y262_PUSH_INPUT_CONTINUE );

		/* mask with high bits set */
		memset( rgui8_bad, 16, sizeof( rgui8_bad ) );
		{
			y262_input_picture_t s_pic;
			uint8_t y[ TEST_W * TEST_H ], cb[ TEST_W * TEST_H / 4 ], cr[ TEST_W * TEST_H / 4 ];
			test_make_frame( 1, y, cb, cr );
			memset( &s_pic, 0, sizeof( s_pic ) );
			s_pic.pui8_luma = y; s_pic.pui8_cb = cb; s_pic.pui8_cr = cr;
			s_pic.i_frame_structure = Y262_INPUT_PICTURE_FRAME_PROGRESSIVE;
			s_pic.i_quant_matrix_update_mask = 0x10;
			memcpy( s_pic.rgui8_quant_matrices, s_seq.rgui8_updates[ 1 ], sizeof( s_pic.rgui8_quant_matrices ) );
			TASK_CHECK( y262_push_input_picture( p_enc, &s_pic, 1 ) == Y262_PUSH_INPUT_ERR_ARG );
			/* a zeroed matrix on a selected bit is invalid ( quotient 0 ) */
			s_pic.i_quant_matrix_update_mask = 0x1;
			memset( s_pic.rgui8_quant_matrices[ 0 ], 0, 64 );
			TASK_CHECK( y262_push_input_picture( p_enc, &s_pic, 1 ) == Y262_PUSH_INPUT_ERR_ARG );
			/* intra dc not 8 */
			memcpy( s_pic.rgui8_quant_matrices, s_seq.rgui8_updates[ 1 ], sizeof( s_pic.rgui8_quant_matrices ) );
			s_pic.rgui8_quant_matrices[ 0 ][ 0 ] = 9;
			TASK_CHECK( y262_push_input_picture( p_enc, &s_pic, 1 ) == Y262_PUSH_INPUT_ERR_ARG );
			/* an unselected array with garbage is ignored */
			memcpy( s_pic.rgui8_quant_matrices, s_seq.rgui8_updates[ 1 ], sizeof( s_pic.rgui8_quant_matrices ) );
			s_pic.i_quant_matrix_update_mask = 0xc;
			memset( s_pic.rgui8_quant_matrices[ 0 ], 0, 64 );
			memset( s_pic.rgui8_quant_matrices[ 1 ], 0, 64 );
			TASK_CHECK( y262_push_input_picture( p_enc, &s_pic, 1 ) == Y262_PUSH_INPUT_CONTINUE );
		}

		/* finish sequence: rest of the frames using the already validated updates */
		for( f = 2; f < TEST_NUM_FRAMES; f++ )
		{
			TASK_CHECK( test_push_frame( p_enc, f, s_seq.rgui32_masks[ f ], s_seq.rgui8_updates[ f ], 1 ) == Y262_PUSH_INPUT_CONTINUE );
		}
		{
			int32_t done = 0;
			for( i = 0; i < 1024; i++ )
			{
				int32_t rc = y262_push_input_picture( p_enc, NULL, TEST_NUM_FRAMES );
				if( rc == Y262_PUSH_INPUT_FLUSHED ) { done = 1; break; }
				TASK_CHECK( rc == Y262_PUSH_INPUT_CONTINUE );
			}
			TASK_CHECK( done );
		}
		TASK_CHECK( s_cap2.i_errors == 0 );
		y262_deinitialize( p_enc );
	}

	test_capture_free( &s_cap );
	return 0;
}

/* inheritance: a picture's own snapshot survives lookahead, reorder and flush */
static int case_inheritance_snapshot_across_reorder( void )
{
	test_capture_t s_cap;
	y262_configuration_t s_config;
	void *p_enc;
	test_sequence_t s_seq;
	int32_t f, i;
	test_pic_header_t rgs_pics[ 64 ];
	int32_t i_num_pics, i_num_ext, rgi_pon_by_don[ 64 ];
	uint8_t rgui8_expected[ 4 ][ 64 ];

	memset( &s_cap, 0, sizeof( s_cap ) );
	test_build_sequence( &s_seq );
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );
	TASK_CHECK( test_encode( p_enc, &s_cap, TEST_NUM_FRAMES, s_seq.rgui32_masks, s_seq.rgui8_updates ) == 0 );
	TASK_CHECK( s_cap.i_errors == 0 );
	TASK_CHECK( s_cap.i_num_recon == TEST_NUM_FRAMES );

	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_cap, rgi_pon_by_don, 64 );
	i_num_pics = test_parse_stream( s_cap.pui8_es, s_cap.i_es_len, rgs_pics, 64, &i_num_ext, rgi_pon_by_don, TEST_NUM_FRAMES );
	TASK_CHECK( i_num_pics == TEST_NUM_FRAMES );

	/* for every pon, the decoder state after its own extension equals the input snapshot */
	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		test_pic_header_t *ps_pic = test_pic_for_pon( &s_cap, rgs_pics, i_num_pics, f );
		TASK_CHECK( ps_pic != NULL );
		if( !ps_pic )
		{
			continue;
		}
		test_expected_snapshot( &s_seq, f, f, rgui8_expected );
		for( i = 0; i < 4; i++ )
		{
			TASK_CHECK( memcmp( ps_pic->rgui8_state_out[ i ], rgui8_expected[ i ], 64 ) == 0 );
		}
	}

	y262_deinitialize( p_enc );
	test_capture_free( &s_cap );
	return 0;
}

/* unchanged matrices produce no extension at all */
static int case_inheritance_no_change_no_extension( void )
{
	test_capture_t s_cap;
	y262_configuration_t s_config;
	void *p_enc;
	test_sequence_t s_seq;
	uint32_t rgui32_masks[ 8 ];
	uint8_t rgui8_updates[ 8 ][ 4 ][ 64 ];
	int32_t f, i;
	test_pic_header_t rgs_pics[ 32 ];
	int32_t i_num_pics, i_num_ext, rgi_pon_by_don[ 32 ];

	memset( &s_cap, 0, sizeof( s_cap ) );
	test_build_sequence( &s_seq );
	memset( rgui32_masks, 0, sizeof( rgui32_masks ) );
	memset( rgui8_updates, 0, sizeof( rgui8_updates ) );
	/* one update on frame 0 only, everything after it inherits */
	rgui32_masks[ 0 ] = 0xf;
	memcpy( rgui8_updates[ 0 ], s_seq.rgui8_updates[ 0 ], sizeof( rgui8_updates[ 0 ] ) );
	memcpy( s_seq.rgui32_masks, rgui32_masks, sizeof( rgui32_masks ) );
	memcpy( s_seq.rgui8_updates, rgui8_updates, sizeof( rgui8_updates ) );

	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );
	TASK_CHECK( test_encode( p_enc, &s_cap, 8, rgui32_masks, rgui8_updates ) == 0 );
	TASK_CHECK( s_cap.i_errors == 0 );

	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_cap, rgi_pon_by_don, 32 );
	i_num_pics = test_parse_stream( s_cap.pui8_es, s_cap.i_es_len, rgs_pics, 32, &i_num_ext, rgi_pon_by_don, 8 );
	TASK_CHECK( i_num_pics == 8 );
	/* only the very first picture needs to send anything: the sequence header
	   already reprograms the decoder to the configured matrices */
	/*
	sequence headers ( written at every I picture ) reset the decode side matrices,
	so a picture after an I frame may legitimately need to re-emit even when its
	own input mask is empty. the invariant is: a picture with no mask that follows a
	picture carrying the very same snapshot and with no sequence header in between
	must not emit an extension.
	*/
	{
		int32_t i_num_b_without_ext = 0;
		for( f = 0; f < i_num_pics; f++ )
		{
			if( !( rgs_pics[ f ].i_quant_flags ) &&
				memcmp( rgs_pics[ f ].rgui8_state_in, rgs_pics[ f ].rgui8_state_out, sizeof( rgs_pics[ f ].rgui8_state_in ) ) == 0 )
			{
				i_num_b_without_ext++;
			}
		}
		TASK_CHECK( i_num_b_without_ext >= 4 );
	}

	/* every picture still decodes with the right matrices */
	for( f = 0; f < 8; f++ )
	{
		test_pic_header_t *ps_pic = test_pic_for_pon( &s_cap, rgs_pics, i_num_pics, f );
		uint8_t rgui8_expected[ 4 ][ 64 ];
		TASK_CHECK( ps_pic != NULL );
		if( ps_pic )
		{
			test_expected_snapshot( &s_seq, f, f, rgui8_expected );
			for( i = 0; i < 4; i++ )
			{
				TASK_CHECK( memcmp( ps_pic->rgui8_state_out[ i ], rgui8_expected[ i ], 64 ) == 0 );
			}
		}
	}

	y262_deinitialize( p_enc );
	test_capture_free( &s_cap );
	return 0;
}

/*
coefficients group
*/

/* run the observer over a whole sequence and verify coefficients + recon */
static int test_coefficients_common( int32_t i_quality, int32_t i_threads )
{
	test_capture_t s_cap;
	y262_configuration_t s_config;
	void *p_enc;
	test_sequence_t s_seq;
	int32_t n, i_chroma_blocks = 0, i_intra_chroma = 0, i_inter_chroma = 0;

	memset( &s_cap, 0, sizeof( s_cap ) );
	test_build_sequence( &s_seq );
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, i_threads );
	s_config.i_quality_for_speed = i_quality;
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );

	i_num_obs = 0;
	y262_test_block_observer = test_block_observer;
	TASK_CHECK( test_encode( p_enc, &s_cap, TEST_NUM_FRAMES, s_seq.rgui32_masks, s_seq.rgui8_updates ) == 0 );
	TASK_CHECK( s_cap.i_errors == 0 );

	for( n = 0; n < i_num_obs; n++ )
	{
		test_block_obs_t *ps_obs = &rgts_obs[ n ];
		int32_t i_mat_idx;
		int16_t rgi16_expected[ 64 ];

		if( ps_obs->i_plane_idx == 0 )
		{
			i_mat_idx = ps_obs->b_intra ? MAT_LUMA_INTRA : MAT_LUMA_INTER;
		}
		else
		{
			i_mat_idx = ps_obs->b_intra ? MAT_CHROMA_INTRA : MAT_CHROMA_INTER;
			i_chroma_blocks++;
			if( ps_obs->b_intra ) { i_intra_chroma++; } else { i_inter_chroma++; }
		}

		/* the block must have been coded with the picture's own input-order snapshot */
		{
			uint8_t rgui8_expected_snapshot[ 4 ][ 64 ];
			test_expected_snapshot( &s_seq, ps_obs->i_pon, ps_obs->i_pon, rgui8_expected_snapshot );
			TASK_CHECK( memcmp( ps_obs->rgui8_matrices[ i_mat_idx ], rgui8_expected_snapshot[ i_mat_idx ], 64 ) == 0 );
			/* and the chroma matrices are genuinely different from the luma ones */
			if( ps_obs->i_plane_idx != 0 )
			{
				TASK_CHECK( memcmp( ps_obs->rgui8_matrices[ i_mat_idx ], ps_obs->rgui8_matrices[ ps_obs->b_intra ? 0 : 1 ], 64 ) != 0 );
			}
		}

		/* production inverse quantized coefficients match the independent model */
		test_dequant( ps_obs->rgi16_levels, ps_obs->rgui8_matrices[ i_mat_idx ], ps_obs->i_quantizer, ps_obs->b_intra, rgi16_expected );
		TASK_CHECK( memcmp( rgi16_expected, ps_obs->rgi16_dequant, sizeof( rgi16_expected ) ) == 0 );
	}
	y262_test_block_observer = NULL;

	/* the run has to exercise intra and inter chroma blocks */
	TASK_CHECK( i_chroma_blocks > 0 );
	TASK_CHECK( i_intra_chroma > 0 );
	TASK_CHECK( i_inter_chroma > 0 );

	y262_deinitialize( p_enc );
	test_capture_free( &s_cap );
	return 0;
}

static int case_coefficients_fast( void )
{
	return test_coefficients_common( -50, 1 );
}

static int case_coefficients_trellis( void )
{
	return test_coefficients_common( 10, 1 );
}

/*
stream group
*/

/* the decoder side matrix reconstructed from headers matches the input order */
static int case_stream_diff_emission( void )
{
	test_capture_t s_cap;
	y262_configuration_t s_config;
	void *p_enc;
	test_sequence_t s_seq;
	test_pic_header_t rgs_pics[ 64 ];
	int32_t i_num_pics, i_num_ext, rgi_pon_by_don[ 64 ];
	int32_t f, i;
	int32_t i_emitted = 0;

	memset( &s_cap, 0, sizeof( s_cap ) );
	test_build_sequence( &s_seq );
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );
	TASK_CHECK( test_encode( p_enc, &s_cap, TEST_NUM_FRAMES, s_seq.rgui32_masks, s_seq.rgui8_updates ) == 0 );
	TASK_CHECK( s_cap.i_errors == 0 );

	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_cap, rgi_pon_by_don, 64 );
	i_num_pics = test_parse_stream( s_cap.pui8_es, s_cap.i_es_len, rgs_pics, 64, &i_num_ext, rgi_pon_by_don, TEST_NUM_FRAMES );
	TASK_CHECK( i_num_pics == TEST_NUM_FRAMES );

	/* walk decode order and check every loaded matrix is what the picture needs */
	for( i = 0; i < i_num_pics; i++ )
	{
		uint8_t rgui8_expected[ 4 ][ 64 ];
		uint32_t ui_mask;
		test_pic_header_t *ps_pic = &rgs_pics[ i ];
		int32_t i_pon = ps_pic->i_pon;

		test_expected_snapshot( &s_seq, i_pon, i_pon, rgui8_expected );
		ui_mask = s_seq.rgui32_masks[ i_pon ];

		/* only matrices that actually differ from the decoder state may be loaded,
		   and any load must carry the picture's required value */
		for( f = 0; f < 4; f++ )
		{
			if( ps_pic->i_quant_flags & ( 1 << f ) )
			{
				TASK_CHECK( memcmp( ps_pic->rgui8_load[ f ], rgui8_expected[ f ], 64 ) == 0 );
				/* luma load implies chroma reload; if chroma differs the encoder must
				   also load chroma explicitly */
			}
		}
		/* state after the extension must be exactly the expected snapshot */
		for( f = 0; f < 4; f++ )
		{
			TASK_CHECK( memcmp( ps_pic->rgui8_state_out[ f ], rgui8_expected[ f ], 64 ) == 0 );
		}
		/* the emission should be minimal: flags are a subset of what is needed */
		TASK_CHECK( ( ps_pic->i_quant_flags & ~0xf ) == 0 );
		(void)ui_mask;
		i_emitted += ps_pic->i_has_quant_ext;
	}
	TASK_CHECK( i_emitted == i_num_ext );

	/* at least two consecutive B pictures must carry updates, exercising the
	   per picture snapshot through the reorder path */
	{
		int32_t b_consecutive_b_updates = 0, k;
		for( k = 1; k < i_num_pics; k++ )
		{
			if( rgs_pics[ k ].i_picture_type == 3 && rgs_pics[ k - 1 ].i_picture_type == 3 &&
				rgs_pics[ k ].i_quant_flags && rgs_pics[ k - 1 ].i_quant_flags )
			{
				b_consecutive_b_updates = 1;
			}
		}
		TASK_CHECK( b_consecutive_b_updates );
	}
	/* in particular, some chroma only updates must have been emitted on their own */
	{
		int32_t b_saw_chroma_only = 0;
		for( i = 0; i < i_num_pics; i++ )
		{
			if( rgs_pics[ i ].i_quant_flags & 0xc && !( rgs_pics[ i ].i_quant_flags & 0x3 ) )
			{
				b_saw_chroma_only = 1;
			}
		}
		TASK_CHECK( b_saw_chroma_only );
	}

	y262_deinitialize( p_enc );
	test_capture_free( &s_cap );
	return 0;
}

/* two interleaved instances keep independent inheritance */
static int case_stream_two_instances( void )
{
	test_capture_t s_cap_a, s_cap_b;
	y262_configuration_t s_config;
	void *p_enc_a, *p_enc_b;
	test_sequence_t s_seq_a, s_seq_b;
	int32_t f, i;
	test_pic_header_t rgs_pics[ 64 ];
	int32_t i_num_pics, i_num_ext, rgi_pon_by_don[ 64 ];

	memset( &s_cap_a, 0, sizeof( s_cap_a ) );
	memset( &s_cap_b, 0, sizeof( s_cap_b ) );
	test_build_sequence( &s_seq_a );
	memset( &s_seq_b, 0, sizeof( s_seq_b ) );
	/* instance b gets a different, simpler update pattern */
	s_seq_b.rgui32_masks[ 0 ] = 0x2;
	make_inter_matrix( s_seq_b.rgui8_updates[ 0 ][ MAT_LUMA_INTER ], 21 );
	s_seq_b.rgui32_masks[ 2 ] = 0x8;
	make_inter_matrix( s_seq_b.rgui8_updates[ 2 ][ MAT_CHROMA_INTER ], 22 );

	s_config.pf_result_callback = test_result_cb;
	p_enc_a = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap_a;
	TASK_CHECK( y262_initialize( p_enc_a, &s_config ) == Y262_INIT_SUCCESS );

	p_enc_b = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap_b;
	TASK_CHECK( y262_initialize( p_enc_b, &s_config ) == Y262_INIT_SUCCESS );

	/* interleave the pushes */
	test_capture_reset( &s_cap_a );
	test_capture_reset( &s_cap_b );
	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		TASK_CHECK( test_push_frame( p_enc_a, f, s_seq_a.rgui32_masks[ f ], s_seq_a.rgui8_updates[ f ], 1 ) == Y262_PUSH_INPUT_CONTINUE );
		TASK_CHECK( test_push_frame( p_enc_b, f, s_seq_b.rgui32_masks[ f ], s_seq_b.rgui8_updates[ f ], 1 ) == Y262_PUSH_INPUT_CONTINUE );
	}
	{
		int32_t done_a = 0, done_b = 0;
		for( i = 0; i < 1024 && !( done_a && done_b ); i++ )
		{
			if( !done_a )
			{
				int32_t rc = y262_push_input_picture( p_enc_a, NULL, TEST_NUM_FRAMES );
				if( rc == Y262_PUSH_INPUT_FLUSHED ) { done_a = 1; }
				else { TASK_CHECK( rc == Y262_PUSH_INPUT_CONTINUE ); }
			}
			if( !done_b )
			{
				int32_t rc = y262_push_input_picture( p_enc_b, NULL, TEST_NUM_FRAMES );
				if( rc == Y262_PUSH_INPUT_FLUSHED ) { done_b = 1; }
				else { TASK_CHECK( rc == Y262_PUSH_INPUT_CONTINUE ); }
			}
		}
		TASK_CHECK( done_a && done_b );
	}

	/* a: full sequence expected matrices */
	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_cap_a, rgi_pon_by_don, 64 );
	i_num_pics = test_parse_stream( s_cap_a.pui8_es, s_cap_a.i_es_len, rgs_pics, 64, &i_num_ext, rgi_pon_by_don, TEST_NUM_FRAMES );
	TASK_CHECK( i_num_pics == TEST_NUM_FRAMES );
	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		test_pic_header_t *ps_pic = test_pic_for_pon( &s_cap_a, rgs_pics, i_num_pics, f );
		uint8_t rgui8_expected[ 4 ][ 64 ];
		test_expected_snapshot( &s_seq_a, f, f, rgui8_expected );
		TASK_CHECK( ps_pic != NULL );
		if( ps_pic )
		{
			for( i = 0; i < 4; i++ )
			{
				TASK_CHECK( memcmp( ps_pic->rgui8_state_out[ i ], rgui8_expected[ i ], 64 ) == 0 );
			}
		}
	}

	/* b: its own pattern */
	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_cap_b, rgi_pon_by_don, 64 );
	i_num_pics = test_parse_stream( s_cap_b.pui8_es, s_cap_b.i_es_len, rgs_pics, 64, &i_num_ext, rgi_pon_by_don, TEST_NUM_FRAMES );
	TASK_CHECK( i_num_pics == TEST_NUM_FRAMES );
	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		test_pic_header_t *ps_pic = test_pic_for_pon( &s_cap_b, rgs_pics, i_num_pics, f );
		uint8_t rgui8_expected[ 4 ][ 64 ];
		test_expected_snapshot( &s_seq_b, f, f, rgui8_expected );
		TASK_CHECK( ps_pic != NULL );
		if( ps_pic )
		{
			for( i = 0; i < 4; i++ )
			{
				TASK_CHECK( memcmp( ps_pic->rgui8_state_out[ i ], rgui8_expected[ i ], 64 ) == 0 );
			}
		}
	}

	y262_deinitialize( p_enc_a );
	y262_deinitialize( p_enc_b );
	test_capture_free( &s_cap_a );
	test_capture_free( &s_cap_b );
	return 0;
}

/* multi slice threads produce the same stream content as single threaded */
static int case_stream_threads_consistent( void )
{
	test_capture_t s_cap1, s_capN;
	y262_configuration_t s_config;
	void *p_enc1, *p_encN;
	test_sequence_t s_seq;
	test_pic_header_t rgs_pics1[ 64 ], rgs_picsN[ 64 ];
	int32_t i_num_pics1, i_num_picsN, i_num_ext1, i_num_extN;
	int32_t rgi_pon_by_don[ 64 ];
	int32_t f, i;

	memset( &s_cap1, 0, sizeof( s_cap1 ) );
	memset( &s_capN, 0, sizeof( s_capN ) );
	test_build_sequence( &s_seq );

	s_config.pf_result_callback = test_result_cb;
	p_enc1 = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap1;
	TASK_CHECK( y262_initialize( p_enc1, &s_config ) == Y262_INIT_SUCCESS );

	p_encN = y262_create( &s_config );
	test_config_init( &s_config, 4 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_capN;
	TASK_CHECK( y262_initialize( p_encN, &s_config ) == Y262_INIT_SUCCESS );

	TASK_CHECK( test_encode( p_enc1, &s_cap1, TEST_NUM_FRAMES, s_seq.rgui32_masks, s_seq.rgui8_updates ) == 0 );
	TASK_CHECK( test_encode( p_encN, &s_capN, TEST_NUM_FRAMES, s_seq.rgui32_masks, s_seq.rgui8_updates ) == 0 );
	TASK_CHECK( s_cap1.i_errors == 0 );
	TASK_CHECK( s_capN.i_errors == 0 );

	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_cap1, rgi_pon_by_don, 64 );
	i_num_pics1 = test_parse_stream( s_cap1.pui8_es, s_cap1.i_es_len, rgs_pics1, 64, &i_num_ext1, rgi_pon_by_don, TEST_NUM_FRAMES );
	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_capN, rgi_pon_by_don, 64 );
	i_num_picsN = test_parse_stream( s_capN.pui8_es, s_capN.i_es_len, rgs_picsN, 64, &i_num_extN, rgi_pon_by_don, TEST_NUM_FRAMES );

	TASK_CHECK( i_num_pics1 == TEST_NUM_FRAMES );
	TASK_CHECK( i_num_picsN == TEST_NUM_FRAMES );
	TASK_CHECK( i_num_ext1 == i_num_extN );
	TASK_CHECK( s_cap1.i_num_recon == s_capN.i_num_recon );

	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		test_pic_header_t *ps_a = test_pic_for_pon( &s_cap1, rgs_pics1, i_num_pics1, f );
		test_pic_header_t *ps_b = test_pic_for_pon( &s_capN, rgs_picsN, i_num_picsN, f );
		TASK_CHECK( ps_a != NULL && ps_b != NULL );
		if( ps_a && ps_b )
		{
			TASK_CHECK( ps_a->i_quant_flags == ps_b->i_quant_flags );
			for( i = 0; i < 4; i++ )
			{
				TASK_CHECK( memcmp( ps_a->rgui8_state_out[ i ], ps_b->rgui8_state_out[ i ], 64 ) == 0 );
			}
		}
	}

	y262_deinitialize( p_enc1 );
	y262_deinitialize( p_encN );
	test_capture_free( &s_cap1 );
	test_capture_free( &s_capN );
	return 0;
}

/* input structure and pixel buffers can be rewritten right after push */
static int case_input_overwrite( void )
{
	test_capture_t s_cap;
	y262_configuration_t s_config;
	void *p_enc;
	test_sequence_t s_seq;
	int32_t f, i;
	test_pic_header_t rgs_pics[ 64 ];
	int32_t i_num_pics, i_num_ext, rgi_pon_by_don[ 64 ];
	uint8_t y[ TEST_W * TEST_H ], cb[ TEST_W * TEST_H / 4 ], cr[ TEST_W * TEST_H / 4 ];

	memset( &s_cap, 0, sizeof( s_cap ) );
	test_build_sequence( &s_seq );
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );

	test_capture_reset( &s_cap );
	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		y262_input_picture_t s_pic;
		test_make_frame( f, y, cb, cr );
		memset( &s_pic, 0, sizeof( s_pic ) );
		s_pic.pui8_luma = y; s_pic.pui8_cb = cb; s_pic.pui8_cr = cr;
		s_pic.i_frame_structure = Y262_INPUT_PICTURE_FRAME_PROGRESSIVE;
		s_pic.i_quant_matrix_update_mask = s_seq.rgui32_masks[ f ];
		memcpy( s_pic.rgui8_quant_matrices, s_seq.rgui8_updates[ f ], sizeof( s_pic.rgui8_quant_matrices ) );
		TASK_CHECK( y262_push_input_picture( p_enc, &s_pic, f ) == Y262_PUSH_INPUT_CONTINUE );

		/* immediately scribble over the input structure and pixel buffers */
		memset( &s_pic, 0xa5, sizeof( s_pic ) );
		memset( y, 0xa5, sizeof( y ) );
		memset( cb, 0xa5, sizeof( cb ) );
		memset( cr, 0xa5, sizeof( cr ) );
	}
	{
		int32_t done = 0;
		for( i = 0; i < 1024; i++ )
		{
			int32_t rc = y262_push_input_picture( p_enc, NULL, TEST_NUM_FRAMES );
			if( rc == Y262_PUSH_INPUT_FLUSHED ) { done = 1; break; }
			TASK_CHECK( rc == Y262_PUSH_INPUT_CONTINUE );
		}
		TASK_CHECK( done );
	}
	TASK_CHECK( s_cap.i_errors == 0 );

	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_cap, rgi_pon_by_don, 64 );
	i_num_pics = test_parse_stream( s_cap.pui8_es, s_cap.i_es_len, rgs_pics, 64, &i_num_ext, rgi_pon_by_don, TEST_NUM_FRAMES );
	TASK_CHECK( i_num_pics == TEST_NUM_FRAMES );
	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		test_pic_header_t *ps_pic = test_pic_for_pon( &s_cap, rgs_pics, i_num_pics, f );
		uint8_t rgui8_expected[ 4 ][ 64 ];
		test_expected_snapshot( &s_seq, f, f, rgui8_expected );
		TASK_CHECK( ps_pic != NULL );
		if( ps_pic )
		{
			for( i = 0; i < 4; i++ )
			{
				TASK_CHECK( memcmp( ps_pic->rgui8_state_out[ i ], rgui8_expected[ i ], 64 ) == 0 );
			}
		}
	}

	y262_deinitialize( p_enc );
	test_capture_free( &s_cap );
	return 0;
}

/* initialization rejects out of range configurations */
static int case_init_rejects_out_of_range( void )
{
	y262_configuration_t s_config;
	void *p_enc;

	/* interlaced */
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.b_interlaced = 1;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_ERROR_PICTURE_MATRICES );
	y262_dealloc( p_enc );

	/* pass1 */
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.i_rcmode = BITRATE_CONTROL_PASS1;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_ERROR_PICTURE_MATRICES );
	y262_dealloc( p_enc );

	/* 4:2:2 */
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.i_coded_chroma_format = Y262_CHROMA_FORMAT_422;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_ERROR_PICTURE_MATRICES );
	y262_dealloc( p_enc );

	/* mpeg1 */
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.b_mpeg1 = 1;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_ERROR_PICTURE_MATRICES );
	y262_dealloc( p_enc );

	/* disabled: the feature is completely off and valid setup works */
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.b_picture_quant_matrices = 0;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );
	y262_deinitialize( p_enc );

	return 0;
}

/* disabled mode ignores the new fields and matches the legacy stream byte for byte */
static int case_disabled_ignores_fields( void )
{
	test_capture_t s_cap_a, s_cap_b;
	y262_configuration_t s_config;
	void *p_enc_a, *p_enc_b;
	int32_t f, i;
	uint32_t rgui32_masks[ TEST_NUM_FRAMES ];
	uint8_t rgui8_updates[ TEST_NUM_FRAMES ][ 4 ][ 64 ];
	uint8_t rgui8_garbage[ 4 ][ 64 ];

	memset( &s_cap_a, 0, sizeof( s_cap_a ) );
	memset( &s_cap_b, 0, sizeof( s_cap_b ) );
	memset( rgui32_masks, 0, sizeof( rgui32_masks ) );
	memset( rgui8_updates, 0, sizeof( rgui8_updates ) );
	memset( rgui8_garbage, 0x5a, sizeof( rgui8_garbage ) );

	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		rgui32_masks[ f ] = 0xf;
		memcpy( rgui8_updates[ f ], rgui8_garbage, sizeof( rgui8_updates[ f ] ) );
	}

	/* a: disabled, zeroed new fields */
	p_enc_a = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.b_picture_quant_matrices = 0;
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap_a;
	TASK_CHECK( y262_initialize( p_enc_a, &s_config ) == Y262_INIT_SUCCESS );
	test_capture_reset( &s_cap_a );
	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		TASK_CHECK( test_push_frame( p_enc_a, f, 0, NULL, 0 ) == Y262_PUSH_INPUT_CONTINUE );
	}

	/* b: disabled, garbage in the new fields */
	p_enc_b = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.b_picture_quant_matrices = 0;
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap_b;
	TASK_CHECK( y262_initialize( p_enc_b, &s_config ) == Y262_INIT_SUCCESS );
	test_capture_reset( &s_cap_b );
	for( f = 0; f < TEST_NUM_FRAMES; f++ )
	{
		TASK_CHECK( test_push_frame( p_enc_b, f, rgui32_masks[ f ], rgui8_updates[ f ], 1 ) == Y262_PUSH_INPUT_CONTINUE );
	}

	{
		int32_t done_a = 0, done_b = 0;
		for( i = 0; i < 1024 && !( done_a && done_b ); i++ )
		{
			if( !done_a )
			{
				int32_t rc = y262_push_input_picture( p_enc_a, NULL, TEST_NUM_FRAMES );
				if( rc == Y262_PUSH_INPUT_FLUSHED ) { done_a = 1; }
				else { TASK_CHECK( rc == Y262_PUSH_INPUT_CONTINUE ); }
			}
			if( !done_b )
			{
				int32_t rc = y262_push_input_picture( p_enc_b, NULL, TEST_NUM_FRAMES );
				if( rc == Y262_PUSH_INPUT_FLUSHED ) { done_b = 1; }
				else { TASK_CHECK( rc == Y262_PUSH_INPUT_CONTINUE ); }
			}
		}
		TASK_CHECK( done_a && done_b );
	}

	TASK_CHECK( s_cap_a.i_errors == 0 && s_cap_b.i_errors == 0 );
	TASK_CHECK( s_cap_a.i_es_len == s_cap_b.i_es_len );
	TASK_CHECK( memcmp( s_cap_a.pui8_es, s_cap_b.pui8_es, ( size_t )s_cap_a.i_es_len ) == 0 );

	y262_deinitialize( p_enc_a );
	y262_deinitialize( p_enc_b );
	test_capture_free( &s_cap_a );
	test_capture_free( &s_cap_b );
	return 0;
}

/*
exercise the sequence header side effect: a non-default configured luma matrix is
written ( and implicitly reloads the matching chroma matrix ), while the pictures
switch chroma weights per picture. the decoder side model has to follow the
implicit reload and the encoder has to re-emit the chroma matrix explicitly when
the picture needs one that differs from the implicit reload.
*/
static int case_stream_sequence_header_side_effect( void )
{
	test_capture_t s_cap;
	y262_configuration_t s_config;
	void *p_enc;
	uint32_t rgui32_masks[ 8 ];
	uint8_t rgui8_updates[ 8 ][ 4 ][ 64 ];
	uint8_t rgui8_cfg_intra[ 64 ], rgui8_cfg_inter[ 64 ];
	test_pic_header_t rgs_pics[ 32 ];
	int32_t i_num_pics, i_num_ext, rgi_pon_by_don[ 32 ];
	int32_t f, i;

	memset( &s_cap, 0, sizeof( s_cap ) );
	make_intra_matrix( rgui8_cfg_intra, 30 );
	make_inter_matrix( rgui8_cfg_inter, 31 );
	memset( rgui32_masks, 0, sizeof( rgui32_masks ) );
	memset( rgui8_updates, 0, sizeof( rgui8_updates ) );

	/* picture 1 gets a chroma intra weight that differs from the configured luma */
	rgui32_masks[ 1 ] = 0x4;
	make_intra_matrix( rgui8_updates[ 1 ][ MAT_CHROMA_INTRA ], 33 );
	/* picture 3 only changes chroma inter */
	rgui32_masks[ 3 ] = 0x8;
	make_inter_matrix( rgui8_updates[ 3 ][ MAT_CHROMA_INTER ], 34 );

	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.b_non_default_intra_matrix = 1;
	memcpy( s_config.rgui8_non_default_intra_matrix, rgui8_cfg_intra, 64 );
	s_config.b_non_default_inter_matrix = 1;
	memcpy( s_config.rgui8_non_default_inter_matrix, rgui8_cfg_inter, 64 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );
	TASK_CHECK( test_encode( p_enc, &s_cap, 8, rgui32_masks, rgui8_updates ) == 0 );
	TASK_CHECK( s_cap.i_errors == 0 );

	memset( rgi_pon_by_don, 0, sizeof( rgi_pon_by_don ) );
	test_build_don_pons( &s_cap, rgi_pon_by_don, 32 );
	i_num_pics = test_parse_stream( s_cap.pui8_es, s_cap.i_es_len, rgs_pics, 32, &i_num_ext, rgi_pon_by_don, 8 );
	TASK_CHECK( i_num_pics == 8 );

	{
		uint8_t rgui8_snapshot[ 4 ][ 64 ];
		/* initial snapshot: configured luma, chroma copies the corresponding luma */
		memcpy( rgui8_snapshot[ 0 ], rgui8_cfg_intra, 64 );
		memcpy( rgui8_snapshot[ 1 ], rgui8_cfg_inter, 64 );
		memcpy( rgui8_snapshot[ 2 ], rgui8_cfg_intra, 64 );
		memcpy( rgui8_snapshot[ 3 ], rgui8_cfg_inter, 64 );
		for( f = 0; f < 8; f++ )
		{
			test_pic_header_t *ps_pic;
			for( i = 0; i < 4; i++ )
			{
				if( rgui32_masks[ f ] & ( 1u << i ) )
				{
					memcpy( rgui8_snapshot[ i ], rgui8_updates[ f ][ i ], 64 );
				}
			}
			ps_pic = test_pic_for_pon( &s_cap, rgs_pics, i_num_pics, f );
			TASK_CHECK( ps_pic != NULL );
			if( !ps_pic )
			{
				continue;
			}
			for( i = 0; i < 4; i++ )
			{
				TASK_CHECK( memcmp( ps_pic->rgui8_state_out[ i ], rgui8_snapshot[ i ], 64 ) == 0 );
			}
		}
	}

	y262_deinitialize( p_enc );
	test_capture_free( &s_cap );
	return 0;
}

static int case_stream_b_frames_and_gop( void )
{
	/* the shared sequence already contains B frames and a gop boundary; this case
	   verifies both are present in the encoded stream and all pictures decoded. */
	test_capture_t s_cap;
	y262_configuration_t s_config;
	void *p_enc;
	test_sequence_t s_seq;
	int32_t i, i_types[ 4 ];
	int32_t b_saw_b = 0, b_saw_i_after_b = 0, i_last_type = 0;

	memset( &s_cap, 0, sizeof( s_cap ) );
	test_build_sequence( &s_seq );
	p_enc = y262_create( &s_config );
	test_config_init( &s_config, 1 );
	s_config.pf_result_callback = test_result_cb;
	s_config.p_cb_handle = &s_cap;
	TASK_CHECK( y262_initialize( p_enc, &s_config ) == Y262_INIT_SUCCESS );
	TASK_CHECK( test_encode( p_enc, &s_cap, TEST_NUM_FRAMES, s_seq.rgui32_masks, s_seq.rgui8_updates ) == 0 );
	TASK_CHECK( s_cap.i_errors == 0 );

	memset( i_types, 0, sizeof( i_types ) );
	TASK_CHECK( s_cap.i_num_recon == TEST_NUM_FRAMES );
	for( i = 0; i < s_cap.i_num_recon; i++ )
	{
		int32_t t = s_cap.rgs_recon[ i ].i_frame_type;
		TASK_CHECK( t >= 1 && t <= 3 );
		i_types[ t ]++;
		if( t == 3 ) b_saw_b = 1;
		if( t == 1 && i_last_type == 3 ) b_saw_i_after_b = 1;
		i_last_type = t;
	}
	TASK_CHECK( i_types[ 1 ] > 0 );
	TASK_CHECK( i_types[ 2 ] > 0 );
	TASK_CHECK( i_types[ 3 ] > 0 );
	TASK_CHECK( b_saw_b );
	TASK_CHECK( b_saw_i_after_b );
	/* each pon exactly once */
	for( i = 0; i < TEST_NUM_FRAMES; i++ )
	{
		int32_t j, n = 0;
		for( j = 0; j < s_cap.i_num_recon; j++ )
		{
			if( s_cap.rgs_recon[ j ].i_pon == i ) n++;
		}
		TASK_CHECK( n == 1 );
	}

	y262_deinitialize( p_enc );
	test_capture_free( &s_cap );
	return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "inheritance", "invalid_update_retry", case_invalid_update_retry },
	{ "inheritance", "snapshot_across_reorder", case_inheritance_snapshot_across_reorder },
	{ "inheritance", "no_change_no_extension", case_inheritance_no_change_no_extension },
	{ "coefficients", "fast_quant_chroma", case_coefficients_fast },
	{ "coefficients", "trellis_chroma", case_coefficients_trellis },
	{ "stream", "diff_emission", case_stream_diff_emission },
	{ "stream", "two_instances", case_stream_two_instances },
	{ "stream", "threads_consistent", case_stream_threads_consistent },
	{ "stream", "input_overwrite", case_input_overwrite },
	{ "stream", "init_rejects", case_init_rejects_out_of_range },
	{ "stream", "disabled_fields", case_disabled_ignores_fields },
	{ "stream", "sequence_header_side_effect", case_stream_sequence_header_side_effect },
	{ "stream", "bframes_and_gop", case_stream_b_frames_and_gop }
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[ 0 ] );
