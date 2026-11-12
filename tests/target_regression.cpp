// Regression tests for WAV 'smpl' sustain loops in FileRead / FileLoop.
//
// Three groups are exercised:
//   uf_metadata  - 'smpl' parsing, validation and record lifetime
//   uf_sustain   - point-wise attack/loop/release behavior vs. an
//                  independently written unfolded-path reference
//   uf_storage   - whole-file vs. 4-frame chunked loading, scalar vs.
//                  StkFrames batch entry points, sentinel channels and
//                  reuse of a FileLoop instance
//
// All fixtures are generated in C++ at runtime; no external files needed.

#include "FileLoop.h"
#include "FileRead.h"
#include "harness.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using stk::StkFloat;

// Create an isolated scratch directory inside the current working tree.
std::string makeTempDir( const char* tag )
{
  char cwd[4096];
  UF_CHECK( getcwd( cwd, sizeof( cwd ) ) != nullptr );
  std::string dir = std::string( cwd ) + "/" + tag + "_" +
                    std::to_string( (long) getpid() );
  UF_CHECK( mkdir( dir.c_str(), 0700 ) == 0 );
  return dir;
}

void removeTempDir( const std::string& dir )
{
  std::error_code ec;
  std::filesystem::remove_all( dir, ec );
  UF_CHECK( !ec );
}

// ----------------------------- WAV fixtures -----------------------------

struct SmplRec {
  unsigned long identifier, type, start, end;
  StkFloat fraction;
  unsigned long playCount;
};

void put16( std::vector<unsigned char>& b, int v )
{
  b.push_back( (unsigned char) ( v & 0xff ) );
  b.push_back( (unsigned char) ( ( v >> 8 ) & 0xff ) );
}

void put32( std::vector<unsigned char>& b, unsigned long v )
{
  for ( int i=0; i<4; i++ ) b.push_back( (unsigned char) ( ( v >> ( 8*i ) ) & 0xff ) );
}

void fourcc( std::vector<unsigned char>& b, const char* id )
{
  for ( int i=0; i<4; i++ ) b.push_back( (unsigned char) id[i] );
}

void appendSmpl( std::vector<unsigned char>& b, const std::vector<SmplRec>& loops )
{
  const unsigned long samplerData = 0;
  unsigned long chunkSize = 36 + 24 * (unsigned long) loops.size() + samplerData;
  fourcc( b, "smpl" );
  put32( b, chunkSize );
  put32( b, 0 );          // manufacturer
  put32( b, 0 );          // product
  put32( b, 44100 );      // sample period
  put32( b, 60 );         // MIDI note
  put32( b, 0 );          // pitch fraction
  put32( b, 0 );          // SMPTE format
  put32( b, 0 );          // SMPTE offset
  put32( b, (unsigned long) loops.size() );
  put32( b, samplerData );
  for ( size_t i=0; i<loops.size(); i++ ) {
    put32( b, loops[i].identifier );
    put32( b, loops[i].type );
    put32( b, loops[i].start );
    put32( b, loops[i].end );
    put32( b, (unsigned long) ( loops[i].fraction * 4294967296.0 ) );
    put32( b, loops[i].playCount );
  }
}

// An odd-sized unknown chunk, including its mandatory pad byte.
void appendOddJunk( std::vector<unsigned char>& b )
{
  fourcc( b, "junk" );
  put32( b, 3 );
  b.push_back( 0x55 );
  b.push_back( 0x66 );
  b.push_back( 0x77 );
  b.push_back( 0x00 ); // pad
}

