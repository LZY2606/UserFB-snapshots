/*
 * transition_mix.c -- mix two audio streams
 * Copyright (C) 2003-2020 Meltytech, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <framework/mlt_transition.h>
#include <framework/mlt_frame.h>
#include <framework/mlt_profile.h>
#include <framework/mlt_log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_CHANNELS (6)
#define MAX_SAMPLES  (192000)
#define SAMPLE_BYTES(samples, channels) ((samples) * (channels) * sizeof(float))
#define MAX_BYTES    SAMPLE_BYTES( MAX_SAMPLES, MAX_CHANNELS )

// Keys of the sample_curve configuration snapshot taken at process() time
// and attached to the output frame. Frames whose audio is fetched later then
// keep using the settings they were processed with.
#define MIX_CURVE_PROPERTY   "_mix_sample_curve"
#define MIX_CURVE_IN         "_mix_curve_in"
#define MIX_CURVE_FPS_NUM    "_mix_curve_fps_num"
#define MIX_CURVE_FPS_DEN    "_mix_curve_fps_den"
#define MIX_CURVE_LENGTH     "_mix_curve_length"
#define MIX_CURVE_REVERSE    "_mix_curve_reverse"
#define MIX_CURVE_BAD_CONFIG "_mix_curve_bad_config"

typedef struct transition_mix_s
{
	mlt_transition parent;
	float src_buffer[MAX_SAMPLES *  MAX_CHANNELS];
	float dest_buffer[MAX_SAMPLES * MAX_CHANNELS];
	int src_buffer_count;
	int dest_buffer_count;
	mlt_position previous_frame_a;
	mlt_position previous_frame_b;
} *transition_mix;

static void mix_audio( double weight_start, double weight_end, float *buffer_a,
	float *buffer_b, int channels_a, int channels_b, int channels_out, int samples )
{
	int i, j;
	double a, b, v;

	// Compute a smooth ramp over start to end
	double mix = weight_start;
	double mix_step = ( weight_end - weight_start ) / samples;

	for ( i = 0; i < samples; i++ )
	{
		for ( j = 0; j < channels_out; j++ )
		{
			a = (double) buffer_a[ i * channels_a + j ];
			b = (double) buffer_b[ i * channels_b + j ];
			v = mix * b + (1.0 - mix) * a;
			buffer_a[ i * channels_a + j ] = v;
		}
		mix += mix_step;
	}
}

static void sum_audio( double weight_start, double weight_end, float *buffer_a,
	float *buffer_b, int channels_a, int channels_b, int channels_out, int samples )
{
	int i, j;
	double a, b;

	// Compute a smooth ramp over start to end
	double mix = weight_start;
	double mix_step = ( weight_end - weight_start ) / samples;

	for ( i = 0; i < samples; i++ )
	{
		for ( j = 0; j < channels_out; j++ )
		{
			a = (double) buffer_a[ i * channels_a + j ];
			b = (double) buffer_b[ i * channels_b + j ];
			buffer_a[ i * channels_a + j ] = mix * b + a;
		}
		mix += mix_step;
	}
}

// This filter uses an inline low pass filter to allow mixing without volume hacking.
static void combine_audio( double weight, float *buffer_a, float *buffer_b,
	int channels_a, int channels_b, int channels_out, int samples )
{
	int i, j;
	double Fc = 0.5;
	double B = exp(-2.0 * M_PI * Fc);
	double A = 1.0 - B;
	double a, b, v;
	double v_prev[MAX_CHANNELS];

	for ( j = 0; j < channels_out; j++ )
		v_prev[j] = (double) buffer_a[j];

	for ( i = 0; i < samples; i++ )
	{
		for ( j = 0; j < channels_out; j++ )
		{
			a = (double) buffer_a[ i * channels_a + j ];
			b = (double) buffer_b[ i * channels_b + j ];
			v = weight * a + b;
			v_prev[j] = buffer_a[ i * channels_a + j ] = v * A + v_prev[j] * B;
		}
	}
}

/** Mix frame audio sample by sample using the sample_curve keyframe string.
 *
 * The curve zero is the transition in point. If the video frame rate is
 * num/den and the audio sample rate is R, the first absolute sample index of
 * frame f is floor(f * R * den / num), and sample j of this frame is evaluated
 * on the curve at:
 *   ( first_sample + j ) * num / ( R * den ) - transition_in
 * The mix is (1 - w) * A + w * B. Reverse is applied after curve evaluation
 * and then w is clamped to [0, 1]. All channels of a sample share one weight.
 *
 * This path only supports f32le inputs with identical sample rate, channel
 * count and sample count on both sides; anything else is an error.
 */

