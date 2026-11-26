/*
 * a52.h
 * Copyright (C) 2000-2002 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of a52dec, a free ATSC A-52 stream decoder.
 * See http://liba52.sourceforge.net/ for updates.
 *
 * a52dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * a52dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef A52_H
#define A52_H

#ifndef LIBA52_DOUBLE
typedef float sample_t;
#else
typedef double sample_t;
#endif

typedef struct a52_state_s a52_state_t;

#define A52_CHANNEL 0
#define A52_MONO 1
#define A52_STEREO 2
#define A52_3F 3
#define A52_2F1R 4
#define A52_3F1R 5
#define A52_2F2R 6
#define A52_3F2R 7
#define A52_CHANNEL1 8
#define A52_CHANNEL2 9
#define A52_DOLBY 10
#define A52_CHANNEL_MASK 15

#define A52_LFE 16
#define A52_ADJUST_LEVEL 32

a52_state_t * a52_init (uint32_t mm_accel);
sample_t * a52_samples (a52_state_t * state);
int a52_syncinfo (uint8_t * buf, int * flags,
		  int * sample_rate, int * bit_rate);
int a52_frame (a52_state_t * state, uint8_t * buf, int * flags,
	       sample_t * level, sample_t bias);
void a52_dynrng (a52_state_t * state,
		 sample_t (* call) (sample_t, void *), void * data);
int a52_block (a52_state_t * state);
void a52_free (a52_state_t * state);

/*
 * Matrix monitoring mode.
 *
 * Puts the decoder in matrix mode, where the 3F2R full-bandwidth source
 * channels (L, C, R, Ls, Rs) plus the optional LFE are projected onto one
 * or two monitoring outputs through a caller supplied matrix. The matrix
 * is stored row-major, each row having exactly 6 coefficients in the fixed
 * order L, C, R, Ls, Rs, LFE. Coefficients may be negative; they must be
 * finite and their absolute value must not exceed 2. A missing LFE source
 * channel is treated as if its (sixth) column was zero.
 *
 * rows must be 1 or 2. On success, the matrix is copied internally and 0
 * is returned; later changes to the caller array do not affect the
 * instance. On invalid arguments, 1 is returned and neither the queued
 * matrix nor the one currently in use is modified.
 *
 * Matrix mode can only be enabled before the first frame is decoded on
 * the instance; enabling it after a frame has been decoded returns 1.
 * Once enabled, the instance stays in matrix mode until it is destroyed.
 * A matrix update is queued and takes effect at the next a52_frame();
 * all six blocks of a frame use the same matrix version. In matrix mode,
 * a52_frame() reports A52_MONO (1 row) or A52_STEREO (2 rows) and the
 * output samples are laid out as rows planes of 256 samples each.
 */
int a52_set_matrix (a52_state_t * state, const sample_t * matrix,
		    unsigned rows);

#endif /* A52_H */
