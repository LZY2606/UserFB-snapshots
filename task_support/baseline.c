#include "wavpack.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"baseline line %d\n",__LINE__);exit(1);}}while(0)
static int block(void*id,void*data,int32_t n){return fwrite(data,1,(size_t)n,(FILE*)id)==(size_t)n;}
int main(void){const uint32_t raw[16]={0x3f800000,0xbf800000,0,0x80000000,0x7f800000,0xff800000,0x7fc12345,0xff812345,1,0x807fffff,0x3f800001,0x3eaaaaab,0x7f7fffff,0x00800000,0x3f7fffff,0xbf000001};int32_t in[16],out[16];memcpy(in,raw,sizeof in);WavpackConfig cfg;memset(&cfg,0,sizeof cfg);cfg.bytes_per_sample=4;cfg.bits_per_sample=32;cfg.float_norm_exp=127;cfg.num_channels=2;cfg.channel_mask=3;cfg.sample_rate=48000;FILE*f=fopen("float.wv","wb");CHECK(f);WavpackContext*w=WavpackOpenFileOutput(block,f,NULL);CHECK(w);CHECK(WavpackSetConfiguration64(w,&cfg,8,NULL));CHECK(WavpackPackInit(w));CHECK(WavpackPackSamples(w,in,8));CHECK(WavpackFlushSamples(w));WavpackCloseFile(w);CHECK(fclose(f)==0);char err[128];w=WavpackOpenFileInput("float.wv",err,0,0);if(!w)fprintf(stderr,"%s\n",err);CHECK(w);CHECK(WavpackGetMode(w)&MODE_FLOAT);CHECK(WavpackUnpackSamples(w,out,8)==8);CHECK(memcmp(raw,out,sizeof raw)==0);CHECK(WavpackGetNumErrors(w)==0);WavpackCloseFile(w);remove("float.wv");puts("WavPack float32 stereo frames=8 exact_bits=16 negative_zero_nan_subnormal preserved");return 0;}
