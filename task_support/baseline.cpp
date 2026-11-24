#include "rtpudpv4transmitter.h"
#include "rtpipv4address.h"
#include "rtprawpacket.h"
#include "rtppacketbuilder.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
static int datagram(int fd,unsigned short port,const void *p,size_t n) {
 sockaddr_in a;std::memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(0x7f000001);a.sin_port=htons(port);
 return sendto(fd,p,n,0,(sockaddr*)&a,sizeof a)==(ssize_t)n?0:1;
}
int main() {
 RTPUDPv4Transmitter t;if(t.Init(false)<0)return 1;
 RTPUDPv4TransmissionParams p;p.SetBindIP(0x7f000001);
 std::list<u_int32_t> ips;ips.push_back(0x7f000001);p.SetLocalIPList(ips);
 unsigned short port=0;
 for(unsigned v=30000;v<50000;v+=2){p.SetPortbase((unsigned short)v);if(t.Create(1500,&p)==0){port=(unsigned short)v;break;}}
 if(!port)return 2;
 int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return 3;
 RTPPacketBuilder b;if(b.Init(1500)<0||b.BuildPacket("media",5,96,true,3000)<0)return 4;
 unsigned char rr[]={0x80,201,0,1,0,0,0,42};
 if(datagram(fd,port,b.GetPacket(),b.GetPacketLength())||datagram(fd,port+1,rr,sizeof rr)||datagram(fd,port,rr,sizeof rr))return 5;
 int media=0,control=0,misplaced=0;
 for(int i=0;i<5&&media+control+misplaced<3;i++) {
  if(t.WaitForIncomingData(RTPTime(0,100000))<0||t.Poll()<0)return 6;
  while(RTPRawPacket *r=t.GetNextPacket()) {
   if(r->GetDataLength()==8&&r->GetData()[1]==201) { if(r->IsRTP())++misplaced;else ++control; }
   else if(r->IsRTP()&&r->GetDataLength()==17)++media;
   delete r;
  }
 }
 close(fd);t.Destroy();
 if(media!=1||control!=1||misplaced!=1)return 7;
 std::printf("BASELINE_OK rtp=%d rtcp=%d rtcp_on_rtp_port_tagged_rtp=%d\n",media,control,misplaced);
 return 0;
}
