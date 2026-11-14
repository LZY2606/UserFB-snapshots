// Author supplied synthetic, non-executable PE resource image. No upstream fixture.
#pragma once
#include <vector>
#include <string>
#include <sstream>
#include <stdexcept>
#include "pelib/PeHeader.h"
#include "pelib/MzHeader.h"
inline void put(std::vector<unsigned char>& b, size_t p, unsigned long long v, size_t n) {
 for(size_t i=0;i<n;++i)b.at(p+i)=static_cast<unsigned char>(v>>(8*i));
}
inline std::vector<unsigned char> resource_image() {
 std::vector<unsigned char>b(0x2800);
 put(b,0,0x5a4d,2);put(b,2,0x80,2);put(b,4,1,2);put(b,8,4,2);put(b,0x18,0x40,2);put(b,0x3c,0x80,4);
 put(b,0x80,0x4550,4);put(b,0x84,0x14c,2);put(b,0x86,3,2);put(b,0x94,224,2);put(b,0x96,0x102,2);
 size_t o=0x98;put(b,o,0x10b,2);put(b,o+28,0x400000,4);put(b,o+32,0x1000,4);put(b,o+36,0x200,4);
 put(b,o+56,0x4000,4);put(b,o+60,0x200,4);put(b,o+68,3,2);put(b,o+72,0x100000,4);put(b,o+76,0x1000,4);
 put(b,o+80,0x100000,4);put(b,o+84,0x1000,4);put(b,o+92,16,4);put(b,o+112,0x2000,4);put(b,o+116,0x100,4);
 size_t s=o+224;
 const char* names[]={".early",".rsrc",".more"};
 unsigned va[]={0x1000,0x2000,0x3000}, raw[]={0x200,0x400,0x1800},sz[]={0x200,0x1000,0x1000};
 for(int i=0;i<3;++i){for(size_t j=0;names[i][j];++j)b[s+j]=names[i][j];put(b,s+8,sz[i],4);put(b,s+12,va[i],4);put(b,s+16,sz[i],4);put(b,s+20,raw[i],4);put(b,s+36,0x40000040,4);s+=40;}
 size_t r=0x400;put(b,r+14,1,2);put(b,r+16,16,4);put(b,r+20,0x80000020,4);
 put(b,r+0x20+14,1,2);put(b,r+0x30,1,4);put(b,r+0x34,0x80000040,4);
 put(b,r+0x40+14,1,2);put(b,r+0x50,1033,4);put(b,r+0x54,0x60,4);
 put(b,r+0x60,0x2100,4);put(b,r+0x64,4,4);put(b,r+0x68,1200,4);
 b[0x500]='R';b[0x501]='S';b[0x502]='R';b[0x503]='C';
 return b;
}
inline std::string image_string(const std::vector<unsigned char>& b){return std::string(b.begin(),b.end());}
inline PeLib::PeHeaderT<32> read_header(std::istream& in) {
 PeLib::MzHeader mz;if(mz.read(in)!=PeLib::ERROR_NONE)throw std::runtime_error("DOS read");
 PeLib::PeHeaderT<32> h;if(h.read(in,0x80,mz)!=PeLib::ERROR_NONE)throw std::runtime_error("PE read");return h;
}
