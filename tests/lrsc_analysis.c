/*
 * Static LR/SC retry-region analysis tests.
 *
 * Every instruction byte below is built here with small local encoders;
 * nothing is executed. The earlier print-oriented tests (tests/t1.c,
 * tests/t2.c, task_support/legacy_decode.c) only assert disassembly text
 * substrings and raw instruction lengths. They leave these gaps, which the
 * cases in this file cover:
 *
 *   - no structured canonical decode (op/rd/rs1/rs2/imm/aq/rl, raw length,
 *     PC, compressed flag) independent of the assembly string;
 *   - no control-flow recovery, reachability or direct-edge enumeration;
 *   - no LR/SC candidate pairing, width checks or region constraints;
 *   - no entry-register-relative address lineage across copies, offsets,
 *     clobbers and control-flow merges;
 *   - no SC success/failure status tracking through lossless copies;
 *   - no proven/unconstrained/unknown verdict with decisive PC evidence.
 */

#include "check.h"
#include "riscv-disas.h"
#include "rv_struct_decode.h"
#include "rv_lrsc.h"

#include <string.h>

/* ----------------------------- encoders ----------------------------- */

static uint32_t enc_r(uint32_t op, uint32_t f3, uint32_t f7,
    unsigned rd, unsigned rs1, unsigned rs2)
{
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
        (rd << 7) | op;
}
static uint32_t enc_i(uint32_t op, uint32_t f3,
    unsigned rd, unsigned rs1, int imm)
{
    return ((uint32_t)(imm & 0xfff) << 20) | (rs1 << 15) | (f3 << 12) |
        (rd << 7) | op;
}
static uint32_t enc_b(uint32_t f3, unsigned rs1, unsigned rs2, int imm)
{
    uint32_t u = (uint32_t)imm;
    return ((u >> 12) & 1) << 31 | ((u >> 5) & 0x3f) << 25 |
        (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
        ((u >> 1) & 0xf) << 8 | ((u >> 11) & 1) << 7 | 0x63;
}
static uint32_t enc_lr(unsigned d, unsigned rs1, int dbl, int aq, int rl)
{
    return (0x02u << 27) | (aq << 26) | (rl << 25) | (rs1 << 15) |
        ((dbl ? 3 : 2) << 12) | (d << 7) | 0x2f;
}
static uint32_t enc_sc(unsigned d, unsigned rs1, unsigned rs2,
    int dbl, int aq, int rl)
{
    return (0x03u << 27) | (aq << 26) | (rl << 25) | (rs2 << 20) |
        (rs1 << 15) | ((dbl ? 3 : 2) << 12) | (d << 7) | 0x2f;
}
static uint16_t enc_cj(int imm)
{
    uint32_t u = (uint32_t)imm;
    uint16_t h = 0xa001u;
    h |= ((u >> 11) & 1) << 12;
    h |= ((u >> 4) & 1) << 11;
    h |= ((u >> 8) & 3) << 9;
    h |= ((u >> 10) & 1) << 8;
    h |= ((u >> 6) & 1) << 7;
    h |= ((u >> 7) & 1) << 6;
    h |= ((u >> 1) & 7) << 3;
    h |= ((u >> 5) & 1) << 2;
    return h;
}
static uint16_t enc_cbranch(int nez, unsigned rs1p, int imm)
{
    uint32_t u = (uint32_t)imm;
    uint16_t h = (nez ? 0xe001u : 0xc001u);
    h |= ((u >> 8) & 1) << 12;
    h |= ((u >> 3) & 3) << 10;
    h |= (rs1p & 7) << 7;
    h |= ((u >> 6) & 3) << 5;
    h |= ((u >> 1) & 3) << 3;
    h |= ((u >> 5) & 1) << 2;
    return h;
}
static uint16_t enc_cmv(unsigned rd, unsigned rs2)
{
    return 0x8002u | ((rd & 31) << 7) | ((rs2 & 31) << 2);
}
static uint16_t enc_cadd(unsigned rd, unsigned rs2)
{
    return 0x9002u | ((rd & 31) << 7) | ((rs2 & 31) << 2);
}
static uint16_t enc_caddi(unsigned rd, int imm)
{
    uint32_t u = (uint32_t)imm;
    return (uint16_t)(((rd & 31) << 7) | (((u >> 5) & 1) << 12) |
        ((u & 0x1f) << 2) | 1);
}

/* --------------------------- program builder ------------------------ */

enum { FX_BNEZ, FX_BEQZ, FX_CJ, FX_CBNEZ, FX_CBEQZ };
typedef struct { size_t site; int kind; unsigned a; size_t lab; } fixup;
typedef struct {
    uint8_t  b[512];
    size_t   n;
    unsigned pcs[80];
    size_t   np;
    fixup    fx[80];
    size_t   nfx;
} prog;

static void p32(prog *p, uint32_t w)
{
    p->pcs[p->np++] = (unsigned)p->n;
    p->b[p->n++] = w & 255; p->b[p->n++] = (w >> 8) & 255;
    p->b[p->n++] = (w >> 16) & 255; p->b[p->n++] = (w >> 24) & 255;
}
static void p16(prog *p, uint16_t h)
{
    p->pcs[p->np++] = (unsigned)p->n;
    p->b[p->n++] = h & 255; p->b[p->n++] = (h >> 8) & 255;
}
static void fxadd(prog *p, int kind, unsigned a, size_t lab, size_t site)
{
    p->fx[p->nfx++] = (fixup){ site, kind, a, lab };
}
static void fxapply(prog *p)
{
    size_t i;
    for (i = 0; i < p->nfx; i++) {
        fixup *f = &p->fx[i];
        int imm = (int)p->pcs[f->lab] - (int)f->site;
        if (f->kind == FX_CJ) {
            uint16_t h = enc_cj(imm);
            p->b[f->site] = h & 255; p->b[f->site + 1] = (h >> 8) & 255;
        } else if (f->kind == FX_CBNEZ || f->kind == FX_CBEQZ) {
            uint16_t h = enc_cbranch(f->kind == FX_CBNEZ,
                f->a & 7, imm);
            p->b[f->site] = h & 255; p->b[f->site + 1] = (h >> 8) & 255;
        } else {
            uint32_t w = enc_b(f->kind == FX_BNEZ ? 1 : 0,
                f->a, 0, imm);
            p->b[f->site + 0] = w & 255; p->b[f->site + 1] = (w >> 8) & 255;
            p->b[f->site + 2] = (w >> 16) & 255;
            p->b[f->site + 3] = (w >> 24) & 255;
        }
    }
}

#define ADDI(rd,rs1,imm) p32(&P, enc_i(0x13,0,(rd),(rs1),(imm)))
#define LUI(rd,imm20)    p32(&P, (((uint32_t)(imm20) << 12) | ((rd) << 7) | 0x37u))
#define ADD(rd,rs1,rs2)  p32(&P, enc_r(0x33,0,0,(rd),(rs1),(rs2)))
#define MUL(rd,rs1,rs2)  p32(&P, enc_r(0x33,0,1,(rd),(rs1),(rs2)))
#define LDD(rd,off,rs1)  p32(&P, enc_i(0x03,3,(rd),(rs1),(off)))
#define BNEZ(rs,lab)     do{size_t s_=P.n; p32(&P,0); fxadd(&P,FX_BNEZ,(rs),(lab),s_);}while(0)
#define BEQZ(rs,lab)     do{size_t s_=P.n; p32(&P,0); fxadd(&P,FX_BEQZ,(rs),(lab),s_);}while(0)
#define CJ(lab)          do{size_t s_=P.n; p16(&P,0); fxadd(&P,FX_CJ,0,(lab),s_);}while(0)
#define CBNEZ(rs1p,lab)  do{size_t s_=P.n; p16(&P,0); fxadd(&P,FX_CBNEZ,(rs1p),(lab),s_);}while(0)
#define CBEQZ(rs1p,lab)  do{size_t s_=P.n; p16(&P,0); fxadd(&P,FX_CBEQZ,(rs1p),(lab),s_);}while(0)
#define CBEQZ(rs1p,lab)  do{size_t s_=P.n; p16(&P,0); fxadd(&P,FX_CBEQZ,(rs1p),(lab),s_);}while(0)
#define LRD(d,rs1)       p32(&P, enc_lr((d),(rs1),1,1,0))
#define LRDNQ(d,rs1,aq,rl) p32(&P, enc_lr((d),(rs1),1,(aq),(rl)))
#define LRW(d,rs1)       p32(&P, enc_lr((d),(rs1),0,0,0))
#define SCD(d,rs1,rs2)   p32(&P, enc_sc((d),(rs1),(rs2),1,0,1))
#define SCDNQ(d,rs1,rs2,aq,rl) p32(&P, enc_sc((d),(rs1),(rs2),1,(aq),(rl)))
#define SCW(d,rs1,rs2)   p32(&P, enc_sc((d),(rs1),(rs2),0,0,0))
#define CMV(rd,rs2)      p16(&P, enc_cmv((rd),(rs2)))
#define CADD(rd,rs2)     p16(&P, enc_cadd((rd),(rs2)))
#define CADDI(rd,imm)    p16(&P, enc_caddi((rd),(imm)))

/* ------------------------------ helpers ----------------------------- */

static const uint64_t BASE = 0x4000;

static int analyze_prog(const prog *p, rv_code_block *blk, rv_lrsc_result **res,
    const uint64_t *entries, size_t nentries)
{
    uint64_t err = 0;
    if (rv_code_block_decode(blk, BASE, p->b, p->n, &err) != 0)
        return -1;
    *res = rv_lrsc_analyze(blk, entries, nentries);
    return *res ? 0 : -1;
}

static int has_reason(const rv_lrsc_candidate *c, rv_lrsc_reason_kind k)
{
    size_t i;
    for (i = 0; i < c->reason_count; i++)
        if (c->reasons[i].kind == k) return 1;
    return 0;
}

static int has_reason_at(const rv_lrsc_candidate *c,
    rv_lrsc_reason_kind k, uint64_t relpc)
{
    size_t i;
    for (i = 0; i < c->reason_count; i++)
        if (c->reasons[i].kind == k &&
            c->reasons[i].pc == BASE + relpc)
            return 1;
    return 0;
}

/* ============================ decode group ========================== */

static int decode_canonical_fields(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;

    LRD(5, 10);            /* 0: lr.d.aq  t0,(a0) */
    ADDI(11, 10, 0);       /* 4: addi a1,a0,0 (canonical, not lifted to mv) */
    CADDI(5, 1);           /* 8: c.addi t0,1 */
    SCD(6, 11, 5);         /* a: sc.d.rl t1,t0,(a1) */
    BNEZ(6, 0);            /* e */
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(blk.count == 5);

    /* raw length / PC / compressed flag, in decode order */
    TASK_CHECK(blk.recs[0].len == 4 && blk.recs[0].compressed == 0);
    TASK_CHECK(blk.recs[0].offset == 0 &&
               blk.recs[0].dec.pc == BASE);
    TASK_CHECK(blk.recs[2].len == 2 && blk.recs[2].compressed == 1);
    TASK_CHECK(blk.recs[2].offset == 8 &&
               blk.recs[2].dec.pc == BASE + 8);

    /* canonical op/operands before pseudo lifting */
    TASK_CHECK(blk.recs[0].dec.op == rv_op_lr_d);
    TASK_CHECK(blk.recs[0].dec.rd == 5 && blk.recs[0].dec.rs1 == 10);
    TASK_CHECK(blk.recs[0].dec.aq == 1 && blk.recs[0].dec.rl == 0);
    TASK_CHECK(blk.recs[1].dec.op == rv_op_addi);
    TASK_CHECK(blk.recs[1].dec.rd == 11 && blk.recs[1].dec.rs1 == 10 &&
               blk.recs[1].dec.imm == 0);
    TASK_CHECK(blk.recs[2].dec.op == rv_op_addi); /* c.addi decompresses */
    TASK_CHECK(blk.recs[2].dec.rd == 5 && blk.recs[2].dec.imm == 1);
    TASK_CHECK(blk.recs[3].dec.op == rv_op_sc_d);
    TASK_CHECK(blk.recs[3].dec.rd == 6 && blk.recs[3].dec.rs1 == 11 &&
               blk.recs[3].dec.rs2 == 5);
    TASK_CHECK(blk.recs[3].dec.aq == 0 && blk.recs[3].dec.rl == 1);
    TASK_CHECK(blk.recs[4].dec.op == rv_op_bne);
    TASK_CHECK(blk.recs[4].dec.rs1 == 6 && blk.recs[4].dec.rs2 == 0);

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int decode_disasm_shares_path(void)
{
    /* The canonical entry and disasm_inst must decode the same word: the
     * canonical op stays ADDI while the printing path lifts it to "mv". */
    rv_decode dec;
    char txt[96];
    uint32_t word = enc_i(0x13, 0, 11, 10, 0); /* addi a1,a0,0 */
    size_t len = rv_decode_canonical(&dec, rv64, BASE, word);

    TASK_CHECK(len == 4);
    TASK_CHECK(dec.op == rv_op_addi && dec.rd == 11 && dec.rs1 == 10 &&
               dec.imm == 0);
    disasm_inst(txt, sizeof txt, rv64, BASE, word);
    TASK_CHECK(strstr(txt, "mv") != NULL);
    return 0;
}

static int decode_self_contained(void)
{
    prog P = {0};
    uint8_t local[512];
    rv_code_block blk;
    uint64_t pc;
    size_t i;

    LRD(5, 10);
    SCD(6, 10, 5);
    BNEZ(6, 0);
    fxapply(&P);
    memcpy(local, P.b, P.n);

    TASK_CHECK(rv_code_block_decode(&blk, BASE, local, P.n, NULL) == 0);
    /* destroy every caller-owned byte and the stack backing store */
    memset(local, 0xAB, sizeof local);
    memset(P.b, 0xCD, sizeof P.b);

    TASK_CHECK(blk.count == 3);
    for (i = 0; i < blk.count; i++) {
        TASK_CHECK(blk.recs[i].dec.op ==
                   (i == 0 ? rv_op_lr_d : i == 1 ? rv_op_sc_d : rv_op_bne));
    }
    TASK_CHECK(rv_code_block_find(&blk, BASE + 4) == 1);
    TASK_CHECK(rv_code_block_find(&blk, BASE + 5) == -1); /* mid-instruction */
    TASK_CHECK(rv_code_rec_branch_target(&blk, 2, &pc) == 0);
    TASK_CHECK(pc == BASE);

    rv_code_block_free(&blk);
    return 0;
}

static int decode_mixed_width_offsets(void)
{
    prog P = {0};
    rv_code_block blk;
    uint64_t err = 0;
    static const unsigned lens[6] = { 4, 2, 2, 4, 2, 4 };
    static const unsigned offs[6] = { 0, 4, 6, 8, 12, 14 };
    size_t i;

    LRD(5, 10);      /* 0  */
    CADDI(5, 1);     /* 4  */
    CMV(11, 10);     /* 6  */
    SCD(6, 11, 5);   /* 8  */
    CADD(6, 6);      /* c  */
    BNEZ(6, 0);      /* e  */
    fxapply(&P);

    TASK_CHECK(rv_code_block_decode(&blk, BASE, P.b, P.n, &err) == 0);
    TASK_CHECK(blk.count == 6);
    for (i = 0; i < 6; i++) {
        TASK_CHECK(blk.recs[i].len == lens[i]);
        TASK_CHECK(blk.recs[i].offset == offs[i]);
        TASK_CHECK(blk.recs[i].dec.pc == BASE + offs[i]);
        TASK_CHECK(blk.recs[i].compressed == (lens[i] == 2));
    }
    rv_code_block_free(&blk);
    return 0;
}

/* =========================== pairing group ========================== */

static int pair_proven_basic(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    LRD(5, 10);
    ADDI(11, 10, 0);
    CADDI(5, 1);
    SCD(6, 11, 5);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidate_count == 1);

    /* The result must own its data: release the block (and thus the input
     * code), then read every reported field from the result alone. */
    rv_code_block_free(&blk);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(c->width == 8);
    TASK_CHECK(c->lr_rec.dec.pc == BASE);
    TASK_CHECK(c->lr_rec.len == 4);
    TASK_CHECK(c->lr_rec.dec.op == rv_op_lr_d);
    TASK_CHECK(c->sc_rec.dec.pc == BASE + 0x0a);
    TASK_CHECK(c->branch_rec.dec.pc == BASE + 0x0e);
    TASK_CHECK(c->region_count == 5);
    TASK_CHECK(c->region_pcs[0] == BASE);
    TASK_CHECK(c->reason_count == 0);
    TASK_CHECK(c->fail_edge_to == BASE);

    rv_lrsc_result_free(res);
    return 0;
}

static int pair_addr_copy_chain_and_offset(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    /* a0 -> c.mv a1,a0 -> add a2,a1,x0 -> addi a3,a2,-8 ; SC uses a3.
     * The LR address is (a0,0); the SC sees (a0,-8): mismatch. */
    LRD(5, 10);
    CMV(11, 10);
    ADD(12, 11, 0);
    ADDI(13, 12, -8);
    SCD(6, 13, 5);
    BNEZ(6, 0);
    ADDI(14, 0, 1);
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_UNKNOWN);
    TASK_CHECK(has_reason(c, RV_LRSC_REASON_ADDR_MISMATCH));

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* same chain but zero net displacement: the symbolic addresses join. */
    memset(&P, 0, sizeof P);
    LRD(5, 10);
    CMV(11, 10);
    ADD(12, 11, 0);
    ADDI(13, 12, 0);
    SCD(6, 13, 5);
    BNEZ(6, 0);
    ADDI(14, 0, 1);
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(!has_reason(c, RV_LRSC_REASON_ADDR_MISMATCH));
    TASK_CHECK(!has_reason(c, RV_LRSC_REASON_ADDR_UNKNOWN));

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int pair_addr_clobber(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    LRD(5, 10);
    ADDI(10, 10, 4);   /* a0, the LR address register, is overwritten */
    SCD(6, 10, 5);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_UNKNOWN);
    TASK_CHECK(has_reason_at(c, RV_LRSC_REASON_ADDR_MISMATCH, 0x08));

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* LUI builds the SC address from a constant: its lineage cannot be tied
     * to any entry register, so the proof is unknown rather than failed. */
    memset(&P, 0, sizeof P);
    LRD(5, 10);          /* 0 */
    LUI(11, 0x123);      /* 4 a1 = 0x123000, no symbolic base */
    SCD(6, 11, 5);       /* 8 */
    BNEZ(6, 0);          /* c */
    ADDI(12, 0, 1);      /* 10 */
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_UNKNOWN);
    TASK_CHECK(has_reason_at(c, RV_LRSC_REASON_ADDR_UNKNOWN, 0x08));
    TASK_CHECK(!has_reason(c, RV_LRSC_REASON_FORBIDDEN_INSN));

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int pair_merge_conflicting_predecessors(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    LRD(5, 10);                 /* 0 */
    BEQZ(7, 3);                 /* 4 fork on an unrelated value t2 */
    ADDI(11, 10, 0);            /* 8 path A: a1 = a0 */
    ADDI(11, 10, 16);           /* c path B (fall-through): a1 = a0+16 */
    SCD(6, 11, 5);              /* 10 merge: address differs across paths */
    BNEZ(6, 0);                 /* 14 */
    ADDI(12, 0, 1);             /* 18 */
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_UNKNOWN);
    TASK_CHECK(has_reason(c, RV_LRSC_REASON_ADDR_MISMATCH));
    TASK_CHECK(!has_reason(c, RV_LRSC_REASON_BACKWARD_EDGE));

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int pair_intervening_lr_and_width(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;

    /* two LRs before one SC: each LR gets a candidate, neither can pair */
    LRD(5, 10);
    LRD(7, 10);
    SCD(6, 10, 5);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidate_count == 2);
    TASK_CHECK(res->candidates[0].verdict ==
               RV_LRSC_VERDICT_UNCONSTRAINED);
    TASK_CHECK(has_reason(&res->candidates[0],
               RV_LRSC_REASON_INTERVENING_LR));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* width mismatch: lr.w paired with sc.d */
    memset(&P, 0, sizeof P);
    LRW(5, 10);
    SCD(6, 10, 5);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidates[0].verdict ==
               RV_LRSC_VERDICT_UNCONSTRAINED);
    TASK_CHECK(res->candidates[0].width == 4);
    TASK_CHECK(has_reason(&res->candidates[0],
               RV_LRSC_REASON_WIDTH_MISMATCH));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* matching .w widths still pair */
    memset(&P, 0, sizeof P);
    LRW(5, 10);
    SCW(6, 10, 5);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidates[0].verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(res->candidates[0].width == 4);
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int pair_forbidden_and_no_sc(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;

    /* an ordinary load inside the region */
    LRD(5, 10);
    LDD(7, 0, 10);
    SCD(6, 10, 5);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidates[0].verdict ==
               RV_LRSC_VERDICT_UNCONSTRAINED);
    TASK_CHECK(has_reason(&res->candidates[0],
               RV_LRSC_REASON_FORBIDDEN_INSN));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* M-extension multiply inside the region */
    memset(&P, 0, sizeof P);
    LRD(5, 10);
    MUL(7, 5, 5);
    SCD(6, 10, 7);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidates[0].verdict ==
               RV_LRSC_VERDICT_UNCONSTRAINED);
    TASK_CHECK(has_reason(&res->candidates[0],
               RV_LRSC_REASON_FORBIDDEN_INSN));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* LR with no SC on any forward path: unknown, never "must fail" */
    memset(&P, 0, sizeof P);
    LRD(5, 10);
    CADDI(5, 1);
    CJ(0);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidates[0].verdict == RV_LRSC_VERDICT_UNKNOWN);
    TASK_CHECK(has_reason(&res->candidates[0], RV_LRSC_REASON_NO_SC));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int pair_external_entry_bypass(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entries[2];
    rv_lrsc_candidate *c;

    /* A second entry points straight at the SC, bypassing the LR. The
     * region body then has an external entry and cannot be certified. */
    LRD(5, 10);          /* 0 label 0 */
    ADDI(11, 10, 0);     /* 4 label 1 */
    SCD(6, 11, 5);       /* 8 label 2: second entry lands here */
    BNEZ(6, 0);          /* c label 3 */
    ADDI(12, 0, 1);      /* 10 label 4 */
    fxapply(&P);

    entries[0] = BASE;
    entries[1] = BASE + P.pcs[2];
    TASK_CHECK(analyze_prog(&P, &blk, &res, entries, 2) == 0);
    TASK_CHECK(res->entry_count == 2);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_UNCONSTRAINED);
    TASK_CHECK(has_reason_at(c, RV_LRSC_REASON_EXTERNAL_ENTRY, 0x08));

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* same bytes, but the only entry is the LR: now it certifies. */
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entries[0], 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(!has_reason(c, RV_LRSC_REASON_EXTERNAL_ENTRY));

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int pair_region_too_long(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    int k;

    LRD(5, 10);
    for (k = 0; k < 15; k++)
        ADDI(7, 7, 1);      /* allowed arithmetic, but exceeds the budget */
    SCD(6, 10, 5);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidates[0].verdict ==
               RV_LRSC_VERDICT_UNCONSTRAINED);
    TASK_CHECK(has_reason(&res->candidates[0],
               RV_LRSC_REASON_REGION_TOO_LONG));

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

