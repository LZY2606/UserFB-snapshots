#include "dsp/chromagram/Chromagram.h"
#include <vector>
#include <cmath>
#include <cstdio>
int main(){
 ChromaConfig cfg={8000,200,1600,12,.005,MathUtilities::NormaliseNone};
 Chromagram c(cfg);int n=c.getFrameSize();std::vector<double> x(n);
 for(int i=0;i<n;i++)x[i]=.4*std::sin(2*3.141592653589793*400*i/8000)+.2*std::cos(2*3.141592653589793*600*i/8000);
 double* y=c.process(x.data());double sum=0;for(int k=0;k<12;k++){if(!std::isfinite(y[k]))return 2;sum+=y[k];}
 if(sum<=.01)return 3;std::printf("QM_CQT_BASELINE framesize=%d bins=12 finite=1 nonzero=1 sum=%.12g\n",n,sum);
}
