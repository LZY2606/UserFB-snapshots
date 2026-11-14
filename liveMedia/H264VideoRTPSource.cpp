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
// H.264 Video RTP Sources
// Implementation

#include "H264VideoRTPSource.hh"
#include "H264MultiSSRCReception.hh"
#include "Base64.hh"
#include <string.h>

////////// H264BufferedPacket and H264BufferedPacketFactory //////////

class H264BufferedPacket: public BufferedPacket {
public:
  H264BufferedPacket(H264VideoRTPSource& ourSource);
  virtual ~H264BufferedPacket();

private: // redefined virtual functions
  virtual unsigned nextEnclosedFrameSize(unsigned char*& framePtr,
					 unsigned dataSize);
private:
  H264VideoRTPSource& fOurSource;
};

class H264BufferedPacketFactory: public BufferedPacketFactory {
private: // redefined virtual functions
  virtual BufferedPacket* createNewPacket(MultiFramedRTPSource* ourSource);
};


///////// H264VideoRTPSource implementation ////////

H264VideoRTPSource*
H264VideoRTPSource::createNew(UsageEnvironment& env, Groupsock* RTPgs,
			      unsigned char rtpPayloadFormat,
			      unsigned rtpTimestampFrequency) {
  return new H264VideoRTPSource(env, RTPgs, rtpPayloadFormat,
					rtpTimestampFrequency);
}

H264VideoRTPSource
::H264VideoRTPSource(UsageEnvironment& env, Groupsock* RTPgs,
		     unsigned char rtpPayloadFormat,
		     unsigned rtpTimestampFrequency)
  : MultiFramedRTPSource(env, RTPgs, rtpPayloadFormat, rtpTimestampFrequency,
			 new H264BufferedPacketFactory),
    fCurPacketNALUnitType(0), fMultiSSRCReceptionEnabled(False),
    fCurPacketSSRC(0), fMultiSSRCReception(NULL),
    fDeliveryTaskPending(False) {
}

H264VideoRTPSource::~H264VideoRTPSource() {
  delete fMultiSSRCReception;
}

void H264VideoRTPSource::enableMultiSSRCReception(Boolean enable) {
  if (enable == fMultiSSRCReceptionEnabled) return;

  fMultiSSRCReceptionEnabled = enable;
  if (enable) {
    if (fMultiSSRCReception == NULL) {
      fMultiSSRCReception = new H264MultiSSRCReception(*this);
    }
  } else {
    delete fMultiSSRCReception;
    fMultiSSRCReception = NULL;
  }
  fCurPacketSSRC = 0;
}

void H264VideoRTPSource::resetReceptionState() {
  if (fMultiSSRCReception != NULL) fMultiSSRCReception->reset();
  fCurPacketSSRC = 0;
}

void H264VideoRTPSource::doGetNextFrame() {
  fDeliveryTaskPending = False;
  // The base implementation starts background network reading (if needed)
  // and drives the single-SSRC reordering buffer.  In multi-SSRC mode that
  // buffer stays empty because every usable packet is claimed by
  // handleReceivedPacket(), so this simply prepares the delivery request:
  MultiFramedRTPSource::doGetNextFrame();

  if (fMultiSSRCReceptionEnabled) deliverNextReadyNAL();
}

void H264VideoRTPSource::doStopGettingFrames() {
  // First the base teardown: stop network reads, drop the base reordering
  // buffer and any in-flight packet descriptor (this also cancels nextTask):
  MultiFramedRTPSource::doStopGettingFrames();
  fDeliveryTaskPending = False;

  // Then forget every per-SSRC reordering queue, half-assembled FU-A NAL and
  // NAL unit waiting for delivery, so a later getNextFrame() starts clean:
  resetReceptionState();
}

Boolean H264VideoRTPSource
::handleReceivedPacket(BufferedPacket* bPacket, u_int32_t rtpSSRC) {
  if (!fMultiSSRCReceptionEnabled || fMultiSSRCReception == NULL) {
    return False; // normal single-SSRC processing in the base class
  }
  fMultiSSRCReception->handleReceivedPacket(bPacket, rtpSSRC);
  return True; // we own the packet now
}

void H264VideoRTPSource::afterFrameDelivered() {
  if (fMultiSSRCReceptionEnabled) deliverNextReadyNAL();
}

