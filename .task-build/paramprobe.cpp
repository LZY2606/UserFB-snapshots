#include "rtpudpv4transmitter.h"
#include "rtperrors.h"
#include <cstdio>
static int run(unsigned port,bool mux){
 RTPUDPv4Transmitter t; if(t.Init(false)<0) return 999;
 RTPUDPv4TransmissionParams p; p.SetBindIP(0x7f000001);
 std::list<u_int32_t> l; l.push_back(0x7f000001); p.SetLocalIPList(l);
 p.SetPortbase((u_int16_t)port); p.SetRTCPMultiplexing(mux);
 int r=t.Create(1500,&p); t.Destroy(); return r;
}
int main(){
 printf("odd nonmux=%d (expect -96)\n",run(33001,false));
 printf("odd mux=%d (expect bind error -74 here)\n",run(33001,true));
 printf("even mux=%d (expect bind error -74 here)\n",run(33000,true));
 return 0;
}
