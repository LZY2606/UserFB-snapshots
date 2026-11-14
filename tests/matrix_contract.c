/*
 * matrix_contract.c - contract tests for the liba52 matrix listening mode.
 *
 * The independent reference is a continuously running normal
 * full-channel decoder (no A52_ADJUST_LEVEL) fed with the same level,
 * the same dynamic-range setting and a zero bias; its per-source PCM
 * is mixed with the test's own matrix and the output bias is added
 * afterwards. Block-type combinations that the self-built long-block
 * fixtures do not contain are exercised through the A52_MATRIX_TEST
 * spectrum injection hook, which runs the same production synthesis
 * path; the reference there is one IMDCT per source channel.
 *
 * Exports task_cases/task_case_count for task_support/driver.c.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"
#include "a52.h"
#include "a52_internal.h"

#define FIXTURE_LFE "task_support/fixtures/surround_lfe.ac3"
#define FIXTURE_NOLFE "task_support/fixtures/surround.ac3"

static uint8_t * load_file (const char * path, long * size_out)
{
    FILE * f = fopen (path, "rb");
    uint8_t * buf;
    long size;

    if (!f)
	return NULL;
    if (fseek (f, 0, SEEK_END) || (size = ftell (f)) <= 0 || fseek (f, 0, SEEK_SET)) {
	fclose (f);
	return NULL;
    }
    buf = malloc (size + 16);
    if (!buf || fread (buf, 1, size, f) != (size_t) size) {
	fclose (f);
	free (buf);
	return NULL;
    }
    fclose (f);
    memset (buf + size, 0, 16);
    *size_out = size;
    return buf;
}

/* matrix column order is L, C, R, Ls, Rs, LFE; the reference decoder
   planes are L, C, R, Ls, Rs with the LFE plane first when present */
static void ref_mix (const sample_t * s, int lfe, const sample_t * m,
		     int rows, sample_t bias, sample_t * out)
{
    int r, n, c;

    for (r = 0; r < rows; r++)
	for (n = 0; n < 256; n++) {
	    double v = bias;

	    for (c = 0; c < 6; c++) {
		int pl;

		if (c == 5) {
		    if (!lfe)
			continue;
		    pl = 0;
		} else
		    pl = c + lfe;
		v += (double) m[r * 6 + c] * (double) s[256 * pl + n];
	    }
	    out[r * 256 + n] = (sample_t) v;
	}
}

static int close_enough (const sample_t * got, const sample_t * ref, int count)
{
    int i;

    for (i = 0; i < count; i++) {
	double d = fabs ((double) got[i] - (double) ref[i]);

	if (!(d <= 3e-5 * (1 + fabs ((double) ref[i]))))
	    return 0;
    }
    return 1;
}

static double peak_of (const sample_t * s, int count)
{
    double p = 0;
    int i;

    for (i = 0; i < count; i++) {
	double a = fabs ((double) s[i]);

	if (a > p)
	    p = a;
    }
    return p;
}

/* asymmetric matrices, both signs, coefficients within +-2 */
static const sample_t MA[12] = {
    0.90f, 0.70710678f, -0.35f, 0.50f, -0.25f, 0.75f,
    -0.40f, 0.59460356f, 0.85f, -0.60f, 0.30f, -0.50f
};
static const sample_t MB[12] = {
    -0.70f, 0.20f, 1.10f, -0.45f, 0.65f, -1.00f,
    0.55f, -0.90f, -0.30f, 1.20f, -0.15f, 0.40f
};

/*
 * matrix group: two-row stereo mode, matrix switched on a non-silent
 * frame boundary, per-sample comparison against the reference mix.
 */
static int case_frame_switch (void)
{
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_LFE, &size);
    a52_state_t * ref, * mx;
    const sample_t * cur = MA;
    sample_t bias = 0.25f;
    int fr = 0, ok = 1;
    double bpeak = 0;

    TASK_CHECK (buf != NULL);
    ref = a52_init (0);
    mx = a52_init (0);
    TASK_CHECK (ref != NULL && mx != NULL);
    TASK_CHECK (a52_set_matrix (mx, MA, 2) == 0);

    pos = 0;
    while (pos < size) {
	int flags, sr, br, a, am, k;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t lev, levm;

	TASK_CHECK (len > 0 && sr == 48000);
	if (fr == 5) {
	    TASK_CHECK (a52_set_matrix (mx, MB, 2) == 0);
	    cur = MB;
	}

	a = A52_3F2R | A52_LFE;
	lev = 1.0f;
	TASK_CHECK (a52_frame (ref, buf + pos, &a, &lev, 0) == 0);
	a52_dynrng (ref, NULL, NULL);

	am = 0x7fffffff;	/* input flags are ignored in matrix mode */
	levm = 1.0f;
	TASK_CHECK (a52_frame (mx, buf + pos, &am, &levm, bias) == 0);
	TASK_CHECK (am == A52_STEREO);		/* no A52_LFE attached */
	TASK_CHECK (levm == 1.0f);		/* level used as given */
	a52_dynrng (mx, NULL, NULL);

	for (k = 0; k < 6; k++) {
	    sample_t expect[512];
	    double p;

	    TASK_CHECK (a52_block (ref) == 0);
	    TASK_CHECK (a52_block (mx) == 0);
	    ref_mix (a52_samples (ref), 1, cur, 2, bias, expect);
	    if (!close_enough (a52_samples (mx), expect, 512))
		ok = 0;
	    if (fr >= 4 && fr <= 6) {
		p = peak_of (expect, 512);
		if (p > bpeak)
		    bpeak = p;
	    }
	}
	fr++;
	pos += len;
    }
    TASK_CHECK (fr >= 12);
    TASK_CHECK (ok);
    TASK_CHECK (bpeak > 0.02);	/* the switch happened on audible content */

    a52_free (ref);
    a52_free (mx);
    free (buf);
    return 0;
}

