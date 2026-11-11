/*
flow_cases.c

Assertion-based cases for the distorm control-flow analysis interface
(include/flow.h, src/flow.c). Each case prints "PASS <name>" and any
failed check aborts with a non-zero exit code.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "distorm.h"
#include "mnemonics.h"
#include "flow.h"

static const char* g_case = "?";

#define CHECK(cond) do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", g_case, __LINE__, #cond); \
			exit(1); \
		} \
	} while (0)

static int insts_equal(const _DInst* a, const _DInst* b)
{
	/* Field-wise compare: _DInst may contain padding. */
	return (a->addr == b->addr) && (a->size == b->size) &&
		   (a->opcode == b->opcode) && (a->flags == b->flags) &&
		   (a->meta == b->meta) && (a->opsNo == b->opsNo);
}

static int graphs_equal(const FlowGraph* a, const FlowGraph* b)
{
	unsigned int i, j;
	const FlowBlock *ba, *bb;
	if ((a->blockCount != b->blockCount) || (a->diagCount != b->diagCount)) return 0;
	for (i = 0; i < a->blockCount; i++) {
		ba = &a->blocks[i];
		bb = &b->blocks[i];
		if ((ba->startOffset != bb->startOffset) || (ba->endOffset != bb->endOffset) ||
			(ba->instCount != bb->instCount) || (ba->edgeCount != bb->edgeCount)) return 0;
		for (j = 0; j < ba->instCount; j++) {
			if (!insts_equal(&ba->insts[j], &bb->insts[j])) return 0;
		}
		for (j = 0; j < ba->edgeCount; j++) {
			if ((ba->edges[j].type != bb->edges[j].type) ||
				(ba->edges[j].targetVA != bb->edges[j].targetVA) ||
				(ba->edges[j].targetOffset != bb->edges[j].targetOffset)) return 0;
		}
	}
	for (i = 0; i < a->diagCount; i++) {
		if (memcmp(&a->diags[i], &b->diags[i], sizeof(FlowDiag)) != 0) return 0;
	}
	return 1;
}

static void analyze(const uint8_t* code, uint64_t codeLen, _OffsetType va, _DecodeType dt,
					const uint64_t* entries, unsigned int entryCount, FlowGraph* g)
{
	memset(g, 0, sizeof(*g));
	CHECK(distorm_flow_analyze(code, codeLen, va, dt, entries, entryCount, g) == DECRES_SUCCESS);
}

/* A direct jump skips padding bytes: eb 02 90 90 c3. */
static void case_jump_over_data(void)
{
	const uint8_t code[] = { 0xeb, 0x02, 0x90, 0x90, 0xc3 };
	const uint64_t entries[] = { 0 };
	FlowGraph g;

	analyze(code, sizeof(code), 0x1000, Decode64Bits, entries, 1, &g);

	CHECK(g.blockCount == 2);
	CHECK(g.diagCount == 0);

	CHECK(g.blocks[0].startOffset == 0 && g.blocks[0].endOffset == 2);
	CHECK(g.blocks[0].instCount == 1);
	CHECK(g.blocks[0].insts[0].addr == 0x1000);
	CHECK(META_GET_FC(g.blocks[0].insts[0].meta) == FC_UNC_BRANCH);
	CHECK(g.blocks[0].edgeCount == 1);
	CHECK(g.blocks[0].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[0].edges[0].targetVA == 0x1004);
	CHECK(g.blocks[0].edges[0].targetOffset == 4);

	/* The two skipped NOPs (offsets 2,3) are not reachable. */
	CHECK(g.blocks[1].startOffset == 4 && g.blocks[1].endOffset == 5);
	CHECK(g.blocks[1].instCount == 1);
	CHECK(META_GET_FC(g.blocks[1].insts[0].meta) == FC_RET);
	CHECK(g.blocks[1].edgeCount == 0);

	distorm_flow_free(&g);
	CHECK(g.blocks == NULL && g.blockCount == 0);
}

