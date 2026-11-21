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

#include "y262.h"

/*
per picture quantization matrix support.

index layout used everywhere in this file:
	0 luma intra
	1 luma inter
	2 chroma intra
	3 chroma inter

all matrices are stored in natural 8x8 frequency coordinates, row major.
*/

/*
inert observation point used by the picture matrix tests. it stays NULL during
normal encoding; a test registers a callback to inspect the real production
quantized coefficients and reconstructed residuals.
*/
y262_block_observer_f y262_test_block_observer = NULL;

void y262_test_observe_block( y262_t *ps_y262, int32_t i_mb_addr, int32_t i_plane_idx, int32_t i_blk_idx, bool_t b_intra,
	int32_t i_quantizer, const int16_t *pi16_levels, const int16_t *pi16_dequant, const int16_t *pi16_recon )
{
	y262_block_observation_t s_obs;

	if( !y262_test_block_observer || !ps_y262->b_picture_quant_matrices )
	{
		return;
	}
	memset( &s_obs, 0, sizeof( s_obs ) );
	s_obs.i_pon = ps_y262->ps_input_picture->i_pon;
	s_obs.i_don = ps_y262->ps_input_picture->i_don;
	s_obs.i_picture_type = ps_y262->ps_input_picture->i_frame_type;
	s_obs.i_mb_addr = i_mb_addr;
	s_obs.i_plane_idx = i_plane_idx;
	s_obs.i_blk_idx = i_blk_idx;
	s_obs.b_intra = b_intra;
	s_obs.i_quantizer = i_quantizer;
	s_obs.i_quality_for_speed = ps_y262->i_quality_for_speed;
	memcpy( s_obs.rgui8_luma_intra_matrix, ps_y262->rgui8_current_quant_matrices[ 0 ], 64 );
	memcpy( s_obs.rgui8_luma_inter_matrix, ps_y262->rgui8_current_quant_matrices[ 1 ], 64 );
	memcpy( s_obs.rgui8_chroma_intra_matrix, ps_y262->rgui8_current_quant_matrices[ 2 ], 64 );
	memcpy( s_obs.rgui8_chroma_inter_matrix, ps_y262->rgui8_current_quant_matrices[ 3 ], 64 );
	s_obs.pi16_levels = pi16_levels;
	s_obs.pi16_dequant = pi16_dequant;
	s_obs.pi16_recon = pi16_recon;
	y262_test_block_observer( &s_obs );
}

void y262_picture_matrices_init( y262_t *ps_y262 )
{
	int32_t i_idx;

	for( i_idx = 0; i_idx < 64; i_idx++ )
	{
		ps_y262->rgui8_picture_quant_matrices[ 0 ][ i_idx ] = ps_y262->rgui8_intra_quantiser_matrix[ i_idx ];
		ps_y262->rgui8_picture_quant_matrices[ 1 ][ i_idx ] = ps_y262->rgui8_non_intra_quantiser_matrix[ i_idx ];
		ps_y262->rgui8_picture_quant_matrices[ 2 ][ i_idx ] = ps_y262->rgui8_intra_quantiser_matrix[ i_idx ];
		ps_y262->rgui8_picture_quant_matrices[ 3 ][ i_idx ] = ps_y262->rgui8_non_intra_quantiser_matrix[ i_idx ];

		/* decode side state at the start of the stream: the mpeg2 defaults */
		ps_y262->rgui8_emitted_quant_matrices[ 0 ][ i_idx ] = rgui8_y262_default_intra_matrix[ i_idx ];
		ps_y262->rgui8_emitted_quant_matrices[ 1 ][ i_idx ] = rgui8_y262_default_non_intra_matrix[ i_idx ];
		ps_y262->rgui8_emitted_quant_matrices[ 2 ][ i_idx ] = rgui8_y262_default_intra_matrix[ i_idx ];
		ps_y262->rgui8_emitted_quant_matrices[ 3 ][ i_idx ] = rgui8_y262_default_non_intra_matrix[ i_idx ];

		ps_y262->rgui8_current_quant_matrices[ 0 ][ i_idx ] = ps_y262->rgui8_picture_quant_matrices[ 0 ][ i_idx ];
		ps_y262->rgui8_current_quant_matrices[ 1 ][ i_idx ] = ps_y262->rgui8_picture_quant_matrices[ 1 ][ i_idx ];
		ps_y262->rgui8_current_quant_matrices[ 2 ][ i_idx ] = ps_y262->rgui8_picture_quant_matrices[ 2 ][ i_idx ];
		ps_y262->rgui8_current_quant_matrices[ 3 ][ i_idx ] = ps_y262->rgui8_picture_quant_matrices[ 3 ][ i_idx ];
	}
}