/*
 * matrix group: mono mode, independent LFE projection, an all-zero
 * row, and a mixed row on the same instance.
 */
static int case_mono_lfe_zero_row (void)
{
    static const sample_t m_lfe[6] = {0, 0, 0, 0, 0, 1};
    static const sample_t m_zero[6] = {0, 0, 0, 0, 0, 0};
    static const sample_t m_mix[6] = {0.8f, -0.6f, 0.4f, -0.3f, 1.1f, -0.9f};
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_LFE, &size);
    a52_state_t * ref, * mx;
    const sample_t * cur = m_lfe;
    sample_t bias = -0.125f;
    int fr = 0, ok = 1, zok = 1;

    TASK_CHECK (buf != NULL);
    ref = a52_init (0);
    mx = a52_init (0);
    TASK_CHECK (ref != NULL && mx != NULL);
    TASK_CHECK (a52_set_matrix (mx, m_lfe, 1) == 0);

    pos = 0;
    while (pos < size) {
	int flags, sr, br, a, am, k, i;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t lev, levm;

	TASK_CHECK (len > 0);
	if (fr == 4) {
	    TASK_CHECK (a52_set_matrix (mx, m_zero, 1) == 0);
	    cur = m_zero;
	}
	if (fr == 8) {
	    TASK_CHECK (a52_set_matrix (mx, m_mix, 1) == 0);
	    cur = m_mix;
	}

	a = A52_3F2R | A52_LFE;
	lev = 1.0f;
	TASK_CHECK (a52_frame (ref, buf + pos, &a, &lev, 0) == 0);
	a52_dynrng (ref, NULL, NULL);

	am = 0;
	levm = 1.0f;
	TASK_CHECK (a52_frame (mx, buf + pos, &am, &levm, bias) == 0);
	TASK_CHECK (am == A52_MONO);
	a52_dynrng (mx, NULL, NULL);

	for (k = 0; k < 6; k++) {
	    sample_t expect[256];
	    const sample_t * got;

	    TASK_CHECK (a52_block (ref) == 0);
	    TASK_CHECK (a52_block (mx) == 0);
	    got = a52_samples (mx);
	    ref_mix (a52_samples (ref), 1, cur, 1, bias, expect);
	    if (cur == m_zero) {
		/* an all-zero row must give exactly the bias */
		for (i = 0; i < 256; i++)
		    if (got[i] != bias)
			zok = 0;
	    } else if (!close_enough (got, expect, 256))
		ok = 0;
	}
	fr++;
	pos += len;
    }
    TASK_CHECK (ok);
    TASK_CHECK (zok);

    a52_free (ref);
    a52_free (mx);
    free (buf);
    return 0;
}

/*
 * matrix group: without a coded LFE the sixth column is treated as
 * zero, on the self-built 5.0 long-block track.
 */
static int case_no_lfe_sixth_column (void)
{
    static const sample_t m[12] = {
	0.6f, -0.8f, 0.5f, 1.0f, -0.4f, 1.5f,
	-0.9f, 0.3f, -1.1f, 0.7f, 0.2f, -1.5f
    };
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_NOLFE, &size);
    a52_state_t * ref, * mx;
    int ok = 1, frames = 0;

    TASK_CHECK (buf != NULL);
    ref = a52_init (0);
    mx = a52_init (0);
    TASK_CHECK (ref != NULL && mx != NULL);
    TASK_CHECK (a52_set_matrix (mx, m, 2) == 0);

    pos = 0;
    while (pos < size) {
	int flags, sr, br, a, am, k;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t lev, levm;

	TASK_CHECK (len > 0);
	TASK_CHECK (!(flags & A52_LFE));

	a = A52_3F2R;
	lev = 1.0f;
	TASK_CHECK (a52_frame (ref, buf + pos, &a, &lev, 0) == 0);
	a52_dynrng (ref, NULL, NULL);

	am = A52_3F2R | A52_LFE;	/* ignored in matrix mode */
	levm = 1.0f;
	TASK_CHECK (a52_frame (mx, buf + pos, &am, &levm, 0) == 0);
	TASK_CHECK (am == A52_STEREO);
	a52_dynrng (mx, NULL, NULL);

	for (k = 0; k < 6; k++) {
	    sample_t expect[512];

	    TASK_CHECK (a52_block (ref) == 0);
	    TASK_CHECK (a52_block (mx) == 0);
	    ref_mix (a52_samples (ref), 0, m, 2, 0, expect);
	    if (!close_enough (a52_samples (mx), expect, 512))
		ok = 0;
	}
	frames++;
	pos += len;
    }
    TASK_CHECK (frames >= 12);
    TASK_CHECK (ok);

    a52_free (ref);
    a52_free (mx);
    free (buf);
    return 0;
}

