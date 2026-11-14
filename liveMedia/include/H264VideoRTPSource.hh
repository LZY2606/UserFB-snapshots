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
// C++ header

#ifndef _H264_VIDEO_RTP_SOURCE_HH
#define _H264_VIDEO_RTP_SOURCE_HH

#ifndef _MULTI_FRAMED_RTP_SOURCE_HH
#include "MultiFramedRTPSource.hh"
#endif

class H264MultiSSRCReception; // internal per-SSRC reassembly/reordering state

class H264VideoRTPSource: public MultiFramedRTPSource {
public:
  static H264VideoRTPSource*
  createNew(UsageEnvironment& env, Groupsock* RTPgs,
	    unsigned char rtpPayloadFormat,
	    unsigned rtpTimestampFrequency = 90000);

  // Multi-SSRC reception.  By default (False), this source behaves exactly
  // like a single-SSRC receiver: a change of SSRC resets the reordering state,
  // and only one in-progress NAL unit can exist at a time.
  // When enabled, packets from several SSRCs arriving interleaved on the same
  // RTP channel are tracked independently.  Each SSRC gets its own sequence
  // number reordering, FU-A reassembly, loss detection and media timing, and
  // complete NAL units are delivered through the usual getNextFrame() callback
  // in the order in which they become ready.  Must be called before the first
  // getNextFrame(); no extra sockets or port rebinding are used.
  void enableMultiSSRCReception(Boolean enable);
  Boolean multiSSRCReceptionEnabled() const { return fMultiSSRCReceptionEnabled; }

  // SSRC of the NAL unit most recently delivered to the getNextFrame() callback.
  u_int32_t curPacketSSRC() const { return fCurPacketSSRC; }

  // Also applies the threshold to each per-SSRC reordering queue:
  virtual void setPacketReorderingThresholdTime(unsigned uSeconds);

protected:
  H264VideoRTPSource(UsageEnvironment& env, Groupsock* RTPgs,
			 unsigned char rtpPayloadFormat,
			 unsigned rtpTimestampFrequency);
      // called only by createNew()

  virtual ~H264VideoRTPSource();

protected:
  // redefined virtual functions:
  virtual Boolean processSpecialHeader(BufferedPacket* packet,
                                       unsigned& resultSpecialHeaderSize);
  virtual char const* MIMEtype() const;
  virtual void doGetNextFrame();
  virtual void doStopGettingFrames();

  // Hooks for the per-SSRC reception state (only active when multi-SSRC
  // reception is enabled).  The default implementations cover single NAL
  // units, STAP-A and FU-A; subclasses can extend them.
  virtual void resetReceptionState();

private:
  // redefined virtual functions:
  virtual Boolean handleReceivedPacket(BufferedPacket* bPacket, u_int32_t rtpSSRC);
  virtual void afterFrameDelivered();

  // Used by H264MultiSSRCReception:
  friend class H264MultiSSRCReception;
  void noteReadyDataAvailable(); // a complete NAL became deliverable
  BufferedPacket* acquireBufferedPacket();
  void releaseAcquiredBufferedPacket(BufferedPacket* packet);
  void deliverNextReadyNAL();
  static void deliveryTask(void* clientData); // event-loop delivery trampoline

private:
  friend class H264BufferedPacket;
  unsigned char fCurPacketNALUnitType;

protected:
  Boolean fMultiSSRCReceptionEnabled;
  u_int32_t fCurPacketSSRC; // SSRC of the frame currently being delivered

private:
  H264MultiSSRCReception* fMultiSSRCReception;
  Boolean fDeliveryTaskPending; // an afterGetting event-loop task is queued
};

class SPropRecord {
public:
  ~SPropRecord() { delete[] sPropBytes; }

  unsigned sPropLength; // in bytes
  unsigned char* sPropBytes;
};

SPropRecord* parseSPropParameterSets(char const* sPropParameterSetsStr,
				     // result parameter:
				     unsigned& numSPropRecords);
    // Returns the binary value of each 'parameter set' specified in a
    // "sprop-parameter-sets" string (in the SDP description for a H.264/RTP stream).
    // The value is returned as an array (length "numSPropRecords") of "SPropRecord"s.
    // This array is dynamically allocated by this routine, and must be delete[]d by the caller.

#endif
