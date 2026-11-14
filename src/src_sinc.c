/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
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

#define	SINC_MAGIC_MARKER	MAKE_MAGIC (' ', 's', 'i', 'n', 'c', ' ')

/*========================================================================================
*/

#define MAKE_INCREMENT_T(x) 	((increment_t) (x))

#define	SHIFT_BITS				12
#define	FP_ONE					((double) (((increment_t) 1) << SHIFT_BITS))
#define	INV_FP_ONE				(1.0 / FP_ONE)

/* Customixe max channls from Kconfig. */
#ifndef CONFIG_CHAN_NR
#define MAX_CHANNELS			128
#else
#define MAX_CHANNELS 			CONFIG_CHAN_NR
#endif

/*========================================================================================
*/

typedef int32_t increment_t ;
typedef float	coeff_t ;
typedef int _CHECK_SHIFT_BITS[2 * (SHIFT_BITS < sizeof (increment_t) * 8 - 1) - 1]; /* sanity check. */

#ifdef ENABLE_SINC_FAST_CONVERTER
  #include "fastest_coeffs.h"
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
  #include "mid_qual_coeffs.h"
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
  #include "high_qual_coeffs.h"
#endif

typedef struct
{	int		sinc_magic_marker ;

	long	in_count, in_used ;
	long	out_count, out_gen ;

	int		coeff_half_len, index_inc ;

	double	src_ratio, input_index ;

	coeff_t const	*coeffs ;

	int		b_current, b_end, b_real_end, b_len ;

	/* Sure hope noone does more than 128 channels at once. */
	double left_calc [MAX_CHANNELS], right_calc [MAX_CHANNELS] ;

	float	*buffer ;

	/* The converter type this filter was created with. */
	int		converter_type ;

	/* Non zero between src_new()/src_reset() and the first committed input. */
	int		pristine ;

	/* Non NULL once src_prepare_sinc_switch() has succeeded. */
	struct SINC_SWITCH_tag	*sw ;
} SINC_FILTER ;

/* Coefficient set of one sinc quality, usable as an independent filter voice. */
typedef struct
{
	coeff_t const	*coeffs ;
	int				coeff_half_len, index_inc ;
} SINC_VOICE ;

typedef struct
{
	coeff_t const	*coeffs ;
	int				coeff_half_len, index_inc ;
	int				valid ;
} SINC_COEFF_DESC ;

typedef struct SINC_SWITCH_tag
{
	/* Quality whose coefficients are currently loaded into the SINC_FILTER.
	** The mere existence of this structure marks the instance as prepared and
	** survives src_reset().
	*/
	int		current_quality ;

	/* Crossfade bookkeeping. */
	int		switching ;
	int		old_quality, new_quality ;
	long	fade_total, fade_done ;

	/* Coefficient tables of all compiled in sinc qualities. */
	SINC_COEFF_DESC	descs [3] ;

	/* The second (new quality) voice while a crossfade is running. */
	SINC_VOICE	voice ;

	/* Scratch for that second voice, allocated once at preparation time so
	** that neither normal processing nor a switch allocates audio memory.
	*/
	double	sw_left_calc [MAX_CHANNELS], sw_right_calc [MAX_CHANNELS] ;
	float	sw_out [MAX_CHANNELS] ;
} SINC_SWITCH ;

static SRC_ERROR sinc_multichan_vari_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR sinc_hex_vari_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR sinc_quad_vari_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR sinc_stereo_vari_process (SRC_STATE *state, SRC_DATA *data) ;
static SRC_ERROR sinc_mono_vari_process (SRC_STATE *state, SRC_DATA *data) ;

static SRC_ERROR prepare_data (SINC_FILTER *filter, int channels, SRC_DATA *data,
			int work_half, int retain_half) WARN_UNUSED ;

static SRC_ERROR sinc_switch_process (SRC_STATE *state, SRC_DATA *data) WARN_UNUSED ;
static void sinc_calc_voice (SINC_FILTER *filter, const SINC_VOICE *voice, int channels,
				increment_t increment, increment_t start_filter_index, double scale,
				double *left, double *right, float *output) ;
static void sinc_load_quality (SINC_FILTER *filter, int converter_type) ;

static void sinc_reset (SRC_STATE *state) ;
static SRC_STATE *sinc_copy (SRC_STATE *state) ;
static void sinc_close (SRC_STATE *state) ;

static SRC_STATE_VT sinc_multichan_state_vt =
{
	sinc_multichan_vari_process,
	sinc_multichan_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close,
	sinc_prepare_switch,
	sinc_set_quality
} ;

static SRC_STATE_VT sinc_hex_state_vt =
{
	sinc_hex_vari_process,
	sinc_hex_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close,
	sinc_prepare_switch,
	sinc_set_quality
} ;

static SRC_STATE_VT sinc_quad_state_vt =
{
	sinc_quad_vari_process,
	sinc_quad_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close,
	sinc_prepare_switch,
	sinc_set_quality
} ;

