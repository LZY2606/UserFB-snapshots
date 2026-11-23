/*
 * Regression tests for the PCMU conference mixer added to JRTPLIB.
 * Every packet on the wire is produced and consumed through the public
 * RTP/RTCP classes; the expected audio is computed independently from the
 * ITU-T G.711 conversions exposed by the mixer.
 */

#include "../task_support/check.h"
#include "rtpsources.h"
#include "rtppacketbuilder.h"
#include "rtppacket.h"
#include "rtprawpacket.h"
#include "rtpipv4address.h"
#include "rtppcmumixer.h"
#include "rtcppacketbuilder.h"
#include "rtcpcompoundpacket.h"
#include "rtcppacket.h"
#include "rtcpsrpacket.h"
#include "rtptimeutilities.h"
#include <vector>
#include <cstring>
#include <cstdint>

namespace
{

const u_int32_t SSRC_A = 100;
const u_int32_t SSRC_B = 200;
const u_int32_t SSRC_C = 300;
const u_int32_t BASE_TS = 1000;
const uint64_t START_US = 100000;
const size_t FRAME = 160;

RTPRawPacket *makeRaw(u_int32_t ssrc,u_int16_t seq,u_int32_t ts,
                      const u_int8_t *payload,size_t len,u_int8_t pt = 0,
                      u_int8_t numcsrcs = 0,const u_int32_t *csrcs = 0)
{
	RTPPacket p(pt,payload,len,seq,ts,ssrc,false,numcsrcs,csrcs,false,0,0,0);
	if (p.GetCreationError() < 0)
		return 0;
	u_int8_t *data = new u_int8_t[p.GetPacketLength()];
	std::memcpy(data,p.GetPacketData(),p.GetPacketLength());
	RTPTime t(100,0);
	return new RTPRawPacket(data,p.GetPacketLength(),
	                        new RTPIPv4Address(0x7f000001,4000),t,true);
}

int feed(RTPSources &s,u_int32_t ssrc,u_int16_t seq,u_int32_t ts,
         const u_int8_t *payload,size_t len,u_int8_t pt = 0,
         u_int8_t numcsrcs = 0,const u_int32_t *csrcs = 0)
{
	RTPRawPacket *raw = makeRaw(ssrc,seq,ts,payload,len,pt,numcsrcs,csrcs);
	if (!raw)
		return -1000;
	int st = s.ProcessRawPacket(raw,(RTPTransmitter**)0,0,false);
	delete raw;
	return st;
}

int feedValue(RTPSources &s,u_int32_t ssrc,u_int16_t seq,size_t slot,u_int8_t value)
{
	u_int8_t audio[FRAME];
	std::memset(audio,value,FRAME);
	u_int32_t ts = (u_int32_t)(BASE_TS + slot * FRAME);
	return feed(s,ssrc,seq,ts,audio,FRAME);
}

RTPPacket *parse(const std::vector<u_int8_t> &wire)
{
	u_int8_t *copy = new u_int8_t[wire.size()];
	std::memcpy(copy,&wire[0],wire.size());
	RTPTime t(1,0);
	RTPRawPacket *raw = new RTPRawPacket(copy,wire.size(),0,t,true);
	RTPPacket *p = new RTPPacket(*raw);
	delete raw;
	if (p->GetCreationError() < 0)
	{
		delete p;
		return 0;
	}
	return p;
}

u_int8_t expectedSingle(u_int8_t code)
{
	int32_t sum = (int32_t)RTPPCMUMixer::PCMUDecode(code);
	if (sum > 32767) sum = 32767;
	if (sum < -32768) sum = -32768;
	return RTPPCMUMixer::PCMUEncode((int16_t)sum);
}

u_int8_t expectedTwo(u_int8_t c1,u_int8_t c2)
{
	int32_t sum = (int32_t)RTPPCMUMixer::PCMUDecode(c1)
	            + (int32_t)RTPPCMUMixer::PCMUDecode(c2);
	if (sum > 32767) sum = 32767;
	if (sum < -32768) sum = -32768;
	return RTPPCMUMixer::PCMUEncode((int16_t)sum);
}

std::vector<RTPPCMUMixInput> twoInputs()
{
	std::vector<RTPPCMUMixInput> in(2);
	in[0].ssrc = SSRC_A; in[0].timestamp = BASE_TS;
	in[1].ssrc = SSRC_B; in[1].timestamp = BASE_TS;
	return in;
}

} // namespace

