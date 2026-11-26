/* tests/amx_analysis.c
 *
 * Target tests for the AMX decoder extension and the read-only tile
 * analysis interface. Every instruction byte below is constructed in
 * software (a tiny fixed-pattern emitter, no assembler dependency) and
 * is only ever decoded, never executed.
 *
 * The legacy decode tests cover MOV, VEX arithmetic and x87 but have no
 * notion of tile registers, tile configuration state, per-row tile
 * footprints or SIB-index-as-stride; these cases fill that gap.
 */
#include <stdio.h>
#include <string.h>

#include "udis86.h"
#include "amx.h"
#include "task_support/check.h"
#include "task_support/amx-inputs.h"

/* ------------------------------------------------------------------ */
/* tiny self-contained byte emitters (no machine code is executed)    */
/* ------------------------------------------------------------------ */

#define REX_W 0x48

static uint8_t *
ev32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;         p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
    return p + 4;
}

static uint8_t *
ev64(uint8_t *p, uint64_t v)
{
    unsigned i;
    for (i = 0; i < 8; ++i) { p[i] = (uint8_t) (v >> (8 * i)); }
    return p + 8;
}

/* REX.W B8+rd io: mov r64, imm64 */
static unsigned
em_mov_imm64(uint8_t *p, unsigned reg, uint64_t imm)
{
    uint8_t *q = p;
    *q++ = (uint8_t) (REX_W | ((reg >> 3) << 2));
    *q++ = (uint8_t) (0xB8 + (reg & 7));
    q = ev64(q, imm);
    return (unsigned) (q - p);
}

/* REX.W 89 /r: mov r/m64, r64 (register form) */
static unsigned
em_mov_rr(uint8_t *p, unsigned dst, unsigned src)
{
    uint8_t rex = (uint8_t) (REX_W | ((dst >> 3) << 0) | ((src >> 3) << 2));
    p[0] = rex; p[1] = 0x89;
    p[2] = (uint8_t) (0xC0 | ((src & 7) << 3) | (dst & 7));
    return 3;
}

/* REX.W 81 /0 id and /5 id: add/sub r64, imm32(sign-extended) */
static unsigned
em_addsub_imm(uint8_t *p, unsigned reg, int sub, int32_t imm)
{
    p[0] = (uint8_t) (REX_W | ((reg >> 3) << 0));
    p[1] = 0x81;
    p[2] = (uint8_t) (0xC0 | ((sub ? 5 : 0) << 3) | (reg & 7));
    ev32(p + 3, (uint32_t) imm);
    return 7;
}

/* REX.W 3B /r: cmp r64, r/m64 */
static unsigned
em_cmp_rr(uint8_t *p, unsigned a, unsigned b)
{
    p[0] = (uint8_t) (REX_W | ((a >> 3) << 2) | ((b >> 3) << 0));
    p[1] = 0x3B;
    p[2] = (uint8_t) (0xC0 | ((a & 7) << 3) | (b & 7));
    return 3;
}

/* REX.W 85 /r: test r64, r/m64 */
static unsigned
em_test_rr(uint8_t *p, unsigned a, unsigned b)
{
    p[0] = (uint8_t) (REX_W | ((a >> 3) << 2) | ((b >> 3) << 0));
    p[1] = 0x85;
    p[2] = (uint8_t) (0xC0 | ((a & 7) << 3) | (b & 7));
    return 3;
}

static unsigned
em_nop(uint8_t *p) { p[0] = 0x90; return 1; }

/* E9 cd: jmp rel32 (target correctness is supplied via explicit succ) */
static unsigned
em_jmp_rel32(uint8_t *p) { p[0] = 0xE9; ev32(p + 1, 0); return 5; }

/* 0F 84 cd: jz rel32 */
static unsigned
em_jz_rel32(uint8_t *p) { p[0] = 0x0F; p[1] = 0x84; ev32(p + 2, 0); return 6; }

/* FF /4: jmp r/m64, an indirect transfer that must clear constants */
static unsigned
em_jmp_indirect(uint8_t *p, unsigned reg)
{
    p[0] = (uint8_t) (REX_W | ((reg >> 3) << 0));
    p[1] = 0xFF;
    p[2] = (uint8_t) (0xE0 | (reg & 7)); /* mod=11, reg=4 */
    return 3;
}

/* 3-byte VEX prefix builder. m is the VEX.m-mmmm value (2 = 0f38),
 * pp the VEX.pp map selector and v the raw VEX.vvvv field. */
static unsigned
em_vex3(uint8_t *p, unsigned m, unsigned R, unsigned X, unsigned B,
        unsigned v, unsigned pp)
{
    /* R/X/B are the already-inverted 1-bit VEX fields; m and pp are
     * raw map selectors. */
    p[0] = 0xC4;
    p[1] = (uint8_t) ((R << 7) | (X << 6) | (B << 5) | m);
    p[2] = (uint8_t) (((v & 0xF) << 3) | pp); /* W=0, L=0 */
    return 3;
}

static unsigned
sib_byte(unsigned scale, unsigned index, unsigned base)
{
    unsigned sc = scale == 1 ? 0 : scale == 2 ? 1 :
                  scale == 4 ? 2 : 3;
    return (sc << 6) | ((index & 7) << 3) | (base & 7);
}

/* ldtilecfg/sttilecfg [base] (mod=00, base direct, no SIB) */
static unsigned
em_tilecfg_mem(uint8_t *p, int store, unsigned base)
{
    unsigned n = em_vex3(p, 2, 1, 1, (base >> 3) ^ 1, 0xF, store ? 1 : 0);
    p[n++] = store ? 0x49 : 0x49;
    p[n++] = (uint8_t) (base & 7); /* modrm mod=00 reg=0 rm=base */
    return n;
}

/* ldtilecfg [rip+disp32] (mod=00 rm=5, no SIB) */
static unsigned
em_tilecfg_rip(uint8_t *p, int32_t disp)
{
    unsigned n = em_vex3(p, 2, 1, 1, 1, 0xF, 0);
    p[n++] = 0x49;
    p[n++] = 0x05; /* mod=00 rm=5 => RIP-relative */
    ev32(p + n, (uint32_t) disp);
    return n + 4;
}

/* tilerelease (mod=11 rm=0) and tilezero tmmN (mod=11 rm=0, reg=N) */
static unsigned
em_tilerelease(uint8_t *p)
{
    unsigned n = em_vex3(p, 2, 1, 1, 1, 0xF, 0);
    p[n++] = 0x49; p[n++] = 0xC0;
    return n;
}

