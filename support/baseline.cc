#include "InstructionRelocation/arm64/InstructionRelocationARM64.h"
#include <cstdio>
#include <cstring>
int main() {
  alignas(16) uint32_t code[] = {0xd503201f,0x1c000040,0xd65f03c0,0x3f800000};
  CodeMemBlock origin((addr_t)code,4), out;
  GenRelocateCode(code,&origin,&out,false);
  if (!out.addr() || out.size!=4 || std::memcmp((void*)out.addr(),code,4)) return 1;
  std::puts("NOP production relocation unchanged");
  return 0;
}
