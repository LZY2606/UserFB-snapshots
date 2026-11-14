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
// RTP source for a common kind of payload format: Those which pack multiple,
// complete codec frames (as many as possible) into each RTP packet.
// C++ header

#ifndef _MULTI_FRAMED_RTP_SOURCE_HH
#define _MULTI_FRAMED_RTP_SOURCE_HH

#ifndef _RTP_SOURCE_HH
#include "RTPSource.hh"
#endif

class BufferedPacket; // forward
class BufferedPacketFactory; // forward

class MultiFramedRTPSource: public RTPSource {
protected:
  MultiFramedRTPSource(UsageEnvironment& env, Groupsock* RTPgs,
		       unsigned char rtpPayloadFormat,
		       unsigned rtpTimestampFrequency,
		       BufferedPacketFactory* packetFactory = NULL);
      // virtual base class
  virtual ~MultiFramedRTPSource();

  virtual Boolean processSpecialHeader(BufferedPacket* packet,
				       unsigned& resultSpecialHeaderSize);
      // Subclasses redefine this to handle any special, payload format
      // specific header that follows the RTP header.

  virtual Boolean packetIsUsableInJitterCalculation(unsigned char* packet,
						    unsigned packetSize);
      // The default implementation returns True, but this can be redefined

  // Called from the network read handler after the RTP header has been
  // validated, reception stats have been recorded, and the packet descriptor
  // has had its RTP parameters assigned.  If this returns True, the subclass
  // has taken ownership of the packet (it must eventually free or consume it)
  // and the base class's single-SSRC reordering/storage logic is skipped.
  // Returning False gives the packet the normal single-SSRC treatment.
  virtual Boolean handleReceivedPacket(BufferedPacket* bPacket, u_int32_t rtpSSRC);

  // Called immediately after a complete frame has been delivered to the
  // client.  A subclass that still holds more ready frames can arrange another
  // delivery here by scheduling an afterGetting task.  Default: do nothing.
  virtual void afterFrameDelivered();

protected:
  // Delivery state used by subclasses that implement their own packet
  // reassembly (see "handleReceivedPacket()"):
  unsigned char*& savedToRef() { return fSavedTo; }
  unsigned& savedMaxSizeRef() { return fSavedMaxSize; }
  unsigned& frameSizeRef() { return fFrameSize; }
  unsigned& numTruncatedBytesRef() { return fNumTruncatedBytes; }
  void setCurrentDeliveryParams(u_int16_t seqNum, u_int32_t rtpTimestamp,
				struct timeval presentationTime,
				Boolean hasBeenSyncedUsingRTCP,
				Boolean markerBit);
  void deliverCurrentFrame(); // invoke the client's afterGetting callback

  // Packet-descriptor pool, backed by the same reordering buffer as the
  // normal receive path.  Subclasses that consume packets themselves (see
  // "handleReceivedPacket()") use these instead of allocating descriptors:
  BufferedPacket* getFreeBufferedPacket();
  void releaseBufferedPacket(BufferedPacket* packet);

protected:
  Boolean fCurrentPacketBeginsFrame;
  Boolean fCurrentPacketCompletesFrame;

protected:
  // redefined virtual functions:
  virtual void doGetNextFrame();
  virtual void doStopGettingFrames();
  virtual void setPacketReorderingThresholdTime(unsigned uSeconds);

private:
  void reset();
  void doGetNextFrame1();

  static void networkReadHandler(MultiFramedRTPSource* source, int /*mask*/);
  void networkReadHandler1();

  Boolean fAreDoingNetworkReads;
  BufferedPacket* fPacketReadInProgress;
  Boolean fNeedDelivery;
  Boolean fPacketLossInFragmentedFrame;
  unsigned char* fSavedTo;
  unsigned fSavedMaxSize;

  // A buffer to (optionally) hold incoming pkts that have been reorderered
  class ReorderingPacketBuffer* fReorderingBuffer;
};


// A 'packet data' class that's used to implement the above.
// Note that this can be subclassed - if desired - to redefine
// "nextEnclosedFrameParameters()".

class BufferedPacket {
public:
  BufferedPacket();
  virtual ~BufferedPacket();

  Boolean hasUsableData() const { return fTail > fHead; }
  unsigned useCount() const { return fUseCount; }

  Boolean fillInData(RTPInterface& rtpInterface, struct sockaddr_storage& fromAddress, Boolean& packetReadWasIncomplete);
  void assignMiscParams(unsigned short rtpSeqNo, unsigned rtpTimestamp,
			struct timeval presentationTime,
			Boolean hasBeenSyncedUsingRTCP,
			Boolean rtpMarkerBit, struct timeval timeReceived);
  void skip(unsigned numBytes); // used to skip over an initial header
  void removePadding(unsigned numBytes); // used to remove trailing bytes
  void appendData(unsigned char* newData, unsigned numBytes);
  void use(unsigned char* to, unsigned toSize,
	   unsigned& bytesUsed, unsigned& bytesTruncated,
	   unsigned short& rtpSeqNo, unsigned& rtpTimestamp,
	   struct timeval& presentationTime,
	   Boolean& hasBeenSyncedUsingRTCP, Boolean& rtpMarkerBit);

  BufferedPacket*& nextPacket() { return fNextPacket; }

  unsigned short rtpSeqNo() const { return fRTPSeqNo; }
  unsigned rtpTimestamp() const { return fRTPTimestamp; }
  struct timeval const& presentationTime() const { return fPresentationTime; }
  Boolean hasBeenSyncedUsingRTCP() const { return fHasBeenSyncedUsingRTCP; }
  struct timeval const& timeReceived() const { return fTimeReceived; }

  unsigned char* data() const { return &fBuf[fHead]; }
  unsigned dataSize() const { return fTail-fHead; }
  Boolean rtpMarkerBit() const { return fRTPMarkerBit; }
  Boolean& isFirstPacket() { return fIsFirstPacket; }
  unsigned bytesAvailable() const { return fPacketSize - fTail; }

protected:
  virtual void reset();
  virtual unsigned nextEnclosedFrameSize(unsigned char*& framePtr,
					 unsigned dataSize);
      // The above function has been deprecated.  Instead, new subclasses should use:
  virtual void getNextEnclosedFrameParameters(unsigned char*& framePtr,
					      unsigned dataSize,
					      unsigned& frameSize,
					      unsigned& frameDurationInMicroseconds);

  unsigned fPacketSize;
  unsigned char* fBuf;
  unsigned fHead;
  unsigned fTail;

private:
  BufferedPacket* fNextPacket; // used to link together packets

  unsigned fUseCount;
  unsigned short fRTPSeqNo;
  unsigned fRTPTimestamp;
  struct timeval fPresentationTime; // corresponding to "fRTPTimestamp"
  Boolean fHasBeenSyncedUsingRTCP;
  Boolean fRTPMarkerBit;
  Boolean fIsFirstPacket;
  struct timeval fTimeReceived;
};

// A 'factory' class for creating "BufferedPacket" objects.
// If you want to subclass "BufferedPacket", then you'll also
// want to subclass this, to redefine createNewPacket()

class BufferedPacketFactory {
public:
  BufferedPacketFactory();
  virtual ~BufferedPacketFactory();

  virtual BufferedPacket* createNewPacket(MultiFramedRTPSource* ourSource);
};

#endif
