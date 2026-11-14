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
// This is a private header (not installed); only H264VideoRTPSource and the
// implementation file include it.
//
// One H264MultiSSRCReception object owns:
//   - one H264SSRCState per incoming SSRC (independent sequence-number
//     reordering, FU-A reassembly and loss detection), and
//   - a single FIFO of fully reassembled NAL units ("H264ReadyNAL") that are
//     handed to the getNextFrame() client in readiness order.

#ifndef _H264_MULTI_SSRC_RECEPTION_HH
#define _H264_MULTI_SSRC_RECEPTION_HH

#ifndef _H264_VIDEO_RTP_SOURCE_HH
#include "H264VideoRTPSource.hh"
#endif

// A fully reassembled NAL unit waiting to be delivered to the client.
class H264ReadyNAL {
public:
  H264ReadyNAL(u_int32_t ssrc, unsigned char* nalData, unsigned nalSize,
	       u_int16_t startSeqNum, u_int16_t endSeqNum,
	       u_int32_t rtpTimestamp, struct timeval presentationTime,
	       Boolean hasBeenSyncedUsingRTCP, Boolean markerBit,
	       H264ReadyNAL* next);
  ~H264ReadyNAL();

  u_int32_t ssrc() const { return fSSRC; }
  unsigned char* data() const { return fData; }
  unsigned size() const { return fSize; }
  u_int16_t startSeqNum() const { return fStartSeqNum; }
  u_int16_t endSeqNum() const { return fEndSeqNum; }
  u_int32_t rtpTimestamp() const { return fRTPTimestamp; }
  struct timeval const& presentationTime() const { return fPresentationTime; }
  Boolean hasBeenSyncedUsingRTCP() const { return fHasBeenSyncedUsingRTCP; }
  Boolean markerBit() const { return fMarkerBit; }
  H264ReadyNAL*& next() { return fNext; }

private:
  u_int32_t fSSRC;
  unsigned char* fData; // owned
  unsigned fSize;
  u_int16_t fStartSeqNum; // FU-A: first fragment; otherwise the packet's seq num
  u_int16_t fEndSeqNum;   // FU-A: last fragment; otherwise the packet's seq num
  u_int32_t fRTPTimestamp;
  struct timeval fPresentationTime;
  Boolean fHasBeenSyncedUsingRTCP;
  Boolean fMarkerBit;
  H264ReadyNAL* fNext;
};

class H264MultiSSRCReception; // forward

// All reception state belonging to a single SSRC.
class H264SSRCState {
public:
  H264SSRCState(u_int32_t ssrc, H264MultiSSRCReception& reception);
  ~H264SSRCState();

  // Takes ownership of "packet" (it is eventually consumed or released back
  // to the source's packet pool):
  void handlePacket(BufferedPacket* packet);

  // Re-evaluate the reordering threshold for everything already queued
  // (called by the reception's drain timer and when any other SSRC receives
  // a packet, so a gap in one source cannot hide behind another source):
  void drain();

  // True when packets are queued but the next expected sequence number has
  // not arrived yet (i.e. a drain timer is worth scheduling):
  Boolean hasGapAhead() const {
    return fHeadPacket != NULL && fHeadPacket->rtpSeqNo() != fNextExpectedSeqNo;
  }

  // Discard every buffered packet, the in-progress FU-A NAL, and all
  // sequence-number state (used by stopGettingFrames / teardown):
  void reset();

  u_int32_t ssrc() const { return fSSRC; }
  H264SSRCState* next() const { return fNext; }
  void setNext(H264SSRCState* next) { fNext = next; }

private:
  // Sequence-number reordering (same algorithm as ReorderingPacketBuffer,
  // but per SSRC):
  void storePacket(BufferedPacket* packet);
  BufferedPacket* takeNextDeliverable(Boolean& lossPreceded);
  void drainDeliverablePackets();
  void releasePacket(BufferedPacket* packet);

  // NAL unit reassembly:
  void processPacket(BufferedPacket* packet, Boolean lossPreceded);
  void processSingleNAL(BufferedPacket* packet);
  void processSTAP_A(BufferedPacket* packet);
  void processFUA(BufferedPacket* packet, Boolean lossPreceded);
  void abortFragment(); // discard an incomplete FU-A NAL, stay recoverable
  void enqueueReadyNAL(unsigned char const* nalData, unsigned nalSize,
		       u_int16_t startSeqNum, u_int16_t endSeqNum,
		       u_int32_t rtpTimestamp,
		       struct timeval presentationTime,
		       Boolean hasBeenSyncedUsingRTCP, Boolean markerBit);

  void appendFragmentByte(unsigned char b);
  void appendFragmentData(unsigned char const* data, unsigned size);

private:
  u_int32_t fSSRC;
  H264MultiSSRCReception& fReception;
  H264SSRCState* fNext;

  // Reordering queue, kept in ascending sequence-number order:
  BufferedPacket* fHeadPacket;
  BufferedPacket* fTailPacket;
  Boolean fHaveSeenFirstPacket;
  u_int16_t fNextExpectedSeqNo;

  // FU-A reassembly state:
  Boolean fFragmentActive;
  Boolean fFragmentBroken; // a middle fragment was lost; do not emit this NAL
  unsigned char* fFragmentBuffer; // owned
  unsigned fFragmentSize;
  unsigned fFragmentCapacity;
  u_int16_t fFragmentStartSeqNum;
  u_int16_t fFragmentEndSeqNum;
  u_int32_t fFragmentRTPTimestamp; // from the start fragment
  struct timeval fFragmentPresentationTime; // from the start fragment
  Boolean fFragmentSyncedUsingRTCP; // from the start fragment
};

class H264MultiSSRCReception {
public:
  H264MultiSSRCReception(H264VideoRTPSource& source);
  ~H264MultiSSRCReception();

  // Entry point used by H264VideoRTPSource::handleReceivedPacket():
  void handleReceivedPacket(BufferedPacket* packet, u_int32_t ssrc);

  // Forget all per-SSRC state and all ready NAL units:
  void reset();

  // Ready-NAL FIFO:
  Boolean hasReadyData() const { return fReadyHead != NULL; }
  H264ReadyNAL* popReadyNAL();

  void setThresholdTime(unsigned uSeconds) { fThresholdTime = uSeconds; }
  unsigned thresholdTime() const { return fThresholdTime; }

  H264VideoRTPSource& source() { return fSource; }

  // Packet-descriptor pool wrappers (backed by the source's
  // ReorderingPacketBuffer so no parallel allocation strategy is needed):
  BufferedPacket* getFreePacket();
  void releasePacket(BufferedPacket* packet);

private:
  H264SSRCState* lookupOrCreateState(u_int32_t ssrc);
  void enqueueReadyNAL(H264ReadyNAL* nal);
  void updateDrainTimer();
  static void drainTimerTask(void* clientData);
  friend class H264SSRCState;

  H264VideoRTPSource& fSource;
  H264SSRCState* fStates;
  H264ReadyNAL* fReadyHead;
  H264ReadyNAL* fReadyTail;
  unsigned fThresholdTime; // uSeconds to wait for an out-of-order packet
  TaskToken fDrainTask;    // re-evaluates threshold-blocked gaps
};

#endif
