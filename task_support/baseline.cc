#include <algorithm>
#include <iostream>
#include <sstream>
#include <vector>
#include "common/dwarf_cu_to_module.h"
#include "common/dwarf/dwarf2diehandler.h"
#include "common/dwarf/bytereader.h"
#include "task_support/fixture.h"
using google_breakpad::Module;
using google_breakpad::DwarfCUToModule;
int main(){
 auto image=sample::fanout();image.write(".task-build/function-fanout.elf");
 Module m("sample","Linux","x86_64","0123456789ABCDEF");
 DwarfCUToModule::FileContext file("function-fanout.elf",&m,true);
 google_breakpad::ElfReader elf(".task-build/function-fanout.elf");
 if(!elf.IsElf64File())return 7;
 for(const char* section:{".debug_abbrev",".debug_info"}){size_t length=0;const char* data=elf.GetSectionByName(section,&length);if(!data||!length)return 8;file.AddSectionToSectionMap(section,reinterpret_cast<const uint8_t*>(data),length);}
 DwarfCUToModule::WarningReporter warnings("function-fanout.elf",0);
 DwarfCUToModule root(&file,nullptr,nullptr,&warnings,true);
 google_breakpad::DIEDispatcher dispatch(&root);
 google_breakpad::ByteReader reader(google_breakpad::ENDIANNESS_LITTLE);
 google_breakpad::CompilationUnit cu("function-fanout.elf",file.section_map(),0,&reader,&dispatch);
 uint64_t consumed=cu.Start();if(consumed!=image.info_size)return 2;
 std::vector<Module::Function*> functions;m.GetFunctions(&functions,functions.begin());
 if(functions.size()!=3)return 3;
 unsigned late=0,normal=0;
 for(auto* f:functions){std::cout<<"FUNCTION address="<<std::hex<<f->address<<std::dec<<" name="<<f->name<<"\n";if(f->name=="late")++late;if(f->name=="normal")++normal;}
 if(normal!=1)return 4;
 std::ostringstream symbols;if(!m.Write(symbols,ALL_SYMBOL_DATA))return 5;
 if(symbols.str().find("FUNC")==std::string::npos)return 6;
 std::cout<<"PARSED_ELF functions="<<functions.size()<<" late_named="<<late<<" normal="<<normal<<"\n";
 // Separately exercise the existing published-origin API without dereferencing
 // an origin after destruction or modifying any target image.
 auto& origins=m.inline_origin_maps["origin-demo"];
 auto name=[&](const char* s){return m.AddStringToPool(s);};
 origins.SetReference(10,10);auto* a=origins.GetOrCreateInlineOrigin(10,name("<name omitted>"));
 origins.SetReference(20,20);auto* b=origins.GetOrCreateInlineOrigin(20,name("<name omitted>"));
 origins.SetReference(10,30);origins.SetReference(20,30);origins.SetReference(30,30);
 origins.GetOrCreateInlineOrigin(30,name("terminal"));
 std::cout<<"ORIGIN_NAMES first="<<a->name<<" second="<<b->name<<"\n";
 auto cross=sample::cross_cu();cross.write(".task-build/cross-cu.elf");
 Module cm("cross","Linux","x86_64","0123456789ABCDEF");
 DwarfCUToModule::FileContext cf("cross-cu.elf",&cm,true);
 google_breakpad::ElfReader ce(".task-build/cross-cu.elf");
 for(const char* section:{".debug_abbrev",".debug_info"}){size_t length=0;const char* data=ce.GetSectionByName(section,&length);if(!data||!length)return 9;cf.AddSectionToSectionMap(section,reinterpret_cast<const uint8_t*>(data),length);}
 uint64_t offset=0;while(offset<cross.info_size){
  DwarfCUToModule::WarningReporter cw("cross-cu.elf",offset);
  DwarfCUToModule cr(&cf,nullptr,nullptr,&cw,true);
  google_breakpad::DIEDispatcher cd(&cr);
  google_breakpad::ByteReader cb(google_breakpad::ENDIANNESS_LITTLE);
  google_breakpad::CompilationUnit cc("cross-cu.elf",cf.section_map(),offset,&cb,&cd);
  uint64_t n=cc.Start();if(!n)return 10;offset+=n;
 }
 if(offset!=cross.info_size)return 11;
 std::vector<Module::Function*> crossfuncs;cm.GetFunctions(&crossfuncs,crossfuncs.begin());
 std::cout<<"CROSS_CU functions="<<crossfuncs.size();for(auto* f:crossfuncs)std::cout<<" name="<<f->name;std::cout<<"\n";
 return 0;
}
