#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bdshemu.h"
int main(void) {
 unsigned char code[]={0xb8,0xff,0xff,0xff,0xff,0x83,0xc0,0x01,0x83,0xd3,0x00};
 unsigned char stack[8192]={0},internal[sizeof(code)+8192];
 SHEMU_CONTEXT c={0};
 c.ArchType=SHEMU_ARCH_TYPE_X86; c.Arch.X86.Mode=ND_CODE_64;
 c.Arch.X86.Registers.RegRip=0x1000; c.Arch.X86.Registers.RegRsp=0x9000;
 c.Arch.X86.Registers.RegRax=0xfeedface00000000ULL;
 c.Shellcode=code; c.ShellcodeBase=0x1000; c.ShellcodeSize=sizeof(code);
 c.Stack=stack;c.StackBase=0x8000;c.StackSize=sizeof(stack);
 c.Intbuf=internal;c.IntbufSize=sizeof(internal);c.MaxInstructionsCount=3;
 SHEMU_STATUS st=ShemuEmulate(&c);
 printf("STATE status=%u count=%llu rax=%llx rbx=%llx\n",st,(unsigned long long)c.InstructionsCount,(unsigned long long)c.Arch.X86.Registers.RegRax,(unsigned long long)c.Arch.X86.Registers.RegRbx);
 assert(st==SHEMU_SUCCESS && c.InstructionsCount==4);
 assert(c.Arch.X86.Registers.RegRax==0 && c.Arch.X86.Registers.RegRbx==1);
 puts("WIDTH_CARRY_BASELINE_OK");
}
