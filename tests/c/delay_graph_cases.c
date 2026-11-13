/* SPDX-FileCopyrightText: © 2024 Decompollaborate */
/* SPDX-License-Identifier: MIT */

/*
 * Regression cases for the delay-slot-aware RabbitizerFlowGraph builder.
 *
 * Every case enumerates the exact node and edge set it expects instead of
 * checking aggregate counts only.
 */

#include "rabbitizer.h"
#include "support/check.h"

#include <string.h>

#define MAX_NODES 64
#define MAX_EDGES 128
#define MAX_DIAGS 32

#define REG_ZERO 0
#define REG_T0 8
#define REG_T1 9
#define REG_T9 25
#define REG_RA 31

#define INVALID_WORD 0x7C000000u

static uint32_t branchI(uint32_t opcode, uint32_t rs, uint32_t rt, int16_t offset) {
    return (opcode << 26) | (rs << 21) | (rt << 16) | ((uint32_t)offset & 0xFFFFu);
}

static uint32_t regimm(uint32_t rtField, uint32_t rs, int16_t offset) {
    return branchI(0x01, rs, rtField, offset);
}

static uint32_t jumpJ(uint32_t opcode, uint32_t targetVram) {
    return (opcode << 26) | ((targetVram >> 2) & 0x3FFFFFFu);
}

static uint32_t jr(uint32_t rs) {
    return (rs << 21) | 0x08u;
}

static uint32_t jalr(uint32_t rd, uint32_t rs) {
    return (rs << 21) | (rd << 11) | 0x09u;
}

static uint32_t addiu(uint32_t rt, uint32_t rs, uint16_t imm) {
    return (0x09u << 26) | (rs << 21) | (rt << 16) | imm;
}

typedef struct GraphBox {
    RabbitizerFlowGraph graph;
    RabbitizerFlowGraphNode nodes[MAX_NODES];
    RabbitizerFlowGraphEdge edges[MAX_EDGES];
    RabbitizerFlowGraphDiagnostic diags[MAX_DIAGS];
} GraphBox;

static void buildGraph(GraphBox *box, const uint32_t *words, size_t wordCount, uint32_t startVram,
                       const uint32_t *entries, size_t entryCount) {
    RabbitizerFlowGraphBuildOptions options = {
        .words = words,
        .wordCount = wordCount,
        .startVram = startVram,
        .entryVrams = entries,
        .entryCount = entryCount,
    };

    RabbitizerFlowGraph_init(&box->graph, box->nodes, MAX_NODES, box->edges, MAX_EDGES, box->diags, MAX_DIAGS);
    RabbitizerFlowGraphError error = RabbitizerFlowGraph_build(&box->graph, &options);
    TASK_CHECK(error == RAB_FLOWGRAPH_ERROR_OK);
    TASK_CHECK(box->graph.complete);
}

static int findNode(GraphBox *box, uint32_t vram, RabbitizerFlowGraphNodeKind kind, uint32_t branchVram,
                    RabbitizerFlowGraphOutcome outcome) {
    size_t i;

    for (i = 0; i < box->graph.nodeCount; i++) {
        RabbitizerFlowGraphNode *node = &box->nodes[i];
        if (node->vram == vram && node->kind == kind && node->branchVram == branchVram &&
            node->outcome == outcome) {
            return (int)i;
        }
    }
    return -1;
}

typedef struct ExpectedNode {
    uint32_t vram;
    RabbitizerFlowGraphNodeKind kind;
    uint32_t branchVram;
    RabbitizerFlowGraphOutcome outcome;
} ExpectedNode;

static void checkNodeSet(GraphBox *box, const ExpectedNode *expected, size_t expectedCount) {
    size_t i;

    TASK_CHECK(box->graph.nodeCount == expectedCount);
    for (i = 0; i < expectedCount; i++) {
        TASK_CHECK(findNode(box, expected[i].vram, expected[i].kind, expected[i].branchVram,
                            expected[i].outcome) >= 0);
    }
}

typedef struct ExpectedEdge {
    RabbitizerFlowGraphEdgeKind kind;
    uint32_t toVram;
    RabbitizerFlowGraphOutcome toOutcome;
    uint32_t targetVram;
} ExpectedEdge;

static void checkEdges(GraphBox *box, int fromIndex, const ExpectedEdge *expected, size_t expectedCount) {
    size_t i;
    size_t actualCount = 0;

    for (i = 0; i < box->graph.edgeCount; i++) {
        if (box->edges[i].fromNodeIndex == (uint32_t)fromIndex) {
            actualCount++;
        }
    }
    TASK_CHECK(actualCount == expectedCount);

    i = 0;
    for (size_t edgeIndex = 0; edgeIndex < box->graph.edgeCount && i < expectedCount; edgeIndex++) {
        RabbitizerFlowGraphEdge *edge = &box->edges[edgeIndex];
        const ExpectedEdge *want;
        const RabbitizerFlowGraphNode *toNode;

        if (edge->fromNodeIndex != (uint32_t)fromIndex) {
            continue;
        }
        want = &expected[i];
        TASK_CHECK(edge->kind == want->kind);
        TASK_CHECK(edge->targetVram == want->targetVram);

        if (want->toVram == RAB_FLOWGRAPH_ADDRESS_NONE) {
            TASK_CHECK(edge->toNodeIndex == RAB_FLOWGRAPH_NODE_ID_NONE);
        } else {
            TASK_CHECK(edge->toNodeIndex != RAB_FLOWGRAPH_NODE_ID_NONE);
            toNode = &box->nodes[edge->toNodeIndex];
            TASK_CHECK(toNode->vram == want->toVram);
            TASK_CHECK(toNode->outcome == want->toOutcome);
        }
        i++;
    }
    TASK_CHECK(i == expectedCount);
}