/*
 * matrix group: the instance owns a copy of the matrix, invalid
 * updates are rejected without touching the queued or active matrix,
 * and +-2 boundary coefficients are accepted.
 */
static int case_ownership_and_invalid (void)
{
    static const sample_t m2[6] = {2.0f, -2.0f, 0.5f, -0.5f, 1.0f, -1.0f};
    static const sample_t m3[12] = {
	0.25f, -0.75f, 1.25f, -1.5f, 0.5f, -0.5f,
	-1.0f, 0.6f, -0.4f, 0.9f, -1.75f, 0.3f
    };
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_LFE, &size);
    a52_state_t * ref, * mx;
    sample_t m[6] = {0.5f, -0.25f, 1.0f, 0.75f, -1.5f, 2.0f};
    sample_t m_keep[6];
    sample_t bad[6];
    const sample_t * cur = m_keep;
    int rows = 1, fr = 0, ok = 1, i;

    TASK_CHECK (buf != NULL);
    ref = a52_init (0);
    mx = a52_init (0);
    TASK_CHECK (ref != NULL && mx != NULL);

    memcpy (m_keep, m, sizeof (m));
    TASK_CHECK (a52_set_matrix (mx, m, 1) == 0);
    /* a second valid call before the first frame replaces the queue */
    TASK_CHECK (a52_set_matrix (mx, m, 1) == 0);
    /* the caller may trash its own array afterwards */
    for (i = 0; i < 6; i++)
	m[i] = 12345.0f;

    pos = 0;
    while (pos < size) {
	int flags, sr, br, a, am, k;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t lev, levm;

	TASK_CHECK (len > 0);

	if (fr == 2) {
	    /* all of these must be rejected without effect */
	    TASK_CHECK (a52_set_matrix (mx, NULL, 1) == 1);
	    TASK_CHECK (a52_set_matrix (mx, m2, 0) == 1);
	    TASK_CHECK (a52_set_matrix (mx, m2, 3) == 1);
	    for (i = 0; i < 6; i++)
		bad[i] = 0.25f;
	    bad[3] = 2.5f;
	    TASK_CHECK (a52_set_matrix (mx, bad, 1) == 1);
	    bad[3] = -2.5f;
	    TASK_CHECK (a52_set_matrix (mx, bad, 1) == 1);
	    bad[3] = (sample_t) NAN;
	    TASK_CHECK (a52_set_matrix (mx, bad, 1) == 1);
	    bad[3] = (sample_t) INFINITY;
	    TASK_CHECK (a52_set_matrix (mx, bad, 1) == 1);
	    bad[3] = -(sample_t) INFINITY;
	    TASK_CHECK (a52_set_matrix (mx, bad, 1) == 1);
	}
	if (fr == 6) {
	    /* +-2 boundary values are legal */
	    TASK_CHECK (a52_set_matrix (mx, m2, 1) == 0);
	    cur = m2;
	    rows = 1;
	}
	if (fr == 9) {
	    /* the row count may change on a frame boundary too */
	    TASK_CHECK (a52_set_matrix (mx, m3, 2) == 0);
	    cur = m3;
	    rows = 2;
	}

	a = A52_3F2R | A52_LFE;
	lev = 1.0f;
	TASK_CHECK (a52_frame (ref, buf + pos, &a, &lev, 0) == 0);
	a52_dynrng (ref, NULL, NULL);

	am = 0;
	levm = 1.0f;
	TASK_CHECK (a52_frame (mx, buf + pos, &am, &levm, 0) == 0);
	TASK_CHECK (am == ((rows == 2) ? A52_STEREO : A52_MONO));
	a52_dynrng (mx, NULL, NULL);

	for (k = 0; k < 6; k++) {
	    sample_t expect[512];

	    TASK_CHECK (a52_block (ref) == 0);
	    TASK_CHECK (a52_block (mx) == 0);
	    ref_mix (a52_samples (ref), 1, cur, rows, 0, expect);
	    if (!close_enough (a52_samples (mx), expect, rows * 256))
		ok = 0;
	}
	fr++;
	pos += len;
    }
    TASK_CHECK (rows == 2);	/* the rows switch was exercised */
    TASK_CHECK (ok);

    a52_free (ref);
    a52_free (mx);
    free (buf);
    return 0;
}

/*
 * matrix group: a setter call inside a frame only queues the matrix;
 * the remaining blocks of that frame still use the old version.
 */
