#include "MidiChannelVoicer.h"
#include "SineWave.h"
#include "Instrmnt.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <vector>

// A real SineWave instrument that logs every frequency, noteOff and
// control change it receives, so the test can inspect what the
// wrapped instruments actually saw rather than only the adapter state.
class ProbeTone : public stk::Instrmnt {
  stk::SineWave oscillator;
  stk::StkFloat gain = 0;
public:
  struct FreqEvent { long clock; double freq; };
  struct OffEvent { long clock; double amplitude; };
  struct CcEvent { int number; double value; };

  stk::StkFloat frequency = 0;
  long clock = 0;
  std::vector<FreqEvent> freqLog;
  std::vector<OffEvent> offLog;
  std::vector<CcEvent> ccLog;

  void setFrequency( stk::StkFloat f ) override {
    frequency = f;
    oscillator.setFrequency( f );
    freqLog.push_back( { clock, (double) f } );
  }
  void noteOn( stk::StkFloat f, stk::StkFloat a ) override {
    setFrequency( f );
    gain = a;
  }
  void noteOff( stk::StkFloat a ) override {
    gain = 0;
    offLog.push_back( { clock, (double) a } );
  }
  void controlChange( int number, stk::StkFloat value ) override {
    ccLog.push_back( { number, (double) value } );
  }
  stk::StkFloat tick( unsigned int = 0 ) override {
    ++clock;
    return lastFrame_[0] = gain * oscillator.tick();
  }
  stk::StkFrames& tick( stk::StkFrames& f, unsigned int c = 0 ) override {
    for ( unsigned i = 0; i < f.frames(); ++i ) f( i, c ) = tick();
    return f;
  }
};

struct Setup {
  stk::Voicer voicer;
  stk::MidiChannelVoicer midi;
  std::map<int, ProbeTone*> groupTone;
  std::vector<ProbeTone*> tones;

  Setup( double decay = 0.05 ) : voicer( decay ), midi( voicer ) {}
  ~Setup() { for ( ProbeTone* t : tones ) delete t; }

  ProbeTone* add( int group ) {
    ProbeTone* t = new ProbeTone();
    tones.push_back( t );
    voicer.addInstrument( t, group );
    if ( groupTone.find( group ) == groupTone.end() ) groupTone[group] = t;
    return t;
  }
};

static double noteFreq( double note ) {
  return 220.0 * std::pow( 2.0, ( note - 57.0 ) / 12.0 );
}

// Expected instrument frequency for a note under the given wheel and
// bend sensitivity (semitones + cents), using the adapter's formula.
static double bentFreq( double note, int wheel, int semis, int cents = 0 ) {
  double range = semis + cents / 100.0;
  double off = ( wheel < 8192 )
    ? range * ( wheel - 8192 ) / 8192.0
    : range * ( wheel - 8192 ) / 8191.0;
  return noteFreq( note + off );
}

static void closeFreq( double got, double expected, const char* what ) {
  double rel = std::abs( got - expected ) / expected;
  if ( !( rel < 1e-12 ) ) {
    std::cerr << what << ": got " << got << " expected " << expected << "\n";
    assert( false );
  }
}

// Sound a short note on a single-voice group at the current wheel and
// return the frequency the instrument actually received.
static double probeFreq( Setup& s, int channel, int note = 69 ) {
  long tag = s.midi.noteOn( channel, note, 64 );
  assert( tag >= 0 );
  double f = s.groupTone[channel]->frequency;
  assert( s.midi.noteOff( tag, 64 ) );
  return f;
}

static void drain( Setup& s, unsigned frames ) {
  for ( unsigned i = 0; i < frames; ++i ) s.voicer.tick();
}

