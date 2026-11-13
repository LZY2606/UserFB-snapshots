#include "Archs/ARM/ArmElfRelocator.h"
#include <cstdio>
int main() {
 ArmElfRelocator reloc(false); RelocationData d{};
 d.opcodeOffset=0x1000;d.relocationBase=0x1100;d.opcode=0xebfffffe;d.targetSymbolType=STT_FUNC;
 std::vector<RelocationAction> a;std::vector<std::string> e;
 bool ok=reloc.relocateOpcode(R_ARM_CALL,d,a,e);
 if(!ok||a.size()!=1||a[0].newValue!=0xeb00003e) return 1;
 std::printf("ARM BL relocated=%08x actions=%zu\n",a[0].newValue,a.size());
 reloc.setSymbolAddress(d,0x2201,STT_FUNC);
 if(d.symbolAddress!=0x2200||d.targetSymbolInfo!=1)return 2;
 std::printf("Thumb symbol address=%llx state=%d\n",(long long)d.symbolAddress,d.targetSymbolInfo);
 return 0;
}
