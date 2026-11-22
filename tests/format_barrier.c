/* Format barrier tests for the PDMP3 streaming API.
 *
 * Segments of MPEG-1 Layer III with different sampling rates and channel
 * counts are concatenated and fed through pdmp3_feed/pdmp3_read. Every
 * format change must surface as exactly one PDMP3_NEW_FORMAT event with
 * done == 0, confirmed through pdmp3_getformat, and the PCM of each segment
 * must match a standalone decode of that segment.
 *
 * All sizes and 'done' values are in bytes; rates are in Hz.
 */
#define PDMP3_HEADER_ONLY
#include "pdmp3.c"
#include "check.h"
#include "frame_fixture.h"
#include <stdlib.h>
#include <string.h>

#define MAX_PCM    (256*1024)
#define MAX_STREAM (96*1024)
#define MAX_EVENTS 8

struct result {
    size_t pcm_len;
    long ev_rate[MAX_EVENTS];
    int ev_ch[MAX_EVENTS];
    unsigned ev_count;
    int status; /* final read status,PDMP3_NEED_MORE on a clean drain */
};

static long pcm_energy(const unsigned char *p,size_t n) {
    long e=0;
    for(size_t i=0;i+1<n;i+=2) { short v; memcpy(&v,p+i,2); e+=(v<0)?-(long)v:(long)v; }
    return e;
}

/* Feed 'inlen' bytes through 'd' in feed_chunk-sized pieces,draining with
 * read_chunk-sized output buffers and confirming every format event.
 * PCM is appended to pcm. Returns 0 on a clean drain. */
static int run_stream(pdmp3_handle *d,const unsigned char *in,size_t inlen,
                      size_t feed_chunk,size_t read_chunk,
                      unsigned char *pcm,size_t pcm_cap,struct result *r) {
    unsigned char out[16384];
    size_t pos=0;
    r->pcm_len=0; r->ev_count=0; r->status=PDMP3_OK;
    if(read_chunk>sizeof(out) || !feed_chunk) return -1;
    for(;;) {
        size_t done=0;
        int rc=pdmp3_read(d,out,read_chunk,&done);
        if(rc==PDMP3_NEW_FORMAT) {
            long rate=0; int ch=0,enc=0;
            if(done!=0) return -2; /* NEW_FORMAT must not carry PCM */
            if(pdmp3_getformat(d,&rate,&ch,&enc)!=PDMP3_OK) return -3;
            if(enc!=PDMP3_ENC_SIGNED_16) return -4;
            if(r->ev_count>=MAX_EVENTS) return -5;
            r->ev_rate[r->ev_count]=rate; r->ev_ch[r->ev_count]=ch; r->ev_count++;
            continue;
        }
        if(rc==PDMP3_OK) {
            if(done==0) return -6; /* no progress: would busy-loop */
            if(r->pcm_len+done>pcm_cap) return -7;
            memcpy(pcm+r->pcm_len,out,done); r->pcm_len+=done;
            continue;
        }
        if(rc==PDMP3_NEED_MORE) {
            if(pos<inlen) {
                size_t n=inlen-pos;
                if(n>feed_chunk) n=feed_chunk;
                if(pdmp3_feed(d,in+pos,n)!=PDMP3_OK) return -8;
                pos+=n;
                continue;
            }
            r->status=rc;
            return 0;
        }
        r->status=rc;
        return -9;
    }
}

/* Decode a stream on a fresh handle: the per-segment reference. */
static int decode_reference(const unsigned char *in,size_t inlen,
                            unsigned char *pcm,size_t cap,struct result *r) {
    pdmp3_handle *d=pdmp3_new(NULL,NULL);
    int rc;
    if(!d) return -1;
    if(pdmp3_open_feed(d)!=PDMP3_OK) { pdmp3_delete(d); return -1; }
    rc=run_stream(d,in,inlen,inlen?inlen:1,16384,pcm,cap,r);
    pdmp3_delete(d);
    return rc;
}

struct segdef { unsigned rate_index,channels,frames,variant,phase; };

