/* SPDX-FileCopyrightText: © 2024 Decompollaborate */
/* SPDX-License-Identifier: MIT */

/*
 * Task tests for the read-only MIPS32 O32 PIC call-source recovery.
 *
 * All instruction words are real MIPS encodings produced by the local
 * helpers below; no word is ever executed.
 */

#include "rabbitizer.h"
#include "task_support/check.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GB_MAX_NODES 80
#define GB_MAX_EDGES 160
#define TEST_RECORD_CAP 64
#define TEST_BASE_VRAM 0x00401000u
#define TEST_OBJECT_A 7u
#define TEST_OBJECT_B 8u

/* ---- raw encoders ---- */

#define ENC_I(op, rs, rt, imm) \
    (((uint32_t)(op) << 26) | ((uint32_t)(rs) << 21) | ((uint32_t)(rt) << 16) | ((uint32_t)(imm) & 0xFFFFu))
#define ENC_R(rs, rt, rd, sa, fun) \
    (((uint32_t)(rs) << 21) | ((uint32_t)(rt) << 16) | ((uint32_t)(rd) << 11) | ((uint32_t)(sa) << 6) | (fun))
#define ENC_J(op, target) ((uint32_t)(op) << 26 | ((target) >> 2))

#define REG_ZERO 0
#define REG_A0 4
#define REG_T0 8
#define REG_S0 16
#define REG_T9 25
#define REG_GP 28
#define REG_RA 31

#define OP_ADDIU 9
#define OP_LUI 15
#define OP_LW 35
#define OP_JAL 3
#define FUN_ADDU 33
#define FUN_OR 37
#define FUN_SLL 0
#define FUN_JALR 9

static uint32_t enc_lui(uint8_t rt, uint16_t imm) {
    return ENC_I(OP_LUI, 0, rt, imm);
}
static uint32_t enc_addiu(uint8_t rt, uint8_t rs, int16_t imm) {
    return ENC_I(OP_ADDIU, rs, rt, imm);
}
static uint32_t enc_addu(uint8_t rd, uint8_t rs, uint8_t rt) {
    return ENC_R(rs, rt, rd, 0, FUN_ADDU);
}
static uint32_t enc_or(uint8_t rd, uint8_t rs, uint8_t rt) {
    return ENC_R(rs, rt, rd, 0, FUN_OR);
}
static uint32_t enc_lw(uint8_t rt, int16_t off, uint8_t rs) {
    return ENC_I(OP_LW, rs, rt, off);
}
static uint32_t enc_sll(uint8_t rd, uint8_t rt, uint8_t sa) {
    return ENC_R(0, rt, rd, sa, FUN_SLL);
}
static uint32_t enc_nop(void) {
    return enc_sll(REG_ZERO, REG_ZERO, 0);
}
static uint32_t enc_jalr(uint8_t link, uint8_t target) {
    return ENC_R(target, 0, link, 0, FUN_JALR);
}
static uint32_t enc_jal(uint32_t targetVram) {
    return ENC_J(OP_JAL, targetVram);
}

/* ---- graph builder ---- */

typedef struct GraphBuilder {
    RabbitizerPicGraphNode nodes[GB_MAX_NODES];
    RabbitizerPicGraphEdge edges[GB_MAX_EDGES];
    size_t rawEdgeFrom[GB_MAX_EDGES];
    size_t rawEdgeTo[GB_MAX_EDGES];
    size_t nodeCount;
    size_t edgeCount;
} GraphBuilder;

static void gb_init(GraphBuilder *gb) {
    memset(gb, 0, sizeof(*gb));
}

static size_t gb_node(GraphBuilder *gb, uint32_t word) {
    size_t index = gb->nodeCount;

    gb->nodes[index].word = word;
    gb->nodes[index].vram = TEST_BASE_VRAM + (uint32_t)index * 4u;
    gb->nodes[index].kind = RABBITIZER_PIC_NODE_NORMAL;
    gb->nodes[index].delaySlotNode = RABBITIZER_PIC_NO_NODE;
    gb->nodes[index].firstEdge = 0;
    gb->nodes[index].edgeCount = 0;
    gb->nodeCount++;
    return index;
}

// Adds the call node and its single delay slot node; returns the call node index.
static size_t gb_call(GraphBuilder *gb, uint32_t callWord, uint32_t slotWord) {
    size_t callIndex = gb_node(gb, callWord);
    size_t slotIndex = gb_node(gb, slotWord);

    gb->nodes[callIndex].kind = RABBITIZER_PIC_NODE_CALL;
    gb->nodes[callIndex].delaySlotNode = slotIndex;
    return callIndex;
}

static void gb_edge(GraphBuilder *gb, size_t from, size_t to) {
    size_t index = gb->edgeCount;

    gb->rawEdgeFrom[index] = from;
    gb->rawEdgeTo[index] = to;
    gb->edgeCount++;
}

static bool gb_is_slot(const GraphBuilder *gb, size_t index) {
    size_t i;

    for (i = 0; i < gb->nodeCount; i++) {
        if (gb->nodes[i].kind == RABBITIZER_PIC_NODE_CALL && gb->nodes[i].delaySlotNode == index) {
            return true;
        }
    }
    return false;
}

// Chains consecutive nodes. A call's edge jumps over its delay slot node to the
// post-return continuation; delay slot nodes themselves have no CFG edges.
static void gb_chain(GraphBuilder *gb, size_t from, size_t to) {
    size_t i;

    for (i = from; i < to; i++) {
        size_t next;

        if (gb_is_slot(gb, i)) {
            continue;
        }
        next = i + 1;
        if (gb->nodes[i].kind == RABBITIZER_PIC_NODE_CALL && gb->nodes[i].delaySlotNode == i + 1) {
            next = i + 2;
        }
        gb_edge(gb, i, next);
    }
}

