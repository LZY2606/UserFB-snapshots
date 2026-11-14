/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Causal minimum phase sinc converter (SRC_SINC_MINIMUM_PHASE).
**
** Unlike the symmetric (linear phase) sinc converters, this converter uses a
** causal minimum phase lowpass kernel so that the bulk of the impulse
** response energy is emitted as early as possible. It is intended for low
** latency monitoring / live return paths where a nonlinear phase response is
** an acceptable trade for minimal delay.
**
** Causality contract : output frame k corresponds to source time k/r (r is
** the output/input ratio) and is computed from input frames 0 .. floor (k/r)
** only, plus the zero history that precedes the stream (the state is zeroed
** on reset). No future input is ever waited for ; as soon as the input frame
** floor (k/r) has been delivered, output k can be produced. end_of_input does
** not flush or extend the signal ; callers that want to observe the FIR tail
** must append zero input explicitly.
**
** The kernel is designed at run time, once, when the conversion ratio is
** locked (the first non-empty conversion after each reset). Two grid
** layouts are used :
**
**   - ratio == 1 : the output times k/r land exactly on the input grid, so
**     the kernel is designed on the input grid itself. A Blackman windowed
**     sinc prototype (cutoff 0.45 cycles/input-frame, MINPHASE_LEN taps) is
**     turned into a minimum phase kernel with the homomorphic (real
**     cepstrum) method : log |H|, inverse FFT, fold the cepstrum, forward
**     FFT, exponentiate, inverse FFT. The result is minimum phase with
**     respect to its own magnitude on the input grid.
**
**   - all other ratios : output times fall between input samples, so the
**     kernel is designed on a fine grid of MINPHASE_PHASES points per input
**     frame. A windowed sinc prototype (cutoff 0.45 * min (1, r) cycles per
**     input frame) is made causal and minimum phase with the same
**     homomorphic transform on the fine grid ; because the kernel is causal
**     on the fine grid, every polyphase branch keeps unity DC gain and the
**     fractional phases needed by the ratio are exact samples of one smooth
**     causal kernel. The coefficient table stores the kernel at
**     MINPHASE_PHASES phases per input frame and neighbouring rows are
**     linearly interpolated.
**
** Both transforms use the small self contained radix-2 FFT below ; no
** external FFT library, no Python and no private machine generated tables
** are involved. The kernel has an effective support of MINPHASE_LEN input
** frames (32, well below the 256 frame limit). Normal processing performs
** no memory allocation and never re-runs the designer ; the only allocation
** outside of src_new () is the scratch FFT buffer used by the one-shot
** designer, and it is freed before the designer returns.
**
** Ratio policy : only ratios in [0.5, 2.0] are supported. The ratio is fixed
** by the first non-empty conversion after each reset ; any later attempt to
** change it (via src_process () or src_set_ratio ()) fails before any input
** is consumed or any output is generated. Passing the same value again is
** fine. After src_reset () the converter behaves exactly like a fresh one.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "common.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846264338
#endif

#define MINPHASE_MAGIC_MARKER	MAKE_MAGIC ('m', 'i', 'n', 'p', 'h', 's')

/* Kernel support in input frames. Must never exceed 256. */
#define MINPHASE_LEN		32

/* Polyphase grid resolution : phases per input frame. */
#define MINPHASE_PHASES		64

/* FFT length for the homomorphic transform : 4 * LEN * PHASES, power of two. */
#define MINPHASE_FFT_LEN	8192

/* Supported conversion ratio (output / input) range. */
#define MINPHASE_MIN_RATIO	0.5
#define MINPHASE_MAX_RATIO	2.0

typedef int _CHECK_MINPHASE_LEN [2 * (MINPHASE_LEN <= 256) - 1] ;
typedef int _CHECK_MINPHASE_FFT [2 * (MINPHASE_FFT_LEN >= 4 * MINPHASE_LEN * MINPHASE_PHASES) - 1] ;

