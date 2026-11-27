/*
 * LR/SC static analysis test cases.
 *
 * Every instruction byte below is created locally with the small encoders
 * in this file (normal RV64 I/A/C words, mixed 16/32-bit width). Nothing is
 * assembled with an external toolchain and nothing here is ever executed:
 * the analyzer consumes the bytes as a read-only image.
 *
 * Coverage the legacy print-based tests (tests/t1.c, tests/t2.c,
 * task_support/legacy_decode.c) cannot provide:
 *   - they only render disassembly text, so pseudo-instruction lifting hides
 *     the canonical op/operands (e.g. c.mv prints as "mv"), and no test ever
 *     inspects rd/rs1/rs2/imm/aq/rl as structured fields;
 *   - they never recover a control-flow graph, so reachability, successor
 *     edges, region boundaries and external entries are untested;
 *   - a nearby LR and SC printed in sequence are not evidence of a pair:
 *     address identity, SC result flow, fail/success edge direction and the
 *     16-instruction / forward-only structural rules are all asserted here
 *     for the first time, on both matching and deliberately broken fixtures.
 */
#include "check.h"
#include "riscv-disas.h"
#include "lrsc.h"
#include <string.h>

#define BASE_PC 0x1000u

/* --------------------------------------------------------------- */
/* local encoders: created normal words, never executed             */
/* --------------------------------------------------------------- */

static uint32_t enc_r(uint32_t f7, unsigned rs2, unsigned rs1, unsigned f3,
                      unsigned rd, unsigned op)
{
    return (f7 & 0x7f) << 25 | (rs2 & 0x1f) << 20 | (rs1 & 0x1f) << 15 |
           (f3 & 7) << 12 | (rd & 0x1f) << 7 | op;
}
static uint32_t enc_i(int32_t imm, unsigned rs1, unsigned f3, unsigned rd,
                      unsigned op)
{
    return ((uint32_t)imm & 0xfff) << 20 | (rs1 & 0x1f) << 15 |
           (f3 & 7) << 12 | (rd & 0x1f) << 7 | op;
}
static uint32_t enc_b(int32_t imm, unsigned rs1, unsigned rs2, unsigned f3,
                      unsigned op)
{
    uint32_t u = (uint32_t)imm;
    return ((u >> 12) & 1) << 31 | ((u >> 5) & 0x3f) << 25 |
           (rs2 & 0x1f) << 20 | (rs1 & 0x1f) << 15 | (f3 & 7) << 12 |
           ((u >> 1) & 0xf) << 8 | ((u >> 11) & 1) << 7 | op;
}
static uint32_t enc_j(unsigned rd, int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    return (rd & 0x1f) << 7 | ((u >> 20) & 1) << 31 |
           ((u >> 1) & 0x3ff) << 21 | ((u >> 11) & 1) << 20 |
           ((u >> 12) & 0xff) << 12 | 0x6f;
}
static uint32_t enc_lr(unsigned rd, unsigned rs1, int aq, int rl)
{
    return 0x2u << 27 | (aq ? 1u << 26 : 0) | (rl ? 1u << 25 : 0) |
           (rs1 & 0x1f) << 15 | 0x3u << 12 | (rd & 0x1f) << 7 | 0x2f;
}
static uint32_t enc_lr_w(unsigned rd, unsigned rs1)
{
    return 0x2u << 27 | (rs1 & 0x1f) << 15 | 0x2u << 12 |
           (rd & 0x1f) << 7 | 0x2f;
}
static uint32_t enc_sc(unsigned rd, unsigned rs2, unsigned rs1, int aq, int rl)
{
    return 0x3u << 27 | (aq ? 1u << 26 : 0) | (rl ? 1u << 25 : 0) |
           (rs2 & 0x1f) << 20 | (rs1 & 0x1f) << 15 | 0x3u << 12 |
           (rd & 0x1f) << 7 | 0x2f;
}
static uint16_t enc_caddi(unsigned rd, int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    return 0x0001 | (rd & 0x1f) << 7 | ((u >> 5) & 1) << 12 | (u & 0x1f) << 2;
}
static uint16_t enc_cmv(unsigned rd, unsigned rs2)
{
    return 0x8002 | (rd & 0x1f) << 7 | (rs2 & 0x1f) << 2;
}
static uint16_t enc_cj(int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    return 0xa001 | ((u >> 11) & 1) << 12 | ((u >> 4) & 1) << 11 |
           ((u >> 8) & 3) << 9 | ((u >> 10) & 1) << 8 |
           ((u >> 6) & 1) << 7 | ((u >> 7) & 1) << 6 |
           ((u >> 1) & 7) << 3 | ((u >> 5) & 1) << 2;
}
static uint16_t enc_cbz(int bnez, unsigned rs1, int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    uint16_t base = bnez ? 0xe001 : 0xc001;
    return base | ((rs1 - 8) & 7) << 7 | ((u >> 8) & 1) << 12 |
           ((u >> 3) & 3) << 10 | ((u >> 6) & 3) << 5 |
           ((u >> 1) & 3) << 3 | ((u >> 5) & 1) << 2;
}

struct codebuf {
    uint8_t b[256];
    size_t len;
};
static void e32(struct codebuf *c, uint32_t w)
{
    memcpy(c->b + c->len, &w, 4);
    c->len += 4;
}
static void e16(struct codebuf *c, uint16_t h)
{
    memcpy(c->b + c->len, &h, 2);
    c->len += 2;
}
static uint64_t pc_at(size_t off)
{
    return BASE_PC + off;
}
static const lrsc_node *node_at(const lrsc_report *r, uint64_t pc)
{
    size_t n;
    const lrsc_node *nodes = lrsc_report_nodes(r, &n);
    for (size_t i = 0; i < n; i++)
        if (nodes[i].dec.pc == pc)
            return &nodes[i];
    return NULL;
}
static const lrsc_candidate *cand_at(const lrsc_report *r, uint64_t lr_pc)
{
    size_t n;
    const lrsc_candidate *cs = lrsc_report_candidates(r, &n);
    for (size_t i = 0; i < n; i++)
        if (cs[i].lr_pc == lr_pc)
            return &cs[i];
    return NULL;
}
static bool region_has_pc(const lrsc_candidate *c, uint64_t pc)
{
    for (uint32_t i = 0; i < c->region_count; i++)
        if (c->region_pc[i] == pc)
            return true;
    return false;
}

