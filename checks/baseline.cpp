#include "soloud.h"
#include "soloud_bus.h"
#include "soloud_duckfilter.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
static void check(bool b) { if(!b) throw std::runtime_error("baseline mismatch"); }
struct Generated : SoLoud::AudioSource {
    struct Instance : SoLoud::AudioSourceInstance {
        Generated &p; unsigned pos=0;
        Instance(Generated &p):p(p) {}
        unsigned getAudio(float *out,unsigned n,unsigned stride) override {
            n=std::min(n,p.length-pos);
            for(unsigned ch=0;ch<2;++ch) for(unsigned i=0;i<n;++i)
                out[ch*stride+i]=p.pulse ? (pos+i==912 ? 0.5f : 0.0f) : (ch ? -0.125f : 0.25f);
            pos+=n; return n;
        }
        bool hasEnded() override { return pos==p.length; }
    };
    bool pulse; unsigned length;
    Generated(bool pulse=false,unsigned length=8192):pulse(pulse),length(length) {mBaseSamplerate=48000;mChannels=2;}
    Instance *createInstance() override {return new Instance(*this);}
};
static void setup(SoLoud::Soloud &e) {
    check(e.init(0,SoLoud::Soloud::NULLDRIVER,48000,2048,2)==0);
    e.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT); e.setPostClipScaler(1);
}
int main() { try {
    Generated src;
    std::vector<float> direct(4096),nested(4096);
    {SoLoud::Soloud e; setup(e); auto h=e.play(src); e.setPanAbsolute(h,1,1); e.mix(direct.data(),2048); e.mix(direct.data(),2048);}
    {SoLoud::Soloud e; setup(e); SoLoud::Bus outer,inner;
     outer.mBaseSamplerate=inner.mBaseSamplerate=48000;
     outer.setResampler(SoLoud::Soloud::RESAMPLER_POINT); inner.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
     auto a=e.play(outer); auto b=outer.play(inner); auto h=inner.play(src);
     e.setPanAbsolute(a,1,1);e.setPanAbsolute(b,1,1);e.setPanAbsolute(h,1,1);
     e.mix(nested.data(),2048); e.mix(nested.data(),2048); check(e.isValidVoiceHandle(h));}
    if(direct!=nested) {for(unsigned i=0;i<direct.size();++i) if(direct[i]!=nested[i]) {std::cerr<<"first direct/nested difference "<<i<<" "<<direct[i]<<" "<<nested[i]<<"\n";break;}} check(direct==nested); check(std::fabs(direct[800]-0.25f)<1e-6f);check(std::fabs(direct[801]+0.125f)<1e-6f);
    std::cout<<"SOLOUD_BASELINE direct=2048 nested=2048 pcm_equal=1\n";
    {SoLoud::Soloud e; setup(e); SoLoud::Bus detector; detector.mBaseSamplerate=48000;
     detector.setVisualizationEnable(true); detector.setResampler(SoLoud::Soloud::RESAMPLER_POINT);
     auto dh=e.play(detector); e.setPanAbsolute(dh,1,1);
     Generated pulse(true); auto ph=detector.play(pulse);e.setPanAbsolute(ph,1,1);
     std::vector<float> out(1024); e.mix(out.data(),512); e.mix(out.data(),512);
     check(out[800]==0.5f); check(detector.getApproximateVolume(0)==0);
     SoLoud::DuckFilter duck; check(duck.setParams(&e,dh,0,0,0.1f)==0);
     auto f=duck.createInstance();std::vector<float> main(1024,0.25f);
     f->filter(main.data(),512,512,2,48000,0);delete f;
     check(main[400]==0.25f);
     std::cout<<"DUCK_BASELINE pulse=912 block_offset=400 pcm=0.5 visualization=0 legacy_gain=1\n";
    }
    return 0;
} catch(const std::exception &e) {std::cerr<<e.what()<<"\n";return 1;}}