static int case_intra_frame_queue (void)
{
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_LFE, &size);
    a52_state_t * ref, * mx;
    int fr = 0, ok = 1, queued = 0;

    TASK_CHECK (buf != NULL);
    ref = a52_init (0);
    mx = a52_init (0);
    TASK_CHECK (ref != NULL && mx != NULL);
    TASK_CHECK (a52_set_matrix (mx, MA, 2) == 0);

    pos = 0;
    while (pos < size) {
	int flags, sr, br, a, am, k;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t lev, levm;
	const sample_t * cur = (fr < 4) ? MA : MB;

	TASK_CHECK (len > 0);

	a = A52_3F2R | A52_LFE;
	lev = 1.0f;
	TASK_CHECK (a52_frame (ref, buf + pos, &a, &lev, 0) == 0);
	a52_dynrng (ref, NULL, NULL);

	am = 0;
	levm = 1.0f;
	TASK_CHECK (a52_frame (mx, buf + pos, &am, &levm, 0) == 0);
	a52_dynrng (mx, NULL, NULL);

	for (k = 0; k < 6; k++) {
	    sample_t expect[512];

	    TASK_CHECK (a52_block (ref) == 0);
	    TASK_CHECK (a52_block (mx) == 0);
	    /* within frame 3 the queued matrix must not leak in */
	    ref_mix (a52_samples (ref), 1, cur, 2, 0, expect);
	    if (!close_enough (a52_samples (mx), expect, 512))
		ok = 0;
	    if (fr == 3 && k == 1 && !queued) {
		TASK_CHECK (a52_set_matrix (mx, MB, 2) == 0);
		queued = 1;
	    }
	}
	fr++;
	pos += len;
    }
    TASK_CHECK (queued);
    TASK_CHECK (ok);

    a52_free (ref);
    a52_free (mx);
    free (buf);
    return 0;
}

/*
 * matrix group: two matrix-mode instances decoded interleaved, each
 * with its own matrix, stay independent of each other.
 */
static int case_two_instances (void)
{
    static const sample_t m1[12] = {
	0.4f, -0.7f, 0.9f, -0.2f, 1.3f, -0.6f,
	-1.1f, 0.5f, -0.3f, 0.8f, -0.9f, 0.1f
    };
    static const sample_t m2[6] = {-0.5f, 1.2f, -0.8f, 0.6f, 0.3f, -1.4f};
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_LFE, &size);
    a52_state_t * ref1, * ref2, * mx1, * mx2;
    int ok = 1, frames = 0;

    TASK_CHECK (buf != NULL);
    ref1 = a52_init (0);
    ref2 = a52_init (0);
    mx1 = a52_init (0);
    mx2 = a52_init (0);
    TASK_CHECK (ref1 && ref2 && mx1 && mx2);
    TASK_CHECK (a52_set_matrix (mx1, m1, 2) == 0);
    TASK_CHECK (a52_set_matrix (mx2, m2, 1) == 0);

    pos = 0;
    while (pos < size) {
	int flags, sr, br, a1, a2, f1, f2, k;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t l1, l2, m1l, m2l;

	TASK_CHECK (len > 0);

	a1 = A52_3F2R | A52_LFE;
	l1 = 1.0f;
	TASK_CHECK (a52_frame (ref1, buf + pos, &a1, &l1, 0) == 0);
	a52_dynrng (ref1, NULL, NULL);
	a2 = A52_3F2R | A52_LFE;
	l2 = 1.0f;
	TASK_CHECK (a52_frame (ref2, buf + pos, &a2, &l2, 0) == 0);
	a52_dynrng (ref2, NULL, NULL);

	f1 = 0;
	m1l = 1.0f;
	TASK_CHECK (a52_frame (mx1, buf + pos, &f1, &m1l, 0) == 0);
	a52_dynrng (mx1, NULL, NULL);
	f2 = 0;
	m2l = 1.0f;
	TASK_CHECK (a52_frame (mx2, buf + pos, &f2, &m2l, 0) == 0);
	a52_dynrng (mx2, NULL, NULL);
	TASK_CHECK (f1 == A52_STEREO && f2 == A52_MONO);

	for (k = 0; k < 6; k++) {
	    sample_t expect[512];

	    TASK_CHECK (a52_block (mx1) == 0);
	    TASK_CHECK (a52_block (mx2) == 0);
	    TASK_CHECK (a52_block (ref1) == 0);
	    TASK_CHECK (a52_block (ref2) == 0);

	    ref_mix (a52_samples (ref1), 1, m1, 2, 0, expect);
	    if (!close_enough (a52_samples (mx1), expect, 512))
		ok = 0;
	    ref_mix (a52_samples (ref2), 1, m2, 1, 0, expect);
	    if (!close_enough (a52_samples (mx2), expect, 256))
		ok = 0;
	}
	frames++;
	pos += len;
    }
    TASK_CHECK (frames >= 12);
    TASK_CHECK (ok);

    a52_free (ref1);
    a52_free (ref2);
    a52_free (mx1);
    a52_free (mx2);
    free (buf);
    return 0;
}

/*
 * overlap group: the first block after a matrix switch reinterprets
 * the overlap with the new matrix; neither the block before nor the
 * block after the switch is skipped.
 */