typedef struct
{	int		magic ;

	/* Ratio lock : cleared by reset, set by the first non-empty conversion. */
	bool	ratio_locked ;
	double	ratio, inv_ratio ;

	/* Absolute count of input frames copied into the ring since reset. */
	long	in_buffered ;
	/* Absolute count of output frames generated since reset. */
	long	out_count ;

	/* Interleaved history ring, MINPHASE_LEN frames * channels. */
	float	*ring ;

	/* Causal kernel table, (MINPHASE_PHASES + 1) * MINPHASE_LEN entries.
	** Row p (0 <= p < MINPHASE_PHASES) holds g (j + p / PHASES) in column j,
	** where g is the minimum phase kernel in input frame units. The extra
	** row PHASES is row 0 shifted by one tap so fractional interpolation
	** between phases can always read rows p and p + 1.
	*/
	float	*coeff ;
} MINPHASE_DATA ;

static SRC_ERROR minphase_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR minphase_set_ratio (SRC_STATE *state, double new_ratio) ;
static void minphase_reset (SRC_STATE *state) ;
static SRC_STATE *minphase_copy (SRC_STATE *state) ;
static void minphase_close (SRC_STATE *state) ;

static SRC_STATE_VT minphase_state_vt =
{
	minphase_process,
	minphase_process,
	minphase_reset,
	minphase_copy,
	minphase_close,
	minphase_set_ratio
} ;

/*----------------------------------------------------------------------------------------
** Small self contained iterative radix-2 FFT (complex, double precision).
** Used only by the one-shot filter designer. Not exported.
*/

static void
minphase_fft (double *re, double *im, int n, int inverse)
{	int i, j, k, len ;

	/* Bit reversal permutation. */
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
} /* minphase_fft */

/*----------------------------------------------------------------------------------------
** Homomorphic minimum phase transform, shared by both design grids.
** On entry re [0..n) holds the zero padded linear phase prototype (im is
** ignored). On exit re [0..n) holds the causal minimum phase kernel with
** the same magnitude response.
*/

static void
minphase_transform (double *re, double *im, int n)
{	double	maxmag, floor_mag ;
	int		k ;

	minphase_fft (re, im, n, 0) ;

	maxmag = 0.0 ;
	for (k = 0 ; k < n ; k++)
	{	double mag = hypot (re [k], im [k]) ;
		if (mag > maxmag)
			maxmag = mag ;
		} ;

	/* Keep the log floor far below the stopband so the designed magnitude
	** response is preserved.
	*/
	floor_mag = maxmag * 1e-7 ;
	for (k = 0 ; k < n ; k++)
	{	re [k] = log (hypot (re [k], im [k]) + floor_mag) ;
		im [k] = 0.0 ;
		} ;

	minphase_fft (re, im, n, 1) ;

	/* Fold the cepstrum : keep the causal half. */
	for (k = 1 ; k < n / 2 ; k++)
		re [k] *= 2.0 ;
	for (k = n / 2 + 1 ; k < n ; k++)
		re [k] = 0.0 ;
	for (k = 0 ; k < n ; k++)
		im [k] = 0.0 ;

	minphase_fft (re, im, n, 0) ;

	for (k = 0 ; k < n ; k++)
	{	double e = exp (re [k]), phase = im [k] ;
		re [k] = e * cos (phase) ;
		im [k] = e * sin (phase) ;
		} ;

	minphase_fft (re, im, n, 1) ;

	return ;
} /* minphase_transform */

/*----------------------------------------------------------------------------------------
** One-shot kernel designer, run when the ratio is locked.
*/

