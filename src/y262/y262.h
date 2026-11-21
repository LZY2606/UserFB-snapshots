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

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#define ALIGNED(x) __declspec(align(x))
#else
#define ALIGNED(x) __attribute__((aligned(x)))
#endif


#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <memory.h>
#include <assert.h>
#include <math.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "y262api.h"

#include "types.h"
#include "tables.h"
#include "bitstream.h"
#include "aboveslicelevel.h"
#include "lookahead.h"
#include "transform.h"
#include "pixelop.h"
#include "me.h"
#include "ratectrl.h"
#include "threads.h"

#ifdef ASSEMBLY_X86
#include "transform_x86.h"
#include "pixelop_x86.h"
#endif

#ifdef ASSEMBLY_ARM64
#include "transform_arm64.h"
#include "pixelop_arm64.h"
#endif

void y262_init_motion_compensation( y262_t *ps_y262 );
void y262_error( y262_t *ps_y262, int32_t i_error_code, int8_t* pi8_format, ... );
void y262_encode_picture( y262_t *ps_y262, y262_picture_t *ps_picture, int32_t i_picture_type, int32_t i_pon );


/*
inert observation point used by the picture matrix tests to look at the real
production quantized coefficients and reconstructed residuals. it does nothing
unless a test registers a callback in y262_test_block_observer.
*/
typedef struct
{
	int32_t i_pon;
	int32_t i_don;
	int32_t i_picture_type;
	int32_t i_mb_addr;
	int32_t i_plane_idx;
	int32_t i_blk_idx;
	int32_t b_intra;
	int32_t i_quantizer;
	int32_t i_quality_for_speed;
	uint8_t rgui8_luma_intra_matrix[ 64 ];
	uint8_t rgui8_luma_inter_matrix[ 64 ];
	uint8_t rgui8_chroma_intra_matrix[ 64 ];
	uint8_t rgui8_chroma_inter_matrix[ 64 ];
	const int16_t *pi16_levels; /* quantized coefficients, natural order, stride 8 */
	const int16_t *pi16_dequant; /* production inverse quantized coefficients, natural order, stride 8 */
	const int16_t *pi16_recon; /* reconstructed residual after idct, natural order, stride 8 */
} y262_block_observation_t;
typedef void ( *y262_block_observer_f ) ( const y262_block_observation_t *ps_obs );
extern y262_block_observer_f y262_test_block_observer;
void y262_test_observe_block( y262_t *ps_y262, int32_t i_mb_addr, int32_t i_plane_idx, int32_t i_blk_idx, bool_t b_intra,
	int32_t i_quantizer, const int16_t *pi16_levels, const int16_t *pi16_dequant, const int16_t *pi16_recon );

/* per picture quantization matrix helpers */
void y262_picture_matrices_init( y262_t *ps_y262 );
bool_t y262_picture_matrices_config_valid( y262_configuration_t *ps_config );
bool_t y262_picture_matrices_validate_update( uint32_t ui_mask, uint8_t rgui8_matrices[ 4 ][ 64 ] );
void y262_picture_matrices_apply_update( uint8_t rgui8_new[ 4 ][ 64 ], uint8_t rgui8_prev[ 4 ][ 64 ], uint32_t ui_mask, uint8_t rgui8_matrices[ 4 ][ 64 ] );
void y262_picture_matrices_build_tables( y262_t *ps_y262 );
void y262_picture_matrices_note_sequence_header( y262_t *ps_y262, bool_t b_load_intra, uint8_t *pui8_intra, bool_t b_load_non_intra, uint8_t *pui8_non_intra );
int32_t y262_picture_matrices_extension_flags( y262_t *ps_y262 );
void y262_picture_matrices_note_extension_written( y262_t *ps_y262 );

uint16_t *y262_get_intra_quantizer_matrix( y262_t *ps_y262, int32_t i_plane_idx, int32_t i_scaled_quantizer );
uint16_t *y262_get_intra_quantizer_matrix_bias( y262_t *ps_y262, int32_t i_plane_idx, int32_t i_scaled_quantizer );
uint16_t *y262_get_intra_quantizer_matrix_trellis_bias( y262_t *ps_y262, int32_t i_plane_idx, int32_t i_scaled_quantizer );
uint16_t *y262_get_inter_quantizer_matrix( y262_t *ps_y262, int32_t i_plane_idx, int32_t i_scaled_quantizer );
uint8_t *y262_get_intra_quantiser_matrix( y262_t *ps_y262, int32_t i_plane_idx );
uint8_t *y262_get_inter_quantiser_matrix( y262_t *ps_y262, int32_t i_plane_idx );



#define MIN( x, y ) ( ( x ) < ( y ) ? ( x ) : ( y ) )
#define MAX( x, y ) ( ( x ) > ( y ) ? ( x ) : ( y ) )

