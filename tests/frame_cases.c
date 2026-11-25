/*
 * frame_cases.c - content checks for libudis86 frame analysis.
 *
 * This file intentionally does not define main(); task_support/driver.c owns
 * the test driver and registers the cases below.
 */
#include "task_support/check.h"

#include "libudis86/frame.h"
#include "udis86.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static const ud_frame_insn_t *
insn_at(const ud_frame_report_t *r, size_t offset)
{
    size_t i;
    if (r == NULL)
        return NULL;
    for (i = 0; i < r->insn_count; ++i) {
        if (r->insns[i].offset == offset)
            return &r->insns[i];
    }
    return NULL;
}

static const ud_frame_access_t *
access_at(const ud_frame_report_t *r, size_t offset, int kind)
{
    size_t i;
    if (r == NULL)
        return NULL;
    for (i = 0; i < r->access_count; ++i) {
        if (r->accesses[i].insn_offset == offset &&
            r->accesses[i].kind == kind)
            return &r->accesses[i];
    }
    return NULL;
}

#define CHECK_REL(v, d) \
    TASK_CHECK((v).kind == UD_FRAME_RELATIVE && (v).offset == (int64_t)(d))
#define CHECK_UNKNOWN(v) TASK_CHECK((v).kind == UD_FRAME_UNKNOWN)
#define CHECK_UNREACHED(v) TASK_CHECK((v).kind == UD_FRAME_UNREACHED)

/* ------------------------------------------------------------------------- */
/* normalize                                                                  */
/* ------------------------------------------------------------------------- */

static int
test_unreachable_block_is_silent(void)
{
    /* 90 nop; 0f 0b ud2 in an unreachable block. */
    static const uint8_t code[] = { 0x90, 0x0f, 0x0b };
    static size_t self[] = { 1 };
    ud_frame_block_t blocks[2];
    ud_frame_graph_t graph;
    ud_frame_error_t error;
    ud_frame_report_t *r;

    blocks[0].begin = 0; blocks[0].end = 1;
    blocks[0].successors = NULL; blocks[0].successor_count = 0;
    blocks[1].begin = 1; blocks[1].end = 3;
    blocks[1].successors = self; blocks[1].successor_count = 1;
    graph.blocks = blocks;
    graph.block_count = 2;
    graph.entry = 0;

    r = ud_frame_analyze(code, sizeof code, &graph, &error);
    TASK_CHECK(r != NULL);
    if (r == NULL)
        return 1;
    TASK_CHECK(r->insn_count == 1);
    TASK_CHECK(r->insns[0].offset == 0);
    TASK_CHECK(r->block_count == 2);
    TASK_CHECK(r->blocks[0].begin == 0 && r->blocks[0].reachable == 1);
    TASK_CHECK(r->blocks[1].begin == 1 && r->blocks[1].reachable == 0);
    CHECK_UNREACHED(r->blocks[1].entry_state.rsp);
    CHECK_UNREACHED(r->blocks[1].entry_state.rbp);
    ud_frame_report_free(r);
    return 0;
}

static int
test_bad_range_edge_and_decode_locations(void)
{
    ud_frame_error_t error;
    ud_frame_report_t *r;

    {
        static const uint8_t code[] = { 0x90 };
        ud_frame_block_t b = { 0, 2, NULL, 0 };
        ud_frame_graph_t g = { &b, 1, 0 };
        r = ud_frame_analyze(code, sizeof code, &g, &error);
        TASK_CHECK(r == NULL);
        TASK_CHECK(error.code == UD_FRAME_ERR_RANGE);
        TASK_CHECK(error.block == 0);
        TASK_CHECK(error.offset == 2);
    }

    {
        static const uint8_t code[] = { 0x90 };
        static size_t bad_succ[] = { 7 };
        ud_frame_block_t b = { 0, 1, bad_succ, 1 };
        ud_frame_graph_t g = { &b, 1, 0 };
        r = ud_frame_analyze(code, sizeof code, &g, &error);
        TASK_CHECK(r == NULL);
        TASK_CHECK(error.code == UD_FRAME_ERR_EDGE);
        TASK_CHECK(error.block == 0);
        TASK_CHECK(error.offset == 0);
    }

    {
        /* Two nops cannot overlap. */
        static const uint8_t code[] = { 0x90, 0x90 };
        ud_frame_block_t b[2] = {
            { 0, 2, NULL, 0 },
            { 0, 1, NULL, 0 }
        };
        ud_frame_graph_t g = { b, 2, 0 };
        r = ud_frame_analyze(code, sizeof code, &g, &error);
        TASK_CHECK(r == NULL);
        TASK_CHECK(error.code == UD_FRAME_ERR_RANGE);
        TASK_CHECK(error.block != UD_FRAME_NO_BLOCK);
    }

    {
        /* nop, then a truncated REX prefix at offset 1. */
        static const uint8_t code[] = { 0x90, 0x48 };
        ud_frame_block_t b = { 0, 2, NULL, 0 };
        ud_frame_graph_t g = { &b, 1, 0 };
        r = ud_frame_analyze(code, sizeof code, &g, &error);
        TASK_CHECK(r == NULL);
        TASK_CHECK(error.code == UD_FRAME_ERR_DECODE);
        TASK_CHECK(error.block == 0);
        TASK_CHECK(error.offset == 1);
    }

    {
        static const uint8_t code[] = { 0x0f, 0x0b };
        ud_frame_block_t b = { 0, 2, NULL, 0 };
        ud_frame_graph_t g = { &b, 1, 0 };
        r = ud_frame_analyze(code, sizeof code, &g, &error);
        TASK_CHECK(r == NULL);
        TASK_CHECK(error.code == UD_FRAME_ERR_UNSUPPORTED);
        TASK_CHECK(error.block == 0);
        TASK_CHECK(error.offset == 0);
        TASK_CHECK(strstr(error.message, "unsupported") != NULL);
    }

    return 0;
}