static int mix_with_sample_curve( mlt_frame frame_a, mlt_transition transition,
	float *buffer_a, float *buffer_b,
	mlt_audio_format format_a, mlt_audio_format format_b,
	int frequency_a, int frequency_b,
	int channels_a, int channels_b,
	int samples_a, int samples_b,
	void **buffer, mlt_audio_format *format, int *frequency, int *channels, int *samples )
{
	mlt_properties frame_props = MLT_FRAME_PROPERTIES( frame_a );

	if ( mlt_properties_get_int( frame_props, MIX_CURVE_BAD_CONFIG ) )
	{
		mlt_log_error( MLT_TRANSITION_SERVICE(transition),
			"mix: sample_curve cannot be combined with sum or combine\n" );
		*buffer = NULL;
		return 1;
	}

	if ( format_a != mlt_audio_f32le || format_b != mlt_audio_f32le
	     || frequency_a <= 0 || frequency_a != frequency_b
	     || channels_a <= 0 || channels_a != channels_b
	     || samples_a <= 0 || samples_a != samples_b
	     || buffer_a == NULL || buffer_b == NULL )
	{
		mlt_log_error( MLT_TRANSITION_SERVICE(transition),
			"mix: sample_curve requires matching f32le audio on both inputs "
			"(format %d/%d rate %d/%d channels %d/%d samples %d/%d)\n",
			format_a, format_b, frequency_a, frequency_b,
			channels_a, channels_b, samples_a, samples_b );
		*buffer = NULL;
		return 1;
	}

	int num = mlt_properties_get_int( frame_props, MIX_CURVE_FPS_NUM );
	int den = mlt_properties_get_int( frame_props, MIX_CURVE_FPS_DEN );
	if ( num <= 0 )
		num = 25;
	if ( den <= 0 )
		den = 1;

	mlt_position in = mlt_properties_get_position( frame_props, MIX_CURVE_IN );
	int length = mlt_properties_get_int( frame_props, MIX_CURVE_LENGTH );
	int reverse = mlt_properties_get_int( frame_props, MIX_CURVE_REVERSE );
	int rate = frequency_a;
	int out_channels = channels_a;
	int out_samples = samples_a;
	mlt_position position = mlt_frame_get_position( frame_a );

	// floor(position * rate * den / num); position is non-negative here
	// because mlt_frame_get_position() clamps negative positions to zero.
	int64_t first_sample = (int64_t) position * rate * den / num;

	size_t bytes = SAMPLE_BYTES( out_samples, out_channels );
	float *output = malloc( bytes );
	if ( output == NULL )
	{
		*buffer = NULL;
		return 1;
	}

	int i, j;
	for ( i = 0; i < out_samples; i++ )
	{
		double curve_position =
			( (double) ( first_sample + i ) * (double) num )
			/ ( (double) rate * (double) den )
			- (double) in;
		double weight = mlt_properties_anim_get_double_at( frame_props,
			MIX_CURVE_PROPERTY, curve_position, length );
		if ( reverse )
			weight = 1.0 - weight;
		weight = CLAMP( weight, 0.0, 1.0 );

		for ( j = 0; j < out_channels; j++ )
		{
			double a = (double) buffer_a[ i * channels_a + j ];
			double b = (double) buffer_b[ i * channels_b + j ];
			output[ i * out_channels + j ] = (float) ( ( 1.0 - weight ) * a + weight * b );
		}
	}

	*buffer = output;
	*format = mlt_audio_f32le;
	*frequency = rate;
	*channels = out_channels;
	*samples = out_samples;
	mlt_frame_set_audio( frame_a, output, mlt_audio_f32le, bytes, free );

	return 0;
}

