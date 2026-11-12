// checks/playable_clip.cpp
//
// Verification for jdksmidi::ExtractPlayableClip().
//
// Every scenario is checked twice: once on the in-memory clip and once on
// the clip after a Standard MIDI File write/read roundtrip through the
// existing MIDIFileWriteMultiTrack / MIDIFileReadMultiTrack classes.
//
// The check compares two independent simplified synthesizer state models:
//   a) the source sequence played from the head up to each event inside
//      the clip interval,
//   b) the extracted clip played from an empty (freshly reset) state.
// Compared: tone identity (bank MSB/LSB/program) and velocity of every
// sounding note instance, the key-held / pedal-held note sets, and the
// current per-channel settings (pending bank, committed bank/program,
// volume, pan, expression, pitch bend, sustain pedal).

#include "jdksmidi/world.h"
#include "jdksmidi/multitrack.h"
#include "jdksmidi/clip.h"
#include "jdksmidi/filewritemultitrack.h"
#include "jdksmidi/filereadmultitrack.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

using namespace jdksmidi;

static const char *g_case_name = "";

#define CHECK(cond, msg)                                                        \
    do                                                                          \
    {                                                                           \
        if ( !(cond) )                                                          \
        {                                                                       \
            std::fprintf ( stderr, "FAIL [%s] %s:%d: %s\n",                     \
                           g_case_name, __FILE__, __LINE__, msg );              \
            std::exit ( 1 );                                                    \
        }                                                                       \
    }                                                                           \
    while ( 0 )

//
// Independent simplified synthesizer state model.
//

struct Tone
{
    int msb, lsb, prog;
};

static bool operator== ( const Tone &a, const Tone &b )
{
    return a.msb == b.msb && a.lsb == b.lsb && a.prog == b.prog;
}

struct Note
{
    int ch, key, vel;
    Tone tone;
    bool held; // false: released, kept alive by the sustain pedal only
};

static bool operator== ( const Note &a, const Note &b )
{
    return a.ch == b.ch && a.key == b.key && a.vel == b.vel &&
           a.tone == b.tone && a.held == b.held;
}

struct Chan
{
    Chan()
        : pend_msb ( 0 ), pend_lsb ( 0 ), vol ( 100 ), pan ( 64 ),
          expr ( 127 ), bend ( 0 ), damper ( 0 )
    {
        tone.msb = 0;
        tone.lsb = 0;
        tone.prog = 0;
    }

    int pend_msb, pend_lsb;
    Tone tone;
    int vol, pan, expr, bend, damper;
};

static bool operator== ( const Chan &a, const Chan &b )
{
    return a.pend_msb == b.pend_msb && a.pend_lsb == b.pend_lsb &&
           a.tone == b.tone && a.vol == b.vol && a.pan == b.pan &&
           a.expr == b.expr && a.bend == b.bend && a.damper == b.damper;
}

struct Synth
{
    Chan ch[16];
    std::deque<Note> notes; // sounding notes, in note-on order