static int
test_failure_reuse_and_owned_input(void)
{
    static const uint8_t prologue[] = {
        0x55,                         /* push rbp */
        0x48, 0x89, 0xe5,             /* mov rbp, rsp */
        0x48, 0x83, 0xec, 0x20,       /* sub rsp, 0x20 */
        0xc9,                         /* leave */
        0xc3                          /* ret */
    };
    static size_t bad_succ[] = { 11 };
    uint8_t *owned;
    ud_frame_block_t *owned_block;
    ud_frame_graph_t *owned_graph;
    ud_frame_block_t bad_block = { 0, 1, bad_succ, 1 };
    ud_frame_graph_t bad_graph = { &bad_block, 1, 0 };
    ud_frame_block_t good_block = { 0, sizeof prologue, NULL, 0 };
    ud_frame_graph_t good_graph = { &good_block, 1, 0 };
    ud_frame_error_t error;
    ud_frame_report_t *r;
    ud_t decoder;
    unsigned int decoded_length;
    const ud_frame_insn_t *push_insn;

    TASK_CHECK(ud_frame_analyze(prologue, 1, &bad_graph, &error) == NULL);
    TASK_CHECK(error.code == UD_FRAME_ERR_EDGE);

    /* The ordinary udis86 ABI is still usable after a frame failure. */
    ud_init(&decoder);
    ud_set_mode(&decoder, 64);
    ud_set_input_buffer(&decoder, prologue, sizeof prologue);
    decoded_length = ud_disassemble(&decoder);
    TASK_CHECK(decoded_length == 1);
    TASK_CHECK(ud_insn_mnemonic(&decoder) == UD_Ipush);

    owned = malloc(sizeof prologue);
    owned_block = malloc(sizeof *owned_block);
    owned_graph = malloc(sizeof *owned_graph);
    TASK_CHECK(owned != NULL && owned_block != NULL && owned_graph != NULL);
    memcpy(owned, prologue, sizeof prologue);
    owned_block->begin = 0;
    owned_block->end = sizeof prologue;
    owned_block->successors = NULL;
    owned_block->successor_count = 0;
    owned_graph->blocks = owned_block;
    owned_graph->block_count = 1;
    owned_graph->entry = 0;

    r = ud_frame_analyze(owned, sizeof prologue, owned_graph, &error);

    /* The report is value-only; releasing all caller-owned inputs is safe. */
    free(owned);
    free(owned_block);
    free(owned_graph);
    TASK_CHECK(r != NULL);
    TASK_CHECK(error.code == UD_FRAME_OK);
    if (r == NULL)
        return 1;

    push_insn = insn_at(r, 0);
    TASK_CHECK(push_insn != NULL);
    if (push_insn != NULL)
        CHECK_REL(push_insn->before.rsp, 0);
    TASK_CHECK(r->insn_count == 5);
    ud_frame_report_free(r);

    /* A later successful call shows API reuse after failure. */
    r = ud_frame_analyze(prologue, sizeof prologue, &good_graph, &error);
    TASK_CHECK(r != NULL);
    ud_frame_report_free(r);
    return 0;
}

