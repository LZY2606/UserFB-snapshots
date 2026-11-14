#include "soxr.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static int run(size_t n, double ratio, int variable) {
  float in[4096], out[8192];
  soxr_error_t e=0;
  soxr_quality_spec_t q=soxr_quality_spec(SOXR_HQ,variable?SOXR_VR:0);
  soxr_io_spec_t io=soxr_io_spec(SOXR_FLOAT32_I,SOXR_FLOAT32_I);
  soxr_runtime_spec_t rt=soxr_runtime_spec(1);
  soxr_t s=soxr_create(8,1,1,&e,&io,&q,&rt);
  size_t i,used=0,got=0,total=0,loops=0;
  if(!s||e||n>4096)return 1;
  if(variable){e=soxr_set_io_ratio(s,ratio,0);if(e)return 2;}
  else {soxr_delete(s);s=soxr_create(ratio,1,1,&e,&io,&q,&rt);if(!s||e)return 3;}
  for(i=0;i<n;i++)in[i]=(float)(.3*sin(i*.031)+.1*cos(i*.057));
  printf("%s N=%zu ratio=%.3f initial_delay=%.0f ",variable?"VR":"CR",n,ratio,soxr_delay(s));
  if(n){e=soxr_process(s,in,n,0,out,8192,&got);if(e)return 4;used=n;total+=got;}
  do{e=soxr_process(s,0,0,0,out,8192,&got);if(e||++loops>100)return 5;total+=got;}while(got);
  printf("accepted=%zu frames=%zu logical=%zu drained_delay=%.0f\n",used,total,(size_t)floor(n/ratio+.5),soxr_delay(s));
  if(!variable && total!=(size_t)floor(n/ratio+.5))return 6;
  soxr_delete(s);return 0;
}
int main(void){
  if(run(1000,2,0)||run(0,2,1)||run(1,2,1)||run(1000,2,1)||run(1000,.75,1))return 1;
  puts("BASELINE_OK real_pcm_converted");return 0;
}
