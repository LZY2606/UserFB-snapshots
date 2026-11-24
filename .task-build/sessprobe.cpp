#include "rtpsession.h"
#include "rtpsessionparams.h"
#include "rtpudpv4transmitter.h"
#include <cstdio>
int main(){
 RTPSessionParams sp; sp.SetUsePollThread(false); sp.SetOwnTimestampUnit(90000.0);
 RTPUDPv4TransmissionParams tp; tp.SetBindIP(0x7f000001); std::list<u_int32_t> l; l.push_back(0x7f000001); tp.SetLocalIPList(l); tp.SetPortbase(33000); tp.SetRTCPMultiplexing(true);
 RTPSession s;
 // Can't Create (bind blocked). Show the builder path via a direct builder with mux.
 printf("params mux=%d\n",(int)tp.IsRTCPMultiplexing());
}
