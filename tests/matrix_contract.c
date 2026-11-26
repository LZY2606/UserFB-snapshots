/*
 * matrix_contract.c
 *
 * Contract tests for the liba52 matrix monitoring mode. This file never
 * defines main(): task_support/driver.c drives the registered cases.
 *
 * The independent reference is a second, ordinary full-channel decoder
 * (A52_3F2R, automatic level adjustment disabled) fed the same frames with
 * the same level, DRC settings and zero bias. Its per-source PCM planes are
 * combined in the time domain with the test's own matrix, and the output
 * bias is added exactly once - independently of the production matrix
 * synthesis path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "check.h"
#include "a52.h"

#ifdef A52_MATRIX_TEST
#include "a52_internal.h"
#endif

#define FIX_LFE "task_support/fixtures/surround_lfe.ac3"
#define FIX_PLAIN "task_support/fixtures/surround.ac3"
#define NBLOCKS 6
#define PLANES 256

/* asymmetric matrices mixing positive and negative coefficients */
static const sample_t MAT_A[2][6] = {
    { 1.00f, -0.50f,  0.25f, -1.25f,  0.75f,  0.50f},
    {-0.75f,  1.50f, -1.00f,  0.50f, -0.25f, -1.50f}
};
static const sample_t MAT_B[2][6] = {
    {-1.50f,  0.25f,  1.75f, -0.50f,  1.25f, -0.75f},
    { 0.50f, -1.75f,  0.00f,  1.50f, -1.00f,  2.00f}
};
static const sample_t MAT_ZERO[2][6] = {
    { 0.00f,  0.00f,  0.00f,  0.00f,  0.00f,  0.00f},
    {-0.75f,  1.50f, -1.00f,  0.50f, -0.25f, -1.50f}
};
static const sample_t MAT_MONO[1][6] = {
    { 0.80f, -0.60f,  1.20f, -0.40f,  0.90f, -1.10f}
};

struct fixture {
    uint8_t *buf;
    size_t size;
    int lfe;
};

static int load_fixture (struct fixture * fx, const char * path, int lfe)
{
    FILE * f = fopen (path, "rb");
    long size;

    if (f == NULL)
	return 0;
    if (fseek (f, 0, SEEK_END) != 0) {
	fclose (f);
	return 0;
    }
    size = ftell (f);
    rewind (f);
    if (size <= 0) {
	fclose (f);
	return 0;
    }
    fx->buf = malloc ((size_t) size + 16);
    if (fx->buf == NULL) {
	fclose (f);
	return 0;
    }
    if (fread (fx->buf, 1, (size_t) size, f) != (size_t) size) {
	free (fx->buf);
	fclose (f);
	return 0;
    }
    fclose (f);
    fx->size = (size_t) size;
    fx->lfe = lfe;
    return 1;
}

static void free_fixture (struct fixture * fx)
{
    free (fx->buf);
    fx->buf = NULL;
}

/* independent full-channel reference decoder */
typedef struct {
    a52_state_t * s;
    int lfe;
} ref_t;

static int ref_open (ref_t * r, int lfe)
{
    r->lfe = lfe;
    r->s = a52_init (0);
    if (r->s == NULL)
	return 0;
    a52_dynrng (r->s, NULL, NULL);	/* DRC off unless a test re-enables */
    return 1;
}

static void ref_close (ref_t * r)
{
    a52_free (r->s);
}

static int ref_begin_frame (ref_t * r, uint8_t * buf, sample_t level,
			    int drc, sample_t (* cb) (sample_t, void *))
{
    int flags = A52_3F2R | (r->lfe ? A52_LFE : 0);
    sample_t lev = level;

    if (a52_frame (r->s, buf, &flags, &lev, 0) != 0)
	return 0;
    if (drc)
	a52_dynrng (r->s, cb, NULL);
    else
	a52_dynrng (r->s, NULL, NULL);
    return 1;
}

static const sample_t * ref_fbw (ref_t * r, int ch)
{
    return a52_samples (r->s) + (r->lfe ? 256 : 0) + 256 * ch;
}

static const sample_t * ref_lfe (ref_t * r)
{
    return a52_samples (r->s);
}

static sample_t drc_sqrt (sample_t range, void * data)
{
    (void) data;
    return (sample_t) sqrt ((double) range);
}

/*
 * Compare one matrix output block against the time-domain reference.
 * Tracks the largest absolute reference sample through *peak.
 */
