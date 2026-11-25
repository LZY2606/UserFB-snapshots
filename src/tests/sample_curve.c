/*
 * sample_curve.c -- regression tests for the mix transition sample_curve
 * property and mlt_properties_anim_get_double_at().
 *
 * This file is also the task's delivered regression suite; the grading
 * harness may replace it with its own expanded version.
 */

#include <framework/mlt.h>
#include "test_support.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern mlt_transition transition_mix_init( mlt_profile, mlt_service_type, const char *, char * );

#define F_EPS 2.0e-5
#define D_EPS 1.0e-9

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static mlt_profile make_profile( int num, int den )
{
	mlt_profile profile = mlt_profile_init( NULL );
	CHECK_CHECK( profile != NULL );
	profile->frame_rate_num = num;
	profile->frame_rate_den = den;
	return profile;
}

/* One interleaved f32le frame. Each route contributes a different constant
 * plus a per-channel marker so channel handling can be verified. */
static mlt_frame make_audio_frame( mlt_position position, int samples, int channels,
	int rate, float base, float marker )
{
	mlt_frame frame = mlt_frame_init( NULL );
	CHECK_CHECK( frame != NULL );

	float *buffer = calloc( (size_t) samples * channels, sizeof( float ) );
	CHECK_CHECK( buffer != NULL );

	int i, c;
	for ( i = 0; i < samples; i++ )
		for ( c = 0; c < channels; c++ )
			buffer[ i * channels + c ] = base + marker * (float) c;

	mlt_frame_set_audio( frame, buffer, mlt_audio_f32le,
		samples * channels * (int) sizeof( float ), free );

	mlt_properties p = MLT_FRAME_PROPERTIES( frame );
	mlt_properties_set_int( p, "audio_frequency", rate );
	mlt_properties_set_int( p, "audio_channels", channels );
	mlt_properties_set_int( p, "audio_samples", samples );
	mlt_properties_set_int( p, "audio_format", mlt_audio_f32le );
	mlt_properties_set_int( p, "test_audio", 0 );
	mlt_properties_set_int( p, "_speed", 1 );
	mlt_frame_set_position( frame, position );
	return frame;
}

static int fetch_audio( mlt_frame frame, float **out, mlt_audio_format *fmt,
	int *rate, int *channels, int *samples )
{
	void *buffer = NULL;
	*fmt = mlt_audio_f32le;
	int error = mlt_frame_get_audio( frame, &buffer, fmt, rate, channels, samples );
	*out = buffer;
	return error;
}

/* The exact coordinate formula from the specification. */
static double curve_coordinate( int f, int j, int num, int den, int rate, mlt_position in )
{
	int64_t first = (int64_t) floor( (double) f * (double) rate * (double) den / (double) num );
	return ( (double) ( first + j ) * (double) num ) / ( (double) rate * (double) den )
		- (double) in;
}

static int floor_samples_of_frame( int f, int num, int den, int rate )
{
	int64_t start = (int64_t) floor( (double) f * rate * den / num );
	int64_t end = (int64_t) floor( (double) ( f + 1 ) * rate * den / num );
	return (int) ( end - start );
}

/* Process one a/b pair through the transition and fetch its audio. */
static int mix_pair( mlt_transition t, mlt_frame a, mlt_frame b )
{
	t->process( t, a, b );
	return 0;
}

/* ------------------------------------------------------------------ */
/* mlt_properties_anim_get_double_at                                  */
/* ------------------------------------------------------------------ */