// Build a stereo 16-bit PCM WAV with N clearly distinct frames.  The
// 'smpl' chunk may be placed before or after the data chunk, and an
// odd-sized unknown chunk can be sprinkled in to exercise padding.
std::string makeWav( const std::string& path, unsigned long nFrames,
                     const std::vector<SmplRec>& loops,
                     bool smplBeforeData, bool oddJunk = false )
{
  std::vector<unsigned char> b;
  fourcc( b, "RIFF" );
  size_t riffSizePos = b.size();
  put32( b, 0 ); // patched later
  fourcc( b, "WAVE" );

  fourcc( b, "fmt " );
  put32( b, 16 );
  put16( b, 1 );                 // PCM
  put16( b, 2 );                 // stereo
  put32( b, 44100 );             // sample rate
  put32( b, 44100 * 2 * 2 );     // byte rate
  put16( b, 4 );                 // block align
  put16( b, 16 );                // bits per sample

  if ( smplBeforeData ) {
    if ( oddJunk ) appendOddJunk( b );
    appendSmpl( b, loops );
  }

  fourcc( b, "data" );
  put32( b, nFrames * 2 * 2 );
  for ( unsigned long i=0; i<nFrames; i++ ) {
    put16( b, (int) ( 100 + 10 * i ) );   // left channel
    put16( b, (int) ( 900 - 20 * i ) );   // right channel
  }

  if ( !smplBeforeData ) {
    if ( oddJunk ) appendOddJunk( b );
    appendSmpl( b, loops );
  }

  unsigned long riffSize = (unsigned long) b.size() - 8;
  b[riffSizePos+0] = (unsigned char) ( riffSize & 0xff );
  b[riffSizePos+1] = (unsigned char) ( ( riffSize >> 8 ) & 0xff );
  b[riffSizePos+2] = (unsigned char) ( ( riffSize >> 16 ) & 0xff );
  b[riffSizePos+3] = (unsigned char) ( ( riffSize >> 24 ) & 0xff );

  std::ofstream f( path, std::ios::binary | std::ios::trunc );
  f.write( (const char*) b.data(), (std::streamsize) b.size() );
  f.close();
  return path;
}

// Write raw bytes, used to craft structurally invalid 'smpl' chunks.
std::string writeRaw( const std::string& path, const std::vector<unsigned char>& b )
{
  std::ofstream f( path, std::ios::binary | std::ios::trunc );
  f.write( (const char*) b.data(), (std::streamsize) b.size() );
  f.close();
  return path;
}

// A valid WAV whose 'smpl' chunk size field has been tampered with.
std::string makeBadSmplWav( const std::string& path, unsigned long nFrames,
                            unsigned long declaredLoops, unsigned long actualLoops )
{
  std::vector<unsigned char> b;
  fourcc( b, "RIFF" ); put32( b, 0 ); fourcc( b, "WAVE" );
  fourcc( b, "fmt " ); put32( b, 16 );
  put16( b, 1 ); put16( b, 2 ); put32( b, 44100 ); put32( b, 176400 );
  put16( b, 4 ); put16( b, 16 );
  fourcc( b, "data" ); put32( b, nFrames * 4 );
  for ( unsigned long i=0; i<nFrames; i++ ) { put16( b, 10 ); put16( b, 20 ); }

  fourcc( b, "smpl" );
  put32( b, 36 + 24 * declaredLoops ); // size claims declaredLoops records
  for ( int i=0; i<7; i++ ) put32( b, 0 ); // seven header fields before the count
  put32( b, declaredLoops );
  put32( b, 0 );
  for ( unsigned long i=0; i<actualLoops; i++ )
    for ( int j=0; j<6; j++ ) put32( b, 0 );

  unsigned long riffSize = (unsigned long) b.size() - 8;
  b[4] = (unsigned char) riffSize; b[5] = (unsigned char) ( riffSize >> 8 );
  b[6] = (unsigned char) ( riffSize >> 16 ); b[7] = (unsigned char) ( riffSize >> 24 );
  return writeRaw( path, b );
}

double leftVal( long frame )  { return 100.0 + 10.0 * frame; }
double rightVal( long frame ) { return 900.0 - 20.0 * frame; }

double lerp( double a, double b, double alpha ) { return a + alpha * ( b - a ); }

bool opensAndFails( const std::string& path )
{
  stk::FileRead fr;
  try { fr.open( path ); }
  catch ( stk::StkError& ) {
    // The throw is the failure signal; the record set must have been
    // cleared by the aborted open either way.
    return fr.wavLoops().empty();
  }
  return false;
}

