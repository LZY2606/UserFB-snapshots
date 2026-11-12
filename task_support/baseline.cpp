#include "soloud.h"
#include "soloud_bus.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
struct ToneInstance:SoLoud::AudioSourceInstance{
 unsigned pos=0;
 unsigned getAudio(float*b,unsigned n,unsigned) override {unsigned k=std::min(n,4096-pos);for(unsigned i=0;i<k;i++)b[i]=((pos+i)%32<16?.1f:-.1f);pos+=k;return k;}
 bool hasEnded()override{return pos==4096;}
};
struct Tone:SoLoud::AudioSource{Tone(){mBaseSamplerate=48000;mChannels=1;}ToneInstance*createInstance()override{return new ToneInstance;}};
int main(){SoLoud::Soloud s;assert(s.init(0,SoLoud::Soloud::NULLDRIVER,48000,512,1)==0);SoLoud::Bus bus;assert(bus.setChannels(1)==0);s.play(bus);Tone tone;auto h=bus.play(tone);assert(h);float data[256];s.mix(data,256);double energy=0;for(float x:data)energy+=x*x;assert(energy>.01);assert(s.isValidVoiceHandle(h));s.stop(h);assert(!s.isValidVoiceHandle(h));s.deinit();printf("BASELINE_SOLOUD_PASS bus=1 audible=1 stop=1\n");}