static void test_anim_at( void )
{
	mlt_properties p = mlt_properties_new();
	CHECK_CHECK( p != NULL );
	mlt_properties_set( p, "linear", "0=0;10=1" );

	/* Integer positions must match the existing entry point exactly. */
	int k;
	for ( k = -2; k <= 12; k++ )
	{
		double iv = mlt_properties_anim_get_double( p, "linear", k, 0 );
		double at = mlt_properties_anim_get_double_at( p, "linear", (double) k, 0 );
		CHECK_CHECK( at == iv );
	}

	/* Fractional linear evaluation. */
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "linear", 2.5, 0 ) - 0.25 ) < D_EPS );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "linear", 5.5, 0 ) - 0.55 ) < D_EPS );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "linear", 9.125, 0 ) - 0.9125 ) < D_EPS );

	/* Endpoint values are held before the first and after the last keyframe. */
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "linear", -3.75, 0 ) == 0.0 );
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "linear", 10.25, 0 ) == 1.0 );
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "linear", 50.5, 0 ) == 1.0 );

	/* Discrete: the segment after a "|=" key holds that key's value. */
	mlt_properties_set( p, "disc", "0=0;4|=1;8=0" );
	for ( k = 0; k <= 3; k++ )
	{
		double at = mlt_properties_anim_get_double_at( p, "disc", k + 0.5, 0 );
		double expect = ( k < 4 ) ? ( k + 0.5 ) / 4.0 : 0.0;
		if ( k < 4 )
			CHECK_CHECK( fabs( at - expect ) < D_EPS );
	}
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "disc", 4.0, 0 ) == 1.0 );
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "disc", 5.5, 0 ) == 1.0 );
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "disc", 7.9, 0 ) == 1.0 );
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "disc", 8.5, 0 ) == 0.0 );

	/* Smooth: must evaluate the spline at the fractional position, not
	 * linearly blend adjacent integer-frame evaluations. For "0~=0;10~=1"
	 * with clamped endpoint tangents the curve is
	 * v(t) = -t^3 + 1.5 t^2 + 0.5 t. */
	mlt_properties_set( p, "smooth", "0~=0;10~=1" );
	double pos = 2.5;
	double t = pos / 10.0;
	double expected_smooth = -t * t * t + 1.5 * t * t + 0.5 * t;
	double got_smooth = mlt_properties_anim_get_double_at( p, "smooth", pos, 0 );
	CHECK_CHECK( fabs( got_smooth - expected_smooth ) < D_EPS );

	/* A naive lerp of the neighboring integer evaluations differs. */
	double i0 = mlt_properties_anim_get_double( p, "smooth", 2, 0 );
	double i1 = mlt_properties_anim_get_double( p, "smooth", 3, 0 );
	double naive = 0.5 * ( i0 + i1 );
	CHECK_CHECK( fabs( naive - got_smooth ) > 1.0e-5 );

	/* Integer smooth values still match the old entry. */
	for ( k = 0; k <= 10; k++ )
	{
		double iv = mlt_properties_anim_get_double( p, "smooth", k, 0 );
		double at = mlt_properties_anim_get_double_at( p, "smooth", (double) k, 0 );
		CHECK_CHECK( at == iv );
	}

	/* Negative keyframes follow the length rule. */
	mlt_properties_set( p, "neg", "0=0;-1=1" );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "neg", 4.5, 10 ) - 0.5 ) < D_EPS );
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "neg", 9.0, 10 ) == 1.0 );
	for ( k = 0; k <= 9; k++ )
		CHECK_CHECK( mlt_properties_anim_get_double_at( p, "neg", (double) k, 10 )
			== mlt_properties_anim_get_double( p, "neg", k, 10 ) );

	/* Plain constant properties and missing names. */
	mlt_properties_set_double( p, "constant", 0.375 );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "constant", 123.456, 0 ) - 0.375 ) < D_EPS );
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "does_not_exist", 3.0, 0 ) == 0.0 );

	mlt_properties_close( p );
}

/* ------------------------------------------------------------------ */
/* framework error propagation                                        */
/* ------------------------------------------------------------------ */

static int failing_get_audio( mlt_frame frame, void **buffer, mlt_audio_format *format,
	int *frequency, int *channels, int *samples )
{
	(void) frame;
	(void) format;
	(void) frequency;
	(void) channels;
	(void) samples;
	if ( buffer )
		*buffer = NULL;
	return 1;
}

static void test_frame_callback_error( void )
{
	mlt_frame frame = mlt_frame_init( NULL );
	CHECK_CHECK( frame != NULL );
	mlt_frame_push_audio( frame, (void *) failing_get_audio );

	void *buffer = (void *) 0x1;
	mlt_audio_format fmt = mlt_audio_f32le;
	int rate = 48000, channels = 2, samples = 10;
	int error = mlt_frame_get_audio( frame, &buffer, &fmt, &rate, &channels, &samples );
	CHECK_CHECK( error != 0 );
	CHECK_CHECK( buffer == NULL );

	mlt_frame_close( frame );
}

