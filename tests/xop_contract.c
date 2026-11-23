/*
xop_contract.c

Contract tests for the AMD XOP subset added to diStorm
(VPCMOV, VPPERM, VPROTB/W/D/Q). The cases only decode static byte arrays;
no machine code is executed and no host XOP support is required.

Three groups are registered through TASK_CHECK:
  routing      - 0x8F POP vs XOP prefix dispatch, reserved/illegal encodings
  operands     - Intel operand order, W driven swapping, real memory fields,
                 tail selector register vs genuine imm8
  continuation - one-shot vs small-capacity streaming, truncation, isolation
                 of invalid bytes from the following valid instruction

main() is provided by task_support/driver.c.
*/

#include "include/distorm.h"
#include "include/mnemonics.h"
#include "task_support/check.h"
#include "task_support/inputs.h"

#include <string.h>

/* One-shot decompose helper; returns the number of instructions produced. */
static unsigned xop_dec(const unsigned char* bytes, unsigned len, _DecodeType dt,
                        _DInst* out, unsigned cap, _OffsetType offset)
{
	_CodeInfo ci;
	unsigned n = 0;
	memset(&ci, 0, sizeof(ci));
	ci.code = bytes;
	ci.codeLen = (int)len;
	ci.codeOffset = offset;
	ci.dt = dt;
	distorm_decompose(&ci, out, cap, &n);
	return n;
}

/* ------------------------------------------------------------------ */
/* routing group                                                       */
/* ------------------------------------------------------------------ */

/* 8F /0 with a low map nibble stays the ordinary POP instruction. */
static int routing_pop(void)
{
	_DInst d[4];
	unsigned n = xop_dec(plain_pop, sizeof plain_pop, Decode64Bits, d, 4, 0x1000);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_POP);
	TASK_CHECK(d[0].size == sizeof plain_pop);
	TASK_CHECK(d[0].flags != FLAG_NOT_DECODABLE);
	TASK_CHECK(d[0].opsNo == 1);
	return 0;
}

/* The canonical XOP byte stream is a single six-byte VPCMOV, not DB bytes. */
static int routing_xop_canonical(void)
{
	_DInst d[4];
	unsigned n = xop_dec(xop_registers, sizeof xop_registers, Decode64Bits, d, 4, 0x2000);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPCMOV);
	TASK_CHECK(d[0].size == 6);
	TASK_CHECK(d[0].flags != FLAG_NOT_DECODABLE);
	return 0;
}

/* An unsupported XOP map falls back to the legacy invalid-byte walk. */
static int routing_unsupported_map(void)
{
	_DInst d[16];
	unsigned n = xop_dec(xop_bad_map, sizeof xop_bad_map, Decode64Bits, d, 16, 0x3000);
	TASK_CHECK(n >= 1);
	TASK_CHECK(d[0].flags == FLAG_NOT_DECODABLE);
	TASK_CHECK(d[0].size == 1);
	TASK_CHECK(d[0].opcode != I_VPCMOV);
	TASK_CHECK(d[0].opsNo == 0);
	return 0;
}

/* 16 bit mode keeps the legacy behavior: the bytes never become XOP. */
static int routing_16bit_legacy(void)
{
	_DInst d[16];
	unsigned n = xop_dec(xop_registers, sizeof xop_registers, Decode16Bits, d, 16, 0x4000);
	TASK_CHECK(n >= 1);
	TASK_CHECK(d[0].opcode != I_VPCMOV);
	TASK_CHECK(d[0].opcode != I_VPPERM);
	/* Legacy decoding of raw 0x8F starts with an undecodable byte. */
	TASK_CHECK(d[0].flags == FLAG_NOT_DECODABLE);
	TASK_CHECK(d[0].size == 1);
	return 0;
}

/* Reserved XOP combinations (VPPERM L=1, map9 W=0) are not swallowed. */
static int routing_reserved(void)
{
	_DInst d[16];
	unsigned n = xop_dec(xop_vpperm_bad_l, sizeof xop_vpperm_bad_l, Decode64Bits, d, 16, 0x5000);
	TASK_CHECK(n >= 1);
	TASK_CHECK(d[0].flags == FLAG_NOT_DECODABLE);
	TASK_CHECK(d[0].opcode != I_VPPERM);

	n = xop_dec(xop_vprot_map9_bad_w, sizeof xop_vprot_map9_bad_w, Decode64Bits, d, 16, 0x5100);
	TASK_CHECK(n >= 1);
	/* It must never be interpreted as the XOP VPROT opcode. */
	TASK_CHECK(d[0].opcode != I_VPROTB);
	TASK_CHECK(d[0].opcode != I_VPROTW);
	TASK_CHECK(d[0].opcode != I_VPROTD);
	TASK_CHECK(d[0].opcode != I_VPROTQ);
	return 0;
}

