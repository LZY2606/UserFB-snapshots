//
// Dynamic instruction graph tests for ShemuEmulateGraph / ShemuGraphFree.
//
// All programs are hand-assembled short byte sequences; no real input machine code is executed.
//
#include "support/check.h"
#include "support/init.h"
#include <string.h>

#define BASE    0x200000ULL


//
// find_node - locate a node by original RIP and exact instruction bytes.
//
static const SHEMU_GRAPH_NODE *
find_node(
    const SHEMU_GRAPH *Graph,
    unsigned long long Rip,
    const unsigned char *Bytes,
    unsigned int Length
    )
{
    for (size_t i = 0; i < Graph->NodeCount; i++)
    {
        if (Graph->Nodes[i].Rip == Rip &&
            Graph->Nodes[i].Length == Length &&
            0 == memcmp(Graph->Nodes[i].Bytes, Bytes, Length))
        {
            return &Graph->Nodes[i];
        }
    }

    return NULL;
}


//
// find_edge - locate an edge by source node, destination node and kind.
//
static const SHEMU_GRAPH_EDGE *
find_edge(
    const SHEMU_GRAPH *Graph,
    const SHEMU_GRAPH_NODE *From,
    const SHEMU_GRAPH_NODE *To,
    SHEMU_GRAPH_EDGE_KIND Kind
    )
{
    size_t from = (size_t)(From - Graph->Nodes);
    size_t to = (size_t)(To - Graph->Nodes);

    for (size_t i = 0; i < Graph->EdgeCount; i++)
    {
        if (Graph->Edges[i].From == from &&
            Graph->Edges[i].To == to &&
            Graph->Edges[i].Kind == Kind)
        {
            return &Graph->Edges[i];
        }
    }

    return NULL;
}


//
// count_edges_from - count the edges leaving a node, regardless of kind.
//
static size_t
count_edges_from(
    const SHEMU_GRAPH *Graph,
    const SHEMU_GRAPH_NODE *From
    )
{
    size_t from = (size_t)(From - Graph->Nodes);
    size_t count = 0;

    for (size_t i = 0; i < Graph->EdgeCount; i++)
    {
        if (Graph->Edges[i].From == from)
        {
            count++;
        }
    }

    return count;
}


