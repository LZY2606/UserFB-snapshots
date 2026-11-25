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
// Implementation

#include "MPEG4GenericRTPSource.hh"
#include "BitVector.hh"
#include "MPEG4LATMAudioRTPSource.hh" // for parseGeneralConfigStr()

#include <stdint.h>
#include <vector>

////////// MPEG4GenericBufferedPacket and MPEG4GenericBufferedPacketFactory

class MPEG4GenericBufferedPacket: public BufferedPacket {
public:
  MPEG4GenericBufferedPacket(MPEG4GenericRTPSource* ourSource);
  virtual ~MPEG4GenericBufferedPacket();

private: // redefined virtual functions
  virtual void getNextEnclosedFrameParameters(unsigned char*& framePtr,
					      unsigned dataSize,
					      unsigned& frameSize,
					      unsigned& frameDurationInMicroseconds);

private:
  MPEG4GenericRTPSource* fOurSource;
};

class MPEG4GenericBufferedPacketFactory: public BufferedPacketFactory {
private: // redefined virtual functions
  virtual BufferedPacket* createNewPacket(MultiFramedRTPSource* ourSource);
};


////////// AUHeader //////////
struct AUHeader {
  unsigned size;
  unsigned index; // first header's absolute AU index
  unsigned indexDelta; // second and later headers' index delta
  unsigned ctsFlag;
  int64_t ctsDelta;
  uint64_t relativeSequence;

  // Number of payload bytes carried for this AU by *this* RTP packet.
  // For a trailing fragment this is smaller than "size".
  unsigned payloadChunkSize;
  Boolean chunkCompletesAU;
};


///////// Local helpers /////////

static int64_t signedBitField(unsigned rawValue, unsigned numBits) {
  if (numBits == 0) return 0;

  uint64_t const signBit = uint64_t(1) << (numBits - 1);
  uint64_t value = rawValue;
  if (numBits < 32) value &= (uint64_t(1) << numBits) - 1;
  else value &= 0xffffffffULL;

  if ((value & signBit) != 0) {
    // Sign-extend the two's-complement value.
    return int64_t(value - (uint64_t(1) << numBits));
  }
  return int64_t(value);
}

static struct timeval addRTPTicksToTimeval(struct timeval tv,
					   int64_t tickDelta,
					   unsigned timestampFrequency) {
  if (timestampFrequency == 0 || tickDelta == 0) return tv;

  // Perform the conversion once, directly from the mapped packet anchor.
  // In particular, do not add a separately rounded microsecond duration for
  // each successive AU.
  int64_t const deltaMicroseconds
    = (tickDelta * 1000000LL) / int64_t(timestampFrequency);
  int64_t const totalMicroseconds
    = int64_t(tv.tv_sec) * 1000000LL + tv.tv_usec + deltaMicroseconds;

  struct timeval result;
  result.tv_sec = (decltype(result.tv_sec))(totalMicroseconds / 1000000LL);
  result.tv_usec = (decltype(result.tv_usec))(totalMicroseconds % 1000000LL);
  return result;
}


///////// MPEG4GenericRTPSource implementation ////////

MPEG4GenericRTPSource*
MPEG4GenericRTPSource::createNew(UsageEnvironment& env, Groupsock* RTPgs,
				 unsigned char rtpPayloadFormat,
				 unsigned rtpTimestampFrequency,
				 char const* mediumName,
				 char const* mode,
				 unsigned sizeLength, unsigned indexLength,
				 unsigned indexDeltaLength,
				 unsigned constantDuration,
				 unsigned ctsDeltaLength) {
  if (!validHeaderLengths(sizeLength, indexLength, indexDeltaLength,
			  ctsDeltaLength)) {
    env << "MPEG4GenericRTPSource: rejected unsupported AU-header bit width(s): "
	<< sizeLength << "/" << indexLength << "/" << indexDeltaLength
	<< "/CTS " << ctsDeltaLength << "\n";
    return NULL;
  }

  return new MPEG4GenericRTPSource(env, RTPgs, rtpPayloadFormat,
				   rtpTimestampFrequency, mediumName,
				   mode, sizeLength, indexLength,
				   indexDeltaLength, constantDuration,
				   ctsDeltaLength);
}

Boolean MPEG4GenericRTPSource::validHeaderLengths(unsigned sizeLength,
						  unsigned indexLength,
						  unsigned indexDeltaLength,
						  unsigned ctsDeltaLength) {
  // BitVector reads at most 32 bits in one field.  Zero is the SDP default
  // and means that the corresponding field is absent.
  return sizeLength <= 32
    && indexLength <= 32
    && indexDeltaLength <= 32
    && ctsDeltaLength <= 32;
}