bool_t y262_picture_matrices_config_valid( y262_configuration_t *ps_config )
{
	if( ps_config->b_mpeg1 )
	{
		return FALSE;
	}
	if( ps_config->b_interlaced )
	{
		return FALSE;
	}
	if( ps_config->i_coded_chroma_format != Y262_CHROMA_FORMAT_420 )
	{
		return FALSE;
	}
	if( ps_config->i_rcmode != BITRATE_CONTROL_CQ )
	{
		return FALSE;
	}
	return TRUE;
}

static bool_t y262_quant_matrix_valid( int32_t i_idx, uint8_t *pui8_matrix )
{
	int32_t i_coeff;

	for( i_coeff = 0; i_coeff < 64; i_coeff++ )
	{
		if( pui8_matrix[ i_coeff ] < 1 || pui8_matrix[ i_coeff ] > 255 )
		{
			return FALSE;
		}
	}
	if( ( i_idx == 0 || i_idx == 2 ) && pui8_matrix[ 0 ] != 8 )
	{
		return FALSE;
	}
	return TRUE;
}

bool_t y262_picture_matrices_validate_update( uint32_t ui_mask, uint8_t rgui8_matrices[ 4 ][ 64 ] )
{
	int32_t i_idx;

	if( ui_mask & ~0xfu )
	{
		return FALSE;
	}
	for( i_idx = 0; i_idx < 4; i_idx++ )
	{
		if( ui_mask & ( 1u << i_idx ) )
		{
			if( !y262_quant_matrix_valid( i_idx, rgui8_matrices[ i_idx ] ) )
			{
				return FALSE;
			}
		}
	}
	return TRUE;
}

void y262_picture_matrices_apply_update( uint8_t rgui8_new[ 4 ][ 64 ], uint8_t rgui8_prev[ 4 ][ 64 ], uint32_t ui_mask, uint8_t rgui8_matrices[ 4 ][ 64 ] )
{
	int32_t i_idx;

	for( i_idx = 0; i_idx < 4; i_idx++ )
	{
		memcpy( rgui8_new[ i_idx ], rgui8_prev[ i_idx ], 64 * sizeof( uint8_t ) );
		if( ui_mask & ( 1u << i_idx ) )
		{
			memcpy( rgui8_new[ i_idx ], rgui8_matrices[ i_idx ], 64 * sizeof( uint8_t ) );
		}
	}
}

static void y262_build_intra_quantizer_tables( uint16_t rgui16_qmat[ 122 ][ 64 ], uint16_t rgui16_bias[ 122 ][ 64 ], uint16_t rgui16_trellis_bias[ 122 ][ 64 ], uint8_t *pui8_matrix )
{
	int32_t i_idx, i_qmat_idx;

	memset( rgui16_qmat, 0, sizeof( uint16_t ) * 122 * 64 );
	for( i_idx = 1; i_idx < 122; i_idx++ )
	{
		for( i_qmat_idx = 0; i_qmat_idx < 64; i_qmat_idx++ )
		{
			int32_t i_qm, i_qt, i_scale;
			i_qm = pui8_matrix[ i_qmat_idx ];
			i_qt = i_qm * i_idx;
			i_scale = MIN( ( 1 << 16 ) - 1, ( ( 1 << 20 ) + ( i_qt / 2 ) ) / i_qt );
			rgui16_qmat[ i_idx ][ i_qmat_idx ] = i_scale;
			if( i_scale > 0 )
			{
				rgui16_bias[ i_idx ][ i_qmat_idx ] = 0x6000 / i_scale;
				rgui16_trellis_bias[ i_idx ][ i_qmat_idx ] = 0x8000 / i_scale;
			}
			else
			{
				rgui16_bias[ i_idx ][ i_qmat_idx ] = 0;
				rgui16_trellis_bias[ i_idx ][ i_qmat_idx ] = 0;
			}
		}
	}
}