static int check_block (a52_state_t * m, ref_t * r,
			const sample_t (* mat)[6], unsigned rows,
			sample_t bias, double * peak)
{
    const sample_t * out = a52_samples (m);
    const sample_t * src[5];
    const sample_t * lfe;
    unsigned row, ch, n;

    for (ch = 0; ch < 5; ch++)
	src[ch] = ref_fbw (r, ch);
    lfe = r->lfe ? ref_lfe (r) : NULL;

    for (row = 0; row < rows; row++) {
	for (n = 0; n < PLANES; n++) {
	    double expected = (double) bias;
	    double got, err, tol;

	    for (ch = 0; ch < 5; ch++)
		expected += (double) mat[row][ch] * (double) src[ch][n];
	    if (lfe != NULL)
		expected += (double) mat[row][5] * (double) lfe[n];
	    got = (double) out[256 * row + n];
	    err = fabs (got - expected);
	    tol = 3e-5 * (1.0 + fabs (expected));
	    if (fabs (expected) > *peak)
		*peak = fabs (expected);
	    TASK_CHECK (isfinite (got));
	    TASK_CHECK (err <= tol);
	}
    }
    return 0;
}

static int matrix_begin (a52_state_t * m, uint8_t * buf, sample_t level,
			 sample_t bias, int drc,
			 sample_t (* cb) (sample_t, void *),
			 int * returned_flags)
{
    int flags = A52_3F2R | A52_LFE;
    sample_t lev = level;

    if (a52_frame (m, buf, &flags, &lev, bias) != 0)
	return 0;
    if (drc)
	a52_dynrng (m, cb, NULL);
    else
	a52_dynrng (m, NULL, NULL);
    if (returned_flags != NULL)
	*returned_flags = flags;
    return 1;
}

/* ---- matrix group: API contract ----------------------------------- */

static int case_contract (void)
{
    struct fixture fx;
    a52_state_t * m, * m2;
    sample_t bad_nan[12], bad_inf[12], bad_big[12], edge[12];
    unsigned i;
    int flags = A52_3F2R | A52_LFE;
    sample_t lev = 1.0f;
    int len, sr, br;

    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    m = a52_init (0);
    TASK_CHECK (m != NULL);

    TASK_CHECK (a52_set_matrix (NULL, &MAT_A[0][0], 2) == 1);
    TASK_CHECK (a52_set_matrix (m, NULL, 2) == 1);
    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 0) == 1);
    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 3) == 1);

    for (i = 0; i < 12; i++) {
	bad_nan[i] = bad_inf[i] = bad_big[i] = edge[i] = 0.0f;
    }
    bad_nan[7] = (sample_t) NAN;
    bad_inf[3] = (sample_t) INFINITY;
    bad_big[10] = 2.0001f;
    edge[5] = -2.0f;
    edge[11] = 2.0f;

    /* invalid arguments never enable matrix mode on a fresh instance */
    TASK_CHECK (a52_set_matrix (m, bad_nan, 2) == 1);
    TASK_CHECK (a52_set_matrix (m, bad_inf, 2) == 1);
    TASK_CHECK (a52_set_matrix (m, bad_big, 2) == 1);

    /* enabling after the first frame must fail and leave normal mode */
    len = a52_syncinfo (fx.buf, &flags, &sr, &br);
    TASK_CHECK (len > 0);
    flags = A52_3F2R | A52_LFE;
    TASK_CHECK (a52_frame (m, fx.buf, &flags, &lev, 0) == 0);
    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 2) == 1);
    TASK_CHECK (a52_block (m) == 0);	/* normal decode still works */
    a52_free (m);

    /* boundary coefficients +/-2 are accepted and enable the mode */
    m2 = a52_init (0);
    TASK_CHECK (m2 != NULL);
    TASK_CHECK (a52_set_matrix (m2, edge, 2) == 0);
    {
	int returned;
	TASK_CHECK (matrix_begin (m2, fx.buf, 1.0f, 0, 0, NULL, &returned));
	TASK_CHECK (returned == A52_STEREO);
	TASK_CHECK (a52_block (m2) == 0);
    }
    /* once enabled, an invalid update leaves the queued/active matrix */
    TASK_CHECK (a52_set_matrix (m2, bad_nan, 2) == 1);
    a52_free (m2);

    free_fixture (&fx);
    return 0;
}

static int case_ownership (void)
{
    struct fixture fx;
    a52_state_t * m;
    ref_t r;
    sample_t local[2][6];
    sample_t bias = 1.25f;
    double peak = 0;
    int fl, sr, br, len, b;

    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    TASK_CHECK (ref_open (&r, 1));
    m = a52_init (0);
    TASK_CHECK (m != NULL);

    memcpy (local, MAT_A, sizeof (local));
    TASK_CHECK (a52_set_matrix (m, &local[0][0], 2) == 0);
    /* corrupt the caller's copy: the instance must keep its own */
    memset (local, 0x55, sizeof (local));

    len = a52_syncinfo (fx.buf, &fl, &sr, &br);
    TASK_CHECK (len > 0);
    TASK_CHECK (matrix_begin (m, fx.buf, 1.0f, bias, 0, NULL, NULL));
    TASK_CHECK (ref_begin_frame (&r, fx.buf, 1.0f, 0, NULL));
    for (b = 0; b < NBLOCKS; b++) {
	TASK_CHECK (a52_block (m) == 0);
	TASK_CHECK (a52_block (r.s) == 0);
	TASK_CHECK (check_block (m, &r, MAT_A, 2, bias, &peak) == 0);
    }
    TASK_CHECK (peak > 0.1);

    a52_free (m);
    ref_close (&r);
    free_fixture (&fx);
    return 0;
}