// ----------------------------- slots group --------------------------------

static int slots_not_due_and_due(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);

	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US - 1,b,out) == 1);
	TASK_CHECK(out.empty());
	TASK_CHECK(b.GetPacketCount() == 0);

	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	TASK_CHECK(out.size() == 12 + FRAME);		// no contributors: bare header
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->HasMarker());					// slot 0 marker
	TASK_CHECK(p->GetPayloadType() == 0);
	TASK_CHECK(p->GetPayloadLength() == FRAME);
	TASK_CHECK(p->GetCSRCCount() == 0);
	delete p;

	// exactly one slot per call, marker only on slot 0
	TASK_CHECK(s.BuildPCMUMixPacket(START_US + 20000,b,out) == 0);
	p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(!p->HasMarker());
	delete p;
	return 0;
}

static int slots_window_boundaries(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);

	// head = 0, window = 4: slots 0..3 accepted, slot 4 is outside
	for (size_t slot = 0; slot <= 4; slot++)
		TASK_CHECK(feedValue(s,SSRC_A,(u_int16_t)(10 + slot),slot,0x42) == 0);

	for (size_t slot = 0; slot < 4; slot++)
	{
		std::vector<u_int8_t> out;
		TASK_CHECK(s.BuildPCMUMixPacket(START_US + slot * 20000,b,out) == 0);
		RTPPacket *p = parse(out);
		TASK_CHECK(p != 0);
		TASK_CHECK(p->GetCSRCCount() == 1);
		TASK_CHECK(p->GetCSRC(0) == SSRC_A);
		delete p;
	}

	// a late packet for consumed slot 0 must not revive it
	TASK_CHECK(feedValue(s,SSRC_A,99,0,0x55) == 0);

	// head is now 4: the previously rejected slot 4 falls inside [4,8)
	TASK_CHECK(feedValue(s,SSRC_A,104,4,0x42) == 0);
	// slot 8 is again one past the new window
	TASK_CHECK(feedValue(s,SSRC_A,108,8,0x42) == 0);
	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US + 4 * 20000,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 1);		// slot 4 content, slot 8 dropped
	delete p;
	return 0;
}

static int slots_first_packet_wins(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);

	TASK_CHECK(feedValue(s,SSRC_A,1,1,0x42) == 0);
	// duplicate for the same slot, different sequence number and content
	TASK_CHECK(feedValue(s,SSRC_A,2,1,0x01) == 0);

	std::vector<u_int8_t> out0;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out0) == 0);	// empty slot 0
	std::vector<u_int8_t> out1;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US + 20000,b,out1) == 0);
	RTPPacket *p = parse(out1);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 1);
	for (size_t i = 0; i < FRAME; i++)
		TASK_CHECK(p->GetPayloadData()[i] == expectedSingle(0x42));
	delete p;
	return 0;
}

static int slots_invalid_packets_excluded(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);

	u_int8_t audio[FRAME];
	std::memset(audio,0x33,FRAME);
	u_int32_t csrc = 5;

	TASK_CHECK(feed(s,SSRC_A,1,BASE_TS,audio,FRAME,8) == 0);			// wrong PT
	TASK_CHECK(feed(s,SSRC_A,2,BASE_TS,audio,100) == 0);				// bad length
	TASK_CHECK(feed(s,SSRC_A,3,BASE_TS,audio,FRAME,0,1,&csrc) == 0);	// has CSRC
	TASK_CHECK(feed(s,SSRC_A,4,BASE_TS + 10,audio,FRAME) == 0);		// unaligned
	TASK_CHECK(feed(s,SSRC_A,5,BASE_TS - 160,audio,FRAME) == 0);		// d < 0

	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 0);
	for (size_t i = 0; i < FRAME; i++)
		TASK_CHECK(p->GetPayloadData()[i] == 0xff);	// pure silence
	delete p;
	return 0;
}

static int slots_absent_source_is_silence(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,1,0,0x7e) == 0);		// B absent

	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 1);
	TASK_CHECK(p->GetCSRC(0) == SSRC_A);
	for (size_t i = 0; i < FRAME; i++)
		TASK_CHECK(p->GetPayloadData()[i] == expectedSingle(0x7e));
	delete p;
	return 0;
}