static SRC_STATE_VT sinc_stereo_state_vt =
{
	sinc_stereo_vari_process,
	sinc_stereo_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close,
	sinc_prepare_switch,
	sinc_set_quality
} ;

static SRC_STATE_VT sinc_mono_state_vt =
{
	sinc_mono_vari_process,
	sinc_mono_vari_process,
	sinc_reset,
	sinc_copy,
	sinc_close,
	sinc_prepare_switch,
	sinc_set_quality
} ;

static inline increment_t
double_to_fp (double x)
{	return (increment_t) (psf_lrint ((x) * FP_ONE)) ;
} /* double_to_fp */

static inline increment_t
int_to_fp (int x)
{	return (((increment_t) (x)) << SHIFT_BITS) ;
} /* int_to_fp */

static inline int
fp_to_int (increment_t x)
{	return (((x) >> SHIFT_BITS)) ;
} /* fp_to_int */

static inline increment_t
fp_fraction_part (increment_t x)
{	return ((x) & ((((increment_t) 1) << SHIFT_BITS) - 1)) ;
} /* fp_fraction_part */

static inline double
fp_to_double (increment_t x)
{	return fp_fraction_part (x) * INV_FP_ONE ;
} /* fp_to_double */

static inline int
int_div_ceil (int divident, int divisor) /* == (int) ceil ((float) divident / divisor) */
{	assert (divident >= 0 && divisor > 0) ; /* For positive numbers only */
	return (divident + (divisor - 1)) / divisor ;
}

/*----------------------------------------------------------------------------------------
*/

LIBSAMPLERATE_DLL_PRIVATE const char*
sinc_get_name (int src_enum)
{
	switch (src_enum)
	{	case SRC_SINC_BEST_QUALITY :
			return "Best Sinc Interpolator" ;

		case SRC_SINC_MEDIUM_QUALITY :
			return "Medium Sinc Interpolator" ;

		case SRC_SINC_FASTEST :
			return "Fastest Sinc Interpolator" ;

		default: break ;
		} ;

	return NULL ;
} /* sinc_get_descrition */

LIBSAMPLERATE_DLL_PRIVATE const char*
sinc_get_description (int src_enum)
{
	switch (src_enum)
	{	case SRC_SINC_FASTEST :
			return "Band limited sinc interpolation, fastest, 97dB SNR, 80% BW." ;

		case SRC_SINC_MEDIUM_QUALITY :
			return "Band limited sinc interpolation, medium quality, 121dB SNR, 90% BW." ;

		case SRC_SINC_BEST_QUALITY :
			return "Band limited sinc interpolation, best quality, 144dB SNR, 96% BW." ;

		default :
			break ;
		} ;

	return NULL ;
} /* sinc_get_descrition */

static SINC_FILTER *
sinc_filter_new (int converter_type, int channels)
{
	assert (converter_type == SRC_SINC_FASTEST ||
		converter_type == SRC_SINC_MEDIUM_QUALITY ||
		converter_type == SRC_SINC_BEST_QUALITY) ;
	assert (channels > 0 && channels <= MAX_CHANNELS) ;

	SINC_FILTER *priv = (SINC_FILTER *) calloc (1, sizeof (SINC_FILTER)) ;
	if (priv)
	{
		priv->sinc_magic_marker = SINC_MAGIC_MARKER ;
		priv->converter_type = converter_type ;
		priv->pristine = SRC_TRUE ;
		switch (converter_type)
		{
#ifdef ENABLE_SINC_FAST_CONVERTER
		case SRC_SINC_FASTEST :
			priv->coeffs = fastest_coeffs.coeffs ;
			priv->coeff_half_len = ARRAY_LEN (fastest_coeffs.coeffs) - 2 ;
			priv->index_inc = fastest_coeffs.increment ;
			break ;
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
		case SRC_SINC_MEDIUM_QUALITY :
			priv->coeffs = slow_mid_qual_coeffs.coeffs ;
			priv->coeff_half_len = ARRAY_LEN (slow_mid_qual_coeffs.coeffs) - 2 ;
			priv->index_inc = slow_mid_qual_coeffs.increment ;
			break ;
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
		case SRC_SINC_BEST_QUALITY :
			priv->coeffs = slow_high_qual_coeffs.coeffs ;
			priv->coeff_half_len = ARRAY_LEN (slow_high_qual_coeffs.coeffs) - 2 ;
			priv->index_inc = slow_high_qual_coeffs.increment ;
			break ;
#endif
		}

		priv->b_len = 3 * (int) psf_lrint ((priv->coeff_half_len + 2.0) / priv->index_inc * SRC_MAX_RATIO + 1) ;
		priv->b_len = MAX (priv->b_len, 4096) ;
		priv->b_len *= channels ;
		priv->b_len += 1 ; // There is a <= check against samples_in_hand requiring a buffer bigger than the calculation above


		priv->buffer = (float *) calloc (priv->b_len + channels, sizeof (float)) ;
		if (!priv->buffer)
		{
			free (priv) ;
			priv = NULL ;
		}
	}

	return priv ;
}