static int case_interleave (void)
{
    struct fixture fx;
    a52_state_t * m1, * m2;
    ref_t r1, r2;
    sample_t bias1 = 0.5f, bias2 = -0.25f;
    double peak1 = 0, peak2 = 0;
    long pos = 0;
    int fl, sr, br, len, b;

    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    TASK_CHECK (ref_open (&r1, 1));
    TASK_CHECK (ref_open (&r2, 1));
    m1 = a52_init (0);
    m2 = a52_init (0);
    TASK_CHECK (m1 != NULL && m2 != NULL);
    TASK_CHECK (a52_set_matrix (m1, &MAT_A[0][0], 2) == 0);
    TASK_CHECK (a52_set_matrix (m2, &MAT_B[0][0], 2) == 0);

    while (pos < (long) fx.size) {
	len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	TASK_CHECK (len > 0 && pos + len <= (long) fx.size);
	TASK_CHECK (matrix_begin (m1, fx.buf + pos, 1.0f, bias1, 0,
				  NULL, NULL));
	TASK_CHECK (matrix_begin (m2, fx.buf + pos, 0.8f, bias2, 0,
				  NULL, NULL));
	TASK_CHECK (ref_begin_frame (&r1, fx.buf + pos, 1.0f, 0, NULL));
	TASK_CHECK (ref_begin_frame (&r2, fx.buf + pos, 0.8f, 0, NULL));
	for (b = 0; b < NBLOCKS; b++) {
	    TASK_CHECK (a52_block (m1) == 0);
	    TASK_CHECK (a52_block (r1.s) == 0);
	    TASK_CHECK (check_block (m1, &r1, MAT_A, 2, bias1, &peak1) == 0);
	    TASK_CHECK (a52_block (m2) == 0);
	    TASK_CHECK (a52_block (r2.s) == 0);
	    TASK_CHECK (check_block (m2, &r2, MAT_B, 2, bias2, &peak2) == 0);
	}
	pos += len;
    }
    TASK_CHECK (peak1 > 0.1 && peak2 > 0.1);

    a52_free (m1);
    a52_free (m2);
    ref_close (&r1);
    ref_close (&r2);
    free_fixture (&fx);
    return 0;
}

static int case_invalid_queued (void)
{
    struct fixture fx;
    a52_state_t * m;
    ref_t r;
    sample_t bad[12];
    sample_t bias = 0.0f;
    double peak = 0;
    long pos = 0;
    int fl, sr, br, len, frame = 0, b;

    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    TASK_CHECK (ref_open (&r, 1));
    m = a52_init (0);
    TASK_CHECK (m != NULL);
    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 2) == 0);

    for (unsigned i = 0; i < 12; i++)
	bad[i] = 0.0f;
    bad[4] = (sample_t) NAN;

    while (pos < (long) fx.size) {
	const sample_t (* mat)[6];

	len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	TASK_CHECK (len > 0);
	if (frame == 2) {
	    /* invalid update: neither queued nor active matrix moves */
	    TASK_CHECK (a52_set_matrix (m, bad, 2) == 1);
	}
	mat = (frame < 4) ? MAT_A : MAT_B;
	TASK_CHECK (matrix_begin (m, fx.buf + pos, 1.0f, bias, 0,
				  NULL, NULL));
	TASK_CHECK (ref_begin_frame (&r, fx.buf + pos, 1.0f, 0, NULL));
	if (frame == 3) {
	    /* valid update queued inside frame 3: commits at frame 4 */
	    TASK_CHECK (a52_set_matrix (m, &MAT_B[0][0], 2) == 0);
	}
	for (b = 0; b < NBLOCKS; b++) {
	    TASK_CHECK (a52_block (m) == 0);
	    TASK_CHECK (a52_block (r.s) == 0);
	    TASK_CHECK (check_block (m, &r, mat, 2, bias, &peak) == 0);
	}
	pos += len;
	frame++;
    }
    TASK_CHECK (frame >= 8);
    TASK_CHECK (peak > 0.1);

    a52_free (m);
    ref_close (&r);
    free_fixture (&fx);
    return 0;
}

