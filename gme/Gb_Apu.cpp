// Gb_Snd_Emu 0.1.5. http://www.slack.net/~ant/

#include "Gb_Apu.h"

#include <string.h>
#include <algorithm>

/* Copyright (C) 2003-2006 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#include "blargg_source.h"

static unsigned const vol_reg    = 0xFF24;
static unsigned const status_reg = 0xFF26;

using std::min;
using std::max;

	Gb_Apu::Gb_Apu()
{
	memset(regs, 0, sizeof(regs));

	square1.synth = square_synth;
	square2.synth = square_synth;
	wave.synth  = other_synth;
	noise.synth = other_synth;

	vol_gain_left = 0;
	vol_gain_right = 0;

	oscs [0] = &square1;
	oscs [1] = &square2;
	oscs [2] = &wave;
	oscs [3] = &noise;

	for ( int i = 0; i < osc_count; i++ )
	{
		Gb_Osc& osc = *oscs [i];
		osc.regs = &regs [i * 5];
		osc.output = 0;
		osc.out_count = 0;
		osc.outputs [0] = 0;
		osc.outputs [1] = 0;
		osc.outputs [2] = 0;
		osc.outputs [3] = 0;
	}

	set_tempo( 1.0 );
	volume( 1.0 );
	reset();
}

void Gb_Apu::treble_eq( const blip_eq_t& eq )
{
	for ( int g = 0; g < 8; g++ )
	{
		square_synth [g].treble_eq( eq );
		other_synth [g].treble_eq( eq );
	}
}

void Gb_Apu::osc_output( int index, Blip_Buffer* center, Blip_Buffer* left, Blip_Buffer* right )
{
	require( (unsigned) index < osc_count );
	require( (center && left && right) || (!center && !left && !right) );
	Gb_Osc& osc = *oscs [index];
	osc.outputs [1] = right;
	osc.outputs [2] = left;
	osc.outputs [3] = center;
	build_outputs( index );
}

void Gb_Apu::output( Blip_Buffer* center, Blip_Buffer* left, Blip_Buffer* right )
{
	for ( int i = 0; i < osc_count; i++ )
		osc_output( i, center, left, right );
}

void Gb_Apu::update_volume()
{
	// NR50: top three bits (6..4) left, low three bits (2..0) right.
	// Each field gives (value + 1) / 8 gain; zero is minimum gain, not mute.
	// The VIN bits (3 and 7) are ignored as before.
	int data = regs [vol_reg - start_addr];
	vol_gain_left  = data >> 4 & 7;
	vol_gain_right = data & 7;

	// Index g synth already includes the (g + 1) numerator; volume_unit
	// contains the common /8 master range factor.
	for ( int g = 0; g < 8; g++ )
	{
		double vol = (g + 1) * volume_unit;
		square_synth [g].volume( vol );
		other_synth [g].volume( vol );
	}
}

void Gb_Apu::build_outputs( int index )
{
	require( (unsigned) index < osc_count );
	Gb_Osc& osc = *oscs [index];
	osc.out_count = 0;
	osc.output = 0;

	Blip_Buffer* right  = osc.outputs [1];
	Blip_Buffer* left   = osc.outputs [2];
	Blip_Buffer* center = osc.outputs [3];
	if ( !center && !left && !right )
		return; // oscillator fully silenced externally

	bool powered = (regs [status_reg - start_addr] & 0x80) != 0;
	int nr51 = regs [0xFF25 - start_addr];
	int en_right = powered && (nr51 >> index & 1);        // low nibble
	int en_left  = powered && (nr51 >> (index + 4) & 1);  // high nibble
	if ( !en_left && !en_right )
		return;

	// Mono convenience output(buffer) assigns the same buffer to all three.
	// Contribute once, at the greater of the gains of the routed sides; never
	// add multiple times just because the pointers alias.
	if ( center == left && left == right )
	{
		int gain = -1;
		if ( en_left )
			gain = vol_gain_left;
		if ( en_right )
			gain = max( gain, vol_gain_right );
		osc.outs [0].buf = center;
		osc.outs [0].gain = gain;
		osc.out_count = 1;
		osc.output = center;
		return;
	}

	// Both sides at equal gain share the single center contribution, keeping
	// the original symmetric output bit-for-bit.
	if ( en_left && en_right && vol_gain_left == vol_gain_right )
	{
		osc.outs [0].buf = center;
		osc.outs [0].gain = vol_gain_left;
		osc.out_count = 1;
		osc.output = center;
		return;
	}

	int n = 0;
	if ( en_left )
	{
		osc.outs [n].buf = left;
		osc.outs [n].gain = vol_gain_left;
		n++;
	}
	if ( en_right )
	{
		osc.outs [n].buf = right;
		osc.outs [n].gain = vol_gain_right;
		n++;
	}
	osc.out_count = n;
	osc.output = osc.outs [0].buf;
}

void Gb_Apu::retract_outputs( int index, blip_time_t time )
{
	Gb_Osc& osc = *oscs [index];
	int amp = osc.last_amp;
	// The standing level is owned by the endpoints currently listed.
	osc.last_amp = 0;
	if ( !amp )
		return;

	for ( int e = 0; e < osc.out_count; e++ )
	{
		int gain = osc.outs [e].gain;
		Blip_Buffer* buf = osc.outs [e].buf;
		if ( index < 2 )
			square_synth [gain].offset( time, -amp, buf );
		else
			other_synth [gain].offset( time, -amp, buf );
	}
}

static unsigned char const powerup_regs [0x20] = {
	0x80,0x3F,0x00,0xFF,0xBF, // square 1
	0xFF,0x3F,0x00,0xFF,0xBF, // square 2
	0x7F,0xFF,0x9F,0xFF,0xBF, // wave
	0xFF,0xFF,0x00,0x00,0xBF, // noise
	0x00, // left/right enables
	0x77, // master volume
	0x80, // power
	0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF
};

void Gb_Apu::set_tempo( double t )
{
	frame_period = 4194304 / 256; // 256 Hz
	if ( t != 1.0 )
		frame_period = blip_time_t (frame_period / t);
}

void Gb_Apu::reset()
{
	next_frame_time = 0;
	last_time       = 0;
	frame_count     = 0;

	square1.reset();
	square2.reset();
	wave.reset();
	noise.reset();
	noise.bits = 1;
	wave.wave_pos = 0;

	// avoid click at beginning
	regs [vol_reg - start_addr] = 0x77;
	update_volume();

	regs [status_reg - start_addr] = 0x01; // force power
	write_register( 0, status_reg, 0x00 );

	static unsigned char const initial_wave [] = {
		0x84,0x40,0x43,0xAA,0x2D,0x78,0x92,0x3C, // wave table
		0x60,0x59,0x59,0xB0,0x34,0xB8,0x2E,0xDA
	};
	memcpy( wave.wave, initial_wave, sizeof initial_wave );
}

void Gb_Apu::run_until( blip_time_t end_time )
{
	require( end_time >= last_time ); // end_time must not be before previous time
	if ( end_time == last_time )
		return;

	while ( true )
	{
		blip_time_t time = next_frame_time;
		if ( time > end_time )
			time = end_time;

		// run oscillators
		for ( int i = 0; i < osc_count; ++i )
		{
			Gb_Osc& osc = *oscs [i];
			if ( osc.output )
			{
				osc.output->set_modified(); // TODO: misses optimization opportunities?
				int playing = false;
				if ( osc.enabled && osc.volume &&
						(!(osc.regs [4] & osc.len_enabled_mask) || osc.length) )
					playing = -1;
				switch ( i )
				{
				case 0: square1.run( last_time, time, playing ); break;
				case 1: square2.run( last_time, time, playing ); break;
				case 2: wave   .run( last_time, time, playing ); break;
				case 3: noise  .run( last_time, time, playing ); break;
				}
			}
		}
		last_time = time;

		if ( time == end_time )
			break;

		next_frame_time += frame_period;

		// 256 Hz actions
		square1.clock_length();
		square2.clock_length();
		wave.clock_length();
		noise.clock_length();

		frame_count = (frame_count + 1) & 3;
		if ( frame_count == 0 )
		{
			// 64 Hz actions
			square1.clock_envelope();
			square2.clock_envelope();
			noise.clock_envelope();
		}

		if ( frame_count & 1 )
			square1.clock_sweep(); // 128 Hz action
	}
}

void Gb_Apu::end_frame( blip_time_t end_time )
{
	if ( end_time > last_time )
		run_until( end_time );

	assert( next_frame_time >= end_time );
	next_frame_time -= end_time;

	assert( last_time >= end_time );
	last_time -= end_time;
}

void Gb_Apu::write_register( blip_time_t time, unsigned addr, int data )
{
	require( (unsigned) data < 0x100 );

	int reg = addr - start_addr;
	if ( (unsigned) reg >= register_count )
		return;

	run_until( time );

	int old_reg = regs [reg];
	regs [reg] = data;

	if ( addr < vol_reg )
	{
		write_osc( reg / 5, reg, data );
	}
	else if ( addr == vol_reg && data != old_reg ) // global volume
	{
		// Retract every standing contribution from the old endpoints using
		// the old gain synths before changing the gain fields.
		for ( int i = 0; i < osc_count; i++ )
			retract_outputs( i, time );

		update_volume();

		// Endpoints stay on the same buffers but their gain indices change.
		// last_amp is zero, so each oscillator's next run continues from the
		// current waveform level through the new configuration at this time.
		for ( int i = 0; i < osc_count; i++ )
			build_outputs( i );
	}
	else if ( addr == 0xFF25 || addr == status_reg )
	{
		// Remove contributions from the previous route/gain endpoint set.
		for ( int i = 0; i < osc_count; i++ )
			retract_outputs( i, time );

		int mask = (regs [status_reg - start_addr] & 0x80) ? ~0 : 0;

		// left/right assignments
		for ( int i = 0; i < osc_count; i++ )
			oscs [i]->enabled &= mask;

		if ( addr == status_reg && data != old_reg )
		{
			if ( !(data & 0x80) )
			{
				for ( unsigned i = 0; i < sizeof powerup_regs; i++ )
				{
					if ( i != status_reg - start_addr )
						write_register( time, i + start_addr, powerup_regs [i] );
				}
			}
			else
			{
				//debug_printf( "APU powered on\n" );
			}
		}

		// Apply NR51 routing and power state to the endpoints. Power-off
		// register restores above have already rebuilt these once.
		for ( int i = 0; i < osc_count; i++ )
			build_outputs( i );
	}
	else if ( addr >= 0xFF30 )
	{
		int index = (addr & 0x0F) * 2;
		wave.wave [index] = data >> 4;
		wave.wave [index + 1] = data & 0x0F;
	}
}

int Gb_Apu::read_register( blip_time_t time, unsigned addr )
{
	run_until( time );

	int index = addr - start_addr;
	require( (unsigned) index < register_count );
	int data = regs [index];

	if ( addr == status_reg )
	{
		data = (data & 0x80) | 0x70;
		for ( int i = 0; i < osc_count; i++ )
		{
			const Gb_Osc& osc = *oscs [i];
			if ( osc.enabled && (osc.length || !(osc.regs [4] & osc.len_enabled_mask)) )
				data |= 1 << i;
		}
	}

	return data;
}
