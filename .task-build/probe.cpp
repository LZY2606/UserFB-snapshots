#include "rtpudpv4transmitter.h"
#include <cstdio>
int main(){RTPUDPv4Transmitter t;printf("init=%d\n",t.Init(false));RTPUDPv4TransmissionParams p;p.SetBindIP(0x7f000001);std::list<u_int32_t> ips;ips.push_back(0x7f000001);p.SetLocalIPList(ips);p.SetPortbase(31000);printf("create=%d\n",t.Create(1500,&p));t.Destroy();}