/* Diamond byte stream is shared by merge and stable-order checks. */
static const uint8_t diamond_code[] = {
    0x48, 0x89, 0xe5,             /* 0: mov rbp, rsp */
    0x48, 0x83, 0xec, 0x10,       /* 3: sub rsp, 0x10 */
    0x74, 0x06,                   /* 7: je -> 15, fallthrough 9 */
    0x48, 0x83, 0xec, 0x20,       /* 9: sub rsp, 0x20 */
    0xeb, 0x06,                   /* 13: jmp -> 21 */
    0x48, 0x83, 0xec, 0x30,       /* 15: sub rsp, 0x30 */
    0xeb, 0x00,                   /* 19: jmp -> 21 */
    0x90,                         /* 21: nop (merge) */
    0x48, 0x89, 0xec,             /* 22: mov rsp, rbp */
    0xc3                          /* 25: ret */
};

static ud_frame_report_t *
analyze_diamond(int renumber_and_reverse, ud_frame_error_t *error)
{
    if (!renumber_and_reverse) {
        static size_t entry_succ[] = { 1, 2 };
        static size_t left_succ[] = { 3 };
        static size_t right_succ[] = { 3 };
        ud_frame_block_t blocks[4];
        ud_frame_graph_t graph;

        blocks[0].begin = 0;  blocks[0].end = 9;
        blocks[0].successors = entry_succ; blocks[0].successor_count = 2;
        blocks[1].begin = 9;  blocks[1].end = 15;
        blocks[1].successors = left_succ; blocks[1].successor_count = 1;
        blocks[2].begin = 15; blocks[2].end = 21;
        blocks[2].successors = right_succ; blocks[2].successor_count = 1;
        blocks[3].begin = 21; blocks[3].end = 26;
        blocks[3].successors = NULL; blocks[3].successor_count = 0;
        graph.blocks = blocks; graph.block_count = 4; graph.entry = 0;
        return ud_frame_analyze(diamond_code, sizeof diamond_code, &graph,
                                error);
    } else {
        /* Address order is 2(entry), 1(left), 3(right), 0(merge). */
        enum { MERGE = 0, LEFT = 1, ENTRY = 2, RIGHT = 3 };
        static size_t entry_succ[] = { RIGHT, LEFT };
        static size_t left_succ[] = { MERGE };
        static size_t right_succ[] = { MERGE };
        ud_frame_block_t blocks[4];
        ud_frame_graph_t graph;

        blocks[ENTRY].begin = 0;  blocks[ENTRY].end = 9;
        blocks[ENTRY].successors = entry_succ;
        blocks[ENTRY].successor_count = 2;
        blocks[LEFT].begin = 9;   blocks[LEFT].end = 15;
        blocks[LEFT].successors = left_succ;
        blocks[LEFT].successor_count = 1;
        blocks[RIGHT].begin = 15; blocks[RIGHT].end = 21;
        blocks[RIGHT].successors = right_succ;
        blocks[RIGHT].successor_count = 1;
        blocks[MERGE].begin = 21; blocks[MERGE].end = 26;
        blocks[MERGE].successors = NULL;
        blocks[MERGE].successor_count = 0;
        graph.blocks = blocks; graph.block_count = 4;
        graph.entry = ENTRY;
        return ud_frame_analyze(diamond_code, sizeof diamond_code, &graph,
                                error);
    }
}

static int
same_value(ud_frame_value_t a, ud_frame_value_t b)
{
    return a.kind == b.kind &&
           (a.kind != UD_FRAME_RELATIVE || a.offset == b.offset);
}

static int
same_state(const ud_frame_state_t *a, const ud_frame_state_t *b)
{
    return same_value(a->rsp, b->rsp) && same_value(a->rbp, b->rbp);
}

static int
same_semantic_report(const ud_frame_report_t *a, const ud_frame_report_t *b)
{
    size_t i;
    if (a->insn_count != b->insn_count ||
        a->access_count != b->access_count ||
        a->block_count != b->block_count)
        return 0;

    for (i = 0; i < a->insn_count; ++i) {
        const ud_frame_insn_t *x = &a->insns[i];
        const ud_frame_insn_t *y = &b->insns[i];
        if (x->offset != y->offset || x->length != y->length ||
            x->mnemonic != y->mnemonic ||
            !same_state(&x->before, &y->before) ||
            x->access_count != y->access_count)
            return 0;
    }
    for (i = 0; i < a->access_count; ++i) {
        const ud_frame_access_t *x = &a->accesses[i];
        const ud_frame_access_t *y = &b->accesses[i];
        if (x->block_begin != y->block_begin ||
            x->insn_offset != y->insn_offset ||
            x->operand_index != y->operand_index ||
            x->kind != y->kind || x->direction != y->direction ||
            x->width != y->width || x->width_bits != y->width_bits ||
            x->address_known != y->address_known ||
            (x->address_known && x->address != y->address))
            return 0;
    }
    for (i = 0; i < a->block_count; ++i) {
        if (a->blocks[i].begin != b->blocks[i].begin ||
            a->blocks[i].reachable != b->blocks[i].reachable ||
            !same_state(&a->blocks[i].entry_state,
                         &b->blocks[i].entry_state))
            return 0;
    }
    return 1;
}