    void apply ( const MIDITimedBigMessage &m )
    {
        if ( m.IsNoteOn() && m.GetVelocity() > 0 )
        {
            Note n;
            n.ch = m.GetChannel();
            n.key = m.GetNote();
            n.vel = m.GetVelocity();
            n.tone = ch[n.ch].tone;
            n.held = true;
            notes.push_back ( n );
        }
        else if ( m.IsNoteOff() || ( m.IsNoteOn() && m.GetVelocity() == 0 ) )
        {
            int c = m.GetChannel();
            int k = m.GetNote();

            for ( std::deque<Note>::iterator i = notes.begin(); i != notes.end(); ++i )
            {
                if ( i->held && i->ch == c && i->key == k )
                {
                    if ( ch[c].damper >= 64 )
                        i->held = false;
                    else
                        notes.erase ( i );

                    break;
                }
            }
        }
        else if ( m.IsControlChange() )
        {
            Chan &c = ch[m.GetChannel()];

            switch ( m.GetController() )
            {
            case 0:
                c.pend_msb = m.GetControllerValue();
                break;

            case 32:
                c.pend_lsb = m.GetControllerValue();
                break;

            case 7:
                c.vol = m.GetControllerValue();
                break;

            case 10:
                c.pan = m.GetControllerValue();
                break;

            case 11:
                c.expr = m.GetControllerValue();
                break;

            case 64:
                c.damper = m.GetControllerValue();

                if ( c.damper < 64 )
                {
                    int cn = m.GetChannel();

                    for ( std::deque<Note>::iterator i = notes.begin(); i != notes.end(); )
                    {
                        if ( !i->held && i->ch == cn )
                            i = notes.erase ( i );
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
            Chan &c = ch[m.GetChannel()];
            c.tone.msb = c.pend_msb;
            c.tone.lsb = c.pend_lsb;
            c.tone.prog = m.GetPGValue();
        }
        else if ( m.IsPitchBend() )
        {
            ch[m.GetChannel()].bend = m.GetBenderValue();
        }
        // tempo, time signature, track name, end of track: no synth state
    }
};

static bool operator== ( const Synth &a, const Synth &b )
{
    for ( int i = 0; i < 16; ++i )
        if ( ! ( a.ch[i] == b.ch[i] ) )
            return false;

    if ( a.notes.size() != b.notes.size() )
        return false;

    for ( std::size_t i = 0; i < a.notes.size(); ++i )
        if ( ! ( a.notes[i] == b.notes[i] ) )
            return false;

    return true;
}

//
// Source event merge: ordered by time, then track number, then event index.
//

struct SrcEv
{
    MIDIClockTime time;
    int track;
    int index;
    const MIDITimedBigMessage *msg;
};

static bool SrcEvLess ( const SrcEv &a, const SrcEv &b )
{
    if ( a.time != b.time )
        return a.time < b.time;

    if ( a.track != b.track )
        return a.track < b.track;

    return a.index < b.index;
}

static std::vector<SrcEv> MergeEvents ( const MIDIMultiTrack &mt )
{
    std::vector<SrcEv> v;

    for ( int t = 0; t < mt.GetNumTracks(); ++t )
    {
        const MIDITrack *trk = mt.GetTrack ( t );

        for ( int i = 0; i < trk->GetNumEvents(); ++i )
        {
            SrcEv e;
            e.msg = trk->GetEventAddress ( i );
            e.time = e.msg->GetTime();
            e.track = t;
            e.index = i;
            v.push_back ( e );
        }
    }

    std::sort ( v.begin(), v.end(), SrcEvLess );
    return v;
}

//
// Event builder helpers.
//

static void PutTempo ( MIDIMultiTrack &mt, int trk, MIDIClockTime t, unsigned long tempo )
{
    MIDITimedBigMessage m;
    m.SetTempo ( tempo );
    m.SetTime ( t );
    mt.GetTrack ( trk )->PutEvent ( m );
}

static void PutTimeSig ( MIDIMultiTrack &mt, int trk, MIDIClockTime t, int num, int denpow )
{
    MIDITimedBigMessage m;
    m.SetTimeSig ( ( unsigned char ) num, ( unsigned char ) denpow, 24, 8 );
    m.SetTime ( t );
    mt.GetTrack ( trk )->PutEvent ( m );
}

static void PutEOT ( MIDIMultiTrack &mt, int trk, MIDIClockTime t )
{
    MIDITimedBigMessage m;
    m.SetEndOfTrack();
    m.SetTime ( t );
    mt.GetTrack ( trk )->PutEvent ( m );
}

static void PutNoteOn ( MIDIMultiTrack &mt, int trk, MIDIClockTime t, int ch, int note, int vel )
{
    MIDITimedBigMessage m;
    m.SetNoteOn ( ( unsigned char ) ch, ( unsigned char ) note, ( unsigned char ) vel );
    m.SetTime ( t );
    mt.GetTrack ( trk )->PutEvent ( m );
}

static void PutNoteOff ( MIDIMultiTrack &mt, int trk, MIDIClockTime t, int ch, int note )
{
    MIDITimedBigMessage m;
    m.SetNoteOff ( ( unsigned char ) ch, ( unsigned char ) note, 0 );
    m.SetTime ( t );
    mt.GetTrack ( trk )->PutEvent ( m );
}

static void PutCC ( MIDIMultiTrack &mt, int trk, MIDIClockTime t, int ch, int ctrl, int val )
{
    MIDITimedBigMessage m;
    m.SetControlChange ( ( unsigned char ) ch, ( unsigned char ) ctrl, ( unsigned char ) val );
    m.SetTime ( t );
    mt.GetTrack ( trk )->PutEvent ( m );
}

static void PutPC ( MIDIMultiTrack &mt, int trk, MIDIClockTime t, int ch, int prog )
{
    MIDITimedBigMessage m;
    m.SetProgramChange ( ( unsigned char ) ch, ( unsigned char ) prog );
    m.SetTime ( t );
    mt.GetTrack ( trk )->PutEvent ( m );
}

static void PutPB ( MIDIMultiTrack &mt, int trk, MIDIClockTime t, int ch, int val )
{
    MIDITimedBigMessage m;
    m.SetPitchBend ( ( unsigned char ) ch, ( short ) val );
    m.SetTime ( t );
    mt.GetTrack ( trk )->PutEvent ( m );
}

//
// Clip structure checks.
//

static void CheckStructure ( const MIDIMultiTrack &clip, MIDIClockTime length )
{
    CHECK ( clip.GetNumTracks() == 2, "clip must have exactly 2 tracks" );

    for ( int t = 0; t < 2; ++t )
    {
        const MIDITrack *trk = clip.GetTrack ( t );
        CHECK ( trk->EventsOrderOK(), "clip track events out of order" );
        CHECK ( trk->GetNumEvents() >= 1, "clip track must not be empty" );

        for ( int i = 0; i < trk->GetNumEvents(); ++i )
        {
            const MIDITimedBigMessage *m = trk->GetEventAddress ( i );
            CHECK ( m->GetTime() <= length, "clip event beyond clip end" );

            if ( t == 0 )
                CHECK ( m->IsTempo() || m->IsTimeSig() || m->IsEndOfTrack(),
                        "track 0 must hold tempo/time signature/EOT only" );
            else
                CHECK ( !m->IsTempo() && !m->IsTimeSig(),
                        "track 1 must not hold tempo/time signature" );
        }

        const MIDITimedBigMessage *last = trk->GetEventAddress ( trk->GetNumEvents() - 1 );
        CHECK ( last->IsEndOfTrack(), "clip track must end with EndOfTrack" );
        CHECK ( last->GetTime() == length, "EndOfTrack must be at end-begin" );
    }
}

//
// Track 0 contents: tempo/time signature in effect at begin at tick 0,
// then the in-interval changes, nothing from the prefix replayed.
//

static void CheckTrack0 ( const MIDIMultiTrack &src, const MIDIMultiTrack &clip,
                          MIDIClockTime begin, MIDIClockTime end )
{
    std::vector<SrcEv> sev = MergeEvents ( src );

    unsigned long tempo = 500000;
    int ts_num = 4, ts_denpow = 2;

    std::size_t p = 0;

    while ( p < sev.size() && sev[p].time < begin )
    {
        if ( sev[p].msg->IsTempo() )
            tempo = sev[p].msg->GetTempo();

        if ( sev[p].msg->IsTimeSig() )
        {
            ts_num = sev[p].msg->GetTimeSigNumerator();
            ts_denpow = sev[p].msg->GetTimeSigDenominatorPower();
        }

        ++p;
    }

    struct Exp
    {
        int kind; // 0=tempo 1=timesig 2=eot
        MIDIClockTime time;
        unsigned long tempo;
        int num, denpow;
    };

    std::vector<Exp> exp;
    Exp e;

    e.kind = 0;
    e.time = 0;
    e.tempo = tempo;
    e.num = 0;
    e.denpow = 0;
    exp.push_back ( e );

    e.kind = 1;
    e.time = 0;
    e.tempo = 0;
    e.num = ts_num;
    e.denpow = ts_denpow;
    exp.push_back ( e );

    while ( p < sev.size() && sev[p].time < end )
    {
        if ( sev[p].msg->IsTempo() )
        {
            e.kind = 0;
            e.time = sev[p].time - begin;
            e.tempo = sev[p].msg->GetTempo();
            e.num = 0;
            e.denpow = 0;
            exp.push_back ( e );
        }

        if ( sev[p].msg->IsTimeSig() )
        {
            e.kind = 1;
            e.time = sev[p].time - begin;
            e.tempo = 0;
            e.num = sev[p].msg->GetTimeSigNumerator();
            e.denpow = sev[p].msg->GetTimeSigDenominatorPower();
            exp.push_back ( e );
        }

        ++p;
    }

    e.kind = 2;
    e.time = end - begin;
    e.tempo = 0;
    e.num = 0;
    e.denpow = 0;
    exp.push_back ( e );

    const MIDITrack *t0 = clip.GetTrack ( 0 );
    CHECK ( t0->GetNumEvents() == ( int ) exp.size(),
            "track 0 event count mismatch (prefix replayed or change lost?)" );

    for ( int i = 0; i < ( int ) exp.size() && i < t0->GetNumEvents(); ++i )
    {
        const MIDITimedBigMessage *m = t0->GetEventAddress ( i );
        CHECK ( m->GetTime() == exp[i].time, "track 0 event time mismatch" );

        if ( exp[i].kind == 0 )
        {
            CHECK ( m->IsTempo(), "track 0 expected tempo event" );
            CHECK ( m->GetTempo() == exp[i].tempo, "track 0 tempo value mismatch" );
        }
        else if ( exp[i].kind == 1 )
        {
            CHECK ( m->IsTimeSig(), "track 0 expected time signature event" );
            CHECK ( m->GetTimeSigNumerator() == exp[i].num, "track 0 time signature mismatch" );
            CHECK ( m->GetTimeSigDenominatorPower() == exp[i].denpow, "track 0 time signature mismatch" );
        }
        else
        {
            CHECK ( m->IsEndOfTrack(), "track 0 expected EndOfTrack" );
        }
    }
}

//
// Playback equivalence between the source (from its head) and the clip
// (from an empty state), compared after every source tick in [begin,end).
//

static void ComparePlayback ( const MIDIMultiTrack &src, const MIDIMultiTrack &clip,
                              MIDIClockTime begin, MIDIClockTime end )
{
    std::vector<SrcEv> sev = MergeEvents ( src );
    const MIDIClockTime length = end - begin;

    Synth a, b;

    std::size_t p = 0;

    while ( p < sev.size() && sev[p].time < begin )
        a.apply ( *sev[p++].msg );

    const MIDITrack *ct1 = clip.GetTrack ( 1 );
    int q = 0;

    while ( p < sev.size() && sev[p].time < end )
    {
        MIDIClockTime t = sev[p].time;

        while ( p < sev.size() && sev[p].time == t )
            a.apply ( *sev[p++].msg );

        MIDIClockTime ct = t - begin;

        while ( q < ct1->GetNumEvents() && ct1->GetEventAddress ( q )->GetTime() <= ct )
            b.apply ( *ct1->GetEventAddress ( q++ ) );

        CHECK ( a == b, "synth state mismatch inside clip interval" );
    }

    // the rest of track 1 must be startup events at tick 0 (only possible
    // when the interval held no source events) or closing events at end-begin
    while ( q < ct1->GetNumEvents() )
    {
        const MIDITimedBigMessage *m = ct1->GetEventAddress ( q++ );
        CHECK ( m->GetTime() == 0 || m->GetTime() == length,
                "unexpected clip event time after interval" );
        b.apply ( *m );
    }

    CHECK ( b.notes.empty(), "hanging notes left at the end of the clip" );

    for ( int c = 0; c < 16; ++c )
        CHECK ( b.ch[c].damper < 64, "sustain pedal left down at the end of the clip" );
}

//
// One scenario: in-memory checks, then SMF write/read roundtrip checks.
//

static void RunCase ( const char *name, MIDIMultiTrack &src,
                      MIDIClockTime begin, MIDIClockTime end,
                      const std::string &dir, const char *file )
{
    g_case_name = name;

    std::vector<int> src_counts;

    for ( int t = 0; t < src.GetNumTracks(); ++t )
        src_counts.push_back ( src.GetTrack ( t )->GetNumEvents() );

    std::unique_ptr<MIDIMultiTrack> clip = ExtractPlayableClip ( src, begin, end );

    CHECK ( clip.get() != 0, "no clip returned" );
    CHECK ( clip->GetClksPerBeat() == src.GetClksPerBeat(), "clocks per beat changed" );

    CheckStructure ( *clip, end - begin );
    CheckTrack0 ( src, *clip, begin, end );
    ComparePlayback ( src, *clip, begin, end );

    for ( int t = 0; t < src.GetNumTracks(); ++t )
        CHECK ( src.GetTrack ( t )->GetNumEvents() == src_counts[t],
                "source multitrack was modified" );

    // SMF roundtrip through the existing writer/reader, then check again
    std::string path = dir + "/" + file + ".mid";

    {
        MIDIFileWriteStreamFileName stream ( path.c_str() );
        MIDIFileWriteMultiTrack writer ( clip.get(), &stream );
        CHECK ( writer.Write(), "failed to write clip as SMF" );
    }

    MIDIMultiTrack back ( 2 );
    MIDIFileReadMultiTrack events ( &back );

    {
        MIDIFileReadStreamFile stream ( path.c_str() );
        MIDIFileRead reader ( &stream, &events );
        CHECK ( reader.Parse(), "failed to read clip SMF back" );
    }

    CHECK ( back.GetNumTracks() == 2, "read-back clip must have 2 tracks" );
    CHECK ( back.GetClksPerBeat() == src.GetClksPerBeat(), "read-back clocks per beat changed" );

    CheckStructure ( back, end - begin );
    CheckTrack0 ( src, back, begin, end );
    ComparePlayback ( src, back, begin, end );

    std::printf ( "case %s ok (in-memory + SMF roundtrip)\n", name );
}

//
// Scenario 1: the example from the specification. Note A struck with
// program 5, note B with program 8, pedal down, A released, begin=50.
// A must be rebuilt with program 5 and its pedal-held identity, B with
// program 8, and the current program 8 must be in effect afterwards.
//

static void Case1 ( const std::string &dir )
{
    MIDIMultiTrack s ( 2 );
    s.SetClksPerBeat ( 480 );

    PutTempo ( s, 0, 0, 500000 );
    PutTimeSig ( s, 0, 0, 4, 2 );
    PutEOT ( s, 0, 200 );

    PutPC ( s, 1, 10, 0, 5 );
    PutNoteOn ( s, 1, 10, 0, 60, 100 ); // A
    PutPC ( s, 1, 20, 0, 8 );
    PutNoteOn ( s, 1, 20, 0, 64, 90 );  // B
    PutCC ( s, 1, 30, 0, 64, 127 );     // pedal down
    PutNoteOff ( s, 1, 40, 0, 60 );     // A released, sustained by pedal
    PutNoteOn ( s, 1, 60, 0, 67, 80 );  // C, inside the clip
    PutCC ( s, 1, 70, 0, 64, 0 );       // pedal up: A stops
    PutNoteOff ( s, 1, 80, 0, 64 );
    PutNoteOff ( s, 1, 90, 0, 67 );
    PutEOT ( s, 1, 200 );

    RunCase ( "spec_example", s, 50, 200, dir, "case1" );
}

//
// Scenario 2: two physical tracks share one MIDI channel. Same-tick
// events across tracks must merge by track number, then event index.
//

static void Case2 ( const std::string &dir )
{
    MIDIMultiTrack s ( 3 );
    s.SetClksPerBeat ( 480 );

    PutTempo ( s, 0, 0, 500000 );
    PutEOT ( s, 0, 200 );

    s.GetTrack ( 1 )->PutTextEvent ( 0, META_TRACK_NAME, "piano" );
    PutPC ( s, 1, 0, 0, 3 );
    PutNoteOn ( s, 1, 10, 0, 60, 100 );
    PutNoteOn ( s, 1, 50, 0, 64, 90 );
    PutCC ( s, 1, 50, 0, 11, 90 ); // same tick, same track: index order
    PutNoteOff ( s, 1, 90, 0, 60 );
    PutEOT ( s, 1, 200 );

    s.GetTrack ( 2 )->PutTextEvent ( 0, META_TRACK_NAME, "piano overdub" );
    PutCC ( s, 2, 20, 0, 7, 80 );
    PutNoteOn ( s, 2, 30, 0, 67, 70 );
    PutCC ( s, 2, 50, 0, 10, 30 ); // same tick as track 1 events: track order
    PutNoteOff ( s, 2, 70, 0, 67 );
    PutEOT ( s, 2, 200 );

    RunCase ( "shared_channel", s, 40, 100, dir, "case2" );

    // explicit merge order check: no source events at begin, so the clip
    // track 1 events strictly inside (0, end-begin) are exactly the shifted
    // in-interval source channel events, in merge order
    std::unique_ptr<MIDIMultiTrack> clip = ExtractPlayableClip ( s, 40, 100 );
    std::vector<SrcEv> sev = MergeEvents ( s );
    std::vector<const MIDITimedBigMessage *> expected, actual;

    for ( std::size_t i = 0; i < sev.size(); ++i )
        if ( sev[i].time >= 40 && sev[i].time < 100 && sev[i].msg->IsChannelMsg() )
            expected.push_back ( sev[i].msg );

    const MIDITrack *t1 = clip->GetTrack ( 1 );

    for ( int i = 0; i < t1->GetNumEvents(); ++i )
    {
        const MIDITimedBigMessage *m = t1->GetEventAddress ( i );

        if ( m->GetTime() > 0 && m->GetTime() < 60 )
            actual.push_back ( m );
    }

    g_case_name = "shared_channel_order";
    CHECK ( actual.size() == expected.size(), "in-interval channel event count mismatch" );

    for ( std::size_t i = 0; i < expected.size() && i < actual.size(); ++i )
    {
        CHECK ( actual[i]->GetTime() == expected[i]->GetTime() - 40, "merge order: time" );
        CHECK ( actual[i]->GetStatus() == expected[i]->GetStatus(), "merge order: status" );
        CHECK ( actual[i]->GetByte1() == expected[i]->GetByte1(), "merge order: byte1" );
        CHECK ( actual[i]->GetByte2() == expected[i]->GetByte2(), "merge order: byte2" );
    }

    std::printf ( "case shared_channel merge order ok\n" );
}

//
// Scenario 3: the same key struck repeatedly with different velocities,
// overlapping instances, pedal-sustained releases.
//

static void Case3 ( const std::string &dir )
{
    MIDIMultiTrack s ( 2 );
    s.SetClksPerBeat ( 480 );

    PutTempo ( s, 0, 0, 500000 );
    PutEOT ( s, 0, 200 );

    PutNoteOn ( s, 1, 10, 0, 60, 100 );
    PutNoteOn ( s, 1, 20, 0, 60, 80 );
    PutCC ( s, 1, 30, 0, 64, 100 );    // pedal down
    PutNoteOff ( s, 1, 40, 0, 60 );    // releases the v100 instance, sustained
    PutNoteOn ( s, 1, 50, 0, 60, 60 );
    PutNoteOff ( s, 1, 60, 0, 60 );    // releases the v80 instance, sustained
    PutCC ( s, 1, 70, 0, 64, 0 );      // pedal up: v100 and v80 stop
    PutNoteOff ( s, 1, 80, 0, 60 );    // releases the v60 instance
    PutEOT ( s, 1, 200 );

    RunCase ( "repeated_key", s, 45, 75, dir, "case3" );
}

//
// Scenario 4: begin falls exactly on a NoteOff and a ProgramChange.
// Events at begin belong to the clip, not to the prefix state.
//

static void Case4 ( const std::string &dir )
{
    MIDIMultiTrack s ( 2 );
    s.SetClksPerBeat ( 480 );

    PutTempo ( s, 0, 0, 500000 );
    PutEOT ( s, 0, 200 );

    PutPC ( s, 1, 10, 0, 5 );
    PutNoteOn ( s, 1, 10, 0, 60, 100 );
    PutNoteOn ( s, 1, 20, 0, 64, 90 );
    PutNoteOff ( s, 1, 40, 0, 60 );    // exactly at begin: kept in the clip
    PutPC ( s, 1, 40, 0, 9 );          // exactly at begin: kept in the clip
    PutNoteOn ( s, 1, 50, 0, 67, 70 ); // must sound with program 9
    PutNoteOff ( s, 1, 60, 0, 64 );
    PutNoteOff ( s, 1, 70, 0, 67 );
    PutEOT ( s, 1, 200 );

    RunCase ( "begin_on_noteoff_and_pc", s, 40, 80, dir, "case4" );
}

//
// Scenario 5: a NoteOn exactly at end must not be copied into the clip.
//

static void Case5 ( const std::string &dir )
{
    MIDIMultiTrack s ( 2 );
    s.SetClksPerBeat ( 480 );

    PutTempo ( s, 0, 0, 500000 );
    PutEOT ( s, 0, 200 );

    PutNoteOn ( s, 1, 10, 0, 60, 100 );
    PutNoteOff ( s, 1, 50, 0, 60 );
    PutNoteOn ( s, 1, 80, 0, 62, 90 ); // exactly at end: excluded
    PutNoteOff ( s, 1, 90, 0, 62 );
    PutEOT ( s, 1, 200 );

    RunCase ( "end_on_noteon", s, 20, 80, dir, "case5" );

    std::unique_ptr<MIDIMultiTrack> clip = ExtractPlayableClip ( s, 20, 80 );
    const MIDITrack *t1 = clip->GetTrack ( 1 );

    g_case_name = "end_on_noteon_exclusion";

    for ( int i = 0; i < t1->GetNumEvents(); ++i )
    {
        const MIDITimedBigMessage *m = t1->GetEventAddress ( i );

        if ( m->IsNoteOn() || m->IsNoteOff() )
            CHECK ( m->GetNote() != 62, "event at end must not be copied into the clip" );
    }

    std::printf ( "case end_on_noteon exclusion ok\n" );
}

//
// Scenario 6: an empty interval. No events inside, nothing sounding at
// begin; the current program is still restored at tick 0.
//

static void Case6 ( const std::string &dir )
{
    MIDIMultiTrack s ( 2 );
    s.SetClksPerBeat ( 480 );

    PutTempo ( s, 0, 0, 400000 );
    PutEOT ( s, 0, 300 );

    PutNoteOn ( s, 1, 10, 0, 60, 100 );
    PutNoteOff ( s, 1, 20, 0, 60 );
    PutPC ( s, 1, 30, 0, 7 );
    PutNoteOn ( s, 1, 200, 0, 62, 90 );
    PutNoteOff ( s, 1, 210, 0, 62 );
    PutEOT ( s, 1, 300 );

    RunCase ( "empty_interval", s, 100, 150, dir, "case6" );
}

//
// Scenario 7: tempo and time signature migration. The values in effect
// before begin go to tick 0, in-interval changes are shifted, the prefix
// is not replayed.
//

static void Case7 ( const std::string &dir )
{
    MIDIMultiTrack s ( 2 );
    s.SetClksPerBeat ( 480 );

    PutTempo ( s, 0, 0, 500000 );
    PutTimeSig ( s, 0, 0, 4, 2 );
    PutTempo ( s, 0, 40, 400000 );
    PutTimeSig ( s, 0, 60, 3, 2 );
    PutTempo ( s, 0, 120, 300000 );
    PutTimeSig ( s, 0, 140, 6, 3 );
    PutEOT ( s, 0, 300 );

    PutNoteOn ( s, 1, 10, 0, 60, 100 );
    PutNoteOff ( s, 1, 50, 0, 60 );
    PutNoteOn ( s, 1, 130, 0, 62, 90 );
    PutNoteOff ( s, 1, 150, 0, 62 );
    PutEOT ( s, 1, 300 );

    RunCase ( "tempo_timesig", s, 100, 200, dir, "case7" );
}

//
// Invalid inputs must throw std::invalid_argument.
//

static void ExpectThrow ( const char *name, MIDIMultiTrack &src,
                          MIDIClockTime begin, MIDIClockTime end )
{
    g_case_name = name;
    bool threw = false;

    try
    {
        std::unique_ptr<MIDIMultiTrack> clip = ExtractPlayableClip ( src, begin, end );
    }
    catch ( const std::invalid_argument & )
    {
        threw = true;
    }

    CHECK ( threw, "expected std::invalid_argument" );
    std::printf ( "reject %s ok\n", name );
}

static void CheckErrors()
{
    {
        MIDIMultiTrack s ( 1 );
        s.SetClksPerBeat ( 480 );
        PutNoteOn ( s, 0, 10, 0, 60, 100 );
        PutNoteOff ( s, 0, 20, 0, 60 );
        ExpectThrow ( "begin_eq_end", s, 100, 100 );
        ExpectThrow ( "begin_gt_end", s, 200, 100 );
    }
    {
        MIDIMultiTrack s ( 1 );
        s.SetClksPerBeat ( 480 );
        MIDITimedBigMessage m;
        m.SetSysEx ( SYSEX_START_N );
        m.SetTime ( 10 );
        s.GetTrack ( 0 )->PutEvent ( m );
        ExpectThrow ( "sysex", s, 0, 100 );
    }
    {
        MIDIMultiTrack s ( 1 );
        s.SetClksPerBeat ( 480 );
        PutCC ( s, 0, 10, 0, 1, 64 ); // modulation: not in the supported set
        ExpectThrow ( "other_cc", s, 0, 100 );
    }
    {
        MIDIMultiTrack s ( 1 );
        s.SetClksPerBeat ( 480 );
        MIDITimedBigMessage m;
        m.SetChannelPressure ( 0, 90 );
        m.SetTime ( 10 );
        s.GetTrack ( 0 )->PutEvent ( m );
        ExpectThrow ( "channel_pressure", s, 0, 100 );
    }
    {
        MIDIMultiTrack s ( 1 );
        s.SetClksPerBeat ( 480 );
        MIDITimedBigMessage m;
        m.SetPolyPressure ( 0, 60, 90 );
        m.SetTime ( 10 );
        s.GetTrack ( 0 )->PutEvent ( m );
        ExpectThrow ( "poly_pressure", s, 0, 100 );
    }
    {
        MIDIMultiTrack s ( 1 );
        s.SetClksPerBeat ( 480 );
        MIDITimedBigMessage m;
        m.SetKeySig ( 2, 0 ); // meta event outside the supported set
        m.SetTime ( 10 );
        s.GetTrack ( 0 )->PutEvent ( m );
        ExpectThrow ( "other_meta", s, 0, 100 );
    }
    {
        MIDIMultiTrack s ( 1 );
        s.SetClksPerBeat ( 0xE728 ); // SMPTE 25 fps, 40 ticks per frame
        PutNoteOn ( s, 0, 10, 0, 60, 100 );
        ExpectThrow ( "smpte_timebase", s, 0, 100 );
    }
    {
        MIDIMultiTrack s ( 1 );
        s.SetClksPerBeat ( 480 );
        PutNoteOn ( s, 0, 100, 0, 60, 100 );
        PutNoteOff ( s, 0, 50, 0, 60 ); // physically unsorted track
        ExpectThrow ( "unsorted_track", s, 0, 200 );
    }
}

int main ( int argc, char **argv )
{
    if ( argc != 2 )
    {
        std::fprintf ( stderr, "usage: playable_clip <output_dir>\n" );
        return 1;
    }

    std::string dir = argv[1];

    Case1 ( dir );
    Case2 ( dir );
    Case3 ( dir );
    Case4 ( dir );
    Case5 ( dir );
    Case6 ( dir );
    Case7 ( dir );
    CheckErrors();

    std::printf ( "PLAYABLE_CLIP_OK cases=7\n" );
    return 0;
}
