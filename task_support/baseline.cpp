#include "buffer_rcv.h"
#include "list.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace srt;
static void need(bool ok) { if(!ok) throw std::runtime_error("production baseline mismatch"); }
static void put(CUnitQueue& q, CRcvBuffer& b, int seq, int msg, uint32_t ts, char payload) {
    CUnit* u=q.getNextAvailUnit();need(u!=NULL);
    CPacket& p=u->m_Packet;p.set_seqno(seq);p.set_timestamp(ts);
    p.set_msgflags(msg|PacketBoundaryBits(PB_SOLO)|MSGNO_PACKET_INORDER::wrap(1));
    p.setLength(1);p.data()[0]=payload;need(b.insert(u)==0);
}
int main() {
 try {
    CRcvLossList losses(64);
    need(losses.insert(100,100)==1);need(losses.insert(102,102)==1);
    need(losses.insert(104,104)==1);need(losses.insert(106,106)==1);
    FixedArray<int32_t> one(3),two(3);
    int n1=losses.getLossArray(one),n2=losses.getLossArray(two);
    need(n1==2 && n2==2 && one[0]==100 && one[1]==102 && two[0]==100 && two[1]==102);
    need(losses.getLossLength()==4 && losses.find(104,106));
    std::cout<<"PREFIX_REPEAT first="<<one[0]<<","<<one[1]<<" second="<<two[0]<<","<<two[1]<<" remaining="<<losses.getLossLength()<<"\n";
    CUnitQueue q(16,1500);CRcvBuffer buffer(100,16,&q,true);
    const auto base=sync::steady_clock::time_point()+sync::microseconds_from(1000000);
    const auto delay=sync::microseconds_from(1000);
    buffer.setPeerRexmitFlag(true);buffer.setTsbPdMode(base,false,delay);
    put(q,buffer,101,1,0,'A');put(q,buffer,103,2,1000,'B');
    put(q,buffer,105,3,2000,'C');put(q,buffer,107,4,3000,'D');
    const auto info=buffer.getFirstValidPacketInfo();
    need(info.seq_gap && info.seqno==101 && info.tsbpd_time==base+delay);
    need(!buffer.isRcvDataReady(base+delay-sync::microseconds_from(1)));
    std::cout<<"TSBPD_GAP seq="<<info.seqno<<" deadline_us="<<sync::count_microseconds(info.tsbpd_time.time_since_epoch())<<"\n";
    // Baseline uses existing primitive operations, not the new coordination/commit path.
    auto drop=buffer.dropUpTo(101);need(drop.first==1 && drop.second==0);
    need(losses.removeUpTo(100)==100);need(losses.getLossLength()==3);
    need(buffer.isRcvDataReady(base+delay));char out[16]={};SRT_MSGCTRL ctrl=srt_msgctrl_default;
    need(buffer.readMessage(out,sizeof out,&ctrl)==1 && out[0]=='A' && ctrl.msgno==1);
    FixedArray<int32_t> after(3);need(losses.getLossArray(after)==2 && after[0]==102 && after[1]==104);
    std::cout<<"PLAYBACK_PRIMITIVES payload="<<out[0]<<" first_loss="<<after[0]<<" losses="<<losses.getLossLength()<<"\n";
    std::cout<<"BASELINE_OK\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
