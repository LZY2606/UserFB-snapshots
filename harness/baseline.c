#include "wavpack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int output(void *id,void *bytes,int32_t n) {return fwrite(bytes,1,n,id)==(size_t)n;}
int main(int argc,char **argv) {
  assert(argc==2); FILE *f=fopen(argv[1],"wb");assert(f);
  WavpackContext *w=WavpackOpenFileOutput(output,f,NULL);assert(w);
  WavpackConfig c={0};c.bytes_per_sample=2;c.bits_per_sample=16;
  c.num_channels=6;c.channel_mask=0x3f;c.sample_rate=48000;c.block_samples=32;
  int32_t original[1025*6],work[1025*6],out[1025*6];
  for(int n=0;n<1025;n++)for(int k=0;k<6;k++)original[n*6+k]=(n*31+k*197)%30001-15000;
  memcpy(work,original,sizeof(work));
  assert(WavpackSetConfiguration64(w,&c,1025,NULL));assert(WavpackPackInit(w));
  assert(WavpackPackSamples(w,work,1025));assert(WavpackFlushSamples(w));
  WavpackCloseFile(w);fclose(f);char error[80];
  w=WavpackOpenFileInput(argv[1],error,0,0);assert(w);
  assert(WavpackGetNumChannels(w)==6);
  int n=0;while(n<1025){unsigned got=WavpackUnpackSamples(w,out+n*6,(1025-n)<7?(1025-n):7);assert(got);n+=got;}
  assert(n==1025 && memcmp(original,out,sizeof(out))==0);
  assert(WavpackGetNumErrors(w)==0);assert(WavpackSeekSample64(w,33));
  assert(WavpackUnpackSamples(w,out,9)==9);
  assert(memcmp(out,original+33*6,9*6*sizeof(*out))==0);
  WavpackCloseFile(w);puts("WAVPACK_6CH_ROUNDTRIP_SEEK_OK");
}
