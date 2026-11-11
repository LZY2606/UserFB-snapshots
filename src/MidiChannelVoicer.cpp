/***************************************************/
/*! \class MidiChannelVoicer
    \brief MIDI channel adapter for a stk::Voicer with per-channel
    pitch-bend wheels and RPN-controlled bend sensitivity.

    This class wraps an existing Voicer without taking ownership of
    it or of any instruments registered with it.  MIDI channels 0 to
    15 map directly to the instrument groups with the same numbers in
    the wrapped Voicer.

    Each channel keeps its own pitch-bend wheel value (centered at
    8192) and its own bend sensitivity, expressed in semitones and
    cents.  The default sensitivity is 2 semitones and 0 cents.  The
    registered parameter number (RPN) 0/0 ("pitch bend sensitivity")
    is supported through control changes: CC101/CC100 select the
    parameter, CC6 sets the semitone value and CC38 sets the cent
    value (0 to 99).

    Pitch is always recomputed from the original note number of each
    noteOn, so repeated bend messages never accumulate drift and the
    wheel returning to center restores the original pitch exactly.

    The adapter does not own the wrapped Voicer, which must remain
    valid for the lifetime of this object.  Existing code that uses a
    Voicer directly is unaffected.

    by Perry R. Cook and Gary P. Scavone, 1995--2023.
*/
/***************************************************/

#include "MidiChannelVoicer.h"
#include <cmath>

namespace stk {

MidiChannelVoicer :: MidiChannelVoicer( Voicer& voicer )
  : voicer_( voicer )
{
}

StkFloat MidiChannelVoicer :: bentNote( StkFloat baseNote, int wheel, StkFloat rangeSemitones )
{
  StkFloat offset;
  if ( wheel < 8192 )
    offset = rangeSemitones * (StkFloat) (wheel - 8192) / 8192.0;
  else
    offset = rangeSemitones * (StkFloat) (wheel - 8192) / 8191.0;
  return baseNote + offset;
}

void MidiChannelVoicer :: pruneStaleVoices( void )
{
  // A tag disappears from the wrapped Voicer as soon as its voice is
  // preempted (a new tag is assigned) or its release tail finishes.
  for ( std::map<long, ActiveVoice>::iterator it = activeVoices_.begin();
        it != activeVoices_.end(); ) {
    if ( voicer_.hasTag( it->first ) )
      ++it;
    else
      it = activeVoices_.erase( it );
  }
}

void MidiChannelVoicer :: retuneChannel( int channel )
{
  const ChannelState& cs = channels_[channel];
  StkFloat range = (StkFloat) cs.semitones + (StkFloat) cs.cents / 100.0;
  for ( std::map<long, ActiveVoice>::iterator it = activeVoices_.begin();
        it != activeVoices_.end(); ++it ) {
    if ( it->second.channel != channel ) continue;
    // Recompute from the original note every time: bending never
    // accumulates and center always restores the original pitch.
    voicer_.setFrequency( it->first,
                          bentNote( it->second.baseNote, cs.wheel, range ) );
  }
}

long MidiChannelVoicer :: noteOn( int channel, int note, int velocity )
{
  if ( channel < 0 || channel > 15 ||
       note < 0 || note > 127 ||
       velocity < 1 || velocity > 127 )
    return -1;

  pruneStaleVoices();

  const ChannelState& cs = channels_[channel];
  StkFloat range = (StkFloat) cs.semitones + (StkFloat) cs.cents / 100.0;
  StkFloat noteNumber = bentNote( (StkFloat) note, cs.wheel, range );

  long tag = voicer_.noteOn( noteNumber, (StkFloat) velocity, channel );
  if ( tag < 0 )
    return -1;

  ActiveVoice voice;
  voice.channel = channel;
  voice.baseNote = (StkFloat) note;
  activeVoices_[tag] = voice;
  return tag;
}

bool MidiChannelVoicer :: noteOff( long tag, int velocity )
{
  pruneStaleVoices();

  std::map<long, ActiveVoice>::iterator it = activeVoices_.find( tag );
  if ( it == activeVoices_.end() || ! voicer_.hasTag( tag ) ) {
    // The voice was preempted or its release tail has expired.  A
    // stale tag must never touch a voice that has been reassigned.
    if ( it != activeVoices_.end() )
      activeVoices_.erase( it );
    return false;
  }

  // Use the wrapped Voicer's own decay time; the instrument's release
  // synthesis is not reimplemented here.
  voicer_.noteOff( tag, (StkFloat) velocity );
  return true;
}

bool MidiChannelVoicer :: pitchBend( int channel, int wheel14 )
{
  if ( channel < 0 || channel > 15 || wheel14 < 0 || wheel14 > 16383 )
    return false;

  channels_[channel].wheel = wheel14;
  pruneStaleVoices();
  retuneChannel( channel );
  return true;
}

bool MidiChannelVoicer :: controlChange( int channel, int controller, int value )
{
  if ( channel < 0 || channel > 15 ||
       controller < 0 || controller > 127 ||
       value < 0 || value > 127 )
    return false;

  ChannelState& cs = channels_[channel];

  switch ( controller ) {

  case 101: // RPN MSB
    cs.rpnMsb = value;
    cs.mode = RPN_SELECTED;
    return true;

  case 100: // RPN LSB
    cs.rpnLsb = value;
    cs.mode = RPN_SELECTED;
    return true;

  case 99: // NRPN MSB
    cs.nrpnMsb = value;
    cs.mode = NRPN_SELECTED;
    return true;

  case 98: // NRPN LSB
    cs.nrpnLsb = value;
    cs.mode = NRPN_SELECTED;
    return true;

  case 121: // Reset all controllers.
    cs.wheel = 8192;
    cs.semitones = 2;
    cs.cents = 0;
    cs.rpnMsb = cs.rpnLsb = 127;
    cs.nrpnMsb = cs.nrpnLsb = 127;
    cs.mode = FRESH_SELECTOR;
    pruneStaleVoices();
    retuneChannel( channel );
    return true;

  case 6: // Data entry MSB: bend sensitivity in semitones.
    if ( cs.mode == RPN_SELECTED && cs.rpnMsb == 0 && cs.rpnLsb == 0 ) {
      cs.semitones = value;
      pruneStaleVoices();
      retuneChannel( channel );
    }
    // Under any other selection the data entry is consumed but does
    // nothing; it is never forwarded as an ordinary controller.
    return true;

  case 38: // Data entry LSB: bend sensitivity in cents (0 to 99).
    if ( cs.mode == RPN_SELECTED && cs.rpnMsb == 0 && cs.rpnLsb == 0 ) {
      if ( value > 99 )
        return false; // Out-of-range cents: keep the old sensitivity.
      cs.cents = value;
      pruneStaleVoices();
      retuneChannel( channel );
    }
    return true;

  default:
    // Ordinary controllers follow the original group path unchanged.
    voicer_.controlChange( controller, (StkFloat) value, channel );
    return true;
  }
}

} // stk namespace