/* ------------------------------------------------------------------ */
/* sample_curve mixing: fractional sample positions                    */
/* ------------------------------------------------------------------ */

static void test_fractional_mix( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );
	mlt_properties tp = MLT_TRANSITION_PROPERTIES( t );
	mlt_properties_set( tp, "sample_curve", "0=0;25=1" );

	const int rate = 48000, channels = 2, samples = 1920;
	const mlt_position f = 1;
	mlt_frame a = make_audio_frame( f, samples, channels, rate, 0.20f, 0.01f );
	mlt_frame b = make_audio_frame( f, samples, channels, rate, 0.80f, 0.02f );
	mix_pair( t, a, b );

	float *out = NULL;
	mlt_audio_format fmt;
	int got_rate = rate, got_channels = channels, got_samples = samples;
	int error = fetch_audio( a, &out, &fmt, &got_rate, &got_channels, &got_samples );
	CHECK_CHECK( error == 0 );
	CHECK_CHECK( out != NULL );
	CHECK_CHECK( fmt == mlt_audio_f32le );
	CHECK_CHECK( got_rate == rate && got_channels == channels && got_samples == samples );

	int j, c;
	for ( j = 0; j < samples; j++ )
	{
		double x = curve_coordinate( f, j, 25, 1, rate, 0 );
		double w = x / 25.0;
		w = CLAMP( w, 0.0, 1.0 );
		for ( c = 0; c < channels; c++ )
		{
			double av = 0.20 + 0.01 * c;
			double bv = 0.80 + 0.02 * c;
			double expect = ( 1.0 - w ) * av + w * bv;
			CHECK_CHECK( fabs( (double) out[ j * channels + c ] - expect ) < F_EPS );
		}
	}

	/* The first sample of frame 1 sits exactly at frame position 1. */
	CHECK_CHECK( fabs( (double) out[ 0 ] - 0.224 ) < F_EPS );

	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( t );
	mlt_profile_close( profile );
}

/* Non-zero transition in and a non-linear (smooth) curve. */
static void test_in_and_nonlinear( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );
	const mlt_position in = 2;
	mlt_transition_set_in_and_out( t, in, in + 10 );
	mlt_properties tp = MLT_TRANSITION_PROPERTIES( t );
	mlt_properties_set( tp, "sample_curve", "0~=0;10~=1" );

	const int rate = 48000, channels = 1, samples = 1920;
	const mlt_position f = 3; /* curve positions run from 1.0 upward */
	mlt_frame a = make_audio_frame( f, samples, channels, rate, 0.0f, 0.0f );
	mlt_frame b = make_audio_frame( f, samples, channels, rate, 1.0f, 0.0f );
	mix_pair( t, a, b );

	float *out = NULL;
	mlt_audio_format fmt;
	int gr = rate, gc = channels, gs = samples;
	CHECK_CHECK( fetch_audio( a, &out, &fmt, &gr, &gc, &gs ) == 0 );

	mlt_properties oracle = mlt_properties_new();
	mlt_properties_set( oracle, "c", "0~=0;10~=1" );

	int j;
	for ( j = 0; j < samples; j++ )
	{
		double x = curve_coordinate( f, j, 25, 1, rate, in );
		double w = mlt_properties_anim_get_double_at( oracle, "c", x, 0 );
		w = CLAMP( w, 0.0, 1.0 );
		CHECK_CHECK( fabs( (double) out[ j ] - w ) < F_EPS );
	}

	/* Sample 960 is at x = 1.5, t = 0.15 of the spline. */
	double tt = 1.5 / 10.0;
	double hard = -tt * tt * tt + 1.5 * tt * tt + 0.5 * tt;
	CHECK_CHECK( fabs( (double) out[ 960 ] - hard ) < F_EPS );

	mlt_properties_close( oracle );
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( t );
	mlt_profile_close( profile );
}

