#include "buffer_rcv.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace srt;
static void need(bool ok) { if (!ok) throw std::runtime_error("baseline content mismatch"); }
static void put(CUnitQueue& q, CRcvBuffer& b, int seq, int msg, PacketBoundary boundary, const char* bytes) {
 CUnit* u=q.getNextAvailUnit();need(u!=NULL);
 CPacket& p=u->m_Packet;p.set_seqno(seq);p.set_timestamp(0);
 p.set_msgflags(msg|PacketBoundaryBits(boundary)|MSGNO_PACKET_INORDER::wrap(1));
 p.setLength((int)std::strlen(bytes));std::memcpy(p.data(),bytes,p.getLength());need(b.insert(u)==0);
}
int main() {
 try {
  const auto base=sync::steady_clock::now();const auto delay=sync::microseconds_from(1000);
  {
   CUnitQueue q(16,1500);CRcvBuffer b(100,16,&q,true);b.setPeerRexmitFlag(true);b.setTsbPdMode(base,false,delay);
   put(q,b,100,1,PB_SOLO,"abc");need(!b.isRcvDataReady(base+delay-sync::microseconds_from(1)));
   need(b.isRcvDataReady(base+delay));char out[32]={};SRT_MSGCTRL ctl=srt_msgctrl_default;
   need(b.readMessage(out,sizeof out,&ctl)==3);need(std::memcmp(out,"abc",3)==0&&ctl.msgno==1);
   std::cout<<"SOLO_OK bytes=3 msgno="<<ctl.msgno<<"\n";
  }
  {
   CUnitQueue q(16,1500);CRcvBuffer b(100,16,&q,true);b.setPeerRexmitFlag(true);b.setTsbPdMode(base,false,delay);
   put(q,b,100,1,PB_FIRST,"abc");put(q,b,101,1,PB_LAST,"def");
   need(b.isRcvDataReady(base+delay));char out[32]={};need(b.readMessage(out,sizeof out)==6);
   need(std::memcmp(out,"abcdef",6)==0);std::cout<<"EXISTING_FRAGMENT_ASSEMBLY_OK bytes=6\n";
  }
  {
   CUnitQueue q(16,1500);CRcvBuffer b(100,16,&q,true);b.setPeerRexmitFlag(true);b.setTsbPdMode(base,false,delay);
   put(q,b,100,1,PB_FIRST,"abc");put(q,b,102,1,PB_LAST,"ghi");put(q,b,103,2,PB_SOLO,"next");
   // Current raw packet-skip behavior leaves a continuation at the head.
   const auto dropped=b.dropUpTo(102);
   std::cout<<"RAW_SKIP_HEAD="<<b.getStartSeqNo()<<" READY="<<b.isRcvDataReady(base+delay)<<" DROP_MISSING="<<dropped.first<<" DISCARD="<<dropped.second<<"\n";
   need(b.size()>0);
  }
  std::cout<<"BASELINE_OK\n";return 0;
 } catch (const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
