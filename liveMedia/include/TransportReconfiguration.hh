/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version. (See <http://www.gnu.org/copyleft/lesser.html>.)
**********/
// "liveMedia"
// Copyright (c) 1996-2026 Live Networks, Inc.  All rights reserved.
//
// TransportReconfiguration.hh
//
// Stateless validation rules used by the in-place transport reconfiguration path
// (see "RTSPServer::enableTransportReconfiguration()").  Keeping these as plain functions lets the
// port/channel accept rules be tested independently of the server state machine.

#ifndef _TRANSPORT_RECONFIGURATION_HH
#define _TRANSPORT_RECONFIGURATION_HH

#ifndef _NET_ADDRESS_HH
#include "NetAddress.hh" // portNumBits
#endif
#ifndef _BOOLEAN_HH
#include "Boolean.hh"
#endif

// RTP/AVP unicast UDP: the client_port pair must be two distinct non-zero 16-bit ports.
Boolean transportReconfig_isValidUdpPortPair(portNumBits clientRTPPortNum,
					     portNumBits clientRTCPPortNum);

// RTP/AVP/TCP: the explicit interleaved pair must use two distinct channel ids (each 0..255).
Boolean transportReconfig_isValidTcpChannelPair(unsigned char rtpChannelId,
						unsigned char rtcpChannelId);

// True if a requested (rtp,rtcp) channel pair overlaps a pair already assigned to another track.
Boolean transportReconfig_channelCollides(unsigned rtpChannelId,
					  unsigned rtcpChannelId,
					  unsigned otherRtpChannelId,
					  unsigned otherRtcpChannelId);

#endif
