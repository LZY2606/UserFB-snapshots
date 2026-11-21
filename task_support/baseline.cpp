#include "CDSPResampler.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>
using namespace r8b;
template<class T,class=void> struct has_seek:std::false_type{};
template<class T> struct has_seek<T,std::void_t<decltype(std::declval<T&>().seekToOutput(int64_t{},(int(*)(void*,int64_t,double*,int))nullptr,nullptr))>>:std::true_type{};
static std::vector<double> convert(std::vector<double> in,int block) {
 CDSPResampler r(44100,48000,block,4.0,100.0);
 std::vector<double> out;
 for(size_t i=0;i<in.size();) {int n=std::min((size_t)block,in.size()-i); double* p=nullptr;int m=r.process(in.data()+i,n,p); if(m)out.insert(out.end(),p,p+m);i+=n;}
 return out;
}
int main(){
 const int N=24000,T=10001,M=512;std::vector<double> in(N);
 for(int i=0;i<N;++i) in[i]=.4*std::sin(i*.071)+.17*std::sin(i*.113); in[1000]+=.3;
 auto a=convert(in,127),b=convert(in,7);size_t n=std::min(a.size(),b.size()); double err=0;
 for(size_t i=0;i<n;++i)err=std::max(err,std::fabs(a[i]-b[i]));
 if(a.size()!=b.size()||n<20000||err>1e-10)return 1;
 int start=(int)std::floor(T*44100.0/48000.0);std::vector<double> tail(in.begin()+start,in.end());auto c=convert(tail,127); double diff=0;
 for(int j=0;j<M;++j)diff=std::max(diff,std::fabs(a[T+j]-c[j]));
 if(diff<1e-4)return 2;
 std::printf("SEQUENTIAL_OK frames=%zu chunk_error=%.3g\n",a.size(),err);
 std::printf("NAIVE_RESTART_MISMATCH output_frame=%d max_error=%.9g\n",T,diff);
 std::printf("SEEK_API_PRESENT=%d\n",(int)has_seek<CDSPResampler>::value);
}
