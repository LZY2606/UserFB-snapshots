/* SPDX-FileCopyrightText: © 2024 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_FLOWGRAPH_H
#define RABBITIZER_FLOWGRAPH_H
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/Utils.h"
#include "instructions/RabbitizerInstrId.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Delay-slot-aware control flow graph.
 *
 * Every node models one instruction dispatch position. The same instruction
 * word gets distinct nodes depending on the context it is dispatched in:
 * ordinary sequential execution or the delay slot of a specific branch.
 * Delay-slot nodes carry the successor that was already selected by their
 * owning branch, so a conditional branch whose two outcomes run the same
 * delay-slot address produces two distinct delay nodes instead of one
 * unconditional successor set.
 *
 * This module only decodes CPU instructions. It never executes code and it
 * never allocates heap memory: every array backing the graph is provided by
 * the caller.
 */

#define RAB_FLOWGRAPH_ADDRESS_NONE UINT32_MAX
#define RAB_FLOWGRAPH_NODE_ID_NONE UINT32_MAX
#define RAB_FLOWGRAPH_REGISTER_NONE (-1)

typedef enum RabbitizerFlowGraphError {
    RAB_FLOWGRAPH_ERROR_OK = 0,
    // A required argument was NULL or the graph struct was not initialised.
    RAB_FLOWGRAPH_ERROR_NULL_ARG,
    // The [startVram, startVram + 4 * wordCount) region wraps around the 32 bit address space.
    RAB_FLOWGRAPH_ERROR_BAD_REGION,
    // An entry address is misaligned or lies outside the provided region.
    RAB_FLOWGRAPH_ERROR_BAD_ENTRY,
    // One of the caller provided arrays ran out of capacity; the graph is marked as incomplete.
    RAB_FLOWGRAPH_ERROR_CAPACITY,
    RAB_FLOWGRAPH_ERROR_MAX,
} RabbitizerFlowGraphError;

typedef enum RabbitizerFlowGraphNodeKind {
    // Ordinary dispatch: after the instruction the flow continues sequentially.
    RAB_FLOWGRAPH_NODE_KIND_NORMAL = 0,
    // Dispatch inside the delay slot of the branch stored in branchVram.
    RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT,
    RAB_FLOWGRAPH_NODE_KIND_MAX,
} RabbitizerFlowGraphNodeKind;

typedef enum RabbitizerFlowGraphOutcome {
    // Only valid for normal nodes.
    RAB_FLOWGRAPH_OUTCOME_NONE = 0,
    // Delay slot of a conditional branch, with the branch taken path selected.
    RAB_FLOWGRAPH_OUTCOME_TAKEN,
    // Delay slot of a conditional branch, with the branch not taken path selected.
    RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN,
    // Delay slot of an unconditional jump or call.
    RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL,
    RAB_FLOWGRAPH_OUTCOME_MAX,
} RabbitizerFlowGraphOutcome;

typedef enum RabbitizerFlowGraphEdgeKind {
    // Normal node falling through to the next sequential instruction.
    RAB_FLOWGRAPH_EDGE_SEQUENTIAL = 0,
    // Branch/jump dispatch entering one of its delay-slot context nodes.
    RAB_FLOWGRAPH_EDGE_DELAY_ENTER,
    // Delay node completing on the taken path of a conditional branch.
    RAB_FLOWGRAPH_EDGE_SLOT_TAKEN,
    // Delay node completing on the not taken path of a conditional branch.
    RAB_FLOWGRAPH_EDGE_SLOT_NOT_TAKEN,
    // Branch likely annul path: straight to PC+8 without running the slot.
    RAB_FLOWGRAPH_EDGE_LIKELY_ANNUL,
    // Delay node of an unconditional direct jump (J).
    RAB_FLOWGRAPH_EDGE_JUMP,
    // Delay node of a call (JAL/JALR). Indirect calls point to RAB_FLOWGRAPH_NODE_ID_NONE.
    RAB_FLOWGRAPH_EDGE_CALL,
    // Terminal edge of a JR $ra delay node.
    RAB_FLOWGRAPH_EDGE_RETURN,
    // Terminal edge of a JR (other register) delay node.
    RAB_FLOWGRAPH_EDGE_UNRESOLVED,
    RAB_FLOWGRAPH_EDGE_MAX,
} RabbitizerFlowGraphEdgeKind;

typedef enum RabbitizerFlowGraphDiagnosticKind {
    // A reachable delay slot address lies outside the provided region.
    RAB_FLOWGRAPH_DIAG_MISSING_SLOT = 0,
    // A reachable instruction word does not decode into a valid CPU instruction.
    RAB_FLOWGRAPH_DIAG_INVALID_INSTRUCTION,
    // A delay slot itself holds a branch or jump.
    RAB_FLOWGRAPH_DIAG_CONTROL_IN_SLOT,
    // A direct branch/jump target is not word aligned.
    RAB_FLOWGRAPH_DIAG_TARGET_MISALIGNED,
    // A direct branch/jump target lies outside the provided region.
    RAB_FLOWGRAPH_DIAG_TARGET_OUT_OF_REGION,
    // A control transfer the graph builder does not model (linking REGIMM, exception returns, ...).
    RAB_FLOWGRAPH_DIAG_UNSUPPORTED_CONTROL,
    // A JALR encoding uses the same register for both rs (target) and rd (link).
    RAB_FLOWGRAPH_DIAG_JALR_RS_EQUALS_RD,
    RAB_FLOWGRAPH_DIAG_MAX,
} RabbitizerFlowGraphDiagnosticKind;