bool throwsStkError( void (*fn)( void* ), void* arg )
{
  try { fn( arg ); }
  catch ( stk::StkError& ) { return true; }
  return false;
}

// ----------------------- independent path reference ----------------------

// Mirrors the specified unfolded sample path (attack, forward or
// alternating loop, released tail) without sharing any FileLoop code.
struct RefLoop {
  long N, A, B;
  bool alt;
  StkFloat rate;
  StkFloat time = 0.0;
  int dir = 1;
  bool released = false, exited = false, finished = false;

  void render( double out[2] )
  {
    if ( finished || time >= N ) {
      finished = true; out[0] = out[1] = 0.0; return;
    }

    bool inLoop = !exited && time >= A;
    double p;
    long i0, i1;
    if ( !inLoop ) {
      p = std::min( time, (double) N - 1.0 );
      i0 = (long) std::floor( p );
      i1 = std::min( i0 + 1, N - 1 );
    }
    else if ( !alt ) {
      double span = B - A + 1.0;
      double off = std::fmod( time - A, span );
      p = A + off;
      i0 = (long) std::floor( p );
      if ( i0 == B && p > B ) { i0 = B; i1 = A; }    // B -> A wrap pair
      else i1 = std::min( i0 + 1, B );
    }
    else {
      p = time;
      i0 = (long) std::floor( p );
      i1 = std::min( i0 + 1, B );
    }

    double alpha = p - std::floor( p );
    out[0] = lerp( leftVal( i0 ), leftVal( i1 ), alpha );
    out[1] = lerp( rightVal( i0 ), rightVal( i1 ), alpha );

    if ( rate == 0.0 ) return;

    if ( !inLoop ) {
      double nt = time + rate;
      if ( released && !exited && nt >= A ) exited = true;
      time = nt;
    }
    else if ( !alt ) {
      double nt = time + rate;
      if ( released && nt >= B ) { time = nt; exited = true; }
      else time = A + std::fmod( nt - A, B - A + 1.0 );
    }
    else {
      double L = B - A, cyc = 2.0 * L;
      double q = time - A;
      double u = ( dir > 0 ) ? q : cyc - q;
      if ( released ) {
        double toB = ( u <= L ) ? ( L - u ) : ( cyc - u + L );
        if ( rate >= toB ) { time = B + ( rate - toB ); exited = true; return; }
      }
      double c = std::fmod( u + rate, cyc );
      if ( c < 0.0 ) c += cyc;
      if ( c <= L ) { time = A + c; dir = 1; }
      else { time = A + ( cyc - c ); dir = -1; }
    }
  }
};

const long kFrames = 12;
const long kFwdA = 3, kFwdB = 7;
const long kAltA = 4, kAltB = 9;

std::vector<SmplRec> standardLoops()
{
  return {
    { 7, 0, (unsigned long) kFwdA, (unsigned long) kFwdB, 0.0, 0 }, // forward
    { 9, 1, (unsigned long) kAltA, (unsigned long) kAltB, 0.0, 0 }, // alternating
    { 3, 3, 1, 2, 0.0, 0 }                                          // unsupported type
  };
}

void expectClose( double got, double want )
{
  UF_CHECK( std::fabs( got - want ) < 1e-8 );
}

// Drive a FileLoop with scalar ticks and compare every channel against
// the independent reference for nTicks output frames.
void compareScalar( stk::FileLoop& fl, RefLoop& ref, unsigned nTicks )
{
  for ( unsigned i=0; i<nTicks; i++ ) {
    double want[2];
    ref.render( want );
    StkFloat gotL = fl.tick( 0 );
    StkFloat gotR = fl.lastOut( 1 );
    expectClose( gotL, want[0] );
    expectClose( gotR, want[1] );
    UF_CHECK( fl.channelsOut() == 2 );
  }
}

} // namespace

// ------------------------------- metadata --------------------------------

