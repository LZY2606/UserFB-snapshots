#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct State {FramedSource* src; unsigned char buf[64];int count;bool bad;EventLoopWatchVariable done;timeval times[2];};
static void got(void* p,unsigned n,unsigned trunc,timeval t,unsigned){State& s=*(State*)p;unsigned char want[2][2]={{0xaa,0xbb},{0xcc,0xdd}};if(s.count>=2||n!=2||trunc||memcmp(s.buf,want[s.count],2))s.bad=true;s.times[s.count++]=t;if(s.count==2)s.done=1;else s.src->getNextFrame(s.buf,sizeof s.buf,got,&s,NULL,NULL);}
static void timeout(void* p){State& s=*(State*)p;s.bad=true;s.done=1;}
int main(){TaskScheduler* sched=BasicTaskScheduler::createNew();UsageEnvironment* env=BasicUsageEnvironment::createNew(*sched);
char const* sdp="v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=aac\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 0 RTP/AVP 96\r\na=rtpmap:96 MPEG4-GENERIC/48000/2\r\na=fmtp:96 mode=AAC-hbr;config=1190;sizelength=13;indexlength=3;indexdeltalength=3;constantduration=1024\r\n";
MediaSession* session=MediaSession::createNew(*env,sdp);if(!session)return 2;MediaSubsessionIterator it(*session);MediaSubsession* sub=it.next();if(!sub||!sub->initiate())return 3;
State st{};st.src=sub->readSource();st.src->getNextFrame(st.buf,sizeof st.buf,got,&st,NULL,NULL);TaskToken timer=sched->scheduleDelayedTask(3000000,timeout,&st);
unsigned char packet[]={0x80,0xe0,0,1,0,0,0xbb,0x80,0,0,0,0x7b,0,32,0,16,0,16,0xaa,0xbb,0xcc,0xdd};
int fd=socket(AF_INET,SOCK_DGRAM,0);sockaddr_in dst{};dst.sin_family=AF_INET;dst.sin_port=htons(sub->clientPortNum());inet_pton(AF_INET,"127.0.0.1",&dst.sin_addr);if(sendto(fd,packet,sizeof packet,0,(sockaddr*)&dst,sizeof dst)!=(int)sizeof packet)return 4;
sched->doEventLoop(&st.done);sched->unscheduleDelayedTask(timer);close(fd);long dt=(st.times[1].tv_sec-st.times[0].tv_sec)*1000000L+st.times[1].tv_usec-st.times[0].tv_usec;
printf("BASELINE AAC frames=%d bytes=aabb,ccdd delta_us=%ld\n",st.count,dt);bool ok=!st.bad&&st.count==2&&dt==0;Medium::close(session);env->reclaim();delete sched;if(!ok)return 5;puts("MISSING constantduration=1024 ignored; expected_new_delta_us=21333");return 0;}