MPEG4GenericRTPSource
::MPEG4GenericRTPSource(UsageEnvironment& env, Groupsock* RTPgs,
			unsigned char rtpPayloadFormat,
			unsigned rtpTimestampFrequency,
			char const* mediumName,
                        char const* mode,
                        unsigned sizeLength, unsigned indexLength,
                        unsigned indexDeltaLength,
			unsigned constantDuration,
			unsigned ctsDeltaLength)
  : MultiFramedRTPSource(env, RTPgs,
			 rtpPayloadFormat, rtpTimestampFrequency,
			 new MPEG4GenericBufferedPacketFactory),
    fMIMEType(NULL), fMode(strDup(mode)),
    fSizeLength(sizeLength), fIndexLength(indexLength),
    fIndexDeltaLength(indexDeltaLength),
    fConstantDuration(constantDuration),
    fCTSDeltaLength(ctsDeltaLength),
    fNumAUHeaders(0), fNextAUHeader(0), fAUHeaders(NULL) {
  resetFragmentState();

  unsigned mimeTypeLength =
    strlen(mediumName) + 14 /* strlen("/MPEG4-GENERIC") */ + 1;
  fMIMEType = new char[mimeTypeLength];
  if (fMIMEType != NULL) {
    sprintf(fMIMEType, "%s/MPEG4-GENERIC", mediumName);
  }

  // Check for a "mode" that we don't yet support: //#####
  if (mode == NULL ||
      (strcmp(mode, "aac-hbr") != 0 && strcmp(mode, "generic") != 0)) {
    envir() << "MPEG4GenericRTPSource Warning: Unknown or unsupported \"mode\": "
	    << mode << "\n";
  }
}

MPEG4GenericRTPSource::~MPEG4GenericRTPSource() {
  resetAUState();
  delete[] fMode;
  delete[] fMIMEType;
}

void MPEG4GenericRTPSource::doStopGettingFrames() {
  resetAUState();
  resetFragmentState();
  MultiFramedRTPSource::doStopGettingFrames();
}

void MPEG4GenericRTPSource::resetAUState() {
  fNumAUHeaders = 0;
  fNextAUHeader = 0;
  delete[] fAUHeaders;
  fAUHeaders = NULL;
}

void MPEG4GenericRTPSource::resetFragmentState() {
  fFragmentActive = False;
  fFragmentFullSize = 0;
  fFragmentReceivedSize = 0;
  fFragmentFirstIndex = 0;
  fFragmentLogicalIndex = 0;
  fFragmentNextSeqNum = 0;
  fFragmentIsContinuationChunk = False;
  fFragmentRTPTimestamp = 0;
  fFragmentPresentationTime.tv_sec = 0;
  fFragmentPresentationTime.tv_usec = 0;
}

Boolean MPEG4GenericRTPSource
::processSpecialHeader(BufferedPacket* packet,
		       unsigned& resultSpecialHeaderSize) {
  unsigned char* headerStart = packet->data();
  unsigned packetSize = packet->dataSize();

  fCurrentPacketBeginsFrame = fCurrentPacketCompletesFrame;
          // whether the *previous* packet ended a frame

  // The RTP "M" (marker) bit says whether the last AU in this packet is
  // complete.  Individual enclosed AUs can still be complete when M=0 because
  // a later AU in the same packet is the fragmented one.
  fCurrentPacketCompletesFrame = packet->rtpMarkerBit();

  // default values:
  resultSpecialHeaderSize = 0;
  resetAUState();

  if (fSizeLength == 0) {
    // No AU Header Section is present under this configuration.
    return True;
  }

  if (packetSize < 2) return False;

  unsigned AU_headers_length = (headerStart[0]<<8)|headerStart[1];
  if (AU_headers_length == 0) return False;

  unsigned AU_headers_length_bytes = (AU_headers_length+7)/8;
  resultSpecialHeaderSize = 2 + AU_headers_length_bytes;
  if (packetSize < resultSpecialHeaderSize) return False;

  unsigned payloadSize = packetSize - resultSpecialHeaderSize;
  if (!parseAUHeaders(packet, resultSpecialHeaderSize, payloadSize)) {
    resetAUState();
    // A packet that cannot be parsed cannot safely continue an earlier
    // fragment either.  Reset the generic accumulation destination.
    resetFragmentState();
    discardPartialFrameAssembly();
    return False;
  }

  return True;
}