/* Conditional branch diamond whose arms rejoin in one shared block. */
static void case_diamond_shared_block(void)
{
	const uint8_t code[] = {
		0x75, 0x02,       /* 0: JNZ 4 */
		0xeb, 0x02,       /* 2: JMP 6 */
		0xeb, 0x00,       /* 4: JMP 6 */
		0xc3              /* 6: RET  */
	};
	const uint64_t entries[] = { 0 };
	const uint64_t entries_rev[] = { 6, 0 };
	const uint64_t entries_mix[] = { 0, 6 };
	FlowGraph g, g2, g3;

	analyze(code, sizeof(code), 0x2000, Decode64Bits, entries, 1, &g);

	CHECK(g.blockCount == 4);
	CHECK(g.diagCount == 0);

	CHECK(g.blocks[0].startOffset == 0 && g.blocks[0].endOffset == 2);
	CHECK(META_GET_FC(g.blocks[0].insts[0].meta) == FC_CND_BRANCH);
	CHECK(g.blocks[0].edgeCount == 2);
	CHECK(g.blocks[0].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[0].edges[0].targetVA == 0x2004);
	CHECK(g.blocks[0].edges[0].targetOffset == 4);
	CHECK(g.blocks[0].edges[1].type == FLOW_EDGE_FALLTHROUGH);
	CHECK(g.blocks[0].edges[1].targetVA == 0x2002);
	CHECK(g.blocks[0].edges[1].targetOffset == 2);

	CHECK(g.blocks[1].startOffset == 2 && g.blocks[1].endOffset == 4);
	CHECK(g.blocks[1].edgeCount == 1);
	CHECK(g.blocks[1].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[1].edges[0].targetOffset == 6);

	CHECK(g.blocks[2].startOffset == 4 && g.blocks[2].endOffset == 6);
	CHECK(g.blocks[2].edgeCount == 1);
	CHECK(g.blocks[2].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[2].edges[0].targetOffset == 6);

	/* Both arms reuse the very same successor block. */
	CHECK(g.blocks[3].startOffset == 6 && g.blocks[3].endOffset == 7);
	CHECK(META_GET_FC(g.blocks[3].insts[0].meta) == FC_RET);
	CHECK(g.blocks[3].edgeCount == 0);

	/* Entry order and duplicates must not change the result. */
	analyze(code, sizeof(code), 0x2000, Decode64Bits, entries_rev, 2, &g2);
	analyze(code, sizeof(code), 0x2000, Decode64Bits, entries_mix, 2, &g3);
	CHECK(graphs_equal(&g, &g2));
	CHECK(graphs_equal(&g, &g3));

	distorm_flow_free(&g);
	distorm_flow_free(&g2);
	distorm_flow_free(&g3);
}

/* A backward edge into the middle of a block splits it. */
static void case_backward_edge_split(void)
{
	const uint8_t code[] = {
		0x90,             /* 0: NOP */
		0x90,             /* 1: NOP */
		0xeb, 0xfd        /* 2: JMP 1 (back edge) */
	};
	const uint64_t entries[] = { 0 };
	FlowGraph g;

	analyze(code, sizeof(code), 0x4000, Decode64Bits, entries, 1, &g);

	CHECK(g.blockCount == 2);
	CHECK(g.diagCount == 0);

	CHECK(g.blocks[0].startOffset == 0 && g.blocks[0].endOffset == 1);
	CHECK(g.blocks[0].instCount == 1);
	CHECK(g.blocks[0].edgeCount == 1);
	CHECK(g.blocks[0].edges[0].type == FLOW_EDGE_FALLTHROUGH);
	CHECK(g.blocks[0].edges[0].targetOffset == 1);
	CHECK(g.blocks[0].edges[0].targetVA == 0x4001);

	CHECK(g.blocks[1].startOffset == 1 && g.blocks[1].endOffset == 4);
	CHECK(g.blocks[1].instCount == 2);
	CHECK(g.blocks[1].edgeCount == 1);
	CHECK(g.blocks[1].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[1].edges[0].targetOffset == 1); /* Back edge to itself. */
	CHECK(g.blocks[1].edges[0].targetVA == 0x4001);

	distorm_flow_free(&g);
}