static int case_switch_overlap (void)
{
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_LFE, &size);
    a52_state_t * ref, * mx;
    int fr = 0, pre_ok = 1, first_ok = 1, post_ok = 1, differs = 0;
    double first_peak = 0;

    TASK_CHECK (buf != NULL);
    ref = a52_init (0);
    mx = a52_init (0);
    TASK_CHECK (ref != NULL && mx != NULL);
    TASK_CHECK (a52_set_matrix (mx, MA, 2) == 0);

    pos = 0;
    while (pos < size) {
	int flags, sr, br, a, am, k, i;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t lev, levm;
	const sample_t * cur = (fr < 5) ? MA : MB;

	TASK_CHECK (len > 0);
	if (fr == 5)
	    TASK_CHECK (a52_set_matrix (mx, MB, 2) == 0);

	a = A52_3F2R | A52_LFE;
	lev = 1.0f;
	TASK_CHECK (a52_frame (ref, buf + pos, &a, &lev, 0) == 0);
	a52_dynrng (ref, NULL, NULL);

	am = 0;
	levm = 1.0f;
	TASK_CHECK (a52_frame (mx, buf + pos, &am, &levm, 0) == 0);
	a52_dynrng (mx, NULL, NULL);

	for (k = 0; k < 6; k++) {
	    sample_t expect[512], stale[512];
	    const sample_t * got;

	    TASK_CHECK (a52_block (ref) == 0);
	    TASK_CHECK (a52_block (mx) == 0);
	    got = a52_samples (mx);
	    ref_mix (a52_samples (ref), 1, cur, 2, 0, expect);

	    if (fr == 4 && k == 5) {
		/* last block on the old matrix, feeds the overlap */
		if (!close_enough (got, expect, 512))
		    pre_ok = 0;
	    }
	    if (fr == 5 && k == 0) {
		/* first block on the new matrix, overlap included */
		double p = peak_of (expect, 512);

		if (p > first_peak)
		    first_peak = p;
		if (!close_enough (got, expect, 512))
		    first_ok = 0;
		/* the switch must be visible in this very block */
		ref_mix (a52_samples (ref), 1, MA, 2, 0, stale);
		for (i = 0; i < 512; i++) {
		    double d = fabs ((double) got[i] - (double) stale[i]);

		    if (d > 3e-5 * (1 + fabs ((double) stale[i])))
			differs++;
		}
	    }
	    if (fr >= 5 && !close_enough (got, expect, 512))
		post_ok = 0;
	}
	fr++;
	pos += len;
    }
    TASK_CHECK (pre_ok);
    TASK_CHECK (first_ok);
    TASK_CHECK (post_ok);
    TASK_CHECK (first_peak > 0.02);	/* non-silent transition block */
    TASK_CHECK (differs > 0);		/* new matrix really in effect */

    a52_free (ref);
    a52_free (mx);
    free (buf);
    return 0;
}

/*
 * overlap group: with a stable matrix, changing the level (and the
 * dynamic-range setting) between frames must not re-apply the new
 * gain to the history carried by the previous block.
 */
static int case_level_change_history (void)
{
    static const sample_t m[12] = {
	0.8f, -0.5f, 0.6f, 0.9f, -0.7f, 0.4f,
	-0.3f, 1.1f, -0.85f, 0.2f, 0.65f, -1.2f
    };
    static const sample_t levels[12] = {
	1.0f, 1.0f, 1.0f, 0.5f, 1.5f, 0.75f,
	1.25f, 1.0f, 0.5f, 1.5f, 1.0f, 1.0f
    };
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_LFE, &size);
    a52_state_t * ref, * mx;
    int fr = 0, ok = 1;
    double bpeak = 0;

    TASK_CHECK (buf != NULL);
    ref = a52_init (0);
    mx = a52_init (0);
    TASK_CHECK (ref != NULL && mx != NULL);
    TASK_CHECK (a52_set_matrix (mx, m, 2) == 0);

    pos = 0;
    while (pos < size && fr < 12) {
	int flags, sr, br, a, am, k;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t lev, levm;

	TASK_CHECK (len > 0);

	a = A52_3F2R | A52_LFE;
	lev = levels[fr];
	TASK_CHECK (a52_frame (ref, buf + pos, &a, &lev, 0) == 0);
	a52_dynrng (ref, NULL, NULL);

	am = 0;
	levm = levels[fr];
	TASK_CHECK (a52_frame (mx, buf + pos, &am, &levm, 0) == 0);
	TASK_CHECK (levm == levels[fr]);	/* no auto gain adjust */
	a52_dynrng (mx, NULL, NULL);

	for (k = 0; k < 6; k++) {
	    sample_t expect[512];
	    double p;

	    TASK_CHECK (a52_block (ref) == 0);
	    TASK_CHECK (a52_block (mx) == 0);
	    ref_mix (a52_samples (ref), 1, m, 2, 0, expect);
	    if (!close_enough (a52_samples (mx), expect, 512))
		ok = 0;
	    p = peak_of (expect, 512);
	    if (p > bpeak)
		bpeak = p;
	}
	fr++;
	pos += len;
    }
    TASK_CHECK (fr == 12);
    TASK_CHECK (ok);
    TASK_CHECK (bpeak > 0.02);

    a52_free (ref);
    a52_free (mx);
    free (buf);
    return 0;
}

/*
 * grouping group: injected spectra with mixed block-switch patterns
 * run through the production synthesis path; the reference is one
 * IMDCT per source channel with per-channel delays.
 */
