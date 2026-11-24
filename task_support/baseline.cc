#include "zita-resampler/resampler.h"
#include "zita-resampler/vresampler.h"
#include <cstdio>
#include <cmath>
#include <vector>
template<class R> static int normal(R& r,const char *name){
 std::vector<float> in(4096*2),out(5000*2);
 for(unsigned i=0;i<4096;++i){in[2*i]=std::sin(i*.03);in[2*i+1]=std::cos(i*.071)*.25;}
 r.inp_data=in.data();r.inp_count=4096;r.out_data=out.data();r.out_count=5000;
 if(r.process())return 1;
 unsigned n=5000-r.out_count;
 double sum=0;for(unsigned i=0;i<n*2;++i){if(!std::isfinite(out[i]))return 2;sum+=out[i]*out[i];}
 if(n<3000||sum<1)return 3;
 std::printf("%s frames=%u remaining=%u energy=%.9f distance=%.12f\n",name,n,r.inp_count,sum,r.inpdist());return 0;
}
int main(){
 Resampler r;if(r.setup(44100,48000,2,32)||normal(r,"LEGACY_FIXED_OK"))return 1;
 VResampler v;if(v.setup(48000.0/44100,2,32)||normal(v,"LEGACY_VARIABLE_OK"))return 2;
 Resampler unusual;int rc=unusual.setup(48000,47999,2,32);
 std::printf("COPRIME_RATE setup_rc=%d status=%s\n",rc,rc?"rejected":"accepted");
 return 0;
}
