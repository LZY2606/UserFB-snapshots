#include "rtpsources.h"
#include "rtppacket.h"
#include "rtprawpacket.h"
#include "rtpipv4address.h"
#include "rtppacketbuilder.h"
#include "rtcppacketbuilder.h"
#include "rtcpcompoundpacket.h"
#include "rtcpsrpacket.h"
#include <cstdio>
#include <cstring>
#define REQUIRE(x) do {if (!(x)){std::fprintf(stderr,"BASELINE line %d\n",__LINE__);return 1;}} while(0)
static int feed(RTPSources& s,unsigned seq,unsigned ts,unsigned char value){
    unsigned char audio[160];std::memset(audio,value,sizeof audio);
    RTPPacket p(0,audio,160,seq,ts,7,false,0,0,false,0,0,0);
    if(p.GetCreationError()<0)return -1;
    unsigned char *data=new unsigned char[p.GetPacketLength()];
    std::memcpy(data,p.GetPacketData(),p.GetPacketLength());RTPTime t(100,seq);
    RTPRawPacket raw(data,p.GetPacketLength(),new RTPIPv4Address(0x7f000001,4000),t,true);
    return s.ProcessRawPacket(&raw,(RTPTransmitter**)0,0,false);
}
int main(){
    RTPSources s;REQUIRE(feed(s,10,1000,0xff)==0);REQUIRE(feed(s,12,1320,0x80)==0);REQUIRE(feed(s,11,1160,0x00)==0);
    REQUIRE(s.GotoFirstSourceWithData());unsigned expect[]={10,11,12};
    for(int i=0;i<3;i++){RTPPacket *p=s.GetNextPacket();REQUIRE(p&&p->GetSequenceNumber()==expect[i]&&p->GetPayloadLength()==160);delete p;}
    REQUIRE(!s.GetNextPacket());
    RTPPacketBuilder b;REQUIRE(b.Init(512)==0);REQUIRE(s.CreateOwnSSRC(b.GetSSRC())==0);REQUIRE(b.AddCSRC(7)==0);
    unsigned char audio[160];std::memset(audio,0xff,160);auto seq=b.GetSequenceNumber();auto ts=b.GetTimestamp();
    REQUIRE(b.BuildPacket(audio,160,0,false,160)==0);REQUIRE(b.GetSequenceNumber()==(u_int16_t)(seq+1)&&b.GetTimestamp()==ts+160);s.SentRTPPacket();
    RTCPPacketBuilder reports(s,b);REQUIRE(reports.Init(1024,1.0/8000,"baseline",8)==0);RTCPCompoundPacket *compound=0;
    REQUIRE(reports.BuildNextPacket(&compound)==0&&compound);compound->GotoFirstPacket();RTCPPacket *packet=compound->GetNextPacket();
    REQUIRE(packet&&packet->GetPacketType()==RTCPPacket::SR);auto sr=static_cast<RTCPSRPacket*>(packet);
    REQUIRE(sr->GetSenderPacketCount()==1&&sr->GetSenderOctetCount()==160);delete compound;
    std::puts("BASELINE receive_order=10,11,12 payload=160");
    std::puts("BASELINE sender_packets=1 sender_octets=160 sequence_delta=1 timestamp_delta=160");return 0;
}
