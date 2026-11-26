/*
** Copyright (c) 2026 libsamplerate contributors
** SPDX-License-Identifier: BSD-2-Clause
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "minphase_coeffs.h"

#define MINPHASE_MAGIC     MAKE_MAGIC ('m', 'p', 'h', 's', 'e', 'p')
#define MINPHASE_R_MIN     0.5
#define MINPHASE_R_MAX     2.0
#define MINPHASE_RING_FRAMES 64

typedef struct
{	int magic ;
	int channels ;

	bool ratio_locked ;
	double fixed_ratio ;

	/* Absolute frame counters since reset. */
	long long input_total ;
	long long output_total ;

	/* Interleaved ring, MINPHASE_RING_FRAMES frames long. */
	float *ring ;
} MINPHASE_STATE ;

static SRC_ERROR minphase_process (SRC_STATE *state, SRC_DATA *data) ;
static void minphase_reset (SRC_STATE *state) ;
static SRC_STATE *minphase_copy (SRC_STATE *state) ;
static void minphase_close (SRC_STATE *state) ;
static int minphase_set_ratio_hook (SRC_STATE *state, double new_ratio) ;

static SRC_STATE_VT minphase_state_vt =
{
	minphase_process,
	minphase_process,
	minphase_reset,
	minphase_copy,
	minphase_close,
	minphase_set_ratio_hook
} ;

LIBSAMPLERATE_DLL_PRIVATE const char *
minphase_get_name (int converter_type)
{
	if (converter_type == SRC_SINC_MINIMUM_PHASE)
		return "Minimum Phase Sinc Interpolator" ;
	return NULL ;
} /* minphase_get_name */

LIBSAMPLERATE_DLL_PRIVATE const char *
minphase_get_description (int converter_type)
{
	if (converter_type == SRC_SINC_MINIMUM_PHASE)
		return "Causal minimum-phase sinc lowpass for fixed output/input ratios from 0.5 to 2.0." ;
	return NULL ;
} /* minphase_get_description */

static int
ratio_supported (double ratio)
{
	return ratio >= MINPHASE_R_MIN && ratio <= MINPHASE_R_MAX ;
} /* ratio_supported */

static int
same_fixed_ratio (double a, double b)
{
	double scale = fmax (1.0, fmax (fabs (a), fabs (b))) ;
	return fabs (a - b) <= 1.0e-12 * scale ;
} /* same_fixed_ratio */

static SRC_ERROR
check_fixed_ratio (MINPHASE_STATE *priv, double ratio)
{
	if (! ratio_supported (ratio))
		return SRC_ERR_MINPHASE_BAD_RATIO ;

	if (priv->ratio_locked && ! same_fixed_ratio (priv->fixed_ratio, ratio))
		return SRC_ERR_NO_VARIABLE_RATIO ;

	return SRC_ERR_NO_ERROR ;
} /* check_fixed_ratio */

static int
minphase_set_ratio_hook (SRC_STATE *state, double new_ratio)
{
	MINPHASE_STATE *priv ;
	SRC_ERROR error ;

	if (state == NULL || state->private_data == NULL)
		return SRC_ERR_BAD_STATE ;

	priv = (MINPHASE_STATE *) state->private_data ;
	if (priv->magic != MINPHASE_MAGIC)
		return SRC_ERR_BAD_PRIV_PTR ;

	error = check_fixed_ratio (priv, new_ratio) ;
	state->error = error ;
	return (int) error ;
} /* minphase_set_ratio_hook */

static MINPHASE_STATE *
minphase_private_new (int channels)
{
	MINPHASE_STATE *priv ;

	priv = (MINPHASE_STATE *) calloc (1, sizeof (*priv)) ;
	if (priv == NULL)
		return NULL ;

	priv->ring = (float *) calloc ((size_t) channels * MINPHASE_RING_FRAMES,
					sizeof (float)) ;
	if (priv->ring == NULL)
	{	free (priv) ;
		return NULL ;
		} ;

	priv->magic = MINPHASE_MAGIC ;
	priv->channels = channels ;
	return priv ;
} /* minphase_private_new */