static int
test_numbering_and_edge_order_are_stable(void)
{
    ud_frame_error_t ea, eb;
    ud_frame_report_t *a = analyze_diamond(0, &ea);
    ud_frame_report_t *b = analyze_diamond(1, &eb);
    TASK_CHECK(a != NULL);
    TASK_CHECK(b != NULL);
    if (a != NULL && b != NULL)
        TASK_CHECK(same_semantic_report(a, b));
    ud_frame_report_free(a);
    ud_frame_report_free(b);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* merge                                                                      */
/* ------------------------------------------------------------------------- */

static int
test_diamond_merge_keeps_rbp_and_recovers_rsp(void)
{
    ud_frame_error_t error;
    ud_frame_report_t *r = analyze_diamond(0, &error);
    const ud_frame_insn_t *p;
    TASK_CHECK(r != NULL);
    if (r == NULL)
        return 1;
    TASK_CHECK(r->insn_count == 10);

    p = insn_at(r, 0);
    TASK_CHECK(p != NULL);
    if (p) CHECK_REL(p->before.rsp, 0);

    p = insn_at(r, 7);
    TASK_CHECK(p != NULL);
    if (p) {
        CHECK_REL(p->before.rsp, -16);
        CHECK_REL(p->before.rbp, 0);
    }

    p = insn_at(r, 13);
    TASK_CHECK(p != NULL);
    if (p) CHECK_REL(p->before.rsp, -48);

    p = insn_at(r, 19);
    TASK_CHECK(p != NULL);
    if (p) CHECK_REL(p->before.rsp, -64);

    p = insn_at(r, 21);
    TASK_CHECK(p != NULL);
    if (p) {
        CHECK_UNKNOWN(p->before.rsp);
        CHECK_REL(p->before.rbp, 0);
    }

    p = insn_at(r, 22);
    TASK_CHECK(p != NULL);
    if (p) {
        CHECK_UNKNOWN(p->before.rsp);
        CHECK_REL(p->before.rbp, 0);
    }

    p = insn_at(r, 25);
    TASK_CHECK(p != NULL);
    if (p) CHECK_REL(p->before.rsp, 0);

    ud_frame_report_free(r);
    return 0;
}

static int
test_backedge_repropagates_and_rbp_stays_fixed(void)
{
    static const uint8_t code[] = {
        0x48, 0x89, 0xe5,       /* 0: mov rbp, rsp */
        0x48, 0x83, 0xec, 0x20, /* 3: sub rsp, 0x20 */
        0x48, 0x83, 0xec, 0x08, /* 7: sub rsp, 8 (loop body) */
        0x75, 0xfa,             /* 11: jnz 7 */
        0x48, 0x89, 0xec,       /* 13: mov rsp, rbp */
        0xc3                    /* 16: ret */
    };
    static size_t entry_succ[] = { 1 };
    static size_t loop_succ[] = { 1, 2 };
    ud_frame_block_t blocks[3];
    ud_frame_graph_t graph;
    ud_frame_error_t error;
    ud_frame_report_t *r;
    const ud_frame_insn_t *p;

    blocks[0].begin = 0; blocks[0].end = 7;
    blocks[0].successors = entry_succ; blocks[0].successor_count = 1;
    blocks[1].begin = 7; blocks[1].end = 13;
    blocks[1].successors = loop_succ; blocks[1].successor_count = 2;
    blocks[2].begin = 13; blocks[2].end = 17;
    blocks[2].successors = NULL; blocks[2].successor_count = 0;
    graph.blocks = blocks; graph.block_count = 3; graph.entry = 0;

    r = ud_frame_analyze(code, sizeof code, &graph, &error);
    TASK_CHECK(r != NULL);
    if (r == NULL)
        return 1;

    /*
     * The back edge carries -40 while the entry edge carries -32.  The final
     * fixed point must therefore be unknown at the header, not the value from
     * whichever edge was processed first.  RBP remains S+0.
     */
    p = insn_at(r, 7);
    TASK_CHECK(p != NULL);
    if (p) {
        CHECK_UNKNOWN(p->before.rsp);
        CHECK_REL(p->before.rbp, 0);
    }
    p = insn_at(r, 11);
    TASK_CHECK(p != NULL);
    if (p) {
        CHECK_UNKNOWN(p->before.rsp);
        CHECK_REL(p->before.rbp, 0);
    }
    p = insn_at(r, 13);
    TASK_CHECK(p != NULL);
    if (p) {
        CHECK_UNKNOWN(p->before.rsp);
        CHECK_REL(p->before.rbp, 0);
    }
    p = insn_at(r, 16);
    TASK_CHECK(p != NULL);
    if (p) CHECK_REL(p->before.rsp, 0);

    TASK_CHECK(r->blocks[1].reachable == 1);
    CHECK_UNKNOWN(r->blocks[1].entry_state.rsp);
    CHECK_REL(r->blocks[1].entry_state.rbp, 0);
    ud_frame_report_free(r);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* effects                                                                    */
/* ------------------------------------------------------------------------- */

static ud_frame_report_t *
analyze_one(const uint8_t *code, size_t size, ud_frame_error_t *error)
{
    ud_frame_block_t block;
    ud_frame_graph_t graph;
    block.begin = 0;
    block.end = size;
    block.successors = NULL;
    block.successor_count = 0;
    graph.blocks = &block;
    graph.block_count = 1;
    graph.entry = 0;
    return ud_frame_analyze(code, size, &graph, error);
}

static int
test_prologue_negative_disp_and_implicit_slots(void)
{
    static const uint8_t code[] = {
        0x55,                         /* 0 push rbp */
        0x48, 0x89, 0xe5,             /* 1 mov rbp, rsp */
        0x48, 0x83, 0xec, 0x20,       /* 4 sub rsp, 0x20 */
        0x48, 0x8b, 0x45, 0xf8,       /* 8 mov rax, [rbp-8] */
        0xc9,                         /* 12 leave */
        0xc3                          /* 13 ret */
    };
    ud_frame_error_t error;
    ud_frame_report_t *r = analyze_one(code, sizeof code, &error);
    const ud_frame_insn_t *p;
    const ud_frame_access_t *a;

    TASK_CHECK(r != NULL);
    if (r == NULL)
        return 1;
    TASK_CHECK(r->insn_count == 6);
    TASK_CHECK(r->access_count == 4);

    p = insn_at(r, 1);
    TASK_CHECK(p != NULL);
    if (p) CHECK_REL(p->before.rsp, -8);
    p = insn_at(r, 4);
    if (p) {
        CHECK_REL(p->before.rsp, -8);
        CHECK_REL(p->before.rbp, -8);
    }
    p = insn_at(r, 8);
    if (p) CHECK_REL(p->before.rsp, -40);
    p = insn_at(r, 13);
    TASK_CHECK(p != NULL);
    if (p) CHECK_REL(p->before.rsp, 0);

    a = access_at(r, 0, UD_FRAME_ACCESS_PUSH);
    TASK_CHECK(a != NULL);
    if (a) {
        TASK_CHECK(a->direction == UD_FRAME_ACCESS_WRITE);
        TASK_CHECK(a->width == 8);
        TASK_CHECK(a->address_known == 1);
        TASK_CHECK(a->address == -8);
    }

    a = access_at(r, 8, UD_FRAME_ACCESS_EXPLICIT);
    TASK_CHECK(a != NULL);
    if (a) {
        TASK_CHECK(a->direction == UD_FRAME_ACCESS_READ);
        TASK_CHECK(a->width == 8);
        TASK_CHECK(a->operand_index == 1);
        TASK_CHECK(a->address_known == 1);
        TASK_CHECK(a->address == -16);
    }

    a = access_at(r, 12, UD_FRAME_ACCESS_LEAVE);
    TASK_CHECK(a != NULL);
    if (a) {
        TASK_CHECK(a->direction == UD_FRAME_ACCESS_READ);
        TASK_CHECK(a->width == 8);
        TASK_CHECK(a->address == -8);
    }

    a = access_at(r, 13, UD_FRAME_ACCESS_RET);
    TASK_CHECK(a != NULL);
    if (a) {
        TASK_CHECK(a->direction == UD_FRAME_ACCESS_READ);
        TASK_CHECK(a->width == 8);
        TASK_CHECK(a->address == 0);
    }

    ud_frame_report_free(r);
    return 0;
}

static int
test_push_pop_call_and_pop_rsp(void)
{
    static const uint8_t code[] = {
        0x50,                         /* 0 push rax */
        0x58,                         /* 1 pop rax */
        0xe8, 0x00, 0x00, 0x00, 0x00, /* 2 call $+7 */
        0xc3                          /* 7 ret */
    };
    static const uint8_t pop_rsp_code[] = { 0x5c, 0x90 };
    static const uint8_t pop_rbp_code[] = {
        0x48, 0x89, 0xe5, /* mov rbp, rsp */
        0x5d,             /* pop rbp */
        0x90              /* nop */
    };
    static const uint8_t unknown_push_code[] = {
        0x48, 0x83, 0xe4, 0xf0, /* and rsp, -16 */
        0x50                    /* push rax */
    };
    ud_frame_error_t error;
    ud_frame_report_t *r = analyze_one(code, sizeof code, &error);
    const ud_frame_access_t *a;
    const ud_frame_insn_t *p;

    TASK_CHECK(r != NULL);
    if (r == NULL)
        return 1;
    a = access_at(r, 0, UD_FRAME_ACCESS_PUSH);
    TASK_CHECK(a && a->address == -8 && a->direction == UD_FRAME_ACCESS_WRITE);
    a = access_at(r, 1, UD_FRAME_ACCESS_POP);
    TASK_CHECK(a && a->address == -8 && a->direction == UD_FRAME_ACCESS_READ);
    a = access_at(r, 2, UD_FRAME_ACCESS_CALL);
    TASK_CHECK(a && a->address == -8 && a->direction == UD_FRAME_ACCESS_WRITE);
    a = access_at(r, 7, UD_FRAME_ACCESS_RET);
    TASK_CHECK(a && a->address == 0 && a->direction == UD_FRAME_ACCESS_READ);
    ud_frame_report_free(r);

    r = analyze_one(pop_rsp_code, sizeof pop_rsp_code, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        a = access_at(r, 0, UD_FRAME_ACCESS_POP);
        TASK_CHECK(a != NULL && a->address == 0);
        p = insn_at(r, 1);
        TASK_CHECK(p != NULL);
        if (p)
            CHECK_UNKNOWN(p->before.rsp); /* not S+8 */
        ud_frame_report_free(r);
    }

    r = analyze_one(pop_rbp_code, sizeof pop_rbp_code, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        p = insn_at(r, 4);
        TASK_CHECK(p != NULL);
        if (p) {
            CHECK_REL(p->before.rsp, 8);
            CHECK_UNKNOWN(p->before.rbp);
        }
        ud_frame_report_free(r);
    }

    r = analyze_one(unknown_push_code, sizeof unknown_push_code, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        p = insn_at(r, 4);
        TASK_CHECK(p != NULL);
        if (p)
            CHECK_UNKNOWN(p->before.rsp);
        a = access_at(r, 4, UD_FRAME_ACCESS_PUSH);
        TASK_CHECK(a != NULL);
        if (a) {
            TASK_CHECK(a->address_known == 0);
            TASK_CHECK(a->width == 8);
            TASK_CHECK(a->direction == UD_FRAME_ACCESS_WRITE);
        }
        ud_frame_report_free(r);
    }
    return 0;
}

static int
test_alias_writes_destroy_relation(void)
{
    static const uint8_t code32[] = { 0x89, 0xcc, 0x90 };
    static const uint8_t code16[] = { 0x66, 0x89, 0xcc, 0x90 };
    static const uint8_t code8[] = { 0x40, 0x88, 0xc4, 0x90 };
    const uint8_t *cases[] = { code32, code16, code8 };
    const size_t sizes[] = { sizeof code32, sizeof code16, sizeof code8 };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        ud_frame_error_t error;
        ud_frame_report_t *r = analyze_one(cases[i], sizes[i], &error);
        const ud_frame_insn_t *alias_insn;
        const ud_frame_insn_t *nop_insn;
        TASK_CHECK(r != NULL);
        if (r == NULL)
            return 1;
        alias_insn = insn_at(r, 0);
        nop_insn = insn_at(r, sizes[i] - 1);
        TASK_CHECK(alias_insn != NULL && nop_insn != NULL);
        if (alias_insn)
            CHECK_REL(alias_insn->before.rsp, 0);
        if (nop_insn)
            CHECK_UNKNOWN(nop_insn->before.rsp);
        ud_frame_report_free(r);
    }
    return 0;
}

static int
test_mov_memory_forms_and_unknown_addresses(void)
{
    static const uint8_t write_rsp[] = { 0x48,0x89,0x44,0x24,0xf8 };
    static const uint8_t read_rsp[] = { 0x48,0x8b,0x44,0x24,0x10 };
    static const uint8_t rbp32[] = {
        0x48,0x89,0xe5,       /* mov rbp,rsp */
        0x89,0x45,0xfc        /* mov [rbp-4],eax */
    };
    static const uint8_t indexed[] = { 0x48,0x8b,0x44,0x84,0x00 };
    static const uint8_t addrsize[] = { 0x67,0x48,0x8b,0x44,0x24,0x10 };
    static const uint8_t fsseg[] = { 0x64,0x48,0x8b,0x04,0x24 };
    ud_frame_error_t error;
    ud_frame_report_t *r;
    const ud_frame_access_t *a;

    r = analyze_one(write_rsp, sizeof write_rsp, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        a = access_at(r, 0, UD_FRAME_ACCESS_EXPLICIT);
        TASK_CHECK(a != NULL);
        if (a) {
            TASK_CHECK(a->direction == UD_FRAME_ACCESS_WRITE);
            TASK_CHECK(a->width == 8);
            TASK_CHECK(a->operand_index == 0);
            TASK_CHECK(a->address_known);
            TASK_CHECK(a->address == -8);
        }
        ud_frame_report_free(r);
    }

    r = analyze_one(read_rsp, sizeof read_rsp, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        a = access_at(r, 0, UD_FRAME_ACCESS_EXPLICIT);
        TASK_CHECK(a != NULL);
        if (a) {
            TASK_CHECK(a->direction == UD_FRAME_ACCESS_READ);
            TASK_CHECK(a->width == 8);
            TASK_CHECK(a->operand_index == 1);
            TASK_CHECK(a->address == 16);
        }
        ud_frame_report_free(r);
    }

    r = analyze_one(rbp32, sizeof rbp32, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        a = access_at(r, 3, UD_FRAME_ACCESS_EXPLICIT);
        TASK_CHECK(a != NULL);
        if (a) {
            TASK_CHECK(a->direction == UD_FRAME_ACCESS_WRITE);
            TASK_CHECK(a->width == 4);
            TASK_CHECK(a->address == -4);
        }
        ud_frame_report_free(r);
    }

    {
        const uint8_t *unknown_cases[] = { indexed, addrsize, fsseg };
        const size_t unknown_sizes[] = {
            sizeof indexed, sizeof addrsize, sizeof fsseg
        };
        size_t i;
        for (i = 0; i < 3; ++i) {
            r = analyze_one(unknown_cases[i], unknown_sizes[i], &error);
            TASK_CHECK(r != NULL);
            if (r == NULL)
                return 1;
            TASK_CHECK(r->access_count == 1);
            a = &r->accesses[0];
            TASK_CHECK(a->kind == UD_FRAME_ACCESS_EXPLICIT);
            TASK_CHECK(a->address_known == 0);
            ud_frame_report_free(r);
        }
    }
    return 0;
}

static int
test_lea_arithmetic_and_logic(void)
{
    static const uint8_t lea_code[] = {
        0x48,0x89,0xe5,             /* 0 mov rbp,rsp */
        0x48,0x8d,0x65,0xf0,       /* 3 lea rsp,[rbp-16] */
        0x90                        /* 7 nop */
    };
    static const uint8_t lea_index_code[] = {
        0x48,0x89,0xe5,             /* 0 */
        0x48,0x8d,0x64,0x85,0x08,  /* 3 lea rsp,[rbp+rax*4+8] */
        0x90                        /* 8 */
    };
    static const uint8_t arith_code[] = {
        0x48,0x83,0xc4,0x10,       /* add rsp, 16 */
        0x90,
        0x48,0x83,0xc4,0xf0,       /* add rsp, -16 */
        0x90,
        0x48,0x83,0xec,0x20,       /* sub rsp, 32 */
        0x90,
        0x48,0x83,0xe4,0xf0,       /* and rsp, -16 */
        0x90
    };
    ud_frame_error_t error;
    ud_frame_report_t *r;
    const ud_frame_insn_t *p;
    const ud_frame_access_t *a;

    r = analyze_one(lea_code, sizeof lea_code, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        a = access_at(r, 3, UD_FRAME_ACCESS_LEA);
        TASK_CHECK(a != NULL);
        if (a) {
            TASK_CHECK(a->direction == 0);
            TASK_CHECK(a->width == 0);
            TASK_CHECK(a->address_known);
            TASK_CHECK(a->address == -16);
        }
        p = insn_at(r, 7);
        TASK_CHECK(p != NULL);
        if (p) CHECK_REL(p->before.rsp, -16);
        ud_frame_report_free(r);
    }

    r = analyze_one(lea_index_code, sizeof lea_index_code, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        a = access_at(r, 3, UD_FRAME_ACCESS_LEA);
        TASK_CHECK(a != NULL);
        if (a)
            TASK_CHECK(a->address_known == 0);
        p = insn_at(r, 8);
        TASK_CHECK(p != NULL);
        if (p) CHECK_UNKNOWN(p->before.rsp);
        ud_frame_report_free(r);
    }

    r = analyze_one(arith_code, sizeof arith_code, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        p = insn_at(r, 4);
        if (p) CHECK_REL(p->before.rsp, 16);
        p = insn_at(r, 9);
        /* After add 16, then add -16: S+0. */
        if (p) CHECK_REL(p->before.rsp, 0);
        p = insn_at(r, 14);
        /* Previous result S+0, then sub 32. */
        if (p) CHECK_REL(p->before.rsp, -32);
        p = insn_at(r, 19);
        if (p) CHECK_UNKNOWN(p->before.rsp);
        ud_frame_report_free(r);
    }
    return 0;
}

static int
test_cmp_test_and_jumps_do_not_change_state(void)
{
    static const uint8_t code[] = {
        0x48,0x83,0xc4,0x08,       /* 0 add rsp,8 */
        0x48,0x83,0xfc,0x00,       /* 4 cmp rsp,0 */
        0x48,0x85,0xc0,             /* 8 test rax,rax */
        0x74,0x00,                  /* 11 je +0 */
        0xeb,0x00,                  /* 13 jmp +0 */
        0x90                        /* 15 nop */
    };
    ud_frame_error_t error;
    ud_frame_report_t *r = analyze_one(code, sizeof code, &error);
    const ud_frame_insn_t *p;
    TASK_CHECK(r != NULL);
    if (r) {
        p = insn_at(r, 15);
        TASK_CHECK(p != NULL);
        if (p) CHECK_REL(p->before.rsp, 8);
        TASK_CHECK(r->access_count == 0);
        ud_frame_report_free(r);
    }
    return 0;
}

static int
test_immediate_push_and_indirect_control(void)
{
    static const uint8_t push_imm_code[] = { 0x6a, 0x20, 0x90 };
    static const uint8_t indirect_code[] = {
        0x48, 0x83, 0xec, 0x08, /* sub rsp, 8 */
        0xff, 0xd0,             /* call rax */
        0xff, 0xe0              /* jmp rax */
    };
    ud_frame_error_t error;
    ud_frame_report_t *r;
    const ud_frame_access_t *a;
    const ud_frame_insn_t *p;

    r = analyze_one(push_imm_code, sizeof push_imm_code, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        a = access_at(r, 0, UD_FRAME_ACCESS_PUSH);
        TASK_CHECK(a != NULL);
        if (a)
            TASK_CHECK(a->address == -8 && a->width == 8);
        p = insn_at(r, 2);
        TASK_CHECK(p != NULL);
        if (p)
            CHECK_REL(p->before.rsp, -8);
        ud_frame_report_free(r);
    }

    r = analyze_one(indirect_code, sizeof indirect_code, &error);
    TASK_CHECK(r != NULL);
    if (r) {
        TASK_CHECK(r->insn_count == 3);
        a = access_at(r, 4, UD_FRAME_ACCESS_CALL);
        TASK_CHECK(a != NULL);
        if (a)
            TASK_CHECK(a->address == -16);
        p = insn_at(r, 6);
        TASK_CHECK(p != NULL);
        if (p)
            CHECK_REL(p->before.rsp, -8); /* call is net-zero */
        ud_frame_report_free(r);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Registration                                                               */
/* ------------------------------------------------------------------------- */

const struct task_case task_cases[] = {
    { "normalize", "unreachable-is-silent", test_unreachable_block_is_silent },
    { "normalize", "bad-range-edge-decode", test_bad_range_edge_and_decode_locations },
    { "normalize", "failure-reuse-and-owned-input", test_failure_reuse_and_owned_input },
    { "normalize", "stable-numbering-edge-order", test_numbering_and_edge_order_are_stable },

    { "merge", "diamond-rbp-and-rsp-recovery", test_diamond_merge_keeps_rbp_and_recovers_rsp },
    { "merge", "backedge-fixed-rbp", test_backedge_repropagates_and_rbp_stays_fixed },

    { "effects", "prologue-negative-disp-slots", test_prologue_negative_disp_and_implicit_slots },
    { "effects", "push-pop-call-poprsp", test_push_pop_call_and_pop_rsp },
    { "effects", "immediate-push-indirect-control", test_immediate_push_and_indirect_control },
    { "effects", "alias-writes-destroy-relation", test_alias_writes_destroy_relation },
    { "effects", "mov-memory-known-and-unknown", test_mov_memory_forms_and_unknown_addresses },
    { "effects", "lea-add-sub-and", test_lea_arithmetic_and_logic },
    { "effects", "cmp-test-jumps-no-effect", test_cmp_test_and_jumps_do_not_change_state }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
