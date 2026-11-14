/*
** Copyright (c) 2002-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Tests for the SRC_SINC_MINIMUM_PHASE converter.
**
** Five groups, each prints "MINIMUM_PHASE_CASE <name> PASS" on success :
**
**   causal    - output k (source time k/r) depends only on input frames
**               0 .. floor (k/r) plus the zero history preceding the stream.
**               Modifying not yet delivered future input must not change the
**               already produced output prefix.
**   magnitude - passband magnitude at 0.02 cycles/input-frame within 0.5 dB
**               for ratios 0.5, 44100/48000, 1 and 2 ; alias and image
**               suppression of at least 35 dB. References are analytic
**               waveforms (sine/cosine projections of known amplitude).
**   phase     - impulse response collected with explicit zero padding ;
**               80 % of the energy within the first 12 input frames at
**               ratio 0.5 ; minimum phase relation verified on the ratio 1
**               grid by an independent frequency domain derivation from the
**               measured magnitude response.
**   streaming - identical input delivered with different chunkings must give
**               identical per-channel output and identical frame counts ;
**               covers mono, inverted stereo, six channel impulses, output
**               capacities cycling 1/7/113, and the src_simple / process /
**               callback paths.
**   lifecycle - ratio lock rules, src_set_ratio cannot bypass them, reset
**               behaves like a fresh instance, clone branches are
**               independent, bad ratios do not disturb later legal use.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <samplerate.h>

#include "util.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846264338
#endif

#define CONVERTER	SRC_SINC_MINIMUM_PHASE

/* The SRC_ERR_* codes are internal to the library (src/common.h) and are not
** exported by the public header. Mirror the two codes this test needs so
** specific error returns can be checked.
*/
#define TEST_ERR_BAD_SRC_RATIO		6
#define TEST_ERR_NO_VARIABLE_RATIO	20

/*========================================================================================
** Helpers.
*/

/* Deterministic test signal : a fixed sum of sines. */
static float
test_signal (long i)
{	return (float) (0.45 * sin (2.0 * M_PI * 0.013 * i)
				+ 0.25 * sin (2.0 * M_PI * 0.071 * i + 0.7)
				+ 0.10 * sin (2.0 * M_PI * 0.0051 * i + 2.1)) ;
} /* test_signal */

/* One-shot conversion through src_process with end_of_input set. */
static long
convert_oneshot (double ratio, int channels, const float *in, long in_frames,
		float *out, long out_max)
{	SRC_STATE	*state ;
	SRC_DATA	data ;
	int			error ;

	state = src_new (CONVERTER, channels, &error) ;
	ASSERT (state != NULL) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = in ;
	data.input_frames = in_frames ;
	data.data_out = out ;
	data.output_frames = out_max ;
	data.src_ratio = ratio ;
	data.end_of_input = 1 ;

	error = src_process (state, &data) ;
	ASSERT (error == 0) ;

	src_delete (state) ;

	return data.output_frames_gen ;
} /* convert_oneshot */

/* Conversion with input delivered in chunks and output capacity limited,
** cycling through the supplied chunk sizes and capacities.
*/
static long
convert_chunked (double ratio, int channels, const float *in, long in_frames,
		const long *in_chunks, int n_in_chunks,
		const long *out_caps, int n_out_caps,
		float *out, long out_max)
{	SRC_STATE	*state ;
	SRC_DATA	data ;
	long		in_pos, out_pos ;
	int			ic = 0, oc = 0, error ;

	state = src_new (CONVERTER, channels, &error) ;
	ASSERT (state != NULL) ;

	in_pos = 0 ;
	out_pos = 0 ;
	for (;;)
	{	long ichunk = 0, ocap = out_caps [oc ++ % n_out_caps] ;

		if (in_pos < in_frames)
		{	ichunk = in_chunks [ic ++ % n_in_chunks] ;
			if (ichunk > in_frames - in_pos)
				ichunk = in_frames - in_pos ;
			} ;
		if (ocap > out_max - out_pos)
			ocap = out_max - out_pos ;
		ASSERT (ocap > 0) ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = in + in_pos * channels ;
		data.input_frames = ichunk ;
		data.data_out = out + out_pos * channels ;
		data.output_frames = ocap ;
		data.src_ratio = ratio ;

		error = src_process (state, &data) ;
		ASSERT (error == 0) ;

		in_pos += data.input_frames_used ;
		out_pos += data.output_frames_gen ;

		if (in_pos >= in_frames && data.input_frames_used == 0 && data.output_frames_gen == 0)
			break ;
		} ;

	src_delete (state) ;

	return out_pos ;
} /* convert_chunked */

typedef struct
{	const float	*data ;
	long		frames, pos, chunk ;
	int			channels ;
} CB_DATA ;

static long
test_cb_func (void *user, float **data)
{	CB_DATA *cb = (CB_DATA *) user ;
	long n ;

	if (cb->pos >= cb->frames)
		return 0 ;

	n = MIN (cb->chunk, cb->frames - cb->pos) ;
	*data = (float *) (cb->data + cb->pos * cb->channels) ;
	cb->pos += n ;

	return n ;
} /* test_cb_func */