static SRC_ERROR
minphase_design (MINPHASE_DATA *priv, double ratio)
{	double	*re = NULL, *im = NULL ;
	double	cutoff, sum ;
	int		m, p, j ;

	re = (double *) calloc (MINPHASE_FFT_LEN, sizeof (double)) ;
	im = (double *) calloc (MINPHASE_FFT_LEN, sizeof (double)) ;
	if (re == NULL || im == NULL)
	{	free (re) ;
		free (im) ;
		return SRC_ERR_MALLOC_FAILED ;
		} ;

	if (ratio == 1.0)
	{	/* Output times land on the input grid : design the minimum phase
		** kernel on the input grid itself.
		*/
		cutoff = 0.45 ;
		sum = 0.0 ;
		for (m = 0 ; m < MINPHASE_LEN ; m++)
		{	double tau = m - 0.5 * (MINPHASE_LEN - 1) ;
			double x = 2.0 * cutoff * tau ;
			double sinc = (fabs (x) < 1e-12) ? 1.0 : sin (M_PI * x) / (M_PI * x) ;
			double window = 0.42
					- 0.5 * cos (2.0 * M_PI * m / (MINPHASE_LEN - 1))
					+ 0.08 * cos (4.0 * M_PI * m / (MINPHASE_LEN - 1)) ;
			re [m] = sinc * window ;
			sum += re [m] ;
			} ;
		if (sum == 0.0)
		{	free (re) ;
			free (im) ;
			return SRC_ERR_BAD_INTERNAL_STATE ;
			} ;
		for (m = 0 ; m < MINPHASE_LEN ; m++)
			re [m] /= sum ;

		minphase_transform (re, im, 4096) ;

		/* Only phase zero is ever used at ratio 1 ; fill every row so the
		** table is fully initialised regardless.
		*/
		for (p = 0 ; p <= MINPHASE_PHASES ; p++)
			for (j = 0 ; j < MINPHASE_LEN ; j++)
				priv->coeff [p * MINPHASE_LEN + j] = (float) re [j] ;
		}
	else
	{	/* Fine grid, MINPHASE_PHASES points per input frame. */
		enum { PROTOLEN = MINPHASE_LEN * MINPHASE_PHASES } ;

		cutoff = 0.45 * MIN (1.0, ratio) ;
		sum = 0.0 ;
		for (m = 0 ; m < PROTOLEN ; m++)
		{	double tau = (m - 0.5 * (PROTOLEN - 1)) / MINPHASE_PHASES ;
			double x = 2.0 * cutoff * tau ;
			double sinc = (fabs (x) < 1e-12) ? 1.0 : sin (M_PI * x) / (M_PI * x) ;
			double window = 0.42
					- 0.5 * cos (2.0 * M_PI * m / (PROTOLEN - 1))
					+ 0.08 * cos (4.0 * M_PI * m / (PROTOLEN - 1)) ;
			re [m] = sinc * window ;
			sum += re [m] ;
			} ;
		if (sum == 0.0)
		{	free (re) ;
			free (im) ;
			return SRC_ERR_BAD_INTERNAL_STATE ;
			} ;
		/* Normalise so that each polyphase branch sums to one. */
		for (m = 0 ; m < PROTOLEN ; m++)
			re [m] *= MINPHASE_PHASES / sum ;

		minphase_transform (re, im, MINPHASE_FFT_LEN) ;

		/* Fill the polyphase table : row p, column j holds g (j + p / PHASES). */
		for (p = 0 ; p < MINPHASE_PHASES ; p++)
			for (j = 0 ; j < MINPHASE_LEN ; j++)
				priv->coeff [p * MINPHASE_LEN + j] = (float) re [j * MINPHASE_PHASES + p] ;

		/* Extra row : g (j + 1) for fractional interpolation at the last phase. */
		for (j = 0 ; j < MINPHASE_LEN - 1 ; j++)
			priv->coeff [MINPHASE_PHASES * MINPHASE_LEN + j] = priv->coeff [j + 1] ;
		priv->coeff [MINPHASE_PHASES * MINPHASE_LEN + MINPHASE_LEN - 1] = 0.0f ;
		} ;

	free (re) ;
	free (im) ;

	return SRC_ERR_NO_ERROR ;
} /* minphase_design */

/*----------------------------------------------------------------------------------------
** Processing.
*/

