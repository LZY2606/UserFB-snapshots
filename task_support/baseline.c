#include "lame.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
static uint32_t rng=0x12345678u;
static int inspect(const unsigned char *b,int n,int sr){
 static const int rate[16]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
 int p=0,frames=0,total=0,distinct=0,seen[16]={0},borrow=0;
 while(p<n){
  if(n-p<4 || b[p]!=255 || (b[p+1]&0xfe)!=0xfa) return 1;
  int ix=b[p+2]>>4, si=(b[p+2]>>2)&3;
  int rates[3]={44100,48000,32000};
  if(ix<1||ix>14||si>2||rates[si]!=sr) return 2;
  int len=144000*rate[ix]/sr+((b[p+2]>>1)&1);
  if(len>n-p||len<36) return 3;
  int off=p+4+((b[p+1]&1)?0:2);
  if((b[off]<<1 | b[off+1]>>7)>0) borrow++;
  if(!seen[ix]++) distinct++;
  p+=len;total+=len;frames++;
 }
 double kbps=(double)total*8*sr/(frames*1152.0*1000);
 printf("BASELINE_AUDIO sr=%d frames=%d bytes=%d actual_kbps=%.3f distinct_rates=%d reservoir_frames=%d\n",sr,frames,total,kbps,distinct,borrow);
 return frames<100||distinct<2||borrow==0;
}
int main(void){
 lame_t g=lame_init();if(!g)return 1;
 lame_set_in_samplerate(g,44100);lame_set_out_samplerate(g,44100);lame_set_num_channels(g,2);
 lame_set_VBR(g,vbr_abr);lame_set_VBR_mean_bitrate_kbps(g,96);lame_set_bWriteVbrTag(g,0);lame_set_quality(g,2);
 if(lame_init_params(g)<0)return 2;
 unsigned char *all=malloc(4000000);int used=0;short pcm[2304];
 for(int f=0;f<300;f++){
  for(int j=0;j<2304;j++){
   rng=1664525u*rng+1013904223u;
   double tone=6000*sin((f*1152+j/2)*2*3.141592653589793*997/44100.0);
   pcm[j]=(short)(f%60<30 ? tone : ((int)(rng>>18)-8192));
  }
  int n=lame_encode_buffer_interleaved(g,pcm,1152,all+used,4000000-used);if(n<0)return 3;used+=n;
 }
 int n=lame_encode_flush(g,all+used,4000000-used);if(n<0)return 4;used+=n;
 int e=inspect(all,used,44100);
 hip_t h=hip_decode_init();short l[1152],r[1152];int decoded=0,nonzero=0;
 for(int p=0;p<used;p+=137){int len=used-p;if(len>137)len=137;int got=hip_decode1(h,all+p,len,l,r);if(got<0)return 5;decoded+=got;for(int i=0;i<got;i++)nonzero|=l[i]!=0||r[i]!=0;}
 for(int k=0;k<500;k++){int got=hip_decode1(h,all,0,l,r);if(got<0)return 6;if(!got)break;decoded+=got;for(int i=0;i<got;i++)nonzero|=l[i]!=0||r[i]!=0;}
 hip_decode_exit(h);lame_close(g);free(all);
 printf("BASELINE_DECODE samples=%d nonzero=%d\n",decoded,nonzero);
 if(e||decoded<300*1152||!nonzero)return 7;
 puts("BASELINE_OK");return 0;
}
