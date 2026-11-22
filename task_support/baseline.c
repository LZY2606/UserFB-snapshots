#include "lame.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define SAMPLES (80*1152+137)
#define CAP (1024*1024)
struct frame { int off, len, side, mdb, p23, padding, capacity; long c; };
static unsigned bits(const unsigned char *p, int at, int n) {
    unsigned v=0; int j; for(j=0;j<n;j++) v=(v<<1)|((p[(at+j)/8]>>(7-(at+j)%8))&1); return v;
}
static int parse(const unsigned char *data,int size,struct frame *f) {
    static const int br[2][16]={{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},{0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0}};
    static const int sr[3]={44100,48000,32000};
    int off=0,n=0; long c=0;
    while(off<size) {
        const unsigned char *h=data+off,*s; int ver,rate,ch,crc,gr,j,at,len,p=0;
        REQUIRE(size-off>=4); REQUIRE(h[0]==255 && (h[1]&0xe0)==0xe0 && ((h[1]>>1)&3)==1);
        ver=((h[1]>>3)&3)==3; REQUIRE(((h[1]>>3)&3)==3 || ((h[1]>>3)&3)==2);
        rate=sr[(h[2]>>2)&3]/(ver?1:2); ch=(h[3]>>6)==3?1:2; crc=(h[1]&1)?0:2;
        len=(ver?144000:72000)*br[ver][h[2]>>4]/rate+((h[2]>>1)&1);
        REQUIRE(len>0 && off+len<=size && n<200);
        f[n].off=off; f[n].len=len; f[n].side=ver?(ch==1?17:32):(ch==1?9:17);
        f[n].capacity=len-4-crc-f[n].side; f[n].c=c; f[n].padding=(h[2]>>1)&1;
        s=h+4+crc; f[n].mdb=bits(s,0,ver?9:8);
        at=ver?(9+(ch==1?5:3)+4*ch):(8+(ch==1?1:2));
        for(gr=0;gr<(ver?2:1);gr++) for(j=0;j<ch;j++) { p+=(int)bits(s,at,12); at+=ver?59:63; }
        f[n].p23=p; REQUIRE(c>=f[n].mdb); REQUIRE(8*(c-f[n].mdb)+p<=8*(c+f[n].capacity));
        c+=f[n].capacity; off+=len; n++;
    }
    REQUIRE(off==size); return n;
}
static void quiet(const char *fmt,va_list ap) { (void)fmt;(void)ap; }
static long decode(unsigned char *data,int size,unsigned long *hash,long *nonzero) {
    hip_t h=hip_decode_init(); short l[1152],r[1152]; int off=0,n=0,guard=0; long total=0;
    REQUIRE(h!=NULL); hip_set_errorf(h,quiet); hip_set_debugf(h,quiet); hip_set_msgf(h,quiet);
    *hash=2166136261u; *nonzero=0;
    do {
        int len=off<size?(size-off>73?73:size-off):0, i;
        n=hip_decode1(h,data+off,(size_t)len,l,r); off+=len;
        if(n<0) { hip_decode_exit(h); return -1; }
        for(i=0;i<n;i++) { *hash=(*hash^(unsigned short)l[i])*16777619u; *hash=(*hash^(unsigned short)r[i])*16777619u; if(l[i]||r[i])(*nonzero)++; }
        total+=n; REQUIRE(++guard<100000);
    } while(off<size || n>0);
    hip_decode_exit(h); return total;
}
static void scenario(int rate,int channels,int crc) {
    short *l=calloc(SAMPLES,sizeof(*l)),*r=calloc(SAMPLES,sizeof(*r));
    unsigned char *data=malloc(CAP),*changed=malloc(CAP); struct frame f[200];
    lame_t g=lame_init(); uint32_t rng=0x12345678; int j,k,used=0,nframes,borrow=0,realborrow=0,pad[2]={0,0},slot=-1,owner=-1;
    long count,nz,count2,nz2; unsigned long hash,hash2;
    REQUIRE(l&&r&&data&&changed&&g);
    for(j=0;j<SAMPLES;j++) { if((j/1152)%40>=8 && (j/1152)%40<32) { rng=1664525u*rng+1013904223u;l[j]=(short)((rng>>18)-8192);rng=1664525u*rng+1013904223u;r[j]=(short)((rng>>18)-8192); } }
    REQUIRE(lame_set_in_samplerate(g,rate)==0); REQUIRE(lame_set_out_samplerate(g,rate)==0);
    REQUIRE(lame_set_num_channels(g,channels)==0); REQUIRE(lame_set_mode(g,channels==1?MONO:STEREO)==0);
    REQUIRE(lame_set_brate(g,128)==0); REQUIRE(lame_set_quality(g,2)==0); REQUIRE(lame_set_VBR(g,vbr_off)==0);
    REQUIRE(lame_set_bWriteVbrTag(g,0)==0); REQUIRE(lame_set_error_protection(g,crc)==0); lame_set_write_id3tag_automatic(g,0);
    REQUIRE(lame_init_params(g)==0);
    for(j=0;j<SAMPLES;) { int len=1+(j%2049),ret; if(len>SAMPLES-j)len=SAMPLES-j;
        ret=lame_encode_buffer(g,l+j,r+j,len,data+used,CAP-used); REQUIRE(ret>=0);used+=ret;j+=len; }
    k=lame_encode_flush(g,data+used,CAP-used); REQUIRE(k>=0);used+=k;
    REQUIRE(lame_encode_flush(g,data+used,CAP-used)==0); lame_close(g);
    nframes=parse(data,used,f);
    for(j=0;j<nframes;j++) {
        borrow+=f[j].mdb>0; realborrow+=f[j].p23>8*f[j].capacity; pad[f[j].padding]++;
        if(j) REQUIRE(8*(f[j].c-f[j].mdb)>=8*(f[j-1].c-f[j-1].mdb)+f[j-1].p23);
        for(k=j;k<nframes && slot<0;k++) {
            long a=8*(f[k].c-f[k].mdb),b=a+f[k].p23,tail=8*(f[j].c+f[j].capacity-32),end=tail+256;
            if(a<end && b>tail) { slot=j;owner=k; }
        }
    }
    REQUIRE(borrow>0 && realborrow>0 && slot>=0);
    count=decode(data,used,&hash,&nz); REQUIRE(nz>0 && count==nframes*(rate>=32000?1152:576));
    memcpy(changed,data,(size_t)used); memset(changed+f[slot].off+f[slot].len-32,0xa5,32);
    count2=decode(changed,used,&hash2,&nz2); REQUIRE(count2!=count || hash2!=hash);
    printf("LAME_BASE rate=%d channels=%d crc=%d frames=%d borrowed=%d over_own=%d pad0=%d pad1=%d pcm=%ld nonzero=%ld\n",rate,channels,crc,nframes,borrow,realborrow,pad[0],pad[1],count,nz);
    printf("LAME_TAIL_OCCUPIED physical_frame=%d audio_owner=%d bytes=32 changed_pcm=1\n",slot,owner);
    free(l);free(r);free(data);free(changed);
}
int main(void) {
    scenario(44100,2,0); scenario(48000,2,1); scenario(24000,1,0);
    puts("LAME_BASELINE_OK"); return 0;
}