static void make_spectra (sample_t (* spec)[256], int blk)
{
    int c, k;

    for (c = 0; c < 6; c++)
	for (k = 0; k < 256; k++) {
	    double v = 0.35 * sin (0.017 * (k + 1) * (c + 2) + 0.29 * blk +
				   0.13 * c) +
		       0.2 * cos (0.11 * k + 0.53 * c - 0.07 * blk);

	    if (c == 5 && k >= 7)
		v = 0;
	    spec[c][k] = (sample_t) v;
	}
}

static int case_mixed_block_types (void)
{
    static const sample_t m[12] = {
	0.7f, -0.4f, 0.9f, 0.3f, -1.2f, 0.6f,
	-0.8f, 1.1f, -0.5f, 0.2f, 0.45f, -0.95f
    };
    static const uint8_t patterns[][5] = {
	{0,0,0,0,0}, {0,0,0,0,0}, {1,1,1,1,1}, {1,1,1,1,1},
	{0,0,0,0,0}, {0,0,1,1,0}, {1,1,0,0,1}, {0,1,0,1,0},
	{1,0,1,0,1}, {0,0,0,0,0}, {0,0,0,0,0}, {1,0,0,0,0},
	{0,1,1,1,1}, {0,0,0,0,0}, {1,1,1,1,1}, {0,0,0,0,0}
    };
    a52_state_t * mx = a52_init (0);
    sample_t dly[6][256];
    sample_t bias = 0.125f;
    int ok = 1, spec_ok = 1;
    unsigned b;

    TASK_CHECK (mx != NULL);
    TASK_CHECK (a52_set_matrix (mx, m, 2) == 0);
    memset (dly, 0, sizeof (dly));

    for (b = 0; b < sizeof (patterns) / sizeof (patterns[0]); b++) {
	sample_t spec[6][256];
	sample_t expect[512];
	uint8_t blksw[5];
	int c, n, r;

	make_spectra (spec, b);
	memcpy (blksw, patterns[b], 5);

	/* reference: one imdct per source channel, then mix */
	for (r = 0; r < 2; r++)
	    for (n = 0; n < 256; n++)
		expect[r * 256 + n] = 0;
	for (c = 0; c < 6; c++) {
	    sample_t tmp[256];
	    int type = (c == 5) ? 0 : blksw[c];

	    memcpy (tmp, spec[c], sizeof (tmp));
	    if (type)
		a52_imdct_256 (tmp, dly[c], 0);
	    else
		a52_imdct_512 (tmp, dly[c], 0);
	    for (r = 0; r < 2; r++)
		for (n = 0; n < 256; n++)
		    expect[r * 256 + n] += m[r * 6 + c] * tmp[n];
	}
	for (n = 0; n < 512; n++)
	    expect[n] += bias;

	a52_test_matrix_inject (mx, &spec[0][0], blksw, bias);
	if (!close_enough (a52_samples (mx), expect, 512))
	    ok = 0;

	/* the parsed/injected spectra are observable under the test macro */
	if (memcmp (mx->matrix->prev_spectrum, spec, sizeof (spec)) != 0)
	    spec_ok = 0;
    }
    TASK_CHECK (ok);
    TASK_CHECK (spec_ok);

    a52_free (mx);
    return 0;
}

/*
 * grouping group: with a stable matrix and all-long input, each block
 * costs exactly rows IMDCT calls - no hidden full-channel transforms
 * maintain the history.
 */