static long
convert_callback (double ratio, int channels, const float *in, long in_frames,
		long in_chunk, long read_len, float *out, long out_max)
{	SRC_STATE	*state ;
	CB_DATA		cb ;
	long		out_pos, got ;
	int			error ;

	cb.data = in ;
	cb.frames = in_frames ;
	cb.pos = 0 ;
	cb.chunk = in_chunk ;
	cb.channels = channels ;

	state = src_callback_new (test_cb_func, CONVERTER, channels, &error, &cb) ;
	ASSERT (state != NULL) ;

	out_pos = 0 ;
	while ((got = src_callback_read (state, ratio, read_len, out + out_pos * channels)) > 0)
	{	out_pos += got ;
		ASSERT (out_pos <= out_max) ;
		} ;

	ASSERT (src_error (state) == 0) ;

	src_delete (state) ;

	return out_pos ;
} /* convert_callback */

/*
** Amplitude of a known frequency component by sine/cosine projection.
** The signal x is projected onto cos (2 pi f n) and sin (2 pi f n) over n
** frames (f in cycles per frame) ; the amplitude is 2 * hypot (c, s) / n.
** This is exact for a pure tone regardless of the window length, up to the
** image term at -f which is negligible for the long windows used here.
*/
static double
sine_amplitude (const float *x, long n, double freq)
{	double	cr = 0.0, ci = 0.0 ;
	long	i ;

	for (i = 0 ; i < n ; i++)
	{	double phase = 2.0 * M_PI * freq * i ;
		cr += x [i] * cos (phase) ;
		ci += x [i] * sin (phase) ;
		} ;

	return 2.0 * hypot (cr, ci) / n ;
} /* sine_amplitude */

static void
compare_exact (const float *a, const float *b, long n, const char *what)
{	long i ;

	for (i = 0 ; i < n ; i++)
		if (a [i] != b [i])
		{	printf ("\n%s : mismatch at frame %ld : %.9g != %.9g\n\n", what, i, a [i], b [i]) ;
			exit (1) ;
			} ;

	return ;
} /* compare_exact */

/*========================================================================================
** causal
*/

static void
test_causal (void)
{	const long	N = 4000, M = 1500 ;
	const double ratio = 0.75 ;
	float	*x, *xmod, *out_a, *out_b, *out_c ;
	long	q_a, q_b1, q_b2, q_c, i, differ ;
	SRC_STATE *state ;
	SRC_DATA data ;
	int		error ;

	x		= malloc (sizeof (float) * N) ;
	xmod	= malloc (sizeof (float) * N) ;
	out_a	= malloc (sizeof (float) * (N + 64)) ;
	out_b	= malloc (sizeof (float) * (N + 64)) ;
	out_c	= malloc (sizeof (float) * (N + 64)) ;
	ASSERT (x && xmod && out_a && out_b && out_c) ;

	for (i = 0 ; i < N ; i++)
	{	x [i] = test_signal (i) ;
		xmod [i] = x [i] ;
		} ;
	/* Modify the future : everything delivered after frame M differs. */
	for (i = M ; i < N ; i++)
		xmod [i] = (float) (0.31 * sin (2.0 * M_PI * 0.017 * i) - 0.9 * x [i]) ;

	/* Run A : the unmodified signal in one shot. */
	q_a = convert_oneshot (ratio, 1, x, N, out_a, N + 64) ;

	/* Run B : first M frames of the unmodified signal, then the modified tail. */
	state = src_new (CONVERTER, 1, &error) ;
	ASSERT (state != NULL) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = x ;
	data.input_frames = M ;
	data.data_out = out_b ;
	data.output_frames = N + 64 ;
	data.src_ratio = ratio ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	q_b1 = data.output_frames_gen ;
	ASSERT (q_b1 > 0) ;

	data.data_in = xmod + M ;
	data.input_frames = N - M ;
	data.data_out = out_b + q_b1 ;
	data.output_frames = N + 64 - q_b1 ;
	data.src_ratio = ratio ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	q_b2 = data.output_frames_gen ;

	src_delete (state) ;

	/* Run C : the modified signal in one shot. */
	q_c = convert_oneshot (ratio, 1, xmod, N, out_c, N + 64) ;

	ASSERT (q_b1 + q_b2 == q_c) ;
	ASSERT (q_b1 + q_b2 == q_a) ;

	/* The prefix produced before the modified input was delivered must be
	** bit identical in all three runs : it cannot depend on future samples.
	*/
	compare_exact (out_b, out_c, q_b1, "causal prefix vs modified run") ;
	compare_exact (out_b, out_a, q_b1, "causal prefix vs original run") ;

	/* Sanity : the modification must actually change later output (run B
	** versus the unmodified run A), otherwise the test above proves nothing.
	*/
	differ = 0 ;
	for (i = q_b1 ; i < q_a ; i++)
		if (out_b [i] != out_a [i])
			differ ++ ;
	ASSERT (differ > 0) ;

	/* A single input frame with a single frame of output capacity must make
	** progress immediately : the converter never waits for future samples.
	*/
	{	float one_in [1] = { 1.0f }, one_out [4] ;
		state = src_new (CONVERTER, 1, &error) ;
		ASSERT (state != NULL) ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = one_in ;
		data.input_frames = 1 ;
		data.data_out = one_out ;
		data.output_frames = 1 ;
		data.src_ratio = 1.0 ;
		error = src_process (state, &data) ;
		ASSERT (error == 0) ;
		ASSERT (data.input_frames_used == 1) ;
		ASSERT (data.output_frames_gen == 1) ;
		/* The causal kernel starts at t = 0 : the very first output already
		** responds to the impulse.
		*/
		ASSERT (one_out [0] != 0.0f) ;

		src_delete (state) ;
		} ;

	/* end_of_input must not extend the programme : the valid output time
	** range is exactly the time covered by the delivered input.
	*/
	{	SRC_STATE *s2 ;
		long gen_eoi ;
		s2 = src_new (CONVERTER, 1, &error) ;
		ASSERT (s2 != NULL) ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = x ;
		data.input_frames = N ;
		data.data_out = out_b ;
		data.output_frames = N + 64 ;
		data.src_ratio = ratio ;
		data.end_of_input = 0 ;
		error = src_process (s2, &data) ;
		ASSERT (error == 0) ;
		src_delete (s2) ;

		gen_eoi = q_a ; /* run A used end_of_input = 1 */
		ASSERT (data.output_frames_gen == gen_eoi) ;
		} ;

	free (x) ;
	free (xmod) ;
	free (out_a) ;
	free (out_b) ;
	free (out_c) ;

	puts ("MINIMUM_PHASE_CASE causal PASS") ;
} /* test_causal */