static void y262_build_inter_quantizer_tables( uint16_t rgui16_qmat[ 122 ][ 64 ], uint8_t *pui8_matrix )
{
	int32_t i_idx, i_qmat_idx;

	memset( rgui16_qmat, 0, sizeof( uint16_t ) * 122 * 64 );
	for( i_idx = 1; i_idx < 122; i_idx++ )
	{
		for( i_qmat_idx = 0; i_qmat_idx < 64; i_qmat_idx++ )
		{
			int32_t i_qm, i_qt, i_scale;
			i_qm = pui8_matrix[ i_qmat_idx ];
			i_qt = i_qm * i_idx;
			i_scale = MIN( ( 1 << 16 ) - 1, ( ( 1 << 20 ) + ( i_qt / 2 ) ) / i_qt );
			rgui16_qmat[ i_idx ][ i_qmat_idx ] = i_scale;
		}
	}
}

void y262_picture_matrices_build_tables( y262_t *ps_y262 )
{
	int32_t i_plane;

	for( i_plane = 0; i_plane < 2; i_plane++ )
	{
		int32_t i_intra_idx = i_plane == 0 ? 0 : 2;
		int32_t i_inter_idx = i_plane == 0 ? 1 : 3;

		y262_build_intra_quantizer_tables( ps_y262->rgui16_picture_intra_quantizer_matrices[ i_plane ],
			ps_y262->rgui16_picture_intra_quantizer_matrices_bias[ i_plane ],
			ps_y262->rgui16_picture_intra_quantizer_matrices_trellis_bias[ i_plane ],
			ps_y262->rgui8_current_quant_matrices[ i_intra_idx ] );
		y262_build_inter_quantizer_tables( ps_y262->rgui16_picture_inter_quantizer_matrices[ i_plane ],
			ps_y262->rgui8_current_quant_matrices[ i_inter_idx ] );
	}
}

/*
selectors used by the macroblock encoder. when per picture matrices are disabled
these return the legacy single matrix tables, so behaviour is unchanged.
*/
uint16_t *y262_get_intra_quantizer_matrix( y262_t *ps_y262, int32_t i_plane_idx, int32_t i_scaled_quantizer )
{
	if( ps_y262->b_picture_quant_matrices )
	{
		return ps_y262->rgui16_picture_intra_quantizer_matrices[ i_plane_idx == 0 ? 0 : 1 ][ i_scaled_quantizer ];
	}
	return ps_y262->rgui16_intra_quantizer_matrices[ i_scaled_quantizer ];
}

uint16_t *y262_get_intra_quantizer_matrix_bias( y262_t *ps_y262, int32_t i_plane_idx, int32_t i_scaled_quantizer )
{
	if( ps_y262->b_picture_quant_matrices )
	{
		return ps_y262->rgui16_picture_intra_quantizer_matrices_bias[ i_plane_idx == 0 ? 0 : 1 ][ i_scaled_quantizer ];
	}
	return ps_y262->rgui16_intra_quantizer_matrices_bias[ i_scaled_quantizer ];
}

uint16_t *y262_get_intra_quantizer_matrix_trellis_bias( y262_t *ps_y262, int32_t i_plane_idx, int32_t i_scaled_quantizer )
{
	if( ps_y262->b_picture_quant_matrices )
	{
		return ps_y262->rgui16_picture_intra_quantizer_matrices_trellis_bias[ i_plane_idx == 0 ? 0 : 1 ][ i_scaled_quantizer ];
	}
	return ps_y262->rgui16_intra_quantizer_matrices_trellis_bias[ i_scaled_quantizer ];
}

uint16_t *y262_get_inter_quantizer_matrix( y262_t *ps_y262, int32_t i_plane_idx, int32_t i_scaled_quantizer )
{
	if( ps_y262->b_picture_quant_matrices )
	{
		return ps_y262->rgui16_picture_inter_quantizer_matrices[ i_plane_idx == 0 ? 0 : 1 ][ i_scaled_quantizer ];
	}
	return ps_y262->rgui16_non_intra_quantizer_matrices[ i_scaled_quantizer ];
}

uint8_t *y262_get_intra_quantiser_matrix( y262_t *ps_y262, int32_t i_plane_idx )
{
	if( ps_y262->b_picture_quant_matrices )
	{
		return ps_y262->rgui8_current_quant_matrices[ i_plane_idx == 0 ? 0 : 2 ];
	}
	return ps_y262->rgui8_intra_quantiser_matrix;
}

uint8_t *y262_get_inter_quantiser_matrix( y262_t *ps_y262, int32_t i_plane_idx )
{
	if( ps_y262->b_picture_quant_matrices )
	{
		return ps_y262->rgui8_current_quant_matrices[ i_plane_idx == 0 ? 1 : 3 ];
	}
	return ps_y262->rgui8_non_intra_quantiser_matrix;
}

