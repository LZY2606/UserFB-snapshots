#define private public
#include "rtpudpv4transmitter.h"
#undef private
#include "rtpudpv4transmitter.cpp"
#include "rtprawpacket.h"
#include "rtpipv4address.h"
#include <cstdio>
#include <cstring>
static int fails=0;
#define C(x) do{ if(!(x)){ printf("FAIL %d %s\n",__LINE__,#x); ++fails; } }while(0)
static RTPUDPv4Transmitter *make(bool mux){
  RTPUDPv4Transmitter *t=new RTPUDPv4Transmitter();
  t->init=true; t->created=true; t->rtcpmultiplexing=mux; t->receivemode=RTPTransmitter::AcceptAll;
  return t;
}
static RTPRawPacket *feed(RTPUDPv4Transmitter *t,const unsigned char *d,int n,unsigned short sport,RTPTime &now){
  struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(sport); a.sin_addr.s_addr=htonl(0x7f000001);
  int st=t->ProcessIncomingData((const char*)d,n,a,now,true);
  if(st<0) return 0;
  if(t->rawpacketlist.empty()) return 0;
  RTPRawPacket *p=t->rawpacketlist.front(); t->rawpacketlist.pop_front(); return p;
}
int main(){
  RTPTime now=RTPTime::CurrentTime();
  unsigned char rtp[]={0x80,0x60,0,1,0,0,0,1,1,2,3,4};
  unsigned char marker[]={0x80,0xE0,0,1,0,0,0,1,9,9,9,9};
  unsigned char rr[]={0x80,201,0,1,0,0,0,42};
  unsigned char damaged_rtcp[]={0x80,201,0,0,0x00};
  unsigned char short1[]={0x80};
  unsigned char badver_rtcp[]={0x40,201,0,1,0,0,0,1};
  unsigned char badver_rtp[]={0x00,0x60,0,1,0,0,0,1};
  unsigned char type192[]={0x80,192,0,1,0,0,0,1};
  unsigned char type223[]={0x80,223,0,1,0,0,0,1};
  unsigned char type224[]={0x80,224,0,1,0,0,0,1};
  RTPUDPv4Transmitter *t=make(true);
  RTPRawPacket *p;
  p=feed(t,rtp,sizeof rtp,5001,now); C(p&&p->IsRTP()); C(p&&((RTPIPv4Address*)p->GetSenderAddress())->GetPort()==5001); delete p;
  p=feed(t,marker,sizeof marker,5002,now); C(p&&p->IsRTP()); delete p;
  p=feed(t,rr,sizeof rr,5003,now); C(p&&!p->IsRTP()); delete p;
  p=feed(t,damaged_rtcp,sizeof damaged_rtcp,5004,now); C(p&&!p->IsRTP()); delete p; // bad RTCP stays RTCP
  p=feed(t,type192,sizeof type192,5005,now); C(p&&!p->IsRTP()); delete p;
  p=feed(t,type223,sizeof type223,5006,now); C(p&&!p->IsRTP()); delete p;
  p=feed(t,type224,sizeof type224,5007,now); C(p&&p->IsRTP()); delete p;
  C(feed(t,short1,sizeof short1,5008,now)==0);
  C(feed(t,short1,0,5008,now)==0);
  C(feed(t,badver_rtcp,sizeof badver_rtcp,5008,now)==0);
  C(feed(t,badver_rtp,sizeof badver_rtp,5008,now)==0);
  p=feed(t,rtp,sizeof rtp,5009,now); C(p&&p->IsRTP()); delete p; // normal after ignored
  // source filtering
  t->receivemode=RTPTransmitter::IgnoreSome;
  unsigned int ignoredPort=5010;
  t->ProcessAddAcceptIgnoreEntry(0x7f000001,(u_int16_t)ignoredPort);
  C(feed(t,rtp,sizeof rtp,ignoredPort,now)==0);
  p=feed(t,rtp,sizeof rtp,5011,now); C(p&&p->IsRTP()); delete p;
  // non-mux classification stays socket based
  RTPUDPv4Transmitter *u=make(false);
  p=feed(u,rr,sizeof rr,5012,now); C(p&&p->IsRTP()); delete p; // on "RTP" path regardless of byte
  delete t; delete u;
  printf(fails?"FAILED %d\n":"CLASSIFY_OK\n",fails); return fails?1:0;
}