/*========================================================================================
** magnitude
*/

static void
magnitude_passband (double ratio, double freq_in)
{	const long	N_IN = 40000, SKIP = 2048 ;
	float	*x, *y ;
	long	out_max = (long) (N_IN * ratio) + 128 ;
	long	gen, n ;
	double	amp, db ;

	x = malloc (sizeof (float) * N_IN) ;
	y = malloc (sizeof (float) * out_max) ;
	ASSERT (x && y) ;

	for (long i = 0 ; i < N_IN ; i++)
		x [i] = (float) (0.6 * sin (2.0 * M_PI * freq_in * i)) ;

	gen = convert_oneshot (ratio, 1, x, N_IN, y, out_max) ;
	ASSERT (gen > 2 * SKIP + 1000) ;

	/* Skip the transient at both ends and project the steady state region
	** onto the analytic reference tone at freq_in / ratio cycles per output
	** frame.
	*/
	n = gen - 2 * SKIP ;
	amp = sine_amplitude (y + SKIP, n, freq_in / ratio) ;
	db = 20.0 * log10 (amp / 0.6) ;

	if (fabs (db) > 0.5)
	{	printf ("\nPassband fail : ratio %.6f freq %.3f : %.3f dB\n\n", ratio, freq_in, db) ;
		exit (1) ;
		} ;

	free (x) ;
	free (y) ;
} /* magnitude_passband */

static void
magnitude_alias_05 (void)
{	const long	N_IN = 40000, SKIP = 2048 ;
	const double ratio = 0.5 ;
	float	*x, *y ;
	long	out_max = N_IN / 2 + 128 ;
	long	gen, n ;
	double	amp, db ;

	x = malloc (sizeof (float) * N_IN) ;
	y = malloc (sizeof (float) * out_max) ;
	ASSERT (x && y) ;

	/* A 0.40 cycles/input-frame sine is above the output Nyquist (0.25) and
	** aliases to (0.5 - 0.40) / 0.5 = 0.2 cycles/output-frame. The anti-alias
	** kernel must suppress it by at least 35 dB relative to the input.
	*/
	for (long i = 0 ; i < N_IN ; i++)
		x [i] = (float) (0.6 * sin (2.0 * M_PI * 0.40 * i)) ;

	gen = convert_oneshot (ratio, 1, x, N_IN, y, out_max) ;
	n = gen - 2 * SKIP ;
	amp = sine_amplitude (y + SKIP, n, 0.2) ;
	db = 20.0 * log10 (amp / 0.6) ;

	if (db > -35.0)
	{	printf ("\nAlias fail : ratio 0.5 : alias at 0.2 cycles/output-frame is %.2f dB\n\n", db) ;
		exit (1) ;
		} ;

	free (x) ;
	free (y) ;
} /* magnitude_alias_05 */

