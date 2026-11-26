/*
** minimum_phase_test.c - behavioural and numerical checks for
** SRC_SINC_MINIMUM_PHASE.
**
** The five required cases are causal, magnitude, phase, streaming and
** lifecycle.  On success each prints:
**     MINIMUM_PHASE_CASE <name> PASS
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <samplerate.h>
#include "../src/common.h"

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif

#define CHECK(condition) \
	do { if (! (condition)) \
	{	fprintf (stderr, "\nFAIL %s:%d: %s\n", __FILE__, __LINE__, #condition) ; \
		exit (1) ; \
	} } while (0)

#define CONVERTER SRC_SINC_MINIMUM_PHASE

/*------------------------------------------------------------------------------
** Small helpers.
*/

static float
pseudo_random (unsigned *seed)
{
	*seed = *seed * 1664525u + 1013904223u ;
	return (float) (((*seed >> 8) & 0xffffffu) / (double) 0xffffff * 2.0 - 1.0) ;
} /* pseudo_random */

static float *
make_random_frames (long frames, int channels, unsigned seed)
{
	float *data = malloc ((size_t) frames * channels * sizeof (float)) ;
	long i ;
	CHECK (data != NULL) ;
	for (i = 0 ; i < frames * channels ; ++i)
		data [i] = pseudo_random (&seed) * 0.7f ;
	return data ;
} /* make_random_frames */

static double
amplitude_db (double measured, double reference)
{
	return 20.0 * log10 (fmax (measured, 1.0e-20) / reference) ;
} /* amplitude_db */

/* Analytic sine/cosine projection at cycles/output-frame frequency. */
static double
project_amplitude (const float *data, int channels, int channel,
			long start, long frames, double frequency)
{
	double c_sum = 0.0, s_sum = 0.0 ;
	long i ;

	for (i = 0 ; i < frames ; ++i)
	{	double sample = data [(start + i) * channels + channel] ;
		double angle = 2.0 * M_PI * frequency * i ;
		c_sum += sample * cos (angle) ;
		s_sum += sample * sin (angle) ;
		} ;

	c_sum *= 2.0 / frames ;
	s_sum *= 2.0 / frames ;
	return hypot (c_sum, s_sum) ;
} /* project_amplitude */

static long
expected_output_frames (long input_frames, double ratio)
{
	return (long) ceil ((double) input_frames * ratio - 1.0e-12) ;
} /* expected_output_frames */

static float *
src_simple_convert (const float *input, long input_frames, int channels,
			double ratio, long *output_frames_gen)
{
	long output_capacity = input_frames * 2 + 128 ;
	float *output = calloc ((size_t) output_capacity * channels, sizeof (float)) ;
	SRC_DATA data ;
	int error ;

	CHECK (output != NULL) ;
	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.input_frames = input_frames ;
	data.data_out = output ;
	data.output_frames = output_capacity ;
	data.src_ratio = ratio ;
	data.end_of_input = 1 ;

	error = src_simple (&data, CONVERTER, channels) ;
	if (error)
	{	fprintf (stderr, "src_simple ratio %.17g: %s\n", ratio, src_strerror (error)) ;
		exit (1) ;
		} ;

	CHECK (data.input_frames_used == input_frames) ;
	CHECK (data.output_frames_gen > 0) ;
	*output_frames_gen = data.output_frames_gen ;
	return output ;
} /* src_simple_convert */

/*------------------------------------------------------------------------------
** Streaming driver.  Input delivery and output capacity are deliberately
** tiny and irregular.
*/

enum delivery_mode
{	DELIVERY_WHOLE = 0,
	DELIVERY_ONE = 1,
	DELIVERY_MIXED = 2
} ;

static long
mixed_chunk_frames (long call_index)
{
	static const long chunks [] = { 1, 3, 19, 7, 29, 11 } ;
	return chunks [call_index % (long) (sizeof (chunks) / sizeof (chunks [0]))] ;
} /* mixed_chunk_frames */