static unsigned
em_tilezero(uint8_t *p, unsigned tmm)
{
    unsigned n = em_vex3(p, 2, (tmm >> 3) ^ 1, 1, 1, 0xF, 3);
    p[n++] = 0x49;
    p[n++] = (uint8_t) (0xC0 | ((tmm & 7) << 3) | 0);
    return n;
}

/* tileloadd/tileloaddt1/tilestored tmm, [base + index*scale + disp32].
 * The SIB index is the stride register and must stay out of row 0. */
static unsigned
em_tile_data_mem(uint8_t *p, enum ud_mnemonic_code mn, unsigned tmm,
                 unsigned base, unsigned index, unsigned scale,
                 int32_t disp)
{
    unsigned pp = mn == UD_Itileloadd ? 3 :
                  mn == UD_Itileloaddt1 ? 1 : 2;
    unsigned op = 0x4B;
    unsigned R = (tmm >> 3) ^ 1;
    unsigned X = (index >> 3) ^ 1;
    unsigned B = (base >> 3) ^ 1;
    unsigned n = em_vex3(p, 2, R, X, B, 0xF, pp);
    p[n++] = op;
    p[n++] = (uint8_t) (0x84 | ((tmm & 7) << 3) | 4); /* mod=10 rm=4 */
    p[n++] = (uint8_t) sib_byte(scale, index, base);
    ev32(p + n, (uint32_t) disp);
    return n + 4;
}

/* tdpbssd tmmC, tmmTH(vvvv), tmmT(rm) */
static unsigned
em_tdpbssd(uint8_t *p, unsigned C, unsigned TH, unsigned T)
{
    unsigned n = em_vex3(p, 2, (C >> 3) ^ 1, 1, (T >> 3) ^ 1,
                         (unsigned) (~TH & 0xF), 3);
    p[n++] = 0x5E;
    p[n++] = (uint8_t) (0xC0 | ((C & 7) << 3) | (T & 7));
    return n;
}

/* ------------------------------------------------------------------ */
/* config blob construction                                           */
/* ------------------------------------------------------------------ */

static void
put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }

/* palette 1, start_row 0; rows[]/colsb[] are the eight tile shapes. */
static void
make_config(uint8_t *p, const uint16_t rows[8], const uint16_t colsb[8])
{
    unsigned i;
    memset(p, 0, 64);
    p[0] = 1; p[1] = 0;
    for (i = 0; i < 8; ++i) put16(p + 2 + 2 * i, rows[i]);
    for (i = 0; i < 8; ++i) put16(p + 18 + 2 * i, colsb[i]);
}

/* ------------------------------------------------------------------ */

static const struct ud_amx_site *
find_site(const struct ud_amx_report *r, enum ud_amx_site_kind kind)
{
    size_t i;
    for (i = 0; i < r->nsites; ++i) {
        if (r->sites[i].kind == kind) return &r->sites[i];
    }
    return NULL;
}

static const struct ud_amx_insn *
find_insn(const struct ud_amx_report *r, unsigned mn)
{
    size_t i;
    for (i = 0; i < r->ninsns; ++i) {
        if (r->insns[i].mnemonic == mn) return &r->insns[i];
    }
    return NULL;
}

/* ============================ decode ============================== */

static int
case_decode_eight(void)
{
    ud_t u;
    const char *want[] = {
        "ldtilecfg", "sttilecfg", "tilezero", "tileloadd",
        "tileloaddt1", "tdpbssd", "tilestored", "tilerelease"
    };
    unsigned lens[] = {9, 5, 5, 6, 7, 5, 7, 5};
    unsigned i = 0;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, amx_inputs, sizeof amx_inputs);
    while (ud_disassemble(&u)) {
        TASK_CHECK(i < 8);
        TASK_CHECK(strcmp(ud_lookup_mnemonic(u.mnemonic), want[i]) == 0);
        TASK_CHECK(ud_insn_len(&u) == lens[i]);
        ++i;
    }
    TASK_CHECK(i == 8);
    return 0;
}

static int
case_decode_tile_vs_vector(void)
{
    ud_t u;
    const struct ud_operand *op;
    /* tilezero tmm1 and tdpbssd tmm0,tmm2,tmm1 slices of amx_inputs */
    static const uint8_t tz[] = {0xc4,0xe2,0x7b,0x49,0xc8};
    static const uint8_t tdp[] = {0xc4,0xe2,0x6b,0x5e,0xc1};
    static const uint8_t vadd[] = {0xc5,0xf4,0x58,0xc2};

    ud_init(&u); ud_set_mode(&u, 64);
    ud_set_input_buffer(&u, tz, sizeof tz);
    TASK_CHECK(ud_disassemble(&u) == sizeof tz);
    TASK_CHECK(u.mnemonic == UD_Itilezero);
    op = ud_insn_opr(&u, 0);
    TASK_CHECK(op && op->type == UD_OP_REG);
    TASK_CHECK(ud_amx_is_tile_reg(op->base));
    TASK_CHECK(op->base == UD_R_TMM1);
    TASK_CHECK(!ud_amx_is_vector_reg(op->base));

    ud_init(&u); ud_set_mode(&u, 64);
    ud_set_input_buffer(&u, tdp, sizeof tdp);
    TASK_CHECK(ud_disassemble(&u) == sizeof tdp);
    TASK_CHECK(u.mnemonic == UD_Itdpbssd);
    TASK_CHECK(ud_insn_opr(&u, 0)->base == UD_R_TMM0);
    TASK_CHECK(ud_insn_opr(&u, 1)->base == UD_R_TMM2);
    TASK_CHECK(ud_insn_opr(&u, 2)->base == UD_R_TMM1);

    ud_init(&u); ud_set_mode(&u, 64);
    ud_set_input_buffer(&u, vadd, sizeof vadd);
    TASK_CHECK(ud_disassemble(&u) == sizeof vadd);
    TASK_CHECK(u.mnemonic == UD_Ivaddps);
    TASK_CHECK(ud_amx_is_vector_reg(ud_insn_opr(&u, 0)->base));
    TASK_CHECK(!ud_amx_is_tile_reg(ud_insn_opr(&u, 0)->base));
    return 0;
}

