/*
flow.h

Optional control-flow analysis interface for diStorm3.

Built on top of the regular distorm_decompose decoder (no separate
instruction decoding happens here). Given a contiguous read-only code
buffer, a virtual start address, a decode mode (Decode32Bits or
Decode64Bits) and a set of entry points expressed as buffer offsets,
it recovers the basic blocks that are reachable from those entries,
their raw _DInst instruction sequences and their typed out-edges.

Lifetime rules:
 - The code buffer is only borrowed: it must stay valid for the
   duration of distorm_flow_analyze(). The returned graph does NOT
   reference the code buffer; all instructions are copied out.
 - The returned FlowGraph is owned by the caller and must be released
   with distorm_flow_free().

Determinism: the set and order of entry points does not affect the
result. Blocks are sorted by start offset, out-edges are sorted by
edge type, diagnostics are sorted by their byte range.

This library is licensed under the BSD license. See the file COPYING.
*/

#ifndef DISTORM_FLOW_H
#define DISTORM_FLOW_H

#include "distorm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Value used for FlowEdge::targetOffset when the target cannot be
 * mapped into the supplied buffer (external target or unresolved
 * indirect branch). */
#define FLOW_NO_OFFSET ((uint64_t)-1)

/* Type of a basic block out-edge. Numerical order is also the
 * canonical sort order of edges inside a block. */
typedef enum {
	/* Direct branch target: taken path of a conditional branch,
	 * or the target of a direct unconditional JMP. */
	FLOW_EDGE_TAKEN = 0,
	/* Direct CALL target. The callee itself is not analyzed. */
	FLOW_EDGE_CALL = 1,
	/* Sequential successor: not-taken path of a conditional branch,
	 * or the return site right after a CALL. */
	FLOW_EDGE_FALLTHROUGH = 2,
	/* Indirect JMP/CALL: the target is unknown, no memory is read
	 * from the host to guess it. */
	FLOW_EDGE_UNRESOLVED = 3
} FlowEdgeType;

typedef struct {
	FlowEdgeType type;
	/* Virtual address of the target, using unsigned wrap-around of
	 * the decode mode (32 or 64 bits). 0 for FLOW_EDGE_UNRESOLVED. */
	_OffsetType targetVA;
	/* Buffer offset of the target, or FLOW_NO_OFFSET when the target
	 * is external (outside the buffer) or unresolved. Buffer
	 * membership is always decided by real offsets, never by
	 * comparing (possibly wrapped) virtual addresses. */
	uint64_t targetOffset;
} FlowEdge;

typedef struct {
	/* Buffer offsets, [startOffset, endOffset). */
	uint64_t startOffset;
	uint64_t endOffset;
	/* Owned by the graph: raw decoder output for this block. */
	_DInst* insts;
	unsigned int instCount;
	/* Owned by the graph: typed out-edges, sorted by type. */
	FlowEdge* edges;
	unsigned int edgeCount;
} FlowBlock;

typedef enum {
	/* A new start point (entry or branch target) landed strictly
	 * inside an already decoded instruction. No overlapping block
	 * was created. */
	FLOW_DIAG_OVERLAP = 0,
	/* A reachable byte could not be decoded; the path ends there
	 * (the byte is NOT treated as a DB to skip past). */
	FLOW_DIAG_UNDECODABLE = 1
} FlowDiagKind;

typedef struct {
	FlowDiagKind kind;
	/* FLOW_DIAG_OVERLAP: start of the existing instruction.
	 * FLOW_DIAG_UNDECODABLE: position of the bad byte. */
	uint64_t startA;
	/* FLOW_DIAG_OVERLAP: the conflicting new start point.
	 * FLOW_DIAG_UNDECODABLE: same as startA. */
	uint64_t startB;
	/* Conflicting byte range, [rangeStart, rangeEnd): the bytes of
	 * the existing instruction (OVERLAP) or the single bad byte
	 * (UNDECODABLE). */
	uint64_t rangeStart;
	uint64_t rangeEnd;
} FlowDiag;

typedef struct {
	/* Owned by the graph, sorted by startOffset. */
	FlowBlock* blocks;
	unsigned int blockCount;
	/* Owned by the graph, sorted by byte range. */
	FlowDiag* diags;
	unsigned int diagCount;
} FlowGraph;

/*
 * distorm_flow_analyze
 *
 * Input:
 *   code       - contiguous read-only code buffer (borrowed).
 *   codeLen    - length of the buffer in bytes.
 *   codeOffset - virtual address of code[0].
 *   dt         - Decode32Bits or Decode64Bits (other modes are an
 *                input error).
 *   entries    - array of entry points, as buffer offsets. May be
 *                NULL when entryCount is 0, which yields an empty
 *                graph.
 *   entryCount - number of entries. An entry >= codeLen is an input
 *                error.
 *   graph      - OUT parameter, receives the result. Must be released
 *                with distorm_flow_free(), even on DECRES_MEMORYERR.
 *
 * Semantics:
 *   - Only instructions reachable from the entries are analyzed.
 *   - Conditional direct branches yield TAKEN + FALLTHROUGH edges.
 *   - Unconditional direct JMP yields a TAKEN edge and does not
 *     continue sequentially; indirect JMP yields an UNRESOLVED edge.
 *   - Direct CALL records a CALL edge but analysis only continues at
 *     the return site (FALLTHROUGH edge); indirect CALL keeps an
 *     UNRESOLVED edge plus the FALLTHROUGH successor.
 *   - RET/INT/SYS/HLT terminate a path; CMOV is a normal instruction.
 *   - Branch targets outside the buffer are kept as external edges
 *     (targetOffset == FLOW_NO_OFFSET).
 *   - Shared successors and back edges reuse the same block; a start
 *     point landing on an instruction boundary inside an existing
 *     block splits that block and its edges are updated.
 *
 * Return: DECRES_SUCCESS, DECRES_INPUTERR (bad arguments or an
 * out-of-range entry) or DECRES_MEMORYERR (allocation failure).
 */
_DecodeResult distorm_flow_analyze(const uint8_t* code, uint64_t codeLen,
								   _OffsetType codeOffset, _DecodeType dt,
								   const uint64_t* entries, unsigned int entryCount,
								   FlowGraph* graph);

/* Releases everything owned by the graph. Safe on a zeroed graph and
 * on NULL. Does not touch the caller's code buffer. */
void distorm_flow_free(FlowGraph* graph);

#ifdef __cplusplus
} /* End Of Extern */
#endif

#endif /* DISTORM_FLOW_H */
