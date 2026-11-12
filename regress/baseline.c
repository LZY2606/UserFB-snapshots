#include "soxr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  size_t i, used=0, done=0; double *in=calloc(48000,sizeof(double)),*out=calloc(46000,sizeof(double));
  double re=0,im=0,amp;soxr_error_t err;
  soxr_io_spec_t io=soxr_io_spec(SOXR_FLOAT64_I,SOXR_FLOAT64_I);
  soxr_quality_spec_t q=soxr_quality_spec(SOXR_VHQ,0);
  if(!in||!out)return 1;
  for(i=0;i<48000;++i)in[i]=sin(2*acos(-1.)*1000*i/48000.);
  err=soxr_oneshot(48000,44100,1,in,48000,&used,out,46000,&done,&io,&q,NULL);
  if(err||used!=48000||done!=44100)return 2;
  for(i=4410;i<39690;++i){double a=2*acos(-1.)*1000*i/44100.;re+=out[i]*cos(a);im+=out[i]*sin(a);}
  amp=2*hypot(re,im)/35280.;
  if(fabs(amp-1)>1e-5)return 3;
  printf("SOXR_BASELINE_OK consumed=%zu generated=%zu passband_amplitude=%.6f\n",used,done,amp);
  free(in);free(out);return 0;
}