static RabbitizerPicGraph gb_finalize(GraphBuilder *gb, size_t entryNodeIndex) {
    RabbitizerPicGraph graph;
    RabbitizerPicGraphEdge grouped[GB_MAX_EDGES];
    size_t perNodeCount[GB_MAX_NODES];
    size_t perNodeCursor[GB_MAX_NODES];
    size_t i;
    size_t running = 0;

    memset(perNodeCount, 0, sizeof(perNodeCount));
    memset(perNodeCursor, 0, sizeof(perNodeCursor));
    for (i = 0; i < gb->edgeCount; i++) {
        perNodeCount[gb->rawEdgeFrom[i]]++;
    }
    for (i = 0; i < gb->nodeCount; i++) {
        gb->nodes[i].firstEdge = running;
        gb->nodes[i].edgeCount = perNodeCount[i];
        perNodeCursor[i] = running;
        running += perNodeCount[i];
    }
    // Group edges by source node so each node's [firstEdge, firstEdge+edgeCount) range is valid.
    for (i = 0; i < gb->edgeCount; i++) {
        size_t from = gb->rawEdgeFrom[i];
        size_t slot = perNodeCursor[from]++;

        grouped[slot].fromNodeIndex = from;
        grouped[slot].toNodeIndex = gb->rawEdgeTo[i];
    }
    memcpy(gb->edges, grouped, gb->edgeCount * sizeof(gb->edges[0]));

    graph.nodes = gb->nodes;
    graph.nodeCount = gb->nodeCount;
    graph.edges = gb->edges;
    graph.edgeCount = gb->edgeCount;
    graph.entryNodeIndex = entryNodeIndex;
    graph.entryVram = gb->nodes[entryNodeIndex].vram;
    return graph;
}

/* ---- relocation / GOT helpers ---- */

static RabbitizerPicRelocation make_reloc(size_t node, RabbitizerPicRelocKind kind, uint32_t objectId,
                                          uint32_t symbolIndex, int32_t addend, size_t pairNode) {
    RabbitizerPicRelocation reloc;

    reloc.nodeIndex = node;
    reloc.kind = kind;
    reloc.symbol.objectId = objectId;
    reloc.symbol.symbolIndex = symbolIndex;
    reloc.addend = addend;
    reloc.pairNodeIndex = pairNode;
    return reloc;
}

static RabbitizerPicGotSlot make_got(uint32_t objectId, int32_t gpOffset, uint32_t symbolIndex,
                                     RabbitizerPicGotSource source) {
    RabbitizerPicGotSlot slot;

    slot.objectId = objectId;
    slot.gpOffset = gpOffset;
    slot.symbol.objectId = objectId;
    slot.symbol.symbolIndex = symbolIndex;
    slot.source = source;
    return slot;
}

static RabbitizerPicAnalysisStatus run_analysis(const RabbitizerPicGraph *graph,
                                                const RabbitizerPicRelocation *relocs, size_t relocCount,
                                                const RabbitizerPicGotSlot *slots, size_t slotCount,
                                                size_t maxStates, RabbitizerPicCallRecord *records,
                                                RabbitizerPicCallResult *result) {
    RabbitizerPicAnalysisConfig config;

    config.maxStateCount = maxStates;
    result->records = records;
    result->recordCapacity = TEST_RECORD_CAP;
    result->recordCount = 0;
    result->peakStateCount = 0;
    result->saturated = false;
    return RabbitizerPicAnalysis_analyze(graph, relocs, relocCount, slots, slotCount, &config, result);
}

static const RabbitizerPicCallRecord *find_record(const RabbitizerPicCallResult *result, size_t nodeIndex) {
    size_t i;

    for (i = 0; i < result->recordCount; i++) {
        if (result->records[i].nodeIndex == nodeIndex) {
            return &result->records[i];
        }
    }
    return NULL;
}

static size_t count_records_at(const RabbitizerPicCallResult *result, size_t nodeIndex) {
    size_t i;
    size_t count = 0;

    for (i = 0; i < result->recordCount; i++) {
        if (result->records[i].nodeIndex == nodeIndex) {
            count++;
        }
    }
    return count;
}

/* ================= PicGp ================= */