/* Build one segment. variant 0: plain 128 kbps frames with varying padding
 * and CRC. variant 1: bitrate,padding,CRC and stereo/joint-stereo mode vary
 * per frame and every third frame carries its main data in the previous
 * frame's tail (main_data_begin > 0). */
static size_t build_segment(unsigned char *out,const struct segdef *s,size_t *offs) {
    static const unsigned br[4]={9,12,6,10};
    size_t total=0;
    for(unsigned i=0;i<s->frames;i++) {
        unsigned padding=0,crc_on=0,bitrate=9,mode=(s->channels==1)?3u:0u,mdb=0,n;
        if(s->variant==0) { padding=i&1u; crc_on=(i%3==0); }
        else {
            bitrate=br[i%4]; padding=(i+1)&1u; crc_on=((i+1)%3==0);
            if(s->channels==2 && (i&1u)) mode=1; /* joint stereo: no format change */
            if(i>0 && (i%3==2)) mdb=4;           /* main data in previous frame */
        }
        if(offs) offs[i]=total;
        n=fixture_frame_ex(out+total,s->rate_index,mode,bitrate,padding,crc_on,mdb,s->phase+i);
        if(!n) return 0;
        total+=n;
    }
    if(offs) offs[s->frames]=total;
    return total;
}

static const long chain_rates[3]={44100,48000,32000};
static const int chain_chs[3]={2,1,2};

static unsigned char g_in[MAX_STREAM];
static unsigned char g_refpcm[3][MAX_PCM];
static unsigned char g_outpcm[MAX_PCM];
static size_t g_seg_len[3];

static size_t build_chain(void) {
    static const struct segdef defs[3]={
        {0,2,12,1,0},   /* 44100 Hz stereo */
        {1,1,14,1,40},  /* 48000 Hz mono */
        {2,2,10,1,80},  /* 32000 Hz stereo */
    };
    size_t total=0;
    for(int s=0;s<3;s++) {
        size_t n=build_segment(g_in+total,&defs[s],NULL);
        if(!n) return 0;
        g_seg_len[s]=n; total+=n;
    }
    return total;
}

/* Build the 44100 stereo -> 48000 mono -> 32000 stereo chain and decode
 * each segment standalone as the reference. Returns total input length. */
static size_t prepare_chain(struct result refs[3]) {
    size_t total=build_chain(),off=0;
    if(!total) return 0;
    for(int s=0;s<3;s++) {
        if(decode_reference(g_in+off,g_seg_len[s],g_refpcm[s],MAX_PCM,&refs[s])) return 0;
        if(refs[s].ev_count!=1) return 0;
        if(refs[s].ev_rate[0]!=chain_rates[s] || refs[s].ev_ch[0]!=chain_chs[s]) return 0;
        if(refs[s].pcm_len==0 || pcm_energy(g_refpcm[s],refs[s].pcm_len)<=0) return 0;
        off+=g_seg_len[s];
    }
    return total;
}

static int chain_run_matches(const struct result refs[3],const struct result *run,
                             const unsigned char *pcm) {
    size_t expect=0,off=0;
    if(run->ev_count!=3) return 0;
    for(int s=0;s<3;s++) {
        if(run->ev_rate[s]!=chain_rates[s] || run->ev_ch[s]!=chain_chs[s]) return 0;
        expect+=refs[s].pcm_len;
    }
    if(run->pcm_len!=expect) return 0;
    for(int s=0;s<3;s++) {
        if(memcmp(pcm+off,g_refpcm[s],refs[s].pcm_len)!=0) return 0;
        off+=refs[s].pcm_len;
    }
    return 1;
}

/* ---------------------------------------------------------------- events */

static int chain_events(void) {
    struct result refs[3],run;
    size_t total=prepare_chain(refs);
    pdmp3_handle *d;
    TASK_CHECK(total>0);
    TASK_CHECK(total>INBUF_SIZE); /* exercises ring wrap as well */
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(run_stream(d,g_in,total,4096,16384,g_outpcm,sizeof(g_outpcm),&run)==0);
    TASK_CHECK(run.status==PDMP3_NEED_MORE); /* input fully consumed */
    TASK_CHECK(run.ev_count==3);
    TASK_CHECK(run.ev_rate[0]==44100 && run.ev_ch[0]==2);
    TASK_CHECK(run.ev_rate[1]==48000 && run.ev_ch[1]==1);
    TASK_CHECK(run.ev_rate[2]==32000 && run.ev_ch[2]==2);
    TASK_CHECK(chain_run_matches(refs,&run,g_outpcm));
    pdmp3_delete(d);
    return 0;
}

