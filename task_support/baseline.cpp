#include "rtp_jitter.h"
#include <arpa/inet.h>
#include <iostream>
#include <cassert>
static rawrtp_ptr packet(uint16_t seq, uint8_t pt, uint32_t ssrc, uint8_t a, uint8_t b) {
    uint8_t wire[15] = {0x80, pt};
    uint16_t nseq=htons(seq); uint32_t ts=htonl(16000), source=htonl(ssrc);
    memcpy(wire+2,&nseq,2); memcpy(wire+4,&ts,4); memcpy(wire+8,&source,4);
    wire[12]=a; wire[13]=b; wire[14]=0x5a;
    rawrtp_ptr p(new RTPPacket(wire,sizeof(wire)));
    p->payload_ms=20; p->payload_type=pt; p->payload_bytes=3;
    return p;
}
int main() {
    RTPJitter q(20); q.reset();
    rawrtp_ptr original=packet(100,0,11,0x42,0x43), out;
    assert(q.push(original)==RTPJitter::SUCCESS);
    assert(q.get_depth()==1 && q.get_depth_ms()==20);
    assert(q.pop(out)==RTPJitter::SUCCESS && out==original && out->pData[12]==0x42);
    assert(q.get_depth()==0 && q.get_depth_ms()==0);
    std::cout << "LEGACY_PRIMARY seq=" << ntohs(reinterpret_cast<RTPHeader*>(out->pData)->sequence)
              << " payload=" << unsigned(out->pData[12]) << " depth=" << q.get_depth() << '\n';
    q.reset();
    rawrtp_ptr rtx=packet(900,97,22,0,101);
    assert(q.push(rtx)==RTPJitter::SUCCESS);
    assert(q.pop(out)==RTPJitter::SUCCESS && out==rtx);
    uint16_t seq=ntohs(reinterpret_cast<RTPHeader*>(out->pData)->sequence);
    assert(seq==900 && (out->pData[1]&127)==97 && out->pData[13]==101);
    std::cout << "LEGACY_RTX_OUTER seq=" << seq << " pt=" << unsigned(out->pData[1]&127)
              << " osn=" << unsigned(out->pData[13]) << '\n';
    return 0;
}
