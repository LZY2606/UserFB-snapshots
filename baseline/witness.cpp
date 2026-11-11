#include <elfio/elfio.hpp>
#include <sstream>
#include <cassert>
#include <iostream>
using namespace ELFIO;
template<class Word> void check(unsigned char cls,unsigned char enc){
 elfio w;w.create(cls,enc);w.set_type(ET_DYN);w.set_machine(EM_NONE);
 section* s=w.sections.add(".relr.dyn");s->set_type(19);s->set_addr_align(sizeof(Word));s->set_entry_size(sizeof(Word));
 array_section_accessor<Word> raw(w,s);raw.add_entry(0x1000);raw.add_entry(0xb);
 section* data=w.sections.add(".data");data->set_type(SHT_PROGBITS);data->set_address(0x1000);data->set_flags(SHF_ALLOC|SHF_WRITE);
 Word add=(*w.get_convertor())(Word(0x1234));data->set_data(reinterpret_cast<const char*>(&add),sizeof(add));
 std::stringstream bytes(std::ios::in|std::ios::out|std::ios::binary);assert(w.save(bytes));
 bytes.seekg(0);elfio r;assert(r.load(bytes));const section* t=r.sections[".relr.dyn"];
 const_array_section_accessor<Word> arr(r,t);Elf64_Addr a=0,b=0;
 assert(arr.get_entries_num()==2 && arr.get_entry(0,a)&&arr.get_entry(1,b));assert(a==0x1000 && b==0xb);
 Elf64_Addr off=0;Elf_Word sym=0;unsigned type=0;Elf_Sxword ad=0;
 const_relocation_section_accessor old(r,t);assert(!old.get_entry(0,off,sym,type,ad));
 std::cout<<"RELR_RAW_OK class="<<unsigned(cls)<<" endian="<<unsigned(enc)<<" words=1000,b legacy_unhandled=1\n";
}
int main(){check<Elf32_Word>(ELFCLASS32,ELFDATA2LSB);check<Elf32_Word>(ELFCLASS32,ELFDATA2MSB);check<Elf64_Addr>(ELFCLASS64,ELFDATA2LSB);check<Elf64_Addr>(ELFCLASS64,ELFDATA2MSB);}