/* =============================================================== */
/* decode group: canonical structured entry, edges, data ownership  */
/* =============================================================== */

static int decode_canonical_fields(void)
{
    /* same shape as task_support/lrsc-inputs, rebuilt from local encoders */
    struct codebuf c = { .len = 0 };
    size_t off_lr = c.len; e32(&c, enc_lr(5, 10, 1, 0));        /* lr.d.aq t0,(a0) */
    size_t off_mv = c.len; e32(&c, enc_i(0, 10, 0, 11, 0x13));  /* addi a1,a0,0 */
    size_t off_cadd = c.len; e16(&c, enc_caddi(5, 1));          /* c.addi t0,1 */
    size_t off_sc = c.len; e32(&c, enc_sc(6, 5, 11, 0, 1));     /* sc.d.rl t1,t0,(a1) */
    size_t off_bne = c.len; e32(&c, enc_b(-14, 6, 0, 1, 0x63)); /* bne t1,x0,lr */
    size_t off_exit = c.len; e32(&c, enc_i(1, 0, 0, 12, 0x13));

    uint64_t entry = pc_at(off_lr);
    lrsc_report *r = lrsc_analyze(rv64, BASE_PC, c.b, c.len, &entry, 1);
    TASK_CHECK(r != NULL);

    size_t nn = 0;
    TASK_CHECK(lrsc_report_nodes(r, &nn) != NULL && nn == 6);
    TASK_CHECK(lrsc_report_reachable_count(r) == 6);

    const lrsc_node *n;
    n = node_at(r, pc_at(off_lr));
    TASK_CHECK(n && n->len == 4 && n->dec.op == rv_op_lr_d);
    TASK_CHECK(n && n->dec.rd == 5 && n->dec.rs1 == 10 && n->dec.rs2 == 0);
    TASK_CHECK(n && n->dec.aq == 1 && n->dec.rl == 0);

    n = node_at(r, pc_at(off_mv));
    /* pre-pseudo-lift canonical form: still addi with imm 0, not rv_op_mv */
    TASK_CHECK(n && n->len == 4 && n->dec.op == rv_op_addi);
    TASK_CHECK(n && n->dec.rd == 11 && n->dec.rs1 == 10 && n->dec.imm == 0);

    n = node_at(r, pc_at(off_cadd));
    TASK_CHECK(n && n->len == 2 && n->dec.op == rv_op_addi);
    TASK_CHECK(n && n->dec.rd == 5 && n->dec.rs1 == 5 && n->dec.imm == 1);

    n = node_at(r, pc_at(off_sc));
    TASK_CHECK(n && n->len == 4 && n->dec.op == rv_op_sc_d);
    TASK_CHECK(n && n->dec.rd == 6 && n->dec.rs1 == 11 && n->dec.rs2 == 5);
    TASK_CHECK(n && n->dec.aq == 0 && n->dec.rl == 1);

    n = node_at(r, pc_at(off_bne));
    TASK_CHECK(n && n->dec.op == rv_op_bne && n->dec.rs1 == 6 &&
               n->dec.rs2 == 0 && n->dec.imm == -14);

    n = node_at(r, pc_at(off_exit));
    TASK_CHECK(n && n->dec.op == rv_op_addi && n->dec.imm == 1);

    /* the printer still uses the same production decode path and may lift */
    rv_inst word;
    size_t wlen;
    inst_fetch(c.b + off_mv, &word, &wlen);
    char text[96];
    disasm_inst(text, sizeof text, rv64, pc_at(off_mv), word);
    TASK_CHECK(strstr(text, "mv") != NULL);

    lrsc_report_free(r);
    return 0;
}

static int decode_compressed_canonical_and_edges(void)
{
    struct codebuf c = { .len = 0 };
    size_t off_lr = c.len; e32(&c, enc_lr(5, 10, 0, 0));
    size_t off_sc = c.len; e32(&c, enc_sc(9, 5, 10, 0, 0));  /* sc.d s1 */
    size_t off_mv = c.len; e16(&c, enc_cmv(9, 6));            /* c.mv s1,t1 */
    size_t off_bz = c.len; e16(&c, enc_cbz(1, 9, (int32_t)(off_lr - c.len))); /* c.bnez s1,lr */
    size_t off_j = c.len; e16(&c, enc_cj(2));                 /* c.j +2 (dead block) */
    size_t off_target = c.len; e32(&c, enc_i(0, 0, 0, 0, 0x13));

    uint64_t entry = pc_at(off_lr);
    lrsc_report *r = lrsc_analyze(rv64, BASE_PC, c.b, c.len, &entry, 1);
    TASK_CHECK(r != NULL);
    TASK_CHECK(lrsc_report_reachable_count(r) == 6);

    const lrsc_node *n = node_at(r, pc_at(off_sc));
    TASK_CHECK(n && n->len == 4 && n->dec.op == rv_op_sc_d &&
               n->dec.rd == 9 && n->dec.rs1 == 10);

    n = node_at(r, pc_at(off_mv));
    TASK_CHECK(n && n->len == 2 && n->dec.op == rv_op_addi &&
               n->dec.rd == 9 && n->dec.rs1 == 6 && n->dec.imm == 0);

    n = node_at(r, pc_at(off_bz));
    TASK_CHECK(n && n->len == 2 && n->dec.op == rv_op_bne &&
               n->dec.rs1 == 9 && n->dec.rs2 == 0);
    TASK_CHECK(n && n->nsucc == 2);
    TASK_CHECK(n && n->succ[0].kind == LRSC_EDGE_FALLTHROUGH &&
               n->succ[0].pc == pc_at(off_j));
    TASK_CHECK(n && n->succ[1].kind == LRSC_EDGE_BRANCH &&
               n->succ[1].pc == pc_at(off_lr));

    /* c.j stays canonical jal with rd x0; it has one direct jump successor */
    n = node_at(r, pc_at(off_j));
    TASK_CHECK(n && n->dec.op == rv_op_jal && n->dec.rd == 0 &&
               n->nsucc == 1 && n->succ[0].kind == LRSC_EDGE_JUMP &&
               n->succ[0].pc == pc_at(off_target));

    /* indirect transfers produce no recoverable direct successor */
    struct codebuf j = { .len = 0 };
    uint32_t jalr = enc_i(0, 1, 0, 0, 0x67); /* jalr x0,0(ra) */
    e32(&j, jalr);
    uint64_t je = pc_at(0);
    lrsc_report *rj = lrsc_analyze(rv64, BASE_PC, j.b, j.len, &je, 1);
    TASK_CHECK(rj != NULL);
    const lrsc_node *jn = node_at(rj, pc_at(0));
    TASK_CHECK(jn && jn->dec.op == rv_op_jalr && jn->nsucc == 0);
    lrsc_report_free(rj);

    lrsc_report_free(r);
    return 0;
}