void uf_metadata()
{
  stk::Stk::setSampleRate( 44100.0 );
  stk::Stk::printErrors( false );

  std::string dir = makeTempDir( ".uf_meta" );

  std::string before = makeWav( dir + "/before.wav", kFrames, standardLoops(), true, true );
  std::string after  = makeWav( dir + "/after.wav",  kFrames, standardLoops(), false, true );
  std::string plain  = makeWav( dir + "/plain.wav",  kFrames, {}, false );

  for ( int layout=0; layout<2; layout++ ) {
    const std::string& path = layout == 0 ? before : after;
    stk::FileRead fr( path );
    UF_CHECK( fr.isOpen() );
    UF_CHECK( fr.fileSize() == (unsigned long) kFrames );
    std::vector<stk::FileRead::WavLoop> loops = fr.wavLoops();
    UF_CHECK( loops.size() == 3 );
    UF_CHECK( loops[0].identifier == 7 && loops[0].type == 0 );
    UF_CHECK( loops[0].start == (unsigned long) kFwdA && loops[0].end == (unsigned long) kFwdB );
    UF_CHECK( loops[0].fraction == 0.0 && loops[0].playCount == 0 );
    UF_CHECK( loops[1].identifier == 9 && loops[1].type == 1 );
    UF_CHECK( loops[1].start == (unsigned long) kAltA && loops[1].end == (unsigned long) kAltB );
    // The unsupported-but-legal type stays queryable and is not rewritten
    // into a forward loop.
    UF_CHECK( loops[2].type == 3 && loops[2].start == 1 && loops[2].end == 2 );

    // The returned set is an independent copy.
    loops.clear();
    UF_CHECK( fr.wavLoops().size() == 3 );

    // Metadata scanning must not move the PCM read offset.
    stk::StkFrames buf( kFrames, 2 );
    fr.read( buf, 0, false );
    for ( long i=0; i<kFrames; i++ ) {
      expectClose( buf( (size_t) i, 0 ), leftVal( i ) );
      expectClose( buf( (size_t) i, 1 ), rightVal( i ) );
    }

    // An unsupported type is visible to FileLoop but cannot be activated.
    stk::FileLoop fl( path, false, false, 1000000, 1024, false );
    bool threw = false;
    try { fl.useWavLoop( 2 ); } catch ( stk::StkError& ) { threw = true; }
    UF_CHECK( threw );
  }

  // No 'smpl' chunk -> empty record set.
  {
    stk::FileRead fr( plain );
    UF_CHECK( fr.wavLoops().empty() );
  }

  // Fractional and play-count records parse, but activation is refused.
  {
    std::vector<SmplRec> recs = {
      { 1, 0, 2, 5, 0.5, 0 },
      { 2, 0, 2, 5, 0.0, 4 },
      { 3, 1, 6, 6, 0.0, 0 }  // ping-pong on a single frame
    };
    std::string p = makeWav( dir + "/frac.wav", kFrames, recs, true );
    stk::FileRead fr( p );
    std::vector<stk::FileRead::WavLoop> loops = fr.wavLoops();
    UF_CHECK( loops.size() == 3 );
    UF_CHECK( std::fabs( loops[0].fraction - 0.5 ) < 1e-9 );
    stk::FileLoop fl( p, false, false, 1000000, 1024, false );
    for ( size_t bad=0; bad<3; bad++ ) {
      bool threw = false;
      try { fl.useWavLoop( bad ); } catch ( stk::StkError& ) { threw = true; }
      UF_CHECK( threw );
    }
    bool threw = false;
    try { fl.useWavLoop( 99 ); } catch ( stk::StkError& ) { threw = true; }
    UF_CHECK( threw );
  }

  // Malformed 'smpl' chunks must fail the open with the format error path.
  {
    std::string mismatch = makeBadSmplWav( dir + "/mismatch.wav", kFrames, 2, 1 );
    UF_CHECK( opensAndFails( mismatch ) );

    std::vector<SmplRec> badRange = { { 1, 0, 5, 4, 0.0, 0 } }; // start > end
    std::string p1 = makeWav( dir + "/se.wav", kFrames, badRange, false );
    UF_CHECK( opensAndFails( p1 ) );

    std::vector<SmplRec> beyond = { { 1, 0, 2, (unsigned long) kFrames, 0.0, 0 } };
    std::string p2 = makeWav( dir + "/beyond.wav", kFrames, beyond, true );
    UF_CHECK( opensAndFails( p2 ) );
  }

  // Records must not leak across close() or a failed reopen.
  {
    stk::FileRead fr( before );
    UF_CHECK( fr.wavLoops().size() == 3 );
    fr.close();
    UF_CHECK( !fr.isOpen() && fr.wavLoops().empty() );

    fr.open( before );
    UF_CHECK( fr.wavLoops().size() == 3 );
    std::string mismatch = makeBadSmplWav( dir + "/mismatch2.wav", kFrames, 1, 0 );
    bool threw = false;
    try { fr.open( mismatch ); } catch ( stk::StkError& ) { threw = true; }
    UF_CHECK( threw && fr.wavLoops().empty() );
  }

  // Recursively remove the fixture directory.
  removeTempDir( dir );
}