static float *
process_convert (const float *input, long input_frames, int channels,
		double ratio, int delivery_mode, long *output_gen)
{
	static const long output_caps [] = { 1, 7, 113 } ;
	long expected = expected_output_frames (input_frames, ratio) ;
	long output_capacity = expected + 256 ;
	float *output = calloc ((size_t) output_capacity * channels, sizeof (float)) ;
	SRC_STATE *state ;
	SRC_DATA data ;
	int error ;
	long input_offset = 0, output_offset = 0 ;
	long chunk_end = 0, chunk_calls = 0, process_calls = 0 ;
	long iterations ;

	CHECK (output != NULL) ;
	state = src_new (CONVERTER, channels, &error) ;
	CHECK (state != NULL) ;

	for (iterations = 0 ; iterations < 200000 ; ++iterations)
	{
		long data_frames, output_frames ;
		const float *data_in ;
		long cap = output_caps [process_calls % 3] ;

		if (delivery_mode == DELIVERY_WHOLE)
		{	data_in = input + input_offset * channels ;
			data_frames = input_frames - input_offset ;
			}
		else
		{
			if (input_offset >= chunk_end && chunk_end < input_frames)
			{	long step = delivery_mode == DELIVERY_ONE ?
						1 : mixed_chunk_frames (chunk_calls) ;
				chunk_end += step ;
				if (chunk_end > input_frames)
					chunk_end = input_frames ;
				chunk_calls ++ ;
				} ;

			data_in = input + input_offset * channels ;
			data_frames = chunk_end - input_offset ;
			if (data_frames < 0)
				data_frames = 0 ;
			} ;

		CHECK (output_offset + cap <= output_capacity) ;
		output_frames = cap ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = data_frames > 0 ? data_in : NULL ;
		data.input_frames = data_frames ;
		data.data_out = output + output_offset * channels ;
		data.output_frames = output_frames ;
		data.src_ratio = ratio ;
		data.end_of_input = input_offset >= input_frames ;

		error = src_process (state, &data) ;
		if (error)
		{	fprintf (stderr, "streaming ratio %.17g: %s\n",
					ratio, src_strerror (error)) ;
			exit (1) ;
			} ;

		CHECK (data.input_frames_used >= 0) ;
		CHECK (data.input_frames_used <= data_frames) ;
		CHECK (data.output_frames_gen >= 0) ;
		CHECK (data.output_frames_gen <= output_frames) ;

		input_offset += data.input_frames_used ;
		output_offset += data.output_frames_gen ;
		process_calls ++ ;

		CHECK (input_offset <= input_frames) ;
		CHECK (output_offset <= output_capacity) ;

		if (input_offset >= input_frames && data.input_frames_used == 0 &&
				data.output_frames_gen == 0)
			break ;

		/* A fragmented window must eventually be advanced. */
		if (delivery_mode != DELIVERY_WHOLE && input_offset == chunk_end &&
				chunk_end >= input_frames &&
				data.output_frames_gen == 0 &&
				data.input_frames_used == 0)
			break ;
		} ;

	CHECK (iterations < 200000) ;
	CHECK (input_offset == input_frames) ;
	src_delete (state) ;

	*output_gen = output_offset ;
	return output ;
} /* process_convert */

typedef struct
{	const float *input ;
	long frames ;
	long position ;
	int channels ;
	long chunk_frames ;
} CALLBACK_DATA ;

static long
callback_supply (void *callback_data, float **data)
{
	CALLBACK_DATA *cbdata = (CALLBACK_DATA *) callback_data ;
	long frames = cbdata->chunk_frames ;

	if (cbdata->position >= cbdata->frames)
	{	*data = NULL ;
		return 0 ;
		} ;

	if (cbdata->position + frames > cbdata->frames)
		frames = cbdata->frames - cbdata->position ;

	*data = (float *) (cbdata->input + cbdata->position * cbdata->channels) ;
	cbdata->position += frames ;
	return frames ;
} /* callback_supply */

static float *
callback_convert (const float *input, long frames, int channels, double ratio,
		long wanted_frames, long *generated)
{
	static const long output_caps [] = { 1, 7, 113 } ;
	CALLBACK_DATA cbdata = { input, frames, 0, channels, 5 } ;
	SRC_STATE *state ;
	float *output ;
	long got = 0, calls = 0 ;
	int error ;

	output = calloc ((size_t) wanted_frames + 256, channels * sizeof (float)) ;
	CHECK (output != NULL) ;
	state = src_callback_new (callback_supply, CONVERTER, channels,
					&error, &cbdata) ;
	CHECK (state != NULL) ;

	while (got < wanted_frames)
	{	long want = output_caps [calls % 3] ;
		long read ;
		if (want > wanted_frames - got)
			want = wanted_frames - got ;
		read = src_callback_read (state, ratio, want,
					output + got * channels) ;
		CHECK (read > 0) ;
		got += read ;
		calls ++ ;
		} ;

	*generated = got ;
	src_delete (state) ;
	return output ;
} /* callback_convert */

static void
expect_same_samples (const float *a, long a_frames,
			const float *b, long b_frames, int channels)
{
	long i ;
	CHECK (a_frames == b_frames) ;
	for (i = 0 ; i < a_frames * channels ; ++i)
		CHECK (a [i] == b [i]) ;
} /* expect_same_samples */

/*------------------------------------------------------------------------------
** causal
*/

