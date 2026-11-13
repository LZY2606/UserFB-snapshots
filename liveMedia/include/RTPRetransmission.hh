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
// RTP retransmission (RFC 4588) support for receiving media subsessions.
//
// This header describes the *static* configuration of one retransmission
// association: a single primary payload type / retransmission payload type
// pair, and the single FID (RFC 5576 "a=ssrc-group:FID") SSRC pair that
// carries the primary stream and its retransmission stream.  It is parsed
// from the SDP by "MediaSubsession" and handed to the subsession's
// "MultiFramedRTPSource", which performs the actual packet recovery.
// C++ header

#ifndef _RTP_RETRANSMISSION_HH
#define _RTP_RETRANSMISSION_HH

#ifndef _BOOLEAN_HH
#include "Boolean.hh"
#endif
#ifndef _NET_COMMON_H
#include "NetCommon.h" // for u_int16_t/u_int32_t
#endif

struct RTPRetransmissionConfig {
  unsigned char primaryPayloadType; // payload type of the original codec stream
  unsigned char rtxPayloadType;     // payload type used by retransmission packets ("rtx")
  u_int32_t primarySSRC;            // SSRC of the original codec stream (first FID member)
  u_int32_t rtxSSRC;                // SSRC of the retransmission stream (second FID member)

  RTPRetransmissionConfig()
    : primaryPayloadType(0xFF), rtxPayloadType(0xFF),
      primarySSRC(0), rtxSSRC(0) {
  }

  Boolean isValid() const {
    // The two payload types and the two SSRCs must be distinct; an SSRC of 0
    // is treated as "not configured" (SDP SSRC identifiers are 32-bit unsigned
    // values, and 0 is reserved in the RTP architecture).
    return rtxPayloadType != 0xFF
      && rtxPayloadType != primaryPayloadType
      && primarySSRC != 0 && rtxSSRC != 0
      && primarySSRC != rtxSSRC;
  }
};


// Reception statistics for the outer retransmission stream.  These are kept
// deliberately separate from the primary stream's "RTPReceptionStatsDB":
// retransmission packets are accounted for here, under the RTX SSRC and with
// their own (outer) sequence numbers, so the primary reception reports keep
// reflecting the packet loss that actually occurred on the primary stream.
class RTPRetransmissionStats {
public:
  RTPRetransmissionStats(u_int32_t rtxSSRC);

  u_int32_t SSRC() const { return fSSRC; }
  unsigned totNumPacketsReceived() const { return fTotNumPacketsReceived; }
  double totNumKBytesReceived() const;

  // Number of retransmission packets that passed validation and were used to
  // fill a gap (i.e., were not duplicates of already-queued/delivered OSNs).
  unsigned numPacketsRecovered() const { return fNumPacketsRecovered; }

  // Number of retransmission packets that were discarded: bad PT/SSRC, short
  // OSN header, truncated extension, bad padding, or a duplicate OSN.
  unsigned numPacketsRejected() const {
    return fTotNumPacketsReceived - fNumPacketsRecovered;
  }

  unsigned baseExtSeqNumReceived() const { return fBaseExtSeqNumReceived; }
  unsigned highestExtSeqNumReceived() const { return fHighestExtSeqNumReceived; }

protected:
  friend class MultiFramedRTPSource;
  void noteIncomingPacket(u_int16_t outerSeqNum, unsigned packetSize);
  void noteRecoveredPacket() { ++fNumPacketsRecovered; }

private:
  u_int32_t fSSRC;
  unsigned fTotNumPacketsReceived;
  unsigned fNumPacketsRecovered;
  Boolean fHaveSeenInitialSequenceNumber;
  unsigned fBaseExtSeqNumReceived;
  unsigned fHighestExtSeqNumReceived;
  u_int32_t fTotBytesReceived_hi, fTotBytesReceived_lo;
};

#endif