LIBSAMPLERATE_DLL_PRIVATE SRC_STATE *
minphase_state_new (int channels, SRC_ERROR *error)
{
	SRC_STATE *state ;

	if (channels < 1 || error == NULL)
	{	if (error != NULL)
			*error = SRC_ERR_BAD_CHANNEL_COUNT ;
		return NULL ;
		} ;

	assert (channels > 0) ;
	assert (error != NULL) ;

	state = (SRC_STATE *) calloc (1, sizeof (*state)) ;
	if (state == NULL)
	{	*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
		} ;

	state->private_data = minphase_private_new (channels) ;
	if (state->private_data == NULL)
	{	free (state) ;
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
		} ;

	state->channels = channels ;
	state->mode = SRC_MODE_PROCESS ;
	state->vt = &minphase_state_vt ;
	state->converter_type = SRC_SINC_MINIMUM_PHASE ;

	minphase_reset (state) ;
	*error = SRC_ERR_NO_ERROR ;
	return state ;
} /* minphase_state_new */

static void
minphase_reset (SRC_STATE *state)
{
	MINPHASE_STATE *priv ;

	if (state == NULL || state->private_data == NULL)
		return ;

	priv = (MINPHASE_STATE *) state->private_data ;
	if (priv->magic != MINPHASE_MAGIC)
		return ;

	priv->ratio_locked = false ;
	priv->fixed_ratio = 0.0 ;
	priv->input_total = 0 ;
	priv->output_total = 0 ;
	memset (priv->ring, 0, (size_t) state->channels *
			MINPHASE_RING_FRAMES * sizeof (float)) ;
} /* minphase_reset */

static SRC_STATE *
minphase_copy (SRC_STATE *orig)
{
	MINPHASE_STATE *from, *to_priv ;
	SRC_STATE *to ;

	if (orig == NULL || orig->private_data == NULL)
		return NULL ;

	from = (MINPHASE_STATE *) orig->private_data ;
	if (from->magic != MINPHASE_MAGIC)
		return NULL ;

	to = (SRC_STATE *) calloc (1, sizeof (*to)) ;
	if (to == NULL)
		return NULL ;
	memcpy (to, orig, sizeof (*to)) ;

	to_priv = (MINPHASE_STATE *) calloc (1, sizeof (*to_priv)) ;
	if (to_priv == NULL)
	{	free (to) ;
		return NULL ;
		} ;
	memcpy (to_priv, from, sizeof (*to_priv)) ;

	to_priv->ring = (float *) malloc ((size_t) orig->channels *
					MINPHASE_RING_FRAMES * sizeof (float)) ;
	if (to_priv->ring == NULL)
	{	free (to_priv) ;
		free (to) ;
		return NULL ;
		} ;
	memcpy (to_priv->ring, from->ring, (size_t) orig->channels *
			MINPHASE_RING_FRAMES * sizeof (float)) ;

	to->private_data = to_priv ;
	return to ;
} /* minphase_copy */

static void
minphase_close (SRC_STATE *state)
{
	MINPHASE_STATE *priv ;

	if (state == NULL)
		return ;

	priv = (MINPHASE_STATE *) state->private_data ;
	if (priv != NULL)
	{	if (priv->magic == MINPHASE_MAGIC)
			free (priv->ring) ;
		free (priv) ;
		} ;
	free (state) ;
} /* minphase_close */

static void
store_input_frame (MINPHASE_STATE *priv, int channels, long long absolute_index,
			const float *frame)
{
	size_t slot = (size_t) (absolute_index % MINPHASE_RING_FRAMES) ;
	int ch ;

	/* Absolute indices are non-negative here. */
	if (absolute_index < 0)
		return ;

	for (ch = 0 ; ch < channels ; ++ch)
		priv->ring [slot * channels + ch] = frame [ch] ;
} /* store_input_frame */

static float
ring_sample (const MINPHASE_STATE *priv, int channels, int ch,
		long long absolute_index)
{
	size_t slot ;

	/* Indices before reset are explicitly zero history. */
	if (absolute_index < 0)
		return 0.0f ;

	slot = (size_t) (absolute_index % MINPHASE_RING_FRAMES) ;
	return priv->ring [slot * channels + ch] ;
} /* ring_sample */