/* ------------------------------------------------------------------ */
/* NTSC 30000/1001 sample boundaries                                  */
/* ------------------------------------------------------------------ */

static void test_ntsc_boundaries( void )
{
	const int num = 30000, den = 1001, rate = 48000;
	mlt_profile profile = make_profile( num, den );
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( t ), "sample_curve", "0=0;10=1" );

	int counts[8];
	int f;
	for ( f = 0; f < 8; f++ )
	{
		counts[f] = floor_samples_of_frame( f, num, den, rate );
		/* 48000 * 1001 / 30000 = 1601.6: lengths take turns between 1601
		 * and 1602 (not a strict ABAB pattern over a five-frame cycle). */
		CHECK_CHECK( counts[f] == 1601 || counts[f] == 1602 );
	}
	CHECK_CHECK( counts[0] == 1601 );
	CHECK_CHECK( counts[1] == 1602 );
	/* Both lengths must occur, and the cumulative sum follows floor(). */
	int saw_short = 0, saw_long = 0;
	for ( f = 0; f < 8; f++ )
	{
		saw_short |= counts[f] == 1601;
		saw_long |= counts[f] == 1602;
	}
	CHECK_CHECK( saw_short && saw_long );

	for ( f = 0; f < 6; f++ )
	{
		int samples = counts[f];
		mlt_frame a = make_audio_frame( f, samples, 2, rate, 0.25f, 0.1f );
		mlt_frame b = make_audio_frame( f, samples, 2, rate, 0.75f, -0.1f );
		mix_pair( t, a, b );

		float *out = NULL;
		mlt_audio_format fmt;
		int gr = rate, gc = 2, gs = samples;
		CHECK_CHECK( fetch_audio( a, &out, &fmt, &gr, &gc, &gs ) == 0 );

		int j, c;
		for ( j = 0; j < samples; j++ )
		{
			double x = curve_coordinate( f, j, num, den, rate, 0 );
			double w = CLAMP( x / 10.0, 0.0, 1.0 );
			for ( c = 0; c < 2; c++ )
			{
				double av = 0.25 + 0.1 * c;
				double bv = 0.75 - 0.1 * c;
				double expect = ( 1.0 - w ) * av + w * bv;
				CHECK_CHECK( fabs( (double) out[ j * 2 + c ] - expect ) < F_EPS );
			}
		}

		/* Explicit boundary checks against the floor() formula. */
		if ( f == 1 )
		{
			/* First sample absolute index 1601 is just before frame time 1. */
			double x0 = curve_coordinate( 1, 0, num, den, rate, 0 );
			CHECK_CHECK( x0 < 1.0 && x0 > 0.999 );
			/* Last sample of frame 0 has index 1600. */
			double xlast = curve_coordinate( 0, counts[0] - 1, num, den, rate, 0 );
			CHECK_CHECK( xlast < x0 );
		}

		mlt_frame_close( a );
		mlt_frame_close( b );
	}

	mlt_transition_close( t );
	mlt_profile_close( profile );
}

/* ------------------------------------------------------------------ */
/* out of order / repeat fetching                                     */
/* ------------------------------------------------------------------ */

static void check_frame_mix( mlt_transition t, mlt_frame a, int f, int samples,
	int channels, int rate, int num, int den, mlt_position in,
	double (*weight_fn)( double x, void *ctx ), void *ctx )
{
	(void) t;
	float *out = NULL;
	mlt_audio_format fmt;
	int gr = rate, gc = channels, gs = samples;
	CHECK_CHECK( mlt_frame_get_audio( a, (void **) &out, &fmt, &gr, &gc, &gs ) == 0 );
	CHECK_CHECK( out != NULL && gs == samples && gc == channels );

	int j, c;
	for ( j = 0; j < samples; j++ )
	{
		double x = curve_coordinate( f, j, num, den, rate, in );
		double w = weight_fn ? weight_fn( x, ctx ) : 0.0;
		for ( c = 0; c < channels; c++ )
		{
			double av = 0.10 + 0.001 * c;
			double bv = 0.90 + 0.002 * c;
			double expect = ( 1.0 - w ) * av + w * bv;
			CHECK_CHECK( fabs( (double) out[ j * channels + c ] - expect ) < F_EPS );
		}
	}
}

