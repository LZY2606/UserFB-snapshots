/*
xop.c

diStorm3 - Powerful disassembler for X86/AMD64
http://ragestorm.net/distorm/
distorm at gmail dot com
Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/

/*
 * AMD XOP (three-byte 0x8F prefix) decoder for the supported subset:
 *
 *   map 8 (mmmmm=8):
 *     A2 VPCMOV  xmm1/ymm1, xmm2/ymm3, r/m, imm8-selector
 *     A3 VPPERM  xmm1, xmm2, r/m, imm8-selector
 *     90 VPROTB / 91 VPROTW / 92 VPROTD / 93 VPROTQ  (vector count)
 *   map 9 (mmmmm=9):
 *     C0 VPROTB / C1 VPROTW / C2 VPROTD / C3 VPROTQ  (imm8 count)
 *
 * XOP layout:
 *   8F | R~ X~ B~ mmmmm | W vvvv L pp | opcode | ModRM [SIB] [disp] | tail
 *
 * Only bytes are read; machine code is never executed and host XOP support
 * is irrelevant. The result lands in the ordinary _DInst operand model, so
 * distorm_decompose / distorm_decode / distorm_format all express the same
 * instruction.
 */

#include "xop.h"
#include "x86defs.h"
#include "operands.h"
#include "../include/mnemonics.h"

/* XOP map selector values carried in the low five bits of the second byte. */
#define XOP_MAP8 8
#define XOP_MAP9 9

/* Bit layout of the third XOP byte (W vvvv L pp), as it appears in memory. */
#define XOP_FL_W (0x80)
#define XOP_FL_L (0x04)

/* Generic inst-info; the real opcode id is fixed up in xop_decode. */
_InstInfo II_XOP = {0, I_UNDEFINED};

/* Register class table lives in operands.c. */
extern uint32_t _REGISTERTORCLASS[];

/* Descriptor of one recognized XOP encoding. */
typedef struct {
	uint16_t opcodeId;
	uint16_t vecSize;     /* 128 or 256; 256 only for VPCMOV. */
	uint8_t opcode;
	uint8_t isVprot;
	uint8_t immCount;    /* VPROT map 9 keeps a real imm8 count. */
} XopInfo;

int xop_is_prefix(const _CodeInfo* ci)
{
	/* 16 bit mode keeps the legacy behavior: 0x8F is never an XOP prefix. */
	if (ci->dt == Decode16Bits) return FALSE;
	if (ci->codeLen < 2) return FALSE;
	if (ci->code[0] != 0x8f) return FALSE;
	/* Low five bits below 8 stay in the normal opcode space (e.g. POP). */
	return (ci->code[1] & 0x1f) >= XOP_MAP8;
}

static int xop_fill_info(_CodeInfo* ci, const uint8_t* p, XopInfo* xi)
{
	unsigned int map, opcode;

	memset(xi, 0, sizeof(*xi));
	map = p[1] & 0x1f;   /* R~ X~ B~ mmmmm */
	opcode = p[3];      /* W vvvv L pp | opcode */

	xi->opcode = (uint8_t)opcode;
	xi->vecSize = (p[2] & XOP_FL_L) ? 256 : 128;

	if (map == XOP_MAP8) {
		if (opcode == 0xa2) {
			xi->opcodeId = I_VPCMOV;
			return TRUE;
		}
		if (opcode == 0xa3) {
			/* VPPERM has no 256 bit form. */
			if (xi->vecSize != 128) return FALSE;
			xi->opcodeId = I_VPPERM;
			return TRUE;
		}
		if ((opcode >= 0x90) && (opcode <= 0x93)) {
			static const uint16_t vprotIds[4] = {I_VPROTB, I_VPROTW, I_VPROTD, I_VPROTQ};
			/* Vector-count VPROT is 128 bit only. */
			if (xi->vecSize != 128) return FALSE;
			/* VPROTQ requires 64 bit mode (there are no 64 bit GPRs in 32 bit). */
			if ((opcode == 0x93) && (ci->dt != Decode64Bits)) return FALSE;
			xi->opcodeId = vprotIds[opcode - 0x90];
			xi->isVprot = 1;
			return TRUE;
		}
		return FALSE;
	}

	if (map == XOP_MAP9) {
		if ((opcode >= 0xc0) && (opcode <= 0xc3)) {
			static const uint16_t vprotIds[4] = {I_VPROTB, I_VPROTW, I_VPROTD, I_VPROTQ};
			/* L must be clear and W must be set for the imm8 VPROT form. */
			if (p[2] & XOP_FL_L) return FALSE;
			if (!(p[2] & XOP_FL_W)) return FALSE;
			if ((opcode == 0xc3) && (ci->dt != Decode64Bits)) return FALSE;
			xi->opcodeId = vprotIds[opcode - 0xc0];
			xi->isVprot = 1;
			xi->immCount = 1;
			return TRUE;
		}
	}

	/* Unsupported XOP encoding keeps the original invalid-byte handling. */
	return FALSE;
}