static SRC_ERROR
minphase_process (SRC_STATE *state, SRC_DATA *data)
{
	MINPHASE_STATE *priv ;
	SRC_ERROR error ;
	long input_frames_used = 0 ;
	long output_frames_gen = 0 ;
	long long entry_input_total ;
	int ch, tap ;

	if (state == NULL)
		return SRC_ERR_BAD_STATE ;
	if (data == NULL)
		return SRC_ERR_BAD_DATA ;
	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	priv = (MINPHASE_STATE *) state->private_data ;
	if (priv->magic != MINPHASE_MAGIC)
		return SRC_ERR_BAD_PRIV_PTR ;

	error = check_fixed_ratio (priv, data->src_ratio) ;
	if (error != SRC_ERR_NO_ERROR)
	{	state->error = error ;
		return error ;
		} ;

	/*
	** The first call carrying input audio fixes the conversion ratio.
	** Zero-length probing calls stay non-committal.
	*/
	if (! priv->ratio_locked && data->input_frames > 0)
	{	priv->ratio_locked = true ;
		priv->fixed_ratio = data->src_ratio ;
		} ;

	/* Without an input frame zero is the only history, and coordinate 0
	** still needs the first delivered input frame. */
	if (! priv->ratio_locked)
	{	data->input_frames_used = 0 ;
		data->output_frames_gen = 0 ;
		return SRC_ERR_NO_ERROR ;
		} ;

	state->error = SRC_ERR_NO_ERROR ;
	entry_input_total = priv->input_total ;

	while (output_frames_gen < data->output_frames)
	{
		long double source_position ;
		long long integer_position ;
		long double fraction_ld ;
		double fraction, phase_position, weight ;
		double coeff [MINPHASE_TAPS] ;
		int phase_index ;
		long long frame_to_consume ;

		/*
		** The required coordinate is exactly output_index / ratio.
		** Using the absolute output count keeps chunk boundaries from
		** changing rounding or the shared fractional position.
		*/
		source_position = (long double) priv->output_total /
				(long double) priv->fixed_ratio ;
		integer_position = (long long) floorl (source_position) ;
		fraction_ld = source_position - (long double) integer_position ;
		if (fraction_ld < 0.0L)
		{	fraction_ld += 1.0L ;
			integer_position -- ;
			} ;
		if (fraction_ld >= 1.0L)
		{	fraction_ld = 0.0L ;
			integer_position ++ ;
			} ;
		fraction = (double) fraction_ld ;

		/* Make input frame integer_position available. */
		while (priv->input_total <= integer_position)
		{
			if (input_frames_used >= data->input_frames)
				goto cannot_generate_more ;

			frame_to_consume = priv->input_total - entry_input_total ;
			store_input_frame (priv, state->channels, priv->input_total,
				data->data_in + frame_to_consume * state->channels) ;
			priv->input_total ++ ;
			input_frames_used ++ ;
			} ;

		phase_position = fraction * MINPHASE_PHASES ;
		phase_index = (int) phase_position ;
		weight = phase_position - phase_index ;
		if (phase_index >= MINPHASE_PHASES)
		{	phase_index = MINPHASE_PHASES - 1 ;
			weight = 1.0 ;
			} ;

		for (tap = 0 ; tap < MINPHASE_TAPS ; ++tap)
			coeff [tap] = (1.0 - weight) *
					minphase_poly [phase_index] [tap] +
					weight * minphase_poly [phase_index + 1] [tap] ;

		for (ch = 0 ; ch < state->channels ; ++ch)
		{
			double accumulator = 0.0 ;
			long long input_index ;

			for (tap = 0 ; tap < MINPHASE_TAPS ; ++tap)
			{	input_index = integer_position - tap ;
				accumulator += coeff [tap] *
					ring_sample (priv, state->channels, ch, input_index) ;
				} ;

			data->data_out [output_frames_gen * state->channels + ch] =
				(float) accumulator ;
			} ;

		output_frames_gen ++ ;
		priv->output_total ++ ;
		} ;

cannot_generate_more:

	/*
	** A one-frame delivery followed by a one-frame output capacity can
	** otherwise leave the caller's only frame unconsumed when ratio >= 1
	** reuses an integer position.  Buffer one such frame without advancing
	** farther than the fixed ring history can support.
	*/
	if (output_frames_gen > 0 && input_frames_used < data->input_frames)
	{
		store_input_frame (priv, state->channels, priv->input_total,
			data->data_in + input_frames_used * state->channels) ;
		priv->input_total ++ ;
		input_frames_used ++ ;
		} ;

	data->input_frames_used = input_frames_used ;
	data->output_frames_gen = output_frames_gen ;
	state->last_ratio = priv->fixed_ratio ;

	return SRC_ERR_NO_ERROR ;
} /* minphase_process */