static SRC_ERROR
minphase_process (SRC_STATE *state, SRC_DATA *data)
{	MINPHASE_DATA	*priv ;
	double			t, frac, fphase, delta ;
	long			in_used, out_gen, n0 ;
	int				ch, j, p, channels ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	priv = (MINPHASE_DATA *) state->private_data ;

	if (priv->magic != MINPHASE_MAGIC_MARKER)
		return SRC_ERR_BAD_INTERNAL_STATE ;

	if (! priv->ratio_locked)
	{	/* An empty conversion does not lock the ratio and, since the
		** history is all zeros right after a reset, cannot produce any
		** output either.
		*/
		if (data->input_frames <= 0)
			return SRC_ERR_NO_ERROR ;

		if (! (data->src_ratio >= MINPHASE_MIN_RATIO && data->src_ratio <= MINPHASE_MAX_RATIO))
			return SRC_ERR_BAD_SRC_RATIO ;

		SRC_ERROR error = minphase_design (priv, data->src_ratio) ;
		if (error != SRC_ERR_NO_ERROR)
			return error ;

		priv->ratio = data->src_ratio ;
		priv->inv_ratio = 1.0 / data->src_ratio ;
		priv->ratio_locked = true ;
		}
	else if (fabs (data->src_ratio - priv->ratio) > 1e-15)
	{	/* Ratio change after the lock : fail before consuming any input
		** or generating any output.
		*/
		return SRC_ERR_NO_VARIABLE_RATIO ;
		} ;

	channels = state->channels ;
	in_used = 0 ;
	out_gen = 0 ;

	while (out_gen < data->output_frames)
	{	/* Source time of the next output frame, in input frames. */
		t = priv->out_count * priv->inv_ratio ;
		n0 = (long) floor (t) ;

		/* Copy input into the ring, but never more than the next output
		** frame needs : the ring holds exactly the causal kernel window.
		*/
		while (priv->in_buffered <= n0 && in_used < data->input_frames)
		{	int slot = (int) (priv->in_buffered & (MINPHASE_LEN - 1)) ;
			for (ch = 0 ; ch < channels ; ch++)
				priv->ring [slot * channels + ch] = data->data_in [in_used * channels + ch] ;
			priv->in_buffered ++ ;
			in_used ++ ;
			} ;

		/* Output k may only use input frames 0 .. floor (k / r). If the
		** frame floor (t) has not been delivered yet, stop and wait for
		** more input.
		*/
		if (priv->in_buffered <= n0)
			break ;

		frac = t - (double) n0 ;
		fphase = frac * MINPHASE_PHASES ;
		p = (int) fphase ;
		if (p > MINPHASE_PHASES - 1)
			p = MINPHASE_PHASES - 1 ;
		delta = fphase - (double) p ;

		{	const float *c0 = priv->coeff + p * MINPHASE_LEN ;
			const float *c1 = c0 + MINPHASE_LEN ;
			double	kernel [MINPHASE_LEN] ;

			for (j = 0 ; j < MINPHASE_LEN ; j++)
				kernel [j] = c0 [j] + delta * (c1 [j] - c0 [j]) ;

			for (ch = 0 ; ch < channels ; ch++)
			{	double sum = 0.0 ;
				long	frame = n0 ;
				for (j = 0 ; j < MINPHASE_LEN ; j++, frame--)
				{	int slot = (int) (frame & (MINPHASE_LEN - 1)) ;
					sum += kernel [j] * priv->ring [slot * channels + ch] ;
					} ;
				data->data_out [out_gen * channels + ch] = (float) sum ;
				} ;
			} ;

		priv->out_count ++ ;
		out_gen ++ ;
		} ;

	data->input_frames_used = in_used ;
	data->output_frames_gen = out_gen ;

	return SRC_ERR_NO_ERROR ;
} /* minphase_process */

static SRC_ERROR
minphase_set_ratio (SRC_STATE *state, double new_ratio)
{	MINPHASE_DATA *priv ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	priv = (MINPHASE_DATA *) state->private_data ;

	if (! (new_ratio >= MINPHASE_MIN_RATIO && new_ratio <= MINPHASE_MAX_RATIO))
		return SRC_ERR_BAD_SRC_RATIO ;

	if (priv->ratio_locked && fabs (new_ratio - priv->ratio) > 1e-15)
		return SRC_ERR_NO_VARIABLE_RATIO ;

	return SRC_ERR_NO_ERROR ;
} /* minphase_set_ratio */

/*----------------------------------------------------------------------------------------
*/

LIBSAMPLERATE_DLL_PRIVATE const char*
minphase_get_name (int src_enum)
{
	if (src_enum == SRC_SINC_MINIMUM_PHASE)
		return "Minimum Phase Sinc Interpolator" ;

	return NULL ;
} /* minphase_get_name */