//
// case_selfmod_versions - the code toggles its own immediate, so the same address executes two
// different instruction versions; restoring the old bytes must merge back into the first node.
//
static void
case_selfmod_versions(
    void
    )
{
    // 0x200000: mov al, 0                  B0 00            (immediate toggled between 0 and 7)
    // 0x200002: xor byte [rip - 8], 7      80 35 F8 FF FF FF 07
    // 0x200009: jmp -11                    EB F5
    unsigned char code[] =
    {
        0xB0, 0x00,
        0x80, 0x35, 0xF8, 0xFF, 0xFF, 0xFF, 0x07,
        0xEB, 0xF5
    };
    static const unsigned char mov0[] = { 0xB0, 0x00 };
    static const unsigned char mov7[] = { 0xB0, 0x07 };
    static const unsigned char xorb[] = { 0x80, 0x35, 0xF8, 0xFF, 0xFF, 0xFF, 0x07 };
    static const unsigned char jmpb[] = { 0xEB, 0xF5 };
    unsigned char stack[8192] = { 0 }, scratch[8192 + sizeof(code)] = { 0 };
    SHEMU_CONTEXT c;
    SHEMU_GRAPH *g = NULL;
    SHEMU_STATUS st;
    const SHEMU_GRAPH_NODE *mov0Node, *mov7Node, *xorNode, *jmpNode;
    const SHEMU_GRAPH_EDGE *e;

    init_context(&c, code, sizeof(code), stack, scratch);
    c.MaxInstructionsCount = 12;

    st = ShemuEmulateGraph(&c, &g);

    TASK_CHECK(st == SHEMU_SUCCESS);
    TASK_CHECK(g != NULL);

    // 12 successful dispatches: mov0, xor, jmp, mov7, xor, jmp, mov0, xor, jmp, mov7, xor, jmp.
    TASK_CHECK(g->ExecutedCount == 12);

    // Two distinct versions at the same address, plus the xor and the jmp.
    TASK_CHECK(g->NodeCount == 4);

    mov0Node = find_node(g, BASE + 0, mov0, sizeof(mov0));
    mov7Node = find_node(g, BASE + 0, mov7, sizeof(mov7));
    xorNode = find_node(g, BASE + 2, xorb, sizeof(xorb));
    jmpNode = find_node(g, BASE + 9, jmpb, sizeof(jmpb));
    TASK_CHECK(mov0Node != NULL);
    TASK_CHECK(mov7Node != NULL);
    TASK_CHECK(xorNode != NULL);
    TASK_CHECK(jmpNode != NULL);

    // The two versions share the address and the length, but not the bytes: they must not merge.
    TASK_CHECK(mov0Node != mov7Node);
    TASK_CHECK(mov0Node->Length == 2 && mov7Node->Length == 2);

    // mov al,0 ran twice (the restored bytes merged back into the original node), mov al,7 twice.
    TASK_CHECK(mov0Node->ExecCount == 2);
    TASK_CHECK(mov7Node->ExecCount == 2);
    TASK_CHECK(xorNode->ExecCount == 4);
    TASK_CHECK(jmpNode->ExecCount == 4);

    // Transfer classes.
    TASK_CHECK(mov0Node->Transfer == SHEMU_GRAPH_TRANSFER_SEQUENTIAL);
    TASK_CHECK(jmpNode->Transfer == SHEMU_GRAPH_TRANSFER_JUMP);

    // Edges, with counts.
    e = find_edge(g, mov0Node, xorNode, SHEMU_GRAPH_EDGE_SEQUENTIAL);
    TASK_CHECK(e != NULL && e->Count == 2);
    e = find_edge(g, mov7Node, xorNode, SHEMU_GRAPH_EDGE_SEQUENTIAL);
    TASK_CHECK(e != NULL && e->Count == 2);
    e = find_edge(g, xorNode, jmpNode, SHEMU_GRAPH_EDGE_SEQUENTIAL);
    TASK_CHECK(e != NULL && e->Count == 4);
    e = find_edge(g, jmpNode, mov0Node, SHEMU_GRAPH_EDGE_JUMP);
    // mov al,0 ran twice, but the first run had no predecessor: only one jmp->mov0 transition.
    TASK_CHECK(e != NULL && e->Count == 1);
    e = find_edge(g, jmpNode, mov7Node, SHEMU_GRAPH_EDGE_JUMP);
    TASK_CHECK(e != NULL && e->Count == 2);
    TASK_CHECK(g->EdgeCount == 5);

    // The self-write was executed, so the detection flag must be set.
    TASK_CHECK(0 != (c.Flags & SHEMU_FLAG_WRITE_SELF));

    // The run stopped normally, by reaching the instruction limit.
    TASK_CHECK(g->Termination.Reason == SHEMU_GRAPH_STOP_LIMIT);
    TASK_CHECK(g->Termination.Status == SHEMU_SUCCESS);
    TASK_CHECK(g->Termination.NextRip == BASE + 0);
    TASK_CHECK(g->Termination.Address == BASE + 0);

    // The report owns its storage: destroy the inputs, the graph must remain readable.
    memset(code, 0xCC, sizeof(code));
    memset(&c, 0, sizeof(c));
    TASK_CHECK(mov0Node->Bytes[0] == 0xB0 && mov0Node->Bytes[1] == 0x00);
    TASK_CHECK(mov7Node->Bytes[0] == 0xB0 && mov7Node->Bytes[1] == 0x07);
    TASK_CHECK(g->NodeCount == 4 && g->ExecutedCount == 12);

    ShemuGraphFree(g);

    // Freeing an empty report must be accepted.
    ShemuGraphFree(NULL);
}