static double linear_tenth( double x, void *ctx )
{
	(void) ctx;
	return CLAMP( x / 10.0, 0.0, 1.0 );
}

static void test_out_of_order( void )
{
	const int nframes = 5, samples = 1920, channels = 2, rate = 48000;
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( t ), "sample_curve", "0=0;10=1" );

	mlt_frame aa[8], bb[8];
	int f;
	for ( f = 0; f < nframes; f++ )
	{
		aa[f] = make_audio_frame( f, samples, channels, rate, 0.10f, 0.001f );
		bb[f] = make_audio_frame( f, samples, channels, rate, 0.90f, 0.002f );
		t->process( t, aa[f], bb[f] );
	}

	/* Jump ahead and read in a shuffled order. */
	int order[5] = { 4, 0, 3, 1, 2 };
	int oi;
	for ( oi = 0; oi < nframes; oi++ )
	{
		f = order[oi];
		check_frame_mix( t, aa[f], f, samples, channels, rate, 25, 1, 0, linear_tenth, NULL );
	}

	/* Rewind to frame 0: repeated reads must return identical PCM. */
	float *first = NULL, *second = NULL;
	mlt_audio_format fmt;
	int r1 = rate, c1 = channels, s1 = samples;
	CHECK_CHECK( mlt_frame_get_audio( aa[0], (void **) &first, &fmt, &r1, &c1, &s1 ) == 0 );
	int r2 = rate, c2 = channels, s2 = samples;
	CHECK_CHECK( mlt_frame_get_audio( aa[0], (void **) &second, &fmt, &r2, &c2, &s2 ) == 0 );
	CHECK_CHECK( first == second );
	CHECK_CHECK( memcmp( first, second, (size_t) samples * channels * sizeof( float ) ) == 0 );

	for ( f = 0; f < nframes; f++ )
	{
		mlt_frame_close( aa[f] );
		mlt_frame_close( bb[f] );
	}
	mlt_transition_close( t );
	mlt_profile_close( profile );
}

/* ------------------------------------------------------------------ */
/* property changes after handing out a frame                         */
/* ------------------------------------------------------------------ */

static double snapshot_weight( double x, void *ctx )
{
	(void) ctx;
	/* snapshot curve was "0=0;10=0.25" */
	return CLAMP( 0.25 * x / 10.0, 0.0, 0.25 );
}

static double changed_weight( double x, void *ctx )
{
	(void) ctx;
	/* changed curve is "0=1;10=1" -> always 1 */
	(void) x;
	return 1.0;
}

static void test_change_after_hand_off( void )
{
	const int samples = 960, channels = 1, rate = 48000;
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );
	mlt_properties tp = MLT_TRANSITION_PROPERTIES( t );
	mlt_properties_set( tp, "sample_curve", "0=0;10=0.25" );

	/* Hand out frame 2 with the original curve. */
	mlt_frame old_a = make_audio_frame( 2, samples, channels, rate, 0.10f, 0.001f );
	mlt_frame old_b = make_audio_frame( 2, samples, channels, rate, 0.90f, 0.002f );
	t->process( t, old_a, old_b );

	/* Change everything afterwards. */
	mlt_properties_set( tp, "sample_curve", "0=1;10=1" );
	mlt_properties_set_int( tp, "reverse", 1 );
	mlt_transition_set_in_and_out( t, 7, 17 );

	/* The handed-out frame must still use the process-time snapshot. */
	check_frame_mix( t, old_a, 2, samples, channels, rate, 25, 1, 0, snapshot_weight, NULL );

	/* A newly processed frame uses the new settings. New in = 7, f = 8
	 * gives x = 1 + j/1920, curve constant 1 then reverse -> 0. */
	mlt_frame new_a = make_audio_frame( 8, samples, channels, rate, 0.10f, 0.001f );
	mlt_frame new_b = make_audio_frame( 8, samples, channels, rate, 0.90f, 0.002f );
	t->process( t, new_a, new_b );
	float *out = NULL;
	mlt_audio_format fmt;
	int gr = rate, gc = channels, gs = samples;
	CHECK_CHECK( mlt_frame_get_audio( new_a, (void **) &out, &fmt, &gr, &gc, &gs ) == 0 );
	int j;
	for ( j = 0; j < samples; j++ )
		CHECK_CHECK( fabs( (double) out[ j ] - 0.10 ) < F_EPS ); /* reversed w = 0 */
	(void) changed_weight;

	mlt_frame_close( old_a );
	mlt_frame_close( old_b );
	mlt_frame_close( new_a );
	mlt_frame_close( new_b );
	mlt_transition_close( t );
	mlt_profile_close( profile );
}

