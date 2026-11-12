#ifndef STK_FILELOOP_H
#define STK_FILELOOP_H

#include "FileWvIn.h"
#include <vector>

namespace stk {

/***************************************************/
/*! \class FileLoop
    \brief STK file looping / oscillator class.

    This class provides audio file looping functionality.  Any audio
    file that can be loaded by FileRead can be looped using this
    class.

    FileLoop supports multi-channel data.  It is important to
    distinguish the tick() method that computes a single frame (and
    returns only the specified sample of a multi-channel frame) from
    the overloaded one that takes an StkFrames object for
    multi-channel and/or multi-frame data.

    By default the whole file is looped forever.  A WAV file with a
    'smpl' chunk additionally offers sampler sustain loops, which can
    be selected with useWavLoop(): playback then runs from the file
    start through the attack, stays inside the chosen loop, and after
    releaseWavLoop() plays the natural file tail before stopping.
    Only forward (type 0) and alternating (type 1) loops with zero
    fractional offset and zero play count can be activated; other
    records remain visible through FileRead::wavLoops() but are
    rejected here.

    by Perry R. Cook and Gary P. Scavone, 1995--2023.
*/
/***************************************************/

class FileLoop : protected FileWvIn
{
 public:
  //! Default constructor.
  FileLoop( unsigned long chunkThreshold = 1000000, unsigned long chunkSize = 1024 );

  //! Class constructor that opens a specified file.
  FileLoop( std::string fileName, bool raw = false, bool doNormalize = true,
            unsigned long chunkThreshold = 1000000, unsigned long chunkSize = 1024,
            bool doInt2FloatScaling = true );

  //! Class destructor.
  ~FileLoop( void );

  //! Open the specified file and load its data.
  /*!
    Data from a previously opened file will be overwritten by this
    function.  An StkError will be thrown if the file is not found,
    its format is unknown, or a read error occurs.  If the file length
    is less than the chunkThreshold limit and \e doNormalize is true,
    the file data will be normalized with respect to the maximum absolute
    value of the data. If the \e doInt2FloatScaling flag is true and the
    input data is fixed-point, a scaling will be applied with respect to
    the fixed-point limits.
  */
  void openFile( std::string fileName, bool raw = false, bool doNormalize = true, bool doInt2FloatScaling = true );

  //! Close a file if one is open.
  /*!
    Any WAV sampler-loop selection is dropped.
  */
  void closeFile( void );

  //! Clear outputs and reset time (file) pointer to zero.
  /*!
    If a WAV sampler loop is active, the loop selection is kept and
    playback restarts from the beginning of the file (the attack),
    as if useWavLoop() had just been called.
  */
  void reset( void );

  //! Return the number of audio channels in the data or stream.
  unsigned int channelsOut( void ) const { return data_.channels(); };

  //! Normalize data to a maximum of +-1.0.
  /*!
    This function has no effect when data is incrementally loaded
    from disk.
  */
  void normalize( void ) { FileWvIn::normalize( 1.0 ); };

  //! Normalize data to a maximum of \e +-peak.
  /*!
    This function has no effect when data is incrementally loaded
    from disk.
  */
  void normalize( StkFloat peak ) { FileWvIn::normalize( peak ); };

  //! Return the file size in sample frames.
  //unsigned long getSize( void ) const { return data_.frames(); };
  unsigned long getSize( void ) const { return fileSize_; };

  //! Select a WAV 'smpl' sampler loop and restart from the file head.
  /*!
    The \e index refers to the records returned by
    FileRead::wavLoops().  On success the record is activated, the
    attack restarts at frame 0 and playback proceeds towards the
    loop; an StkError is thrown on an invalid index or a record that
    is not currently supported, in which case the playback state is
    left untouched.  Activatable records must be type 0 (forward) or
    type 1 (alternating), have a zero fractional offset and a zero
    play count, and an alternating loop must span at least two
    distinct frames.
  */
  void useWavLoop( size_t index );

  //! Request the natural release of the active WAV sampler loop.
  /*!
    The current playback position is not moved.  If the attack is
    still playing, it simply continues to the file end without ever
    looping.  Inside the loop, playback continues until it next
    reaches the loop end in the forward direction, including finishing
    a return leg of an alternating loop, and then continues into the
    file tail.  Fractional position is preserved across the transition
    and repeated calls have no additional effect.  Has no effect when
    no sampler loop is active.
  */
  void releaseWavLoop( void );

  //! Query whether playback has finished.
  /*!
    Returns \e true only when a sampler loop has been released and the
    last frame of the file has been played; subsequent ticks output
    zeros.  The default whole-file looping mode never finishes.
  */
  bool isFinished( void ) const { return finished_; };