/** Get the audio.
*/

static int transition_get_audio( mlt_frame frame_a, void **buffer, mlt_audio_format *format, int *frequency, int *channels, int *samples )
{
	int error = 0;

	// Get the b frame from the stack
	mlt_frame frame_b = mlt_frame_pop_audio( frame_a );

	// Get the effect
	mlt_transition transition = mlt_frame_pop_audio( frame_a );

	// Get the properties of the b frame
	mlt_properties b_props = MLT_FRAME_PROPERTIES( frame_b );

	transition_mix self = transition->child;
	float *buffer_b, *buffer_a;
	mlt_audio_format format_b;
	int frequency_b = *frequency, frequency_a = *frequency;
	int channels_b = *channels, channels_a = *channels;
	int samples_b = *samples, samples_a = *samples;

	// We can only mix interleaved 32-bit float.
	*format = mlt_audio_f32le;
	// Get the audio from our producers. Keep the original shared format
	// pointer behaviour for the legacy path.
	format_b = *format;
	mlt_frame_get_audio( frame_b, (void**) &buffer_b, &format_b, &frequency_b, &channels_b, &samples_b );
	*format = format_b;
	mlt_frame_get_audio( frame_a, (void**) &buffer_a, format, &frequency_a, &channels_a, &samples_a );

	// Prevent dividing by zero.
	if ( !channels_a || !channels_b || !buffer_a || !buffer_b )
	{
		*buffer = NULL;
		return 1;
	}

	if ( buffer_b == buffer_a )
	{
		*samples = samples_b;
		*channels = channels_b;
		*buffer = buffer_b;
		*frequency = frequency_b;
		return error;
	}

	// I do not recall what these silent_audio properties are about.
	int silent = mlt_properties_get_int( MLT_FRAME_PROPERTIES( frame_a ), "silent_audio" );
	mlt_properties_set_int( MLT_FRAME_PROPERTIES( frame_a ), "silent_audio", 0 );
	if ( silent )
		memset( buffer_a, 0, samples_a * channels_a * sizeof( float ) );
	silent = mlt_properties_get_int( b_props, "silent_audio" );
	mlt_properties_set_int( b_props, "silent_audio", 0 );
	if ( silent )
		memset( buffer_b, 0, samples_b * channels_b * sizeof( float ) );

	// The sample_curve path evaluates a keyframe curve per sample. It is
	// stateless and only accepts matching f32le inputs on both frames.
	if ( mlt_properties_get( MLT_FRAME_PROPERTIES( frame_a ), MIX_CURVE_PROPERTY ) )
	{
		mlt_audio_format format_a = *format;
		return mix_with_sample_curve( frame_a, transition,
			buffer_a, buffer_b, format_a, format_b,
			frequency_a, frequency_b, channels_a, channels_b,
			samples_a, samples_b, buffer, format, frequency, channels, samples );
	}

	// At this point we have two frames of audio with possibly differing sample
	// counts. How to reconcile this?

#ifdef KEEP_IT_SIMPLE_AND_STUPID
	// The simple and stupid way to deal with different sample counts was to
	// use the lesser of the two. This sounds good. You can #define SIMPLE_AND_STUPID
	// and hear what it sounds like.
	*samples = MIN(samples_a, samples_b);
	*channels = MIN( MIN( channels_b, channels_a ), MAX_CHANNELS );
	*frequency = frequency_a;
	// Note this direct call to sum_audio() skips ramping and the alternative
	// mixing methods.
	sum_audio( 1, 1, buffer_a, buffer_b, channels_a, channels_b, *channels, *samples );
	*buffer = buffer_a;

	return error;
#endif

	// However, the simple and stupid approach drops samples. Over time, this
	// can accumulate and cause an A/V sync drift, which addressed in b2640656
	// by saving the unused samples in a buffer and then using them first on the
	// next iteration.

	// determine number of samples to process
	*samples = MIN( self->src_buffer_count + samples_b, self->dest_buffer_count + samples_a );
	*channels = MIN( MIN( channels_b, channels_a ), MAX_CHANNELS );
	*frequency = frequency_a;

	// Prevent src buffer overflow by discarding oldest samples.
	samples_b = MIN( samples_b, MAX_SAMPLES * MAX_CHANNELS / channels_b );
	size_t bytes = SAMPLE_BYTES( samples_b, channels_b );
	if ( SAMPLE_BYTES( self->src_buffer_count + samples_b, channels_b ) > MAX_BYTES ) {
		mlt_log_verbose( MLT_TRANSITION_SERVICE(transition), "buffer overflow: src_buffer_count %d\n",
					  self->src_buffer_count );
		self->src_buffer_count = MAX_SAMPLES * MAX_CHANNELS / channels_b - samples_b;
		memmove( self->src_buffer, &self->src_buffer[MAX_SAMPLES * MAX_CHANNELS - samples_b * channels_b],
				 SAMPLE_BYTES( samples_b, channels_b ) );
	}

	// Silence src buffer if discontinuity
	if (self->src_buffer_count > 0 && mlt_frame_get_position(frame_b) != self->previous_frame_b + 1)
		memset(self->src_buffer, 0, SAMPLE_BYTES(self->src_buffer_count, channels_b));
	self->previous_frame_b = mlt_frame_get_position(frame_b);

	// Append the new samples from frame B to the src buffer
	memcpy( &self->src_buffer[self->src_buffer_count * channels_b], buffer_b, bytes );
	self->src_buffer_count += samples_b;
	buffer_b = self->src_buffer;

	// Prevent dest buffer overflow by discarding oldest samples.
	samples_a = MIN( samples_a, MAX_SAMPLES * MAX_CHANNELS / channels_a );
	bytes = SAMPLE_BYTES( samples_a, channels_a );
	if ( SAMPLE_BYTES( self->dest_buffer_count + samples_a, channels_a ) > MAX_BYTES ) {
		mlt_log_verbose( MLT_TRANSITION_SERVICE(transition), "buffer overflow: dest_buffer_count %d\n",
					  self->dest_buffer_count );
		self->dest_buffer_count = MAX_SAMPLES * MAX_CHANNELS / channels_a - samples_a;
		memmove( self->dest_buffer, &self->dest_buffer[MAX_SAMPLES * MAX_CHANNELS - samples_a * channels_a],
				 SAMPLE_BYTES( samples_a, channels_a ) );
	}

	// Silence dest buffer if discontinuity
	if (self->dest_buffer_count > 0 && mlt_frame_get_position(frame_a) != self->previous_frame_a + 1)
		memset(self->dest_buffer, 0, SAMPLE_BYTES(self->dest_buffer_count, channels_a));
	self->previous_frame_a = mlt_frame_get_position(frame_a);

	// Append the new samples from frame A to the dest buffer
	memcpy( &self->dest_buffer[self->dest_buffer_count * channels_a], buffer_a, bytes );
	self->dest_buffer_count += samples_a;
	buffer_a = self->dest_buffer;

	// Do the mixing.
	if ( mlt_properties_get_int( MLT_TRANSITION_PROPERTIES(transition), "sum" ) )
	{
		double mix_start = 1.0, mix_end = 1.0;
		if ( mlt_properties_get( b_props, "audio.previous_mix" ) )
			mix_start = mlt_properties_get_double( b_props, "audio.previous_mix" );
		if ( mlt_properties_get( b_props, "audio.mix" ) )
			mix_end = mlt_properties_get_double( b_props, "audio.mix" );
		if ( mlt_properties_get_int( b_props, "audio.reverse" ) )
		{
			mix_start = 1.0 - mix_start;
			mix_end = 1.0 - mix_end;
		}
		sum_audio( mix_start, mix_end, buffer_a, buffer_b, channels_a, channels_b, *channels, *samples );
	}
	else if ( mlt_properties_get_int( MLT_TRANSITION_PROPERTIES(transition), "combine" ) )
	{
		double weight = 1.0;
		if ( mlt_properties_get_int( MLT_FRAME_PROPERTIES( frame_a ), "meta.mixdown" ) )
			weight = 1.0 - mlt_properties_get_double( MLT_FRAME_PROPERTIES( frame_a ), "meta.volume" );
		combine_audio( weight, buffer_a, buffer_b, channels_a, channels_b, *channels, *samples );
	}
	else
	{
		double mix_start = 0.5, mix_end = 0.5;
		if ( mlt_properties_get( b_props, "audio.previous_mix" ) )
			mix_start = mlt_properties_get_double( b_props, "audio.previous_mix" );
		if ( mlt_properties_get( b_props, "audio.mix" ) )
			mix_end = mlt_properties_get_double( b_props, "audio.mix" );
		if ( mlt_properties_get_int( b_props, "audio.reverse" ) )
		{
			mix_start = 1.0 - mix_start;
			mix_end = 1.0 - mix_end;
		}
		mix_audio( mix_start, mix_end, buffer_a, buffer_b, channels_a, channels_b, *channels, *samples );
	}

	// Copy the audio from the dest buffer into the frame.
	bytes = SAMPLE_BYTES( *samples, *channels );
	*buffer = mlt_pool_alloc( bytes );
	memcpy( *buffer, buffer_a, bytes );
	mlt_frame_set_audio( frame_a, *buffer, *format, bytes, mlt_pool_release );

	if ( mlt_properties_get_int( b_props, "_speed" ) == 0 )
	{
		// Flush the buffer when paused and scrubbing.
		samples_b = self->src_buffer_count;
		samples_a = self->dest_buffer_count;
	}
	else
	{
		// It is also not good for A/V sync to let many samples accumulate in
		// the buffer. This part provides a time-based buffer limit.

		// Determine the maximum amount of latency permitted in the buffer.
		int max_latency = CLAMP( *frequency / 1000, 0, MAX_SAMPLES ); // samples in 1ms
		// samples_b becomes the new target src buffer count.
		samples_b = CLAMP( self->src_buffer_count - *samples, 0, max_latency );
		// samples_b becomes the number of samples to consume: difference between actual and the target.
		samples_b = self->src_buffer_count - samples_b;
		// samples_a becomes the new target dest buffer count.
		samples_a = CLAMP( self->dest_buffer_count - *samples, 0, max_latency );
		// samples_a becomes the number of samples to consume: difference between actual and the target.
		samples_a = self->dest_buffer_count - samples_a;
	}

	// Consume the src buffer.
	self->src_buffer_count -= samples_b;
	if ( self->src_buffer_count ) {
		memmove( self->src_buffer, &self->src_buffer[samples_b * channels_b],
			SAMPLE_BYTES( self->src_buffer_count, channels_b ));
	}
	// Consume the dest buffer.
	self->dest_buffer_count -= samples_a;
	if ( self->dest_buffer_count > 0 ) {
		memmove( self->dest_buffer, &self->dest_buffer[samples_a * channels_a],
			SAMPLE_BYTES( self->dest_buffer_count, channels_a ));
	}

	return error;
}


