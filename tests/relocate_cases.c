/*
relocate_cases.c

Offline relocation test cases for diStorm's relocate.h interface.

All expectations are derived by hand from the documented x86-64 encodings and
independent helper calculations here; the relocation output is never fed back
into the same interface as its own correctness proof (re-decoding is at most
an extra cross-check). No machine code under test is ever executed: the LOOP
family is validated through a restricted interpreter written in this file.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "distorm.h"
#include "mnemonics.h"
#include "relocate.h"

static int g_failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("CHECK_FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		g_failures++; \
		return; \
	} \
} while (0)

static uint32_t rd32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t* p)
{
	uint64_t v = 0;
	unsigned int i;
	for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
	return v;
}

static int32_t rel32_calc(uint64_t dest, uint64_t fromEnd)
{
	return (int32_t)(int64_t)(dest - fromEnd);
}

static int8_t rel8_calc(uint64_t dest, uint64_t fromEnd)
{
	return (int8_t)(int64_t)(dest - fromEnd);
}

/* Mappings are indexed by instruction order; lookups use original offsets. */
static const _RelocMapping* map_at(const _RelocateResult* r, uint64_t origOff)
{
	size_t i;
	for (i = 0; i < r->mappingCount; i++) {
		if (r->mappings[i].origOffset == (_OffsetType)origOff)
			return &r->mappings[i];
	}
	return NULL;
}

static int fields_decode(const uint8_t* code, size_t len, uint64_t addr,
                         _RelocFieldInst* out, unsigned cap, unsigned* usedOut)
{
	_CodeInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.code = code;
	ci.codeLen = (int)len;
	ci.codeOffset = addr;
	ci.dt = Decode64Bits;
	ci.features = DF_NONE;
	return distorm_decompose_fields64(&ci, out, cap, usedOut) == DECRES_SUCCESS;
}