static void
causal_case (void)
{
	static const double ratios [] = { 0.5, 44100.0 / 48000.0, 1.0, 2.0 } ;
	const long frames = 257 ;
	int r_index ;

	for (r_index = 0 ; r_index < (int) (sizeof (ratios) / sizeof (ratios [0])) ;
			++r_index)
	{
		double ratio = ratios [r_index] ;
		unsigned seed = 0x1234 + r_index * 977 ;
		float *reference = make_random_frames (frames, 1, seed) ;
		float *mutable_input = malloc (frames * sizeof (float)) ;
		float ref_out [1024], causal_out [1024] ;
		SRC_STATE *ref_state, *causal_state ;
		int error ;
		long ref_in = 0, causal_in = 0 ;
		long ref_out_frames = 0, causal_out_frames = 0 ;
		long iter ;

		CHECK (mutable_input != NULL) ;
		memcpy (mutable_input, reference, frames * sizeof (float)) ;

		ref_state = src_new (CONVERTER, 1, &error) ;
		causal_state = src_new (CONVERTER, 1, &error) ;
		CHECK (ref_state && causal_state) ;

		for (iter = 0 ; iter < 200000 ; ++iter)
		{
			SRC_DATA rd, cd ;
			int mutated_index ;
			int ref_progress, causal_progress ;

			/* Mutate samples which have not yet been delivered. */
			if (causal_in < frames)
				for (mutated_index = (int) causal_in + 1 ;
						mutated_index < frames ; ++mutated_index)
					mutable_input [mutated_index] =
						pseudo_random (&seed) * 0.91f ;

			memset (&rd, 0, sizeof (rd)) ;
			rd.data_in = ref_in < frames ? reference + ref_in : NULL ;
			rd.input_frames = ref_in < frames ? 1 : 0 ;
			rd.data_out = ref_out + ref_out_frames ;
			rd.output_frames = 1 ;
			rd.src_ratio = ratio ;

			memset (&cd, 0, sizeof (cd)) ;
			cd.data_in = causal_in < frames ? mutable_input + causal_in : NULL ;
			cd.input_frames = causal_in < frames ? 1 : 0 ;
			cd.data_out = causal_out + causal_out_frames ;
			cd.output_frames = 1 ;
			cd.src_ratio = ratio ;

			error = src_process (ref_state, &rd) ;
			CHECK (error == 0) ;
			error = src_process (causal_state, &cd) ;
			CHECK (error == 0) ;

			/* The call has returned; restore future frames before delivery. */
			if (causal_in < frames - 1)
				memcpy (mutable_input + causal_in + 1,
					reference + causal_in + 1,
					(size_t) (frames - causal_in - 1) * sizeof (float)) ;

			CHECK (rd.input_frames_used == cd.input_frames_used) ;
			CHECK (rd.output_frames_gen == cd.output_frames_gen) ;

			if (rd.output_frames_gen == 1)
			{	CHECK (ref_out_frames < (long) (sizeof (ref_out) / sizeof (ref_out [0]))) ;
				CHECK (causal_out_frames == ref_out_frames) ;
				if (ref_out [ref_out_frames] != causal_out [causal_out_frames])
					fprintf (stderr, "mismatch r=%g iter=%ld out=%ld in=%ld rv=%.9g cv=%.9g\n",
						ratio, iter, ref_out_frames, causal_in,
						ref_out [ref_out_frames], causal_out [causal_out_frames]) ;
				/* Every already produced prefix sample is identical. */
				CHECK (ref_out [ref_out_frames] == causal_out [causal_out_frames]) ;
				ref_out_frames ++ ;
				causal_out_frames ++ ;
				} ;

			ref_in += rd.input_frames_used ;
			causal_in += cd.input_frames_used ;
			CHECK (ref_in == causal_in) ;

			ref_progress = rd.input_frames_used + rd.output_frames_gen ;
			causal_progress = cd.input_frames_used + cd.output_frames_gen ;
			if (ref_in >= frames && causal_in >= frames &&
					ref_progress == 0 && causal_progress == 0)
				break ;
			} ;

		CHECK (iter < 200000) ;
		CHECK (ref_out_frames > 20) ;
		CHECK (ref_out_frames == causal_out_frames) ;

		free (reference) ;
		free (mutable_input) ;
		src_delete (ref_state) ;
		src_delete (causal_state) ;
		} ;

	printf ("MINIMUM_PHASE_CASE causal PASS\n") ;
	fflush (stdout) ;
} /* causal_case */

/*------------------------------------------------------------------------------
** magnitude
*/