static int slots_disordered_delivery(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);

	// arrive in scrambled slot/sequence order; mixer keys on timestamp
	TASK_CHECK(feedValue(s,SSRC_A,30,2,0x22) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,28,0,0x20) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,29,1,0x21) == 0);
	TASK_CHECK(feedValue(s,SSRC_B,40,1,0x31) == 0);
	TASK_CHECK(feedValue(s,SSRC_B,39,0,0x30) == 0);

	const u_int8_t expectA[3] = { 0x20,0x21,0x22 };
	for (size_t slot = 0; slot < 3; slot++)
	{
		std::vector<u_int8_t> out;
		TASK_CHECK(s.BuildPCMUMixPacket(START_US + slot * 20000,b,out) == 0);
		RTPPacket *p = parse(out);
		TASK_CHECK(p != 0);
		TASK_CHECK(p->GetCSRCCount() == (slot == 2 ? 1 : 2));
		if (slot < 2)
		{
			TASK_CHECK(p->GetCSRC(0) == SSRC_A && p->GetCSRC(1) == SSRC_B);
			TASK_CHECK(p->GetPayloadData()[0] == expectedTwo(expectA[slot],0x30 + slot));
		}
		else
		{
			TASK_CHECK(p->GetCSRC(0) == SSRC_A);
			TASK_CHECK(p->GetPayloadData()[0] == expectedSingle(0x22));
		}
		delete p;
	}
	return 0;
}

static int slots_silence_provider_still_csrc(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);

	// source A sends a perfectly silent frame; it still contributed a packet
	u_int8_t silence[FRAME];
	std::memset(silence,0xff,FRAME);
	TASK_CHECK(feed(s,SSRC_A,1,BASE_TS,silence,FRAME) == 0);

	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 1);
	TASK_CHECK(p->GetCSRC(0) == SSRC_A);
	for (size_t i = 0; i < FRAME; i++)
		TASK_CHECK(p->GetPayloadData()[i] == 0xff);
	delete p;
	return 0;
}

// ------------------------------ wire group --------------------------------

static int wire_csrc_sorted_and_summed(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	std::vector<RTPPCMUMixInput> in(3);
	in[0].ssrc = SSRC_C; in[0].timestamp = BASE_TS;
	in[1].ssrc = SSRC_A; in[1].timestamp = BASE_TS;
	in[2].ssrc = SSRC_B; in[2].timestamp = BASE_TS;
	TASK_CHECK(s.ConfigurePCMUMix(in,START_US,4) == 0);

	TASK_CHECK(feedValue(s,SSRC_C,1,0,0x10) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,2,0,0x20) == 0);
	TASK_CHECK(feedValue(s,SSRC_B,3,0,0x30) == 0);

	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	TASK_CHECK(out.size() == 12 + 3 * 4 + FRAME);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 3);
	TASK_CHECK(p->GetCSRC(0) == SSRC_A);		// ascending SSRC order
	TASK_CHECK(p->GetCSRC(1) == SSRC_B);
	TASK_CHECK(p->GetCSRC(2) == SSRC_C);
	int32_t sum = (int32_t)RTPPCMUMixer::PCMUDecode(0x10)
	            + (int32_t)RTPPCMUMixer::PCMUDecode(0x20)
	            + (int32_t)RTPPCMUMixer::PCMUDecode(0x30);
	if (sum > 32767) sum = 32767;
	if (sum < -32768) sum = -32768;
	u_int8_t expect = RTPPCMUMixer::PCMUEncode((int16_t)sum);
	for (size_t i = 0; i < FRAME; i++)
		TASK_CHECK(p->GetPayloadData()[i] == expect);
	delete p;
	return 0;
}

