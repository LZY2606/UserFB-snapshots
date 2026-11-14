/* SPDX-FileCopyrightText: © 2024 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_PIC_CALL_ANALYSIS_H
#define RABBITIZER_PIC_CALL_ANALYSIS_H
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/Utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read-only MIPS32 O32 PIC call-source recovery.
 *
 * The caller supplies a pre-recovered control flow graph, normalized
 * relocation records and a GOT slot map. No machine code is executed and
 * no ELF file is parsed. The analysis never mutates its inputs.
 */

#define RABBITIZER_PIC_NO_NODE ((size_t)-1)
#define RABBITIZER_PIC_NO_SYMBOL UINT32_MAX

/** Identity of a symbol: owning object id plus the index in that object's symbol table. */
typedef struct RabbitizerPicSymbolId {
    uint32_t objectId;
    uint32_t symbolIndex; // RABBITIZER_PIC_NO_SYMBOL when the record does not name a symbol
} RabbitizerPicSymbolId;

typedef enum RabbitizerPicRelocKind {
    RABBITIZER_PIC_RELOC_NONE = 0,
    RABBITIZER_PIC_RELOC_GP_DISP_HI16, // %hi(_gp_disp), paired with a GP_DISP_LO16
    RABBITIZER_PIC_RELOC_GP_DISP_LO16, // %lo(_gp_disp), pairNodeIndex points at the HI16 node
    RABBITIZER_PIC_RELOC_CALL16,       // %call16(sym), usually on a GP-relative lw
    RABBITIZER_PIC_RELOC_GOT16,        // %got(sym) / %got_disp(sym), usually on a GP-relative lw
    RABBITIZER_PIC_RELOC_HI16,         // generic %hi(sym)
    RABBITIZER_PIC_RELOC_LO16,         // generic %lo(sym), pairNodeIndex points at the HI16 node
    RABBITIZER_PIC_RELOC_LOCAL_LO16,   // %lo(sym) adjustment on a GOT-derived local symbol
} RabbitizerPicRelocKind;

/**
 * Normalized relocation record.
 *
 * Relocations are matched to instruction nodes by node index and to symbols
 * by symbol table index, never by display name. The explicit addend and the
 * pairing source are supplied by the caller; no implicit ELF addends are used.
 */
typedef struct RabbitizerPicRelocation {
    size_t nodeIndex;
    RabbitizerPicRelocKind kind;
    RabbitizerPicSymbolId symbol;
    int32_t addend;        // explicit addend, already resolved by the caller
    size_t pairNodeIndex;  // RABBITIZER_PIC_NO_NODE when this record is not half of a pair
} RabbitizerPicRelocation;

typedef enum RabbitizerPicGotSource {
    RABBITIZER_PIC_GOT_SOURCE_UNKNOWN = 0,
    RABBITIZER_PIC_GOT_SOURCE_CALL16,
    RABBITIZER_PIC_GOT_SOURCE_GOT16,
    RABBITIZER_PIC_GOT_SOURCE_LOCAL,
} RabbitizerPicGotSource;

/** One entry of the caller-provided GOT map. */
typedef struct RabbitizerPicGotSlot {
    uint32_t objectId;
    int32_t gpOffset;                 // signed offset of the slot relative to GP
    RabbitizerPicSymbolId symbol;     // identity of the value held by the slot
    RabbitizerPicGotSource source;    // how the slot value is produced
} RabbitizerPicGotSlot;

typedef enum RabbitizerPicNodeKind {
    RABBITIZER_PIC_NODE_NORMAL = 0,
    RABBITIZER_PIC_NODE_CALL,         // jal/jalr, delaySlotNode holds its single delay slot node
} RabbitizerPicNodeKind;

typedef struct RabbitizerPicGraphNode {
    uint32_t word;                    // raw instruction word
    uint32_t vram;                    // address of the instruction
    RabbitizerPicNodeKind kind;
    size_t delaySlotNode;             // RABBITIZER_PIC_NO_NODE for non-call nodes
    size_t firstEdge;                 // range into the graph edge array
    size_t edgeCount;
} RabbitizerPicGraphNode;

typedef struct RabbitizerPicGraphEdge {
    size_t fromNodeIndex;
    size_t toNodeIndex;               // call edges represent the post-return continuation
} RabbitizerPicGraphEdge;

/**
 * Pre-recovered control flow graph.
 *
 * Every node executes one instruction. A call node contains jal/jalr plus
 * exactly one ordinary delay slot node; branch delay slots and branch-likely
 * annul paths have already been expanded into ordinary nodes and edges.
 * Nodes unreachable from entryNodeIndex are not analyzed.
 */
typedef struct RabbitizerPicGraph {
    const RabbitizerPicGraphNode *nodes;
    size_t nodeCount;
    const RabbitizerPicGraphEdge *edges;
    size_t edgeCount;
    size_t entryNodeIndex;
    uint32_t entryVram;               // value t9 holds on function entry
} RabbitizerPicGraph;

typedef struct RabbitizerPicAnalysisConfig {
    size_t maxStateCount; // per-node full-state cap; 0 selects the built-in default
} RabbitizerPicAnalysisConfig;