static int case_intra_frame_queue (void)
{
    struct fixture fx;
    a52_state_t * m;
    ref_t r;
    sample_t bias = 0.75f;
    double peak = 0;
    long pos = 0;
    int fl, sr, br, len, frame = 0, b;

    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    TASK_CHECK (ref_open (&r, 1));
    m = a52_init (0);
    TASK_CHECK (m != NULL);
    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 2) == 0);

    while (pos < (long) fx.size) {
	const sample_t (* mat)[6];

	len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	TASK_CHECK (len > 0);
	mat = (frame < 2) ? MAT_A : MAT_B;
	TASK_CHECK (matrix_begin (m, fx.buf + pos, 1.0f, bias, 0,
				  NULL, NULL));
	TASK_CHECK (ref_begin_frame (&r, fx.buf + pos, 1.0f, 0, NULL));
	if (frame == 1) {
	    /* queue B in the middle of frame 1: its blocks still use A */
	    TASK_CHECK (a52_set_matrix (m, &MAT_B[0][0], 2) == 0);
	}
	for (b = 0; b < NBLOCKS; b++) {
	    TASK_CHECK (a52_block (m) == 0);
	    TASK_CHECK (a52_block (r.s) == 0);
	    TASK_CHECK (check_block (m, &r, mat, 2, bias, &peak) == 0);
	}
	pos += len;
	frame++;
    }
    TASK_CHECK (peak > 0.1);

    a52_free (m);
    ref_close (&r);
    free_fixture (&fx);
    return 0;
}

static int case_zero_and_mono (void)
{
    struct fixture fx;
    a52_state_t * m2, * m1;
    ref_t r;
    sample_t bias = 2.0f;
    double peak = 0;
    int fl, sr, br, len, returned, b, n;

    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    TASK_CHECK (ref_open (&r, 1));
    m2 = a52_init (0);
    m1 = a52_init (0);
    TASK_CHECK (m2 != NULL && m1 != NULL);
    TASK_CHECK (a52_set_matrix (m2, &MAT_ZERO[0][0], 2) == 0);
    TASK_CHECK (a52_set_matrix (m1, &MAT_MONO[0][0], 1) == 0);

    len = a52_syncinfo (fx.buf, &fl, &sr, &br);
    TASK_CHECK (len > 0);
    TASK_CHECK (matrix_begin (m2, fx.buf, 1.0f, bias, 0, NULL, &returned));
    TASK_CHECK (returned == A52_STEREO);
    TASK_CHECK (matrix_begin (m1, fx.buf, 1.0f, bias, 0, NULL, &returned));
    TASK_CHECK (returned == A52_MONO);
    TASK_CHECK (ref_begin_frame (&r, fx.buf, 1.0f, 0, NULL));

    for (b = 0; b < NBLOCKS; b++) {
	const sample_t * out2;

	TASK_CHECK (a52_block (m2) == 0);
	TASK_CHECK (a52_block (m1) == 0);
	TASK_CHECK (a52_block (r.s) == 0);
	out2 = a52_samples (m2);
	/* whole zero row: bias is the only contribution */
	for (n = 0; n < PLANES; n++)
	    TASK_CHECK (fabs ((double) out2[n] - (double) bias) <= 1e-6);
	TASK_CHECK (check_block (m2, &r, MAT_ZERO, 2, bias, &peak) == 0);
	TASK_CHECK (check_block (m1, &r, MAT_MONO, 1, bias, &peak) == 0);
    }
    TASK_CHECK (peak > 0.1);

    a52_free (m2);
    a52_free (m1);
    ref_close (&r);
    free_fixture (&fx);
    return 0;
}

/* ---- overlap group: matrix switches at non-silent boundaries ------ */

