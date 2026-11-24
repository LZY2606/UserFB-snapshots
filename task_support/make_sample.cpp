// MIT; self-authored, valid offline ET_REL specimen writer, no analysis results.
#include <elfio/elfio.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace ELFIO;
static void insn(std::string& s, unsigned op, unsigned dst=0, unsigned src=0,
                 int off=0, std::int32_t imm=0) {
    s.push_back(char(op)); s.push_back(char(dst | (src<<4)));
    s.push_back(char(off)); s.push_back(char(unsigned(off)>>8));
    auto u=std::uint32_t(imm);
    for(unsigned i=0;i<4;++i) s.push_back(char(u>>(8*i)));
}
static void wide(std::string& s, unsigned dst, std::uint32_t low, std::uint32_t high=0) {
    insn(s,0x18,dst,0,0,std::int32_t(low)); insn(s,0,0,0,0,std::int32_t(high));
}
static section* data(elfio& e,const char* name,unsigned type,Elf_Xword flags,
                     const std::string& payload, unsigned align=8) {
    auto* s=e.sections.add(name); s->set_type(type); s->set_flags(flags);
    s->set_addr_align(align); s->set_data(payload); return s;
}
struct Rel {unsigned offset,symbol,type;};
static void write_one(const std::string& path,bool alternate) {
    elfio e; e.create(ELFCLASS64,ELFDATA2LSB); e.set_type(ET_REL); e.set_machine(EM_BPF);
    std::string a,b,m;
    insn(a,0xb7,0,0,0,0); wide(a,1,20); insn(a,0x85,0,1,0,-1);
    insn(a,0x15,0,0,2,0); insn(a,0x85,0,1,0,4); insn(a,0x05,0,0,1);
    insn(a,0x85,0,0,0,5); insn(a,0x85,0,1,0,5); insn(a,0x05,0,0,2);
    wide(a,1,0); insn(a,0xb7,0,0,0,0); insn(a,0x95);
    wide(a,1,0); insn(a,0xb7,0,0,0,1); insn(a,0x95);
    insn(a,0x85,0,1,0,-5); insn(a,0x85,0,1,0,-1);
    insn(a,0x85,0,1,0,-1); insn(a,0x95);
    wide(b,1,0); insn(b,0xb7,0,0,0,0); insn(b,0x85,0,1,0,1); insn(b,0x95);
    insn(b,0xb7,0,0,0,0); insn(b,0x15,0,0,1,0); insn(b,0x05,0,0,-2);
    wide(b,1,20); insn(b,0xb7,0,0,0,3); insn(b,0x95);
    wide(b,1,0x12345678,0x85); insn(b,0xb7,0,0,0,0); insn(b,0x95);
    // Two legacy map definitions: type, key size, value size, max entries, flags.
    for(unsigned n=0;n<2;++n) for(std::uint32_t v:{2u,4u,8u,1u,0u})
        for(unsigned i=0;i<4;++i) m.push_back(char(v>>(8*i)));
    section *as,*bs;
    if(alternate) {bs=data(e,"code_aux",SHT_PROGBITS,SHF_ALLOC|SHF_EXECINSTR,b);
                  as=data(e,"code_entry",SHT_PROGBITS,SHF_ALLOC|SHF_EXECINSTR,a);}
    else {as=data(e,"code_entry",SHT_PROGBITS,SHF_ALLOC|SHF_EXECINSTR,a);
          bs=data(e,"code_aux",SHT_PROGBITS,SHF_ALLOC|SHF_EXECINSTR,b);}
    auto* maps=data(e,"maps",SHT_PROGBITS,SHF_ALLOC|SHF_WRITE,m);
    auto* str=data(e,".strtab",SHT_STRTAB,0,"",1);
    auto* syms=data(e,".symtab",SHT_SYMTAB,0,""); syms->set_link(str->get_index()); syms->set_entry_size(24);
    string_section_accessor strings(str); symbol_section_accessor symbols(e,syms);
    auto sym=[&](const char* n,Elf64_Addr v,Elf_Xword size,unsigned bind,unsigned type,section* sec) {
        return symbols.add_symbol(strings,n,v,size,bind,type,0,sec->get_index());
    };
    sym("",0,0,STB_LOCAL,STT_SECTION,as);
    unsigned sb=sym("",0,0,STB_LOCAL,STT_SECTION,bs);
    unsigned sm=sym("",0,0,STB_LOCAL,STT_SECTION,maps);
    sym("dup",112,32,STB_LOCAL,STT_FUNC,as);
    unsigned shared=sym("shared",0,40,STB_LOCAL,STT_FUNC,bs);
    sym("dup",40,56,STB_LOCAL,STT_FUNC,bs);
    sym("cold",96,32,STB_LOCAL,STT_FUNC,bs);
    unsigned ma=sym("map_a",0,20,STB_LOCAL,STT_OBJECT,maps);
    sym("map_b",20,20,STB_LOCAL,STT_OBJECT,maps);
    syms->set_info(10);
    sym("entry",0,112,STB_GLOBAL,STT_FUNC,as);
    sym("resume",144,32,STB_GLOBAL,STT_FUNC,as);
    auto rel=[&](const char* name,section* target,std::vector<Rel> list) {
        auto* s=data(e,name,SHT_REL,0,""); s->set_link(syms->get_index());
        s->set_info(target->get_index()); s->set_entry_size(16);
        relocation_section_accessor acc(e,s);
        if(alternate) std::reverse(list.begin(),list.end());
        for(auto r:list) acc.add_entry(r.offset,r.symbol,r.type);
    };
    rel("records_x",as,{{160,sb,10},{8,sm,1},{24,shared,10},{80,ma,1},{112,ma,1},{40,sb,10},{152,shared,10}});
    rel("records_y",bs,{{64,sm,1},{0,ma,1}});
    if(!e.save(path)) throw std::runtime_error("save failed");
}
int main(int argc,char** argv) {
    try {if(argc!=2) return 2; write_one(std::string(argv[1])+"/one.o",false);
        write_one(std::string(argv[1])+"/two.o",true); std::cout<<"SAMPLES_WRITTEN count=2\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
