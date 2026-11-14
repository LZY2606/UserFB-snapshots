#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "bcg729/encoder.h"
#include "bcg729/decoder.h"
int main(void){
 bcg729EncoderChannelContextStruct *e=initBcg729EncoderChannel(0);
 bcg729DecoderChannelContextStruct *d=initBcg729DecoderChannel();
 int16_t in[80],out[80];uint8_t packet[10],len;unsigned nonzero=0,bytes=0;uint32_t hash=2166136261u;
 assert(e&&d);
 for(int f=0;f<24;f++){
  for(int i=0;i<80;i++)in[i]=(int16_t)(((i+f*80)%47<23?5000:-4200)+((i*31+f*17)%101));
  bcg729Encoder(e,in,packet,&len);assert(len==10);bytes+=len;
  bcg729Decoder(d,packet,len,0,0,0,out);
  for(int i=0;i<80;i++){nonzero+=out[i]!=0;hash=(hash^(uint16_t)out[i])*16777619u;}
 }
 assert(bytes==240&&nonzero>1600);
 printf("BCG729_BASELINE_OK frames=24 bytes=%u nonzero=%u pcm=%08x\n",bytes,nonzero,hash);
 closeBcg729EncoderChannel(e);closeBcg729DecoderChannel(d);return 0;
}
