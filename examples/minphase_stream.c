/*
** Copyright (c) 2002-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** minphase_stream : demonstrate the SRC_SINC_MINIMUM_PHASE converter in a
** low latency streaming (live foldback) scenario.
**
** An impulse is streamed through the converter, one input frame and one
** output frame of capacity per call. Latency is measured as the number of
** process calls between delivering the impulse and the call that produces
** the impulse response peak, comparing SRC_SINC_MINIMUM_PHASE with the
** symmetric (linear phase) SRC_SINC_FASTEST, whose kernel must wait for
** future input before its main lobe can appear.
**
** No audio I/O is required ; the "input signal" is synthesised in memory.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <samplerate.h>

#define IMPULSE_AT	64	/* call index at which the impulse is delivered */
#define TOTAL_CALLS	512	/* total process calls made */

static int
measure_peak_latency (int converter, double ratio, long *peak_call)
{
	SRC_STATE	*state ;
	SRC_DATA	data ;
	float		in [1], out [1] ;
	long		frame = 0 ;	/* next input frame to deliver */
	int			error ;
	double		best = 0.0 ;

	*peak_call = -1 ;

	state = src_new (converter, 1, &error) ;
	if (state == NULL)
	{	fprintf (stderr, "src_new failed : %s\n", src_strerror (error)) ;
		return 1 ;
		} ;

	for (long call = 0 ; call < TOTAL_CALLS && frame < 256 ; call++)
	{	in [0] = (frame == IMPULSE_AT) ? 1.0f : 0.0f ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = in ;
		data.input_frames = 1 ;
		data.data_out = out ;
		data.output_frames = 1 ;
		data.src_ratio = ratio ;

		if ((error = src_process (state, &data)) != 0)
		{	fprintf (stderr, "src_process failed : %s\n", src_strerror (error)) ;
			src_delete (state) ;
			return 1 ;
			} ;

		/* Re-present the frame until the converter actually consumes it. */
		frame += data.input_frames_used ;

		if (data.output_frames_gen > 0 && fabs (out [0]) > best)
		{	best = fabs (out [0]) ;
			*peak_call = call ;
			} ;
		} ;

	src_delete (state) ;
	return 0 ;
}

int
main (void)
{
	const double ratio = 48000.0 / 44100.0 ; /* within [0.5, 2.0] */
	long peak_mp = -1, peak_sinc = -1 ;

	if (measure_peak_latency (SRC_SINC_MINIMUM_PHASE, ratio, &peak_mp))
		return 1 ;
	if (measure_peak_latency (SRC_SINC_FASTEST, ratio, &peak_sinc))
		return 1 ;

	printf ("ratio %.5f, impulse delivered at call %d, streamed 1-in/1-out\n\n",
			ratio, IMPULSE_AT) ;
	printf ("  SRC_SINC_MINIMUM_PHASE : response peak at call %ld (%ld calls later)\n",
			peak_mp, peak_mp - IMPULSE_AT) ;
	printf ("  SRC_SINC_FASTEST       : response peak at call %ld (%ld calls later)\n",
			peak_sinc, peak_sinc - IMPULSE_AT) ;
	printf ("\nThe minimum phase converter's main impulse energy arrives\n"
			"almost immediately ; the symmetric sinc converter's peak only\n"
			"appears after its kernel delay.\n") ;

	return 0 ;
}