static int run_switch (const char * path, int lfe, int drc,
		       sample_t level, sample_t bias,
		       sample_t (* cb) (sample_t, void *))
{
    struct fixture fx;
    a52_state_t * m;
    ref_t r;
    double peak = 0;
    long pos = 0;
    int fl, sr, br, len, frame = 0, b;
    int boundary_block_seen = 0;

    TASK_CHECK (load_fixture (&fx, path, lfe));
    TASK_CHECK (ref_open (&r, lfe));
    m = a52_init (0);
    TASK_CHECK (m != NULL);
    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 2) == 0);

    while (pos < (long) fx.size) {
	const sample_t (* mat)[6];
	int returned;

	len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	TASK_CHECK (len > 0 && pos + len <= (long) fx.size);
	mat = (frame < 4) ? MAT_A : MAT_B;
	TASK_CHECK (matrix_begin (m, fx.buf + pos, level, bias, drc, cb,
				  &returned));
	TASK_CHECK (returned == A52_STEREO);
	TASK_CHECK ((returned & A52_LFE) == 0);
	TASK_CHECK (ref_begin_frame (&r, fx.buf + pos, level, drc, cb));
	for (b = 0; b < NBLOCKS; b++) {
	    TASK_CHECK (a52_block (m) == 0);
	    TASK_CHECK (a52_block (r.s) == 0);
	    TASK_CHECK (check_block (m, &r, mat, 2, bias, &peak) == 0);
	    if (frame == 3 && b == NBLOCKS - 1)
		boundary_block_seen++;
	    if (frame == 4 && b == 0) {
		double energy = 0;
		const sample_t * out = a52_samples (m);
		for (int n = 0; n < PLANES; n++)
		    energy += (double) out[n] * (double) out[n];
		/* first block with the new matrix is produced, not skipped */
		TASK_CHECK (energy > 1e-6);
		boundary_block_seen++;
	    }
	}
	if (frame == 3)
	    TASK_CHECK (a52_set_matrix (m, &MAT_B[0][0], 2) == 0);
	pos += len;
	frame++;
    }
    TASK_CHECK (frame >= 8);
    TASK_CHECK (boundary_block_seen == 2);
    TASK_CHECK (peak > 0.1);

    a52_free (m);
    ref_close (&r);
    free_fixture (&fx);
    return 0;
}

static int case_switch_lfe (void)
{
    return run_switch (FIX_LFE, 1, 0, 1.0f, 1.25f, NULL);
}

static int case_switch_nolfe_drc (void)
{
    /* no-LFE source, DRC callback enabled, non-unit level, nonzero bias */
    return run_switch (FIX_PLAIN, 0, 1, 0.7f, -0.5f, drc_sqrt);
}

static int case_row_count_switch (void)
{
    struct fixture fx;
    a52_state_t * m;
    ref_t r;
    sample_t bias = 0.9f;
    double peak = 0;
    long pos = 0;
    int fl, sr, br, len, frame = 0, b, returned;

    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    TASK_CHECK (ref_open (&r, 1));
    m = a52_init (0);
    TASK_CHECK (m != NULL);
    TASK_CHECK (a52_set_matrix (m, &MAT_MONO[0][0], 1) == 0);

    while (pos < (long) fx.size) {
	unsigned rows;
	const sample_t (* mat)[6];

	len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	TASK_CHECK (len > 0);
	if (frame == 3)
	    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 2) == 0);
	if (frame == 7)
	    TASK_CHECK (a52_set_matrix (m, &MAT_MONO[0][0], 1) == 0);
	rows = (frame >= 3 && frame < 7) ? 2 : 1;
	mat = (rows == 2) ? MAT_A : MAT_MONO;
	TASK_CHECK (matrix_begin (m, fx.buf + pos, 1.0f, bias, 0, NULL,
				  &returned));
	TASK_CHECK (returned == (rows == 2 ? A52_STEREO : A52_MONO));
	TASK_CHECK ((returned & A52_LFE) == 0);
	TASK_CHECK (ref_begin_frame (&r, fx.buf + pos, 1.0f, 0, NULL));
	for (b = 0; b < NBLOCKS; b++) {
#ifdef A52_MATRIX_TEST
	    unsigned c5 = a52_test_imdct_512_calls (m);
	    unsigned c2 = a52_test_imdct_256_calls (m);
#endif
	    TASK_CHECK (a52_block (m) == 0);
	    TASK_CHECK (a52_block (r.s) == 0);
	    TASK_CHECK (check_block (m, &r, mat, rows, bias, &peak) == 0);
#ifdef A52_MATRIX_TEST
	    /* steady mono long blocks after the 2->1 switch: exactly one
	       long IMDCT per block, no hidden full-channel transforms */
	    if (frame >= 8 && b >= 1) {
		TASK_CHECK (a52_test_imdct_512_calls (m) - c5 == 1);
		TASK_CHECK (a52_test_imdct_256_calls (m) - c2 == 0);
	    }
#endif
	}
	pos += len;
	frame++;
    }
    TASK_CHECK (frame >= 8);
    TASK_CHECK (peak > 0.1);

    a52_free (m);
    ref_close (&r);
    free_fixture (&fx);
    return 0;
}