static int wire_cancel_and_saturate(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);

	u_int8_t pa[FRAME],pb[FRAME];
	for (size_t i = 0; i < FRAME; i++)
	{
		if ((i & 1) == 0)
		{
			pa[i] = RTPPCMUMixer::PCMUEncode(30000);
			pb[i] = RTPPCMUMixer::PCMUEncode(30000);	// 60000 -> +saturation
		}
		else
		{
			pa[i] = RTPPCMUMixer::PCMUEncode(1234);
			pb[i] = RTPPCMUMixer::PCMUEncode(-1234);	// exact cancellation
		}
	}
	TASK_CHECK(feed(s,SSRC_A,1,BASE_TS,pa,FRAME) == 0);
	TASK_CHECK(feed(s,SSRC_B,2,BASE_TS,pb,FRAME) == 0);

	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 2);
	for (size_t i = 0; i < FRAME; i++)
	{
		int32_t wide = (int32_t)RTPPCMUMixer::PCMUDecode(pa[i])
		             + (int32_t)RTPPCMUMixer::PCMUDecode(pb[i]);
		if (wide > 32767) wide = 32767;
		if (wide < -32768) wide = -32768;
		TASK_CHECK(p->GetPayloadData()[i] == RTPPCMUMixer::PCMUEncode((int16_t)wide));
	}
	TASK_CHECK(p->GetPayloadData()[1] == 0xff);		// cancelled zero
	delete p;
	return 0;
}

static int wire_dual_wraparound(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	std::vector<RTPPCMUMixInput> in(1);
	in[0].ssrc = SSRC_A;
	in[0].timestamp = 0xFFFFFFF0u;		// slot 0 just before the wrap
	TASK_CHECK(s.ConfigurePCMUMix(in,START_US,4) == 0);

	u_int8_t audio[FRAME];
	std::memset(audio,0x66,FRAME);
	// sequence numbers 0xFFFE, 0xFFFF, 0x0000 cross zero as well
	TASK_CHECK(feed(s,SSRC_A,0xFFFE,0xFFFFFFF0u,audio,FRAME) == 0);
	TASK_CHECK(feed(s,SSRC_A,0xFFFF,0x00000090u,audio,FRAME) == 0);
	TASK_CHECK(feed(s,SSRC_A,0x0000,0x00000130u,audio,FRAME) == 0);

	for (size_t slot = 0; slot < 3; slot++)
	{
		std::vector<u_int8_t> out;
		TASK_CHECK(s.BuildPCMUMixPacket(START_US + slot * 20000,b,out) == 0);
		RTPPacket *p = parse(out);
		TASK_CHECK(p != 0);
		TASK_CHECK(p->GetCSRCCount() == 1);
		TASK_CHECK(p->GetPayloadData()[0] == expectedSingle(0x66));
		delete p;
	}
	RTPSourceData *info = s.GetSourceInfo(SSRC_A);
	TASK_CHECK(info != 0);
	TASK_CHECK(info->INF_GetNumPacketsReceived() == 3);
	return 0;
}

static int wire_small_mtu_retry_same_slot(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(100) == 0);		// too small for header + 2 CSRC + 160
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,1,0,0x42) == 0);
	TASK_CHECK(feedValue(s,SSRC_B,2,0,0x43) == 0);

	u_int16_t seqBefore = b.GetSequenceNumber();
	u_int32_t tsBefore = b.GetTimestamp();
	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) < 0);
	TASK_CHECK(out.empty());						// output vector untouched
	TASK_CHECK(b.GetPacketCount() == 0);			// counters untouched
	TASK_CHECK(b.GetSequenceNumber() == seqBefore);
	TASK_CHECK(b.GetTimestamp() == tsBefore);

	TASK_CHECK(b.SetMaximumPacketSize(512) == 0);	// raise the limit, retry
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 2);				// same slot samples survived
	TASK_CHECK(p->GetSequenceNumber() == seqBefore);
	TASK_CHECK(p->GetTimestamp() == tsBefore);
	TASK_CHECK(p->GetPayloadData()[0] == expectedTwo(0x42,0x43));
	delete p;
	TASK_CHECK(b.GetPacketCount() == 1);
	TASK_CHECK(b.GetSequenceNumber() == (u_int16_t)(seqBefore + 1));
	TASK_CHECK(b.GetTimestamp() == tsBefore + 160);
	return 0;
}