// ------------------------------- sustain ---------------------------------

stk::FileLoop* makeLoop( const std::string& path, bool chunked )
{
  if ( chunked )
    return new stk::FileLoop( path, false, false, 2, 4, false ); // 4-frame cache
  return new stk::FileLoop( path, false, false, 1000000, 1024, false );
}

void uf_sustain()
{
  stk::Stk::setSampleRate( 44100.0 );
  stk::Stk::printErrors( false );

  std::string dir = makeTempDir( ".uf_sus" );
  std::string path = makeWav( dir + "/loops.wav", kFrames, standardLoops(), true );

  for ( int storage=0; storage<2; storage++ ) {
    bool chunked = ( storage == 1 );

    // Unit-rate forward loop: explicit frame-index sequence through the
    // attack, then A..B repeated with B inclusive.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 0 );
      fl->setRate( 1.0 );
      const long expected[] = { 0,1,2,3,4,5,6,7,3,4,5,6,7,3,4 };
      for ( unsigned i=0; i<sizeof(expected)/sizeof(expected[0]); i++ ) {
        expectClose( fl->tick( 0 ), leftVal( expected[i] ) );
        expectClose( fl->lastOut( 1 ), rightVal( expected[i] ) );
      }
      UF_CHECK( !fl->isFinished() );
      delete fl;
    }

    // Unit-rate alternating loop: each turnaround frame occurs once.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 1 );
      fl->setRate( 1.0 );
      const long expected[] = { 0,1,2,3,4,5,6,7,8,9,8,7,6,5,4,5,6,7,8,9,8,7,6,5,4 };
      for ( unsigned i=0; i<sizeof(expected)/sizeof(expected[0]); i++ ) {
        expectClose( fl->tick( 0 ), leftVal( expected[i] ) );
        expectClose( fl->lastOut( 1 ), rightVal( expected[i] ) );
      }
      delete fl;
    }

    // Fractional forward rate crossing the B -> A boundary, including a
    // large step that spans more than one loop cycle in a single tick.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 0 );
      fl->setRate( 0.7 );
      RefLoop ref; ref.N = kFrames; ref.A = kFwdA; ref.B = kFwdB;
      ref.alt = false; ref.rate = 0.7;
      compareScalar( *fl, ref, 18 );

      fl->setRate( 7.5 );
      ref.rate = 7.5;
      compareScalar( *fl, ref, 12 );
      delete fl;
    }

    // Fractional alternating rate, including multi-cycle steps.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 1 );
      fl->setRate( 1.3 );
      RefLoop ref; ref.N = kFrames; ref.A = kAltA; ref.B = kAltB;
      ref.alt = true; ref.rate = 1.3;
      compareScalar( *fl, ref, 20 );

      fl->setRate( 11.25 );
      ref.rate = 11.25;
      compareScalar( *fl, ref, 10 );
      delete fl;
    }

    // Zero rate holds the path position: after the rate change the next
    // render position is the one already advanced to, and it never moves.
    // Check it against the reference, including the fractional value.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 0 );
      fl->setRate( 0.7 );
      RefLoop ref; ref.N = kFrames; ref.A = kFwdA; ref.B = kFwdB;
      ref.alt = false; ref.rate = 0.7;
      double want[2];
      for ( int i=0; i<5; i++ ) { ref.render( want ); fl->tick(); }

      fl->setRate( 0.0 );
      ref.rate = 0.0;
      ref.render( want );                 // anchor: fractional position 3.5
      StkFloat anchorL = fl->tick( 0 );
      expectClose( anchorL, want[0] );
      expectClose( fl->lastOut( 1 ), want[1] );
      for ( int i=0; i<4; i++ ) {        // the position never drifts
        ref.render( want );
        expectClose( fl->tick( 0 ), anchorL );
        expectClose( fl->lastOut( 1 ), want[1] );
      }
      delete fl;
    }

    // Release on the alternating return leg: finish the descent to A,
    // climb back to B, then enter the tail without a position jump.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 1 );
      fl->setRate( 1.0 );
      RefLoop ref; ref.N = kFrames; ref.A = kAltA; ref.B = kAltB;
      ref.alt = true; ref.rate = 1.0;
      double want[2];
      for ( int i=0; i<12; i++ ) { ref.render( want ); fl->tick(); } // descending leg
      fl->releaseWavLoop();
      ref.released = true;
      fl->releaseWavLoop(); // repeated release changes nothing
      compareScalar( *fl, ref, 14 );
      UF_CHECK( fl->isFinished() );
      expectClose( fl->tick( 0 ), 0.0 );
      expectClose( fl->lastOut( 1 ), 0.0 );

      // A finished loop zero-fills the batch target channels but leaves
      // channels outside the target pair exactly as they were.
      stk::StkFrames tail( 3, 4 );
      for ( size_t i=0; i<tail.size(); i++ ) tail[i] = -33.0;
      fl->tick( tail, 1 );
      for ( unsigned i=0; i<3; i++ ) {
        expectClose( tail( i, 1 ), 0.0 );
        expectClose( tail( i, 2 ), 0.0 );
        expectClose( tail( i, 0 ), -33.0 );
        expectClose( tail( i, 3 ), -33.0 );
      }
      delete fl;
    }

    // Release in the forward loop keeps fractional overshoot into B+1.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 0 );
      fl->setRate( 0.6 );
      RefLoop ref; ref.N = kFrames; ref.A = kFwdA; ref.B = kFwdB;
      ref.alt = false; ref.rate = 0.6;
      double want[2];
      for ( int i=0; i<9; i++ ) { ref.render( want ); fl->tick(); }
      fl->releaseWavLoop();
      ref.released = true;
      compareScalar( *fl, ref, 16 );
      UF_CHECK( fl->isFinished() );
      delete fl;
    }

    // Release during the attack skips the loop and plays to the file end.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 0 );
      fl->setRate( 1.0 );
      fl->tick(); fl->tick();            // outputs frames 0 and 1
      fl->releaseWavLoop();
      const long expected[] = { 2,3,4,5,6,7,8,9,10,11 }; // frame 2 onward
      for ( unsigned i=0; i<sizeof(expected)/sizeof(expected[0]); i++ )
        expectClose( fl->tick( 0 ), leftVal( expected[i] ) );
      expectClose( fl->tick( 0 ), 0.0 ); // past the end: zeros, finished
      UF_CHECK( fl->isFinished() );
      delete fl;
    }

    // Reset keeps the selection but restarts from the file head.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 1 );
      fl->setRate( 1.0 );
      for ( int i=0; i<7; i++ ) fl->tick();
      fl->reset();
      const long expected[] = { 0,1,2,3,4,5,6 };
      for ( unsigned i=0; i<sizeof(expected)/sizeof(expected[0]); i++ )
        expectClose( fl->tick( 0 ), leftVal( expected[i] ) );
      delete fl;
    }

    // Active-mode rate and phase-jump restrictions.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 0 );
      auto reject = [&]( void (*fn)( void* ) ) { UF_CHECK( throwsStkError( fn, fl ) ); };
      struct Local {
        static void negRate( void* p )    { ((stk::FileLoop*)p)->setRate( -1.0 ); }
        static void infRate( void* p )    { ((stk::FileLoop*)p)->setRate(
                                              std::numeric_limits<StkFloat>::infinity() ); }
        static void timeJump( void* p )   { ((stk::FileLoop*)p)->addTime( 1.0 ); }
        static void phaseJump( void* p )  { ((stk::FileLoop*)p)->addPhase( 0.25 ); }
        static void offsetJump( void* p ) { ((stk::FileLoop*)p)->addPhaseOffset( 0.5 ); }
      };
      reject( Local::negRate );
      reject( Local::infRate );
      reject( Local::timeJump );
      reject( Local::phaseJump );
      reject( Local::offsetJump );
      // Zero-valued phase calls remain legal.
      fl->addTime( 0.0 );
      fl->addPhase( 0.0 );
      fl->addPhaseOffset( 0.0 );
      delete fl;
    }

    // A failed useWavLoop() leaves the running playback state untouched.
    {
      stk::FileLoop* fl = makeLoop( path, chunked );
      fl->useWavLoop( 0 );
      fl->setRate( 1.0 );
      fl->tick(); fl->tick();
      StkFloat beforeL = fl->lastOut( 0 );
      bool threw = false;
      try { fl->useWavLoop( 2 ); } catch ( stk::StkError& ) { threw = true; }
      UF_CHECK( threw );
      expectClose( fl->tick( 0 ), leftVal( 2 ) ); // sequence continues unchanged
      expectClose( fl->lastOut( 0 ), beforeL + 10.0 );
      delete fl;
    }
  }

  // Negative rates keep working in the default whole-file mode, and that
  // mode never finishes even after traveling past the file tail.
  {
    stk::FileLoop fl( path, false, false, 1000000, 1024, false );
    fl.setRate( -1.0 );
    for ( int i=0; i<40; i++ ) fl.tick();
    UF_CHECK( !fl.isFinished() );
    fl.setRate( 5.0 );
    for ( int i=0; i<40; i++ ) fl.tick();
    UF_CHECK( !fl.isFinished() );
  }

  // setFrequency still derives the rate from the whole file length.
  {
    stk::FileLoop fl( path, false, false, 1000000, 1024, false );
    fl.useWavLoop( 0 );
    fl.setFrequency( 1.0 );
    RefLoop ref; ref.N = kFrames; ref.A = kFwdA; ref.B = kFwdB;
    ref.alt = false; ref.rate = (StkFloat) kFrames / 44100.0;
    compareScalar( fl, ref, 25 );
  }

  removeTempDir( dir );
}

