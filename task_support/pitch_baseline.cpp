// Existing pitch-shift playback witness only. No phase-lock implementation/reference.
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
static constexpr int frames=32768;
static constexpr double pi=3.14159265358979323846;
static void need(bool ok,const char *msg) {
    if(!ok) { std::fprintf(stderr,"PITCH_BASELINE_FAIL %s AL=%x\n",msg,alGetError());std::exit(1); }
}
static std::vector<float> render(int coarse,bool split) {
    ALCdevice *d=alcLoopbackOpenDeviceSOFT(nullptr);need(d,"open");
    ALCint attrs[]={ALC_FREQUENCY,48000,ALC_FORMAT_CHANNELS_SOFT,ALC_STEREO_SOFT,
      ALC_FORMAT_TYPE_SOFT,ALC_FLOAT_SOFT,ALC_OUTPUT_LIMITER_SOFT,ALC_FALSE,
      ALC_HRTF_SOFT,ALC_FALSE,0};
    ALCcontext *c=alcCreateContext(d,attrs);need(c && alcMakeContextCurrent(c),"context");
    alDistanceModel(AL_NONE);
    ALuint b{},s{},e{},a{},f{};
    alGenBuffers(1,&b);alGenSources(1,&s);alGenEffects(1,&e);
    alGenAuxiliaryEffectSlots(1,&a);alGenFilters(1,&f);
    alEffecti(e,AL_EFFECT_TYPE,AL_EFFECT_PITCH_SHIFTER);
    alEffecti(e,AL_PITCH_SHIFTER_COARSE_TUNE,coarse);
    alEffecti(e,AL_PITCH_SHIFTER_FINE_TUNE,0);
    alAuxiliaryEffectSloti(a,AL_EFFECTSLOT_EFFECT,e);
    alFilteri(f,AL_FILTER_TYPE,AL_FILTER_LOWPASS);
    alFilterf(f,AL_LOWPASS_GAIN,0.0f);alFilterf(f,AL_LOWPASS_GAINHF,1.0f);
    std::vector<float> input(frames);
    for(int n=0;n<frames;++n)input[n]=0.06f*std::sin(2*pi*750*n/48000);
    alBufferData(b,AL_FORMAT_MONO_FLOAT32,input.data(),int(input.size()*sizeof(float)),48000);
    alSourcei(s,AL_BUFFER,b);alSourcei(s,AL_SOURCE_RELATIVE,AL_TRUE);alSource3f(s,AL_POSITION,0,0,0);
    alSourcei(s,AL_DIRECT_FILTER,f);alSource3i(s,AL_AUXILIARY_SEND_FILTER,a,0,AL_FILTER_NULL);
    alSourcePlay(s);need(alGetError()==AL_NO_ERROR,"wet pitch chain");
    std::vector<float> output(frames*2);int off=0,index=0;const int sizes[]={1,7,257,1025};
    while(off<frames) {int count=split?std::min(sizes[index++%4],frames-off):frames;
      alcRenderSamplesSOFT(d,output.data()+2*off,count);off+=count;}
    need(alGetError()==AL_NO_ERROR && alcGetError(d)==ALC_NO_ERROR,"render");
    for(float x:output)need(std::isfinite(x),"finite PCM");
    alDeleteSources(1,&s);alDeleteBuffers(1,&b);alDeleteAuxiliaryEffectSlots(1,&a);
    alDeleteEffects(1,&e);alDeleteFilters(1,&f);alcMakeContextCurrent(nullptr);alcDestroyContext(c);
    need(alcCloseDevice(d),"close");return output;
}
static double amplitude(const std::vector<float>& x,int channel,double freq) {
    double re=0,im=0;for(int n=8192;n<24576;++n) {
      const double angle=2*pi*freq*n/48000;re+=x[2*n+channel]*std::cos(angle);im+=x[2*n+channel]*std::sin(angle);}
    return 2*std::hypot(re,im)/16384;
}
int main() {
    const auto unity=render(0,false);const auto octave=render(12,false);const auto split=render(12,true);
    double error=0;for(size_t i=8192*2;i<octave.size();++i)error=std::max(error,std::abs(double(octave[i]-split[i])));
    for(int c=0;c<2;++c) {
      const double un=amplitude(unity,c,750),up=amplitude(octave,c,1500),dry=amplitude(octave,c,750);
      std::printf("LEGACY_PITCH_PCM channel=%d unity=%.9g octave=%.9g dry=%.9g\n",c,un,up,dry);
      need(un>0.005 && up>0.005 && dry<up*.02,"old pitch frequencies");
    }
    std::printf("LEGACY_PITCH_CHUNKS max_error=%.9g\n",error);need(error<2e-5,"old chunk continuity");
    std::puts("PITCH_STARTPOINT_OK");
}
