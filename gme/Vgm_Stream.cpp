// VGM 1.71 DAC stream control (commands 0x90-0x95)
// Specification: https://vgmrips.net/wiki/VGM_Specification
//
// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/

#include "Vgm_Stream.h"

#include "Vgm_Emu_Impl.h"
#include <string.h>
#include <limits.h>

/* Copyright (C) 2024. This module is free software; you can redistribute it
and/or modify it under the terms of the GNU Lesser General Public License as
published by the Free Software Foundation; either version 2.1 of the License,
or (at your option) any later version. */

#include "blargg_source.h"

// Supported target: first YM2612, port 0, DAC register 0x2A, uncompressed
// bank 0. The YM2612 DAC stream setup token is encoded with chip type 0x00 or
// 0x02 by different VGM 1.71 writers; both select the same port-0 0x2A path
// used by explicit 0x52 DAC writes, so both are accepted.
enum {
	supported_chip_a  = 0x00,
	supported_chip_b  = 0x02,
	supported_target  = 0x00,
	supported_reg     = 0x2A,
	supported_bank    = 0x00
};

void Vgm_Stream_Ctrl::Stream::clear()
{
	memset( this, 0, sizeof *this );
}

void Vgm_Stream_Ctrl::reset()
{
	for ( int i = 0; i < stream_count; ++i )
		stream [i].clear();
	blocks.clear();
	block_count_   = 0;
	bank_bytes     = 0;
	sched_tick     = 0;
	legacy_pos     = 0;
	legacy_warned  = false;
}

blargg_err_t Vgm_Stream_Ctrl::add_block( byte const* phys, long size )
{
	require( phys && size >= 0 );

	// A physical block revisited through a VGM loop is not appended again.
	for ( int i = 0; i < block_count_; ++i )
		if ( blocks [i].phys == phys )
			return 0;

	// Keep every bank offset inside the range the position arithmetic allows.
	if ( size > INT_MAX - bank_bytes )
		return "VGM data bank too large";

	RETURN_ERR( blocks.resize( block_count_ + 1 ) );
	blocks [block_count_].phys  = phys;
	blocks [block_count_].start = bank_bytes;
	blocks [block_count_].size  = size;
	++block_count_;
	bank_bytes += size;
	return 0;
}

const Vgm_Stream_Ctrl::Block* Vgm_Stream_Ctrl::find_block( long offset ) const
{
	for ( int i = 0; i < block_count_; ++i )
	{
		Block const& b = blocks [i];
		if ( offset >= b.start && offset < b.start + b.size )
			return &b;
	}
	return 0;
}

bool Vgm_Stream_Ctrl::read_bank( long offset, byte* out ) const
{
	if ( offset < 0 || offset >= bank_bytes )
		return false;
	Block const* b = find_block( offset );
	if ( !b )
		return false;
	*out = b->phys [offset - b->start];
	return true;
}

void Vgm_Stream_Ctrl::fail( Vgm_Emu_Impl& owner, int id, const char* what )
{
	owner.set_warning( what );
	if ( id >= 0 && id < stream_count )
		stream [id].active = false;
}

bool Vgm_Stream_Ctrl::config_usable( Stream const& s ) const
{
	return s.configured && s.bank_id == supported_bank && s.step > 0 &&
			s.base < s.step && s.frequency >= 1 &&
			(unsigned) s.frequency <= ticks_per_sec;
}

void Vgm_Stream_Ctrl::setup( Vgm_Emu_Impl& owner, int id,
		int chip_type, int target, int reg )
{
	if ( id < 0 || id >= stream_count )
	{
		// 0xFF is only special for 0x94; everywhere else it is ignored.
		owner.set_warning( "VGM stream: reserved stream ID ignored" );
		return;
	}

	Stream& s = stream [id];
	s.clear(); // reconfiguration stops the stream first

	bool ym2612_dac = ( chip_type == supported_chip_a ||
			chip_type == supported_chip_b ) &&
			target == supported_target && reg == supported_reg;
	if ( !ym2612_dac )
	{
		fail( owner, id, "VGM stream: unsupported chip, port or register" );
		return;
	}

	s.configured = true;
	s.bank_id    = supported_bank;
	s.step       = 1;
	s.base       = 0;
}