/* ------------------------------------------------------------------ */
/* operands group                                                      */
/* ------------------------------------------------------------------ */

/* VPCMOV register form W=0: destination then each source in Intel order. */
static int operands_vpcmov_order(void)
{
	_DInst d[4];
	unsigned n = xop_dec(xop_vpcmov_reg_w0, sizeof xop_vpcmov_reg_w0, Decode64Bits, d, 4, 0x1000);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPCMOV);
	TASK_CHECK(d[0].opsNo == 4);
	TASK_CHECK((d[0].flags & FLAG_DST_WR) != 0);
	TASK_CHECK((d[0].usedRegistersMask & RM_SSE) != 0);

	TASK_CHECK(d[0].ops[0].type == O_REG && d[0].ops[0].index == R_XMM4 && d[0].ops[0].size == 128);
	TASK_CHECK(d[0].ops[1].type == O_REG && d[0].ops[1].index == R_XMM3 && d[0].ops[1].size == 128);
	TASK_CHECK(d[0].ops[2].type == O_REG && d[0].ops[2].index == R_XMM2 && d[0].ops[2].size == 128);
	/* The tail high nibble is a register operand, never an immediate. */
	TASK_CHECK(d[0].ops[3].type == O_REG && d[0].ops[3].index == R_XMM1 && d[0].ops[3].size == 128);
	return 0;
}

/* W swaps the two ModRM/vvvv source positions while the destination stays. */
static int operands_w_swap(void)
{
	_DInst d[4];
	unsigned n = xop_dec(xop_vpcmov_reg_w1, sizeof xop_vpcmov_reg_w1, Decode64Bits, d, 4, 0x1100);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opsNo == 4);
	TASK_CHECK(d[0].ops[0].index == R_XMM4);
	TASK_CHECK(d[0].ops[1].index == R_XMM2); /* ModRM source moves first when W=1. */
	TASK_CHECK(d[0].ops[2].index == R_XMM3); /* vvvv source moves to the next slot. */
	TASK_CHECK(d[0].ops[3].index == R_XMM1);
	return 0;
}

/* W=1 memory form: the memory lands in the semantic source slot, with real
 * addressing fields (RIP relative, displacement), not just renamed text. */
static int operands_w1_memory(void)
{
	_DInst d[4];
	unsigned n = xop_dec(xop_vpcmov_mem_rip, sizeof xop_vpcmov_mem_rip, Decode64Bits, d, 4, 0x1200);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].size == sizeof xop_vpcmov_mem_rip);
	TASK_CHECK(d[0].ops[1].type == O_SMEM);
	TASK_CHECK(d[0].ops[1].index == R_RIP);
	TASK_CHECK(d[0].ops[1].size == 128);
	TASK_CHECK((d[0].flags & FLAG_RIP_RELATIVE) != 0);
	TASK_CHECK(d[0].dispSize == 32);
	TASK_CHECK(d[0].disp == 0x12345678u);
	/* The other source and the selector keep their register slots. */
	TASK_CHECK(d[0].ops[2].type == O_REG && d[0].ops[2].index == R_XMM3);
	TASK_CHECK(d[0].ops[3].type == O_REG && d[0].ops[3].index == R_XMM1);

	/* A negative disp32 must be sign extended into the 64 bit disp field. */
	n = xop_dec(xop_vpcmov_mem_rip_neg, sizeof xop_vpcmov_mem_rip_neg, Decode64Bits, d, 4, 0x1210);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].ops[1].type == O_SMEM && d[0].ops[1].index == R_RIP);
	TASK_CHECK(d[0].dispSize == 32);
	TASK_CHECK(d[0].disp == (uint64_t)0xfffffffffffffffcULL);
	TASK_CHECK((d[0].flags & FLAG_RIP_RELATIVE) != 0);
	return 0;
}

