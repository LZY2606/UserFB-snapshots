#include "soloud.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
class ConstantInstance : public SoLoud::AudioSourceInstance {
public:
 unsigned int getAudio(float *p,unsigned int n,unsigned int) override {for(unsigned i=0;i<n;++i)p[i]=.25f;return n;}
 bool hasEnded() override{return false;}
};
class Constant : public SoLoud::AudioSource {
public:
 Constant(){mBaseSamplerate=48000;mChannels=1;}
 SoLoud::AudioSourceInstance *createInstance() override{return new ConstantInstance;}
};
static std::vector<float> render(unsigned block,bool fade,bool global){
 Constant source;SoLoud::Soloud s;
 if(s.init(0,SoLoud::Soloud::NULLDRIVER,48000,512,1)!=0)std::exit(2);
 s.setPostClipScaler(1);s.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);
 auto h=s.play(source);float warm[1024];s.mix(warm,1024);
 if(fade){if(global)s.fadeGlobalVolume(0,64.0/48000);else s.fadeVolume(h,0,64.0/48000);}
 std::vector<float> v(128);
 for(unsigned n=0;n<128;n+=block)s.mix(v.data()+n,block);
 s.stopAll();return v;
}
int main(){
 auto normal=render(128,false,false);for(float f:normal)if(!std::isfinite(f)||std::fabs(f-normal[0])>1e-5||std::fabs(f)<.01f)return 3;
 std::printf("CONSTANT_VALUE=%.9f\n",normal[0]);
 std::puts("CONSTANT_PCM_OK frames=128");
 for(int global=0;global<2;++global){auto a=render(128,true,global),b=render(16,true,global);float delta=0,tail=0;
  for(unsigned i=0;i<128;++i){if(!std::isfinite(a[i])||!std::isfinite(b[i]))return 4;delta=std::fmax(delta,std::fabs(a[i]-b[i]));if(i>=64)tail=std::fmax(tail,std::fabs(a[i]));}
  std::printf("%s BLOCK_MAX_DIFF=%.9f POST_END_PEAK=%.9f\n",global?"GLOBAL":"VOICE",delta,tail);
 }
}
