// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/

#include "Vgm_Emu.h"

#include <math.h>
#include <string.h>
#include "blargg_endian.h"

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

enum {
	cmd_gg_stereo       = 0x4F,
	cmd_psg             = 0x50,
	cmd_ym2413          = 0x51,
	cmd_ym2612_port0    = 0x52,
	cmd_ym2612_port1    = 0x53,
	cmd_ym2151          = 0x54,
	cmd_delay           = 0x61,
	cmd_delay_735       = 0x62,
	cmd_delay_882       = 0x63,
	cmd_byte_delay      = 0x64,
	cmd_end             = 0x66,
	cmd_data_block      = 0x67,
	cmd_short_delay     = 0x70,
	cmd_pcm_delay       = 0x80,
	cmd_pcm_seek        = 0xE0,

	// VGM 1.71 DAC stream control
	cmd_stream_setup    = 0x90,
	cmd_stream_data     = 0x91,
	cmd_stream_freq     = 0x92,
	cmd_stream_start    = 0x93,
	cmd_stream_stop     = 0x94,
	cmd_stream_fast     = 0x95,

	cmd_gg_stereo_2     = 0x3F,
	cmd_psg_2           = 0x30,
	cmd_ym2413_2        = 0xA1,
	cmd_ym2612_2_port0  = 0xA2,
	cmd_ym2612_2_port1  = 0xA3,

	pcm_block_type      = 0x00,
	ym2612_dac_port     = 0x2A
};

static inline int command_len( int command )
{
	switch ( command >> 4 )
	{
		case 0x03:
		case 0x04:
			return 2;

		case 0x05:
		case 0x0A:
		case 0x0B:
			return 3;

		case 0x0C:
		case 0x0D:
			return 4;

		case 0x0E:
		case 0x0F:
			return 5;
	}

	check( false );
	return 1;
}

template<class Emu>
inline void Ym_Emu<Emu>::begin_frame( short* p )
{
	require( enabled() );
	out = p;
	last_time = 0;
}

template<class Emu>
inline int Ym_Emu<Emu>::run_until( int time )
{
	int count = time - last_time;
	if ( count > 0 )
	{
		if ( last_time < 0 )
			return false;
		last_time = time;
		short* p = out;
		out += count * Emu::out_chan_count;
		Emu::run( count, p );
	}
	return true;
}

inline Vgm_Emu_Impl::fm_time_t Vgm_Emu_Impl::to_fm_time( vgm_time_t t ) const
{
	return (t * fm_time_factor + fm_time_offset) >> fm_time_bits;
}

inline blip_time_t Vgm_Emu_Impl::to_blip_time( vgm_time_t t ) const
{
	return (t * blip_time_factor) >> blip_time_bits;
}

void Vgm_Emu_Impl::write_pcm( vgm_time_t vgm_time, int amp )
{
	blip_time_t blip_time = to_blip_time( vgm_time );
	int old = dac_amp;
	int delta = amp - old;
	dac_amp = amp;
	if ( old >= 0 )
		dac_synth.offset_inline( blip_time, delta, &blip_buf );
	else
		dac_amp |= dac_disabled;
}

// Bounded parameter readers for the stream control commands. On truncation
// they warn, move the cursor to the end and leave the output untouched, so a
// short file ends in a controlled way instead of reading past the buffer.

bool Vgm_Emu_Impl::read_cmd_byte( byte const*& p, int& out )
{
	if ( p >= data_end )
	{
		truncate_stream();
		return false;
	}
	out = *p++;
	return true;
}

bool Vgm_Emu_Impl::read_cmd_u32( byte const*& p, uint32_t& out )
{
	if ( data_end - p < 4 )
	{
		truncate_stream();
		return false;
	}
	out = get_le32( p );
	p += 4;
	return true;
}

void Vgm_Emu_Impl::truncate_stream()
{
	pos = data_end;
	set_warning( "VGM stream: truncated command" );
	stream_ctrl.stop( Vgm_Stream_Ctrl::invalid_id );
	set_track_ended();
}