static int first_notification(void) {
    unsigned char in[4096],one[1024],out[4608];
    struct segdef s={0,2,4,0,3};
    size_t len=build_segment(in,&s,NULL);
    size_t first=fixture_frame_ex(one,0,0,9,0,1,0,3); /* identical to frame 0 */
    size_t done=77;
    long rate=-1; int ch=-1,enc=-1;
    pdmp3_handle *d;
    TASK_CHECK(len>0 && first>0 && first<len);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    /* Only a fragment of the first frame: no notification,no format yet. */
    TASK_CHECK(pdmp3_feed(d,in,first-1)==PDMP3_OK);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_NEED_MORE);
    TASK_CHECK(rate==-1 && ch==-1 && enc==-1); /* outputs untouched */
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEED_MORE && done==0);
    /* Complete the stream: the first read announces the format,done == 0. */
    TASK_CHECK(pdmp3_feed(d,in+first-1,len-(first-1))==PDMP3_OK);
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEW_FORMAT && done==0);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK);
    TASK_CHECK(rate==44100 && ch==2 && enc==PDMP3_ENC_SIGNED_16);
    /* Repeated queries just report the active format. */
    rate=0; ch=0; enc=0;
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK);
    TASK_CHECK(rate==44100 && ch==2 && enc==PDMP3_ENC_SIGNED_16);
    /* PCM flows after confirmation. */
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_OK && done>0);
    TASK_CHECK(pcm_energy(out,done)>0);
    pdmp3_delete(d);
    return 0;
}

static int unconfirmed_repeat(void) {
    unsigned char in[8192],out[4608];
    static unsigned char refa[32768],refb[32768],outa[32768],outb[32768];
    struct segdef a={0,2,3,0,5},b={1,1,3,0,9};
    struct result ra,rb;
    size_t lena,lenb,done=0,na=0,nb=0,guard=0;
    long rate=0; int ch=0,enc=0,rc;
    pdmp3_handle *d;
    lena=build_segment(in,&a,NULL);
    lenb=build_segment(in+lena,&b,NULL);
    TASK_CHECK(lena>0 && lenb>0);
    TASK_CHECK(decode_reference(in,lena,refa,sizeof(refa),&ra)==0);
    TASK_CHECK(decode_reference(in+lena,lenb,refb,sizeof(refb),&rb)==0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(pdmp3_feed(d,in,lena+lenb)==PDMP3_OK);
    /* The same notification is repeated until it is confirmed,and the new
     * segment is not decoded before that. */
    for(int i=0;i<3;i++) {
        TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEW_FORMAT && done==0);
    }
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK);
    TASK_CHECK(rate==44100 && ch==2);
    /* Drain segment A,then expect the second notification. */
    for(;;) {
        TASK_CHECK(++guard<1000);
        rc=pdmp3_read(d,out,sizeof(out),&done);
        if(rc==PDMP3_NEW_FORMAT) break;
        TASK_CHECK(rc==PDMP3_OK && done>0 && na+done<=sizeof(outa));
        memcpy(outa+na,out,done);
        na+=done;
    }
    TASK_CHECK(done==0);
    TASK_CHECK(na==ra.pcm_len && memcmp(outa,refa,na)==0);
    /* Again: repeated reads restate the pending event without decoding. */
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEW_FORMAT && done==0);
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEW_FORMAT && done==0);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK);
    TASK_CHECK(rate==48000 && ch==1);
    guard=0;
    for(;;) {
        TASK_CHECK(++guard<1000);
        rc=pdmp3_read(d,out,sizeof(out),&done);
        if(rc==PDMP3_NEED_MORE) break;
        TASK_CHECK(rc==PDMP3_OK && done>0 && nb+done<=sizeof(outb));
        memcpy(outb+nb,out,done);
        nb+=done;
    }
    TASK_CHECK(done==0);
    TASK_CHECK(nb==rb.pcm_len && memcmp(outb,refb,nb)==0);
    pdmp3_delete(d);
    return 0;
}