/* An entry landing inside an instruction is a diagnostic, not a block. */
static void case_mid_instruction_conflict(void)
{
	const uint8_t code[] = {
		0x66, 0x90,       /* 0: one 2-byte instruction (66 90) */
		0xc3              /* 2: RET */
	};
	const uint64_t entries[] = { 0, 1, 2 };
	const uint64_t entries_rev[] = { 2, 1, 0 };
	FlowGraph g, g2;

	analyze(code, sizeof(code), 0x5000, Decode64Bits, entries, 3, &g);

	CHECK(g.blockCount == 2);
	CHECK(g.blocks[0].startOffset == 0 && g.blocks[0].endOffset == 2);
	CHECK(g.blocks[0].instCount == 1 && g.blocks[0].insts[0].size == 2);
	CHECK(g.blocks[1].startOffset == 2 && g.blocks[1].endOffset == 3);

	/* No overlapping block at offset 1; a diagnostic describes both
	 * start points and the conflicting byte range. */
	CHECK(g.diagCount == 1);
	CHECK(g.diags[0].kind == FLOW_DIAG_OVERLAP);
	CHECK(g.diags[0].startA == 0);
	CHECK(g.diags[0].startB == 1);
	CHECK(g.diags[0].rangeStart == 0);
	CHECK(g.diags[0].rangeEnd == 2);

	analyze(code, sizeof(code), 0x5000, Decode64Bits, entries_rev, 3, &g2);
	CHECK(graphs_equal(&g, &g2));

	distorm_flow_free(&g);
	distorm_flow_free(&g2);
}

/* Direct and indirect calls: only the return site is followed. */
static void case_direct_indirect_call(void)
{
	const uint8_t code[] = {
		0xe8, 0x03, 0x00, 0x00, 0x00,  /* 0: CALL 8 (direct) */
		0xff, 0xd0,                    /* 5: CALL RAX (indirect) */
		0xc3,                          /* 7: RET (return site) */
		0xc3                           /* 8: RET (callee, not followed) */
	};
	const uint64_t entries[] = { 0 };
	FlowGraph g;

	analyze(code, sizeof(code), 0x6000, Decode64Bits, entries, 1, &g);

	CHECK(g.blockCount == 3);
	CHECK(g.diagCount == 0);

	CHECK(g.blocks[0].startOffset == 0 && g.blocks[0].endOffset == 5);
	CHECK(META_GET_FC(g.blocks[0].insts[0].meta) == FC_CALL);
	CHECK(g.blocks[0].edgeCount == 2);
	CHECK(g.blocks[0].edges[0].type == FLOW_EDGE_CALL);
	CHECK(g.blocks[0].edges[0].targetVA == 0x6008);
	CHECK(g.blocks[0].edges[0].targetOffset == 8);
	CHECK(g.blocks[0].edges[1].type == FLOW_EDGE_FALLTHROUGH);
	CHECK(g.blocks[0].edges[1].targetVA == 0x6005);
	CHECK(g.blocks[0].edges[1].targetOffset == 5);

	CHECK(g.blocks[1].startOffset == 5 && g.blocks[1].endOffset == 7);
	CHECK(META_GET_FC(g.blocks[1].insts[0].meta) == FC_CALL);
	CHECK(g.blocks[1].edgeCount == 2);
	CHECK(g.blocks[1].edges[0].type == FLOW_EDGE_FALLTHROUGH);
	CHECK(g.blocks[1].edges[0].targetOffset == 7);
	CHECK(g.blocks[1].edges[1].type == FLOW_EDGE_UNRESOLVED);
	CHECK(g.blocks[1].edges[1].targetOffset == FLOW_NO_OFFSET);

	CHECK(g.blocks[2].startOffset == 7 && g.blocks[2].endOffset == 8);
	CHECK(META_GET_FC(g.blocks[2].insts[0].meta) == FC_RET);

	/* The callee at offset 8 was recorded on the call edge but its
	 * function was never entered: no block starts there. */
	{
		unsigned int i;
		for (i = 0; i < g.blockCount; i++) {
			CHECK(g.blocks[i].startOffset != 8);
		}
	}

	distorm_flow_free(&g);
}