blip_time_t Vgm_Emu_Impl::run_commands( vgm_time_t end_time )
{
	vgm_time_t vgm_time = this->vgm_time;
	byte const* pos = this->pos;
	if ( pos >= data_end )
	{
		set_track_ended();
		if ( pos > data_end )
			set_warning( "Stream lacked end event" );
	}

	while ( vgm_time < end_time && pos < data_end )
	{
		// Drain every stream value due up to this command's tick before the
		// command itself runs, so a stream write at tick T always precedes a
		// file command at the same tick. Multiple streams due together come
		// out of the controller in ascending stream ID order.
		int64_t abs_tick = stream_base_tick + vgm_time;
		if ( stream_ctrl.tick() < abs_tick )
			stream_ctrl.advance( *this, abs_tick );

		// TODO: be sure there are enough bytes left in stream for particular command
		// so we don't read past end
		switch ( *pos++ )
		{
		case cmd_end:
			pos = loop_begin; // if not looped, loop_begin == data_end
			if ( pos >= data_end )
			{
				// No real loop: the command stream ends here, so no stream
				// may keep emitting past the file's last tick. A genuine VGM
				// loop instead keeps running on each stream's own phase.
				stream_ctrl.stop( Vgm_Stream_Ctrl::invalid_id );
			}
			break;

		case cmd_delay_735:
			vgm_time += 735;
			break;

		case cmd_delay_882:
			vgm_time += 882;
			break;

		case cmd_gg_stereo:
			psg[0].write_ggstereo( to_blip_time( vgm_time ), *pos++ );
			break;

		case cmd_psg:
			psg[0].write_data( to_blip_time( vgm_time ), *pos++ );
			break;

		case cmd_gg_stereo_2:
			psg[1].write_ggstereo( to_blip_time( vgm_time ), *pos++ );
			break;

		case cmd_psg_2:
			psg[1].write_data( to_blip_time( vgm_time ), *pos++ );
			break;

		case cmd_delay:
			vgm_time += pos [1] * 0x100L + pos [0];
			pos += 2;
			break;

		case cmd_byte_delay:
			vgm_time += *pos++;
			break;

		case cmd_ym2413:
			if ( ym2413[0].run_until( to_fm_time( vgm_time ) ) )
				ym2413[0].write( pos [0], pos [1] );
			pos += 2;
			break;

		case cmd_ym2413_2:
			if ( ym2413[1].run_until( to_fm_time( vgm_time ) ) )
				ym2413[1].write( pos [0], pos [1] );
			pos += 2;
			break;

		case cmd_ym2612_port0:
			if ( pos [0] == ym2612_dac_port )
			{
				write_pcm( vgm_time, pos [1] );
			}
			else if ( ym2612[0].run_until( to_fm_time( vgm_time ) ) )
			{
				if ( pos [0] == 0x2B )
				{
					dac_disabled = (pos [1] >> 7 & 1) - 1;
					dac_amp |= dac_disabled;
				}
				ym2612[0].write0( pos [0], pos [1] );
			}
			pos += 2;
			break;

		case cmd_ym2612_port1:
			if ( ym2612[0].run_until( to_fm_time( vgm_time ) ) )
				ym2612[0].write1( pos [0], pos [1] );
			pos += 2;
			break;

		case cmd_ym2612_2_port0:
			if ( pos [0] == ym2612_dac_port )
			{
				write_pcm( vgm_time, pos [1] );
			}
			else if ( ym2612[1].run_until( to_fm_time( vgm_time ) ) )
			{
				if ( pos [0] == 0x2B )
				{
					dac_disabled = (pos [1] >> 7 & 1) - 1;
					dac_amp |= dac_disabled;
				}
				ym2612[1].write0( pos [0], pos [1] );
			}
			pos += 2;
			break;

		case cmd_ym2612_2_port1:
			if ( ym2612[1].run_until( to_fm_time( vgm_time ) ) )
				ym2612[1].write1( pos [0], pos [1] );
			pos += 2;
			break;

		case cmd_data_block: {
			// 67 66 type size(32 LE) data...
			if ( data_end - pos < 6 )
			{
				truncate_stream();
				break;
			}
			check( pos [0] == cmd_end );
			int type = pos [1];
			long size = (long) get_le32( pos + 2 );
			pos += 6;
			if ( size < 0 || data_end - pos < size )
			{
				set_warning( "VGM data block extends past end of file" );
				pos = data_end;
				stream_ctrl.stop( Vgm_Stream_Ctrl::invalid_id );
				set_track_ended();
				break;
			}
			if ( type == pcm_block_type )
			{
				// Type-0 blocks concatenate into the bank in first-seen order.
				// A block revisited by a loop is deduplicated by address.
				if ( stream_ctrl.add_block( pos, size ) )
					set_warning( "Out of memory" );
			}
			pos += size;
			break;
		}

		case cmd_pcm_seek: {
			if ( data_end - pos < 4 )
			{
				truncate_stream();
				break;
			}
			uint32_t off = get_le32( pos );
			pos += 4;
			// Legacy seek addresses the same concatenated bank as the streams;
			// an out-of-bank offset is recorded as a warning by the controller.
			stream_ctrl.legacy_seek( *this, (long) off );
			break;
		}

		// ---- VGM 1.71 DAC stream control ----
		case cmd_stream_setup: {
			// 90 id chipType chipTarget regAddr
			int id, chip, target, reg;
			if ( !read_cmd_byte( pos, id ) || !read_cmd_byte( pos, chip ) ||
					!read_cmd_byte( pos, target ) || !read_cmd_byte( pos, reg ) )
				break;
			stream_ctrl.setup( *this, id, chip, target, reg );
			break;
		}

		case cmd_stream_data: {
			// 91 id bank step base
			int id, bank, step, base;
			if ( !read_cmd_byte( pos, id ) || !read_cmd_byte( pos, bank ) ||
					!read_cmd_byte( pos, step ) || !read_cmd_byte( pos, base ) )
				break;
			stream_ctrl.set_data( *this, id, bank, step, base );
			break;
		}

		case cmd_stream_freq: {
			// 92 id frequency(32 LE)
			int id;
			uint32_t freq;
			if ( !read_cmd_byte( pos, id ) || !read_cmd_u32( pos, freq ) )
				break;
			stream_ctrl.set_frequency( *this, id, freq );
			break;
		}

		case cmd_stream_start: {
			// 93 id offset(32) lengthModeByte length(32)
			int id, mode;
			uint32_t offset, length;
			if ( !read_cmd_byte( pos, id ) || !read_cmd_u32( pos, offset ) ||
					!read_cmd_byte( pos, mode ) || !read_cmd_u32( pos, length ) )
				break;
			stream_ctrl.start( *this, id, offset, mode, length, abs_tick );
			break;
		}

		case cmd_stream_stop: {
			// 94 id (0xFF stops all streams)
			int id;
			if ( !read_cmd_byte( pos, id ) )
				break;
			stream_ctrl.stop( id );
			break;
		}

		case cmd_stream_fast: {
			// 95 id blockId flags
			int id, block_id, flags;
			if ( !read_cmd_byte( pos, id ) || !read_cmd_byte( pos, block_id ) ||
					!read_cmd_byte( pos, flags ) )
				break;
			stream_ctrl.fast_start( *this, id, block_id, flags, abs_tick );
			break;
		}

		default:
			int cmd = pos [-1];
			switch ( cmd & 0xF0 )
			{
				case cmd_pcm_delay: {
					// Legacy single-byte PCM read, bounds-checked against the
					// concatenated bank; its cursor is independent of streams.
					// A failed read is recorded as a warning by the controller.
					byte value = 0;
					if ( stream_ctrl.legacy_read( *this, &value ) )
						write_pcm( vgm_time, value );
					vgm_time += cmd & 0x0F;
					break;
				}

				case cmd_short_delay:
					vgm_time += (cmd & 0x0F) + 1;
					break;

				case 0x50:
					pos += 2;
					break;

				default:
					pos += command_len( cmd ) - 1;
					set_warning( "Unknown stream event" );
			}
		}
	}

	// If the command stream ran out without a loop point, end the streams as
	// well: a well-formed file reaches this through 0x66 (handled above), and
	// a malformed file missing its end event must not leave streams emitting
	// forever. A genuine loop keeps loop_begin inside the file and is excluded.
	if ( pos >= data_end && loop_begin >= data_end )
		stream_ctrl.stop( Vgm_Stream_Ctrl::invalid_id );

	// Drain stream values due inside this frame, i.e. with local tick
	// strictly smaller than end_time. An explicit register write at a tick
	// equal to end_time would likewise be deferred to the next frame's local
	// tick 0 by the loop condition above, so matching that boundary keeps a
	// stream-controlled file sample-identical to its fully expanded twin.
	if ( end_time > 0 )
		stream_ctrl.advance( *this, stream_base_tick + end_time - 1 );

	vgm_time -= end_time;
	this->pos = pos;
	this->vgm_time = vgm_time;
	stream_base_tick += end_time; // next frame's local tick 0 is this instant

	return to_blip_time( end_time );
}

