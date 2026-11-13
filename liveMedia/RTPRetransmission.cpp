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
// RTP retransmission (RFC 4588) - outer-stream reception statistics
// Implementation

#include "RTPRetransmission.hh"
#include "MultiFramedRTPSource.hh"

////////// RTPRetransmissionStats implementation //////////

RTPRetransmissionStats::RTPRetransmissionStats(u_int32_t rtxSSRC)
  : fSSRC(rtxSSRC),
    fTotNumPacketsReceived(0), fNumPacketsRecovered(0),
    fHaveSeenInitialSequenceNumber(False),
    fBaseExtSeqNumReceived(0), fHighestExtSeqNumReceived(0),
    fTotBytesReceived_hi(0), fTotBytesReceived_lo(0) {
}

void RTPRetransmissionStats
::noteIncomingPacket(u_int16_t outerSeqNum, unsigned packetSize) {
  ++fTotNumPacketsReceived;

  u_int32_t prevLo = fTotBytesReceived_lo;
  fTotBytesReceived_lo += packetSize;
  if (fTotBytesReceived_lo < prevLo) ++fTotBytesReceived_hi; // wrap-around

  // Track the outer (RTX) sequence-number space exactly like the primary
  // reception stats do, so an RTCP report over this stream would see the
  // real retransmission-stream delivery pattern.
  if (!fHaveSeenInitialSequenceNumber) {
    fBaseExtSeqNumReceived = 0x10000 | outerSeqNum;
    fHighestExtSeqNumReceived = 0x10000 | outerSeqNum;
    fHaveSeenInitialSequenceNumber = True;
    return;
  }

  unsigned oldSeqNum = (fHighestExtSeqNumReceived&0xFFFF);
  unsigned seqNumCycle = (fHighestExtSeqNumReceived&0xFFFF0000);
  int seqNumDifference = (int)outerSeqNum-(int)oldSeqNum;
  if (seqNumLT((u_int16_t)oldSeqNum, outerSeqNum)) {
    if (seqNumDifference >= 0x8000) seqNumCycle += 0x10000;
    unsigned newSeqNum = seqNumCycle|outerSeqNum;
    if (newSeqNum > fHighestExtSeqNumReceived) {
      fHighestExtSeqNumReceived = newSeqNum;
    }
  }
}

double RTPRetransmissionStats::totNumKBytesReceived() const {
  double const hiMultiplier = 0x20000000/125.0; // == (2^32)/(10^3)
  return fTotBytesReceived_hi*hiMultiplier + fTotBytesReceived_lo/1000.0;
}
