#ifndef STK_FILEREAD_H
#define STK_FILEREAD_H

#include "Stk.h"
#include <vector>

namespace stk {

/***************************************************/
/*! \class FileRead
    \brief STK audio file input class.

    This class provides input support for various
    audio file formats.  Multi-channel (>2)
    soundfiles are supported.  The file data is
    returned via an external StkFrames object
    passed to the read() function.  This class
    does not store its own copy of the file data,
    rather the data is read directly from disk.

    FileRead currently supports uncompressed WAV,
    AIFF/AIFC, SND (AU), MAT-file (Matlab), and
    STK RAW file formats.  Signed integer (8-,
    16-, 24-, and 32-bit) and floating-point (32- and
    64-bit) data types are supported.  Compressed
    data types are not supported.

    WAV files may carry a 'smpl' chunk with sampler
    loop points.  The wavLoops() function returns a
    copy of any loop records found there.  Parsing
    the 'smpl' chunk never changes the audio data
    offset used by read().

    STK RAW files have no header and are assumed to
    contain a monophonic stream of 16-bit signed
    integers in big-endian byte order at a sample
    rate of 22050 Hz.  MAT-file data should be saved
    in an array with each data channel filling a
    matrix row.  The sample rate for MAT-files should
    be specified in a variable named "fs".  If no
    such variable is found, the sample rate is
    assumed to be 44100 Hz.

    by Perry R. Cook and Gary P. Scavone, 1995--2023.
*/
/***************************************************/

class FileRead : public Stk
{
public:

  //! One sampler loop record from a WAV 'smpl' chunk.
  /*!
    The fields mirror the entries of the sample loop table of the
    'smpl' chunk.  The \e start and \e end points are expressed in
    sample frames and the frame at \e end is included in the loop.
    A \e type of 0 is a forward (uni-directional) loop and 1 is an
    alternating (ping-pong) loop; other values are valid records but
    name loop types not currently supported by FileLoop.
  */
  struct WavLoop {
    unsigned long identifier;
    unsigned long type;
    unsigned long start;
    unsigned long end;
    StkFloat fraction;
    unsigned long playCount;
  };

  //! Default constructor.
  FileRead( void );

  //! Overloaded constructor that opens a file during instantiation.
  /*!
    An StkError will be thrown if the file is not found or its
    format is unknown or unsupported.  The optional arguments allow a
    headerless file type to be supported.  If \c typeRaw is false (the
    default), the subsequent parameters are ignored.
  */
  FileRead( std::string fileName, bool typeRaw = false, unsigned int nChannels = 1,
            StkFormat format = STK_SINT16, StkFloat rate = 22050.0 );

  //! Class destructor.
  ~FileRead( void );

  //! Open the specified file and determine its formatting.
  /*!
    An StkError will be thrown if the file is not found or its
    format is unknown or unsupported.  The optional arguments allow a
    headerless file type to be supported.  If \c typeRaw is false (the
    default), the subsequent parameters are ignored.
  */
  void open( std::string fileName, bool typeRaw = false, unsigned int nChannels = 1,
             StkFormat format = STK_SINT16, StkFloat rate = 22050.0 );

  //! If a file is open, close it.
  void close( void );

  //! Returns \e true if a file is currently open.
  bool isOpen( void );

  //! Return the file size in sample frames.
  unsigned long fileSize( void ) const { return fileSize_; };

  //! Return the number of audio channels in the file.
  unsigned int channels( void ) const { return channels_; };

  //! Return the data format of the file.
  StkFormat format( void ) const { return dataType_; };

  //! Return the file sample rate in Hz.
  /*!
    WAV, SND, and AIF formatted files specify a sample rate in
    their headers.  By definition, STK RAW files have a sample rate of
    22050 Hz.  MAT-files are assumed to have a rate of 44100 Hz.
  */
  StkFloat fileRate( void ) const { return fileRate_; };

  //! Return the sampler loop records found in a WAV 'smpl' chunk.
  /*!
    The returned vector is a self-contained copy owned by the
    caller; it is empty when no 'smpl' chunk is present or when the
    open file is not a WAV file.  Records of loop \e types other
    than 0 (forward) or 1 (alternating) are returned as found: they
    are valid metadata but cannot be activated by FileLoop.  The
    records are cleared whenever a new file is opened or close() is
    called.  An StkError is thrown while opening if the 'smpl'
    chunk is truncated, declares a loop count inconsistent with its
    size, contains a loop with \e start > \e end, or names frames
    beyond the actual file length.
  */
  std::vector<WavLoop> wavLoops( void ) const { return wavLoops_; };

  //! Read sample frames from the file into an StkFrames object.
  /*!
    The number of sample frames to read will be determined from the
    number of frames of the StkFrames argument.  If this size is
    larger than the available data in the file (given the file size
    and starting frame index), the extra frames will be unaffected
    (the StkFrames object will not be resized).  Optional parameters
    are provided to specify the starting sample frame within the file
    (default = 0) and whether to normalize the data with respect to
    fixed-point limits (default = true).  An StkError will be thrown
    if a file error occurs or if the number of channels in the
    StkFrames argument is not equal to that in the file.
   */
  void read( StkFrames& buffer, unsigned long startFrame = 0, bool doNormalize = true );

protected:

  // Get STK RAW file information.
  bool getRawInfo( const char *fileName, unsigned int nChannels,
                   StkFormat format, StkFloat rate );

  // Get WAV file header information.
  bool getWavInfo( const char *fileName );

  // Parse the WAV 'smpl' chunk (chunkSize bytes follow the chunk id).
  bool getWavSmplInfo( SINT32 chunkSize );

  // Get SND (AU) file header information.
  bool getSndInfo( const char *fileName );

  // Get AIFF file header information.
  bool getAifInfo( const char *fileName );

  // Get MAT-file header information.
  bool getMatInfo( const char *fileName );

  // Helper function for MAT-file parsing.
  bool findNextMatArray( SINT32 *chunkSize, SINT32 *rows, SINT32 *columns, SINT32 *nametype );

  FILE *fd_;
  bool byteswap_;
  bool wavFile_;
  unsigned long fileSize_;
  unsigned long dataOffset_;
  unsigned int channels_;
  StkFormat dataType_;
  StkFloat fileRate_;
  std::vector<WavLoop> wavLoops_;
};

} // stk namespace

#endif
