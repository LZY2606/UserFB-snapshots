/*
 * PCMU conference mixing example.
 *
 * Two remote sources are synthesized entirely in memory, fed through the
 * normal RTPSources reception path (RTPPacket -> RTPRawPacket ->
 * ProcessRawPacket) and mixed with RTPSources::BuildPCMUMixPacket. The
 * resulting wire packet is parsed again with the production RTPPacket class
 * and its CSRC list and actual samples are printed. No threads, sockets or
 * sleeping are involved.
 */

#include "rtppcmumixer.h"
#include "rtpsources.h"
#include "rtppacketbuilder.h"
#include "rtppacket.h"
#include "rtprawpacket.h"
#include "rtpipv4address.h"
#include "rtptimeutilities.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

const u_int32_t SSRC_ONE = 111;
const u_int32_t SSRC_TWO = 222;
const u_int32_t BASE_TS  = 1600;
const uint64_t START_US  = 0;
const size_t FRAME       = 160;

RTPRawPacket *makePacket(u_int32_t ssrc,u_int16_t seq,u_int32_t ts,u_int8_t code)
{
	u_int8_t payload[FRAME];
	std::memset(payload,code,FRAME);
	RTPPacket p(0,payload,FRAME,seq,ts,ssrc,false,0,0,false,0,0,0);
	if (p.GetCreationError() < 0)
		return 0;
	u_int8_t *data = new u_int8_t[p.GetPacketLength()];
	std::memcpy(data,p.GetPacketData(),p.GetPacketLength());
	RTPTime recvtime(1,0);
	return new RTPRawPacket(data,p.GetPacketLength(),
	                        new RTPIPv4Address(0x7f000001,4000),recvtime,true);
}

void printSlot(const std::vector<u_int8_t> &wire,const char *label,int nshow)
{
	u_int8_t *copy = new u_int8_t[wire.size()];
	std::memcpy(copy,&wire[0],wire.size());
	RTPTime t(1,0);
	RTPRawPacket raw(copy,wire.size(),0,t,true);
	RTPPacket packet(raw);
	if (packet.GetCreationError() < 0)
	{
		std::printf("%s: failed to parse output packet\n",label);
		delete[] copy;
		return;
	}

	std::printf("%s: wire=%zu marker=%d pt=%u seq=%u ts=%u payload=%zu csrc=%d [",
	            label,wire.size(),packet.HasMarker() ? 1 : 0,
	            (unsigned)packet.GetPayloadType(),
	            (unsigned)packet.GetSequenceNumber(),packet.GetTimestamp(),
	            packet.GetPayloadLength(),packet.GetCSRCCount());
	for (int i = 0 ; i < packet.GetCSRCCount() ; i++)
		std::printf("%s%u",i ? "," : "",packet.GetCSRC(i));
	std::printf("] first samples (PCMU -> linear):");
	for (int i = 0 ; i < nshow ; i++)
	{
		u_int8_t code = packet.GetPayloadData()[i];
		std::printf(" 0x%02x->%d",(unsigned)code,
		            (int)RTPPCMUMixer::PCMUDecode(code));
	}
	std::printf("\n");
}

} // namespace

int main()
{
	RTPSources sources;
	RTPPacketBuilder builder;
	if (builder.Init(512) < 0 || sources.CreateOwnSSRC(builder.GetSSRC()) < 0)
	{
		std::fprintf(stderr,"pcmu_mix: initialization failed\n");
		return 1;
	}

	std::vector<RTPPCMUMixInput> inputs(2);
	inputs[0].ssrc = SSRC_ONE;
	inputs[0].timestamp = BASE_TS;
	inputs[1].ssrc = SSRC_TWO;
	inputs[1].timestamp = BASE_TS;
	if (sources.ConfigurePCMUMix(inputs,START_US,4) < 0)
	{
		std::fprintf(stderr,"pcmu_mix: ConfigurePCMUMix failed\n");
		return 1;
	}

	const u_int8_t CODE_ONE = 0x42;		// loud tone from source one
	const u_int8_t CODE_TWO = 0x72;		// softer tone from source two

	// Slot 0: both sources contribute.
	RTPRawPacket *r1 = makePacket(SSRC_ONE,10,BASE_TS,CODE_ONE);
	RTPRawPacket *r2 = makePacket(SSRC_TWO,20,BASE_TS,CODE_TWO);
	if (sources.ProcessRawPacket(r1,(RTPTransmitter**)0,0,false) < 0 ||
	    sources.ProcessRawPacket(r2,(RTPTransmitter**)0,0,false) < 0)
	{
		std::fprintf(stderr,"pcmu_mix: slot 0 reception failed\n");
		delete r1; delete r2;
		return 1;
	}
	delete r1;
	delete r2;

	// Slot 1: only source one contributes; source two is absent (silence).
	RTPRawPacket *r3 = makePacket(SSRC_ONE,11,BASE_TS + FRAME,CODE_ONE);
	if (sources.ProcessRawPacket(r3,(RTPTransmitter**)0,0,false) < 0)
	{
		std::fprintf(stderr,"pcmu_mix: slot 1 reception failed\n");
		delete r3;
		return 1;
	}
	delete r3;

	int32_t both = (int32_t)RTPPCMUMixer::PCMUDecode(CODE_ONE)
	             + (int32_t)RTPPCMUMixer::PCMUDecode(CODE_TWO);
	std::printf("input source %u: PCMU 0x%02x -> linear %d\n",
	            (unsigned)SSRC_ONE,(unsigned)CODE_ONE,
	            (int)RTPPCMUMixer::PCMUDecode(CODE_ONE));
	std::printf("input source %u: PCMU 0x%02x -> linear %d (slot 0 only)\n",
	            (unsigned)SSRC_TWO,(unsigned)CODE_TWO,
	            (int)RTPPCMUMixer::PCMUDecode(CODE_TWO));
	std::printf("independent slot-0 sum before saturation: %d\n",(int)both);

	std::vector<u_int8_t> packet;
	if (sources.BuildPCMUMixPacket(START_US,builder,packet) != 0)
	{
		std::fprintf(stderr,"pcmu_mix: slot 0 build failed\n");
		return 1;
	}
	printSlot(packet,"slot 0",4);

	packet.clear();
	if (sources.BuildPCMUMixPacket(START_US + 20000,builder,packet) != 0)
	{
		std::fprintf(stderr,"pcmu_mix: slot 1 build failed\n");
		return 1;
	}
	printSlot(packet,"slot 1",4);

	std::printf("builder packets=%u payload octets=%u\n",
	            builder.GetPacketCount(),builder.GetPayloadOctetCount());
	return 0;
}