static int
case_decode_sib_stride_role(void)
{
    ud_t u;
    const struct ud_operand *m;
    /* tileloadd tmm2, [rax+rcx] from the supplied stream */
    static const uint8_t tl[] = {0xc4,0xe2,0x7b,0x4b,0x14,0x08};
    ud_init(&u); ud_set_mode(&u, 64);
    ud_set_input_buffer(&u, tl, sizeof tl);
    TASK_CHECK(ud_disassemble(&u) == sizeof tl);
    TASK_CHECK(u.mnemonic == UD_Itileloadd);
    TASK_CHECK(ud_insn_opr(&u, 0)->base == UD_R_TMM2);
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->type == UD_OP_MEM);
    TASK_CHECK(m->base == UD_R_RAX);
    TASK_CHECK(m->index == UD_R_RCX);   /* index kept distinct from base */
    TASK_CHECK(m->scale == 0);          /* factor 1 stored as UD_NONE */
    return 0;
}

static int
case_decode_intel_syntax(void)
{
    ud_t u;
    unsigned i = 0;
    const char *needles[] = {"ldtilecfg", "sttilecfg", "tilezero tmm1",
                             "tileloadd tmm2", "tileloaddt1 tmm3",
                             "tdpbssd tmm0", "tilestored", "tilerelease"};
    ud_init(&u); ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, amx_inputs, sizeof amx_inputs);
    while (ud_disassemble(&u)) {
        TASK_CHECK(i < 8);
        TASK_CHECK(strstr(ud_insn_asm(&u), needles[i]) != NULL);
        ++i;
    }
    TASK_CHECK(i == 8);
    return 0;
}

static int
case_decode_legacy_preserved(void)
{
    ud_t u;
    static const uint8_t plain[] = {
        0xd9,0xe8,0xd9,0xc9,0xdd,0xd8,             /* fld1; fxch; fstp */
        0x48,0x8b,0x44,0x88,0x10,                  /* mov rax,[rax+rcx*4+0x10] */
        0xc5,0xf4,0x58,0xc2                        /* vaddps ymm0,ymm1,ymm2 */
    };
    unsigned n = 0;
    ud_init(&u); ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, plain, sizeof plain);
    while (ud_disassemble(&u)) { ++n; TASK_CHECK(u.mnemonic != UD_Iinvalid); }
    TASK_CHECK(n == 5);
    return 0;
}

/* ============================== flow ============================== */

/* Shared fixture addresses. */
#define A_B0    0x10000ull
#define A_B1    0x10100ull
#define A_B2    0x10200ull
#define A_B3    0x10300ull
#define CFG_A   0x40000ull
#define CFG_B   0x40080ull
#define DATA    0x50000ull
#define STRIDE  64ull

static void
fixture_regions(struct ud_amx_region *rg, uint8_t *mem)
{
    /* config A: tmm0 4x16, tmm1 8x16, tmm2 4x8 */
    static const uint16_t rows[8]  = {4, 8, 4, 0,0,0,0,0};
    static const uint16_t colsb[8] = {16,16, 8, 0,0,0,0,0};
    /* config B: tmm2 3x16 (a different shape for the same tile) */
    static const uint16_t rowsB[8]  = {0, 0, 3, 0,0,0,0,0};
    static const uint16_t colsbB[8] = {0, 0, 16,0,0,0,0,0};
    memset(mem, 0, 256);
    make_config(mem + (CFG_A - 0x40000), rows, colsb);
    make_config(mem + (CFG_B - 0x40000), rowsB, colsbB);
    rg->addr = 0x40000; rg->size = 256; rg->data = mem;
}

static int
case_flow_branch_merge_known_unknown(void)
{
    /* B0: cmp; jz B_known; (fall) B_unknown
     * B_unknown: jmp rax (clears); jmp B_merge
     * B_known: ldtilecfg [rdi]; jmp B_merge
     * B_merge: tileloadd tmm1 [rbx+rsi] */
    uint8_t b0[64], bu[64], bk[64], bm[64];
    unsigned n0 = 0, nu = 0, nk = 0, nm = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[3];
    struct ud_amx_block blk[4];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *s;
    size_t k;
    int have_known = 0, have_unknown = 0;

    n0 += em_cmp_rr(b0 + n0, 2 /*rdx*/, 3 /*rbx*/);
    n0 += em_jz_rel32(b0 + n0);
    nu += em_jmp_indirect(bu + nu, 0 /*rax*/);
    nu += em_jmp_rel32(bu + nu);
    nk += em_mov_imm64(bk + nk, 7 /*rdi*/, CFG_A);
    nk += em_tilecfg_mem(bk + nk, 0, 7);
    nk += em_jmp_rel32(bk + nk);
    nm += em_tile_data_mem(bm + nm, UD_Itileloadd, 1,
                           3 /*rbx*/, 6 /*rsi*/, 1, 0);

    fixture_regions(&rg, mem);
    gi[0].reg = UD_R_RDI; gi[0].value = CFG_A;
    gi[1].reg = UD_R_RBX; gi[1].value = DATA;
    gi[2].reg = UD_R_RSI; gi[2].value = STRIDE;

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0,
                (uint64_t[]){A_B1, A_B2}, 2};
    blk[1] = (struct ud_amx_block){A_B1, bu, nu,
                (uint64_t[]){A_B3}, 1};
    blk[2] = (struct ud_amx_block){A_B2, bk, nk,
                (uint64_t[]){A_B3}, 1};
    blk[3] = (struct ud_amx_block){A_B3, bm, nm, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 4; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 3;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    s = find_site(r, UD_AMX_SITE_TILELOADD);
    TASK_CHECK(s != NULL);
    TASK_CHECK(s->nstates == 2);
    for (k = 0; k < s->nstates; ++k) {
        if (s->states[k].kind == UD_AMX_ST_KNOWN) have_known = 1;
        if (s->states[k].kind == UD_AMX_ST_UNKNOWN) have_unknown = 1;
    }
    TASK_CHECK(have_known && have_unknown);
    TASK_CHECK(r->nclear_sites >= 1); /* indirect jmp recorded */
    ud_amx_report_free(r);
    return 0;
}

