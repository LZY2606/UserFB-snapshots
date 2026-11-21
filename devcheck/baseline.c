#include "layer3.h"
#include "vendor/minimp3/minimp3.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int16_t value(size_t i){return (int16_t)(((int)(i%97)-48)*220);}
static size_t encode(int planar,unsigned char*out){
 shine_config_t cfg;memset(&cfg,0,sizeof(cfg));shine_set_config_mpeg_defaults(&cfg.mpeg);
 cfg.wave.channels=2;cfg.wave.samplerate=48000;cfg.mpeg.mode=JOINT_STEREO;cfg.mpeg.bitr=128;
 REQUIRE(shine_check_config(48000,128)>=0);shine_t s=shine_initialise(&cfg);REQUIRE(s);int n=shine_samples_per_pass(s);REQUIRE(n==1152);
 int16_t left[SHINE_MAX_SAMPLES],right[SHINE_MAX_SAMPLES],inter[2*SHINE_MAX_SAMPLES];size_t used=0;
 for(int frame=0;frame<12;frame++){
  for(int j=0;j<n;j++){left[j]=value((size_t)frame*n+j);right[j]=(frame<6?left[j]:-left[j]);inter[2*j]=left[j];inter[2*j+1]=right[j];}
  int written=0;int16_t*channels[2]={left,right};unsigned char*p=planar?shine_encode_buffer(s,channels,&written):shine_encode_buffer_interleaved(s,inter,&written);
  REQUIRE(written>=0&&used+(size_t)written<131072);if(written){REQUIRE(p);memcpy(out+used,p,written);used+=written;}
 }
 int written=0;unsigned char*p=shine_flush(s,&written);REQUIRE(written>=0&&used+(size_t)written<131072);if(written){REQUIRE(p);memcpy(out+used,p,written);used+=written;}shine_close(s);return used;
}
int main(void){
 unsigned char *a=malloc(131072),*b=malloc(131072);REQUIRE(a&&b);size_t na=encode(1,a),nb=encode(0,b);REQUIRE(na==nb&&na>0&&!memcmp(a,b,na));
 size_t at=0,frames=0;while(at<na){REQUIRE(na-at>=4);const unsigned char*h=a+at;REQUIRE(h[0]==255&&(h[1]&0xfe)==0xfa);REQUIRE((h[2]>>4)==9&&((h[2]>>2)&3)==1);REQUIRE((h[3]>>6)==JOINT_STEREO&&((h[3]>>4)&3)==0);size_t len=384+((h[2]>>1)&1);REQUIRE(at+len<=na);at+=len;frames++;}REQUIRE(frames==12);
 mp3dec_t d;mp3dec_init(&d);short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];at=0;size_t scalars=0;unsigned long long energy=0;
 while(at<na){mp3dec_frame_info_t info={0};int samples=mp3dec_decode_frame(&d,a+at,(int)(na-at),pcm,&info);REQUIRE(info.frame_bytes>0&&info.channels==2&&samples==1152);for(int j=0;j<samples*2;j++)energy+=(unsigned long long)abs((int)pcm[j]);scalars+=(size_t)samples*2;at+=(size_t)info.frame_bytes;}
 REQUIRE(scalars==27648&&energy>0);uint64_t hash=UINT64_C(14695981039346656037);for(size_t i=0;i<na;i++){hash^=a[i];hash*=UINT64_C(1099511628211);}
 printf("LEGACY_BYTES_FNV=%016llx\n",(unsigned long long)hash);
 printf("SHINE_BASELINE_OK frames=%zu bytes=%zu planar_equal=1 joint_mode_ext=0 decoded_scalars=%zu nonzero=1\n",frames,na,scalars);free(a);free(b);return 0;
}
