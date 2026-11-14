#include "rtp-payload.h"
#include "rtp-packet.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct state {void *decoder;unsigned int packets,decoded;size_t bytes;uint8_t expected[2000];};
static void *get(void *p,int n){(void)p;return malloc((size_t)n);}
static void put(void *p,void *m){(void)p;free(m);}
static int decoded(void *p,const void *data,int n,uint32_t ts,int flags){
 struct state *s=p;assert(ts==0xfffffff0U && flags==0 && n>0);
 assert(s->bytes+(size_t)n<=sizeof(s->expected));
 assert(memcmp(data,s->expected+s->bytes,(size_t)n)==0);
 s->bytes+=(size_t)n;s->decoded++;return 0;
}
static int encoded(void *p,const void *data,int n,uint32_t ts,int flags){
 struct state *s=p;struct rtp_packet_t pkt;
 assert(n<=564 && flags==0 && ts==0xfffffff0U);
 assert(rtp_packet_deserialize(&pkt,data,n)==0);
 assert(pkt.rtp.seq==(uint16_t)(65535U+s->packets));
 assert(pkt.rtp.ssrc==0x10203040U && pkt.rtp.pt==0 && pkt.rtp.m==0);
 s->packets++;
 assert(rtp_payload_decode_input(s->decoder,data,n)==1);return 0;
}
int main(void){
 struct state s={0};unsigned int i;uint16_t seq;uint32_t ts;
 struct rtp_payload_t rx={get,put,decoded},tx={get,put,encoded};void *encoder;
 for(i=0;i<sizeof(s.expected);i++)s.expected[i]=(uint8_t)(i*13U+7U);
 rtp_packet_setsize(564);
 s.decoder=rtp_payload_decode_create(0,"PCMU",&rx,&s);assert(s.decoder);
 encoder=rtp_payload_encode_create(0,"PCMU",65535,0x10203040U,&tx,&s);assert(encoder);
 assert(rtp_payload_encode_input(encoder,s.expected,sizeof(s.expected),0xfffffff0U)==0);
 assert(s.bytes==sizeof(s.expected) && s.packets==4 && s.decoded==4);
 rtp_payload_encode_getinfo(encoder,&seq,&ts);assert(seq==3 && ts==0xfffffff0U);
 rtp_payload_encode_destroy(encoder);rtp_payload_decode_destroy(s.decoder);
 puts("BASELINE_PCMU_OK bytes=2000 packets=4 sequence=65535,0,1,2 timestamp=4294967280");
 return 0;
}
