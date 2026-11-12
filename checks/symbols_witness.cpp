#include <elfio/elfio.hpp>
#include <cassert>
#include <sstream>
#include <iostream>
using namespace ELFIO;
int main() {
  for (auto cls : {ELFCLASS32, ELFCLASS64}) for (auto enc : {ELFDATA2LSB, ELFDATA2MSB}) {
    elfio e; e.create(cls, enc); e.set_type(ET_DYN); e.set_machine(EM_X86_64);
    auto *s=e.sections.add(".dynstr"); s->set_type(SHT_STRTAB);
    string_section_accessor names(s);
    auto *d=e.sections.add(".dynsym"); d->set_type(SHT_DYNSYM); d->set_link(s->get_index());
    d->set_entry_size(cls==ELFCLASS64 ? sizeof(Elf64_Sym):sizeof(Elf32_Sym));
    symbol_section_accessor syms(e,d);
    auto i=syms.add_symbol(names,"service",0,0,STB_GLOBAL,STT_FUNC,0,SHN_UNDEF);
    assert(i==1);
    std::stringstream bytes(std::ios::in|std::ios::out|std::ios::binary);
    assert(e.save(bytes)); bytes.seekg(0); elfio loaded; assert(loaded.load(bytes));
    const_symbol_section_accessor a(loaded,loaded.sections[".dynsym"]);
    std::string n; Elf64_Addr value; Elf_Xword size; unsigned char bind,type,other; Elf_Half sh;
    assert(a.get_symbol(1,n,value,size,bind,type,sh,other));
    assert(n=="service" && sh==SHN_UNDEF && bind==STB_GLOBAL);
    std::cout<<"ELF_SYMBOL_ROUNDTRIP class="<<int(cls)<<" encoding="<<int(enc)<<"\n";
  }
}