void Vgm_Stream_Ctrl::set_data( Vgm_Emu_Impl& owner, int id,
		int bank_id, int step, int base )
{
	if ( id < 0 || id >= stream_count )
	{
		owner.set_warning( "VGM stream: reserved stream ID ignored" );
		return;
	}
	Stream& s = stream [id];
	if ( !s.configured )
	{
		fail( owner, id, "VGM stream: data setup before stream setup" );
		return;
	}

	s.active = false; // reconfiguration stops the stream first
	if ( bank_id != supported_bank )
	{
		fail( owner, id, "VGM stream: unsupported data bank" );
		return;
	}
	if ( step == 0 )
	{
		fail( owner, id, "VGM stream: step must be non-zero" );
		return;
	}
	if ( base >= step )
	{
		fail( owner, id, "VGM stream: base must be smaller than step" );
		return;
	}
	s.bank_id = (uint8_t) bank_id;
	s.step    = (uint16_t) step;
	s.base    = (uint16_t) base;
}

void Vgm_Stream_Ctrl::set_frequency( Vgm_Emu_Impl& owner, int id,
		uint32_t frequency )
{
	if ( id < 0 || id >= stream_count )
	{
		owner.set_warning( "VGM stream: reserved stream ID ignored" );
		return;
	}
	Stream& s = stream [id];
	if ( !s.configured )
	{
		fail( owner, id, "VGM stream: frequency set before stream setup" );
		return;
	}
	if ( frequency < 1 || frequency > (unsigned) ticks_per_sec )
	{
		fail( owner, id, "VGM stream: frequency out of range" );
		return;
	}
	// Position and phase remainder are preserved; the new frequency applies
	// from this tick onward.
	s.frequency = frequency;
}

void Vgm_Stream_Ctrl::begin_playback( Vgm_Emu_Impl& owner, Stream& s, int id,
		int64_t start_pos, uint32_t total, bool loop, int64_t tick )
{
	if ( !config_usable( s ) )
	{
		fail( owner, id, "VGM stream: start with invalid configuration" );
		return;
	}
	if ( total == 0 )
	{
		// Zero writes: the range is valid but produces no sound.
		s.active = false;
		return;
	}

	// Overflow-checked extent of the whole fixed pass. The bank itself can
	// grow while playback encounters more 0x67 blocks, so an offset is only
	// rejected here on arithmetic overflow; out-of-bank reads are caught at
	// emit time (warning plus stopping just this stream).
	int64_t last_pos = start_pos + (int64_t) (total - 1) * s.step;
	if ( start_pos < 0 || start_pos > INT_MAX || last_pos > INT_MAX ||
			last_pos < start_pos )
	{
		fail( owner, id, "VGM stream: range overflow" );
		return;
	}

	s.start_pos    = (long) start_pos;
	s.cur_pos      = (long) start_pos;
	s.writes_total = total;
	s.writes_left  = total;
	s.loop_flag    = loop;
	s.phase        = 0;
	s.active       = true;

	// The first value is written at the start tick immediately.
	byte value;
	if ( !read_bank( s.cur_pos, &value ) )
	{
		fail( owner, id, "VGM stream: data block not available" );
		return;
	}
	owner.stream_write_pcm( tick, value );
	--s.writes_left;
	if ( s.writes_left == 0 )
	{
		if ( s.loop_flag )
		{
			s.cur_pos     = s.start_pos;
			s.writes_left = s.writes_total;
		}
		else
		{
			s.active = false;
		}
	}
	else
	{
		s.cur_pos += s.step;
	}
}

void Vgm_Stream_Ctrl::start( Vgm_Emu_Impl& owner, int id,
		uint32_t bank_offset, int flags, uint32_t length, int64_t tick )
{
	if ( id < 0 || id >= stream_count )
	{
		owner.set_warning( "VGM stream: reserved stream ID ignored" );
		return;
	}
	Stream& s = stream [id];
	if ( !s.configured )
	{
		fail( owner, id, "VGM stream: start before stream setup" );
		return;
	}

	// A start replaces any previous playback range and clears the phase.
	s.active = false;
	s.phase  = 0;

	if ( bank_offset == 0xFFFFFFFFu )
	{
		fail( owner, id, "VGM stream: 0xFFFFFFFF offset is not supported" );
		return;
	}
	if ( (flags & 0x03) != 1 )
	{
		fail( owner, id, "VGM stream: only write-count length mode is supported" );
		return;
	}

	int64_t start_pos = (int64_t) bank_offset + s.base;
	begin_playback( owner, s, id, start_pos, length, (flags & 0x80) != 0, tick );
}

