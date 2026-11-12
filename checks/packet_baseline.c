#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "intel-pt.h"
int main(void) {
 uint8_t buf[128]; uint64_t n;
 struct pt_config c; struct pt_packet p,out;
 struct pt_encoder *e; struct pt_packet_decoder *d;
 pt_config_init(&c); c.begin=buf;c.end=buf+sizeof(buf);
 e=pt_alloc_encoder(&c);assert(e);
 memset(&p,0,sizeof p);p.type=ppt_psb;assert(pt_enc_next(e,&p)>0);
 p.type=ppt_psbend;assert(pt_enc_next(e,&p)>0);
 p.type=ppt_tnt_8;p.payload.tnt.bit_size=3;p.payload.tnt.payload=5;
 assert(pt_enc_next(e,&p)>0);assert(pt_enc_get_offset(e,&n)==0);
 pt_free_encoder(e);c.end=buf+n;d=pt_pkt_alloc_decoder(&c);assert(d);
 assert(pt_pkt_sync_forward(d)==0);
 assert(pt_pkt_next(d,&out,sizeof out)>0 && out.type==ppt_psb);
 assert(pt_pkt_next(d,&out,sizeof out)>0 && out.type==ppt_psbend);
 assert(pt_pkt_next(d,&out,sizeof out)>0 && out.type==ppt_tnt_8);
 assert(out.payload.tnt.bit_size==3 && out.payload.tnt.payload==5);
 assert(pt_pkt_next(d,&out,sizeof out)==-pte_eos);
 pt_pkt_free_decoder(d);puts("PT_PACKET_OK tnt=101 packets=3");return 0;
}