void H264VideoRTPSource
::setPacketReorderingThresholdTime(unsigned uSeconds) {
  MultiFramedRTPSource::setPacketReorderingThresholdTime(uSeconds);
  if (fMultiSSRCReception != NULL) {
    fMultiSSRCReception->setThresholdTime(uSeconds);
  }
}

BufferedPacket* H264VideoRTPSource::acquireBufferedPacket() {
  return getFreeBufferedPacket();
}

void H264VideoRTPSource
::releaseAcquiredBufferedPacket(BufferedPacket* packet) {
  releaseBufferedPacket(packet);
}

void H264VideoRTPSource::noteReadyDataAvailable() {
  // Only deliver if a client is currently waiting for a frame; otherwise the
  // NAL stays queued until the next getNextFrame():
  if (isCurrentlyAwaitingData()) deliverNextReadyNAL();
}

void H264VideoRTPSource::deliverNextReadyNAL() {
  if (!fMultiSSRCReceptionEnabled || fMultiSSRCReception == NULL) return;
  if (!isCurrentlyAwaitingData()) return;
  // A frame already assembled into the client's buffer is waiting for its
  // event-loop delivery.  Do not pop another NAL over it; the pending task
  // drains the queue when it runs:
  if (fDeliveryTaskPending) return;

  H264ReadyNAL* nal = fMultiSSRCReception->popReadyNAL();
  if (nal == NULL) return; // nothing ready yet; the network handler will wake us

  // Assemble the frame into the client's buffer, with the same truncation
  // semantics as the normal BufferedPacket delivery path:
  unsigned char* savedTo = savedToRef();
  unsigned savedMaxSize = savedMaxSizeRef();
  unsigned nalSize = nal->size();
  unsigned bytesUsed = nalSize <= savedMaxSize ? nalSize : savedMaxSize;
  numTruncatedBytesRef() = nalSize - bytesUsed;
  memmove(savedTo, nal->data(), bytesUsed);
  frameSizeRef() = bytesUsed;

  if (numTruncatedBytesRef() > 0) {
    envir() << "H264VideoRTPSource::deliverNextReadyNAL(): The received NAL unit size exceeds the client's buffer size ("
	    << savedMaxSize << ").  "
	    << numTruncatedBytesRef() << " bytes of trailing data will be dropped!\n";
  }

  // Metadata of the delivered frame.  For FU-A the timestamp/presentation
  // time come from the start fragment and the sequence number/marker from
  // the end fragment; H264ReadyNAL already recorded exactly those values.
  fCurPacketSSRC = nal->ssrc();
  setCurrentDeliveryParams(nal->endSeqNum(), nal->rtpTimestamp(),
			   nal->presentationTime(),
			   nal->hasBeenSyncedUsingRTCP(), nal->markerBit());
  delete nal;

  if (fMultiSSRCReception->hasReadyData()) {
    // More NAL units are queued.  Deliver this one through the event loop to
    // avoid unbounded recursion through the client's callback.  The guard
    // ensures a packet arriving re-entrantly while a delivery task is already
    // queued never overwrites "nextTask()" with a second task:
    if (!fDeliveryTaskPending) {
      fDeliveryTaskPending = True;
      nextTask() = envir().taskScheduler()
	.scheduleDelayedTask(0, (TaskFunc*)deliveryTask, this);
    }
  } else {
    // Common case: nothing else is queued, so a direct callback is safe.
    deliverCurrentFrame();
  }
}

void H264VideoRTPSource::deliveryTask(void* clientData) {
  H264VideoRTPSource* source = static_cast<H264VideoRTPSource*>(clientData);
  source->fDeliveryTaskPending = False;

  // Deliver the frame already assembled into the client's buffer:
  FramedSource::afterGetting(source);

  // The client callback has either re-armed a read or stopped.  Keep draining
  // any further ready NAL units while a read is still outstanding:
  if (source->isCurrentlyAwaitingData()) source->deliverNextReadyNAL();
}

