#include "FFTConvolver.h"
#include "TwoStageFFTConvolver.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
template<class Engine> static double run(const std::vector<float>& in,const std::vector<float>& ir, Engine& engine, bool fragments){
 std::vector<float> out(in.size());
 size_t cursor=0,call=0;
 while(cursor<in.size()){
   const size_t sizes[]={1,7,31,257};
   size_t n=std::min(in.size()-cursor,fragments?sizes[call++%4]:(size_t)256);
   engine.process(in.data()+cursor,out.data()+cursor,n);cursor+=n;
 }
 double err=0, energy=0;
 for(size_t i=0;i<in.size();++i){
   double expected=0;
   for(size_t j=0;j<ir.size()&&j<=i;++j)expected+=(double)ir[j]*in[i-j];
   if(!std::isfinite(out[i]))return 1e10;
   err=std::max(err,std::abs(expected-out[i]));energy+=(double)out[i]*out[i];
 }
 if(energy<1e-3)return 1e10;
 return err;
}
int main(){
 const size_t N=10000,L=1027;std::vector<float> in(N),ir(L,0);
 uint32_t state=0x1742cafe;
 for(size_t i=0;i<N-2*L;++i){state^=state<<13;state^=state>>17;state^=state<<5;in[i]=(float)((int32_t)(state&65535)-32768)/131072;}
 ir[0]=.6f;ir[17]=-.2f;ir[128]=.15f;ir[511]=-.08f;ir[1026]=.03f;
 for(bool fragments:{false,true}){
   fftconvolver::FFTConvolver a;fftconvolver::TwoStageFFTConvolver b;
   if(!a.init(32,ir.data(),ir.size())||!b.init(32,256,ir.data(),ir.size()))return 1;
   double x=run(in,ir,a,fragments),y=run(in,ir,b,fragments);
   std::printf("FIXED_CONV fragments=%d single_error=%.9g two_stage_error=%.9g frames=%zu\n",fragments,x,y,N);
   if(x>2e-5||y>2e-5)return 2;
 }
 std::puts("FFTCONVOLVER_BASELINE_OK cases=4");return 0;
}
