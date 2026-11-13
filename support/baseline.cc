#include "dwarf/internal.hh"
#include <cstring>
#include <cstdio>
struct Fixture : dwarf::loader {
 unsigned char info[11]={7,0,0,0,4,0,0,0,0,0,8};
 unsigned char abbrev[1]={0};
 unsigned char frame[19]={15,0,0,0,0xff,0xff,0xff,0xff,3,0,1,0x78,16,0x0c,7,8,0x90,1,0};
 const void *load(dwarf::section_type s, size_t *n) override {
 if(s==dwarf::section_type::info) { *n=sizeof(info);return info; }
 if(s==dwarf::section_type::abbrev) { *n=sizeof(abbrev);return abbrev; }
 if(s==dwarf::section_type::frame) { *n=sizeof(frame);return frame; }
 *n=0;return nullptr;
 }
};
int main() {
 auto l=std::make_shared<Fixture>(); dwarf::dwarf file(l);
 auto s=file.get_section(dwarf::section_type::frame); dwarf::cursor cur(s);
 auto len=cur.fixed<uint32_t>(); auto id=cur.fixed<uint32_t>(); auto ver=cur.fixed<uint8_t>();
 std::string aug;cur.string(aug);auto ca=cur.uleb128();auto da=cur.sleb128();
 std::printf("CU=%zu frame=%zu CIE=%u version=%u code_align=%llu data_align=%lld\n",file.compilation_units().size(),s->size(),id,ver,(unsigned long long)ca,(long long)da);
 return file.compilation_units().size()!=1 || len!=15 || id!=0xffffffffu || ver!=3 || ca!=1 || da!=-8;
}
