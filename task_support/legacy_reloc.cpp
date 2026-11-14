#include <pe-parse/parse.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
static void put(std::vector<std::uint8_t>&b,unsigned o,unsigned v,unsigned n=4){for(unsigned i=0;i<n;i++)b[o+i]=static_cast<std::uint8_t>(v>>(8*i));}
static int relocs(void *p,const peparse::VA &va,const peparse::reloc_type &type){
 unsigned &n=*static_cast<unsigned*>(p);
 if(n==0){assert(va==0x401100 && type==peparse::RELOC_HIGHADJ);}
 else if(n==1){assert(va==0x401234 && static_cast<unsigned>(type)==15);}
 else assert(false);++n;return 0;
}
int main(){
 std::vector<std::uint8_t>b(0x600,0);put(b,0,0x5a4d,2);put(b,0x3c,0x80);put(b,0x80,0x4550);
 put(b,0x84,0x14c,2);put(b,0x86,1,2);put(b,0x94,224,2);put(b,0x96,0x2102,2);
 unsigned o=0x98;put(b,o,0x10b,2);put(b,o+16,0x1000);put(b,o+28,0x400000);
 put(b,o+32,0x1000);put(b,o+36,0x200);put(b,o+56,0x2000);put(b,o+60,0x200);put(b,o+92,16);

 put(b,o+96+5*8,0x1000);put(b,o+96+5*8+4,12);
 unsigned s=o+224;memcpy(&b[s],".reloc",6);
 put(b,s+8,0x400);put(b,s+12,0x1000);put(b,s+16,0x400);put(b,s+20,0x200);put(b,s+36,0x40000040);
 put(b,0x200,0x1000);put(b,0x204,12);put(b,0x208,0x4100,2);put(b,0x20a,0xf234,2);put(b,0x300,0x1234,2);
 const auto original=b;
 auto *p=peparse::ParsePEFromPointer(b.data(),static_cast<std::uint32_t>(b.size()));
 if(!p){fprintf(stderr,"parse failure %s\n",peparse::GetPEErrString().c_str());return 1;}
 unsigned count=0;peparse::IterRelocs(p,relocs,&count);assert(count==2);
 std::uint8_t lo=0,hi=0;assert(peparse::ReadByteAtVA(p,0x401100,lo) && peparse::ReadByteAtVA(p,0x401101,hi));
 assert(lo==0x34 && hi==0x12 && b==original);peparse::DestructParsedPE(p);
 puts("RELOC_ENUM_OK raw_entries=2 pair_nibble=15 target=0x1234 input_unchanged=1");
}