/** Mix transition processing.
*/

static mlt_frame transition_process( mlt_transition transition, mlt_frame a_frame, mlt_frame b_frame )
{
	mlt_properties properties = MLT_TRANSITION_PROPERTIES( transition );
	mlt_properties b_props = MLT_FRAME_PROPERTIES( b_frame );

	// Snapshot the sample_curve configuration onto the output frame. Audio is
	// often fetched lazily, and the frame must keep using the settings from
	// this process() call even if the transition properties change later.
	const char *sample_curve = mlt_properties_get( properties, "sample_curve" );
	if ( sample_curve )
	{
		mlt_properties a_props = MLT_FRAME_PROPERTIES( a_frame );
		mlt_profile profile = mlt_service_profile( MLT_TRANSITION_SERVICE( transition ) );
		int fps_num = profile ? profile->frame_rate_num
			: mlt_properties_get_int( properties, "frame_rate_num" );
		int fps_den = profile ? profile->frame_rate_den
			: mlt_properties_get_int( properties, "frame_rate_den" );
		if ( fps_num <= 0 )
			fps_num = 25;
		if ( fps_den <= 0 )
			fps_den = 1;

		// The curve zero is the transition in point. Some direct callers
		// convey the in point through the b frame's "_producer" properties
		// (same convention as the start/end block below); use that only when
		// the transition itself was not given a non-zero in point.
		mlt_position curve_in = mlt_transition_get_in( transition );
		if ( curve_in == 0 )
		{
			mlt_properties producer_props =
				mlt_properties_get_data( b_props, "_producer", NULL );
			if ( producer_props && mlt_properties_get( producer_props, "in" ) )
				curve_in = mlt_properties_get_position( producer_props, "in" );
		}

		int curve_length = mlt_properties_get_int( properties, "length" );
		if ( curve_length <= 0 )
			curve_length = (int) mlt_transition_get_length( transition );

		// Reverse may be given on the transition or, following the existing
		// mix branches' convention, as "audio.reverse" on the b frame.
		int curve_reverse = mlt_properties_get_int( properties, "reverse" )
			|| mlt_properties_get_int( b_props, "audio.reverse" );

		mlt_properties_set( a_props, MIX_CURVE_PROPERTY, sample_curve );
		mlt_properties_set_position( a_props, MIX_CURVE_IN, curve_in );
		mlt_properties_set_int( a_props, MIX_CURVE_FPS_NUM, fps_num );
		mlt_properties_set_int( a_props, MIX_CURVE_FPS_DEN, fps_den );
		mlt_properties_set_int( a_props, MIX_CURVE_LENGTH, curve_length );
		mlt_properties_set_int( a_props, MIX_CURVE_REVERSE, curve_reverse );
		mlt_properties_set_int( a_props, MIX_CURVE_BAD_CONFIG,
			mlt_properties_get_int( properties, "sum" )
			|| mlt_properties_get_int( properties, "combine" ) );
		// Let anim_get_double_at() resolve frame rate dependent keyframe strings.
		if ( profile )
			mlt_properties_set_data( a_props, "_profile", profile, 0, NULL, NULL );
	}

	// Only if mix is specified, otherwise a producer may set the mix
	if ( mlt_properties_get( properties, "start" ) )
	{
		// Determine the time position of this frame in the transition duration
		mlt_properties props = mlt_properties_get_data( MLT_FRAME_PROPERTIES( b_frame ), "_producer", NULL );
		mlt_position in = mlt_properties_get_int( props, "in" );
		mlt_position out = mlt_properties_get_int( props, "out" );
		int length = mlt_properties_get_int( properties, "length" );
		mlt_position time = mlt_properties_get_int( props, "_frame" );
		double mix = mlt_transition_get_progress( transition, b_frame );
		if ( mlt_properties_get_int(  properties, "always_active" ) )
			mix = ( double ) ( time - in ) / ( double ) ( out - in + 1 );

		// TODO: Check the logic here - shouldn't we be computing current and next mixing levels in all cases?
		if ( length == 0 )
		{
			// If there is an end mix level adjust mix to the range
			if ( mlt_properties_get( properties, "end" ) )
			{
				double start = mlt_properties_get_double( properties, "start" );
				double end = mlt_properties_get_double( properties, "end" );
				mix = start + ( end - start ) * mix;
			}
			// A negative means total crossfade (uses position)
			else if ( mlt_properties_get_double( properties, "start" ) >= 0 )
			{
				// Otherwise, start/constructor is a constant mix level
		    	mix = mlt_properties_get_double( properties, "start" );
			}
		
			// Finally, set the mix property on the frame
			mlt_properties_set_double( b_props, "audio.mix", mix );
	
			// Initialise transition previous mix value to prevent an inadvertent jump from 0
			mlt_position last_position = mlt_properties_get_position( properties, "_last_position" );
			mlt_position current_position = mlt_frame_get_position( b_frame );
			mlt_properties_set_position( properties, "_last_position", current_position );
			if ( !mlt_properties_get( properties, "_previous_mix" )
			     || current_position != last_position + 1 )
				mlt_properties_set_double( properties, "_previous_mix", mix );
				
			// Tell b frame what the previous mix level was
			mlt_properties_set_double( b_props, "audio.previous_mix", mlt_properties_get_double( properties, "_previous_mix" ) );

			// Save the current mix level for the next iteration
			mlt_properties_set_double( properties, "_previous_mix", mlt_properties_get_double( b_props, "audio.mix" ) );
		
			mlt_properties_set_double( b_props, "audio.reverse", mlt_properties_get_double( properties, "reverse" ) );
		}
		else
		{
			double level = mlt_properties_get_double( properties, "start" );
			double mix_start = level;
			double mix_end = mix_start;
			double mix_increment = 1.0 / length;
			if ( time - in < length )
			{
				mix_start = mix_start * ( ( double )( time - in ) / length );
				mix_end = mix_start + mix_increment;
			}
			else if ( time > out - length )
			{
				mix_end = mix_start * ( ( double )( out - time - in ) / length );
				mix_start = mix_end - mix_increment;
			}

			mix_start = mix_start < 0 ? 0 : mix_start > level ? level : mix_start;
			mix_end = mix_end < 0 ? 0 : mix_end > level ? level : mix_end;
			mlt_properties_set_double( b_props, "audio.previous_mix", mix_start );
			mlt_properties_set_double( b_props, "audio.mix", mix_end );
		}
	}

	// Override the get_audio method
	mlt_frame_push_audio( a_frame, transition );
	mlt_frame_push_audio( a_frame, b_frame );
	mlt_frame_push_audio( a_frame, transition_get_audio );

	// Ensure transition_get_audio is called if test_audio=1.
	if ( mlt_properties_get_int( properties, "accepts_blanks" ) )
		mlt_properties_set_int( MLT_FRAME_PROPERTIES(a_frame), "test_audio", 0 );

	return a_frame;
}

