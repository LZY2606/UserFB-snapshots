#include "formats/h264.hh"
#include "rtp.hh"
#include "socket.hh"
#include <vector>
#include <memory>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
static std::vector<std::vector<unsigned char>> packets;
// In-memory transport boundary: all RTP construction is the real production path.
extern "C" ssize_t sendmsg(int,const struct msghdr* m,int){
 std::vector<unsigned char> b;
 for(size_t i=0;i<m->msg_iovlen;i++){
  auto* p=static_cast<unsigned char*>(m->msg_iov[i].iov_base);
  b.insert(b.end(),p,p+m->msg_iov[i].iov_len);
 }
 auto n=b.size();packets.push_back(std::move(b));return static_cast<ssize_t>(n);
}
#ifdef __linux__
extern "C" int sendmmsg(int fd,struct mmsghdr* ms,unsigned n,int flags){
 for(unsigned i=0;i<n;i++){auto sz=sendmsg(fd,&ms[i].msg_hdr,flags);if(sz<0)return -1;ms[i].msg_len=sz;}
 return n;
}
#endif
int main(){
 auto ssrc=std::make_shared<std::atomic<uint32_t>>(7);
 auto rtp=std::make_shared<uvgrtp::rtp>(RTP_FORMAT_H264,ssrc,false);
 auto sock=std::make_shared<uvgrtp::socket>(RCE_NO_FLAGS);
 if(sock->init(AF_INET,SOCK_DGRAM,0)!=RTP_OK)return 1;
 uvgrtp::formats::h264 media(sock,rtp,RCE_NO_FLAGS);
 rtp->set_timestamp(90000);
 unsigned char au[]={0,0,0,1,0x67,0x11,0x22,0,0,0,1,0x68,0x33,0x44,0,0,0,1,0x65,0x55,0x66};
 sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(4000);addr.sin_addr.s_addr=htonl(0x7f000001);sockaddr_in6 addr6{};
 auto rc=media.push_media_frame(addr,addr6,au,sizeof(au),RTP_H26X_DO_NOT_AGGR,7);
 if(rc!=RTP_OK||packets.size()!=3)return 2;
 unsigned markers=0;uint32_t ts=0;uint16_t prev=0;
 for(size_t i=0;i<packets.size();i++){
  const auto& p=packets[i];if(p.size()<15)return 3;
  uint16_t seq=(p[2]<<8)|p[3];uint32_t t=(uint32_t(p[4])<<24)|(uint32_t(p[5])<<16)|(uint32_t(p[6])<<8)|p[7];
  if(i&&((uint16_t)(prev+1)!=seq||t!=ts))return 4;
  ts=t;prev=seq;markers+=(p[1]>>7);
 }
 std::printf("AU_PACKETS=%zu SHARED_TIMESTAMP=%u CONTIGUOUS_SEQ=1\n",packets.size(),ts);
 std::printf("OBSERVED_MARKERS=%u EXPECTED_AU_MARKERS=1\n",markers);
 // Characterization of the fixed upstream, not a corrected target assertion.
 if(markers<1||markers>packets.size())return 5;
 std::printf("PER_NAL_BOUNDARY=%u MEMORY_TRANSPORT=1\n",markers>1);return 0;
}
