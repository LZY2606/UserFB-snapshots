#include "DspFilters/Dsp.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
int main() {
  typedef Dsp::RBJ::Design::LowPass LP;
  Dsp::SmoothedFilterDesign<LP,1,Dsp::DirectFormI> a(128);
  Dsp::SmoothedFilterDesign<LP,1,Dsp::DirectFormII> b(128);
  Dsp::Params p; p.clear(); p[0]=48000; p[1]=800; p[2]=.8;
  a.setParams(p); b.setParams(p); std::vector<double>x(512),y(512);
  for(int i=0;i<512;i++)x[i]=y[i]=.3*std::sin(i*.07);
  double *xp=x.data(),*yp=y.data(); a.process(512,&xp);b.process(512,&yp);
  double fixed=0;for(int i=0;i<512;i++)fixed=std::max(fixed,std::abs(x[i]-y[i]));
  assert(fixed<1e-6);puts("FIXED_REALIZATIONS_OK");
  p[1]=6000;p[2]=1.7; a.setParams(p);b.setParams(p);
  for(int i=0;i<512;i++)x[i]=y[i]=.3*std::sin((i+512)*.07);
  a.process(512,&xp);b.process(512,&yp);
  double moving=0;for(int i=0;i<512;i++){assert(std::isfinite(y[i]));moving=std::max(moving,std::abs(x[i]-y[i]));}
  assert(moving>1e-3);std::printf("MODULATION_REALIZATION_DIFFERENCE max=%.8f\n",moving);
}