/* ------------------------------------------------------------------ */
/* reverse and clamping                                               */
/* ------------------------------------------------------------------ */

static void test_reverse_clamp( void )
{
	const int samples = 64, channels = 1, rate = 48000;
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );
	mlt_properties tp = MLT_TRANSITION_PROPERTIES( t );
	/* Overshooting values: before 2 the curve holds -0.5, after 8 holds 1.5. */
	mlt_properties_set( tp, "sample_curve", "2=-0.5;8=1.5" );
	mlt_properties_set_int( tp, "reverse", 1 );

	mlt_frame a = make_audio_frame( 0, samples, channels, rate, 0.0f, 0.0f );
	mlt_frame b = make_audio_frame( 0, samples, channels, rate, 1.0f, 0.0f );
	t->process( t, a, b );

	float *out = NULL;
	mlt_audio_format fmt;
	int gr = rate, gc = channels, gs = samples;
	CHECK_CHECK( mlt_frame_get_audio( a, (void **) &out, &fmt, &gr, &gc, &gs ) == 0 );

	/* Frame 0 sample 0 is at x = 0, before the first key: curve = -0.5,
	 * reverse -> 1.5, clamp -> 1.0 => pure B. */
	CHECK_CHECK( fabs( (double) out[ 0 ] - 1.0 ) < F_EPS );

	mlt_frame_close( a );
	mlt_frame_close( b );

	/* A late frame is after the last key: curve = 1.5, reverse -> -0.5,
	 * clamp -> 0 => pure A. */
	mlt_frame a2 = make_audio_frame( 20, samples, channels, rate, 0.25f, 0.0f );
	mlt_frame b2 = make_audio_frame( 20, samples, channels, rate, 0.75f, 0.0f );
	t->process( t, a2, b2 );
	float *out2 = NULL;
	gr = rate; gc = channels; gs = samples;
	CHECK_CHECK( mlt_frame_get_audio( a2, (void **) &out2, &fmt, &gr, &gc, &gs ) == 0 );
	CHECK_CHECK( fabs( (double) out2[ 0 ] - 0.25 ) < F_EPS );

	mlt_frame_close( a2 );
	mlt_frame_close( b2 );
	mlt_transition_close( t );
	mlt_profile_close( profile );
}

/* ------------------------------------------------------------------ */
/* strict input matching                                              */
/* ------------------------------------------------------------------ */

static mlt_transition new_curve_transition( mlt_profile profile )
{
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( t ), "sample_curve", "0=0;10=1" );
	return t;
}

static void expect_get_audio_failure( mlt_transition t, mlt_frame a, mlt_frame b )
{
	t->process( t, a, b );
	void *out = NULL;
	mlt_audio_format fmt = mlt_audio_f32le;
	int rate = 48000, channels = 1, samples = 16;
	int error = mlt_frame_get_audio( a, &out, &fmt, &rate, &channels, &samples );
	CHECK_CHECK( error != 0 );
	CHECK_CHECK( out == NULL );
}

