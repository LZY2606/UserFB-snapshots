#include <elfio/elfio.hpp>
#include <sstream>
#include <iostream>
#include <cstring>
using namespace ELFIO;
int main() {
 elfio w;w.create(ELFCLASS64,ELFDATA2LSB);w.set_type(ET_REL);w.set_machine(EM_RISCV);
 auto t=w.sections.add(".text");t->set_type(SHT_PROGBITS);t->set_flags(SHF_ALLOC|SHF_EXECINSTR);t->set_addr_align(2);
 const char code[]={0x17,0x05,0,0,0x13,0x05,0x05,0,0x23,0x20,0x15,0};t->set_data(code,sizeof(code));
 auto s=w.sections.add(".strtab");s->set_type(SHT_STRTAB);string_section_accessor strings(s);
 auto y=w.sections.add(".symtab");y->set_type(SHT_SYMTAB);y->set_link(s->get_index());y->set_info(2);y->set_entry_size(w.get_default_entry_size(SHT_SYMTAB));
 symbol_section_accessor sy(w,y);auto anchor=sy.add_symbol(strings,".Lanchor",0,0,STB_LOCAL,STT_NOTYPE,0,t->get_index());auto target=sy.add_symbol(strings,"target",0x1800,0,STB_GLOBAL,STT_OBJECT,0,SHN_ABS);
 auto r=w.sections.add(".rela.text");r->set_type(SHT_RELA);r->set_info(t->get_index());r->set_link(y->get_index());r->set_entry_size(w.get_default_entry_size(SHT_RELA));relocation_section_accessor rel(w,r);
 rel.add_entry(4,anchor,R_RISCV_PCREL_LO12_I,0);rel.add_entry(0,target,R_RISCV_PCREL_HI20,0);rel.add_entry(0,0,R_RISCV_RELAX,0);
 std::stringstream stream(std::ios::in|std::ios::out|std::ios::binary);if(!w.save(stream))return 1;
 elfio rd;stream.seekg(0);if(!rd.load(stream))return 2;
 auto text=rd.sections[".text"];if(!text || text->get_size()!=sizeof(code) || std::memcmp(text->get_data(),code,sizeof(code)))return 3;
 const_relocation_section_accessor rr(rd,rd.sections[".rela.text"]);
 if(rr.get_entries_num()!=3)return 4;
 for(Elf_Xword i=0;i<rr.get_entries_num();i++){Elf64_Addr off;Elf_Word sym;unsigned type;Elf_Sxword add;if(!rr.get_entry(i,off,sym,type,add))return 5;std::cout<<"RV_RELA index="<<i<<" off="<<off<<" sym="<<sym<<" type="<<type<<" add="<<add<<"\n";}
 std::cout<<"RV_OBJECT_BASELINE_OK unchanged_instruction_bytes=12\n";
}
