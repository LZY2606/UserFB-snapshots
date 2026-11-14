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
// Per-SSRC reception state for H264VideoRTPSource's multi-SSRC mode.
// Implementation.  See H264MultiSSRCReception.hh for the design.

#include "H264MultiSSRCReception.hh"
#include <string.h>
#include <sys/time.h>

#define H264_NAL_TYPE_STAP_A 24
#define H264_NAL_TYPE_FU_A   28
#define H264_MAX_REASSEMBLED_NAL_SIZE 1000000

////////// H264ReadyNAL //////////

H264ReadyNAL::H264ReadyNAL(u_int32_t ssrc, unsigned char* nalData, unsigned nalSize,
			   u_int16_t startSeqNum, u_int16_t endSeqNum,
			   u_int32_t rtpTimestamp, struct timeval presentationTime,
			   Boolean hasBeenSyncedUsingRTCP, Boolean markerBit,
			   H264ReadyNAL* next)
  : fSSRC(ssrc), fData(new unsigned char[nalSize]), fSize(nalSize),
    fStartSeqNum(startSeqNum), fEndSeqNum(endSeqNum),
    fRTPTimestamp(rtpTimestamp), fPresentationTime(presentationTime),
    fHasBeenSyncedUsingRTCP(hasBeenSyncedUsingRTCP), fMarkerBit(markerBit),
    fNext(next) {
  memmove(fData, nalData, nalSize);
}

H264ReadyNAL::~H264ReadyNAL() {
  delete[] fData;
}

////////// H264SSRCState //////////

H264SSRCState::H264SSRCState(u_int32_t ssrc, H264MultiSSRCReception& reception)
  : fSSRC(ssrc), fReception(reception), fNext(NULL),
    fHeadPacket(NULL), fTailPacket(NULL),
    fHaveSeenFirstPacket(False), fNextExpectedSeqNo(0),
    fFragmentActive(False), fFragmentBroken(False),
    fFragmentBuffer(NULL), fFragmentSize(0), fFragmentCapacity(0),
    fFragmentStartSeqNum(0), fFragmentEndSeqNum(0),
    fFragmentRTPTimestamp(0), fFragmentSyncedUsingRTCP(False) {
  fFragmentPresentationTime.tv_sec = fFragmentPresentationTime.tv_usec = 0;
}

H264SSRCState::~H264SSRCState() {
  reset();
}

void H264SSRCState::reset() {
  // Release every packet still held in the reordering queue:
  while (fHeadPacket != NULL) {
    BufferedPacket* next = fHeadPacket->nextPacket();
    fHeadPacket->nextPacket() = NULL;
    releasePacket(fHeadPacket);
    fHeadPacket = next;
  }
  fTailPacket = NULL;
  fHaveSeenFirstPacket = False;

  // Discard any half-assembled FU-A NAL:
  delete[] fFragmentBuffer;
  fFragmentBuffer = NULL;
  fFragmentSize = fFragmentCapacity = 0;
  fFragmentActive = False;
  fFragmentBroken = False;
}

void H264SSRCState::releasePacket(BufferedPacket* packet) {
  fReception.releasePacket(packet);
}

// Sequence-number ordered insertion.  This is the same reordering algorithm
// as ReorderingPacketBuffer, kept independent per SSRC so that a gap or
// wrap-around in one source never affects another.
void H264SSRCState::storePacket(BufferedPacket* bPacket) {
  unsigned short rtpSeqNo = bPacket->rtpSeqNo();

  if (!fHaveSeenFirstPacket) {
    fNextExpectedSeqNo = rtpSeqNo; // initialization
    bPacket->isFirstPacket() = True;
    fHaveSeenFirstPacket = True;
  }

  // Older than the sequence number we are committed to: excessively delayed.
  if (seqNumLT(rtpSeqNo, fNextExpectedSeqNo)) {
    releasePacket(bPacket);
    return;
  }

  if (fTailPacket == NULL) {
    bPacket->nextPacket() = NULL;
    fHeadPacket = fTailPacket = bPacket;
    return;
  }

  if (seqNumLT(fTailPacket->rtpSeqNo(), rtpSeqNo)) {
    // In order: append to the tail.
    bPacket->nextPacket() = NULL;
    fTailPacket->nextPacket() = bPacket;
    fTailPacket = bPacket;
    return;
  }

  if (rtpSeqNo == fTailPacket->rtpSeqNo()) {
    // Duplicate: ignore.
    releasePacket(bPacket);
    return;
  }

  // Out of order: walk from the head to find its place.
  BufferedPacket* beforePtr = NULL;
  BufferedPacket* afterPtr = fHeadPacket;
  while (afterPtr != NULL) {
    if (seqNumLT(rtpSeqNo, afterPtr->rtpSeqNo())) break;
    if (rtpSeqNo == afterPtr->rtpSeqNo()) {
      // Duplicate: ignore.
      releasePacket(bPacket);
      return;
    }
    beforePtr = afterPtr;
    afterPtr = afterPtr->nextPacket();
  }

  bPacket->nextPacket() = afterPtr;
  if (beforePtr == NULL) {
    fHeadPacket = bPacket;
  } else {
    beforePtr->nextPacket() = bPacket;
  }
}

