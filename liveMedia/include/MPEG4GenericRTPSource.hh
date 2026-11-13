/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// "liveMedia"
// Copyright (c) 1996-2026 Live Networks, Inc.  All rights reserved.
// MPEG4-GENERIC ("audio", "video", or "application") RTP stream sources
// C++ header

#ifndef _MPEG4_GENERIC_RTP_SOURCE_HH
#define _MPEG4_GENERIC_RTP_SOURCE_HH

#ifndef _MULTI_FRAMED_RTP_SOURCE_HH
#include "MultiFramedRTPSource.hh"
#endif

class MPEG4GenericRTPSource: public MultiFramedRTPSource {
public:
  static MPEG4GenericRTPSource*
  createNew(UsageEnvironment& env, Groupsock* RTPgs,
	    unsigned char rtpPayloadFormat,
	    unsigned rtpTimestampFrequency,
	    char const* mediumName,
	    char const* mode, unsigned sizeLength, unsigned indexLength,
	    unsigned indexDeltaLength,
	    unsigned constantDuration = 0, unsigned ctsDeltaLength = 0);
  // mediumName is "audio", "video", or "application"
  // it *cannot* be NULL
  // "constantDuration" is in RTP timestamp units.  "ctsDeltaLength" (like the
  // other *_LENGTH parameters) is a count of bits; each must be <= 32.
  // Returns NULL if a parameter is out of range.

protected:
  MPEG4GenericRTPSource(UsageEnvironment& env, Groupsock* RTPgs,
			unsigned char rtpPayloadFormat,
			unsigned rtpTimestampFrequency,
			char const* mediumName,
			char const* mode,
			unsigned sizeLength, unsigned indexLength,
			unsigned indexDeltaLength,
			unsigned constantDuration, unsigned ctsDeltaLength
			);
      // called only by createNew(), or by subclass constructors
  virtual ~MPEG4GenericRTPSource();

protected:
  // redefined virtual functions:
  virtual Boolean processSpecialHeader(BufferedPacket* packet,
                                       unsigned& resultSpecialHeaderSize);
  virtual char const* MIMEtype() const;

private:
  char* fMIMEType;

  char* fMode;
  unsigned fSizeLength, fIndexLength, fIndexDeltaLength;
  unsigned fConstantDuration, fCTSDeltaLength;
  unsigned fNumAUHeaders; // in the most recently read packet
  unsigned fNextAUHeader; // index of the next AU Header to read
  struct AUHeader* fAUHeaders;
  Boolean fLastAUComplete; // whether the packet's last AU is fully contained in it

  // State about an AU that's fragmented across multiple packets:
  Boolean fAUFragmentActive;
  unsigned fAUFragmentFullSize; // the AU's "AU-size" is its complete size
  unsigned fAUFragmentBytesRemaining;
  unsigned fAUFragmentRTPTimestamp; // all fragments of an AU share its RTP timestamp

  friend class MPEG4GenericBufferedPacket;
};



// A function that looks up the sampling frequency from an
// "AudioSpecificConfig" string.  (0 means 'unknown')
unsigned samplingFrequencyFromAudioSpecificConfig(char const* configStr);

#endif