/* 256 bit VPCMOV with a high destination register and an SIB operand. */
static int operands_ymm_sib(void)
{
	_DInst d[4];
	unsigned n = xop_dec(xop_vpcmov_ymm_sib, sizeof xop_vpcmov_ymm_sib, Decode64Bits, d, 4, 0x1300);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].ops[0].type == O_REG && d[0].ops[0].index == R_YMM8 && d[0].ops[0].size == 256);
	/* SIB [RSP] is the memory source; it carries the stack-register class. */
	TASK_CHECK((d[0].ops[2].type == O_SMEM) || (d[0].ops[2].type == O_MEM));
	TASK_CHECK(d[0].ops[2].index == R_RSP);
	TASK_CHECK(d[0].ops[2].size == 256);
	TASK_CHECK((d[0].usedRegistersMask & RM_AVX) != 0);
	TASK_CHECK((d[0].usedRegistersMask & RM_SP) != 0);
	/* The selector tail remains a 128 bit XMM even in a 256 bit instruction. */
	TASK_CHECK(d[0].ops[3].type == O_REG && d[0].ops[3].size == 128);
	return 0;
}

/* 32 bit mode: W still selects the layout and 0x67/disp8 memory is honored. */
static int operands_32bit(void)
{
	_DInst d[4];
	unsigned n = xop_dec(xop_vpcmov_reg_w1, sizeof xop_vpcmov_reg_w1, Decode32Bits, d, 4, 0x1400);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].ops[1].index == R_XMM2);
	TASK_CHECK(d[0].ops[2].index == R_XMM3);

	n = xop_dec(xop_vpcmov_32_disp8, sizeof xop_vpcmov_32_disp8, Decode32Bits, d, 4, 0x1410);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].ops[2].type == O_SMEM);
	TASK_CHECK(d[0].ops[2].index == R_ECX);
	TASK_CHECK(d[0].dispSize == 8);
	TASK_CHECK(d[0].disp == 5);

	n = xop_dec(xop_vpcmov_32_adr, sizeof xop_vpcmov_32_adr, Decode32Bits, d, 4, 0x1420);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPCMOV);
	TASK_CHECK(d[0].size == sizeof xop_vpcmov_32_adr);
	return 0;
}

/* VPPERM memory form keeps four operands and a register selector tail. */
static int operands_vpperm(void)
{
	_DInst d[4];
	unsigned n = xop_dec(xop_vpperm_mem, sizeof xop_vpperm_mem, Decode64Bits, d, 4, 0x1500);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPPERM);
	TASK_CHECK(d[0].opsNo == 4);
	TASK_CHECK(d[0].ops[1].type == O_SMEM && d[0].ops[1].index == R_RAX && d[0].ops[1].size == 128);
	TASK_CHECK(d[0].ops[2].type == O_REG && d[0].ops[2].index == R_XMM1);
	TASK_CHECK(d[0].ops[3].type == O_REG && d[0].ops[3].index == R_XMM2);
	return 0;
}

/* Vector-count VPROT: the count is a register whose size matches the width. */
static int operands_vprot_vector(void)
{
	_DInst d[4];
	unsigned n;

	n = xop_dec(xop_vprotb_vec, sizeof xop_vprotb_vec, Decode64Bits, d, 4, 0x1600);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPROTB);
	TASK_CHECK(d[0].opsNo == 3);
	TASK_CHECK(d[0].ops[2].type == O_REG && d[0].ops[2].size == 8);

	n = xop_dec(xop_vprotd_vec, sizeof xop_vprotd_vec, Decode64Bits, d, 4, 0x1610);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPROTD);
	TASK_CHECK(d[0].ops[0].index == R_XMM3);
	TASK_CHECK(d[0].ops[2].type == O_REG && d[0].ops[2].size == 32);

	n = xop_dec(xop_vprotw_vec, sizeof xop_vprotw_vec, Decode64Bits, d, 4, 0x1618);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPROTW);
	TASK_CHECK(d[0].ops[2].type == O_REG && d[0].ops[2].size == 16);

	n = xop_dec(xop_vprotq_vec64, sizeof xop_vprotq_vec64, Decode64Bits, d, 4, 0x1620);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPROTQ);
	TASK_CHECK(d[0].ops[2].type == O_REG && d[0].ops[2].size == 64);

	/* VPROTQ vector form is not encodable in 32 bit mode. */
	n = xop_dec(xop_vprotq_vec32_bad, sizeof xop_vprotq_vec32_bad, Decode32Bits, d, 4, 0x1630);
	TASK_CHECK(n >= 1);
	TASK_CHECK(d[0].opcode != I_VPROTQ);
	TASK_CHECK(d[0].flags == FLAG_NOT_DECODABLE);
	return 0;
}

