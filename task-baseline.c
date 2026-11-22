#include "librtmp/rtmp.h"
#include "librtmp/log.h"
#include "task_support/memory_io.h"
#include <stdio.h>
#include <string.h>
static int put(RTMP *writer,uint32_t ts,const unsigned char body[6]) {
    RTMPPacket p={0};
    if(!RTMPPacket_Alloc(&p,6)) return 0;
    p.m_headerType=RTMP_PACKET_SIZE_LARGE;p.m_nChannel=6;
    p.m_packetType=RTMP_PACKET_TYPE_VIDEO;p.m_nTimeStamp=ts;
    p.m_nInfoField2=1;p.m_nBodySize=6;
    memcpy(p.m_body,body,6);
    int ok=RTMP_SendPacket(writer,&p,0);
    RTMPPacket_Free(&p);return ok;
}
int main(void) {
    RTMP writer,reader;
    RTMPPacket p={0};
    const unsigned char key[6]={0x17,1,0,0,2,0x65};
    const unsigned char pred[6]={0x27,1,0,0,3,0x41};
    unsigned seen=0,steps=0;
    RTMP_LogSetLevel(RTMP_LOGERROR);
    RTMP_Init(&writer);RTMP_Init(&reader);
    writer.m_outChunkSize=reader.m_inChunkSize=3;
    task_io_reset();
    if(!put(&writer,0,key)||!put(&writer,40,pred))return 1;
    const unsigned char *w=task_io_data();
    if(task_io_size()!=38||w[0]!=6||w[7]!=9||w[19]!=6||w[22]!=40)return 2;
    puts("SYNC_SEND messages=2 wire_bytes=38 immediate=2");
    task_io_rewind();
    while(seen<2 && ++steps<10) {
        if(!RTMP_ReadPacket(&reader,&p))return 3;
        if(!RTMPPacket_IsReady(&p))continue;
        if(p.m_nBodySize!=6||p.m_packetType!=RTMP_PACKET_TYPE_VIDEO||p.m_nInfoField2!=1||p.m_nTimeStamp!=(seen?40:0)||memcmp(p.m_body,seen?pred:key,6))return 4;
        ++seen;RTMPPacket_Free(&p);memset(&p,0,sizeof(p));
    }
    if(seen!=2)return 5;
    printf("PRODUCTION_DECHUNK messages=%u timestamps=0,40 cts=2,3 payloads=match\n",seen);
    RTMP_Close(&reader);RTMP_Close(&writer);
    puts("BASELINE_COMPLETE");return 0;
}