int Vgm_Emu_Impl::play_frame( blip_time_t blip_time, int sample_count, sample_t* buf )
{
	// to do: timing is working mostly by luck

	int min_pairs = sample_count >> 1;
	int vgm_time = ((long) min_pairs << fm_time_bits) / fm_time_factor - 1;
	assert( to_fm_time( vgm_time ) <= min_pairs );
	int pairs = min_pairs;
	while ( (pairs = to_fm_time( vgm_time )) < min_pairs )
		vgm_time++;
	//debug_printf( "pairs: %d, min_pairs: %d\n", pairs, min_pairs );

	if ( ym2612[0].enabled() )
	{
		ym2612[0].begin_frame( buf );
		if ( ym2612[1].enabled() )
			ym2612[1].begin_frame( buf );
		memset( buf, 0, pairs * stereo * sizeof *buf );
	}
	else if ( ym2413[0].enabled() )
	{
		ym2413[0].begin_frame( buf );
		if ( ym2413[1].enabled() )
			ym2413[1].begin_frame( buf );
		memset( buf, 0, pairs * stereo * sizeof *buf );
	}

	run_commands( vgm_time );

	if ( ym2612[0].enabled() )
		ym2612[0].run_until( pairs );
	if ( ym2612[1].enabled() )
		ym2612[1].run_until( pairs );

	if ( ym2413[0].enabled() )
		ym2413[0].run_until( pairs );
	if ( ym2413[1].enabled() )
		ym2413[1].run_until( pairs );

	fm_time_offset = (vgm_time * fm_time_factor + fm_time_offset) -
			((long) pairs << fm_time_bits);

	psg[0].end_frame( blip_time );
	if ( psg_dual )
		psg[1].end_frame( blip_time );

	return pairs * stereo;
}

// Update pre-1.10 header FM rates by scanning commands
void Vgm_Emu_Impl::update_fm_rates( long* ym2413_rate, long* ym2612_rate ) const
{
	byte const* p = data + 0x40;
	while ( p < data_end )
	{
		switch ( *p )
		{
		case cmd_end:
			return;

		case cmd_psg:
		case cmd_byte_delay:
			p += 2;
			break;

		case cmd_delay:
			p += 3;
			break;

		case cmd_data_block:
			p += 7 + get_le32( p + 3 );
			break;

		case cmd_ym2413:
			*ym2612_rate = 0;
			return;

		case cmd_ym2612_port0:
		case cmd_ym2612_port1:
			*ym2612_rate = *ym2413_rate;
			*ym2413_rate = 0;
			return;

		case cmd_ym2151:
			*ym2413_rate = 0;
			*ym2612_rate = 0;
			return;

		default:
			p += command_len( *p );
		}
	}
}