static int hasDiagnostic(GraphBox *box, RabbitizerFlowGraphDiagnosticKind kind, uint32_t branchVram,
                         uint32_t currentVram) {
    size_t i;

    for (i = 0; i < box->graph.diagnosticCount; i++) {
        RabbitizerFlowGraphDiagnostic *diag = &box->diags[i];
        if (diag->kind == kind && diag->branchVram == branchVram && diag->currentVram == currentVram) {
            return 1;
        }
    }
    return 0;
}

static void checkStableOrdering(GraphBox *box) {
    size_t i;

    for (i = 1; i < box->graph.nodeCount; i++) {
        const RabbitizerFlowGraphNode *left = &box->nodes[i - 1];
        const RabbitizerFlowGraphNode *right = &box->nodes[i];
        TASK_CHECK(left->vram < right->vram ||
                   (left->vram == right->vram && left->kind <= right->kind));
    }
    for (i = 1; i < box->graph.edgeCount; i++) {
        const RabbitizerFlowGraphEdge *left = &box->edges[i - 1];
        const RabbitizerFlowGraphEdge *right = &box->edges[i];
        TASK_CHECK(left->fromNodeIndex <= right->fromNodeIndex);
    }
    for (i = 1; i < box->graph.diagnosticCount; i++) {
        const RabbitizerFlowGraphDiagnostic *left = &box->diags[i - 1];
        const RabbitizerFlowGraphDiagnostic *right = &box->diags[i];
        TASK_CHECK(left->kind < right->kind ||
                   (left->kind == right->kind && left->branchVram <= right->branchVram));
    }
}

/* Likely branch: the annul path reaches PC+8 without dispatching the slot. */
static void case_likely_both_paths(void) {
    const uint32_t start = 0x1000;
    const uint32_t words[] = {
        branchI(0x14, REG_T0, REG_T1, 2), /* 0x1000 BEQL -> 0x100C */
        0x00000000u,                      /* 0x1004 nop, slot only on the taken path */
        addiu(REG_T0, REG_ZERO, 1),       /* 0x1008 annul landing */
        addiu(REG_T0, REG_ZERO, 2),       /* 0x100C taken landing */
        jr(REG_RA),                       /* 0x1010 */
        0x00000000u,                      /* 0x1014 */
        0x00000000u,                      /* 0x1018 */
        0x00000000u,                      /* 0x101C */
    };
    const uint32_t entries[] = { start };
    GraphBox box;
    int branchNode;
    int slotTaken;
    int annulNode;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries, 1);
    TASK_CHECK(box.graph.diagnosticCount == 0);

    const ExpectedNode expectedNodes[] = {
        { 0x1000, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x1008, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x100C, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x1010, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x1014, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x1010, RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL },
        { 0x1004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x1000, RAB_FLOWGRAPH_OUTCOME_TAKEN },
    };
    checkNodeSet(&box, expectedNodes, sizeof(expectedNodes) / sizeof(expectedNodes[0]));

    branchNode = findNode(&box, 0x1000, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                          RAB_FLOWGRAPH_OUTCOME_NONE);
    slotTaken = findNode(&box, 0x1004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x1000,
                         RAB_FLOWGRAPH_OUTCOME_TAKEN);
    annulNode = findNode(&box, 0x1008, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                         RAB_FLOWGRAPH_OUTCOME_NONE);
    TASK_CHECK(branchNode >= 0 && slotTaken >= 0 && annulNode >= 0);

    TASK_CHECK(box.nodes[slotTaken].pendingTargetVram == 0x100C);
    TASK_CHECK(findNode(&box, 0x1004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x1000,
                        RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN) == -1);

    const ExpectedEdge branchEdges[] = {
        { RAB_FLOWGRAPH_EDGE_DELAY_ENTER, 0x1004, RAB_FLOWGRAPH_OUTCOME_TAKEN, 0x1004 },
        { RAB_FLOWGRAPH_EDGE_LIKELY_ANNUL, 0x1008, RAB_FLOWGRAPH_OUTCOME_NONE, 0x1008 },
    };
    checkEdges(&box, branchNode, branchEdges, 2);
    checkEdges(&box, slotTaken,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SLOT_TAKEN, 0x100C, RAB_FLOWGRAPH_OUTCOME_NONE,
                                         0x100C } },
               1);
}