static void
check_sine_passband (double ratio, double input_frequency)
{
	const long frames = 32768 ;
	const double amplitude = 0.7 ;
	float *input = malloc ((size_t) frames * sizeof (float)) ;
	long output_frames ;
	float *output ;
	long start, analysis_frames ;
	double output_frequency, measured ;
	long i ;
	double error_db ;

	CHECK (input != NULL) ;
	for (i = 0 ; i < frames ; ++i)
		input [i] = (float) (amplitude *
				sin (2.0 * M_PI * input_frequency * i)) ;

	output = src_simple_convert (input, frames, 1, ratio, &output_frames) ;
	start = output_frames / 4 ;
	analysis_frames = output_frames / 2 ;
	CHECK (analysis_frames > 4096) ;

	output_frequency = input_frequency / ratio ;
	CHECK (output_frequency < 0.5) ;
	measured = project_amplitude (output, 1, 0, start,
					analysis_frames, output_frequency) ;
	error_db = amplitude_db (measured, amplitude) ;

	if (fabs (error_db) > 0.5)
	{	fprintf (stderr, "passband ratio %.17g f %.3f error %.3f dB\n",
				ratio, input_frequency, error_db) ;
		CHECK (0) ;
		} ;

	free (input) ;
	free (output) ;
} /* check_sine_passband */

static void
magnitude_case (void)
{
	const long frames = 32768 ;
	const double amplitude = 0.7 ;
	static const double pass_ratios [] =
	{	0.5, 44100.0 / 48000.0, 1.0, 2.0 } ;
	float *input = malloc ((size_t) frames * sizeof (float)) ;
	long output_frames, start, analysis_frames, i, r ;
	float *output ;
	double measured, attenuation ;

	CHECK (input != NULL) ;

	for (r = 0 ; r < (long) (sizeof (pass_ratios) / sizeof (pass_ratios [0])) ; ++r)
		check_sine_passband (pass_ratios [r], 0.02) ;

	/* Ratio 1/2: 0.10 input must stay in band. */
	for (i = 0 ; i < frames ; ++i)
		input [i] = (float) (amplitude * sin (2.0 * M_PI * 0.10 * i)) ;
	output = src_simple_convert (input, frames, 1, 0.5, &output_frames) ;
	start = output_frames / 4 ;
	analysis_frames = output_frames / 2 ;
	measured = project_amplitude (output, 1, 0, start, analysis_frames, 0.20) ;
	CHECK (fabs (amplitude_db (measured, amplitude)) <= 0.5) ;
	free (output) ;

	/* Ratio 1/2: 0.40 input aliases to 0.20 cycles/output frame. */
	for (i = 0 ; i < frames ; ++i)
		input [i] = (float) (amplitude * sin (2.0 * M_PI * 0.40 * i)) ;
	output = src_simple_convert (input, frames, 1, 0.5, &output_frames) ;
	measured = project_amplitude (output, 1, 0, start, analysis_frames, 0.20) ;
	attenuation = amplitude_db (measured, amplitude) ;
	if (attenuation > -35.0)
	{	fprintf (stderr, "downsampling alias only %.2f dB\n", attenuation) ;
		CHECK (0) ;
		} ;
	free (output) ;

	/* Ratio 2: 0.10 input gives 0.05 main and a 0.45 image. */
	for (i = 0 ; i < frames ; ++i)
		input [i] = (float) (amplitude * sin (2.0 * M_PI * 0.10 * i)) ;
	output = src_simple_convert (input, frames, 1, 2.0, &output_frames) ;
	start = output_frames / 4 ;
	analysis_frames = output_frames / 2 ;
	CHECK (analysis_frames > 8192) ;
	{
		double main_amp = project_amplitude (output, 1, 0, start,
					analysis_frames, 0.05) ;
		double image_amp = project_amplitude (output, 1, 0, start,
					analysis_frames, 0.45) ;
		double image_db = amplitude_db (image_amp, main_amp) ;
		CHECK (main_amp > 0.5) ;
		if (image_db > -35.0)
		{	fprintf (stderr, "upsampling image only %.2f dB below main\n",
						image_db) ;
			CHECK (0) ;
			} ;
	} ;
	free (output) ;
	free (input) ;

	printf ("MINIMUM_PHASE_CASE magnitude PASS\n") ;
	fflush (stdout) ;
} /* magnitude_case */

/*------------------------------------------------------------------------------
** phase and early energy
*/

static float *
collect_impulse (double ratio, long zero_frames, long *collected)
{
	long input_frames = zero_frames ;
	float *input = calloc ((size_t) input_frames, sizeof (float)) ;
	long output_capacity = input_frames * 2 + 64 ;
	float *output = calloc ((size_t) output_capacity, sizeof (float)) ;
	SRC_STATE *state ;
	SRC_DATA data ;
	int error ;

	CHECK (input && output) ;
	input [0] = 1.0f ; /* Explicit impulse; all following inputs are zeros. */

	state = src_new (CONVERTER, 1, &error) ;
	CHECK (state != NULL) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.input_frames = input_frames ;
	data.data_out = output ;
	data.output_frames = output_capacity ;
	data.src_ratio = ratio ;
	data.end_of_input = 0 ; /* EOI does not append a tail; zeros do. */

	error = src_process (state, &data) ;
	CHECK (error == 0) ;
	CHECK (data.input_frames_used == input_frames) ;
	CHECK (data.output_frames_gen > 16) ;

	*collected = data.output_frames_gen ;
	src_delete (state) ;
	free (input) ;
	return output ;
} /* collect_impulse */

