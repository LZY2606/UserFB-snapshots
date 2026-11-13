/* SPDX-FileCopyrightText: © 2024 Decompollaborate */
/* SPDX-License-Identifier: MIT */

/*
 * Example for RabbitizerFlowGraph.
 *
 * The flow graph performs no heap allocation: every node, edge and diagnostic
 * is stored in caller provided buffers. This example keeps them on the stack,
 * but embedding contexts can hand in static storage or arena slices instead.
 *
 * Build (from the repository root):
 *   cc -std=c11 -I include -I tables $(find src -name '*.c') \
 *      examples/c/flowgraph_example.c -o flowgraph_example
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rabbitizer.h"

#define NODE_CAPACITY 32
#define EDGE_CAPACITY 64
#define DIAGNOSTIC_CAPACITY 16

static uint32_t branchI(uint32_t opcode, uint32_t rs, uint32_t rt, int16_t offset) {
    return (opcode << 26) | (rs << 21) | (rt << 16) | ((uint32_t)offset & 0xFFFFu);
}

static const char *nodeKindName(RabbitizerFlowGraphNodeKind kind) {
    return RabbitizerFlowGraphNodeKind_getName(kind);
}

int main(void) {
    /*
     * 0x80000000: beql $t0, $t1, +2  (likely branch; taken -> 0x8000000C)
     * 0x80000004: nop                (delay slot, annul path never runs it)
     * 0x80000008: addiu $t0, $zero, 1 (annul landing)
     * 0x8000000C: addiu $t0, $zero, 2 (taken landing)
     */
    const uint32_t words[] = {
        branchI(0x14, 8, 9, 2),
        0x00000000u,
        0x24080001u,
        0x24080002u,
    };
    const uint32_t startVram = 0x80000000u;
    const uint32_t entries[] = { startVram };

    RabbitizerFlowGraphNode nodeBuffer[NODE_CAPACITY];
    RabbitizerFlowGraphEdge edgeBuffer[EDGE_CAPACITY];
    RabbitizerFlowGraphDiagnostic diagnosticBuffer[DIAGNOSTIC_CAPACITY];
    RabbitizerFlowGraph graph;
    size_t i;

    RabbitizerFlowGraph_init(&graph, nodeBuffer, NODE_CAPACITY, edgeBuffer, EDGE_CAPACITY, diagnosticBuffer,
                             DIAGNOSTIC_CAPACITY);

    RabbitizerFlowGraphBuildOptions options = {
        .words = words,
        .wordCount = sizeof(words) / sizeof(words[0]),
        .startVram = startVram,
        .entryVrams = entries,
        .entryCount = sizeof(entries) / sizeof(entries[0]),
    };

    RabbitizerFlowGraphError error = RabbitizerFlowGraph_build(&graph, &options);
    if (error != RAB_FLOWGRAPH_ERROR_OK) {
        fprintf(stderr, "RabbitizerFlowGraph_build failed: %s (complete=%d)\n",
                RabbitizerFlowGraphError_getName(error), graph.complete);
        return 1;
    }

    printf("nodes: %zu\n", graph.nodeCount);
    for (i = 0; i < graph.nodeCount; i++) {
        const RabbitizerFlowGraphNode *node = &graph.nodes[i];
        printf("  [%zu] vram=0x%08" PRIx32 " word=0x%08" PRIx32 " kind=%s", i, node->vram, node->word,
               nodeKindName(node->kind));
        if (node->kind == RAB_FLOWGRAPH_NODE_KIND_DELAY_SLOT) {
            printf(" branch=0x%08" PRIx32 " outcome=%d target=0x%08" PRIx32, node->branchVram,
                   (int)node->outcome, node->pendingTargetVram);
        }
        printf("\n");
    }

    printf("edges: %zu\n", graph.edgeCount);
    for (i = 0; i < graph.edgeCount; i++) {
        const RabbitizerFlowGraphEdge *edge = &graph.edges[i];
        printf("  [%zu] node %" PRIu32 " -> ", i, edge->fromNodeIndex);
        if (edge->toNodeIndex == RAB_FLOWGRAPH_NODE_ID_NONE) {
            printf("<none>");
        } else {
            printf("node %" PRIu32, edge->toNodeIndex);
        }
        printf(" kind=%s target=0x%08" PRIx32 "\n", RabbitizerFlowGraphEdgeKind_getName(edge->kind),
               edge->targetVram);
    }

    printf("diagnostics: %zu\n", graph.diagnosticCount);
    for (i = 0; i < graph.diagnosticCount; i++) {
        const RabbitizerFlowGraphDiagnostic *diagnostic = &graph.diagnostics[i];
        printf("  %s branch=0x%08" PRIx32 " current=0x%08" PRIx32 "\n",
               RabbitizerFlowGraphDiagnosticKind_getName(diagnostic->kind), diagnostic->branchVram,
               diagnostic->currentVram);
    }

    return 0;
}
