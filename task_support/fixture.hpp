#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
inline std::vector<std::uint8_t> basic_debug_pe(bool wide,bool overlay) {
 std::vector<std::uint8_t> b(0x440);
 auto put=[&](unsigned o,std::uint64_t n,unsigned w){for(unsigned i=0;i<w;i++)b.at(o+i)=std::uint8_t(n>>(8*i));};
 put(0,0x5a4d,2);put(0x3c,0x80,4);put(0x80,0x4550,4);put(0x84,wide?0x8664:0x14c,2);put(0x86,1,2);put(0x94,wide?240:224,2);put(0x96,0x102,2);
 unsigned o=0x98;put(o,wide?0x20b:0x10b,2);put(o+16,0x1000,4);put(o+20,0x1000,4);
 if(wide)put(o+24,0x140000000ULL,8);else put(o+28,0x400000,4);
 put(o+32,0x1000,4);put(o+36,0x200,4);put(o+56,0x2000,4);put(o+60,0x200,4);put(o+68,3,2);
 unsigned dd=o+(wide?112:96);put(dd-4,16,4);put(dd+6*8,0x1000,4);put(dd+6*8+4,28,4);
 unsigned s=o+(wide?240:224);std::memcpy(b.data()+s,".rdata",6);put(s+8,0x200,4);put(s+12,0x1000,4);put(s+16,0x200,4);put(s+20,0x200,4);put(s+36,0x40000040,4);
 unsigned payload=overlay?0x400:0x280;
 put(0x200+12,2,4);put(0x200+16,33,4);put(0x200+20,overlay?0:0x1080,4);put(0x200+24,payload,4);
 std::memcpy(b.data()+payload,"RSDS",4);for(unsigned i=0;i<16;i++)b[payload+4+i]=i+1;put(payload+20,1,4);std::memcpy(b.data()+payload+24,"demo.pdb",9);
 return b;
}