/*
update the decode side model for the matrices that a newly written sequence
header puts into effect. a sequence header resets the luma matrices to the
defaults unless loaded, and always reloads the chroma matrices from the
corresponding luma matrices.
*/
void y262_picture_matrices_note_sequence_header( y262_t *ps_y262, bool_t b_load_intra, uint8_t *pui8_intra, bool_t b_load_non_intra, uint8_t *pui8_non_intra )
{
	int32_t i_idx;

	for( i_idx = 0; i_idx < 64; i_idx++ )
	{
		ps_y262->rgui8_emitted_quant_matrices[ 0 ][ i_idx ] = b_load_intra ? pui8_intra[ i_idx ] : rgui8_y262_default_intra_matrix[ i_idx ];
		ps_y262->rgui8_emitted_quant_matrices[ 2 ][ i_idx ] = ps_y262->rgui8_emitted_quant_matrices[ 0 ][ i_idx ];
		ps_y262->rgui8_emitted_quant_matrices[ 1 ][ i_idx ] = b_load_non_intra ? pui8_non_intra[ i_idx ] : rgui8_y262_default_non_intra_matrix[ i_idx ];
		ps_y262->rgui8_emitted_quant_matrices[ 3 ][ i_idx ] = ps_y262->rgui8_emitted_quant_matrices[ 1 ][ i_idx ];
	}
}

int32_t y262_picture_matrices_extension_flags( y262_t *ps_y262 )
{
	int32_t i_flags, i_luma_intra_changed, i_luma_inter_changed;
	uint8_t *pui8_emitted, *pui8_current;

	pui8_emitted = &ps_y262->rgui8_emitted_quant_matrices[ 0 ][ 0 ];
	pui8_current = &ps_y262->rgui8_current_quant_matrices[ 0 ][ 0 ];

	i_flags = 0;

	/*
	per spec a luma matrix load also reloads the matching chroma matrix with the
	luma values, unless that chroma matrix is loaded explicitly in the same
	extension. account for that side effect when computing the minimal diff
	against the state the decoder already has in decode order, and then reload
	the chroma matrix explicitly if this picture needs one that differs.
	*/
	i_luma_intra_changed = memcmp( pui8_current + 0 * 64, pui8_emitted + 0 * 64, 64 * sizeof( uint8_t ) ) != 0;
	
	if( i_luma_intra_changed )
	{
		i_flags |= 1;
	}
	if( memcmp( pui8_current + 2 * 64, i_luma_intra_changed ? pui8_current + 0 * 64 : pui8_emitted + 2 * 64, 64 * sizeof( uint8_t ) ) != 0 )
	{
		i_flags |= 4;
	}

	i_luma_inter_changed = memcmp( pui8_current + 1 * 64, pui8_emitted + 1 * 64, 64 * sizeof( uint8_t ) ) != 0;
	if( i_luma_inter_changed )
	{
		i_flags |= 2;
	}
	if( memcmp( pui8_current + 3 * 64, i_luma_inter_changed ? pui8_current + 1 * 64 : pui8_emitted + 3 * 64, 64 * sizeof( uint8_t ) ) != 0 )
	{
		i_flags |= 8;
	}

	return i_flags;
}

void y262_picture_matrices_note_extension_written( y262_t *ps_y262 )
{
	int32_t i_flags;

	i_flags = ps_y262->i_quant_matrix_extension_flags;

	if( i_flags & 1 )
	{
		memcpy( ps_y262->rgui8_emitted_quant_matrices[ 0 ], ps_y262->rgui8_current_quant_matrices[ 0 ], 64 );
		memcpy( ps_y262->rgui8_emitted_quant_matrices[ 2 ], ps_y262->rgui8_current_quant_matrices[ 0 ], 64 );
	}
	if( i_flags & 2 )
	{
		memcpy( ps_y262->rgui8_emitted_quant_matrices[ 1 ], ps_y262->rgui8_current_quant_matrices[ 1 ], 64 );
		memcpy( ps_y262->rgui8_emitted_quant_matrices[ 3 ], ps_y262->rgui8_current_quant_matrices[ 1 ], 64 );
	}
	if( i_flags & 4 )
	{
		memcpy( ps_y262->rgui8_emitted_quant_matrices[ 2 ], ps_y262->rgui8_current_quant_matrices[ 2 ], 64 );
	}
	if( i_flags & 8 )
	{
		memcpy( ps_y262->rgui8_emitted_quant_matrices[ 3 ], ps_y262->rgui8_current_quant_matrices[ 3 ], 64 );
	}
}
