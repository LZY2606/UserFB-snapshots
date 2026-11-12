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

    by Perry R. Cook and Gary P. Scavone, 1995--2023.
*/
/***************************************************/

#include "FileLoop.h"
#include <cmath>

namespace stk {

FileLoop :: FileLoop( unsigned long chunkThreshold, unsigned long chunkSize )
  : FileWvIn( chunkThreshold, chunkSize ), phaseOffset_(0.0),
    wavLoopActive_( false ), wavLoopReleased_( false ), wavLoopExited_( false ),
    wavLoopAlternate_( false ), wavLoopA_( 0 ), wavLoopB_( 0 ), wavDirection_( 1 )
{
  Stk::addSampleRateAlert( this );
}

FileLoop :: FileLoop( std::string fileName, bool raw, bool doNormalize,
                      unsigned long chunkThreshold, unsigned long chunkSize,
                      bool doInt2FloatScaling )
  : FileWvIn( chunkThreshold, chunkSize ), phaseOffset_(0.0),
    wavLoopActive_( false ), wavLoopReleased_( false ), wavLoopExited_( false ),
    wavLoopAlternate_( false ), wavLoopA_( 0 ), wavLoopB_( 0 ), wavDirection_( 1 )
{
  this->openFile( fileName, raw, doNormalize, doInt2FloatScaling );
  Stk::addSampleRateAlert( this );
}

FileLoop :: ~FileLoop( void )
{
  Stk::removeSampleRateAlert( this );
}

void FileLoop :: openFile( std::string fileName, bool raw, bool doNormalize, bool doInt2FloatScaling )
{
  // Opening or closing a file drops any sampler-loop selection.
  wavLoopActive_ = false;
  wavLoopReleased_ = false;
  wavLoopExited_ = false;

  // Call close() in case another file is already open.
  this->closeFile();

  // Attempt to open the file ... an error might be thrown here.
  file_.open( fileName, raw );

  // Keep our own copy of the 'smpl' records: in non-chunking mode the
  // FileRead closes its handle below, which clears its metadata.
  wavLoops_ = file_.wavLoops();

  // Determine whether chunking or not.
  if ( file_.fileSize() > chunkThreshold_ ) {
    chunking_ = true;
    chunkPointer_ = 0;
    data_.resize( chunkSize_ + 1, file_.channels() );
  }
  else {
    chunking_ = false;
    data_.resize( file_.fileSize() + 1, file_.channels() );
  }

  if ( doInt2FloatScaling )
    int2floatscaling_ = true;
  else
    int2floatscaling_ = false;

  // Load all or part of the data.
  file_.read( data_, 0, int2floatscaling_ );

  if ( chunking_ ) { // If chunking, save the first sample frame for later.
    firstFrame_.resize( 1, data_.channels() );
    for ( unsigned int i=0; i<data_.channels(); i++ )
      firstFrame_[i] = data_[i];
  }
  else {  // If not chunking, copy the first sample frame to the last.
    for ( unsigned int i=0; i<data_.channels(); i++ )
      data_( data_.frames() - 1, i ) = data_[i];
  }

  // Resize our lastOutputs container.
  lastFrame_.resize( 1, file_.channels() );

  // Close the file unless chunking
  fileSize_ = file_.fileSize();
  if ( !chunking_ ) file_.close();

  // Set default rate based on file sampling rate.
  this->setRate( data_.dataRate() / Stk::sampleRate() );

  if ( doNormalize & !chunking_ ) this->normalize();

  this->reset();
}

void FileLoop :: closeFile( void )
{
  wavLoopActive_ = false;
  wavLoopReleased_ = false;
  wavLoopExited_ = false;
  wavLoops_.clear();
  FileWvIn::closeFile();
}

void FileLoop :: reset( void )
{
  if ( wavLoopActive_ ) this->restartWavLoop();
  else FileWvIn::reset();
}

void FileLoop :: restartWavLoop( void )
{
  time_ = (StkFloat) 0.0;
  wavDirection_ = 1;
  wavLoopReleased_ = false;
  wavLoopExited_ = false;
  finished_ = false;
  for ( unsigned int i=0; i<lastFrame_.size(); i++ ) lastFrame_[i] = 0.0;
}

void FileLoop :: setRate( StkFloat rate )
{
  if ( wavLoopActive_ && ( !std::isfinite( rate ) || rate < 0.0 ) ) {
    oStream_ << "FileLoop::setRate: WAV sampler-loop mode requires a finite, non-negative rate.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }

  rate_ = rate;

  if ( fmod( rate_, 1.0 ) != 0.0 ) interpolate_ = true;
  else interpolate_ = false;
}

void FileLoop :: addTime( StkFloat time )
{
  if ( wavLoopActive_ && time != 0.0 ) {
    oStream_ << "FileLoop::addTime: phase jumps are not supported while a WAV sampler loop is active.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }

  // Add an absolute time in samples.
  time_ += time;

  while ( time_ < 0.0 )
    time_ += fileSize_;
  while ( time_ >= fileSize_ )
    time_ -= fileSize_;
}

void FileLoop :: addPhase( StkFloat angle )
{
  if ( wavLoopActive_ && angle != 0.0 ) {
    oStream_ << "FileLoop::addPhase: phase jumps are not supported while a WAV sampler loop is active.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }

  // Add a time in cycles (one cycle = fileSize).
  time_ += fileSize_ * angle;

  while ( time_ < 0.0 )
    time_ += fileSize_;
  while ( time_ >= fileSize_ )
    time_ -= fileSize_;
}

void FileLoop :: addPhaseOffset( StkFloat angle )
{
  if ( wavLoopActive_ && angle != 0.0 ) {
    oStream_ << "FileLoop::addPhaseOffset: phase offsets are not supported while a WAV sampler loop is active.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }

  // Add a phase offset in cycles, where 1.0 = fileSize.
  phaseOffset_ = fileSize_ * angle;
}

void FileLoop :: useWavLoop( size_t index )
{
  if ( index >= wavLoops_.size() ) {
    oStream_ << "FileLoop::useWavLoop: loop index (" << index << ") is out of range.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }

  const FileRead::WavLoop& loop = wavLoops_[ index ];

  // Validate everything before touching playback state, so a failure
  // leaves the current playback position and mode untouched.
  if ( loop.type > 1 ) {
    oStream_ << "FileLoop::useWavLoop: loop type (" << loop.type << ") is not supported.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }
  if ( loop.fraction != 0.0 ) {
    oStream_ << "FileLoop::useWavLoop: fractional loop offsets are not supported.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }
  if ( loop.playCount != 0 ) {
    oStream_ << "FileLoop::useWavLoop: finite loop play counts are not supported.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }
  if ( loop.type == 1 && loop.start == loop.end ) {
    oStream_ << "FileLoop::useWavLoop: an alternating loop needs at least two distinct frames.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }
  if ( !std::isfinite( rate_ ) || rate_ < 0.0 ) {
    oStream_ << "FileLoop::useWavLoop: current rate must be finite and non-negative.";
    handleError( StkError::FUNCTION_ARGUMENT );
  }

  wavLoopActive_ = true;
  wavLoopAlternate_ = ( loop.type == 1 );
  wavLoopA_ = loop.start;
  wavLoopB_ = loop.end;
  this->restartWavLoop();
}

void FileLoop :: releaseWavLoop( void )
{
  if ( !wavLoopActive_ ) return;
  // Repeated releases change nothing; the current position is not moved.
  wavLoopReleased_ = true;
}

// Make sure the chunk window covers the inclusive physical frame range
// [needMin, needMax].  The window moves by overlapping chunks, exactly
// like the whole-file path; no per-loop iteration is involved.
void FileLoop :: ensureWavChunk( long needMin, long needMax )
{
  if ( !chunking_ ) return;

  bool moved = false;
  while ( needMax > (long) ( chunkPointer_ + chunkSize_ - 1 ) ) {
    chunkPointer_ += chunkSize_ - 1; // overlap chunks by one frame
    if ( chunkPointer_ + chunkSize_ > fileSize_ ) { // at end of file
      chunkPointer_ = fileSize_ - chunkSize_ + 1;   // leave extra frame at end of buffer
      for ( unsigned int j=0; j<firstFrame_.channels(); j++ )
        data_( data_.frames() - 1, j ) = firstFrame_[j];
    }
    moved = true;
  }
  while ( needMin < chunkPointer_ ) { // alternating loop on its return leg
    chunkPointer_ -= chunkSize_ - 1;
    if ( chunkPointer_ < 0 ) chunkPointer_ = 0;
    moved = true;
  }

  if ( moved ) file_.read( data_, chunkPointer_, int2floatscaling_ );
}

// Fetch one physical frame (all channels) outside the current window.
// Used only for the forward wrap interpolation between frames B and A;
// no loop audio is retained between ticks.
void FileLoop :: readWavFrame( unsigned long frame, StkFrames& oneFrame )
{
  oneFrame.resize( 1, data_.channels() );
  file_.read( oneFrame, frame, int2floatscaling_ );
}

StkFloat FileLoop :: tickWavLoop( unsigned int channel )
{
  if ( finished_ ) return 0.0;

  const StkFloat N = (StkFloat) fileSize_;
  const StkFloat A = (StkFloat) wavLoopA_;
  const StkFloat B = (StkFloat) wavLoopB_;

  // Once the position passes the last physical frame the tail is done.
  if ( time_ >= N ) {
    for ( unsigned int i=0; i<lastFrame_.size(); i++ ) lastFrame_[i] = 0.0;
    finished_ = true;
    return 0.0;
  }

  // Classify the current position and decide what to render.
  enum Region { ATTACK, LOOP, TAIL };
  Region region;
  if ( wavLoopExited_ )
    region = TAIL;                       // loop left, playing the file tail
  else if ( time_ < A )
    region = ATTACK;                     // file head before the loop start
  else
    region = LOOP;

  StkFloat p = time_;                    // physical fractional frame to render
  long needMin = (long) std::floor( p );
  long needMax = needMin + 1;
  bool wrapPair = false;                 // interpolate across B -> A of a forward loop

  if ( region == LOOP && !wavLoopAlternate_ ) {
    // Forward loop: fold onto A..B with the fraction preserved.
    StkFloat span = B - A + 1.0;
    StkFloat off = std::fmod( time_ - A, span );
    p = A + off;
    needMin = (long) std::floor( p );
    if ( needMin == (long) wavLoopB_ && p > B ) {
      // Fractional slot past B: the next unfolded frame is A, so the
      // interpolation must cross the B -> A wrap, not read physical B+1.
      wrapPair = true;
      needMin = needMax = (long) wavLoopB_;
    }
    else if ( needMin == (long) wavLoopB_ ) {
      needMax = (long) wavLoopB_;        // exactly B
    }
    else needMax = needMin + 1;
  }
  else if ( region == LOOP ) {
    // Alternating loop: time_ is kept folded inside [A,B] after every
    // advance, so the interpolation pair are adjacent physical frames.
    p = time_;
    needMin = (long) std::floor( p );
    needMax = needMin + 1;
    if ( needMax > (long) wavLoopB_ ) needMax = (long) wavLoopB_;
  }
  else {
    // Attack or tail: ordinary linear playback; clamp to the last frame.
    p = time_;
    needMin = (long) std::floor( p );
    needMax = needMin + 1;
    if ( needMax >= (long) fileSize_ ) needMax = (long) fileSize_ - 1;
    if ( needMin >= (long) fileSize_ ) needMin = (long) fileSize_ - 1;
    if ( p > N - 1.0 ) p = N - 1.0;
  }

  if ( wrapPair )
    this->ensureWavChunk( (long) wavLoopB_, (long) wavLoopB_ );
  else
    this->ensureWavChunk( needMin, needMax );

  if ( wrapPair ) {
    // Linear interpolation between the last loop frame B and the first, A.
    // The window may be too small to hold both ends of a wide loop, so in
    // chunking mode frame A is fetched with its own single-frame read.
    StkFloat alpha = p - B;              // in [0, 1)
    StkFrames frameA( 1, data_.channels() );
    if ( chunking_ )
      this->readWavFrame( wavLoopA_, frameA );
    else
      for ( unsigned int i=0; i<lastFrame_.channels(); i++ )
        frameA[i] = data_( (size_t) wavLoopA_, i );
    size_t bIndex = chunking_ ? (size_t) ( (long) wavLoopB_ - chunkPointer_ )
                              : (size_t) wavLoopB_;
    for ( unsigned int i=0; i<lastFrame_.channels(); i++ )
      lastFrame_[i] = ( 1.0 - alpha ) * data_( bIndex, i ) + alpha * frameA[i];
  }
  else {
    StkFloat tyme = chunking_ ? p - (StkFloat) chunkPointer_ : p;
    // Interpolate whenever the rendered position is fractional, regardless
    // of the current rate: a zero rate must hold a fractional position at
    // its exact value, and an integer rate can still leave a fractional
    // position behind after a rate change.
    StkFloat frac = tyme - std::floor( tyme );
    if ( frac != 0.0 ) {
      for ( unsigned int i=0; i<lastFrame_.size(); i++ )
        lastFrame_[i] = data_.interpolate( tyme, i );
    }
    else {
      for ( unsigned int i=0; i<lastFrame_.size(); i++ )
        lastFrame_[i] = data_( (size_t) tyme, i );
    }
  }

  // A zero rate holds the current path position and direction exactly.
  if ( rate_ == 0.0 ) return lastFrame_[ channel ];

  // Advance the unfolded path position.  Large steps are folded with
  // modulo arithmetic, never by iterating around the loop one turn at
  // a time, and no unfolded physical index is ever handed to data_.
  const StkFloat r = rate_;
  if ( region == ATTACK ) {
    StkFloat next = time_ + r;
    // A release requested before the loop was ever reached skips the
    // loop entirely: crossing A enters the tail without a discontinuity.
    if ( wavLoopReleased_ && next >= A ) wavLoopExited_ = true;
    time_ = next;
  }
  else if ( region == TAIL ) {
    time_ += r;
  }
  else if ( !wavLoopAlternate_ ) {
    StkFloat span = B - A + 1.0;
    StkFloat next = time_ + r;
    if ( wavLoopReleased_ && next >= B ) {
      // Next forward arrival at B: keep the overshoot and enter the tail.
      time_ = next;
      wavLoopExited_ = true;
    }
    else {
      time_ = A + std::fmod( next - A, span );
    }
  }
  else {
    StkFloat L = B - A;                  // >= 1 for an alternating loop
    StkFloat cycle = 2.0 * L;
    // Reconstruct the monotonic phase (rate is always non-negative here):
    // ascending legs occupy [0, L] of the cycle, descending legs (L, 2L).
    StkFloat q = time_ - A;
    StkFloat u = ( wavDirection_ > 0 ) ? q : cycle - q;
    StkFloat step = u + r;

    if ( wavLoopReleased_ ) {
      // Distance, along the unfolded path, to the next forward arrival
      // at B.  A descending leg first has to return to A before climbing
      // back to B, so no direct jump out of the return leg is possible.
      StkFloat toForwardB;
      if ( u <= L ) toForwardB = L - u;                  // already climbing
      else          toForwardB = cycle - u + L;          // finish the return leg first
      if ( r >= toForwardB ) {
        // The fractional overshoot past B becomes the offset into B+1.
        time_ = B + ( r - toForwardB );
        wavLoopExited_ = true;
        return lastFrame_[ channel ];
      }
    }

    StkFloat c = std::fmod( step, cycle );
    if ( c < 0.0 ) c += cycle;
    if ( c <= L ) {                      // ascending leg, A and B occur once
      time_ = A + c;
      wavDirection_ = 1;
    }
    else {                               // descending leg
      time_ = A + ( cycle - c );
      wavDirection_ = -1;
    }
  }

  return lastFrame_[ channel ];
}

StkFloat FileLoop :: tick( unsigned int channel )
{
#if defined(_STK_DEBUG_)
  if ( channel >= data_.channels() ) {
    oStream_ << "FileLoop::tick(): channel argument and soundfile data are incompatible!";
    handleError( StkError::FUNCTION_ARGUMENT );
  }
#endif

  if ( wavLoopActive_ ) return tickWavLoop( channel );

  if ( finished_ ) return 0.0;

  // Check limits of time address ... if necessary, recalculate modulo
  // fileSize.
  while ( time_ < 0.0 )
    time_ += fileSize_;
  while ( time_ >= fileSize_ )
    time_ -= fileSize_;

  StkFloat tyme = time_;
  if ( phaseOffset_ ) {
    tyme += phaseOffset_;
    while ( tyme < 0.0 )
      tyme += fileSize_;
    while ( tyme >= fileSize_ )
      tyme -= fileSize_;
  }

  if ( chunking_ ) {

    // Check the time address vs. our current buffer limits.
    if ( ( time_ < (StkFloat) chunkPointer_ ) ||
         ( time_ > (StkFloat) ( chunkPointer_ + chunkSize_ - 1 ) ) ) {

      while ( time_ < (StkFloat) chunkPointer_ ) { // negative rate
        chunkPointer_ -= chunkSize_ - 1; // overlap chunks by one frame
        if ( chunkPointer_ < 0 ) chunkPointer_ = 0;
      }
      while ( time_ > (StkFloat) ( chunkPointer_ + chunkSize_ - 1 ) ) { // positive rate
        chunkPointer_ += chunkSize_ - 1; // overlap chunks by one frame
        if ( chunkPointer_ + chunkSize_ > fileSize_ ) { // at end of file
          chunkPointer_ = fileSize_ - chunkSize_ + 1; // leave extra frame at end of buffer
          // Now fill extra frame with first frame data.
          for ( unsigned int j=0; j<firstFrame_.channels(); j++ )
            data_( data_.frames() - 1, j ) = firstFrame_[j];
        }
      }

      // Load more data.
      file_.read( data_, chunkPointer_, int2floatscaling_ );
    }

    // Adjust index for the current buffer.
    tyme -= chunkPointer_;
  }

  if ( interpolate_ ) {
    for ( unsigned int i=0; i<lastFrame_.size(); i++ )
      lastFrame_[i] = data_.interpolate( tyme, i );
  }
  else {
    for ( unsigned int i=0; i<lastFrame_.size(); i++ )
      lastFrame_[i] = data_( (size_t) tyme, i );
  }

  // Increment time, which can be negative.
  time_ += rate_;

  return lastFrame_[channel];
}

StkFrames& FileLoop :: tick( StkFrames& frames, unsigned int channel)
{
  unsigned int nChannels = lastFrame_.channels();
#if defined(_STK_DEBUG_)
  if ( channel > frames.channels() - nChannels ) {
    oStream_ << "FileLoop::tick(): channel and StkFrames arguments are incompatible!";
    handleError( StkError::FUNCTION_ARGUMENT );
  }
#endif

  if ( finished_ ) {
    // With no sampler loop selected (e.g. the file was closed) keep the
    // historical behavior of leaving the StkFrames argument untouched.
    if ( !wavLoopActive_ ) return frames;

    // A released sampler loop has played its last frame: fill the target
    // channels with zeros while leaving any surrounding channels untouched.
    StkFloat *samples = &frames[channel];
    unsigned int j, hop = frames.channels() - nChannels;
    for ( unsigned int i=0; i<frames.frames(); i++, samples += hop ) {
      for ( j=0; j<nChannels; j++ )
        *samples++ = 0.0;
    }
    return frames;
  }

  StkFloat *samples = &frames[channel];
  unsigned int j, hop = frames.channels() - nChannels;
  if ( nChannels == 1 ) {
    for ( unsigned int i=0; i<frames.frames(); i++, samples += hop )
      *samples++ = tick();
  }
  else {
    for ( unsigned int i=0; i<frames.frames(); i++, samples += hop ) {
      *samples++ = tick();
      for ( j=1; j<nChannels; j++ )
        *samples++ = lastFrame_[j];
    }
  }
  return frames;

}

} // stk namespace