static int wire_later_failure_keeps_previous_packet(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	// 182 bytes fits a 2-CSRC packet (180) but not a 3-CSRC packet (184);
	// the buffer is never reallocated in this test, so its bytes are stable.
	TASK_CHECK(b.Init(182) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	std::vector<RTPPCMUMixInput> in(3);
	in[0].ssrc = SSRC_A; in[0].timestamp = BASE_TS;
	in[1].ssrc = SSRC_B; in[1].timestamp = BASE_TS;
	in[2].ssrc = SSRC_C; in[2].timestamp = BASE_TS;
	TASK_CHECK(s.ConfigurePCMUMix(in,START_US,4) == 0);

	TASK_CHECK(feedValue(s,SSRC_A,1,0,0x42) == 0);
	TASK_CHECK(feedValue(s,SSRC_B,2,0,0x43) == 0);
	std::vector<u_int8_t> out0;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out0) == 0);
	std::vector<u_int8_t> prevOut = out0;
	std::vector<u_int8_t> prevBuilder(b.GetPacket(),b.GetPacket() + b.GetPacketLength());
	u_int16_t seqAfter0 = b.GetSequenceNumber();
	u_int32_t tsAfter0 = b.GetTimestamp();

	// slot 1 has three contributors and does not fit; the build must fail
	// without disturbing the previous packet anywhere.
	TASK_CHECK(feedValue(s,SSRC_A,3,1,0x42) == 0);
	TASK_CHECK(feedValue(s,SSRC_B,4,1,0x43) == 0);
	TASK_CHECK(feedValue(s,SSRC_C,5,1,0x44) == 0);
	std::vector<u_int8_t> out1;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US + 20000,b,out1) < 0);
	TASK_CHECK(out1.empty());							// fresh vector untouched
	TASK_CHECK(out0 == prevOut);						// previous output kept
	TASK_CHECK(b.GetPacketLength() == prevBuilder.size());
	TASK_CHECK(std::memcmp(b.GetPacket(),&prevBuilder[0],prevBuilder.size()) == 0);
	TASK_CHECK(b.GetPacketCount() == 1);
	TASK_CHECK(b.GetSequenceNumber() == seqAfter0);
	TASK_CHECK(b.GetTimestamp() == tsAfter0);

	TASK_CHECK(b.SetMaximumPacketSize(512) == 0);		// enlarge, retry same slot
	TASK_CHECK(s.BuildPCMUMixPacket(START_US + 20000,b,out1) == 0);
	RTPPacket *q = parse(out1);
	TASK_CHECK(q != 0);
	TASK_CHECK(q->GetCSRCCount() == 3);					// all slot-1 samples survived
	TASK_CHECK(q->GetSequenceNumber() == seqAfter0);
	TASK_CHECK(q->GetTimestamp() == tsAfter0);
	delete q;
	TASK_CHECK(b.GetPacketCount() == 2);
	return 0;
}

static int wire_persistent_csrc_restored(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(b.AddCSRC(9) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,1,0,0x42) == 0);

	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 1);
	TASK_CHECK(p->GetCSRC(0) == SSRC_A);			// 9 is not part of the mix
	delete p;

	u_int8_t silence[FRAME];
	std::memset(silence,0xff,FRAME);
	TASK_CHECK(b.BuildPacket(silence,FRAME,0,false,160) == 0);
	std::vector<u_int8_t> plainwire(b.GetPacket(),b.GetPacket() + b.GetPacketLength());
	RTPPacket *plain = parse(plainwire);
	TASK_CHECK(plain != 0);
	TASK_CHECK(plain->GetCSRCCount() == 1);
	TASK_CHECK(plain->GetCSRC(0) == 9);				// persistent list survived
	delete plain;

	// ephemeral list rejects duplicates and overlong lists
	std::vector<u_int32_t> dup(2,7);
	TASK_CHECK(b.BuildPacketWithCSRC(silence,FRAME,0,false,160,dup) < 0);
	std::vector<u_int32_t> tooMany(16,1);
	TASK_CHECK(b.BuildPacketWithCSRC(silence,FRAME,0,false,160,tooMany) < 0);
	return 0;
}

