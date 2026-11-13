#include "rtp-payload-internal.h"
#include <stdio.h>
#include <string.h>
extern int normal_h264(void);
static unsigned char got[3];static int count;
static int packet(void *p,const void *data,int n,uint32_t t,int f){(void)p;(void)t;(void)f;if(n!=2||count>=3)return -2;got[count++]=((const unsigned char*)data)[1];return 0;}
int main(void){unsigned char p[]={0x80,96,0,1,0,0,0x23,0x28,0,0,0,7,0x79,0,102,0,2,0x65,102};int i;unsigned don[]={102,100,101};struct rtp_payload_t h={0,0,packet};struct rtp_payload_decode_t *v=rtp_h264_decode();void *d;if(!normal_h264())return 2;d=v->create(&h,0);if(!d)return 3;for(i=0;i<3;i++){p[3]=(unsigned char)(i+1);p[14]=(unsigned char)don[i];p[18]=(unsigned char)don[i];if(v->input(d,p,sizeof(p))!=1)return 4;}v->destroy(d);printf("transmission_don_order=%u,%u,%u callbacks=%d\n",got[0],got[1],got[2],count);return count==3&&got[0]==102&&got[1]==100&&got[2]==101?0:5;}