static int deferred_notice_one_sample(void) {
    unsigned char in[8192],out[4608];
    struct segdef a={0,2,2,0,1},b={1,1,2,0,2};
    size_t lena,lenb,done=0;
    long rate=0; int ch=0,enc=0;
    pdmp3_handle *d;
    lena=build_segment(in,&a,NULL);
    lenb=build_segment(in+lena,&b,NULL);
    TASK_CHECK(lena>0 && lenb>0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(pdmp3_feed(d,in,lena+lenb)==PDMP3_OK);
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEW_FORMAT && done==0);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK && rate==44100 && ch==2);
    /* First stereo frame: 1152 sample frames = 4608 bytes. */
    TASK_CHECK(pdmp3_read(d,out,4608,&done)==PDMP3_OK && done==4608);
    /* Leave exactly one sample frame (4 bytes) of old PCM undelivered. */
    TASK_CHECK(pdmp3_read(d,out,4604,&done)==PDMP3_OK && done==4604);
    /* This read delivers the last old PCM; the notice is deferred. */
    TASK_CHECK(pdmp3_read(d,out,4096,&done)==PDMP3_OK && done==4);
    /* Now the format switch is published,without PCM. */
    TASK_CHECK(pdmp3_read(d,out,4096,&done)==PDMP3_NEW_FORMAT && done==0);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK && rate==48000 && ch==1);
    TASK_CHECK(pdmp3_read(d,out,2304,&done)==PDMP3_OK && done==2304);
    TASK_CHECK(pdmp3_read(d,out,2304,&done)==PDMP3_OK && done==2304);
    TASK_CHECK(pdmp3_read(d,out,2304,&done)==PDMP3_NEED_MORE && done==0);
    pdmp3_delete(d);
    return 0;
}

static int same_format_header_changes(void) {
    static unsigned char in[32768],ref[81920],a[81920],b[81920];
    struct segdef s1={0,2,9,1,7};   /* 44100 stereo,all header params vary */
    struct segdef s2={0,2,6,1,23};  /* same format,different params again */
    struct result r1,r2,refa;
    size_t l1,l2;
    pdmp3_handle *d;
    l1=build_segment(in,&s1,NULL);
    l2=build_segment(in+l1,&s2,NULL);
    TASK_CHECK(l1>0 && l2>0);
    /* The first segment decoded alone is the reference for its prefix. */
    TASK_CHECK(decode_reference(in,l1,ref,sizeof(ref),&refa)==0);
    TASK_CHECK(refa.ev_count==1 && refa.ev_rate[0]==44100 && refa.ev_ch[0]==2);
    /* Concatenated with more frames of the same format: exactly one event,
     * no matter how bitrate,padding,CRC or stereo mode change. */
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(run_stream(d,in,l1+l2,512,8192,a,sizeof(a),&r1)==0);
    TASK_CHECK(r1.ev_count==1);
    TASK_CHECK(r1.ev_rate[0]==44100 && r1.ev_ch[0]==2);
    TASK_CHECK(r1.pcm_len==refa.pcm_len+6u*4608u);
    /* The first segment's PCM is untouched by the frames that follow. */
    TASK_CHECK(memcmp(a,ref,refa.pcm_len)==0);
    /* Chunking must not matter: one-byte feeds give the same stream. */
    pdmp3_delete(d);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(run_stream(d,in,l1+l2,1,4096,b,sizeof(b),&r2)==0);
    TASK_CHECK(r2.ev_count==1 && r2.pcm_len==r1.pcm_len);
    TASK_CHECK(memcmp(a,b,r1.pcm_len)==0);
    TASK_CHECK(pcm_energy(a,r1.pcm_len)>0);
    pdmp3_delete(d);
    return 0;
}