_InstInfo* xop_lookup(_CodeInfo* ci, _PrefixState* ps)
{
	const uint8_t* p;
	XopInfo xi;

	if (!xop_is_prefix(ci)) return NULL;
	/* Need prefix(3) + opcode(1) just to identify the instruction. */
	if (ci->codeLen < 4) return NULL;
	p = ci->code;

	if (!xop_fill_info(ci, p, &xi)) return NULL;

	/*
	 * Mirror the VEX bookkeeping so the rest of the pipeline sees a normal
	 * vector prefix: decodedPrefixes marks the prefix as used and vrex keeps
	 * the inverted R/X/B extension bits plus W/L. In 32 bit mode W is kept,
	 * because it selects the operand layout rather than a 64 bit GPR.
	 */
	ps->decodedPrefixes |= INST_PRE_VEX;
	ps->prefixExtType = PET_VEX3BYTES;
	ps->vexPos = ci->code + 1;
	ps->vexV = (unsigned int)((~p[2] >> 3) & 0xf);
	ps->vrex = ((~p[1] >> 5) & 0x7); /* R, X, B. */
	if (p[2] & XOP_FL_L) ps->vrex |= PREFIX_EX_L;
	if (p[2] & XOP_FL_W) ps->vrex |= PREFIX_EX_W;

	/* Consume the three prefix bytes and the opcode byte. */
	ci->code += 4;
	ci->codeLen -= 4;
	if (ci->codeLen < 0) return NULL;

	return &II_XOP;
}

/*
 * Decode the ModRM/SIB/disp addressing into op. The memory indirection size
 * is filled in by the caller. This follows the same 32/64 bit ModRM rules as
 * operands_extract_modrm (rm=5 mod=0 is disp32 / RIP-relative, rm=4 is SIB).
 */
static void xop_use_segment(_PrefixState* ps, _DInst* di, _iflags defaultSeg);

/* Sign-extending displacement readers; they advance ci->code/ci->codeLen. */
static int xop_read_s8(_CodeInfo* ci, int64_t* out)
{
	ci->codeLen -= 1;
	if (ci->codeLen < 0) return FALSE;
	*out = *(const int8_t*)ci->code;
	ci->code += 1;
	return TRUE;
}

static int xop_read_s16(_CodeInfo* ci, int64_t* out)
{
	ci->codeLen -= 2;
	if (ci->codeLen < 0) return FALSE;
	*out = (int16_t)(ci->code[0] | (ci->code[1] << 8));
	ci->code += 2;
	return TRUE;
}

static int xop_read_s32(_CodeInfo* ci, int64_t* out)
{
	ci->codeLen -= 4;
	if (ci->codeLen < 0) return FALSE;
	*out = (int32_t)(ci->code[0] | (ci->code[1] << 8) |
	                 (ci->code[2] << 16) | ((int32_t)ci->code[3] << 24));
	ci->code += 4;
	return TRUE;
}