static int
case_flow_loop_reconfigure_converges(void)
{
    /* B0: mov rdi,CFG_A; ldtilecfg [rdi]; jmp B1
     * B1: cmp rcx,rdx; jz B3; fall B2
     * B2: ldtilecfg [rdi]; add rcx,1; jmp B1
     * B3: tileloadd tmm1 [rax+rsi] */
    uint8_t b0[64], b1[64], b2[64], b3[64];
    unsigned n0 = 0, n1 = 0, n2 = 0, n3 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[4];
    struct ud_amx_block blk[4];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *s;
    const struct ud_amx_state *known = NULL;
    size_t k;

    n0 += em_mov_imm64(b0 + n0, 7, CFG_A);
    n0 += em_tilecfg_mem(b0 + n0, 0, 7);
    n0 += em_jmp_rel32(b0 + n0);
    n1 += em_cmp_rr(b1 + n1, 1 /*rcx*/, 2 /*rdx*/);
    n1 += em_jz_rel32(b1 + n1);
    n2 += em_tilecfg_mem(b2 + n2, 0, 7);
    n2 += em_addsub_imm(b2 + n2, 1, 0, 1);
    n2 += em_jmp_rel32(b2 + n2);
    n3 += em_tile_data_mem(b3 + n3, UD_Itileloadd, 1, 0, 6, 1, 0);

    fixture_regions(&rg, mem);
    gi[0].reg = UD_R_RDI; gi[0].value = CFG_A;
    gi[1].reg = UD_R_RAX; gi[1].value = DATA;
    gi[2].reg = UD_R_RSI; gi[2].value = STRIDE;
    gi[3].reg = UD_R_RCX; gi[3].value = 0;
    /* rdx deliberately left unknown: cmp must preserve other constants */

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0,
                (uint64_t[]){A_B1}, 1};
    blk[1] = (struct ud_amx_block){A_B1, b1, n1,
                (uint64_t[]){A_B3, A_B2}, 2};
    blk[2] = (struct ud_amx_block){A_B2, b2, n2,
                (uint64_t[]){A_B1}, 1};
    blk[3] = (struct ud_amx_block){A_B3, b3, n3, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 4; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 4;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    /* identical content merges to one config but both loads are kept */
    TASK_CHECK(r->nconfigs == 1);
    s = find_site(r, UD_AMX_SITE_TILELOADD);
    TASK_CHECK(s != NULL && s->nstates == 1);
    known = &s->states[0];
    TASK_CHECK(known->kind == UD_AMX_ST_KNOWN);
    TASK_CHECK(known->nload_sites == 2);
    for (k = 0; k < known->nload_sites; ++k) {
        TASK_CHECK(known->load_sites[k] == A_B0 + 10 ||
                   known->load_sites[k] == A_B2);
    }
    TASK_CHECK(known->ncfg_addrs == 1 && known->cfg_addrs[0] == CFG_A);
    ud_amx_report_free(r);
    return 0;
}

static int
case_flow_release_and_palette0(void)
{
    /* B0: ldtilecfg [rdi]; tilerelease; jmp B1
     * B1: ldtilecfg [rsi] where [rsi] holds palette 0 (released) */
    uint8_t b0[64], b1[64];
    unsigned n0 = 0, n1 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[2];
    struct ud_amx_block blk[2];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *rel, *ld2;
    static const uint16_t rows[8]  = {1,0,0,0,0,0,0,0};
    static const uint16_t colsb[8] = {4,0,0,0,0,0,0,0};

    memset(mem, 0, 256);
    make_config(mem, rows, colsb);                 /* CFG_A region base */
    memset(mem + 0x80, 0, 64);                     /* palette 0 blob */
    rg.addr = 0x40000; rg.size = 256; rg.data = mem;

    n0 += em_tilecfg_mem(b0 + n0, 0, 7 /*rdi*/);
    n0 += em_tilerelease(b0 + n0);
    n0 += em_jmp_rel32(b0 + n0);
    n1 += em_tilecfg_mem(b1 + n1, 0, 6 /*rsi*/);

    gi[0].reg = UD_R_RDI; gi[0].value = CFG_A;
    gi[1].reg = UD_R_RSI; gi[1].value = CFG_B; /* palette 0 */
    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0,
                (uint64_t[]){A_B1}, 1};
    blk[1] = (struct ud_amx_block){A_B1, b1, n1, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 2; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 2;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    rel = find_site(r, UD_AMX_SITE_TILERELEASE);
    TASK_CHECK(rel != NULL && rel->released);
    ld2 = r->sites + r->nsites - 1;
    TASK_CHECK(ld2->kind == UD_AMX_SITE_LDTILECFG);
    TASK_CHECK(ld2->nstates == 1);
    TASK_CHECK(ld2->states[0].kind == UD_AMX_ST_RELEASED);
    ud_amx_report_free(r);
    return 0;
}

static int
case_flow_gpr_propagation_and_clear(void)
{
    /* mov rax,0x1000; mov rbx,rax; lea rcx,[rbx+0x10]; add rcx,4;
     * sub rcx,1; test rcx,rcx; nop; jmp B1; B1: jmp rax (clears) */
    uint8_t b0[80], b1[16];
    unsigned n0 = 0, n1 = 0;
    struct ud_amx_block blk[2];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_insn *ins;
    unsigned rax = 0, rbx = 3, rcx = 1;

    n0 += em_mov_imm64(b0 + n0, rax, 0x1000);
    n0 += em_mov_rr(b0 + n0, rbx, rax);
    /* lea rcx, [rbx+0x10]: REX.W 8D, modrm mod10 reg=rcx rm=rbx + disp */
    b0[n0++] = REX_W | ((rcx >> 3) << 2) | ((rbx >> 3) << 0);
    b0[n0++] = 0x8D;
    b0[n0++] = (uint8_t) (0x80 | ((rcx & 7) << 3) | (rbx & 7));
    ev32(b0 + n0, 0x10); n0 += 4;
    n0 += em_addsub_imm(b0 + n0, rcx, 0, 4);
    n0 += em_addsub_imm(b0 + n0, rcx, 1, 1);
    n0 += em_test_rr(b0 + n0, rcx, rcx);
    n0 += em_nop(b0 + n0);
    n0 += em_jmp_rel32(b0 + n0);
    n1 += em_jmp_indirect(b1 + n1, rax);
    n1 += em_nop(b1 + n1); /* observe state after the clear */

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0,
                (uint64_t[]){A_B1}, 1};
    blk[1] = (struct ud_amx_block){A_B1, b1, n1, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 2; in.entry = A_B0;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    ins = find_insn(r, UD_Iadd);
    TASK_CHECK(ins && ins->gpr_known[rcx]);       /* 0x1010 entering add */
    TASK_CHECK(ins->gpr_val[rcx] == 0x1010);
    ins = find_insn(r, UD_Isub);
    TASK_CHECK(ins && ins->gpr_val[rcx] == 0x1014);
    /* the indirect jmp clears constants; the following nop observes it */
    {
        const struct ud_amx_insn *post = NULL;
        size_t ix;
        for (ix = 0; ix < r->ninsns; ++ix) {
            if (r->insns[ix].mnemonic == UD_Inop &&
                r->insns[ix].block_addr == A_B1) {
                post = &r->insns[ix];
            }
        }
        TASK_CHECK(post != NULL);
        {
            size_t i, any = 0;
            for (i = 0; i < 16; ++i) any |= post->gpr_known[i];
            TASK_CHECK(any == 0);
        }
        TASK_CHECK(r->nclear_sites == 1);
        TASK_CHECK(r->clear_sites[0] == A_B1);
    }
    /* the direct jmp at the end of B0 keeps the propagated constants */
    {
        const struct ud_amx_insn *djmp = NULL;
        size_t ix;
        for (ix = 0; ix < r->ninsns; ++ix) {
            if (r->insns[ix].mnemonic == UD_Ijmp &&
                r->insns[ix].block_addr == A_B0) {
                djmp = &r->insns[ix];
            }
        }
        TASK_CHECK(djmp != NULL && djmp->gpr_known[rcx]);
        TASK_CHECK(djmp->gpr_val[rcx] == 0x1013);
    }
    ud_amx_report_free(r);
    return 0;
}