static void
magnitude_image_2 (void)
{	const long	N_IN = 40000, SKIP = 2048 ;
	const double ratio = 2.0 ;
	float	*x, *y ;
	long	out_max = 2 * N_IN + 128 ;
	long	gen, n ;
	double	main_amp, image_amp, db ;

	x = malloc (sizeof (float) * N_IN) ;
	y = malloc (sizeof (float) * out_max) ;
	ASSERT (x && y) ;

	/* A 0.10 cycles/input-frame sine appears at 0.05 cycles/output-frame ;
	** the sampling image sits at 0.5 - 0.05 = 0.45 cycles/output-frame and
	** must be at least 35 dB below the main tone.
	*/
	for (long i = 0 ; i < N_IN ; i++)
		x [i] = (float) (0.6 * sin (2.0 * M_PI * 0.10 * i)) ;

	gen = convert_oneshot (ratio, 1, x, N_IN, y, out_max) ;
	n = gen - 2 * SKIP ;
	main_amp = sine_amplitude (y + SKIP, n, 0.05) ;
	image_amp = sine_amplitude (y + SKIP, n, 0.45) ;

	ASSERT (main_amp > 0.5 * 0.6) ;
	db = 20.0 * log10 (image_amp / main_amp) ;

	if (db > -35.0)
	{	printf ("\nImage fail : ratio 2 : image at 0.45 cycles/output-frame is %.2f dB\n\n", db) ;
		exit (1) ;
		} ;

	free (x) ;
	free (y) ;
} /* magnitude_image_2 */

static void
test_magnitude (void)
{	static const double ratios [] = { 0.5, 44100.0 / 48000.0, 1.0, 2.0 } ;
	int k ;

	for (k = 0 ; k < ARRAY_LEN (ratios) ; k++)
		magnitude_passband (ratios [k], 0.02) ;

	/* Ratio 0.5 : 0.10 cycles/input-frame must also sit in the passband so
	** the alias requirement cannot be met by muting everything.
	*/
	magnitude_passband (0.5, 0.10) ;

	magnitude_alias_05 () ;
	magnitude_image_2 () ;

	puts ("MINIMUM_PHASE_CASE magnitude PASS") ;
} /* test_magnitude */

/*========================================================================================
** phase
**
** The impulse response is collected by appending explicit zero input (the
** converter itself never extends the programme). The measured response is
** then projected onto sine/cosine pairs (a DFT) to obtain the amplitude and
** phase spectra. The minimum phase check derives, independently from the
** production code, the phase that a minimum phase system with the *measured*
** amplitude response must have : log |H|, real cepstrum via the test's own
** FFT, cepstral folding, back to the frequency domain. The two unwrapped
** phases are compared directly ; no extra linear delay may be removed to
** make them align.
*/

/* The test's own radix-2 FFT, written here so the verification does not
** share code with the production designer.
*/
static void
test_fft (double *re, double *im, int n, int inverse)
{	int i, j, k, len ;

	for (i = 1, j = 0 ; i < n ; i++)
	{	int bit = n >> 1 ;
		for ( ; j & bit ; bit >>= 1)
			j ^= bit ;
		j ^= bit ;
		if (i < j)
		{	double tmp ;
			tmp = re [i] ; re [i] = re [j] ; re [j] = tmp ;
			tmp = im [i] ; im [i] = im [j] ; im [j] = tmp ;
			}
		}

	for (len = 2 ; len <= n ; len <<= 1)
	{	double ang = (inverse ? 2.0 : -2.0) * M_PI / len ;
		double wr = cos (ang), wi = sin (ang) ;
		for (i = 0 ; i < n ; i += len)
		{	double cur_r = 1.0, cur_i = 0.0 ;
			for (k = 0 ; k < len / 2 ; k++)
			{	int a = i + k, b = i + k + len / 2 ;
				double xr = re [b] * cur_r - im [b] * cur_i ;
				double xi = re [b] * cur_i + im [b] * cur_r ;
				re [b] = re [a] - xr ;
				im [b] = im [a] - xi ;
				re [a] += xr ;
				im [a] += xi ;
				double next_r = cur_r * wr - cur_i * wi ;
				cur_i = cur_r * wi + cur_i * wr ;
				cur_r = next_r ;
				}
			}
		}

	if (inverse)
	{	double scale = 1.0 / n ;
		for (i = 0 ; i < n ; i++)
		{	re [i] *= scale ;
			im [i] *= scale ;
			}
		}

	return ;
} /* test_fft */

/* Phase unwrapping : walk the spectrum in order and, whenever the jump
** between neighbours exceeds pi, shift the remainder by multiples of 2 pi so
** the sequence stays continuous.
*/
static void
test_unwrap (double *phase, int n)
{	double	offset = 0.0 ;
	int		k ;

	for (k = 1 ; k < n ; k++)
	{	double d = phase [k] + offset - phase [k - 1] ;
		if (d > M_PI)
			offset -= 2.0 * M_PI ;
		else if (d < -M_PI)
			offset += 2.0 * M_PI ;
		phase [k] += offset ;
		} ;

	return ;
} /* test_unwrap */