static void transition_close( mlt_transition transition )
{
	free( transition->child );
	transition->close = NULL;
	mlt_transition_close( transition );
}

/** Constructor for the transition.
*/

mlt_transition transition_mix_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg )
{
	transition_mix mix = calloc( 1 , sizeof( struct transition_mix_s ) );
	mlt_transition transition = calloc( 1, sizeof( struct mlt_transition_s ) );
	if ( mix && transition && !mlt_transition_init( transition, mix ) )
	{
		mix->parent = transition;
		transition->close = transition_close;
		transition->process = transition_process;
		// Direct users of the constructor (and the unit tests) pass the
		// profile here; factory-created transitions already carry "_profile".
		if ( profile )
			mlt_service_set_profile( MLT_TRANSITION_SERVICE( transition ), profile );
		if ( arg )
		{
			mlt_properties_set_double( MLT_TRANSITION_PROPERTIES( transition ), "start", atof( arg ) );
			if ( atof( arg ) < 0 )
				mlt_properties_set_int( MLT_TRANSITION_PROPERTIES( transition ), "accepts_blanks", 1 );
		}
		// Inform apps and framework that this is an audio only transition
		mlt_properties_set_int( MLT_TRANSITION_PROPERTIES( transition ), "_transition_type", 2 );
	} else {
		if ( transition )
			mlt_transition_close( transition );
		if ( mix )
			free( mix );
	}
	return transition;
}

