#include "buffer_rcv.h"
#include "queue.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){
 using namespace srt;using namespace srt::sync;
 CUnitQueue queue(16,1500);CRcvBuffer receiver(71,16,&queue,true);
 const steady_clock::time_point base=steady_clock::time_point()+microseconds_from(10000000);
 receiver.setTsbPdMode(base,false,microseconds_from(120000));
 CUnit *u=queue.getNextAvailUnit();assert(u);
 const char data[]="local-srt-media";
 u->m_Packet.set_seqno(71);u->m_Packet.set_timestamp(2300);
 u->m_Packet.set_msgflags(1|PacketBoundaryBits(PB_SOLO)|MSGNO_PACKET_INORDER::wrap(1));
 u->m_Packet.setLength(sizeof(data));std::memcpy(u->m_Packet.data(),data,sizeof(data));
 assert(receiver.insert(u)==0);
 const steady_clock::time_point due=base+microseconds_from(122300);
 assert(!receiver.isRcvDataReady(due-microseconds_from(1)));
 assert(receiver.isRcvDataReady(due));
 CRcvBuffer::PacketInfo info=receiver.getFirstValidPacketInfo();assert(info.seqno==71&&info.tsbpd_time==due);
 char out[64]={};SRT_MSGCTRL ctrl={};
 assert(receiver.readMessage(out,sizeof(out),&ctrl)==sizeof(data));assert(!std::memcmp(out,data,sizeof(data)));
 assert(ctrl.srctime==count_microseconds(due.time_since_epoch()));assert(ctrl.pktseq==71);
 assert(!receiver.isRcvDataReady(due));
 std::puts("SRT_RECEIVE_BASELINE_OK payload=16 before_due=0 at_due=1 srctime=10122300");return 0;
}