static int decode_report_owns_input_and_is_stable(void)
{
    struct codebuf c = { .len = 0 };
    e32(&c, enc_lr(5, 10, 0, 0));
    e32(&c, enc_sc(6, 5, 10, 0, 0));
    e32(&c, enc_b(-8, 6, 0, 1, 0x63));
    e32(&c, enc_i(1, 0, 0, 12, 0x13));
    uint8_t saved[sizeof c.b];
    memcpy(saved, c.b, c.len);

    uint64_t entry = pc_at(0);
    lrsc_report *r1 = lrsc_analyze(rv64, BASE_PC, c.b, c.len, &entry, 1);
    TASK_CHECK(r1 != NULL);

    /* destroy the caller's buffer: the report must keep its own copy */
    memset(c.b, 0xa5, c.len);
    size_t owned_len = 0;
    const uint8_t *owned = lrsc_report_code(r1, &owned_len);
    TASK_CHECK(owned && owned_len == 4 + 4 + 4 + 4);
    TASK_CHECK(memcmp(owned, saved, owned_len) == 0);
    TASK_CHECK(lrsc_report_base_pc(r1) == BASE_PC);

    /* identical input yields byte-identical structured output */
    lrsc_report *r2 = lrsc_analyze(rv64, BASE_PC, saved, owned_len, &entry, 1);
    TASK_CHECK(r2 != NULL);
    size_t n1, n2;
    const lrsc_node *a = lrsc_report_nodes(r1, &n1);
    const lrsc_node *b = lrsc_report_nodes(r2, &n2);
    TASK_CHECK(n1 == n2 && memcmp(a, b, n1 * sizeof(*a)) == 0);
    size_t k1, k2;
    const lrsc_candidate *ca = lrsc_report_candidates(r1, &k1);
    const lrsc_candidate *cb = lrsc_report_candidates(r2, &k2);
    TASK_CHECK(k1 == k2 && memcmp(ca, cb, k1 * sizeof(*ca)) == 0);

    lrsc_report_free(r1);
    lrsc_report_free(r2);
    return 0;
}

/* =============================================================== */
/* pairing group: LR/SC pairing and retry-region structure          */
/* =============================================================== */

static int pairing_mixed_width_proven(void)
{
    struct codebuf c = { .len = 0 };
    size_t off_lr = c.len; e32(&c, enc_lr(5, 10, 0, 0));
    size_t off_mv = c.len; e16(&c, enc_cmv(11, 10));          /* c.mv a1,a0 */
    size_t off_add = c.len; e16(&c, enc_caddi(5, 1));         /* c.addi t0,1 */
    size_t off_sc = c.len; e32(&c, enc_sc(9, 5, 11, 0, 1));   /* sc.d.rl s1 */
    size_t off_br = c.len; e16(&c, enc_cbz(1, 9, -12));       /* c.bnez s1,lr */
    size_t off_exit = c.len; e32(&c, enc_i(1, 0, 0, 12, 0x13));

    uint64_t entry = pc_at(off_lr);
    lrsc_report *r = lrsc_analyze(rv64, BASE_PC, c.b, c.len, &entry, 1);
    TASK_CHECK(r != NULL);
    const lrsc_candidate *cd = cand_at(r, pc_at(off_lr));
    TASK_CHECK(cd != NULL);
    TASK_CHECK(cd->verdict == LRSC_PROVEN);
    TASK_CHECK(cd->reason == LRSC_REASON_NONE);
    TASK_CHECK(cd->sc_pc == pc_at(off_sc));
    TASK_CHECK(cd->branch_pc == pc_at(off_br));
    TASK_CHECK(cd->fail_target == pc_at(off_lr));
    TASK_CHECK(cd->success_target == pc_at(off_exit));

    /* region: lr, c.mv, c.addi, sc, branch = 5 nodes, strictly ordered */
    TASK_CHECK(cd->region_count == 5);
    TASK_CHECK(cd->region_pc[0] == pc_at(off_lr));
    for (uint32_t i = 1; i < cd->region_count; i++)
        TASK_CHECK(cd->region_pc[i] > cd->region_pc[i - 1]);
    TASK_CHECK(region_has_pc(cd, pc_at(off_mv)));
    TASK_CHECK(region_has_pc(cd, pc_at(off_add)));
    TASK_CHECK(region_has_pc(cd, pc_at(off_sc)));
    TASK_CHECK(region_has_pc(cd, pc_at(off_br)));
    /* success tail instruction is outside the certified region */
    TASK_CHECK(!region_has_pc(cd, pc_at(off_exit)));

    lrsc_report_free(r);
    return 0;
}

