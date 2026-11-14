/* SPDX-FileCopyrightText: © 2024 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "analysis/RabbitizerPicCallAnalysis.h"

#include <stdlib.h>
#include <string.h>

#include "instructions/RabbitizerInstruction.h"
#include "instructions/RabbitizerRegister.h"

#define PIC_REG_ZERO 0
#define PIC_REG_A0 4
#define PIC_REG_T9 25
#define PIC_REG_GP 28
#define PIC_REG_RA 31
#define PIC_REGISTER_COUNT 32
#define PIC_ARG_COUNT 4
#define PIC_DEFAULT_MAX_STATES 64

typedef struct PicState {
    RabbitizerPicValue regs[PIC_REGISTER_COUNT];
    bool saturated;
} PicState;

typedef struct PicNodeStates {
    PicState *items;
    size_t count;
    size_t capacity;
    bool saturated; // an explicit saturated unknown state is held in this list
} PicNodeStates;

typedef struct PicAnalyzer {
    const RabbitizerPicGraph *graph;
    const RabbitizerPicRelocation *relocations;
    size_t relocationCount;
    const RabbitizerPicGotSlot *gotSlots;
    size_t gotSlotCount;
    size_t maxStateCount;

    RabbitizerInstruction *decoded; // one decoded instruction per node
    bool *isSlotNode;               // nodes used as a call delay slot

    // Relocation lookup, grouped by node index through an indirection array.
    size_t *relocFirst;
    size_t *relocCount;
    size_t *relocOrder;

    PicNodeStates *nodeStates;
    size_t peakStateCount;
    bool saturatedAnywhere;

    RabbitizerPicCallRecord *records;
    size_t recordCapacity;
    size_t recordCount;
    bool recordOverflow;
} PicAnalyzer;

static RabbitizerPicValue makeUnknown(uint32_t reasons, int originNodeIndex) {
    RabbitizerPicValue value;

    (void)originNodeIndex;
    memset(&value, 0, sizeof(value));
    value.kind = RABBITIZER_PIC_VALUE_UNKNOWN;
    value.symbolIndex = RABBITIZER_PIC_NO_SYMBOL;
    // An unknown value has no production position; a fixed origin is also what
    // keeps loop bodies converging to a fixpoint.
    value.originNodeIndex = -1;
    value.reasons = reasons;
    return value;
}

static RabbitizerPicValue makeConst(uint32_t constValue, int originNodeIndex) {
    RabbitizerPicValue value;

    memset(&value, 0, sizeof(value));
    value.kind = RABBITIZER_PIC_VALUE_CONST;
    value.symbolIndex = RABBITIZER_PIC_NO_SYMBOL;
    value.constValue = constValue;
    value.originNodeIndex = originNodeIndex;
    return value;
}

bool RabbitizerPicSymbolId_equals(const RabbitizerPicSymbolId *a, const RabbitizerPicSymbolId *b) {
    return a->objectId == b->objectId && a->symbolIndex == b->symbolIndex;
}

bool RabbitizerPicValue_isUnknown(const RabbitizerPicValue *value) {
    return value->kind == RABBITIZER_PIC_VALUE_UNKNOWN;
}

bool RabbitizerPicValue_equals(const RabbitizerPicValue *a, const RabbitizerPicValue *b) {
    return a->kind == b->kind && a->objectId == b->objectId && a->symbolIndex == b->symbolIndex &&
           a->addend == b->addend && a->constValue == b->constValue && a->gpBias == b->gpBias &&
           a->originNodeIndex == b->originNodeIndex && a->reasons == b->reasons;
}

static int compareValues(const RabbitizerPicValue *a, const RabbitizerPicValue *b) {
    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    if (a->objectId != b->objectId) {
        return a->objectId < b->objectId ? -1 : 1;
    }
    if (a->symbolIndex != b->symbolIndex) {
        return a->symbolIndex < b->symbolIndex ? -1 : 1;
    }
    if (a->addend != b->addend) {
        return a->addend < b->addend ? -1 : 1;
    }
    if (a->constValue != b->constValue) {
        return a->constValue < b->constValue ? -1 : 1;
    }
    if (a->gpBias != b->gpBias) {
        return a->gpBias < b->gpBias ? -1 : 1;
    }
    if (a->originNodeIndex != b->originNodeIndex) {
        return a->originNodeIndex < b->originNodeIndex ? -1 : 1;
    }
    if (a->reasons != b->reasons) {
        return a->reasons < b->reasons ? -1 : 1;
    }
    return 0;
}

static bool picState_equals(const PicState *a, const PicState *b) {
    size_t i;

    if (a->saturated != b->saturated) {
        return false;
    }
    for (i = 0; i < PIC_REGISTER_COUNT; i++) {
        if (!RabbitizerPicValue_equals(&a->regs[i], &b->regs[i])) {
            return false;
        }
    }
    return true;
}

static const RabbitizerPicRelocation *findAnyRelocation(const PicAnalyzer *analyzer, size_t nodeIndex) {
    size_t first = analyzer->relocFirst[nodeIndex];
    size_t count = analyzer->relocCount[nodeIndex];

    if (count == 0) {
        return NULL;
    }
    return &analyzer->relocations[analyzer->relocOrder[first]];
}

static const RabbitizerPicGotSlot *findGotSlot(const PicAnalyzer *analyzer, uint32_t objectId, int32_t gpOffset) {
    size_t i;

    for (i = 0; i < analyzer->gotSlotCount; i++) {
        const RabbitizerPicGotSlot *slot = &analyzer->gotSlots[i];

        if (slot->objectId == objectId && slot->gpOffset == gpOffset) {
            return slot;
        }
    }
    return NULL;
}

static RabbitizerPicValue addConstToValue(const RabbitizerPicValue *value, int32_t amount, int originNodeIndex) {
    RabbitizerPicValue result = *value;
    uint32_t wrap = (uint32_t)amount;

    result.originNodeIndex = originNodeIndex;
    switch (result.kind) {
        case RABBITIZER_PIC_VALUE_CONST:
            // All 32-bit arithmetic is defined to wrap modulo 2^32.
            result.constValue = result.constValue + wrap;
            break;

        case RABBITIZER_PIC_VALUE_GP:
            result.gpBias = (int32_t)((uint32_t)result.gpBias + wrap);
            break;

        case RABBITIZER_PIC_VALUE_GOT:
        case RABBITIZER_PIC_VALUE_SYMBOL:
        case RABBITIZER_PIC_VALUE_GP_PARTIAL:
        case RABBITIZER_PIC_VALUE_ENTRY:
            result.addend = (int32_t)((uint32_t)result.addend + wrap);
            break;

        case RABBITIZER_PIC_VALUE_UNKNOWN:
            // An unknown has no production position, so propagation leaves it
            // untouched; this is what lets loop bodies reach a fixpoint.
            return *value;
    }
    return result;
}

// Merge two values for an addu. Zero-register copies are handled by the caller.
static RabbitizerPicValue addValues(const RabbitizerPicValue *left, const RabbitizerPicValue *right,
                                    int originNodeIndex) {
    const RabbitizerPicValue *partial = NULL;
    RabbitizerPicValue result;

    if (left->kind == RABBITIZER_PIC_VALUE_UNKNOWN) {
        return *left;
    }
    if (right->kind == RABBITIZER_PIC_VALUE_UNKNOWN) {
        return *right;
    }

    if (left->kind == RABBITIZER_PIC_VALUE_CONST) {
        return addConstToValue(right, (int32_t)left->constValue, originNodeIndex);
    }
    if (right->kind == RABBITIZER_PIC_VALUE_CONST) {
        return addConstToValue(left, (int32_t)right->constValue, originNodeIndex);
    }

    // cpload completion: a hi/lo _gp_disp partial joined with the entry t9 value.
    if (left->kind == RABBITIZER_PIC_VALUE_GP_PARTIAL && right->kind == RABBITIZER_PIC_VALUE_ENTRY) {
        partial = left;
    } else if (right->kind == RABBITIZER_PIC_VALUE_GP_PARTIAL && left->kind == RABBITIZER_PIC_VALUE_ENTRY) {
        partial = right;
    }
    if (partial != NULL) {
        memset(&result, 0, sizeof(result));
        result.kind = RABBITIZER_PIC_VALUE_GP;
        result.objectId = partial->objectId;
        result.symbolIndex = RABBITIZER_PIC_NO_SYMBOL;
        result.originNodeIndex = originNodeIndex;
        return result;
    }

    return makeUnknown(left->reasons | right->reasons, originNodeIndex);
}

static void writeRegister(PicState *state, uint8_t reg, RabbitizerPicValue value) {
    if (reg == PIC_REG_ZERO) {
        // $zero is hardwired to zero, writes are discarded.
        state->regs[PIC_REG_ZERO] = makeConst(0, value.originNodeIndex);
        return;
    }
    state->regs[reg] = value;
}

static void killRegister(PicState *state, uint8_t reg, uint32_t reasons, int originNodeIndex) {
    if (reg == PIC_REG_ZERO) {
        return;
    }
    state->regs[reg] = makeUnknown(reasons, originNodeIndex);
}

static bool symbolIdMatchesValue(const RabbitizerPicValue *value, const RabbitizerPicSymbolId *symbol) {
    RabbitizerPicSymbolId valueSymbol = { value->objectId, value->symbolIndex };

    return RabbitizerPicSymbolId_equals(&valueSymbol, symbol);
}

static RabbitizerPicValue handleLui(const PicAnalyzer *analyzer, const RabbitizerInstruction *instr,
                                    size_t nodeIndex) {
    const RabbitizerPicRelocation *reloc = findAnyRelocation(analyzer, nodeIndex);
    uint32_t upper = (uint32_t)(uint16_t)RAB_INSTR_GET_immediate(instr) << 16;

    if (reloc != NULL && (reloc->kind == RABBITIZER_PIC_RELOC_GP_DISP_HI16 ||
                          reloc->kind == RABBITIZER_PIC_RELOC_HI16)) {
        RabbitizerPicValue value;

        memset(&value, 0, sizeof(value));
        value.objectId = reloc->symbol.objectId;
        value.symbolIndex = reloc->symbol.symbolIndex;
        value.originNodeIndex = (int)nodeIndex;
        value.addend = reloc->addend;
        value.constValue = upper;
        if (reloc->kind == RABBITIZER_PIC_RELOC_GP_DISP_HI16) {
            value.kind = RABBITIZER_PIC_VALUE_GP_PARTIAL;
        } else {
            value.kind = RABBITIZER_PIC_VALUE_SYMBOL;
            value.addend += (int32_t)upper;
        }
        return value;
    }

    return makeConst(upper, (int)nodeIndex);
}

static RabbitizerPicValue handleAddiu(const PicAnalyzer *analyzer, const RabbitizerInstruction *instr,
                                      size_t nodeIndex, const RabbitizerPicValue *source) {
    const RabbitizerPicRelocation *reloc = findAnyRelocation(analyzer, nodeIndex);
    int32_t imm = RabbitizerInstruction_getProcessedImmediate(instr);

    if (reloc == NULL) {
        return addConstToValue(source, imm, (int)nodeIndex);
    }

    switch (reloc->kind) {
        case RABBITIZER_PIC_RELOC_GP_DISP_LO16:
            // The pairing source must be the caller-identified HI16 node.
            if (source->kind == RABBITIZER_PIC_VALUE_GP_PARTIAL &&
                source->objectId == reloc->symbol.objectId &&
                (reloc->pairNodeIndex == RABBITIZER_PIC_NO_NODE ||
                 source->originNodeIndex == (int)reloc->pairNodeIndex)) {
                RabbitizerPicValue result = *source;

                result.constValue = result.constValue + (uint32_t)imm + (uint32_t)reloc->addend;
                result.originNodeIndex = (int)nodeIndex;
                return result;
            }
            return makeUnknown(RABBITIZER_PIC_REASON_BAD_RELOC, (int)nodeIndex);

        case RABBITIZER_PIC_RELOC_LO16:
            if (source->kind == RABBITIZER_PIC_VALUE_SYMBOL && symbolIdMatchesValue(source, &reloc->symbol) &&
                (reloc->pairNodeIndex == RABBITIZER_PIC_NO_NODE ||
                 source->originNodeIndex == (int)reloc->pairNodeIndex)) {
                RabbitizerPicValue result = *source;

                result.addend = (int32_t)((uint32_t)result.addend + (uint32_t)imm +
                                          (uint32_t)reloc->addend);
                result.originNodeIndex = (int)nodeIndex;
                return result;
            }
            return makeUnknown(RABBITIZER_PIC_REASON_BAD_RELOC, (int)nodeIndex);

        case RABBITIZER_PIC_RELOC_LOCAL_LO16:
            // A local %lo adjustment only combines with a GOT source of the
            // same symbol identity in the same object.
            if (source->kind == RABBITIZER_PIC_VALUE_GOT && symbolIdMatchesValue(source, &reloc->symbol)) {
                RabbitizerPicValue result = *source;

                result.kind = RABBITIZER_PIC_VALUE_SYMBOL;
                result.addend = (int32_t)((uint32_t)result.addend + (uint32_t)imm +
                                          (uint32_t)reloc->addend);
                result.originNodeIndex = (int)nodeIndex;
                return result;
            }
            return makeUnknown(RABBITIZER_PIC_REASON_BAD_RELOC, (int)nodeIndex);

        default:
            return makeUnknown(RABBITIZER_PIC_REASON_BAD_RELOC, (int)nodeIndex);
    }
}

static RabbitizerPicValue handleLw(const PicAnalyzer *analyzer, const RabbitizerInstruction *instr,
                                   size_t nodeIndex, const RabbitizerPicValue *base) {
    const RabbitizerPicRelocation *reloc = findAnyRelocation(analyzer, nodeIndex);
    int32_t imm = RabbitizerInstruction_getProcessedImmediate(instr);

    if (reloc == NULL ||
        (reloc->kind != RABBITIZER_PIC_RELOC_CALL16 && reloc->kind != RABBITIZER_PIC_RELOC_GOT16)) {
        // A plain lw must never resolve into a GOT symbol just because an offset matches.
        return makeUnknown(0, (int)nodeIndex);
    }

    if (base->kind != RABBITIZER_PIC_VALUE_GP) {
        return makeUnknown(RABBITIZER_PIC_REASON_BAD_GOT, (int)nodeIndex);
    }

    {
        int32_t effectiveOffset = (int32_t)((uint32_t)imm + (uint32_t)base->gpBias);
        const RabbitizerPicGotSlot *slot = findGotSlot(analyzer, base->objectId, effectiveOffset);
        RabbitizerPicValue value;

        if (slot == NULL) {
            return makeUnknown(RABBITIZER_PIC_REASON_BAD_GOT, (int)nodeIndex);
        }

        memset(&value, 0, sizeof(value));
        value.kind = RABBITIZER_PIC_VALUE_GOT;
        value.objectId = slot->symbol.objectId;
        value.symbolIndex = slot->symbol.symbolIndex;
        value.originNodeIndex = (int)nodeIndex;
        return value;
    }
}

// Apply one ordinary instruction (never a call node or its delay slot) to a state.
static void transferOrdinary(PicAnalyzer *analyzer, size_t nodeIndex, PicState *state) {
    const RabbitizerInstruction *instr = &analyzer->decoded[nodeIndex];
    uint8_t rs = RAB_INSTR_GET_rs(instr);
    uint8_t rt = RAB_INSTR_GET_rt(instr);
    uint8_t rd = RAB_INSTR_GET_rd(instr);
    const RabbitizerPicValue *rsValue = &state->regs[rs];
    const RabbitizerPicValue *rtValue = &state->regs[rt];

    switch (instr->uniqueId) {
        case RABBITIZER_INSTR_ID_cpu_lui:
            writeRegister(state, rt, handleLui(analyzer, instr, nodeIndex));
            return;

        case RABBITIZER_INSTR_ID_cpu_addiu:
            writeRegister(state, rt, handleAddiu(analyzer, instr, nodeIndex, rsValue));
            return;

        case RABBITIZER_INSTR_ID_cpu_addu:
            if (rs == PIC_REG_ZERO) {
                writeRegister(state, rd, *rtValue);
            } else if (rt == PIC_REG_ZERO) {
                writeRegister(state, rd, *rsValue);
            } else {
                writeRegister(state, rd, addValues(rsValue, rtValue, (int)nodeIndex));
            }
            return;

        case RABBITIZER_INSTR_ID_cpu_or:
            // Only copies through the real zero register are recognized.
            if (rs == PIC_REG_ZERO) {
                writeRegister(state, rd, *rtValue);
            } else if (rt == PIC_REG_ZERO) {
                writeRegister(state, rd, *rsValue);
            } else {
                killRegister(state, rd, rsValue->reasons | rtValue->reasons, (int)nodeIndex);
            }
            return;

        case RABBITIZER_INSTR_ID_cpu_move:
            // `move rd, rs` is the disassembler's pseudo form of an or/addu
            // copy through the real zero register.
            writeRegister(state, rd, *rsValue);
            return;

        case RABBITIZER_INSTR_ID_cpu_lw:
            writeRegister(state, rt, handleLw(analyzer, instr, nodeIndex, rsValue));
            return;

        default:
            break;
    }

    // Unmodeled write: kill every produced register according to its descriptor.
    if (RabbitizerInstrDescriptor_modifiesRs(instr->descriptor)) {
        killRegister(state, rs, 0, (int)nodeIndex);
    }
    if (RabbitizerInstrDescriptor_modifiesRt(instr->descriptor)) {
        killRegister(state, rt, 0, (int)nodeIndex);
    }
    if (RabbitizerInstrDescriptor_modifiesRd(instr->descriptor)) {
        killRegister(state, rd, 0, (int)nodeIndex);
    }
}

static bool callRecord_equals(const RabbitizerPicCallRecord *a, const RabbitizerPicCallRecord *b) {
    size_t i;

    if (a->nodeIndex != b->nodeIndex || a->isDirectCall != b->isDirectCall ||
        a->staticTargetVram != b->staticTargetVram || a->saturated != b->saturated ||
        a->unknownReasons != b->unknownReasons ||
        !RabbitizerPicValue_equals(&a->callee, &b->callee)) {
        return false;
    }
    for (i = 0; i < PIC_ARG_COUNT; i++) {
        if (!RabbitizerPicValue_equals(&a->args[i], &b->args[i])) {
            return false;
        }
    }
    return true;
}

static void appendCallRecord(PicAnalyzer *analyzer, const RabbitizerPicCallRecord *record) {
    size_t i;

    for (i = 0; i < analyzer->recordCount; i++) {
        if (callRecord_equals(&analyzer->records[i], record)) {
            return;
        }
    }
    if (analyzer->recordCount >= analyzer->recordCapacity) {
        analyzer->recordOverflow = true;
        return;
    }
    analyzer->records[analyzer->recordCount++] = *record;
}

// Build the call observation for one incoming full state of a call node.
static void recordCall(PicAnalyzer *analyzer, size_t nodeIndex, const PicState *incoming) {
    const RabbitizerPicGraphNode *node = &analyzer->graph->nodes[nodeIndex];
    const RabbitizerInstruction *callInstr = &analyzer->decoded[nodeIndex];
    RabbitizerPicCallRecord record;
    PicState afterSlot;
    size_t i;
    uint32_t reasons = 0;

    memset(&record, 0, sizeof(record));
    record.nodeIndex = nodeIndex;
    record.callVram = node->vram;
    record.linkVram = node->vram + 8;
    record.saturated = incoming->saturated;

    if (callInstr->uniqueId == RABBITIZER_INSTR_ID_cpu_jal) {
        record.isDirectCall = true;
        record.staticTargetVram = RabbitizerInstruction_getInstrIndexAsVram(callInstr);
        record.callee = makeConst(record.staticTargetVram, (int)nodeIndex);
    } else {
        // Indirect call: the callee is the source value *before* the delay slot.
        record.isDirectCall = false;
        record.callee = incoming->regs[RAB_INSTR_GET_rs(callInstr)];
        if (RabbitizerPicValue_isUnknown(&record.callee)) {
            reasons |= RABBITIZER_PIC_REASON_CALLEE_UNKNOWN;
            reasons |= record.callee.reasons;
        }
    }

    // The link register receives PC+8, then the single delay slot instruction runs.
    afterSlot = *incoming;
    {
        uint8_t linkReg = callInstr->uniqueId == RABBITIZER_INSTR_ID_cpu_jal
                              ? PIC_REG_RA
                              : RAB_INSTR_GET_rd(callInstr);

        if (linkReg != PIC_REG_ZERO) {
            afterSlot.regs[linkReg] = makeConst(record.linkVram, (int)nodeIndex);
        }
    }
    if (node->delaySlotNode != RABBITIZER_PIC_NO_NODE) {
        transferOrdinary(analyzer, node->delaySlotNode, &afterSlot);
    }

    // Arguments are read after the delay slot has executed.
    for (i = 0; i < PIC_ARG_COUNT; i++) {
        record.args[i] = afterSlot.regs[PIC_REG_A0 + i];
        if (RabbitizerPicValue_isUnknown(&record.args[i])) {
            reasons |= (RABBITIZER_PIC_REASON_ARG0_UNKNOWN << i);
            reasons |= record.args[i].reasons;
        }
    }
    record.unknownReasons = reasons;
    appendCallRecord(analyzer, &record);
}

// O32 post-call clobber applied on the return continuation state.
static void applyCallClobber(PicState *state, int originNodeIndex) {
    size_t reg;

    for (reg = 0; reg < PIC_REGISTER_COUNT; reg++) {
        const RabbitizerRegisterDescriptor *descriptor = RabbitizerRegister_getDescriptor_Gpr((uint8_t)reg);

        if (RabbitizerRegisterDescriptor_isClobberedByFuncCall(descriptor)) {
            killRegister(state, (uint8_t)reg, 0, originNodeIndex);
        }
    }
    // GP is considered destroyed across the call; callee-saved registers survive.
    killRegister(state, PIC_REG_GP, 0, originNodeIndex);
}

static PicState makeSaturatedState(int originNodeIndex) {
    PicState state;
    size_t i;

    for (i = 0; i < PIC_REGISTER_COUNT; i++) {
        state.regs[i] = makeUnknown(RABBITIZER_PIC_REASON_STATE_LIMIT, originNodeIndex);
    }
    state.regs[PIC_REG_ZERO] = makeConst(0, originNodeIndex);
    state.saturated = true;
    return state;
}

static bool ensureHolderCapacity(PicNodeStates *holder) {
    size_t newCapacity;
    PicState *grown;

    if (holder->count < holder->capacity) {
        return true;
    }
    newCapacity = holder->capacity == 0 ? 4 : holder->capacity * 2;
    grown = realloc(holder->items, newCapacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    holder->items = grown;
    holder->capacity = newCapacity;
    return true;
}

// Merge a full state into a node's state set, honoring the state cap.
static void mergeState(PicAnalyzer *analyzer, size_t nodeIndex, const PicState *incoming,
                       size_t *worklist, size_t *worklistCount, size_t worklistCapacity, bool *inQueue) {
    PicNodeStates *holder = &analyzer->nodeStates[nodeIndex];
    PicState saturated;
    size_t i;

    // A holder that itself overflowed now holds one wildcard covering every
    // incoming path, so nothing else can change it.
    if (holder->saturated) {
        return;
    }

    for (i = 0; i < holder->count; i++) {
        if (picState_equals(&holder->items[i], incoming)) {
            return;
        }
    }

    if (holder->count >= analyzer->maxStateCount) {
        // Explicit saturation: the whole state set collapses into one unknown
        // wildcard. The wildcard still reaches every successor, and definite
        // writes on later nodes reclaim facts, so no path is dropped because of
        // traversal order.
        analyzer->saturatedAnywhere = true;
        if (!ensureHolderCapacity(holder)) {
            return;
        }
        saturated = makeSaturatedState((int)nodeIndex);
        holder->items[0] = saturated;
        holder->count = 1;
        holder->saturated = true;
    } else {
        if (!ensureHolderCapacity(holder)) {
            return;
        }
        // A propagated wildcard is kept as one item alongside concrete states;
        // only a local overflow marks the holder itself as saturated.
        holder->items[holder->count++] = *incoming;
    }

    if (holder->count > analyzer->peakStateCount) {
        analyzer->peakStateCount = holder->count;
    }

    if (!inQueue[nodeIndex] && *worklistCount < worklistCapacity) {
        inQueue[nodeIndex] = true;
        worklist[(*worklistCount)++] = nodeIndex;
    }
}

static void processNode(PicAnalyzer *analyzer, size_t nodeIndex, size_t *worklist, size_t *worklistCount,
                        size_t worklistCapacity, bool *inQueue) {
    const RabbitizerPicGraphNode *node = &analyzer->graph->nodes[nodeIndex];
    PicNodeStates *holder = &analyzer->nodeStates[nodeIndex];
    size_t stateIndex;
    size_t edgeIndex;

    if (analyzer->isSlotNode[nodeIndex]) {
        // A delay slot node is executed by its owning call node and is not a
        // standalone CFG node, so it neither transfers state nor fans out edges.
        return;
    }

    for (stateIndex = 0; stateIndex < holder->count; stateIndex++) {
        PicState current = holder->items[stateIndex];

        if (node->kind == RABBITIZER_PIC_NODE_CALL) {
            recordCall(analyzer, nodeIndex, &current);
            applyCallClobber(&current, (int)nodeIndex);
        } else {
            transferOrdinary(analyzer, nodeIndex, &current);
        }

        for (edgeIndex = 0; edgeIndex < node->edgeCount; edgeIndex++) {
            const RabbitizerPicGraphEdge *edge = &analyzer->graph->edges[node->firstEdge + edgeIndex];
            PicState successor = current;

            mergeState(analyzer, edge->toNodeIndex, &successor, worklist, worklistCount, worklistCapacity,
                       inQueue);
        }
    }
}

static RabbitizerPicAnalysisStatus validateInputs(const RabbitizerPicGraph *graph,
                                                  const RabbitizerPicRelocation *relocations,
                                                  size_t relocationCount) {
    size_t i;

    if (graph->nodes == NULL || graph->entryNodeIndex >= graph->nodeCount) {
        return RABBITIZER_PIC_ANALYSIS_ERROR_ENTRY;
    }
    if (graph->edgeCount > 0 && graph->edges == NULL) {
        return RABBITIZER_PIC_ANALYSIS_ERROR_EDGE;
    }
    for (i = 0; i < graph->nodeCount; i++) {
        const RabbitizerPicGraphNode *node = &graph->nodes[i];

        if (node->firstEdge > graph->edgeCount || node->firstEdge + node->edgeCount > graph->edgeCount) {
            return RABBITIZER_PIC_ANALYSIS_ERROR_EDGE;
        }
        if (node->kind == RABBITIZER_PIC_NODE_CALL && node->delaySlotNode >= graph->nodeCount) {
            return RABBITIZER_PIC_ANALYSIS_ERROR_NODE;
        }
    }
    for (i = 0; i < graph->edgeCount; i++) {
        if (graph->edges[i].fromNodeIndex >= graph->nodeCount ||
            graph->edges[i].toNodeIndex >= graph->nodeCount) {
            return RABBITIZER_PIC_ANALYSIS_ERROR_EDGE;
        }
    }
    for (i = 0; i < relocationCount; i++) {
        if (relocations[i].nodeIndex >= graph->nodeCount ||
            (relocations[i].pairNodeIndex != RABBITIZER_PIC_NO_NODE &&
             relocations[i].pairNodeIndex >= graph->nodeCount)) {
            return RABBITIZER_PIC_ANALYSIS_ERROR_RELOC;
        }
    }
    return RABBITIZER_PIC_ANALYSIS_OK;
}

static int compareCallRecords(const void *a, const void *b) {
    const RabbitizerPicCallRecord *left = a;
    const RabbitizerPicCallRecord *right = b;
    int cmp;
    size_t i;

    if (left->nodeIndex != right->nodeIndex) {
        return left->nodeIndex < right->nodeIndex ? -1 : 1;
    }
    if (left->isDirectCall != right->isDirectCall) {
        return left->isDirectCall ? -1 : 1;
    }
    if (left->staticTargetVram != right->staticTargetVram) {
        return left->staticTargetVram < right->staticTargetVram ? -1 : 1;
    }
    cmp = compareValues(&left->callee, &right->callee);
    if (cmp != 0) {
        return cmp;
    }
    for (i = 0; i < PIC_ARG_COUNT; i++) {
        cmp = compareValues(&left->args[i], &right->args[i]);
        if (cmp != 0) {
            return cmp;
        }
    }
    if (left->saturated != right->saturated) {
        return left->saturated ? -1 : 1;
    }
    if (left->unknownReasons != right->unknownReasons) {
        return left->unknownReasons < right->unknownReasons ? -1 : 1;
    }
    return 0;
}

RabbitizerPicAnalysisStatus RabbitizerPicAnalysis_analyze(const RabbitizerPicGraph *graph,
                                                          const RabbitizerPicRelocation *relocations,
                                                          size_t relocationCount,
                                                          const RabbitizerPicGotSlot *gotSlots,
                                                          size_t gotSlotCount,
                                                          const RabbitizerPicAnalysisConfig *config,
                                                          RabbitizerPicCallResult *result) {
    PicAnalyzer analyzer;
    RabbitizerPicAnalysisStatus status;
    PicState entryState;
    size_t *worklist = NULL;
    bool *inQueue = NULL;
    size_t worklistCount = 0;
    size_t i;

    result->recordCount = 0;
    result->peakStateCount = 0;
    result->saturated = false;
    if (result->records == NULL || result->recordCapacity == 0) {
        return RABBITIZER_PIC_ANALYSIS_ERROR_NULL;
    }
    if ((relocationCount > 0 && relocations == NULL) || (gotSlotCount > 0 && gotSlots == NULL)) {
        return RABBITIZER_PIC_ANALYSIS_ERROR_NULL;
    }

    status = validateInputs(graph, relocations, relocationCount);
    if (status != RABBITIZER_PIC_ANALYSIS_OK) {
        return status;
    }

    memset(&analyzer, 0, sizeof(analyzer));
    analyzer.graph = graph;
    analyzer.relocations = relocations;
    analyzer.relocationCount = relocationCount;
    analyzer.gotSlots = gotSlots;
    analyzer.gotSlotCount = gotSlotCount;
    analyzer.maxStateCount = (config != NULL && config->maxStateCount > 0) ? config->maxStateCount
                                                                            : PIC_DEFAULT_MAX_STATES;
    analyzer.records = result->records;
    analyzer.recordCapacity = result->recordCapacity;

    analyzer.decoded = calloc(graph->nodeCount, sizeof(*analyzer.decoded));
    analyzer.isSlotNode = calloc(graph->nodeCount, sizeof(*analyzer.isSlotNode));
    analyzer.relocFirst = calloc(graph->nodeCount, sizeof(*analyzer.relocFirst));
    analyzer.relocCount = calloc(graph->nodeCount, sizeof(*analyzer.relocCount));
    analyzer.relocOrder = calloc(relocationCount, sizeof(*analyzer.relocOrder));
    analyzer.nodeStates = calloc(graph->nodeCount, sizeof(*analyzer.nodeStates));
    worklist = malloc(graph->nodeCount * sizeof(*worklist));
    inQueue = calloc(graph->nodeCount, sizeof(*inQueue));
    if (analyzer.decoded == NULL || analyzer.isSlotNode == NULL || analyzer.relocFirst == NULL ||
        analyzer.relocCount == NULL || analyzer.nodeStates == NULL || worklist == NULL || inQueue == NULL ||
        (relocationCount > 0 && analyzer.relocOrder == NULL)) {
        status = RABBITIZER_PIC_ANALYSIS_ERROR_NULL;
        goto cleanup;
    }

    for (i = 0; i < graph->nodeCount; i++) {
        RabbitizerInstruction_init(&analyzer.decoded[i], graph->nodes[i].word, graph->nodes[i].vram);
        RabbitizerInstruction_processUniqueId(&analyzer.decoded[i]);
    }
    for (i = 0; i < graph->nodeCount; i++) {
        const RabbitizerPicGraphNode *node = &graph->nodes[i];

        if (node->kind == RABBITIZER_PIC_NODE_CALL &&
            node->delaySlotNode != RABBITIZER_PIC_NO_NODE) {
            analyzer.isSlotNode[node->delaySlotNode] = true;
        }
    }

    // Group relocation records by node index without assuming contiguity.
    for (i = 0; i < relocationCount; i++) {
        analyzer.relocCount[relocations[i].nodeIndex]++;
    }
    {
        size_t running = 0;

        for (i = 0; i < graph->nodeCount; i++) {
            analyzer.relocFirst[i] = running;
            running += analyzer.relocCount[i];
            analyzer.relocCount[i] = 0;
        }
    }
    for (i = 0; i < relocationCount; i++) {
        size_t nodeIndex = relocations[i].nodeIndex;
        size_t slot = analyzer.relocFirst[nodeIndex] + analyzer.relocCount[nodeIndex];

        analyzer.relocOrder[slot] = i;
        analyzer.relocCount[nodeIndex]++;
    }

    // Entry state: t9 is the function entry, zero is zero, everything else is unknown.
    memset(&entryState, 0, sizeof(entryState));
    for (i = 0; i < PIC_REGISTER_COUNT; i++) {
        entryState.regs[i] = makeUnknown(0, -1);
    }
    entryState.regs[PIC_REG_ZERO] = makeConst(0, -1);
    {
        RabbitizerPicValue entryValue;

        memset(&entryValue, 0, sizeof(entryValue));
        entryValue.kind = RABBITIZER_PIC_VALUE_ENTRY;
        entryValue.symbolIndex = RABBITIZER_PIC_NO_SYMBOL;
        entryValue.constValue = graph->entryVram;
        entryValue.originNodeIndex = -1;
        entryState.regs[PIC_REG_T9] = entryValue;
    }

    mergeState(&analyzer, graph->entryNodeIndex, &entryState, worklist, &worklistCount, graph->nodeCount,
               inQueue);
    inQueue[graph->entryNodeIndex] = false;

    // Worklist fixpoint over the full-state sets.
    while (worklistCount > 0) {
        size_t nodeIndex = worklist[0];

        memmove(&worklist[0], &worklist[1], (worklistCount - 1) * sizeof(*worklist));
        worklistCount--;
        inQueue[nodeIndex] = false;
        processNode(&analyzer, nodeIndex, worklist, &worklistCount, graph->nodeCount, inQueue);
    }

    qsort(analyzer.records, analyzer.recordCount, sizeof(*analyzer.records), compareCallRecords);

    result->recordCount = analyzer.recordCount;
    result->peakStateCount = analyzer.peakStateCount;
    result->saturated = analyzer.saturatedAnywhere;
    status = analyzer.recordOverflow ? RABBITIZER_PIC_ANALYSIS_ERROR_CAPACITY : RABBITIZER_PIC_ANALYSIS_OK;

cleanup:
    if (analyzer.nodeStates != NULL) {
        for (i = 0; i < graph->nodeCount; i++) {
            free(analyzer.nodeStates[i].items);
        }
        free(analyzer.nodeStates);
    }
    if (analyzer.decoded != NULL) {
        for (i = 0; i < graph->nodeCount; i++) {
            RabbitizerInstruction_destroy(&analyzer.decoded[i]);
        }
        free(analyzer.decoded);
    }
    free(analyzer.isSlotNode);
    free(analyzer.relocFirst);
    free(analyzer.relocCount);
    free(analyzer.relocOrder);
    free(worklist);
    free(inQueue);
    return status;
}
