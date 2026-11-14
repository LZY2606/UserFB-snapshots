#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ilbc/interface/ilbc.h"
#include "ilbc/defines.h"
#include "ilbc/unpack_bits.h"
#include "ilbc/swap_bytes.h"
static uint64_t add(uint64_t h, const int16_t *x, int n) {
  for (int i=0;i<n;i++) {uint16_t u=(uint16_t)x[i]; h=(h^(u&255))*UINT64_C(1099511628211); h=(h^(u>>8))*UINT64_C(1099511628211);}return h;
}
int main(void) {
  for(int mode=20;mode<=30;mode+=10){
    iLBC_encinst_t *e=NULL; iLBC_decinst_t *d=NULL;
    if(WebRtcIlbcfix_EncoderCreate(&e)||WebRtcIlbcfix_DecoderCreate(&d))return 2;
    if(WebRtcIlbcfix_EncoderInit(e,mode)||WebRtcIlbcfix_DecoderInit(d,mode))return 3;
    uint64_t h=UINT64_C(14695981039346656037),p=h; uint32_t rng=7;
    unsigned starts[6]={0},first[2]={0}; int samples=mode*8,bytes=(mode==20?38:50),nz=0;
    for(int f=0;f<96;f++){
      int16_t in[240],out[240],bits[25],type=-1;uint16_t swapped[25];iLBC_bits fields;
      int peak=f%(samples/40);
      for(int i=0;i<samples;i++){
        rng=rng*1664525U+1013904223U;
        int distance=i/40-peak;if(distance<0)distance=-distance;
        int v=((int)(rng>>16)-32768)/8 + (((f*samples+i)%37)-18)*180;
        in[i]=(int16_t)(v/(1+distance*3));
      }
      int len=WebRtcIlbcfix_Encode(e,in,(int16_t)samples,bits);if(len!=bytes)return 4;
      h=add(h,bits,len/2);
      WebRtcIlbcfix_SwapBytes((uint16_t*)bits,(int16_t)(len/2),swapped);
      if(WebRtcIlbcfix_UnpackBits(swapped,&fields,(int16_t)mode))return 5;
      if(fields.startIdx<1||fields.startIdx>samples/40-1||fields.state_first<0||fields.state_first>1)return 6;
      starts[fields.startIdx]++;first[fields.state_first]++;
      len=WebRtcIlbcfix_Decode(d,bits,(int16_t)bytes,out,&type);if(len!=samples||type!=1)return 7;
      p=add(p,out,len);for(int i=0;i<len;i++)nz+=out[i]!=0;
    }
    if(!nz||!first[0]||!first[1])return 8;
    for(int i=1;i<samples/40;i++)if(!starts[i])return 9;
    printf("ILBC_BASELINE_OK mode=%d frames=96 bytes=%d samples=%d stream=%016llx pcm=%016llx state_first=%u/%u starts=",mode,bytes*96,samples*96,(unsigned long long)h,(unsigned long long)p,first[0],first[1]);
    for(int i=1;i<samples/40;i++)printf("%s%u",i==1?"":",",starts[i]);puts("");
    WebRtcIlbcfix_EncoderFree(e);WebRtcIlbcfix_DecoderFree(d);
  }
  return 0;
}
