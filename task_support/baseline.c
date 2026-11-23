#include <re.h>
#include <rem.h>
#include <stdio.h>
#define REQUIRE(x) do { if (!(x)) {fprintf(stderr,"baseline line %d\n",__LINE__);return 1;} } while (0)
int main(void) {
    struct mbuf *mb=mbuf_alloc(64); struct rtp_header in={0}, out={0};
    struct rtp_source src={0}; struct aubuf *ab=NULL;
    int16_t a[]={11,12}, b[]={21,22}, pcm[4]={0}; struct auframe af;
    REQUIRE(mb);
    in.ver=2;in.pt=96;in.seq=65535;in.ts=0xfffffffe;in.ssrc=7;
    REQUIRE(!rtp_hdr_encode(mb,&in)); REQUIRE(!mbuf_write_mem(mb,(uint8_t[]){0,11,0,12},4));
    mb->pos=0; REQUIRE(!rtp_hdr_decode(&out,mb));
    REQUIRE(out.seq==65535 && out.ts==0xfffffffe && mbuf_get_left(mb)==4);
    rtp_source_init_seq(&src,65535); REQUIRE(rtp_source_update_seq(&src,0));
    REQUIRE(src.cycles==65536 && src.received==1);
    REQUIRE(!aubuf_alloc(&ab,0,64)); aubuf_set_live(ab,false);
    auframe_init(&af,AUFMT_S16LE,a,2,8000,1); af.timestamp=1000000;
    REQUIRE(!aubuf_write_auframe(ab,&af));
    auframe_init(&af,AUFMT_S16LE,b,2,8000,1); af.timestamp=1000500;
    REQUIRE(!aubuf_write_auframe(ab,&af));
    aubuf_read_samp(ab,pcm,4);
    REQUIRE(pcm[0]==11 && pcm[1]==12 && pcm[2]==21 && pcm[3]==22);
    printf("BASELINE rtp_payload=4 sequence_cycles=%u\n",src.cycles);
    printf("BASELINE aubuf_pcm=%d,%d,%d,%d gap_metadata_us=250\n",pcm[0],pcm[1],pcm[2],pcm[3]);
    mem_deref(ab);mem_deref(mb);return 0;
}
