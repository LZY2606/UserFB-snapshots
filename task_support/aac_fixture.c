#include "aac_fixture.h"
#include <string.h>
struct writer { unsigned char *out; size_t cap, bits; int failed; };
static void put(struct writer *w, unsigned value, unsigned n) {
    while(n--) {
        if(w->bits/8>=w->cap) w->failed=1;
        else w->out[w->bits/8]|=((value>>n)&1u)<<(7-w->bits%8);
        ++w->bits;
    }
}
static void align_byte(struct writer *w) {while(w->bits%8) put(w,0,1);}
size_t fixture_asc(unsigned char *out, size_t cap,
                   const struct fixture_element *e, size_t count) {
    struct writer w={out,cap,0,0}; size_t i;
    if(!out||!cap||!e||!count||count>15) return 0;
    memset(out,0,cap);
    put(&w,2,5); put(&w,3,4); put(&w,0,4); /* LC, 48k, explicit PCE */
    put(&w,0,3); /* frameLengthFlag, dependsOnCoreCoder, extensionFlag */
    put(&w,0,4); put(&w,1,2); put(&w,3,4); /* PCE tag, LC profile, rate */
    put(&w,(unsigned)count,4); put(&w,0,4); put(&w,0,4);
    put(&w,0,2); put(&w,0,3); put(&w,0,4); put(&w,0,3);
    for(i=0;i<count;++i) {
        if(e[i].pair>1||e[i].tag>15) return 0;
        put(&w,e[i].pair,1);put(&w,e[i].tag,4);
    }
    align_byte(&w);put(&w,0,8); /* no comment */
    return w.failed?0:(w.bits+7)/8;
}
static void channel(struct writer *w,unsigned seq,unsigned shape,unsigned gain) {
    unsigned i;
    put(w,gain,8); put(w,0,1); put(w,seq,2); put(w,shape,1);
    if(seq==2) {put(w,1,4);put(w,127,7);} /* eight windows, one group */
    else {put(w,1,6);put(w,0,1);} /* max_sfb=1, no prediction */
    put(w,1,4);put(w,1,seq==2?3:5); /* codebook1, one band */
    put(w,0,1); /* scalefactor delta 0: HCB_SF symbol 60 */
    put(w,0,3); /* no pulse, TNS or gain control */
    /* HCB1 10000 is (1,0,0,0); first 48kHz band has four lines.
       A grouped short band contains one quadruple for each window. */
    for(i=0;i<(seq==2?8u:1u);++i) put(w,16,5);
}
size_t fixture_packet(unsigned char *out,size_t cap,
                      const struct fixture_element *e,size_t count,
                      unsigned seq,unsigned shape,unsigned gain) {
    struct writer w={out,cap,0,0};size_t i;
    if(!out||!cap||!e||!count||seq>3||shape>1||gain>220) return 0;
    memset(out,0,cap);
    for(i=0;i<count;++i) {
        if(e[i].pair>1||e[i].tag>15) return 0;
        put(&w,e[i].pair?1:0,3);put(&w,e[i].tag,4);
        if(e[i].pair) put(&w,0,1); /* independent channel windows */
        channel(&w,seq,shape,gain+e[i].tag);
        if(e[i].pair) channel(&w,seq,shape,gain+e[i].tag+4);
    }
    put(&w,7,3);align_byte(&w);
    return w.failed?0:(w.bits+7)/8;
}
