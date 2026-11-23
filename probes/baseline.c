#include "resampler.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    enum {N=4096, CAP=8192};
    artsample_t in[N*2], out[CAP*2], resetout[128*2], freshout[128*2];
    Resample *r=resampleInit(2,64,128,0.9,SUBSAMPLE_INTERPOLATE|BLACKMAN_HARRIS);
    Resample *fresh=resampleInit(2,64,128,0.9,SUBSAMPLE_INTERPOLATE|BLACKMAN_HARRIS);
    assert(r && fresh);
    for(int i=0;i<N;i++){in[2*i]=0.3*sin(i*0.031)+0.2*cos(i*0.071);in[2*i+1]=-in[2*i];}
    ResampleResult a=resampleProcessInterleaved(r,in,N/2,out,CAP,1.001);
    assert(a.input_used==N/2 && a.output_generated>1000);
    ResampleResult b=resampleProcessInterleaved(r,in+N,N/2,out+2*a.output_generated,CAP-a.output_generated,1.0008);
    unsigned total=a.output_generated+b.output_generated;
    assert(b.input_used==N/2 && total>4000 && total<CAP);
    double energy=0,error=0;
    for(unsigned j=0;j<total;j++){assert(isfinite(out[2*j])&&isfinite(out[2*j+1]));energy+=out[2*j]*out[2*j];error=fmax(error,fabs(out[2*j]+out[2*j+1]));}
    assert(energy>50 && error<1e-12);
    double position=resampleGetPosition(r);
    ResampleResult z=resampleProcessInterleaved(r,in,10,out,0,1.0008);
    assert(z.input_used==0 && z.output_generated==0 && resampleGetPosition(r)==position);
    resampleReset(r);
    ResampleResult x=resampleProcessInterleaved(r,in,N,resetout,128,1.001);
    ResampleResult y=resampleProcessInterleaved(fresh,in,N,freshout,128,1.001);
    assert(x.output_generated==128 && y.output_generated==128);
    for(int j=0;j<256;j++)assert(resetout[j]==freshout[j]);
    printf("RAW_RATIO_STREAM input=%u output=%u energy=%.9f channel_error=%.3g\n",a.input_used+b.input_used,total,energy,error);
    puts("ZERO_CAPACITY_PRESERVES_POSITION RESET_MATCHES_FRESH");
    puts("CAPABILITY_GAP raw-ratio API has no paired hardware-clock estimator or per-output servo");
    resampleFree(r);resampleFree(fresh);return 0;
}
