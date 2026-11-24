// Exercises RTPSession create/mux flag wiring without binding.
#include "rtpsession.h"
#include "rtpsessionparams.h"
#include "rtpudpv4transmitter.h"
#include "rtppacketbuilder.h"
#include <cstdio>
int main(){
 // The builder is a private member; emulate Create wiring by directly checking
 // the transmitter reports its mode before Create would bind.
 RTPUDPv4TransmissionParams p; p.SetRTCPMultiplexing(true);
 RTPUDPv4Transmitter t; t.Init(false);
 // without Create, mux flag default is false
 printf("pre-create mux=%d\n",(int)t.IsRTCPMultiplexing());
 printf("params mux=%d\n",(int)p.IsRTCPMultiplexing());
 RTPUDPv4TransmissionParams q;
 printf("default mux=%d\n",(int)q.IsRTCPMultiplexing());
 return 0;
}