typedef struct RabbitizerFlowGraphNode {
    uint32_t vram;
    uint32_t word;
    RabbitizerInstrId uniqueId;
    RabbitizerFlowGraphNodeKind kind;

    /* Delay-slot context, zeroed/RAB_FLOWGRAPH_*_NONE for normal nodes. */

    // Branch instruction owning this delay slot.
    uint32_t branchVram;
    RabbitizerFlowGraphOutcome outcome;
    // Successor selected by the branch before the slot was dispatched.
    // RAB_FLOWGRAPH_ADDRESS_NONE for indirect jumps/calls.
    uint32_t pendingTargetVram;
    // GPR read at the branch instruction to resolve an indirect target, -1 otherwise.
    // A write to this register inside the slot deliberately does not change this value.
    int8_t targetRegister;
    // True when the pending indirect target is a JR $ra.
    bool isReturn;
    // True when the pending transfer is a call (JAL/JALR).
    bool isCall;
    // GPR receiving the link value on a call, RAB_FLOWGRAPH_REGISTER_NONE when there is no link.
    int8_t linkRegister;
    // PC+8 of the call, recorded before entering the slot; RAB_FLOWGRAPH_ADDRESS_NONE otherwise.
    uint32_t returnSiteVram;
} RabbitizerFlowGraphNode;

typedef struct RabbitizerFlowGraphEdge {
    uint32_t fromNodeIndex;
    // RAB_FLOWGRAPH_NODE_ID_NONE when the flow leaves the modelled region, is unresolved or
    // is a call whose callee is deliberately not followed.
    uint32_t toNodeIndex;
    // Address named by this edge even when it lies outside the region and no node exists for it;
    // RAB_FLOWGRAPH_ADDRESS_NONE for terminals without a computed address (return, unresolved jump).
    uint32_t targetVram;
    RabbitizerFlowGraphEdgeKind kind;
} RabbitizerFlowGraphEdge;

typedef struct RabbitizerFlowGraphDiagnostic {
    RabbitizerFlowGraphDiagnosticKind kind;
    // Branch affected by the problem, RAB_FLOWGRAPH_ADDRESS_NONE when the problem is on the
    // instruction itself rather than on its slot or target.
    uint32_t branchVram;
    // Address at which the problem was observed.
    uint32_t currentVram;
} RabbitizerFlowGraphDiagnostic;

typedef struct RabbitizerFlowGraph {
    RabbitizerFlowGraphNode *nodes;
    size_t nodeCapacity;
    size_t nodeCount;

    RabbitizerFlowGraphEdge *edges;
    size_t edgeCapacity;
    size_t edgeCount;

    RabbitizerFlowGraphDiagnostic *diagnostics;
    size_t diagnosticCapacity;
    size_t diagnosticCount;

    // False when a capacity failure left the graph truncated or the arguments were rejected.
    bool complete;
} RabbitizerFlowGraph;

typedef struct RabbitizerFlowGraphBuildOptions {
    // Instruction words, ordered by ascending address.
    const uint32_t *words;
    size_t wordCount;
    // Non wrapping 32 bit address of words[0]. Address 0 is a valid image address.
    uint32_t startVram;
    // Dispatch addresses to seed the graph from. Duplicates and ordering do not affect the result.
    const uint32_t *entryVrams;
    size_t entryCount;
} RabbitizerFlowGraphBuildOptions;

// Initialises an empty graph backed by the provided caller allocated arrays.
NON_NULL(1)
void RabbitizerFlowGraph_init(RabbitizerFlowGraph *self,
                              RabbitizerFlowGraphNode *nodeBuffer, size_t nodeCapacity,
                              RabbitizerFlowGraphEdge *edgeBuffer, size_t edgeCapacity,
                              RabbitizerFlowGraphDiagnostic *diagnosticBuffer, size_t diagnosticCapacity);

/*
 * Builds the canonical delay-slot-aware flow graph reachable from the given entries.
 *
 * Returns RAB_FLOWGRAPH_ERROR_OK on success. On RAB_FLOWGRAPH_ERROR_CAPACITY the build aborts
 * immediately and self->complete is set to false; the caller arrays may hold a truncated prefix
 * and must not be treated as a complete graph. Diagnostics produced while decoding are part of
 * a successful result and do not cause an error return by themselves.
 */
// Deliberately not marked NON_NULL: NULL pointers are reported as RAB_FLOWGRAPH_ERROR_NULL_ARG.
NODISCARD
RabbitizerFlowGraphError RabbitizerFlowGraph_build(RabbitizerFlowGraph *self,
                                                   const RabbitizerFlowGraphBuildOptions *options);

NODISCARD PURE
const char *RabbitizerFlowGraphError_getName(RabbitizerFlowGraphError error);
NODISCARD PURE
const char *RabbitizerFlowGraphDiagnosticKind_getName(RabbitizerFlowGraphDiagnosticKind kind);
NODISCARD PURE
const char *RabbitizerFlowGraphEdgeKind_getName(RabbitizerFlowGraphEdgeKind kind);
NODISCARD PURE
const char *RabbitizerFlowGraphNodeKind_getName(RabbitizerFlowGraphNodeKind kind);


#ifdef __cplusplus
}
#endif

#endif