/* 32-bit address wrap-around: VAs wrap, buffer offsets stay real. */
static void case_address_wrap(void)
{
	const uint8_t code[] = {
		0xeb, 0x02,       /* 0: JMP +2 -> VA 0x00000002 (offset 4) */
		0x90, 0x90,       /* 2: skipped NOPs */
		0xeb, 0xfa        /* 4: JMP -6 -> VA 0xfffffffe (offset 0) */
	};
	const uint64_t entries[] = { 0 };
	FlowGraph g;

	analyze(code, sizeof(code), 0xfffffffe, Decode32Bits, entries, 1, &g);

	CHECK(g.blockCount == 2);
	CHECK(g.diagCount == 0);

	CHECK(g.blocks[0].startOffset == 0 && g.blocks[0].endOffset == 2);
	CHECK(g.blocks[0].insts[0].addr == 0xfffffffe);
	CHECK(g.blocks[0].edgeCount == 1);
	CHECK(g.blocks[0].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[0].edges[0].targetVA == 2); /* Wrapped past 4GB. */
	CHECK(g.blocks[0].edges[0].targetOffset == 4); /* Real offset, not external. */

	CHECK(g.blocks[1].startOffset == 4 && g.blocks[1].endOffset == 6);
	CHECK(g.blocks[1].insts[0].addr == 2); /* Wrapped VA of offset 4. */
	CHECK(g.blocks[1].edgeCount == 1);
	CHECK(g.blocks[1].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[1].edges[0].targetVA == 0xfffffffe);
	CHECK(g.blocks[1].edges[0].targetOffset == 0);

	distorm_flow_free(&g);
}

/* Out-of-buffer targets stay external; bad bytes stop the path. */
static void case_external_and_undecodable(void)
{
	const uint8_t code_ext[] = { 0xeb, 0x10 }; /* JMP +0x10, past the end. */
	const uint8_t code_bad[] = { 0xeb, 0x01, 0x90, 0xff }; /* 0xff: truncated. */
	const uint64_t entries[] = { 0 };
	FlowGraph g;

	analyze(code_ext, sizeof(code_ext), 0x3000, Decode64Bits, entries, 1, &g);
	CHECK(g.blockCount == 1);
	CHECK(g.diagCount == 0);
	CHECK(g.blocks[0].edgeCount == 1);
	CHECK(g.blocks[0].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[0].edges[0].targetVA == 0x3012);
	CHECK(g.blocks[0].edges[0].targetOffset == FLOW_NO_OFFSET); /* External. */
	distorm_flow_free(&g);

	analyze(code_bad, sizeof(code_bad), 0x3000, Decode64Bits, entries, 1, &g);
	CHECK(g.blockCount == 1);
	CHECK(g.blocks[0].startOffset == 0 && g.blocks[0].endOffset == 2);
	CHECK(g.blocks[0].edges[0].targetOffset == 3);
	/* The reachable undecodable byte terminates the path with a
	 * position diagnostic instead of being skipped as a DB. */
	CHECK(g.diagCount == 1);
	CHECK(g.diags[0].kind == FLOW_DIAG_UNDECODABLE);
	CHECK(g.diags[0].startA == 3);
	CHECK(g.diags[0].rangeStart == 3);
	CHECK(g.diags[0].rangeEnd == 4);
	distorm_flow_free(&g);
}