static int pairing_intervening_lr_and_width(void)
{
    struct codebuf c = { .len = 0 };
    size_t lr1 = c.len; e32(&c, enc_lr(5, 10, 0, 0));
    size_t lr2 = c.len; e32(&c, enc_lr(28, 11, 0, 0));        /* lr.d t3,(a1) */
    size_t sc1 = c.len; e32(&c, enc_sc(6, 5, 10, 0, 0));
    size_t br1 = c.len; e32(&c, enc_b((int32_t)(lr1 - br1), 6, 0, 1, 0x63));
    size_t sc2 = c.len; e32(&c, enc_sc(29, 28, 11, 0, 0));
    size_t br2 = c.len; e32(&c, enc_b((int32_t)(lr2 - br2), 29, 0, 1, 0x63));
    e32(&c, enc_i(1, 0, 0, 12, 0x13));

    uint64_t entry = pc_at(lr1);
    lrsc_report *r = lrsc_analyze(rv64, BASE_PC, c.b, c.len, &entry, 1);
    TASK_CHECK(r != NULL);
    TASK_CHECK(node_at(r, pc_at(sc2)) != NULL &&
               node_at(r, pc_at(sc2))->dec.op == rv_op_sc_d);
    size_t nc = 0;
    TASK_CHECK(lrsc_report_candidates(r, &nc) != NULL && nc == 2);

    const lrsc_candidate *c1 = cand_at(r, pc_at(lr1));
    TASK_CHECK(c1 && c1->verdict == LRSC_UNCONSTRAINED);
    TASK_CHECK(c1->reason == LRSC_REASON_INTERVENING_LR);
    TASK_CHECK(c1->reason_pc == pc_at(lr2));
    TASK_CHECK(c1->sc_pc == 0);

    const lrsc_candidate *c2 = cand_at(r, pc_at(lr2));
    TASK_CHECK(c2 && c2->verdict == LRSC_UNCONSTRAINED);
    /* c2's forward region reaches lr1 through br1's retry edge: the decisive
     * fault is a hard structural one pinned at lr1 (the other loop's LR),
     * never a soft address/status reason or a borrowed SC token */
    TASK_CHECK(c2->reason == LRSC_REASON_INTERVENING_LR ||
               c2->reason == LRSC_REASON_EXTERNAL_ENTRY ||
               c2->reason == LRSC_REASON_LOOP ||
               c2->reason == LRSC_REASON_BACKWARD_EDGE);
    TASK_CHECK(c2->reason_pc == pc_at(lr1));
    TASK_CHECK(c2->sc_pc == pc_at(sc1));

    /* mismatched width: lr.w followed by sc.d */
    struct codebuf w = { .len = 0 };
    size_t wlr = w.len; e32(&w, enc_lr_w(5, 10));
    size_t wsc = w.len; e32(&w, enc_sc(6, 5, 10, 0, 0));
    size_t wbr = w.len; e32(&w, enc_b((int32_t)(wlr - wbr), 6, 0, 1, 0x63));
    e32(&w, enc_i(1, 0, 0, 12, 0x13));
    uint64_t wentry = pc_at(wlr);
    lrsc_report *rw = lrsc_analyze(rv64, BASE_PC, w.b, w.len, &wentry, 1);
    TASK_CHECK(rw != NULL);
    const lrsc_candidate *cw = cand_at(rw, pc_at(wlr));
    TASK_CHECK(cw && cw->verdict == LRSC_UNCONSTRAINED);
    TASK_CHECK(cw->reason == LRSC_REASON_MIXED_WIDTH);
    TASK_CHECK(cw->reason_pc == pc_at(wsc));
    lrsc_report_free(rw);

    lrsc_report_free(r);
    return 0;
}

static int pairing_structural_violations(void)
{
    /* helper shape: lr; one offending instruction; sc; retry branch; exit */
    struct {
        const char *name;
        lrsc_reason reason;
        uint32_t bad;
    } cases[] = {
        { "fence",  LRSC_REASON_FENCE,  0x0000000f },
        { "mul",    LRSC_REASON_MULDIV, enc_r(1, 6, 5, 0, 7, 0x33) },
        { "load",   LRSC_REASON_MEMORY_OP, enc_i(0, 10, 0x2, 12, 0x03) },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        struct codebuf c = { .len = 0 };
        size_t lr = c.len; e32(&c, enc_lr(5, 10, 0, 0));
        size_t bad = c.len; e32(&c, cases[k].bad);
        size_t sc = c.len; e32(&c, enc_sc(6, 5, 10, 0, 0));
        size_t br = c.len; e32(&c, enc_b((int32_t)(lr - br), 6, 0, 1, 0x63));
        e32(&c, enc_i(1, 0, 0, 12, 0x13));
        uint64_t entry = pc_at(lr);
        lrsc_report *r = lrsc_analyze(rv64, BASE_PC, c.b, c.len, &entry, 1);
        TASK_CHECK(r != NULL);
        const lrsc_candidate *cd = cand_at(r, pc_at(lr));
        TASK_CHECK(cd && cd->verdict == LRSC_UNCONSTRAINED);
        TASK_CHECK(cd->reason == cases[k].reason);
        TASK_CHECK(cd->reason_pc == pc_at(bad));
        TASK_CHECK(cd->sc_pc == pc_at(sc));
        lrsc_report_free(r);
    }

    /* backward edge to an interior node is a loop, not a retry edge */
    struct codebuf lb = { .len = 0 };
    size_t l_lr = lb.len; e32(&lb, enc_lr(5, 10, 0, 0));
    size_t l_add = lb.len; e32(&lb, enc_i(1, 5, 0, 5, 0x13));
    size_t l_if = lb.len; e32(&lb, enc_b(8, 12, 0, 0, 0x63));
    size_t l_jmp = lb.len; e32(&lb, enc_j(0, (int32_t)(l_add - l_jmp)));
    size_t l_sc = lb.len; e32(&lb, enc_sc(6, 5, 10, 0, 0));
    size_t l_br = lb.len; e32(&lb, enc_b((int32_t)(l_lr - l_br), 6, 0, 1, 0x63));
    e32(&lb, enc_i(1, 0, 0, 12, 0x13));
    uint64_t lentry = pc_at(l_lr);
    lrsc_report *rl = lrsc_analyze(rv64, BASE_PC, lb.b, lb.len, &lentry, 1);
    TASK_CHECK(rl != NULL);
    const lrsc_candidate *cl = cand_at(rl, pc_at(l_lr));
    TASK_CHECK(cl && cl->verdict == LRSC_UNCONSTRAINED);
    TASK_CHECK(cl->reason == LRSC_REASON_BACKWARD_EDGE);
    TASK_CHECK(cl->reason_pc == pc_at(l_jmp));
    TASK_CHECK(node_at(rl, pc_at(l_if))->dec.op == rv_op_beq);
    TASK_CHECK(node_at(rl, pc_at(l_sc))->dec.op == rv_op_sc_d);
    lrsc_report_free(rl);

    /* a second entry landing inside the region breaks single-entry property */
    struct codebuf ee = { .len = 0 };
    size_t e_lr = ee.len; e32(&ee, enc_lr(5, 10, 0, 0));
    size_t e_mv = ee.len; e32(&ee, enc_i(0, 10, 0, 11, 0x13));
    size_t e_sc = ee.len; e32(&ee, enc_sc(6, 5, 11, 0, 0));
    size_t e_br = ee.len; e32(&ee, enc_b((int32_t)(e_lr - e_br), 6, 0, 1, 0x63));
    e32(&ee, enc_i(1, 0, 0, 12, 0x13));
    uint64_t entries[2] = { pc_at(e_lr), pc_at(e_mv) };
    lrsc_report *re = lrsc_analyze(rv64, BASE_PC, ee.b, ee.len, entries, 2);
    TASK_CHECK(re != NULL);
    const lrsc_candidate *ce = cand_at(re, pc_at(e_lr));
    TASK_CHECK(ce && ce->verdict == LRSC_UNCONSTRAINED);
    TASK_CHECK(ce->reason == LRSC_REASON_EXTERNAL_ENTRY);
    TASK_CHECK(ce->reason_pc == pc_at(e_mv));
    TASK_CHECK(ce->sc_pc == pc_at(e_sc));
    lrsc_report_free(re);

    /* illegal compressed halfword reachable inside the region */
    struct codebuf il = { .len = 0 };
    size_t i_lr = il.len; e32(&il, enc_lr(5, 10, 0, 0));
    size_t i_bad = il.len; e16(&il, 0x0000);
    size_t i_sc = il.len; e32(&il, enc_sc(6, 5, 10, 0, 0));
    size_t i_br = il.len; e32(&il, enc_b((int32_t)(i_lr - i_br), 6, 0, 1, 0x63));
    e32(&il, enc_i(1, 0, 0, 12, 0x13));
    uint64_t ientry = pc_at(i_lr);
    lrsc_report *ri = lrsc_analyze(rv64, BASE_PC, il.b, il.len, &ientry, 1);
    TASK_CHECK(ri != NULL);
    const lrsc_candidate *ci = cand_at(ri, pc_at(i_lr));
    TASK_CHECK(ci && ci->verdict == LRSC_UNCONSTRAINED);
    TASK_CHECK(ci->reason == LRSC_REASON_ILLEGAL_INST);
    TASK_CHECK(ci->reason_pc == pc_at(i_bad));
    TASK_CHECK(ci->sc_pc == 0);
    /* the SC still decodes in the sweep even though the illegal op cuts flow */
    TASK_CHECK(node_at(ri, pc_at(i_sc))->dec.op == rv_op_sc_d);
    lrsc_report_free(ri);
    return 0;
}

