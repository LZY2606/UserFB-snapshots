/*
** Minimum-phase coefficient generator for libsamplerate.
**
** This is a build-time/design-time tool, not part of libsamplerate itself.
** It deliberately uses a small built-in radix-2 FFT and therefore needs
** neither Python nor an external FFT library.
**
** Typical regeneration:
**   cc -std=c99 -O2 src/minphase_gen.c -lm -o /tmp/minphase_gen
**   /tmp/minphase_gen src/minphase_coeffs.h
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif

#define MP_TAPS       20
#define MP_FFT_SIZE   4096
#define MP_PHASES     256
#define MP_CUTOFF     0.235
#define MP_KAISER_BETA 8.6

static double
bessel_i0 (double x)
{
	double sum = 1.0 ;
	double term = 1.0 ;
	int k ;

	for (k = 1 ; k < 100 ; ++k)
	{	term *= (x * x / 4.0) / ((double) k * k) ;
		sum += term ;
		if (term < 1.0e-18 * sum)
			break ;
		} ;

	return sum ;
} /* bessel_i0 */

/* In-place radix-2 FFT. inverse == 0 gives exp(-2*pi*i*...). */
static void
fft (double *re, double *im, int n, int inverse)
{
	int i, j, bit ;
	int length ;

	for (i = 1, j = 0 ; i < n ; ++i)
	{	bit = n >> 1 ;
		while (j & bit)
		{	j ^= bit ;
			bit >>= 1 ;
			} ;
		j ^= bit ;
		if (i < j)
		{	double tr = re [i], ti = im [i] ;
			re [i] = re [j] ;
			im [i] = im [j] ;
			re [j] = tr ;
			im [j] = ti ;
			} ;
		} ;

	for (length = 2 ; length <= n ; length <<= 1)
	{	int half = length / 2 ;
		double angle = (inverse ? 2.0 : -2.0) * M_PI / length ;
		double wr = cos (angle), wi = sin (angle) ;

		for (i = 0 ; i < n ; i += length)
		{	double cr = 1.0, ci = 0.0 ;
			for (j = i ; j < i + half ; ++j)
			{	double vr = re [j + half] * cr - im [j + half] * ci ;
				double vi = re [j + half] * ci + im [j + half] * cr ;
				double ur = re [j], ui = im [j] ;

				re [j] = ur + vr ;
				im [j] = ui + vi ;
				re [j + half] = ur - vr ;
				im [j + half] = ui - vi ;

				{	double ncr = cr * wr - ci * wi ;
					ci = cr * wi + ci * wr ;
					cr = ncr ;
					} ;
				} ;
			} ;
		} ;

	if (inverse)
		for (i = 0 ; i < n ; ++i)
		{	re [i] /= n ;
			im [i] /= n ;
			} ;
} /* fft */

static void
design_integer_kernel (double h [MP_TAPS])
{
	static double re [MP_FFT_SIZE], im [MP_FFT_SIZE] ;
	double prototype [MP_TAPS + 1] ;
	int prototype_len = MP_TAPS + 1 ; /* Odd length 33. */
	int half = prototype_len / 2 ;
	double i0_beta = bessel_i0 (MP_KAISER_BETA) ;
	double dc = 0.0, scale ;
	int n, k ;

	memset (re, 0, sizeof (re)) ;
	memset (im, 0, sizeof (im)) ;

	/* Windowed sinc zero-phase prototype. */
	for (n = 0 ; n < prototype_len ; ++n)
	{	int x = n - half ;
		double sinc_value ;
		double r, arg, window ;

		if (x == 0)
			sinc_value = 2.0 * MP_CUTOFF ;
		else
			sinc_value = sin (2.0 * M_PI * MP_CUTOFF * x) / (M_PI * x) ;

		r = 2.0 * x / (prototype_len - 1) ;
		arg = sqrt (fmax (0.0, 1.0 - r * r)) ;
		window = bessel_i0 (MP_KAISER_BETA * arg) / i0_beta ;
		prototype [n] = sinc_value * window ;
		dc += prototype [n] ;
		} ;

	for (n = 0 ; n < prototype_len ; ++n)
		re [(MP_FFT_SIZE - prototype_len) / 2 + n] = prototype [n] / dc ;

	/* |H| -> real cepstrum -> causal cepstrum -> minimum-phase H. */
	fft (re, im, MP_FFT_SIZE, 0) ;
	for (k = 0 ; k < MP_FFT_SIZE ; ++k)
	{	double magnitude = hypot (re [k], im [k]) ;
		re [k] = log (fmax (magnitude, 1.0e-12)) ;
		im [k] = 0.0 ;
		} ;

	fft (re, im, MP_FFT_SIZE, 1) ;

	/* Keep only causal/positive-quefrency cepstral coefficients. */
	for (k = 1 ; k < MP_FFT_SIZE / 2 ; ++k)
	{	re [k] *= 2.0 ;
		im [k] = 0.0 ;
		} ;
	for (k = MP_FFT_SIZE / 2 + 1 ; k < MP_FFT_SIZE ; ++k)
	{	re [k] = 0.0 ;
		im [k] = 0.0 ;
		} ;
	re [MP_FFT_SIZE / 2] = re [MP_FFT_SIZE / 2] ;
	im [MP_FFT_SIZE / 2] = 0.0 ;
	re [0] = re [0] ;
	im [0] = 0.0 ;

	fft (re, im, MP_FFT_SIZE, 0) ;
	for (k = 0 ; k < MP_FFT_SIZE ; ++k)
	{	double exp_re = exp (re [k]) ;
		double log_im = im [k] ;
		re [k] = exp_re * cos (log_im) ;
		im [k] = exp_re * sin (log_im) ;
		} ;

	fft (re, im, MP_FFT_SIZE, 1) ;

	dc = 0.0 ;
	for (n = 0 ; n < MP_TAPS ; ++n)
	{	h [n] = re [n] ;
		dc += h [n] ;
		} ;

	/* Unity DC gain. A positive scale does not change minimum phase. */
	scale = 1.0 / dc ;
	for (n = 0 ; n < MP_TAPS ; ++n)
		h [n] *= scale ;
} /* design_integer_kernel */