/* =========================== footprint ============================ */

static int
case_footprint_per_row_stride(void)
{
    /* single known config; tile tmm1 rows=4 colsb=16; base rax, stride
     * rsi=64, no displacement. */
    uint8_t b0[32];
    unsigned n0 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[2];
    struct ud_amx_block blk[1];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *s;
    unsigned row;

    fixture_regions(&rg, mem);
    gi[0].reg = UD_R_RAX; gi[0].value = DATA;
    gi[1].reg = UD_R_RSI; gi[1].value = STRIDE;
    n0 += em_mov_imm64(b0 + n0, 7, CFG_A);
    n0 += em_tilecfg_mem(b0 + n0, 0, 7);
    n0 += em_tile_data_mem(b0 + n0, UD_Itileloadd, 1, 0, 6, 1, 0);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 1; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 2;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    s = find_site(r, UD_AMX_SITE_TILELOADD);
    TASK_CHECK(s != NULL);
    TASK_CHECK(s->nrows == 8);                    /* tmm1: 8 rows x 16 colsb */
    for (row = 0; row < 8; ++row) {
        const struct ud_amx_row *w = &s->rows[row];
        TASK_CHECK(w->row == row);              /* row numbers kept */
        TASK_CHECK(w->resolved);
        TASK_CHECK(w->tile == 1);
        /* index register must not contribute to row 0 */
        TASK_CHECK(w->lo == DATA + row * STRIDE);
        TASK_CHECK(w->hi == w->lo + 16);       /* half-open, colsb wide */
    }
    ud_amx_report_free(r);
    return 0;
}

static int
case_footprint_missing_stride_symbolic(void)
{
    uint8_t b0[32];
    unsigned n0 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[1];
    struct ud_amx_block blk[1];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *s;

    fixture_regions(&rg, mem);
    gi[0].reg = UD_R_RAX; gi[0].value = DATA; /* rsi stride unknown */
    n0 += em_mov_imm64(b0 + n0, 7, CFG_A);
    n0 += em_tilecfg_mem(b0 + n0, 0, 7);
    n0 += em_tile_data_mem(b0 + n0, UD_Itileloadd, 1, 0, 6, 1, 0);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 1; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 1;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    s = find_site(r, UD_AMX_SITE_TILELOADD);
    TASK_CHECK(s != NULL && s->nrows == 8);
    TASK_CHECK(s->missing & UD_AMX_MISS_STRIDE);
    TASK_CHECK(s->rows[0].resolved);                 /* base alone resolves row 0 */
    TASK_CHECK(!s->rows[1].resolved);
    TASK_CHECK(s->rows[1].missing & UD_AMX_MISS_STRIDE);
    TASK_CHECK(s->rows[1].sym.index_reg == UD_R_RSI); /* role retained */
    TASK_CHECK(s->rows[1].sym.base_reg == UD_R_RAX);
    ud_amx_report_free(r);
    return 0;
}

static int
case_footprint_two_configs_no_bounding_box(void)
{
    /* B0: cmp; jz B_A; fall B_B
     * B_A: mov rdi,CFG_A; ldtilecfg[rdi]; jmp Bm
     * B_B: mov rdi,CFG_B; ldtilecfg[rdi]; jmp Bm
     * Bm: tileloadd tmm2 [rax+rsi] (shape differs per config) */
    uint8_t b0[16], ba[80], bb[80], bm[32];
    unsigned n0 = 0, na = 0, nb = 0, nm = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[2];
    struct ud_amx_block blk[4];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *s;
    size_t k;

    fixture_regions(&rg, mem);
    n0 += em_cmp_rr(b0, 1, 2);
    n0 += em_jz_rel32(b0 + n0);
    na += em_mov_imm64(ba + na, 7, CFG_A);
    na += em_tilecfg_mem(ba + na, 0, 7);
    na += em_jmp_rel32(ba + na);
    nb += em_mov_imm64(bb + nb, 7, CFG_B);
    nb += em_tilecfg_mem(bb + nb, 0, 7);
    nb += em_jmp_rel32(bb + nb);
    nm += em_tile_data_mem(bm + nm, UD_Itileloadd, 2, 0, 6, 1, 0);

    gi[0].reg = UD_R_RAX; gi[0].value = DATA;
    gi[1].reg = UD_R_RSI; gi[1].value = STRIDE;
    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0,
                (uint64_t[]){A_B2, A_B1}, 2};
    blk[1] = (struct ud_amx_block){A_B1, bb, nb,
                (uint64_t[]){A_B3}, 1};
    blk[2] = (struct ud_amx_block){A_B2, ba, na,
                (uint64_t[]){A_B3}, 1};
    blk[3] = (struct ud_amx_block){A_B3, bm, nm, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 4; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 2;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    TASK_CHECK(r->nconfigs == 2);
    s = find_site(r, UD_AMX_SITE_TILELOADD);
    TASK_CHECK(s != NULL && s->nstates == 2);
    /* tmm2 is shaped in both configs: A gives 4 rows of 8 colsb,
     * B gives 3 rows of 16 colsb. The rows stay separate per source
     * instead of collapsing into one bounding interval. */
    TASK_CHECK(s->nrows == 7);
    {
        unsigned seen_ids[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        unsigned nids = 0;
        unsigned width8 = 0, width16 = 0;
        for (k = 0; k < s->nrows; ++k) {
            unsigned id = s->rows[k].cfg_id;
            unsigned w = (unsigned) (s->rows[k].hi - s->rows[k].lo);
            unsigned j, found = 0;
            TASK_CHECK(s->rows[k].resolved);
            TASK_CHECK(s->rows[k].tile == 2);
            for (j = 0; j < nids; ++j) {
                if (seen_ids[j] == id) { found = 1; break; }
            }
            if (!found) { TASK_CHECK(nids < 2); seen_ids[nids++] = id; }
            if (w == 8) ++width8;
            else if (w == 16) ++width16;
        }
        TASK_CHECK(nids == 2);
        TASK_CHECK(width8 == 4 && width16 == 3);
    }
    ud_amx_report_free(r);
    return 0;
}

static int
case_footprint_tilezero_and_sttilecfg_no_data_rows(void)
{
    uint8_t b0[40];
    unsigned n0 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[1];
    struct ud_amx_block blk[1];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *z, *st;

    fixture_regions(&rg, mem);
    gi[0].reg = UD_R_RDI; gi[0].value = CFG_A;
    n0 += em_tilezero(b0 + n0, 1);
    n0 += em_tilecfg_mem(b0 + n0, 1 /*store*/, 7);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 1; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 1;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    z = find_site(r, UD_AMX_SITE_TILEZERO);
    TASK_CHECK(z != NULL);
    TASK_CHECK(z->nrows == 0 && !z->mem.present);      /* no memory access */
    TASK_CHECK(z->ntiles == 1 && z->tiles[0].tile == 1);
    TASK_CHECK(z->tiles[0].role == UD_AMX_TILE_WRITE);
    st = find_site(r, UD_AMX_SITE_STTILECFG);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->config_write && !st->config_load);
    TASK_CHECK(st->nrows == 0);                        /* not a tile data row */
    TASK_CHECK(st->mem.present);
    ud_amx_report_free(r);
    return 0;
}

