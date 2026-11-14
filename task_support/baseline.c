#include <samplerate.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define FRAMES 8192
static float input[FRAMES*2], output[FRAMES*4], cbout[FRAMES*4];
struct feed {long at;};
static long feed(void *v,float **p) {
 struct feed *f=v;long n=FRAMES-f->at;if(n>79)n=79;
 *p=input+f->at*2;f->at+=n;return n;
}
int main(void) {
 for(int i=0;i<FRAMES;i++){input[2*i]=(float)(.4*sin(.131*i)+.2*cos(.373*i));input[2*i+1]=-input[2*i];}
 for(int q=0;q<3;q++) {
  int e=0;SRC_STATE *s=src_new(q,2,&e);if(!s||e)return 2;
  SRC_DATA d;memset(&d,0,sizeof(d));d.data_in=input;d.data_out=output;
  d.input_frames=FRAMES;d.output_frames=FRAMES*2;d.end_of_input=1;d.src_ratio=.75;
  if(src_process(s,&d)||d.output_frames_gen<6000)return 3;
  double energy=0;for(long i=0;i<d.output_frames_gen;i++){
   if(!isfinite(output[2*i])||fabs(output[2*i]+output[2*i+1])>1e-6)return 4;
   energy+=output[2*i]*output[2*i];
  }
  src_delete(s);struct feed f={0};s=src_callback_new(feed,q,2,&e,&f);if(!s||e)return 5;
  long n=src_callback_read(s,.75,FRAMES*2,cbout);src_delete(s);
  if(n!=d.output_frames_gen||energy<1)return 6;
  for(long i=0;i<n*2;i++)if(fabs(output[i]-cbout[i])>2e-6)return 7;
  printf("BASELINE_SINC quality=%d frames=%ld energy=%.9f callback_match=1\n",q,n,energy);
 }
 puts("BASELINE_OK sinc=3 channels=2 ratio=0.75");return 0;
}
