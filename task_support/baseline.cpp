#include "signalsmith-stretch.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
int main(){
 for(int split=0;split!=2;++split){
  signalsmith::stretch::SignalsmithStretch<float> stretch(8712);
  stretch.configure(2,1024,256,split!=0);
  stretch.setTransposeFactor(1.25f);stretch.setFormantFactor(1.1f);
  std::vector<float> in[2],out[2];
  for(int c=0;c<2;c++){in[c].resize(16384);out[c].resize(16384);}
  for(int i=0;i<16384;i++){float v=0.15f*std::sin(i*0.0572f)+0.05f*std::sin(i*0.1305f);in[0][i]=v;in[1][i]=-0.5f*v;}
  for(int pos=0;pos<16384;){int n=std::min(127,16384-pos);float* ip[]={in[0].data()+pos,in[1].data()+pos};float* op[]={out[0].data()+pos,out[1].data()+pos};stretch.process(ip,n,op,n);pos+=n;}
  double e=0;for(int c=0;c<2;c++)for(float v:out[c]){if(!std::isfinite(v))return 2;e+=v*v;}
  if(e<1.0)return 3;
  std::vector<float> tail[2];tail[0].resize(512);tail[1].resize(512);float* tp[]={tail[0].data(),tail[1].data()};stretch.flush(tp,512);
  double te=0;for(int c=0;c<2;c++)for(float v:tail[c]){if(!std::isfinite(v))return 4;te+=v*v;}
  std::printf("VOCODER_PRODUCTION split=%d frames=16384 energy=%.9f tail=%.9f latency=%d\n",split,e,te,stretch.outputLatency());
 }
 std::puts("BASELINE_COMPLETE denoise_api=absent");return 0;
}
