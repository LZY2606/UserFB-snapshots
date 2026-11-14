#include "vp9rtpdepacketizer.hpp"
#include "vp9rtppacketizer.hpp"
#include "rtp.hpp"
#include <iostream>
#include <stdexcept>
using namespace rtc;
static message_ptr packet(uint16_t seq,uint32_t ts,bool marker,uint8_t sid,uint8_t data) {
 auto p=make_message(19);auto*h=reinterpret_cast<RtpHeader*>(p->data());h->preparePacket();h->setSeqNumber(seq);h->setTimestamp(ts);h->setSsrc(123);h->setPayloadType(98);h->setMarker(marker);
 // I=1,L=1,F=0,B=1,E=1; 15-bit PictureID=7, TID=0, TL0PICIDX=42.
 const uint8_t b[]={0xac,0x80,7,uint8_t((sid<<1)|(sid?1:0)),42,0x82,data};
 for(size_t i=0;i<7;i++)(*p)[12+i]=byte(b[i]);return p;
}
int main(){
 auto cfg=std::make_shared<RtpPacketizationConfig>(123,"baseline",98,90000);cfg->sequenceNumber=100;
 auto pack=std::make_shared<VP9RtpPacketizer>(cfg,8);auto fi=std::make_shared<FrameInfo>(uint32_t(20000));
 message_vector tx{make_message(binary{byte{0x82},byte{0x11},byte{0x82},byte{0x22}},fi)};pack->outgoingChain(tx,[](message_ptr){});
 if(tx.size()!=1||reinterpret_cast<const RtpHeader*>(tx[0]->data())->timestamp()!=20000||std::to_integer<uint8_t>((*tx[0])[12])!=12)throw std::runtime_error("default packetizer failed");

 auto dep=std::make_shared<VP9RtpDepacketizer>();message_vector in{packet(10,9000,false,0,0x11)};
 dep->incomingChain(in,[](message_ptr){});size_t early=in.size();if(early!=0)throw std::runtime_error("legacy unexpectedly emitted early");
 in={packet(11,9000,true,1,0x22)};dep->incomingChain(in,[](message_ptr){});
 if(in.size()!=1||*in[0]!=binary{byte{0x82},byte{0x11},byte{0x82},byte{0x22}}||!in[0]->frameInfo||in[0]->frameInfo->timestamp!=9000)throw std::runtime_error("legacy VP9 output mismatch");
 auto normal=std::make_shared<VP9RtpDepacketizer>();in={packet(20,18000,true,0,0x33)};normal->incomingChain(in,[](message_ptr){});
 if(in.size()!=1||*in[0]!=binary{byte{0x82},byte{0x33}})throw std::runtime_error("single layer failed");
 std::cout<<"BASELINE_OK default_sender_packets=1 single_layer_bytes=2 selected_E_outputs="<<early<<" marker_outputs=1 spatial_concat_bytes=4 superframe_index=absent timestamp=9000\n";
}
