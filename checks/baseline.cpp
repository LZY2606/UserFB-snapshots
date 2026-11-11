#include "stft.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <algorithm>
template<bool split> void run(){
 using S=signalsmith::linear::DynamicSTFT<double,split,signalsmith::linear::STFT_SPECTRUM_UNPACKED>;
 S s;s.configure(3,2,128,0,32);
 std::vector<double>x(2048),r(2048),side(2048),y(2048),z(2048);
 for(size_t i=0;i<x.size();i++){x[i]=.3*std::sin(i*.13)+.1; r[i]=-2*x[i];side[i]=std::cos(i*.41);}
 for(size_t p=0;p+32<x.size();p+=32){
  s.readOutput(0,32,y.data()+p);s.readOutput(1,32,z.data()+p);
  s.writeInput(0,32,x.data()+p);s.writeInput(1,32,r.data()+p);s.writeInput(2,32,side.data()+p);s.moveInput(32);
  for(size_t j=0;j<s.analyseSteps();j++)s.analyseStep(j);
  s.moveOutput(32);
  for(size_t j=0;j<s.synthesiseSteps();j++)s.synthesiseStep(j);
 }
 double e=0;
 for(size_t i=128;i+128<1952;i++){e=std::max(e,std::abs(y[i+s.latency()]-x[i]));e=std::max(e,std::abs(z[i+s.latency()]-r[i]));}
 assert(s.bands()==65 && s.latency()==128 && e<1e-10);
 std::printf("baseline: split=%d 3in/2out unpacked bins=65 latency=128 max_error=%.3g\n",split,e);
}
int main(){run<false>();run<true>();}