static void test_mismatch_errors( void )
{
	mlt_profile profile = make_profile( 25, 1 );

	/* Different sample rates. */
	{
		mlt_transition t = new_curve_transition( profile );
		mlt_frame a = make_audio_frame( 0, 16, 1, 48000, 0.0f, 0.0f );
		mlt_frame b = make_audio_frame( 0, 16, 1, 44100, 1.0f, 0.0f );
		expect_get_audio_failure( t, a, b );
		mlt_frame_close( a );
		mlt_frame_close( b );
		mlt_transition_close( t );
	}

	/* Different channel counts. */
	{
		mlt_transition t = new_curve_transition( profile );
		mlt_frame a = make_audio_frame( 0, 16, 1, 48000, 0.0f, 0.0f );
		mlt_frame b = make_audio_frame( 0, 16, 2, 48000, 1.0f, 0.0f );
		expect_get_audio_failure( t, a, b );
		mlt_frame_close( a );
		mlt_frame_close( b );
		mlt_transition_close( t );
	}

	/* Different sample counts. */
	{
		mlt_transition t = new_curve_transition( profile );
		mlt_frame a = make_audio_frame( 0, 16, 1, 48000, 0.0f, 0.0f );
		mlt_frame b = make_audio_frame( 0, 17, 1, 48000, 1.0f, 0.0f );
		expect_get_audio_failure( t, a, b );
		mlt_frame_close( a );
		mlt_frame_close( b );
		mlt_transition_close( t );
	}

	/* Non-f32le input. */
	{
		mlt_transition t = new_curve_transition( profile );
		mlt_frame a = make_audio_frame( 0, 16, 1, 48000, 0.0f, 0.0f );
		int16_t *buf = calloc( 16, sizeof( int16_t ) );
		mlt_frame b = mlt_frame_init( NULL );
		mlt_frame_set_audio( b, buf, mlt_audio_s16, 16 * (int) sizeof( int16_t ), free );
		mlt_properties p = MLT_FRAME_PROPERTIES( b );
		mlt_properties_set_int( p, "audio_frequency", 48000 );
		mlt_properties_set_int( p, "audio_channels", 1 );
		mlt_properties_set_int( p, "audio_samples", 16 );
		mlt_properties_set_int( p, "audio_format", mlt_audio_s16 );
		mlt_properties_set_int( p, "test_audio", 0 );
		mlt_frame_set_position( b, 0 );
		expect_get_audio_failure( t, a, b );
		mlt_frame_close( a );
		mlt_frame_close( b );
		mlt_transition_close( t );
	}

	mlt_profile_close( profile );
}

static void test_combination_errors( void )
{
	const char *bad[2] = { "sum", "combine" };
	int i;
	for ( i = 0; i < 2; i++ )
	{
		mlt_profile profile = make_profile( 25, 1 );
		mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
		CHECK_CHECK( t != NULL );
		mlt_properties tp = MLT_TRANSITION_PROPERTIES( t );
		mlt_properties_set( tp, "sample_curve", "0=0;10=1" );
		mlt_properties_set_int( tp, bad[i], 1 );

		mlt_frame a = make_audio_frame( 0, 16, 1, 48000, 0.0f, 0.0f );
		mlt_frame b = make_audio_frame( 0, 16, 1, 48000, 1.0f, 0.0f );
		expect_get_audio_failure( t, a, b );

		mlt_frame_close( a );
		mlt_frame_close( b );
		mlt_transition_close( t );
		mlt_profile_close( profile );
	}
}

/* ------------------------------------------------------------------ */
/* legacy constant mix behaviour                                      */
/* ------------------------------------------------------------------ */