/* Same slot address reached as a normal entry and as both branch outcomes. */
static void case_slot_with_normal_entry(void) {
    const uint32_t start = 0x2000;
    const uint32_t words[] = {
        branchI(0x04, REG_ZERO, REG_ZERO, 4), /* 0x2000 BEQ -> 0x2014 */
        addiu(REG_T0, REG_ZERO, 7),           /* 0x2004 slot and standalone entry */
        addiu(REG_T1, REG_ZERO, 1),           /* 0x2008 not taken landing */
        addiu(REG_T0, REG_ZERO, 2),           /* 0x200C */
        jr(REG_RA),                           /* 0x2010 */
        0x00000000u,                          /* 0x2014 jr slot / taken landing */
        jr(REG_RA),                           /* 0x2018 */
        0x00000000u,                          /* 0x201C */
    };
    const uint32_t entries[] = { 0x2004, 0x2000 };
    const uint32_t reversedEntries[] = { 0x2000, 0x2004, 0x2000 };
    GraphBox box;
    GraphBox reversed;
    int normalSlot;
    int slotTaken;
    int slotNotTaken;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries, 2);
    TASK_CHECK(box.graph.diagnosticCount == 0);

    const ExpectedNode expectedNodes[] = {
        { 0x2000, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x2004, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x2008, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x200C, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x2010, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x2014, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x2018, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x2004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x2000, RAB_FLOWGRAPH_OUTCOME_TAKEN },
        { 0x2004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x2000, RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN },
        { 0x2014, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x2010, RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL },
        { 0x201C, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x2018, RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL },
    };
    checkNodeSet(&box, expectedNodes, sizeof(expectedNodes) / sizeof(expectedNodes[0]));

    normalSlot = findNode(&box, 0x2004, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                          RAB_FLOWGRAPH_OUTCOME_NONE);
    slotTaken = findNode(&box, 0x2004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x2000,
                         RAB_FLOWGRAPH_OUTCOME_TAKEN);
    slotNotTaken = findNode(&box, 0x2004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x2000,
                            RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN);
    TASK_CHECK(normalSlot >= 0 && slotTaken >= 0 && slotNotTaken >= 0);

    /* The normal dispatch of the slot address just falls through, it inherits no pending jump. */
    checkEdges(&box, normalSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SEQUENTIAL, 0x2008, RAB_FLOWGRAPH_OUTCOME_NONE,
                                         0x2008 } },
               1);
    checkEdges(&box, slotNotTaken,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SLOT_NOT_TAKEN, 0x2008, RAB_FLOWGRAPH_OUTCOME_NONE,
                                         0x2008 } },
               1);
    checkEdges(&box, slotTaken,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SLOT_TAKEN, 0x2014, RAB_FLOWGRAPH_OUTCOME_NONE,
                                         0x2014 } },
               1);
    TASK_CHECK(box.nodes[slotTaken].pendingTargetVram == 0x2014);
    TASK_CHECK(box.nodes[slotNotTaken].pendingTargetVram == 0x2008);

    /* Entry ordering and duplicates must not change the canonical graph. */
    buildGraph(&reversed, words, sizeof(words) / sizeof(words[0]), start, reversedEntries, 3);
    TASK_CHECK(reversed.graph.nodeCount == box.graph.nodeCount);
    TASK_CHECK(reversed.graph.edgeCount == box.graph.edgeCount);
    TASK_CHECK(reversed.graph.diagnosticCount == box.graph.diagnosticCount);
    TASK_CHECK(memcmp(reversed.nodes, box.nodes, box.graph.nodeCount * sizeof(box.nodes[0])) == 0);
    TASK_CHECK(memcmp(reversed.edges, box.edges, box.graph.edgeCount * sizeof(box.edges[0])) == 0);
    TASK_CHECK(memcmp(reversed.diags, box.diags, box.graph.diagnosticCount * sizeof(box.diags[0])) == 0);
}

/* A slot write to the indirect target register must not change the captured target source. */
static void case_slot_overwrites_indirect_register(void) {
    const uint32_t start = 0x3000;
    const uint32_t words[] = {
        jr(REG_T0),                 /* 0x3000 JR $t0 */
        addiu(REG_T0, REG_ZERO, 0x55), /* 0x3004 slot overwrites $t0 */
        jr(REG_RA),                 /* 0x3008 */
        0x00000000u,                /* 0x300C */
    };
    const uint32_t entries[] = { 0x3000, 0x3008 };
    GraphBox box;
    int indirectSlot;
    int returnSlot;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries,
               sizeof(entries) / sizeof(entries[0]));
    TASK_CHECK(box.graph.diagnosticCount == 0);

    const ExpectedNode expectedNodes[] = {
        { 0x3000, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x3008, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x3004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x3000, RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL },
        { 0x300C, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x3008, RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL },
    };
    checkNodeSet(&box, expectedNodes, sizeof(expectedNodes) / sizeof(expectedNodes[0]));

    indirectSlot = findNode(&box, 0x3004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x3000,
                            RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    returnSlot = findNode(&box, 0x300C, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x3008,
                          RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    TASK_CHECK(indirectSlot >= 0 && returnSlot >= 0);

    TASK_CHECK(box.nodes[indirectSlot].targetRegister == REG_T0);
    TASK_CHECK(box.nodes[indirectSlot].pendingTargetVram == RAB_FLOWGRAPH_ADDRESS_NONE);
    TASK_CHECK(box.nodes[indirectSlot].isReturn == false);
    TASK_CHECK(box.nodes[returnSlot].isReturn == true);

    checkEdges(&box, indirectSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_UNRESOLVED, RAB_FLOWGRAPH_ADDRESS_NONE,
                                         RAB_FLOWGRAPH_OUTCOME_NONE, RAB_FLOWGRAPH_ADDRESS_NONE } },
               1);
    checkEdges(&box, returnSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_RETURN, RAB_FLOWGRAPH_ADDRESS_NONE,
                                         RAB_FLOWGRAPH_OUTCOME_NONE, RAB_FLOWGRAPH_ADDRESS_NONE } },
               1);
}

/* Missing slot: the likely annul edge is still recorded and the other paths keep going. */
static void case_missing_slot_annul_kept(void) {
    const uint32_t start = 0x4000;
    const uint32_t words[] = {
        branchI(0x14, REG_T0, REG_T1, 2), /* 0x4000 BEQL, slot 0x4004 absent, target 0x400C outside */
    };
    const uint32_t entries[] = { start };
    GraphBox box;
    int branchNode;

    buildGraph(&box, words, 1, start, entries, 1);
    TASK_CHECK(box.graph.complete);
    TASK_CHECK(box.graph.nodeCount == 1);
    TASK_CHECK(hasDiagnostic(&box, RAB_FLOWGRAPH_DIAG_MISSING_SLOT, 0x4000, 0x4004));
    TASK_CHECK(hasDiagnostic(&box, RAB_FLOWGRAPH_DIAG_TARGET_OUT_OF_REGION, 0x4000, 0x400C));
    checkStableOrdering(&box);

    branchNode = findNode(&box, 0x4000, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                          RAB_FLOWGRAPH_OUTCOME_NONE);
    TASK_CHECK(branchNode >= 0);
    /* The annul path survives even though the slot is unreachable. */
    checkEdges(&box, branchNode,
               (const ExpectedEdge[]){
                   { RAB_FLOWGRAPH_EDGE_DELAY_ENTER, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE,
                     0x4004 },
                   { RAB_FLOWGRAPH_EDGE_LIKELY_ANNUL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE,
                     0x4008 },
               },
               2);
}

