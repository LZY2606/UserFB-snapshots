#include "neaacdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
static unsigned char *readall(const char *path,size_t *size){
 FILE*f=fopen(path,"rb");if(!f)exit(2);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 if(n<=0)exit(3);unsigned char*b=malloc((size_t)n+16);if(!b)exit(4);
 if(fread(b,1,(size_t)n,f)!=(size_t)n)exit(5);fclose(f);*size=(size_t)n;return b;
}
int main(void){
 size_t n;unsigned char*b=readall("task_support/tone.aac",&n);unsigned char asc[]={0x12,0x08};
 NeAACDecHandle h=NeAACDecOpen();unsigned long rate=0;unsigned char ch=0;
 NeAACDecConfigurationPtr cfg=NeAACDecGetCurrentConfiguration(h);cfg->outputFormat=FAAD_FMT_FLOAT;cfg->dontUpSampleImplicitSBR=1;
 if(!NeAACDecSetConfiguration(h,cfg)||NeAACDecInit2(h,asc,2,&rate,&ch)||rate!=44100||ch!=1)return 6;
 size_t p=0,frames=0;unsigned long total=0;double energy=0;
 while(p<n){
  if(n-p<7||b[p]!=255||(b[p+1]&0xf6)!=0xf0)return 7;
  size_t len=((size_t)(b[p+3]&3)<<11)|((size_t)b[p+4]<<3)|(b[p+5]>>5);
  size_t head=(b[p+1]&1)?7:9;if(len<head||len>n-p)return 8;
  NeAACDecFrameInfo info;float*pcm=NeAACDecDecode(h,&info,b+p+head,(unsigned long)(len-head));
  if(info.error||(!pcm&&info.samples))return 9;
  for(unsigned long k=0;k<info.samples;k++)energy+=fabs(pcm[k]);
  total+=info.samples;frames++;p+=len;
 }
 NeAACDecClose(h);free(b);if(frames<4||!total||energy<1)return 10;
 printf("RAW_OK aus=%zu samples=%lu energy=%.3f\n",frames,total,energy);
 b=readall("task_support/single.loas",&n);h=NeAACDecOpen();
 long init=NeAACDecInit(h,b,(unsigned long)n,&rate,&ch);NeAACDecFrameInfo info={0};
 if(init>=0)NeAACDecDecode(h,&info,b,(unsigned long)n);else info.error=255;
 printf("LOAS_OBSERVATION init=%ld error=%u samples=%lu\n",init,(unsigned)info.error,info.samples);
 NeAACDecClose(h);free(b);return 0;
}
