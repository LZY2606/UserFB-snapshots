/*
 * Copyright (c) 2020 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 */

//
// shemu_graph_example - demonstrates how to obtain the dynamic instruction graph of an emulation
// with ShemuEmulateGraph, and how the ownership of the report works:
//
//  - ShemuEmulateGraph emulates exactly like ShemuEmulate (same context side-effects, same returned
//    status), and additionally hands out a SHEMU_GRAPH report;
//  - the report is owned by the caller, and is fully independent of the emulation context and of
//    the shellcode/stack buffers - those may be reused or released right after the call, and the
//    graph remains readable;
//  - the report must be released with ShemuGraphFree; freeing a NULL graph is allowed.
//

#include <stdio.h>
#include <string.h>

#include "bdshemu.h"


static const char *
edge_kind_name(
    SHEMU_GRAPH_EDGE_KIND Kind
    )
{
    switch (Kind)
    {
    case SHEMU_GRAPH_EDGE_SEQUENTIAL:    return "sequential";
    case SHEMU_GRAPH_EDGE_COND_TAKEN:    return "cond-taken";
    case SHEMU_GRAPH_EDGE_COND_NOT_TAKEN:return "cond-not-taken";
    case SHEMU_GRAPH_EDGE_JUMP:          return "jump";
    case SHEMU_GRAPH_EDGE_CALL:          return "call";
    case SHEMU_GRAPH_EDGE_RETURN:        return "return";
    default:                             return "?";
    }
}


int
main(
    void
    )
{
    // A self-modifying program: it toggles its own immediate between 0 and 7, then jumps back.
    //   0x200000: mov al, 0                      B0 00
    //   0x200002: xor byte [rip - 8], 7          80 35 F8 FF FF FF 07
    //   0x200009: jmp -11                        EB F5
    unsigned char code[] =
    {
        0xB0, 0x00,
        0x80, 0x35, 0xF8, 0xFF, 0xFF, 0xFF, 0x07,
        0xEB, 0xF5
    };
    unsigned char stack[8192] = { 0 };
    unsigned char scratch[8192 + sizeof(code)] = { 0 };
    SHEMU_CONTEXT ctx;
    SHEMU_GRAPH *graph = NULL;
    SHEMU_STATUS status;

    memset(&ctx, 0, sizeof(ctx));
    ctx.ArchType = SHEMU_ARCH_TYPE_X86;
    ctx.Shellcode = code;
    ctx.ShellcodeSize = sizeof(code);
    ctx.ShellcodeBase = 0x200000;
    ctx.Stack = stack;
    ctx.StackBase = 0x100000;
    ctx.StackSize = sizeof(stack);
    ctx.Intbuf = scratch;
    ctx.IntbufSize = sizeof(scratch);
    ctx.Arch.X86.Mode = ND_CODE_64;
    ctx.Arch.X86.Ring = 3;
    ctx.Arch.X86.Registers.RegRip = ctx.ShellcodeBase;
    ctx.Arch.X86.Registers.RegRsp = 0x101000;
    ctx.Arch.X86.Registers.RegFlags = 2;
    ctx.MaxInstructionsCount = 12;

    status = ShemuEmulateGraph(&ctx, &graph);

    printf("emulation status: %u, flags: 0x%llx\n", status, (unsigned long long)ctx.Flags);

    if (NULL == graph)
    {
        // The graph could not be recorded (out of memory); the emulation itself still ran.
        return 1;
    }

    // The graph owns its storage: the context and the input buffers are no longer needed, and can
    // be destroyed - the report remains fully readable.
    memset(code, 0, sizeof(code));
    memset(&ctx, 0, sizeof(ctx));

    printf("nodes: %llu, edges: %llu, executed: %llu\n",
           (unsigned long long)graph->NodeCount,
           (unsigned long long)graph->EdgeCount,
           (unsigned long long)graph->ExecutedCount);

    for (ND_SIZET i = 0; i < graph->NodeCount; i++)
    {
        const SHEMU_GRAPH_NODE *node = &graph->Nodes[i];

        printf("node #%llu: rip 0x%llx, bytes", (unsigned long long)i, (unsigned long long)node->Rip);
        for (ND_UINT32 b = 0; b < node->Length; b++)
        {
            printf(" %02X", node->Bytes[b]);
        }
        printf(", executed %llu time(s)\n", (unsigned long long)node->ExecCount);
    }

    for (ND_SIZET i = 0; i < graph->EdgeCount; i++)
    {
        const SHEMU_GRAPH_EDGE *edge = &graph->Edges[i];

        printf("edge: #%llu -> #%llu (%s), %llu time(s)\n",
               (unsigned long long)edge->From, (unsigned long long)edge->To,
               edge_kind_name(edge->Kind), (unsigned long long)edge->Count);
    }

    printf("termination: reason %d, status %u, address 0x%llx, next rip 0x%llx\n",
           (int)graph->Termination.Reason, graph->Termination.Status,
           (unsigned long long)graph->Termination.Address,
           (unsigned long long)graph->Termination.NextRip);

    // Release the report. ShemuGraphFree also accepts NULL, so error paths need no special care.
    ShemuGraphFree(graph);
    ShemuGraphFree(NULL);

    return 0;
}