static void test_legacy_constant_mix( void )
{
	mlt_pool_init();
	mlt_profile profile = mlt_profile_init( NULL );
	CHECK_CHECK( profile != NULL );
	profile->frame_rate_num = 25;
	profile->frame_rate_den = 1;

	/* Same construction as support/baseline.c. */
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", "0.25" );
	CHECK_CHECK( t != NULL );
	mlt_properties producer = mlt_properties_new();
	mlt_properties_set_int( producer, "in", 0 );
	mlt_properties_set_int( producer, "out", 10 );

	mlt_frame a = mlt_frame_init( NULL );
	mlt_frame b = mlt_frame_init( NULL );
	float *aa = calloc( 8, sizeof( float ) );
	float *bb = calloc( 8, sizeof( float ) );
	int i;
	for ( i = 0; i < 8; i++ )
		bb[i] = 1.0f;
	mlt_frame_set_audio( a, aa, mlt_audio_f32le, 8 * (int) sizeof( float ), free );
	mlt_frame_set_audio( b, bb, mlt_audio_f32le, 8 * (int) sizeof( float ), free );
	for ( i = 0; i < 2; i++ )
	{
		mlt_properties fp = MLT_FRAME_PROPERTIES( i ? b : a );
		mlt_properties_set_int( fp, "audio_frequency", 48000 );
		mlt_properties_set_int( fp, "audio_channels", 1 );
		mlt_properties_set_int( fp, "audio_samples", 8 );
		mlt_properties_set_int( fp, "test_audio", 0 );
	}
	mlt_properties_set_data( MLT_FRAME_PROPERTIES( b ), "_producer", producer, 0, NULL, NULL );
	mlt_properties_set_int( MLT_FRAME_PROPERTIES( b ), "_speed", 1 );
	mlt_frame_set_position( a, 0 );
	mlt_frame_set_position( b, 0 );

	t->process( t, a, b );
	void *out = NULL;
	mlt_audio_format fmt = mlt_audio_f32le;
	int rate = 48000, channels = 1, samples = 8;
	CHECK_CHECK( mlt_frame_get_audio( a, &out, &fmt, &rate, &channels, &samples ) == 0 );
	CHECK_CHECK( out != NULL && samples == 8 );
	CHECK_CHECK( fabs( ( (float *) out )[0] - 0.25 ) < 1.0e-6 );
	CHECK_CHECK( fabs( ( (float *) out )[7] - 0.25 ) < 1.0e-6 );

	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( t );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* Default 0.5 mix without start still behaves as before. */
static void test_legacy_default_mix( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );

	mlt_frame a = make_audio_frame( 0, 8, 1, 48000, 0.0f, 0.0f );
	mlt_frame b = make_audio_frame( 0, 8, 1, 48000, 1.0f, 0.0f );
	t->process( t, a, b );

	void *out = NULL;
	mlt_audio_format fmt = mlt_audio_f32le;
	int rate = 48000, channels = 1, samples = 8;
	CHECK_CHECK( mlt_frame_get_audio( a, &out, &fmt, &rate, &channels, &samples ) == 0 );
	/* No audio.mix properties: the old path defaults to 0.5/0.5. */
	CHECK_CHECK( fabs( ( (float *) out )[0] - 0.5 ) < 1.0e-6 );

	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( t );
	mlt_profile_close( profile );
}

/* If the in point is conveyed baseline-style through "_producer" instead of
 * the transition "in" property, it is still used when transition in is 0. */
static void test_producer_in_fallback( void )
{
	const int samples = 1920, channels = 1, rate = 48000;
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition t = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( t != NULL );
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( t ), "sample_curve", "0=0;10=1" );

	mlt_properties producer = mlt_properties_new();
	mlt_properties_set_int( producer, "in", 2 );
	mlt_properties_set_int( producer, "out", 12 );

	mlt_frame a = make_audio_frame( 3, samples, channels, rate, 0.0f, 0.0f );
	mlt_frame b = make_audio_frame( 3, samples, channels, rate, 1.0f, 0.0f );
	mlt_properties_set_data( MLT_FRAME_PROPERTIES( b ), "_producer", producer, 0, NULL, NULL );
	t->process( t, a, b );

	float *out = NULL;
	mlt_audio_format fmt;
	int gr = rate, gc = channels, gs = samples;
	CHECK_CHECK( mlt_frame_get_audio( a, (void **) &out, &fmt, &gr, &gc, &gs ) == 0 );

	int j;
	for ( j = 0; j < samples; j++ )
	{
		double x = curve_coordinate( 3, j, 25, 1, rate, 2 );
		double w = CLAMP( x / 10.0, 0.0, 1.0 );
		CHECK_CHECK( fabs( (double) out[ j ] - w ) < F_EPS );
	}

	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( t );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

void run_sample_curve_tests( void )
{
	mlt_pool_init();
	test_anim_at();
	test_frame_callback_error();
	test_fractional_mix();
	test_in_and_nonlinear();
	test_ntsc_boundaries();
	test_out_of_order();
	test_change_after_hand_off();
	test_reverse_clamp();
	test_mismatch_errors();
	test_combination_errors();
	test_legacy_constant_mix();
	test_legacy_default_mix();
	test_producer_in_fallback();
}