static int case_lfe_column_inert (void)
{
    struct fixture fx;
    a52_state_t * m, * m2;
    static sample_t mat_col6[2][6];
    static sample_t mat_zero6[2][6];
    long pos = 0;
    int fl, sr, br, len, b, n;

    memcpy (mat_col6, MAT_A, sizeof (mat_col6));
    memcpy (mat_zero6, MAT_A, sizeof (mat_zero6));
    for (unsigned row = 0; row < 2; row++) {
	mat_col6[row][5] = 2.0f;	/* extreme sixth column */
	mat_zero6[row][5] = 0.0f;
    }

    TASK_CHECK (load_fixture (&fx, FIX_PLAIN, 0));
    m = a52_init (0);
    m2 = a52_init (0);
    TASK_CHECK (m != NULL && m2 != NULL);
    TASK_CHECK (a52_set_matrix (m, &mat_col6[0][0], 2) == 0);
    TASK_CHECK (a52_set_matrix (m2, &mat_zero6[0][0], 2) == 0);

    while (pos < (long) fx.size) {
	len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	TASK_CHECK (len > 0);
	TASK_CHECK (matrix_begin (m, fx.buf + pos, 1.0f, 0, 0, NULL, NULL));
	TASK_CHECK (matrix_begin (m2, fx.buf + pos, 1.0f, 0, 0, NULL, NULL));
	for (b = 0; b < NBLOCKS; b++) {
	    TASK_CHECK (a52_block (m) == 0);
	    TASK_CHECK (a52_block (m2) == 0);
	    for (n = 0; n < 2 * PLANES; n++)
		TASK_CHECK (fabs ((double) a52_samples (m)[n] -
				  (double) a52_samples (m2)[n]) <= 1e-6);
	}
	pos += len;
    }

    a52_free (m);
    a52_free (m2);
    free_fixture (&fx);
    return 0;
}

/* ---- grouping group ------------------------------------------------ */

static int case_grouping_api (void)
{
    struct fixture fx;
    a52_state_t * m;
    ref_t r;
    sample_t bias = 0.3f;
    double peak = 0;
    long pos = 0;
    int fl, sr, br, len, b;

    /* drives the whole production synthesis path on the real long-block
       fixture and checks every block against the independent decoder */
    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    TASK_CHECK (ref_open (&r, 1));
    m = a52_init (0);
    TASK_CHECK (m != NULL);
    TASK_CHECK (a52_set_matrix (m, &MAT_B[0][0], 2) == 0);

    while (pos < (long) fx.size) {
	len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	TASK_CHECK (len > 0);
	TASK_CHECK (matrix_begin (m, fx.buf + pos, 1.0f, bias, 0,
				  NULL, NULL));
	TASK_CHECK (ref_begin_frame (&r, fx.buf + pos, 1.0f, 0, NULL));
	for (b = 0; b < NBLOCKS; b++) {
	    TASK_CHECK (a52_block (m) == 0);
	    TASK_CHECK (a52_block (r.s) == 0);
	    TASK_CHECK (check_block (m, &r, MAT_B, 2, bias, &peak) == 0);
#ifdef A52_MATRIX_TEST
	{
	    const uint8_t * bs = a52_test_prev_blksw (m);
	    for (int c = 0; c < 5; c++) {
		const sample_t * sp = a52_test_prev_spectrum (m, c);
		TASK_CHECK (bs[c] == 0 || bs[c] == 1);
		for (int k = 0; k < PLANES; k += 31)
		    TASK_CHECK (isfinite (sp[k]));
		TASK_CHECK (isfinite (a52_test_lfe_spectrum (m)[7]));
	    }
	}
#endif
	}
	pos += len;
    }
    TASK_CHECK (peak > 0.1);

    a52_free (m);
    ref_close (&r);
    free_fixture (&fx);
    return 0;
}

#ifdef A52_MATRIX_TEST

/*
 * Independent per-source time-domain reference: one delay line per source
 * channel, exactly like the ordinary per-channel decoder, driven with the
 * same production IMDCT primitives but separate bookkeeping.
 */
static sample_t g_delay[6][PLANES];
static sample_t g_pcm[6][PLANES];

static void ref_synth (int ch, const sample_t * spec, int blksw, int lfe)
{
    sample_t data[PLANES];

    memcpy (data, spec, sizeof (data));
    if (lfe || !blksw)
	a52_imdct_512 (data, g_delay[ch], 0);
    else
	a52_imdct_256 (data, g_delay[ch], 0);
    memcpy (g_pcm[ch], data, sizeof (data));
}

static void make_spec (sample_t spec[6][PLANES], int block)
{
    int c, k;

    for (c = 0; c < 6; c++)
	for (k = 0; k < PLANES; k++)
	    spec[c][k] = (sample_t)
		(0.05 * sin ((block + 1) * (c + 3) * (k + 2) * 0.013) +
		 0.02 * cos ((block + 2) * (c + 1) * (k + 1) * 0.029));
}

