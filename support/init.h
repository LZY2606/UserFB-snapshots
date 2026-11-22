#pragma once
#include "bdshemu.h"
#include <string.h>
static void init_context(SHEMU_CONTEXT *c, unsigned char *code, size_t n, unsigned char *stack, unsigned char *scratch) {
 memset(c,0,sizeof(*c)); c->ArchType=SHEMU_ARCH_TYPE_X86;
 c->Shellcode=code; c->ShellcodeSize=n; c->ShellcodeBase=0x200000;
 c->Stack=stack; c->StackBase=0x100000; c->StackSize=8192;
 c->Intbuf=scratch; c->IntbufSize=8192+n;
 c->Arch.X86.Mode=ND_CODE_64; c->Arch.X86.Ring=3;
 c->Arch.X86.Registers.RegRip=c->ShellcodeBase; c->Arch.X86.Registers.RegRsp=0x101000;
 c->Arch.X86.Registers.RegFlags=2; c->MaxInstructionsCount=3;
}