static int pairing_length_limit_and_missing_sc(void)
{
    /* 17 instructions: lr + 15 addi + sc, before the retry branch */
    struct codebuf big = { .len = 0 };
    size_t lr = big.len; e32(&big, enc_lr(5, 10, 0, 0));
    for (unsigned k = 0; k < 15; k++)
        e32(&big, enc_i(1, 5, 0, 5, 0x13));
    size_t sc = big.len; e32(&big, enc_sc(6, 5, 10, 0, 0));
    size_t br = big.len; e32(&big, enc_b((int32_t)(lr - br), 6, 0, 1, 0x63));
    e32(&big, enc_i(1, 0, 0, 12, 0x13));
    uint64_t entry = pc_at(lr);
    lrsc_report *r = lrsc_analyze(rv64, BASE_PC, big.b, big.len, &entry, 1);
    TASK_CHECK(r != NULL);
    const lrsc_candidate *cd = cand_at(r, pc_at(lr));
    TASK_CHECK(cd && cd->verdict == LRSC_UNCONSTRAINED);
    TASK_CHECK(cd->reason == LRSC_REASON_REGION_TOO_LONG);
    TASK_CHECK(cd->reason_pc == pc_at(lr) && cd->sc_pc == pc_at(sc));
    lrsc_report_free(r);

    /* LR with no SC at all: unknown, and no fabricated candidate pair */
    struct codebuf none = { .len = 0 };
    size_t nlr = none.len; e32(&none, enc_lr(5, 10, 0, 0));
    e32(&none, enc_i(1, 5, 0, 5, 0x13));
    size_t nj_pc = none.len; e32(&none, enc_j(0, (int32_t)(nlr - nj_pc)));
    uint64_t nentry = pc_at(nlr);
    lrsc_report *rn = lrsc_analyze(rv64, BASE_PC, none.b, none.len, &nentry, 1);
    TASK_CHECK(rn != NULL);
    const lrsc_candidate *cn = cand_at(rn, pc_at(nlr));
    TASK_CHECK(cn && cn->verdict == LRSC_UNKNOWN);
    TASK_CHECK(cn->reason == LRSC_REASON_NO_STATUS_BRANCH);
    TASK_CHECK(cn->reason_pc == pc_at(nlr) && cn->sc_pc == 0);
    lrsc_report_free(rn);
    return 0;
}

/* =============================================================== */
/* certificate group: address identity and SC result/edge proofs    */
/* =============================================================== */