Boolean H264VideoRTPSource
::processSpecialHeader(BufferedPacket* packet,
                       unsigned& resultSpecialHeaderSize) {
  unsigned char* headerStart = packet->data();
  unsigned packetSize = packet->dataSize();
  unsigned numBytesToSkip;

  // Check the 'nal_unit_type' for special 'aggregation' or 'fragmentation' packets:
  if (packetSize < 1) return False;
  fCurPacketNALUnitType = (headerStart[0]&0x1F);
  switch (fCurPacketNALUnitType) {
  case 24: { // STAP-A
    numBytesToSkip = 1; // discard the type byte
    break;
  }
  case 25: case 26: case 27: { // STAP-B, MTAP16, or MTAP24
    numBytesToSkip = 3; // discard the type byte, and the initial DON
    break;
  }
  case 28: case 29: { // // FU-A or FU-B
    // For these NALUs, the first two bytes are the FU indicator and the FU header.
    // If the start bit is set, we reconstruct the original NAL header into byte 1:
    if (packetSize < 2) return False;
    unsigned char startBit = headerStart[1]&0x80;
    unsigned char endBit = headerStart[1]&0x40;
    if (startBit) {
      fCurrentPacketBeginsFrame = True;

      headerStart[1] = (headerStart[0]&0xE0)|(headerStart[1]&0x1F);
      numBytesToSkip = 1;
    } else {
      // The start bit is not set, so we skip both the FU indicator and header:
      fCurrentPacketBeginsFrame = False;
      numBytesToSkip = 2;
    }
    fCurrentPacketCompletesFrame = (endBit != 0);
    break;
  }
  default: {
    // This packet contains one complete NAL unit:
    fCurrentPacketBeginsFrame = fCurrentPacketCompletesFrame = True;
    numBytesToSkip = 0;
    break;
  }
  }

  resultSpecialHeaderSize = numBytesToSkip;
  return True;
}

char const* H264VideoRTPSource::MIMEtype() const {
  return "video/H264";
}

SPropRecord* parseSPropParameterSets(char const* sPropParameterSetsStr,
                                     // result parameter:
                                     unsigned& numSPropRecords) {
  // Make a copy of the input string, so we can replace the commas with '\0's:
  char* inStr = strDup(sPropParameterSetsStr);
  if (inStr == NULL) {
    numSPropRecords = 0;
    return NULL;
  }

  // Count the number of commas (and thus the number of parameter sets):
  numSPropRecords = 1;
  char* s;
  for (s = inStr; *s != '\0'; ++s) {
    if (*s == ',') {
      ++numSPropRecords;
      *s = '\0';
    }
  }

  // Allocate and fill in the result array:
  SPropRecord* resultArray = new SPropRecord[numSPropRecords];
  s = inStr;
  for (unsigned i = 0; i < numSPropRecords; ++i) {
    resultArray[i].sPropBytes = base64Decode(s, resultArray[i].sPropLength);
    s += strlen(s) + 1;
  }

  delete[] inStr;
  return resultArray;
}


////////// H264BufferedPacket and H264BufferedPacketFactory implementation //////////

H264BufferedPacket::H264BufferedPacket(H264VideoRTPSource& ourSource)
  : fOurSource(ourSource) {
}

H264BufferedPacket::~H264BufferedPacket() {
}

unsigned H264BufferedPacket
::nextEnclosedFrameSize(unsigned char*& framePtr, unsigned dataSize) {
  unsigned resultNALUSize = 0; // if an error occurs

  switch (fOurSource.fCurPacketNALUnitType) {
  case 24: case 25: { // STAP-A or STAP-B
    // The first two bytes are NALU size:
    if (dataSize < 2) break;
    resultNALUSize = (framePtr[0]<<8)|framePtr[1];
    framePtr += 2;
    break;
  }
  case 26: { // MTAP16
    // The first two bytes are NALU size.  The next three are the DOND and TS offset:
    if (dataSize < 5) break;
    resultNALUSize = (framePtr[0]<<8)|framePtr[1];
    framePtr += 5;
    break;
  }
  case 27: { // MTAP24
    // The first two bytes are NALU size.  The next four are the DOND and TS offset:
    if (dataSize < 6) break;
    resultNALUSize = (framePtr[0]<<8)|framePtr[1];
    framePtr += 6;
    break;
  }
  default: {
    // Common case: We use the entire packet data:
    return dataSize;
  }
  }

  return (resultNALUSize <= dataSize) ? resultNALUSize : dataSize;
}

BufferedPacket* H264BufferedPacketFactory
::createNewPacket(MultiFramedRTPSource* ourSource) {
  return new H264BufferedPacket((H264VideoRTPSource&)(*ourSource));
}
