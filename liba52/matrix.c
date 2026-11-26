/*
 * matrix.c
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

/*
 * Matrix monitoring mode.
 *
 * The five 3F2R full-bandwidth source spectra (and the optional LFE) are
 * projected in the frequency domain with the caller's matrix before any
 * IMDCT. Full-bandwidth channels sharing the same block switch type are
 * projected together, so with a stable matrix and all long blocks each
 * output row only needs one long transform per block; the LFE always uses
 * the long transform and is folded into the long projection.
 *
 * Overlap-add history lives in two delay lines per output row (long and
 * short). The previous block's already level/DRC-scaled source spectra are
 * retained, so when the matrix or the block grouping changes the exact
 * delay lines the current transforms must consume are rebuilt by running
 * those stored spectra through the same production IMDCTs with zero delay.
 * History is never cleared and the previous block's gain is never reapplied.
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "a52.h"
#include "a52_internal.h"

#define MB_FBW 5			/* full-bandwidth channels in 3F2R */
#define MB_COLS 6			/* L, C, R, Ls, Rs, LFE */
#define MB_SRC_OFFSET 1536		/* 6 * 256 source planes in samples */
#define MB_LFE_OFFSET 1280		/* LFE plane inside the source area */

static void mb_imdct_512 (a52_state_t * state, sample_t * data,
			  sample_t * delay, sample_t bias)
{
    a52_imdct_512 (data, delay, bias);
#ifdef A52_MATRIX_TEST
    state->test_imdct_512++;
#else
    (void) state;
#endif
}

static void mb_imdct_256 (a52_state_t * state, sample_t * data,
			  sample_t * delay, sample_t bias)
{
    a52_imdct_256 (data, delay, bias);
#ifdef A52_MATRIX_TEST
    state->test_imdct_256++;
#else
    (void) state;
#endif
}

int a52_set_matrix (a52_state_t * state, const sample_t * matrix,
		    unsigned rows)
{
    unsigned i;

    if (state == NULL || matrix == NULL)
	return 1;
    if (rows != 1 && rows != 2)
	return 1;

    for (i = 0; i < rows * MB_COLS; i++)
	if (!isfinite (matrix[i]) || fabs (matrix[i]) > 2.0)
	    return 1;

    if (!state->matrix_mode) {
	/* matrix mode can only be enabled before the first frame */
	if (state->frame_seen)
	    return 1;

	state->matrix_mode = 1;
	state->matrix_rows = (uint8_t) rows;
	state->matrix_new_rows = (uint8_t) rows;
	memcpy (state->matrix_cur, matrix,
		rows * MB_COLS * sizeof (sample_t));
	memcpy (state->matrix_new, matrix,
		rows * MB_COLS * sizeof (sample_t));
	state->matrix_pending = 0;
	state->matrix_have_prev = 0;
	return 0;
    }

    /* already in matrix mode: only queue the update */
    memcpy (state->matrix_new, matrix,
	    rows * MB_COLS * sizeof (sample_t));
    state->matrix_new_rows = (uint8_t) rows;
    state->matrix_pending = 1;
    return 0;
}

void a52_matrix_commit (a52_state_t * state)
{
    unsigned rows, old_rows, r;

    if (!state->matrix_mode || !state->matrix_pending)
	return;

    old_rows = state->matrix_rows;
    rows = state->matrix_new_rows;
    memcpy (state->matrix_cur, state->matrix_new,
	    rows * MB_COLS * sizeof (sample_t));
    state->matrix_rows = (uint8_t) rows;
    state->matrix_pending = 0;
    /* a newly appearing output row starts with zero overlap history */
    for (r = old_rows; r < rows; r++) {
	memset (state->matrix_delay_long[r], 0,
		256 * sizeof (sample_t));
	memset (state->matrix_delay_short[r], 0,
		256 * sizeof (sample_t));
    }
}

static int matrix_equal (const sample_t a[2][6], const sample_t b[2][6],
			 unsigned rows)
{
    return memcmp (a, b, rows * MB_COLS * sizeof (sample_t)) == 0;
}

/*
 * Rebuild the overlap delay lines from the previous block's source
 * spectra. Each source spectrum is run once through the transform matching
 * its previous block type with a zero delay, producing the contribution
 * the current block's transforms must overlap-add. The contributions are
 * weighted with the current matrix and distributed into the long/short
 * lines according to the current block's grouping.
 */
static void matrix_rebuild (a52_state_t * state, const uint8_t * cur_blksw)
{
    unsigned rows = state->matrix_rows;
    unsigned r, c, n;
    sample_t scratch[256];
    sample_t ch_delay[MB_FBW][256];
    sample_t lfe_delay[256];

    memset (ch_delay, 0, sizeof (ch_delay));
    memset (lfe_delay, 0, sizeof (lfe_delay));

    for (c = 0; c < MB_FBW; c++) {
	memcpy (scratch, state->matrix_prev_spec[c],
		256 * sizeof (sample_t));
	if (state->matrix_prev_blksw[c])
	    mb_imdct_256 (state, scratch, ch_delay[c], 0);
	else
	    mb_imdct_512 (state, scratch, ch_delay[c], 0);
    }

    if (state->matrix_prev_lfeon) {
	memcpy (scratch, state->matrix_prev_lfe,
		256 * sizeof (sample_t));
	mb_imdct_512 (state, scratch, lfe_delay, 0);
    }

    for (r = 0; r < rows; r++) {
	memset (state->matrix_delay_long[r], 0,
		256 * sizeof (sample_t));
	memset (state->matrix_delay_short[r], 0,
		256 * sizeof (sample_t));

	for (c = 0; c < MB_FBW; c++) {
	    sample_t coeff = state->matrix_cur[r][c];
	    sample_t * dst = cur_blksw[c] ?
		state->matrix_delay_short[r] :
		state->matrix_delay_long[r];

	    if (coeff == 0)
		continue;
	    for (n = 0; n < 256; n++)
		dst[n] += coeff * ch_delay[c][n];
	}

	if (state->matrix_prev_lfeon) {
	    sample_t coeff = state->matrix_cur[r][5];

	    for (n = 0; n < 256; n++)
		state->matrix_delay_long[r][n] += coeff * lfe_delay[n];
	}
    }
}