//
// case_cond_branch_edges - a conditional branch to the very next address is still a taken
// conditional edge; the kind must reflect the dynamic outcome, not the address layout.
//
static void
case_cond_branch_edges(
    void
    )
{
    // Taken variant: xor sets ZF, so jz +0 jumps to the next sequential address.
    // 0x200000: xor eax, eax     31 C0
    // 0x200002: jz +0            74 00    (target is 0x200004, the next instruction)
    // 0x200004: nop              90
    // 0x200005: nop              90
    unsigned char codeTaken[] = { 0x31, 0xC0, 0x74, 0x00, 0x90, 0x90 };
    // Not-taken variant: jnz +0 with ZF set falls through to the next sequential address.
    unsigned char codeFall[] = { 0x31, 0xC0, 0x75, 0x00, 0x90, 0x90 };
    static const unsigned char xorb[] = { 0x31, 0xC0 };
    static const unsigned char jzb[] = { 0x74, 0x00 };
    static const unsigned char jnzb[] = { 0x75, 0x00 };
    static const unsigned char nopb[] = { 0x90 };
    unsigned char stack[8192] = { 0 }, scratch[8192 + 16] = { 0 };
    SHEMU_CONTEXT c;
    SHEMU_GRAPH *g = NULL;
    const SHEMU_GRAPH_NODE *xorNode, *jccNode, *nop4Node, *nop5Node;
    const SHEMU_GRAPH_EDGE *e;

    // Taken conditional to the next address.
    init_context(&c, codeTaken, sizeof(codeTaken), stack, scratch);
    c.MaxInstructionsCount = 4;
    TASK_CHECK(ShemuEmulateGraph(&c, &g) == SHEMU_SUCCESS);
    TASK_CHECK(g != NULL);
    TASK_CHECK(g->NodeCount == 4);
    TASK_CHECK(g->ExecutedCount == 4);

    xorNode = find_node(g, BASE + 0, xorb, sizeof(xorb));
    jccNode = find_node(g, BASE + 2, jzb, sizeof(jzb));
    nop4Node = find_node(g, BASE + 4, nopb, sizeof(nopb));
    nop5Node = find_node(g, BASE + 5, nopb, sizeof(nopb));
    TASK_CHECK(xorNode != NULL && jccNode != NULL && nop4Node != NULL && nop5Node != NULL);
    TASK_CHECK(jccNode->Transfer == SHEMU_GRAPH_TRANSFER_CONDITIONAL);

    // The branch target equals the fall-through address, yet the edge must be cond-taken.
    e = find_edge(g, jccNode, nop4Node, SHEMU_GRAPH_EDGE_COND_TAKEN);
    TASK_CHECK(e != NULL && e->Count == 1);
    TASK_CHECK(find_edge(g, jccNode, nop4Node, SHEMU_GRAPH_EDGE_SEQUENTIAL) == NULL);
    TASK_CHECK(find_edge(g, jccNode, nop4Node, SHEMU_GRAPH_EDGE_COND_NOT_TAKEN) == NULL);
    TASK_CHECK(count_edges_from(g, jccNode) == 1);

    e = find_edge(g, xorNode, jccNode, SHEMU_GRAPH_EDGE_SEQUENTIAL);
    TASK_CHECK(e != NULL && e->Count == 1);
    e = find_edge(g, nop4Node, nop5Node, SHEMU_GRAPH_EDGE_SEQUENTIAL);
    TASK_CHECK(e != NULL && e->Count == 1);

    TASK_CHECK(g->Termination.Reason == SHEMU_GRAPH_STOP_LIMIT);
    ShemuGraphFree(g);
    g = NULL;

    // Not-taken conditional.
    memset(stack, 0, sizeof(stack));
    memset(scratch, 0, sizeof(scratch));
    init_context(&c, codeFall, sizeof(codeFall), stack, scratch);
    c.MaxInstructionsCount = 4;
    TASK_CHECK(ShemuEmulateGraph(&c, &g) == SHEMU_SUCCESS);
    TASK_CHECK(g != NULL);
    TASK_CHECK(g->NodeCount == 4);

    jccNode = find_node(g, BASE + 2, jnzb, sizeof(jnzb));
    nop4Node = find_node(g, BASE + 4, nopb, sizeof(nopb));
    TASK_CHECK(jccNode != NULL && nop4Node != NULL);
    TASK_CHECK(jccNode->Transfer == SHEMU_GRAPH_TRANSFER_CONDITIONAL);

    e = find_edge(g, jccNode, nop4Node, SHEMU_GRAPH_EDGE_COND_NOT_TAKEN);
    TASK_CHECK(e != NULL && e->Count == 1);
    TASK_CHECK(find_edge(g, jccNode, nop4Node, SHEMU_GRAPH_EDGE_COND_TAKEN) == NULL);
    TASK_CHECK(count_edges_from(g, jccNode) == 1);

    ShemuGraphFree(g);
}