static int xop_decode_mem(_CodeInfo* ci, _PrefixState* ps, _DInst* di,
                          _DecodeType effAdrSz, unsigned int modrm, _Operand* op)
{
	unsigned int mod = (modrm >> 6) & 3;
	unsigned int rm = modrm & 7;
	unsigned char sib = 0;
	unsigned int vrex = ps->vrex;

	if (mod == 3) return FALSE;

	if (effAdrSz != Decode16Bits) {
		if ((rm == 5) && (mod == 0)) {
			int64_t d;
			di->dispSize = 32;
			if (!xop_read_s32(ci, &d)) return FALSE;
			di->disp = (uint64_t)d;

			if (ci->dt == Decode64Bits) {
				op->type = O_SMEM;
				op->index = R_RIP;
				di->flags |= FLAG_RIP_RELATIVE;
			}
			else {
				op->type = O_DISP;
			}
			xop_use_segment(ps, di, INST_PRE_DS);
			return TRUE;
		}

		if (rm == 4) {
			unsigned int scale, index, base;
			ci->codeLen -= 1;
			if (ci->codeLen < 0) return FALSE;
			sib = *ci->code;
			ci->code += 1;

			scale = (sib >> 6) & 3;
			index = (sib >> 3) & 7;
			base = sib & 7;

			if (vrex & PREFIX_EX_X) index += EX_GPR_BASE;

			if (index == 4) {
				op->type = O_SMEM;
			}
			else {
				op->type = O_MEM;
				op->index = (uint8_t)((effAdrSz == Decode64Bits ? REGS64_BASE : REGS32_BASE) + index);
				di->usedRegistersMask |= _REGISTERTORCLASS[op->index];
				di->scale = scale != 0 ? (uint8_t)(1 << scale) : 0;
			}

			if (base != 5) {
				unsigned int bidx = (effAdrSz == Decode64Bits ? REGS64_BASE : REGS32_BASE)
				                    + base + ((vrex & PREFIX_EX_B) ? EX_GPR_BASE : 0);
				if (op->type == O_SMEM) op->index = (uint8_t)bidx;
				else di->base = (uint8_t)bidx;
				di->usedRegistersMask |= _REGISTERTORCLASS[bidx];
			}
			else if (mod != 0) {
				unsigned int bidx = (effAdrSz == Decode64Bits ? REGS64_BASE : REGS32_BASE)
				                    + 5 + ((vrex & PREFIX_EX_B) ? EX_GPR_BASE : 0);
				if (op->type == O_SMEM) op->index = (uint8_t)bidx;
				else di->base = (uint8_t)bidx;
				di->usedRegistersMask |= _REGISTERTORCLASS[bidx];
			}
			else if (index == 4) {
				op->type = O_DISP;
			}
		}
		else {
			op->type = O_SMEM;
			if (vrex & PREFIX_EX_B) rm += EX_GPR_BASE;
			op->index = (uint8_t)((effAdrSz == Decode64Bits ? REGS64_BASE : REGS32_BASE) + rm);
			di->usedRegistersMask |= _REGISTERTORCLASS[op->index];
		}

		if (mod == 1) {
			int64_t d;
			di->dispSize = 8;
			if (!xop_read_s8(ci, &d)) return FALSE;
			di->disp = (uint64_t)d;
		}
		else if ((mod == 2) || ((sib != 0) && ((sib & 7) == 5))) {
			int64_t d;
			di->dispSize = 32;
			if (!xop_read_s32(ci, &d)) return FALSE;
			di->disp = (uint64_t)d;
		}

		/* Default segment: SS for rBP/rSP based addressing, DS otherwise. */
		if (op->type == O_DISP) {
			xop_use_segment(ps, di, INST_PRE_DS);
		}
		else if (op->type == O_SMEM) {
			if ((op->index == R_BP) || (op->index == R_SP) ||
			    (op->index == R_EBP) || (op->index == R_ESP) ||
			    (op->index == R_RBP) || (op->index == R_RSP))
				xop_use_segment(ps, di, INST_PRE_SS);
			else
				xop_use_segment(ps, di, INST_PRE_DS);
		}
		else if (op->type == O_MEM) {
			if ((di->base == R_BP) || (di->base == R_SP) ||
			    (di->base == R_EBP) || (di->base == R_ESP) ||
			    (di->base == R_RBP) || (di->base == R_RSP))
				xop_use_segment(ps, di, INST_PRE_SS);
			else
				xop_use_segment(ps, di, INST_PRE_DS);
		}
		return TRUE;
	}

	/* 16 bit ModRM addressing table. */
	if ((mod == 0) && (rm == 6)) {
		int64_t d;
		op->type = O_DISP;
		di->dispSize = 16;
		if (!xop_read_s16(ci, &d)) return FALSE;
		di->disp = (uint64_t)d;
		xop_use_segment(ps, di, INST_PRE_DS);
		return TRUE;
	}
	{
		static const uint8_t mods1[] = {R_BX, R_BX, R_BP, R_BP, R_SI, R_DI, R_BP, R_BX};
		static const uint8_t mods2[] = {R_SI, R_DI, R_SI, R_DI};
		if (rm < 4) {
			op->type = O_MEM;
			di->base = mods1[rm];
			di->usedRegistersMask |= _REGISTERTORCLASS[mods1[rm]];
			op->index = mods2[rm];
			di->usedRegistersMask |= _REGISTERTORCLASS[op->index];
		}
		else {
			op->type = O_SMEM;
			op->index = mods1[rm];
			di->usedRegistersMask |= _REGISTERTORCLASS[op->index];
		}
		if (mod == 1) {
			int64_t d;
			di->dispSize = 8;
			if (!xop_read_s8(ci, &d)) return FALSE;
			di->disp = (uint64_t)d;
		}
		else if (mod == 2) {
			int64_t d;
			di->dispSize = 16;
			if (!xop_read_s16(ci, &d)) return FALSE;
			di->disp = (uint64_t)d;
		}
		xop_use_segment(ps, di, INST_PRE_DS);
	}
	return TRUE;
}