/* Invalid slot word kills the taken path but the likely annul path still lands on PC+8. */
static void case_invalid_slot_annul_kept(void) {
    const uint32_t start = 0x4100;
    const uint32_t words[] = {
        branchI(0x14, REG_T0, REG_T1, 2), /* 0x4100 BEQL -> 0x410C */
        INVALID_WORD,                     /* 0x4104 invalid slot */
        addiu(REG_T0, REG_ZERO, 1),       /* 0x4108 annul landing */
        addiu(REG_T0, REG_ZERO, 2),       /* 0x410C */
    };
    const uint32_t entries[] = { start };
    GraphBox box;
    int branchNode;
    int badSlot;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries, 1);
    TASK_CHECK(hasDiagnostic(&box, RAB_FLOWGRAPH_DIAG_INVALID_INSTRUCTION, 0x4100, 0x4104));

    branchNode = findNode(&box, 0x4100, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                          RAB_FLOWGRAPH_OUTCOME_NONE);
    badSlot = findNode(&box, 0x4104, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x4100,
                       RAB_FLOWGRAPH_OUTCOME_TAKEN);
    TASK_CHECK(branchNode >= 0 && badSlot >= 0);

    TASK_CHECK(findNode(&box, 0x410C, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                        RAB_FLOWGRAPH_OUTCOME_NONE) >= 0);
    checkEdges(&box, branchNode,
               (const ExpectedEdge[]){
                   { RAB_FLOWGRAPH_EDGE_DELAY_ENTER, 0x4104, RAB_FLOWGRAPH_OUTCOME_TAKEN, 0x4104 },
                   { RAB_FLOWGRAPH_EDGE_LIKELY_ANNUL, 0x4108, RAB_FLOWGRAPH_OUTCOME_NONE, 0x4108 },
               },
               2);
    /* The invalid slot produces no outgoing edge, but the annul path is untouched. */
    checkEdges(&box, badSlot, NULL, 0);
}

/* Back edge: the taken delay context reuses the existing loop head node. */
static void case_back_edge(void) {
    const uint32_t start = 0x5000;
    const uint32_t words[] = {
        addiu(REG_T0, REG_ZERO, 1),          /* 0x5000 loop head */
        branchI(0x05, REG_T0, REG_T1, -2),  /* 0x5004 BNE -> 0x5000 */
        0x00000000u,                         /* 0x5008 slot */
        jr(REG_RA),                          /* 0x500C unreachable */
        0x00000000u,                         /* 0x5010 not taken landing */
    };
    const uint32_t entries[] = { start };
    GraphBox box;
    int branchNode;
    int headNode;
    int slotTaken;
    int slotNotTaken;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries, 1);
    TASK_CHECK(box.graph.diagnosticCount == 0);

    const ExpectedNode expectedNodes[] = {
        { 0x5000, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x5004, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x500C, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE },
        { 0x5008, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x5004, RAB_FLOWGRAPH_OUTCOME_TAKEN },
        { 0x5008, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x5004, RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN },
        { 0x5010, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x500C, RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL },
    };
    checkNodeSet(&box, expectedNodes, sizeof(expectedNodes) / sizeof(expectedNodes[0]));

    branchNode = findNode(&box, 0x5004, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                          RAB_FLOWGRAPH_OUTCOME_NONE);
    headNode = findNode(&box, 0x5000, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                        RAB_FLOWGRAPH_OUTCOME_NONE);
    slotTaken = findNode(&box, 0x5008, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x5004,
                         RAB_FLOWGRAPH_OUTCOME_TAKEN);
    slotNotTaken = findNode(&box, 0x5008, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x5004,
                            RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN);
    TASK_CHECK(branchNode >= 0 && headNode >= 0 && slotTaken >= 0 && slotNotTaken >= 0);

    checkEdges(&box, slotTaken,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SLOT_TAKEN, 0x5000, RAB_FLOWGRAPH_OUTCOME_NONE,
                                         0x5000 } },
               1);
    checkEdges(&box, slotNotTaken,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SLOT_NOT_TAKEN, 0x500C, RAB_FLOWGRAPH_OUTCOME_NONE,
                                         0x500C } },
               1);
}

/* J target uses the real PC high nibble, including across a region boundary and at address 0. */
static void case_jump_targets(void) {
    const uint32_t boundaryStart = 0x0FFFFFFC;
    const uint32_t boundaryWords[] = {
        jumpJ(0x02, 0x10000004), /* 0x0FFFFFFC J across the region end */
        0x00000000u,             /* 0x10000000 slot */
    };
    const uint32_t boundaryEntries[] = { boundaryStart };
    GraphBox boundary;
    int boundaryJump;
    int boundarySlot;

    buildGraph(&boundary, boundaryWords, 2, boundaryStart, boundaryEntries, 1);
    TASK_CHECK(hasDiagnostic(&boundary, RAB_FLOWGRAPH_DIAG_TARGET_OUT_OF_REGION, 0x0FFFFFFC, 0x10000004));

    boundaryJump = findNode(&boundary, 0x0FFFFFFC, RAB_FLOWGRAPH_NODE_KIND_NORMAL,
                            RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE);
    boundarySlot = findNode(&boundary, 0x10000000, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x0FFFFFFC,
                            RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    TASK_CHECK(boundaryJump >= 0 && boundarySlot >= 0);
    TASK_CHECK(boundary.nodes[boundarySlot].pendingTargetVram == 0x10000004);
    checkEdges(&boundary, boundarySlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_JUMP, RAB_FLOWGRAPH_ADDRESS_NONE,
                                         RAB_FLOWGRAPH_OUTCOME_NONE, 0x10000004 } },
               1);

    const uint32_t zeroStart = 0x00000000;
    const uint32_t zeroWords[] = {
        jumpJ(0x02, 0x00000004), /* 0x0 J, address 0 must not pull in 0x80000000 */
        0x00000000u,             /* 0x4 slot and jump target */
    };
    const uint32_t zeroEntries[] = { zeroStart };
    GraphBox zero;
    int zeroSlot;

    buildGraph(&zero, zeroWords, 2, zeroStart, zeroEntries, 1);
    TASK_CHECK(zero.graph.diagnosticCount == 0);
    zeroSlot = findNode(&zero, 0x4, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x0,
                        RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    TASK_CHECK(zeroSlot >= 0);
    TASK_CHECK(zero.nodes[zeroSlot].pendingTargetVram == 0x4);
    checkEdges(&zero, zeroSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_JUMP, 0x4, RAB_FLOWGRAPH_OUTCOME_NONE, 0x4 } },
               1);
}

