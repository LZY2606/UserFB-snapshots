#include "soxr.h"
#include <math.h>
#include <stdio.h>
int main(void) {
  double in[4800*2],out[5000*2],energy=0; size_t i,idone=0,odone=0;
  soxr_error_t e=0; soxr_io_spec_t io=soxr_io_spec(SOXR_FLOAT64_I,SOXR_FLOAT64_I);
  soxr_quality_spec_t quality=soxr_quality_spec(SOXR_VHQ,0);
  for(i=0;i<4800;i++){in[2*i]=.4*sin(2*3.141592653589793*1000*i/48000);in[2*i+1]=-in[2*i];}
  e=soxr_oneshot(48000,44100,2,in,4800,&idone,out,5000,&odone,&io,&quality,0);
  if(e||idone!=4800||odone!=4410)return 1;
  for(i=0;i<odone;i++){if(!isfinite(out[2*i])||fabs(out[2*i]+out[2*i+1])>1e-10)return 2;energy+=out[2*i]*out[2*i];}
  if(energy<300||energy>400)return 3;
  printf("SOXR_BASELINE input=%zu output=%zu channels=2 energy=%.9f\n",idone,odone,energy);return 0;
}
