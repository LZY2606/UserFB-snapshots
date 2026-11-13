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

////////// MPEG4GenericBufferedPacket and MPEG4GenericBufferedPacketFactory

class MPEG4GenericBufferedPacket: public BufferedPacket {
public:
  MPEG4GenericBufferedPacket(MPEG4GenericRTPSource* ourSource);
  virtual ~MPEG4GenericBufferedPacket();

private: // redefined virtual functions
  virtual void getNextEnclosedFrameParameters(unsigned char*& framePtr,
					      unsigned dataSize,
					      unsigned& frameSize,
					      int& frameDurationInMicroseconds);
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
  unsigned index; // indexDelta for the 2nd & subsequent headers
  Boolean ctsDeltaValid;
  int ctsDelta; // the signed "CTS-delta" (in RTP timestamp ticks), if "ctsDeltaValid"
  long long presentationTimeOffset;
      // this AU's presentation-time offset (in microseconds) from the
      // presentation time of the packet's RTP timestamp (the packet's 'anchor')
};


///////// MPEG4GenericRTPSource implementation ////////

//##### NOTE: INCOMPLETE!!! Support more modes, and interleaving #####

MPEG4GenericRTPSource*
MPEG4GenericRTPSource::createNew(UsageEnvironment& env, Groupsock* RTPgs,
				 unsigned char rtpPayloadFormat,
				 unsigned rtpTimestampFrequency,
				 char const* mediumName,
				 char const* mode,
				 unsigned sizeLength, unsigned indexLength,
				 unsigned indexDeltaLength,
				 unsigned constantDuration,
				 unsigned ctsDeltaLength
				 ) {
  // Sanity-check the AU-header field widths; each must fit within the
  // 32-bit fields that our bit-parsing supports:
  if (sizeLength > 32 || indexLength > 32 || indexDeltaLength > 32
      || ctsDeltaLength > 32) {
    env.setResultMsg("MPEG4GenericRTPSource: AU-header field width(s) out of range (> 32)");
    return NULL;
  }

  return new MPEG4GenericRTPSource(env, RTPgs, rtpPayloadFormat,
				   rtpTimestampFrequency, mediumName,
				   mode, sizeLength, indexLength,
				   indexDeltaLength, constantDuration,
				   ctsDeltaLength
				   );
}

MPEG4GenericRTPSource
::MPEG4GenericRTPSource(UsageEnvironment& env, Groupsock* RTPgs,
			unsigned char rtpPayloadFormat,
			unsigned rtpTimestampFrequency,
			char const* mediumName,
                        char const* mode,
                        unsigned sizeLength, unsigned indexLength,
                        unsigned indexDeltaLength,
                        unsigned constantDuration, unsigned ctsDeltaLength
                        )
  : MultiFramedRTPSource(env, RTPgs,
			 rtpPayloadFormat, rtpTimestampFrequency,
			 new MPEG4GenericBufferedPacketFactory),
  fSizeLength(sizeLength), fIndexLength(indexLength),
  fIndexDeltaLength(indexDeltaLength),
  fConstantDuration(constantDuration), fCTSDeltaLength(ctsDeltaLength),
  fNumAUHeaders(0), fNextAUHeader(0), fAUHeaders(NULL),
  fLastAUComplete(True),
  fAUFragmentActive(False), fAUFragmentFullSize(0),
  fAUFragmentBytesRemaining(0), fAUFragmentRTPTimestamp(0) {
    unsigned mimeTypeLength =
      strlen(mediumName) + 14 /* strlen("/MPEG4-GENERIC") */ + 1;
    fMIMEType = new char[mimeTypeLength];
    if (fMIMEType != NULL) {
      sprintf(fMIMEType, "%s/MPEG4-GENERIC", mediumName);
    }

    fMode = strDup(mode);
    // Check for a "mode" that we don't yet support: //#####
    if (mode == NULL ||
	(strcmp(mode, "aac-hbr") != 0 && strcmp(mode, "generic") != 0)) {
      envir() << "MPEG4GenericRTPSource Warning: Unknown or unsupported \"mode\": "
	      << mode << "\n";
    }
}

MPEG4GenericRTPSource::~MPEG4GenericRTPSource() {
  delete[] fAUHeaders;
  delete[] fMode;
  delete[] fMIMEType;
}

// Convert a (signed) count of RTP timestamp ticks to microseconds, rounding
// to the nearest microsecond.  Each AU's offset is converted independently
// (exactly once), so rounding errors don't accumulate across AUs:
static long long rtpTicksToMicroseconds(int numTicks, unsigned timestampFrequency) {
  if (timestampFrequency == 0) return 0;
  long long const num = (long long)numTicks*1000000;
  long long const denom = (long long)timestampFrequency;
  return num >= 0 ? (num + denom/2)/denom : -((-num + denom/2)/denom);
}