/* ========================= certificate group ======================= */

static int cert_bne_and_beq_directions(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    /* BNE: non-zero status takes the branch back to the LR */
    LRD(5, 10);
    SCD(6, 10, 5);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(c->fail_edge_to == BASE);              /* back to LR */
    TASK_CHECK(c->success_edge_to == BASE + 0x0c);    /* leaves region */
    TASK_CHECK(c->fail_edge_from == BASE + 0x08);
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* BEQ: zero status branches forward to the leave path, non-zero falls
     * through into a C.J that returns to the LR. Direction must come from
     * the opcode, not from where the instructions sit. */
    memset(&P, 0, sizeof P);
    LRD(5, 10);          /* 0 label 0 */
    SCD(6, 10, 5);       /* 4 label 1 */
    BEQZ(6, 4);          /* 8 label 2: success -> label 4 */
    CJ(0);               /* c label 3: retry (fall-through of the BEQ) */
    ADDI(12, 0, 1);      /* 10 label 4: leave target */
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(c->fail_edge_to == BASE);              /* via the C.J */
    TASK_CHECK(c->success_edge_to == BASE + 0x0e);
    TASK_CHECK(c->fail_edge_from == BASE + 0x0c);     /* relayed by C.J */
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int cert_compressed_branch_and_jump(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    LRD(15, 10);         /* lr.d a5,(a0): status lands in x8..x15 range */
    SCD(15, 10, 15);     /* sc.d a5 */
    CBNEZ(7, 0);         /* c.bnez a5 -> retry (a5 is x15, rs1p=7) */
    CJ(4);               /* c.j leave */
    ADDI(12, 0, 1);
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(c->branch_rec.compressed == 1);
    TASK_CHECK(c->fail_edge_to == BASE);
    /* the success edge is relayed through the C.J (0x0a) to the ADDI (0x0c) */
    TASK_CHECK(c->success_edge_from == BASE + 0x0a);
    TASK_CHECK(c->success_edge_to == BASE + 0x0c);

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* C.BEQZ: the zero (success) condition takes the branch forward; the
     * non-zero fall-through is relayed back through the C.J. This pins the
     * compressed branch direction independently of instruction order. */
    memset(&P, 0, sizeof P);
    LRD(15, 10);         /* 0 label 0 */
    SCD(15, 10, 15);     /* 4 label 1 */
    CBEQZ(7, 4);         /* 8 label 2: success -> label 4 */
    CJ(0);               /* a label 3: non-zero retry via C.J */
    ADDI(12, 0, 1);      /* c label 4: leave target */
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(c->branch_rec.compressed == 1);
    TASK_CHECK(c->success_edge_from == BASE + 0x08);
    TASK_CHECK(c->success_edge_to == BASE + 0x0c);
    TASK_CHECK(c->fail_edge_from == BASE + 0x0a); /* relayed by C.J */
    TASK_CHECK(c->fail_edge_to == BASE);

    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int cert_status_copied_and_clobbered(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    /* SC result survives lossless copies (C.MV then ADD x0) */
    LRD(5, 10);
    SCD(6, 10, 5);       /* t1 = status */
    CMV(28, 6);          /* c.mv t3,t1 */
    ADD(29, 0, 28);      /* t4 = x0 + t3 */
    BNEZ(29, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(!has_reason(c, RV_LRSC_REASON_STATUS_CLOBBERED));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* overwriting the status register before the test destroys the proof */
    memset(&P, 0, sizeof P);
    LRD(5, 10);
    SCD(6, 10, 5);
    ADDI(6, 6, 1);       /* t1 clobbered */
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_UNKNOWN);
    TASK_CHECK(has_reason_at(c, RV_LRSC_REASON_STATUS_CLOBBERED, 0x08));
    TASK_CHECK(has_reason(c, RV_LRSC_REASON_NO_STATUS_BRANCH));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int cert_foreign_status_and_independent_candidates(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    /* the branch tests a different SC's status, not this region's */
    LRD(5, 10);
    SCD(6, 10, 5);       /* SC A -> t1 */
    SCD(7, 10, 5);       /* SC B -> t2 (also a forbidden second SC) */
    BNEZ(7, 0);          /* tests t2: foreign to SC A */
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict != RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(has_reason(c, RV_LRSC_REASON_STATUS_FOREIGN) ||
               has_reason(c, RV_LRSC_REASON_FORBIDDEN_INSN));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* two independent loops must each certify without borrowing state */
    memset(&P, 0, sizeof P);
    LRD(5, 10);          /* 0 loop A, base a0 */
    SCD(6, 10, 5);       /* 4 */
    BNEZ(6, 0);          /* 8 */
    LRD(5, 11);          /* c loop B, base a1 (different symbolic base) */
    SCD(6, 11, 5);       /* 10 */
    BNEZ(6, 3);          /* 14 retry to loop B */
    ADDI(12, 0, 1);      /* 18 */
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidate_count == 2);
    TASK_CHECK(res->candidates[0].verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(res->candidates[1].verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(res->candidates[0].lr_rec.dec.pc == BASE);
    TASK_CHECK(res->candidates[1].lr_rec.dec.pc == BASE + 0x0c);
    TASK_CHECK(res->candidates[0].fail_edge_to == BASE);
    TASK_CHECK(res->candidates[1].fail_edge_to == BASE + 0x0c);
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int cert_status_predecessor_merge(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;
    rv_lrsc_candidate *c;

    /* On one predecessor the tested register equals the SC status, on the
     * other it is unrelated. A merge must not borrow the single-path
     * equivalence: the retry cannot be confirmed (unknown). */
    LRD(5, 10);          /* 0  label 0 */
    SCD(6, 10, 5);       /* 4  label 1: t1 = status */
    BEQZ(7, 5);          /* 8  label 2: fork on unrelated t2 -> label 5 */
    ADDI(28, 6, 0);      /* c  label 3: fall path A, t3 = t1 */
    CJ(6);               /* 10 label 4: -> merge label 6 */
    ADDI(28, 0, 7);      /* 12 label 5: path B, t3 = unrelated constant */
    BNEZ(28, 0);         /* 14 label 6: merged t3 cannot confirm status */
    ADDI(12, 0, 1);      /* 18 label 7 */
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_UNKNOWN);
    TASK_CHECK(!has_reason(c, RV_LRSC_REASON_BACKWARD_EDGE));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* Both predecessors copy the same SC status into the tested register:
     * the merge now keeps a single owner and the loop certifies. */
    memset(&P, 0, sizeof P);
    LRD(5, 10);          /* 0  label 0 */
    SCD(6, 10, 5);       /* 4  label 1: t1 = status */
    BEQZ(7, 5);          /* 8  label 2 -> label 5 */
    ADDI(28, 6, 0);      /* c  label 3: path A, t3 = t1 */
    CJ(6);               /* 10 label 4: -> merge label 6 */
    ADDI(28, 6, 0);      /* 12 label 5: path B, t3 = t1 as well */
    BNEZ(28, 0);         /* 14 label 6: both predecessors agree */
    ADDI(12, 0, 1);      /* 18 label 7 */
    fxapply(&P);

    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    c = &res->candidates[0];
    TASK_CHECK(c->verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(c->fail_edge_to == BASE);
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

static int cert_interior_loop_and_aqrl_neutrality(void)
{
    prog P = {0};
    rv_code_block blk;
    rv_lrsc_result *res;
    uint64_t entry = BASE;

    /* backward edge between LR and SC: an interior loop, not a retry tail */
    LRD(5, 10);          /* 0 */
    ADDI(7, 7, 1);       /* 4 */
    BNEZ(7, 1);          /* 8 loops back to offset 4 */
    SCD(6, 10, 5);       /* c */
    BNEZ(6, 0);          /* 10 */
    ADDI(12, 0, 1);      /* 14 */
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidates[0].verdict ==
               RV_LRSC_VERDICT_UNCONSTRAINED);
    TASK_CHECK(has_reason_at(&res->candidates[0],
               RV_LRSC_REASON_BACKWARD_EDGE, 0x08));
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);

    /* aq/rl ordering bits must not change pairing or the verdict */
    memset(&P, 0, sizeof P);
    LRDNQ(5, 10, 0, 0);
    SCDNQ(6, 10, 5, 1, 0);
    BNEZ(6, 0);
    ADDI(12, 0, 1);
    fxapply(&P);
    TASK_CHECK(analyze_prog(&P, &blk, &res, &entry, 1) == 0);
    TASK_CHECK(res->candidates[0].verdict == RV_LRSC_VERDICT_PROVEN);
    TASK_CHECK(res->candidates[0].lr_rec.dec.aq == 0);
    TASK_CHECK(res->candidates[0].sc_rec.dec.rl == 0);
    rv_lrsc_result_free(res);
    rv_code_block_free(&blk);
    return 0;
}

/* ----------------------------- registry ----------------------------- */

const struct task_case task_cases[] = {
    { "decode", "canonical_fields",      decode_canonical_fields },
    { "decode", "disasm_shares_path",    decode_disasm_shares_path },
    { "decode", "self_contained",        decode_self_contained },
    { "decode", "mixed_width_offsets",   decode_mixed_width_offsets },

    { "pairing", "proven_basic",             pair_proven_basic },
    { "pairing", "addr_copy_chain_offset",   pair_addr_copy_chain_and_offset },
    { "pairing", "addr_clobber",             pair_addr_clobber },
    { "pairing", "merge_conflict",           pair_merge_conflicting_predecessors },
    { "pairing", "intervening_lr_width",     pair_intervening_lr_and_width },
    { "pairing", "forbidden_and_no_sc",      pair_forbidden_and_no_sc },
    { "pairing", "external_entry_bypass",    pair_external_entry_bypass },
    { "pairing", "region_too_long",          pair_region_too_long },

    { "certificate", "bne_beq_directions",       cert_bne_and_beq_directions },
    { "certificate", "compressed_branch_jump",   cert_compressed_branch_and_jump },
    { "certificate", "status_copy_clobber",      cert_status_copied_and_clobbered },
    { "certificate", "foreign_independent",      cert_foreign_status_and_independent_candidates },
    { "certificate", "status_predecessor_merge", cert_status_predecessor_merge },
    { "certificate", "interior_loop_aqrl",       cert_interior_loop_and_aqrl_neutrality },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
