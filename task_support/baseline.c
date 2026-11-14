#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "a52.h"
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"BASELINE_FAIL line=%d expr=%s\n",__LINE__,#x);return 1;}}while(0)
static int decode(const char*path,int lfe){
 FILE*f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long size=ftell(f);REQUIRE(size>0);rewind(f);
 uint8_t*buf=calloc((size_t)size+16,1);REQUIRE(buf);REQUIRE(fread(buf,1,(size_t)size,f)==(size_t)size);fclose(f);
 a52_state_t*full=a52_init(0),*stereo=a52_init(0);REQUIRE(full&&stereo);
 unsigned frames=0,blocks=0;long pos=0;double en[6]={0},stenergy=0;
 while(pos<size){int fl,sr,br;REQUIRE(size-pos>=7);int len=a52_syncinfo(buf+pos,&fl,&sr,&br);REQUIRE(len>0&&len<=size-pos&&sr==48000);REQUIRE((fl&A52_CHANNEL_MASK)==A52_3F2R);REQUIRE(!!(fl&A52_LFE)==lfe);
  int a=A52_3F2R|(lfe?A52_LFE:0),b=A52_STEREO;sample_t lev=1,levb=1;
  REQUIRE(a52_frame(full,buf+pos,&a,&lev,0)==0);REQUIRE(a52_frame(stereo,buf+pos,&b,&levb,0)==0);
  REQUIRE(a==(A52_3F2R|(lfe?A52_LFE:0))&&b==A52_STEREO);REQUIRE(lev==1&&levb==1);
  a52_dynrng(full,NULL,NULL);a52_dynrng(stereo,NULL,NULL);
  for(int k=0;k<6;k++){
   REQUIRE(a52_block(full)==0&&a52_block(stereo)==0);sample_t*x=a52_samples(full),*y=a52_samples(stereo);
   int off=lfe?256:0;
   /* AC-3 cmixlev/surmixlev from the fixture header, independent of downmix helper. */
   static const double cm[4]={.7071067811865476,.5946035575013605,.5,.5946035575013605};
   static const double sm[4]={.7071067811865476,.5,0,.5};
   double c=cm[(buf[pos+6]>>3)&3],s=sm[(buf[pos+6]>>1)&3];
   for(int n=0;n<256;n++){
    for(int ch=0;ch<5+lfe;ch++){double v=x[256*ch+n];REQUIRE(isfinite(v));en[ch]+=v*v;}
    double left=x[off+n]+c*x[off+256+n]+s*x[off+768+n];
    double right=x[off+512+n]+c*x[off+256+n]+s*x[off+1024+n];
    REQUIRE(isfinite(y[n])&&isfinite(y[256+n]));
    REQUIRE(fabs(y[n]-left)<3e-5*(1+fabs(left)));REQUIRE(fabs(y[256+n]-right)<3e-5*(1+fabs(right)));
    stenergy+=(double)y[n]*y[n]+(double)y[256+n]*y[256+n];
   }
   blocks++;
  }
  frames++;pos+=len;
 }
 REQUIRE(frames>=12&&blocks==frames*6&&pos==size&&stenergy>0);
 for(int ch=0;ch<5+lfe;ch++)REQUIRE(en[ch]>1e-8);
 printf("A52_BASELINE_STREAM lfe=%d frames=%u blocks=%u active_channels=%d stereo_matrix_equal=1\n",lfe,frames,blocks,5+lfe);
 a52_free(full);a52_free(stereo);free(buf);return 0;
}
int main(void){if(decode("task_support/fixtures/surround_lfe.ac3",1)||decode("task_support/fixtures/surround.ac3",0))return 1;puts("A52_BASELINE_OK fixtures=2 layouts=2 real_pcm=1");return 0;}