static int
case_footprint_tdpbssd_roles_and_dims(void)
{
    /* Two reachable configurations meet at one TDPBSSD site.
     * GOOD: tmm0 4x16 (C), tmm1 8x16 (T), tmm2 4x8 (TH) -> compatible
     * BAD : tmm0 2x16,      tmm1 4x16,     tmm2 2x8      -> the
     *       k dimension (8 vs 4) does not match -> incompatible. */
    uint8_t b0[16], bb[80], bg[80], bm[16];
    unsigned n0 = 0, nb = 0, ng = 0, nm = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_block blk[4];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *s;
    static const uint16_t grow[8] = {4, 8, 4, 0,0,0,0,0};
    static const uint16_t gcol[8] = {16,16,8, 0,0,0,0,0};
    static const uint16_t brow[8] = {2, 4, 2, 0,0,0,0,0};
    static const uint16_t bcol[8] = {16,16,8, 0,0,0,0,0};
    int saw_compat = 0, saw_incompat = 0;
    size_t k;

    memset(mem, 0, sizeof mem);
    make_config(mem + (CFG_A - 0x40000), grow, gcol);
    make_config(mem + (CFG_B - 0x40000), brow, bcol);
    rg.addr = 0x40000; rg.size = 256; rg.data = mem;

    n0 += em_cmp_rr(b0, 1, 2);
    n0 += em_jz_rel32(b0 + n0);
    nb += em_mov_imm64(bb + nb, 7, CFG_B);
    nb += em_tilecfg_mem(bb + nb, 0, 7);
    nb += em_jmp_rel32(bb + nb);
    ng += em_mov_imm64(bg + ng, 7, CFG_A);
    ng += em_tilecfg_mem(bg + ng, 0, 7);
    ng += em_jmp_rel32(bg + ng);
    nm += em_tdpbssd(bm + nm, 0 /*C*/, 2 /*TH*/, 1 /*T*/);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0,
                (uint64_t[]){A_B2, A_B1}, 2};
    blk[1] = (struct ud_amx_block){A_B1, bb, nb,
                (uint64_t[]){A_B3}, 1};
    blk[2] = (struct ud_amx_block){A_B2, bg, ng,
                (uint64_t[]){A_B3}, 1};
    blk[3] = (struct ud_amx_block){A_B3, bm, nm, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 4; in.entry = A_B0;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    s = find_site(r, UD_AMX_SITE_TDPBSSD);
    TASK_CHECK(s != NULL);
    TASK_CHECK(s->ntiles == 3);
    TASK_CHECK(s->tiles[0].tile == 0 &&
               s->tiles[0].role == UD_AMX_TILE_RDWR);
    TASK_CHECK(s->tiles[1].tile == 2 &&
               s->tiles[1].role == UD_AMX_TILE_READ);
    TASK_CHECK(s->tiles[2].tile == 1 &&
               s->tiles[2].role == UD_AMX_TILE_READ);
    TASK_CHECK(s->nrows == 0);                 /* no numeric simulation */
    TASK_CHECK(s->nstates == 2);
    for (k = 0; k < s->nstates; ++k) {
        TASK_CHECK(s->states[k].kind == UD_AMX_ST_KNOWN);
        if (s->states[k].dims_compatible == 1) saw_compat = 1;
        if (s->states[k].dims_compatible == 0) saw_incompat = 1;
    }
    TASK_CHECK(saw_compat && saw_incompat);
    ud_amx_report_free(r);
    return 0;
}

/* ============================ registry =========================== */

static int
case_flow_rip_relative_config(void)
{
    uint8_t b0[16];
    unsigned n0 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_block blk[1];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *s;
    int32_t disp;

    static const uint16_t rows[8]  = {1, 8, 0,0,0,0,0,0};
    static const uint16_t colsb[8] = {4, 16,0,0,0,0,0,0};
    memset(mem, 0, sizeof mem);
    make_config(mem + (CFG_A - 0x40000), rows, colsb);
    rg.addr = 0x40000; rg.size = 256; rg.data = mem;

    disp = (int32_t) (CFG_A - (A_B0 + 9)); /* 9 = rip-relative instr len */
    n0 += em_tilecfg_rip(b0 + n0, disp);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 1; in.entry = A_B0;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    s = find_site(r, UD_AMX_SITE_LDTILECFG);
    TASK_CHECK(s != NULL);
    TASK_CHECK(s->mem.present && s->mem.rip_relative);
    TASK_CHECK(s->nstates == 1);
    TASK_CHECK(s->states[0].kind == UD_AMX_ST_KNOWN);
    TASK_CHECK(s->states[0].ncfg_addrs == 1);
    TASK_CHECK(s->states[0].cfg_addrs[0] == CFG_A);
    TASK_CHECK(r->nconfigs == 1);
    TASK_CHECK(r->configs[0].shape[1].rows == 8);
    TASK_CHECK(r->configs[0].shape[1].colsb == 16);
    ud_amx_report_free(r);
    return 0;
}

