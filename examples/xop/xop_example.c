/*
xop_example.c

Minimal, self-contained example of the AMD XOP support added to diStorm.
It only reads static byte arrays; nothing is executed and the host CPU does
not need to implement XOP. Both the structured distorm_decompose model and
the textual distorm_decode output are shown for the same instructions.

Build (from the repository root), C99 + standard library only:
  cc -std=c99 -I. -Iinclude (all .c files under src) examples/xop/xop_example.c -o xop_example
*/

#include <stdio.h>
#include <string.h>

#include "../../include/distorm.h"

struct xop_sample {
	const char* note;
	_DecodeType dt;
	const unsigned char* bytes;
	unsigned int len;
};

int main(void)
{
	/* Every array is documented with its expected Intel operand order. */
	static const unsigned char vpcmov_reg[]  = {0x8f,0xe8,0x60,0xa2,0xe2,0x10};
	/* VPCMOV xmm0, [rip+0x12345678], xmm3, xmm1 (W=1, RIP relative). */
	static const unsigned char vpcmov_mem[]  = {0x8f,0xe8,0xe0,0xa2,0x05,0x78,0x56,0x34,0x12,0x10};
	/* VPPERM xmm0, [rax], xmm1, xmm2. */
	static const unsigned char vpperm_mem[]  = {0x8f,0xe8,0xf0,0xa3,0x00,0x20};
	/* VPROTB xmm0, xmm0, xmm1 (vector count form, map 8, W=0). */
	static const unsigned char vprotb_vec[]  = {0x8f,0xe8,0x70,0x90,0xc0};
	/* VPROTD xmm3, xmm0, xmm1 (count register is 32 bit). */
	static const unsigned char vprotd_vec[]  = {0x8f,0xe8,0x70,0x92,0xd8};
	/* VPROTB xmm0, xmm0, 0x7 (immediate count form, map 9). */
	static const unsigned char vprotb_imm[]  = {0x8f,0xe9,0xf8,0xc0,0xc0,0x07};

	const struct xop_sample samples[] = {
		{"VPCMOV register (64 bit)", Decode64Bits, vpcmov_reg, sizeof vpcmov_reg},
		{"VPCMOV RIP memory (64 bit)", Decode64Bits, vpcmov_mem, sizeof vpcmov_mem},
		{"VPPERM memory (64 bit)", Decode64Bits, vpperm_mem, sizeof vpperm_mem},
		{"VPROTB vector count (64 bit)", Decode64Bits, vprotb_vec, sizeof vprotb_vec},
		{"VPROTD vector count (64 bit)", Decode64Bits, vprotd_vec, sizeof vprotd_vec},
		{"VPROTB imm8 count (64 bit)", Decode64Bits, vprotb_imm, sizeof vprotb_imm},
		{"VPCMOV register (32 bit)", Decode32Bits, vpcmov_reg, sizeof vpcmov_reg},
	};
	const unsigned int sampleCount = sizeof samples / sizeof samples[0];
	unsigned int s;

	for (s = 0; s < sampleCount; s++) {
		_CodeInfo ci;
		_DInst di[8];
		_DecodedInst text[8];
		unsigned int decomposed = 0, decoded = 0, i;

		memset(&ci, 0, sizeof ci);
		ci.code = samples[s].bytes;
		ci.codeLen = (int)samples[s].len;
		ci.codeOffset = 0x1000;
		ci.dt = samples[s].dt;

		if (distorm_decompose(&ci, di, 8, &decomposed) != DECRES_SUCCESS) {
			printf("[%s] decompose failed\n", samples[s].note);
			continue;
		}

		distorm_decode(0x1000, samples[s].bytes, (int)samples[s].len,
		               samples[s].dt, text, 8, &decoded);

		printf("== %s ==\n", samples[s].note);
		for (i = 0; i < decomposed; i++) {
			printf("  decompose: size=%u opcode=%u ops=%u dstwr=%d\n",
			       di[i].size, di[i].opcode, di[i].opsNo,
			       (di[i].flags & FLAG_DST_WR) ? 1 : 0);
			if (i < decoded)
				printf("  text:      %s %s\n",
				       (char*)text[i].mnemonic.p, (char*)text[i].operands.p);
		}
	}

	return 0;
}
