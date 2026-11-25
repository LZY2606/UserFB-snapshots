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
		    unsigned constantDuration = 0,
		    unsigned ctsDeltaLength = 0);

  // True iff all supported AU-header bit widths are in their legal range.
  static Boolean validHeaderLengths(unsigned sizeLength,
				    unsigned indexLength,
				    unsigned indexDeltaLength,
				    unsigned ctsDeltaLength);
  // mediumName is "audio", "video", or "application"
  // it *cannot* be NULL

protected:
  MPEG4GenericRTPSource(UsageEnvironment& env, Groupsock* RTPgs,
			unsigned char rtpPayloadFormat,
			unsigned rtpTimestampFrequency,
			char const* mediumName,
			char const* mode,
			unsigned sizeLength, unsigned indexLength,
			unsigned indexDeltaLength,
			unsigned constantDuration = 0,
			unsigned ctsDeltaLength = 0);
      // called only by createNew(), or by subclass constructors
  virtual ~MPEG4GenericRTPSource();

protected:
  // redefined virtual functions:
  virtual Boolean processSpecialHeader(BufferedPacket* packet,
                                       unsigned& resultSpecialHeaderSize);
  virtual void doStopGettingFrames();
  virtual char const* MIMEtype() const;

private:
  void resetAUState();
  void resetFragmentState();
  Boolean parseAUHeaders(BufferedPacket* packet, unsigned payloadOffset,
			 unsigned payloadSize);
  Boolean prepareNextAU(unsigned dataSize, unsigned& frameSize,
			Boolean& frameCompletes,
			struct timeval& presentationTime);
  struct timeval presentationTimeForAU(unsigned auIndex,
				       struct timeval const& anchorTime) const;

private:
  char* fMIMEType;

  char* fMode;
  unsigned fSizeLength, fIndexLength, fIndexDeltaLength;
  unsigned fConstantDuration; // in RTP timestamp clock units
  unsigned fCTSDeltaLength;
  unsigned fNumAUHeaders; // in the most recently read packet
  unsigned fNextAUHeader; // index of the next AU Header to read
  struct AUHeader* fAUHeaders;

  // State for an AU spread across consecutive RTP packets.
  Boolean fFragmentActive;
  unsigned fFragmentFullSize;
  unsigned fFragmentReceivedSize;
  unsigned fFragmentFirstIndex;
  unsigned fFragmentLogicalIndex;
  unsigned short fFragmentNextSeqNum;
  Boolean fFragmentIsContinuationChunk;
  u_int32_t fFragmentRTPTimestamp;
  struct timeval fFragmentPresentationTime;

  friend class MPEG4GenericBufferedPacket;
};



// A function that looks up the sampling frequency from an
// "AudioSpecificConfig" string.  (0 means 'unknown')
unsigned samplingFrequencyFromAudioSpecificConfig(char const* configStr);

#endif
