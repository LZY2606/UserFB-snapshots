#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "neaacdec.h"
static void need(int yes,const char*s){if(!yes){fprintf(stderr,"BASELINE_FAIL %s\n",s);exit(2);}}
int main(void){
 FILE*f=fopen("task_support/normal.aac","rb");need(f!=0,"file");
 need(fseek(f,0,SEEK_END)==0,"seek");long length=ftell(f);need(length>7,"length");rewind(f);
 unsigned char*data=malloc((size_t)length);need(data!=0,"malloc");need(fread(data,1,(size_t)length,f)==(size_t)length,"read");fclose(f);
 NeAACDecHandle h=NeAACDecOpen();need(h!=0,"open");
 NeAACDecConfigurationPtr c=NeAACDecGetCurrentConfiguration(h);c->outputFormat=FAAD_FMT_16BIT;c->dontUpSampleImplicitSBR=1;need(NeAACDecSetConfiguration(h,c),"config");
 unsigned char asc[2]={0x11,0x90},channels=0;unsigned long rate=0;
 need(NeAACDecInit2(h,asc,2,&rate,&channels)==0,"init2");need(rate==48000 && channels==2,"format");
 size_t at=0;unsigned frames=0;unsigned long samples=0;uint32_t hash=2166136261u;unsigned nonzero=0;
 while(at<(size_t)length){
  need((size_t)length-at>=7,"header");unsigned char*b=data+at;
  unsigned len=((unsigned)(b[3]&3)<<11)|((unsigned)b[4]<<3)|(b[5]>>5);
  need(b[0]==255 && (b[1]&246)==240 && (b[1]&1) && len>7 && len<=(size_t)length-at,"adts");
  NeAACDecFrameInfo info={0}; int16_t*pcm=NeAACDecDecode(h,&info,b+7,len-7);
  need(info.error==0,"decode");need(info.bytesconsumed==len-7,"consumed");need(info.channels==2 && info.samplerate==48000,"decoded_format");
  need(info.samples==(frames?2048u:0u),"priming");
  for(unsigned long i=0;i<info.samples;i++){uint16_t v=(uint16_t)pcm[i];if(v)nonzero++;hash=(hash^(v&255))*16777619u;hash=(hash^(v>>8))*16777619u;}
  samples+=info.samples;frames++;at+=len;
 }
 need(frames==24 && samples==47104 && nonzero>0,"content");
 NeAACDecClose(h);free(data);
 printf("FAAD_BASELINE frames=%u samples=%lu nonzero=%u pcm_fnv=%08x\n",frames,samples,nonzero,hash);return 0;
}