BufferedPacket* H264SSRCState::takeNextDeliverable(Boolean& lossPreceded) {
  lossPreceded = False;
  if (fHeadPacket == NULL) return NULL;

  if (fHeadPacket->rtpSeqNo() == fNextExpectedSeqNo) {
    lossPreceded = fHeadPacket->isFirstPacket();
    BufferedPacket* packet = fHeadPacket;
    fHeadPacket = packet->nextPacket();
    if (fHeadPacket == NULL) fTailPacket = NULL;
    packet->nextPacket() = NULL;
    ++fNextExpectedSeqNo; // 16-bit wrap is handled by the unsigned short type
    return packet;
  }

  // There is a sequence-number gap.  Wait for the missing packet unless the
  // reordering time threshold has been exceeded:
  Boolean timeThresholdHasBeenExceeded;
  unsigned threshold = fReception.thresholdTime();
  if (threshold == 0) {
    timeThresholdHasBeenExceeded = True;
  } else {
    struct timeval timeNow;
    gettimeofday(&timeNow, NULL);
    unsigned uSecondsSinceReceived
      = (timeNow.tv_sec - fHeadPacket->timeReceived().tv_sec)*1000000
      + (timeNow.tv_usec - fHeadPacket->timeReceived().tv_usec);
    timeThresholdHasBeenExceeded = uSecondsSinceReceived > threshold;
  }
  if (!timeThresholdHasBeenExceeded) return NULL;

  // Give up on the missing packet(s) and skip ahead:
  fNextExpectedSeqNo = fHeadPacket->rtpSeqNo();
  lossPreceded = True;
  BufferedPacket* packet = fHeadPacket;
  fHeadPacket = packet->nextPacket();
  if (fHeadPacket == NULL) fTailPacket = NULL;
  packet->nextPacket() = NULL;
  ++fNextExpectedSeqNo;
  return packet;
}

void H264SSRCState::handlePacket(BufferedPacket* packet) {
  storePacket(packet);
  drain();
}

void H264SSRCState::drain() {
  BufferedPacket* deliverable;
  Boolean lossPreceded;
  while ((deliverable = takeNextDeliverable(lossPreceded)) != NULL) {
    processPacket(deliverable, lossPreceded);
    releasePacket(deliverable);
  }
}

void H264SSRCState::appendFragmentByte(unsigned char b) {
  if (fFragmentSize >= fFragmentCapacity) {
    unsigned newCapacity = fFragmentCapacity == 0 ? 256 : fFragmentCapacity*2;
    unsigned char* newBuffer = new unsigned char[newCapacity];
    if (fFragmentSize > 0) memmove(newBuffer, fFragmentBuffer, fFragmentSize);
    delete[] fFragmentBuffer;
    fFragmentBuffer = newBuffer;
    fFragmentCapacity = newCapacity;
  }
  fFragmentBuffer[fFragmentSize++] = b;
}

void H264SSRCState::appendFragmentData(unsigned char const* data, unsigned size) {
  if (fFragmentSize + size > fFragmentCapacity) {
    unsigned newCapacity = fFragmentCapacity == 0 ? 256 : fFragmentCapacity;
    while (newCapacity < fFragmentSize + size) newCapacity *= 2;
    unsigned char* newBuffer = new unsigned char[newCapacity];
    if (fFragmentSize > 0) memmove(newBuffer, fFragmentBuffer, fFragmentSize);
    delete[] fFragmentBuffer;
    fFragmentBuffer = newBuffer;
    fFragmentCapacity = newCapacity;
  }
  memmove(&fFragmentBuffer[fFragmentSize], data, size);
  fFragmentSize += size;
}

void H264SSRCState::abortFragment() {
  delete[] fFragmentBuffer;
  fFragmentBuffer = NULL;
  fFragmentSize = fFragmentCapacity = 0;
  fFragmentActive = False;
  fFragmentBroken = False;
}

