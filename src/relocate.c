/*
relocate.c

diStorm3 - Offline x86-64 code relocation.

See include/relocate.h for the contract. Everything here operates on caller
supplied buffers and virtual addresses only: no process memory is read, and
the produced bytes are never loaded or executed.

Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/

#include "../include/relocate.h"
#include "config.h"
#include "decoder.h"
#include "../include/mnemonics.h"

#include <stdlib.h>

/* Sanity bound so allocation sizes cannot be driven to absurd values. */
#define RELOCATE_MAX_CODE ((size_t)64 * 1024 * 1024)

/* Output encoding forms selected during layout stabilization. */
enum {
	FORM_COPY = 0,        /* Untouched instruction. */
	FORM_RIP_PATCH = 1,   /* Same bytes, RIP disp32 rewritten. */
	FORM_SHORT = 2,       /* rel8 branch, original short form kept. */
	FORM_NEAR_JMP = 3,    /* E9 rel32. */
	FORM_NEAR_CALL = 4,   /* E8 rel32. */
	FORM_NEAR_JCC = 5,    /* 0F 8x rel32. */
	FORM_INDIR_JMP = 6,   /* FF 25 disp32, tail table entry. */
	FORM_INDIR_CALL = 7,  /* FF 15 disp32, tail table entry. */
	FORM_JCC_AROUND = 8     /* 7(cc^1) 06 ; FF 25 disp32. */
};

/* Branch families. */
enum {
	BR_JMP = 1,
	BR_JCC = 2,
	BR_CALL = 3,
	BR_LOOP = 4
};

typedef struct {
	_DInst di;
	_FieldTracker ft;
	size_t origOff;
	size_t origSize;

	int isBranch;
	int branchFamily;
	int condition;       /* Jcc condition code, low nibble of the 7x/8x opcode. */
	int hadAddr67;       /* Single 0x67 prefix (loop/JECXZ form only). */

	int isRip;
	_OffsetType ripTarget;
	unsigned int ripSpanOff;

	_OffsetType branchTarget;

	int form;
	size_t newOff;
	size_t newSize;
	int widened;
} RelocInst;

typedef struct {
	_OffsetType target;
	size_t offset;       /* Offset inside the constant region. */
} TableEntry;

typedef struct {
	RelocInst* items;
	size_t count;
	const uint8_t* code;
	size_t codeLen;
	_OffsetType oldStart;
	_OffsetType oldEnd;
	_OffsetType newStart;

	TableEntry* table;
	size_t tableCap;
	size_t tableCount;

	uint8_t* out;
	size_t instrRegion; /* Instruction bytes including the tail jump. */
} RelocCtx;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int rel_fits(_OffsetType dest, _OffsetType fromEnd, unsigned int bits)
{
	if (dest >= fromEnd) {
		uint64_t d = (uint64_t)dest - (uint64_t)fromEnd;
		return d <= ((uint64_t)1 << (bits - 1)) - 1;
	}
	uint64_t d = (uint64_t)fromEnd - (uint64_t)dest;
	return d <= ((uint64_t)1 << (bits - 1));
}

static int32_t rel32_of(_OffsetType dest, _OffsetType fromEnd)
{
	return (int32_t)((int64_t)(uint64_t)dest - (int64_t)(uint64_t)fromEnd);
}

static int8_t rel8_of(_OffsetType dest, _OffsetType fromEnd)
{
	return (int8_t)((int64_t)(uint64_t)dest - (int64_t)(uint64_t)fromEnd);
}

static void put_rel32(uint8_t* p, int32_t v)
{
	p[0] = (uint8_t)((uint32_t)v);
	p[1] = (uint8_t)((uint32_t)v >> 8);
	p[2] = (uint8_t)((uint32_t)v >> 16);
	p[3] = (uint8_t)((uint32_t)v >> 24);
}

static void put_rel8(uint8_t* p, int8_t v)
{
	p[0] = (uint8_t)v;
}