static int cert_address_signed_offset_copy_and_overwrite(void)
{
    /* proven chain through a signed constant that cancels:
     * lr (a0); addi a1,a0,+8; addi a1,a1,-8; sc (a1) -> same address */
    struct codebuf ok = { .len = 0 };
    size_t lr = ok.len; e32(&ok, enc_lr(5, 10, 0, 0));
    size_t p1 = ok.len; e32(&ok, enc_i(8, 10, 0, 11, 0x13));
    size_t p2 = ok.len; e32(&ok, enc_i(-8, 11, 0, 11, 0x13));
    size_t sc = ok.len; e32(&ok, enc_sc(6, 5, 11, 0, 0));
    size_t br = ok.len; e32(&ok, enc_b((int32_t)(lr - br), 6, 0, 1, 0x63));
    e32(&ok, enc_i(1, 0, 0, 12, 0x13));
    uint64_t entry = pc_at(lr);
    lrsc_report *r = lrsc_analyze(rv64, BASE_PC, ok.b, ok.len, &entry, 1);
    TASK_CHECK(r != NULL);
    const lrsc_candidate *cd = cand_at(r, pc_at(lr));
    TASK_CHECK(cd && cd->verdict == LRSC_PROVEN);
    TASK_CHECK(cd->sc_pc == pc_at(sc));
    lrsc_report_free(r);

    /* overwrite: a1 copied from a0, then redefined from a2 before the SC */
    struct codebuf bad = { .len = 0 };
    size_t b_lr = bad.len; e32(&bad, enc_lr(5, 10, 0, 0));
    e32(&bad, enc_i(0, 10, 0, 11, 0x13));               /* mv a1,a0 */
    size_t b_def = bad.len; e32(&bad, enc_r(0, 12, 0, 0, 11, 0x33)); /* add a1,x0,a2 */
    size_t b_sc = bad.len; e32(&bad, enc_sc(6, 5, 11, 0, 0));
    size_t b_br = bad.len; e32(&bad, enc_b((int32_t)(b_lr - b_br), 6, 0, 1, 0x63));
    e32(&bad, enc_i(1, 0, 0, 12, 0x13));
    uint64_t bentry = pc_at(b_lr);
    lrsc_report *rb = lrsc_analyze(rv64, BASE_PC, bad.b, bad.len, &bentry, 1);
    TASK_CHECK(rb != NULL);
    const lrsc_candidate *cb = cand_at(rb, pc_at(b_lr));
    TASK_CHECK(cb && cb->verdict == LRSC_UNKNOWN);
    TASK_CHECK(cb->reason == LRSC_REASON_ADDR_OVERWRITTEN);
    TASK_CHECK(cb->reason_pc == pc_at(b_def));
    TASK_CHECK(cb->sc_pc == pc_at(b_sc));
    lrsc_report_free(rb);

    /* different predecessors disagree at a merge: tokens must not be spliced */
    struct codebuf dia = { .len = 0 };
    size_t d_lr = dia.len; e32(&dia, enc_lr(5, 10, 0, 0));
    size_t d_if = dia.len; e32(&dia, enc_b(12, 12, 0, 0, 0x63));
    size_t d_mv = dia.len; e32(&dia, enc_i(0, 10, 0, 11, 0x13));
    size_t d_jmp = dia.len; e32(&dia, enc_j(0, 8));
    size_t d_alt = dia.len; e32(&dia, enc_i(4, 10, 0, 11, 0x13));
    size_t d_sc = dia.len; e32(&dia, enc_sc(6, 5, 11, 0, 0));
    size_t d_br = dia.len; e32(&dia, enc_b((int32_t)(d_lr - d_br), 6, 0, 1, 0x63));
    e32(&dia, enc_i(1, 0, 0, 12, 0x13));
    uint64_t dentry = pc_at(d_lr);
    lrsc_report *rd = lrsc_analyze(rv64, BASE_PC, dia.b, dia.len, &dentry, 1);
    TASK_CHECK(rd != NULL);
    const lrsc_candidate *cc = cand_at(rd, pc_at(d_lr));
    TASK_CHECK(cc && cc->verdict == LRSC_UNKNOWN);
    TASK_CHECK(cc->reason == LRSC_REASON_ADDR_MERGE);
    TASK_CHECK(cc->reason_pc == pc_at(d_sc));
    TASK_CHECK(cc->sc_pc == pc_at(d_sc));
    lrsc_report_free(rd);
    (void)p1; (void)p2; (void)d_if; (void)d_mv; (void)d_jmp; (void)d_alt;
    return 0;
}

