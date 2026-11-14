/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version. (See <http://www.gnu.org/copyleft/lesser.html>.)
**********/
// "liveMedia"
// Copyright (c) 1996-2026 Live Networks, Inc.  All rights reserved.
//
// TransportReconfiguration.cpp
//
// Pure validation helpers for in-place transport reconfiguration
// (RTSPServer::enableTransportReconfiguration()).  They are deliberately free of any object state
// so the accept/reject rules for a re-SETUP can be reasoned about - and unit-tested - in
// isolation from the RTSPServer/OnDemandServerMediaSubsession state machine.
//
// The reconfiguration path accepts only:
//   * a complete RTP/AVP unicast UDP client port pair: two distinct, non-zero 16-bit ports, or
//   * an explicit RTP/AVP/TCP interleaved channel pair: two distinct integers in 0..255.
// Anything else (RAW_UDP, multicast, implicit/missing channels, bad ports) is rejected with
// "461 Unsupported Transport" and the existing transport is left untouched.

#include "TransportReconfiguration.hh"

Boolean transportReconfig_isValidUdpPortPair(portNumBits clientRTPPortNum,
					     portNumBits clientRTCPPortNum) {
  // Both ports must be non-zero (a full pair) and distinct 16-bit values:
  return clientRTPPortNum != 0
      && clientRTCPPortNum != 0
      && clientRTPPortNum != clientRTCPPortNum;
}

Boolean transportReconfig_isValidTcpChannelPair(unsigned char rtpChannelId,
						unsigned char rtcpChannelId) {
  // Both channels are implicitly in 0..255 (the parameter type guarantees the range); they only
  // need to differ.  A missing/implicit "interleaved=" pair is represented by 0xFF upstream and is
  // checked there before calling this routine.
  return rtpChannelId != rtcpChannelId;
}

Boolean transportReconfig_channelCollides(unsigned rtpChannelId,
					  unsigned rtcpChannelId,
					  unsigned otherRtpChannelId,
					  unsigned otherRtcpChannelId) {
  // True if either requested channel overlaps either channel already assigned to another track on
  // the same RTSP control connection.
  return rtpChannelId == otherRtpChannelId
      || rtpChannelId == otherRtcpChannelId
      || rtcpChannelId == otherRtpChannelId
      || rtcpChannelId == otherRtcpChannelId;
}