static int case_imdct_budget (void)
{
    static const sample_t m2[12] = {
	0.5f, -0.5f, 1.0f, -1.0f, 0.25f, -0.25f,
	-0.75f, 0.75f, -1.5f, 1.5f, 0.5f, -0.5f
    };
    static const sample_t m2b[12] = {
	-0.6f, 0.4f, -0.2f, 1.0f, -0.8f, 0.3f,
	0.9f, -1.1f, 0.7f, -0.5f, 0.2f, -0.1f
    };
    static const sample_t m1[6] = {0.9f, -0.3f, 0.6f, -1.2f, 0.45f, 0.8f};
    a52_state_t * mx = a52_init (0);
    a52_state_t * mono = a52_init (0);
    sample_t spec[6][256];
    uint8_t all_long[5] = {0, 0, 0, 0, 0};
    uint8_t all_short[5] = {1, 1, 1, 1, 1};
    uint8_t mixed[5] = {0, 0, 1, 1, 0};
    unsigned long b256, b512;
    int i, ok = 1;

    TASK_CHECK (mx != NULL && mono != NULL);
    TASK_CHECK (a52_set_matrix (mx, m2, 2) == 0);
    TASK_CHECK (a52_set_matrix (mono, m1, 1) == 0);
    make_spectra (spec, 3);

    a52_test_imdct_256_calls = 0;
    a52_test_imdct_512_calls = 0;

    /* stable matrix, all-long input: exactly rows calls per block */
    for (i = 0; i < 6; i++) {
	b256 = a52_test_imdct_256_calls;
	b512 = a52_test_imdct_512_calls;
	a52_test_matrix_inject (mx, &spec[0][0], all_long, 0);
	if (a52_test_imdct_512_calls - b512 != 2 ||
	    a52_test_imdct_256_calls - b256 != 0)
	    ok = 0;
    }
    /* mono instance: one call per block */
    for (i = 0; i < 3; i++) {
	b512 = a52_test_imdct_512_calls;
	a52_test_matrix_inject (mono, &spec[0][0], all_long, 0);
	if (a52_test_imdct_512_calls - b512 != 1)
	    ok = 0;
    }
    /* a block-type switch may rebuild the delays once, with a bounded
       number of transforms - never one per source channel per block */
    b256 = a52_test_imdct_256_calls;
    b512 = a52_test_imdct_512_calls;
    a52_test_matrix_inject (mx, &spec[0][0], mixed, 0);
    if (a52_test_imdct_512_calls - b512 +
	a52_test_imdct_256_calls - b256 > 8)
	ok = 0;
    /* stable on the mixed pattern: one long and one short per row */
    for (i = 0; i < 2; i++) {
	b256 = a52_test_imdct_256_calls;
	b512 = a52_test_imdct_512_calls;
	a52_test_matrix_inject (mx, &spec[0][0], mixed, 0);
	if (a52_test_imdct_512_calls - b512 != 2 ||
	    a52_test_imdct_256_calls - b256 != 2)
	    ok = 0;
    }
    /* all-short input: the LFE still takes the long transform */
    a52_test_matrix_inject (mx, &spec[0][0], all_short, 0);	/* transition */
    for (i = 0; i < 2; i++) {
	b256 = a52_test_imdct_256_calls;
	b512 = a52_test_imdct_512_calls;
	a52_test_matrix_inject (mx, &spec[0][0], all_short, 0);
	if (a52_test_imdct_512_calls - b512 != 2 ||
	    a52_test_imdct_256_calls - b256 != 2)
	    ok = 0;
    }
    /* back to all-long with the same matrix: steady state again */
    a52_test_matrix_inject (mx, &spec[0][0], all_long, 0);	/* transition */
    for (i = 0; i < 3; i++) {
	b256 = a52_test_imdct_256_calls;
	b512 = a52_test_imdct_512_calls;
	a52_test_matrix_inject (mx, &spec[0][0], all_long, 0);
	if (a52_test_imdct_512_calls - b512 != 2 ||
	    a52_test_imdct_256_calls - b256 != 0)
	    ok = 0;
    }
    /* a matrix change may rebuild once, then the budget applies again */
    TASK_CHECK (a52_set_matrix (mx, m2b, 2) == 0);
    b256 = a52_test_imdct_256_calls;
    b512 = a52_test_imdct_512_calls;
    a52_test_matrix_inject (mx, &spec[0][0], all_long, 0);
    if (a52_test_imdct_512_calls - b512 +
	a52_test_imdct_256_calls - b256 > 8)
	ok = 0;
    for (i = 0; i < 3; i++) {
	b256 = a52_test_imdct_256_calls;
	b512 = a52_test_imdct_512_calls;
	a52_test_matrix_inject (mx, &spec[0][0], all_long, 0);
	if (a52_test_imdct_512_calls - b512 != 2 ||
	    a52_test_imdct_256_calls - b256 != 0)
	    ok = 0;
    }
    TASK_CHECK (ok);

    a52_free (mx);
    a52_free (mono);
    return 0;
}

/*
 * compatibility group: the normal decode path is unchanged - stereo
 * downmix of the self-built 5.1/5.0 tracks still equals the manual
 * mix of the full-channel decode with the header mix levels.
 */
static int compat_decode (const char * path, int lfe)
{
    static const double cm[4] = {.7071067811865476, .5946035575013605,
				 .5, .5946035575013605};
    static const double sm[4] = {.7071067811865476, .5, 0, .5};
    long size, pos;
    uint8_t * buf = load_file (path, &size);
    a52_state_t * full, * stereo;
    int ok = 1, frames = 0;
    double energy = 0;

    TASK_CHECK (buf != NULL);
    full = a52_init (0);
    stereo = a52_init (0);
    TASK_CHECK (full != NULL && stereo != NULL);

    pos = 0;
    while (pos < size) {
	int flags, sr, br, a, b, k, n;
	int len = a52_syncinfo (buf + pos, &flags, &sr, &br);
	sample_t lev, levb;
	double c, s;

	TASK_CHECK (len > 0 && sr == 48000);
	TASK_CHECK ((flags & A52_CHANNEL_MASK) == A52_3F2R);
	TASK_CHECK (!(flags & A52_LFE) == !lfe);

	a = A52_3F2R | (lfe ? A52_LFE : 0);
	lev = 1.0f;
	TASK_CHECK (a52_frame (full, buf + pos, &a, &lev, 0) == 0);
	TASK_CHECK (a == (A52_3F2R | (lfe ? A52_LFE : 0)) && lev == 1.0f);
	a52_dynrng (full, NULL, NULL);

	b = A52_STEREO;
	levb = 1.0f;
	TASK_CHECK (a52_frame (stereo, buf + pos, &b, &levb, 0) == 0);
	TASK_CHECK (b == A52_STEREO && levb == 1.0f);
	a52_dynrng (stereo, NULL, NULL);

	c = cm[(buf[pos + 6] >> 3) & 3];
	s = sm[(buf[pos + 6] >> 1) & 3];

	for (k = 0; k < 6; k++) {
	    const sample_t * x, * y;
	    int off = lfe ? 256 : 0;

	    TASK_CHECK (a52_block (full) == 0);
	    TASK_CHECK (a52_block (stereo) == 0);
	    x = a52_samples (full);
	    y = a52_samples (stereo);
	    for (n = 0; n < 256; n++) {
		double left = x[off + n] + c * x[off + 256 + n] +
		    s * x[off + 768 + n];
		double right = x[off + 512 + n] + c * x[off + 256 + n] +
		    s * x[off + 1024 + n];

		if (fabs (y[n] - left) >= 3e-5 * (1 + fabs (left)) ||
		    fabs (y[256 + n] - right) >= 3e-5 * (1 + fabs (right)))
		    ok = 0;
		energy += (double) y[n] * y[n] + (double) y[256 + n] * y[256 + n];
	    }
	}
	frames++;
	pos += len;
    }
    TASK_CHECK (frames >= 12);
    TASK_CHECK (ok);
    TASK_CHECK (energy > 0);

    a52_free (full);
    a52_free (stereo);
    free (buf);
    return 0;
}