// Full cpload chain, CALL16/GOT16 loads, gp saved to s0 and restored across the call.
static int test_pic_gp_full_chain(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[8];
    RabbitizerPicGotSlot slots[2];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t n_lui, n_lo, n_lw1, n_call1, n_lw2, n_call2, n_end;
    const RabbitizerPicCallRecord *first;
    const RabbitizerPicCallRecord *second;
    size_t r = 0;

    gb_init(&gb);
    n_lui = gb_node(&gb, enc_lui(REG_GP, 0x1234));
    n_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, -16));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    n_lw1 = gb_node(&gb, enc_lw(REG_T9, -32752, REG_GP));
    gb_node(&gb, enc_addu(REG_S0, REG_GP, REG_ZERO));
    n_call1 = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_addiu(REG_A0, REG_ZERO, 7));
    gb_node(&gb, enc_addu(REG_GP, REG_S0, REG_ZERO));
    n_lw2 = gb_node(&gb, enc_lw(REG_T9, 16, REG_GP));
    n_call2 = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_end = gb_node(&gb, enc_nop());
    gb_chain(&gb, n_lui, n_end);

    relocs[r++] = make_reloc(n_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_A, 90, 0, n_lui);
    relocs[r++] = make_reloc(n_lw1, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_A, 5, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lw2, RABBITIZER_PIC_RELOC_GOT16, TEST_OBJECT_A, 6, 0,
                             RABBITIZER_PIC_NO_NODE);
    slots[0] = make_got(TEST_OBJECT_A, -32752, 5, RABBITIZER_PIC_GOT_SOURCE_CALL16);
    slots[1] = make_got(TEST_OBJECT_A, 16, 6, RABBITIZER_PIC_GOT_SOURCE_GOT16);

    graph = gb_finalize(&gb, n_lui);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 2, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(result.recordCount == 2);
    TASK_CHECK(!result.saturated);
    TASK_CHECK(result.peakStateCount == 1);

    first = find_record(&result, n_call1);
    TASK_CHECK(first != NULL);
    TASK_CHECK(!first->isDirectCall);
    TASK_CHECK(first->callee.kind == RABBITIZER_PIC_VALUE_GOT);
    TASK_CHECK(first->callee.objectId == TEST_OBJECT_A);
    TASK_CHECK(first->callee.symbolIndex == 5);
    TASK_CHECK(first->linkVram == gb.nodes[n_call1].vram + 8);
    // The delay slot's a0 write is visible in the arguments.
    TASK_CHECK(first->args[0].kind == RABBITIZER_PIC_VALUE_CONST);
    TASK_CHECK(first->args[0].constValue == 7);
    TASK_CHECK(first->args[1].kind == RABBITIZER_PIC_VALUE_UNKNOWN);
    TASK_CHECK((first->unknownReasons & RABBITIZER_PIC_REASON_CALLEE_UNKNOWN) == 0);
    TASK_CHECK((first->unknownReasons & RABBITIZER_PIC_REASON_ARG0_UNKNOWN) == 0);
    TASK_CHECK((first->unknownReasons & RABBITIZER_PIC_REASON_ARG1_UNKNOWN) != 0);
    TASK_CHECK((first->unknownReasons & RABBITIZER_PIC_REASON_ARG2_UNKNOWN) != 0);
    TASK_CHECK((first->unknownReasons & RABBITIZER_PIC_REASON_ARG3_UNKNOWN) != 0);

    // gp was clobbered by the call but restored from callee-saved s0, so the
    // second GOT load resolves again.
    second = find_record(&result, n_call2);
    TASK_CHECK(second != NULL);
    TASK_CHECK(second->callee.kind == RABBITIZER_PIC_VALUE_GOT);
    TASK_CHECK(second->callee.objectId == TEST_OBJECT_A);
    TASK_CHECK(second->callee.symbolIndex == 6);
    TASK_CHECK((second->unknownReasons & RABBITIZER_PIC_REASON_CALLEE_UNKNOWN) == 0);
    return 0;
}

// A plain lw with no relocation must never resolve into a GOT symbol.
static int test_pic_gp_plain_lw_no_impersonation(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[3];
    RabbitizerPicGotSlot slots[1];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t n_lui, n_lo, n_call, n_end;
    const RabbitizerPicCallRecord *record;
    size_t r = 0;

    gb_init(&gb);
    n_lui = gb_node(&gb, enc_lui(REG_GP, 0x1234));
    n_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, -16));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    // Same offset as the mapped GOT slot, but no CALL16/GOT16 relocation on this node.
    gb_node(&gb, enc_lw(REG_T9, -32752, REG_GP));
    n_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_end = gb_node(&gb, enc_nop());
    gb_chain(&gb, n_lui, n_end);

    relocs[r++] = make_reloc(n_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_A, 90, 0, n_lui);
    slots[0] = make_got(TEST_OBJECT_A, -32752, 5, RABBITIZER_PIC_GOT_SOURCE_CALL16);

    graph = gb_finalize(&gb, n_lui);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 1, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    record = find_record(&result, n_call);
    TASK_CHECK(record != NULL);
    TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_UNKNOWN);
    TASK_CHECK((record->unknownReasons & RABBITIZER_PIC_REASON_CALLEE_UNKNOWN) != 0);
    TASK_CHECK((record->unknownReasons & RABBITIZER_PIC_REASON_BAD_GOT) == 0);
    return 0;
}

// A _gp_disp lo16 referencing a different object than the hi16 cannot complete GP.
static int test_pic_gp_inconsistent_object(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[4];
    RabbitizerPicGotSlot slots[1];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t n_lui, n_lo, n_lw, n_call, n_end;
    const RabbitizerPicCallRecord *record;
    size_t r = 0;

    gb_init(&gb);
    n_lui = gb_node(&gb, enc_lui(REG_GP, 0x1234));
    n_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, -16));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    n_lw = gb_node(&gb, enc_lw(REG_T9, -32752, REG_GP));
    n_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_end = gb_node(&gb, enc_nop());
    gb_chain(&gb, n_lui, n_end);

    relocs[r++] = make_reloc(n_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    // Wrong object identity on the paired lo16.
    relocs[r++] = make_reloc(n_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_B, 90, 0, n_lui);
    relocs[r++] = make_reloc(n_lw, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_A, 5, 0,
                             RABBITIZER_PIC_NO_NODE);
    slots[0] = make_got(TEST_OBJECT_A, -32752, 5, RABBITIZER_PIC_GOT_SOURCE_CALL16);

    graph = gb_finalize(&gb, n_lui);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 1, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    record = find_record(&result, n_call);
    TASK_CHECK(record != NULL);
    TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_UNKNOWN);
    TASK_CHECK((record->unknownReasons & RABBITIZER_PIC_REASON_BAD_GOT) != 0);
    TASK_CHECK((record->unknownReasons & RABBITIZER_PIC_REASON_CALLEE_UNKNOWN) != 0);
    return 0;
}

// A constant addiu offset on top of GP shifts the effective GOT slot offset.
static int test_pic_gp_addiu_bias(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[4];
    RabbitizerPicGotSlot slots[1];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t n_lui, n_lo, n_bias, n_lw, n_call, n_end;
    const RabbitizerPicCallRecord *record;
    size_t r = 0;

    gb_init(&gb);
    n_lui = gb_node(&gb, enc_lui(REG_GP, 0x1234));
    n_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, -16));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    n_bias = gb_node(&gb, enc_addiu(REG_GP, REG_GP, 8)); // gpBias = +8
    n_lw = gb_node(&gb, enc_lw(REG_T9, -108, REG_GP));   // effective slot offset = -100
    n_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_end = gb_node(&gb, enc_nop());
    gb_chain(&gb, n_lui, n_end);

    relocs[r++] = make_reloc(n_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_A, 90, 0, n_lui);
    (void)n_bias;
    relocs[r++] = make_reloc(n_lw, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_A, 7, 0,
                             RABBITIZER_PIC_NO_NODE);
    slots[0] = make_got(TEST_OBJECT_A, -100, 7, RABBITIZER_PIC_GOT_SOURCE_CALL16);

    graph = gb_finalize(&gb, n_lui);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 1, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    record = find_record(&result, n_call);
    TASK_CHECK(record != NULL);
    TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_GOT);
    TASK_CHECK(record->callee.symbolIndex == 7);
    return 0;
}

