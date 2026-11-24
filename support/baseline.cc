#include "common/dwarf/dwarf2reader.h"
#include "common/dwarf/elf_reader.h"
#include <cstdio>
#include <cstring>
using namespace google_breakpad;
struct Inventory : Dwarf2Handler {
  uint64_t root=0, root_low=0;
  unsigned variables=0, listed=0, direct=0;
  bool StartCompilationUnit(uint64_t,uint8_t,uint8_t,uint64_t,uint8_t) override {return true;}
  bool NeedSplitDebugInfo() override {return false;}
  bool StartDIE(uint64_t offset,DwarfTag tag) override {
    if(tag==DW_TAG_compile_unit)root=offset;
    if(tag==DW_TAG_variable || tag==DW_TAG_formal_parameter)++variables;
    return true;
  }
  void ProcessAttributeUnsigned(uint64_t offset,DwarfAttribute attr,DwarfForm,uint64_t value) override {
    if(offset==root && attr==DW_AT_low_pc)root_low=value;
    if(attr==DW_AT_location)++listed;
  }
  void ProcessAttributeBuffer(uint64_t,DwarfAttribute attr,DwarfForm,const uint8_t*,uint64_t) override {
    if(attr==DW_AT_location)++direct;
  }
};
int main(int argc,char** argv) {
 if(argc!=3)return 2;
 for(int f=1;f<argc;++f) {
  ElfReader elf(argv[f]);
  if(!elf.IsElf64File())return 3;
  SectionMap sections;
  for(uint64_t i=1;i<elf.GetNumSections();++i) {
   const char* name=elf.GetSectionName(i);
   if(!name || strncmp(name,".debug_",7))continue;
   size_t size=0;const char* bytes=elf.GetSectionByIndex(i,&size);
   if(!bytes)return 4;
   sections[name]=std::make_pair(reinterpret_cast<const uint8_t*>(bytes),size);
  }
  if(!sections.count(".debug_info") || !sections.count(".debug_abbrev"))return 5;
  ByteReader reader(ENDIANNESS_LITTLE);
  uint64_t offset=0;unsigned units=0,total_variables=0,total_listed=0,total_direct=0;
  while(offset<sections[".debug_info"].second) {
   Inventory h;CompilationUnit cu(argv[f],sections,offset,&reader,&h);
   uint64_t size=cu.Start();
   if(!size || size>sections[".debug_info"].second-offset)return 6;
   if(!h.root_low || !h.variables || !h.listed || !h.direct)return 7;
   std::printf("CU offset=0x%llx root_low=0x%llx existing_GetLowPC=0x%llx variables=%u listed=%u inline=%u\n",
       (unsigned long long)offset,(unsigned long long)h.root_low,(unsigned long long)cu.GetLowPC(),h.variables,h.listed,h.direct);
   offset+=size;++units;total_variables+=h.variables;total_listed+=h.listed;total_direct+=h.direct;
  }
  if(units!=2 || total_variables<6 || total_listed<4 || !total_direct)return 8;
  std::printf("BASELINE sample=%s cu=%u variables=%u listed=%u inline=%u\n",argv[f],units,total_variables,total_listed,total_direct);
 }
 return 0;
}
