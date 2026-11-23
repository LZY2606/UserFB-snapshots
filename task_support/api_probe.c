/* Compile/link consumer of the requested API; supplies no implementation. */
#include <re.h>
#include <rem.h>
#include <re_pcm.h>
#include <stdio.h>
#define REQUIRE(x) do {if (!(x)) {fprintf(stderr,"API_PROBE line %d\n",__LINE__);return 1;}} while (0)
int main(void) {
    struct aubuf *ab=NULL; struct rtp_pcm *rx=NULL;
    struct rtp_source s={0}; struct mbuf *mb=mbuf_alloc(16);
    struct rtp_header h={0};int16_t in[]={123,-45},out[2]={99,99};
    REQUIRE(!aubuf_timeline_alloc(&ab,1,8));
    REQUIRE(!aubuf_timeline_write(ab,0,in,2));
    REQUIRE(!aubuf_timeline_read(ab,out,2) && out[0]==123 && out[1]==-45);
    mem_deref(ab); rtp_source_init_seq(&s,10);
    REQUIRE(rtp_source_update_seq_ex(&s,11)==RTP_SEQ_ACCEPT);
    REQUIRE(!rtp_pcm_alloc(&rx,7,96,1,8));
    h.ver=2;h.pt=96;h.ssrc=7;h.seq=10;h.ts=1234;
    REQUIRE(mb && !rtp_hdr_encode(mb,&h));
    REQUIRE(!mbuf_write_mem(mb,(uint8_t[]){0,123,255,211},4));mb->pos=0;
    REQUIRE(!rtp_pcm_push(rx,mb)); REQUIRE(mb->pos==0);
    REQUIRE(!rtp_pcm_read(rx,out,2) && out[0]==123 && out[1]==-45);
    rtp_pcm_reset(rx);out[0]=99;
    REQUIRE(!rtp_pcm_read(rx,out,1) && out[0]==0);
    mem_deref(rx);mem_deref(mb);puts("API_PROBE_COMPLETE");return 0;
}