static int wire_real_sr_counts(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,8) == 0);

	const int N = 5;
	for (int slot = 0; slot < N; slot++)
	{
		TASK_CHECK(feedValue(s,SSRC_A,(u_int16_t)slot,(size_t)slot,0x42) == 0);
		std::vector<u_int8_t> out;
		TASK_CHECK(s.BuildPCMUMixPacket(START_US + (uint64_t)slot * 20000,b,out) == 0);
		TASK_CHECK(b.GetPacketCount() == (u_int32_t)(slot + 1));
	}
	TASK_CHECK(b.GetPayloadOctetCount() == (u_int32_t)(N * FRAME));

	RTCPPacketBuilder reports(s,b);
	TASK_CHECK(reports.Init(1024,1.0/8000.0,"mix",3) == 0);
	RTCPCompoundPacket *compound = 0;
	TASK_CHECK(reports.BuildNextPacket(&compound) == 0 && compound != 0);
	compound->GotoFirstPacket();
	RTCPPacket *rp = compound->GetNextPacket();
	TASK_CHECK(rp != 0 && rp->GetPacketType() == RTCPPacket::SR);
	RTCPSRPacket *sr = static_cast<RTCPSRPacket *>(rp);
	TASK_CHECK(sr->GetSenderPacketCount() == (u_int32_t)N);
	TASK_CHECK(sr->GetSenderOctetCount() == (u_int32_t)(N * FRAME));
	delete compound;
	return 0;
}

// --------------------------- lifecycle group ------------------------------

static int lifecycle_configure_rules(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);

	std::vector<RTPPCMUMixInput> empty;
	TASK_CHECK(s.ConfigurePCMUMix(empty,START_US,4) < 0);

	std::vector<RTPPCMUMixInput> dup(2);
	dup[0].ssrc = SSRC_A; dup[0].timestamp = BASE_TS;
	dup[1].ssrc = SSRC_A; dup[1].timestamp = BASE_TS;
	TASK_CHECK(s.ConfigurePCMUMix(dup,START_US,4) < 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,0) < 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,257) < 0);

	std::vector<RTPPCMUMixInput> sixteen(16);
	for (size_t i = 0; i < 16; i++) { sixteen[i].ssrc = (u_int32_t)(i + 1); sixteen[i].timestamp = BASE_TS; }
	TASK_CHECK(s.ConfigurePCMUMix(sixteen,START_US,4) < 0);

	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) < 0);	// already configured

	// boundary window sizes are accepted on a fresh source table
	RTPSources w1;
	TASK_CHECK(w1.ConfigurePCMUMix(twoInputs(),START_US,1) == 0);
	w1.Clear();
	TASK_CHECK(w1.ConfigurePCMUMix(twoInputs(),START_US,256) == 0);

	// building needs an own SSRC that matches the builder's SSRC
	RTPSources noOwn;
	RTPPacketBuilder other;
	TASK_CHECK(other.Init(512) == 0);
	std::vector<u_int8_t> ignored;
	TASK_CHECK(noOwn.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(noOwn.BuildPCMUMixPacket(START_US,other,ignored) < 0);	// no own source
	noOwn.CreateOwnSSRC(other.GetSSRC());
	RTPPacketBuilder mismatched;
	TASK_CHECK(mismatched.Init(512) == 0);
	TASK_CHECK(noOwn.BuildPCMUMixPacket(START_US,mismatched,ignored) < 0); // SSRC mismatch

	// an SSRC already present in the table can never join a new mix
	RTPSources s2;
	TASK_CHECK(feedValue(s2,SSRC_A,1,0,0x42) == 0);
	TASK_CHECK(s2.ConfigurePCMUMix(twoInputs(),START_US,4) < 0);
	return 0;
}

static int lifecycle_timeout_revokes_membership(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,1,0,0x42) == 0);

	s.Timeout(RTPTime(1000,0),RTPTime(1,0));		// removes idle SSRC_A
	TASK_CHECK(!s.GotEntry(SSRC_A));

	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 0);				// contribution revoked
	TASK_CHECK(p->GetPayloadData()[0] == 0xff);
	delete p;

	// same SSRC reappears: treated as an ordinary, non-mixer source
	TASK_CHECK(feedValue(s,SSRC_A,2,0,0x43) == 0);
	TASK_CHECK(s.GotEntry(SSRC_A));
	TASK_CHECK(s.GotoFirstSourceWithData());
	RTPPacket *queued = s.GetNextPacket();
	TASK_CHECK(queued != 0);
	TASK_CHECK(queued->GetSequenceNumber() == 2);
	delete queued;
	return 0;
}

