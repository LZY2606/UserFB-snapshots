#include <assert.h>
#include <math.h>
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
int main(int argc,char **argv){
 assert(argc==4);int rate=atoi(argv[2]),mono=atoi(argv[3]);
 plm_audio_t *a=plm_audio_create_with_buffer(plm_buffer_create_with_filename(argv[1]),1);assert(a);
 double energy=0,difference=0;float first[8*2304];
 for(int pass=0;pass<2;pass++){
  int frames=0;plm_samples_t *s;
  while((s=plm_audio_decode(a))){
   assert(frames<8);assert(s->count==1152);assert(plm_audio_get_samplerate(a)==rate);
   assert(fabs(s->time-(frames*1152.0/rate))<1e-10);
   for(int n=0;n<2304;n++){
    float x=s->interleaved[n];assert(isfinite(x));
    if(pass){if(frames>0||n>=1024)assert(x==first[frames*2304+n]);}else first[frames*2304+n]=x;
    energy+=x*x;
   }
   for(int n=0;n<1152;n++)difference+=fabs(s->interleaved[2*n]-s->interleaved[2*n+1]);
   ++frames;
  }
  assert(frames==8);assert(!plm_audio_decode(a));
  if(!pass)plm_audio_rewind(a);
 }
 assert(energy>10);assert(mono?difference==0:difference>10);
 printf("MP2_BASELINE_OK rate=%d frames=8 replay=8 mono=%d\n",rate,mono);
 plm_audio_destroy(a);return 0;
}