static int case_normal_mode_baseline (void)
{
    TASK_CHECK (compat_decode (FIXTURE_LFE, 1) == 0);
    TASK_CHECK (compat_decode (FIXTURE_NOLFE, 0) == 0);
    return 0;
}

/*
 * compatibility group: mode gating - enabling matrix mode after
 * frames were decoded fails and leaves the normal instance usable;
 * matrix mode reports plain A52_MONO/A52_STEREO flags and leaves the
 * requested level untouched even with A52_ADJUST_LEVEL set.
 */
static int case_mode_gating (void)
{
    static const sample_t m[6] = {0.5f, -0.5f, 1.0f, -1.0f, 0.25f, 0.75f};
    long size, pos;
    uint8_t * buf = load_file (FIXTURE_LFE, &size);
    a52_state_t * st, * mx;
    int flags, sr, br, len, a, k;
    sample_t lev;

    TASK_CHECK (buf != NULL);
    st = a52_init (0);
    mx = a52_init (0);
    TASK_CHECK (st != NULL && mx != NULL);

    len = a52_syncinfo (buf, &flags, &sr, &br);
    TASK_CHECK (len > 0);

    /* a normal instance that already decoded a frame refuses matrix mode */
    a = A52_STEREO;
    lev = 1.0f;
    TASK_CHECK (a52_frame (st, buf, &a, &lev, 0) == 0);
    for (k = 0; k < 6; k++)
	TASK_CHECK (a52_block (st) == 0);
    TASK_CHECK (a52_set_matrix (st, m, 1) == 1);
    /* and it keeps decoding normally afterwards */
    pos = len;
    len = a52_syncinfo (buf + pos, &flags, &sr, &br);
    TASK_CHECK (len > 0);
    a = A52_STEREO;
    lev = 1.0f;
    TASK_CHECK (a52_frame (st, buf + pos, &a, &lev, 0) == 0);
    TASK_CHECK (a == A52_STEREO);
    for (k = 0; k < 6; k++)
	TASK_CHECK (a52_block (st) == 0);
    TASK_CHECK (peak_of (a52_samples (st), 512) > 0);

    /* matrix mode flags and level semantics on the public entry */
    TASK_CHECK (a52_set_matrix (mx, m, 1) == 0);
    a = A52_3F2R | A52_LFE | A52_ADJUST_LEVEL;
    lev = 0.7f;
    TASK_CHECK (a52_frame (mx, buf, &a, &lev, 0.5f) == 0);
    TASK_CHECK (a == A52_MONO);		/* no A52_LFE attached */
    TASK_CHECK (lev == 0.7f);		/* no automatic gain adjust */
    a52_dynrng (mx, NULL, NULL);
    for (k = 0; k < 6; k++)
	TASK_CHECK (a52_block (mx) == 0);
    TASK_CHECK (a52_samples (mx) != NULL);

    a52_free (st);
    a52_free (mx);
    free (buf);
    return 0;
}

const struct task_case task_cases[] = {
    {"matrix", "frame_switch", case_frame_switch},
    {"matrix", "mono_lfe_zero_row", case_mono_lfe_zero_row},
    {"matrix", "no_lfe_sixth_column", case_no_lfe_sixth_column},
    {"matrix", "ownership_and_invalid", case_ownership_and_invalid},
    {"matrix", "intra_frame_queue", case_intra_frame_queue},
    {"matrix", "two_instances", case_two_instances},
    {"overlap", "switch_overlap", case_switch_overlap},
    {"overlap", "level_change_history", case_level_change_history},
    {"grouping", "mixed_block_types", case_mixed_block_types},
    {"grouping", "imdct_budget", case_imdct_budget},
    {"compatibility", "normal_mode_baseline", case_normal_mode_baseline},
    {"compatibility", "mode_gating", case_mode_gating}
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
