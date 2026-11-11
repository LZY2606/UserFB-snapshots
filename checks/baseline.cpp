#include "FFTConvolver.h"
#include "TwoStageFFTConvolver.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cassert>
int main() {
  std::vector<float> x(513), h(177), a(x.size()), b(x.size());
  for (size_t i=0;i<201;++i) x[i]=float(int(i%13)-6)/13;
  for (size_t i=0;i<h.size();++i) h[i]=float(int(i%7)-3)/float(20+i);
  fftconvolver::FFTConvolver u; fftconvolver::TwoStageFFTConvolver t;
  assert(u.init(16,h.data(),h.size())); assert(t.init(16,64,h.data(),h.size()));
  for(size_t p=0;p<x.size();) {
    size_t n=std::min(size_t(1+p%37),x.size()-p);
    u.process(x.data()+p,a.data()+p,n); t.process(x.data()+p,b.data()+p,n); p+=n;
  }
  for(size_t n=0;n<x.size();++n) {
    double ref=0; for(size_t k=0;k<h.size() && k<=n;++k) ref+=double(h[k])*x[n-k];
    assert(std::abs(a[n]-ref)<2e-5); assert(std::abs(b[n]-ref)<2e-5);
  }
  std::puts("FFT_BASELINE_OK uniform=513 two_stage=513 tail=177");
}