/* ================= PicCorrelation ================= */

// Same symbol index on two different object identities reach a shared call;
// callee/argument correlations stay paired and the unknown path is kept.
static int test_correlation_distinct_identities_paired(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[8];
    RabbitizerPicGotSlot slots[2];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry;
    size_t a_lui, a_lo, a_lw, a_arg;
    size_t b_lui, b_lo, b_lw, b_arg;
    size_t c_kill;
    size_t merge_call, merge_end;
    const RabbitizerPicCallRecord *record;
    size_t i;
    int sawA = 0, sawB = 0, sawUnknown = 0;
    size_t r = 0;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    a_lui = gb_node(&gb, enc_lui(REG_GP, 0x1000));
    a_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, 0));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    a_lw = gb_node(&gb, enc_lw(REG_T9, -100, REG_GP));
    a_arg = gb_node(&gb, enc_addiu(REG_A0, REG_ZERO, 11));
    b_lui = gb_node(&gb, enc_lui(REG_GP, 0x2000));
    b_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, 0));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    b_lw = gb_node(&gb, enc_lw(REG_T9, -100, REG_GP));
    b_arg = gb_node(&gb, enc_addiu(REG_A0, REG_ZERO, 22));
    c_kill = gb_node(&gb, enc_sll(REG_T9, REG_T9, 1)); // t9 becomes unknown
    merge_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    merge_end = gb_node(&gb, enc_nop());

    gb_edge(&gb, entry, a_lui);
    gb_edge(&gb, entry, b_lui);
    gb_edge(&gb, entry, c_kill);
    gb_chain(&gb, a_lui, a_arg);
    gb_edge(&gb, a_arg, merge_call);
    gb_chain(&gb, b_lui, b_arg);
    gb_edge(&gb, b_arg, merge_call);
    gb_edge(&gb, c_kill, merge_call);
    gb_edge(&gb, merge_call, merge_end);

    relocs[r++] = make_reloc(a_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(a_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_A, 90, 0, a_lui);
    relocs[r++] = make_reloc(a_lw, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_A, 11, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(b_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_B, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(b_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_B, 90, 0, b_lui);
    relocs[r++] = make_reloc(b_lw, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_B, 11, 0,
                             RABBITIZER_PIC_NO_NODE);
    // Same symbol index 11 in both objects: identical display name, different identity.
    slots[0] = make_got(TEST_OBJECT_A, -100, 11, RABBITIZER_PIC_GOT_SOURCE_CALL16);
    slots[1] = make_got(TEST_OBJECT_B, -100, 11, RABBITIZER_PIC_GOT_SOURCE_CALL16);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 2, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(count_records_at(&result, merge_call) == 3); // no Cartesian product, unknown path kept

    for (i = 0; i < result.recordCount; i++) {
        record = &result.records[i];
        if (record->nodeIndex != merge_call) {
            continue;
        }
        if (record->callee.kind == RABBITIZER_PIC_VALUE_GOT && record->callee.objectId == TEST_OBJECT_A) {
            sawA = 1;
            TASK_CHECK(record->callee.symbolIndex == 11);
            TASK_CHECK(record->args[0].kind == RABBITIZER_PIC_VALUE_CONST);
            TASK_CHECK(record->args[0].constValue == 11);
        } else if (record->callee.kind == RABBITIZER_PIC_VALUE_GOT &&
                   record->callee.objectId == TEST_OBJECT_B) {
            sawB = 1;
            TASK_CHECK(record->callee.symbolIndex == 11);
            TASK_CHECK(record->args[0].kind == RABBITIZER_PIC_VALUE_CONST);
            TASK_CHECK(record->args[0].constValue == 22);
        } else {
            sawUnknown = 1;
            TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_UNKNOWN);
            TASK_CHECK(record->args[0].kind == RABBITIZER_PIC_VALUE_UNKNOWN);
            TASK_CHECK((record->unknownReasons & RABBITIZER_PIC_REASON_CALLEE_UNKNOWN) != 0);
        }
    }
    TASK_CHECK(sawA && sawB && sawUnknown);
    return 0;
}

// Local %lo adjustment only combines with a GOT source of the same symbol identity.
static int test_correlation_local_lo16_identity(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[6];
    RabbitizerPicGotSlot slots[2];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t n_lui, n_lo, n_addu, n_lw, n_local_ok, n_call_ok;
    size_t n_bad_lw, n_local_bad, n_call_bad, n_end;
    const RabbitizerPicCallRecord *okRecord;
    const RabbitizerPicCallRecord *badRecord;
    size_t r = 0;

    gb_init(&gb);
    n_lui = gb_node(&gb, enc_lui(REG_GP, 0x1234));
    n_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, -16));
    n_addu = gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    // Two branches share the same GP; no call sits between them, so gp stays live.
    n_lw = gb_node(&gb, enc_lw(REG_T9, -200, REG_GP));
    n_local_ok = gb_node(&gb, enc_addiu(REG_T9, REG_T9, 4));
    n_call_ok = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_bad_lw = gb_node(&gb, enc_lw(REG_T9, -204, REG_GP));
    n_local_bad = gb_node(&gb, enc_addiu(REG_T9, REG_T9, 8));
    n_call_bad = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_end = gb_node(&gb, enc_nop());
    gb_chain(&gb, n_lui, n_addu);
    gb_edge(&gb, n_addu, n_lw);
    gb_chain(&gb, n_lw, n_call_ok);
    gb_edge(&gb, n_call_ok, n_end);
    gb_edge(&gb, n_addu, n_bad_lw);
    gb_chain(&gb, n_bad_lw, n_call_bad);
    gb_edge(&gb, n_call_bad, n_end);

    relocs[r++] = make_reloc(n_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_A, 90, 0, n_lui);
    relocs[r++] = make_reloc(n_lw, RABBITIZER_PIC_RELOC_GOT16, TEST_OBJECT_A, 30, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_local_ok, RABBITIZER_PIC_RELOC_LOCAL_LO16, TEST_OBJECT_A, 30, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_bad_lw, RABBITIZER_PIC_RELOC_GOT16, TEST_OBJECT_A, 31, 0,
                             RABBITIZER_PIC_NO_NODE);
    // The GOT value held here is symbol 31, but the adjustment names symbol 30.
    relocs[r++] = make_reloc(n_local_bad, RABBITIZER_PIC_RELOC_LOCAL_LO16, TEST_OBJECT_A, 30, 0,
                             RABBITIZER_PIC_NO_NODE);
    slots[0] = make_got(TEST_OBJECT_A, -200, 30, RABBITIZER_PIC_GOT_SOURCE_GOT16);
    slots[1] = make_got(TEST_OBJECT_A, -204, 31, RABBITIZER_PIC_GOT_SOURCE_GOT16);

    graph = gb_finalize(&gb, n_lui);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 2, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);

    okRecord = find_record(&result, n_call_ok);
    TASK_CHECK(okRecord != NULL);
    TASK_CHECK(okRecord->callee.kind == RABBITIZER_PIC_VALUE_SYMBOL);
    TASK_CHECK(okRecord->callee.objectId == TEST_OBJECT_A);
    TASK_CHECK(okRecord->callee.symbolIndex == 30);
    TASK_CHECK(okRecord->callee.addend == 4);

    badRecord = find_record(&result, n_call_bad);
    TASK_CHECK(badRecord != NULL);
    TASK_CHECK(badRecord->callee.kind == RABBITIZER_PIC_VALUE_UNKNOWN);
    TASK_CHECK((badRecord->unknownReasons & RABBITIZER_PIC_REASON_BAD_RELOC) != 0);
    return 0;
}