Boolean MPEG4GenericRTPSource
::parseAUHeaders(BufferedPacket* packet, unsigned /*payloadOffset*/,
		 unsigned payloadSize) {
  unsigned char* headerStart = packet->data();
  unsigned AU_headers_length = (headerStart[0]<<8)|headerStart[1];

  std::vector<AUHeader> headers;
  BitVector bv(&headerStart[2], 0, AU_headers_length);

  // Parse by the real bit cursor.  Optional CTS flags make the header width
  // variable, so the old fixed-field-count division is not valid.
  while (bv.curBitIndex() < AU_headers_length) {
    unsigned const indexBits
      = headers.empty() ? fIndexLength : fIndexDeltaLength;
    unsigned const ctsFlagBits = fCTSDeltaLength > 0 ? 1 : 0;
    unsigned const fixedBits = fSizeLength + indexBits + ctsFlagBits;
    if (AU_headers_length - bv.curBitIndex() < fixedBits) return False;

    AUHeader h;
    h.size = bv.getBits(fSizeLength);
    h.index = 0;
    h.indexDelta = 0;
    if (headers.empty()) {
      h.index = bv.getBits(fIndexLength);
    } else {
      h.indexDelta = bv.getBits(fIndexDeltaLength);
    }

    h.ctsFlag = 0;
    h.ctsDelta = 0;
    if (fCTSDeltaLength > 0) {
      h.ctsFlag = bv.get1Bit();
      if (h.ctsFlag) {
	if (AU_headers_length - bv.curBitIndex() < fCTSDeltaLength) {
	  return False;
	}
	h.ctsDelta = signedBitField(bv.getBits(fCTSDeltaLength),
				    fCTSDeltaLength);
      }
    }

    if (bv.curBitIndex() > AU_headers_length) return False;
    h.relativeSequence = 0;
    h.payloadChunkSize = 0;
    h.chunkCompletesAU = False;
    headers.push_back(h);
  }

  // The last header must end exactly at the declared bit boundary.  Padding
  // bits in the final byte are outside the BitVector and are not another AU.
  if (headers.empty() || bv.curBitIndex() != AU_headers_length) return False;

  uint64_t relativeSequence = 0;
  for (unsigned i = 0; i < headers.size(); ++i) {
    if (i == 0) {
      // The anchor AU cannot have a packet-relative CTS displacement.
      // The constant-duration first-index rule is applied after deciding
      // whether this header continues an earlier fragmented AU.
      if (fCTSDeltaLength > 0 && headers[i].ctsFlag != 0) return False;
      headers[i].relativeSequence = 0;
    } else {
      relativeSequence += uint64_t(headers[i].indexDelta) + 1;
      headers[i].relativeSequence = relativeSequence;
    }
  }

  unsigned short const seqNum = packet->rtpSeqNo();
  u_int32_t const rtpTimestamp = packet->rtpTimestamp();
  Boolean continuationOfFragment = False;

  if (fFragmentActive) {
    Boolean const sequenceContinuous = seqNum == fFragmentNextSeqNum;
    Boolean const sameTimestamp = rtpTimestamp == fFragmentRTPTimestamp;
    unsigned const candidateIndex = headers[0].index;
    Boolean const sameIndex
      = fIndexLength == 0
      || candidateIndex == fFragmentFirstIndex
      || candidateIndex == fFragmentLogicalIndex;
    Boolean const sameAU
      = sameTimestamp
      && sameIndex
      && headers[0].size == fFragmentFullSize;

    if (sequenceContinuous && sameAU) {
      continuationOfFragment = True;
    } else if (sameAU) {
      // This appears to be a missing/late fragment rather than a new AU.
      // It cannot be merged without a sequence gap.
      resetFragmentState();
      discardPartialFrameAssembly();
      return False;
    } else {
      // The unfinished AU is gone, but this packet starts a different AU.
      resetFragmentState();
      discardPartialFrameAssembly();
      fCurrentPacketBeginsFrame = True;
    }
  }

  unsigned payloadLeft = payloadSize;

  if (!continuationOfFragment
      && fConstantDuration != 0 && headers[0].index != 0) {
    return False;
  }

  if (continuationOfFragment) {
    if (fFrameSize != fFragmentReceivedSize) return False;

    AUHeader& first = headers[0];
    unsigned const needed = fFragmentFullSize - fFragmentReceivedSize;
    unsigned const chunkSize = payloadLeft < needed ? payloadLeft : needed;
    if (chunkSize == 0) return False;

    first.payloadChunkSize = chunkSize;
    first.chunkCompletesAU = (fFragmentReceivedSize + chunkSize
			      == fFragmentFullSize);
    payloadLeft -= chunkSize;

    if (!first.chunkCompletesAU) {
      // A middle fragment carries only continuation bytes.  Additional AU
      // headers would have no payload boundary to describe.
      if (headers.size() != 1 || payloadLeft != 0) return False;
      if (packet->rtpMarkerBit()) return False;

      fFragmentReceivedSize += chunkSize;
      fFragmentNextSeqNum = seqNum + 1;
      fCurrentPacketBeginsFrame = False;
    } else {
      // Keep the fragment state (including its first-piece presentation
      // time) until prepareNextAU() consumes this final chunk.

      // The completing packet may also carry subsequent complete AUs.
      for (unsigned i = 1; i < headers.size(); ++i) {
	AUHeader& h = headers[i];
	if (h.size == 0 || h.size > payloadLeft) return False;
	h.payloadChunkSize = h.size;
	h.chunkCompletesAU = True;
	payloadLeft -= h.size;
      }
      if (payloadLeft != 0) return False;
    }
  } else {
    // This packet starts a fresh AU sequence.  Validate every declared size
    // against the payload before exposing any AU to a callback.
    fCurrentPacketBeginsFrame = True;

    for (unsigned i = 0; i < headers.size(); ++i) {
      AUHeader& h = headers[i];
      if (h.size == 0) return False;

      if (i + 1 < headers.size()) {
	if (h.size > payloadLeft) return False;
	h.payloadChunkSize = h.size;
	h.chunkCompletesAU = True;
	payloadLeft -= h.size;
      } else {
	if (h.size <= payloadLeft) {
	  // The final header must consume exactly the remaining AU payload.
	  if (h.size != payloadLeft) return False;
	  h.payloadChunkSize = h.size;
	  h.chunkCompletesAU = True;
	  payloadLeft = 0;
	} else {
	  // Trailing fragment: AU-size remains the size of the complete AU.
	  if (payloadLeft == 0) return False;
	  if (packet->rtpMarkerBit()) return False;

	  h.payloadChunkSize = payloadLeft;
	  h.chunkCompletesAU = False;

	  fFragmentActive = True;
	  fFragmentFullSize = h.size;
	  fFragmentReceivedSize = payloadLeft;
	  fFragmentFirstIndex = headers[0].index;
	  unsigned const logicalIndexMask
	    = fIndexLength >= 32 ? 0xffffffffU : ((1U << fIndexLength) - 1);
	  fFragmentLogicalIndex
	    = (headers[0].index + unsigned(h.relativeSequence))
	      & logicalIndexMask;
	  fFragmentNextSeqNum = seqNum + 1;
	  fFragmentRTPTimestamp = rtpTimestamp;
	  fFragmentPresentationTime = packet->presentationTime();
	  payloadLeft = 0;
	}
      }
    }

    if (payloadLeft != 0 || fFragmentActive == False) {
      // payloadLeft is expected to be zero after the final-header branch.
      if (payloadLeft != 0) return False;
    }
  }

  fFragmentIsContinuationChunk = continuationOfFragment;

  resetAUState();
  fNumAUHeaders = unsigned(headers.size());
  fAUHeaders = new AUHeader[fNumAUHeaders];
  for (unsigned i = 0; i < fNumAUHeaders; ++i) fAUHeaders[i] = headers[i];
  fNextAUHeader = 0;
  return True;
}

