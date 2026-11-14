#include "liveMedia.hh"
#include "GroupsockHelper.hh"
#include "BasicUsageEnvironment.hh"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <vector>
class Subsession: public OnDemandServerMediaSubsession {
public:
  unsigned creates=0;
  explicit Subsession(UsageEnvironment& e):OnDemandServerMediaSubsession(e,False){}
  void setup(unsigned id,int fd,unsigned char ch,void*& token) {
    sockaddr_storage addr{},dest{}; auto& a=(sockaddr_in&)addr;
    a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    u_int8_t ttl=1;Boolean multicast=False;Port rp(0),cp(0);
    getStreamParameters(id,addr,Port(0),Port(1),fd,ch,ch+1,nullptr,dest,ttl,multicast,rp,cp,token);
  }
  void play(unsigned id,void* token) {
    unsigned short seq=0;unsigned ts=0;
    startStream(id,token,nullptr,nullptr,seq,ts,nullptr,nullptr);
  }
  void erase(unsigned id,void*& token){deleteStream(id,token);}
protected:
  FramedSource* createNewStreamSource(unsigned,unsigned& bitrate) override {
    static unsigned char audio[]={0xa1,0xb2,0xc3,0xd4};
    bitrate=64;++creates;
    return ByteStreamMemoryBufferSource::createNew(envir(),audio,sizeof audio,False);
  }
  RTPSink* createNewRTPSink(Groupsock* gs,unsigned char,FramedSource*) override {
    return SimpleRTPSink::createNew(envir(),gs,8,8000,"audio","PCMA",1,False);
  }
};
struct Capture {int fd;unsigned char channel;EventLoopWatchVariable done{0};bool ok=false;std::vector<unsigned char> bytes;};
static void readable(void* data,int) {
  auto& c=*(Capture*)data;unsigned char temp[4096];int n=recv(c.fd,temp,sizeof temp,0);
  if(n<=0){c.done=1;return;}c.bytes.insert(c.bytes.end(),temp,temp+n);
  size_t p=0;
  while(c.bytes.size()-p>=4){
    if(c.bytes[p]!='$'){c.done=1;return;}
    unsigned len=(c.bytes[p+2]<<8)|c.bytes[p+3];if(c.bytes.size()-p<4+len)break;
    if(c.bytes[p+1]==c.channel){
      static const unsigned char want[]={0xa1,0xb2,0xc3,0xd4};
      c.ok=len==16 && !memcmp(&c.bytes[p+4+12],want,4);c.done=1;
    }
    p+=4+len;
  }
  if(p)c.bytes.erase(c.bytes.begin(),c.bytes.begin()+p);
}
static void timeout(void* data){((Capture*)data)->done=1;}
static bool run(Subsession& sub,TaskScheduler& sched,unsigned char ch) {
  int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))return false;
  makeSocketNonBlocking(pair[0]);makeSocketNonBlocking(pair[1]);
  Capture cap{pair[1],ch};void* token=nullptr;sub.setup(9,pair[0],ch,token);
  sched.setBackgroundHandling(pair[1],SOCKET_READABLE,readable,&cap);
  auto guard=sched.scheduleDelayedTask(1000000,timeout,&cap);sub.play(9,token);sched.doEventLoop(&cap.done);
  sched.unscheduleDelayedTask(guard);sched.disableBackgroundHandling(pair[1]);sub.erase(9,token);
  close(pair[0]);close(pair[1]);return cap.ok;
}
int main(){
  auto* sched=BasicTaskScheduler::createNew();auto* env=BasicUsageEnvironment::createNew(*sched);
  auto* sub=new Subsession(*env);auto* sms=ServerMediaSession::createNew(*env,"baseline");sms->addSubsession(sub);
  bool a=run(*sub,*sched,0),b=run(*sub,*sched,2);
  printf("BASELINE interleaved_payload=%s source_creations=%u repeated_setup_preserves_source=no\n",a&&b?"a1b2c3d4":"FAIL",sub->creates);
  int result=a&&b&&sub->creates==2?0:1;Medium::close(sms);env->reclaim();delete sched;return result;
}
