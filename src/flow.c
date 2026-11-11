/*
flow.c

Optional control-flow analysis for diStorm3.

Recovers the basic blocks reachable from a set of entry points by
driving the regular distorm_decompose() decoder one instruction at a
time. No instruction decoding logic of its own lives here.

This library is licensed under the BSD license. See the file COPYING.
*/

#include "config.h"
#include "../include/flow.h"
#include "../include/mnemonics.h"

#include <stdlib.h>

typedef struct {
	const uint8_t* code;
	uint64_t codeLen;
	_OffsetType codeOffset;
	uint64_t mask; /* Address wrap mask of the decode mode. */
	_DecodeType dt;

	/* Result under construction, sorted by startOffset. */
	FlowBlock* blocks;
	unsigned int blockCount, blockCap;

	FlowDiag* diags;
	unsigned int diagCount, diagCap;

	/* Start points waiting to be decoded, sorted ascending so that
	 * discovery order can never influence the outcome. */
	uint64_t* queue;
	unsigned int queueCount, queueCap;

	/* Every start point ever submitted, sorted ascending (dedup). */
	uint64_t* seen;
	unsigned int seenCount, seenCap;
} FlowCtx;

/* Generic growable-array helper. Returns 1 on success, 0 on OOM. */
static int flow_grow(void** arr, size_t elemSize, unsigned int* cap, unsigned int need)
{
	void* p;
	unsigned int ncap;
	if (*cap >= need) return 1;
	ncap = (*cap != 0) ? *cap : 8;
	while (ncap < need) ncap *= 2;
	p = realloc(*arr, (size_t)ncap * elemSize);
	if (p == NULL) return 0;
	*arr = p;
	*cap = ncap;
	return 1;
}

/* Virtual address of a buffer offset, wrapped to the decode mode. */
static _OffsetType flow_va_of(const FlowCtx* ctx, uint64_t off)
{
	return (_OffsetType)((ctx->codeOffset + off) & ctx->mask);
}

/* Buffer offset of a virtual address, or FLOW_NO_OFFSET when the
 * address does not belong to the buffer. Membership is decided purely
 * by real offsets: the wrapped subtraction maps the (possibly
 * wrapped-around) virtual range back onto [0, codeLen). */
static uint64_t flow_off_of_va(const FlowCtx* ctx, _OffsetType va)
{
	uint64_t off = (uint64_t)(((uint64_t)va - (uint64_t)ctx->codeOffset) & ctx->mask);
	return (off < ctx->codeLen) ? off : FLOW_NO_OFFSET;
}

/* Index of the block with the greatest startOffset <= off, or -1. */
static int flow_find_block(const FlowCtx* ctx, uint64_t off)
{
	int lo = 0, hi, mid;
	if (ctx->blockCount == 0) return -1;
	hi = (int)ctx->blockCount - 1;
	if (ctx->blocks[0].startOffset > off) return -1;
	while (lo < hi) {
		mid = (lo + hi + 1) / 2;
		if (ctx->blocks[mid].startOffset <= off) lo = mid;
		else hi = mid - 1;
	}
	return lo;
}

static int flow_u64_contains(const uint64_t* arr, unsigned int count, uint64_t v, unsigned int* pos)
{
	unsigned int lo = 0, hi = count, mid;
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (arr[mid] < v) lo = mid + 1;
		else hi = mid;
	}
	*pos = lo;
	return (lo < count) && (arr[lo] == v);
}

static int flow_u64_insert(uint64_t** arr, unsigned int* count, unsigned int* cap, uint64_t v)
{
	unsigned int pos;
	if (flow_u64_contains(*arr, *count, v, &pos)) return 1;
	if (!flow_grow((void**)arr, sizeof(uint64_t), cap, *count + 1)) return 0;
	memmove(&(*arr)[pos + 1], &(*arr)[pos], (size_t)(*count - pos) * sizeof(uint64_t));
	(*arr)[pos] = v;
	(*count)++;
	return 1;
}