Boolean MPEG4GenericRTPSource
::prepareNextAU(unsigned dataSize, unsigned& frameSize,
		Boolean& frameCompletes, struct timeval& presentationTime) {
  if (fAUHeaders == NULL) {
    // No AU Header Section (the historical SizeLength==0 path): hand over
    // the remaining payload exactly as the old implementation did.
    frameSize = dataSize;
    frameCompletes = fCurrentPacketCompletesFrame;
    presentationTime = fPresentationTime;
    return True;
  }
  if (fNextAUHeader >= fNumAUHeaders) return False;

  AUHeader& h = fAUHeaders[fNextAUHeader];
  if (h.payloadChunkSize == 0 || h.payloadChunkSize > dataSize) return False;

  frameSize = h.payloadChunkSize;
  frameCompletes = h.chunkCompletesAU;

  if (fFragmentActive && fFragmentIsContinuationChunk) {
    // This is a later chunk of an already started fragmented AU.  Keep the
    // presentation time calculated and saved from its first chunk.
    presentationTime = fFragmentPresentationTime;
    fPresentationTime = presentationTime;
    fCurrentPacketCompletesFrame = frameCompletes;
    ++fNextAUHeader;
    if (frameCompletes) resetFragmentState();
    return True;
  }

  // BufferedPacket::use() has just put this packet's mapped RTP time into
  // fPresentationTime.  Use that as the clock anchor and derive this AU's
  // time directly, rather than accumulating rounded frame durations.
  presentationTime = presentationTimeForAU(fNextAUHeader, fPresentationTime);
  fPresentationTime = presentationTime;
  fCurrentPacketCompletesFrame = frameCompletes;
  if (fFragmentActive && !frameCompletes) {
    fFragmentPresentationTime = presentationTime;
  }

  ++fNextAUHeader;
  return True;
}

