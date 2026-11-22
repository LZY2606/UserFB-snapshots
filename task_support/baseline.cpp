#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
class Owner : public Medium { public: Owner(UsageEnvironment& e):Medium(e){} ~Owner(){} };
int main() {
  TaskScheduler* scheduler=BasicTaskScheduler::createNew();
  UsageEnvironment* env=BasicUsageEnvironment::createNew(*scheduler);
  int pair[2]; if(socketpair(AF_UNIX,SOCK_STREAM,0,pair)!=0)return 1;
  {
    sockaddr_storage address{};address.ss_family=AF_INET;
    Groupsock gs(*env,address,Port(0),1);gs.removeAllDestinations();
    Owner owner(*env);RTPInterface output(&owner,&gs);
    output.addStreamSocket(pair[0],2,nullptr);
    unsigned char rtp[]={0x80,0x60,0xff,0xff,0xff,0xff,0xff,0xf0,0,0,0,1,0x61,0x11};
    if(!output.sendPacket(rtp,sizeof rtp))return 2;
    output.addStreamSocket(pair[0],3,nullptr);
    output.removeStreamSocket(pair[0],2);
    unsigned char rtcp[]={0x80,201,0,1,0,0,0,1};
    if(!output.sendPacket(rtcp,sizeof rtcp))return 3;
    unsigned char got[30]{};size_t n=0;
    while(n<sizeof got){ssize_t r=recv(pair[1],got+n,sizeof(got)-n,MSG_DONTWAIT);if(r<=0)return 4;n+=(size_t)r;}
    if(got[0]!='$'||got[1]!=2||got[3]!=sizeof rtp||memcmp(got+4,rtp,sizeof rtp)||got[18]!='$'||got[19]!=3||got[21]!=sizeof rtcp||memcmp(got+22,rtcp,sizeof rtcp))return 5;
    std::printf("PRODUCTION_TCP bytes=%zu channels=2,3 rtp_seq=65535 rtp_timestamp=4294967280 payload=6111 rtcp_type=201\n",n);
    output.removeStreamSocket(pair[0],3);
  }
  close(pair[0]);close(pair[1]);env->reclaim();delete scheduler;return 0;
}
