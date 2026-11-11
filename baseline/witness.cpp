// Starting-point smoke only: no rotation oracle, HOA suite or mutation runner.
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void require(bool v, const char *s) {
    if(!v) { std::fprintf(stderr,"FAIL: %s (AL=%x)\n",s,alGetError()); std::exit(1); }
}
int main() {
    auto *dev=alcLoopbackOpenDeviceSOFT(nullptr);
    require(dev!=nullptr,"loopback open");
    const ALCint attrs[]={ALC_FREQUENCY,48000,
        ALC_FORMAT_CHANNELS_SOFT,ALC_BFORMAT3D_SOFT,
        ALC_FORMAT_TYPE_SOFT,ALC_FLOAT_SOFT,
        ALC_AMBISONIC_LAYOUT_SOFT,ALC_ACN_SOFT,
        ALC_AMBISONIC_SCALING_SOFT,ALC_N3D_SOFT,
        ALC_AMBISONIC_ORDER_SOFT,1,ALC_OUTPUT_LIMITER_SOFT,ALC_FALSE,0};
    auto *ctx=alcCreateContext(dev,attrs);
    require(ctx && alcMakeContextCurrent(ctx),"FOA context");
    require(alIsExtensionPresent("AL_SOFT_bformat_ex"),"bformat extension");
    constexpr int frames=4096, channels=4;
    std::vector<float> silent(frames*channels,99.0f);
    alcRenderSamplesSOFT(dev,silent.data(),frames);
    for(float x:silent) require(x==0.0f,"empty mix silence");
    std::puts("EMPTY_MIX_SILENCE frames=4096 channels=4");
    ALuint b{},s{};
    alGenBuffers(1,&b); alGenSources(1,&s);
    alBufferi(b,AL_AMBISONIC_LAYOUT_SOFT,AL_ACN_SOFT);
    alBufferi(b,AL_AMBISONIC_SCALING_SOFT,AL_N3D_SOFT);
    std::vector<float> in(frames*channels,0.0f),out(in.size());
    for(int n=0;n<frames;++n) in[n*channels+3]=0.05f*std::sin(n*0.071f);
    alBufferData(b,AL_FORMAT_BFORMAT3D_FLOAT32,in.data(),int(in.size()*sizeof(float)),48000);
    alSourcei(s,AL_BUFFER,b); alSourcei(s,AL_SOURCE_RELATIVE,AL_TRUE);
    alSource3f(s,AL_POSITION,0,0,0); alDistanceModel(AL_NONE);
    alSourcePlay(s); require(alGetError()==AL_NO_ERROR,"source start");
    alcRenderSamplesSOFT(dev,out.data(),frames);
    require(alcGetError(dev)==ALC_NO_ERROR,"render");
    double maxerr=0.0,leak=0.0,energy=0.0;
    // Discard the mixer's initial gain ramp, then check actual PCM, not API echo.
    for(int n=1024;n<frames;++n) {
        maxerr=std::max(maxerr,std::abs(double(out[n*channels+3]-in[n*channels+3])));
        energy+=out[n*channels+3]*out[n*channels+3];
        for(int c=0;c<3;++c) leak=std::max(leak,std::abs(double(out[n*channels+c])));
    }
    require(energy>1.0 && maxerr<1e-5 && leak<1e-6,"identity PCM route");
    std::printf("FOA_IDENTITY_PCM samples=3072 max_error=%.9g leakage=%.9g energy=%.9g\n",maxerr,leak,energy);
    alDeleteSources(1,&s); alDeleteBuffers(1,&b);
    alcMakeContextCurrent(nullptr); alcDestroyContext(ctx); require(alcCloseDevice(dev),"close");
    std::puts("STARTPOINT_OK");
}