/* RET/INT/SYS/HLT terminate; CMOV is ordinary; indirect JMP is unresolved. */
static void case_terminators(void)
{
	const uint8_t code[] = {
		0xc3,                    /* 0: RET */
		0xcc,                    /* 1: INT3 */
		0xf4,                    /* 2: HLT */
		0x0f, 0x05,              /* 3: SYSCALL */
		0x0f, 0x44, 0xc0,        /* 5: CMOVZ EAX, EAX */
		0xeb, 0x00,              /* 8: JMP 10 (falls through CMOV) */
		0xff, 0xe0               /* 10: JMP RAX (indirect) */
	};
	const uint64_t entries[] = { 0, 1, 2, 3, 5 };
	FlowGraph g;

	analyze(code, sizeof(code), 0x7000, Decode64Bits, entries, 5, &g);

	/* RET, INT3, HLT, SYSCALL: each a singleton block with no out-edges. */
	CHECK(g.blockCount == 6);
	CHECK(g.diagCount == 0);
	CHECK(g.blocks[0].startOffset == 0 && g.blocks[0].endOffset == 1);
	CHECK(META_GET_FC(g.blocks[0].insts[0].meta) == FC_RET);
	CHECK(g.blocks[0].edgeCount == 0);
	CHECK(g.blocks[1].startOffset == 1 && g.blocks[1].endOffset == 2);
	/* INT3 terminates the path (the raw _DInst keeps the decoder's
	 * own meta, which leaves 0xCC unflagged in this diStorm version). */
	CHECK(g.blocks[1].insts[0].opcode == I_INT3);
	CHECK(g.blocks[1].edgeCount == 0);
	CHECK(g.blocks[2].startOffset == 2 && g.blocks[2].endOffset == 3);
	CHECK(META_GET_FC(g.blocks[2].insts[0].meta) == FC_HLT);
	CHECK(g.blocks[2].edgeCount == 0);
	CHECK(g.blocks[3].startOffset == 3 && g.blocks[3].endOffset == 5);
	CHECK(META_GET_FC(g.blocks[3].insts[0].meta) == FC_SYS);
	CHECK(g.blocks[3].edgeCount == 0);

	/* CMOV does not break the block: it merges with the JMP at 8. */
	CHECK(g.blocks[4].startOffset == 5 && g.blocks[4].endOffset == 10);
	CHECK(g.blocks[4].instCount == 2);
	CHECK(META_GET_FC(g.blocks[4].insts[0].meta) == FC_CMOV);
	CHECK(g.blocks[4].edgeCount == 1);
	CHECK(g.blocks[4].edges[0].type == FLOW_EDGE_TAKEN);
	CHECK(g.blocks[4].edges[0].targetOffset == 10);

	/* Indirect JMP: only an unresolved edge, no fallthrough. */
	CHECK(g.blocks[5].startOffset == 10 && g.blocks[5].endOffset == 12);
	CHECK(g.blocks[5].edgeCount == 1);
	CHECK(g.blocks[5].edges[0].type == FLOW_EDGE_UNRESOLVED);
	CHECK(g.blocks[5].edges[0].targetOffset == FLOW_NO_OFFSET);

	distorm_flow_free(&g);
}

/* Empty entries give an empty graph; bad input is an input error. */
static void case_input_validation(void)
{
	const uint8_t code[] = { 0x90, 0xc3 };
	const uint64_t entries[] = { 0 };
	const uint64_t bad_entries[] = { 2 };
	FlowGraph g;

	memset(&g, 0, sizeof(g));
	CHECK(distorm_flow_analyze(code, sizeof(code), 0x1000, Decode64Bits,
							   NULL, 0, &g) == DECRES_SUCCESS);
	CHECK(g.blockCount == 0 && g.blocks == NULL);
	CHECK(g.diagCount == 0 && g.diags == NULL);
	distorm_flow_free(&g);

	memset(&g, 0, sizeof(g));
	CHECK(distorm_flow_analyze(code, sizeof(code), 0x1000, Decode64Bits,
							   bad_entries, 1, &g) == DECRES_INPUTERR);

	memset(&g, 0, sizeof(g));
	CHECK(distorm_flow_analyze(code, sizeof(code), 0x1000, Decode16Bits,
							   entries, 1, &g) == DECRES_INPUTERR);

	memset(&g, 0, sizeof(g));
	CHECK(distorm_flow_analyze(NULL, sizeof(code), 0x1000, Decode64Bits,
							   entries, 1, &g) == DECRES_INPUTERR);
}

int main(void)
{
	struct {
		const char* name;
		void (*fn)(void);
	} cases[] = {
		{ "jump-over-data", case_jump_over_data },
		{ "diamond-shared-block", case_diamond_shared_block },
		{ "backward-edge-split", case_backward_edge_split },
		{ "mid-instruction-conflict", case_mid_instruction_conflict },
		{ "direct-indirect-call", case_direct_indirect_call },
		{ "address-wrap", case_address_wrap },
		{ "external-and-undecodable", case_external_and_undecodable },
		{ "terminators", case_terminators },
		{ "input-validation", case_input_validation },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		g_case = cases[i].name;
		cases[i].fn();
		printf("PASS %s\n", cases[i].name);
	}
	return 0;
}
