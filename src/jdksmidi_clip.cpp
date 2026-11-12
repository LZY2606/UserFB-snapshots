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

#include "jdksmidi/world.h"
#include "jdksmidi/clip.h"

#include <stdexcept>
#include <vector>
#include <deque>

namespace jdksmidi
{

namespace
{

// One source event taking part in the merge: ordered by time, then source
// track number, then event number inside the source track.
struct ClipSrcEvent
{
    MIDIClockTime time;
    int track;
    int index;
    const MIDITimedBigMessage *msg;
};

bool ClipSrcEventLess ( const ClipSrcEvent &a, const ClipSrcEvent &b )
{
    if ( a.time != b.time )
        return a.time < b.time;

    if ( a.track != b.track )
        return a.track < b.track;

    return a.index < b.index;
}

// The clip extractor only understands this fixed set of events.
bool IsAllowedClipEvent ( const MIDITimedBigMessage &m )
{
    if ( m.IsServiceMsg() )
        return false;

    if ( m.IsNoteOn() || m.IsNoteOff() || m.IsProgramChange() || m.IsPitchBend() )
        return true;

    if ( m.IsControlChange() )
    {
        switch ( m.GetController() )
        {
        case C_GM_BANK:     // 0, bank select MSB
        case C_LSB:         // 32, bank select LSB
        case C_MAIN_VOLUME: // 7
        case C_PAN:         // 10
        case C_EXPRESSION:  // 11
        case C_DAMPER:      // 64, sustain pedal
            return true;

        default:
            return false;
        }
    }

    if ( m.IsTempo() || m.IsTimeSig() || m.IsTrackName() || m.IsDataEnd() )
        return true;

    return false;
}

struct ClipTone
{
    int bank_msb;
    int bank_lsb;
    int program;
};

bool operator== ( const ClipTone &a, const ClipTone &b )
{
    return a.bank_msb == b.bank_msb && a.bank_lsb == b.bank_lsb && a.program == b.program;
}

bool operator!= ( const ClipTone &a, const ClipTone &b )
{
    return ! ( a == b );
}

// One still sounding note instance.
struct ClipNote
{
    int channel;
    int note;
    int velocity;
    ClipTone tone; // bank/program in effect when the note was struck
    bool held;     // false: key released, kept alive by the sustain pedal only
};

struct ClipChannel
{
    ClipChannel()
        : pend_msb ( 0 ), pend_lsb ( 0 ),
          volume ( 100 ), pan ( 64 ), expression ( 127 ),
          bend ( 0 ), damper ( 0 )
    {
        tone.bank_msb = 0;
        tone.bank_lsb = 0;
        tone.program = 0;
    }

    int pend_msb;   // bank select MSB received, not necessarily committed yet
    int pend_lsb;   // bank select LSB received, not necessarily committed yet
    ClipTone tone;  // committed bank/program, used by the following note-ons
    int volume;
    int pan;
    int expression;
    int bend;
    int damper;
};

struct ClipState
{
    ClipState()
        : tempo ( 500000 ), ts_num ( 4 ), ts_den_pow ( 2 ), ts_clocks ( 24 ), ts_32nd ( 8 )
    {
    }

