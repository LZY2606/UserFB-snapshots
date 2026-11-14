#ifndef MOTION_BASELINE_FIXTURES_H
#define MOTION_BASELINE_FIXTURES_H
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include "Horde3D.h"
namespace motion_fixture {
struct Bytes {
    std::vector<char> data;
    void u32(uint32_t v) { for(int i=0;i<4;++i) data.push_back(char(v>>(8*i))); }
    void u16(int v) { data.push_back(char(v)); data.push_back(char(v>>8)); }
    void u8(int v) { data.push_back(char(v)); }
    void f32(float v) { uint32_t bits; std::memcpy(&bits,&v,4); u32(bits); }
    void text(const char *s, size_t n) { size_t len=std::strlen(s); for(size_t i=0;i<n;++i) data.push_back(i<len?s[i]:0); }
    void matrix(float x,float y,float z) { for(int i=0;i<16;++i) f32(i==12?x:i==13?y:i==14?z:(i%5==0?1.f:0.f)); }
    void frame(float yaw,float x,float y,float z) {
        f32(0); f32(std::sin(yaw/2)); f32(0); f32(std::cos(yaw/2));
        f32(x); f32(y); f32(z); f32(1); f32(1); f32(1);
    }
};
inline Bytes animation() {
    Bytes b; b.text("H3DA",4); b.u32(3); b.u32(3); b.u32(3);
    b.text("root",256); b.u8(0);
    for(int i=0;i<3;++i) b.frame(float(i*1.5707963267948966),float(1+2*i),0,0);
    b.text("limb",256); b.u8(0);
    for(int i=0;i<3;++i) b.frame(0,0,float(2+2*i),0);
    b.text("constant",256); b.u8(1); b.frame(0,0,0,1);
    return b;
}
inline Bytes geometry() {
    Bytes b; b.text("H3DG",4); b.u32(5); b.u32(2);
    b.matrix(-1,0,0); b.matrix(-1,-2,0);
    b.u32(6); b.u32(3);
    const float xyz[3][3]={{2,2,0},{1,3,0},{1,2,1}};
    b.u32(0); b.u32(12); for(auto &p:xyz) for(float v:p)b.f32(v);
    for(int stream=1;stream<=3;++stream) {
        b.u32(stream); b.u32(6);
        for(int i=0;i<3;++i) for(int c=0;c<3;++c)b.u16(c==stream-1?32767:0);
    }
    b.u32(4); b.u32(4);
    for(int i=0;i<3;++i) {b.u8(0);b.u8(1);b.u8(0);b.u8(0);}
    b.u32(5); b.u32(4);
    for(int i=0;i<3;++i) {b.u8(128);b.u8(127);b.u8(0);b.u8(0);}
    b.u32(3); b.u32(0); b.u32(1); b.u32(2);
    b.u32(1); b.text("lift",256); b.u32(1); b.u32(0); b.u32(1);
    b.u32(0); b.u32(12); b.f32(0); b.f32(1); b.f32(0);
    return b;
}
inline H3DRes load(int type,const char*name,const Bytes& b) {
    H3DRes r=h3dAddResource(type,name,0);
    return r && h3dLoadResource(r,b.data.data(),int(b.data.size()))?r:0;
}
}
#endif