LIBSAMPLERATE_DLL_PRIVATE SRC_STATE *
sinc_state_new (int converter_type, int channels, SRC_ERROR *error)
{
	assert (converter_type == SRC_SINC_FASTEST ||
		converter_type == SRC_SINC_MEDIUM_QUALITY ||
		converter_type == SRC_SINC_BEST_QUALITY) ;
	assert (channels > 0) ;
	assert (error != NULL) ;

	if (channels > MAX_CHANNELS)
	{
		*error = SRC_ERR_BAD_CHANNEL_COUNT ;
		return NULL ;
	}

	SRC_STATE *state = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!state)
	{
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->channels = channels ;
	state->mode = SRC_MODE_PROCESS ;

	if (state->channels == 1)
		state->vt = &sinc_mono_state_vt ;
	else if (state->channels == 2)
		state->vt = &sinc_stereo_state_vt ;
	else if (state->channels == 4)
		state->vt = &sinc_quad_state_vt ;
	else if (state->channels == 6)
		state->vt = &sinc_hex_state_vt ;
	else
		state->vt = &sinc_multichan_state_vt ;

	state->private_data = sinc_filter_new (converter_type, state->channels) ;
	if (!state->private_data)
	{
		free (state) ;
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	sinc_reset (state) ;

	*error = SRC_ERR_NO_ERROR ;

	return state ;
}

/*----------------------------------------------------------------------------------------
**	Seamless quality switching.
*/

static void
sinc_fill_descs (SINC_SWITCH *sw)
{
	memset (sw->descs, 0, sizeof (sw->descs)) ;
#ifdef ENABLE_SINC_FAST_CONVERTER
	sw->descs [SRC_SINC_FASTEST].coeffs = fastest_coeffs.coeffs ;
	sw->descs [SRC_SINC_FASTEST].coeff_half_len = ARRAY_LEN (fastest_coeffs.coeffs) - 2 ;
	sw->descs [SRC_SINC_FASTEST].index_inc = fastest_coeffs.increment ;
	sw->descs [SRC_SINC_FASTEST].valid = SRC_TRUE ;
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
	sw->descs [SRC_SINC_MEDIUM_QUALITY].coeffs = slow_mid_qual_coeffs.coeffs ;
	sw->descs [SRC_SINC_MEDIUM_QUALITY].coeff_half_len = ARRAY_LEN (slow_mid_qual_coeffs.coeffs) - 2 ;
	sw->descs [SRC_SINC_MEDIUM_QUALITY].index_inc = slow_mid_qual_coeffs.increment ;
	sw->descs [SRC_SINC_MEDIUM_QUALITY].valid = SRC_TRUE ;
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
	sw->descs [SRC_SINC_BEST_QUALITY].coeffs = slow_high_qual_coeffs.coeffs ;
	sw->descs [SRC_SINC_BEST_QUALITY].coeff_half_len = ARRAY_LEN (slow_high_qual_coeffs.coeffs) - 2 ;
	sw->descs [SRC_SINC_BEST_QUALITY].index_inc = slow_high_qual_coeffs.increment ;
	sw->descs [SRC_SINC_BEST_QUALITY].valid = SRC_TRUE ;
#endif
} /* sinc_fill_descs */

/* Buffer length that sinc_filter_new() would choose for the given quality. */
static int
sinc_buffer_len_for (const SINC_COEFF_DESC *desc, int channels)
{
	int b_len ;

	b_len = 3 * (int) psf_lrint ((desc->coeff_half_len + 2.0) / desc->index_inc * SRC_MAX_RATIO + 1) ;
	b_len = MAX (b_len, 4096) ;
	b_len *= channels ;
	b_len += 1 ;

	return b_len ;
} /* sinc_buffer_len_for */

static const SINC_COEFF_DESC *
sinc_widest_desc (const SINC_SWITCH *sw)
{
	const SINC_COEFF_DESC *widest = NULL ;

	for (int k = 0 ; k < ARRAY_LEN (sw->descs) ; k++)
	{	if (! sw->descs [k].valid)
			continue ;
		if (widest == NULL ||
				(sw->descs [k].coeff_half_len + 2.0) / sw->descs [k].index_inc >
				(widest->coeff_half_len + 2.0) / widest->index_inc)
			widest = &sw->descs [k] ;
		} ;

	return widest ;
} /* sinc_widest_desc */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
sinc_prepare_switch (SRC_STATE *state)
{
	SINC_FILTER *filter ;
	SINC_SWITCH *sw ;
	const SINC_COEFF_DESC *widest ;
	int new_b_len ;

	if (state == NULL || state->private_data == NULL)
		return SRC_ERR_BAD_STATE ;

	filter = (SINC_FILTER *) state->private_data ;

	/* Already prepared: keep the existing allocation, nothing to do. */
	if (filter->sw != NULL)
		return SRC_ERR_NO_ERROR ;

	if (! filter->pristine)
		return SRC_ERR_SINC_SWITCH_NOT_PRISTINE ;

	sw = (SINC_SWITCH *) calloc (1, sizeof (SINC_SWITCH)) ;
	if (sw == NULL)
		return SRC_ERR_MALLOC_FAILED ;

	sinc_fill_descs (sw) ;
	sw->current_quality = filter->converter_type ;
	sw->switching = SRC_FALSE ;
	sw->old_quality = sw->new_quality = filter->converter_type ;
	sw->fade_total = sw->fade_done = 0 ;

	widest = sinc_widest_desc (sw) ;
	if (widest == NULL)
	{	free (sw) ;
		return SRC_ERR_BAD_CONVERTER ;
		} ;

	/* Size the ring buffer for the widest window so no past sample is ever
	** overwritten while a narrower quality is running.
	*/
	new_b_len = sinc_buffer_len_for (widest, state->channels) ;

	if (new_b_len > filter->b_len)
	{	float *new_buffer = (float *) calloc (new_b_len + state->channels, sizeof (float)) ;
		if (new_buffer == NULL)
		{	free (sw) ;
			return SRC_ERR_MALLOC_FAILED ;
			} ;

		free (filter->buffer) ;
		filter->buffer = new_buffer ;
		filter->b_len = new_b_len ;
		} ;

	filter->b_current = filter->b_end = 0 ;
	filter->b_real_end = -1 ;

	filter->sw = sw ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_prepare_switch */

LIBSAMPLERATE_DLL_PRIVATE SRC_ERROR
sinc_set_quality (SRC_STATE *state, int converter_type, long fade_frames)
{
	SINC_FILTER *filter ;
	SINC_SWITCH *sw ;
	const SINC_COEFF_DESC *desc ;

	if (state == NULL || state->private_data == NULL)
		return SRC_ERR_BAD_STATE ;

	filter = (SINC_FILTER *) state->private_data ;

	if (fade_frames < 0)
		return SRC_ERR_BAD_FADE_LEN ;

	if (filter->sw == NULL)
		return SRC_ERR_SINC_SWITCH_NOT_PREPARED ;

	sw = filter->sw ;

	if (converter_type < 0 || converter_type >= ARRAY_LEN (sw->descs) ||
			! sw->descs [converter_type].valid)
		return SRC_ERR_BAD_CONVERTER ;

	if (sw->switching)
		return SRC_ERR_SINC_SWITCH_ACTIVE ;

	/* Same quality request never produces a transition. */
	if (converter_type == sw->current_quality)
		return SRC_ERR_NO_ERROR ;

	desc = &sw->descs [converter_type] ;

	if (fade_frames == 0)
	{	/* Switch from the next output frame onwards; history is untouched. */
		sinc_load_quality (filter, converter_type) ;
		return SRC_ERR_NO_ERROR ;
		} ;

	sw->switching = SRC_TRUE ;
	sw->old_quality = sw->current_quality ;
	sw->new_quality = converter_type ;
	sw->fade_total = fade_frames ;
	sw->fade_done = 0 ;

	sw->voice.coeffs = desc->coeffs ;
	sw->voice.coeff_half_len = desc->coeff_half_len ;
	sw->voice.index_inc = desc->index_inc ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_set_quality */

static void
sinc_load_quality (SINC_FILTER *filter, int converter_type)
{
	const SINC_COEFF_DESC *desc ;

	if (filter->sw == NULL)
		return ;

	desc = &filter->sw->descs [converter_type] ;
	if (! desc->valid)
		return ;

	filter->coeffs = desc->coeffs ;
	filter->coeff_half_len = desc->coeff_half_len ;
	filter->index_inc = desc->index_inc ;
	filter->converter_type = converter_type ;
	filter->sw->current_quality = converter_type ;
} /* sinc_load_quality */

static void
sinc_reset (SRC_STATE *state)
{	SINC_FILTER *filter ;

	filter = (SINC_FILTER*) state->private_data ;
	if (filter == NULL)
		return ;

	filter->b_current = filter->b_end = 0 ;
	filter->b_real_end = -1 ;

	filter->src_ratio = filter->input_index = 0.0 ;

	memset (filter->buffer, 0, filter->b_len * sizeof (filter->buffer [0])) ;

	/* Set this for a sanity check */
	memset (filter->buffer + filter->b_len, 0xAA, state->channels * sizeof (filter->buffer [0])) ;

	filter->pristine = SRC_TRUE ;

	if (filter->sw != NULL)
	{	/* An unfinished transition is discarded and the previously stable
		** (old) quality is restored. Preparation itself is retained.
		*/
		if (filter->sw->switching)
			sinc_load_quality (filter, filter->sw->old_quality) ;

		filter->sw->switching = SRC_FALSE ;
		filter->sw->fade_total = filter->sw->fade_done = 0 ;
		filter->sw->old_quality = filter->sw->new_quality = filter->sw->current_quality ;
		filter->sw->voice.coeffs = NULL ;
		} ;
} /* sinc_reset */

static SRC_STATE *
sinc_copy (SRC_STATE *state)
{
	assert (state != NULL) ;

	if (state->private_data == NULL)
		return NULL ;

	SRC_STATE *to = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!to)
		return NULL ;
	memcpy (to, state, sizeof (SRC_STATE)) ;


	SINC_FILTER* from_filter = (SINC_FILTER*) state->private_data ;
	SINC_FILTER *to_filter = (SINC_FILTER *) calloc (1, sizeof (SINC_FILTER)) ;
	if (!to_filter)
	{
		free (to) ;
		return NULL ;
	}
	memcpy (to_filter, from_filter, sizeof (SINC_FILTER)) ;
	to_filter->buffer = (float *) malloc (sizeof (float) * (from_filter->b_len + state->channels)) ;
	if (!to_filter->buffer)
	{
		free (to) ;
		free (to_filter) ;
		return NULL ;
	}
	memcpy (to_filter->buffer, from_filter->buffer, sizeof (float) * (from_filter->b_len + state->channels)) ;

	to->private_data = to_filter ;

	/* Deep copy the switch state so a clone can continue a half finished
	** crossfade independently of the original.
	*/
	if (from_filter->sw != NULL)
	{
		SINC_SWITCH *to_sw = (SINC_SWITCH *) calloc (1, sizeof (SINC_SWITCH)) ;
		if (to_sw == NULL)
		{
			free (to_filter->buffer) ;
			free (to_filter) ;
			free (to) ;
			return NULL ;
		}
		memcpy (to_sw, from_filter->sw, sizeof (SINC_SWITCH)) ;
		to_filter->sw = to_sw ;
	}

	return to ;
} /* sinc_copy */

/*========================================================================================
**	Beware all ye who dare pass this point. There be dragons here.
*/

static inline double
calc_output_single (SINC_FILTER *filter, increment_t increment, increment_t start_filter_index)
{	double		fraction, left, right, icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = -data_index ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps ;
	}
	left = 0.0 ;
	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index < filter->b_len) ;
		assert (data_index < filter->b_end) ;
		left += icoeff * filter->buffer [data_index] ;

		filter_index -= increment ;
		data_index = data_index + 1 ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + 1 + coeff_count ;

	right = 0.0 ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index < filter->b_len) ;
		assert (data_index < filter->b_end) ;
		right += icoeff * filter->buffer [data_index] ;

		filter_index -= increment ;
		data_index = data_index - 1 ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	return (left + right) ;
} /* calc_output_single */

