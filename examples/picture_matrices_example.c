/*
per picture quantization matrix example.

this example shows how to drive the per picture matrix update feature from a
plain C application. it encodes a tiny synthetic clip and switches the luma and
chroma frequency weights between pictures.

build it against the static library, e.g.

	cc -std=c99 -Isrc/y262 examples/picture_matrices_example.c liby262.a -lm -pthread

the feature only works for an 8 bit mpeg2 progressive 4:2:0 constant quantizer
setup. see y262api.h for the exact semantics.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "y262api.h"

#define EX_W 64
#define EX_H 64
#define EX_FRAMES 8

/* matrix index layout: 0 luma intra, 1 luma inter, 2 chroma intra, 3 chroma inter */
#define MAT_LUMA_INTRA   0
#define MAT_LUMA_INTER   1
#define MAT_CHROMA_INTRA 2
#define MAT_CHROMA_INTER 3

static void on_error( void *p_handle, int32_t i_code, int8_t *pi8_text )
{
	( void )p_handle;
	fprintf( stderr, "encoder error %d: %s\n", i_code, ( const char * )pi8_text );
}

static void on_result( void *p_handle, int32_t i_result_type, y262_result_t *ps_result )
{
	FILE *f_out = ( FILE * )p_handle;
	if( i_result_type == Y262_RESULT_BITSTREAM )
	{
		fwrite( ps_result->bitstream_unit.pui8_unit, 1, ( size_t )ps_result->bitstream_unit.i_unit_length, f_out );
	}
}

static void make_frame( int32_t i_idx, uint8_t *pui8_y, uint8_t *pui8_cb, uint8_t *pui8_cr )
{
	int32_t x, y;
	for( y = 0; y < EX_H; y++ )
	{
		for( x = 0; x < EX_W; x++ )
		{
			pui8_y[ y * EX_W + x ] = ( uint8_t )( ( x * 3 + y * 2 + i_idx * 5 ) & 0xff );
		}
	}
	for( y = 0; y < EX_H / 2; y++ )
	{
		for( x = 0; x < EX_W / 2; x++ )
		{
			pui8_cb[ y * ( EX_W / 2 ) + x ] = ( uint8_t )( 60 + ( ( x * 9 + i_idx * 3 ) & 0x7f ) );
			pui8_cr[ y * ( EX_W / 2 ) + x ] = ( uint8_t )( 60 + ( ( y * 11 + i_idx * 5 ) & 0x7f ) );
		}
	}
}

static void make_matrix( uint8_t *pui8_matrix, int32_t i_base, int32_t i_step )
{
	int32_t i;
	for( i = 0; i < 64; i++ )
	{
		pui8_matrix[ i ] = ( uint8_t )( i_base + ( ( i * i_step ) % 60 ) );
	}
	/* both intra matrices must have 8 as their first element */
	pui8_matrix[ 0 ] = 8;
}

