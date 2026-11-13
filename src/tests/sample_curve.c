/*
 * sample_curve.c -- regression tests for per-sample mix curves in transition_mix
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

#include <framework/mlt.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

extern mlt_transition transition_mix_init( mlt_profile, mlt_service_type, const char *, char * );

#define RATE 48000
#define TOL 1e-6

/* The two streams use distinct constants and per-channel markers so that a
 * wrong source or a per-channel weight mix-up is visible in the output. */
#define A_CH0 0.0f
#define A_CH1 ( -0.5f )
#define B_CH0 1.0f
#define B_CH1 0.5f

static mlt_profile make_profile( int num, int den )
{
	mlt_profile profile = mlt_profile_init( NULL );
	if ( profile )
	{
		profile->frame_rate_num = num;
		profile->frame_rate_den = den;
	}
	return profile;
}

static mlt_properties make_producer( void )
{
	mlt_properties producer = mlt_properties_new();
	mlt_properties_set_int( producer, "in", 0 );
	mlt_properties_set_int( producer, "out", 10 );
	return producer;
}

static mlt_frame make_frame( mlt_properties producer, float ch0, float ch1, int channels, int samples,
	mlt_position position )
{
	mlt_frame frame = mlt_frame_init( NULL );
	int count = channels * samples;
	float *data = malloc( count * sizeof( float ) );
	for ( int i = 0; i < samples; i++ )
	{
		data[ i * channels ] = ch0;
		if ( channels > 1 )
			data[ i * channels + 1 ] = ch1;
	}
	mlt_frame_set_audio( frame, data, mlt_audio_f32le, count * sizeof( float ), free );
	mlt_properties properties = MLT_FRAME_PROPERTIES( frame );
	mlt_properties_set_int( properties, "audio_frequency", RATE );
	mlt_properties_set_int( properties, "audio_channels", channels );
	mlt_properties_set_int( properties, "audio_samples", samples );
	mlt_properties_set_int( properties, "test_audio", 0 );
	mlt_properties_set_data( properties, "_producer", producer, 0, NULL, NULL );
	mlt_properties_set_int( properties, "_speed", 1 );
	mlt_frame_set_position( frame, position );
	return frame;
}

/* Process one a/b frame pair through the transition and fetch the mixed audio.
 * Returns the get_audio error code. */
static int mix_and_get( mlt_transition transition, mlt_frame a, mlt_frame b, float **out, int *samples,
	int *channels )
{
	transition->process( transition, a, b );
	mlt_audio_format format = mlt_audio_f32le;
	int rate = RATE;
	void *buffer = NULL;
	*channels = 2;
	*samples = 0;
	int error = mlt_frame_get_audio( a, &buffer, &format, &rate, channels, samples );
	*out = buffer;
	return error;
}

