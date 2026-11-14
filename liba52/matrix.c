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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "a52.h"
#include "a52_internal.h"

/*
 * Matrix listening mode.
 *
 * Instead of running every source channel through its own IMDCT and
 * downmixing in the time domain, the source spectra are projected onto
 * each output row while still in the frequency domain. Channels are
 * grouped by block-switch type: all long-block channels (plus the LFE,
 * which always uses the long transform) share one projection and one
 * IMDCT per output row, all short-block channels share another. With a
 * stable matrix and an all-long input this costs exactly rows IMDCT
 * calls per block.
 *
 * The IMDCT delay of each projected row is a linear combination of the
 * per-channel delays, so when the matrix or the block-type grouping
 * changes, the delays are rebuilt from the previous block's scaled
 * source spectra instead of being carried over with the stale matrix.
 * The previous spectra are kept with the level/dynamic-range scaling
 * of their own block, so no gain is applied twice.
 */

int a52_set_matrix (a52_state_t * state, const sample_t * matrix,
		    unsigned rows)
{
    a52_matrix_t * mx;
    unsigned r, c;

    if ((matrix == NULL) || ((rows != 1) && (rows != 2)))
	return 1;

    /* validate everything before touching the instance, so that a
       rejected update leaves the active and queued matrices alone */
    for (r = 0; r < rows; r++)
	for (c = 0; c < 6; c++) {
	    sample_t v = matrix[r * 6 + c];

	    if (!isfinite ((double) v) || (v > 2) || (v < -2))
		return 1;
	}

    mx = state->matrix;
    if (mx == NULL) {
	/* matrix mode can only be enabled before the first frame */
	if (state->started)
	    return 1;
	mx = malloc (sizeof (a52_matrix_t));
	if (mx == NULL)
	    return 1;
	memset (mx, 0, sizeof (a52_matrix_t));
	state->matrix = mx;
    }

    memcpy (mx->queued, matrix, rows * 6 * sizeof (sample_t));
    if (rows == 1)
	memset (mx->queued[1], 0, 6 * sizeof (sample_t));
    mx->queued_rows = rows;
    mx->pending = 1;

    return 0;
}

/* adopt the queued matrix at a frame boundary */
void a52_matrix_adopt (a52_state_t * state)
{
    a52_matrix_t * mx = state->matrix;

    if (mx->pending) {
	memcpy (mx->active, mx->queued, sizeof (mx->queued));
	mx->rows = mx->queued_rows;
	mx->pending = 0;
	mx->dirty = 1;
    }
}

/* project the given spectra onto one output row, one block-type group */
static void matrix_project (a52_matrix_t * mx, sample_t * dest,
			    sample_t (* spectra)[256], const uint8_t * blksw,
			    int group, int row)
{
    int ch, i;

    for (i = 0; i < 256; i++)
	dest[i] = 0;

    for (ch = 0; ch < 5; ch++)
	if ((blksw[ch] != 0) == group) {
	    sample_t c = mx->active[row][ch];

	    if (c != 0) {
		const sample_t * s = spectra[ch];

		for (i = 0; i < 256; i++)
		    dest[i] += c * s[i];
	    }
	}

    if (group == 0) {	/* the LFE always uses the long transform */
	sample_t c = mx->active[row][5];

	if (c != 0) {
	    const sample_t * s = spectra[5];

	    for (i = 0; i < 256; i++)
		dest[i] += c * s[i];
	}
    }
}

/*
 * Rebuild the per-row IMDCT delays so that the overlap into the
 * current block is interpreted with the current matrix and the
 * current block-type grouping. The delay a channel contributes is a
 * pure function of the previous block's scaled spectrum and block
 * type, both of which were kept, so the rebuild transforms the
 * projected previous spectra instead of replaying anything.
 */