// Identical incoming states merge into one de-duplicated, stable record.
static int test_correlation_dedup(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[3];
    RabbitizerPicGotSlot slots[1];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry, branch1, branch2, merge_call, end;
    size_t r = 0;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    branch1 = gb_node(&gb, enc_nop());
    branch2 = gb_node(&gb, enc_nop());
    merge_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_addiu(REG_A0, REG_ZERO, 9));
    end = gb_node(&gb, enc_nop());
    gb_edge(&gb, entry, branch1);
    gb_edge(&gb, entry, branch2);
    gb_edge(&gb, branch1, merge_call);
    gb_edge(&gb, branch2, merge_call);
    gb_edge(&gb, merge_call, end);

    // t9 is unknown on both paths, so the merged state set has a single state.
    relocs[r++] = make_reloc(entry, RABBITIZER_PIC_RELOC_NONE, 0, RABBITIZER_PIC_NO_SYMBOL, 0,
                             RABBITIZER_PIC_NO_NODE);
    slots[0] = make_got(TEST_OBJECT_A, 0, 1, RABBITIZER_PIC_GOT_SOURCE_CALL16);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 1, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(result.recordCount == 1);
    TASK_CHECK(count_records_at(&result, merge_call) == 1);
    TASK_CHECK(result.records[0].args[0].constValue == 9);
    TASK_CHECK(result.peakStateCount == 1);
    return 0;
}

/* ================= PicCallOrder ================= */

// A delay slot overwrite of t9 cannot change the already-selected callee.
static int test_call_order_delay_slot_t9(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[4];
    RabbitizerPicGotSlot slots[1];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t n_lui, n_lo, n_lw, n_call, n_end;
    const RabbitizerPicCallRecord *record;
    size_t r = 0;

    gb_init(&gb);
    n_lui = gb_node(&gb, enc_lui(REG_GP, 0x1234));
    n_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, -16));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    n_lw = gb_node(&gb, enc_lw(REG_T9, -32752, REG_GP));
    // The slot rewrites t9; the callee was selected from the pre-slot value.
    n_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_addiu(REG_T9, REG_T9, 1));
    n_end = gb_node(&gb, enc_nop());
    gb_chain(&gb, n_lui, n_end);

    relocs[r++] = make_reloc(n_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_A, 90, 0, n_lui);
    relocs[r++] = make_reloc(n_lw, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_A, 5, 0,
                             RABBITIZER_PIC_NO_NODE);
    slots[0] = make_got(TEST_OBJECT_A, -32752, 5, RABBITIZER_PIC_GOT_SOURCE_CALL16);

    graph = gb_finalize(&gb, n_lui);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 1, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    record = find_record(&result, n_call);
    TASK_CHECK(record != NULL);
    TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_GOT);
    TASK_CHECK(record->callee.symbolIndex == 5);
    // The slot's t9 rewrite does not leak into the selected callee.
    TASK_CHECK(record->callee.addend == 0);
    TASK_CHECK(record->linkVram == gb.nodes[n_call].vram + 8);
    // a0 was never assigned on this path.
    TASK_CHECK(record->args[0].kind == RABBITIZER_PIC_VALUE_UNKNOWN);
    TASK_CHECK((record->unknownReasons & RABBITIZER_PIC_REASON_ARG0_UNKNOWN) != 0);
    return 0;
}