/* The legacy constant mix level must be unaffected by the new code. */
static void test_legacy_constant_mix( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", "0.25" );
	CHECK_CHECK( transition );
	mlt_properties producer = make_producer();
	mlt_frame a = make_frame( producer, A_CH0, A_CH0, 1, 8, 0 );
	mlt_frame b = make_frame( producer, B_CH0, B_CH0, 1, 8, 0 );
	transition->process( transition, a, b );
	mlt_audio_format format = mlt_audio_f32le;
	int rate = RATE, channels = 1, samples = 8;
	void *out = NULL;
	CHECK_CHECK( !mlt_frame_get_audio( a, &out, &format, &rate, &channels, &samples ) );
	CHECK_CHECK( out && samples == 8 );
	for ( int i = 0; i < samples; i++ )
		CHECK_CHECK( fabs( ( (float*) out )[ i ] - 0.25 ) < TOL );
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* mlt_properties_anim_get_double_at evaluates numeric animations at
 * fractional frame positions. */
static void test_anim_get_double_at( void )
{
	mlt_properties p = mlt_properties_new();
	mlt_properties_set( p, "curve", "0=0;10=1" );

	// Integer positions agree with the existing entry point.
	for ( int i = 0; i <= 10; i++ )
		CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "curve", i, 11 )
			- mlt_properties_anim_get_double( p, "curve", i, 11 ) ) < 1e-12 );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "curve", 5, 11 ) - 0.5 ) < 1e-12 );

	// Fractional positions follow the linear segment.
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "curve", 2.5, 11 ) - 0.25 ) < 1e-12 );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "curve", 0.25, 11 ) - 0.025 ) < 1e-12 );

	// Missing property evaluates to 0.
	CHECK_CHECK( mlt_properties_anim_get_double_at( p, "missing", 2.5, 11 ) == 0.0 );

	// Positions before the first and after the last keyframe hold the end values.
	mlt_properties_set( p, "ends", "5=0.4;10=0.8" );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "ends", 2.5, 11 ) - 0.4 ) < 1e-12 );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "ends", 12.5, 11 ) - 0.8 ) < 1e-12 );

	// Discrete keyframes hold their value until the next keyframe.
	mlt_properties_set( p, "steps", "0|=0.2;10|=0.6" );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "steps", 9.5, 11 ) - 0.2 ) < 1e-12 );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "steps", 10.0, 11 ) - 0.6 ) < 1e-12 );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "steps", 10.5, 11 ) - 0.6 ) < 1e-12 );

	// Smooth keyframes follow the spline, not a line between integer evaluations.
	mlt_properties_set( p, "smooth", "0~=0;10~=1;20~=0" );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "smooth", 5.0, 21 ) - 0.5625 ) < 1e-12 );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "smooth", 2.5, 21 ) - 0.2265625 ) < 1e-12 );
	double midpoint = ( mlt_properties_anim_get_double( p, "smooth", 2, 21 )
		+ mlt_properties_anim_get_double( p, "smooth", 3, 21 ) ) / 2.0;
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "smooth", 2.5, 21 ) - midpoint ) > 1e-3 );

	// Negative keyframe times are interpreted relative to the length.
	mlt_properties_set( p, "neg", "0=0;-1=1" );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "neg", 5.0, 11 ) - 0.5 ) < 1e-12 );
	CHECK_CHECK( fabs( mlt_properties_anim_get_double_at( p, "neg", 10.0, 11 ) - 1.0 ) < 1e-12 );

	mlt_properties_close( p );
}

/* The weight of every sample comes from the curve at the fractional position
 * of that sample, not from a linear ramp across the frame. */
static void test_fractional_positions( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( transition );
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( transition ), "sample_curve", "0=0;10=1" );
	mlt_properties producer = make_producer();
	mlt_frame a = make_frame( producer, A_CH0, A_CH1, 2, 1920, 0 );
	mlt_frame b = make_frame( producer, B_CH0, B_CH1, 2, 1920, 0 );
	float *out = NULL;
	int samples = 0, channels = 0;
	CHECK_CHECK( !mix_and_get( transition, a, b, &out, &samples, &channels ) );
	CHECK_CHECK( out && samples == 1920 && channels == 2 );
	// 25 fps at 48000 Hz: sample j sits at curve position j / 1920.
	for ( int j = 0; j < 1920; j += 137 )
	{
		double w = (double) j / 1920.0 / 10.0;
		CHECK_CHECK( fabs( out[ j * 2 ] - ( ( 1.0 - w ) * A_CH0 + w * B_CH0 ) ) < TOL );
		CHECK_CHECK( fabs( out[ j * 2 + 1 ] - ( ( 1.0 - w ) * A_CH1 + w * B_CH1 ) ) < TOL );
	}
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* A non-linear curve is followed within the frame. */
static void test_nonlinear_curve( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( transition );
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( transition ), "sample_curve", "0~=0;10~=1;20~=0" );
	mlt_properties producer = make_producer();
	mlt_frame a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	mlt_frame b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	float *out = NULL;
	int samples = 0, channels = 0;
	CHECK_CHECK( !mix_and_get( transition, a, b, &out, &samples, &channels ) );
	CHECK_CHECK( out && samples == 1920 );
	// Sample 960 is at curve position 0.5, where the smooth curve evaluates
	// to 0.0298125; a straight ramp between frame ends would give 0.05.
	CHECK_CHECK( fabs( out[ 960 ] - 0.0298125 ) < TOL );
	CHECK_CHECK( fabs( out[ 0 ] - 0.0 ) < TOL );
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* NTSC frame rate alternates the number of samples per frame; the curve
 * position of a sample comes from its absolute sample index. */