int main( void )
{
	y262_configuration_t s_config;
	y262_input_picture_t s_picture;
	void *p_encoder;
	FILE *f_out;
	uint8_t y[ EX_W * EX_H ], cb[ EX_W * EX_H / 4 ], cr[ EX_W * EX_H / 4 ];
	int32_t f, i;

	f_out = fopen( "picture_matrices_example.m2v", "wb" );
	if( !f_out )
	{
		return 1;
	}

	p_encoder = y262_create( &s_config );
	s_config.i_coded_width = EX_W;
	s_config.i_display_width = EX_W;
	s_config.i_coded_height = EX_H;
	s_config.i_display_height = EX_H;
	s_config.i_coded_chroma_format = Y262_CHROMA_FORMAT_420;
	s_config.i_multithreading = 0;
	s_config.i_num_threads = 1;
	s_config.i_lookahead_pictures = 10;
	s_config.i_bframes = 2;
	s_config.i_keyframe_ref_distance = 1;
	s_config.i_rcmode = BITRATE_CONTROL_CQ;
	s_config.i_quantizer = 8;
	s_config.b_variance_aq = 0;
	s_config.b_picture_quant_matrices = 1; /* enable per picture matrix updates */
	s_config.pf_error_callback = on_error;
	s_config.pf_result_callback = on_result;
	s_config.p_cb_handle = f_out;

	if( y262_initialize( p_encoder, &s_config ) != Y262_INIT_SUCCESS )
	{
		fprintf( stderr, "failed to initialize encoder\n" );
		fclose( f_out );
		return 1;
	}

	for( f = 0; f < EX_FRAMES; f++ )
	{
		int32_t rc;

		make_frame( f, y, cb, cr );
		memset( &s_picture, 0, sizeof( s_picture ) );
		s_picture.pui8_luma = y;
		s_picture.pui8_cb = cb;
		s_picture.pui8_cr = cr;
		s_picture.i_frame_structure = Y262_INPUT_PICTURE_FRAME_PROGRESSIVE;

		/*
		the update mask selects which of the four matrices are replaced for this
		picture. only the selected arrays are read by the encoder. a picture
		inherits the full four matrix state of the previous successfully pushed
		picture, so an application only has to send what actually changes.
		*/
		switch( f % 4 )
		{
			case 0:
				/* all four matrices, chroma deliberately different from luma */
				s_picture.i_quant_matrix_update_mask = 0xf;
				make_matrix( s_picture.rgui8_quant_matrices[ MAT_LUMA_INTRA ], 8, 2 );
				make_matrix( s_picture.rgui8_quant_matrices[ MAT_LUMA_INTER ], 12, 1 );
				make_matrix( s_picture.rgui8_quant_matrices[ MAT_CHROMA_INTRA ], 10, 3 );
				make_matrix( s_picture.rgui8_quant_matrices[ MAT_CHROMA_INTER ], 14, 5 );
				break;
			case 1:
				/* only the chroma weights, leave luma untouched */
				s_picture.i_quant_matrix_update_mask = 0x4 | 0x8;
				make_matrix( s_picture.rgui8_quant_matrices[ MAT_CHROMA_INTRA ], 8, 4 );
				make_matrix( s_picture.rgui8_quant_matrices[ MAT_CHROMA_INTER ], 16, 2 );
				break;
			case 2:
				/* only the luma intra weight */
				s_picture.i_quant_matrix_update_mask = 0x1;
				make_matrix( s_picture.rgui8_quant_matrices[ MAT_LUMA_INTRA ], 8, 6 );
				break;
			default:
				/* no change at all: the picture simply inherits everything */
				s_picture.i_quant_matrix_update_mask = 0;
				break;
		}

		rc = y262_push_input_picture( p_encoder, &s_picture, f );
		if( rc == Y262_PUSH_INPUT_ERR_ARG )
		{
			/* invalid mask or matrix: nothing was consumed, fix and retry */
			fprintf( stderr, "picture %d rejected, retrying without update\n", f );
			s_picture.i_quant_matrix_update_mask = 0;
			rc = y262_push_input_picture( p_encoder, &s_picture, f );
		}
		if( rc != Y262_PUSH_INPUT_CONTINUE )
		{
			fprintf( stderr, "push failed at picture %d\n", f );
			break;
		}

		/* the encoder copies everything, so the caller may overwrite the input */
		memset( y, 0, sizeof( y ) );
		memset( cb, 0, sizeof( cb ) );
		memset( cr, 0, sizeof( cr ) );
	}

	for( i = 0; i < 1024; i++ )
	{
		if( y262_push_input_picture( p_encoder, NULL, EX_FRAMES ) == Y262_PUSH_INPUT_FLUSHED )
		{
			break;
		}
	}

	y262_deinitialize( p_encoder );
	fclose( f_out );
	puts( "wrote picture_matrices_example.m2v" );
	return 0;
}
