/* SPDX-FileCopyrightText: © 2024 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "analysis/RabbitizerFlowGraph.h"

#include <stdlib.h>
#include <string.h>

#include "common/RabbitizerConfig.h"
#include "instructions/RabbitizerInstruction.h"
#include "instructions/RabbitizerInstrDescriptor.h"
#include "instructions/RabbitizerRegisterDescriptor.h"
#include "instructions/RabbitizerRegister.h"

#define RAB_FG_JUMP_HIGH_MASK 0xF0000000u
#define RAB_FG_ADDRESS_SPACE_SIZE 0x100000000ULL

typedef struct RabbitizerFlowGraphPending {
    uint32_t branchVram;
    RabbitizerFlowGraphOutcome outcome;
    uint32_t targetVram;
    int8_t targetRegister;
    bool isReturn;
    bool isCall;
    int8_t linkRegister;
    uint32_t returnSiteVram;
} RabbitizerFlowGraphPending;

static const RabbitizerFlowGraphPending RAB_FG_NO_PENDING = {
    .branchVram = RAB_FLOWGRAPH_ADDRESS_NONE,
    .outcome = RAB_FLOWGRAPH_OUTCOME_NONE,
    .targetVram = RAB_FLOWGRAPH_ADDRESS_NONE,
    .targetRegister = RAB_FLOWGRAPH_REGISTER_NONE,
    .isReturn = false,
    .isCall = false,
    .linkRegister = RAB_FLOWGRAPH_REGISTER_NONE,
    .returnSiteVram = RAB_FLOWGRAPH_ADDRESS_NONE,
};

typedef struct RabbitizerFlowGraphBuilder {
    RabbitizerFlowGraph *graph;
    const uint32_t *words;
    size_t wordCount;
    uint32_t startVram;
    uint64_t regionEnd; // exclusive, never wraps
} RabbitizerFlowGraphBuilder;


const char *RabbitizerFlowGraphError_getName(RabbitizerFlowGraphError error) {
    switch (error) {
        case RAB_FLOWGRAPH_ERROR_OK:
            return "OK";
        case RAB_FLOWGRAPH_ERROR_NULL_ARG:
            return "NULL_ARG";
        case RAB_FLOWGRAPH_ERROR_BAD_REGION:
            return "BAD_REGION";
        case RAB_FLOWGRAPH_ERROR_BAD_ENTRY:
            return "BAD_ENTRY";
        case RAB_FLOWGRAPH_ERROR_CAPACITY:
            return "CAPACITY";
        case RAB_FLOWGRAPH_ERROR_MAX:
            break;
    }
    return "UNKNOWN";
}

const char *RabbitizerFlowGraphDiagnosticKind_getName(RabbitizerFlowGraphDiagnosticKind kind) {
    switch (kind) {
        case RAB_FLOWGRAPH_DIAG_MISSING_SLOT:
            return "MISSING_SLOT";
        case RAB_FLOWGRAPH_DIAG_INVALID_INSTRUCTION:
            return "INVALID_INSTRUCTION";
        case RAB_FLOWGRAPH_DIAG_CONTROL_IN_SLOT:
            return "CONTROL_IN_SLOT";
        case RAB_FLOWGRAPH_DIAG_TARGET_MISALIGNED:
            return "TARGET_MISALIGNED";
        case RAB_FLOWGRAPH_DIAG_TARGET_OUT_OF_REGION:
            return "TARGET_OUT_OF_REGION";
        case RAB_FLOWGRAPH_DIAG_UNSUPPORTED_CONTROL:
            return "UNSUPPORTED_CONTROL";
        case RAB_FLOWGRAPH_DIAG_JALR_RS_EQUALS_RD:
            return "JALR_RS_EQUALS_RD";
        case RAB_FLOWGRAPH_DIAG_MAX:
            break;
    }
    return "UNKNOWN";
}

const char *RabbitizerFlowGraphEdgeKind_getName(RabbitizerFlowGraphEdgeKind kind) {
    switch (kind) {
        case RAB_FLOWGRAPH_EDGE_SEQUENTIAL:
            return "SEQUENTIAL";
        case RAB_FLOWGRAPH_EDGE_DELAY_ENTER:
            return "DELAY_ENTER";
        case RAB_FLOWGRAPH_EDGE_SLOT_TAKEN:
            return "SLOT_TAKEN";
        case RAB_FLOWGRAPH_EDGE_SLOT_NOT_TAKEN:
            return "SLOT_NOT_TAKEN";
        case RAB_FLOWGRAPH_EDGE_LIKELY_ANNUL:
            return "LIKELY_ANNUL";
        case RAB_FLOWGRAPH_EDGE_JUMP:
            return "JUMP";
        case RAB_FLOWGRAPH_EDGE_CALL:
            return "CALL";
        case RAB_FLOWGRAPH_EDGE_RETURN:
            return "RETURN";
        case RAB_FLOWGRAPH_EDGE_UNRESOLVED:
            return "UNRESOLVED";
        case RAB_FLOWGRAPH_EDGE_MAX:
            break;
    }
    return "UNKNOWN";
}

const char *RabbitizerFlowGraphNodeKind_getName(RabbitizerFlowGraphNodeKind kind) {
    switch (kind) {
        case RAB_FLOWGRAPH_NODE_KIND_NORMAL:
            return "NORMAL";
        case RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT:
            return "DELAY_SLOT";
        case RAB_FLOWGRAPH_NODE_KIND_MAX:
            break;
    }
    return "UNKNOWN";
}

void RabbitizerFlowGraph_init(RabbitizerFlowGraph *self,
                              RabbitizerFlowGraphNode *nodeBuffer, size_t nodeCapacity,
                              RabbitizerFlowGraphEdge *edgeBuffer, size_t edgeCapacity,
                              RabbitizerFlowGraphDiagnostic *diagnosticBuffer, size_t diagnosticCapacity) {
    self->nodes = nodeBuffer;
    self->nodeCapacity = nodeCapacity;
    self->nodeCount = 0;
    self->edges = edgeBuffer;
    self->edgeCapacity = edgeCapacity;
    self->edgeCount = 0;
    self->diagnostics = diagnosticBuffer;
    self->diagnosticCapacity = diagnosticCapacity;
    self->diagnosticCount = 0;
    self->complete = false;
}

static bool RabbitizerFlowGraph_addressInRegion(const RabbitizerFlowGraphBuilder *builder, uint32_t address) {
    return (uint64_t)address >= builder->startVram && (uint64_t)address < builder->regionEnd;
}

static bool RabbitizerFlowGraph_addressToIndex(const RabbitizerFlowGraphBuilder *builder, uint32_t address,
                                               size_t *dstIndex) {
    if (!RabbitizerFlowGraph_addressInRegion(builder, address) || (address & 3u) != 0) {
        return false;
    }
    *dstIndex = (size_t)((address - builder->startVram) / 4u);
    return true;
}

// Direct J/JAL target. The high nibble comes from the real PC of the delay slot (PC+4), so
// address 0 keeps a zero high nibble instead of the library's no-vram 0x80000000 placeholder.
static uint32_t RabbitizerFlowGraph_jumpTarget(uint32_t branchVram, uint32_t word) {
    return (SHIFTR(word, 0, 26) << 2) | ((branchVram + 4u) & RAB_FG_JUMP_HIGH_MASK);
}

static bool RabbitizerFlowGraph_sameIdentity(const RabbitizerFlowGraphNode *node, uint32_t vram,
                                             RabbitizerFlowGraphNodeKind kind,
                                             const RabbitizerFlowGraphPending *pending) {
    if (node->vram != vram || node->kind != kind) {
        return false;
    }
    if (kind == RAB_FLOWGRAPH_NODE_KIND_NORMAL) {
        return true;
    }
    return node->branchVram == pending->branchVram && node->outcome == pending->outcome &&
           node->pendingTargetVram == pending->targetVram;
}

// Returns the node index, appending a new node when the context is seen for the first time.
static RabbitizerFlowGraphError RabbitizerFlowGraph_getOrCreateNode(RabbitizerFlowGraphBuilder *builder,
                                                                     size_t wordIndex, uint32_t vram,
                                                                     RabbitizerFlowGraphNodeKind kind,
                                                                     const RabbitizerFlowGraphPending *pending,
                                                                     size_t *dstIndex) {
    RabbitizerFlowGraph *graph = builder->graph;
    RabbitizerFlowGraphNode *node;
    RabbitizerInstruction instr;
    size_t i;

    for (i = 0; i < graph->nodeCount; i++) {
        if (RabbitizerFlowGraph_sameIdentity(&graph->nodes[i], vram, kind, pending)) {
            *dstIndex = i;
            return RAB_FLOWGRAPH_ERROR_OK;
        }
    }

    if (graph->nodeCount >= graph->nodeCapacity) {
        return RAB_FLOWGRAPH_ERROR_CAPACITY;
    }

    RabbitizerInstruction_init(&instr, builder->words[wordIndex], vram);
    RabbitizerInstruction_processUniqueId(&instr);

    node = &graph->nodes[graph->nodeCount];
    node->vram = vram;
    node->word = builder->words[wordIndex];
    node->uniqueId = instr.uniqueId;
    node->kind = kind;
    if (kind == RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT) {
        node->branchVram = pending->branchVram;
        node->outcome = pending->outcome;
        node->pendingTargetVram = pending->targetVram;
        node->targetRegister = pending->targetRegister;
        node->isReturn = pending->isReturn;
        node->isCall = pending->isCall;
        node->linkRegister = pending->linkRegister;
        node->returnSiteVram = pending->returnSiteVram;
    } else {
        node->branchVram = RAB_FLOWGRAPH_ADDRESS_NONE;
        node->outcome = RAB_FLOWGRAPH_OUTCOME_NONE;
        node->pendingTargetVram = RAB_FLOWGRAPH_ADDRESS_NONE;
        node->targetRegister = RAB_FLOWGRAPH_REGISTER_NONE;
        node->isReturn = false;
        node->isCall = false;
        node->linkRegister = RAB_FLOWGRAPH_REGISTER_NONE;
        node->returnSiteVram = RAB_FLOWGRAPH_ADDRESS_NONE;
    }

    RabbitizerInstruction_destroy(&instr);

    *dstIndex = graph->nodeCount;
    graph->nodeCount++;
    return RAB_FLOWGRAPH_ERROR_OK;
}

static RabbitizerFlowGraphError RabbitizerFlowGraph_addDiagnostic(RabbitizerFlowGraphBuilder *builder,
                                                                  RabbitizerFlowGraphDiagnosticKind kind,
                                                                  uint32_t branchVram, uint32_t currentVram) {
    RabbitizerFlowGraph *graph = builder->graph;
    size_t i;

    // The same slot is reachable through several branch outcomes; report each problem once.
    for (i = 0; i < graph->diagnosticCount; i++) {
        const RabbitizerFlowGraphDiagnostic *other = &graph->diagnostics[i];
        if (other->kind == kind && other->branchVram == branchVram && other->currentVram == currentVram) {
            return RAB_FLOWGRAPH_ERROR_OK;
        }
    }

    if (graph->diagnosticCount >= graph->diagnosticCapacity) {
        return RAB_FLOWGRAPH_ERROR_CAPACITY;
    }

    graph->diagnostics[graph->diagnosticCount++] = (RabbitizerFlowGraphDiagnostic){
        .kind = kind,
        .branchVram = branchVram,
        .currentVram = currentVram,
    };
    return RAB_FLOWGRAPH_ERROR_OK;
}

static bool RabbitizerFlowGraph_isConditionalBranch(RabbitizerInstrId uniqueId, bool *likely) {
    switch (uniqueId) {
        case RABBITIZER_INSTR_ID_cpu_beq:
        case RABBITIZER_INSTR_ID_cpu_bne:
        case RABBITIZER_INSTR_ID_cpu_blez:
        case RABBITIZER_INSTR_ID_cpu_bgtz:
        case RABBITIZER_INSTR_ID_cpu_bltz:
        case RABBITIZER_INSTR_ID_cpu_bgez:
            *likely = false;
            return true;

        case RABBITIZER_INSTR_ID_cpu_beql:
        case RABBITIZER_INSTR_ID_cpu_bnel:
        case RABBITIZER_INSTR_ID_cpu_blezl:
        case RABBITIZER_INSTR_ID_cpu_bgtzl:
        case RABBITIZER_INSTR_ID_cpu_bltzl:
        case RABBITIZER_INSTR_ID_cpu_bgezl:
            *likely = true;
            return true;

        default:
            return false;
    }
}

static RabbitizerFlowGraphError RabbitizerFlowGraph_ensureNormal(RabbitizerFlowGraphBuilder *builder,
                                                                 uint32_t address) {
    size_t index;
    size_t nodeIndex;

    if (!RabbitizerFlowGraph_addressToIndex(builder, address, &index)) {
        return RAB_FLOWGRAPH_ERROR_OK;
    }
    return RabbitizerFlowGraph_getOrCreateNode(builder, index, address, RAB_FLOWGRAPH_NODE_KIND_NORMAL,
                                               &RAB_FG_NO_PENDING, &nodeIndex);
}

// Fills the delay-slot context selected at the branch instruction. Register values are captured
// here, before the slot runs, so a slot write cannot change the indirect target source.
static void RabbitizerFlowGraph_makeDelayPending(const RabbitizerInstruction *instr, uint32_t branchVram,
                                                 RabbitizerFlowGraphOutcome outcome, uint32_t targetVram,
                                                 RabbitizerFlowGraphPending *pending) {
    pending->branchVram = branchVram;
    pending->outcome = outcome;
    pending->targetVram = targetVram;
    pending->targetRegister = RAB_FLOWGRAPH_REGISTER_NONE;
    pending->isReturn = false;
    pending->isCall = false;
    pending->linkRegister = RAB_FLOWGRAPH_REGISTER_NONE;
    pending->returnSiteVram = RAB_FLOWGRAPH_ADDRESS_NONE;

    switch (instr->uniqueId) {
        case RABBITIZER_INSTR_ID_cpu_jal:
            pending->isCall = true;
            pending->linkRegister = (int8_t)RABBITIZER_REG_GPR_O32_ra;
            pending->returnSiteVram = branchVram + 8u;
            break;

        case RABBITIZER_INSTR_ID_cpu_jalr: {
            uint8_t rd = RAB_INSTR_GET_rd(instr);
            if (rd != 0) {
                // rd == 0 produces no link.
                pending->isCall = true;
                pending->linkRegister = (int8_t)rd;
                pending->returnSiteVram = branchVram + 8u;
            }
            pending->targetRegister = (int8_t)RAB_INSTR_GET_rs(instr);
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_jr:
            pending->targetRegister = (int8_t)RAB_INSTR_GET_rs(instr);
            pending->isReturn =
                RabbitizerRegisterDescriptor_isRa(RabbitizerRegister_getDescriptor_Gpr(RAB_INSTR_GET_rs(instr)));
            break;

        default:
            break;
    }
}

// Validates a direct target, reporting the branch that owns the decision. A recorded diagnostic
// does not stop the other outcomes of the same branch.
static RabbitizerFlowGraphError RabbitizerFlowGraph_checkDirectTarget(RabbitizerFlowGraphBuilder *builder,
                                                                      uint32_t branchVram, uint32_t targetVram) {
    if ((targetVram & 3u) != 0) {
        return RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_TARGET_MISALIGNED, branchVram,
                                                 targetVram);
    }
    if (!RabbitizerFlowGraph_addressInRegion(builder, targetVram)) {
        return RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_TARGET_OUT_OF_REGION, branchVram,
                                                 targetVram);
    }
    return RAB_FLOWGRAPH_ERROR_OK;
}

static RabbitizerFlowGraphError RabbitizerFlowGraph_processNormal(RabbitizerFlowGraphBuilder *builder,
                                                                  RabbitizerFlowGraphNode *node) {
    RabbitizerFlowGraphError error;
    RabbitizerInstruction instr;
    uint32_t pc = node->vram;
    uint32_t slotVram = pc + 4u;
    size_t slotIndex;
    bool likely = false;

    RabbitizerInstruction_init(&instr, node->word, pc);
    RabbitizerInstruction_processUniqueId(&instr);

    if (!RabbitizerInstruction_isValid(&instr)) {
        error = RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_INVALID_INSTRUCTION,
                                                  RAB_FLOWGRAPH_ADDRESS_NONE, pc);
        RabbitizerInstruction_destroy(&instr);
        return error;
    }

    if (RabbitizerFlowGraph_isConditionalBranch(instr.uniqueId, &likely)) {
        uint32_t takenTarget = RabbitizerInstruction_getBranchVramGeneric(&instr);
        RabbitizerFlowGraphPending takenPending;
        RabbitizerFlowGraphPending notTakenPending;

        error = RabbitizerFlowGraph_checkDirectTarget(builder, pc, takenTarget);
        if (error != RAB_FLOWGRAPH_ERROR_OK) {
            RabbitizerInstruction_destroy(&instr);
            return error;
        }

        if (likely) {
            // Annul path: PC+8 is reached directly and the slot is never dispatched on it,
            // so an invalid slot word cannot invalidate this path.
            error = RabbitizerFlowGraph_ensureNormal(builder, pc + 8u);
            if (error != RAB_FLOWGRAPH_ERROR_OK) {
                RabbitizerInstruction_destroy(&instr);
                return error;
            }
        }

        if (!RabbitizerFlowGraph_addressToIndex(builder, slotVram, &slotIndex)) {
            error = RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_MISSING_SLOT, pc, slotVram);
            RabbitizerInstruction_destroy(&instr);
            return error;
        }

        RabbitizerFlowGraph_makeDelayPending(&instr, pc, RAB_FLOWGRAPH_OUTCOME_TAKEN, takenTarget, &takenPending);
        error = RabbitizerFlowGraph_getOrCreateNode(builder, slotIndex, slotVram,
                                                    RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, &takenPending,
                                                    &(size_t){ 0 });
        if (error != RAB_FLOWGRAPH_ERROR_OK) {
            RabbitizerInstruction_destroy(&instr);
            return error;
        }

        if (!likely) {
            // Both outcomes run the slot, but with distinct pending successors.
            RabbitizerFlowGraph_makeDelayPending(&instr, pc, RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN, pc + 8u,
                                                 &notTakenPending);
            error = RabbitizerFlowGraph_getOrCreateNode(builder, slotIndex, slotVram,
                                                        RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, &notTakenPending,
                                                        &(size_t){ 0 });
        }

        RabbitizerInstruction_destroy(&instr);
        return error;
    }

    switch (instr.uniqueId) {
        case RABBITIZER_INSTR_ID_cpu_j:
        case RABBITIZER_INSTR_ID_cpu_jal:
        case RABBITIZER_INSTR_ID_cpu_jr:
        case RABBITIZER_INSTR_ID_cpu_jalr: {
            RabbitizerFlowGraphPending pending;
            uint32_t targetVram = RAB_FLOWGRAPH_ADDRESS_NONE;

            if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jalr &&
                RAB_INSTR_GET_rs(&instr) == RAB_INSTR_GET_rd(&instr)) {
                // Restricted JALR form: diagnose it and stop this dispatch path.
                error = RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_JALR_RS_EQUALS_RD,
                                                          RAB_FLOWGRAPH_ADDRESS_NONE, pc);
                RabbitizerInstruction_destroy(&instr);
                return error;
            }

            if (!RabbitizerFlowGraph_addressToIndex(builder, slotVram, &slotIndex)) {
                error = RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_MISSING_SLOT, pc, slotVram);
                RabbitizerInstruction_destroy(&instr);
                return error;
            }

            if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_j || instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jal) {
                targetVram = RabbitizerFlowGraph_jumpTarget(pc, node->word);
                error = RabbitizerFlowGraph_checkDirectTarget(builder, pc, targetVram);
                if (error != RAB_FLOWGRAPH_ERROR_OK) {
                    RabbitizerInstruction_destroy(&instr);
                    return error;
                }
            }

            RabbitizerFlowGraph_makeDelayPending(&instr, pc, RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL, targetVram,
                                                 &pending);
            error = RabbitizerFlowGraph_getOrCreateNode(builder, slotIndex, slotVram,
                                                        RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, &pending,
                                                        &(size_t){ 0 });
            RabbitizerInstruction_destroy(&instr);
            return error;
        }

        default:
            break;
    }

    if (RabbitizerInstruction_hasDelaySlot(&instr) || instr.uniqueId == RABBITIZER_INSTR_ID_cpu_eret) {
        // Linking REGIMM branches, coprocessor branches, exception returns and any other
        // control transfer the builder does not model.
        error = RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_UNSUPPORTED_CONTROL,
                                                  RAB_FLOWGRAPH_ADDRESS_NONE, pc);
        RabbitizerInstruction_destroy(&instr);
        return error;
    }

    // Ordinary instruction: sequential dispatch.
    error = RabbitizerFlowGraph_ensureNormal(builder, pc + 4u);
    RabbitizerInstruction_destroy(&instr);
    return error;
}

static RabbitizerFlowGraphError RabbitizerFlowGraph_processDelaySlot(RabbitizerFlowGraphBuilder *builder,
                                                                     RabbitizerFlowGraphNode *node) {
    RabbitizerFlowGraphError error;
    RabbitizerInstruction instr;

    RabbitizerInstruction_init(&instr, node->word, node->vram);
    RabbitizerInstruction_processUniqueId(&instr);

    if (!RabbitizerInstruction_isValid(&instr)) {
        error = RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_INVALID_INSTRUCTION,
                                                  node->branchVram, node->vram);
        RabbitizerInstruction_destroy(&instr);
        return error;
    }

    if (RabbitizerInstruction_hasDelaySlot(&instr)) {
        error = RabbitizerFlowGraph_addDiagnostic(builder, RAB_FLOWGRAPH_DIAG_CONTROL_IN_SLOT, node->branchVram,
                                                  node->vram);
        RabbitizerInstruction_destroy(&instr);
        return error;
    }

    RabbitizerInstruction_destroy(&instr);

    // The successor was selected before dispatching the slot; writes performed by the slot
    // (including to the condition or indirect target register) do not reopen the decision.
    // Calls record the target but never follow the callee.
    if (node->outcome == RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL) {
        if (node->isCall) {
            return RAB_FLOWGRAPH_ERROR_OK;
        }
    }
    return RabbitizerFlowGraph_ensureNormal(builder, node->pendingTargetVram);
}

static int RabbitizerFlowGraph_compareNodes(const void *a, const void *b) {
    const RabbitizerFlowGraphNode *left = a;
    const RabbitizerFlowGraphNode *right = b;

    if (left->vram != right->vram) {
        return left->vram < right->vram ? -1 : 1;
    }
    if (left->kind != right->kind) {
        return (int)left->kind - (int)right->kind;
    }
    if (left->branchVram != right->branchVram) {
        return left->branchVram < right->branchVram ? -1 : 1;
    }
    if (left->outcome != right->outcome) {
        return (int)left->outcome - (int)right->outcome;
    }
    if (left->pendingTargetVram != right->pendingTargetVram) {
        return left->pendingTargetVram < right->pendingTargetVram ? -1 : 1;
    }
    return 0;
}

static int RabbitizerFlowGraph_compareEdges(const void *a, const void *b) {
    const RabbitizerFlowGraphEdge *left = a;
    const RabbitizerFlowGraphEdge *right = b;

    if (left->fromNodeIndex != right->fromNodeIndex) {
        return left->fromNodeIndex < right->fromNodeIndex ? -1 : 1;
    }
    if (left->kind != right->kind) {
        return (int)left->kind - (int)right->kind;
    }
    if (left->targetVram != right->targetVram) {
        return left->targetVram < right->targetVram ? -1 : 1;
    }
    if (left->toNodeIndex != right->toNodeIndex) {
        return left->toNodeIndex < right->toNodeIndex ? -1 : 1;
    }
    return 0;
}

static int RabbitizerFlowGraph_compareDiagnostics(const void *a, const void *b) {
    const RabbitizerFlowGraphDiagnostic *left = a;
    const RabbitizerFlowGraphDiagnostic *right = b;

    if (left->kind != right->kind) {
        return (int)left->kind - (int)right->kind;
    }
    if (left->branchVram != right->branchVram) {
        return left->branchVram < right->branchVram ? -1 : 1;
    }
    if (left->currentVram != right->currentVram) {
        return left->currentVram < right->currentVram ? -1 : 1;
    }
    return 0;
}

// Identity lookup over the canonically sorted node array.
static bool RabbitizerFlowGraph_findNode(const RabbitizerFlowGraph *graph, uint32_t vram,
                                         RabbitizerFlowGraphNodeKind kind, uint32_t branchVram,
                                         RabbitizerFlowGraphOutcome outcome, uint32_t pendingTargetVram,
                                         uint32_t *dstIndex) {
    size_t low = 0;
    size_t high = graph->nodeCount;

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const RabbitizerFlowGraphNode *node = &graph->nodes[middle];
        int relation;

        if (node->vram != vram) {
            relation = node->vram < vram ? -1 : 1;
        } else if (node->kind != kind) {
            relation = (int)node->kind - (int)kind;
        } else if (node->branchVram != branchVram) {
            relation = node->branchVram < branchVram ? -1 : 1;
        } else if (node->outcome != outcome) {
            relation = (int)node->outcome - (int)outcome;
        } else if (node->pendingTargetVram != pendingTargetVram) {
            relation = node->pendingTargetVram < pendingTargetVram ? -1 : 1;
        } else {
            relation = 0;
        }

        if (relation == 0) {
            *dstIndex = (uint32_t)middle;
            return true;
        }
        if (relation < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

static RabbitizerFlowGraphError RabbitizerFlowGraph_appendEdge(RabbitizerFlowGraph *graph, uint32_t fromIndex,
                                                               RabbitizerFlowGraphEdgeKind kind,
                                                               uint32_t targetVram, uint32_t toIndex) {
    if (graph->edgeCount >= graph->edgeCapacity) {
        return RAB_FLOWGRAPH_ERROR_CAPACITY;
    }
    graph->edges[graph->edgeCount++] = (RabbitizerFlowGraphEdge){
        .fromNodeIndex = fromIndex,
        .toNodeIndex = toIndex,
        .targetVram = targetVram,
        .kind = kind,
    };
    return RAB_FLOWGRAPH_ERROR_OK;
}

static RabbitizerFlowGraphError RabbitizerFlowGraph_appendEdgeTo(RabbitizerFlowGraph *graph, uint32_t fromIndex,
                                                                 RabbitizerFlowGraphEdgeKind kind, uint32_t vram,
                                                                 RabbitizerFlowGraphNodeKind nodeKind,
                                                                 uint32_t branchVram,
                                                                 RabbitizerFlowGraphOutcome outcome,
                                                                 uint32_t pendingTargetVram) {
    uint32_t toIndex = RAB_FLOWGRAPH_NODE_ID_NONE;
    RabbitizerFlowGraph_findNode(graph, vram, nodeKind, branchVram, outcome, pendingTargetVram, &toIndex);
    return RabbitizerFlowGraph_appendEdge(graph, fromIndex, kind, vram, toIndex);
}

static RabbitizerFlowGraphError RabbitizerFlowGraph_generateEdges(RabbitizerFlowGraph *graph) {
    RabbitizerFlowGraphError error;
    size_t i;

    for (i = 0; i < graph->nodeCount; i++) {
        const RabbitizerFlowGraphNode *node = &graph->nodes[i];
        uint32_t from = (uint32_t)i;

        if (node->kind == RAB_FLOWGRAPH_NODE_KIND_NORMAL) {
            RabbitizerInstruction instr;
            bool likely = false;

            RabbitizerInstruction_init(&instr, node->word, node->vram);
            RabbitizerInstruction_processUniqueId(&instr);

            if (!RabbitizerInstruction_isValid(&instr)) {
                RabbitizerInstruction_destroy(&instr);
                continue;
            }

            if (RabbitizerFlowGraph_isConditionalBranch(instr.uniqueId, &likely)) {
                uint32_t takenTarget = RabbitizerInstruction_getBranchVramGeneric(&instr);

                error = RabbitizerFlowGraph_appendEdgeTo(graph, from, RAB_FLOWGRAPH_EDGE_DELAY_ENTER,
                                                         node->vram + 4u, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT,
                                                         node->vram, RAB_FLOWGRAPH_OUTCOME_TAKEN, takenTarget);
                if (error != RAB_FLOWGRAPH_ERROR_OK) {
                    RabbitizerInstruction_destroy(&instr);
                    return error;
                }

                if (likely) {
                    error = RabbitizerFlowGraph_appendEdgeTo(graph, from, RAB_FLOWGRAPH_EDGE_LIKELY_ANNUL,
                                                             node->vram + 8u, RAB_FLOWGRAPH_NODE_KIND_NORMAL,
                                                             RAB_FLOWGRAPH_ADDRESS_NONE,
                                                             RAB_FLOWGRAPH_OUTCOME_NONE,
                                                             RAB_FLOWGRAPH_ADDRESS_NONE);
                } else {
                    error = RabbitizerFlowGraph_appendEdgeTo(graph, from, RAB_FLOWGRAPH_EDGE_DELAY_ENTER,
                                                             node->vram + 4u, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT,
                                                             node->vram, RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN,
                                                             node->vram + 8u);
                }
                RabbitizerInstruction_destroy(&instr);
                if (error != RAB_FLOWGRAPH_ERROR_OK) {
                    return error;
                }
                continue;
            }

            switch (instr.uniqueId) {
                case RABBITIZER_INSTR_ID_cpu_j:
                case RABBITIZER_INSTR_ID_cpu_jal:
                case RABBITIZER_INSTR_ID_cpu_jr:
                case RABBITIZER_INSTR_ID_cpu_jalr: {
                    uint32_t target = RAB_FLOWGRAPH_ADDRESS_NONE;
                    if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_j ||
                        instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jal) {
                        target = RabbitizerFlowGraph_jumpTarget(node->vram, node->word);
                    }
                    error = RabbitizerFlowGraph_appendEdgeTo(graph, from, RAB_FLOWGRAPH_EDGE_DELAY_ENTER,
                                                             node->vram + 4u, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT,
                                                             node->vram, RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL,
                                                             target);
                    RabbitizerInstruction_destroy(&instr);
                    if (error != RAB_FLOWGRAPH_ERROR_OK) {
                        return error;
                    }
                    continue;
                }

                default:
                    break;
            }

            if (RabbitizerInstruction_hasDelaySlot(&instr) || instr.uniqueId == RABBITIZER_INSTR_ID_cpu_eret) {
                RabbitizerInstruction_destroy(&instr);
                continue;
            }

            error = RabbitizerFlowGraph_appendEdgeTo(graph, from, RAB_FLOWGRAPH_EDGE_SEQUENTIAL, node->vram + 4u,
                                                     RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                                                     RAB_FLOWGRAPH_OUTCOME_NONE, RAB_FLOWGRAPH_ADDRESS_NONE);
            RabbitizerInstruction_destroy(&instr);
            if (error != RAB_FLOWGRAPH_ERROR_OK) {
                return error;
            }
            continue;
        }

        // Delay-slot node: its outgoing edge is the successor selected by the owning branch.
        // An invalid slot word or a control transfer inside the slot stops the affected path,
        // so the pending successor is deliberately not emitted.
        {
            RabbitizerInstruction slotInstr;
            RabbitizerInstruction_init(&slotInstr, node->word, node->vram);
            RabbitizerInstruction_processUniqueId(&slotInstr);
            if (!RabbitizerInstruction_isValid(&slotInstr) ||
                RabbitizerInstruction_hasDelaySlot(&slotInstr)) {
                RabbitizerInstruction_destroy(&slotInstr);
                continue;
            }
            RabbitizerInstruction_destroy(&slotInstr);
        }

        switch (node->outcome) {
            case RAB_FLOWGRAPH_OUTCOME_TAKEN:
                error = RabbitizerFlowGraph_appendEdgeTo(
                    graph, from, node->isCall ? RAB_FLOWGRAPH_EDGE_CALL : RAB_FLOWGRAPH_EDGE_SLOT_TAKEN,
                    node->pendingTargetVram, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                    RAB_FLOWGRAPH_OUTCOME_NONE, RAB_FLOWGRAPH_ADDRESS_NONE);
                if (error != RAB_FLOWGRAPH_ERROR_OK) {
                    return error;
                }
                break;

            case RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN:
                error = RabbitizerFlowGraph_appendEdgeTo(graph, from, RAB_FLOWGRAPH_EDGE_SLOT_NOT_TAKEN,
                                                         node->branchVram + 8u, RAB_FLOWGRAPH_NODE_KIND_NORMAL,
                                                         RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE,
                                                         RAB_FLOWGRAPH_ADDRESS_NONE);
                if (error != RAB_FLOWGRAPH_ERROR_OK) {
                    return error;
                }
                break;

            case RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL: {
                RabbitizerFlowGraphEdgeKind kind;
                if (node->isCall) {
                    kind = RAB_FLOWGRAPH_EDGE_CALL;
                } else if (node->isReturn) {
                    kind = RAB_FLOWGRAPH_EDGE_RETURN;
                } else if (node->pendingTargetVram == RAB_FLOWGRAPH_ADDRESS_NONE) {
                    kind = RAB_FLOWGRAPH_EDGE_UNRESOLVED;
                } else {
                    kind = RAB_FLOWGRAPH_EDGE_JUMP;
                }
                error = RabbitizerFlowGraph_appendEdgeTo(graph, from, kind, node->pendingTargetVram,
                                                         RAB_FLOWGRAPH_NODE_KIND_NORMAL,
                                                         RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE,
                                                         RAB_FLOWGRAPH_ADDRESS_NONE);
                if (error != RAB_FLOWGRAPH_ERROR_OK) {
                    return error;
                }
                break;
            }

            case RAB_FLOWGRAPH_OUTCOME_NONE:
            case RAB_FLOWGRAPH_OUTCOME_MAX:
                break;
        }
    }

    qsort(graph->edges, graph->edgeCount, sizeof(*graph->edges), RabbitizerFlowGraph_compareEdges);
    return RAB_FLOWGRAPH_ERROR_OK;
}

RabbitizerFlowGraphError RabbitizerFlowGraph_build(RabbitizerFlowGraph *self,
                                                   const RabbitizerFlowGraphBuildOptions *options) {
    RabbitizerFlowGraphError error = RAB_FLOWGRAPH_ERROR_OK;
    RabbitizerFlowGraphBuilder builder;
    RabbitizerConfig_PseudoInstr savedPseudos;
    size_t i;

    if (self == NULL || options == NULL) {
        return RAB_FLOWGRAPH_ERROR_NULL_ARG;
    }
    if ((options->words == NULL && options->wordCount != 0) ||
        (options->entryVrams == NULL && options->entryCount != 0)) {
        return RAB_FLOWGRAPH_ERROR_NULL_ARG;
    }

    self->nodeCount = 0;
    self->edgeCount = 0;
    self->diagnosticCount = 0;
    self->complete = false;

    if ((uint64_t)options->startVram + (uint64_t)options->wordCount * 4u > RAB_FG_ADDRESS_SPACE_SIZE) {
        return RAB_FLOWGRAPH_ERROR_BAD_REGION;
    }

    builder.graph = self;
    builder.words = options->words;
    builder.wordCount = options->wordCount;
    builder.startVram = options->startVram;
    builder.regionEnd = (uint64_t)options->startVram + (uint64_t)options->wordCount * 4u;

    for (i = 0; i < options->entryCount; i++) {
        size_t index;
        if (!RabbitizerFlowGraph_addressToIndex(&builder, options->entryVrams[i], &index)) {
            return RAB_FLOWGRAPH_ERROR_BAD_ENTRY;
        }
    }

    // The graph structure must be independent of the pseudo-instruction configuration.
    savedPseudos = RabbitizerConfig_Cfg.pseudos;
    RabbitizerConfig_Cfg.pseudos.enablePseudos = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBeqz = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBnez = false;
    RabbitizerConfig_Cfg.pseudos.pseudoB = false;
    RabbitizerConfig_Cfg.pseudos.pseudoBal = false;

    // Seed entries. Duplicates and entry ordering collapse into identical node identities.
    for (i = 0; i < options->entryCount; i++) {
        size_t index = (size_t)((options->entryVrams[i] - options->startVram) / 4u);
        size_t nodeIndex;
        error = RabbitizerFlowGraph_getOrCreateNode(&builder, index, options->entryVrams[i],
                                                    RAB_FLOWGRAPH_NODE_KIND_NORMAL, &RAB_FG_NO_PENDING,
                                                    &nodeIndex);
        if (error != RAB_FLOWGRAPH_ERROR_OK) {
            goto cleanup;
        }
    }

    // The node array is also the work queue: newly discovered contexts get appended and the
    // same loop picks them up, so loops terminate once every finite context has been seen.
    for (i = 0; i < self->nodeCount; i++) {
        if (self->nodes[i].kind == RAB_FLOWGRAPH_NODE_KIND_NORMAL) {
            error = RabbitizerFlowGraph_processNormal(&builder, &self->nodes[i]);
        } else {
            error = RabbitizerFlowGraph_processDelaySlot(&builder, &self->nodes[i]);
        }
        if (error != RAB_FLOWGRAPH_ERROR_OK) {
            goto cleanup;
        }
    }

    qsort(self->nodes, self->nodeCount, sizeof(*self->nodes), RabbitizerFlowGraph_compareNodes);

    error = RabbitizerFlowGraph_generateEdges(self);
    if (error != RAB_FLOWGRAPH_ERROR_OK) {
        goto cleanup;
    }

    qsort(self->diagnostics, self->diagnosticCount, sizeof(*self->diagnostics),
          RabbitizerFlowGraph_compareDiagnostics);

    self->complete = true;
    error = RAB_FLOWGRAPH_ERROR_OK;

cleanup:
    RabbitizerConfig_Cfg.pseudos = savedPseudos;
    return error;
}