static int cert_sc_clobber_and_status_copies(void)
{
    /* SC writes its own address register: address is read before the def,
     * and the result token then lives in that same register */
    struct codebuf cl = { .len = 0 };
    size_t cl_lr = cl.len; e32(&cl, enc_lr(5, 10, 0, 0));
    size_t cl_sc = cl.len; e32(&cl, enc_sc(10, 5, 10, 0, 0)); /* sc.d a0,t0,(a0) */
    size_t cl_br = cl.len; e32(&cl, enc_b((int32_t)(cl_lr - cl_br), 10, 0, 1, 0x63));
    size_t cl_ex = cl.len; e32(&cl, enc_i(1, 0, 0, 12, 0x13));
    uint64_t centry = pc_at(cl_lr);
    lrsc_report *rc = lrsc_analyze(rv64, BASE_PC, cl.b, cl.len, &centry, 1);
    TASK_CHECK(rc != NULL);
    const lrsc_candidate *cc = cand_at(rc, pc_at(cl_lr));
    TASK_CHECK(cc && cc->verdict == LRSC_PROVEN);
    TASK_CHECK(cc->sc_pc == pc_at(cl_sc));
    TASK_CHECK(cc->branch_pc == pc_at(cl_br));
    TASK_CHECK(cc->fail_target == pc_at(cl_lr));
    TASK_CHECK(cc->success_target == pc_at(cl_ex));
    lrsc_report_free(rc);

    /* lossless copies: addi 0 and add x0, both in 32-bit form */
    struct codebuf cp = { .len = 0 };
    size_t cp_lr = cp.len; e32(&cp, enc_lr(5, 10, 0, 0));
    size_t cp_sc = cp.len; e32(&cp, enc_sc(6, 5, 10, 0, 0));
    size_t cp_add = cp.len; e32(&cp, enc_r(0, 6, 0, 0, 7, 0x33)); /* add t2,x0,t1 */
    size_t cp_br = cp.len; e32(&cp, enc_b((int32_t)(cp_lr - cp_br), 7, 0, 1, 0x63));
    size_t cp_ex = cp.len; e32(&cp, enc_i(1, 0, 0, 12, 0x13));
    uint64_t pentry = pc_at(cp_lr);
    lrsc_report *rp = lrsc_analyze(rv64, BASE_PC, cp.b, cp.len, &pentry, 1);
    TASK_CHECK(rp != NULL);
    const lrsc_candidate *cpd = cand_at(rp, pc_at(cp_lr));
    TASK_CHECK(cpd && cpd->verdict == LRSC_PROVEN);
    TASK_CHECK(cpd->region_count == 4);
    TASK_CHECK(cpd->sc_pc == pc_at(cp_sc));
    TASK_CHECK(cpd->success_target == pc_at(cp_ex));
    lrsc_report_free(rp);

    /* compressed status copy: c.mv s1,t1 then c.bnez s1 */
    struct codebuf cc2 = { .len = 0 };
    size_t c2_lr = cc2.len; e32(&cc2, enc_lr(5, 10, 0, 0));
    size_t c2_sc = cc2.len; e32(&cc2, enc_sc(6, 5, 10, 0, 0));
    size_t c2_mv = cc2.len; e16(&cc2, enc_cmv(9, 6));
    size_t c2_br = cc2.len; e16(&cc2, enc_cbz(1, 9, (int32_t)(c2_lr - c2_br)));
    size_t c2_ex = cc2.len; e32(&cc2, enc_i(0, 0, 0, 0, 0x13));
    uint64_t c2entry = pc_at(c2_lr);
    lrsc_report *r2 = lrsc_analyze(rv64, BASE_PC, cc2.b, cc2.len, &c2entry, 1);
    TASK_CHECK(r2 != NULL);
    const lrsc_candidate *c2 = cand_at(r2, pc_at(c2_lr));
    TASK_CHECK(c2 && c2->verdict == LRSC_PROVEN);
    TASK_CHECK(c2->branch_pc == pc_at(c2_br) && c2->sc_pc == pc_at(c2_sc));
    TASK_CHECK(c2->success_target == pc_at(c2_ex));
    lrsc_report_free(r2);

    /* overwritten status cannot confirm a retry */
    struct codebuf lost = { .len = 0 };
    size_t l_lr = lost.len; e32(&lost, enc_lr(5, 10, 0, 0));
    size_t l_sc = lost.len; e32(&lost, enc_sc(6, 5, 10, 0, 0));
    size_t l_def = lost.len; e32(&lost, enc_i(1, 0, 0, 6, 0x13)); /* t1 = 1 */
    size_t l_br = lost.len; e32(&lost, enc_b((int32_t)(l_lr - l_br), 6, 0, 1, 0x63));
    e32(&lost, enc_i(1, 0, 0, 12, 0x13));
    uint64_t lentry = pc_at(l_lr);
    lrsc_report *rl = lrsc_analyze(rv64, BASE_PC, lost.b, lost.len, &lentry, 1);
    TASK_CHECK(rl != NULL);
    const lrsc_candidate *lc = cand_at(rl, pc_at(l_lr));
    TASK_CHECK(lc && lc->verdict == LRSC_UNKNOWN);
    TASK_CHECK(lc->reason == LRSC_REASON_STATUS_LOST);
    TASK_CHECK(lc->reason_pc == pc_at(l_br));
    TASK_CHECK(lc->branch_pc == 0 && lc->sc_pc == pc_at(l_sc));
    lrsc_report_free(rl);

    /* a status produced by no SC here (t2) is foreign to this SC's token */
    struct codebuf fr = { .len = 0 };
    size_t f_lr = fr.len; e32(&fr, enc_lr(5, 10, 0, 0));
    size_t f_sc = fr.len; e32(&fr, enc_sc(6, 5, 10, 0, 0));
    size_t f_br = fr.len; e32(&fr, enc_b((int32_t)(f_lr - f_br), 7, 0, 1, 0x63));
    e32(&fr, enc_i(1, 0, 0, 12, 0x13));
    uint64_t fentry = pc_at(f_lr);
    lrsc_report *rf = lrsc_analyze(rv64, BASE_PC, fr.b, fr.len, &fentry, 1);
    TASK_CHECK(rf != NULL);
    const lrsc_candidate *fc = cand_at(rf, pc_at(f_lr));
    TASK_CHECK(fc && fc->verdict == LRSC_UNKNOWN);
    TASK_CHECK(fc->reason == LRSC_REASON_STATUS_LOST);
    TASK_CHECK(fc->reason_pc == pc_at(f_br) && fc->sc_pc == pc_at(f_sc));
    lrsc_report_free(rf);

    /* merge conflict: one predecessor keeps the SC token, the other overwrote
     * it; equivalence on a single predecessor must not confirm the retry */
    struct codebuf mg = { .len = 0 };
    size_t m_lr = mg.len; e32(&mg, enc_lr(5, 10, 0, 0));
    size_t m_sc = mg.len; e32(&mg, enc_sc(6, 5, 10, 0, 0));
    size_t m_if = mg.len; e32(&mg, enc_b(12, 12, 0, 0, 0x63)); /* beqz a2 -> branch */
    size_t m_def = mg.len; e32(&mg, enc_i(1, 0, 0, 6, 0x13));  /* t1 = 1 */
    size_t m_j = mg.len; e32(&mg, enc_j(0, 4));                /* j -> branch */
    size_t m_br = mg.len; e32(&mg, enc_b((int32_t)(m_lr - m_br), 6, 0, 1, 0x63));
    e32(&mg, enc_i(1, 0, 0, 12, 0x13));
    uint64_t mentry = pc_at(m_lr);
    lrsc_report *rm = lrsc_analyze(rv64, BASE_PC, mg.b, mg.len, &mentry, 1);
    TASK_CHECK(rm != NULL);
    const lrsc_candidate *mc = cand_at(rm, pc_at(m_lr));
    TASK_CHECK(mc && mc->verdict == LRSC_UNKNOWN);
    TASK_CHECK(mc->reason == LRSC_REASON_STATUS_MERGE);
    TASK_CHECK(mc->reason_pc == pc_at(m_br) && mc->sc_pc == pc_at(m_sc));
    lrsc_report_free(rm);
    (void)cl_ex; (void)cp_add; (void)c2_mv; (void)l_def;
    (void)m_if; (void)m_def; (void)m_j;
    return 0;
}