static int lifecycle_bye_kept_until_bye_timeout(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,1,0,0x42) == 0);

	// RTCP from the same host as the RTP source so the address checks accept it
	RTPIPv4Address byeAddr(0x7f000001,4001);
	TASK_CHECK(s.ProcessBYE(SSRC_A,0,0,RTPTime(100,0),&byeAddr) == 0);
	// BYE alone does not remove the source or the contribution
	s.BYETimeout(RTPTime(50,0),RTPTime(1,0));
	TASK_CHECK(s.GotEntry(SSRC_A));
	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 1);
	delete p;

	// actual BYE timeout removal revokes membership
	s.BYETimeout(RTPTime(2000,0),RTPTime(1,0));
	TASK_CHECK(!s.GotEntry(SSRC_A));
	std::vector<u_int8_t> out2;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US + 20000,b,out2) == 0);
	p = parse(out2);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetCSRCCount() == 0);
	delete p;
	return 0;
}

static int lifecycle_clear_and_reconfigure(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(feedValue(s,SSRC_A,1,0,0x42) == 0);

	s.Clear();
	std::vector<u_int8_t> out;
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) < 0);	// mix disabled
	TASK_CHECK(!s.GotEntry(SSRC_A));

	// fresh configuration starts at slot 0 again, old state fully gone
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);
	TASK_CHECK(s.BuildPCMUMixPacket(START_US,b,out) == 0);
	RTPPacket *p = parse(out);
	TASK_CHECK(p != 0);
	TASK_CHECK(p->HasMarker());
	TASK_CHECK(p->GetCSRCCount() == 0);
	delete p;
	return 0;
}

static int lifecycle_legacy_queue_unchanged(void)
{
	RTPSources s;
	RTPPacketBuilder b;
	TASK_CHECK(b.Init(512) == 0);
	TASK_CHECK(s.CreateOwnSSRC(b.GetSSRC()) == 0);
	TASK_CHECK(s.ConfigurePCMUMix(twoInputs(),START_US,4) == 0);

	// configured member: statistics only, never enters the legacy queue
	TASK_CHECK(feedValue(s,SSRC_A,1,0,0x42) == 0);
	TASK_CHECK(!s.GotoFirstSourceWithData());
	RTPSourceData *info = s.GetSourceInfo(SSRC_A);
	TASK_CHECK(info != 0 && info->INF_GetNumPacketsReceived() == 1);

	// unrelated source keeps the classic queue ownership path
	TASK_CHECK(feedValue(s,999,7,0,0x55) == 0);
	TASK_CHECK(s.GotoFirstSourceWithData());
	RTPPacket *p = s.GetNextPacket();
	TASK_CHECK(p != 0);
	TASK_CHECK(p->GetSSRC() == 999 && p->GetSequenceNumber() == 7);
	delete p;
	TASK_CHECK(s.GetNextPacket() == 0);
	return 0;
}

extern const struct task_case task_cases[] =
{
	{ "slots",     "not_due_and_due",        slots_not_due_and_due        },
	{ "slots",     "window_boundaries",      slots_window_boundaries      },
	{ "slots",     "disordered_delivery",    slots_disordered_delivery    },
	{ "slots",     "first_packet_wins",      slots_first_packet_wins      },
	{ "slots",     "invalid_excluded",       slots_invalid_packets_excluded },
	{ "slots",     "absent_is_silence",      slots_absent_source_is_silence },
	{ "slots",     "silence_still_csrc",     slots_silence_provider_still_csrc },
	{ "wire",      "csrc_sorted_summed",     wire_csrc_sorted_and_summed  },
	{ "wire",      "cancel_saturate",        wire_cancel_and_saturate     },
	{ "wire",      "dual_wraparound",        wire_dual_wraparound         },
	{ "wire",      "small_mtu_retry",        wire_small_mtu_retry_same_slot },
	{ "wire",      "failure_keeps_packet",   wire_later_failure_keeps_previous_packet },
	{ "wire",      "persistent_csrc",        wire_persistent_csrc_restored },
	{ "wire",      "real_sr_counts",         wire_real_sr_counts          },
	{ "lifecycle", "configure_rules",        lifecycle_configure_rules    },
	{ "lifecycle", "timeout_revokes",        lifecycle_timeout_revokes_membership },
	{ "lifecycle", "bye_timeout",            lifecycle_bye_kept_until_bye_timeout },
	{ "lifecycle", "clear_reconfigure",      lifecycle_clear_and_reconfigure },
	{ "lifecycle", "legacy_queue",           lifecycle_legacy_queue_unchanged },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
