#include "task_support/rational_reference.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static void require(bool b) { if(!b) { std::fputs("REFERENCE_ASSERT_FAILED\n",stderr); std::exit(2); } }
static std::vector<double> render(unsigned S,unsigned D,const std::vector<double>& x,bool split) {
    r8b::TaskRationalResampler r(S,D,127,2,120,r8b::fprLinearPhase);
    const auto M=task_reference::output_frames(x.size(),S,D);
    std::vector<double> out; std::size_t p=0; unsigned calls=0;
    while(out.size()<M) {
        const unsigned plan[]={1,7,127};
        int n=split ? plan[calls++%3] : 127;
        double block[127]={};
        if(p<x.size()) { n=std::min<std::size_t>(n,x.size()-p);std::copy_n(x.data()+p,n,block);p+=n; }
        double* op=nullptr;int count=r.process(block,n,op);
        if(count) out.insert(out.end(),op,op+std::min<std::uint64_t>(count,M-out.size()));
        require(calls<1000000); // bounded short-input diagnostic
    }
    return out;
}
int main() {
    // Independent small-cursor recurrence; no absolute products or floating drift.
    for(auto pair: {std::pair<unsigned,unsigned>{95998,48001},{88200,48000},{48000,47999},{768000,8001}}) {
        const auto S=pair.first,D=pair.second;
        for(double initial: {0.0,0.25,0.5,0.999999999999}) {
            std::uint64_t q=0,r=0;
            for(std::uint64_t j=0;j<100003;++j) {
                const auto actual=task_reference::phase_at(j,S,D,initial);
                const double t=initial+double(r)/D;const unsigned carry=t>=1;
                require(actual.integer==q+carry && actual.fraction==t-carry);
                r+=S; q+=r/D; r%=D;
            }
        }
    }
    // Large positions are checked by a different bounded modular double-and-add.
    for(auto j: {(1ULL<<32)+3,(1ULL<<36)+127,(1ULL<<40)-1,1ULL<<40}) {
        const std::uint64_t S=95998,D=48001;
        std::uint64_t q=0,r=0;
        for(int bit=63;bit>=0;--bit) { q*=2;r*=2; if((j>>bit)&1)r+=S;q+=r/D;r%=D; }
        const auto point=task_reference::phase_at(j,S,D,0.5);
        const double t=0.5+double(r)/D;const unsigned c=t>=1;
        require(point.integer==q+c && point.fraction==t-c);
        const auto next=task_reference::phase_at(j+1,S,D,0.5);
        require(next.integer>=point.integer && next.integer-point.integer<=2);
        std::printf("RATIONAL_CURSOR j=%llu integer=%llu fraction=%.17g\n",j,(unsigned long long)point.integer,point.fraction);
    }
    unsigned cases=0;
    for(auto rates: {std::pair<unsigned,unsigned>{44100,48000},{48000,192000},{192000,48000},{47999,48001},{48000,48000}}) {
        for(unsigned N: {0,1,13,4099}) {
            std::vector<double> x(N);
            for(unsigned i=0;i<N;++i)x[i]=0.2*std::sin(i*.017)+0.1*std::cos(i*.071);
            if(N) {x.front()+=.5;x.back()-=.7;}
            const auto a=render(rates.first,rates.second,x,false);
            const auto b=render(rates.first,rates.second,x,true);
            require(a.size()==task_reference::output_frames(N,rates.first,rates.second) && a.size()==b.size());
            double error=0;for(std::size_t i=0;i<a.size();++i) {require(std::isfinite(a[i]));error=std::max(error,std::abs(a[i]-b[i]));}
            require(error<1e-10);
            std::printf("RATIONAL_FORWARD rate=%u->%u N=%u M=%zu split_error=%.3g\n",rates.first,rates.second,N,a.size(),error);
            ++cases;
        }
    }
    std::printf("RATIONAL_REFERENCE_OK cases=%u cursor_max=1099511627776\n",cases);
}
