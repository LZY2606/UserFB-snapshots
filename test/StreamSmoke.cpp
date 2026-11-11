// Synthetic public signal: sparse impulses and a five-tap FIR.
#include "FFTConvolver.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
int main() {
    using namespace fftconvolver;
    Sample x[13]={1,0,-.25f,0,.5f}, h[5]={1,.5f,0,-.25f,.125f}, y[13]={};
    FFTConvolver c;
    if(!c.init(4,h,5)) return 2;
    c.process(x,y,3); c.process(x+3,y+3,10);
    double err=0;
    for(int n=0;n<13;++n) {
        double ref=0; for(int k=0;k<5 && k<=n;++k) ref+=h[k]*x[n-k];
        err=std::max(err,std::fabs(y[n]-ref));
    }
    std::printf("samples=13 max_error=%.9g\n",err);
    return err<1e-6 ? 0 : 1;
}
