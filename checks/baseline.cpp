#include "DspFilters/Dsp.h"
#include <cmath>
#include <cstdio>
int main(){Dsp::SimpleFilter<Dsp::Butterworth::BandPass<8>,2> f;f.setup(5,48000,11000,6000);f.reset();double a[4096],b[4096];for(int i=0;i<4096;i++){a[i]=.3*sin(i*.4);b[i]=-a[i];}double *p[]={a,b};f.process(4096,p);double energy=0;for(int i=0;i<4096;i++){if(!std::isfinite(a[i])||fabs(a[i]+b[i])>1e-8){printf("BAD i=%d a=%.17g b=%.17g\n",i,a[i],b[i]);return 1;}energy+=a[i]*a[i];}if(energy<1e-8)return 2;printf("DSPFILTERS_BASELINE_OK stages=%d energy=%.9g\n",f.getNumStages(),energy);}
