#ifndef STK_MIDICHANNELVOICER_H
#define STK_MIDICHANNELVOICER_H

#include "Voicer.h"
#include <map>

namespace stk {

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

class MidiChannelVoicer : public Stk
{
 public:
  //! Constructor.
  /*!
    The supplied Voicer is borrowed, not owned: it and the instruments
    registered with it must outlive this object.  MIDI channels 0 to 15
    correspond to Voicer groups with the same numbers.
  */
  MidiChannelVoicer( Voicer& voicer );

  //! Initiate a noteOn on the given MIDI channel (0 to 15).
  /*!
    The note number must be in the range 0 to 127 and the velocity in
    the range 1 to 127.  The note is sounded through the wrapped Voicer
    on the group matching the channel, with the channel's current bend
    offset already applied.  The Voicer's note tag is returned, or -1
    if the arguments are invalid or no voice is available.
  */
  long noteOn( int channel, int note, int velocity );

  //! Release the voice with the given note tag using the Voicer's decay time.
  /*!
    Returns true if the tag still belongs to a voice of the wrapped
    Voicer and false otherwise (for example, after the voice has been
    preempted or fully released).  A stale tag can never retune or
    release a voice that has since been reassigned.
  */
  bool noteOff( long tag, int velocity );

  //! Set the 14-bit pitch-bend wheel value (0 to 16383) of a channel.
  /*!
    All resident voices of the channel, including voices still in
    their release tail, are retuned immediately.  Returns false for an
    invalid channel or wheel value without changing any sound.
  */
  bool pitchBend( int channel, int wheel14 );

  //! Process a MIDI control change (controller 0 to 121, value 0 to 127).
  /*!
    RPN selection (CC101/CC100/CC99/CC98) and data entry (CC6/CC38)
    are interpreted per channel to update bend sensitivity; CC121
    resets the wheel and sensitivity of the channel and clears the
    parameter selectors.  All other controllers are forwarded to the
    matching group through the wrapped Voicer.  Returns false for an
    invalid channel, controller or value without changing any sound.
  */
  bool controlChange( int channel, int controller, int value );

 private:

  enum SelectorMode {
    RPN_SELECTED,    //!< CC101/CC100 last used (a concrete RPN or the 127/127 null choice).
    NRPN_SELECTED,   //!< CC99/CC98 last used; data entry must not touch RPN state.
    FRESH_SELECTOR   //!< Nothing selected yet; data entry is ignored.
  };

  struct ChannelState {
    int wheel;            //!< 14-bit wheel value, 8192 at center.
    int semitones;        //!< Bend sensitivity in semitones.
    int cents;            //!< Bend sensitivity in cents (0 to 99).
    int rpnMsb;           //!< Last CC101 value.
    int rpnLsb;           //!< Last CC100 value.
    int nrpnMsb;          //!< Last CC99 value.
    int nrpnLsb;          //!< Last CC98 value.
    SelectorMode mode;    //!< Which selector family was used most recently.

    ChannelState()
      :wheel( 8192 ), semitones( 2 ), cents( 0 ),
       rpnMsb( 127 ), rpnLsb( 127 ), nrpnMsb( 127 ), nrpnLsb( 127 ),
       mode( FRESH_SELECTOR ) {}
  };

  struct ActiveVoice {
    int channel;
    StkFloat baseNote; //!< Original note number from noteOn, before bending.
  };

  //! Recompute and apply the bent note number for every active voice of a channel.
  void retuneChannel( int channel );

  //! Bent note number for a base note under the given wheel and sensitivity.
  static StkFloat bentNote( StkFloat baseNote, int wheel, StkFloat rangeSemitones );

  //! Drop active records whose tags no longer belong to the wrapped Voicer.
  void pruneStaleVoices( void );

  Voicer& voicer_;
  ChannelState channels_[16];
  std::map<long, ActiveVoice> activeVoices_;
};

} // stk namespace

#endif
