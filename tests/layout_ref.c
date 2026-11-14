#include "layout_ref.h"
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

void asc_spec_init(struct asc_spec *s) {
    memset(s, 0, sizeof *s);
    s->chconf = 0;
    s->sf_index = 3;   /* 48 kHz */
    s->pce_profile = 1;/* LC */
    s->pce_sf = 3;
}

static void put_elements(struct writer *w, const struct fixture_element *e,
                         unsigned count) {
    unsigned i;
    for(i=0;i<count;++i) {
        if(e[i].pair>1||e[i].tag>15) { w->failed=1; return; }
        put(w,e[i].pair,1); put(w,e[i].tag,4);
    }
}

static void put_pce_body(struct writer *w, const struct asc_spec *s) {
    unsigned i;
    put(w,0,4); put(w,s->pce_profile,2); put(w,s->pce_sf,4);
    put(w,s->nfront,4); put(w,s->nside,4); put(w,s->nback,4);
    put(w,s->nlfe,2); put(w,s->nassoc,3); put(w,s->ncc,4);
    put(w,s->mono_mix,1); if(s->mono_mix) put(w,0,4);
    put(w,s->stereo_mix,1); if(s->stereo_mix) put(w,0,4);
    put(w,s->matrix_mix,1); if(s->matrix_mix) { put(w,0,2); put(w,0,1); }
    put_elements(w,s->front,s->nfront);
    put_elements(w,s->side,s->nside);
    put_elements(w,s->back,s->nback);
    for(i=0;i<s->nlfe;++i) put(w,s->lfe_tag[i],4);
    for(i=0;i<s->nassoc;++i) put(w,s->assoc_tag[i],4);
    for(i=0;i<s->ncc;++i) { put(w,0,1); put(w,s->cc_tag[i],4); }
    align_byte(w);
    put(w,0,8); /* no comment */
}

size_t layout_asc(unsigned char *out, size_t cap, const struct asc_spec *s) {
    struct writer w={out,cap,0,0};
    if(!out||!cap||!s) return 0;
    if(s->nfront>16||s->nside>16||s->nback>16||s->nlfe>4||s->nassoc>8||s->ncc>16)
        return 0;
    memset(out,0,cap);
    put(&w,2,5); put(&w,s->sf_index,4); put(&w,s->chconf,4);
    put(&w,s->framelen_flag,1); put(&w,0,1); put(&w,s->ext_flag,1);
    if(s->ext_flag) put(&w,0,1); /* extensionFlag3 */
    if(s->chconf==0) put_pce_body(&w,s);
    align_byte(&w);
    return w.failed?0:(w.bits+7)/8;
}

/* same ordinary spectrum payload as aac_fixture.c */
static void put_channel(struct writer *w, unsigned seq, unsigned shape,
                        unsigned gain) {
    unsigned i;
    put(w,gain,8); put(w,0,1); put(w,seq,2); put(w,shape,1);
    if(seq==2) {put(w,1,4);put(w,127,7);} /* eight windows, one group */
    else {put(w,1,6);put(w,0,1);} /* max_sfb=1, no prediction */
    put(w,1,4);put(w,1,seq==2?3:5); /* codebook1, one band */
    put(w,0,1); /* scalefactor delta 0 */
    put(w,0,3); /* no pulse, TNS or gain control */
    for(i=0;i<(seq==2?8u:1u);++i) put(w,16,5); /* HCB1 (1,0,0,0) */
}

size_t layout_packet_pce(unsigned char *out, size_t cap,
                         const struct asc_spec *pce_spec,
                         const struct fixture_element *e, unsigned count,
                         unsigned seq, unsigned shape, unsigned gain) {
    struct writer w={out,cap,0,0}; unsigned i;
    if(!out||!cap||!pce_spec||!e||!count||seq>3||shape>1||gain>220) return 0;
    memset(out,0,cap);
    put(&w,5,3); /* ID_PCE */
    put_pce_body(&w,pce_spec);
    for(i=0;i<count;++i) {
        if(e[i].pair>1||e[i].tag>15) return 0;
        put(&w,e[i].pair?1:0,3); put(&w,e[i].tag,4);
        if(e[i].pair) put(&w,0,1); /* independent channel windows */
        put_channel(&w,seq,shape,gain+e[i].tag);
        if(e[i].pair) put_channel(&w,seq,shape,gain+e[i].tag+4);
    }
    put(&w,7,3); align_byte(&w);
    return w.failed?0:(w.bits+7)/8;
}

unsigned spec_channels(const struct asc_spec *s) {
    unsigned i, ch = 0;
    for(i=0;i<s->nfront;++i) ch += s->front[i].pair?2:1;
    for(i=0;i<s->nside;++i)  ch += s->side[i].pair?2:1;
    for(i=0;i<s->nback;++i)  ch += s->back[i].pair?2:1;
    ch += s->nlfe;
    return ch;
}

int spec_slot(const struct asc_spec *s, unsigned pair, unsigned tag) {
    unsigned i, ch = 0;
    for(i=0;i<s->nfront;++i) {
        if(s->front[i].pair==pair&&s->front[i].tag==tag) return (int)ch;
        ch += s->front[i].pair?2:1;
    }
    for(i=0;i<s->nside;++i) {
        if(s->side[i].pair==pair&&s->side[i].tag==tag) return (int)ch;
        ch += s->side[i].pair?2:1;
    }
    for(i=0;i<s->nback;++i) {
        if(s->back[i].pair==pair&&s->back[i].tag==tag) return (int)ch;
        ch += s->back[i].pair?2:1;
    }
    return -1;
}

NeAACDecHandle layout_open(const unsigned char *asc, unsigned long n) {
    unsigned long rate=0; unsigned char channels=0;
    NeAACDecHandle h=NeAACDecOpen(); NeAACDecConfigurationPtr c;
    if(!h) return 0;
    c=NeAACDecGetCurrentConfiguration(h);
    c->outputFormat=FAAD_FMT_FLOAT; c->downMatrix=0; c->dontUpSampleImplicitSBR=1;
    if(!NeAACDecSetConfiguration(h,c)) { NeAACDecClose(h); return 0; }
    if(NeAACDecInit2(h,(unsigned char*)asc,n,&rate,&channels)!=0) {
        NeAACDecClose(h); return 0;
    }
    if(rate!=48000||channels==0) { NeAACDecClose(h); return 0; }
    return h;
}