static void put_u64_le(uint8_t* p, uint64_t v)
{
	unsigned int i;
	for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

/*
 * Inverse Jcc condition for the "branch around the indirect jump" sequence.
 * E/NE (4/5) are each other's complements despite not differing by bit 0.
 */
static uint8_t jcc_inverse(unsigned int cc)
{
	if (cc == 4) return 5;
	if (cc == 5) return 4;
	return (uint8_t)(cc ^ 1);
}

static int is_legacy_prefix(uint8_t b)
{
	switch (b) {
		case 0xf0: case 0xf2: case 0xf3:
		case 0x26: case 0x2e: case 0x36: case 0x3e: case 0x64: case 0x65:
		case 0x66: case 0x67:
			return 1;
		default:
			return 0;
	}
}

static int is_loop_opcode(unsigned int opcode)
{
	return opcode == I_LOOP || opcode == I_LOOPZ || opcode == I_LOOPNZ ||
	       opcode == I_JRCXZ || opcode == I_JECXZ || opcode == I_JCXZ;
}

/* Finds (or creates) the shared 8-byte table entry for an absolute target. */
static int table_get(RelocCtx* cx, _OffsetType target, size_t* indexOut)
{
	size_t i;
	for (i = 0; i < cx->tableCount; i++) {
		if (cx->table[i].target == target) {
			*indexOut = i;
			return 1;
		}
	}
	if (cx->tableCount >= cx->tableCap) return 0;
	cx->table[cx->tableCount].target = target;
	cx->table[cx->tableCount].offset = cx->tableCount * 8;
	*indexOut = cx->tableCount;
	cx->tableCount++;
	return 1;
}

static void fail(_RelocateResult* result, _RelocateError code,
                 _OffsetType offset, const char* message)
{
	memset(result, 0, sizeof(*result));
	result->failure.code = code;
	result->failure.offset = offset;
	result->failure.message = message;
}

/* ------------------------------------------------------------------ */
/* Field-aware decompose entry point                                   */
/* ------------------------------------------------------------------ */

_DLLEXPORT_ _DecodeResult distorm_decompose_fields64(_CodeInfo* ci, _RelocFieldInst result[],
                                         unsigned int maxInstructions,
                                         unsigned int* usedInstructionsCount)
{
	_DecodeResult dr;
	_DInst* dinsts;
	_FieldTracker* trackers;
	unsigned int i, j;

	if (usedInstructionsCount == NULL) return DECRES_SUCCESS;
	if ((ci == NULL) || (ci->codeLen < 0) || (ci->dt != Decode64Bits) ||
	    (ci->code == NULL) || (result == NULL) || (maxInstructions == 0) ||
	    ((ci->features & (DF_MAXIMUM_ADDR16 | DF_MAXIMUM_ADDR32)) ==
	     (DF_MAXIMUM_ADDR16 | DF_MAXIMUM_ADDR32))) {
		return DECRES_INPUTERR;
	}

	dinsts = ( _DInst*)malloc((size_t)maxInstructions * sizeof(_DInst));
	trackers = (_FieldTracker*)malloc((size_t)maxInstructions * sizeof(_FieldTracker));
	if ((dinsts == NULL) || (trackers == NULL)) {
		free(dinsts);
		free(trackers);
		return DECRES_MEMORYERR;
	}

	/* All bookkeeping is local to this call, so interleaved calls never mix. */
	dr = decode_internal_tracked(ci, FALSE, dinsts, maxInstructions,
	                             usedInstructionsCount, trackers);

	for (i = 0; i < *usedInstructionsCount; i++) {
		memset(&result[i], 0, sizeof(_RelocFieldInst));
		result[i].inst = dinsts[i];
		result[i].fieldCount = trackers[i].count;
		for (j = 0; j < trackers[i].count; j++) {
			result[i].fields[j].kind = trackers[i].kind[j] == FIELD_KIND_RIP
			                           ? RFIELD_RIP : RFIELD_PC;
			result[i].fields[j].byteOffset = trackers[i].offset[j];
			result[i].fields[j].byteWidth = trackers[i].width[j];
			if (trackers[i].kind[j] == FIELD_KIND_RIP)
				result[i].fields[j].target = INSTRUCTION_GET_RIP_TARGET(&dinsts[i]);
			else
				result[i].fields[j].target = INSTRUCTION_GET_TARGET(&dinsts[i]);
		}
	}

	free(dinsts);
	free(trackers);
	return dr;
}

/* ------------------------------------------------------------------ */
/* Decode + classification                                             */
/* ------------------------------------------------------------------ */

/*
 * Validates prefixes of a direct branch. Only bare branches, and the loop
 * family with a single 0x67 address-size prefix, are supported. Everything
 * else (segment/lock/REP/REX/operand-size/VEX, redundant prefixes) is refused.
 */
static int classify_branch(RelocCtx* cx, RelocInst* ri, _RelocateResult* result)
{
	const _DInst* di = &ri->di;
	const uint8_t* p = cx->code + ri->origOff;
	size_t left = ri->origSize;
	unsigned int fc = (unsigned int)META_GET_FC(di->meta);
	size_t pfx = 0;
	int has67 = 0;
	uint8_t op;

	if (di->opcode == I_XBEGIN) {
		fail(result, RLOC_UNSUPPORTED, ri->origOff, "XBEGIN is not supported");
		return 0;
	}
	if ((di->opcode == I_JMP_FAR) || (di->opcode == I_CALL_FAR)) {
		fail(result, RLOC_UNSUPPORTED, ri->origOff, "far direct branch is not supported");
		return 0;
	}
	if (fc != FC_UNC_BRANCH && fc != FC_CALL && fc != FC_CND_BRANCH) return 1;

	/* Flow control without O_PC is an indirect CALL/JMP: leave untouched. */
	{
		int hasPc = 0;
		unsigned int k;
		for (k = 0; k < di->opsNo; k++)
			if (di->ops[k].type == O_PC) hasPc = 1;
		if (!hasPc) return 1;
	}

	while ((pfx < left) && is_legacy_prefix(p[pfx])) {
		if (p[pfx] == 0x67) has67 = 1;
		pfx++;
	}
	/* REX or VEX in front of a branch is invalid for every supported form. */
	if ((pfx < left) && ((p[pfx] & 0xf0) == 0x40)) pfx++;

	if (!((pfx == 0) || ((pfx == 1) && has67 && is_loop_opcode(di->opcode)))) {
		fail(result, RLOC_UNSUPPORTED, ri->origOff,
		     "prefixed direct branch is not supported");
		return 0;
	}
	if (pfx + 1 > left) {
		fail(result, RLOC_UNDECODABLE, ri->origOff, "truncated branch");
		return 0;
	}
	op = p[pfx];

	ri->isBranch = 1;
	ri->hadAddr67 = has67;
	ri->branchTarget = INSTRUCTION_GET_TARGET(di);

	if (op == 0xe8) {
		ri->branchFamily = BR_CALL;
	} else if (op == 0xe9 || op == 0xeb) {
		ri->branchFamily = BR_JMP;
	} else if ((op & 0xf0) == 0x70) {
		ri->branchFamily = BR_JCC;
		ri->condition = op & 0x0f;
	} else if (op == 0x0f && pfx + 2 <= left && (p[pfx + 1] & 0xf0) == 0x80) {
		ri->branchFamily = BR_JCC;
		ri->condition = p[pfx + 1] & 0x0f;
	} else if (op >= 0xe0 && op <= 0xe3) {
		ri->branchFamily = BR_LOOP;
	} else {
		fail(result, RLOC_UNSUPPORTED, ri->origOff,
		     "unrecognized direct branch encoding");
		return 0;
	}
	return 1;
}

static int decode_all(RelocCtx* cx, _RelocateResult* result)
{
	_CodeInfo ci;
	_DInst* dinsts;
	_FieldTracker* trackers;
	unsigned int used = 0;
	size_t i;
	_DecodeResult dr;

	dinsts = (_DInst*)malloc(cx->codeLen * sizeof(_DInst));
	trackers = (_FieldTracker*)malloc(cx->codeLen * sizeof(_FieldTracker));
	if ((dinsts == NULL) || (trackers == NULL)) {
		free(dinsts);
		free(trackers);
		fail(result, RLOC_INPUT, 0, "out of memory");
		return 0;
	}

	memset(&ci, 0, sizeof(ci));
	ci.code = cx->code;
	ci.codeLen = (int)cx->codeLen;
	ci.codeOffset = cx->oldStart;
	ci.dt = Decode64Bits;
	ci.features = DF_NONE;

	dr = decode_internal_tracked(&ci, FALSE, dinsts, (unsigned int)cx->codeLen,
	                             &used, trackers);
	if (dr == DECRES_MEMORYERR) {
		free(dinsts);
		free(trackers);
		fail(result, RLOC_INPUT, 0, "decoder capacity failure");
		return 0;
	}

	for (i = 0; i < used; i++) {
		if (dinsts[i].flags == FLAG_NOT_DECODABLE) {
			_OffsetType off = (_OffsetType)((uint64_t)dinsts[i].addr -
			                                (uint64_t)cx->oldStart);
			free(dinsts);
			free(trackers);
			fail(result, RLOC_UNDECODABLE, off, "undecodable or truncated instruction");
			return 0;
		}
	}
	if ((_OffsetType)ci.nextOffset != cx->oldEnd) {
		free(dinsts);
		free(trackers);
		fail(result, RLOC_UNDECODABLE, cx->codeLen, "decoder did not consume the whole buffer");
		return 0;
	}

	cx->count = used;
	for (i = 0; i < used; i++) {
		RelocInst* ri = &cx->items[i];
		unsigned int j;
		memset(ri, 0, sizeof(*ri));
		ri->di = dinsts[i];
		ri->ft = trackers[i];
		ri->origOff = (size_t)((uint64_t)dinsts[i].addr - (uint64_t)cx->oldStart);
		ri->origSize = dinsts[i].size;
		ri->newSize = ri->origSize;
		ri->form = FORM_COPY;

		/* 32-bit address size on a memory operand is explicitly out of scope. */
		if (FLAG_GET_ADDRSIZE(dinsts[i].flags) == Decode32Bits) {
			int hasMem = 0;
			for (j = 0; j < dinsts[i].opsNo; j++) {
				unsigned int t = dinsts[i].ops[j].type;
				if (t == O_MEM || t == O_SMEM || t == O_DISP) hasMem = 1;
			}
			if (hasMem) {
				free(dinsts);
				free(trackers);
				fail(result, RLOC_UNSUPPORTED, ri->origOff,
				     "32-bit address size memory addressing is not supported");
				return 0;
			}
		}

		for (j = 0; j < trackers[i].count; j++) {
			if (trackers[i].kind[j] == FIELD_KIND_RIP) {
				ri->isRip = 1;
				ri->ripTarget = INSTRUCTION_GET_RIP_TARGET(&dinsts[i]);
				ri->ripSpanOff = trackers[i].offset[j];
				ri->form = FORM_RIP_PATCH;
			}
		}

		if (!classify_branch(cx, ri, result)) {
			free(dinsts);
			free(trackers);
			return 0;
		}
	}

	free(dinsts);
	free(trackers);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Target validation and layout decisions                              */
/* ------------------------------------------------------------------ */

/*
 * Maps an absolute branch target to its destination in the new image.
 * Returns 0 on an illegal internal target. *outContinuation is set when the
 * target is exactly the original segment end.
 */
static int resolve_target(RelocCtx* cx, const RelocInst* ri,
                          _OffsetType target, _OffsetType* dest,
                          int* outContinuation, _RelocateResult* result)
{
	size_t j;
	*outContinuation = 0;

	if (target == cx->oldEnd) {
		*dest = cx->oldEnd; /* Continuation keeps its original virtual address. */
		*outContinuation = 1;
		return 1;
	}
	if (target >= cx->oldStart && target < cx->oldEnd) {
		size_t rel = (size_t)((uint64_t)target - (uint64_t)cx->oldStart);
		for (j = 0; j < cx->count; j++) {
			if (cx->items[j].origOff == rel) {
				*dest = cx->newStart + cx->items[j].newOff;
				return 1;
			}
		}
		fail(result, RLOC_BAD_TARGET, ri->origOff,
		     "branch target lands inside an instruction");
		return 0;
	}
	*dest = target; /* External target keeps its absolute address. */
	return 1;
}

/*
 * Chooses the output form and size of one branch for the current layout.
 * Sizes only ever grow between iterations, so the layout reaches a fixed point.
 */
static int choose_form(RelocCtx* cx, RelocInst* ri, _RelocateResult* result)
{
	_OffsetType dest;
	int cont;
	_OffsetType addr = cx->newStart + ri->newOff;
	size_t candidate;

	if (!resolve_target(cx, ri, ri->branchTarget, &dest, &cont, result)) return 0;

	switch (ri->branchFamily) {
		case BR_LOOP:
			/* LOOP/JRCXZ only exist as rel8; no register-free widening exists. */
			candidate = ri->origSize;
			if (!rel_fits(dest, addr + candidate, 8)) {
				fail(result, RLOC_OVERFLOW, ri->origOff,
				     "loop branch is out of rel8 reach in the relocated layout");
				return 0;
			}
			ri->form = FORM_SHORT;
			ri->newSize = candidate;
			break;

		case BR_JMP: {
			const uint8_t* p = cx->code + ri->origOff;
			int originallyShort = p[ri->hadAddr67 ? 1 : 0] == 0xeb;
			if (originallyShort && rel_fits(dest, addr + 2, 8)) {
				ri->form = FORM_SHORT;
				ri->newSize = 2;
			} else if (rel_fits(dest, addr + 5, 32)) {
				ri->form = FORM_NEAR_JMP;
				ri->newSize = 5;
			} else {
				ri->form = FORM_INDIR_JMP;
				ri->newSize = 6;
			}
			break;
		}

		case BR_CALL:
			if (rel_fits(dest, addr + 5, 32)) {
				ri->form = FORM_NEAR_CALL;
				ri->newSize = 5;
			} else {
				ri->form = FORM_INDIR_CALL;
				ri->newSize = 6;
			}
			break;

		case BR_JCC: {
			const uint8_t* p = cx->code + ri->origOff;
			int originallyShort = (p[ri->hadAddr67 ? 1 : 0] & 0xf0) == 0x70;
			if (originallyShort && rel_fits(dest, addr + 2, 8)) {
				ri->form = FORM_SHORT;
				ri->newSize = 2;
			} else if (rel_fits(dest, addr + 6, 32)) {
				ri->form = FORM_NEAR_JCC;
				ri->newSize = 6;
			} else {
				ri->form = FORM_JCC_AROUND;
				ri->newSize = 8;
			}
			break;
		}
	}
	return 1;
}

static int stabilize_layout(RelocCtx* cx, _RelocateResult* result)
{
	size_t iter, i;
	int changed;

	/* Bounded by one growth step per branch, with generous headroom. */
	for (iter = 0; iter < cx->count * 4 + 16; iter++) {
		size_t off = 0;
		changed = 0;
		for (i = 0; i < cx->count; i++) {
			cx->items[i].newOff = off;
			off += cx->items[i].newSize;
		}

		for (i = 0; i < cx->count; i++) {
			RelocInst* ri = &cx->items[i];
			size_t oldSize = ri->newSize;
			int oldForm = ri->form;

			if (ri->isBranch) {
				if (!choose_form(cx, ri, result)) return 0;
			} else if (ri->isRip) {
				_OffsetType end = cx->newStart + ri->newOff + ri->origSize;
				if (!rel_fits(ri->ripTarget, end, 32)) {
					fail(result, RLOC_RIP_UNREACHABLE, ri->origOff,
					     "RIP-relative data address cannot be expressed with disp32");
					return 0;
				}
			}
			if (ri->newSize != oldSize || ri->form != oldForm) changed = 1;
			if (ri->newSize > ri->origSize) ri->widened = 1;
		}
		if (!changed) return 1;
	}
	fail(result, RLOC_OVERFLOW, 0, "layout did not stabilize");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Emission                                                            */
/* ------------------------------------------------------------------ */

/* Materializes a FF 25/FF 15 reference to a shared tail table entry. */
static int emit_indirect(RelocCtx* cx, uint8_t* p, int isCall,
                         _OffsetType instrAddr, size_t instrSize,
                         _OffsetType target)
{
	size_t idx;
	_OffsetType tableEntryAddr, end;
	if (!table_get(cx, target, &idx)) return 0;
	tableEntryAddr = cx->newStart + cx->instrRegion + cx->table[idx].offset;
	end = instrAddr + instrSize;
	if (!rel_fits(tableEntryAddr, end, 32)) return 0;
	p[0] = 0xff;
	p[1] = (uint8_t)(isCall ? 0x15 : 0x25);
	put_rel32(p + 2, rel32_of(tableEntryAddr, end));
	return 1;
}

static int emit_all(RelocCtx* cx, _RelocateResult* result)
{
	size_t i;
	size_t tailSize;
	_OffsetType tailAddr, tailEnd;
	int tailIndirect = 0;
	size_t tailEntry = 0;
	uint8_t* out;
	size_t totalInstr = 0;

	cx->tableCount = 0;

	for (i = 0; i < cx->count; i++) totalInstr += cx->items[i].newSize;
	tailAddr = cx->newStart + totalInstr;

	/* Tail jump: natural fall-through must reach the continuation explicitly. */
	if (rel_fits(cx->oldEnd, tailAddr + 5, 32)) {
		tailSize = 5;
	} else {
		tailSize = 6;
		tailIndirect = 1;
		if (!table_get(cx, cx->oldEnd, &tailEntry)) {
			fail(result, RLOC_OVERFLOW, cx->codeLen, "address table is full");
			return 0;
		}
	}

	/*
	 * Reserve every table entry before sizing the allocation so emission can
	 * only look entries up, never grow the table.
	 */
	for (i = 0; i < cx->count; i++) {
		RelocInst* ri = &cx->items[i];
		_OffsetType dest;
		int cont;
		size_t dummy;
		if (ri->form != FORM_INDIR_JMP && ri->form != FORM_INDIR_CALL &&
		    ri->form != FORM_JCC_AROUND) continue;
		if (!resolve_target(cx, ri, ri->branchTarget, &dest, &cont, result)) return 0;
		if (!table_get(cx, dest, &dummy)) {
			fail(result, RLOC_OVERFLOW, ri->origOff, "address table is full");
			return 0;
		}
	}

	cx->instrRegion = totalInstr + tailSize;
	out = (uint8_t*)malloc(cx->instrRegion + cx->tableCount * 8);
	if (out == NULL) {
		fail(result, RLOC_INPUT, 0, "out of memory");
		return 0;
	}
	cx->out = out;

	for (i = 0; i < cx->count; i++) {
		RelocInst* ri = &cx->items[i];
		uint8_t* p = out + ri->newOff;
		const uint8_t* src = cx->code + ri->origOff;
		_OffsetType instrAddr = cx->newStart + ri->newOff;
		_OffsetType dest;
		int cont;

		if (ri->form == FORM_COPY) {
			memcpy(p, src, ri->origSize);
			continue;
		}

		if (ri->form == FORM_RIP_PATCH) {
			int32_t d;
			_OffsetType end = instrAddr + ri->origSize;
			memcpy(p, src, ri->origSize);
			if (!rel_fits(ri->ripTarget, end, 32)) {
				fail(result, RLOC_RIP_UNREACHABLE, ri->origOff,
				     "RIP-relative data address cannot be expressed with disp32");
				return 0;
			}
			d = rel32_of(ri->ripTarget, end);
			put_rel32(p + ri->ripSpanOff, d);
			continue;
		}

		if (!resolve_target(cx, ri, ri->branchTarget, &dest, &cont, result)) return 0;

		switch (ri->form) {
			case FORM_SHORT: {
				/* LOOP/JRCXZ keep their optional 0x67 prefix; Jcc/JMP do not. */
				size_t keep = ri->hadAddr67 ? 1 : 0;
				if (keep) p[0] = 0x67;
				p[keep] = src[keep];
				put_rel8(p + keep + 1,
				         rel8_of(dest, instrAddr + ri->newSize));
				break;
			}
			case FORM_NEAR_JMP:
				p[0] = 0xe9;
				put_rel32(p + 1, rel32_of(dest, instrAddr + 5));
				break;
			case FORM_NEAR_CALL:
				p[0] = 0xe8;
				put_rel32(p + 1, rel32_of(dest, instrAddr + 5));
				break;
			case FORM_NEAR_JCC:
				p[0] = 0x0f;
				p[1] = (uint8_t)(0x80 + ri->condition);
				put_rel32(p + 2, rel32_of(dest, instrAddr + 6));
				break;
			case FORM_INDIR_JMP:
				if (!emit_indirect(cx, p, 0, instrAddr, 6, dest)) {
					fail(result, RLOC_OVERFLOW, ri->origOff,
					     "tail table is out of rel32 reach");
					return 0;
				}
				break;
			case FORM_INDIR_CALL:
				if (!emit_indirect(cx, p, 1, instrAddr, 6, dest)) {
					fail(result, RLOC_OVERFLOW, ri->origOff,
					     "tail table is out of rel32 reach");
					return 0;
				}
				break;
			case FORM_JCC_AROUND: {
				size_t idx;
				_OffsetType entryAddr, end = instrAddr + 8;
				if (!table_get(cx, dest, &idx)) {
					fail(result, RLOC_OVERFLOW, ri->origOff, "address table is full");
					return 0;
				}
				entryAddr = cx->newStart + cx->instrRegion + cx->table[idx].offset;
				if (!rel_fits(entryAddr, end, 32)) {
					fail(result, RLOC_OVERFLOW, ri->origOff,
					     "tail table is out of rel32 reach");
					return 0;
				}
				/* Inverted condition branches around the indirect jump when not taken. */
				p[0] = (uint8_t)(0x70 + jcc_inverse(ri->condition));
				p[1] = 6;
				p[2] = 0xff;
				p[3] = 0x25;
				put_rel32(p + 4, rel32_of(entryAddr, end));
				break;
			}
			default:
				fail(result, RLOC_OVERFLOW, ri->origOff, "internal form error");
				return 0;
		}
	}

	/* Tail jump itself. */
	tailEnd = tailAddr + tailSize;
	if (!tailIndirect) {
		out[cx->instrRegion - 5] = 0xe9;
		put_rel32(out + cx->instrRegion - 4, rel32_of(cx->oldEnd, tailEnd));
	} else {
		_OffsetType entryAddr = cx->newStart + cx->instrRegion +
		                        cx->table[tailEntry].offset;
		uint8_t* tp = out + cx->instrRegion - 6;
		tp[0] = 0xff;
		tp[1] = 0x25;
		put_rel32(tp + 2, rel32_of(entryAddr, tailEnd));
	}

	/* Constant region: unique 8-byte absolute targets, never interleaved. */
	for (i = 0; i < cx->tableCount; i++) {
		put_u64_le(out + cx->instrRegion + cx->table[i].offset,
		           (uint64_t)cx->table[i].target);
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

_DLLEXPORT_ int distorm_relocate64(const uint8_t* code, size_t codeLen,
                       _OffsetType oldStart, _OffsetType newStart,
                       _RelocateResult* result)
{
	RelocCtx cx;
	size_t i;

	if (result == NULL) return 0;
	memset(result, 0, sizeof(*result));
	result->newStartAddr = newStart;
	result->continuation = oldStart + codeLen;

	if (codeLen == 0) {
		/* A zero-length input (including a NULL pointer) is an empty success. */
		return 1;
	}
	if (code == NULL) {
		fail(result, RLOC_INPUT, 0, "null code buffer");
		return 0;
	}
	if (codeLen > RELOCATE_MAX_CODE) {
		fail(result, RLOC_INPUT, 0, "code buffer too large");
		return 0;
	}
	/* Old range must not wrap. */
	if ((uint64_t)oldStart + codeLen < (uint64_t)oldStart) {
		fail(result, RLOC_OVERFLOW, 0, "original address range wraps around");
		return 0;
	}

	memset(&cx, 0, sizeof(cx));
	cx.code = code;
	cx.codeLen = codeLen;
	cx.oldStart = oldStart;
	cx.oldEnd = oldStart + codeLen;
	cx.newStart = newStart;
	cx.items = (RelocInst*)calloc(codeLen, sizeof(RelocInst));
	/* At most one table entry per branch plus one for the continuation. */
	cx.table = (TableEntry*)calloc(codeLen + 1, sizeof(TableEntry));
	if (cx.items == NULL || cx.table == NULL) {
		free(cx.items);
		free(cx.table);
		fail(result, RLOC_INPUT, 0, "out of memory");
		return 0;
	}
	cx.tableCap = codeLen + 1;

	if (!decode_all(&cx, result)) goto failed;
	if (!stabilize_layout(&cx, result)) goto failed;
	if (!emit_all(&cx, result)) goto failed;

	/* New range, including continuation and the constant region, must not wrap. */
	{
		size_t total = cx.instrRegion + cx.tableCount * 8;
		uint64_t end64 = (uint64_t)newStart + total;
		if (end64 < (uint64_t)newStart) {
			fail(result, RLOC_OVERFLOW, 0, "relocated address range wraps around");
			goto failed;
		}
	}

	/* Success: hand ownership over, nothing half-built is exposed on errors. */
	result->code = cx.out;
	result->codeSize = cx.instrRegion + cx.tableCount * 8;
	result->instructionSize = cx.instrRegion;
	result->constTableSize = cx.tableCount * 8;
	result->mappingCount = cx.count;
	result->mappings = (_RelocMapping*)malloc(cx.count * sizeof(_RelocMapping));
	if (result->mappings == NULL) {
		free(result->code);
		result->code = NULL;
		fail(result, RLOC_INPUT, 0, "out of memory");
		goto failed;
	}
	for (i = 0; i < cx.count; i++) {
		RelocInst* ri = &cx.items[i];
		result->mappings[i].origOffset = ri->origOff;
		result->mappings[i].newOffset = ri->newOff;
		result->mappings[i].origSize = (uint16_t)ri->origSize;
		result->mappings[i].newSize = (uint16_t)ri->newSize;
		result->mappings[i].widened = (uint8_t)ri->widened;
	}
	result->failure.code = RLOC_OK;
	free(cx.items);
	free(cx.table);
	return 1;

failed:
	free(cx.out);
	free(cx.items);
	free(cx.table);
	return 0;
}

_DLLEXPORT_ void distorm_relocate_free(_RelocateResult* result)
{
	if (result == NULL) return;
	free(result->code);
	free(result->mappings);
	memset(result, 0, sizeof(*result));
}