static SRC_ERROR
sinc_mono_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* Prepared (switch capable) instances share one generic voice aware loop. */
	if (filter->sw != NULL)
		return sinc_switch_process (state, data) ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (psf_lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate > filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		data->data_out [filter->out_gen] = (float) ((float_increment / filter->index_inc) *
										calc_output_single (filter, increment, start_filter_index)) ;
		filter->out_gen ++ ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_mono_vari_process */

static inline void
calc_output_stereo (SINC_FILTER *filter, int channels, increment_t increment, increment_t start_filter_index, double scale, float * output)
{	double		fraction, left [2], right [2], icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, 2) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * 2;
	}
	left [0] = left [1] = 0.0 ;
	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 1 < filter->b_len) ;
		assert (data_index + 1 < filter->b_end) ;
		for (int ch = 0; ch < 2; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + 2 ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	right [0] = right [1] = 0.0 ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 1 < filter->b_len) ;
		assert (data_index + 1 < filter->b_end) ;
		for (int ch = 0; ch < 2; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index - 2 ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for (int ch = 0; ch < 2; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;
} /* calc_output_stereo */

SRC_ERROR
sinc_stereo_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* Prepared (switch capable) instances share one generic voice aware loop. */
	if (filter->sw != NULL)
		return sinc_switch_process (state, data) ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (psf_lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_stereo (filter, state->channels, increment, start_filter_index, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += 2 ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_stereo_vari_process */

static inline void
calc_output_quad (SINC_FILTER *filter, int channels, increment_t increment, increment_t start_filter_index, double scale, float * output)
{	double		fraction, left [4], right [4], icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, 4) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * 4;
	}
	left [0] = left [1] = left [2] = left [3] = 0.0 ;
	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 3 < filter->b_len) ;
		assert (data_index + 3 < filter->b_end) ;
		for (int ch = 0; ch < 4; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + 4 ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	right [0] = right [1] = right [2] = right [3] = 0.0 ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 3 < filter->b_len) ;
		assert (data_index + 3 < filter->b_end) ;
		for (int ch = 0; ch < 4; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;


		filter_index -= increment ;
		data_index = data_index - 4 ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for (int ch = 0; ch < 4; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;
} /* calc_output_quad */

SRC_ERROR
sinc_quad_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* Prepared (switch capable) instances share one generic voice aware loop. */
	if (filter->sw != NULL)
		return sinc_switch_process (state, data) ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (psf_lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_quad (filter, state->channels, increment, start_filter_index, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += 4 ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_quad_vari_process */

static inline void
calc_output_hex (SINC_FILTER *filter, int channels, increment_t increment, increment_t start_filter_index, double scale, float * output)
{	double		fraction, left [6], right [6], icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, 6) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * 6;
	}
	left [0] = left [1] = left [2] = left [3] = left [4] = left [5] = 0.0 ;
	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 5 < filter->b_len) ;
		assert (data_index + 5 < filter->b_end) ;
		for (int ch = 0; ch < 6; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + 6 ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	right [0] = right [1] = right [2] = right [3] = right [4] = right [5] = 0.0 ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + 5 < filter->b_len) ;
		assert (data_index + 5 < filter->b_end) ;
		for (int ch = 0; ch < 6; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index - 6 ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for (int ch = 0; ch < 6; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;
} /* calc_output_hex */

SRC_ERROR
sinc_hex_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* Prepared (switch capable) instances share one generic voice aware loop. */
	if (filter->sw != NULL)
		return sinc_switch_process (state, data) ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (psf_lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_hex (filter, state->channels, increment, start_filter_index, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += 6 ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_hex_vari_process */

static inline void
calc_output_multi (SINC_FILTER *filter, increment_t increment, increment_t start_filter_index, int channels, double scale, float * output)
{	double		fraction, icoeff ;
	/* The following line is 1999 ISO Standard C. If your compiler complains, get a better compiler. */
	double		*left, *right ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	left = filter->left_calc ;
	right = filter->right_calc ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, channels) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * channels ;
	}

	memset (left, 0, sizeof (left [0]) * channels) ;

	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;

		assert (data_index >= 0 && data_index + channels - 1 < filter->b_len) ;
		assert (data_index + channels - 1 < filter->b_end) ;
		for (int ch = 0; ch < channels; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + channels ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	memset (right, 0, sizeof (right [0]) * channels) ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		icoeff = filter->coeffs [indx] + fraction * (filter->coeffs [indx + 1] - filter->coeffs [indx]) ;
		assert (data_index >= 0 && data_index + channels - 1 < filter->b_len) ;
		assert (data_index + channels - 1 < filter->b_end) ;
		for (int ch = 0; ch < channels; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index - channels ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for(int ch = 0; ch < channels; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;

	return ;
} /* calc_output_multi */

/*
** Convolution for an arbitrary quality voice over the shared ring buffer.
** Identical maths to calc_output_multi(), but using the supplied coefficient
** set and caller provided scratch arrays, so the second quality during a
** crossfade needs neither its own buffer nor its own converter instance.
*/
static void
sinc_calc_voice (SINC_FILTER *filter, const SINC_VOICE *voice, int channels,
			increment_t increment, increment_t start_filter_index, double scale,
			double *left, double *right, float *output)
{	double		fraction, icoeff ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	max_filter_index = int_to_fp (voice->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0)
	{	int steps = int_div_ceil (-data_index, channels) ;
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * channels ;
	}

	memset (left, 0, sizeof (left [0]) * channels) ;

	while (filter_index >= MAKE_INCREMENT_T (0))
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < voice->coeff_half_len + 2) ;
		icoeff = voice->coeffs [indx] + fraction * (voice->coeffs [indx + 1] - voice->coeffs [indx]) ;

		assert (data_index >= 0 && data_index + channels - 1 < filter->b_len) ;
		assert (data_index + channels - 1 < filter->b_end) ;
		for (int ch = 0; ch < channels; ch++)
			left [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + channels ;
	} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	memset (right, 0, sizeof (right [0]) * channels) ;
	do
	{	fraction = fp_to_double (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < voice->coeff_half_len + 2) ;
		icoeff = voice->coeffs [indx] + fraction * (voice->coeffs [indx + 1] - voice->coeffs [indx]) ;

		assert (data_index >= 0 && data_index + channels - 1 < filter->b_len) ;
		assert (data_index + channels - 1 < filter->b_end) ;
		for (int ch = 0; ch < channels; ch++)
			right [ch] += icoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index - channels ;
	}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for (int ch = 0; ch < channels; ch++)
		output [ch] = (float) (scale * (left [ch] + right [ch])) ;
} /* sinc_calc_voice */

/* Half window (in samples) that a voice needs around the current point. */
static int
sinc_voice_half_len (const SINC_VOICE *voice, int channels, double min_ratio)
{	double count ;

	count = (voice->coeff_half_len + 2.0) / voice->index_inc ;
	if (min_ratio < 1.0)
		count /= min_ratio ;

	return channels * (int) (psf_lrint (count) + 1) ;
} /* sinc_voice_half_len */

/*
** Retention window for a prepared instance: enough history for the widest
** compiled in quality at the current ratio, so that even after a long run on
** a narrow quality every past sample a later upgrade needs is still present.
*/
static int
sinc_retain_half_len (const SINC_SWITCH *sw, int channels, double min_ratio)
{	int half_len = 0 ;

	for (int k = 0 ; k < ARRAY_LEN (sw->descs) ; k++)
	{	SINC_VOICE voice ;
		if (! sw->descs [k].valid)
			continue ;
		voice.coeffs = sw->descs [k].coeffs ;
		voice.coeff_half_len = sw->descs [k].coeff_half_len ;
		voice.index_inc = sw->descs [k].index_inc ;
		half_len = MAX (half_len, sinc_voice_half_len (&voice, channels, min_ratio)) ;
	} ;

	return half_len ;
} /* sinc_retain_half_len */

/*
** Process loop used by every prepared (switch capable) sinc instance,
** independently of channel count. It drives one shared ring buffer and one
** source phase; while a crossfade is active the new quality is evaluated as a
** second coefficient voice at the very same source coordinate and blended
** into the current quality's result.
*/
static SRC_ERROR
sinc_switch_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	SINC_SWITCH *sw ;
	double		input_index, src_ratio, float_increment, terminate, rem ;
	double		voice_increment_f, voice_scale ;
	increment_t	increment, start_filter_index ;
	int			work_half, retain_half, samples_in_hand ;
	SINC_VOICE	current_voice ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;
	sw = filter->sw ;

	if (sw == NULL)
		return SRC_ERR_SINC_SWITCH_NOT_PREPARED ;

	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Work window: the active voice (or both voices during a crossfade). It
	** drives the refill threshold and end of input handling, so when no
	** crossfade runs it is exactly the ordinary converter's window and the
	** output frame count is unchanged.
	*/
	current_voice.coeffs = filter->coeffs ;
	current_voice.coeff_half_len = filter->coeff_half_len ;
	current_voice.index_inc = filter->index_inc ;
	work_half = sinc_voice_half_len (&current_voice, state->channels,
							MIN (state->last_ratio, data->src_ratio)) ;
	if (sw->switching)
		work_half = MAX (work_half, sinc_voice_half_len (&sw->voice, state->channels,
							MIN (state->last_ratio, data->src_ratio))) ;

	/* Retention window: the widest compiled in quality. It only governs how
	** much history is kept during ring buffer compaction, so a long run on a
	** narrow quality never overwrites samples a later upgrade still needs.
	*/
	retain_half = sinc_retain_half_len (sw, state->channels,
							MIN (state->last_ratio, data->src_ratio)) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	while (filter->out_gen < filter->out_count)
	{
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= work_half)
		{	if ((state->error = prepare_data (filter, state->channels, data, work_half, retain_half)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= work_half)
				break ;
			} ;

		/* This is the termination condition. The specialised mono loop uses a
		** strict comparison while all multi channel loops use >=; preserve each
		** original behaviour so prepared instances stay bit compatible.
		*/
		if (filter->b_real_end >= 0)
		{	double reach = filter->b_current + input_index + terminate ;
			int past_end = (state->channels == 1) ? (reach > filter->b_real_end)
												  : (reach >= filter->b_real_end) ;
			if (past_end)
				break ;
		} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		/* Current (old) quality result at this source coordinate. */
		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;
		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_multi (filter, increment, start_filter_index, state->channels,
				float_increment / filter->index_inc, data->data_out + filter->out_gen) ;

		if (sw->switching)
		{	/* New quality result at the identical source coordinate. */
			voice_increment_f = sw->voice.index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
			increment_t voice_increment = double_to_fp (voice_increment_f) ;
			increment_t voice_start = double_to_fp (input_index * voice_increment_f) ;
			voice_scale = voice_increment_f / sw->voice.index_inc ;

			sinc_calc_voice (filter, &sw->voice, state->channels, voice_increment, voice_start,
					voice_scale, sw->sw_left_calc, sw->sw_right_calc, sw->sw_out) ;

			/* Frame j (1 .. N): old * (1 - j/N) + new * (j/N). */
			sw->fade_done ++ ;
			double w_new = (double) sw->fade_done / (double) sw->fade_total ;
			double w_old = 1.0 - w_new ;

			for (int ch = 0 ; ch < state->channels ; ch++)
			{	float old_val = data->data_out [filter->out_gen + ch] ;
				data->data_out [filter->out_gen + ch] = (float) (w_old * old_val + w_new * sw->sw_out [ch]) ;
				} ;

			if (sw->fade_done >= sw->fade_total)
			{	/* Transition complete: promote the new quality. */
				sw->switching = SRC_FALSE ;
				sinc_load_quality (filter, sw->new_quality) ;
				sw->voice.coeffs = NULL ;
				sw->old_quality = sw->new_quality = sw->current_quality ;
				sw->fade_total = sw->fade_done = 0 ;
				} ;
			} ;

		filter->out_gen += state->channels ;

		/* A single shared phase advances exactly once per output frame. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
	} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_switch_process */

static SRC_ERROR
sinc_multichan_vari_process (SRC_STATE *state, SRC_DATA *data)
{	SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (SINC_FILTER*) state->private_data ;

	/* Prepared (switch capable) instances share one generic voice aware loop. */
	if (filter->sw != NULL)
		return sinc_switch_process (state, data) ;

	/* If there is not a problem, this will be optimised out. */
	if (sizeof (filter->buffer [0]) != sizeof (data->data_in [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	filter->in_count = data->input_frames * state->channels ;
	filter->out_count = data->output_frames * state->channels ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (psf_lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = prepare_data (filter, state->channels, data, half_filter_chan_len, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. */
		if (filter->b_real_end >= 0)
		{	if (filter->b_current + input_index + terminate >= filter->b_real_end)
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		calc_output_multi (filter, increment, start_filter_index, state->channels, float_increment / filter->index_inc, data->data_out + filter->out_gen) ;
		filter->out_gen += state->channels ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used / state->channels ;
	data->output_frames_gen = filter->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* sinc_multichan_vari_process */

/*----------------------------------------------------------------------------------------
*/

static SRC_ERROR
prepare_data (SINC_FILTER *filter, int channels, SRC_DATA *data, int work_half, int retain_half)
{	int len = 0 ;

	if (filter->b_real_end >= 0)
		return SRC_ERR_NO_ERROR ;	/* Should be terminating. Just return. */

	if (data->data_in == NULL)
		return SRC_ERR_NO_ERROR ;

	if (filter->b_current == 0)
	{	/* Initial state. Set up zeros at the start of the buffer and
		** then load new data after that. The leading gap matches the work
		** window so output alignment is unchanged.
		*/
		len = filter->b_len - 2 * work_half ;

		filter->b_current = filter->b_end = work_half ;
		}
	else if (filter->b_end + retain_half + channels < filter->b_len)
	{	/*  Load data at current end position. */
		len = MAX (filter->b_len - filter->b_current - retain_half, 0) ;
		}
	else
	{	/* Move data at end of buffer back to the start of the buffer. The
		** retention window keeps the widest quality's worth of past samples so
		** a later upgrade never finds its history overwritten.
		*/
		int keep = MIN (retain_half, filter->b_current) ;
		len = filter->b_end - filter->b_current ;
		memmove (filter->buffer, filter->buffer + filter->b_current - keep,
						(keep + len) * sizeof (filter->buffer [0])) ;

		filter->b_current = keep ;
		filter->b_end = filter->b_current + len ;

		/* Now load data at current end of buffer. */
		len = MAX (filter->b_len - filter->b_current - retain_half, 0) ;
		} ;

	len = MIN ((int) (filter->in_count - filter->in_used), len) ;
	len -= (len % channels) ;

	if (len < 0 || filter->b_end + len > filter->b_len)
		return SRC_ERR_SINC_PREPARE_DATA_BAD_LEN ;

	memcpy (filter->buffer + filter->b_end, data->data_in + filter->in_used,
						len * sizeof (filter->buffer [0])) ;

	if (len > 0)
		filter->pristine = SRC_FALSE ;

	filter->b_end += len ;
	filter->in_used += len ;

	/* End of input handling always follows the active work window so that the
	** termination point and output frame count match an ordinary converter.
	*/
	if (filter->in_used == filter->in_count &&
			filter->b_end - filter->b_current < 2 * work_half && data->end_of_input)
	{	/* Handle the case where all data in the current buffer has been
		** consumed and this is the last buffer.
		*/

		if (filter->b_len - filter->b_end < work_half + 5)
		{	/* If necessary, move data down to the start of the buffer. */
			len = filter->b_end - filter->b_current ;
			memmove (filter->buffer, filter->buffer + filter->b_current - work_half,
							(work_half + len) * sizeof (filter->buffer [0])) ;

			filter->b_current = work_half ;
			filter->b_end = filter->b_current + len ;
			} ;

		filter->b_real_end = filter->b_end ;
		len = work_half + 5 ;

		if (len < 0 || filter->b_end + len > filter->b_len)
			len = filter->b_len - filter->b_end ;

		memset (filter->buffer + filter->b_end, 0, len * sizeof (filter->buffer [0])) ;
		filter->b_end += len ;
		} ;

	return SRC_ERR_NO_ERROR ;
} /* prepare_data */

static void
sinc_close (SRC_STATE *state)
{
	if (state)
	{
		SINC_FILTER *sinc = (SINC_FILTER *) state->private_data ;
		if (sinc)
		{
			if (sinc->buffer)
			{
				free (sinc->buffer) ;
				sinc->buffer = NULL ;
			}
			if (sinc->sw)
			{
				free (sinc->sw) ;
				sinc->sw = NULL ;
			}
			free (sinc) ;
			sinc = NULL ;
		}
		free (state) ;
		state = NULL ;
	}
} /* sinc_close */