static int case_grouping_inject (void)
{
    static const int patterns[18][5] = {
	{0, 0, 0, 0, 0}, {1, 1, 1, 1, 1}, {1, 0, 1, 0, 1},
	{0, 1, 0, 1, 0}, {0, 0, 0, 0, 0}, {1, 1, 1, 1, 1},
	{0, 0, 0, 0, 0}, {0, 1, 0, 1, 0}, {1, 1, 1, 1, 1},
	{0, 0, 0, 0, 0}, {1, 0, 1, 0, 1}, {1, 1, 1, 1, 1},
	{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}
    };
    struct fixture fx;
    a52_state_t * m;
    sample_t spec[6][PLANES];
    sample_t bias = 0.4f;
    long pos = 0;
    int fl, sr, br, len;
    int block = 0, frame, b;
    unsigned calls512_before, calls256_before, d512, d256;

    memset (g_delay, 0, sizeof (g_delay));
    TASK_CHECK (load_fixture (&fx, FIX_LFE, 1));
    m = a52_init (0);
    TASK_CHECK (m != NULL);
    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 2) == 0);

    /* 3 frames x 6 blocks; matrix B is queued during frame 0 */
    for (frame = 0; frame < 3 && pos < (long) fx.size; frame++) {
	const sample_t (* mat)[6] = (frame < 1) ? MAT_A : MAT_B;

	len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	TASK_CHECK (len > 0);
	TASK_CHECK (matrix_begin (m, fx.buf + pos, 1.0f, bias, 0,
				  NULL, NULL));
	if (frame == 0)
	    TASK_CHECK (a52_set_matrix (m, &MAT_B[0][0], 2) == 0);
	for (b = 0; b < NBLOCKS; b++, block++) {
	    calls512_before = a52_test_imdct_512_calls (m);
	    calls256_before = a52_test_imdct_256_calls (m);

	    make_spec (spec, block);
	    /* LFE stays present on every block; blksw patterns vary */
	    a52_test_inject (m, &spec[0][0], patterns[block], 1);
	    TASK_CHECK (a52_block (m) == 0);
	    d512 = a52_test_imdct_512_calls (m) - calls512_before;
	    d256 = a52_test_imdct_256_calls (m) - calls256_before;

	    /* independent per-source synthesis of the same spectra */
	    for (int c = 0; c < 5; c++)
		ref_synth (c, spec[c], patterns[block][c], 0);
	    ref_synth (5, spec[5], 0, 1);

	    for (int row = 0; row < 2; row++) {
		for (int n = 0; n < PLANES; n++) {
		    double expected = (double) bias;
		    double got;

		    for (int c = 0; c < 5; c++)
			expected += (double) mat[row][c] *
			    (double) g_pcm[c][n];
		    expected += (double) mat[row][5] *
			(double) g_pcm[5][n];
		    got = (double) a52_samples (m)[256 * row + n];
		    TASK_CHECK (isfinite (got));
		    TASK_CHECK (fabs (got - expected) <=
				3e-5 * (1.0 + fabs (expected)));
		}
	    }

	    /* observed parsed spectra are exactly the injected ones */
	    for (int c = 0; c < 5; c++) {
		const sample_t * observed = a52_test_prev_spectrum (m, c);
		double maxerr = 0;
		for (int k = 0; k < PLANES; k++) {
		    double e = fabs ((double) observed[k] -
				     (double) spec[c][k]);
		    if (e > maxerr)
			maxerr = e;
		}
		TASK_CHECK (maxerr == 0.0);
		TASK_CHECK (a52_test_prev_blksw (m)[c] ==
			    (uint8_t) patterns[block][c]);
	    }

	    /* IMDCT accounting */
	    if (block == 0) {			/* all-long, no rebuild */
		TASK_CHECK (d512 == 2);
		TASK_CHECK (d256 == 0);
	    }
	    if (block == 1) {			/* all-long -> all-short */
		TASK_CHECK (d512 == 8);	/* 6 rebuild + 2 row long (LFE) */
		TASK_CHECK (d256 == 2);
	    }
	    if (block == 2) {			/* all-short -> mixed */
		TASK_CHECK (d512 == 3);	/* 1 LFE rebuild + 2 row long */
		TASK_CHECK (d256 == 7);	/* 5 fbw rebuild + 2 row short */
	    }
	    if (block >= 13) {			/* steady all-long tail */
		TASK_CHECK (d512 == 2);
		TASK_CHECK (d256 == 0);
	    }
	    TASK_CHECK (d512 + d256 <= 10);
	}
	pos += len;
    }
    TASK_CHECK (block == 18);

    a52_free (m);
    free_fixture (&fx);
    return 0;
}

#endif /* A52_MATRIX_TEST */

/* ---- compatibility group: ordinary decoder stays identical -------- */

