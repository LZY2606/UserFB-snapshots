#include "TwoStageFFTConvolver.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
struct Deferred:fftconvolver::TwoStageFFTConvolver{
 bool pending=false; unsigned starts=0,waits=0;
 void startBackgroundProcessing()override {pending=true;++starts;}
 void waitForBackgroundProcessing()override {if(pending){++waits;doBackgroundProcessing();pending=false;}}
 void finish(){waitForBackgroundProcessing();}
};
int main(){
 std::vector<float> x(4096),ir(513),y(4096);
 for(unsigned i=0;i<1500;i++)x[i]=.2f*std::sin(i*.031)+.07f*std::sin(i*i*.00019);
 ir[0]=.5f;ir[37]=-.12f;ir[129]=.18f;ir[512]=.07f;
 Deferred c;if(!c.init(16,64,ir.data(),ir.size()))return 2;
 unsigned pos=0,part=0;unsigned sizes[]={1,7,16,67};
 while(pos<x.size()){unsigned n=std::min<unsigned>(sizes[part++%4],x.size()-pos);c.process(x.data()+pos,y.data()+pos,n);pos+=n;}
 c.finish();double err=0,energy=0;
 for(unsigned n=0;n<x.size();n++){double ref=0;for(unsigned k=0;k<ir.size()&&k<=n;k++)ref+=double(ir[k])*x[n-k];err=std::max(err,std::abs(ref-y[n]));energy+=y[n]*y[n];}
 std::printf("LEGACY_TAIL starts=%u deferred_waits=%u max_error=%.9g energy=%.6f\n",c.starts,c.waits,err,energy);
 if(err>2e-5||energy<1||!c.waits)return 3;
 std::puts("BASELINE_CONVOLUTION_OK");
}
