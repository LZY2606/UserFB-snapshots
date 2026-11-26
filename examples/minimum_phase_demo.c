/*
** minimum_phase_demo.c - small dependency-free example for
** SRC_SINC_MINIMUM_PHASE.
**
** Build from an existing CMake build tree, or directly:
**   cc -std=c99 -Iinclude examples/minimum_phase_demo.c \
**      build/src/libsamplerate.a -lm -o minimum_phase_demo
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <samplerate.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define INPUT_FRAMES 512

int
main (int argc, char **argv)
{
	double ratio = 1.0 ;
	float input [INPUT_FRAMES] ;
	float output [INPUT_FRAMES * 2 + 64] ;
	SRC_DATA data ;
	int error ;
	long i ;

	if (argc > 1)
		ratio = atof (argv [1]) ;

	if (ratio < 0.5 || ratio > 2.0)
	{	fprintf (stderr, "ratio must be in [0.5, 2.0] (output/input)\n") ;
		return 2 ;
		} ;

	/* Deterministic 0.02 cycles/input-frame demonstration tone. */
	for (i = 0 ; i < INPUT_FRAMES ; ++i)
		input [i] = 0.5f * (float) sin (2.0 * M_PI * 0.02 * i) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.input_frames = INPUT_FRAMES ;
	data.data_out = output ;
	data.output_frames = (long) (sizeof (output) / sizeof (output [0])) ;
	data.src_ratio = ratio ;

	/*
	** src_simple() sets end_of_input, but this converter never invents an
	** FIR tail.  To inspect that tail, append explicit zero-valued input
	** frames and use the streaming src_new()/src_process() API.
	*/
	error = src_simple (&data, SRC_SINC_MINIMUM_PHASE, 1) ;
	if (error)
	{	fprintf (stderr, "%s\n", src_strerror (error)) ;
		return 1 ;
		} ;

	printf ("converter : %s\n", src_get_name (SRC_SINC_MINIMUM_PHASE)) ;
	printf ("ratio     : %.10g\n", ratio) ;
	printf ("input     : %ld frames\n", data.input_frames) ;
	printf ("used      : %ld frames\n", data.input_frames_used) ;
	printf ("output    : %ld frames\n", data.output_frames_gen) ;
	printf ("first     : %.7f %.7f %.7f\n",
		output [0], output [1], output [2]) ;

	return 0 ;
} /* main */
