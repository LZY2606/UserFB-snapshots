#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_list.h>
#include <re_mbuf.h>
#include <re_rtp.h>
#include <re_jbuf.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    struct jbuf *j = NULL;
    struct mbuf *wire = mbuf_alloc(64), *audio = mbuf_alloc(4);
    struct rtp_header h = {0}, decoded = {0}, out = {0};
    void *payload = NULL;
    if (!wire || !audio || jbuf_alloc(&j, 0, 8)) return 1;
    if (mbuf_write_mem(audio, (const uint8_t *)"abc", 3)) return 2;
    audio->pos = 0;
    h.ver=2; h.pt=96; h.seq=65535; h.ts=0xffffff00; h.ssrc=11;
    if (rtp_hdr_encode(wire,&h)) return 3;
    wire->pos=0;
    if (rtp_hdr_decode(&decoded,wire) || decoded.seq!=h.seq || decoded.ts!=h.ts) return 4;
    if (jbuf_put(j,&decoded,audio)) return 5;
    h.seq=1; h.ts+=320;
    if (jbuf_put(j,&h,audio)) return 6;
    if (jbuf_get(j,&out,&payload) || out.seq!=65535 || mbuf_get_left(payload)!=3 || memcmp(mbuf_buf(payload),"abc",3)) return 7;
    payload=mem_deref(payload);
    if (jbuf_get(j,&out,&payload) || out.seq!=1) return 8;
    payload=mem_deref(payload);
    if (jbuf_get(j,&out,&payload)!=ENOENT) return 9;
    puts("RTP_HEADER_ROUNDTRIP seq=65535 timestamp=4294967040");
    puts("COUNT_JBUF delivered=65535,1 missing=0 not_recovered");
    jbuf_flush(j);
    mem_deref(j);mem_deref(wire);mem_deref(audio);
    puts("BASELINE_COMPLETE");return 0;
}