static int plain_decode(const uint8_t* code, size_t len, uint64_t addr,
                        _DInst* out, unsigned cap, unsigned* usedOut)
{
	_CodeInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.code = code;
	ci.codeLen = (int)len;
	ci.codeOffset = addr;
	ci.dt = Decode64Bits;
	ci.features = DF_NONE;
	return distorm_decompose(&ci, out, cap, usedOut) == DECRES_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Group 1: field spans come straight from the decoder's byte stream   */
/* ------------------------------------------------------------------ */

static void case_field_spans(void)
{
	_RelocFieldInst r[16];
	_DInst d[16];
	unsigned n = 0;

	/* MOV r/m64, imm32 sign-extended: RIP disp32 at offset 3, then imm32. */
	static const uint8_t mov[] = {
		0x48,0xc7,0x05,0x78,0x56,0x34,0x12,0xef,0xbe,0xad,0xde
	};
	CHECK(fields_decode(mov, sizeof(mov), 0x4000, r, 16, &n));
	CHECK(n == 1);
	CHECK(r[0].inst.opcode == I_MOV && r[0].inst.size == 11);
	CHECK(r[0].fieldCount == 1);
	CHECK(r[0].fields[0].kind == RFIELD_RIP);
	CHECK(r[0].fields[0].byteOffset == 3);
	CHECK(r[0].fields[0].byteWidth == 4);
	CHECK(r[0].fields[0].target == 0x12349683ULL);
	/* The displacement bytes themselves, and the following immediate. */
	CHECK(mov[3] == 0x78 && mov[6] == 0x12);
	CHECK(r[0].inst.imm.dword == 0xdeadbeef);

	/* PALIGNR: RIP disp32 at offset 5, immediate byte 7 sits at offset 9. */
	static const uint8_t palignr[] = {
		0x66,0x0f,0x3a,0x0f,0x05,0xf0,0xff,0xff,0xff,0x07
	};
	CHECK(fields_decode(palignr, sizeof(palignr), 0x5000, r, 16, &n));
	CHECK(n == 1);
	CHECK(r[0].inst.opcode == I_PALIGNR && r[0].inst.size == 10);
	CHECK(r[0].fieldCount == 1);
	CHECK(r[0].fields[0].kind == RFIELD_RIP);
	CHECK(r[0].fields[0].byteOffset == 5);
	CHECK(r[0].fields[0].byteWidth == 4);
	CHECK(r[0].fields[0].target == 0x4ffaULL);
	CHECK(r[0].inst.imm.byte == 7);

	/* Short Jcc: PC field is one byte at offset 1. */
	static const uint8_t jnz[] = { 0x75, 0x7c };
	CHECK(fields_decode(jnz, sizeof(jnz), 0x1000, r, 16, &n));
	CHECK(n == 1);
	CHECK(r[0].fieldCount == 1);
	CHECK(r[0].fields[0].kind == RFIELD_PC);
	CHECK(r[0].fields[0].byteOffset == 1);
	CHECK(r[0].fields[0].byteWidth == 1);
	CHECK(r[0].fields[0].target == 0x107eULL);

	/* Near CALL: PC field is four bytes at offset 1. */
	static const uint8_t call[] = { 0xe8, 0x00,0x00,0x00,0x00 };
	CHECK(fields_decode(call, sizeof(call), 0x2000, r, 16, &n));
	CHECK(n == 1);
	CHECK(r[0].fields[0].kind == RFIELD_PC);
	CHECK(r[0].fields[0].byteOffset == 1 && r[0].fields[0].byteWidth == 4);
	CHECK(r[0].fields[0].target == 0x2005ULL);

	/* Plain NOP: empty field record. */
	static const uint8_t nop[] = { 0x90 };
	CHECK(fields_decode(nop, sizeof(nop), 0x3000, r, 16, &n));
	CHECK(n == 1 && r[0].fieldCount == 0);

	/* Undecodable entry still pairs 1:1 with an empty field record. */
	static const uint8_t bad[] = { 0x90, 0xf0 };
	CHECK(fields_decode(bad, sizeof(bad), 0x3100, r, 16, &n));
	CHECK(n == 2);
	CHECK(r[0].fieldCount == 0);
	CHECK(r[1].inst.flags == FLAG_NOT_DECODABLE);
	CHECK(r[1].fieldCount == 0);

	/* Mixed batch: records line up with instructions, no carry-over. */
	static const uint8_t mix[] = {
		0x90,                         /* 0: nop, no fields */
		0x75, 0x00,                   /* 1: jnz +0 */
		0x48,0xc7,0x05,0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x00, /* rip mov */
		0xc3                          /* ret */
	};
	CHECK(fields_decode(mix, sizeof(mix), 0x7000, r, 16, &n));
	CHECK(n == 4);
	CHECK(r[0].fieldCount == 0);
	CHECK(r[1].fieldCount == 1 && r[1].fields[0].kind == RFIELD_PC);
	CHECK(r[2].fieldCount == 1 && r[2].fields[0].kind == RFIELD_RIP);
	CHECK(r[2].fields[0].byteOffset == 3);
	CHECK(r[3].fieldCount == 0);

	/* Same input through the plain decompose interface must match. */
	CHECK(plain_decode(mix, sizeof(mix), 0x7000, d, 16, &n));
	CHECK(n == 4);
	CHECK(memcmp(d, &r[0].inst, sizeof(_DInst)) == 0);
	CHECK(memcmp(d + 2, &r[2].inst, sizeof(_DInst)) == 0);

	printf("PASS field_spans\n");
}

/* ------------------------------------------------------------------ */
/* Group 2: short branches widen only when reach is actually lost      */
/* ------------------------------------------------------------------ */

static void case_widening_cascade(void)
{
	_RelocateResult rr;
	uint8_t block[132];
	uint64_t oldStart = 0x10000;
	uint64_t oldEnd;

	/* Witness-style fragment: two internal short Jcc and one external JMP. */
	memset(block, 0x90, sizeof(block));
	block[0] = 0x75; block[1] = 124;             /* jnz -> offset 0x7e */
	block[2] = 0x75; block[3] = 127;             /* jnz -> offset 0x83 */
	block[4] = 0xe9;                             /* jmp 0x200000 (external) */
	{
		int32_t ext = (int32_t)(0x200000 - (oldStart + 9));
		block[5] = (uint8_t)(uint32_t)ext;
		block[6] = (uint8_t)((uint32_t)ext >> 8);
		block[7] = (uint8_t)((uint32_t)ext >> 16);
		block[8] = (uint8_t)((uint32_t)ext >> 24);
	}
	block[131] = 0xc3;
	oldEnd = oldStart + sizeof(block);

	/*
	 * Part A: a slide small enough that every distance keeps its encoding.
	 * The short Jcc stay short and the external JMP stays a near E9: only
	 * displacements are rewritten, no padding and no widening.
	 */
	{
		uint64_t newStart = 0x80000;
		CHECK(distorm_relocate64(block, sizeof(block), oldStart, newStart, &rr) == 1);
		CHECK(rr.failure.code == RLOC_OK);
		CHECK(rr.constTableSize == 0);
		CHECK(rr.mappings[0].origSize == 2 && rr.mappings[0].newSize == 2);
		CHECK(rr.mappings[0].widened == 0);
		CHECK(rr.mappings[1].newSize == 2 && rr.mappings[2].newSize == 5);
		CHECK(rr.code[0] == 0x75 && rr.code[1] == 124);
		CHECK(rr.code[2] == 0x75 && rr.code[3] == 127);
		CHECK(rr.code[4] == 0xe9);
		CHECK(rd32(rr.code + 5) ==
		      (uint32_t)rel32_calc(0x200000, newStart + 9));
		CHECK(rr.continuation == oldEnd);
		distorm_relocate_free(&rr);
	}

	/*
	 * Part B: move past the 4 GiB boundary. The external JMP can no longer use
	 * rel32 and becomes a six-byte RIP-indirect transfer; that single extra
	 * byte pushes the second internal short Jcc to rel8 distance 128, so it
	 * widens, which in turn pushes the first one past 127 as well. Both short
	 * Jcc therefore end up as 0F 8x near forms; this is the witness cascade.
	 */
	{
		uint64_t newStart = 0x100000000ULL;
		const _RelocMapping* m1;
		const _RelocMapping* m2;
		size_t entryOff;
		CHECK(distorm_relocate64(block, sizeof(block), oldStart, newStart, &rr) == 1);
		CHECK(rr.mappings[0].origSize == 2 && rr.mappings[0].newSize == 6);
		CHECK(rr.mappings[0].widened == 1);
		CHECK(rr.mappings[1].origSize == 2 && rr.mappings[1].newSize == 6);
		CHECK(rr.mappings[1].newOffset == 6);
		CHECK(rr.mappings[2].origSize == 5 && rr.mappings[2].newSize == 6);
		CHECK(rr.mappings[2].newOffset == 12);
		CHECK(rr.constTableSize == 16); /* 0x200000 and the continuation. */

		CHECK(rr.code[0] == 0x0f && rr.code[1] == 0x85);
		m1 = map_at(&rr, 0x7e);
		CHECK(m1 != NULL);
		CHECK(rd32(rr.code + 2) ==
		      (uint32_t)rel32_calc(newStart + m1->newOffset, newStart + 6));
		CHECK(rr.code[6] == 0x0f && rr.code[7] == 0x85);
		m2 = map_at(&rr, 0x83);
		CHECK(m2 != NULL);
		CHECK(rd32(rr.code + 8) ==
		      (uint32_t)rel32_calc(newStart + m2->newOffset, newStart + 12));

		/* The external JMP now references a shared 8-byte 0x200000 entry. */
		CHECK(rr.code[12] == 0xff && rr.code[13] == 0x25);
		entryOff = 18 + (size_t)(int64_t)(int32_t)rd32(rr.code + 14);
		CHECK(entryOff >= rr.instructionSize);
		CHECK(rd64(rr.code + entryOff) == 0x200000);
		distorm_relocate_free(&rr);
	}

	/* Small move: a reachable short branch stays short, rel8 only adjusted. */
	{
		uint8_t small[8];
		_RelocateResult s;
		uint64_t ns = oldStart + 0x10;
		const _RelocMapping* m;
		memset(small, 0x90, sizeof(small));
		small[0] = 0x75; small[1] = 5;       /* jnz -> offset 7 */
		small[7] = 0xc3;
		CHECK(distorm_relocate64(small, sizeof(small), oldStart, ns, &s) == 1);
		CHECK(s.mappings[0].newSize == 2 && s.mappings[0].widened == 0);
		CHECK(s.code[0] == 0x75);
		m = map_at(&s, 7);
		CHECK(m != NULL);
		CHECK((int8_t)s.code[1] ==
		      rel8_calc(ns + m->newOffset, ns + 2));
		distorm_relocate_free(&s);
	}

	/*
	 * Cascade trigger: an external branch widens and pushes an internal short
	 * branch past rel8 range, even though the address move alone kept it short.
	 * The external target sits near the negative rel32 boundary, so the +256
	 * forward slide pushes its displacement below INT32_MIN: valid E9 in the
	 * original, but only expressible through the tail table after the move.
	 */
	{
		uint8_t cas[140];
		_RelocateResult c;
		uint64_t co = 0x100000000ULL, cn = 0x100000100ULL;
		int32_t edge = INT32_MIN + 24; /* Reachable originally. */
		uint64_t farTarget = (co + 7) + (int64_t)edge;
		const _RelocMapping* m;
		memset(cas, 0x90, sizeof(cas));
		cas[0] = 0xeb; cas[1] = 0x7f;        /* jmp short -> offset 0x81 */
		cas[2] = 0xe9;                       /* external JMP near -2GiB edge */
		cas[3] = (uint8_t)(uint32_t)edge;
		cas[4] = (uint8_t)((uint32_t)edge >> 8);
		cas[5] = (uint8_t)((uint32_t)edge >> 16);
		cas[6] = (uint8_t)((uint32_t)edge >> 24);
		cas[139] = 0xc3;
		/* Sanity: the original displacement is encodable, the new one is not. */
		CHECK(edge >= INT32_MIN);
		CHECK((int64_t)farTarget - (int64_t)(cn + 7) < INT32_MIN);
		CHECK(distorm_relocate64(cas, sizeof(cas), co, cn, &c) == 1);
		/* The short JMP was forced to near by the external transfer's growth. */
		CHECK(c.mappings[0].origSize == 2 && c.mappings[0].newSize == 5);
		CHECK(c.mappings[0].widened == 1);
		CHECK(c.code[0] == 0xe9);
		CHECK(c.mappings[1].origSize == 5 && c.mappings[1].newSize == 6);
		CHECK(c.constTableSize >= 8);
		m = map_at(&c, 0x81);
		CHECK(m != NULL);
		CHECK(rd32(c.code + 1) ==
		      (uint32_t)rel32_calc(cn + m->newOffset, cn + 5));
		/* The table preserves the original absolute external target. */
		{
			/* External JMP occupies new offsets 5..10; disp32 sits at 7. */
			size_t eoff = 11 + (size_t)(int64_t)(int32_t)rd32(c.code + 7);
			CHECK(eoff >= c.instructionSize);
			CHECK(rd64(c.code + eoff) == farTarget);
		}
		distorm_relocate_free(&c);
	}

	printf("PASS widening_cascade\n");
}

/* ------------------------------------------------------------------ */
/* Group 3: out-of-rel32 transfers share tail table entries            */
/* ------------------------------------------------------------------ */

static void case_far_transfers(void)
{
	uint8_t block[16];
	_RelocateResult rr;
	uint64_t oldStart = 0x10000, newStart = 0x100000000ULL;
	uint64_t oldEnd;
	size_t offA, offCall, offB, offC;

	memset(block, 0x90, sizeof(block));
	block[0] = 0xe9; /* JMP 0x4000 (external) */
	{
		int32_t r = (int32_t)(0x4000 - (oldStart + 5));
		block[1] = (uint8_t)(uint32_t)r;
		block[2] = (uint8_t)((uint32_t)r >> 8);
		block[3] = (uint8_t)((uint32_t)r >> 16);
		block[4] = (uint8_t)((uint32_t)r >> 24);
	}
	block[5] = 0xe8; /* CALL 0x4000 (same absolute target) */
	{
		int32_t r = (int32_t)(0x4000 - (oldStart + 10));
		block[6] = (uint8_t)(uint32_t)r;
		block[7] = (uint8_t)((uint32_t)r >> 8);
		block[8] = (uint8_t)((uint32_t)r >> 16);
		block[9] = (uint8_t)((uint32_t)r >> 24);
	}
	block[10] = 0xe9; /* JMP 0x8000 (different external target) */
	{
		int32_t r = (int32_t)(0x8000 - (oldStart + 15));
		block[11] = (uint8_t)(uint32_t)r;
		block[12] = (uint8_t)((uint32_t)r >> 8);
		block[13] = (uint8_t)((uint32_t)r >> 16);
		block[14] = (uint8_t)((uint32_t)r >> 24);
	}
	block[15] = 0xc3;
	oldEnd = oldStart + sizeof(block);

	CHECK(distorm_relocate64(block, sizeof(block), oldStart, newStart, &rr) == 1);
	CHECK(rr.constTableSize == 24); /* 0x4000, 0x8000 and continuation. */

	/* Both 5-byte direct transfers became six-byte RIP-indirect ones. */
	CHECK(rr.mappings[0].newSize == 6 && rr.code[0] == 0xff && rr.code[1] == 0x25);
	CHECK(rr.mappings[1].newSize == 6 && rr.code[6] == 0xff && rr.code[7] == 0x15);
	CHECK(rr.mappings[2].newSize == 6 && rr.code[12] == 0xff && rr.code[13] == 0x25);

	/*
	 * JMP and CALL share one 8-byte entry for 0x4000. The displacements
	 * themselves differ (different RIP bases) but resolve to one offset.
	 */
	offA = 6 + (size_t)(int64_t)(int32_t)rd32(rr.code + 2);
	offCall = 12 + (size_t)(int64_t)(int32_t)rd32(rr.code + 8);
	CHECK(offA == offCall);
	CHECK(offA >= rr.instructionSize && offA < rr.codeSize);
	CHECK(rd64(rr.code + offA) == 0x4000);

	/* Second absolute target gets its own eight-byte entry. */
	offB = 18 + (size_t)(int64_t)(int32_t)rd32(rr.code + 14);
	CHECK(offB >= rr.instructionSize && offB != offA);
	CHECK(rd64(rr.code + offB) == 0x8000);

	/* CALL return successor is exactly the next relocated instruction. */
	CHECK(rr.mappings[2].newOffset == rr.mappings[1].newOffset + 6);

	/* Tail: indirect jump whose table entry holds the continuation address. */
	CHECK(rr.code[rr.instructionSize - 6] == 0xff);
	CHECK(rr.code[rr.instructionSize - 5] == 0x25);
	{
		uint64_t tailEnd = newStart + rr.instructionSize;
		offC = rr.instructionSize +
		       (size_t)(int64_t)(int32_t)rd32(rr.code + rr.instructionSize - 4);
		CHECK((uint32_t)rel32_calc(newStart + offC, tailEnd) ==
		      rd32(rr.code + rr.instructionSize - 4));
		CHECK(rd64(rr.code + offC) == oldEnd);
	}

	/* Constants live strictly beyond the instruction region. */
	CHECK(rr.instructionSize + rr.constTableSize == rr.codeSize);
	distorm_relocate_free(&rr);
	printf("PASS far_transfers\n");
}

/* ------------------------------------------------------------------ */
/* Group 4: LOOP family semantics checked by a restricted interpreter  */
/* ------------------------------------------------------------------ */

/*
 * Abstract machine for the interpreter: only the counter register (RCX or ECX
 * with proper width/wrap), ZF and the instruction pointer are modeled. The
 * interpreter handles the LOOP family (E0-E3, optional single 67 prefix) and
 * never executes real machine code.
 */
typedef struct {
	uint64_t rcx;
	int zf;
	int use32;
} LoopState;

static void loop_step(const uint8_t* p, size_t ip, LoopState* st,
                      int* taken, size_t* next)
{
	size_t q = ip;
	int addr32 = 0;
	uint8_t op;
	int8_t rel;
	uint64_t counter;

	if (p[q] == 0x67) { addr32 = 1; q++; }
	op = p[q];
	rel = (int8_t)p[q + 1];
	*next = ip + (addr32 ? 3 : 2);

	/* Counter decrement honors the address size width and wraps. */
	counter = st->rcx;
	if (addr32) counter = (uint32_t)counter;
	if (op != 0xe3) { /* JRCXZ/JECXZ does not decrement. */
		counter--;
		if (addr32) counter = (uint32_t)counter;
		st->rcx = counter;
	}
	st->use32 = addr32;

	switch (op) {
		case 0xe2: /* LOOP */
			*taken = (counter != 0);
			break;
		case 0xe1: /* LOOPE/LOOPZ */
			*taken = (counter != 0 && st->zf == 1);
			break;
		case 0xe0: /* LOOPNE/LOOPNZ */
			*taken = (counter != 0 && st->zf == 0);
			break;
		case 0xe3: /* JRCXZ/JECXZ */
			*taken = (counter == 0);
			break;
		default:
			*taken = 0;
			return;
	}
	if (*taken) *next = (size_t)((int64_t)*next + rel);
}

static void case_loop_flags(void)
{
	uint8_t block[32];
	_RelocateResult rr;
	uint64_t oldStart = 0x20000, newStart = 0x30000;
	size_t k;

	memset(block, 0x90, sizeof(block));
	block[0] = 0xe2; block[1] = (uint8_t)0xfe;             /* LOOP self (RCX) */
	block[2] = 0x67; block[3] = 0xe2; block[4] = 0xfd;  /* LOOP self (ECX) */
	block[5] = 0x67; block[6] = 0xe1; block[7] = 0xfd;  /* LOOPE self (ECX) */
	block[8] = 0xe3; block[9] = (uint8_t)0xfe;          /* JRCXZ self */
	block[10] = 0x67; block[11] = 0xe3; block[12] = 0xfd; /* JECXZ self */
	block[13] = 0xe0; block[14] = 0x00;                  /* LOOPNE over next */
	block[31] = 0xc3;

	CHECK(distorm_relocate64(block, sizeof(block), oldStart, newStart, &rr) == 1);

	/* Encodings and prefixes are preserved; only displacements are adjusted. */
	CHECK(rr.code[0] == 0xe2 && rr.code[1] == (uint8_t)0xfe);
	CHECK(rr.code[2] == 0x67 && rr.code[3] == 0xe2 && rr.code[4] == 0xfd);
	CHECK(rr.code[5] == 0x67 && rr.code[6] == 0xe1);
	CHECK(rr.code[8] == 0xe3 && rr.code[9] == (uint8_t)0xfe);
	CHECK(rr.code[10] == 0x67 && rr.code[11] == 0xe3 && rr.code[12] == 0xfd);
	CHECK(rr.mappings[0].newSize == 2 && rr.mappings[1].newSize == 3);

	/*
	 * Interpret both streams on identical abstract states and require
	 * identical taken/fall-through decisions, counter widths and wrap behavior.
	 */
	{
		static const uint64_t counters[] =
			{0, 1, 2, 0x7fffffff, 0xffffffff, 0x100000001ULL, 0xffffffffffffffffULL};
		static const size_t loopIp[] = {0, 2, 5, 8, 10, 13};
		size_t ci;
		for (ci = 0; ci < sizeof(counters) / sizeof(counters[0]); ci++) {
			int zf;
			for (zf = 0; zf <= 1; zf++) {
				for (k = 0; k < sizeof(loopIp) / sizeof(loopIp[0]); k++) {
					LoopState a = { counters[ci], zf, 0 };
					LoopState b = { counters[ci], zf, 0 };
					int ta, tb;
					size_t na, nb;
					const _RelocMapping* m = map_at(&rr, loopIp[k]);
					CHECK(m != NULL);
					loop_step(block, loopIp[k], &a, &ta, &na);
					loop_step(rr.code, m->newOffset, &b, &tb, &nb);
					CHECK(ta == tb);
					CHECK(a.rcx == b.rcx && a.use32 == b.use32);
					/* Taken target must be the moved counterpart instruction. */
					if (ta) {
						const _RelocMapping* tm = map_at(&rr, na);
						CHECK(tm != NULL);
						CHECK(nb == tm->newOffset);
					}
				}
			}
		}
	}
	distorm_relocate_free(&rr);

	/*
	 * A LOOP loses rel8 reach because a branch between it and its target grows
	 * in the new layout: the external JMP below becomes a 6-byte indirect.
	 */
	{
		uint8_t far[140];
		_RelocateResult f;
		uint64_t co = 0x60000, cn = 0x100000000ULL;
		memset(far, 0x90, sizeof(far));
		far[0] = 0xe2; far[1] = 0x7f;   /* LOOP -> original offset 129 */
		far[2] = 0xe9;                  /* external JMP 0x4000: grows by 1 */
		{
			int32_t ext = (int32_t)(0x4000 - (co + 7));
			far[3] = (uint8_t)(uint32_t)ext;
			far[4] = (uint8_t)((uint32_t)ext >> 8);
			far[5] = (uint8_t)((uint32_t)ext >> 16);
			far[6] = (uint8_t)((uint32_t)ext >> 24);
		}
		far[139] = 0xc3;
		CHECK(distorm_relocate64(far, sizeof(far), co, cn, &f) == 0);
		CHECK(f.failure.code == RLOC_OVERFLOW);
		CHECK(f.failure.offset == 0);
		CHECK(f.code == NULL && f.mappings == NULL && f.codeSize == 0);
		distorm_relocate_free(&f);
	}
	printf("PASS loop_flags\n");
}

/* ------------------------------------------------------------------ */
/* Group 5: RIP-relative data addresses are preserved, never remapped  */
/* ------------------------------------------------------------------ */

static void case_rip_data(void)
{
	uint64_t oldStart = 0x70000, newStart = 0x90000;

	/* MOV with RIP disp32 followed by a separate imm32 (witness pattern). */
	{
		uint8_t mov[11] = {
			0x48,0xc7,0x05,0x78,0x56,0x34,0x12,0xef,0xbe,0xad,0xde
		};
		_RelocateResult r;
		uint64_t dataTarget;
		int32_t newDisp;
		CHECK(distorm_relocate64(mov, sizeof(mov), oldStart, newStart, &r) == 1);
		dataTarget = oldStart + 11 + 0x12345678ULL; /* Independently derived. */
		newDisp = rel32_calc(dataTarget, newStart + 11);
		CHECK(r.code[0] == 0x48 && r.code[1] == 0xc7 && r.code[2] == 0x05);
		CHECK(rd32(r.code + 3) == (uint32_t)newDisp);
		/* Immediate after the displacement is byte-identical. */
		CHECK(r.code[7] == 0xef && r.code[8] == 0xbe &&
		      r.code[9] == 0xad && r.code[10] == 0xde);
		/* The effective data address is unchanged by the move. */
		CHECK((uint64_t)(int64_t)newDisp + (newStart + 11) == dataTarget);
		distorm_relocate_free(&r);
	}

	/* PALIGNR: disp32 then the imm8 must survive. */
	{
		uint8_t pa[10] = {
			0x66,0x0f,0x3a,0x0f,0x05,0xf0,0xff,0xff,0xff,0x07
		};
		_RelocateResult r;
		uint64_t dataTarget = oldStart + 10 + (uint64_t)(int64_t)-16;
		int32_t newDisp = rel32_calc(dataTarget, newStart + 10);
		CHECK(distorm_relocate64(pa, sizeof(pa), oldStart, newStart, &r) == 1);
		CHECK(rd32(r.code + 5) == (uint32_t)newDisp);
		CHECK(r.code[9] == 0x07);
		distorm_relocate_free(&r);
	}

	/* Data reference pointing inside the original code region stays put. */
	{
		uint8_t in[8];
		_RelocateResult r;
		uint64_t inside = oldStart + 5;
		int32_t origDisp = rel32_calc(inside, oldStart + 8);
		in[0] = 0x8b; in[1] = 0x05;
		in[2] = (uint8_t)(uint32_t)origDisp;
		in[3] = (uint8_t)((uint32_t)origDisp >> 8);
		in[4] = (uint8_t)((uint32_t)origDisp >> 16);
		in[5] = (uint8_t)((uint32_t)origDisp >> 24);
		in[6] = 0x90; in[7] = 0xc3;
		CHECK(distorm_relocate64(in, sizeof(in), oldStart, newStart, &r) == 1);
		/* Effective address must remain the OLD address, not a moved one. */
		CHECK((uint64_t)(int64_t)(int32_t)rd32(r.code + 2) +
		      (newStart + 8) == inside);
		distorm_relocate_free(&r);
	}

	/* RIP target that disp32 cannot express after the move must be refused. */
	{
		uint8_t bad[8];
		_RelocateResult r;
		uint64_t nearData = 0x1000;
		int32_t origDisp = rel32_calc(nearData, oldStart + 8);
		bad[0] = 0x8b; bad[1] = 0x05;
		bad[2] = (uint8_t)(uint32_t)origDisp;
		bad[3] = (uint8_t)((uint32_t)origDisp >> 8);
		bad[4] = (uint8_t)((uint32_t)origDisp >> 16);
		bad[5] = (uint8_t)((uint32_t)origDisp >> 24);
		bad[6] = 0x90; bad[7] = 0xc3;
		CHECK(distorm_relocate64(bad, sizeof(bad), oldStart,
		                        0x100000000ULL, &r) == 0);
		CHECK(r.failure.code == RLOC_RIP_UNREACHABLE);
		CHECK(r.failure.offset == 0);
		CHECK(r.code == NULL);
		distorm_relocate_free(&r);
	}

	/* Non-RIP, non-branch instructions are copied verbatim. */
	{
		uint8_t plain[6] = { 0x48, 0x89, 0xe5, 0x31, 0xc0, 0xc3 };
		_RelocateResult r;
		CHECK(distorm_relocate64(plain, sizeof(plain), oldStart, newStart, &r) == 1);
		CHECK(memcmp(r.code, plain, sizeof(plain)) == 0);
		distorm_relocate_free(&r);
	}

	/* Indirect JMP stays indirect; its RIP data address is preserved. */
	{
		uint8_t ind[7] = { 0xff, 0x25, 0x00, 0x00, 0x00, 0x00, 0xc3 };
		_RelocateResult r;
		uint64_t dataTarget = 0x1000 + 6;
		CHECK(distorm_relocate64(ind, sizeof(ind), 0x1000, 0x2000, &r) == 1);
		CHECK(r.code[0] == 0xff && r.code[1] == 0x25);
		CHECK(r.mappings[0].newSize == 6 && r.constTableSize == 0);
		CHECK((uint64_t)(int64_t)(int32_t)rd32(r.code + 2) +
		      (0x2000 + 6) == dataTarget);
		distorm_relocate_free(&r);
	}
	printf("PASS rip_data\n");
}

/* ------------------------------------------------------------------ */
/* Group 6: failure diagnostics always name an original offset         */
/* ------------------------------------------------------------------ */

static void case_diagnostics(void)
{
	_RelocateResult r;

	/* Truncated/undecodable tail. */
	{
		uint8_t bad[4] = { 0x90, 0x90, 0x90, 0xf0 };
		CHECK(distorm_relocate64(bad, sizeof(bad), 0x1000, 0x2000, &r) == 0);
		CHECK(r.failure.code == RLOC_UNDECODABLE);
		CHECK(r.failure.offset == 3);
		CHECK(r.code == NULL && r.mappings == NULL && r.codeSize == 0);
		distorm_relocate_free(&r);
	}

	/* Direct branch landing in the middle of an instruction. */
	{
		uint8_t mid[8];
		memset(mid, 0x90, sizeof(mid));
		mid[0] = 0xeb; mid[1] = 0x03; /* jmp -> offset 5 */
		mid[4] = 0x48; mid[5] = 0x89; mid[6] = 0xe5; /* 3-byte MOV at 4 */
		mid[7] = 0xc3;
		CHECK(distorm_relocate64(mid, sizeof(mid), 0x1000, 0x5000, &r) == 0);
		CHECK(r.failure.code == RLOC_BAD_TARGET);
		CHECK(r.failure.offset == 0);
		distorm_relocate_free(&r);
	}

	/* Wrapping original range. */
	{
		uint8_t one[1] = { 0xc3 };
		CHECK(distorm_relocate64(one, 1, UINT64_MAX, 0x1000, &r) == 0);
		CHECK(r.failure.code == RLOC_OVERFLOW);
		distorm_relocate_free(&r);
	}

	/* Prefixed direct branches and XBEGIN are explicitly unsupported. */
	{
		uint8_t csjmp[3] = { 0x2e, 0xeb, 0x00 };
		CHECK(distorm_relocate64(csjmp, sizeof(csjmp), 0x1000, 0x2000, &r) == 0);
		CHECK(r.failure.code == RLOC_UNSUPPORTED && r.failure.offset == 0);
		distorm_relocate_free(&r);
	}
	{
		uint8_t rel16[4] = { 0x66, 0xe9, 0x00, 0x00 };
		CHECK(distorm_relocate64(rel16, sizeof(rel16), 0x1000, 0x2000, &r) == 0);
		CHECK(r.failure.code == RLOC_UNSUPPORTED);
		distorm_relocate_free(&r);
	}
	{
		uint8_t xbegin[6] = { 0xc7, 0xf8, 0x00, 0x00, 0x00, 0x00 };
		CHECK(distorm_relocate64(xbegin, sizeof(xbegin), 0x1000, 0x2000, &r) == 0);
		CHECK(r.failure.code == RLOC_UNSUPPORTED);
		distorm_relocate_free(&r);
	}
	{
		/* 32-bit address size memory operand. */
		uint8_t a32[7] = { 0x67, 0x8b, 0x05, 0x00, 0x00, 0x00, 0x00 };
		CHECK(distorm_relocate64(a32, sizeof(a32), 0x1000, 0x2000, &r) == 0);
		CHECK(r.failure.code == RLOC_UNSUPPORTED);
		distorm_relocate_free(&r);
	}

	/* Empty input: empty success, repeated free is safe. */
	{
		CHECK(distorm_relocate64(NULL, 0, 0x1000, 0x2000, &r) == 1);
		CHECK(r.code == NULL && r.codeSize == 0 && r.mappingCount == 0);
		distorm_relocate_free(&r);
		distorm_relocate_free(&r);
	}

	/* Argument validation. */
	CHECK(distorm_relocate64((const uint8_t*)"x", 1, 0, 0, NULL) == 0);
	{
		CHECK(distorm_relocate64(NULL, 1, 0, 0, &r) == 0);
		CHECK(r.failure.code == RLOC_INPUT);
		distorm_relocate_free(&r);
	}
	printf("PASS diagnostics\n");
}

/* ------------------------------------------------------------------ */
/* Group 7: ABI stability, call interleaving and ownership reuse       */
/* ------------------------------------------------------------------ */

static void case_abi_reuse(void)
{
	static const uint8_t bufA[] = {
		0x75, 0x00, 0x48,0xc7,0x05,0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x00, 0xc3
	};
	static const uint8_t bufB[] = {
		0x66,0x0f,0x3a,0x0f,0x05,0xf0,0xff,0xff,0xff,0x07, 0x90
	};
	_RelocFieldInst fa[16], fb[16], fa2[16];
	_DInst da[16];
	unsigned na = 0, nb = 0, na2 = 0, pa = 0;
	unsigned i;

	CHECK(fields_decode(bufA, sizeof(bufA), 0x7000, fa, 16, &na));
	CHECK(fields_decode(bufB, sizeof(bufB), 0x8000, fb, 16, &nb));
	CHECK(fields_decode(bufA, sizeof(bufA), 0x7000, fa2, 16, &na2));
	CHECK(na == na2 && nb == 2);
	for (i = 0; i < na; i++) {
		CHECK(memcmp(&fa[i].inst, &fa2[i].inst, sizeof(_DInst)) == 0);
		CHECK(fa[i].fieldCount == fa2[i].fieldCount);
	}
	/* The interleaved B call must not have bled into A's records. */
	CHECK(fa[0].fields[0].kind == RFIELD_PC);
	CHECK(fa[1].fields[0].kind == RFIELD_RIP);
	CHECK(fb[0].fields[0].kind == RFIELD_RIP);

	/* Old decompose ABI: identical _DInst stream. */
	CHECK(plain_decode(bufA, sizeof(bufA), 0x7000, da, 16, &pa));
	CHECK(pa == na);
	for (i = 0; i < na; i++)
		CHECK(memcmp(&da[i], &fa[i].inst, sizeof(_DInst)) == 0);

	/* Old text formatting ABI remains intact. */
	{
		_DecodedInst txt[4];
		unsigned used = 0;
		static const uint8_t stac[3] = { 0x0f, 0x01, 0xcb };
		CHECK(distorm_decode(0, stac, 3, Decode32Bits, txt, 4, &used) ==
		      DECRES_SUCCESS);
		CHECK(used == 1);
		CHECK(strcmp((const char*)txt[0].mnemonic.p, "STAC") == 0);
		CHECK(strcmp((const char*)txt[0].instructionHex.p, "0f01cb") == 0);
	}

	/* Failure then success reuse one result struct; input stays untouched. */
	{
		uint8_t good[4] = { 0x90, 0x90, 0x90, 0xc3 };
		uint8_t snapshot[4];
		/* 48 C7 05 is the truncated head of a RIP-relative MOV. */
		uint8_t bad[3] = { 0x48, 0xc7, 0x05 };
		_RelocateResult r;
		memcpy(snapshot, good, sizeof(good));
		CHECK(distorm_relocate64(bad, sizeof(bad), 0x1000, 0x2000, &r) == 0);
		CHECK(r.failure.code == RLOC_UNDECODABLE);
		CHECK(r.code == NULL);
		CHECK(distorm_relocate64(good, sizeof(good), 0x1000, 0x2000, &r) == 1);
		CHECK(r.code != NULL && r.mappingCount == 4);
		CHECK(memcmp(good, snapshot, sizeof(good)) == 0);
		distorm_relocate_free(&r);
		CHECK(r.code == NULL && r.mappings == NULL);
		distorm_relocate_free(&r); /* Idempotent. */
		CHECK(memcmp(good, snapshot, sizeof(good)) == 0);
	}
	printf("PASS abi_reuse\n");
}

int main(void)
{
	case_field_spans();
	case_widening_cascade();
	case_far_transfers();
	case_loop_flags();
	case_rip_data();
	case_diagnostics();
	case_abi_reuse();
	if (g_failures != 0) {
		printf("RELOCATE_FAILURES count=%d\n", g_failures);
		return 1;
	}
	printf("RELOCATE_CASES_OK cases=7\n");
	return 0;
}
