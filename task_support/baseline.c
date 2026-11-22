#include "ltc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define MUST(x) do { if (!(x)) { fprintf(stderr,"baseline:%d %s\n",__LINE__,#x); return 1; } } while(0)
static uint32_t hash(const unsigned char *p,size_t n) { uint32_t h=2166136261u; while(n--) h=(h^*p++)*16777619u; return h; }
int main(void) {
    unsigned char audio[30000]; size_t total=0; int i,decoded=0;
    LTCEncoder *e=ltc_encoder_create(48000,25,LTC_TV_625_50,0);
    LTCDecoder *d=ltc_decoder_create(1920,16); LTCFrameExt f;
    MUST(e && d); ltc_encoder_set_filter(e,0);
    for(i=0;i<6;i++) { ltc_encoder_encode_frame(e); int n=ltc_encoder_copy_buffer(e,audio+total); MUST(n==1920);total+=(size_t)n; ltc_encoder_inc_timecode(e); }
    ltc_encoder_end_encode(e); total+=(size_t)ltc_encoder_copy_buffer(e,audio+total);
    ltc_decoder_write(d,audio,total,0);
    while(ltc_decoder_read(d,&f)) { MUST(f.ltc.frame_units==(unsigned)decoded); decoded++; }
    MUST(decoded==6);
    printf("LEGACY_PCM frames=%d samples=%zu hash=%08x\n",decoded,total,hash(audio,total));
    ltc_decoder_free(d); ltc_encoder_free(e);

    /* Independent square-wave source: eight explicit canonical labels,
       variable bit widths, and labels with even bit parity. */
    total=0; int level=0; size_t starts[8];
    const int widths[4]={22,24,26,24};
    for(i=0;i<8;i++) {
        unsigned char raw[10]={0xa0,0x50,0x40,0x30,0x20,0x10,0x00,0xb0,0xfc,0xbf};
        raw[0]|=(unsigned char)i;
        unsigned ones=0; int bit;
        for(bit=0;bit<80;bit++) ones+=(raw[bit/8]>>(bit%8))&1;
        raw[3]|=(unsigned char)((ones&1)<<3);
        starts[i]=total;
        for(bit=0;bit<80;bit++) {
            int w=widths[bit%4], one=(raw[bit/8]>>(bit%8))&1, j;
            level=!level;
            for(j=0;j<w;j++) { if(one && j==w/2) level=!level; audio[total++]=(unsigned char)(level?218:38); }
        }
    }
    /* One extra bit boundary permits the last complete tag to finish. */
    level=!level; for(i=0;i<26;i++) audio[total++]=(unsigned char)(level?218:38);
    d=ltc_decoder_create(1920,16); MUST(d); const ltc_off_t origin=((ltc_off_t)1<<33)+317;
    for(size_t at=0;at<total;) { size_t n=317; if(n>total-at)n=total-at; ltc_decoder_write(d,audio+at,n,origin+(ltc_off_t)at); at+=n; }
    decoded=0; double max_grid_error=0;
    while(ltc_decoder_read(d,&f)) {
        unsigned tag=f.ltc.frame_units; MUST(tag<8); MUST(f.ltc.user1==10 && f.ltc.user8==11); MUST(!f.reverse);
        if(tag>0 && tag<7) {
            double estimate=(double)f.off_start;
            size_t exact=starts[tag];
            for(int b=0;b<80;b++) { double err=fabs(estimate-(double)(origin+(ltc_off_t)exact)); if(err>max_grid_error)max_grid_error=err; estimate+=f.biphase_tics[b];exact+=(size_t)widths[b%4]; }
        }
        decoded++;
    }
    MUST(decoded==8);
    printf("MEASURED_SOURCE frames=%d origin=%lld samples=%zu hash=%08x\n",decoded,(long long)origin,total,hash(audio,total));
    printf("OBSERVATION smoothed_grid_max_error=%.6f\n",max_grid_error);
    puts("LTC_CLOCK_NORMAL_BASELINE_OK"); ltc_decoder_free(d); return 0;
}