/* Calls record link/target metadata, never follow the callee and capture JALR restrictions. */
static void case_calls(void) {
    const uint32_t start = 0x6000;
    const uint32_t words[] = {
        jumpJ(0x03, 0x6100),             /* 0x6000 JAL -> outside region */
        0x00000000u,                     /* 0x6004 slot */
        addiu(REG_T0, REG_ZERO, 1),      /* 0x6008 return site, own entry */
        jalr(REG_T9, REG_T0),            /* 0x600C JALR $t9, $t0 */
        addiu(REG_T0, REG_ZERO, 1),      /* 0x6010 slot overwrites $t0 */
        jr(REG_RA),                      /* 0x6014 */
        0x00000000u,                     /* 0x6018 */
        0x00000000u,                     /* 0x601C padding */
    };
    const uint32_t entries[] = { 0x6014, 0x6000, 0x6008 };
    GraphBox box;
    int jalSlot;
    int jalrSlot;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries, 3);
    TASK_CHECK(hasDiagnostic(&box, RAB_FLOWGRAPH_DIAG_TARGET_OUT_OF_REGION, 0x6000, 0x6100));

    jalSlot = findNode(&box, 0x6004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x6000,
                       RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    jalrSlot = findNode(&box, 0x6010, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x600C,
                        RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    TASK_CHECK(jalSlot >= 0 && jalrSlot >= 0);

    TASK_CHECK(box.nodes[jalSlot].isCall);
    TASK_CHECK(box.nodes[jalSlot].linkRegister == REG_RA);
    TASK_CHECK(box.nodes[jalSlot].returnSiteVram == 0x6008);
    TASK_CHECK(box.nodes[jalSlot].pendingTargetVram == 0x6100);
    checkEdges(&box, jalSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_CALL, RAB_FLOWGRAPH_ADDRESS_NONE,
                                         RAB_FLOWGRAPH_OUTCOME_NONE, 0x6100 } },
               1);

    TASK_CHECK(box.nodes[jalrSlot].isCall);
    TASK_CHECK(box.nodes[jalrSlot].linkRegister == REG_T9);
    TASK_CHECK(box.nodes[jalrSlot].returnSiteVram == 0x6014);
    TASK_CHECK(box.nodes[jalrSlot].targetRegister == REG_T0);
    TASK_CHECK(box.nodes[jalrSlot].pendingTargetVram == RAB_FLOWGRAPH_ADDRESS_NONE);
    checkEdges(&box, jalrSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_CALL, RAB_FLOWGRAPH_ADDRESS_NONE,
                                         RAB_FLOWGRAPH_OUTCOME_NONE, RAB_FLOWGRAPH_ADDRESS_NONE } },
               1);

    /* The callee is never followed. */
    TASK_CHECK(findNode(&box, 0x6100, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                        RAB_FLOWGRAPH_OUTCOME_NONE) == -1);

    /* JALR with rd == 0 performs no link. */
    const uint32_t noLinkStart = 0x7000;
    const uint32_t noLinkWords[] = { jalr(REG_ZERO, REG_T0), 0x00000000u };
    const uint32_t noLinkEntries[] = { noLinkStart };
    GraphBox noLink;
    int noLinkSlot;

    buildGraph(&noLink, noLinkWords, 2, noLinkStart, noLinkEntries, 1);
    noLinkSlot = findNode(&noLink, 0x7004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x7000,
                          RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    TASK_CHECK(noLinkSlot >= 0);
    TASK_CHECK(noLink.nodes[noLinkSlot].isCall == false);
    TASK_CHECK(noLink.nodes[noLinkSlot].linkRegister == RAB_FLOWGRAPH_REGISTER_NONE);
    TASK_CHECK(noLink.nodes[noLinkSlot].targetRegister == REG_T0);
    checkEdges(&noLink, noLinkSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_UNRESOLVED, RAB_FLOWGRAPH_ADDRESS_NONE,
                                         RAB_FLOWGRAPH_OUTCOME_NONE, RAB_FLOWGRAPH_ADDRESS_NONE } },
               1);

    /* JALR with rs == rd is a restricted form and stops the dispatch path. */
    const uint32_t restrictedStart = 0x8000;
    const uint32_t restrictedWords[] = { jalr(REG_T0, REG_T0), 0x00000000u };
    const uint32_t restrictedEntries[] = { restrictedStart };
    GraphBox restricted;

    buildGraph(&restricted, restrictedWords, 2, restrictedStart, restrictedEntries, 1);
    TASK_CHECK(hasDiagnostic(&restricted, RAB_FLOWGRAPH_DIAG_JALR_RS_EQUALS_RD, RAB_FLOWGRAPH_ADDRESS_NONE,
                             0x8000));
    TASK_CHECK(restricted.graph.nodeCount == 1);
    TASK_CHECK(findNode(&restricted, 0x8004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x8000,
                        RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL) == -1);
}

