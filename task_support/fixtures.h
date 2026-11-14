#ifndef TE_FIXTURES_H
#define TE_FIXTURES_H
#include <cstdint>
#include <vector>
#include <cstring>
namespace fixtures {
using Bytes=std::vector<std::uint8_t>;
inline void w16(Bytes&b,unsigned o,std::uint16_t v){b.at(o)=v;b.at(o+1)=v>>8;}
inline void w32(Bytes&b,unsigned o,std::uint32_t v){w16(b,o,v);w16(b,o+2,v>>16);}
inline void w64(Bytes&b,unsigned o,std::uint64_t v){w32(b,o,v);w32(b,o+4,v>>32);}
inline void section(Bytes&b,unsigned o,const char*n,unsigned rva,unsigned raw,unsigned size){
 std::memcpy(b.data()+o,n,std::strlen(n));w32(b,o+8,size);w32(b,o+12,rva);w32(b,o+16,size);w32(b,o+20,raw);w32(b,o+36,0x60000020);
}
inline Bytes pe(){
 Bytes b(0x400);w16(b,0,0x5a4d);w32(b,0x3c,0x80);w32(b,0x80,0x4550);
 w16(b,0x84,0x14c);w16(b,0x86,1);w16(b,0x94,224);w16(b,0x96,0x102);
 unsigned o=0x98;w16(b,o,0x10b);w32(b,o+16,0x1000);w32(b,o+20,0x1000);w32(b,o+28,0x400000);
 w32(b,o+32,0x1000);w32(b,o+36,0x200);w32(b,o+56,0x2000);w32(b,o+60,0x200);w16(b,o+68,10);w32(b,o+92,16);
 section(b,0x178,".text",0x1000,0x200,0x200);b[0x200]=0xc3;return b;
}
// Header follows PI TE layout. Raw fields still refer to the pre-stripping PE.
inline Bytes te(bool wide=true,unsigned stripped=0x1a0){
 const unsigned bias=stripped-40;Bytes b(0x500-bias);w16(b,0,0x5a56);w16(b,2,wide?0x8664:0x14c);
 b[4]=2;b[5]=10;w16(b,6,stripped);w32(b,8,0x1000);w32(b,12,0x1000);w64(b,16,wide?0x140000000ULL:0x400000ULL);
 w32(b,24,0x2020);w32(b,28,12);w32(b,32,0x1040);w32(b,36,28);
 section(b,40,".text",0x1000,0x200,0x200);section(b,80,".reloc",0x2000,0x400,0x100);
 b[0x200-bias]=0xc3;unsigned r=0x420-bias;w32(b,r,0x1000);w32(b,r+4,12);w16(b,r+8,wide?0xa008:0x3008);
 unsigned d=0x240-bias;w32(b,d+12,2);w32(b,d+16,24);w32(b,d+20,0x10a0);w32(b,d+24,0x2a0);
 unsigned p=0x2a0-bias;std::memcpy(b.data()+p,"RSDS",4);w32(b,p+20,7);return b;
}
}
#endif
