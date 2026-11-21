#include "rtpsources.h"
#include "rtppacket.h"
#include "rtprawpacket.h"
#include "rtpipv4address.h"
#include <cstdio>
#include <cstring>
static int feed(RTPSources& s, unsigned seq, unsigned ts) {
 unsigned char b[13]={0x80,96,0,0,0,0,0,0,0,0,0,7,0x55};
 b[2]=seq>>8;b[3]=seq;b[4]=ts>>24;b[5]=ts>>16;b[6]=ts>>8;b[7]=ts;
 unsigned char* p=new unsigned char[13];std::memcpy(p,b,13);
 RTPTime now(10,0);RTPRawPacket raw(p,13,new RTPIPv4Address(0x7f000001,4000),now,true);
 return s.ProcessRawPacket(&raw,(RTPTransmitter*)0,false);
}
int main(){
 RTPSources s;
 for(unsigned n=98;n<=101;n++)if(feed(s,n,n*900)<0)return 1;
 if(feed(s,103,92700)<0||feed(s,102,91800)<0)return 2;
 if(s.ProcessRTCPSenderInfo(7,RTPNTPTime(4000000000U,0),90000,6,6,RTPTime(10,0),new RTPIPv4Address(0x7f000001,4001))<0)return 3;
 RTPSourceData* d=s.GetSourceInfo(7);if(!d||!d->SR_HasInfo())return 4;
 unsigned last=0,count=0;bool reorder=false;
 while(RTPPacket* p=d->GetNextPacket()){
  unsigned seq=p->GetSequenceNumber();if(count&&seq<=last)return 5;
  if(seq==102)reorder=true;last=seq;++count;delete p;
 }
 if(!count||!reorder||last!=103)return 6;
 std::printf("SOURCE_ORDER last=%u reordered=%d count=%u\n",last,reorder,count);
 std::printf("SR_RTP=%u SR_NTP=%u immediate_drain=1\n",d->SR_GetRTPTimestamp(),d->SR_GetNTPTimestamp().GetMSW());
 s.Clear();if(s.GetSourceInfo(7))return 7;
 std::puts("CLEAR_RELEASED=1");return 0;
}