int a52_matrix_block (a52_state_t * state, uint8_t blksw[MB_FBW])
{
    sample_t * src = state->samples + MB_SRC_OFFSET;
    sample_t * out = state->samples;
    unsigned rows = state->matrix_rows;
    unsigned r, c, n;
    int lfeon = state->lfeon;
    int matrix_changed;
    int grouping_changed;
    sample_t scratch[256];

#ifdef A52_MATRIX_TEST
    if (state->test_inject_armed) {
	memcpy (src, state->test_inject_spec,
		(MB_FBW + 1) * 256 * sizeof (sample_t));
	for (c = 0; c < MB_FBW; c++)
	    blksw[c] = state->test_inject_blksw[c];
	lfeon = state->lfeon = state->test_inject_lfeon;
	state->test_inject_armed = 0;
    }
#endif

    matrix_changed = state->matrix_have_prev &&
	!matrix_equal (state->matrix_cur, state->matrix_used, rows);
    grouping_changed = 0;
    if (state->matrix_have_prev) {
	for (c = 0; c < MB_FBW; c++)
	    if (blksw[c] != state->matrix_prev_blksw[c])
		grouping_changed = 1;
	if (lfeon != state->matrix_prev_lfeon)
	    grouping_changed = 1;
    }

    if (state->matrix_have_prev && (matrix_changed || grouping_changed))
	matrix_rebuild (state, blksw);

    for (r = 0; r < rows; r++) {
	sample_t * plane = out + 256 * r;
	int any_long = lfeon;
	int any_short = 0;

	for (c = 0; c < MB_FBW; c++) {
	    if (blksw[c])
		any_short = 1;
	    else
		any_long = 1;
	}

	memset (plane, 0, 256 * sizeof (sample_t));

	if (any_long) {
	    for (n = 0; n < 256; n++) {
		sample_t acc = 0;

		for (c = 0; c < MB_FBW; c++)
		    if (!blksw[c])
			acc += state->matrix_cur[r][c] * src[256 * c + n];
		if (lfeon)
		    acc += state->matrix_cur[r][5] *
			src[MB_LFE_OFFSET + n];
		scratch[n] = acc;
	    }
	    mb_imdct_512 (state, scratch,
			  state->matrix_delay_long[r], 0);
	    for (n = 0; n < 256; n++)
		plane[n] += scratch[n];
	}

	if (any_short) {
	    for (n = 0; n < 256; n++) {
		sample_t acc = 0;

		for (c = 0; c < MB_FBW; c++)
		    if (blksw[c])
			acc += state->matrix_cur[r][c] * src[256 * c + n];
		scratch[n] = acc;
	    }
	    mb_imdct_256 (state, scratch,
			  state->matrix_delay_short[r], 0);
	    for (n = 0; n < 256; n++)
		plane[n] += scratch[n];
	}

	/* bias is added exactly once per final output sample */
	for (n = 0; n < 256; n++)
		plane[n] += state->bias;
    }

    /* retain the previous block's already-scaled source spectra */
    for (c = 0; c < MB_FBW; c++)
	memcpy (state->matrix_prev_spec[c], src + 256 * c,
		256 * sizeof (sample_t));
    memcpy (state->matrix_prev_lfe, src + MB_LFE_OFFSET,
	    256 * sizeof (sample_t));
    memcpy (state->matrix_prev_blksw, blksw, MB_FBW);
    state->matrix_prev_lfeon = (uint8_t) lfeon;
    memcpy (state->matrix_used, state->matrix_cur,
	    rows * MB_COLS * sizeof (sample_t));
    state->matrix_have_prev = 1;

    return 0;
}

#ifdef A52_MATRIX_TEST
void a52_test_inject (a52_state_t * state, const sample_t * spec,
		      const int * blksw, int lfeon)
{
    unsigned c;

    memcpy (state->test_inject_spec, spec,
	    (MB_FBW + 1) * 256 * sizeof (sample_t));
    for (c = 0; c < MB_FBW; c++)
	state->test_inject_blksw[c] = (uint8_t) !!blksw[c];
    state->test_inject_lfeon = lfeon;
    state->test_inject_armed = 1;
}

unsigned a52_test_imdct_calls (a52_state_t * state)
{
    return state->test_imdct_256 + state->test_imdct_512;
}

unsigned a52_test_imdct_256_calls (a52_state_t * state)
{
    return state->test_imdct_256;
}

unsigned a52_test_imdct_512_calls (a52_state_t * state)
{
    return state->test_imdct_512;
}

const sample_t * a52_test_prev_spectrum (a52_state_t * state, int channel)
{
    return state->matrix_prev_spec[channel];
}

const sample_t * a52_test_lfe_spectrum (a52_state_t * state)
{
    return state->matrix_prev_lfe;
}

const uint8_t * a52_test_prev_blksw (a52_state_t * state)
{
    return state->matrix_prev_blksw;
}
#endif