/* JR $ra is a return, any other JR is an unresolved jump. */
static void case_jr_classification(void) {
    const uint32_t start = 0x9000;
    const uint32_t words[] = {
        jr(REG_RA), 0x00000000u, /* 0x9000 / 0x9004 */
        jr(REG_T0), 0x00000000u, /* 0x9008 / 0x900C */
    };
    const uint32_t entries[] = { 0x9000, 0x9008 };
    GraphBox box;
    int returnSlot;
    int jumpSlot;

    buildGraph(&box, words, 4, start, entries, 2);
    returnSlot = findNode(&box, 0x9004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x9000,
                          RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    jumpSlot = findNode(&box, 0x900C, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x9008,
                        RAB_FLOWGRAPH_OUTCOME_UNCONDITIONAL);
    TASK_CHECK(returnSlot >= 0 && jumpSlot >= 0);
    TASK_CHECK(box.nodes[returnSlot].isReturn);
    TASK_CHECK(box.nodes[jumpSlot].isReturn == false);
    checkEdges(&box, returnSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_RETURN, RAB_FLOWGRAPH_ADDRESS_NONE,
                                         RAB_FLOWGRAPH_OUTCOME_NONE, RAB_FLOWGRAPH_ADDRESS_NONE } },
               1);
    checkEdges(&box, jumpSlot,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_UNRESOLVED, RAB_FLOWGRAPH_ADDRESS_NONE,
                                         RAB_FLOWGRAPH_OUTCOME_NONE, RAB_FLOWGRAPH_ADDRESS_NONE } },
               1);
}

/* Control inside a slot and unsupported control transfers are classified diagnostics. */
static void case_diagnostic_classification(void) {
    const uint32_t start = 0xA000;
    const uint32_t words[] = {
        jumpJ(0x02, 0xA010),    /* 0xA000 J */
        jr(REG_RA),             /* 0xA004 control transfer inside the slot */
        0x00000000u,            /* 0xA008 */
        0x00000000u,            /* 0xA00C */
        0x42000018u,            /* 0xA010 ERET */
        0x00000000u,            /* 0xA014 */
        0x00000000u,            /* 0xA018 */
        0x00000000u,            /* 0xA01C */
    };
    const uint32_t entries[] = { 0xA000, 0xA010 };
    GraphBox box;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries, 2);
    TASK_CHECK(hasDiagnostic(&box, RAB_FLOWGRAPH_DIAG_CONTROL_IN_SLOT, 0xA000, 0xA004));
    TASK_CHECK(hasDiagnostic(&box, RAB_FLOWGRAPH_DIAG_UNSUPPORTED_CONTROL, RAB_FLOWGRAPH_ADDRESS_NONE, 0xA010));
    checkStableOrdering(&box);
    /* A failed path does not take the other entries down. */
    TASK_CHECK(findNode(&box, 0xA010, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                        RAB_FLOWGRAPH_OUTCOME_NONE) >= 0);

    const uint32_t regimmStart = 0xB000;
    const uint32_t regimmWords[] = {
        regimm(0x10, REG_T0, 2), /* 0xB000 BLTZAL, linking REGIMM */
        0x00000000u,
    };
    const uint32_t regimmEntries[] = { regimmStart };
    GraphBox regimm;

    buildGraph(&regimm, regimmWords, 2, regimmStart, regimmEntries, 1);
    TASK_CHECK(hasDiagnostic(&regimm, RAB_FLOWGRAPH_DIAG_UNSUPPORTED_CONTROL, RAB_FLOWGRAPH_ADDRESS_NONE,
                             0xB000));
    TASK_CHECK(regimm.graph.nodeCount == 1);
}

/* Rejected arguments and exhausted capacities are explicit failures, never partial successes. */
static void case_failures(void) {
    const uint32_t words[] = { branchI(0x04, REG_ZERO, REG_ZERO, 2), 0x00000000u, 0x00000000u };
    const uint32_t entries[] = { 0xC000 };
    RabbitizerFlowGraphBuildOptions options = {
        .words = words,
        .wordCount = 3,
        .startVram = 0xC000,
        .entryVrams = entries,
        .entryCount = 1,
    };
    RabbitizerFlowGraph graph;
    RabbitizerFlowGraphNode tinyNodeBuffer[1];
    RabbitizerFlowGraphEdge edgeBuffer[8];
    RabbitizerFlowGraphDiagnostic diagBuffer[8];

    TASK_CHECK(RabbitizerFlowGraph_build(NULL, &options) == RAB_FLOWGRAPH_ERROR_NULL_ARG);
    {
        RabbitizerFlowGraphBuildOptions nullOptions = { 0 };
        RabbitizerFlowGraphNode nodeBuffer[4];
        nullOptions.entryVrams = entries;
        nullOptions.entryCount = 1;
        nullOptions.words = NULL;
        nullOptions.wordCount = 1;
        RabbitizerFlowGraph_init(&graph, nodeBuffer, 4, edgeBuffer, 8, diagBuffer, 8);
        TASK_CHECK(RabbitizerFlowGraph_build(&graph, &nullOptions) == RAB_FLOWGRAPH_ERROR_NULL_ARG);
    }
    {
        /* Zero counts with NULL arrays is a valid empty build. */
        RabbitizerFlowGraphBuildOptions emptyOptions = { 0 };
        RabbitizerFlowGraphNode nodeBuffer[4];
        RabbitizerFlowGraph_init(&graph, nodeBuffer, 4, edgeBuffer, 8, diagBuffer, 8);
        TASK_CHECK(RabbitizerFlowGraph_build(&graph, &emptyOptions) == RAB_FLOWGRAPH_ERROR_OK);
        TASK_CHECK(graph.complete);
        TASK_CHECK(graph.nodeCount == 0 && graph.edgeCount == 0 && graph.diagnosticCount == 0);
    }

    {
        const uint32_t badEntries[] = { 0xC010 };
        RabbitizerFlowGraphBuildOptions badOptions = options;
        badOptions.entryVrams = badEntries;
        RabbitizerFlowGraph_init(&graph, tinyNodeBuffer, 1, edgeBuffer, 8, diagBuffer, 8);
        TASK_CHECK(RabbitizerFlowGraph_build(&graph, &badOptions) == RAB_FLOWGRAPH_ERROR_BAD_ENTRY);
    }

    {
        RabbitizerFlowGraphBuildOptions wrappingOptions = options;
        wrappingOptions.startVram = 0xFFFFFFFCu;
        wrappingOptions.wordCount = 2;
        RabbitizerFlowGraph_init(&graph, tinyNodeBuffer, 1, edgeBuffer, 8, diagBuffer, 8);
        TASK_CHECK(RabbitizerFlowGraph_build(&graph, &wrappingOptions) == RAB_FLOWGRAPH_ERROR_BAD_REGION);
    }

    RabbitizerFlowGraph_init(&graph, tinyNodeBuffer, 1, edgeBuffer, 8, diagBuffer, 8);
    TASK_CHECK(RabbitizerFlowGraph_build(&graph, &options) == RAB_FLOWGRAPH_ERROR_CAPACITY);
    TASK_CHECK(graph.complete == false);
}

