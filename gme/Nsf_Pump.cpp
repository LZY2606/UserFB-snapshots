// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/

#include "Nsf_Pump.h"

#include "Nsf_Emu.h"
#include "Data_Reader.h"

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

using std::min;

Nsf_Pump::Nsf_Pump()
{
	emu_ = 0;
	frame_active = false;
	total_instructions = 0;
	total_frames = 0;
	total_clocks = 0;
}

Nsf_Pump::~Nsf_Pump()
{
	delete emu_;
}

void Nsf_Pump::reset()
{
	delete emu_;
	emu_ = 0;
	frame_active = false;
	total_instructions = 0;
	total_frames = 0;
	total_clocks = 0;
}

blargg_err_t Nsf_Pump::begin( const void* data, long size, long rate, int track )
{
	if ( !data || size <= Nsf_Emu::header_size )
		return "NSF file is missing or truncated";

	if ( rate != 44100 && rate != 48000 )
		return "Sample rate must be 44100 or 48000";

	const unsigned char* h = STATIC_CAST( const unsigned char*, data );
	if ( memcmp( h, "NESM\x1A", 5 ) )
		return "Not an NSF file";
	if ( h [5] != 1 )
		return "Only NSF version 1 files are supported";
	if ( h [0x7B] )
		return "NSF expansion audio is not supported";
	if ( track < 0 || track >= h [6] )
		return "Track number out of range";

	// Load into a fresh emulator so a failure keeps the previous playback
	Nsf_Emu* fresh = BLARGG_NEW Nsf_Emu;
	CHECK_ALLOC( fresh );

	blargg_err_t err = fresh->set_sample_rate( rate );
	if ( !err )
	{
		// fixed configuration: tempo 1.0, default equalizer, no fade,
		// no automatic silence trimming
		fresh->ignore_silence( true );
		Mem_File_Reader in( data, size );
		err = fresh->load( in );
	}
	if ( !err )
		err = fresh->start_track( track );
	if ( err )
	{
		delete fresh;
		return err;
	}

	reset();
	emu_ = fresh;
	return 0;
}

blargg_err_t Nsf_Pump::pump( short* out, long capacity_frames, unsigned budget, Result* result )
{
	if ( !result )
		return "Result pointer must not be NULL";

	result->work = 0;
	result->instructions = 0;
	result->frames = 0;

	if ( !emu_ )
		return "begin() has not been called successfully";
	if ( capacity_frames < 0 )
		return "Frame capacity must not be negative";
	if ( capacity_frames > 0 && !out )
		return "Output pointer must not be NULL";
	if ( capacity_frames == 0 || budget == 0 )
		return 0;

	long remaining = budget;
	long capacity_left = capacity_frames;
	short* out_pos = out;

	while ( remaining > 0 && capacity_left > 0 )
	{
		// hand out any samples left over from a completed internal frame
		long avail = emu_->samples_avail() / 2;
		if ( avail > 0 )
		{
			long n = min( min( remaining, capacity_left ), avail );
			long got = emu_->read_samples( out_pos, n * 2 ) / 2;
			if ( got <= 0 )
				break;
			out_pos += got * 2;
			capacity_left -= got;
			remaining -= got;
			result->frames += got;
			total_frames += got;
			continue;
		}

		if ( !frame_active )
		{
			emu_->frame_begin( emu_->frame_length() );
			frame_active = true;
		}

		nes_time_t start_time = emu_->frame_time();
		bool done = false;
		long ran = emu_->frame_step( remaining, &done );
		total_clocks += emu_->frame_time() - start_time;
		remaining -= ran;
		result->instructions += ran;
		total_instructions += ran;

		if ( done )
		{
			blip_time_t clocks = emu_->frame_end();
			emu_->end_frame_samples( clocks );
			frame_active = false;
		}
		else if ( ran == 0 )
		{
			break; // budget ran out exactly on an idle stretch
		}
	}

	result->work = result->instructions + STATIC_CAST( unsigned, result->frames );
	return 0;
}

Nsf_Pump::Progress Nsf_Pump::progress() const
{
	Progress p;
	p.instructions = total_instructions;
	p.delivered_frames = total_frames;
	p.emulated_clocks = total_clocks;
	return p;
}