static int flow_add_diag(FlowCtx* ctx, FlowDiagKind kind, uint64_t startA, uint64_t startB,
						 uint64_t rangeStart, uint64_t rangeEnd)
{
	FlowDiag* d;
	unsigned int i;
	/* The same conflict can be reached through different paths; keep it once. */
	for (i = 0; i < ctx->diagCount; i++) {
		d = &ctx->diags[i];
		if ((d->kind == kind) && (d->startA == startA) && (d->startB == startB)) return 1;
	}
	if (!flow_grow((void**)&ctx->diags, sizeof(FlowDiag), &ctx->diagCap, ctx->diagCount + 1)) return 0;
	d = &ctx->diags[ctx->diagCount++];
	d->kind = kind;
	d->startA = startA;
	d->startB = startB;
	d->rangeStart = rangeStart;
	d->rangeEnd = rangeEnd;
	return 1;
}

/*
 * Splits the block at index `idx` so that instruction `instIdx` starts
 * a new block. The new block inherits the tail instructions and all
 * out-edges; the original block is truncated and gets a single
 * fallthrough edge into the new block.
 */
static int flow_split_block(FlowCtx* ctx, int idx, unsigned int instIdx)
{
	FlowBlock nb;
	FlowEdge* fe;
	uint64_t off;
	unsigned int i;

	off = ctx->blocks[idx].startOffset;
	for (i = 0; i < instIdx; i++) off += ctx->blocks[idx].insts[i].size;

	nb.startOffset = off;
	nb.endOffset = ctx->blocks[idx].endOffset;
	nb.instCount = ctx->blocks[idx].instCount - instIdx;
	nb.insts = ( _DInst*)malloc((size_t)nb.instCount * sizeof(_DInst));
	fe = (FlowEdge*)malloc(sizeof(FlowEdge));
	if ((nb.insts == NULL) || (fe == NULL)) {
		free(nb.insts);
		free(fe);
		return 0;
	}
	memcpy(nb.insts, &ctx->blocks[idx].insts[instIdx], (size_t)nb.instCount * sizeof(_DInst));
	/* Move the out-edges to the tail block, they belong to its terminator. */
	nb.edges = ctx->blocks[idx].edges;
	nb.edgeCount = ctx->blocks[idx].edgeCount;

	fe->type = FLOW_EDGE_FALLTHROUGH;
	fe->targetVA = flow_va_of(ctx, off);
	fe->targetOffset = off;

	if (!flow_grow((void**)&ctx->blocks, sizeof(FlowBlock), &ctx->blockCap, ctx->blockCount + 1)) {
		free(nb.insts);
		free(fe);
		return 0;
	}
	/* The new block sorts right after the one being split. */
	memmove(&ctx->blocks[idx + 2], &ctx->blocks[idx + 1],
			(size_t)(ctx->blockCount - (unsigned int)idx - 1) * sizeof(FlowBlock));
	ctx->blockCount++;

	ctx->blocks[idx].instCount = instIdx;
	ctx->blocks[idx].endOffset = off;
	ctx->blocks[idx].edges = fe;
	ctx->blocks[idx].edgeCount = 1;
	ctx->blocks[idx + 1] = nb;
	return 1;
}

/*
 * Resolves a start point against already decoded blocks.
 * Returns 1 when `off` is covered by an existing block (splitting it
 * when `off` falls on an instruction boundary, recording a diagnostic
 * when it falls inside an instruction), 0 when it still needs decoding.
 * Returns -1 on allocation failure.
 */
static int flow_resolve_existing(FlowCtx* ctx, uint64_t off)
{
	FlowBlock* b;
	uint64_t pos, npos;
	unsigned int k;
	int idx = flow_find_block(ctx, off);

	if (idx < 0) return 0;
	b = &ctx->blocks[idx];
	if (off >= b->endOffset) return 0;
	if (b->startOffset == off) return 1;

	pos = b->startOffset;
	for (k = 0; k < b->instCount; k++) {
		if (pos == off) {
			/* New start lands on an instruction boundary: split. */
			if (!flow_split_block(ctx, idx, k)) return -1;
			return 1;
		}
		npos = pos + b->insts[k].size;
		if (off < npos) {
			/* New start lands inside an instruction: diagnose, never overlap. */
			if (!flow_add_diag(ctx, FLOW_DIAG_OVERLAP, pos, off, pos, npos)) return -1;
			return 1;
		}
		pos = npos;
	}
	return 0; /* Unreachable: off < endOffset guarantees a hit above. */
}

/* Submits a newly discovered start point (entry, branch target,
 * fallthrough). Returns 1 on success, 0 on allocation failure. */