void H264SSRCState::enqueueReadyNAL(unsigned char const* nalData, unsigned nalSize,
				    u_int16_t startSeqNum, u_int16_t endSeqNum,
				    u_int32_t rtpTimestamp,
				    struct timeval presentationTime,
				    Boolean hasBeenSyncedUsingRTCP,
				    Boolean markerBit) {
  if (nalSize == 0) return;
  H264ReadyNAL* nal = new H264ReadyNAL(fSSRC,
    const_cast<unsigned char*>(nalData), nalSize,
    startSeqNum, endSeqNum, rtpTimestamp, presentationTime,
    hasBeenSyncedUsingRTCP, markerBit, NULL);
  fReception.enqueueReadyNAL(nal);
}

void H264SSRCState::processSingleNAL(BufferedPacket* packet) {
  // The whole payload is one complete NAL unit:
  enqueueReadyNAL(packet->data(), packet->dataSize(),
		  packet->rtpSeqNo(), packet->rtpSeqNo(),
		  packet->rtpTimestamp(), packet->presentationTime(),
		  packet->hasBeenSyncedUsingRTCP(), packet->rtpMarkerBit());
}

void H264SSRCState::processSTAP_A(BufferedPacket* packet) {
  unsigned char const* payload = packet->data();
  unsigned size = packet->dataSize();
  if (size < 1) return; // malformed

  // Skip the STAP-A header byte; each enclosed NAL is prefixed by its
  // 16-bit size.  Enclosed NALs keep their in-packet order.
  unsigned offset = 1;
  while (offset + 2 <= size) {
    unsigned naluSize = (payload[offset]<<8)|payload[offset+1];
    offset += 2;
    if (offset + naluSize > size) break; // malformed tail; stop here
    enqueueReadyNAL(&payload[offset], naluSize,
		    packet->rtpSeqNo(), packet->rtpSeqNo(),
		    packet->rtpTimestamp(), packet->presentationTime(),
		    packet->hasBeenSyncedUsingRTCP(), packet->rtpMarkerBit());
    offset += naluSize;
  }
}

void H264SSRCState::processFUA(BufferedPacket* packet, Boolean lossPreceded) {
  unsigned char const* payload = packet->data();
  unsigned size = packet->dataSize();
  if (size < 2) return; // malformed

  unsigned char fuIndicator = payload[0];
  unsigned char fuHeader = payload[1];
  Boolean startBit = (fuHeader&0x80) != 0;
  Boolean endBit = (fuHeader&0x40) != 0;

  if (startBit) {
    if (fFragmentActive) {
      // The previous NAL's end fragment never arrived.  Drop it; the new
      // start bit begins a fresh, recoverable NAL.
      abortFragment();
    }
    fFragmentActive = True;
    fFragmentBroken = False;

    // Reconstruct the original NAL header:
    unsigned char nalHeader = (fuIndicator&0xE0)|(fuHeader&0x1F);
    appendFragmentByte(nalHeader);
    if (size > 2) appendFragmentData(&payload[2], size-2);

    // Timing comes from the START fragment:
    fFragmentStartSeqNum = packet->rtpSeqNo();
    fFragmentRTPTimestamp = packet->rtpTimestamp();
    fFragmentPresentationTime = packet->presentationTime();
    fFragmentSyncedUsingRTCP = packet->hasBeenSyncedUsingRTCP();

    if (fFragmentSize > H264_MAX_REASSEMBLED_NAL_SIZE) {
      // Pathological input: abandon this NAL but keep receiving.
      fFragmentBroken = True;
    }

    if (endBit) {
      // A complete NAL carried in a single FU-A packet:
      fFragmentEndSeqNum = packet->rtpSeqNo();
      if (!fFragmentBroken) {
	enqueueReadyNAL(fFragmentBuffer, fFragmentSize,
			fFragmentStartSeqNum, fFragmentEndSeqNum,
			fFragmentRTPTimestamp, fFragmentPresentationTime,
			fFragmentSyncedUsingRTCP, packet->rtpMarkerBit());
      }
      abortFragment();
    }
    return;
  }

  // Continuation (or end) fragment:
  if (!fFragmentActive) {
    // The start fragment was lost.  This can never become a valid NAL;
    // ignore it and stay ready for the next complete NAL.
    return;
  }
  if (lossPreceded) {
    // A fragment in the middle of this NAL went missing: the NAL is unusable.
    fFragmentBroken = True;
  }
  if (fFragmentBroken) {
    // Don't spend memory on a NAL we will discard; just wait for the end bit.
  } else if (size > 2) {
    appendFragmentData(&payload[2], size-2);
    if (fFragmentSize > H264_MAX_REASSEMBLED_NAL_SIZE) fFragmentBroken = True;
  }

  if (endBit) {
    fFragmentEndSeqNum = packet->rtpSeqNo();
    if (!fFragmentBroken) {
      // Sequence number and marker come from the END fragment; the timestamp
      // and presentation time come from the start fragment.
      enqueueReadyNAL(fFragmentBuffer, fFragmentSize,
		      fFragmentStartSeqNum, fFragmentEndSeqNum,
		      fFragmentRTPTimestamp, fFragmentPresentationTime,
		      fFragmentSyncedUsingRTCP, packet->rtpMarkerBit());
    }
    abortFragment();
  }
}