typedef enum RabbitizerPicValueKind {
    RABBITIZER_PIC_VALUE_UNKNOWN = 0,
    RABBITIZER_PIC_VALUE_CONST,       // pure constant, constValue valid
    RABBITIZER_PIC_VALUE_ENTRY,       // the function entry value t9 arrives with; constValue is entryVram
    RABBITIZER_PIC_VALUE_GP_PARTIAL,  // cpload hi/lo accumulation, not yet joined with entry t9
    RABBITIZER_PIC_VALUE_GP,          // complete, consistently associated GP for objectId
    RABBITIZER_PIC_VALUE_GOT,         // value loaded from a GOT slot, symbol identity valid
    RABBITIZER_PIC_VALUE_SYMBOL,      // symbolic hi/lo or local %lo adjusted value
} RabbitizerPicValueKind;

/**
 * A register or argument value.
 *
 * Symbolic values keep their identity (objectId + symbolIndex), explicit
 * addend and the node that produced them. All 32-bit arithmetic wraps modulo 2^32.
 */
typedef struct RabbitizerPicValue {
    RabbitizerPicValueKind kind;
    uint32_t objectId;
    uint32_t symbolIndex;             // RABBITIZER_PIC_NO_SYMBOL when no symbol is attached
    int32_t addend;                   // explicit caller-supplied addend
    uint32_t constValue;              // constant / folded value
    int32_t gpBias;                   // extra addiu offset accumulated on top of GP
    int originNodeIndex;              // node that produced this value, -1 when unknown/entry
    uint32_t reasons;                 // RABBITIZER_PIC_REASON_* bits collected while producing it
} RabbitizerPicValue;

typedef enum RabbitizerPicAnalysisStatus {
    RABBITIZER_PIC_ANALYSIS_OK = 0,
    RABBITIZER_PIC_ANALYSIS_ERROR_NULL,
    RABBITIZER_PIC_ANALYSIS_ERROR_NODE,
    RABBITIZER_PIC_ANALYSIS_ERROR_EDGE,
    RABBITIZER_PIC_ANALYSIS_ERROR_RELOC,
    RABBITIZER_PIC_ANALYSIS_ERROR_GOT,
    RABBITIZER_PIC_ANALYSIS_ERROR_ENTRY,
    RABBITIZER_PIC_ANALYSIS_ERROR_CAPACITY,
} RabbitizerPicAnalysisStatus;

// Bits for RabbitizerPicCallRecord.unknownReasons and RabbitizerPicValue.reasons.
#define RABBITIZER_PIC_REASON_NONE          0x00000000u
#define RABBITIZER_PIC_REASON_CALLEE_UNKNOWN 0x00000001u
#define RABBITIZER_PIC_REASON_ARG0_UNKNOWN   0x00000002u
#define RABBITIZER_PIC_REASON_ARG1_UNKNOWN   0x00000004u
#define RABBITIZER_PIC_REASON_ARG2_UNKNOWN   0x00000008u
#define RABBITIZER_PIC_REASON_ARG3_UNKNOWN   0x00000010u
#define RABBITIZER_PIC_REASON_STATE_LIMIT    0x00000020u
#define RABBITIZER_PIC_REASON_BAD_GOT        0x00000040u
#define RABBITIZER_PIC_REASON_BAD_RELOC      0x00000080u

/** One stable, de-duplicated call observation. */
typedef struct RabbitizerPicCallRecord {
    size_t nodeIndex;                 // call node in the input graph
    uint32_t callVram;                // address of the call instruction
    uint32_t linkVram;                // PC+8 written into the link register
    bool isDirectCall;                // true for a static jal, false for jalr
    uint32_t staticTargetVram;        // valid only when isDirectCall
    RabbitizerPicValue callee;        // source register value before the delay slot
    RabbitizerPicValue args[4];       // a0..a3 read after the delay slot
    uint32_t unknownReasons;          // RABBITIZER_PIC_REASON_* bits
    bool saturated;                   // this observation came from a saturated unknown state
} RabbitizerPicCallRecord;

/**
 * Output container.
 *
 * The caller provides records/recordCapacity; the analysis fills at most
 * recordCapacity entries and returns RABBITIZER_PIC_ANALYSIS_ERROR_CAPACITY
 * if more distinct calls were observed.
 */
typedef struct RabbitizerPicCallResult {
    RabbitizerPicCallRecord *records;
    size_t recordCapacity;
    size_t recordCount;
    size_t peakStateCount;            // largest full-state set held on any node
    bool saturated;                   // true if the state cap was hit somewhere
} RabbitizerPicCallResult;

NODISCARD NON_NULL(1, 7)
RabbitizerPicAnalysisStatus RabbitizerPicAnalysis_analyze(const RabbitizerPicGraph *graph,
                                                          const RabbitizerPicRelocation *relocations,
                                                          size_t relocationCount,
                                                          const RabbitizerPicGotSlot *gotSlots,
                                                          size_t gotSlotCount,
                                                          const RabbitizerPicAnalysisConfig *config,
                                                          RabbitizerPicCallResult *result);

NODISCARD PURE
bool RabbitizerPicSymbolId_equals(const RabbitizerPicSymbolId *a, const RabbitizerPicSymbolId *b);
NODISCARD PURE
bool RabbitizerPicValue_isUnknown(const RabbitizerPicValue *value);
NODISCARD PURE
bool RabbitizerPicValue_equals(const RabbitizerPicValue *a, const RabbitizerPicValue *b);

#ifdef __cplusplus
}
#endif

#endif