static void
phase_case (void)
{
#define PHASE_DFT 1024
	long collected ;
	float *impulse = collect_impulse (1.0, 128, &collected) ;
	double measured_re [PHASE_DFT], log_mag [PHASE_DFT] ;
	double cepstrum [PHASE_DFT], shaped [PHASE_DFT] ;
	double raw_phase [PHASE_DFT], unwrapped [PHASE_DFT] ;
	int m, n, bin ;
	double worst_error = 0.0 ;
	int worst_bin = 0 ;

	CHECK (collected >= 32) ;

	/*
	** Sine/cosine projections of the measured impulse give
	** H(f) = sum h[n] cos(...) - i sum h[n] sin(...).  The measured
	** phase is atan2(-sine_projection, cosine_projection).  Adjacent
	** principal phase values are unwrapped by adding/subtracting 2*pi.
	*/
	for (m = 0 ; m < PHASE_DFT ; ++m)
	{	double c = 0.0, s = 0.0 ;
		for (n = 0 ; n < collected && n < 128 ; ++n)
		{	double angle = 2.0 * M_PI * m * n / PHASE_DFT ;
			c += impulse [n] * cos (angle) ;
			s += impulse [n] * sin (angle) ;
			} ;
		measured_re [m] = c ; /* Keep magnitude in a separate array below. */
		log_mag [m] = log (fmax (hypot (c, s), 1.0e-12)) ;
		raw_phase [m] = atan2 (-s, c) ;
		} ;

	/* Force exact conjugate symmetry for the real-cepstrum derivation. */
	for (m = 1 ; m < PHASE_DFT / 2 ; ++m)
		log_mag [PHASE_DFT - m] = log_mag [m] ;
	log_mag [0] = log (fmax (fabs (measured_re [0]), 1.0e-12)) ;

	/* Inverse DFT of log amplitude: real cepstrum. */
	for (n = 0 ; n < PHASE_DFT ; ++n)
	{	double sum = 0.0 ;
		for (m = 0 ; m < PHASE_DFT ; ++m)
			sum += log_mag [m] * cos (2.0 * M_PI * m * n / PHASE_DFT) ;
		cepstrum [n] = sum / PHASE_DFT ;
		} ;

	/* Causal minimum-phase cepstrum. */
	memset (shaped, 0, sizeof (shaped)) ;
	shaped [0] = cepstrum [0] ;
	for (n = 1 ; n < PHASE_DFT / 2 ; ++n)
		shaped [n] = 2.0 * cepstrum [n] ;
	shaped [PHASE_DFT / 2] = cepstrum [PHASE_DFT / 2] ;

	/* Unwrap measured phase without removing any fitted linear delay. */
	unwrapped [0] = raw_phase [0] ;
	for (m = 1 ; m <= PHASE_DFT / 2 ; ++m)
	{	double value = raw_phase [m] ;
		double difference = value - unwrapped [m - 1] ;
		while (difference > M_PI)
		{	value -= 2.0 * M_PI ;
			difference = value - unwrapped [m - 1] ;
			} ;
		while (difference < -M_PI)
		{	value += 2.0 * M_PI ;
			difference = value - unwrapped [m - 1] ;
			} ;
		unwrapped [m] = value ;
		} ;

	/* Forward DFT of causal cepstrum gives expected unwrapped phase. */
	for (bin = 1 ; bin < PHASE_DFT / 2 ; ++bin)
	{	double expected_phase = 0.0 ;
		double magnitude_db ;
		double error ;

		for (n = 0 ; n < PHASE_DFT / 2 + 1 ; ++n)
			expected_phase -= shaped [n] *
				sin (2.0 * M_PI * bin * n / PHASE_DFT) ;

		magnitude_db = 20.0 * log10 (fmax (exp (log_mag [bin]), 1.0e-12)) ;
		error = fabs (unwrapped [bin] - expected_phase) ;
		if (magnitude_db > -35.0 && error > worst_error)
		{	worst_error = error ;
			worst_bin = bin ;
			} ;
		} ;

	/* A few hundredths of a radian allow float storage/projection noise. */
	/* A few hundredths of a radian allow float storage/projection noise. */
	if (worst_error > 0.08)
	{	fprintf (stderr, "minimum phase mismatch %.4f rad at bin %d\n",
				worst_error, worst_bin) ;
		CHECK (0) ;
		} ;

	free (impulse) ;

	/* Early-energy requirement at ratio 1/2. */
	{
		long half_collected ;
		float *half_impulse = collect_impulse (0.5, 128, &half_collected) ;
		double total_energy = 0.0, early_energy = 0.0 ;
		long first = -1, k ;

		for (k = 0 ; k < half_collected ; ++k)
			if (fabs (half_impulse [k]) > 1.0e-10f)
			{	first = k ;
				break ;
				} ;
		CHECK (first == 0) ;

		for (k = first ; k < half_collected ; ++k)
		{	double energy = half_impulse [k] * half_impulse [k] ;
			double input_span = (double) (k - first) / 0.5 ;
			total_energy += energy ;
			/* Source coordinates less than 12 input frames ahead. */
			if (input_span < 12.0)
				early_energy += energy ;
			} ;

		CHECK (total_energy > 0.0) ;
		if (early_energy / total_energy <= 0.80)
		{	fprintf (stderr, "early energy only %.3f%%\n",
					100.0 * early_energy / total_energy) ;
			CHECK (0) ;
			} ;
		free (half_impulse) ;
	}

	printf ("MINIMUM_PHASE_CASE phase PASS\n") ;
	fflush (stdout) ;
} /* phase_case */