struct timeval MPEG4GenericRTPSource
::presentationTimeForAU(unsigned auIndex,
			 struct timeval const& anchorTime) const {
  if (auIndex >= fNumAUHeaders || fAUHeaders == NULL) return anchorTime;
  AUHeader const& h = fAUHeaders[auIndex];

  int64_t tickDelta = 0;
  if (fCTSDeltaLength > 0 && h.ctsFlag != 0) {
    // An explicit CTS-delta is absolute relative to this packet's RTP anchor;
    // it does not additionally accumulate constantDuration.
    tickDelta = h.ctsDelta;
  } else if (fConstantDuration != 0) {
    // Deliberately reduce the sequence and duration to 32 bits before the
    // modular RTP-timestamp multiplication; malformed large delta fields
    // must not overflow a 64-bit product.
    uint32_t const relativeSequence32 = uint32_t(h.relativeSequence);
    uint32_t const duration32 = uint32_t(fConstantDuration);
    uint32_t const wrappedOffset = relativeSequence32 * duration32;
    tickDelta = int64_t(wrappedOffset);
    if (tickDelta >= 0x80000000LL) tickDelta -= 0x100000000LL;
  }

  return addRTPTicksToTimeval(anchorTime, tickDelta, timestampFrequency());
}

char const* MPEG4GenericRTPSource::MIMEtype() const {
  return fMIMEType;
}


////////// MPEG4GenericBufferedPacket
////////// and MPEG4GenericBufferedPacketFactory implementation

MPEG4GenericBufferedPacket
::MPEG4GenericBufferedPacket(MPEG4GenericRTPSource* ourSource)
  : fOurSource(ourSource) {
}

MPEG4GenericBufferedPacket::~MPEG4GenericBufferedPacket() {
}

void MPEG4GenericBufferedPacket
::getNextEnclosedFrameParameters(unsigned char*& /*framePtr*/,
				 unsigned dataSize,
				 unsigned& frameSize,
				 unsigned& frameDurationInMicroseconds) {
  frameDurationInMicroseconds = 0;

  Boolean frameCompletes = True;
  struct timeval presentationTime;
  if (fOurSource == NULL
      || !fOurSource->prepareNextAU(dataSize, frameSize, frameCompletes,
				    presentationTime)) {
    // Parsing is fully validated in processSpecialHeader(); reaching here
    // indicates an internal state error.  Return no bytes rather than handing
    // a caller an unvalidated payload span.
    frameSize = 0;
    if (fOurSource != NULL) fOurSource->handleClosure();
  }
}

BufferedPacket* MPEG4GenericBufferedPacketFactory
::createNewPacket(MultiFramedRTPSource* ourSource) {
  return new MPEG4GenericBufferedPacket((MPEG4GenericRTPSource*)ourSource);
}


////////// samplingFrequencyFromAudioSpecificConfig() implementation //////////

static unsigned const samplingFrequencyFromIndex[16] = {
  96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
  16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

unsigned samplingFrequencyFromAudioSpecificConfig(char const* configStr) {
  unsigned char* config = NULL;
  unsigned result = 0; // if returned, indicates an error

  do {
    // Begin by parsing the config string:
    unsigned configSize;
    config = parseGeneralConfigStr(configStr, configSize);
    if (config == NULL) break;

    if (configSize < 2) break;
    unsigned char samplingFrequencyIndex = ((config[0]&0x07)<<1) | (config[1]>>7);
    if (samplingFrequencyIndex < 15) {
      result = samplingFrequencyFromIndex[samplingFrequencyIndex];
      break;
    }

    // Index == 15 means that the actual frequency is next (24 bits):
    if (configSize < 5) break;
    result = ((config[1]&0x7F)<<17) | (config[2]<<9) | (config[3]<<1) | (config[4]>>7);
  } while (0);

  delete[] config;
  return result;
}