static void xop_set_reg(_DInst* di, _Operand* op, unsigned int reg, uint16_t size)
{
	op->type = O_REG;
	op->index = (uint8_t)reg;
	op->size = size;
	di->usedRegistersMask |= _REGISTERTORCLASS[reg];
}

/* Resolve the effective segment: explicit override prefix wins, else default. */
static void xop_use_segment(_PrefixState* ps, _DInst* di, _iflags defaultSeg)
{
	_iflags ovr = ps->decodedPrefixes & INST_PRE_SEGOVRD_MASK;
	if (ovr && (ovr != defaultSeg)) {
		ps->usedPrefixes |= ovr;
		switch (ovr) {
			case INST_PRE_CS: di->segment = R_CS; break;
			case INST_PRE_SS: di->segment = R_SS; break;
			case INST_PRE_DS: di->segment = R_DS; break;
			case INST_PRE_ES: di->segment = R_ES; break;
			case INST_PRE_FS: di->segment = R_FS; break;
			case INST_PRE_GS: di->segment = R_GS; break;
			default: di->segment = R_DS; break;
		}
	}
	else if (defaultSeg == INST_PRE_SS) di->segment = SEGMENT_DEFAULT | R_SS;
	else di->segment = SEGMENT_DEFAULT | R_DS;
}

