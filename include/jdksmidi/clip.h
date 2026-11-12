/*
 *  libjdksmidi-2004 C++ Class Library for MIDI
 *
 *  Copyright (C) 2004  J.D. Koftinoff Software, Ltd.
 *  www.jdkoftinoff.com
 *  jeffk@jdkoftinoff.com
 *
 *  *** RELEASED UNDER THE GNU GENERAL PUBLIC LICENSE (GPL) April 27, 2004 ***
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef JDKSMIDI_CLIP_H
#define JDKSMIDI_CLIP_H

#include "jdksmidi/multitrack.h"

#include <memory>

namespace jdksmidi
{

///
/// ExtractPlayableClip() cuts the half-open tick interval [begin,end) out of a
/// multi-track MIDI sequence and returns it as a new, self contained, directly
/// playable two track MIDIMultiTrack with the same clocks-per-beat value.
///
/// The source is not modified. The result is laid out as:
///
///  - track 0: the tempo and time signature in effect at @a begin, placed at
///    tick 0 (defaults are 120 bpm and 4/4), followed by the tempo/time
///    signature changes found inside the interval, shifted by -begin. It ends
///    with an EndOfTrack meta event at tick end-begin.
///
///  - track 1: startup events at tick 0 that make a freshly reset synthesizer
///    reproduce every note that is still sounding at @a begin (with the bank
///    select/program/velocity captured at each original note-on, and with the
///    released-but-sustained identity of notes only kept alive by the sustain
///    pedal), followed by the current bank/program/volume/pan/expression/pitch
///    bend settings in effect at @a begin, followed by all channel events of
///    the interval shifted by -begin (merged in original order: time, then
///    source track number, then source event number). At tick end-begin the
///    sustain pedals are released first, then every note still held down gets
///    a note-off, and the track ends with an EndOfTrack meta event, so no
///    hanging notes remain.
///
/// Only NoteOn/NoteOff, ProgramChange, PitchBend, CC 0/32/7/10/11/64 and the
/// tempo, time signature, track name and end-of-track meta events are
/// supported. std::invalid_argument is thrown (and nothing is returned) for
/// any other channel message, system exclusive data, a SMPTE time base, a
/// physically unsorted source track, or begin >= end.
///

std::unique_ptr<MIDIMultiTrack> ExtractPlayableClip (
    const MIDIMultiTrack &src,
    MIDIClockTime begin,
    MIDIClockTime end );

}

#endif