static int flow_process_start(FlowCtx* ctx, uint64_t off)
{
	unsigned int pos;
	int r;

	if (flow_u64_contains(ctx->seen, ctx->seenCount, off, &pos)) return 1;
	if (!flow_u64_insert(&ctx->seen, &ctx->seenCount, &ctx->seenCap, off)) return 0;
	r = flow_resolve_existing(ctx, off);
	if (r != 0) return (r > 0);
	/* Not covered by any block yet: queue it for decoding. */
	return flow_u64_insert(&ctx->queue, &ctx->queueCount, &ctx->queueCap, off);
}

/* Decodes exactly one instruction at buffer offset `off` using the
 * regular decompose interface. Returns 1 on success, 0 when the bytes
 * at `off` are not decodable. */
static int flow_decode_one(const FlowCtx* ctx, uint64_t off, _DInst* di)
{
	_CodeInfo ci;
	unsigned int used = 0;
	uint64_t rem = ctx->codeLen - off;

	ci.codeOffset = (_OffsetType)((ctx->codeOffset + off) & ctx->mask);
	ci.addrMask = (_OffsetType)ctx->mask;
	ci.code = ctx->code + off;
	ci.codeLen = (rem > 0x7fffffff) ? 0x7fffffff : (int)rem;
	ci.dt = ctx->dt;
	ci.features = DF_STOP_ON_UNDECODEABLE;

	distorm_decompose(&ci, di, 1, &used);
	return (used == 1) && (di->flags != FLAG_NOT_DECODABLE);
}

static int flow_add_edge(FlowEdge** edges, unsigned int* count, unsigned int* cap,
						 FlowEdgeType type, _OffsetType targetVA, uint64_t targetOffset)
{
	FlowEdge* e;
	if (!flow_grow((void**)edges, sizeof(FlowEdge), cap, *count + 1)) return 0;
	e = &(*edges)[(*count)++];
	e->type = type;
	e->targetVA = targetVA;
	e->targetOffset = targetOffset;
	return 1;
}

/* Computes the masked virtual target address of a direct branch. */
static _OffsetType flow_branch_target(const FlowCtx* ctx, const _DInst* di)
{
	uint64_t t;
	if (di->ops[0].type == O_PTR) t = di->imm.ptr.off;
	else t = (uint64_t)di->addr + (uint64_t)di->imm.addr + di->size;
	return (_OffsetType)(t & ctx->mask);
}

static int flow_is_direct_branch(const _DInst* di)
{
	return (di->opsNo > 0) && ((di->ops[0].type == O_PC) || (di->ops[0].type == O_PTR));
}

/* Inserts a finished block, keeping the array sorted by startOffset. */
static int flow_insert_block(FlowCtx* ctx, uint64_t start, uint64_t end,
							 _DInst* insts, unsigned int instCount,
							 FlowEdge* edges, unsigned int edgeCount)
{
	FlowBlock* b;
	unsigned int pos = 0;

	while ((pos < ctx->blockCount) && (ctx->blocks[pos].startOffset < start)) pos++;
	if (!flow_grow((void**)&ctx->blocks, sizeof(FlowBlock), &ctx->blockCap, ctx->blockCount + 1)) return 0;
	memmove(&ctx->blocks[pos + 1], &ctx->blocks[pos],
			(size_t)(ctx->blockCount - pos) * sizeof(FlowBlock));
	ctx->blockCount++;
	b = &ctx->blocks[pos];
	b->startOffset = start;
	b->endOffset = end;
	b->insts = insts;
	b->instCount = instCount;
	b->edges = edges;
	b->edgeCount = edgeCount;
	return 1;
}

/*
 * Decodes one basic block starting at offset `start` (guaranteed not
 * to be covered by any existing block) and queues its successors.
 * Returns 1 on success, 0 on allocation failure.
 */
