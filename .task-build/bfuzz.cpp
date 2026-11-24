#include "rtppacketbuilder.h"
#include "rtperrors.h"
#include <cstdio>
#include <cstring>
static int fails=0;
#define C(x) do{ if(!(x)){ printf("FAIL %d %s\n",__LINE__,#x); ++fails; } }while(0)
int main(){
  RTPPacketBuilder b; C(b.Init(1500)==0); C(!b.IsRTCPMultiplexing());
  C(b.SetDefaultPayloadType(64)==0);
  C(b.SetRTCPMultiplexing(true)<0); C(!b.IsRTCPMultiplexing());
  C(b.SetDefaultMark(true)==0); C(b.SetDefaultTimestampIncrement(3)==0);
  C(b.BuildPacket("a",1)==0);  // PT 64 allowed without mux
  u_int16_t s=b.GetSequenceNumber(); u_int32_t t=b.GetTimestamp(); u_int32_t c=b.GetPacketCount();
  size_t l=b.GetPacketLength(); unsigned char copy[1500]; memcpy(copy,b.GetPacket(),l);
  RTPPacketBuilder m; C(m.Init(1500)==0); C(m.SetDefaultPayloadType(96)==0);
  C(m.SetDefaultMark(true)==0); C(m.SetDefaultTimestampIncrement(3)==0);
  C(m.SetRTCPMultiplexing(true)==0); C(m.IsRTCPMultiplexing());
  C(m.SetDefaultPayloadType(63)==0); C(m.SetDefaultPayloadType(96)==0);
  C(m.SetDefaultPayloadType(64)<0); C(m.SetDefaultPayloadType(80)<0); C(m.SetDefaultPayloadType(95)<0);
  C(m.SetDefaultPayloadType(192)==0); C(m.SetDefaultPayloadType(96)==0);
  u_int16_t ms=m.GetSequenceNumber(); u_int32_t mt=m.GetTimestamp(); u_int32_t mc=m.GetPacketCount();
  size_t ml=m.GetPacketLength(); unsigned char mcopy[1500]; memcpy(mcopy,m.GetPacket(),ml);
  unsigned char ext[4]={0,0,0,0};
  C(m.BuildPacket("x",1,64,false,9)<0); C(m.BuildPacket("x",1,95,true,9)<0);
  C(m.BuildPacketEx("x",1,64,true,9,1,ext,1)<0); C(m.BuildPacketEx("x",1,95,false,9,1,ext,1)<0);
  C(m.BuildPacketEx("x",1,1,ext,1)==0); // default PT 96 + ext is fine
  C(m.GetSequenceNumber()==ms+1); C(m.GetTimestamp()==mt+3); C(m.GetPacketCount()==mc+1);
  C(m.BuildPacket("y",1,96,true,9)==0);
  m.Destroy(); C(!m.IsRTCPMultiplexing());
  C(m.Init(1500)==0); C(!m.IsRTCPMultiplexing()); C(m.SetDefaultPayloadType(70)==0); m.Destroy();
  C(b.GetSequenceNumber()==s); C(b.GetTimestamp()==t); C(b.GetPacketCount()==c);
  C(b.GetPacketLength()==l); C(memcmp(copy,b.GetPacket(),l)==0);
  printf(fails?"FAILED %d\n":"BUILDER_OK\n",fails); return fails?1:0;
}