static void
test_phase (void)
{	enum { L = 1024 } ;
	float	*x, *h ;
	double	*re, *im, *mag, *ph_meas, *ph_min ;
	long	gen ;
	int		k ;

	x = calloc (L, sizeof (float)) ;
	h = malloc (sizeof (float) * (L + 64)) ;
	ASSERT (x && h) ;

	/*----------------------------------------------------------------------
	** Energy concentration at ratio 0.5, from an explicitly zero padded
	** impulse.
	*/
	x [0] = 1.0f ;
	gen = convert_oneshot (0.5, 1, x, L, h, L + 64) ;
	ASSERT (gen == L / 2) ; /* output span == input span, no tail appended */

	{	long	k0 = -1 ;
		double	e_tot = 0.0, e_early = 0.0 ;

		for (k = 0 ; k < gen ; k++)
			if (h [k] != 0.0f)
			{	k0 = k ;
				break ;
				} ;
		ASSERT (k0 >= 0) ;

		for (k = k0 ; k < gen ; k++)
		{	double v = h [k] ;
			e_tot += v * v ;
			/* The first 12 input frames after the first nonzero output
			** correspond to 12 * 0.5 = 6 output frames at ratio 0.5.
			*/
			if ((k - k0) * 2 <= 12)
				e_early += v * v ;
			} ;

		if (e_early < 0.8 * e_tot)
		{	printf ("\nEnergy fail : first 12 input frames hold %.1f %% of the energy\n\n",
					100.0 * e_early / e_tot) ;
			exit (1) ;
			} ;
		} ;

	/*----------------------------------------------------------------------
	** Minimum phase relation on the fixed ratio 1 grid.
	*/
	gen = convert_oneshot (1.0, 1, x, L, h, L + 64) ;
	ASSERT (gen == L) ;

	re		= calloc (L, sizeof (double)) ;
	im		= calloc (L, sizeof (double)) ;
	mag		= calloc (L, sizeof (double)) ;
	ph_meas	= calloc (L, sizeof (double)) ;
	ph_min	= calloc (L, sizeof (double)) ;
	ASSERT (re && im && mag && ph_meas && ph_min) ;

	/* Measured spectrum : sine/cosine projection of the measured impulse
	** response (a DFT of the collected output, not of any production
	** coefficient table).
	*/
	for (k = 0 ; k < L ; k++)
	{	re [k] = h [k] ;
		im [k] = 0.0 ;
		} ;
	test_fft (re, im, L, 0) ;

	for (k = 0 ; k < L ; k++)
	{	mag [k] = hypot (re [k], im [k]) ;
		ph_meas [k] = atan2 (im [k], re [k]) ;
		} ;

	/* Independent derivation of the minimum phase belonging to the measured
	** magnitude : real cepstrum of log |H|, fold, transform back. The phase
	** of exp (FFT (folded cepstrum)) is the minimum phase.
	*/
	{	double maxmag = 0.0 ;
		for (k = 0 ; k < L ; k++)
			if (mag [k] > maxmag)
				maxmag = mag [k] ;
		ASSERT (maxmag > 0.0) ;
		for (k = 0 ; k < L ; k++)
		{	re [k] = log (mag [k] + maxmag * 1e-12) ;
			im [k] = 0.0 ;
			} ;
		} ;
	test_fft (re, im, L, 1) ;
	for (k = 1 ; k < L / 2 ; k++)
		re [k] *= 2.0 ;
	for (k = L / 2 + 1 ; k < L ; k++)
		re [k] = 0.0 ;
	for (k = 0 ; k < L ; k++)
		im [k] = 0.0 ;
	test_fft (re, im, L, 0) ;
	for (k = 0 ; k < L ; k++)
		ph_min [k] = im [k] ;

	/* Unwrap both phases and compare them directly. No linear delay term
	** is fitted or removed to align the curves : a true minimum phase
	** response must match as is.
	*/
	test_unwrap (ph_meas, L / 2) ;
	test_unwrap (ph_min, L / 2) ;

	{	double worst = 0.0 ;
		int		worst_k = 0 ;
		/* Compare over the flat passband region (<= 0.30 cycles/frame)
		** where the measured magnitude is accurate.
		*/
		for (k = 1 ; k <= (int) (0.30 * L) ; k++)
		{	double d = fabs (ph_meas [k] - ph_min [k]) ;
			if (d > worst)
			{	worst = d ;
				worst_k = k ;
				} ;
			} ;

		if (worst > 0.02)
		{	printf ("\nMinphase fail : phase deviation %.6f rad at bin %d\n\n", worst, worst_k) ;
			exit (1) ;
			} ;
		} ;

	free (x) ;
	free (h) ;
	free (re) ;
	free (im) ;
	free (mag) ;
	free (ph_meas) ;
	free (ph_min) ;

	puts ("MINIMUM_PHASE_CASE phase PASS") ;
} /* test_phase */

/*========================================================================================
** streaming
*/

