#ifndef TASK_INPUTS_H
#define TASK_INPUTS_H
/* Handwritten static byte arrays; data only, never executed.
   XOP example agrees with LLVM test/MC/X86/xop-att.s VPCMOV example.
   Intel operand order: xmm4, xmm3, xmm2, xmm1. */
static const unsigned char xop_registers[]={0x8f,0xe8,0x60,0xa2,0xe2,0x10};
static const unsigned char plain_mov[]={0x48,0x89,0xd8};
static const unsigned char plain_pop[]={0x8f,0x00};
static const unsigned char plain_vex[]={0xc5,0xe8,0x58,0xcb};
#endif
