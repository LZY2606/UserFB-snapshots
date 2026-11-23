#ifndef TASK_INPUTS_H
#define TASK_INPUTS_H
/* Handwritten static byte arrays; data only, never executed.
   XOP example agrees with LLVM test/MC/X86/xop-att.s VPCMOV example.
   Intel operand order: xmm4, xmm3, xmm2, xmm1. */
static const unsigned char xop_registers[]={0x8f,0xe8,0x60,0xa2,0xe2,0x10};
static const unsigned char plain_mov[]={0x48,0x89,0xd8};
static const unsigned char plain_pop[]={0x8f,0x00};
static const unsigned char plain_vex[]={0xc5,0xe8,0x58,0xcb};

/*
 * Additional AMD XOP fixtures (all static bytes, never run).
 * Naming: xop_<instruction>_<form>. Every array is documented with its
 * expected Intel-syntax operand order in tests/xop_contract.c.
 */

/* VPCMOV register form, W=0: xmm4, xmm3, xmm2, xmm1 (same as xop_registers). */
static const unsigned char xop_vpcmov_reg_w0[]={0x8f,0xe8,0x60,0xa2,0xe2,0x10};
/* VPCMOV register form, W=1: xmm4, xmm2, xmm3, xmm1. */
static const unsigned char xop_vpcmov_reg_w1[]={0x8f,0xe8,0xe0,0xa2,0xe2,0x10};
/* VPCMOV W=1, RIP-relative memory source: xmm0, [rip+0x12345678], xmm3, xmm1. */
static const unsigned char xop_vpcmov_mem_rip[]={0x8f,0xe8,0xe0,0xa2,0x05,0x78,0x56,0x34,0x12,0x10};
/* VPCMOV W=1, RIP-relative with a negative disp32 (-4): xmm0, [rip-4], xmm3, xmm1. */
static const unsigned char xop_vpcmov_mem_rip_neg[]={0x8f,0xe8,0xe0,0xa2,0x05,0xfc,0xff,0xff,0xff,0x10};
/* VPCMOV 256 bit (L=1), high destination ymm8, SIB [rsp]: ymm8, ymm3, [rsp], xmm1. */
static const unsigned char xop_vpcmov_ymm_sib[]={0x8f,0x68,0x64,0xa2,0x04,0x24,0x10};
/* VPCMOV in 32 bit mode with an address-size override prefix. */
static const unsigned char xop_vpcmov_32_adr[]={0x67,0x8f,0xe8,0x60,0xa2,0xe2,0x10};
/* VPCMOV 32 bit mode, disp8 simple memory [ecx+5]. */
static const unsigned char xop_vpcmov_32_disp8[]={0x8f,0xe8,0x60,0xa2,0x49,0x05,0x10};

/* VPPERM xmm0, [rax], xmm1, xmm2 (W=1, selector xmm2 in tail 0x20). */
static const unsigned char xop_vpperm_mem[]={0x8f,0xe8,0xf0,0xa3,0x00,0x20};
/* VPPERM with L=1 is reserved and must not decode. */
static const unsigned char xop_vpperm_bad_l[]={0x8f,0xe8,0xf4,0xa3,0xc0,0x10};

/* VPROTB vector count, W=1: xmm0, xmm0(data vvvv), xmm0(count modrm), 128 bit. */
static const unsigned char xop_vprotb_vec[]={0x8f,0xe8,0xf0,0x90,0xc0};
/* VPROTW vector count, W=0 (flags 0x70): xmm0, xmm0(data), xmm0(count, 16 bit). */
static const unsigned char xop_vprotw_vec[]={0x8f,0xe8,0x70,0x91,0xc0};
/* VPROTD vector count, W=0 (flags 0x70): xmm3, xmm0(data rm), xmm1(count vvvv, 32 bit). */
static const unsigned char xop_vprotd_vec[]={0x8f,0xe8,0x70,0x92,0xd8};
/* VPROTQ vector count (64 bit mode only). */
static const unsigned char xop_vprotq_vec64[]={0x8f,0xe8,0xf0,0x93,0xc0};
/* VPROTB imm8 count (map 9): xmm0, xmm0, 0x7. */
static const unsigned char xop_vprotb_imm[]={0x8f,0xe9,0xf8,0xc0,0xc0,0x07};
/* VPROTQ imm8 count, 64 bit mode. */
static const unsigned char xop_vprotq_imm64[]={0x8f,0xe9,0xf8,0xc3,0xc0,0x03};
/* VPROTD imm8 count (map 9, opcode C2): xmm0, xmm0, 0x9. */
static const unsigned char xop_vprotd_imm[]={0x8f,0xe9,0xf8,0xc2,0xc0,0x09};
/* VPROTW imm8 with a memory operand [rax], count 5. */
static const unsigned char xop_vprotw_imm_mem[]={0x8f,0xe9,0xf9,0xc1,0x00,0x05};
/* map 9 encoding with W=0 (flags 0x70) is reserved. */
static const unsigned char xop_vprot_map9_bad_w[]={0x8f,0xe9,0x70,0xc0,0xc0,0x07};
/* VPROTQ vector form in 32 bit mode is not encodable. */
static const unsigned char xop_vprotq_vec32_bad[]={0x8f,0xe8,0xf0,0x93,0xc0};

/* Second byte low-five = 0: stays in the normal POP opcode space. */
static const unsigned char xop_popafter_8f_e0[]={0x8f,0xe0};
/* Unsupported XOP map (mmmmm = 10); must follow the legacy invalid path. */
static const unsigned char xop_bad_map[]={0x8f,0xea,0x60,0xa2,0xe2,0x10};
/* Canonical VPCMOV truncated to its first three prefix bytes. */
static const unsigned char xop_trunc3[]={0x8f,0xe8,0x60};
#endif
