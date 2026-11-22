#include "init.h"
#include <stdio.h>
int main(void) {
 unsigned char code[]={0xb8,1,0,0,0,0x83,0xc0,2,0x90};
 unsigned char stack[8192]={0},scratch[8192+sizeof(code)]={0}; SHEMU_CONTEXT c;
 init_context(&c,code,sizeof(code),stack,scratch);
 SHEMU_STATUS st=ShemuEmulate(&c);
 printf("Shemu status=%u eax=%llu rip=%llx\n",st,(unsigned long long)c.Arch.X86.Registers.RegRax,(unsigned long long)c.Arch.X86.Registers.RegRip);
 return st!=SHEMU_SUCCESS || c.Arch.X86.Registers.RegRax!=3;
}