static void test_ntsc_sample_boundaries( void )
{
	mlt_profile profile = make_profile( 30000, 1001 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( transition );
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( transition ), "sample_curve", "0=0;10=1" );
	mlt_properties producer = make_producer();
	// 48000 / (30000/1001) = 1601.6 samples per frame: 1601, 1602, 1601, ...
	int counts[] = { 1601, 1602, 1601 };
	mlt_frame a[ 3 ], b[ 3 ];
	float *out[ 3 ] = { NULL, NULL, NULL };
	for ( int f = 0; f < 3; f++ )
	{
		a[ f ] = make_frame( producer, A_CH0, A_CH0, 1, counts[ f ], f );
		b[ f ] = make_frame( producer, B_CH0, B_CH0, 1, counts[ f ], f );
		int samples = 0, channels = 0;
		CHECK_CHECK( !mix_and_get( transition, a[ f ], b[ f ], &out[ f ], &samples, &channels ) );
		CHECK_CHECK( out[ f ] && samples == counts[ f ] );
	}
	// Frame 1 starts at absolute sample floor(1 * 48000 * 1001 / 30000) = 1601,
	// which is curve position 1601 * 30000 / (48000 * 1001), not 1.0.
	CHECK_CHECK( fabs( out[ 1 ][ 0 ] - 0.0999625375 ) < TOL );
	// Frame 2 starts at absolute sample 3203.
	CHECK_CHECK( fabs( out[ 2 ][ 0 ] - 0.1999875125 ) < TOL );
	// The last sample of frame 0 sits just before curve position 1.
	CHECK_CHECK( fabs( out[ 0 ][ 1600 ] - 0.0999000999 ) < TOL );
	for ( int f = 0; f < 3; f++ )
	{
		mlt_frame_close( a[ f ] );
		mlt_frame_close( b[ f ] );
	}
	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* Sequential rendering, seeking and re-reading a frame give the same sound. */
static void test_out_of_order( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( transition );
	// A discrete step between frames must not leak across frames through any
	// remembered mix level.
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( transition ), "sample_curve", "0|=0.9;2|=0.1" );
	mlt_properties producer = make_producer();
	mlt_frame a[ 4 ], b[ 4 ];
	for ( int f = 0; f < 4; f++ )
	{
		a[ f ] = make_frame( producer, A_CH0, A_CH0, 1, 1920, f );
		b[ f ] = make_frame( producer, B_CH0, B_CH0, 1, 1920, f );
		transition->process( transition, a[ f ], b[ f ] );
	}
	// Fetch in a shuffled order.
	int order[] = { 2, 0, 3, 1 };
	float *out[ 4 ] = { NULL, NULL, NULL, NULL };
	for ( int k = 0; k < 4; k++ )
	{
		int f = order[ k ];
		mlt_audio_format format = mlt_audio_f32le;
		int rate = RATE, channels = 1, samples = 1920;
		void *buffer = NULL;
		CHECK_CHECK( !mlt_frame_get_audio( a[ f ], &buffer, &format, &rate, &channels, &samples ) );
		out[ f ] = buffer;
		CHECK_CHECK( out[ f ] && samples == 1920 );
	}
	// Frames 0 and 1 sit on the 0.9 plateau, frames 2 and 3 on 0.1.
	for ( int j = 0; j < 1920; j += 191 )
	{
		CHECK_CHECK( fabs( out[ 0 ][ j ] - 0.9 ) < TOL );
		CHECK_CHECK( fabs( out[ 1 ][ j ] - 0.9 ) < TOL );
		CHECK_CHECK( fabs( out[ 2 ][ j ] - 0.1 ) < TOL );
		CHECK_CHECK( fabs( out[ 3 ][ j ] - 0.1 ) < TOL );
	}
	// Re-reading a frame gives the same samples again.
	mlt_audio_format format = mlt_audio_f32le;
	int rate = RATE, channels = 1, samples = 1920;
	void *again = NULL;
	CHECK_CHECK( !mlt_frame_get_audio( a[ 2 ], &again, &format, &rate, &channels, &samples ) );
	CHECK_CHECK( again && !memcmp( again, out[ 2 ], 1920 * sizeof( float ) ) );
	for ( int f = 0; f < 4; f++ )
	{
		mlt_frame_close( a[ f ] );
		mlt_frame_close( b[ f ] );
	}
	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* A frame that already went through process keeps the curve settings from
 * that moment, even if the transition properties change afterwards. */
static void test_property_change_after_process( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( transition );
	mlt_properties properties = MLT_TRANSITION_PROPERTIES( transition );
	mlt_properties_set( properties, "sample_curve", "0=0;10=1" );
	mlt_properties producer = make_producer();
	mlt_frame a0 = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	mlt_frame b0 = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	transition->process( transition, a0, b0 );
	// Change the curve before the processed frame is asked for its audio.
	mlt_properties_set( properties, "sample_curve", "0=1;10=1" );
	mlt_frame a1 = make_frame( producer, A_CH0, A_CH0, 1, 1920, 1 );
	mlt_frame b1 = make_frame( producer, B_CH0, B_CH0, 1, 1920, 1 );
	transition->process( transition, a1, b1 );

	mlt_audio_format format = mlt_audio_f32le;
	int rate = RATE, channels = 1, samples = 1920;
	void *out0 = NULL, *out1 = NULL;
	CHECK_CHECK( !mlt_frame_get_audio( a0, &out0, &format, &rate, &channels, &samples ) );
	CHECK_CHECK( !mlt_frame_get_audio( a1, &out1, &format, &rate, &channels, &samples ) );
	// The first frame still uses the old curve: weight 0 at its first sample.
	CHECK_CHECK( out0 && fabs( ( (float*) out0 )[ 0 ] - A_CH0 ) < TOL );
	CHECK_CHECK( fabs( ( (float*) out0 )[ 960 ] - 0.05 * B_CH0 ) < TOL );
	// The second frame uses the new constant curve.
	CHECK_CHECK( out1 && fabs( ( (float*) out1 )[ 0 ] - B_CH0 ) < TOL );
	CHECK_CHECK( fabs( ( (float*) out1 )[ 1919 ] - B_CH0 ) < TOL );
	mlt_frame_close( a0 );
	mlt_frame_close( b0 );
	mlt_frame_close( a1 );
	mlt_frame_close( b1 );
	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* reverse inverts the curve value and the weight is clamped to [0, 1]. */
static void test_reverse_and_clamp( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( transition );
	mlt_properties properties = MLT_TRANSITION_PROPERTIES( transition );
	mlt_properties_set( properties, "sample_curve", "0=0;10=1" );
	mlt_properties_set_int( properties, "reverse", 1 );
	mlt_properties producer = make_producer();
	mlt_frame a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	mlt_frame b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	float *out = NULL;
	int samples = 0, channels = 0;
	CHECK_CHECK( !mix_and_get( transition, a, b, &out, &samples, &channels ) );
	CHECK_CHECK( out && fabs( out[ 0 ] - B_CH0 ) < TOL );
	CHECK_CHECK( fabs( out[ 960 ] - 0.95 * B_CH0 ) < TOL );
	mlt_frame_close( a );
	mlt_frame_close( b );

	// Out of range curve values are clamped.
	mlt_properties_set_int( properties, "reverse", 0 );
	mlt_properties_set( properties, "sample_curve", "0=2;10=2" );
	a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	CHECK_CHECK( !mix_and_get( transition, a, b, &out, &samples, &channels ) );
	CHECK_CHECK( out && fabs( out[ 0 ] - B_CH0 ) < TOL && fabs( out[ 1919 ] - B_CH0 ) < TOL );
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_properties_set( properties, "sample_curve", "0=-1;10=-1" );
	a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	CHECK_CHECK( !mix_and_get( transition, a, b, &out, &samples, &channels ) );
	CHECK_CHECK( out && fabs( out[ 0 ] - A_CH0 ) < TOL && fabs( out[ 1919 ] - A_CH0 ) < TOL );
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* The curve zero point is the transition in point. */
static void test_curve_origin_at_in( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( transition );
	mlt_transition_set_in_and_out( transition, 5, 15 );
	mlt_properties_set( MLT_TRANSITION_PROPERTIES( transition ), "sample_curve", "0=0;10=1" );
	mlt_properties producer = make_producer();
	mlt_frame a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 5 );
	mlt_frame b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 5 );
	float *out = NULL;
	int samples = 0, channels = 0;
	CHECK_CHECK( !mix_and_get( transition, a, b, &out, &samples, &channels ) );
	// The frame at the in point starts at curve position 0.
	CHECK_CHECK( out && fabs( out[ 0 ] - A_CH0 ) < TOL );
	CHECK_CHECK( fabs( out[ 960 ] - 0.05 * B_CH0 ) < TOL );
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

/* sample_curve is mutually exclusive with sum and combine, and mismatched
 * streams are an error instead of a silent resample. */
static void test_configuration_errors( void )
{
	mlt_profile profile = make_profile( 25, 1 );
	mlt_transition transition = transition_mix_init( profile, mlt_service_transition_type, "mix", NULL );
	CHECK_CHECK( transition );
	mlt_properties properties = MLT_TRANSITION_PROPERTIES( transition );
	mlt_properties producer = make_producer();
	mlt_frame a = NULL, b = NULL;
	float *out = NULL;
	int samples = 0, channels = 0;

	mlt_properties_set( properties, "sample_curve", "0=0;10=1" );

	// sample_curve + sum is an error.
	mlt_properties_set_int( properties, "sum", 1 );
	a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	CHECK_CHECK( mix_and_get( transition, a, b, &out, &samples, &channels ) != 0 );
	CHECK_CHECK( out == NULL );
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_properties_set_int( properties, "sum", 0 );

	// sample_curve + combine is an error.
	mlt_properties_set_int( properties, "combine", 1 );
	a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	CHECK_CHECK( mix_and_get( transition, a, b, &out, &samples, &channels ) != 0 );
	CHECK_CHECK( out == NULL );
	mlt_frame_close( a );
	mlt_frame_close( b );
	mlt_properties_set_int( properties, "combine", 0 );

	// Mismatched channel counts are an error.
	a = make_frame( producer, A_CH0, A_CH1, 2, 1920, 0 );
	b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	CHECK_CHECK( mix_and_get( transition, a, b, &out, &samples, &channels ) != 0 );
	CHECK_CHECK( out == NULL );
	mlt_frame_close( a );
	mlt_frame_close( b );

	// Mismatched sample counts are an error.
	a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	b = make_frame( producer, B_CH0, B_CH0, 1, 1600, 0 );
	CHECK_CHECK( mix_and_get( transition, a, b, &out, &samples, &channels ) != 0 );
	CHECK_CHECK( out == NULL );
	mlt_frame_close( a );
	mlt_frame_close( b );

	// Mismatched sample rates are an error.
	a = make_frame( producer, A_CH0, A_CH0, 1, 1920, 0 );
	b = make_frame( producer, B_CH0, B_CH0, 1, 1920, 0 );
	mlt_properties_set_int( MLT_FRAME_PROPERTIES( b ), "audio_frequency", 44100 );
	CHECK_CHECK( mix_and_get( transition, a, b, &out, &samples, &channels ) != 0 );
	CHECK_CHECK( out == NULL );
	mlt_frame_close( a );
	mlt_frame_close( b );

	mlt_transition_close( transition );
	mlt_properties_close( producer );
	mlt_profile_close( profile );
}

void run_sample_curve_tests( void )
{
	mlt_pool_init();
	test_legacy_constant_mix();
	test_anim_get_double_at();
	test_fractional_positions();
	test_nonlinear_curve();
	test_ntsc_sample_boundaries();
	test_out_of_order();
	test_property_change_after_process();
	test_reverse_and_clamp();
	test_curve_origin_at_in();
	test_configuration_errors();
	mlt_pool_close();
}