static int cert_fail_and_success_edge_direction(void)
{
    /* bne taken must return to the LR; here it points forward at the exit */
    struct codebuf wrong = { .len = 0 };
    size_t w_lr = wrong.len; e32(&wrong, enc_lr(5, 10, 0, 0));
    size_t w_sc = wrong.len; e32(&wrong, enc_sc(6, 5, 10, 0, 0));
    size_t w_br = wrong.len; e32(&wrong, enc_b(8, 6, 0, 1, 0x63)); /* bne +8 */
    size_t w_j = wrong.len; e32(&wrong, enc_j(0, (int32_t)(w_lr - w_j)));
    size_t w_ex = wrong.len; e32(&wrong, enc_i(1, 0, 0, 12, 0x13));
    uint64_t wentry = pc_at(w_lr);
    lrsc_report *rw = lrsc_analyze(rv64, BASE_PC, wrong.b, wrong.len, &wentry, 1);
    TASK_CHECK(rw != NULL);
    const lrsc_candidate *wc = cand_at(rw, pc_at(w_lr));
    TASK_CHECK(wc && wc->verdict == LRSC_UNKNOWN);
    TASK_CHECK(wc->reason == LRSC_REASON_FAIL_EDGE);
    TASK_CHECK(wc->reason_pc == pc_at(w_br));
    TASK_CHECK(wc->sc_pc == pc_at(w_sc));
    TASK_CHECK(wc->fail_target == pc_at(w_ex));
    TASK_CHECK(wc->success_target == pc_at(w_lr));
    lrsc_report_free(rw);

    /* beq zero-compare with a c.j retry tail: success leaves, failure retries */
    struct codebuf ok = { .len = 0 };
    size_t o_lr = ok.len; e32(&ok, enc_lr(5, 10, 0, 0));
    size_t o_sc = ok.len; e32(&ok, enc_sc(6, 5, 10, 0, 0));
    size_t o_br = ok.len; e32(&ok, enc_b(8, 0, 6, 0, 0x63));   /* beq x0,t1,+8 */
    size_t o_j = ok.len; e16(&ok, enc_cj((int32_t)(o_lr - o_j)));
    size_t o_pad = ok.len; e16(&ok, enc_caddi(12, 1));
    size_t o_ex = ok.len; e32(&ok, enc_i(1, 0, 0, 13, 0x13));
    uint64_t oentry = pc_at(o_lr);
    lrsc_report *ro = lrsc_analyze(rv64, BASE_PC, ok.b, ok.len, &oentry, 1);
    TASK_CHECK(ro != NULL);
    const lrsc_candidate *oc = cand_at(ro, pc_at(o_lr));
    TASK_CHECK(oc && oc->verdict == LRSC_PROVEN);
    TASK_CHECK(oc->sc_pc == pc_at(o_sc));
    TASK_CHECK(oc->branch_pc == pc_at(o_br));
    TASK_CHECK(oc->fail_target == pc_at(o_lr));
    TASK_CHECK(oc->success_target == pc_at(o_ex));
    TASK_CHECK(!region_has_pc(oc, pc_at(o_ex)));
    TASK_CHECK(!region_has_pc(oc, pc_at(o_pad)));
    TASK_CHECK(region_has_pc(oc, pc_at(o_j)));
    lrsc_report_free(ro);

    /* a signed BLT against x0 is not a BEQ/BNE zero test: no status proof */
    struct codebuf lt = { .len = 0 };
    size_t t_lr = lt.len; e32(&lt, enc_lr(5, 10, 0, 0));
    size_t t_sc = lt.len; e32(&lt, enc_sc(6, 5, 10, 0, 0));
    size_t t_br = lt.len; e32(&lt, enc_b((int32_t)(t_lr - t_br), 6, 0, 4, 0x63)); /* blt t1,x0 */
    e32(&lt, enc_i(1, 0, 0, 12, 0x13));
    uint64_t tentry = pc_at(t_lr);
    lrsc_report *rt = lrsc_analyze(rv64, BASE_PC, lt.b, lt.len, &tentry, 1);
    TASK_CHECK(rt != NULL);
    const lrsc_candidate *tc = cand_at(rt, pc_at(t_lr));
    TASK_CHECK(tc && tc->verdict == LRSC_UNKNOWN);
    TASK_CHECK(tc->reason == LRSC_REASON_NO_STATUS_BRANCH);
    TASK_CHECK(tc->branch_pc == 0 && tc->sc_pc == pc_at(t_sc));
    lrsc_report_free(rt);
    return 0;
}

static int cert_independent_candidates_do_not_share_state(void)
{
    /* two self-contained loops in one image; the first exits to the second */
    struct codebuf c = { .len = 0 };
    size_t lr1 = c.len; e32(&c, enc_lr(5, 10, 0, 0));
    size_t sc1 = c.len; e32(&c, enc_sc(6, 5, 10, 0, 0));
    size_t br1 = c.len; e32(&c, enc_b((int32_t)(lr1 - br1), 6, 0, 1, 0x63));
    size_t lr2 = c.len; e32(&c, enc_lr(28, 11, 0, 0));
    size_t sc2 = c.len; e32(&c, enc_sc(29, 28, 11, 0, 0));
    size_t br2 = c.len; e32(&c, enc_b((int32_t)(lr2 - br2), 29, 0, 1, 0x63));
    e32(&c, enc_i(1, 0, 0, 12, 0x13));

    uint64_t entries[2] = { pc_at(lr1), pc_at(lr2) };
    lrsc_report *r = lrsc_analyze(rv64, BASE_PC, c.b, c.len, entries, 2);
    TASK_CHECK(r != NULL);
    size_t nc = 0;
    TASK_CHECK(lrsc_report_candidates(r, &nc) != NULL && nc == 2);

    const lrsc_candidate *a = cand_at(r, pc_at(lr1));
    const lrsc_candidate *b = cand_at(r, pc_at(lr2));
    TASK_CHECK(a && b);
    TASK_CHECK(a->verdict == LRSC_PROVEN && b->verdict == LRSC_PROVEN);
    TASK_CHECK(a->sc_pc == pc_at(sc1) && b->sc_pc == pc_at(sc2));
    TASK_CHECK(a->fail_target == pc_at(lr1) && b->fail_target == pc_at(lr2));

    /* disjoint regions: neither candidate borrows the other's nodes */
    for (uint32_t i = 0; i < a->region_count; i++)
        TASK_CHECK(!region_has_pc(b, a->region_pc[i]));
    for (uint32_t i = 0; i < b->region_count; i++)
        TASK_CHECK(!region_has_pc(a, b->region_pc[i]));

    lrsc_report_free(r);
    return 0;
}

/* --------------------------------------------------------------- */
/* registration                                                      */
/* --------------------------------------------------------------- */

const struct task_case task_cases[] = {
    { "decode", "canonical-fields",            decode_canonical_fields },
    { "decode", "compressed-and-edges",        decode_compressed_canonical_and_edges },
    { "decode", "ownership-and-stability",     decode_report_owns_input_and_is_stable },
    { "pairing", "mixed-width-proven",         pairing_mixed_width_proven },
    { "pairing", "intervening-lr-and-width",   pairing_intervening_lr_and_width },
    { "pairing", "structural-violations",      pairing_structural_violations },
    { "pairing", "length-limit-and-missing-sc", pairing_length_limit_and_missing_sc },
    { "certificate", "address-copy-overwrite-merge", cert_address_signed_offset_copy_and_overwrite },
    { "certificate", "sc-clobber-and-status-copies", cert_sc_clobber_and_status_copies },
    { "certificate", "fail-success-edge-direction",  cert_fail_and_success_edge_direction },
    { "certificate", "candidate-state-isolation",    cert_independent_candidates_do_not_share_state },
};
const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
