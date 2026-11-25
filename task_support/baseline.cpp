#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_filter.h"
#include <vector>
#include <cmath>
#include <cstdio>
struct PulseInstance:SoLoud::AudioSourceInstance {
 unsigned pos=0;
 unsigned getAudio(float*b,unsigned n,unsigned stride) override {
  for(unsigned c=0;c<mChannels;++c)for(unsigned i=0;i<n;++i)b[c*stride+i]=(pos+i==4096+c*3)?(0.1f*(c+1)):0.f;
  pos+=n;return n;
 }
 bool hasEnded() override{return false;}
};
struct Pulse:SoLoud::AudioSource {
 Pulse(){mChannels=2;mBaseSamplerate=48000;}
 SoLoud::AudioSourceInstance*createInstance()override{return new PulseInstance;}
};
struct DelayInstance:SoLoud::FilterInstance {
 unsigned d,p=0;std::vector<float> ring;
 explicit DelayInstance(unsigned n):d(n),ring(n*8,0.f){}
 void filter(float*b,unsigned n,unsigned stride,unsigned channels,float,SoLoud::time)override{
  if(!d)return;
  for(unsigned i=0;i<n;++i){for(unsigned c=0;c<channels;++c){float &v=ring[c*d+p];float x=b[c*stride+i];b[c*stride+i]=v;v=x;}p=(p+1)%d;}
 }
};
struct Delay:SoLoud::Filter {
 unsigned d;explicit Delay(unsigned n):d(n){}
 SoLoud::FilterInstance*createInstance()override{return new DelayInstance(d);}
};
int main(){
 for(unsigned d: {0u,37u,701u}){
  SoLoud::Soloud engine;Pulse pulse;Delay delay(d);SoLoud::Bus bus;
  if(engine.init(0,SoLoud::Soloud::NULLDRIVER,48000,1024,2))return 1;
  engine.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);engine.setPostClipScaler(1);
  SoLoud::handle h;
  if(d==701){bus.setChannels(2);bus.mBaseSamplerate=48000;bus.setResampler(SoLoud::Soloud::RESAMPLER_POINT);bus.setFilter(0,&delay);auto bh=engine.play(bus);engine.setPanAbsolute(bh,1,1);h=bus.play(pulse);}
  else{pulse.setFilter(0,&delay);h=engine.play(pulse);}
  engine.setPanAbsolute(h,1,1);engine.setAutoStop(h,false);
  std::vector<float> warm(2048*2);engine.mix(warm.data(),2048);
  std::vector<float> out(4096*2);engine.mix(out.data(),4096);
  for(unsigned c=0;c<2;++c)for(unsigned i=0;i<4096;++i){float want=i==2048+d+3*c?0.1f*(c+1):0.f;if(std::fabs(out[2*i+c]-want)>1e-6f){std::fprintf(stderr,"mismatch delay=%u frame=%u ch=%u got=%g want=%g\n",d,i,c,out[2*i+c],want);return 2;}}
  std::printf("BASELINE delay=%u channels=2 frames=4096 PCM_OK\n",d);engine.deinit();
 }
}
