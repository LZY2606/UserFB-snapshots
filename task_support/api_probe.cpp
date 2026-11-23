/* Fixed public API consumer, intentionally not a mixer implementation. */
#include "rtppcmumixer.h"
#include "rtpsources.h"
#include "rtppacketbuilder.h"
#include "rtppacket.h"
#include "rtprawpacket.h"
#include "rtpipv4address.h"
#include <vector>
#include <cstdio>
#include <cstring>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"API_PROBE line %d\n",__LINE__);return 1;}}while(0)
int main(){
    RTPSources s;RTPPacketBuilder b;REQUIRE(b.Init(512)==0);REQUIRE(s.CreateOwnSSRC(b.GetSSRC())==0);
    std::vector<RTPPCMUMixInput> inputs(1);inputs[0].ssrc=7;inputs[0].timestamp=1000;
    REQUIRE(s.ConfigurePCMUMix(inputs,20000,4)==0);
    unsigned char silence[160];std::memset(silence,0xff,160);
    RTPPacket in(0,silence,160,10,1000,7,false,0,0,false,0,0,0);REQUIRE(in.GetCreationError()==0);
    unsigned char *data=new unsigned char[in.GetPacketLength()];std::memcpy(data,in.GetPacketData(),in.GetPacketLength());RTPTime t(1,0);
    RTPRawPacket raw(data,in.GetPacketLength(),new RTPIPv4Address(0x7f000001,4000),t,true);
    REQUIRE(s.ProcessRawPacket(&raw,(RTPTransmitter**)0,0,false)==0);
    std::vector<u_int8_t> output;auto seq=b.GetSequenceNumber();auto ts=b.GetTimestamp();
    REQUIRE(s.BuildPCMUMixPacket(20000,b,output)==0);REQUIRE(output.size()==176);
    unsigned char *copy=new unsigned char[output.size()];std::memcpy(copy,&output[0],output.size());
    RTPRawPacket parsedraw(copy,output.size(),0,t,true);RTPPacket packet(parsedraw);REQUIRE(packet.GetCreationError()==0);
    REQUIRE(packet.GetCSRCCount()==1&&packet.GetCSRC(0)==7&&packet.GetPayloadLength()==160);
    REQUIRE(packet.GetSequenceNumber()==seq&&packet.GetTimestamp()==ts&&packet.GetPayloadData()[0]==0xff);
    REQUIRE(b.GetPacketCount()==1&&b.GetPayloadOctetCount()==160&&b.GetTimestamp()==ts+160);
    std::vector<u_int32_t> csrcs(1,9);
    REQUIRE(b.BuildPacketWithCSRC(silence,160,0,false,160,csrcs)==0);
    REQUIRE((b.GetPacket()[0]&15)==1&&b.GetPacket()[15]==9&&b.GetPacketCount()==2);
    std::puts("API_PROBE_COMPLETE");return 0;
}