int main() {
  stk::Stk::setSampleRate( 48000 );

  // ---------------------------------------------------------------
  // 1. RPN bend sensitivity, wheel endpoints, exact return to center.
  // ---------------------------------------------------------------
  {
    Setup s;
    s.add( 0 );
    ProbeTone* a = s.groupTone[0];
    ProbeTone* b = s.add( 0 );

    long tag = s.midi.noteOn( 0, 69, 64 );
    assert( tag >= 0 );
    double centerFreq = a->frequency;
    closeFreq( centerFreq, 440.0, "default A4" );

    // RPN 0/0 = pitch bend sensitivity: 12 semitones, 50 cents.
    assert( s.midi.controlChange( 0, 101, 0 ) );
    assert( s.midi.controlChange( 0, 100, 0 ) );
    assert( s.midi.controlChange( 0, 6, 12 ) );
    assert( s.midi.controlChange( 0, 38, 50 ) );

    assert( s.midi.pitchBend( 0, 16383 ) );
    closeFreq( a->frequency, bentFreq( 69, 16383, 12, 50 ),
               "wheel top, 12.5 semitones" );
    closeFreq( a->frequency, 440.0 * std::pow( 2.0, 12.5 / 12.0 ),
               "wheel top, explicit ratio" );

    assert( s.midi.pitchBend( 0, 0 ) );
    closeFreq( a->frequency, bentFreq( 69, 0, 12, 50 ),
               "wheel bottom, 12.5 semitones" );
    closeFreq( a->frequency, 440.0 * std::pow( 2.0, -12.5 / 12.0 ),
               "wheel bottom, explicit ratio" );

    // Repeated bends must not accumulate; same wheel gives same pitch.
    double first = a->frequency;
    assert( s.midi.pitchBend( 0, 0 ) );
    assert( a->frequency == first );

    // Center restores the original pitch exactly, not just approximately.
    assert( s.midi.pitchBend( 0, 8192 ) );
    assert( a->frequency == centerFreq );

    // A new note triggered while the wheel sits off-center carries the
    // offset immediately.
    assert( s.midi.pitchBend( 0, 16383 ) );
    long tag2 = s.midi.noteOn( 0, 69, 64 );
    assert( tag2 >= 0 && tag2 != tag );
    closeFreq( b->frequency, bentFreq( 69, 16383, 12, 50 ),
               "new note at non-center wheel" );
    assert( s.midi.noteOff( tag, 64 ) );
    assert( s.midi.noteOff( tag2, 64 ) );
    drain( s, 2500 );
    std::cout << "section 1 ok: RPN range, endpoints, exact center restore\n";
  }

  // ---------------------------------------------------------------
  // 2. RPN / NRPN selector interleaving and data-entry ordering.
  // ---------------------------------------------------------------
  {
    Setup s;
    s.add( 0 );

    assert( s.midi.controlChange( 0, 101, 0 ) ); // RPN MSB only so far
    assert( s.midi.controlChange( 0, 99, 1 ) );  // switch into NRPN mode
    assert( s.midi.controlChange( 0, 6, 7 ) );   // must not touch the range
    assert( s.midi.controlChange( 0, 38, 10 ) ); // same for the LSB entry
    s.midi.pitchBend( 0, 16383 );
    closeFreq( probeFreq( s, 0 ), bentFreq( 69, 16383, 2, 0 ),
               "NRPN data entry must not change RPN sensitivity" );

    // Back to RPN 0/0: LSB first, then CC38 before CC6.  Both parts of
    // the sensitivity must survive the ordering.
    assert( s.midi.controlChange( 0, 100, 0 ) );
    assert( s.midi.controlChange( 0, 38, 40 ) );
    assert( s.midi.controlChange( 0, 6, 5 ) );
    closeFreq( probeFreq( s, 0 ), bentFreq( 69, 16383, 5, 40 ),
               "CC38 before CC6, both retained" );

    // 127/127 null RPN selection: data entry changes nothing.
    assert( s.midi.controlChange( 0, 101, 127 ) );
    assert( s.midi.controlChange( 0, 100, 127 ) );
    assert( s.midi.controlChange( 0, 6, 9 ) );
    assert( s.midi.controlChange( 0, 38, 10 ) );
    closeFreq( probeFreq( s, 0 ), bentFreq( 69, 16383, 5, 40 ),
               "null RPN selection leaves range untouched" );

    // Re-select RPN 0/0: cents 100..127 are rejected, old range kept.
    assert( s.midi.controlChange( 0, 101, 0 ) );
    assert( s.midi.controlChange( 0, 100, 0 ) );
    assert( ! s.midi.controlChange( 0, 38, 100 ) );
    assert( ! s.midi.controlChange( 0, 38, 127 ) );
    closeFreq( probeFreq( s, 0 ), bentFreq( 69, 16383, 5, 40 ),
               "out-of-range cents rejected, old range kept" );
    assert( s.midi.controlChange( 0, 38, 99 ) );
    closeFreq( probeFreq( s, 0 ), bentFreq( 69, 16383, 5, 99 ),
               "cents 99 accepted" );

    // CC121: wheel and sensitivity reset, selectors cleared.
    long live = s.midi.noteOn( 0, 69, 64 );
    assert( s.midi.controlChange( 0, 121, 0 ) );
    closeFreq( s.groupTone[0]->frequency, 440.0,
               "CC121 retunes resident voice to center" );
    assert( s.midi.controlChange( 0, 6, 8 ) ); // no selector: ignored
    s.midi.pitchBend( 0, 16383 );
    closeFreq( s.groupTone[0]->frequency, bentFreq( 69, 16383, 2, 0 ),
               "CC121 restores default 2-semitone range" );
    assert( s.midi.noteOff( live, 64 ) );
    drain( s, 2500 );
    std::cout << "section 2 ok: RPN/NRPN interleaving, null select, CC121 reset\n";
  }

  // ---------------------------------------------------------------
  // 3. Input validation: failures return the right code and are silent.
  // ---------------------------------------------------------------
  {
    Setup s;
    s.add( 0 );
    ProbeTone* a = s.groupTone[0];

    assert( s.midi.noteOn( -1, 69, 64 ) == -1 );
    assert( s.midi.noteOn( 16, 69, 64 ) == -1 );
    assert( s.midi.noteOn( 0, -1, 64 ) == -1 );
    assert( s.midi.noteOn( 0, 128, 64 ) == -1 );
    assert( s.midi.noteOn( 0, 69, 0 ) == -1 );
    assert( s.midi.noteOn( 0, 69, 128 ) == -1 );
    assert( a->freqLog.empty() );

    assert( ! s.midi.pitchBend( -1, 0 ) );
    assert( ! s.midi.pitchBend( 16, 0 ) );
    assert( ! s.midi.pitchBend( 0, -1 ) );
    assert( ! s.midi.pitchBend( 0, 16384 ) );

    // A rejected wheel message must not move the wheel.
    assert( s.midi.pitchBend( 0, 0 ) );
    assert( ! s.midi.pitchBend( 0, 16384 ) );
    closeFreq( probeFreq( s, 0 ), bentFreq( 69, 0, 2, 0 ),
               "rejected bend leaves wheel at bottom" );

    assert( ! s.midi.controlChange( 16, 7, 100 ) );
    assert( ! s.midi.controlChange( 0, 128, 0 ) );
    assert( ! s.midi.controlChange( 0, 7, 200 ) );
    assert( a->ccLog.empty() );

    // Valid ordinary controllers are forwarded on the group path.
    assert( s.midi.controlChange( 0, 7, 42 ) );
    assert( a->ccLog.size() == 1 && a->ccLog[0].number == 7
            && a->ccLog[0].value == 42.0 );
    drain( s, 2500 );
    std::cout << "section 3 ok: invalid messages rejected and silent\n";
  }

  // ---------------------------------------------------------------
  // 4. Channel isolation: channel 1 must not follow channel 0.
  // ---------------------------------------------------------------
  {
    Setup s;
    s.add( 0 );
    s.add( 1 );
    ProbeTone* a = s.groupTone[0];
    ProbeTone* c = s.groupTone[1];

    long t0 = s.midi.noteOn( 0, 69, 64 );
    long t1 = s.midi.noteOn( 1, 69, 64 );
    assert( t0 >= 0 && t1 >= 0 );
    closeFreq( c->frequency, 440.0, "channel 1 starts at 440" );

    s.midi.controlChange( 0, 101, 0 );
    s.midi.controlChange( 0, 100, 0 );
    s.midi.controlChange( 0, 6, 12 );
    assert( s.midi.pitchBend( 0, 16383 ) );
    closeFreq( a->frequency, bentFreq( 69, 16383, 12, 0 ),
               "channel 0 bends wide" );
    assert( c->frequency == 440.0 ); // untouched

    // Channel 1 still has its own default 2-semitone sensitivity.
    assert( s.midi.pitchBend( 1, 0 ) );
    closeFreq( c->frequency, bentFreq( 69, 0, 2, 0 ),
               "channel 1 keeps its own default range" );
    closeFreq( a->frequency, bentFreq( 69, 16383, 12, 0 ),
               "channel 0 still wide" );
    assert( s.midi.noteOff( t0, 64 ) );
    assert( s.midi.noteOff( t1, 64 ) );
    drain( s, 2500 );
    std::cout << "section 4 ok: channel isolation\n";
  }

  // ---------------------------------------------------------------
  // 5. Release tails are still retuned, then expire cleanly.
  // ---------------------------------------------------------------
  {
    Setup s( 0.05 ); // 2400-sample release tail at 48 kHz.
    s.add( 0 );
    ProbeTone* a = s.groupTone[0];

    long tag = s.midi.noteOn( 0, 69, 64 );
    assert( s.midi.noteOff( tag, 64 ) );
    assert( a->offLog.size() == 1 ); // instrument release, Voicer decay

    size_t freqEvents = a->freqLog.size();
    assert( s.midi.pitchBend( 0, 16383 ) );
    assert( a->freqLog.size() > freqEvents ); // tail voice still retuned
    closeFreq( a->frequency, bentFreq( 69, 16383, 2, 0 ),
               "release tail follows the wheel" );

    drain( s, 2401 ); // tail fully expires
    assert( ! s.midi.noteOff( tag, 64 ) ); // stale tag, no second release
    assert( a->offLog.size() == 1 );
    std::cout << "section 5 ok: release tail retune and expiry\n";
  }

  // ---------------------------------------------------------------
  // 6. Preemption: stale tags cannot touch reassigned voices.
  // ---------------------------------------------------------------
  {
    Setup s;
    ProbeTone* a = s.add( 0 );
    ProbeTone* b = s.add( 0 );

    long t1 = s.midi.noteOn( 0, 60, 64 );
    long t2 = s.midi.noteOn( 0, 64, 64 );
    assert( t1 >= 0 && t2 >= 0 && t1 != t2 );
    assert( a->offLog.empty() && b->offLog.empty() );

    // Third note preempts the oldest voice (t1); preemption is a
    // noteOn on that instrument, never a noteOff.
    long t3 = s.midi.noteOn( 0, 67, 64 );
    assert( t3 >= 0 && t3 != t1 && t3 != t2 );
    assert( a->offLog.empty() );

    // The stale tag must not release or retune the new voice.
    assert( ! s.midi.noteOff( t1, 64 ) );
    assert( a->offLog.empty() );

    // The preempting voice is still live and bends with its own note.
    assert( s.midi.pitchBend( 0, 16383 ) );
    closeFreq( a->frequency, bentFreq( 67, 16383, 2, 0 ),
               "preempting voice bends from its own note" );
    closeFreq( b->frequency, bentFreq( 64, 16383, 2, 0 ),
               "other voice bends from its own note" );

    // The surviving tag releases normally.
    assert( s.midi.noteOff( t2, 64 ) );
    assert( b->offLog.size() == 1 );
    assert( s.midi.noteOff( t3, 64 ) );
    assert( a->offLog.size() == 1 );

    // A long run must not accumulate dead records: early tags are
    // rejected and the newest voices stay usable.
    long early = s.midi.noteOn( 0, 60, 64 );
    for ( int i = 0; i < 50; ++i ) {
      long t = s.midi.noteOn( 0, 60 + ( i % 12 ), 64 );
      assert( t >= 0 );
    }
    assert( ! s.midi.noteOff( early, 64 ) );
    drain( s, 2500 );
    std::cout << "section 6 ok: preemption and stale tag handling\n";
  }

  // ---------------------------------------------------------------
  // 7. Non-zero PCM, quiet after release, single-frame vs block parity.
  // ---------------------------------------------------------------
  auto scenario = []( Setup& s, bool blocked ) {
    s.add( 0 ); s.add( 0 ); s.add( 1 );
    std::vector<double> out;
    auto advance = [&]( unsigned frames ) {
      if ( blocked ) {
        unsigned done = 0;
        while ( done < frames ) {
          unsigned n = std::min( (unsigned) 11, frames - done );
          stk::StkFrames block( n, 1 );
          s.voicer.tick( block );
          for ( unsigned i = 0; i < n; ++i ) out.push_back( block[i] );
          done += n;
        }
      }
      else {
        for ( unsigned i = 0; i < frames; ++i ) out.push_back( s.voicer.tick() );
      }
    };

    long t0 = s.midi.noteOn( 0, 60, 64 );
    long t1 = s.midi.noteOn( 0, 67, 64 );
    long tc = s.midi.noteOn( 1, 69, 64 );
    (void) t0;
    advance( 32 );
    s.midi.controlChange( 0, 101, 0 );
    s.midi.controlChange( 0, 100, 0 );
    s.midi.controlChange( 0, 6, 7 );
    s.midi.pitchBend( 0, 12000 );
    s.midi.pitchBend( 1, 4000 );
    advance( 40 );
    long t3 = s.midi.noteOn( 0, 72, 64 ); // preempts the oldest voice
    (void) t3;
    s.midi.noteOff( t1, 64 );
    advance( 64 );
    s.midi.noteOff( tc, 64 );
    advance( 128 );
    s.midi.noteOff( t3, 64 );
    return out;
  };

  std::vector<double> singleSamples, blockSamples;
  double singleEnergy = 0.0, blockEnergy = 0.0;
  {
    Setup single( 0.05 ), block( 0.05 );
    singleSamples = scenario( single, false );
    blockSamples = scenario( block, true );
    for ( double x : singleSamples ) singleEnergy += x * x;
    for ( double x : blockSamples ) blockEnergy += x * x;

    assert( singleSamples.size() == blockSamples.size() );
    for ( size_t i = 0; i < singleSamples.size(); ++i )
      assert( singleSamples[i] == blockSamples[i] ); // bit-identical
    assert( singleEnergy > 0.0 && blockEnergy > 0.0 );

    // Everything released and drained: the mix goes quiet.
    drain( single, 2500 );
    drain( block, 2500 );
    assert( single.voicer.tick() == 0.0 );
    assert( block.voicer.tick() == 0.0 );
  }
  std::cout << "section 7 ok: nonzero PCM, quiet release, frame/block parity ("
            << singleSamples.size() << " frames, energy "
            << singleEnergy << ")\n";

  std::cout << "MIDI_RPN_OK\n";
  return 0;
}