//
// case_call_ret_edges - near call and near return produce their own edge kinds, and the regular
// side-effects (return address pushed on the stack) are preserved.
//
static void
case_call_ret_edges(
    void
    )
{
    // 0x200000: call +2          E8 02 00 00 00    (target 0x200007, pushes 0x200005)
    // 0x200005: nop              90
    // 0x200006: nop              90
    // 0x200007: ret              C3
    unsigned char code[] = { 0xE8, 0x02, 0x00, 0x00, 0x00, 0x90, 0x90, 0xC3 };
    static const unsigned char callb[] = { 0xE8, 0x02, 0x00, 0x00, 0x00 };
    static const unsigned char nopb[] = { 0x90 };
    static const unsigned char retb[] = { 0xC3 };
    unsigned char stack[8192] = { 0 }, scratch[8192 + sizeof(code)] = { 0 };
    SHEMU_CONTEXT c;
    SHEMU_GRAPH *g = NULL;
    const SHEMU_GRAPH_NODE *callNode, *retNode, *nop5Node, *nop6Node;
    const SHEMU_GRAPH_EDGE *e;

    init_context(&c, code, sizeof(code), stack, scratch);
    c.MaxInstructionsCount = 4;

    TASK_CHECK(ShemuEmulateGraph(&c, &g) == SHEMU_SUCCESS);
    TASK_CHECK(g != NULL);

    // call, ret, nop, nop - 4 successful dispatches.
    TASK_CHECK(g->NodeCount == 4);
    TASK_CHECK(g->ExecutedCount == 4);

    callNode = find_node(g, BASE + 0, callb, sizeof(callb));
    nop5Node = find_node(g, BASE + 5, nopb, sizeof(nopb));
    nop6Node = find_node(g, BASE + 6, nopb, sizeof(nopb));
    retNode = find_node(g, BASE + 7, retb, sizeof(retb));
    TASK_CHECK(callNode != NULL && nop5Node != NULL && nop6Node != NULL && retNode != NULL);
    TASK_CHECK(callNode->Transfer == SHEMU_GRAPH_TRANSFER_CALL);
    TASK_CHECK(retNode->Transfer == SHEMU_GRAPH_TRANSFER_RETURN);

    e = find_edge(g, callNode, retNode, SHEMU_GRAPH_EDGE_CALL);
    TASK_CHECK(e != NULL && e->Count == 1);
    e = find_edge(g, retNode, nop5Node, SHEMU_GRAPH_EDGE_RETURN);
    TASK_CHECK(e != NULL && e->Count == 1);
    e = find_edge(g, nop5Node, nop6Node, SHEMU_GRAPH_EDGE_SEQUENTIAL);
    TASK_CHECK(e != NULL && e->Count == 1);
    TASK_CHECK(g->EdgeCount == 3);

    // The return address 0x200005 was pushed at RSP - 8 (0x100FF8) and is still there.
    TASK_CHECK(stack[0xFF8] == 0x05 && stack[0xFF9] == 0x00 && stack[0xFFA] == 0x20);
    TASK_CHECK(c.Arch.X86.Registers.RegRsp == 0x101000);

    TASK_CHECK(g->Termination.Reason == SHEMU_GRAPH_STOP_LIMIT);
    TASK_CHECK(g->Termination.Status == SHEMU_SUCCESS);

    ShemuGraphFree(g);
}