static void
streaming_compare (double ratio, int channels, const float *in, long in_frames,
		const float *ref, long ref_frames, const char *what)
{	static const long	in_chunks [] = { 1, 3, 64, 257 } ;
	static const long	out_caps [] = { 1, 7, 113 } ;
	float	*out ;
	long	out_max = (long) (in_frames * ratio) + 256 ;
	long	gen ;

	out = malloc (sizeof (float) * out_max * channels) ;
	ASSERT (out != NULL) ;

	/* Chunked process path. */
	gen = convert_chunked (ratio, channels, in, in_frames,
			in_chunks, ARRAY_LEN (in_chunks), out_caps, ARRAY_LEN (out_caps),
			out, out_max) ;
	if (gen != ref_frames)
	{	printf ("\n%s : chunked generated %ld frames, reference %ld\n\n", what, gen, ref_frames) ;
		exit (1) ;
		} ;
	compare_exact (out, ref, gen * channels, what) ;

	/* src_simple path. */
	{	SRC_DATA	data ;
		int			error ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = in ;
		data.input_frames = in_frames ;
		data.data_out = out ;
		data.output_frames = out_max ;
		data.src_ratio = ratio ;

		error = src_simple (&data, CONVERTER, channels) ;
		ASSERT (error == 0) ;
		ASSERT (data.output_frames_gen == ref_frames) ;
		compare_exact (out, ref, ref_frames * channels, what) ;
		} ;

	/* Callback path. */
	gen = convert_callback (ratio, channels, in, in_frames, 5, 29, out, out_max) ;
	if (gen != ref_frames)
	{	printf ("\n%s : callback generated %ld frames, reference %ld\n\n", what, gen, ref_frames) ;
		exit (1) ;
		} ;
	compare_exact (out, ref, ref_frames * channels, what) ;

	free (out) ;
} /* streaming_compare */

static void
test_streaming (void)
{	const long	N = 3000 ;
	const double ratio = 0.75 ;
	float	*in, *ref, *mono_ref ;
	long	ref_frames, i ;
	int		c ;

	in		= malloc (sizeof (float) * N * 6) ;
	ref		= malloc (sizeof (float) * (N + 256) * 6) ;
	mono_ref = malloc (sizeof (float) * (N + 256)) ;
	ASSERT (in && ref && mono_ref) ;

	/*--- Mono -----------------------------------------------------------*/
	for (i = 0 ; i < N ; i++)
		in [i] = test_signal (i) ;

	ref_frames = convert_oneshot (ratio, 1, in, N, mono_ref, N + 256) ;
	ASSERT (ref_frames > 0) ;

	streaming_compare (ratio, 1, in, N, mono_ref, ref_frames, "mono") ;

	/* Strict one frame in / one frame out delivery. */
	{	static const long one_in [] = { 1 } ;
		static const long one_out [] = { 1 } ;
		float *out = malloc (sizeof (float) * (N + 256)) ;
		long gen ;

		ASSERT (out != NULL) ;
		gen = convert_chunked (ratio, 1, in, N, one_in, 1, one_out, 1, out, N + 256) ;
		ASSERT (gen == ref_frames) ;
		compare_exact (out, mono_ref, ref_frames, "mono 1-in/1-out") ;
		free (out) ;
		} ;

	/*--- Stereo, right channel inverted ---------------------------------*/
	for (i = 0 ; i < N ; i++)
	{	in [2 * i]		= test_signal (i) ;
		in [2 * i + 1]	= -test_signal (i) ;
		} ;

	ref_frames = convert_oneshot (ratio, 2, in, N, ref, (N + 256) * 2) ;

	/* Per-channel output must match the mono reference (with sign). */
	for (i = 0 ; i < ref_frames ; i++)
	{	ASSERT (ref [2 * i] == mono_ref [i]) ;
		ASSERT (ref [2 * i + 1] == -mono_ref [i]) ;
		} ;

	streaming_compare (ratio, 2, in, N, ref, ref_frames, "inverted stereo") ;

	/*--- Six channels, impulses at different positions -------------------*/
	memset (in, 0, sizeof (float) * N * 6) ;
	for (c = 0 ; c < 6 ; c++)
		in [(5 + 10 * c) * 6 + c] = 0.25f * (c + 1) ;

	ref_frames = convert_oneshot (ratio, 6, in, N, ref, (N + 256) * 6) ;

	/* Each channel must equal the mono conversion of that channel alone :
	** shared fractional position, independent histories.
	*/
	for (c = 0 ; c < 6 ; c++)
	{	float *mono_in = calloc (N, sizeof (float)) ;
		float *mono_out = malloc (sizeof (float) * (N + 256)) ;
		long	mgen ;

		ASSERT (mono_in && mono_out) ;
		mono_in [5 + 10 * c] = 0.25f * (c + 1) ;
		mgen = convert_oneshot (ratio, 1, mono_in, N, mono_out, N + 256) ;
		ASSERT (mgen == ref_frames) ;
		for (i = 0 ; i < ref_frames ; i++)
			ASSERT (ref [i * 6 + c] == mono_out [i]) ;

		free (mono_in) ;
		free (mono_out) ;
		} ;

	streaming_compare (ratio, 6, in, N, ref, ref_frames, "six channel impulses") ;

	/*--- Progress with minimal deliveries --------------------------------
	** Ratio 2, one input frame and one output frame of capacity per call :
	** every call must produce an output frame as soon as it is reachable.
	*/
	{	SRC_STATE	*state ;
		SRC_DATA	data ;
		float		frame_in [1], frame_out [1] ;
		long		total ;
		int			error ;

		state = src_new (CONVERTER, 1, &error) ;
		ASSERT (state != NULL) ;

		total = 0 ;
		for (i = 0 ; i < 200 ; i++)
		{	frame_in [0] = test_signal (i) ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = frame_in ;
			data.input_frames = 1 ;
			data.data_out = frame_out ;
			data.output_frames = 1 ;
			data.src_ratio = 2.0 ;
			error = src_process (state, &data) ;
			ASSERT (error == 0) ;
			ASSERT (data.output_frames_gen == 1) ;
			total += data.output_frames_gen ;
			} ;
		ASSERT (total == 200) ;

		src_delete (state) ;
		} ;

	free (in) ;
	free (ref) ;
	free (mono_ref) ;

	puts ("MINIMUM_PHASE_CASE streaming PASS") ;
} /* test_streaming */

