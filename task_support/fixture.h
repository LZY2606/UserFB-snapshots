#ifndef USERFB_FIXTURE_H
#define USERFB_FIXTURE_H
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "common/dwarf/dwarf2reader.h"
namespace sample {
using Bytes=std::vector<uint8_t>;
inline void put(Bytes& b,uint64_t x,unsigned n){for(unsigned i=0;i<n;++i)b.push_back(uint8_t(x>>(8*i)));}
inline void patch(Bytes& b,size_t at,uint64_t x,unsigned n){for(unsigned i=0;i<n;++i)b.at(at+i)=uint8_t(x>>(8*i));}
inline void text(Bytes& b,const char* s){while(*s)b.push_back(*s++);b.push_back(0);}
struct Image {
 Bytes elf;
 size_t abbrev_at=0,abbrev_size=0,info_at=0,info_size=0;
 google_breakpad::SectionMap sections() const {
  google_breakpad::SectionMap s;
  s[".debug_abbrev"]={elf.data()+abbrev_at,abbrev_size};
  s[".debug_info"]={elf.data()+info_at,info_size};
  return s;
 }
 void write(const char* path)const {std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(elf.data()),elf.size());if(!f)throw std::runtime_error("write sample");}
};
inline Image wrap(const Bytes& a,const Bytes& i){
 Image out;out.elf.resize(64,0);auto& e=out.elf;
 const uint8_t ident[]={0x7f,'E','L','F',2,1,1};for(size_t k=0;k<sizeof ident;++k)e[k]=ident[k];
 patch(e,16,2,2);patch(e,18,62,2);patch(e,20,1,4);patch(e,52,64,2);patch(e,58,64,2);patch(e,60,4,2);patch(e,62,1,2);
 const char names[]="\0.shstrtab\0.debug_abbrev\0.debug_info\0";
 size_t names_at=e.size();e.insert(e.end(),names,names+sizeof names);
 out.abbrev_at=e.size();out.abbrev_size=a.size();e.insert(e.end(),a.begin(),a.end());
 out.info_at=e.size();out.info_size=i.size();e.insert(e.end(),i.begin(),i.end());
 while(e.size()%8)e.push_back(0);size_t shoff=e.size();patch(e,40,shoff,8);e.resize(shoff+64*4,0);
 auto section=[&](size_t slot,uint32_t name,uint32_t type,size_t offset,size_t length){size_t at=shoff+slot*64;patch(e,at,name,4);patch(e,at+4,type,4);patch(e,at+24,offset,8);patch(e,at+32,length,8);patch(e,at+48,1,8);};
 section(1,1,3,names_at,sizeof names);section(2,11,1,out.abbrev_at,a.size());section(3,25,1,out.info_at,i.size());
 return out;
}
// A minimal ELF64 ET_EXEC with two non-allocated lawful DWARF4 sections.
inline Image fanout() {
 Bytes a={1,0x11,1,0x13,0x05,0x03,0x08,0,0,
  2,0x2e,0,0x11,0x01,0x12,0x06,0x31,0x13,0,0,
  3,0x2e,0,0x03,0x08,0x20,0x0b,0,0,
  4,0x2e,0,0x11,0x01,0x12,0x06,0x03,0x08,0,0,0};
 Bytes i;put(i,0,4);put(i,4,2);put(i,0,4);put(i,8,1);
 put(i,1,1);put(i,4,2);text(i,"sample.cc");
 put(i,4,1);put(i,0x2000,8);put(i,0x10,4);text(i,"normal");
 put(i,2,1);put(i,0x1000,8);put(i,0x10,4);size_t r1=i.size();put(i,0,4);
 put(i,2,1);put(i,0x1100,8);put(i,0x10,4);size_t r2=i.size();put(i,0,4);
 size_t target=i.size();put(i,3,1);text(i,"late");put(i,1,1);put(i,0,1);
 patch(i,r1,target,4);patch(i,r2,target,4);patch(i,0,i.size()-4,4);
 return wrap(a,i);
}
// Two same-address concrete functions in CU1 refer to distinct later origins.
inline Image cross_cu() {
 Bytes a={1,0x11,1,0x13,0x05,0x03,0x08,0,0,
  2,0x2e,0,0x11,0x01,0x12,0x06,0x31,0x10,0,0,
  3,0x2e,0,0x03,0x08,0x20,0x0b,0,0,0};
 Bytes i;auto cu=[&](const char* name){size_t at=i.size();put(i,0,4);put(i,4,2);put(i,0,4);put(i,8,1);put(i,1,1);put(i,4,2);text(i,name);return at;};
 auto first=cu("first.cc");
 put(i,2,1);put(i,0x3000,8);put(i,0x10,4);size_t r1=i.size();put(i,0,4);
 put(i,2,1);put(i,0x3000,8);put(i,0x10,4);size_t r2=i.size();put(i,0,4);
 put(i,0,1);patch(i,first,i.size()-first-4,4);
 auto second=cu("second.cc");
 size_t alpha=i.size();put(i,3,1);text(i,"Alpha");put(i,1,1);
 size_t beta=i.size();put(i,3,1);text(i,"Beta");put(i,1,1);
 put(i,0,1);patch(i,second,i.size()-second-4,4);
 patch(i,r1,alpha,4);patch(i,r2,beta,4);
 return wrap(a,i);
}

// Raw DWARF4: two inline instances in host reference distinct unnamed
// declarations, which both specify the same later named declaration.
inline Image inline_convergence() {
 Bytes a={1,0x11,1,0x13,0x05,0x03,0x08,0,0,
  2,0x2e,1,0x11,0x01,0x12,0x06,0x03,0x08,0,0,
  3,0x1d,0,0x11,0x01,0x12,0x06,0x31,0x13,0x59,0x0b,0,0,
  4,0x2e,0,0x47,0x13,0x20,0x0b,0,0,
  5,0x2e,0,0x03,0x08,0x20,0x0b,0,0,0};
 Bytes i;put(i,0,4);put(i,4,2);put(i,0,4);put(i,8,1);
 put(i,1,1);put(i,4,2);text(i,"inline.cc");
 put(i,2,1);put(i,0x5000,8);put(i,0x40,4);text(i,"host");
 put(i,3,1);put(i,0x5010,8);put(i,4,4);size_t r1=i.size();put(i,0,4);put(i,10,1);
 put(i,3,1);put(i,0x5020,8);put(i,4,4);size_t r2=i.size();put(i,0,4);put(i,20,1);
 put(i,0,1);
 size_t first=i.size();put(i,4,1);size_t s1=i.size();put(i,0,4);put(i,1,1);
 size_t second=i.size();put(i,4,1);size_t s2=i.size();put(i,0,4);put(i,1,1);
 size_t terminal=i.size();put(i,5,1);text(i,"late_inline");put(i,1,1);put(i,0,1);
 patch(i,r1,first,4);patch(i,r2,second,4);patch(i,s1,terminal,4);patch(i,s2,terminal,4);
 patch(i,0,i.size()-4,4);return wrap(a,i);
}

}
#endif
