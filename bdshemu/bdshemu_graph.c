/*
 * Copyright (c) 2020 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bdshemu.h"
#include "include/bdshemu_common.h"

#include <stdlib.h>
#include <string.h>


//
// The graph recorder state. The public report (SHEMU_GRAPH) is the first member, so the pointer
// handed out to the caller also identifies the whole allocation; ShemuGraphFree releases it.
//
typedef struct _SHEMU_GRAPH_BUILDER
{
    // The report that will be handed out to the caller. Must be the first member.
    SHEMU_GRAPH         Graph;

    // Capacities of the Nodes and Edges arrays.
    ND_SIZET            NodeCap;
    ND_SIZET            EdgeCap;

    // Set if a recording allocation failed. Recording stops, but the emulation is not affected.
    ND_BOOL             Oom;

    // The dispatch currently in progress: the instruction version captured before execution.
    ND_BOOL             HasPending;
    ND_UINT64           PendingRip;
    ND_UINT32           PendingLength;
    ND_UINT8            PendingBytes[SHEMU_GRAPH_MAX_INSTR_BYTES];

    // The address of the most recently attempted dispatch.
    ND_BOOL             HasAttempt;
    ND_UINT64           AttemptRip;

    // The next RIP as left behind by the last committed instruction, its node index (~0 if none)
    // and the kind of transition its execution produced (which labels its outgoing edge).
    ND_UINT64           LastNextRip;
    ND_UINT64           LastNode;
    SHEMU_GRAPH_EDGE_KIND LastKind;

} SHEMU_GRAPH_BUILDER;


#define SHEMU_GRAPH_NO_NODE     (~(ND_UINT64)0)


//
// ShemuGraphClassify
//
static void
ShemuGraphClassify(
    ND_INS_CLASS Instruction,
    ND_BOOL Taken,
    SHEMU_GRAPH_TRANSFER *Transfer,
    SHEMU_GRAPH_EDGE_KIND *Kind
    )
//
// Maps an instruction to its transfer class, and to the kind of edge its execution produces.
// Taken is consulted only for conditional branches.
//
{
    switch (Instruction)
    {
    case ND_INS_Jcc:
    case ND_INS_JrCXZ:
    case ND_INS_LOOP:
    case ND_INS_LOOPZ:
    case ND_INS_LOOPNZ:
        *Transfer = SHEMU_GRAPH_TRANSFER_CONDITIONAL;
        *Kind = Taken ? SHEMU_GRAPH_EDGE_COND_TAKEN : SHEMU_GRAPH_EDGE_COND_NOT_TAKEN;
        break;

    case ND_INS_JMPNR:
    case ND_INS_JMPNI:
        *Transfer = SHEMU_GRAPH_TRANSFER_JUMP;
        *Kind = SHEMU_GRAPH_EDGE_JUMP;
        break;

    case ND_INS_CALLNR:
    case ND_INS_CALLNI:
        *Transfer = SHEMU_GRAPH_TRANSFER_CALL;
        *Kind = SHEMU_GRAPH_EDGE_CALL;
        break;

    case ND_INS_RETN:
        *Transfer = SHEMU_GRAPH_TRANSFER_RETURN;
        *Kind = SHEMU_GRAPH_EDGE_RETURN;
        break;

    default:
        *Transfer = SHEMU_GRAPH_TRANSFER_SEQUENTIAL;
        *Kind = SHEMU_GRAPH_EDGE_SEQUENTIAL;
        break;
    }
}


//
// ShemuGraphAttempt
//
void
ShemuGraphAttempt(
    SHEMU_CONTEXT *Context
    )
{
    SHEMU_GRAPH_BUILDER *builder = (SHEMU_GRAPH_BUILDER *)Context->GraphBuilder;

    if ((NULL == builder) || builder->Oom)
    {
        return;
    }

    builder->HasAttempt = ND_TRUE;
    builder->AttemptRip = Context->Arch.X86.Registers.RegRip;
    builder->HasPending = ND_FALSE;
}


//
// ShemuGraphPrepare
//
void
ShemuGraphPrepare(
    SHEMU_CONTEXT *Context
    )
{
    SHEMU_GRAPH_BUILDER *builder = (SHEMU_GRAPH_BUILDER *)Context->GraphBuilder;

    if ((NULL == builder) || builder->Oom)
    {
        return;
    }

    // Snapshot the instruction version that is about to execute. The bytes are copied now, so a
    // later self-modification of the shellcode cannot alter the node that will be created.
    builder->PendingRip = Context->Arch.X86.Registers.RegRip;
    builder->PendingLength = Context->Arch.X86.Instruction.Length;
    memcpy(builder->PendingBytes, Context->Arch.X86.Instruction.InstructionBytes,
           Context->Arch.X86.Instruction.Length);
    builder->HasPending = ND_TRUE;
}


//
// ShemuGraphCommit
//
void
ShemuGraphCommit(
    SHEMU_CONTEXT *Context,
    ND_BOOL Taken
    )
{
    SHEMU_GRAPH_BUILDER *builder = (SHEMU_GRAPH_BUILDER *)Context->GraphBuilder;
    SHEMU_GRAPH_TRANSFER transfer;
    SHEMU_GRAPH_EDGE_KIND kind;
    ND_UINT64 nodeIndex, i;

    if ((NULL == builder) || builder->Oom || !builder->HasPending)
    {
        return;
    }

    builder->HasPending = ND_FALSE;

    // The transfer class of the current instruction, and the kind of transition its execution
    // produced. The kind labels the edge that LEAVES this instruction, so it is only used when
    // the next instruction commits; the edge created below is labeled with the kind stashed by
    // the previously committed instruction.
    ShemuGraphClassify(Context->Arch.X86.Instruction.Instruction, Taken, &transfer, &kind);

    // Locate an existing node for this exact instruction version: same original RIP, same decoded
    // length and identical bytes. A modified instruction at the same address yields a new node.
    for (nodeIndex = 0; nodeIndex < builder->Graph.NodeCount; nodeIndex++)
    {
        SHEMU_GRAPH_NODE *node = &builder->Graph.Nodes[nodeIndex];

        if (node->Rip == builder->PendingRip &&
            node->Length == builder->PendingLength &&
            0 == memcmp(node->Bytes, builder->PendingBytes, builder->PendingLength))
        {
            break;
        }
    }

    if (nodeIndex == builder->Graph.NodeCount)
    {
        SHEMU_GRAPH_NODE *node;

        // No such version yet - create a new node.
        if (builder->Graph.NodeCount == builder->NodeCap)
        {
            ND_SIZET newCap = (0 == builder->NodeCap) ? 16 : builder->NodeCap * 2;
            SHEMU_GRAPH_NODE *newNodes = realloc(builder->Graph.Nodes, newCap * sizeof(*newNodes));

            if (NULL == newNodes)
            {
                builder->Oom = ND_TRUE;
                return;
            }

            builder->Graph.Nodes = newNodes;
            builder->NodeCap = newCap;
        }

        node = &builder->Graph.Nodes[builder->Graph.NodeCount++];
        node->Rip = builder->PendingRip;
        node->Length = builder->PendingLength;
        memset(node->Bytes, 0, sizeof(node->Bytes));
        memcpy(node->Bytes, builder->PendingBytes, builder->PendingLength);
        node->Transfer = transfer;
        node->ExecCount = 0;
    }

    builder->Graph.Nodes[nodeIndex].ExecCount++;
    builder->Graph.ExecutedCount++;

    // Link the previously committed instruction to this one, if any. The edge is labeled with the
    // transition kind produced by that previous instruction (stashed in LastKind).
    if (builder->LastNode != SHEMU_GRAPH_NO_NODE)
    {
        for (i = 0; i < builder->Graph.EdgeCount; i++)
        {
            SHEMU_GRAPH_EDGE *edge = &builder->Graph.Edges[i];

            if (edge->From == builder->LastNode && edge->To == nodeIndex && edge->Kind == builder->LastKind)
            {
                break;
            }
        }

        if (i == builder->Graph.EdgeCount)
        {
            SHEMU_GRAPH_EDGE *edge;

            if (builder->Graph.EdgeCount == builder->EdgeCap)
            {
                ND_SIZET newCap = (0 == builder->EdgeCap) ? 16 : builder->EdgeCap * 2;
                SHEMU_GRAPH_EDGE *newEdges = realloc(builder->Graph.Edges, newCap * sizeof(*newEdges));

                if (NULL == newEdges)
                {
                    builder->Oom = ND_TRUE;
                    return;
                }

                builder->Graph.Edges = newEdges;
                builder->EdgeCap = newCap;
            }

            edge = &builder->Graph.Edges[builder->Graph.EdgeCount++];
            edge->From = builder->LastNode;
            edge->To = nodeIndex;
            edge->Kind = builder->LastKind;
            edge->Count = 0;
        }

        builder->Graph.Edges[i].Count++;
    }

    builder->LastNode = nodeIndex;
    builder->LastKind = kind;
    builder->LastNextRip = Context->Arch.X86.Registers.RegRip;
}


//
// ShemuEmulateGraph
//
SHEMU_STATUS
ShemuEmulateGraph(
    SHEMU_CONTEXT *Context,
    SHEMU_GRAPH **Graph
    )
{
    SHEMU_GRAPH_BUILDER *builder;
    SHEMU_STATUS status;

    if ((NULL == Context) || (NULL == Graph))
    {
        return SHEMU_ABORT_INVALID_PARAMETER;
    }

    *Graph = NULL;

    builder = calloc(1, sizeof(*builder));
    if (NULL == builder)
    {
        // The graph cannot be recorded, but the emulation itself must behave exactly as usual.
        return ShemuEmulate(Context);
    }

    builder->LastNode = SHEMU_GRAPH_NO_NODE;
    builder->LastNextRip = Context->Arch.X86.Registers.RegRip;

    // Record the graph on top of the regular emulation: the executor, the context side-effects and
    // the returned status are exactly those of a plain ShemuEmulate call.
    Context->GraphBuilder = builder;
    status = ShemuEmulate(Context);
    Context->GraphBuilder = NULL;

    // Record how the emulation ended. No successor is fabricated for a dispatch that did not
    // complete; the termination record simply describes where and why the emulation stopped.
    if (SHEMU_SUCCESS == status)
    {
        // The only way the emulation completes successfully is by reaching the instruction limit.
        builder->Graph.Termination.Reason = SHEMU_GRAPH_STOP_LIMIT;
        builder->Graph.Termination.Address = builder->LastNextRip;
    }
    else
    {
        builder->Graph.Termination.Reason = SHEMU_GRAPH_STOP_ABORT;
        builder->Graph.Termination.Address = builder->HasAttempt ? builder->AttemptRip
                                                                 : builder->LastNextRip;
    }
    builder->Graph.Termination.Status = status;
    builder->Graph.Termination.NextRip = builder->LastNextRip;

    if (builder->Oom)
    {
        // Recording ran out of memory midway; do not hand out a partial report. The emulation
        // itself completed normally, so its status is still returned.
        ShemuGraphFree(&builder->Graph);
        return status;
    }

    *Graph = &builder->Graph;

    return status;
}


//
// ShemuGraphFree
//
void
ShemuGraphFree(
    SHEMU_GRAPH *Graph
    )
{
    if (NULL == Graph)
    {
        return;
    }

    free(Graph->Nodes);
    free(Graph->Edges);

    // The report is the first member of the builder allocation, so this releases the whole recorder.
    free(Graph);
}