_DecodeResult xop_decode(_CodeInfo* ci, _PrefixState* ps, const uint8_t* startCode, _DInst* di)
{
	const uint8_t* p = NULL;
	const uint8_t* q;
	XopInfo xi;
	unsigned int modrm, mod, reg, rm;
	unsigned int w, vvvv;
	_DecodeType effAdrSz;
	_Operand* op;
	unsigned int tail;

	/*
	 * Legacy prefixes may precede the XOP prefix; 0x8F is never a legacy
	 * prefix itself, so the first occurrence inside the instruction is it.
	 * (Bound the scan by the 15 byte instruction limit.)
	 */
	for (q = startCode; q < ci->code; q++) {
		if (*q == 0x8f) {
			p = q;
			break;
		}
	}
	if (p == NULL) goto _Undecodable;

	if (!xop_fill_info(ci, p, &xi)) goto _Undecodable;
	if (ci->codeLen < 1) goto _Undecodable; /* ModRM. */

	memset(di, 0, sizeof(_DInst));
	di->addr = ci->codeOffset & ci->addrMask;

	modrm = *ci->code;
	ci->code += 1;
	ci->codeLen -= 1;

	mod = (modrm >> 6) & 3;
	reg = (modrm >> 3) & 7;
	rm = modrm & 7;
	if (ps->vrex & PREFIX_EX_R) reg += EX_GPR_BASE;

	w = (p[2] & XOP_FL_W) ? 1 : 0;
	/* vvvv is stored already inverted and 4 bits wide. */
	vvvv = ps->vexV;

	/* Effective address size honors the 0x67 override like every other inst. */
	if (ps->decodedPrefixes & INST_PRE_ADDR_SIZE)
		effAdrSz = (ci->dt == Decode64Bits) ? Decode32Bits : Decode16Bits;
	else
		effAdrSz = ci->dt;

	di->base = R_NONE;
	di->segment = R_NONE;
	di->scale = 0;
	di->dispSize = 0;
	di->disp = 0;
	/* Vector instructions operate on the 32 bit operand-size class. */
	FLAG_SET_OPSIZE(di, Decode32Bits);
	FLAG_SET_ADDRSIZE(di, effAdrSz);

	/* Operand 0: destination, always a vector register from ModRM.reg. */
	op = &di->ops[0];
	xop_set_reg(di, op, (xi.vecSize == 256 ? AVXREGS_BASE : SSEREGS_BASE) + reg, xi.vecSize);
	di->opsNo = 1;

	if (xi.isVprot) {
		unsigned int width;
		/*
		 * VPROT xmm1, xmm2/m, count
		 * The selector source is vvvv for the vector form (map 8) and a real
		 * imm8 for the immediate form (map 9).
		 */
		if (xi.opcode == 0x93 || xi.opcode == 0xc3) width = 64;      /* VPROTQ. */
		else if (xi.opcode == 0x92 || xi.opcode == 0xc2) width = 32; /* VPROTD. */
		else if (xi.opcode == 0x91 || xi.opcode == 0xc1) width = 16; /* VPROTW. */
		else width = 8;                                              /* VPROTB. */

		/*
		 * W picks which of the two source positions is supplied by ModRM.
		 * W=0: ModRM is the data source (xmm2/m), vvvv is the count register.
		 * W=1: ModRM is the count register, vvvv is the data source.
		 */
		if (xi.immCount) {
			/* Immediate form is always W=1: ModRM is the data source. */
			op = &di->ops[1];
			if (mod == 3) {
				if (ps->vrex & PREFIX_EX_B) rm += EX_GPR_BASE;
				xop_set_reg(di, op, SSEREGS_BASE + rm, 128);
			}
			else {
				if (!xop_decode_mem(ci, ps, di, effAdrSz, modrm, op)) goto _Undecodable;
				op->size = 128;
			}
			di->opsNo = 2;
		}
		else if (w == 0) {
			/* ops[1] = ModRM data source, ops[2] = vvvv count register. */
			op = &di->ops[1];
			if (mod == 3) {
				if (ps->vrex & PREFIX_EX_B) rm += EX_GPR_BASE;
				xop_set_reg(di, op, SSEREGS_BASE + rm, 128);
			}
			else {
				if (!xop_decode_mem(ci, ps, di, effAdrSz, modrm, op)) goto _Undecodable;
				op->size = 128;
			}
			op = &di->ops[2];
			xop_set_reg(di, op, SSEREGS_BASE + vvvv, width);
			di->opsNo = 3;
		}
		else {
			/* ops[1] = vvvv data source, ops[2] = ModRM count register. */
			op = &di->ops[1];
			xop_set_reg(di, op, SSEREGS_BASE + vvvv, 128);
			op = &di->ops[2];
			if (mod != 3) goto _Undecodable; /* Count must be a register. */
			if (ps->vrex & PREFIX_EX_B) rm += EX_GPR_BASE;
			xop_set_reg(di, op, SSEREGS_BASE + rm, width);
			di->opsNo = 3;
		}
	}
	else {
		/*
		 * VPCMOV / VPPERM:
		 * W=0: ops[1] = vvvv source, ops[2] = ModRM source.
		 * W=1: ops[1] = ModRM source, ops[2] = vvvv source.
		 * The memory form therefore follows W into its semantic slot.
		 */
		unsigned int srcPos = w ? 1 : 2;
		unsigned int vPos = w ? 2 : 1;

		op = &di->ops[srcPos];
		if (mod == 3) {
			if (ps->vrex & PREFIX_EX_B) rm += EX_GPR_BASE;
			xop_set_reg(di, op, (xi.vecSize == 256 ? AVXREGS_BASE : SSEREGS_BASE) + rm, xi.vecSize);
		}
		else {
			if (!xop_decode_mem(ci, ps, di, effAdrSz, modrm, op)) goto _Undecodable;
			op->size = xi.vecSize;
		}

		op = &di->ops[vPos];
		xop_set_reg(di, op, (xi.vecSize == 256 ? AVXREGS_BASE : SSEREGS_BASE) + vvvv, xi.vecSize);
		di->opsNo = 3;

		/*
		 * The tail byte's high nibble is a vector register (the selector),
		 * not a displayed immediate. It is consumed after ModRM/SIB/disp.
		 */
		op = &di->ops[3];
		ci->codeLen -= 1;
		if (ci->codeLen < 0) goto _Undecodable;
		tail = *ci->code;
		ci->code += 1;
		reg = (tail >> 4) & (ci->dt == Decode64Bits ? 0xf : 0x7);
		xop_set_reg(di, op, SSEREGS_BASE + reg, 128);
		di->opsNo = 4;
	}

	if (xi.immCount) {
		/* The trailing byte is a genuine imm8 rotation count. */
		op = &di->ops[2];
		ci->codeLen -= 1;
		if (ci->codeLen < 0) goto _Undecodable;
		di->imm.byte = *ci->code;
		ci->code += 1;
		op->type = O_IMM;
		op->size = 8;
		op->index = 0;
		di->opsNo = 3;
	}

	di->opcode = xi.opcodeId;
	di->flags |= FLAG_DST_WR;
	META_SET_ISC(di, ISC_AVX);
	di->size = (uint8_t)(ci->code - startCode);
	if (di->size > INST_MAXIMUM_SIZE) goto _Undecodable;

	/* Mark the legacy prefixes the instruction actually used. */
	if (ps->decodedPrefixes & INST_PRE_ADDR_SIZE) ps->usedPrefixes |= INST_PRE_ADDR_SIZE;
	di->unusedPrefixesMask = prefixes_set_unused_mask(ps);

	return DECRES_SUCCESS;

_Undecodable:
	return DECRES_INPUTERR;
}