static int flow_decode_block(FlowCtx* ctx, uint64_t start)
{
	_DInst* insts = NULL;
	FlowEdge* edges = NULL;
	unsigned int instCount = 0, instCap = 0;
	unsigned int edgeCount = 0, edgeCap = 0;
	uint64_t cur = start, next, end, toff;
	_OffsetType tva;
	_DInst di;
	unsigned int fc;
	int r;

	for (;;) {
		r = flow_resolve_existing(ctx, cur);
		if (r < 0) goto fail;
		if (r > 0) {
			/* Ran into existing code. flow_resolve_existing split a
			 * block when cur is an interior boundary and diagnosed a
			 * mid-instruction conflict. */
			int idx = flow_find_block(ctx, cur);
			if ((idx >= 0) && (ctx->blocks[idx].startOffset == cur)) {
				/* Clean junction: fall through into the existing block. */
				if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_FALLTHROUGH,
								   flow_va_of(ctx, cur), cur)) goto fail;
				end = cur;
				goto finish;
			}
			/* Mid-instruction conflict: the partial block's instruction
			 * boundaries are incompatible with the existing ones, so no
			 * overlapping block may be kept. The diagnostic was already
			 * recorded by flow_resolve for the conflicting start. */
			free(insts);
			free(edges);
			return 1;
		}
		if (cur >= ctx->codeLen) {
			/* Fell off the end of the buffer. */
			end = cur;
			goto finish;
		}
		if (!flow_decode_one(ctx, cur, &di)) {
			/* Reachable but not decodable: terminate the path, do not
			 * treat the byte as a DB and keep going. */
			if (!flow_add_diag(ctx, FLOW_DIAG_UNDECODABLE, cur, cur, cur, cur + 1)) goto fail;
			end = cur;
			goto finish;
		}
		if (!flow_grow((void**)&insts, sizeof(_DInst), &instCap, instCount + 1)) goto fail;
		insts[instCount++] = di;
		next = cur + di.size;
		end = next;

		fc = META_GET_FC(di.meta);
		/* The decoder's table leaves INT 3 (0xCC) unflagged; classify
		 * it as FC_INT so every INT form terminates a path. The stored
		 * _DInst itself is kept exactly as the decoder produced it. */
		if ((fc == FC_NONE) && (di.opcode == I_INT3)) fc = FC_INT;
		switch (fc) {
		case FC_CND_BRANCH:
			tva = flow_branch_target(ctx, &di);
			toff = flow_off_of_va(ctx, tva);
			if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_TAKEN, tva, toff)) goto fail;
			if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_FALLTHROUGH,
							   flow_va_of(ctx, next),
							   (next < ctx->codeLen) ? next : FLOW_NO_OFFSET)) goto fail;
			if (!flow_insert_block(ctx, start, end, insts, instCount, edges, edgeCount)) goto fail;
			if ((toff != FLOW_NO_OFFSET) && !flow_process_start(ctx, toff)) return 0;
			if ((next < ctx->codeLen) && !flow_process_start(ctx, next)) return 0;
			return 1;

		case FC_UNC_BRANCH:
			toff = FLOW_NO_OFFSET;
			if (flow_is_direct_branch(&di)) {
				tva = flow_branch_target(ctx, &di);
				toff = flow_off_of_va(ctx, tva);
				if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_TAKEN, tva, toff)) goto fail;
			}
			else {
				/* Indirect jump: never read host memory to guess. */
				if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_UNRESOLVED,
								   0, FLOW_NO_OFFSET)) goto fail;
			}
			if (!flow_insert_block(ctx, start, end, insts, instCount, edges, edgeCount)) goto fail;
			if ((toff != FLOW_NO_OFFSET) && !flow_process_start(ctx, toff)) return 0;
			return 1;

		case FC_CALL:
			if (flow_is_direct_branch(&di)) {
				tva = flow_branch_target(ctx, &di);
				toff = flow_off_of_va(ctx, tva);
				/* Record the call edge, but do not follow the callee. */
				if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_CALL, tva, toff)) goto fail;
				/* Analysis continues at the return site only. */
				if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_FALLTHROUGH,
								   flow_va_of(ctx, next),
								   (next < ctx->codeLen) ? next : FLOW_NO_OFFSET)) goto fail;
			}
			else {
				/* Indirect call: keep the successor, edges stay sorted
				 * by type (FALLTHROUGH < UNRESOLVED). */
				if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_FALLTHROUGH,
								   flow_va_of(ctx, next),
								   (next < ctx->codeLen) ? next : FLOW_NO_OFFSET)) goto fail;
				if (!flow_add_edge(&edges, &edgeCount, &edgeCap, FLOW_EDGE_UNRESOLVED,
								   0, FLOW_NO_OFFSET)) goto fail;
			}
			if (!flow_insert_block(ctx, start, end, insts, instCount, edges, edgeCount)) goto fail;
			if ((next < ctx->codeLen) && !flow_process_start(ctx, next)) return 0;
			return 1;

		case FC_RET:
		case FC_INT:
		case FC_SYS:
		case FC_HLT:
			goto finish;

		default: /* FC_NONE, FC_CMOV: ordinary sequential instruction. */
			cur = next;
			break;
		}
	}

