#include "AsyncFFTConvolver.h"
#include <vector>
#include <cmath>
#include <algorithm>
struct Reject:fftconvolver::TailExecutor{unsigned submissions=0,barriers=0;bool submit(void(*)(void*),void*)override{++submissions;return false;}void quiesce()override{++barriers;}};
int main(){
 Reject e;fftconvolver::AsyncFFTConvolver c;
 std::vector<float> h(513),x(4096),y(4096);h[0]=.5f;h[129]=.25f;h[512]=-.1f;x[0]=1;x[37]=.3f;
 if(!c.init(16,64,h.data(),h.size(),&e))return 2;
 c.process(x.data(),y.data(),x.size());
 for(unsigned n=0;n<x.size();n++){double ref=0;for(unsigned k=0;k<h.size()&&k<=n;k++)ref+=double(h[k])*x[n-k];if(!std::isfinite(y[n])||std::abs(y[n]-ref)>2e-5)return 3;}
 if(!e.submissions||e.barriers)return 4;c.reset();return 0;
}
