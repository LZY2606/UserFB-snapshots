#include <elfio/elfio.hpp>
#include <sstream>
#include <iostream>
#include <stdexcept>
using namespace ELFIO;
static void need(bool b) { if(!b) throw std::runtime_error("baseline mismatch"); }
int main() {
 for(auto cls:{ELFCLASS32,ELFCLASS64}) for(auto enc:{ELFDATA2LSB,ELFDATA2MSB}) {
  elfio e; e.create(cls,enc); e.set_type(ET_REL);e.set_machine(EM_X86_64);
  auto g=e.sections.add(".group");g->set_type(SHT_GROUP);g->set_entry_size(4);g->set_addr_align(4);
  auto text=e.sections.add(".text.f");text->set_type(SHT_PROGBITS);text->set_flags(SHF_GROUP);text->set_data("abc",3);
  auto str=e.sections.add(".strtab");str->set_type(SHT_STRTAB);string_section_accessor strings(str);
  auto sym=e.sections.add(".symtab");sym->set_type(SHT_SYMTAB);sym->set_info(1);sym->set_link(str->get_index());sym->set_entry_size(cls==ELFCLASS32?16:24);sym->set_addr_align(cls==ELFCLASS32?4:8);
  symbol_section_accessor symbols(e,sym);auto si=symbols.add_symbol(strings,"f",0,3,STB_GLOBAL,STT_FUNC,0,text->get_index());
  g->set_link(sym->get_index());g->set_info(si);
  Elf_Word group[]={e.get_convertor()(Elf_Word(GRP_COMDAT)),e.get_convertor()(Elf_Word(text->get_index()))};g->set_data(reinterpret_cast<char*>(group),sizeof group);
  std::stringstream stream;need(e.save(stream));elfio r;need(r.load(stream));
  const_symbol_section_accessor rs(r,r.sections[".symtab"]);std::string n;Elf64_Addr v;Elf_Xword z;unsigned char b,t,o;Elf_Half sec;
  need(rs.get_symbol(si,n,v,z,b,t,sec,o));need(n=="f" && sec==r.sections[".text.f"]->get_index());need(r.sections[".group"]->get_info()==si);
  std::cout<<"class="<<int(cls)<<" endian="<<int(enc)<<" symbol="<<n<<" raw_shndx="<<sec<<" group_bytes="<<r.sections[".group"]->get_size()<<"\n";
 }
 std::cout<<"ELFIO_BASELINE_OK\n";
}
