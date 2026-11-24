#include "rtcpcompoundpacket.h"
#include "rtprawpacket.h"
#include "rtpipv4address.h"
#include <cstdio>
#include <cstring>
int main(){
  unsigned char rr[]={0x80,201,0,1,0,0,0,42};
  unsigned char damaged[]={0x80,201,0,0,0x00};
  for(int k=0;k<2;k++){
    size_t len=k?sizeof damaged:sizeof rr;
    unsigned char *d=new unsigned char[len]; memcpy(d,k?damaged:rr,len);
    RTPIPv4Address *a=new RTPIPv4Address(0x7f000001,1234);
    RTPTime t=RTPTime::CurrentTime();
    RTPRawPacket rp(d,len,a,t,false);
    RTCPCompoundPacket c(rp);
    printf("k=%d err=%d\n",k,c.GetCreationError());
  }
}