/*------------------------------------------------------------------------------
** streaming
*/

static void
streaming_signal (const char *name, const float *input, long frames,
			int channels, double ratio)
{
	long simple_frames, whole_frames, one_frames, mixed_frames, callback_frames ;
	float *simple = src_simple_convert (input, frames, channels, ratio, &simple_frames) ;
	float *whole = process_convert (input, frames, channels, ratio,
					DELIVERY_WHOLE, &whole_frames) ;
	float *one = process_convert (input, frames, channels, ratio,
					DELIVERY_ONE, &one_frames) ;
	float *mixed = process_convert (input, frames, channels, ratio,
					DELIVERY_MIXED, &mixed_frames) ;
	float *callback = callback_convert (input, frames, channels, ratio,
					simple_frames, &callback_frames) ;

	expect_same_samples (simple, simple_frames, whole, whole_frames, channels) ;
	expect_same_samples (simple, simple_frames, one, one_frames, channels) ;
	expect_same_samples (simple, simple_frames, mixed, mixed_frames, channels) ;
	expect_same_samples (simple, simple_frames, callback, callback_frames, channels) ;

	free (simple) ;
	free (whole) ;
	free (one) ;
	free (mixed) ;
	free (callback) ;
	(void) name ;
} /* streaming_signal */

static void
streaming_case (void)
{
	static const double ratios [] = { 0.5, 44100.0 / 48000.0, 1.0, 2.0 } ;
	static const long pulse_positions [6] = { 0, 7, 13, 29, 41, 58 } ;
	const long frames = 192 ;
	float *mono = make_random_frames (frames, 1, 771) ;
	float *stereo = malloc ((size_t) frames * 2 * sizeof (float)) ;
	float *six = calloc ((size_t) 96 * 6, sizeof (float)) ;
	long i, ch, r ;

	CHECK (stereo != NULL && six != NULL) ;
	for (i = 0 ; i < frames ; ++i)
	{	float s = (float) sin (2.0 * M_PI * 0.037 * i) * 0.6f ;
		stereo [2 * i] = s ;
		stereo [2 * i + 1] = -s ; /* left/right inverse */
		} ;

	for (ch = 0 ; ch < 6 ; ++ch)
	{	long position = pulse_positions [ch] ;
		six [position * 6 + ch] = 1.0f ;
		} ;

	for (r = 0 ; r < (long) (sizeof (ratios) / sizeof (ratios [0])) ; ++r)
	{	streaming_signal ("mono", mono, frames, 1, ratios [r]) ;
		streaming_signal ("stereo inverse", stereo, frames, 2, ratios [r]) ;
		streaming_signal ("six pulses", six, 96, 6, ratios [r]) ;
		} ;

	free (mono) ;
	free (stereo) ;
	free (six) ;

	printf ("MINIMUM_PHASE_CASE streaming PASS\n") ;
	fflush (stdout) ;
} /* streaming_case */

/*------------------------------------------------------------------------------
** lifecycle
*/

static void
feed_all (SRC_STATE *state, const float *input, long frames, int channels,
		double ratio)
{
	long offset = 0 ;
	long output_seen = 0 ;
	float junk_output [127] ;
	long iter ;

	for (iter = 0 ; iter < 100000 ; ++iter)
	{
		SRC_DATA data ;
		long input_frames = frames - offset ;

		if (input_frames > 13)
			input_frames = 13 ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = input + offset * channels ;
		data.input_frames = input_frames ;
		data.data_out = junk_output ;
		data.output_frames = 37 ;
		data.src_ratio = ratio ;

		CHECK (src_process (state, &data) == 0) ;
		offset += data.input_frames_used ;
		output_seen += data.output_frames_gen ;
		if (offset >= frames && data.input_frames_used == 0 &&
				data.output_frames_gen == 0)
			break ;
		} ;

	CHECK (iter < 100000) ;
	CHECK (offset == frames) ;
	(void) output_seen ;
} /* feed_all */