// A direct jal keeps its static target and does not need a register value.
static int test_call_order_direct_jal(void) {
    GraphBuilder gb;
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry, n_call, end;
    const RabbitizerPicCallRecord *record;
    const uint32_t targetVram = 0x00402000u;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    n_call = gb_call(&gb, enc_jal(targetVram), enc_nop());
    end = gb_node(&gb, enc_nop());
    gb_edge(&gb, entry, n_call);
    gb_edge(&gb, n_call, end);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, NULL, 0, NULL, 0, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    record = find_record(&result, n_call);
    TASK_CHECK(record != NULL);
    TASK_CHECK(record->isDirectCall);
    TASK_CHECK(record->staticTargetVram == targetVram);
    TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_CONST);
    TASK_CHECK(record->callee.constValue == targetVram);
    TASK_CHECK(record->linkVram == gb.nodes[n_call].vram + 8);
    return 0;
}

// Caller-saved registers die across the call; s0 keeps gp alive for a restore.
static int test_call_order_clobber_and_restore(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[5];
    RabbitizerPicGotSlot slots[2];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t n_lui, n_lo, n_lw1, n_call, n_lw2, n_call2, n_end;
    const RabbitizerPicCallRecord *first;
    const RabbitizerPicCallRecord *second;
    size_t r = 0;

    gb_init(&gb);
    n_lui = gb_node(&gb, enc_lui(REG_GP, 0x1234));
    n_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, -16));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    n_lw1 = gb_node(&gb, enc_lw(REG_T9, -32752, REG_GP));
    gb_node(&gb, enc_addu(REG_S0, REG_GP, REG_ZERO));
    gb_node(&gb, enc_addiu(REG_T0, REG_ZERO, 5));
    n_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    // Restore gp through an `or` copy from the callee-saved s0.
    gb_node(&gb, enc_or(REG_GP, REG_S0, REG_ZERO));
    n_lw2 = gb_node(&gb, enc_lw(REG_T9, 16, REG_GP));
    n_call2 = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_end = gb_node(&gb, enc_nop());
    gb_chain(&gb, n_lui, n_end);

    relocs[r++] = make_reloc(n_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_A, 90, 0, n_lui);
    relocs[r++] = make_reloc(n_lw1, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_A, 5, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lw2, RABBITIZER_PIC_RELOC_GOT16, TEST_OBJECT_A, 6, 0,
                             RABBITIZER_PIC_NO_NODE);
    slots[0] = make_got(TEST_OBJECT_A, -32752, 5, RABBITIZER_PIC_GOT_SOURCE_CALL16);
    slots[1] = make_got(TEST_OBJECT_A, 16, 6, RABBITIZER_PIC_GOT_SOURCE_GOT16);

    graph = gb_finalize(&gb, n_lui);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 2, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);

    first = find_record(&result, n_call);
    TASK_CHECK(first != NULL);
    TASK_CHECK(first->callee.symbolIndex == 5);
    // The second call's GOT resolution proves gp was restored from s0 after the clobber.
    second = find_record(&result, n_call2);
    TASK_CHECK(second != NULL);
    TASK_CHECK(second->callee.kind == RABBITIZER_PIC_VALUE_GOT);
    TASK_CHECK(second->callee.symbolIndex == 6);
    TASK_CHECK(!result.saturated);
    return 0;
}

/* ================= PicFixpoint ================= */

// A loop feeding an unknown register reaches a fixpoint and still reports the call.
static int test_fixpoint_loop_unknown(void) {
    GraphBuilder gb;
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry, loop, exit_call, end;
    const RabbitizerPicCallRecord *record;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    loop = gb_node(&gb, enc_addiu(REG_T0, REG_T0, 1)); // unknown + const stays unknown
    exit_call = gb_call(&gb, enc_jalr(REG_RA, REG_T0), enc_nop());
    end = gb_node(&gb, enc_nop());
    gb_edge(&gb, entry, loop);
    gb_edge(&gb, loop, loop);  // back edge
    gb_edge(&gb, loop, exit_call);
    gb_edge(&gb, exit_call, end);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, NULL, 0, NULL, 0, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(!result.saturated);
    TASK_CHECK(result.peakStateCount == 1);
    record = find_record(&result, exit_call);
    TASK_CHECK(record != NULL);
    TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_UNKNOWN);
    TASK_CHECK((record->unknownReasons & RABBITIZER_PIC_REASON_CALLEE_UNKNOWN) != 0);
    return 0;
}

// Hitting the state cap saturates to unknown; a later definite write reclaims the fact.
static int test_fixpoint_saturation_reclaim(void) {
    GraphBuilder gb;
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry;
    size_t leaves[5];
    size_t merge, reclaim, call, end;
    const RabbitizerPicCallRecord *saturatedRecord = NULL;
    size_t i;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    for (i = 0; i < 5; i++) {
        leaves[i] = gb_node(&gb, enc_addiu(REG_T0, REG_ZERO, (int16_t)(i + 1)));
        gb_edge(&gb, entry, leaves[i]);
    }
    merge = gb_node(&gb, enc_nop());
    reclaim = gb_node(&gb, enc_addiu(REG_A0, REG_ZERO, 42));
    call = gb_call(&gb, enc_jalr(REG_RA, REG_T0), enc_nop());
    end = gb_node(&gb, enc_nop());
    for (i = 0; i < 5; i++) {
        gb_edge(&gb, leaves[i], merge);
    }
    gb_edge(&gb, merge, reclaim);
    gb_edge(&gb, reclaim, call);
    gb_edge(&gb, call, end);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, NULL, 0, NULL, 0, 2, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(result.saturated);
    for (i = 0; i < result.recordCount; i++) {
        if (result.records[i].nodeIndex == call && result.records[i].saturated) {
            saturatedRecord = &result.records[i];
        }
    }
    TASK_CHECK(saturatedRecord != NULL);
    TASK_CHECK(saturatedRecord->callee.kind == RABBITIZER_PIC_VALUE_UNKNOWN);
    TASK_CHECK((saturatedRecord->unknownReasons & RABBITIZER_PIC_REASON_STATE_LIMIT) != 0);
    TASK_CHECK((saturatedRecord->unknownReasons & RABBITIZER_PIC_REASON_CALLEE_UNKNOWN) != 0);
    // The deterministic a0 assignment is recovered even on the saturated path.
    TASK_CHECK(saturatedRecord->args[0].kind == RABBITIZER_PIC_VALUE_CONST);
    TASK_CHECK(saturatedRecord->args[0].constValue == 42);
    TASK_CHECK((saturatedRecord->unknownReasons & RABBITIZER_PIC_REASON_ARG0_UNKNOWN) == 0);
    return 0;
}