/* Immediate-count VPROT keeps a genuine O_IMM imm8, distinct from a tail reg. */
static int operands_vprot_imm(void)
{
	_DInst d[4];
	unsigned n = xop_dec(xop_vprotb_imm, sizeof xop_vprotb_imm, Decode64Bits, d, 4, 0x1700);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPROTB);
	TASK_CHECK(d[0].opsNo == 3);
	TASK_CHECK(d[0].ops[1].type == O_REG && d[0].ops[1].index == R_XMM0);
	TASK_CHECK(d[0].ops[2].type == O_IMM && d[0].ops[2].size == 8);
	TASK_CHECK(d[0].imm.byte == 7);

	n = xop_dec(xop_vprotq_imm64, sizeof xop_vprotq_imm64, Decode64Bits, d, 4, 0x1710);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPROTQ);
	TASK_CHECK(d[0].ops[2].type == O_IMM && d[0].imm.byte == 3);

	n = xop_dec(xop_vprotd_imm, sizeof xop_vprotd_imm, Decode64Bits, d, 4, 0x1718);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPROTD);
	TASK_CHECK(d[0].ops[2].type == O_IMM && d[0].ops[2].size == 8 && d[0].imm.byte == 9);

	/* A memory data source plus an imm8 count coexist. */
	n = xop_dec(xop_vprotw_imm_mem, sizeof xop_vprotw_imm_mem, Decode64Bits, d, 4, 0x1720);
	TASK_CHECK(n == 1);
	TASK_CHECK(d[0].opcode == I_VPROTW);
	TASK_CHECK(d[0].ops[1].type == O_SMEM && d[0].ops[1].index == R_RAX);
	TASK_CHECK(d[0].ops[2].type == O_IMM && d[0].imm.byte == 5);
	return 0;
}

