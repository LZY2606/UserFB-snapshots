#include "srt.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstdio>
#include <cstring>
#include <vector>
static int fail(const char* where) {
  std::fprintf(stderr,"%s: %s\n",where,srt_getlasterror_str()); return 1;
}
static bool configure(SRTSOCKET sock) {
  SRT_TRANSTYPE type=SRTT_FILE;bool message=true;int timeout=2000;
  return srt_setsockflag(sock,SRTO_TRANSTYPE,&type,sizeof type)==0
    && srt_setsockflag(sock,SRTO_MESSAGEAPI,&message,sizeof message)==0
    && srt_setsockflag(sock,SRTO_CONNTIMEO,&timeout,sizeof timeout)==0
    && srt_setsockflag(sock,SRTO_RCVTIMEO,&timeout,sizeof timeout)==0
    && srt_setsockflag(sock,SRTO_SNDTIMEO,&timeout,sizeof timeout)==0;
}
int main() {
  if(srt_startup()!=0)return fail("startup");
  SRTSOCKET listener=srt_create_socket(),sender=srt_create_socket();
  if(listener==SRT_INVALID_SOCK || sender==SRT_INVALID_SOCK || !configure(listener) || !configure(sender))return fail("configure");
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(srt_bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof addr)!=0 || srt_listen(listener,2)!=0)return fail("listen");
  int addrlen=sizeof addr;
  if(srt_getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&addrlen)!=0)return fail("getsockname");
  if(srt_connect(sender,reinterpret_cast<sockaddr*>(&addr),sizeof addr)!=0)return fail("connect");
  SRTSOCKET receiver=srt_accept(listener,nullptr,nullptr);
  if(receiver==SRT_INVALID_SOCK)return fail("accept");
  int timeout=2000;
  if(srt_setsockflag(receiver,SRTO_RCVTIMEO,&timeout,sizeof timeout)!=0)return fail("receive timeout");
  std::vector<char> data(3500),out(4000);
  for(size_t i=0;i<data.size();++i)data[i]=char((i*13+7)&255);
  SRT_MSGCTRL sent=srt_msgctrl_default;sent.inorder=1;
  if(srt_sendmsg2(sender,data.data(),int(data.size()),&sent)!=int(data.size()))return fail("sendmsg2");
  SRT_MSGCTRL received=srt_msgctrl_default;
  int n=srt_recvmsg2(receiver,out.data(),int(out.size()),&received);
  bool match=n==int(data.size()) && std::memcmp(data.data(),out.data(),data.size())==0 && sent.msgno==received.msgno;
  std::printf("message_bytes=%d msgno_match=%s payload_match=%s\n",n,sent.msgno==received.msgno?"yes":"no",match?"yes":"no");
  srt_close(receiver);srt_close(sender);srt_close(listener);srt_cleanup();
  return match?0:2;
}