// ------------------------------- storage ---------------------------------

void uf_storage()
{
  stk::Stk::setSampleRate( 44100.0 );
  stk::Stk::printErrors( false );

  std::string dir = makeTempDir( ".uf_sto" );
  std::string path = makeWav( dir + "/loops.wav", kFrames, standardLoops(), false );

  // One scenario that crosses a forward B->A wrap (loop wider than the
  // 4-frame window), an alternating turnaround, a return-leg release and
  // the tail.  Whole-load and chunked-load must match bit for bit.
  auto runScenario = [&]( stk::FileLoop& fl, std::vector<double>& lout,
                          std::vector<double>& rout ) {
    fl.useWavLoop( 1 );
    fl.setRate( 1.0 );
    for ( int i=0; i<10; i++ ) { fl.tick(); }
    fl.releaseWavLoop();
    for ( int i=0; i<16; i++ ) {
      lout.push_back( fl.tick( 0 ) );
      rout.push_back( fl.lastOut( 1 ) );
    }
  };

  std::vector<double> fullL, fullR, chunkL, chunkR;
  {
    stk::FileLoop full( path, false, false, 1000000, 1024, false );
    runScenario( full, fullL, fullR );
  }
  {
    stk::FileLoop chunk( path, false, false, 2, 4, false );
    runScenario( chunk, chunkL, chunkR );
  }
  UF_CHECK( fullL.size() == chunkL.size() );
  for ( size_t i=0; i<fullL.size(); i++ ) {
    expectClose( fullL[i], chunkL[i] );
    expectClose( fullR[i], chunkR[i] );
  }

  // Scalar ticks and StkFrames batch ticks (mixed batch sizes) produce
  // the identical sequence, on both storage paths.
  for ( int storage=0; storage<2; storage++ ) {
    bool chunked = ( storage == 1 );
    stk::FileLoop scalar( path, false, false, chunked ? 2 : 1000000, chunked ? 4 : 1024, false );
    stk::FileLoop batch( path, false, false, chunked ? 2 : 1000000, chunked ? 4 : 1024, false );
    scalar.useWavLoop( 0 );
    batch.useWavLoop( 0 );
    scalar.setRate( 0.7 );
    batch.setRate( 0.7 );

    const unsigned batchSizes[] = { 1, 3, 5, 2, 4 };
    unsigned produced = 0;
    for ( unsigned b=0; b<sizeof(batchSizes)/sizeof(batchSizes[0]); b++ ) {
      unsigned n = batchSizes[b];
      stk::StkFrames bf( n, 4 );          // 4 channels, loop writes the middle two
      for ( unsigned i=0; i<bf.size(); i++ ) bf[i] = -77.0;
      batch.tick( bf, 1 );

      for ( unsigned i=0; i<n; i++ ) {
        StkFloat sl = scalar.tick( 0 );
        StkFloat sr = scalar.lastOut( 1 );
        expectClose( bf( i, 1 ), sl );
        expectClose( bf( i, 2 ), sr );
        // Sentinel channels outside the target pair are untouched.
        expectClose( bf( i, 0 ), -77.0 );
        expectClose( bf( i, 3 ), -77.0 );
        produced++;
      }
    }
    UF_CHECK( produced == 15 );
  }

  // Reuse one object across files with and without loop metadata.
  {
    stk::FileLoop fl( path, false, false, 2, 4, false );
    fl.useWavLoop( 0 );
    fl.setRate( 1.0 );
    for ( int i=0; i<6; i++ ) fl.tick();
    UF_CHECK( !fl.isFinished() );

    std::string noLoops = makeWav( dir + "/noloops.wav", kFrames, {}, true );
    fl.openFile( noLoops, false, false, false );
    bool threw = false;
    try { fl.useWavLoop( 0 ); } catch ( stk::StkError& ) { threw = true; }
    UF_CHECK( threw );
    fl.setRate( 1.0 );
    // Default whole-file wrapping is back, including across the tail.
    for ( int i=0; i<3 * kFrames; i++ ) {
      expectClose( fl.tick( 0 ), leftVal( i % kFrames ) );
    }
    UF_CHECK( !fl.isFinished() );

    // Reopening a file with loops restores the feature and chunking works.
    fl.openFile( path, false, false, false );
    fl.useWavLoop( 1 );
    fl.setRate( 1.0 );
    const long expected[] = { 0,1,2,3,4,5,6,7,8,9,8,7,6,5,4 };
    for ( unsigned i=0; i<sizeof(expected)/sizeof(expected[0]); i++ )
      expectClose( fl.tick( 0 ), leftVal( expected[i] ) );

    // Closing drops the selection and silences output.
    fl.closeFile();
    threw = false;
    try { fl.useWavLoop( 0 ); } catch ( stk::StkError& ) { threw = true; }
    UF_CHECK( threw );
    expectClose( fl.tick( 0 ), 0.0 );
  }

  removeTempDir( dir );
}