Boolean MPEG4GenericRTPSource
::processSpecialHeader(BufferedPacket* packet,
		       unsigned& resultSpecialHeaderSize) {
  unsigned char* headerStart = packet->data();
  unsigned packetSize = packet->dataSize();

  fCurrentPacketBeginsFrame = fCurrentPacketCompletesFrame;
          // whether the *previous* packet ended a frame

  // default values:
  resultSpecialHeaderSize = 0;
  fNumAUHeaders = 0;
  fNextAUHeader = 0;
  delete[] fAUHeaders; fAUHeaders = NULL;
  fLastAUComplete = True;
  fCurrentPacketCompletesFrame = packet->rtpMarkerBit();

  if (fSizeLength == 0) return True; // there's no "AU Header Section" to parse

  // The packet begins with a "AU Header Section".  Parse it, to
  // determine the "AU-header"s for each frame present in this packet:
  AUHeader* auHeaders = NULL;
  do {
    resultSpecialHeaderSize = 2;
    if (packetSize < resultSpecialHeaderSize) break;

    unsigned AU_headers_length = (headerStart[0]<<8)|headerStart[1]; // in bits, not bytes
    unsigned AU_headers_length_bytes = (AU_headers_length+7)/8;
    if (packetSize < resultSpecialHeaderSize + AU_headers_length_bytes) break;
    resultSpecialHeaderSize += AU_headers_length_bytes;
    unsigned const payloadBytes = packetSize - resultSpecialHeaderSize;

    if (AU_headers_length == 0) return True; // no AU-headers; the payload is a single frame

    // Parse each AU-header in turn, tracking a genuine bit cursor.  Each
    // header must fit within the declared bit length, and the last header
    // must end exactly at it (unused bits in the final byte, if any, are
    // padding, not the start of a new header):
    unsigned const ctsFlagSize = fCTSDeltaLength > 0 ? 1 : 0;
    unsigned const minHeaderSize = fSizeLength + ctsFlagSize; // (index sizes can be 0)
    unsigned const maxAUHeaders = AU_headers_length/minHeaderSize + 1;
    auHeaders = new AUHeader[maxAUHeaders];
    unsigned numAUHeaders = 0;
    Boolean headersAreMalformed = False;
    BitVector bv(&headerStart[2], 0, AU_headers_length);
    while (bv.numBitsRemaining() > 0) {
      unsigned const indexSize = numAUHeaders == 0 ? fIndexLength : fIndexDeltaLength;
      if (bv.numBitsRemaining() < fSizeLength + indexSize + ctsFlagSize) {
	headersAreMalformed = True; break;
      }
      AUHeader& auHeader = auHeaders[numAUHeaders];
      auHeader.size = bv.getBits(fSizeLength);
      auHeader.index = bv.getBits(indexSize);
      auHeader.ctsDeltaValid = False;
      auHeader.ctsDelta = 0;
      if (fCTSDeltaLength > 0 && bv.get1Bit() != 0) {
	// A "CTS-delta" field is present in this AU-header:
	if (bv.numBitsRemaining() < fCTSDeltaLength) {
	  headersAreMalformed = True; break;
	}
	unsigned raw = bv.getBits(fCTSDeltaLength);
	// Interpret it as a signed, two's-complement value:
	if (fCTSDeltaLength < 32) {
	  u_int32_t const signBit = 1u << (fCTSDeltaLength - 1);
	  raw = (raw ^ signBit) - signBit;
	}
	auHeader.ctsDelta = (int)raw;
	auHeader.ctsDeltaValid = True;
      }
      ++numAUHeaders;
    }
    if (headersAreMalformed) break;

    if (!fCurrentPacketBeginsFrame) {
      // This packet should contain the next fragment of an AU that began in
      // an earlier packet.  It does so only if its (single) AU-header is for
      // the same AU, and its payload fits within the AU's remaining bytes:
      if (!fAUFragmentActive || numAUHeaders != 1
	  || auHeaders[0].size != fAUFragmentFullSize
	  || packet->rtpTimestamp() != fAUFragmentRTPTimestamp
	  || payloadBytes == 0 || payloadBytes > fAUFragmentBytesRemaining) break;
      fAUFragmentBytesRemaining -= payloadBytes;
      fLastAUComplete = fAUFragmentBytesRemaining == 0;
      if (fLastAUComplete != packet->rtpMarkerBit()) break; // the "M" bit disagrees
      if (fLastAUComplete) fAUFragmentActive = False;
      auHeaders[0].presentationTimeOffset = 0; // (not used for continuations)
      fCurrentPacketCompletesFrame = fLastAUComplete;
      fAUHeaders = auHeaders;
      fNumAUHeaders = numAUHeaders;
      return True;
    }

    // This packet begins a new frame (or frames).  Assign each AU a
    // presentation-time offset from the packet's RTP timestamp 'anchor':
    if (auHeaders[0].ctsDeltaValid) break;
        // the first AU of a packet must not carry an explicit "CTS-delta"
    if (fConstantDuration != 0 && auHeaders[0].index != 0) break;
        // with "constantDuration", the first AU's index must be 0
    auHeaders[0].presentationTimeOffset = 0;
    unsigned relativeIndex = 0; // of AU #0 (its index is 0 whenever it matters)
    for (unsigned i = 1; i < numAUHeaders; ++i) {
      relativeIndex = relativeIndex + auHeaders[i].index + 1; // (mod 2^32)
      int tickOffset;
      if (auHeaders[i].ctsDeltaValid) {
	tickOffset = auHeaders[i].ctsDelta; // explicit; no duration is added
      } else if (fConstantDuration != 0) {
	tickOffset = (int)(relativeIndex * fConstantDuration); // (mod 2^32)
      } else {
	tickOffset = 0; // no timing information; use the packet's time (as before)
      }
      auHeaders[i].presentationTimeOffset
	= rtpTicksToMicroseconds(tickOffset, timestampFrequency());
    }

    // Check the AU sizes against the payload, noting any trailing fragment:
    unsigned payloadBytesRemaining = payloadBytes;
    for (unsigned i = 0; i < numAUHeaders; ++i) {
      unsigned const auSize = auHeaders[i].size;
      if (auSize <= payloadBytesRemaining) {
	payloadBytesRemaining -= auSize;
	continue; // this AU is fully contained in the packet
      }
      // This AU is not fully contained in the packet.  That's allowed only
      // for the last AU, and only if the "M" bit says that it's incomplete:
      if (i != numAUHeaders-1 || packet->rtpMarkerBit()) {
	headersAreMalformed = True; break;
      }
      fAUFragmentActive = True;
      fAUFragmentFullSize = auSize;
      fAUFragmentBytesRemaining = auSize - payloadBytesRemaining;
      fAUFragmentRTPTimestamp = packet->rtpTimestamp();
      payloadBytesRemaining = 0;
      fLastAUComplete = False;
    }
    if (headersAreMalformed) break;
    if (payloadBytesRemaining != 0) break; // the AU sizes don't match the payload
    if (fLastAUComplete != packet->rtpMarkerBit()) break; // the "M" bit disagrees
    fCurrentPacketCompletesFrame = fLastAUComplete;

    fAUHeaders = auHeaders;
    fNumAUHeaders = numAUHeaders;
    return True;
  } while (0);

  // The packet is malformed.  Drop it (including any apparently-complete AUs
  // that it contains), drop any incomplete AU that was being assembled, and
  // leave the AU-header state clean for subsequent packets:
  delete[] auHeaders;
  resultSpecialHeaderSize = 0;
  fAUFragmentActive = False;
  fAUFragmentBytesRemaining = 0;
  fCurrentPacketCompletesFrame = True; // so the next packet begins a new frame
  notePacketLossInFragmentedFrame(); // so partial delivery state is discarded
  return False;
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
::getNextEnclosedFrameParameters(unsigned char*& /*framePtr*/, unsigned dataSize,
				 unsigned& frameSize,
				 int& frameDurationInMicroseconds) {
  // WE CURRENTLY DON'T IMPLEMENT INTERLEAVING.  FIX THIS! #####
  MPEG4GenericRTPSource* ourSource = fOurSource;
  frameDurationInMicroseconds = 0; // by default
  AUHeader* auHeader = ourSource->fAUHeaders;
  if (auHeader == NULL) {
    frameSize = dataSize;
    return;
  }
  unsigned numAUHeaders = ourSource->fNumAUHeaders;
  unsigned nextAUHeader = ourSource->fNextAUHeader;

  if (nextAUHeader >= numAUHeaders) {
    ourSource->envir() << "MPEG4GenericBufferedPacket::getNextEnclosedFrameParameters("
		       << dataSize << "): data error ("
		       << nextAUHeader << " >= " << numAUHeaders << ")!\n";
    frameSize = dataSize;
    return;
  }

  auHeader = &auHeader[nextAUHeader];
  frameSize = auHeader->size <= dataSize ? auHeader->size : dataSize;

  // The duration from this AU to the next one in this packet (if any).
  // Because each AU's offset was computed independently from the packet's
  // anchor, these differences telescope without accumulating rounding error:
  if (nextAUHeader+1 < numAUHeaders) {
    long long duration
      = auHeader[1].presentationTimeOffset - auHeader[0].presentationTimeOffset;
    if (duration > 0x7FFFFFFF) duration = 0x7FFFFFFF;
    if (duration < -0x7FFFFFFF) duration = -0x7FFFFFFF;
    frameDurationInMicroseconds = (int)duration;
  }

  // Update the source's frame-boundary flags for this AU, so that each
  // complete AU is delivered individually, even if the packet's last AU is
  // only a fragment (the RTP "M" bit describes only that last AU):
  ourSource->fCurrentPacketCompletesFrame
    = nextAUHeader+1 < numAUHeaders ? True : ourSource->fLastAUComplete;
  ourSource->fCurrentPacketBeginsFrame = True; // for the next AU (if any) here

  ++ourSource->fNextAUHeader;
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
