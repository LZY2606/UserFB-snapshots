#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "gsm.h"
static uint32_t mix(uint32_t h,unsigned v){return (h^(v&255))*16777619u;}
int main(void){
 gsm e=gsm_create(),d=gsm_create();gsm_signal in[160],out[160];gsm_frame f;
 uint32_t enc=2166136261u,dec=enc,rng=7;int nonzero=0;
 assert(e&&d);
 for(int frame=0;frame<12;frame++){
  for(int i=0;i<160;i++){
   rng=rng*1664525u+1013904223u;
   in[i]=(gsm_signal)((int)(rng>>16)-32768);
   if(frame==0) in[i]=0;
   if(frame==1) in[i]=i%2?32767:-32768;
  }
  gsm_encode(e,in,f);assert((f[0]>>4)==0xd);
  assert(gsm_decode(d,f,out)==0);
  for(int i=0;i<33;i++)enc=mix(enc,f[i]);
  for(int i=0;i<160;i++){
   dec=mix(mix(dec,(uint16_t)out[i]),(uint16_t)out[i]>>8);nonzero+=out[i]!=0;
  }
 }
 assert(enc==0x1727e7c8u&&dec==0x7fc5a90au&&nonzero==1918);gsm_destroy(e);gsm_destroy(d);
 printf("GSM_BASELINE_OK frames=12 encoded=%08x pcm=%08x nonzero=%d\n",enc,dec,nonzero);
}
