#include "H264VideoRTPSource.hh"
#include "BasicUsageEnvironment.hh"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
struct Capture {
  EventLoopWatchVariable done{0};
  bool ok=false;
  unsigned char bytes[128]{};
  H264VideoRTPSource* source=nullptr;
};
static void timeout(void* p) { static_cast<Capture*>(p)->done=1; }
static void frame(void* p, unsigned n, unsigned truncated, timeval, unsigned) {
  Capture& c=*static_cast<Capture*>(p);
  const unsigned char expected[]={0x65,0xaa,0xbb};
  c.ok=n==3 && truncated==0 && std::memcmp(c.bytes,expected,3)==0
    && c.source->curPacketRTPSeqNum()==77;
  std::printf("single_ssrc=%08x nal=",c.source->lastReceivedSSRC());
  for(unsigned i=0;i<n;++i) std::printf("%02x",c.bytes[i]);
  std::printf(" seq=%u\n",c.source->curPacketRTPSeqNum());
  c.done=1;
}
int main() {
  auto* scheduler=BasicTaskScheduler::createNew();
  auto* env=BasicUsageEnvironment::createNew(*scheduler);
  sockaddr_storage address{};
  auto& ipv4=reinterpret_cast<sockaddr_in&>(address);
  ipv4.sin_family=AF_INET; ipv4.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  int result=1;
  {
    Groupsock socket(*env,address,Port(0),1);
    if(::bind(socket.socketNum(),reinterpret_cast<sockaddr*>(&ipv4),sizeof ipv4)) return 5;
    sockaddr_in destination{}; socklen_t length=sizeof destination;
    if(getsockname(socket.socketNum(),reinterpret_cast<sockaddr*>(&destination),&length)) return 2;
    destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    Capture capture;capture.source=H264VideoRTPSource::createNew(*env,&socket,96);
    capture.source->getNextFrame(capture.bytes,sizeof capture.bytes,frame,&capture,nullptr,nullptr);
    auto guard=scheduler->scheduleDelayedTask(1000000,timeout,&capture);
    int sender=::socket(AF_INET,SOCK_DGRAM,0);
    const unsigned char packet[]={0x80,0xe0,0,77,0,1,0x5f,0x90,0x11,0x22,0x33,0x44,0x65,0xaa,0xbb};
    if(sendto(sender,packet,sizeof packet,0,reinterpret_cast<sockaddr*>(&destination),sizeof destination)!=sizeof packet) return 3;
    scheduler->doEventLoop(&capture.done);
    scheduler->unscheduleDelayedTask(guard);
    close(sender);result=capture.ok?0:4;
    Medium::close(capture.source);
  }
  env->reclaim();delete scheduler;
  return result;
}