// With a generous cap every concrete path survives as its own de-duplicated record.
static int test_fixpoint_all_paths_kept(void) {
    GraphBuilder gb;
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry;
    size_t leaves[5];
    size_t merge, call, end;
    size_t i;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    for (i = 0; i < 5; i++) {
        leaves[i] = gb_node(&gb, enc_addiu(REG_T0, REG_ZERO, (int16_t)(i + 1)));
        gb_edge(&gb, entry, leaves[i]);
    }
    merge = gb_node(&gb, enc_nop());
    call = gb_call(&gb, enc_jalr(REG_RA, REG_T0), enc_nop());
    end = gb_node(&gb, enc_nop());
    for (i = 0; i < 5; i++) {
        gb_edge(&gb, leaves[i], merge);
    }
    gb_edge(&gb, merge, call);
    gb_edge(&gb, call, end);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, NULL, 0, NULL, 0, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(!result.saturated);
    TASK_CHECK(count_records_at(&result, call) == 5);
    for (i = 0; i < result.recordCount; i++) {
        TASK_CHECK(result.records[i].callee.kind == RABBITIZER_PIC_VALUE_CONST);
        TASK_CHECK(result.records[i].callee.constValue >= 1);
        TASK_CHECK(result.records[i].callee.constValue <= 5);
        TASK_CHECK(!result.records[i].saturated);
    }
    return 0;
}

// A generic %hi(sym)/%lo(sym) pair keeps symbol identity, addend and origin.
static int test_correlation_generic_hi_lo_pair(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[2];
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry, n_hi, n_lo, n_call, n_end;
    const RabbitizerPicCallRecord *record;
    size_t r = 0;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    n_hi = gb_node(&gb, enc_lui(REG_T0, 0x0050));
    n_lo = gb_node(&gb, enc_addiu(REG_T0, REG_T0, 0x10));
    gb_node(&gb, enc_addu(REG_T9, REG_T0, REG_ZERO));
    n_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_end = gb_node(&gb, enc_nop());
    gb_chain(&gb, entry, n_end);

    relocs[r++] = make_reloc(n_hi, RABBITIZER_PIC_RELOC_HI16, TEST_OBJECT_A, 44, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(n_lo, RABBITIZER_PIC_RELOC_LO16, TEST_OBJECT_A, 44, 12, n_hi);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, relocs, r, NULL, 0, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    record = find_record(&result, n_call);
    TASK_CHECK(record != NULL);
    TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_SYMBOL);
    TASK_CHECK(record->callee.objectId == TEST_OBJECT_A);
    TASK_CHECK(record->callee.symbolIndex == 44);
    // upper half folded into the addend, plus the lo immediate and explicit addend.
    TASK_CHECK(record->callee.addend == (int32_t)0x00500000 + 0x10 + 12);
    return 0;
}

// t9 carries the function entry at function start; an indirect call before any
// other t9 write observes that entry value.
static int test_correlation_entry_t9(void) {
    GraphBuilder gb;
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry, n_call, n_end;
    const RabbitizerPicCallRecord *record;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    n_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    n_end = gb_node(&gb, enc_nop());
    gb_edge(&gb, entry, n_call);
    gb_edge(&gb, n_call, n_end);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, NULL, 0, NULL, 0, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    record = find_record(&result, n_call);
    TASK_CHECK(record != NULL);
    TASK_CHECK(record->callee.kind == RABBITIZER_PIC_VALUE_ENTRY);
    TASK_CHECK(record->callee.constValue == graph.entryVram);
    TASK_CHECK((record->unknownReasons & RABBITIZER_PIC_REASON_CALLEE_UNKNOWN) == 0);
    return 0;
}

