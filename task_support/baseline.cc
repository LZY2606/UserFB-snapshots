#include "fixture.hh"
#include "dwarf/dwarf++.hh"
#include <iostream>
int main(){
 try {
 auto raw=fixture::elf_image({{".debug_info",fixture::baseline_info()},{".debug_abbrev",fixture::baseline_abbrev()}});
 ::elf::elf f(std::make_shared<fixture::Loader>(raw));
 dwarf::dwarf d(dwarf::elf::create_loader(f));
 auto &cu=d.compilation_units().at(0);auto root=cu.root();auto it=root.begin();
 auto child=*it; auto name=child[dwarf::DW_AT::name].as_string();auto pc=child[dwarf::DW_AT::low_pc].as_address();
 if(root[dwarf::DW_AT::name].as_string()!="baseline-unit"||name!="entry"||pc!=0x401020||child[dwarf::DW_AT::high_pc].as_uconstant()!=0x18)return 2;
 ++it;if(it!=root.end())return 3;
 dwarf::section_type type; bool known=dwarf::elf::section_name_to_type(".debug_str_offsets",&type);
 std::cout<<"BASELINE_OK cu=baseline-unit function="<<name<<" low_pc=0x"<<std::hex<<pc<<" size=0x18\n";
 std::cout<<"STR_OFFSETS_SECTION="<<(known?"known":"missing")<<"\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
}
