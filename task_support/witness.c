#include "rtmp.h"
#include "log.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
extern int normal_rtmp(void);
int main(void){int f[2],flags,sent,connected;char data[4096];RTMP r;RTMPPacket p;if(!normal_rtmp())return 2;if(socketpair(AF_UNIX,SOCK_STREAM,0,f))return 3;flags=fcntl(f[0],F_GETFL);if(fcntl(f[0],F_SETFL,flags|O_NONBLOCK))return 4;memset(data,1,sizeof(data));while(send(f[0],data,sizeof(data),0)>0){}if(errno!=EAGAIN&&errno!=EWOULDBLOCK)return 5;RTMP_Init(&r);r.m_sb.sb_socket=f[0];RTMPPacket_Reset(&p);if(!RTMPPacket_Alloc(&p,2))return 6;p.m_nChannel=4;p.m_headerType=RTMP_PACKET_SIZE_LARGE;p.m_packetType=RTMP_PACKET_TYPE_AUDIO;p.m_nInfoField2=1;p.m_nBodySize=2;p.m_body[0]=(char)0xaf;p.m_body[1]=1;RTMP_LogSetLevel(RTMP_LOGERROR);sent=RTMP_SendPacket(&r,&p,0);connected=RTMP_IsConnected(&r);printf("backpressure_send=%d connected=%d\n",sent,connected);RTMPPacket_Free(&p);RTMP_Close(&r);close(f[1]);return sent==0&&connected==0?0:7;}