static int tail_frame(void) {
    unsigned char in[8192],ref[32768],pcm[32768],out[4608];
    struct segdef b={1,1,4,0,11}; /* 48000 Hz mono: 384-byte frames < 1152 */
    size_t offs[5],len,done=0,n=0;
    struct result r;
    long rate=0; int ch=0,enc=0,rc;
    pdmp3_handle *d;
    len=build_segment(in,&b,offs);
    TASK_CHECK(len>0 && offs[4]==len);
    TASK_CHECK(decode_reference(in,len,ref,sizeof(ref),&r)==0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    /* Feed all but the last frame,confirm the format,drain. */
    TASK_CHECK(pdmp3_feed(d,in,offs[3])==PDMP3_OK);
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEW_FORMAT && done==0);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK && rate==48000 && ch==1);
    while((rc=pdmp3_read(d,out,sizeof(out),&done))==PDMP3_OK) { memcpy(pcm+n,out,done); n+=done; }
    TASK_CHECK(rc==PDMP3_NEED_MORE && n==3*2304);
    /* Half of the tail frame: no output,no history change. */
    TASK_CHECK(pdmp3_feed(d,in+offs[3],(offs[4]-offs[3])/2)==PDMP3_OK);
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEED_MORE && done==0);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK && rate==48000 && ch==1);
    /* The complete tail frame is decoded promptly once it is whole. */
    TASK_CHECK(pdmp3_feed(d,in+offs[3]+(offs[4]-offs[3])/2,
                          (offs[4]-offs[3])-(offs[4]-offs[3])/2)==PDMP3_OK);
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_OK && done==2304);
    memcpy(pcm+n,out,done); n+=done;
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEED_MORE && done==0);
    TASK_CHECK(n==r.pcm_len && memcmp(pcm,ref,n)==0);
    pdmp3_delete(d);
    return 0;
}

static int no_space_need_more(void) {
    unsigned char in[8192],out[4608];
    struct segdef a={0,2,3,0,4};
    size_t len,done=55;
    long rate=0; int ch=0,enc=0;
    pdmp3_handle *d;
    len=build_segment(in,&a,NULL);
    TASK_CHECK(len>0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    /* Nothing deliverable and no input: NEED_MORE with done == 0. */
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEED_MORE && done==0);
    TASK_CHECK(pdmp3_feed(d,in,len)==PDMP3_OK);
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_NEW_FORMAT && done==0);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK && rate==44100 && ch==2);
    /* Less than one stereo sample frame (4 bytes) of output space. */
    TASK_CHECK(pdmp3_read(d,out,3,&done)==PDMP3_NO_SPACE && done==0);
    TASK_CHECK(pdmp3_read(d,out,3,&done)==PDMP3_NO_SPACE && done==0); /* no busy loop */
    TASK_CHECK(pdmp3_read(d,out,1,&done)==PDMP3_NO_SPACE && done==0);
    /* State was preserved: a proper buffer decodes the frame. */
    TASK_CHECK(pdmp3_read(d,out,sizeof(out),&done)==PDMP3_OK && done==4608);
    TASK_CHECK(pcm_energy(out,done)>0);
    pdmp3_delete(d);
    return 0;
}

/* ------------------------------------------------------------------- pcm */