/*
** Sample the band-limited continuation of the discrete minimum-phase
** kernel at lag j + fraction.  The integer phase is exactly h because
** sinc(integer) is a Kronecker delta.
*/
static double
fractional_coefficient (const double h [MP_TAPS], int j, double fraction)
{
	double value = 0.0 ;
	int n ;

	for (n = 0 ; n < MP_TAPS ; ++n)
	{	double x = (double) j + fraction - (double) n ;
		double sinc ;

		if (fabs (x) < 1.0e-13)
			sinc = 1.0 ;
		else if (fabs (x - floor (x + 0.5)) < 1.0e-12)
			sinc = 0.0 ;
		else
			sinc = sin (M_PI * x) / (M_PI * x) ;

		value += h [n] * sinc ;
		} ;

	return value ;
} /* fractional_coefficient */

int
main (int argc, char **argv)
{
	FILE *out = stdout ;
	double h [MP_TAPS] ;
	int phase, tap ;

	if (argc > 2)
	{	fprintf (stderr, "usage: %s [output-header]\n", argv [0]) ;
		return 2 ;
		} ;

	if (argc == 2)
	{	out = fopen (argv [1], "w") ;
		if (out == NULL)
		{	perror (argv [1]) ;
			return 1 ;
			} ;
		} ;

	design_integer_kernel (h) ;

	fprintf (out, "/* SPDX-License-Identifier: BSD-2-Clause */\n") ;
	fprintf (out, "/*\n") ;
	fprintf (out, "** Pre-generated SRC_SINC_MINIMUM_PHASE coefficients.\n") ;
	fprintf (out, "** Do not edit by hand.  Regenerate with:\n") ;
	fprintf (out, "**   cc -std=c99 -O2 src/minphase_gen.c -lm -o /tmp/minphase_gen\n") ;
	fprintf (out, "**   /tmp/minphase_gen src/minphase_coeffs.h\n") ;
	fprintf (out, "**\n") ;
	fprintf (out, "** The generator uses a Kaiser-windowed sinc prototype and a real\n") ;
	fprintf (out, "** cepstral minimum-phase spectral factor.  It contains its own FFT.\n") ;
	fprintf (out, "*/\n\n") ;
	fprintf (out, "#ifndef MINPHASE_COEFFS_H_INCLUDED\n") ;
	fprintf (out, "#define MINPHASE_COEFFS_H_INCLUDED\n\n") ;
	fprintf (out, "#define MINPHASE_TAPS %d\n", MP_TAPS) ;
	fprintf (out, "#define MINPHASE_PHASES %d\n\n", MP_PHASES) ;

	fprintf (out, "static const double minphase_integer [MINPHASE_TAPS] =\n{\n") ;
	for (tap = 0 ; tap < MP_TAPS ; ++tap)
		fprintf (out, "\t%.17g%s\n", h [tap], tap + 1 == MP_TAPS ? "" : ",") ;
	fprintf (out, "} ;\n\n") ;

	fprintf (out, "/* Rows 0 through MINPHASE_PHASES inclusive; last row is fraction 1. */\n") ;
	fprintf (out, "static const double minphase_poly [MINPHASE_PHASES + 1] [MINPHASE_TAPS] =\n{\n") ;
	for (phase = 0 ; phase <= MP_PHASES ; ++phase)
	{	double fraction = (double) phase / MP_PHASES ;
		fprintf (out, "\t{") ;
		for (tap = 0 ; tap < MP_TAPS ; ++tap)
		{	double c = fractional_coefficient (h, tap, fraction) ;
			fprintf (out, " %.17g%s", c, tap + 1 == MP_TAPS ? "" : ",") ;
			} ;
		fprintf (out, " }%s\n", phase == MP_PHASES ? "" : ",") ;
		} ;
	fprintf (out, "} ;\n\n") ;
	fprintf (out, "#endif /* MINPHASE_COEFFS_H_INCLUDED */\n") ;

	if (argc == 2)
		fclose (out) ;

	return 0 ;
} /* main */