//
// case_abort_prefix - a denied memory access aborts the emulation; the successful prefix of the
// graph remains available, and the termination record describes the failed dispatch.
//
static void
case_abort_prefix(
    void
    )
{
    // 0x200000: xor eax, eax         31 C0
    // 0x200002: mov rbx, [rax]       48 8B 18    (external load, no callback: access denied)
    unsigned char code[] = { 0x31, 0xC0, 0x48, 0x8B, 0x18 };
    static const unsigned char xorb[] = { 0x31, 0xC0 };
    unsigned char stack[8192] = { 0 }, scratch[8192 + sizeof(code)] = { 0 };
    SHEMU_CONTEXT c;
    SHEMU_GRAPH *g = NULL;
    SHEMU_STATUS st;
    const SHEMU_GRAPH_NODE *xorNode;

    init_context(&c, code, sizeof(code), stack, scratch);
    c.MaxInstructionsCount = 16;

    st = ShemuEmulateGraph(&c, &g);

    TASK_CHECK(st == SHEMU_ABORT_GLA_OUTSIDE);
    TASK_CHECK(g != NULL);

    // Only the xor completed; the failed dispatch created neither a node nor an edge.
    TASK_CHECK(g->NodeCount == 1);
    TASK_CHECK(g->EdgeCount == 0);
    TASK_CHECK(g->ExecutedCount == 1);

    xorNode = find_node(g, BASE + 0, xorb, sizeof(xorb));
    TASK_CHECK(xorNode != NULL);
    TASK_CHECK(xorNode->ExecCount == 1);
    TASK_CHECK(xorNode->Transfer == SHEMU_GRAPH_TRANSFER_SEQUENTIAL);

    // The termination record carries the attempted address, the original status and the next RIP
    // as left by the last successful instruction.
    TASK_CHECK(g->Termination.Reason == SHEMU_GRAPH_STOP_ABORT);
    TASK_CHECK(g->Termination.Status == SHEMU_ABORT_GLA_OUTSIDE);
    TASK_CHECK(g->Termination.Address == BASE + 2);
    TASK_CHECK(g->Termination.NextRip == BASE + 2);

    // The aborted instruction did not corrupt the register state.
    TASK_CHECK(c.Arch.X86.Registers.RegRbx == 0);

    ShemuGraphFree(g);
}


//
// run_prog - run a program either plainly or with graph recording, and snapshot the observable
// emulation state for comparison.
//
typedef struct _SNAPSHOT
{
    SHEMU_STATUS        Status;
    SHEMU_X86_GPR_REGS  Registers;
    SHEMU_X86_SEG_REGS  Segments;
    unsigned long long  Flags;
    unsigned long long  InstructionsCount;
    unsigned long long  UniqueCount;
    unsigned long long  NopCount;
    unsigned long long  NullCount;
    unsigned long long  ExtMemAccess;
    unsigned char       Code[64];
    unsigned char       Stack[8192];
} SNAPSHOT;