static int case_normal_downmix (void)
{
    static const double cm[4] = {
	0.7071067811865476, 0.5946035575013605, 0.5, 0.5946035575013605
    };
    static const double sm[4] = {
	0.7071067811865476, 0.5, 0.0, 0.5
    };
    const char * paths[2] = { FIX_LFE, FIX_PLAIN };
    const int lfes[2] = { 1, 0 };

    for (int t = 0; t < 2; t++) {
	struct fixture fx;
	a52_state_t * full, * stereo;
	long pos = 0;
	int fl, sr, br, len;
	double c, s;

	TASK_CHECK (load_fixture (&fx, paths[t], lfes[t]));
	full = a52_init (0);
	stereo = a52_init (0);
	TASK_CHECK (full != NULL && stereo != NULL);

	while (pos < (long) fx.size) {
	    int a = A52_3F2R | (lfes[t] ? A52_LFE : 0);
	    int b = A52_STEREO;
	    sample_t lev = 1.0f, levb = 1.0f;
	    int off = lfes[t] ? 256 : 0;

	    len = a52_syncinfo (fx.buf + pos, &fl, &sr, &br);
	    TASK_CHECK (len > 0 && sr == 48000);
	    TASK_CHECK (a52_frame (full, fx.buf + pos, &a, &lev, 0) == 0);
	    TASK_CHECK (a52_frame (stereo, fx.buf + pos, &b, &levb, 0) == 0);
	    TASK_CHECK (b == A52_STEREO);
	    a52_dynrng (full, NULL, NULL);
	    a52_dynrng (stereo, NULL, NULL);
	    c = cm[(fx.buf[pos + 6] >> 3) & 3];
	    s = sm[(fx.buf[pos + 6] >> 1) & 3];
	    for (int k = 0; k < NBLOCKS; k++) {
		TASK_CHECK (a52_block (full) == 0);
		TASK_CHECK (a52_block (stereo) == 0);
		for (int n = 0; n < PLANES; n++) {
		    const sample_t * x = a52_samples (full);
		    const sample_t * y = a52_samples (stereo);
		    double left = x[off + n] + c * x[off + 256 + n] +
				  s * x[off + 768 + n];
		    double right = x[off + 512 + n] +
				   c * x[off + 256 + n] +
				   s * x[off + 1024 + n];
		    TASK_CHECK (fabs ((double) y[n] - left) <=
				3e-5 * (1.0 + fabs (left)));
		    TASK_CHECK (fabs ((double) y[256 + n] - right) <=
				3e-5 * (1.0 + fabs (right)));
		}
	    }
	    pos += len;
	}
	a52_free (full);
	a52_free (stereo);
	free_fixture (&fx);
    }
    return 0;
}

static int case_rejected_enable_keeps_normal (void)
{
    struct fixture fx;
    a52_state_t * m;
    int fl, sr, br, len;
    sample_t lev = 1.0f;

    TASK_CHECK (load_fixture (&fx, FIX_PLAIN, 0));
    m = a52_init (0);
    TASK_CHECK (m != NULL);
    len = a52_syncinfo (fx.buf, &fl, &sr, &br);
    TASK_CHECK (len > 0);
    fl = A52_STEREO;
    TASK_CHECK (a52_frame (m, fx.buf, &fl, &lev, 0) == 0);
    TASK_CHECK (fl == A52_STEREO);
    TASK_CHECK (a52_set_matrix (m, &MAT_A[0][0], 2) == 1);
    TASK_CHECK (a52_block (m) == 0);
    /* ordinary layout and channel count are untouched */
    TASK_CHECK (isfinite (a52_samples (m)[0]));
    TASK_CHECK (isfinite (a52_samples (m)[255]));
    a52_free (m);
    free_fixture (&fx);
    return 0;
}

/* ---- registration -------------------------------------------------- */

const struct task_case task_cases[] = {
    {"matrix", "contract", case_contract},
    {"matrix", "ownership", case_ownership},
    {"matrix", "interleave", case_interleave},
    {"matrix", "invalid_queued", case_invalid_queued},
    {"matrix", "intra_frame_queue", case_intra_frame_queue},
    {"matrix", "zero_row_and_mono", case_zero_and_mono},
    {"matrix", "row_count_switch", case_row_count_switch},
    {"overlap", "switch_lfe", case_switch_lfe},
    {"overlap", "switch_nolfe_drc", case_switch_nolfe_drc},
    {"overlap", "lfe_column_inert", case_lfe_column_inert},
    {"grouping", "grouping_api", case_grouping_api},
#ifdef A52_MATRIX_TEST
    {"grouping", "grouping_inject", case_grouping_inject},
#endif
    {"compatibility", "normal_downmix", case_normal_downmix},
    {"compatibility", "rejected_enable_keeps_normal",
     case_rejected_enable_keeps_normal}
};
const size_t task_case_count =
    sizeof (task_cases) / sizeof (task_cases[0]);