  //! Return the input file sample rate in Hz (not the data read rate).
  /*!
    WAV, SND, and AIF formatted files specify a sample rate in
    their headers.  STK RAW files have a sample rate of 22050 Hz
    by definition.  MAT-files are assumed to have a rate of 44100 Hz.
  */
  StkFloat getFileRate( void ) const { return data_.dataRate(); };

  //! Set the data read rate in samples.  The rate can be negative.
  /*!
    If the rate value is negative, the data is read in reverse order.
    Negative rates are only allowed in the default whole-file looping
    mode: while a WAV sampler loop is active the rate must be finite
    and non-negative, otherwise an StkError is thrown.
  */
  void setRate( StkFloat rate );

  //! Set the data interpolation rate based on a looping frequency.
  /*!
    This function determines the interpolation rate based on the file
    size and the current Stk::sampleRate.  The \e frequency value
    corresponds to file cycles per second.  The frequency can be
    negative in the default whole-file mode, in which case the loop is
    read in reverse order; while a WAV sampler loop is active the
    resulting rate must be finite and non-negative (see setRate()).
  */
  void setFrequency( StkFloat frequency ) { this->setRate( fileSize_ * frequency / Stk::sampleRate() ); };

  //! Increment the read pointer by \e time samples, modulo file size.
  /*!
    An StkError is thrown for a non-zero argument while a WAV sampler
    loop is active, where whole-file phase arithmetic does not apply.
  */
  void addTime( StkFloat time );

  //! Increment current read pointer by \e angle, relative to a looping frequency.
  /*!
    This function increments the read pointer based on the file
    size and the current Stk::sampleRate.  The \e anAngle value
    is a multiple of file size.  An StkError is thrown for a non-zero
    argument while a WAV sampler loop is active.
  */
  void addPhase( StkFloat angle );

  //! Add a phase offset to the current read pointer.
  /*!
    This function determines a time offset based on the file
    size and the current Stk::sampleRate.  The \e angle value
    is a multiple of file size.  An StkError is thrown for a non-zero
    argument while a WAV sampler loop is active.
  */
  void addPhaseOffset( StkFloat angle );

  //! Return the specified channel value of the last computed frame.
  /*!
    For multi-channel files, use the lastFrame() function to get
    all values from the last computed frame.  If no file data is
    loaded, the returned value is 0.0.  The \c channel argument must
    be less than the number of channels in the file data (the first
    channel is specified by 0).  However, range checking is only
    performed if _STK_DEBUG_ is defined during compilation, in which
    case an out-of-range value will trigger an StkError exception.
  */
  StkFloat lastOut( unsigned int channel = 0 ) { return FileWvIn::lastOut( channel ); };

  //! Compute a sample frame and return the specified \c channel value.
  /*!
    For multi-channel files, use the lastFrame() function to get
    all values from the computed frame.  If no file data is loaded,
    the returned value is 0.0.  The \c channel argument must be less
    than the number of channels in the file data (the first channel is
    specified by 0).  However, range checking is only performed if
    _STK_DEBUG_ is defined during compilation, in which case an
    out-of-range value will trigger an StkError exception.
  */
  StkFloat tick( unsigned int channel = 0 );

  //! Fill the StkFrames object with computed sample frames, starting at the specified channel and return the same reference.
  /*!
    The \c channel argument plus the number of output channels must
    be less than the number of channels in the StkFrames argument (the
    first channel is specified by 0).  However, range checking is only
    performed if _STK_DEBUG_ is defined during compilation, in which
    case an out-of-range value will trigger an StkError exception.
  */
  virtual StkFrames& tick( StkFrames& frames,unsigned int channel = 0 );

 protected:

  StkFrames firstFrame_;
  StkFloat phaseOffset_;

  // WAV 'smpl' sampler-loop state.
  std::vector<FileRead::WavLoop> wavLoops_; // local copy of the file's loop records
  bool wavLoopActive_;       // a sampler loop has been selected
  bool wavLoopReleased_;     // releaseWavLoop() has been called
  bool wavLoopExited_;       // playback has left the loop into the tail
  bool wavLoopAlternate_;    // true = ping-pong (type 1), false = forward
  unsigned long wavLoopA_;   // loop start frame, inclusive
  unsigned long wavLoopB_;   // loop end frame, inclusive
  long wavDirection_;        // +1 forward, -1 backward, only meaningful in loop

  // Restart playback from the file head while keeping the loop choice.
  void restartWavLoop( void );

  // Move the overlapping chunk window until the inclusive physical
  // frame range [needMin, needMax] is buffered, then reload it.
  void ensureWavChunk( long needMin, long needMax );

  // Read a single frame directly from the open, chunking file.
  void readWavFrame( unsigned long frame, StkFrames& oneFrame );

  // Sample-level implementation used while a sampler loop is active.
  StkFloat tickWavLoop( unsigned int channel );

};

} // stk namespace

#endif