/* Pseudo instruction settings and input mutations must not affect the graph contents. */
static void case_determinism_and_independence(void) {
    const uint32_t start = 0xD000;
    uint32_t words[] = {
        branchI(0x04, REG_ZERO, REG_ZERO, 2), /* 0xD000 BEQ $zero,$zero, becomes `b` as a pseudo */
        0x00000000u,                          /* 0xD004 */
        0x00000000u,                          /* 0xD008 */
        jr(REG_RA),                           /* 0xD00C */
        0x00000000u,                          /* 0xD010 */
    };
    const uint32_t entries[] = { start };
    GraphBox pseudoOn;
    GraphBox pseudoOff;

    RabbitizerConfig_Cfg.pseudos.enablePseudos = true;
    buildGraph(&pseudoOn, words, 5, start, entries, 1);
    RabbitizerConfig_Cfg.pseudos.enablePseudos = false;
    buildGraph(&pseudoOff, words, 5, start, entries, 1);

    TASK_CHECK(pseudoOn.graph.nodeCount == pseudoOff.graph.nodeCount);
    TASK_CHECK(memcmp(pseudoOn.nodes, pseudoOff.nodes, pseudoOn.graph.nodeCount * sizeof(pseudoOn.nodes[0])) == 0);
    TASK_CHECK(memcmp(pseudoOn.edges, pseudoOff.edges, pseudoOn.graph.edgeCount * sizeof(pseudoOn.edges[0])) == 0);
    TASK_CHECK(pseudoOn.nodes[0].uniqueId == RABBITIZER_INSTR_ID_cpu_beq);
    checkStableOrdering(&pseudoOn);

    /* Nodes copy the raw word, they do not reference the caller's array. */
    {
        uint32_t saved = words[0];
        int nodeIndex = findNode(&pseudoOn, start, RAB_FLOWGRAPH_NODE_KIND_NORMAL,
                                 RAB_FLOWGRAPH_ADDRESS_NONE, RAB_FLOWGRAPH_OUTCOME_NONE);
        TASK_CHECK(nodeIndex >= 0);
        words[0] = INVALID_WORD;
        TASK_CHECK(pseudoOn.nodes[nodeIndex].word == saved);
        words[0] = saved;
    }
}

/* Both branch outcomes select the same successor address: the two delay contexts must
 * still be distinct nodes instead of collapsing into one unconditional successor. */
static void case_converging_outcomes_stay_distinct(void) {
    const uint32_t start = 0xE000;
    const uint32_t words[] = {
        branchI(0x04, REG_ZERO, REG_ZERO, 1), /* 0xE000 BEQ -> 0xE008, same address as the fallthrough */
        addiu(REG_T0, REG_ZERO, 1),           /* 0xE004 slot shared by both outcomes */
        addiu(REG_T1, REG_ZERO, 2),           /* 0xE008 shared landing */
        jr(REG_RA),                           /* 0xE00C */
        0x00000000u,                          /* 0xE010 */
    };
    const uint32_t entries[] = { start };
    GraphBox box;
    int slotTaken;
    int slotNotTaken;
    int landing;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries, 1);
    TASK_CHECK(box.graph.diagnosticCount == 0);

    slotTaken = findNode(&box, 0xE004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0xE000,
                         RAB_FLOWGRAPH_OUTCOME_TAKEN);
    slotNotTaken = findNode(&box, 0xE004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0xE000,
                            RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN);
    TASK_CHECK(slotTaken >= 0 && slotNotTaken >= 0);
    TASK_CHECK(slotTaken != slotNotTaken);
    TASK_CHECK(box.nodes[slotTaken].pendingTargetVram == 0xE008);
    TASK_CHECK(box.nodes[slotNotTaken].pendingTargetVram == 0xE008);

    checkEdges(&box, slotTaken,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SLOT_TAKEN, 0xE008, RAB_FLOWGRAPH_OUTCOME_NONE,
                                         0xE008 } },
               1);
    checkEdges(&box, slotNotTaken,
               (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SLOT_NOT_TAKEN, 0xE008, RAB_FLOWGRAPH_OUTCOME_NONE,
                                         0xE008 } },
               1);

    landing = findNode(&box, 0xE008, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                       RAB_FLOWGRAPH_OUTCOME_NONE);
    TASK_CHECK(landing >= 0);
}