static void
run_prog(
    const unsigned char *Code,
    size_t CodeSize,
    unsigned long long MaxInstructions,
    int UseGraph,
    SNAPSHOT *Out,
    SHEMU_GRAPH **Graph
    )
{
    SHEMU_CONTEXT c;
    unsigned char codeBuf[64], stack[8192], scratch[8192 + 64];

    memset(codeBuf, 0, sizeof(codeBuf));
    memcpy(codeBuf, Code, CodeSize);
    memset(stack, 0, sizeof(stack));
    memset(scratch, 0, sizeof(scratch));

    init_context(&c, codeBuf, CodeSize, stack, scratch);
    c.MaxInstructionsCount = MaxInstructions;

    *Graph = NULL;
    Out->Status = UseGraph ? ShemuEmulateGraph(&c, Graph) : ShemuEmulate(&c);
    Out->Registers = c.Arch.X86.Registers;
    Out->Segments = c.Arch.X86.Segments;
    Out->Flags = c.Flags;
    Out->InstructionsCount = c.InstructionsCount;
    Out->UniqueCount = c.UniqueCount;
    Out->NopCount = c.NopCount;
    Out->NullCount = c.NullCount;
    Out->ExtMemAccess = c.ExtMemAccess;
    memcpy(Out->Code, codeBuf, sizeof(codeBuf));
    memcpy(Out->Stack, stack, sizeof(stack));
}


//
// case_graph_matches_plain - recording the graph must not change the emulation: registers, memory,
// detection flags, counters and the returned status are identical with and without the graph.
//
static void
case_graph_matches_plain(
    void
    )
{
    static const unsigned char selfmod[] =
    {
        0xB0, 0x00,
        0x80, 0x35, 0xF8, 0xFF, 0xFF, 0xFF, 0x07,
        0xEB, 0xF5
    };
    static const unsigned char callret[] =
    {
        0xE8, 0x02, 0x00, 0x00, 0x00, 0x90, 0x90, 0xC3
    };
    static const unsigned char denied[] =
    {
        0x31, 0xC0, 0x48, 0x8B, 0x18
    };
    static const struct
    {
        const unsigned char *Code;
        size_t CodeSize;
        unsigned long long MaxInstructions;
    } programs[] =
    {
        { selfmod, sizeof(selfmod), 12 },
        { callret, sizeof(callret), 4 },
        { denied, sizeof(denied), 16 },
    };

    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++)
    {
        SNAPSHOT plain, graphed;
        SHEMU_GRAPH *g = NULL;

        run_prog(programs[i].Code, programs[i].CodeSize, programs[i].MaxInstructions, 0, &plain, &g);
        TASK_CHECK(g == NULL);

        run_prog(programs[i].Code, programs[i].CodeSize, programs[i].MaxInstructions, 1, &graphed, &g);
        TASK_CHECK(g != NULL);

        TASK_CHECK(plain.Status == graphed.Status);
        TASK_CHECK(0 == memcmp(&plain.Registers, &graphed.Registers, sizeof(plain.Registers)));
        TASK_CHECK(0 == memcmp(&plain.Segments, &graphed.Segments, sizeof(plain.Segments)));
        TASK_CHECK(plain.Flags == graphed.Flags);
        TASK_CHECK(plain.InstructionsCount == graphed.InstructionsCount);
        TASK_CHECK(plain.UniqueCount == graphed.UniqueCount);
        TASK_CHECK(plain.NopCount == graphed.NopCount);
        TASK_CHECK(plain.NullCount == graphed.NullCount);
        TASK_CHECK(plain.ExtMemAccess == graphed.ExtMemAccess);
        TASK_CHECK(0 == memcmp(plain.Code, graphed.Code, sizeof(plain.Code)));
        TASK_CHECK(0 == memcmp(plain.Stack, graphed.Stack, sizeof(plain.Stack)));

        // The graph's own execution counter agrees with the number of successful dispatches.
        if (graphed.Status == SHEMU_SUCCESS)
        {
            TASK_CHECK(g->ExecutedCount == programs[i].MaxInstructions);
        }

        ShemuGraphFree(g);
    }
}