static int chain_pcm(void) {
    struct result refs[3],run;
    size_t total=prepare_chain(refs);
    pdmp3_handle *d;
    size_t lr_diff=0;
    long el=0,er=0;
    TASK_CHECK(total>0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(run_stream(d,g_in,total,2048,16384,g_outpcm,sizeof(g_outpcm),&run)==0);
    TASK_CHECK(chain_run_matches(refs,&run,g_outpcm));
    /* The stereo reference is real two-channel sound: compare complete
     * left and right samples,both non-silent and not identical. */
    TASK_CHECK(refs[0].pcm_len%4==0);
    for(size_t i=0;i+3<refs[0].pcm_len;i+=4) {
        short l,r;
        memcpy(&l,g_refpcm[0]+i,2); memcpy(&r,g_refpcm[0]+i+2,2);
        el+=(l<0)?-(long)l:(long)l; er+=(r<0)?-(long)r:(long)r;
        if(l!=r) lr_diff++;
    }
    TASK_CHECK(el>0 && er>0);
    TASK_CHECK(lr_diff>0);
    pdmp3_delete(d);
    return 0;
}

static int one_byte_feed(void) {
    struct result refs[3],run;
    size_t total=prepare_chain(refs);
    pdmp3_handle *d;
    TASK_CHECK(total>0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(run_stream(d,g_in,total,1,8192,g_outpcm,sizeof(g_outpcm),&run)==0);
    TASK_CHECK(chain_run_matches(refs,&run,g_outpcm));
    pdmp3_delete(d);
    return 0;
}

static int ring_wrap_small_reads(void) {
    struct result refs[3],run;
    size_t total=prepare_chain(refs);
    pdmp3_handle *d;
    TASK_CHECK(total>0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    /* Odd chunk sizes: partial sample frames,wrapped ring pointers. */
    TASK_CHECK(run_stream(d,g_in,total,997,701,g_outpcm,sizeof(g_outpcm),&run)==0);
    TASK_CHECK(chain_run_matches(refs,&run,g_outpcm));
    pdmp3_delete(d);
    return 0;
}

static int decode_api(void) {
    struct result refs[3],run;
    size_t total=prepare_chain(refs),pos=0;
    unsigned char out[16384];
    pdmp3_handle *d;
    unsigned guard=0;
    TASK_CHECK(total>0);
    run.pcm_len=0; run.ev_count=0; run.status=PDMP3_OK;
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    for(;;) {
        size_t done=0,n;
        int rc;
        TASK_CHECK(++guard<100000);
        if(pos<total) {
            n=total-pos; if(n>700) n=700;
            rc=pdmp3_decode(d,g_in+pos,n,out,sizeof(out),&done);
            pos+=n;
        } else {
            rc=pdmp3_decode(d,NULL,0,out,sizeof(out),&done); /* zero input drains */
        }
        if(rc==PDMP3_NEW_FORMAT) {
            long rate=0; int ch=0,enc=0;
            TASK_CHECK(done==0);
            TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_OK);
            TASK_CHECK(run.ev_count<MAX_EVENTS);
            run.ev_rate[run.ev_count]=rate; run.ev_ch[run.ev_count]=ch; run.ev_count++;
            continue;
        }
        if(rc==PDMP3_OK) { memcpy(g_outpcm+run.pcm_len,out,done); run.pcm_len+=done; continue; }
        if(rc==PDMP3_NEED_MORE) {
            TASK_CHECK(done==0);
            if(pos>=total) break;
            continue;
        }
        TASK_CHECK(0); /* unexpected status */
    }
    TASK_CHECK(chain_run_matches(refs,&run,g_outpcm));
    pdmp3_delete(d);
    return 0;
}

static int back_reference_pcm(void) {
    static unsigned char in[32768],a[65536],b[65536];
    struct segdef s={2,2,9,1,13}; /* 32000 stereo with main_data_begin > 0 */
    struct result r1,r2;
    size_t len;
    pdmp3_handle *d;
    len=build_segment(in,&s,NULL);
    TASK_CHECK(len>0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(run_stream(d,in,len,len,16384,a,sizeof(a),&r1)==0);
    TASK_CHECK(r1.ev_count==1 && r1.ev_rate[0]==32000 && r1.ev_ch[0]==2);
    /* Every frame decoded: the bit reservoir survived between frames. */
    TASK_CHECK(r1.pcm_len==9u*4608u);
    TASK_CHECK(pcm_energy(a,r1.pcm_len)>0);
    pdmp3_delete(d);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(run_stream(d,in,len,1,517,b,sizeof(b),&r2)==0);
    TASK_CHECK(r2.pcm_len==r1.pcm_len && memcmp(a,b,r1.pcm_len)==0);
    pdmp3_delete(d);
    return 0;
}

/* ------------------------------------------------------------- lifecycle */

static int open_feed_reuse(void) {
    struct result refs[3],run1,run2;
    size_t total=prepare_chain(refs);
    long rate=-1; int ch=-1,enc=-1;
    pdmp3_handle *d;
    TASK_CHECK(total>0);
    d=pdmp3_new(NULL,NULL);
    TASK_CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(run_stream(d,g_in,total,4096,16384,g_outpcm,sizeof(g_outpcm),&run1)==0);
    TASK_CHECK(chain_run_matches(refs,&run1,g_outpcm));
    /* Reopening starts a fresh stream on the same handle. */
    TASK_CHECK(pdmp3_open_feed(d)==PDMP3_OK);
    TASK_CHECK(pdmp3_getformat(d,&rate,&ch,&enc)==PDMP3_NEED_MORE);
    TASK_CHECK(rate==-1 && ch==-1 && enc==-1);
    TASK_CHECK(run_stream(d,g_in,total,4096,16384,g_outpcm,sizeof(g_outpcm),&run2)==0);
    TASK_CHECK(chain_run_matches(refs,&run2,g_outpcm));
    TASK_CHECK(run2.ev_count==run1.ev_count && run2.pcm_len==run1.pcm_len);
    pdmp3_delete(d);
    return 0;
}

/* Pump one handle once: feed a chunk and read once. Returns 0 while work
 * remains,1 when the stream is drained,< 0 on protocol violations. */
static int pump(pdmp3_handle *d,const unsigned char *in,size_t inlen,size_t *pos,
                size_t feed_n,unsigned char *pcm,size_t cap,struct result *r) {
    unsigned char out[2048];
    size_t done=0;
    int rc;
    if(*pos<inlen) {
        size_t n=inlen-*pos;
        if(n>feed_n) n=feed_n;
        if(pdmp3_feed(d,in+*pos,n)!=PDMP3_OK) return -1;
        *pos+=n;
    }
    rc=pdmp3_read(d,out,sizeof(out),&done);
    if(rc==PDMP3_NEW_FORMAT) {
        long rate=0; int ch=0,enc=0;
        if(done!=0) return -2;
        if(pdmp3_getformat(d,&rate,&ch,&enc)!=PDMP3_OK) return -3;
        if(r->ev_count>=MAX_EVENTS) return -4;
        r->ev_rate[r->ev_count]=rate; r->ev_ch[r->ev_count]=ch; r->ev_count++;
        return 0;
    }
    if(rc==PDMP3_OK) {
        if(r->pcm_len+done>cap) return -5;
        memcpy(pcm+r->pcm_len,out,done); r->pcm_len+=done;
        return 0;
    }
    if(rc==PDMP3_NEED_MORE) return (*pos<inlen)?0:1;
    return -6;
}

static int dual_instances(void) {
    static unsigned char in1[32768],in2[16384],in3[16384];
    static unsigned char pcm1[65536],pcm2[65536],pcm3[65536];
    struct segdef a={0,2,6,1,2},b={1,1,6,1,18},c={2,2,5,1,34};
    struct result r1,r2,r3,refa,refb,refc;
    static unsigned char ra[65536],rb[65536],rc_[65536];
    size_t l1a,l1b,l2,l3,p1=0,p2=0,p3=0;
    int done1=0,done2=0,done3=0;
    pdmp3_handle *h1,*h2,*h3;
    l1a=build_segment(in1,&a,NULL);
    l1b=build_segment(in1+l1a,&b,NULL);
    l2=build_segment(in2,&c,NULL);
    l3=build_segment(in3,&a,NULL);
    TASK_CHECK(l1a>0 && l1b>0 && l2>0 && l3>0);
    TASK_CHECK(decode_reference(in1,l1a,ra,sizeof(ra),&refa)==0);
    TASK_CHECK(decode_reference(in1+l1a,l1b,rb,sizeof(rb),&refb)==0);
    TASK_CHECK(decode_reference(in2,l2,rc_,sizeof(rc_),&refc)==0);
    h1=pdmp3_new(NULL,NULL); h2=pdmp3_new(NULL,NULL);
    TASK_CHECK(h1 && h2);
    TASK_CHECK(pdmp3_open_feed(h1)==PDMP3_OK && pdmp3_open_feed(h2)==PDMP3_OK);
    r1.pcm_len=0; r1.ev_count=0; r2.pcm_len=0; r2.ev_count=0;
    /* Interleave both streams until the first handle is drained. */
    while(!done1 || !done2) {
        int rc;
        if(!done1) { rc=pump(h1,in1,l1a+l1b,&p1,300,pcm1,sizeof(pcm1),&r1); TASK_CHECK(rc>=0); done1=(rc==1); }
        if(!done2) { rc=pump(h2,in2,l2,&p2,300,pcm2,sizeof(pcm2),&r2); TASK_CHECK(rc>=0); done2=(rc==1); }
        if(done1 && !done2) break; /* h1 finished first: exercise delete now */
    }
    /* Deleting a finished handle must not disturb the other one. */
    pdmp3_delete(h1);
    /* A third handle,reusing nothing,decodes segment A while h2 continues. */
    h3=pdmp3_new(NULL,NULL);
    TASK_CHECK(h3 && pdmp3_open_feed(h3)==PDMP3_OK);
    r3.pcm_len=0; r3.ev_count=0;
    while(!done2 || !done3) {
        int rc;
        if(!done2) { rc=pump(h2,in2,l2,&p2,300,pcm2,sizeof(pcm2),&r2); TASK_CHECK(rc>=0); done2=(rc==1); }
        if(!done3) { rc=pump(h3,in3,l3,&p3,300,pcm3,sizeof(pcm3),&r3); TASK_CHECK(rc>=0); done3=(rc==1); }
    }
    /* Each stream matches its standalone reference. */
    TASK_CHECK(r1.ev_count==2 && r1.ev_rate[0]==44100 && r1.ev_ch[0]==2);
    TASK_CHECK(r1.ev_rate[1]==48000 && r1.ev_ch[1]==1);
    TASK_CHECK(r1.pcm_len==refa.pcm_len+refb.pcm_len);
    TASK_CHECK(memcmp(pcm1,ra,refa.pcm_len)==0);
    TASK_CHECK(memcmp(pcm1+refa.pcm_len,rb,refb.pcm_len)==0);
    TASK_CHECK(r2.ev_count==1 && r2.ev_rate[0]==32000 && r2.ev_ch[0]==2);
    TASK_CHECK(r2.pcm_len==refc.pcm_len && memcmp(pcm2,rc_,r2.pcm_len)==0);
    TASK_CHECK(r3.ev_count==1 && r3.pcm_len==refa.pcm_len);
    TASK_CHECK(memcmp(pcm3,ra,r3.pcm_len)==0);
    /* Reopening h2 mid-life must not disturb h3's handle either. */
    TASK_CHECK(pdmp3_open_feed(h2)==PDMP3_OK);
    r2.pcm_len=0; r2.ev_count=0; p2=0;
    done2=0;
    while(!done2) {
        int rc=pump(h2,in2,l2,&p2,512,pcm2,sizeof(pcm2),&r2);
        TASK_CHECK(rc>=0);
        done2=(rc==1);
    }
    TASK_CHECK(r2.pcm_len==refc.pcm_len && memcmp(pcm2,rc_,r2.pcm_len)==0);
    pdmp3_delete(h2);
    pdmp3_delete(h3);
    return 0;
}

const struct task_case task_cases[] = {
    {"events","chain_events",chain_events},
    {"events","first_notification",first_notification},
    {"events","unconfirmed_repeat",unconfirmed_repeat},
    {"events","deferred_notice_one_sample",deferred_notice_one_sample},
    {"events","same_format_header_changes",same_format_header_changes},
    {"events","tail_frame",tail_frame},
    {"events","no_space_need_more",no_space_need_more},
    {"pcm","chain_pcm",chain_pcm},
    {"pcm","one_byte_feed",one_byte_feed},
    {"pcm","ring_wrap_small_reads",ring_wrap_small_reads},
    {"pcm","decode_api",decode_api},
    {"pcm","back_reference_pcm",back_reference_pcm},
    {"lifecycle","open_feed_reuse",open_feed_reuse},
    {"lifecycle","dual_instances",dual_instances},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
