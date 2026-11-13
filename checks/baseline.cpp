#include "CDSPResampler.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
static std::vector<double> render(int block, int channel) {
 r8b::CDSPResampler r(44100,48000,256,2,120);
 std::vector<double> out; int pos=0;
 while(pos<12288){int n=std::min(block,12288-pos);double in[256];for(int i=0;i<n;i++)in[i]=pos+i<8192 ? (channel? -0.5:1)*std::sin((pos+i)*0.13):0;double *p=nullptr;int m=r.process(in,n,p);for(int j=0;j<m;j++){if(!std::isfinite(p[j]))std::abort();out.push_back(p[j]);}pos+=n;}
 return out;
}
int main(){auto a=render(256,0),b=render(7,0),c=render(31,1);if(a.size()!=b.size()||a.size()!=c.size()||a.size()<8192)return 1;double error=0;for(size_t i=0;i<a.size();i++)error=std::max(error,std::max(std::abs(a[i]-b[i]),std::abs(a[i]+2*c[i])));if(error>1e-10)return 2;std::printf("R8B_BASELINE_OK frames=%zu error=%.3g\n",a.size(),error);}