void H264SSRCState::processPacket(BufferedPacket* packet, Boolean lossPreceded) {
  if (packet->dataSize() < 1) return;
  unsigned char nalUnitType = packet->data()[0]&0x1F;

  switch (nalUnitType) {
  case H264_NAL_TYPE_STAP_A:
    processSTAP_A(packet);
    break;
  case H264_NAL_TYPE_FU_A:
    processFUA(packet, lossPreceded);
    break;
  default:
    // A single, complete NAL unit (aggregation/fragmentation types outside
    // the required scope are passed through unchanged as well):
    processSingleNAL(packet);
    break;
  }
}

////////// H264MultiSSRCReception //////////

H264MultiSSRCReception::H264MultiSSRCReception(H264VideoRTPSource& source)
  : fSource(source), fStates(NULL), fReadyHead(NULL), fReadyTail(NULL),
    fThresholdTime(100000), fDrainTask(NULL) {
}

H264MultiSSRCReception::~H264MultiSSRCReception() {
  reset();
}

BufferedPacket* H264MultiSSRCReception::getFreePacket() {
  return fSource.acquireBufferedPacket();
}

void H264MultiSSRCReception::releasePacket(BufferedPacket* packet) {
  fSource.releaseAcquiredBufferedPacket(packet);
}

H264SSRCState* H264MultiSSRCReception::lookupOrCreateState(u_int32_t ssrc) {
  for (H264SSRCState* state = fStates; state != NULL; state = state->next()) {
    if (state->ssrc() == ssrc) return state;
  }
  H264SSRCState* state = new H264SSRCState(ssrc, *this);
  state->setNext(fStates);
  fStates = state;
  return state;
}

void H264MultiSSRCReception::handleReceivedPacket(BufferedPacket* packet,
						  u_int32_t ssrc) {
  H264SSRCState* state = lookupOrCreateState(ssrc);
  state->handlePacket(packet);

  // Re-evaluate every other source as well: its gap may now be older than the
  // reordering threshold, and a missing packet must not be kept waiting just
  // because no further packet for that SSRC has arrived.
  for (H264SSRCState* other = fStates; other != NULL;
       other = other->next()) {
    if (other != state) other->drain();
  }
  updateDrainTimer();
}

void H264MultiSSRCReception::updateDrainTimer() {
  TaskScheduler& scheduler = fSource.envir().taskScheduler();
  if (fDrainTask != NULL) {
    scheduler.unscheduleDelayedTask(fDrainTask);
    fDrainTask = NULL;
  }
  if (fThresholdTime == 0) return; // gaps are skipped immediately on arrival

  for (H264SSRCState* state = fStates; state != NULL;
       state = state->next()) {
    if (state->hasGapAhead()) {
      fDrainTask = scheduler.scheduleDelayedTask(fThresholdTime + 1000,
						 drainTimerTask, this);
      break;
    }
  }
}

void H264MultiSSRCReception::drainTimerTask(void* clientData) {
  H264MultiSSRCReception* reception =
    static_cast<H264MultiSSRCReception*>(clientData);
  reception->fDrainTask = NULL;
  for (H264SSRCState* state = reception->fStates; state != NULL;
       state = state->next()) {
    state->drain();
  }
  reception->updateDrainTimer();
}

void H264MultiSSRCReception::enqueueReadyNAL(H264ReadyNAL* nal) {
  if (fReadyTail == NULL) {
    fReadyHead = fReadyTail = nal;
  } else {
    fReadyTail->next() = nal;
    fReadyTail = nal;
  }
  fSource.noteReadyDataAvailable();
}

H264ReadyNAL* H264MultiSSRCReception::popReadyNAL() {
  H264ReadyNAL* nal = fReadyHead;
  if (nal != NULL) {
    fReadyHead = nal->next();
    if (fReadyHead == NULL) fReadyTail = NULL;
    nal->next() = NULL;
  }
  return nal;
}

void H264MultiSSRCReception::reset() {
  if (fDrainTask != NULL) {
    fSource.envir().taskScheduler().unscheduleDelayedTask(fDrainTask);
    fDrainTask = NULL;
  }

  while (fStates != NULL) {
    H264SSRCState* next = fStates->next();
    delete fStates; // releases queued packets and any in-progress fragment
    fStates = next;
  }

  while (fReadyHead != NULL) {
    H264ReadyNAL* next = fReadyHead->next();
    delete fReadyHead;
    fReadyHead = next;
  }
  fReadyTail = NULL;
}
