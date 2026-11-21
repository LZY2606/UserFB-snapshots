#include "AudioFile.h"
#include <cstdio>
#include <cstdlib>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"normal path failed %d: %s\n",__LINE__,#x); return 1; } } while(0)
using Bytes=std::vector<uint8_t>;
static uint32_t u32(const Bytes& b,size_t p) {
    return uint32_t(b[p])|(uint32_t(b[p+1])<<8)|(uint32_t(b[p+2])<<16)|(uint32_t(b[p+3])<<24);
}
static void put32(Bytes& b,size_t p,uint32_t v) {
    for(unsigned k=0;k<4;++k)b[p+k]=uint8_t(v>>(8*k));
}
static size_t chunk(const Bytes& b,const char* id) {
    for(size_t p=12;p+8<=b.size();) {
        const size_t n=u32(b,p+4);
        if(n>b.size()-p-8)return 0;
        if(!std::memcmp(b.data()+p,id,4))return p;
        p+=8+n+(n&1);
    }
    return 0;
}
static Bytes withBext(const Bytes& wav) {
    Bytes b(wav.begin(),wav.begin()+12);
    b.insert(b.end(),{'b','e','x','t',0x5a,2,0,0});
    b.resize(b.size()+602,0);
    b[20+346]=1;
    const char name[]="AudioFile normal PCM provenance";
    std::copy(name,name+sizeof(name),b.begin()+20);
    b.insert(b.end(),wav.begin()+12,wav.end());
    put32(b,4,uint32_t(b.size()-8));
    return b;
}
int main(int argc,char** argv) {
    size_t observed=0;
    for(int bits: {8,16,24,32}) {
        AudioFile<double> a;
        a.shouldLogErrorsToConsole(false);
        a.setAudioBufferSize(2,1200);
        a.setSampleRate(48000);
        a.setBitDepth(bits);
        for(int i=0;i<1200;++i) {
            a.samples[0][i]=double((i%31)-15)/32.;
            a.samples[1][i]=-a.samples[0][i];
        }
        const auto original=a.samples;
        Bytes wav;
        REQUIRE(a.saveToMemory(wav));
        REQUIRE(a.samples==original);
        REQUIRE(u32(wav,4)==wav.size()-8);
        size_t d=chunk(wav,"data"),f=chunk(wav,"fmt ");
        REQUIRE(d && f);
        REQUIRE(u32(wav,d+4)==1200u*2u*unsigned(bits/8));
        AudioFile<double> read;
        read.shouldLogErrorsToConsole(false);
        REQUIRE(read.loadFromMemory(wav));
        REQUIRE(read.getNumSamplesPerChannel()==1200 && read.getNumChannels()==2);
        REQUIRE(read.getSampleRate()==48000 && read.getBitDepth()==bits);
        for(int c=0;c<2;++c)for(int i=0;i<1200;++i) {
            const double bound=bits==8?1./127.:bits==16?1./32767.:bits==24?1./8388607.:1e-7;
            REQUIRE(std::fabs(read.samples[c][i]-original[c][i])<=bound+1e-12);
            ++observed;
        }
        std::printf("WAVE_NORMAL bits=%d channels=2 frames=1200 data_bytes=%u\n",bits,u32(wav,d+4));
        if(bits==16 && argc>1 && std::string(argv[1])=="--inspect-gap") {
            Bytes bwf=withBext(wav),saved;
            AudioFile<double> source;
            source.shouldLogErrorsToConsole(false);
            REQUIRE(source.loadFromMemory(bwf));
            REQUIRE(source.saveToMemory(saved));
            REQUIRE(source.getNumSamplesPerChannel()==1200);
            std::printf("STARTING_BWF source_bext=%d ordinary_save_bext=%d\n",chunk(bwf,"bext")!=0,chunk(saved,"bext")!=0);
        }
    }
    AudioFile<double> quiet;
    quiet.setAudioBufferSize(1,48000);
    quiet.setSampleRate(48000); quiet.setBitDepth(8);
    for(int i=0;i<48000;++i)quiet.samples[0][i]=.003+.002*std::sin(2.*3.141592653589793*1000.*i/48000.);
    Bytes q; REQUIRE(quiet.saveToMemory(q));
    size_t d=chunk(q,"data"); REQUIRE(d);
    for(size_t p=d+8;p<d+8+u32(q,d+4);++p)REQUIRE(q[p]==128);
    std::printf("QUANTIZATION positive_modulation_frames=48000 unsigned_silence_bytes=48000\n");
    std::printf("NORMAL_COMPLETE sample_checks=%zu\n",observed);
    return 0;
}