/* REGIMM (BLTZ/BGEZ) and the remaining I-type test families, including their likely forms. */
static void case_regimm_and_remaining_branches(void) {
    const uint32_t start = 0xF000;
    const uint32_t words[] = {
        branchI(0x06, REG_T0, 0, 1),       /* 0xF000 BLEZ -> 0xF008 (both outcomes converge) */
        0x00000000u,                       /* 0xF004 slot */
        branchI(0x17, REG_T0, 0, 1),       /* 0xF008 BGTZL likely -> 0xF010 */
        0x00000000u,                       /* 0xF00C slot, taken only */
        regimm(0x00, REG_T0, -3),          /* 0xF010 BLTZ -> 0xF008, back edge */
        0x00000000u,                       /* 0xF014 slot */
        jr(REG_RA),                        /* 0xF018 */
        0x00000000u,                       /* 0xF01C */
    };
    const uint32_t entries[] = { start };
    GraphBox box;
    int blezNode;
    int bgtzlNode;

    buildGraph(&box, words, sizeof(words) / sizeof(words[0]), start, entries, 1);
    TASK_CHECK(box.graph.diagnosticCount == 0);
    TASK_CHECK(box.graph.nodeCount == 10);

    blezNode = findNode(&box, 0xF000, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                        RAB_FLOWGRAPH_OUTCOME_NONE);
    bgtzlNode = findNode(&box, 0xF008, RAB_FLOWGRAPH_NODE_KIND_NORMAL, RAB_FLOWGRAPH_ADDRESS_NONE,
                         RAB_FLOWGRAPH_OUTCOME_NONE);
    TASK_CHECK(blezNode >= 0 && bgtzlNode >= 0);
    TASK_CHECK(box.nodes[blezNode].uniqueId == RABBITIZER_INSTR_ID_cpu_blez);
    TASK_CHECK(box.nodes[bgtzlNode].uniqueId == RABBITIZER_INSTR_ID_cpu_bgtzl);

    /* BLEZ: both outcomes run the slot even though they converge on the same successor. */
    TASK_CHECK(findNode(&box, 0xF004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0xF000,
                        RAB_FLOWGRAPH_OUTCOME_TAKEN) >= 0);
    TASK_CHECK(findNode(&box, 0xF004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0xF000,
                        RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN) >= 0);

    /* BGTZL: only the taken path gets a slot, the annul path goes straight to PC+8. */
    TASK_CHECK(findNode(&box, 0xF00C, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0xF008,
                        RAB_FLOWGRAPH_OUTCOME_TAKEN) >= 0);
    TASK_CHECK(findNode(&box, 0xF00C, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0xF008,
                        RAB_FLOWGRAPH_OUTCOME_NOT_TAKEN) == -1);
    checkEdges(&box, bgtzlNode,
               (const ExpectedEdge[]){
                   { RAB_FLOWGRAPH_EDGE_DELAY_ENTER, 0xF00C, RAB_FLOWGRAPH_OUTCOME_TAKEN, 0xF00C },
                   { RAB_FLOWGRAPH_EDGE_LIKELY_ANNUL, 0xF010, RAB_FLOWGRAPH_OUTCOME_NONE, 0xF010 },
               },
               2);

    /* BLTZ back edge: the taken slot context points back at the existing 0xF008 node. */
    {
        int backSlot = findNode(&box, 0xF014, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0xF010,
                                RAB_FLOWGRAPH_OUTCOME_TAKEN);
        TASK_CHECK(backSlot >= 0);
        TASK_CHECK(box.nodes[backSlot].pendingTargetVram == 0xF008);
        checkEdges(&box, backSlot,
                   (const ExpectedEdge[]){ { RAB_FLOWGRAPH_EDGE_SLOT_TAKEN, 0xF008,
                                             RAB_FLOWGRAPH_OUTCOME_NONE, 0xF008 } },
                   1);
    }

    /* Every remaining supported branch family produces a delay-slot context. */
    const struct {
        uint32_t word;
        RabbitizerInstrId expectedId;
    } families[] = {
        { branchI(0x05, REG_T0, REG_T1, 1), RABBITIZER_INSTR_ID_cpu_bne },
        { branchI(0x15, REG_T0, REG_T1, 1), RABBITIZER_INSTR_ID_cpu_bnel },
        { branchI(0x16, REG_T0, 0, 1), RABBITIZER_INSTR_ID_cpu_blezl },
        { regimm(0x01, REG_T0, 1), RABBITIZER_INSTR_ID_cpu_bgez },
        { regimm(0x02, REG_T0, 1), RABBITIZER_INSTR_ID_cpu_bltzl },
        { regimm(0x03, REG_T0, 1), RABBITIZER_INSTR_ID_cpu_bgezl },
    };
    for (size_t family = 0; family < sizeof(families) / sizeof(families[0]); family++) {
        const uint32_t miniWords[] = { families[family].word, 0x00000000u };
        const uint32_t miniEntries[] = { 0x10000 };
        GraphBox mini;

        buildGraph(&mini, miniWords, 2, 0x10000, miniEntries, 1);
        TASK_CHECK(mini.nodes[0].uniqueId == families[family].expectedId);
        TASK_CHECK(findNode(&mini, 0x10004, RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT, 0x10000,
                            RAB_FLOWGRAPH_OUTCOME_TAKEN) >= 0);
    }
}

const TaskCase task_cases[] = {
    { "likely_both_paths", case_likely_both_paths },
    { "slot_with_normal_entry", case_slot_with_normal_entry },
    { "slot_overwrites_indirect_register", case_slot_overwrites_indirect_register },
    { "missing_slot_annul_kept", case_missing_slot_annul_kept },
    { "invalid_slot_annul_kept", case_invalid_slot_annul_kept },
    { "back_edge", case_back_edge },
    { "jump_targets", case_jump_targets },
    { "calls", case_calls },
    { "jr_classification", case_jr_classification },
    { "diagnostic_classification", case_diagnostic_classification },
    { "converging_outcomes_stay_distinct", case_converging_outcomes_stay_distinct },
    { "regimm_and_remaining_branches", case_regimm_and_remaining_branches },
    { "failures", case_failures },
    { "determinism_and_independence", case_determinism_and_independence },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
