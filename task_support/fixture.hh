#ifndef TASK_FIXTURE_HH
#define TASK_FIXTURE_HH
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "elf/elf++.hh"
namespace fixture {
using Bytes = std::vector<unsigned char>;
inline void put(Bytes &b, std::uint64_t v, unsigned n) { for(unsigned i=0;i<n;++i) b.push_back((v>>(8*i))&255); }
inline void patch(Bytes &b, size_t at, std::uint64_t v, unsigned n) { for(unsigned i=0;i<n;++i) b.at(at+i)=(v>>(8*i))&255; }
inline void text(Bytes &b,const char*s) { do {b.push_back(*s);} while(*s++); }
inline Bytes baseline_info() {
 Bytes b; put(b,0,4); put(b,4,2); put(b,0,4); put(b,8,1);
 put(b,1,1); text(b,"baseline-unit"); put(b,2,1); text(b,"entry");
 put(b,0x401020,8); put(b,0x18,4); put(b,0,1); patch(b,0,b.size()-4,4);return b;
}
inline Bytes baseline_abbrev() { return Bytes{1,0x11,1,3,8,0,0,2,0x2e,0,3,8,0x11,1,0x12,6,0,0,0}; }
inline Bytes elf_image(const std::map<std::string,Bytes> &sections) {
 std::map<std::string,Bytes> all=sections; Bytes names{0}; std::map<std::string,size_t> ni;
 ni[".shstrtab"]=names.size();text(names,".shstrtab");
 for(auto &s:sections){ni[s.first]=names.size();text(names,s.first.c_str());}
 all[".shstrtab"]=names;
 Bytes b(64,0); b[0]=0x7f;b[1]='E';b[2]='L';b[3]='F';b[4]=2;b[5]=1;b[6]=1;
 patch(b,16,1,2);patch(b,18,62,2);patch(b,20,1,4);patch(b,52,64,2);patch(b,58,64,2);
 std::map<std::string,size_t> off; for(auto &s:all){off[s.first]=b.size();b.insert(b.end(),s.second.begin(),s.second.end());}
 while(b.size()%8)b.push_back(0);size_t sh=b.size();b.resize(sh+64*(all.size()+1),0);patch(b,40,sh,8);patch(b,60,all.size()+1,2);
 size_t i=1;for(auto&s:all){size_t h=sh+64*i;patch(b,h,ni.at(s.first),4);patch(b,h+4,s.first==".shstrtab"?3:1,4);patch(b,h+24,off[s.first],8);patch(b,h+32,s.second.size(),8);patch(b,h+48,1,8);if(s.first==".shstrtab")patch(b,62,i,2);++i;}
 return b;
}
class Loader:public ::elf::loader {
 Bytes bytes;
 public: explicit Loader(Bytes b):bytes(std::move(b)){}
 const void*load(off_t o,size_t n) override {if(o<0||size_t(o)>bytes.size()||n>bytes.size()-size_t(o))throw std::out_of_range("fixture span");return bytes.data()+o;}
};
}
#endif