void Vgm_Stream_Ctrl::fast_start( Vgm_Emu_Impl& owner, int id,
		int block_id, int flags, int64_t tick )
{
	if ( id < 0 || id >= stream_count )
	{
		owner.set_warning( "VGM stream: reserved stream ID ignored" );
		return;
	}
	Stream& s = stream [id];
	if ( !s.configured )
	{
		fail( owner, id, "VGM stream: fast start before stream setup" );
		return;
	}

	s.active = false;
	s.phase  = 0;

	if ( block_id < 0 || block_id >= block_count_ )
	{
		fail( owner, id, "VGM stream: invalid block number" );
		return;
	}
	if ( !config_usable( s ) )
	{
		fail( owner, id, "VGM stream: fast start with invalid configuration" );
		return;
	}

	Block const& b = blocks [block_id];
	if ( (long) s.base >= b.size )
	{
		fail( owner, id, "VGM stream: fast start base past block end" );
		return;
	}

	// All values, including the last one, stay inside the named block.
	long remain = b.size - s.base;
	uint32_t total = (uint32_t) ((remain + s.step - 1) / s.step);
	int64_t start_pos = (int64_t) b.start + s.base;
	begin_playback( owner, s, id, start_pos, total, (flags & 0x01) != 0, tick );
}

void Vgm_Stream_Ctrl::stop( int id )
{
	if ( id == invalid_id )
	{
		for ( int i = 0; i < stream_count; ++i )
			stream [i].active = false;
		return;
	}
	if ( id >= 0 && id < stream_count )
		stream [id].active = false;
}

void Vgm_Stream_Ctrl::advance( Vgm_Emu_Impl& owner, int64_t target )
{
	require( target >= sched_tick );
	if ( target == sched_tick )
		return;

	// With nothing playing there is no phase to advance; align the clock in
	// one step instead of ticking through silence.
	bool any = false;
	for ( int i = 0; i < stream_count; ++i )
	{
		if ( stream [i].active )
		{
			any = true;
			break;
		}
	}
	if ( !any )
	{
		sched_tick = target;
		return;
	}

	// Pump ticks (sched_tick, target] one at a time; within a tick streams
	// are handled in ascending ID order.
	while ( sched_tick < target )
	{
		++sched_tick;
		bool still_active = false;
		for ( int id = 0; id < stream_count; ++id )
		{
			Stream& s = stream [id];
			if ( !s.active )
				continue;

			s.phase += s.frequency;
			if ( s.phase < (uint32_t) ticks_per_sec )
			{
				still_active = true;
				continue;
			}
			s.phase -= (uint32_t) ticks_per_sec;

			byte value;
			if ( !read_bank( s.cur_pos, &value ) )
			{
				fail( owner, id, "VGM stream: read outside data bank" );
				continue;
			}
			owner.stream_write_pcm( sched_tick, value );

			--s.writes_left;
			if ( s.writes_left == 0 )
			{
				if ( s.loop_flag )
				{
					// Return to the start of this pass. The next value still
					// waits for the following sample period.
					s.cur_pos     = s.start_pos;
					s.writes_left = s.writes_total;
				}
				else
				{
					s.active = false;
					continue;
				}
			}
			else
			{
				s.cur_pos += s.step;
			}
			still_active = true;
		}

		// Every stream has stopped: nothing left to schedule, so align the
		// clock to the frame boundary in one step instead of ticking through
		// silence.
		if ( !still_active )
		{
			sched_tick = target;
			break;
		}
	}
}

bool Vgm_Stream_Ctrl::legacy_seek( Vgm_Emu_Impl& owner, long offset )
{
	if ( offset < 0 || offset >= bank_bytes )
	{
		legacy_pos = bank_bytes;
		if ( !legacy_warned )
		{
			legacy_warned = true;
			owner.set_warning( "VGM PCM seek outside data bank" );
		}
		return false;
	}
	legacy_pos = offset;
	return true;
}

bool Vgm_Stream_Ctrl::legacy_read( Vgm_Emu_Impl& owner, byte* out )
{
	if ( legacy_pos < 0 || legacy_pos >= bank_bytes ||
			!read_bank( legacy_pos, out ) )
	{
		if ( !legacy_warned )
		{
			legacy_warned = true;
			owner.set_warning( "VGM PCM read outside data bank" );
		}
		return false;
	}
	++legacy_pos;
	return true;
}