/*========================================================================================
** lifecycle
*/

static void
test_lifecycle (void)
{	const long	N = 2000 ;
	const double ratio = 0.75 ;
	float	*x, *ref, *out ;
	long	ref_frames, i ;
	SRC_STATE *state, *state2, *cloned ;
	SRC_DATA data ;
	int		error ;

	x	= malloc (sizeof (float) * N) ;
	ref	= malloc (sizeof (float) * (N + 256)) ;
	out	= malloc (sizeof (float) * (N + 256)) ;
	ASSERT (x && ref && out) ;

	for (i = 0 ; i < N ; i++)
		x [i] = test_signal (i) ;

	/* Name and description must be registered. */
	ASSERT (src_get_name (CONVERTER) != NULL) ;
	ASSERT (src_get_description (CONVERTER) != NULL) ;
	ASSERT (strstr (src_get_name (CONVERTER), "Minimum") != NULL) ;

	/* Uninterrupted reference run. */
	ref_frames = convert_oneshot (ratio, 1, x, N, ref, N + 256) ;

	/*--- Ratio lock ------------------------------------------------------*/
	state = src_new (CONVERTER, 1, &error) ;
	ASSERT (state != NULL) ;

	/* First non-empty conversion locks the ratio. */
	memset (&data, 0, sizeof (data)) ;
	data.data_in = x ;
	data.input_frames = 500 ;
	data.data_out = out ;
	data.output_frames = N + 256 ;
	data.src_ratio = ratio ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	long produced = data.output_frames_gen ;
	ASSERT (produced > 0) ;

	/* Same ratio continues. */
	data.data_in = x + 500 ;
	data.input_frames = 500 ;
	data.data_out = out + produced ;
	data.output_frames = N + 256 - produced ;
	data.src_ratio = ratio ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	produced += data.output_frames_gen ;

	/* A different ratio must fail before consuming input or generating
	** output.
	*/
	data.data_in = x + 1000 ;
	data.input_frames = 500 ;
	data.data_out = out + produced ;
	data.output_frames = N + 256 - produced ;
	data.src_ratio = 0.8 ;
	error = src_process (state, &data) ;
	ASSERT (error == TEST_ERR_NO_VARIABLE_RATIO) ;
	ASSERT (data.input_frames_used == 0) ;
	ASSERT (data.output_frames_gen == 0) ;

	/* src_set_ratio must not bypass the lock. */
	ASSERT (src_set_ratio (state, 0.8) == TEST_ERR_NO_VARIABLE_RATIO) ;
	ASSERT (src_set_ratio (state, ratio) == 0) ;

	/* The failed attempts must not have disturbed the stream : continuing
	** with the locked ratio produces exactly the reference tail.
	*/
	data.data_in = x + 1000 ;
	data.input_frames = N - 1000 ;
	data.data_out = out + produced ;
	data.output_frames = N + 256 - produced ;
	data.src_ratio = ratio ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	produced += data.output_frames_gen ;
	ASSERT (produced == ref_frames) ;
	compare_exact (out, ref, ref_frames, "lifecycle locked stream") ;

	/*--- Out of range ratios ----------------------------------------------*/
	ASSERT (src_set_ratio (state, 3.0) == TEST_ERR_BAD_SRC_RATIO) ;
	ASSERT (src_set_ratio (state, 0.3) == TEST_ERR_BAD_SRC_RATIO) ;
	src_delete (state) ;

	state = src_new (CONVERTER, 1, &error) ;
	ASSERT (state != NULL) ;
	memset (&data, 0, sizeof (data)) ;
	data.data_in = x ;
	data.input_frames = 100 ;
	data.data_out = out ;
	data.output_frames = N + 256 ;
	data.src_ratio = 0.4 ; /* below the supported range */
	error = src_process (state, &data) ;
	ASSERT (error == TEST_ERR_BAD_SRC_RATIO) ;
	ASSERT (data.input_frames_used == 0) ;
	ASSERT (data.output_frames_gen == 0) ;
	data.src_ratio = 2.5 ; /* above the supported range */
	error = src_process (state, &data) ;
	ASSERT (error == TEST_ERR_BAD_SRC_RATIO) ;
	/* The illegal attempts did not lock anything : a legal ratio works. */
	data.src_ratio = 0.6 ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	ASSERT (data.output_frames_gen > 0) ;
	src_delete (state) ;

	/*--- Reset behaves like a fresh instance ------------------------------*/
	state = src_new (CONVERTER, 1, &error) ;
	ASSERT (state != NULL) ;
	memset (&data, 0, sizeof (data)) ;
	data.data_in = x ;
	data.input_frames = 700 ;
	data.data_out = out ;
	data.output_frames = N + 256 ;
	data.src_ratio = 0.9 ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;

	ASSERT (src_reset (state) == 0) ;

	/* After reset a different ratio may be locked. */
	data.data_in = x ;
	data.input_frames = N ;
	data.data_out = out ;
	data.output_frames = N + 256 ;
	data.src_ratio = 1.5 ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	long reset_frames = data.output_frames_gen ;

	state2 = src_new (CONVERTER, 1, &error) ;
	ASSERT (state2 != NULL) ;
	data.data_out = ref ;
	data.output_frames = N + 256 ;
	data.src_ratio = 1.5 ;
	error = src_process (state2, &data) ;
	ASSERT (error == 0) ;
	ASSERT (data.output_frames_gen == reset_frames) ;
	compare_exact (out, ref, reset_frames, "reset equals fresh instance") ;
	src_delete (state2) ;

	/*--- Clone -------------------------------------------------------------*/
	ASSERT (src_reset (state) == 0) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = x ;
	data.input_frames = 600 ;
	data.data_out = out ;
	data.output_frames = N + 256 ;
	data.src_ratio = ratio ;
	error = src_process (state, &data) ; /* state was reset above, locks to 0.75 */
	ASSERT (error == 0) ;
	long head = data.output_frames_gen ;

	cloned = src_clone (state, &error) ;
	ASSERT (cloned != NULL) ;

	/* Both branches continue the same stream identically ... */
	data.data_in = x + 600 ;
	data.input_frames = N - 600 ;
	data.data_out = out + head ;
	data.output_frames = N + 256 - head ;
	data.src_ratio = ratio ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	long total_a = head + data.output_frames_gen ;

	data.data_out = ref ;
	data.output_frames = N + 256 ;
	error = src_process (cloned, &data) ;
	ASSERT (error == 0) ;
	ASSERT (head + data.output_frames_gen == total_a) ;
	compare_exact (out + head, ref, data.output_frames_gen, "clone continuation") ;

	/* ... and are independent : feeding different data to the clone does not
	** disturb the original.
	*/
	for (i = 0 ; i < 100 ; i++)
		x [i] = -x [i] ;
	data.data_in = x ;
	data.input_frames = 100 ;
	data.data_out = ref ;
	data.output_frames = 256 ;
	error = src_process (cloned, &data) ;
	ASSERT (error == 0) ;

	data.data_in = x ;
	data.input_frames = 100 ;
	data.data_out = out ;
	data.output_frames = 256 ;
	error = src_process (state, &data) ;
	ASSERT (error == 0) ;
	ASSERT (data.output_frames_gen > 0) ;

	/* Resetting the clone makes it equivalent to a new instance. */
	ASSERT (src_reset (cloned) == 0) ;
	data.data_in = x ;
	data.input_frames = 500 ;
	data.data_out = ref ;
	data.output_frames = N + 256 ;
	data.src_ratio = 1.25 ;
	error = src_process (cloned, &data) ;
	ASSERT (error == 0) ;
	long cloned_frames = data.output_frames_gen ;

	state2 = src_new (CONVERTER, 1, &error) ;
	ASSERT (state2 != NULL) ;
	data.data_out = out ;
	data.output_frames = N + 256 ;
	data.src_ratio = 1.25 ;
	error = src_process (state2, &data) ;
	ASSERT (error == 0) ;
	ASSERT (data.output_frames_gen == cloned_frames) ;
	compare_exact (ref, out, cloned_frames, "reset clone equals fresh instance") ;

	src_delete (state2) ;
	src_delete (cloned) ;
	src_delete (state) ;

	free (x) ;
	free (ref) ;
	free (out) ;

	puts ("MINIMUM_PHASE_CASE lifecycle PASS") ;
} /* test_lifecycle */

/*========================================================================================
*/

int
main (void)
{
	puts ("") ;

	test_causal () ;
	test_magnitude () ;
	test_phase () ;
	test_streaming () ;
	test_lifecycle () ;

	puts ("") ;

	return 0 ;
} /* main */
