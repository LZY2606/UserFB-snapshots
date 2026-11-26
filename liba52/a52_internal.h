/*
 * a52_internal.h
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

typedef struct {
    uint8_t bai;		/* fine SNR offset, fast gain */
    uint8_t deltbae;		/* delta bit allocation exists */
    int8_t deltba[50];		/* per-band delta bit allocation */
} ba_t;

typedef struct {
    uint8_t exp[256];		/* decoded channel exponents */
    int8_t bap[256];		/* derived channel bit allocation */
} expbap_t;

struct a52_state_s {
    uint8_t fscod;		/* sample rate */
    uint8_t halfrate;		/* halfrate factor */
    uint8_t acmod;		/* coded channels */
    uint8_t lfeon;		/* coded lfe channel */
    sample_t clev;		/* centre channel mix level */
    sample_t slev;		/* surround channels mix level */

    int output;			/* type of output */
    sample_t level;		/* output level */
    sample_t bias;		/* output bias */

    int dynrnge;		/* apply dynamic range */
    sample_t dynrng;		/* dynamic range */
    void * dynrngdata;		/* dynamic range callback funtion and data */
    sample_t (* dynrngcall) (sample_t range, void * dynrngdata);

    uint8_t chincpl;		/* channel coupled */
    uint8_t phsflginu;		/* phase flags in use (stereo only) */
    uint8_t cplstrtmant;	/* coupling channel start mantissa */
    uint8_t cplendmant;		/* coupling channel end mantissa */
    uint32_t cplbndstrc;	/* coupling band structure */
    sample_t cplco[5][18];	/* coupling coordinates */

    /* derived information */
    uint8_t cplstrtbnd;		/* coupling start band (for bit allocation) */
    uint8_t ncplbnd;		/* number of coupling bands */

    uint8_t rematflg;		/* stereo rematrixing */

    uint8_t endmant[5];		/* channel end mantissa */

    uint16_t bai;		/* bit allocation information */

    uint32_t * buffer_start;
    uint16_t lfsr_state;	/* dither state */
    uint32_t bits_left;
    uint32_t current_word;

    uint8_t csnroffst;		/* coarse SNR offset */
    ba_t cplba;			/* coupling bit allocation parameters */
    ba_t ba[5];			/* channel bit allocation parameters */
    ba_t lfeba;			/* lfe bit allocation parameters */

    uint8_t cplfleak;		/* coupling fast leak init */
    uint8_t cplsleak;		/* coupling slow leak init */

    expbap_t cpl_expbap;
    expbap_t fbw_expbap[5];
    expbap_t lfe_expbap;

    sample_t * samples;
    int downmixed;
    uint8_t frame_seen;		/* a52_frame has successfully run once */

    /* matrix monitoring mode state */
    uint8_t matrix_mode;	/* a52_set_matrix has enabled matrix mode */
    uint8_t matrix_rows;	/* rows of the matrix currently in use */
    uint8_t matrix_new_rows;	/* rows of the queued matrix */
    uint8_t matrix_pending;	/* a new matrix is queued for a52_frame */
    uint8_t matrix_have_prev;	/* previous-block history is valid */
    sample_t matrix_cur[2][6];	/* matrix in use for the current frame */
    sample_t matrix_new[2][6];	/* queued matrix, applied at next frame */
    sample_t matrix_used[2][6];/* matrix used by the previous block */

    /*
     * Previous block's already-scaled source spectra and block switch
     * flags. They let the overlap delay lines be rebuilt when the matrix
     * or the block grouping changes; the previous level/DRC scaling is
     * baked into the spectra and is never applied a second time.
     * fbw covers the five 3F2R full-bandwidth channels, lfe the LFE.
     */
    sample_t matrix_prev_spec[5][256];
    sample_t matrix_prev_lfe[256];
    uint8_t matrix_prev_blksw[5];
    uint8_t matrix_prev_lfeon;

    /*
     * Persistent overlap-add delay lines, one per output row and transform
     * family. The LFE is always a long block and is folded into the long
     * projection. Total per-instance storage is fixed at channel/block size
     * and does not grow with the decoded duration.
     */
    sample_t matrix_delay_long[2][256];
    sample_t matrix_delay_short[2][256];

#ifdef A52_MATRIX_TEST
    unsigned test_imdct_256;
    unsigned test_imdct_512;
    int test_inject_armed;	/* one-shot spectrum injection */
    sample_t test_inject_spec[6][256];
    uint8_t test_inject_blksw[5];
    int test_inject_lfeon;
#endif
};

/* matrix mode support (matrix.c) */
int a52_set_matrix (a52_state_t * state, const sample_t * matrix,
		    unsigned rows);
void a52_matrix_commit (a52_state_t * state);
int a52_matrix_block (a52_state_t * state, uint8_t blksw[5]);

#ifdef A52_MATRIX_TEST
/* test-only observation hooks; absent from ordinary builds */
unsigned a52_test_imdct_calls (a52_state_t * state);
unsigned a52_test_imdct_256_calls (a52_state_t * state);
unsigned a52_test_imdct_512_calls (a52_state_t * state);
const sample_t * a52_test_prev_spectrum (a52_state_t * state, int channel);
const sample_t * a52_test_lfe_spectrum (a52_state_t * state);
const uint8_t * a52_test_prev_blksw (a52_state_t * state);
void a52_test_inject (a52_state_t * state, const sample_t * spec,
		      const int * blksw, int lfeon);
#endif

#define LEVEL_PLUS6DB 2.0
#define LEVEL_PLUS3DB 1.4142135623730951
#define LEVEL_3DB 0.7071067811865476
#define LEVEL_45DB 0.5946035575013605
#define LEVEL_6DB 0.5

#define EXP_REUSE (0)
#define EXP_D15   (1)
#define EXP_D25   (2)
#define EXP_D45   (3)

#define DELTA_BIT_REUSE (0)
#define DELTA_BIT_NEW (1)
#define DELTA_BIT_NONE (2)
#define DELTA_BIT_RESERVED (3)

void a52_bit_allocate (a52_state_t * state, ba_t * ba, int bndstart,
		       int start, int end, int fastleak, int slowleak,
		       expbap_t * expbap);

int a52_downmix_init (int input, int flags, sample_t * level,
		      sample_t clev, sample_t slev);
int a52_downmix_coeff (sample_t * coeff, int acmod, int output, sample_t level,
		       sample_t clev, sample_t slev);
void a52_downmix (sample_t * samples, int acmod, int output, sample_t bias,
		  sample_t clev, sample_t slev);
void a52_upmix (sample_t * samples, int acmod, int output);

void a52_imdct_init (uint32_t mm_accel);
void a52_imdct_256 (sample_t * data, sample_t * delay, sample_t bias);
void a52_imdct_512 (sample_t * data, sample_t * delay, sample_t bias);
