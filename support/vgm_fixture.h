#ifndef CHECK_VGM_FIXTURE_H
#define CHECK_VGM_FIXTURE_H
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "gme/gme.h"
namespace fixture {
typedef std::vector<unsigned char> Bytes;
inline void u32(Bytes& v, unsigned n) {
    for (int i=0; i<4; ++i) v.push_back((n>>(8*i))&255);
}
inline void put32(Bytes& v, unsigned at, unsigned n) {
    for (int i=0; i<4; ++i) v[at+i]=(n>>(8*i))&255;
}
inline void reg(Bytes& v, unsigned a, unsigned x) {
    v.push_back(0x52); v.push_back(a); v.push_back(x);
}
inline void wait(Bytes& v, unsigned ticks) {
    v.push_back(0x61); v.push_back(ticks&255); v.push_back(ticks>>8);
}
inline Bytes header() {
    Bytes v(0x40,0); std::memcpy(v.data(), "Vgm ",4);
    put32(v,8,0x160); put32(v,0x2c,7670454); put32(v,0x34,0x0c);
    return v;
}
inline void finish(Bytes& v, unsigned ticks) {
    v.push_back(0x66); put32(v,4,unsigned(v.size()-4)); put32(v,0x18,ticks);
}
inline Bytes samples() {
    Bytes v;
    for (unsigned i=0;i<1024;++i) v.push_back((i%32)<16 ? 32:224);
    return v;
}
inline void bank(Bytes& v, const Bytes& p) {
    v.push_back(0x67); v.push_back(0x66); v.push_back(0);
    u32(v,unsigned(p.size())); v.insert(v.end(),p.begin(),p.end());
}
inline Bytes explicit_dac() {
    Bytes v=header(), p=samples(); reg(v,0x2b,0x80);
    for (unsigned i=0;i<p.size();++i) {reg(v,0x2a,p[i]);wait(v,4);}
    wait(v,1000);finish(v,5096);return v;
}
inline Bytes stream_dac() {
    Bytes v=header(),p=samples();bank(v,p);reg(v,0x2b,0x80);
    const unsigned char setup[]={0x90,0,2,0,0x2a,0x91,0,0,1,0,0x92,0};
    v.insert(v.end(),setup,setup+sizeof setup);u32(v,11025);
    v.push_back(0x93);v.push_back(0);u32(v,0);v.push_back(1);u32(v,1024);
    wait(v,4096);v.push_back(0x94);v.push_back(0);
    wait(v,1000);finish(v,5096);return v;
}
inline void check(gme_err_t e) {
    if (e) {std::fprintf(stderr,"gme error: %s\n",e);std::exit(2);}
}
inline std::vector<short> render(const Bytes& v, bool chopped, bool* warning=0) {
    Music_Emu* e=gme_new_emu(gme_vgm_type,44100);
    if (!e) std::exit(2);
    check(gme_load_data(e,v.data(),long(v.size())));
    gme_ignore_silence(e,1);gme_set_autoload_playback_limit(e,0);
    check(gme_start_track(e,0));
    std::vector<short> out(12000);
    const int sizes[]={2,30,514,6,2048,126};
    for (unsigned i=0,k=0;i<out.size();++k) {
        unsigned n=chopped ? unsigned(sizes[k%6]):unsigned(out.size());
        if (n>out.size()-i) n=unsigned(out.size()-i);
        check(gme_play(e,int(n),out.data()+i));i+=n;
    }
    if(warning) *warning=gme_warning(e)!=0;
    gme_delete(e);return out;
}
inline unsigned nonzero(const std::vector<short>& v) {
    unsigned n=0;for(unsigned i=0;i<v.size();++i)n+=v[i]!=0;return n;
}
}
#endif
