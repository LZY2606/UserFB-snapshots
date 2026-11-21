#include "ScreamTx.h"
#include "RtpQueue.h"
#include <cstdio>
#include <cstdlib>

extern unsigned int task_packet_freed;
static bool transmit(ScreamV2Tx& tx, RtpQueue& q, uint32_t now, uint32_t expected, uint16_t seq) {
    uint32_t chosen=0;
    if(tx.isOkToTransmit(now,chosen)!=0.0f || chosen!=expected)return false;
    void* p=nullptr;int bytes=0;uint32_t ssrc=0,ts=0;uint16_t sn=0;bool marker=false;
    if(!q.pop(&p,bytes,ssrc,sn,marker,ts) || sn!=seq || ssrc!=expected || bytes!=100 || !marker)return false;
    tx.addTransmitted(now,ssrc,bytes,sn,marker,0.0f,ts);
    std::free(p);return true;
}
int main() {
    RtpQueue q;
    ScreamV2Tx tx;
    const uint32_t ssrc=42,t=1024;
    tx.enablePacketPacing(false);
    tx.registerNewStream(&q,ssrc,1.0f,10000,100000,1000000,10.0f);
    if(!q.push(std::malloc(100),100,ssrc,10,true,float(t)/65536,9000))return 10;
    tx.newMediaFrame(t,ssrc,100,true);
    if(!transmit(tx,q,t,ssrc,10))return 11;
    tx.incomingStandardizedFeedback(t+100,0,t+50,10,0,true);
    if(!q.push(std::malloc(100),100,ssrc,11,true,float(t+200)/65536,12000))return 12;
    tx.newMediaFrame(t+200,ssrc,100,true);
    if(!transmit(tx,q,t+200,ssrc,11) || q.sizeOfQueue()!=0 || q.bytesInQueue()!=0)return 13;
    if(!q.push(std::malloc(100),100,ssrc,12,true,float(t+300)/65536,15000))return 14;
    unsigned int before=task_packet_freed;
    if(q.clear()!=1 || task_packet_freed!=before+1 || q.sizeOfNextRtp()!=-1)return 15;
    std::puts("BASELINE sent=2 sequences=10,11 feedback=positive queue=0 freed=1");
    return 0;
}
