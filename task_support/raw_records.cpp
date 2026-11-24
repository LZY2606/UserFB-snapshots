// MIT; raw records only. Deliberately does not resolve calls or build a graph.
#include <elfio/elfio.hpp>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace ELFIO;
static void need(bool b){if(!b) throw std::runtime_error("raw record check failed");}
static std::uint32_t u32(const char* p) {std::uint32_t n=0;for(int i=0;i<4;++i)n|=std::uint32_t((unsigned char)p[i])<<(8*i);return n;}
int main(int argc,char** argv) {
 try {
    need(argc==2);elfio e;need(e.load(argv[1]));
    need(e.get_class()==ELFCLASS64&&e.get_encoding()==ELFDATA2LSB&&e.get_type()==ET_REL&&e.get_machine()==EM_BPF);
    need(e.segments.size()==0);unsigned code=0,slot=0,rel=0,func=0,obj=0;
    for(unsigned i=1;i<e.sections.size();++i) {
        const auto* s=e.sections[i];
        std::cout<<"SECTION "<<i<<" name="<<s->get_name()<<" type="<<s->get_type()<<" offset="<<s->get_offset()<<" size="<<s->get_size()<<" link="<<s->get_link()<<" info="<<s->get_info()<<'\n';
        if(s->get_flags()&SHF_EXECINSTR) {
            ++code;need(s->get_size()%8==0);slot+=s->get_size()/8;
            for(Elf_Xword off=0;off<s->get_size();off+=8) {
                const auto* p=s->get_data()+off;
                std::cout<<"SLOT "<<i<<':'<<off<<" opcode="<<unsigned((unsigned char)p[0])<<" regs="<<unsigned((unsigned char)p[1])<<" imm_u32="<<u32(p+4)<<'\n';
            }
        }
        if(s->get_type()==SHT_SYMTAB) {
            const_symbol_section_accessor acc(e,s);
            for(Elf_Xword n=0;n<acc.get_symbols_num();++n) {
                std::string name;Elf64_Addr value;Elf_Xword size;unsigned char bind,type,other;Elf_Half sec;
                need(acc.get_symbol(n,name,value,size,bind,type,sec,other));
                func+=type==STT_FUNC;obj+=type==STT_OBJECT;
                std::cout<<"SYMBOL "<<i<<':'<<n<<" name="<<name<<" section="<<sec<<" value="<<value<<" size="<<size<<" bind="<<unsigned(bind)<<" type="<<unsigned(type)<<'\n';
            }
        }
        if(s->get_type()==SHT_REL) {
            const_relocation_section_accessor acc(e,s);
            for(Elf_Xword n=0;n<acc.get_entries_num();++n) {
                Elf64_Addr off;Elf_Word sym;unsigned type;Elf_Sxword add;
                need(acc.get_entry(n,off,sym,type,add));++rel;
                std::cout<<"REL "<<i<<':'<<n<<" offset="<<off<<" symbol="<<sym<<" type="<<type<<" accessor_addend="<<add<<'\n';
            }
        }
    }
    need(code==2&&slot==38&&rel==9&&func==6&&obj==2);
    std::ostringstream first(std::ios::binary);need(e.save(first));
    elfio again;std::istringstream in(first.str(),std::ios::binary);need(again.load(in));
    std::ostringstream second(std::ios::binary);need(again.save(second));need(first.str()==second.str());
    std::cout<<"RAW_BASELINE_OK code_sections=2 slots=38 relocations=9 functions=6 maps=2\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