// Nodes unreachable from the entry are never analyzed and produce no records.
static int test_fixpoint_unreachable_not_analyzed(void) {
    GraphBuilder gb;
    RabbitizerPicCallRecord records[TEST_RECORD_CAP];
    RabbitizerPicCallResult result;
    RabbitizerPicGraph graph;
    size_t entry, reachable_call, reachable_end;
    size_t orphan, unreachable_call, unreachable_end;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    reachable_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    reachable_end = gb_node(&gb, enc_nop());
    orphan = gb_node(&gb, enc_addiu(REG_T0, REG_ZERO, 3));
    unreachable_call = gb_call(&gb, enc_jalr(REG_RA, REG_T0), enc_nop());
    unreachable_end = gb_node(&gb, enc_nop());

    gb_edge(&gb, entry, reachable_call);
    gb_edge(&gb, reachable_call, reachable_end);
    // The orphan chain has no incoming edge from the entry component.
    gb_edge(&gb, orphan, unreachable_call);
    gb_edge(&gb, unreachable_call, unreachable_end);

    graph = gb_finalize(&gb, entry);
    TASK_CHECK(run_analysis(&graph, NULL, 0, NULL, 0, 64, records, &result) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(result.recordCount == 1);
    TASK_CHECK(find_record(&result, reachable_call) != NULL);
    TASK_CHECK(find_record(&result, unreachable_call) == NULL);
    return 0;
}

// Repeated runs give identical output and the caller's inputs stay untouched.
static int test_fixpoint_determinism_and_immutability(void) {
    GraphBuilder gb;
    RabbitizerPicRelocation relocs[8];
    RabbitizerPicGotSlot slots[2];
    RabbitizerPicCallRecord recordsA[TEST_RECORD_CAP];
    RabbitizerPicCallRecord recordsB[TEST_RECORD_CAP];
    RabbitizerPicCallResult resultA;
    RabbitizerPicCallResult resultB;
    RabbitizerPicGraph graph;
    RabbitizerPicGraphNode savedNodes[GB_MAX_NODES];
    RabbitizerPicGraphEdge savedEdges[GB_MAX_EDGES];
    RabbitizerPicRelocation savedRelocs[8];
    RabbitizerPicGotSlot savedSlots[2];
    size_t entry;
    size_t a_lui, a_lo, a_lw, b_lui, b_lo, b_lw, merge_call, end;
    size_t r = 0;

    gb_init(&gb);
    entry = gb_node(&gb, enc_nop());
    a_lui = gb_node(&gb, enc_lui(REG_GP, 0x1000));
    a_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, 0));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    a_lw = gb_node(&gb, enc_lw(REG_T9, -100, REG_GP));
    b_lui = gb_node(&gb, enc_lui(REG_GP, 0x2000));
    b_lo = gb_node(&gb, enc_addiu(REG_GP, REG_GP, 0));
    gb_node(&gb, enc_addu(REG_GP, REG_GP, REG_T9));
    b_lw = gb_node(&gb, enc_lw(REG_T9, -100, REG_GP));
    merge_call = gb_call(&gb, enc_jalr(REG_RA, REG_T9), enc_nop());
    end = gb_node(&gb, enc_nop());
    gb_edge(&gb, entry, a_lui);
    gb_edge(&gb, entry, b_lui);
    gb_chain(&gb, a_lui, a_lw);
    gb_edge(&gb, a_lw, merge_call);
    gb_chain(&gb, b_lui, b_lw);
    gb_edge(&gb, b_lw, merge_call);
    gb_edge(&gb, merge_call, end);

    relocs[r++] = make_reloc(a_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_A, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(a_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_A, 90, 0, a_lui);
    relocs[r++] = make_reloc(a_lw, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_A, 11, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(b_lui, RABBITIZER_PIC_RELOC_GP_DISP_HI16, TEST_OBJECT_B, 90, 0,
                             RABBITIZER_PIC_NO_NODE);
    relocs[r++] = make_reloc(b_lo, RABBITIZER_PIC_RELOC_GP_DISP_LO16, TEST_OBJECT_B, 90, 0, b_lui);
    relocs[r++] = make_reloc(b_lw, RABBITIZER_PIC_RELOC_CALL16, TEST_OBJECT_B, 11, 0,
                             RABBITIZER_PIC_NO_NODE);
    slots[0] = make_got(TEST_OBJECT_A, -100, 11, RABBITIZER_PIC_GOT_SOURCE_CALL16);
    slots[1] = make_got(TEST_OBJECT_B, -100, 11, RABBITIZER_PIC_GOT_SOURCE_CALL16);

    graph = gb_finalize(&gb, entry);
    memcpy(savedNodes, graph.nodes, graph.nodeCount * sizeof(savedNodes[0]));
    memcpy(savedEdges, graph.edges, graph.edgeCount * sizeof(savedEdges[0]));
    memcpy(savedRelocs, relocs, r * sizeof(savedRelocs[0]));
    memcpy(savedSlots, slots, 2 * sizeof(savedSlots[0]));

    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 2, 64, recordsA, &resultA) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(run_analysis(&graph, relocs, r, slots, 2, 64, recordsB, &resultB) ==
               RABBITIZER_PIC_ANALYSIS_OK);
    TASK_CHECK(resultA.recordCount == resultB.recordCount);
    TASK_CHECK(memcmp(recordsA, recordsB, resultA.recordCount * sizeof(recordsA[0])) == 0);

    // The analysis must not mutate any of its inputs.
    TASK_CHECK(memcmp(savedNodes, graph.nodes, graph.nodeCount * sizeof(savedNodes[0])) == 0);
    TASK_CHECK(memcmp(savedEdges, graph.edges, graph.edgeCount * sizeof(savedEdges[0])) == 0);
    TASK_CHECK(memcmp(savedRelocs, relocs, r * sizeof(savedRelocs[0])) == 0);
    TASK_CHECK(memcmp(savedSlots, slots, 2 * sizeof(savedSlots[0])) == 0);
    TASK_CHECK(count_records_at(&resultA, merge_call) == 2);
    return 0;
}

const struct task_case task_cases[] = {
    { "PicGp", "full_chain", test_pic_gp_full_chain },
    { "PicGp", "plain_lw_no_impersonation", test_pic_gp_plain_lw_no_impersonation },
    { "PicGp", "inconsistent_gp_object", test_pic_gp_inconsistent_object },
    { "PicGp", "addiu_gp_bias", test_pic_gp_addiu_bias },

    { "PicCorrelation", "distinct_identities_paired", test_correlation_distinct_identities_paired },
    { "PicCorrelation", "local_lo16_identity", test_correlation_local_lo16_identity },
    { "PicCorrelation", "generic_hi_lo_pair", test_correlation_generic_hi_lo_pair },
    { "PicCorrelation", "entry_t9", test_correlation_entry_t9 },
    { "PicCorrelation", "dedup", test_correlation_dedup },

    { "PicCallOrder", "delay_slot_t9", test_call_order_delay_slot_t9 },
    { "PicCallOrder", "direct_jal", test_call_order_direct_jal },
    { "PicCallOrder", "clobber_and_restore", test_call_order_clobber_and_restore },

    { "PicFixpoint", "loop_unknown", test_fixpoint_loop_unknown },
    { "PicFixpoint", "saturation_reclaim", test_fixpoint_saturation_reclaim },
    { "PicFixpoint", "all_paths_kept", test_fixpoint_all_paths_kept },
    { "PicFixpoint", "unreachable_not_analyzed", test_fixpoint_unreachable_not_analyzed },
    { "PicFixpoint", "determinism_and_immutability", test_fixpoint_determinism_and_immutability },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