static int
case_flow_unknown_then_definite_recovers(void)
{
    /* B0: ldtilecfg [rax] with rax unknown -> UNKNOWN
     * B1: mov rdi,CFG_A; ldtilecfg [rdi] -> definite KNOWN
     * B2: tileloadd tmm1 [rax+rsi] sees only the recovered config */
    uint8_t b0[16], b1[40], b2[16];
    unsigned n0 = 0, n1 = 0, n2 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[2];
    struct ud_amx_block blk[3];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *ld0, *ld1, *tl;
    size_t k;

    fixture_regions(&rg, mem);
    n0 += em_tilecfg_mem(b0 + n0, 0, 0 /*rax, no initial value*/);
    n1 += em_mov_imm64(b1 + n1, 7, CFG_A);
    n1 += em_tilecfg_mem(b1 + n1, 0, 7);
    n2 += em_tile_data_mem(b2 + n2, UD_Itileloadd, 1, 0, 6, 1, 0);

    gi[0].reg = UD_R_RAX; gi[0].value = DATA;
    gi[1].reg = UD_R_RSI; gi[1].value = STRIDE;
    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0,
                (uint64_t[]){A_B1}, 1};
    blk[1] = (struct ud_amx_block){A_B1, b1, n1,
                (uint64_t[]){A_B2}, 1};
    blk[2] = (struct ud_amx_block){A_B2, b2, n2, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 3; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 2;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    TASK_CHECK(r->nsites == 3);
    ld0 = &r->sites[0];
    TASK_CHECK(ld0->kind == UD_AMX_SITE_LDTILECFG);
    TASK_CHECK(ld0->nstates == 1 &&
               ld0->states[0].kind == UD_AMX_ST_UNKNOWN);
    ld1 = &r->sites[1];
    TASK_CHECK(ld1->kind == UD_AMX_SITE_LDTILECFG);
    TASK_CHECK(ld1->nstates == 1 &&
               ld1->states[0].kind == UD_AMX_ST_KNOWN);
    tl = &r->sites[2];
    TASK_CHECK(tl->kind == UD_AMX_SITE_TILELOADD);
    /* the definite load replaces the unknown state rather than merging */
    TASK_CHECK(tl->nstates == 1);
    TASK_CHECK(tl->states[0].kind == UD_AMX_ST_KNOWN);
    for (k = 0; k < tl->nrows; ++k) TASK_CHECK(tl->rows[k].resolved);
    ud_amx_report_free(r);
    return 0;
}

static int
case_footprint_scaled_stride(void)
{
    /* stride register holds 16, SIB scale 4 -> effective stride 64.
     * The unscaled register value must not be used for row spacing. */
    uint8_t b0[32];
    unsigned n0 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[2];
    struct ud_amx_block blk[1];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *s;
    unsigned row;

    fixture_regions(&rg, mem);
    gi[0].reg = UD_R_RAX; gi[0].value = DATA;
    gi[1].reg = UD_R_RSI; gi[1].value = 16;
    n0 += em_mov_imm64(b0 + n0, 7, CFG_A);
    n0 += em_tilecfg_mem(b0 + n0, 0, 7);
    n0 += em_tile_data_mem(b0 + n0, UD_Itileloadd, 1, 0, 6, 4, 0);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 1; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 2;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    s = find_site(r, UD_AMX_SITE_TILELOADD);
    TASK_CHECK(s != NULL && s->nrows == 8);
    TASK_CHECK(s->mem.scale == 4 && s->mem.index_reg == UD_R_RSI);
    for (row = 0; row < 8; ++row) {
        TASK_CHECK(s->rows[row].resolved);
        TASK_CHECK(s->rows[row].lo == DATA + row * 64);
        TASK_CHECK(s->rows[row].hi == s->rows[row].lo + 16);
    }
    ud_amx_report_free(r);
    return 0;
}

static int
case_flow_sttilecfg_preserves_config(void)
{
    /* known load, then a config write, then a tile data use. The write
     * must not become a new configuration nor clear the loaded one. */
    uint8_t b0[48];
    unsigned n0 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[3];
    struct ud_amx_block blk[1];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    const struct ud_amx_site *st, *tl;

    fixture_regions(&rg, mem);
    gi[0].reg = UD_R_RDI; gi[0].value = CFG_A;
    gi[1].reg = UD_R_RAX; gi[1].value = DATA;
    gi[2].reg = UD_R_RSI; gi[2].value = STRIDE;
    n0 += em_mov_imm64(b0 + n0, 7, CFG_A);
    n0 += em_tilecfg_mem(b0 + n0, 0, 7); /* ldtilecfg */
    n0 += em_tilecfg_mem(b0 + n0, 1, 7); /* sttilecfg */
    n0 += em_tile_data_mem(b0 + n0, UD_Itileloadd, 1, 0, 6, 1, 0);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 1; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 3;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    st = find_site(r, UD_AMX_SITE_STTILECFG);
    tl = find_site(r, UD_AMX_SITE_TILELOADD);
    TASK_CHECK(st != NULL && tl != NULL);
    TASK_CHECK(st->config_write && !st->config_load);
    /* the data use still sees exactly the one loaded config */
    TASK_CHECK(tl->nstates == 1);
    TASK_CHECK(tl->states[0].kind == UD_AMX_ST_KNOWN);
    TASK_CHECK(tl->states[0].nload_sites == 1);
    TASK_CHECK(tl->nrows == 8);
    ud_amx_report_free(r);
    return 0;
}