static void matrix_rebuild (a52_state_t * state, const uint8_t * blksw)
{
    a52_matrix_t * mx = state->matrix;
    sample_t * proj = state->samples + 256 * 6;
    sample_t * dtmp = state->samples + 256 * 7;
    int r, g, pt, ch, i;

    for (r = 0; r < 2; r++)
	for (g = 0; g < 2; g++)
	    memset (mx->delay[r][g], 0, 256 * sizeof (sample_t));

    if (!mx->have_prev)
	return;

    for (g = 0; g < 2; g++)
	for (pt = 0; pt < 2; pt++) {
	    int members = (g == 0) && (pt == 0);	/* LFE is long->long */

	    for (ch = 0; ch < 5; ch++)
		if (((blksw[ch] != 0) == g) &&
		    ((mx->prev_blksw[ch] != 0) == pt))
		    members = 1;
	    if (!members)
		continue;

	    for (r = 0; r < mx->rows; r++) {
		for (i = 0; i < 256; i++)
		    proj[i] = 0;
		for (ch = 0; ch < 5; ch++)
		    if (((blksw[ch] != 0) == g) &&
			((mx->prev_blksw[ch] != 0) == pt)) {
			sample_t c = mx->active[r][ch];
			const sample_t * s = mx->prev_spectrum[ch];

			for (i = 0; i < 256; i++)
			    proj[i] += c * s[i];
		    }
		if ((g == 0) && (pt == 0)) {
		    sample_t c = mx->active[r][5];
		    const sample_t * s = mx->prev_spectrum[5];

		    for (i = 0; i < 256; i++)
			proj[i] += c * s[i];
		}

		memset (dtmp, 0, 256 * sizeof (sample_t));
		if (pt)
		    a52_imdct_256 (proj, dtmp, 0);
		else
		    a52_imdct_512 (proj, dtmp, 0);
		for (i = 0; i < 256; i++)
		    mx->delay[r][g][i] += dtmp[i];
	    }
	}
}

/*
 * Synthesize one block in matrix mode. On entry the scaled full-band
 * spectra are in sample planes 0..4 and the scaled LFE spectrum in
 * plane 5 of state->samples; on exit planes 0..rows-1 hold the output.
 */
void a52_matrix_synth (a52_state_t * state, uint8_t * blksw)
{
    a52_matrix_t * mx = state->matrix;
    sample_t * samples = state->samples;
    sample_t * proj = samples + 256 * 6;
    int r, g, ch, i;

    if (mx->dirty || !mx->have_prev ||
	memcmp (blksw, mx->prev_blksw, sizeof (mx->prev_blksw)))
	matrix_rebuild (state, blksw);
    mx->dirty = 0;

    /* keep this block's scaled spectra and types for future rebuilds */
    memcpy (mx->prev_spectrum, samples, 5 * 256 * sizeof (sample_t));
    memcpy (mx->prev_spectrum[5], samples + 256 * 5, 256 * sizeof (sample_t));
    memcpy (mx->prev_blksw, blksw, sizeof (mx->prev_blksw));
    mx->have_prev = 1;

    for (r = 0; r < mx->rows; r++) {
	sample_t * out = samples + 256 * r;

	for (i = 0; i < 256; i++)
	    out[i] = 0;
    }

    for (g = 0; g < 2; g++) {
	int members = (g == 0);	/* the long group always carries the LFE */

	for (ch = 0; ch < 5; ch++)
	    if ((blksw[ch] != 0) == g)
		members = 1;
	if (!members)
	    continue;

	for (r = 0; r < mx->rows; r++) {
	    sample_t * out = samples + 256 * r;

	    matrix_project (mx, proj, mx->prev_spectrum, blksw, g, r);
	    if (g)
		a52_imdct_256 (proj, mx->delay[r][g], 0);
	    else
		a52_imdct_512 (proj, mx->delay[r][g], 0);
	    for (i = 0; i < 256; i++)
		out[i] += proj[i];
	}
    }

    /* the bias is added exactly once per final output sample */
    for (r = 0; r < mx->rows; r++) {
	sample_t * out = samples + 256 * r;

	for (i = 0; i < 256; i++)
	    out[i] += state->bias;
    }
}

#ifdef A52_MATRIX_TEST
/*
 * Test-only entry point: run one injected block of scaled spectra
 * (five full-band planes plus one LFE plane, 256 coefficients each)
 * through the production matrix synthesis path.
 */
void a52_test_matrix_inject (a52_state_t * state, const sample_t * spectra,
			     const uint8_t * blksw, sample_t bias)
{
    if (state->matrix->pending)
	a52_matrix_adopt (state);
    state->bias = bias;
    memcpy (state->samples, spectra, 5 * 256 * sizeof (sample_t));
    memcpy (state->samples + 256 * 5, spectra + 5 * 256,
	    256 * sizeof (sample_t));
    a52_matrix_synth (state, (uint8_t *) blksw);
}
#endif
