#include <elfio/elfio.hpp>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
using namespace ELFIO;
static void u32(std::string& s, uint32_t n, int enc) {
  for (int i=0; i<4; ++i) s.push_back(char(n >> (8*(enc==ELFDATA2LSB ? i : 3-i))));
}
static void u16(std::string& s, uint16_t n, int enc) {
  for (int i=0; i<2; ++i) s.push_back(char(n >> (8*(enc==ELFDATA2LSB ? i : 1-i))));
}
static void wordlist(std::string& s, std::initializer_list<uint32_t> words, int enc) {
  for (auto w: words) u32(s,w,enc);
}
static std::string btf_blob(int enc, std::string& strings) {
  auto name=[&](const char* text) { auto n=strings.size(); strings+=text; strings.push_back(0); return uint32_t(n); };
  strings=std::string(1,0);
  auto iname=name("int"), node=name("node"), next=name("next"), arg=name("head"), fun=name("inspect");
  name(".text"); name("0:0"); name("sample.c"); name("return 0;");
  std::string t;
  wordlist(t,{iname,1u<<24,4,0x01000020},enc);                 // 1 INT
  wordlist(t,{0,2u<<24,3},enc);                               // 2 PTR
  wordlist(t,{node,(4u<<24)|1,8,next,2,0},enc);                // 3 STRUCT
  wordlist(t,{iname,1u<<24,4,0x01000020},enc);                 // 4 INT
  wordlist(t,{0,2u<<24,6},enc);                               // 5 PTR
  wordlist(t,{node,(4u<<24)|1,8,next,5,0},enc);                // 6 STRUCT
  wordlist(t,{0,(13u<<24)|1,1,arg,2},enc);                    // 7 FUNC_PROTO
  wordlist(t,{fun,(12u<<24)|1,7},enc);                        // 8 FUNC
  wordlist(t,{0,(13u<<24)|1,4,arg,5},enc);                    // 9 FUNC_PROTO
  wordlist(t,{fun,(12u<<24)|1,9},enc);                        // 10 FUNC instance
  std::string b; u16(b,0xeb9f,enc); b.push_back(1); b.push_back(0);
  wordlist(b,{24,0,uint32_t(t.size()),uint32_t(t.size()),uint32_t(strings.size())},enc);
  return b+t+strings;
}
static std::string ext_blob(int enc, const std::string& strings) {
  auto pos=[&](const char* s) { auto p=strings.find(std::string(s)+char(0)); assert(p!=std::string::npos); return uint32_t(p); };
  std::string f,l,c;
  wordlist(f,{8,pos(".text"),2,0,8,8,10},enc);
  wordlist(l,{16,pos(".text"),1,0,pos("sample.c"),pos("return 0;"),1u<<10},enc);
  wordlist(c,{16,pos(".text"),2,0,3,pos("0:0"),0,8,6,pos("0:0"),0},enc);
  std::string b; u16(b,0xeb9f,enc); b.push_back(1); b.push_back(0);
  wordlist(b,{32,0,uint32_t(f.size()),uint32_t(f.size()),uint32_t(l.size()),uint32_t(f.size()+l.size()),uint32_t(c.size())},enc);
  return b+f+l+c;
}
int main() {
  unsigned count=0;
  for (auto cls : {ELFCLASS32,ELFCLASS64}) for (auto enc : {ELFDATA2LSB,ELFDATA2MSB}) {
    elfio image; image.create(cls,enc); image.set_type(ET_REL); image.set_machine(EM_BPF);
    std::string names; auto btf=btf_blob(enc,names), ext=ext_blob(enc,names);
    auto add=[&](const char* name,const std::string& data) {
      auto s=image.sections.add(name); s->set_type(SHT_PROGBITS); s->set_addr_align(4); s->set_data(data);
    };
    add(".BTF",btf); add(".BTF.ext",ext); add(".text",std::string(16,0));
    std::stringstream file(std::ios::in|std::ios::out|std::ios::binary);
    assert(image.save(file)); file.seekg(0); elfio reader; assert(reader.load(file));
    auto bytes=[&](const char* name){auto s=reader.sections[name]; assert(s);return std::string(s->get_data(),s->get_size());};
    assert(bytes(".BTF")==btf && bytes(".BTF.ext")==ext && bytes(".text")==std::string(16,0));
    std::stringstream second(std::ios::in|std::ios::out|std::ios::binary);
    assert(reader.save(second)); assert(second.str()==file.str());
    ++count;
    std::cout << "BTF_OPAQUE_ROUNDTRIP class=" << int(cls) << " encoding=" << int(enc)
              << " types=10 func_refs=8,10 core_refs=3,6\n";
  }
  std::cout << "BTF_BASELINE_OK combinations=" << count << "\n";
}