finish:
	if (instCount == 0) {
		/* Nothing decodable here (diagnostics were already recorded). */
		free(insts);
		free(edges);
		return 1;
	}
	if (!flow_insert_block(ctx, start, end, insts, instCount, edges, edgeCount)) goto fail;
	return 1;

fail:
	free(insts);
	free(edges);
	return 0;
}

static int flow_diag_cmp(const void* a, const void* b)
{
	const FlowDiag* da = (const FlowDiag*)a;
	const FlowDiag* db = (const FlowDiag*)b;
	if (da->rangeStart != db->rangeStart) return (da->rangeStart < db->rangeStart) ? -1 : 1;
	if (da->rangeEnd != db->rangeEnd) return (da->rangeEnd < db->rangeEnd) ? -1 : 1;
	if (da->startA != db->startA) return (da->startA < db->startA) ? -1 : 1;
	if (da->startB != db->startB) return (da->startB < db->startB) ? -1 : 1;
	return (int)da->kind - (int)db->kind;
}

static void flow_ctx_free(FlowCtx* ctx)
{
	unsigned int i;
	for (i = 0; i < ctx->blockCount; i++) {
		free(ctx->blocks[i].insts);
		free(ctx->blocks[i].edges);
	}
	free(ctx->blocks);
	free(ctx->diags);
	free(ctx->queue);
	free(ctx->seen);
}

_DecodeResult distorm_flow_analyze(const uint8_t* code, uint64_t codeLen,
								   _OffsetType codeOffset, _DecodeType dt,
								   const uint64_t* entries, unsigned int entryCount,
								   FlowGraph* graph)
{
	FlowCtx ctx;
	unsigned int i;
	uint64_t s;
	int r;

	if (graph == NULL) return DECRES_INPUTERR;
	memset(graph, 0, sizeof(*graph));

	if ((code == NULL) ||
		((dt != Decode32Bits) && (dt != Decode64Bits)) ||
		((entryCount > 0) && (entries == NULL))) {
		return DECRES_INPUTERR;
	}
	for (i = 0; i < entryCount; i++) {
		if (entries[i] >= codeLen) return DECRES_INPUTERR;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.code = code;
	ctx.codeLen = codeLen;
	ctx.codeOffset = codeOffset;
	ctx.dt = dt;
	ctx.mask = (dt == Decode32Bits) ? 0xffffffffULL : ~(uint64_t)0;

	for (i = 0; i < entryCount; i++) {
		if (!flow_process_start(&ctx, entries[i])) goto memerr;
	}

	while (ctx.queueCount > 0) {
		/* Always decode the lowest pending start point first. */
		s = ctx.queue[0];
		memmove(&ctx.queue[0], &ctx.queue[1], (size_t)(ctx.queueCount - 1) * sizeof(uint64_t));
		ctx.queueCount--;

		r = flow_resolve_existing(&ctx, s);
		if (r < 0) goto memerr;
		if (r > 0) continue; /* Became covered while queued. */
		if (!flow_decode_block(&ctx, s)) goto memerr;
	}

	if (ctx.diagCount > 1) qsort(ctx.diags, ctx.diagCount, sizeof(FlowDiag), flow_diag_cmp);

	graph->blocks = ctx.blocks;
	graph->blockCount = ctx.blockCount;
	graph->diags = ctx.diags;
	graph->diagCount = ctx.diagCount;
	free(ctx.queue);
	free(ctx.seen);
	return DECRES_SUCCESS;

memerr:
	flow_ctx_free(&ctx);
	return DECRES_MEMORYERR;
}

void distorm_flow_free(FlowGraph* graph)
{
	unsigned int i;
	if (graph == NULL) return;
	for (i = 0; i < graph->blockCount; i++) {
		free(graph->blocks[i].insts);
		free(graph->blocks[i].edges);
	}
	free(graph->blocks);
	free(graph->diags);
	memset(graph, 0, sizeof(*graph));
}