static float *
collect_block (SRC_STATE *state, const float *input, long frames, int channels,
		double ratio, long *gen)
{
	long capacity = expected_output_frames (frames, ratio) + 128 ;
	float *output = calloc ((size_t) capacity * channels, sizeof (float)) ;
	long input_offset = 0, output_offset = 0, iter ;
	CHECK (output != NULL) ;

	for (iter = 0 ; iter < 100000 ; ++iter)
	{
		SRC_DATA data ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input + input_offset * channels ;
		data.input_frames = frames - input_offset ;
		data.data_out = output + output_offset * channels ;
		data.output_frames = capacity - output_offset ;
		data.src_ratio = ratio ;

		CHECK (src_process (state, &data) == 0) ;
		input_offset += data.input_frames_used ;
		output_offset += data.output_frames_gen ;

		if (input_offset >= frames && data.input_frames_used == 0 &&
				data.output_frames_gen == 0)
			break ;
		} ;

	CHECK (input_offset == frames) ;
	*gen = output_offset ;
	return output ;
} /* collect_block */

static void
lifecycle_case (void)
{
	SRC_STATE *state, *clone_a, *clone_b, *fresh_a, *fresh_b ;
	int error ;
	SRC_DATA data ;
	float input [256], output [256] ;
	float branch_a [48], branch_b [48] ;
	float *clone_out_a, *clone_out_b, *fresh_out_a, *fresh_out_b ;
	long clone_gen_a, clone_gen_b, fresh_gen_a, fresh_gen_b, i ;
	const char *name, *description ;

	/* Old numbering is unchanged and the new id is appended. */
	CHECK (SRC_SINC_BEST_QUALITY == 0) ;
	CHECK (SRC_SINC_MEDIUM_QUALITY == 1) ;
	CHECK (SRC_SINC_FASTEST == 2) ;
	CHECK (SRC_ZERO_ORDER_HOLD == 3) ;
	CHECK (SRC_LINEAR == 4) ;
	CHECK (CONVERTER == 5) ;

	name = src_get_name (CONVERTER) ;
	description = src_get_description (CONVERTER) ;
	CHECK (name != NULL && strstr (name, "Minimum Phase") != NULL) ;
	CHECK (description != NULL && strstr (description, "0.5") != NULL) ;
	CHECK (src_get_name (9999) == NULL) ;
	CHECK (src_get_description (9999) == NULL) ;

	state = src_new (CONVERTER, 2, &error) ;
	CHECK (state != NULL && error == 0) ;

	/* src_set_ratio validates the supported range before first work. */
	CHECK (src_set_ratio (state, 0.25) != 0) ;
	CHECK (src_set_ratio (state, 3.0) != 0) ;
	CHECK (src_set_ratio (state, 0.5) == 0) ;
	CHECK (src_reset (state) == 0) ;

	for (i = 0 ; i < 128 ; ++i)
		input [i] = (float) sin (2.0 * M_PI * 0.03 * i) * 0.4f ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.input_frames = 32 ;
	data.data_out = output ;
	data.output_frames = 32 ;
	data.src_ratio = 1.0 ;
	CHECK (src_process (state, &data) == 0) ;
	CHECK (data.output_frames_gen > 0) ;

	/* Same ratio through src_set_ratio and src_process may continue. */
	CHECK (src_set_ratio (state, 1.0) == 0) ;
	memset (&data, 0, sizeof (data)) ;
	data.data_in = input + 32 * 2 ;
	data.input_frames = 32 ;
	data.data_out = output ;
	data.output_frames = 32 ;
	data.src_ratio = 1.0 ;
	CHECK (src_process (state, &data) == 0) ;

	/* A changed fixed ratio is rejected before use/generation. */
	CHECK (src_set_ratio (state, 0.5) == SRC_ERR_NO_VARIABLE_RATIO) ;
	for (i = 0 ; i < 4 ; ++i)
	{	data.input_frames_used = 999 ;
		data.output_frames_gen = 999 ;
		} ;
	data.data_in = input ;
	data.input_frames = 1 ;
	data.data_out = output ;
	data.output_frames = 1 ;
	data.src_ratio = 0.5 ;
	error = src_process (state, &data) ;
	CHECK (error == SRC_ERR_NO_VARIABLE_RATIO) ;
	CHECK (data.input_frames_used == 0) ;
	CHECK (data.output_frames_gen == 0) ;

	/* The locked ratio still works after that error. */
	data.src_ratio = 1.0 ;
	CHECK (src_process (state, &data) == 0) ;
	CHECK (data.output_frames_gen == 1) ;
	src_delete (state) ;

	/* An illegal first ratio must not poison a later legal conversion. */
	state = src_new (CONVERTER, 1, &error) ;
	CHECK (state != NULL) ;
	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.input_frames = 8 ;
	data.data_out = output ;
	data.output_frames = 8 ;
	data.src_ratio = 4.0 ;
	CHECK (src_process (state, &data) == SRC_ERR_MINPHASE_BAD_RATIO) ;
	CHECK (data.input_frames_used == 0 && data.output_frames_gen == 0) ;
	data.src_ratio = 0.1 ;
	CHECK (src_process (state, &data) == SRC_ERR_MINPHASE_BAD_RATIO) ;
	data.src_ratio = 1.0 ;
	CHECK (src_process (state, &data) == 0) ;
	CHECK (data.output_frames_gen > 0) ;

	/* Reset makes the state equivalent to a new instance. */
	CHECK (src_reset (state) == 0) ;
	{
		long reset_gen, fresh_gen ;
		float *reset_out = calloc (256, sizeof (float)) ;
		float *fresh_out ;
		SRC_STATE *fresh = src_new (CONVERTER, 1, &error) ;
		long cap = 128 ;

		CHECK (reset_out && fresh) ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input ;
		data.input_frames = 40 ;
		data.data_out = reset_out ;
		data.output_frames = cap ;
		data.src_ratio = 2.0 ;
		CHECK (src_process (state, &data) == 0) ;
		reset_gen = data.output_frames_gen ;

		fresh_out = calloc (256, sizeof (float)) ;
		CHECK (fresh_out != NULL) ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input ;
		data.input_frames = 40 ;
		data.data_out = fresh_out ;
		data.output_frames = cap ;
		data.src_ratio = 2.0 ;
		CHECK (src_process (fresh, &data) == 0) ;
		fresh_gen = data.output_frames_gen ;
		CHECK (reset_gen == fresh_gen) ;
		CHECK (memcmp (reset_out, fresh_out,
				(size_t) reset_gen * sizeof (float)) == 0) ;
		free (reset_out) ;
		free (fresh_out) ;
		src_delete (fresh) ;
		} ;
	src_delete (state) ;

	/* Clone then feed two independent continuations. */
	state = src_new (CONVERTER, 1, &error) ;
	CHECK (state != NULL) ;
	feed_all (state, input, 40, 1, 1.0) ;
	clone_a = src_clone (state, &error) ;
	clone_b = src_clone (state, &error) ;
	CHECK (clone_a && clone_b && error == 0) ;

	for (i = 0 ; i < 48 ; ++i)
	{	branch_a [i] = (i == 10) ? 1.0f : 0.0f ;
		branch_b [i] = (i == 20) ? -0.8f : 0.0f ;
		} ;

	clone_out_a = collect_block (clone_a, branch_a, 48, 1, 1.0, &clone_gen_a) ;
	clone_out_b = collect_block (clone_b, branch_b, 48, 1, 1.0, &clone_gen_b) ;
	CHECK (clone_gen_a == clone_gen_b) ;

	fresh_a = src_new (CONVERTER, 1, &error) ;
	fresh_b = src_new (CONVERTER, 1, &error) ;
	CHECK (fresh_a && fresh_b) ;
	feed_all (fresh_a, input, 40, 1, 1.0) ;
	feed_all (fresh_b, input, 40, 1, 1.0) ;
	fresh_out_a = collect_block (fresh_a, branch_a, 48, 1, 1.0, &fresh_gen_a) ;
	fresh_out_b = collect_block (fresh_b, branch_b, 48, 1, 1.0, &fresh_gen_b) ;

	expect_same_samples (clone_out_a, clone_gen_a, fresh_out_a, fresh_gen_a, 1) ;
	expect_same_samples (clone_out_b, clone_gen_b, fresh_out_b, fresh_gen_b, 1) ;

	/* The branches really carry different signals. */
	CHECK (memcmp (clone_out_a, clone_out_b,
			(size_t) clone_gen_a * sizeof (float)) != 0) ;

	free (clone_out_a) ;
	free (clone_out_b) ;
	free (fresh_out_a) ;
	free (fresh_out_b) ;
	src_delete (clone_a) ;
	src_delete (clone_b) ;
	src_delete (fresh_a) ;
	src_delete (fresh_b) ;
	src_delete (state) ;

	printf ("MINIMUM_PHASE_CASE lifecycle PASS\n") ;
	fflush (stdout) ;
} /* lifecycle_case */

int
main (void)
{
	causal_case () ;
	magnitude_case () ;
	phase_case () ;
	streaming_case () ;
	lifecycle_case () ;
	return 0 ;
} /* main */
