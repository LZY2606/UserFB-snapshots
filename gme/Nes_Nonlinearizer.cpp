// Implementation of the joint nonlinear DAC mixer.

// Nes_Snd_Emu 0.1.8
#include "Nes_Nonlinearizer.h"

#include "Nes_Apu.h"

#include "blargg_source.h"

#include <algorithm>
#include <math.h>

Nes_Nonlinearizer::Nes_Nonlinearizer()
{
	apu = 0;
	buf = 0;
	event_count = 0;
	mute_mask = 0;
	last_mixed = 0;
	listener = 0;
	listener_data = 0;
	for ( int i = 0; i < 5; i++ )
		raw [i] = 0;
}

void Nes_Nonlinearizer::attach( Nes_Apu* a, Blip_Buffer* b )
{
	require( b != 0 );
	apu = a;
	buf = b;
	event_count = 0;
	synth.volume( 1.0 );
	reset_reference();
}

void Nes_Nonlinearizer::detach()
{
	apu = 0;
	buf = 0;
	event_count = 0;
}

void Nes_Nonlinearizer::reset_reference()
{
	event_count = 0;

	if ( apu )
	{
		// Raw DAC levels right after the APU reset.
		raw [0] = apu->square1.last_amp;
		raw [1] = apu->square2.last_amp;
		raw [2] = apu->triangle.calc_amp();
		raw [3] = apu->noise.last_amp;
		raw [4] = apu->dmc.dac;

		// Keep oscillator bookkeeping aligned so regular run loops do not
		// produce spurious initial edges.
		apu->triangle.last_amp = raw [2];
		apu->dmc.last_amp = raw [4];
	}

	last_mixed = current_mixed();
}

int Nes_Nonlinearizer::mix_levels( int p1, int p2, int t, int n, int d )
{
	int const P = p1 + p2;
	double pulse = 0.0;
	if ( P != 0 )
		pulse = 95.88 / (8128.0 / P + 100.0);

	double const S = t / 8227.0 + n / 12241.0 + d / 22638.0;
	double tnd = 0.0;
	if ( S != 0.0 )
		tnd = 159.79 / (1.0 / S + 100.0);

	return (int) floor( 16384.0 * (pulse + tnd) + 0.5 );
}

int Nes_Nonlinearizer::current_mixed() const
{
	int levels [5];
	for ( int i = 0; i < 5; i++ )
		levels [i] = (mute_mask >> i) & 1 ? 0 : raw [i];

	return mix_levels( levels [0], levels [1], levels [2], levels [3], levels [4] );
}

void Nes_Nonlinearizer::flush( nes_time_t end_time )
{
	if ( !buf || event_count == 0 )
		return;

	// Events are appended oscillator by oscillator; a stable sort keeps the
	// register-write call order for edges landing on the same CPU cycle.
	std::stable_sort( events.begin(), events.begin() + event_count,
			[]( Nes_Nonlinear_Event const& a, Nes_Nonlinear_Event const& b ) {
				return a.time < b.time;
			} );

	int processed = 0;
	while ( processed < event_count && events [processed].time <= end_time )
	{
		nes_time_t t = events [processed].time;
		do
		{
			Nes_Nonlinear_Event const& ev = events [processed];
			raw [ev.osc] = ev.amp;
			++processed;
		}
		while ( processed < event_count && events [processed].time == t );

		int mixed = current_mixed();
		int delta = mixed - last_mixed;
		if ( delta )
		{
			synth.offset( t, delta, buf );
			last_mixed = mixed;
		}
		if ( listener )
			listener( listener_data, t, mixed );
	}

	// Keep events beyond end_time (DMC run ahead), packed at the front.
	int remain = event_count - processed;
	for ( int i = 0; i < remain; i++ )
		events [i] = events [processed + i];
	event_count = remain;
}

void Nes_Nonlinearizer::end_frame( nes_time_t frame_time )
{
	flush( frame_time );

	// Any events left over were produced by the DMC, which may have run past
	// the frame end. They belong to the next frame's time origin.
	for ( int i = 0; i < event_count; i++ )
		events [i].time -= frame_time;
}

void Nes_Nonlinearizer::set_mute_mask( nes_time_t time, int mask )
{
	mask &= 0x1F;
	if ( mask == mute_mask )
		return;

	flush( time );
	mute_mask = mask;

	int mixed = current_mixed();
	int delta = mixed - last_mixed;
	if ( delta && buf )
	{
		synth.offset( time, delta, buf );
		last_mixed = mixed;
		if ( listener )
			listener( listener_data, time, mixed );
	}
}
