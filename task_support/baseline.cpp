#include "CDSPResampler.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
static std::vector<double> convert(double sr, double dr, r8b::EDSPFilterPhaseResponse phase, const std::vector<double>& in, bool small) {
  r8b::CDSPResampler r(sr,dr,127,2.0,120.0,phase);
  std::vector<double> result;
  int cursor=0, call=0;
  while(cursor<(int)in.size()) {
    int n=std::min((int)in.size()-cursor,small ? (call++%3==0?1:call%3==1?7:127):127);
    double scratch[127];std::copy_n(in.data()+cursor,n,scratch);double* out=nullptr;
    int count=r.process(scratch,n,out);
    if(count) result.insert(result.end(),out,out+count);
    cursor+=n;
  }
  return result;
}
int main(){
 std::vector<double> in(24000);
 for(size_t i=0;i<in.size();++i) in[i]=0.3*std::sin(i*.017)+.1*std::cos(i*.071)+(i%997==0?.5:0);
 double rates[][2]={{44100,48000},{48000,192000},{192000,48000},{44056.3,47999.7},{48000,48000}};
 int cases=0;
 for(auto& rateset:rates) for(auto phase:{r8b::fprLinearPhase,r8b::fprMinPhase}){
   auto a=convert(rateset[0],rateset[1],phase,in,false);auto b=convert(rateset[0],rateset[1],phase,in,true);
   if(a.empty()||a.size()!=b.size()) return 2;
   double err=0;for(size_t i=0;i<a.size();++i){if(!std::isfinite(a[i])||!std::isfinite(b[i]))return 3;err=std::max(err,std::abs(a[i]-b[i]));}
   std::printf("PRODUCTION rate=%.1f->%.1f phase=%d frames=%zu split_error=%.3g\n",rateset[0],rateset[1],(int)phase,a.size(),err);
   if(err>1e-10)return 4; ++cases;
 }
 std::puts("R8B_BASELINE_OK cases=10");return cases==10?0:5;
}