    ClipChannel ch[16];
    std::deque<ClipNote> notes; // sounding notes, in note-on order
    unsigned long tempo;        // microseconds per quarter note
    int ts_num;
    int ts_den_pow;
    int ts_clocks;
    int ts_32nd;
};

void ClipApplyEvent ( ClipState &st, const MIDITimedBigMessage &m )
{
    if ( m.IsNoteOn() && m.GetVelocity() > 0 )
    {
        ClipNote n;
        n.channel = m.GetChannel();
        n.note = m.GetNote();
        n.velocity = m.GetVelocity();
        n.tone = st.ch[n.channel].tone;
        n.held = true;
        st.notes.push_back ( n );
    }
    else if ( m.IsNoteOff() || ( m.IsNoteOn() && m.GetVelocity() == 0 ) )
    {
        // release the earliest still held note with the same channel and pitch
        int chn = m.GetChannel();
        int note = m.GetNote();

        for ( std::deque<ClipNote>::iterator i = st.notes.begin(); i != st.notes.end(); ++i )
        {
            if ( i->held && i->channel == chn && i->note == note )
            {
                if ( st.ch[chn].damper >= 64 )
                    i->held = false; // sustained by the pedal
                else
                    st.notes.erase ( i );

                break;
            }
        }
    }
    else if ( m.IsControlChange() )
    {
        ClipChannel &c = st.ch[m.GetChannel()];

        switch ( m.GetController() )
        {
        case C_GM_BANK:
            c.pend_msb = m.GetControllerValue();
            break;

        case C_LSB:
            c.pend_lsb = m.GetControllerValue();
            break;

        case C_MAIN_VOLUME:
            c.volume = m.GetControllerValue();
            break;

        case C_PAN:
            c.pan = m.GetControllerValue();
            break;

        case C_EXPRESSION:
            c.expression = m.GetControllerValue();
            break;

        case C_DAMPER:
            c.damper = m.GetControllerValue();

            if ( c.damper < 64 )
            {
                // pedal up: notes that were only sustained by the pedal are gone
                int chn = m.GetChannel();

                for ( std::deque<ClipNote>::iterator i = st.notes.begin(); i != st.notes.end(); )
                {
                    if ( !i->held && i->channel == chn )
                        i = st.notes.erase ( i );
                    else
                        ++i;
                }
            }

            break;

        default:
            break;
        }
    }
    else if ( m.IsProgramChange() )
    {
        // the pending bank select is committed together with the program
        ClipChannel &c = st.ch[m.GetChannel()];
        c.tone.bank_msb = c.pend_msb;
        c.tone.bank_lsb = c.pend_lsb;
        c.tone.program = m.GetPGValue();
    }
    else if ( m.IsPitchBend() )
    {
        st.ch[m.GetChannel()].bend = m.GetBenderValue();
    }
    else if ( m.IsTempo() )
    {
        st.tempo = m.GetTempo();
    }
    else if ( m.IsTimeSig() )
    {
        st.ts_num = m.GetTimeSigNumerator();
        st.ts_den_pow = m.GetTimeSigDenominatorPower();
        st.ts_clocks = m.GetByte5();
        st.ts_32nd = m.GetByte6();
    }
}

void ClipPutCC ( MIDITrack *t, MIDIClockTime time, int chn, int ctrl, int val )
{
    MIDITimedBigMessage m;
    m.SetControlChange ( ( unsigned char ) chn, ( unsigned char ) ctrl, ( unsigned char ) val );
    m.SetTime ( time );
    t->PutEvent ( m );
}

void ClipPutPC ( MIDITrack *t, MIDIClockTime time, int chn, int program )
{
    MIDITimedBigMessage m;
    m.SetProgramChange ( ( unsigned char ) chn, ( unsigned char ) program );
    m.SetTime ( time );
    t->PutEvent ( m );
}

} // namespace

std::unique_ptr<MIDIMultiTrack> ExtractPlayableClip (
    const MIDIMultiTrack &src,
    MIDIClockTime begin,
    MIDIClockTime end )
{
    if ( begin >= end )
        throw std::invalid_argument ( "ExtractPlayableClip: begin >= end" );

    const int clks_per_beat = src.GetClksPerBeat();

    if ( clks_per_beat <= 0 || ( clks_per_beat & 0x8000 ) )
        throw std::invalid_argument ( "ExtractPlayableClip: SMPTE or invalid time base" );

    // gather and validate all source events
    std::vector<ClipSrcEvent> events;

    for ( int t = 0; t < src.GetNumTracks(); ++t )
    {
        const MIDITrack *trk = src.GetTrack ( t );

        if ( !trk )
            continue;

        if ( !trk->EventsOrderOK() )
            throw std::invalid_argument ( "ExtractPlayableClip: source track events out of order" );

        for ( int i = 0; i < trk->GetNumEvents(); ++i )
        {
            const MIDITimedBigMessage *m = trk->GetEventAddress ( i );

            if ( !m )
                continue;

            if ( !IsAllowedClipEvent ( *m ) )
                throw std::invalid_argument ( "ExtractPlayableClip: unsupported event type" );

            ClipSrcEvent e;
            e.time = m->GetTime();
            e.track = t;
            e.index = i;
            e.msg = m;
            events.push_back ( e );
        }
    }

    std::sort ( events.begin(), events.end(), ClipSrcEventLess );

    // run the source material up to begin, then up to end
    ClipState state;
    std::size_t pos = 0;

    while ( pos < events.size() && events[pos].time < begin )
        ClipApplyEvent ( state, *events[pos++].msg );

    const ClipState begin_state = state;
    const std::size_t in_begin = pos;

    while ( pos < events.size() && events[pos].time < end )
        ClipApplyEvent ( state, *events[pos++].msg );

    const std::size_t in_end = pos;
    const ClipState end_state = state;

    // build the result
    std::unique_ptr<MIDIMultiTrack> out ( new MIDIMultiTrack ( 2 ) );
    out->SetClksPerBeat ( clks_per_beat );
    MIDITrack *trk0 = out->GetTrack ( 0 );
    MIDITrack *trk1 = out->GetTrack ( 1 );
    const MIDIClockTime length = end - begin;

    // track 0: tempo and time signature in effect at begin, then the
    // tempo/time signature changes inside the interval
    {
        MIDITimedBigMessage m;
        m.SetTempo ( begin_state.tempo );
        m.SetTime ( 0 );
        trk0->PutEvent ( m );
        m.SetTimeSig (
            ( unsigned char ) begin_state.ts_num,
            ( unsigned char ) begin_state.ts_den_pow,
            ( unsigned char ) begin_state.ts_clocks,
            ( unsigned char ) begin_state.ts_32nd );
        m.SetTime ( 0 );
        trk0->PutEvent ( m );
    }

    for ( std::size_t i = in_begin; i < in_end; ++i )
    {
        const MIDITimedBigMessage &m = *events[i].msg;

        if ( m.IsTempo() || m.IsTimeSig() )
        {
            MIDITimedBigMessage c ( m );
            c.SetTime ( m.GetTime() - begin );
            trk0->PutEvent ( c );
        }
    }

    // track 1: startup events at tick 0; outch mirrors the channel settings
    // of the fresh synthesizer as our own events modify them
    ClipChannel outch[16];

    // sustain pedal down first, so re-released notes below stay sustained
    for ( int chn = 0; chn < 16; ++chn )
    {
        if ( begin_state.ch[chn].damper >= 64 )
        {
            ClipPutCC ( trk1, 0, chn, C_DAMPER, begin_state.ch[chn].damper );
            outch[chn].damper = begin_state.ch[chn].damper;
        }
    }

    // rebuild every sounding note in its original note-on order, with the
    // bank/program/velocity captured when the note was struck
    for ( std::deque<ClipNote>::const_iterator i = begin_state.notes.begin(); i != begin_state.notes.end(); ++i )
    {
        ClipChannel &c = outch[i->channel];
        ClipPutCC ( trk1, 0, i->channel, C_GM_BANK, i->tone.bank_msb );
        c.pend_msb = i->tone.bank_msb;
        ClipPutCC ( trk1, 0, i->channel, C_LSB, i->tone.bank_lsb );
        c.pend_lsb = i->tone.bank_lsb;
        ClipPutPC ( trk1, 0, i->channel, i->tone.program );
        c.tone = i->tone;

        MIDITimedBigMessage m;
        m.SetNoteOn ( ( unsigned char ) i->channel, ( unsigned char ) i->note, ( unsigned char ) i->velocity );
        m.SetTime ( 0 );
        trk1->PutEvent ( m );
    }

    // notes kept alive by the sustain pedal only must get back their
    // already-released identity, in their original release order
    for ( std::deque<ClipNote>::const_iterator i = begin_state.notes.begin(); i != begin_state.notes.end(); ++i )
    {
        if ( !i->held )
        {
            MIDITimedBigMessage m;
            m.SetNoteOff ( ( unsigned char ) i->channel, ( unsigned char ) i->note, 0 );
            m.SetTime ( 0 );
            trk1->PutEvent ( m );
        }
    }

    // restore the channel settings in effect at begin, so notes struck
    // inside the interval get the current (not the rebuilt) sound
    for ( int chn = 0; chn < 16; ++chn )
    {
        const ClipChannel &want = begin_state.ch[chn];
        ClipChannel &cur = outch[chn];

        if ( cur.tone != want.tone )
        {
            ClipPutCC ( trk1, 0, chn, C_GM_BANK, want.tone.bank_msb );
            ClipPutCC ( trk1, 0, chn, C_LSB, want.tone.bank_lsb );
            ClipPutPC ( trk1, 0, chn, want.tone.program );
            cur.tone = want.tone;
            cur.pend_msb = want.tone.bank_msb;
            cur.pend_lsb = want.tone.bank_lsb;
        }

        if ( cur.pend_msb != want.pend_msb )
        {
            ClipPutCC ( trk1, 0, chn, C_GM_BANK, want.pend_msb );
            cur.pend_msb = want.pend_msb;
        }

        if ( cur.pend_lsb != want.pend_lsb )
        {
            ClipPutCC ( trk1, 0, chn, C_LSB, want.pend_lsb );
            cur.pend_lsb = want.pend_lsb;
        }

        if ( cur.volume != want.volume )
        {
            ClipPutCC ( trk1, 0, chn, C_MAIN_VOLUME, want.volume );
            cur.volume = want.volume;
        }

        if ( cur.pan != want.pan )
        {
            ClipPutCC ( trk1, 0, chn, C_PAN, want.pan );
            cur.pan = want.pan;
        }

        if ( cur.expression != want.expression )
        {
            ClipPutCC ( trk1, 0, chn, C_EXPRESSION, want.expression );
            cur.expression = want.expression;
        }

        if ( cur.bend != want.bend )
        {
            MIDITimedBigMessage m;
            m.SetPitchBend ( ( unsigned char ) chn, ( short ) want.bend );
            m.SetTime ( 0 );
            trk1->PutEvent ( m );
            cur.bend = want.bend;
        }
    }

    // the channel events of the interval, merged in the original order
    for ( std::size_t i = in_begin; i < in_end; ++i )
    {
        const MIDITimedBigMessage &m = *events[i].msg;

        if ( m.IsChannelMsg() )
        {
            MIDITimedBigMessage c ( m );
            c.SetTime ( m.GetTime() - begin );
            trk1->PutEvent ( c );
        }
    }

    // end of the clip: close the sustain pedals first, then note-off every
    // note still held down, so nothing keeps sounding after the clip
    for ( int chn = 0; chn < 16; ++chn )
    {
        if ( end_state.ch[chn].damper >= 64 )
            ClipPutCC ( trk1, length, chn, C_DAMPER, 0 );
    }

    for ( std::deque<ClipNote>::const_iterator i = end_state.notes.begin(); i != end_state.notes.end(); ++i )
    {
        if ( i->held )
        {
            MIDITimedBigMessage m;
            m.SetNoteOff ( ( unsigned char ) i->channel, ( unsigned char ) i->note, 0 );
            m.SetTime ( length );
            trk1->PutEvent ( m );
        }
    }

    // both tracks end with a proper EndOfTrack at end-begin
    MIDITimedBigMessage eot;
    eot.SetEndOfTrack();
    eot.SetTime ( length );
    trk0->PutEvent ( eot );
    trk1->PutEvent ( eot );

    return out;
}

}
