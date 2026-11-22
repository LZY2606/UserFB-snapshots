#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "samplerate.h"
#include "common.h"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x); return 1;} } while(0)
typedef struct { float *data; long frames; int calls; } Feed;
static long feed(void *ctx,float **data){Feed *f=ctx; f->calls++; *data=f->data; if(f->calls==1)return f->frames; return 0;}
int main(void){
 const int chans[]={1,2,4,6,7}; uint32_t r=13; double energy=0;
 for(int type=0;type<3;type++)for(int ci=0;ci<5;ci++){
  int ch=chans[ci],err=0; float *in=malloc(4096*ch*sizeof(float));float a[17*7],b[17*7];
  CHECK(in);for(int i=0;i<4096*ch;i++){r=r*1664525u+1013904223u;in[i]=(float)((int)(r>>16)-32768)/65536;}
  SRC_STATE *s=src_new(type,ch,&err); CHECK(s&&err==0);
  SRC_DATA d={0};d.data_in=in;d.input_frames=4096;d.data_out=a;d.output_frames=7;d.src_ratio=48000.0/44100;
  CHECK(src_process(s,&d)==0);CHECK(d.output_frames_gen==7&&d.input_frames_used>0);
  long consumed=d.input_frames_used; SRC_STATE *t=src_clone(s,&err);CHECK(t&&err==0);
  SRC_DATA x={0};x.data_in=in+consumed*ch;x.input_frames=4096-consumed;x.data_out=a;x.output_frames=17;x.src_ratio=d.src_ratio;
  SRC_DATA y=x;y.data_out=b;CHECK(src_process(s,&x)==0);CHECK(src_process(t,&y)==0);
  CHECK(x.input_frames_used==y.input_frames_used&&x.output_frames_gen==y.output_frames_gen);
  CHECK(x.output_frames_gen==17);CHECK(memcmp(a,b,17*ch*sizeof(float))==0);
  for(int i=0;i<17*ch;i++){CHECK(isfinite(a[i]));energy+=a[i]*a[i];}
  src_delete(s);src_delete(t);free(in);
 }
 CHECK(energy>0.1);printf("SINC_BASELINE combinations=15 clone_bits=equal energy=%.9g\n",energy);
 int err=0,ch=7;Feed f={calloc(1000000*ch,sizeof(float)),1000000,0};CHECK(f.data);
 for(long i=0;i<f.frames;i++)f.data[i*ch]=(float)(i%97)/97;
 SRC_STATE *s=src_callback_new(feed,SRC_SINC_FASTEST,ch,&err,&f);float out[7];CHECK(s&&err==0);
 CHECK(src_callback_read(s,1.1,1,out)==1);CHECK(f.calls==1);CHECK(s->saved_frames>0);
 printf("CALLBACK_BASELINE calls=%d pending_frames=%ld sample=%.9g\n",f.calls,s->saved_frames,out[0]);
 src_delete(s);free(f.data);puts("PRODUCTION_BASELINE_OK");return 0;
}