/* Decompose, text decode and formatting express the same single instruction. */
static int operands_text_matches(void)
{
	_DInst d[4];
	_DecodedInst tx[4];
	unsigned dn = 0, tn = 0;
	_CodeInfo ci;

	dn = xop_dec(xop_registers, sizeof xop_registers, Decode64Bits, d, 4, 0x1800);
	TASK_CHECK(dn == 1);

	memset(&ci, 0, sizeof(ci));
	ci.code = xop_registers;
	ci.codeLen = (int)sizeof xop_registers;
	ci.codeOffset = 0x1800;
	ci.dt = Decode64Bits;
	ci.features = DF_USE_ADDR_MASK;
	ci.addrMask = (_OffsetType)-1;
	distorm_decode(0x1800, xop_registers, (int)sizeof xop_registers, Decode64Bits, tx, 4, &tn);
	TASK_CHECK(tn == 1);
	TASK_CHECK(tx[0].size == d[0].size);
	TASK_CHECK(tx[0].offset == d[0].addr);
	TASK_CHECK(strcmp((const char*)tx[0].mnemonic.p, "VPCMOV") == 0);
	TASK_CHECK(strstr((const char*)tx[0].operands.p, "XMM4") != NULL);
	TASK_CHECK(strstr((const char*)tx[0].operands.p, "XMM1") != NULL);

	distorm_format(&ci, &d[0], &tx[0]);
	TASK_CHECK(strcmp((const char*)tx[0].mnemonic.p, "VPCMOV") == 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* continuation group                                                  */
/* ------------------------------------------------------------------ */

/* A mixed stream gives identical addresses/sizes one-shot or cap-1 chunks. */
static int continuation_stream(void)
{
	static const unsigned char stream[] = {
		0x48,0x89,0xd8,                       /* MOV  (3) */
		0x8f,0x00,                           /* POP  (2) */
		0x8f,0xe8,0x60,0xa2,0xe2,0x10,       /* VPCMOV (6) */
		0xc5,0xe8,0x58,0xcb                  /* VADDPS (4) */
	};
	const unsigned total = sizeof stream;
	_DInst big[8];
	unsigned bn = xop_dec(stream, total, Decode64Bits, big, 8, 0x6000);
	TASK_CHECK(bn == 4);
	TASK_CHECK(big[0].size == 3 && big[1].size == 2 && big[2].size == 6 && big[3].size == 4);
	TASK_CHECK(big[2].opcode == I_VPCMOV);

	/* Re-decode one instruction at a time with a capacity of one. */
	const unsigned char* p = stream;
	int left = (int)total;
	_OffsetType off = 0x6000;
	unsigned consumed = 0;
	for (unsigned i = 0; i < bn; i++) {
		_DInst chunk[1];
		unsigned cn = 0;
		_CodeInfo ci;
		memset(&ci, 0, sizeof(ci));
		ci.code = p;
		ci.codeLen = left;
		ci.codeOffset = off;
		ci.dt = Decode64Bits;
		distorm_decompose(&ci, chunk, 1, &cn);
		TASK_CHECK(cn == 1);
		TASK_CHECK(chunk[0].addr == big[i].addr);
		TASK_CHECK(chunk[0].size == big[i].size);
		TASK_CHECK(chunk[0].opcode == big[i].opcode);
		consumed += chunk[0].size;
		p += chunk[0].size;
		left -= chunk[0].size;
		off += chunk[0].size;
	}
	TASK_CHECK(consumed == total);
	TASK_CHECK(left == 0);
	return 0;
}

/* Every byte-by-byte truncation yields only one-byte DBs, never half operands. */
static int continuation_truncation(void)
{
	unsigned L;
	for (L = 1; L < sizeof xop_vpcmov_reg_w0; L++) {
		_DInst d[8];
		unsigned n = xop_dec(xop_vpcmov_reg_w0, L, Decode64Bits, d, 8, 0x7000);
		TASK_CHECK(n == L);
		for (unsigned i = 0; i < n; i++) {
			TASK_CHECK(d[i].flags == FLAG_NOT_DECODABLE);
			TASK_CHECK(d[i].size == 1);
			TASK_CHECK(d[i].opsNo == 0);
		}
	}
	return 0;
}

/* Invalid XOP prefixes must not pollute a valid instruction that follows. */
static int continuation_isolation(void)
{
	_DInst d[16];
	unsigned n;

	/* Unsupported map bytes, then a clean three-byte MOV. */
	static const unsigned char bad_then_mov[] = {
		0x8f,0xea,0x60,0xa2,0xe2,0x10,
		0x48,0x89,0xd8
	};
	n = xop_dec(bad_then_mov, sizeof bad_then_mov, Decode64Bits, d, 16, 0x8000);
	/* The unsupported prefix must not consume the MOV; it stays the last inst. */
	TASK_CHECK(n >= 2);
	TASK_CHECK(d[n - 1].opcode == I_MOV);
	TASK_CHECK(d[n - 1].size == 3);
	TASK_CHECK(d[n - 1].flags != FLAG_NOT_DECODABLE);
	TASK_CHECK(d[n - 1].opsNo >= 1);

	/* A three-byte truncated prefix, then a valid POP. */
	n = xop_dec(xop_trunc3, sizeof xop_trunc3, Decode64Bits, d, 16, 0x8100);
	TASK_CHECK(n == 3);
	TASK_CHECK(d[0].flags == FLAG_NOT_DECODABLE && d[0].size == 1);
	TASK_CHECK(d[2].flags == FLAG_NOT_DECODABLE && d[2].size == 1);

	static const unsigned char trunc_then_pop[] = {0x8f,0xe8,0x60, 0x8f,0x00};
	n = xop_dec(trunc_then_pop, sizeof trunc_then_pop, Decode64Bits, d, 16, 0x8200);
	TASK_CHECK(n >= 2);
	/* All bytes are accounted for and the POP is the final instruction. */
	{
		unsigned consumed = 0, i;
		for (i = 0; i < n; i++) consumed += d[i].size;
		TASK_CHECK(consumed == sizeof trunc_then_pop);
	}
	TASK_CHECK(d[n - 1].opcode == I_POP);
	TASK_CHECK(d[n - 1].size == 2);
	TASK_CHECK(d[n - 1].flags != FLAG_NOT_DECODABLE);
	return 0;
}

const struct task_case task_cases[] = {
	{"routing", "pop-not-swallowed", routing_pop},
	{"routing", "canonical-vpcmov", routing_xop_canonical},
	{"routing", "unsupported-map", routing_unsupported_map},
	{"routing", "16bit-legacy", routing_16bit_legacy},
	{"routing", "reserved-encodings", routing_reserved},

	{"operands", "vpcmov-intel-order", operands_vpcmov_order},
	{"operands", "w-source-swap", operands_w_swap},
	{"operands", "w1-memory-fields", operands_w1_memory},
	{"operands", "ymm-sib-high-reg", operands_ymm_sib},
	{"operands", "32bit-w-and-memory", operands_32bit},
	{"operands", "vpperm", operands_vpperm},
	{"operands", "vprot-vector-count", operands_vprot_vector},
	{"operands", "vprot-imm8-count", operands_vprot_imm},
	{"operands", "text-matches-decompose", operands_text_matches},

	{"continuation", "stream-one-shot-vs-cap1", continuation_stream},
	{"continuation", "byte-truncation", continuation_truncation},
	{"continuation", "invalid-no-pollution", continuation_isolation},
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
