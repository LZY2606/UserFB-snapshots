#include "rtmp.h"
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int normal_rtmp(void){int f[2],ok;unsigned char wire[32];RTMP r;RTMPPacket p;if(socketpair(AF_UNIX,SOCK_STREAM,0,f))return 0;RTMP_Init(&r);r.m_sb.sb_socket=f[0];RTMPPacket_Reset(&p);if(!RTMPPacket_Alloc(&p,2))return 0;p.m_nChannel=4;p.m_headerType=RTMP_PACKET_SIZE_LARGE;p.m_packetType=RTMP_PACKET_TYPE_AUDIO;p.m_nInfoField2=1;p.m_nTimeStamp=7;p.m_nBodySize=2;p.m_body[0]=(char)0xaf;p.m_body[1]=1;ok=RTMP_SendPacket(&r,&p,0);if(read(f[1],wire,14)!=14)ok=0;if(wire[0]!=4||wire[3]!=7||wire[6]!=2||wire[7]!=8||wire[8]!=1||wire[12]!=0xaf||wire[13]!=1)ok=0;RTMPPacket_Free(&p);RTMP_Close(&r);close(f[1]);if(ok)puts("normal_rtmp_wire=ok");return ok;}