LIBSAMPLERATE_DLL_PRIVATE const char*
minphase_get_description (int src_enum)
{
	if (src_enum == SRC_SINC_MINIMUM_PHASE)
		return "Causal minimum phase sinc interpolator, low latency, fixed ratio in [0.5, 2.0]." ;

	return NULL ;
} /* minphase_get_description */

static MINPHASE_DATA *
minphase_data_new (int channels)
{
	assert (channels > 0) ;

	MINPHASE_DATA *priv = (MINPHASE_DATA *) calloc (1, sizeof (MINPHASE_DATA)) ;
	if (priv == NULL)
		return NULL ;

	priv->magic = MINPHASE_MAGIC_MARKER ;

	priv->ring = (float *) calloc (MINPHASE_LEN * channels, sizeof (float)) ;
	priv->coeff = (float *) calloc ((MINPHASE_PHASES + 1) * MINPHASE_LEN, sizeof (float)) ;
	if (priv->ring == NULL || priv->coeff == NULL)
	{	free (priv->ring) ;
		free (priv->coeff) ;
		free (priv) ;
		return NULL ;
		} ;

	return priv ;
} /* minphase_data_new */

LIBSAMPLERATE_DLL_PRIVATE SRC_STATE *
minphase_state_new (int channels, SRC_ERROR *error)
{
	assert (channels > 0) ;
	assert (error != NULL) ;

	SRC_STATE *state = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (! state)
	{	*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
		} ;

	state->channels = channels ;
	state->mode = SRC_MODE_PROCESS ;

	state->private_data = minphase_data_new (channels) ;
	if (! state->private_data)
	{	free (state) ;
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
		} ;

	state->vt = &minphase_state_vt ;

	minphase_reset (state) ;

	*error = SRC_ERR_NO_ERROR ;

	return state ;
} /* minphase_state_new */

/*===================================================================================
*/

static void
minphase_reset (SRC_STATE *state)
{	MINPHASE_DATA *priv ;

	priv = (MINPHASE_DATA *) state->private_data ;
	if (priv == NULL)
		return ;

	priv->ratio_locked = false ;
	priv->ratio = 0.0 ;
	priv->inv_ratio = 0.0 ;
	priv->in_buffered = 0 ;
	priv->out_count = 0 ;

	/* Zero the history : the stream is preceded by zero input. */
	memset (priv->ring, 0, MINPHASE_LEN * state->channels * sizeof (priv->ring [0])) ;

	return ;
} /* minphase_reset */

static SRC_STATE *
minphase_copy (SRC_STATE *state)
{
	assert (state != NULL) ;

	if (state->private_data == NULL)
		return NULL ;

	SRC_STATE *to = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (! to)
		return NULL ;
	memcpy (to, state, sizeof (SRC_STATE)) ;

	MINPHASE_DATA *from_priv = (MINPHASE_DATA *) state->private_data ;
	MINPHASE_DATA *to_priv = (MINPHASE_DATA *) calloc (1, sizeof (MINPHASE_DATA)) ;
	if (! to_priv)
	{	free (to) ;
		return NULL ;
		} ;
	memcpy (to_priv, from_priv, sizeof (MINPHASE_DATA)) ;

	to_priv->ring = (float *) malloc (MINPHASE_LEN * state->channels * sizeof (float)) ;
	to_priv->coeff = (float *) malloc ((MINPHASE_PHASES + 1) * MINPHASE_LEN * sizeof (float)) ;
	if (! to_priv->ring || ! to_priv->coeff)
	{	free (to_priv->ring) ;
		free (to_priv->coeff) ;
		free (to_priv) ;
		free (to) ;
		return NULL ;
		} ;
	memcpy (to_priv->ring, from_priv->ring, MINPHASE_LEN * state->channels * sizeof (float)) ;
	memcpy (to_priv->coeff, from_priv->coeff, (MINPHASE_PHASES + 1) * MINPHASE_LEN * sizeof (float)) ;

	to->private_data = to_priv ;

	return to ;
} /* minphase_copy */

static void
minphase_close (SRC_STATE *state)
{
	if (state)
	{	MINPHASE_DATA *priv = (MINPHASE_DATA *) state->private_data ;
		if (priv)
		{	free (priv->ring) ;
			free (priv->coeff) ;
			free (priv) ;
			} ;
		free (state) ;
		}
} /* minphase_close */