//
// case_rep_single_node - the iterations of a repeated string instruction are a single outer
// dispatch: one node, one execution, one edge.
//
static void
case_rep_single_node(
    void
    )
{
    // 0x200000: mov ecx, 3     B9 03 00 00 00
    // 0x200005: mov rsi, rsp   48 89 E6
    // 0x200008: mov rdi, rsp   48 89 E7
    // 0x20000B: rep movsb      F3 A4          (3 internal iterations, 1 dispatch)
    // 0x20000D: nop            90
    unsigned char code[] =
    {
        0xB9, 0x03, 0x00, 0x00, 0x00,
        0x48, 0x89, 0xE6,
        0x48, 0x89, 0xE7,
        0xF3, 0xA4,
        0x90
    };
    static const unsigned char movsb[] = { 0xF3, 0xA4 };
    static const unsigned char nopb[] = { 0x90 };
    unsigned char stack[8192] = { 0 }, scratch[8192 + sizeof(code)] = { 0 };
    SHEMU_CONTEXT c;
    SHEMU_GRAPH *g = NULL;
    const SHEMU_GRAPH_NODE *repNode, *nopNode;
    const SHEMU_GRAPH_EDGE *e;

    init_context(&c, code, sizeof(code), stack, scratch);
    // The REP iterations consume the regular instruction budget: 3 movs + 3 iterations + 1 nop.
    c.MaxInstructionsCount = 8;

    TASK_CHECK(ShemuEmulateGraph(&c, &g) == SHEMU_SUCCESS);
    TASK_CHECK(g != NULL);

    // 5 outer dispatches, even though rep movsb iterated 3 times internally. Note how this is
    // not derived from the context InstructionsCount, which the REP iterations inflated.
    TASK_CHECK(g->NodeCount == 5);
    TASK_CHECK(g->ExecutedCount == 5);
    TASK_CHECK(c.InstructionsCount > g->ExecutedCount);

    repNode = find_node(g, BASE + 11, movsb, sizeof(movsb));
    nopNode = find_node(g, BASE + 13, nopb, sizeof(nopb));
    TASK_CHECK(repNode != NULL && nopNode != NULL);
    TASK_CHECK(repNode->ExecCount == 1);

    e = find_edge(g, repNode, nopNode, SHEMU_GRAPH_EDGE_SEQUENTIAL);
    TASK_CHECK(e != NULL && e->Count == 1);

    // The string operation itself ran: RCX was consumed.
    TASK_CHECK(c.Arch.X86.Registers.RegRcx == 0);

    ShemuGraphFree(g);
}


//
// case_api_robustness - invalid parameters and empty reports are handled gracefully.
//
static void
case_api_robustness(
    void
    )
{
    unsigned char code[] = { 0x90 };
    unsigned char stack[8192] = { 0 }, scratch[8192 + sizeof(code)] = { 0 };
    SHEMU_CONTEXT c;
    SHEMU_GRAPH *g = NULL;

    TASK_CHECK(ShemuEmulateGraph(NULL, &g) == SHEMU_ABORT_INVALID_PARAMETER);
    TASK_CHECK(g == NULL);

    init_context(&c, code, sizeof(code), stack, scratch);
    c.MaxInstructionsCount = 1;
    TASK_CHECK(ShemuEmulateGraph(&c, NULL) == SHEMU_ABORT_INVALID_PARAMETER);

    // A NULL report is accepted by the free function.
    ShemuGraphFree(NULL);
}


const TaskCase task_cases[] =
{
    { "selfmod_immediate_versions", case_selfmod_versions },
    { "cond_branch_edge_kinds", case_cond_branch_edges },
    { "near_call_ret_edges", case_call_ret_edges },
    { "abort_keeps_prefix_graph", case_abort_prefix },
    { "graph_matches_plain_emulation", case_graph_matches_plain },
    { "rep_string_single_node", case_rep_single_node },
    { "api_robustness", case_api_robustness },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
