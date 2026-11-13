#include "rtp-payload-internal.h"
#include <stdio.h>
#include <string.h>
static int count;
static int packet(void *p,const void *data,int n,uint32_t t,int f){(void)p;(void)f;if(n!=2||memcmp(data,"\x65\xaa",2)||t!=9000)return -9;count++;return 0;}
int normal_h264(void){const unsigned char p[]={0x80,96,0,1,0,0,0x23,0x28,0,0,0,7,0x65,0xaa};struct rtp_payload_t h={0,0,packet};struct rtp_payload_decode_t *v=rtp_h264_decode();void *d=v->create(&h,0);int r;if(!d)return 0;count=0;r=v->input(d,p,sizeof(p));v->destroy(d);if(r!=1||count!=1)return 0;puts("normal_h264=ok");return 1;}