static int
case_report_repeatable_and_input_untouched(void)
{
    uint8_t b0[32];
    unsigned n0 = 0;
    uint8_t mem[256], bytes_snapshot[32];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[2];
    struct ud_amx_block blk[1];
    struct ud_amx_input in;
    const struct ud_amx_report *r1, *r2;
    const struct ud_amx_site *s1, *s2;

    fixture_regions(&rg, mem);
    gi[0].reg = UD_R_RAX; gi[0].value = DATA;
    gi[1].reg = UD_R_RSI; gi[1].value = STRIDE;
    n0 += em_mov_imm64(b0 + n0, 7, CFG_A);
    n0 += em_tilecfg_mem(b0 + n0, 0, 7);
    n0 += em_tile_data_mem(b0 + n0, UD_Itileloadd, 1, 0, 6, 1, 0);
    memcpy(bytes_snapshot, b0, n0);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 1; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 2;
    in.regions = &rg; in.nregions = 1;

    r1 = ud_amx_analyze(&in);
    TASK_CHECK(r1 != NULL);
    r2 = ud_amx_analyze(&in);           /* repeated query */
    TASK_CHECK(r2 != NULL);
    TASK_CHECK(r1 != r2);               /* independent allocations */
    TASK_CHECK(r1->sites != r2->sites);
    s1 = find_site(r1, UD_AMX_SITE_TILELOADD);
    s2 = find_site(r2, UD_AMX_SITE_TILELOADD);
    TASK_CHECK(s1 != s2);
    TASK_CHECK(s1->nrows == s2->nrows);
    TASK_CHECK(s1->rows[3].lo == s2->rows[3].lo);
    TASK_CHECK(s1->rows[3].hi == s2->rows[3].hi);
    /* the caller's instruction bytes are not modified */
    TASK_CHECK(memcmp(b0, bytes_snapshot, n0) == 0);
    ud_amx_report_free(r1);
    ud_amx_report_free(r2);
    return 0;
}

static int
case_flow_unreachable_block_excluded(void)
{
    /* B0 (entry) ends the CFG; B1 is supplied but has no predecessor.
     * Its instructions and AMX site must not appear in the report. */
    uint8_t b0[16], b1[32];
    unsigned n0 = 0, n1 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_block blk[2];
    struct ud_amx_input in;
    const struct ud_amx_report *r;
    size_t i, saw_b1 = 0;

    fixture_regions(&rg, mem);
    n0 += em_nop(b0 + n0);
    n1 += em_mov_imm64(b1 + n1, 7, CFG_A);
    n1 += em_tilecfg_mem(b1 + n1, 0, 7);
    n1 += em_tilezero(b1 + n1, 2);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0, NULL, 0};
    blk[1] = (struct ud_amx_block){A_B1, b1, n1, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 2; in.entry = A_B0;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    TASK_CHECK(r->ninsns == 1);
    TASK_CHECK(r->nsites == 0);
    for (i = 0; i < r->ninsns; ++i) {
        if (r->insns[i].block_addr == A_B1) saw_b1 = 1;
    }
    TASK_CHECK(!saw_b1);
    ud_amx_report_free(r);
    return 0;
}

static int
case_flow_untrusted_config_is_unknown(void)
{
    /* A full-64-byte blob outside any region, and a blob whose palette is
     * not 0/1, must both yield UNKNOWN rather than an invented shape. */
    uint8_t b0[16], b1[16];
    unsigned n0 = 0, n1 = 0;
    uint8_t mem[256];
    struct ud_amx_region rg;
    struct ud_amx_gpr_init gi[2];
    struct ud_amx_block blk[2];
    struct ud_amx_input in;
    const struct ud_amx_report *r;

    memset(mem, 0, sizeof mem);
    mem[0] = 7; /* unsupported palette */
    rg.addr = CFG_A; rg.size = 64; rg.data = mem;
    gi[0].reg = UD_R_RDI; gi[0].value = CFG_A;   /* bad palette */
    gi[1].reg = UD_R_RAX; gi[1].value = 0x60000; /* no region there */
    n0 += em_mov_imm64(b0 + n0, 7, CFG_A);
    n0 += em_tilecfg_mem(b0 + n0, 0, 7);
    n1 += em_mov_imm64(b1 + n1, 0, 0x60000);
    n1 += em_tilecfg_mem(b1 + n1, 0, 0);

    memset(blk, 0, sizeof blk);
    blk[0] = (struct ud_amx_block){A_B0, b0, n0,
                (uint64_t[]){A_B1}, 1};
    blk[1] = (struct ud_amx_block){A_B1, b1, n1, NULL, 0};
    memset(&in, 0, sizeof in);
    in.blocks = blk; in.nblocks = 2; in.entry = A_B0;
    in.gpr_init = gi; in.ngpr_init = 2;
    in.regions = &rg; in.nregions = 1;

    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    TASK_CHECK(r->nsites == 2);
    TASK_CHECK(r->sites[0].nstates == 1 &&
               r->sites[0].states[0].kind == UD_AMX_ST_UNKNOWN);
    TASK_CHECK(r->sites[1].nstates == 1 &&
               r->sites[1].states[0].kind == UD_AMX_ST_UNKNOWN);
    /* no configuration content was ever interned */
    TASK_CHECK(r->nconfigs == 0);
    ud_amx_report_free(r);
    return 0;
}

const struct task_case task_cases[] = {
    {"decode", "eight_amx_mnemonics",      case_decode_eight},
    {"decode", "tile_distinct_from_vector",case_decode_tile_vs_vector},
    {"decode", "sib_index_stride_role",    case_decode_sib_stride_role},
    {"decode", "intel_syntax_recognizes",  case_decode_intel_syntax},
    {"decode", "legacy_mov_vex_x87",       case_decode_legacy_preserved},

    {"flow", "branch_merge_known_unknown", case_flow_branch_merge_known_unknown},
    {"flow", "loop_reconfigure_converges", case_flow_loop_reconfigure_converges},
    {"flow", "release_and_palette0",       case_flow_release_and_palette0},
    {"flow", "gpr_propagation_and_clear",  case_flow_gpr_propagation_and_clear},
    {"flow", "rip_relative_config",        case_flow_rip_relative_config},
    {"flow", "unknown_recovers",           case_flow_unknown_then_definite_recovers},
    {"flow", "sttilecfg_preserves_config", case_flow_sttilecfg_preserves_config},
    {"flow", "repeatable_input_untouched", case_report_repeatable_and_input_untouched},
    {"flow", "unreachable_block_excluded", case_flow_unreachable_block_excluded},
    {"flow", "untrusted_config_unknown",   case_flow_untrusted_config_is_unknown},

    {"footprint", "per_row_stride",        case_footprint_per_row_stride},
    {"footprint", "scaled_stride",         case_footprint_scaled_stride},
    {"footprint", "missing_stride_symbolic",case_footprint_missing_stride_symbolic},
    {"footprint", "two_configs_no_box",    case_footprint_two_configs_no_bounding_box},
    {"footprint", "tilezero_stcfg_no_rows",case_footprint_tilezero_and_sttilecfg_no_data_rows},
    {"footprint", "tdpbssd_roles_dims",    case_footprint_tdpbssd_roles_and_dims},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
