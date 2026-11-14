/* tests/amx_analysis.c
 *
 * Target tests for the AMX decoder extensions and the read-only AMX
 * analysis interface. Every instruction byte below is hand-assembled
 * data; no machine code in here is ever executed.
 *
 * Coverage the legacy tests never had:
 *   - VEX AMX opcodes (49/4b/5e in the 0f38 map) were decoded as
 *     invalid or as unrelated legacy instructions;
 *   - tmm0-tmm7 did not exist as a register class distinct from xmm/ymm;
 *   - the SIB index of tile load/store was indistinguishable from an
 *     ordinary scaled address addend, hiding the per-row stride;
 *   - nothing modeled tile configuration flow, CFG joins, loops or the
 *     per-row memory footprint of a tile transfer.
 */
#include <stdio.h>
#include <string.h>
#include "udis86.h"
#include "amx_analysis.h"
#include "../task_support/check.h"
#include "../task_support/amx-inputs.h"

/* ------------------------------------------------------------------ */
/* hand assembler helpers                                              */
/* ------------------------------------------------------------------ */

struct cursor {
    uint8_t *p;
    size_t   n;
};

static void
put(struct cursor *c, const uint8_t *b, size_t n)
{
    memcpy(c->p + c->n, b, n);
    c->n += n;
}

static void
b1(struct cursor *c, uint8_t x) { put(c, &x, 1); }

/* mov r64, imm64 ; reg: 0=rax .. 7=rdi, 8=r8 .. 15=r15 */
static void
mov_imm64(struct cursor *c, unsigned reg, uint64_t v)
{
    int i;
    b1(c, (uint8_t)(0x48 | (reg >= 8 ? 0x04 : 0)));
    b1(c, (uint8_t)(0xb8 + (reg & 7)));
    for (i = 0; i < 8; ++i) b1(c, (uint8_t)(v >> (8 * i)));
}

/* mov dst, src (both 64-bit GPRs) */
static void
mov_reg64(struct cursor *c, unsigned dst, unsigned src)
{
    uint8_t rex = (uint8_t)(0x48 | ((src >= 8) ? 0x04 : 0) |
                                   ((dst >= 8) ? 0x01 : 0));
    b1(c, rex);
    b1(c, 0x89);
    b1(c, (uint8_t)(0xc0 | ((src & 7) << 3) | (dst & 7)));
}

/* lea dst, [base + disp32] */
static void
lea_disp32(struct cursor *c, unsigned dst, unsigned base, int32_t disp)
{
    int i;
    uint8_t rex = (uint8_t)(0x48 | ((dst >= 8) ? 0x04 : 0) |
                                   ((base >= 8) ? 0x01 : 0));
    b1(c, rex);
    b1(c, 0x8d);
    b1(c, (uint8_t)(0x80 | ((dst & 7) << 3) | (base & 7)));
    for (i = 0; i < 4; ++i) b1(c, (uint8_t)(disp >> (8 * i)));
}

/* add/sub r64, imm8 (sign extended) */
static void
addsub_imm8(struct cursor *c, int is_sub, unsigned reg, int8_t imm)
{
    b1(c, (uint8_t)(0x48 | (reg >= 8 ? 0x04 : 0)));
    b1(c, 0x83);
    b1(c, (uint8_t)(0xc0 | ((is_sub ? 5 : 0) << 3) | (reg & 7)));
    b1(c, (uint8_t) imm);
}

static void nop(struct cursor *c) { b1(c, 0x90); }

static void
cmp_rax_imm8(struct cursor *c, int8_t imm)
{
    b1(c, 0x48); b1(c, 0x83); b1(c, 0xf8); b1(c, (uint8_t) imm);
}

static void
test_rax(struct cursor *c)
{
    b1(c, 0x48); b1(c, 0x85); b1(c, 0xc0);
}

/* xor eax, eax: a generic (non-modelled) write, used as a clobber */
static void
xor_eax(struct cursor *c)
{
    b1(c, 0x31); b1(c, 0xc0);
}

static void
jmp_rel32(struct cursor *c, uint64_t from, uint64_t target)
{
    int32_t rel = (int32_t)(target - (from + 5));
    int i;
    b1(c, 0xe9);
    for (i = 0; i < 4; ++i) b1(c, (uint8_t)(rel >> (8 * i)));
}

static void
jne_rel32(struct cursor *c, uint64_t from, uint64_t target)
{
    int32_t rel = (int32_t)(target - (from + 6));
    int i;
    b1(c, 0x0f); b1(c, 0x85);
    for (i = 0; i < 4; ++i) b1(c, (uint8_t)(rel >> (8 * i)));
}

/* fixed AMX blobs verified against task_support/amx-inputs.hex */
static const uint8_t blob_ldtilecfg_rip[] =
    {0xc4,0xe2,0x78,0x49,0x05,0,0,0,0};            /* ldtilecfg [rip+0] */
static const uint8_t blob_tilerelease[] =
    {0xc4,0xe2,0x78,0x49,0xc0};
static const uint8_t blob_tdpbssd_012[] =
    {0xc4,0xe2,0x6b,0x5e,0xc1};                   /* tdpbssd tmm0,tmm1,tmm2 */
static const uint8_t blob_sttilecfg_rax[] =
    {0xc4,0xe2,0x79,0x49,0x00};                   /* sttilecfg [rax] */

static void
ldtilecfg_rax(struct cursor *c)
{
    b1(c, 0xc4); b1(c, 0xe2); b1(c, 0x78); b1(c, 0x49); b1(c, 0x00);
}

/* ldtilecfg [base] */
static void
ldtilecfg_reg(struct cursor *c, unsigned base)
{
    b1(c, 0xc4); b1(c, 0xe2); b1(c, 0x78); b1(c, 0x49);
    b1(c, (uint8_t)(base & 7));
}

static void
tilezero(struct cursor *c, unsigned tile)
{
    b1(c, 0xc4); b1(c, 0xe2); b1(c, 0x7b); b1(c, 0x49);
    b1(c, (uint8_t)(0xc8 | (tile & 7)));
}

/* tileloadd tmm, [base + index] (no displacement, scale 1) */
static void
tileloadd_sib(struct cursor *c, unsigned tile, unsigned base, unsigned index)
{
    b1(c, 0xc4); b1(c, 0xe2); b1(c, 0x7b); b1(c, 0x4b);
    b1(c, (uint8_t)((tile & 7) << 3 | 0x04));            /* mod=00, rm=SIB */
    b1(c, (uint8_t)(((index & 7) << 3) | (base & 7)));  /* scale=1 */
}

/* tileloaddt1 tmm, [base + index + disp8] */
static void
tileloaddt1_sib_disp8(struct cursor *c, unsigned tile, unsigned base,
                      unsigned index, int8_t disp)
{
    b1(c, 0xc4); b1(c, 0xe2); b1(c, 0x79); b1(c, 0x4b);
    b1(c, (uint8_t)(0x44 | ((tile & 7) << 3)));          /* mod=01, rm=SIB */
    b1(c, (uint8_t)(((index & 7) << 3) | (base & 7)));
    b1(c, (uint8_t) disp);
}

/* tilestored [base + index + disp8], tmm */
static void
tilestored_sib_disp8(struct cursor *c, unsigned tile, unsigned base,
                     unsigned index, int8_t disp)
{
    b1(c, 0xc4); b1(c, 0xe2); b1(c, 0x7a); b1(c, 0x4b);
    b1(c, (uint8_t)(0x44 | ((tile & 7) << 3)));
    b1(c, (uint8_t)(((index & 7) << 3) | (base & 7)));
    b1(c, (uint8_t) disp);
}

/* ------------------------------------------------------------------ */
/* configuration and address layout                                    */
/* ------------------------------------------------------------------ */

#define CFG_ADDR_A   0x70000ull
#define CFG_ADDR_B   0x70200ull
#define DATA_BASE    0x80000ull
#define BLK_ADDR(i)  (0x10000ull + (i) * 0x200ull)

static void
make_cfg(uint8_t cfg[64], unsigned palette,
         const unsigned rows[8], const unsigned colsb[8])
{
    int i;
    memset(cfg, 0, 64);
    cfg[0] = (uint8_t) palette;
    cfg[1] = 0; /* start_row = 0 */
    for (i = 0; i < 8; ++i) {
        cfg[0x10 + i] = (uint8_t) rows[i];
        cfg[0x18 + 2 * i] = (uint8_t)(colsb[i] & 0xff);
        cfg[0x19 + 2 * i] = (uint8_t)(colsb[i] >> 8);
    }
}

static const struct ud_amx_use *
find_use(const struct ud_amx_report *r, enum ud_mnemonic_code m)
{
    size_t i;
    for (i = 0; i < r->nuses; ++i) {
        if (r->uses[i].mnemonic == m) return &r->uses[i];
    }
    return NULL;
}

static const struct ud_amx_use *
find_nth_use(const struct ud_amx_report *r, enum ud_mnemonic_code m, unsigned n)
{
    size_t i;
    unsigned seen = 0;
    for (i = 0; i < r->nuses; ++i) {
        if (r->uses[i].mnemonic == m) {
            if (seen == n) return &r->uses[i];
            ++seen;
        }
    }
    return NULL;
}

static int
has_clobber(const struct ud_amx_report *r, uint64_t addr)
{
    size_t i;
    for (i = 0; i < r->nclobber_sites; ++i) {
        if (r->clobber_sites[i] == addr) return 1;
    }
    return 0;
}

/* ============================ decode =============================== */

static int
decode_all_mnemonics(void)
{
    ud_t u;
    static const struct {
        enum ud_mnemonic_code m;
        unsigned              len;
    } expect[8] = {
        { UD_Ildtilecfg,   9 },
        { UD_Isttilecfg,   5 },
        { UD_Itilezero,    5 },
        { UD_Itileloadd,   6 },
        { UD_Itileloaddt1, 7 },
        { UD_Itdpbssd,     5 },
        { UD_Itilestored,  7 },
        { UD_Itilerelease, 5 },
    };
    unsigned i = 0;

    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, amx_inputs, sizeof amx_inputs);

    while (ud_disassemble(&u)) {
        TASK_CHECK(i < 8);
        TASK_CHECK(u.mnemonic == expect[i].m);
        TASK_CHECK(ud_insn_len(&u) == expect[i].len);
        TASK_CHECK(strstr(ud_insn_asm(&u),
                          ud_lookup_mnemonic(expect[i].m)) != NULL);
        ++i;
    }
    TASK_CHECK(i == 8);
    return 0;
}

static int
decode_tile_class_and_order(void)
{
    ud_t u;
    const ud_operand_t *o;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_input_buffer(&u, blob_tdpbssd_012, sizeof blob_tdpbssd_012);
    TASK_CHECK(ud_disassemble(&u) == sizeof blob_tdpbssd_012);
    TASK_CHECK(u.mnemonic == UD_Itdpbssd);

    /* three distinct tile registers, in operand order tmm0,tmm1,tmm2 */
    o = ud_insn_opr(&u, 0);
    TASK_CHECK(o && o->type == UD_OP_REG && o->base == UD_R_TMM0);
    o = ud_insn_opr(&u, 1);
    TASK_CHECK(o && o->type == UD_OP_REG && o->base == UD_R_TMM1);
    o = ud_insn_opr(&u, 2);
    TASK_CHECK(o && o->type == UD_OP_REG && o->base == UD_R_TMM2);

    /* tiles never alias the vector register range */
    TASK_CHECK(UD_R_TMM0 > UD_R_YMM15);
    TASK_CHECK(ud_insn_opr(&u, 0)->base < UD_R_RIP);

    /* tilezero carries exactly one tile operand and no memory access */
    {
        uint8_t b[5];
        struct cursor c = { b, 0 };
        tilezero(&c, 3);
        ud_init(&u); ud_set_mode(&u, 64);
        ud_set_input_buffer(&u, b, c.n);
        TASK_CHECK(ud_disassemble(&u) == 5);
        TASK_CHECK(u.mnemonic == UD_Itilezero);
        o = ud_insn_opr(&u, 0);
        TASK_CHECK(o && o->base == UD_R_TMM3);
        TASK_CHECK(ud_insn_opr(&u, 1) == NULL);
    }

    /* ldtilecfg is memory-only */
    ud_init(&u); ud_set_mode(&u, 64);
    ud_set_input_buffer(&u, blob_ldtilecfg_rip, sizeof blob_ldtilecfg_rip);
    TASK_CHECK(ud_disassemble(&u) == sizeof blob_ldtilecfg_rip);
    TASK_CHECK(u.mnemonic == UD_Ildtilecfg);
    o = ud_insn_opr(&u, 0);
    TASK_CHECK(o && o->type == UD_OP_MEM && o->base == UD_R_RIP);
    TASK_CHECK(ud_insn_opr(&u, 1) == NULL);
    return 0;
}

static int
decode_stride_role_and_legacy(void)
{
    ud_t u;
    uint8_t b[8];
    struct cursor c = { b, 0 };
    const ud_operand_t *m;

    /* tileloadd tmm2, [rax + rcx]: rcx is the row stride */
    tileloadd_sib(&c, 2, 0, 1);
    ud_init(&u); ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, b, c.n);
    TASK_CHECK(ud_disassemble(&u) == 6);
    TASK_CHECK(u.mnemonic == UD_Itileloadd);
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m && m->type == UD_OP_MEM);
    TASK_CHECK(m->base == UD_R_RAX && m->index == UD_R_RCX);
    TASK_CHECK(u.amx_sib_stride == 1);
    TASK_CHECK(strstr(ud_insn_asm(&u), "tileloadd") != NULL);

    /* an ordinary SIB addressing mode keeps the scaled addend, no role */
    {
        static const uint8_t plain[] =
            {0x48,0x8b,0x44,0x88,0x10}; /* mov rax, [rax+rcx*4+0x10] */
        ud_init(&u); ud_set_mode(&u, 64);
        ud_set_syntax(&u, UD_SYN_INTEL);
        ud_set_input_buffer(&u, plain, sizeof plain);
        TASK_CHECK(ud_disassemble(&u) == sizeof plain);
        TASK_CHECK(u.mnemonic == UD_Imov);
        /* 8b /r: op0 is the register destination, op1 is the memory source */
        m = ud_insn_opr(&u, 1);
        TASK_CHECK(m && m->type == UD_OP_MEM && m->index == UD_R_RCX &&
                   m->scale == 4);
        TASK_CHECK(u.amx_sib_stride == 0);
    }

    /* legacy VEX arithmetic and x87 keep working */
    {
        static const uint8_t vaddps[] = {0xc5,0xf4,0x58,0xc2};
        ud_init(&u); ud_set_mode(&u, 64);
        ud_set_input_buffer(&u, vaddps, sizeof vaddps);
        TASK_CHECK(ud_disassemble(&u) == sizeof vaddps);
        TASK_CHECK(u.mnemonic == UD_Ivaddps);
        TASK_CHECK(ud_insn_opr(&u, 0)->base == UD_R_YMM0);
    }
    {
        static const uint8_t x87[] = {0xd9,0xe8}; /* fld1 */
        ud_init(&u); ud_set_mode(&u, 64);
        ud_set_input_buffer(&u, x87, sizeof x87);
        TASK_CHECK(ud_disassemble(&u) == sizeof x87);
        TASK_CHECK(u.mnemonic == UD_Ifld1);
    }
    return 0;
}

/* ============================== flow =============================== */

static int
flow_constant_propagation(void)
{
    /* mov rax, CFG_ADDR_A ; ldtilecfg [rax] ; jmp
       mov rcx, rax ; lea rdx, [rax+0x40] ; add rdx, 0x10 ; sub rcx, 8 */
    static uint8_t b0[64], b1[64];
    static const uint64_t s0[] = { BLK_ADDR(1) };
    static const uint64_t s1[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {4,0,2,0,0,0,0,0};
    unsigned colsb[8] = {16,0,32,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, sizeof cfg };
    struct ud_amx_block blocks[2];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *u;
    struct cursor c;
    size_t n0;

    make_cfg(cfg, 1, rows, colsb);

    c = (struct cursor){ b0, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    n0 = c.n;
    jmp_rel32(&c, BLK_ADDR(0) + n0, BLK_ADDR(1));

    c = (struct cursor){ b1, 0 };
    mov_reg64(&c, 1, 0);           /* rcx = rax */
    lea_disp32(&c, 2, 0, 0x40);    /* rdx = rax + 0x40 */
    addsub_imm8(&c, 0, 2, 0x10);   /* rdx += 0x10 */
    addsub_imm8(&c, 1, 1, -8);     /* rcx -= 8 */
    nop(&c);

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, n0 + 5, s0, 1 };
    blocks[1] = (struct ud_amx_block){ BLK_ADDR(1), b1, c.n, s1, 0 };

    in = (struct ud_amx_input){ blocks, 2, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Ildtilecfg);
    TASK_CHECK(u && u->is_config_load);
    TASK_CHECK(u->nconfigs == 1);
    TASK_CHECK(u->configs[0]->kind == UD_AMX_CFG_PALETTE1);
    TASK_CHECK(u->configs[0]->rows[0] == 4);
    TASK_CHECK(u->configs[0]->colsb[2] == 32);
    TASK_CHECK(u->configs[0]->nsources == 1);
    TASK_CHECK(u->configs[0]->sources[0] == BLK_ADDR(0) + 10);
    TASK_CHECK(u->mem.base_known && u->mem.base_value == CFG_ADDR_A);
    ud_amx_report_free(r);
    return 0;
}

static int
flow_entry_register_constants(void)
{
    /* constants arrive through the entry register set, not instructions:
       rbx holds the config address, rax the data base, rcx the stride */
    static uint8_t b0[32];
    static const uint64_t s0[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {0,0,2,0,0,0,0,0};
    unsigned colsb[8] = {0,0,16,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_entry_reg eregs[3] = {
        { UD_R_RAX, DATA_BASE },
        { UD_R_RCX, 32 },
        { UD_R_RBX, CFG_ADDR_A }
    };
    struct ud_amx_block blocks[1];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *u;
    struct cursor c;

    make_cfg(cfg, 1, rows, colsb);
    c = (struct cursor){ b0, 0 };
    ldtilecfg_reg(&c, 3);           /* ldtilecfg [rbx] */
    tileloadd_sib(&c, 2, 0, 1);    /* tileloadd tmm2, [rax+rcx] */

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, c.n, s0, 0 };
    in = (struct ud_amx_input){ blocks, 1, BLK_ADDR(0),
                                eregs, 3, &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Itileloadd);
    TASK_CHECK(u && u->nfootprints == 1 && !u->footprints[0].symbolic);
    TASK_CHECK(u->mem.base_known && u->mem.base_value == DATA_BASE);
    TASK_CHECK(u->mem.stride_known && u->mem.stride_value == 32);
    TASK_CHECK(u->footprints[0].nrows == 2);
    TASK_CHECK(u->footprints[0].rows[1].lo == DATA_BASE + 32);
    ud_amx_report_free(r);
    return 0;
}

static int
flow_merge_gpr_unknown(void)
{
    /* a header block branches to two predecessors giving rax different
       constants; the join makes rax unknown while rcx stays known */
    static uint8_t bH[8], bA[32], bB[32], bJ[32];
    static const uint64_t sH[] = { BLK_ADDR(1), BLK_ADDR(2) };
    static const uint64_t sA[] = { BLK_ADDR(3) };
    static const uint64_t sB[] = { BLK_ADDR(3) };
    static const uint64_t sJ[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {2,0,3,0,0,0,0,0};
    unsigned colsb[8] = {16,0,16,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, sizeof cfg };
    struct ud_amx_block blocks[4];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *u;
    struct cursor c;

    make_cfg(cfg, 1, rows, colsb);

    c = (struct cursor){ bH, 0 };
    nop(&c);

    c = (struct cursor){ bA, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    mov_imm64(&c, 0, DATA_BASE);
    jmp_rel32(&c, BLK_ADDR(1) + 25, BLK_ADDR(3));

    c = (struct cursor){ bB, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    mov_imm64(&c, 0, DATA_BASE + 0x1000);
    jmp_rel32(&c, BLK_ADDR(2) + 25, BLK_ADDR(3));

    c = (struct cursor){ bJ, 0 };
    mov_imm64(&c, 1, 64);           /* rcx stride known on both paths */
    tileloadd_sib(&c, 2, 0, 1);    /* base rax: merged to unknown */

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), bH, 1, sH, 2 };
    blocks[1] = (struct ud_amx_block){ BLK_ADDR(1), bA, 30, sA, 1 };
    blocks[2] = (struct ud_amx_block){ BLK_ADDR(2), bB, 30, sB, 1 };
    blocks[3] = (struct ud_amx_block){ BLK_ADDR(3), bJ, c.n, sJ, 0 };
    in = (struct ud_amx_input){ blocks, 4, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Itileloadd);
    TASK_CHECK(u && u->nfootprints == 1);
    TASK_CHECK(u->footprints[0].symbolic);
    TASK_CHECK(!u->mem.base_known);
    TASK_CHECK(u->mem.stride_known && u->mem.stride_value == 64);
    TASK_CHECK(strstr(u->footprints[0].unknown_what, "base") != NULL);
    ud_amx_report_free(r);
    return 0;
}

static int
flow_merge_configs_same_content(void)
{
    /* identical config bytes at two addresses; after the join they merge
       into one shape carrying both load addresses */
    static uint8_t bH[8], bA[32], bB[32], bJ[32];
    static const uint64_t sH[] = { BLK_ADDR(1), BLK_ADDR(2) };
    static const uint64_t sA[] = { BLK_ADDR(3) };
    static const uint64_t sB[] = { BLK_ADDR(3) };
    static const uint64_t sJ[] = { 0 };
    static uint8_t cfgA[64], cfgB[64];
    unsigned rows[8] = {1,1,1,1,0,0,0,0};
    unsigned colsb[8] = {16,16,16,16,0,0,0,0};
    struct ud_amx_ro_region regions[2] = {
        { CFG_ADDR_A, cfgA, 64 }, { CFG_ADDR_B, cfgB, 64 }
    };
    struct ud_amx_block blocks[4];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *u;
    struct cursor c;

    make_cfg(cfgA, 1, rows, colsb);
    make_cfg(cfgB, 1, rows, colsb);

    c = (struct cursor){ bH, 0 };
    nop(&c);

    c = (struct cursor){ bA, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    jmp_rel32(&c, BLK_ADDR(1) + 15, BLK_ADDR(3));

    c = (struct cursor){ bB, 0 };
    mov_imm64(&c, 0, CFG_ADDR_B);
    ldtilecfg_rax(&c);
    jmp_rel32(&c, BLK_ADDR(2) + 15, BLK_ADDR(3));

    c = (struct cursor){ bJ, 0 };
    mov_imm64(&c, 0, DATA_BASE);
    mov_imm64(&c, 1, 16);
    tileloadd_sib(&c, 0, 0, 1);

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), bH, 1, sH, 2 };
    blocks[1] = (struct ud_amx_block){ BLK_ADDR(1), bA, 20, sA, 1 };
    blocks[2] = (struct ud_amx_block){ BLK_ADDR(2), bB, 20, sB, 1 };
    blocks[3] = (struct ud_amx_block){ BLK_ADDR(3), bJ, c.n, sJ, 0 };
    in = (struct ud_amx_input){ blocks, 4, BLK_ADDR(0), NULL, 0,
                                regions, 2 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Itileloadd);
    TASK_CHECK(u && u->nconfigs == 1);
    TASK_CHECK(u->configs[0]->kind == UD_AMX_CFG_PALETTE1);
    TASK_CHECK(u->configs[0]->nsources == 2);
    /* per-tile association of the single merged config is retained */
    TASK_CHECK(u->configs[0]->rows[3] == 1);
    TASK_CHECK(u->nfootprints == 1);
    ud_amx_report_free(r);
    return 0;
}

static int
flow_loop_convergence(void)
{
    /* config loaded before the loop stays known across the back edge;
       nop/cmp/test and the direct branch preserve GPR constants */
    static uint8_t bH[64], bL[64];
    static const uint64_t sH[] = { BLK_ADDR(1) };
    static const uint64_t sL[] = { BLK_ADDR(1), BLK_ADDR(2) };
    static const uint64_t sX[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {0,0,4,0,0,0,0,0};
    unsigned colsb[8] = {0,0,16,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[3];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *load, *tile;
    struct cursor c;
    size_t lstart, hsize;

    make_cfg(cfg, 1, rows, colsb);

    c = (struct cursor){ bH, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    mov_imm64(&c, 0, DATA_BASE);
    mov_imm64(&c, 1, 32);
    jmp_rel32(&c, BLK_ADDR(0) + c.n, BLK_ADDR(1));
    hsize = c.n;

    c = (struct cursor){ bL, 0 };
    nop(&c);
    test_rax(&c);
    cmp_rax_imm8(&c, 5);
    tileloadd_sib(&c, 2, 0, 1);
    lstart = c.n;
    jne_rel32(&c, BLK_ADDR(1) + lstart, BLK_ADDR(1)); /* back edge */

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), bH, hsize, sH, 1 };
    blocks[1] = (struct ud_amx_block){ BLK_ADDR(1), bL, c.n, sL, 2 };
    blocks[2] = (struct ud_amx_block){ BLK_ADDR(2), bL, 0, sX, 0 };
    in = (struct ud_amx_input){ blocks, 3, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    load = find_use(r, UD_Ildtilecfg);
    tile = find_use(r, UD_Itileloadd);
    TASK_CHECK(load && load->nconfigs == 1 &&
               load->configs[0]->kind == UD_AMX_CFG_PALETTE1);
    TASK_CHECK(tile && tile->nconfigs == 1);
    TASK_CHECK(tile->configs[0]->kind == UD_AMX_CFG_PALETTE1);
    TASK_CHECK(tile->configs[0]->rows[2] == 4);
    TASK_CHECK(tile->mem.base_known && tile->mem.base_value == DATA_BASE);
    TASK_CHECK(tile->mem.stride_known && tile->mem.stride_value == 32);
    ud_amx_report_free(r);
    return 0;
}

static int
flow_unknown_then_reconfigured(void)
{
    /* an unresolvable load creates the unknown state; a later determined
       load restores a concrete configuration */
    static uint8_t b0[32], b1[64];
    static const uint64_t s0[] = { BLK_ADDR(1) };
    static const uint64_t s1[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {2,2,2,0,0,0,0,0};
    unsigned colsb[8] = {16,16,16,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[2];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *first, *second;
    struct cursor c;

    make_cfg(cfg, 1, rows, colsb);

    /* rax is never set here: the first load cannot be located */
    c = (struct cursor){ b0, 0 };
    ldtilecfg_rax(&c);
    jmp_rel32(&c, BLK_ADDR(0) + c.n, BLK_ADDR(1));
    {
        size_t n0 = c.n;
        blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, n0, s0, 1 };
    }

    c = (struct cursor){ b1, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);

    blocks[1] = (struct ud_amx_block){ BLK_ADDR(1), b1, c.n, s1, 0 };
    in = (struct ud_amx_input){ blocks, 2, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    first = find_nth_use(r, UD_Ildtilecfg, 0);
    second = find_nth_use(r, UD_Ildtilecfg, 1);
    TASK_CHECK(first && first->nconfigs == 1 &&
               first->configs[0]->kind == UD_AMX_CFG_UNKNOWN);
    TASK_CHECK(second && second->nconfigs == 1 &&
               second->configs[0]->kind == UD_AMX_CFG_PALETTE1);
    TASK_CHECK(second->configs[0]->rows[1] == 2);
    ud_amx_report_free(r);
    return 0;
}

static int
flow_unknown_known_coexist(void)
{
    /* one predecessor's load cannot be located (unknown config), the
       other's resolves to palette 1; after the join both states survive
       rather than one being dropped or merged into a fabricated shape */
    static uint8_t bH[8], bA[16], bB[32], bJ[16];
    static const uint64_t sH[] = { BLK_ADDR(1), BLK_ADDR(2) };
    static const uint64_t sA[] = { BLK_ADDR(3) };
    static const uint64_t sB[] = { BLK_ADDR(3) };
    static const uint64_t sJ[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {1,1,0,0,0,0,0,0};
    unsigned colsb[8] = {16,16,0,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_entry_reg ereg = { UD_R_RCX, 16 };
    struct ud_amx_block blocks[4];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *u;
    struct cursor c;
    int saw_unknown = 0, saw_palette = 0;
    size_t k;

    make_cfg(cfg, 1, rows, colsb);

    c = (struct cursor){ bH, 0 };
    nop(&c);

    /* rax deliberately never set on this path: load is unlocatable */
    c = (struct cursor){ bA, 0 };
    ldtilecfg_rax(&c);
    jmp_rel32(&c, BLK_ADDR(1) + c.n, BLK_ADDR(3));

    c = (struct cursor){ bB, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    jmp_rel32(&c, BLK_ADDR(2) + c.n, BLK_ADDR(3));

    c = (struct cursor){ bJ, 0 };
    tileloadd_sib(&c, 0, 0, 1);

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), bH, 1, sH, 2 };
    blocks[1] = (struct ud_amx_block){ BLK_ADDR(1), bA, 10, sA, 1 };
    blocks[2] = (struct ud_amx_block){ BLK_ADDR(2), bB, 20, sB, 1 };
    blocks[3] = (struct ud_amx_block){ BLK_ADDR(3), bJ, c.n, sJ, 0 };
    in = (struct ud_amx_input){ blocks, 4, BLK_ADDR(0), &ereg, 1,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Itileloadd);
    TASK_CHECK(u && u->nconfigs == 2);   /* unknown and known coexist */
    for (k = 0; k < u->nconfigs; ++k) {
        if (u->configs[k]->kind == UD_AMX_CFG_UNKNOWN) saw_unknown = 1;
        if (u->configs[k]->kind == UD_AMX_CFG_PALETTE1) saw_palette = 1;
    }
    TASK_CHECK(saw_unknown && saw_palette);
    /* the known config keeps its own per-tile association */
    for (k = 0; k < u->nconfigs; ++k) {
        if (u->configs[k]->kind == UD_AMX_CFG_PALETTE1) {
            TASK_CHECK(u->configs[k]->rows[0] == 1 &&
                       u->configs[k]->colsb[1] == 16);
        }
    }
    ud_amx_report_free(r);
    return 0;
}

static int
flow_release_store_and_clobber(void)
{
    static uint8_t b0[64];
    static const uint64_t s0[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {1,0,0,0,0,0,0,0};
    unsigned colsb[8] = {16,0,0,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[1];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *store, *rel, *load;
    uint64_t clob_addr;
    struct cursor c;

    make_cfg(cfg, 1, rows, colsb);
    c = (struct cursor){ b0, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    clob_addr = BLK_ADDR(0) + c.n;
    xor_eax(&c);                    /* drops GPR constants and config */
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    put(&c, blob_sttilecfg_rax, sizeof blob_sttilecfg_rax);
    put(&c, blob_tilerelease, sizeof blob_tilerelease);

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, c.n, s0, 0 };
    in = (struct ud_amx_input){ blocks, 1, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    TASK_CHECK(has_clobber(r, clob_addr));

    /* STTILECFG is a write only, never a new configuration source */
    store = find_use(r, UD_Isttilecfg);
    TASK_CHECK(store && store->is_config_write && !store->is_config_load);
    TASK_CHECK(store->nfootprints == 0 && store->ntiles == 0);

    /* TILERELEASE drives the state to released */
    rel = find_use(r, UD_Itilerelease);
    TASK_CHECK(rel && rel->nconfigs == 1 &&
               rel->configs[0]->kind == UD_AMX_CFG_RELEASED);

    /* the concrete load after the clobber is still recovered */
    load = find_nth_use(r, UD_Ildtilecfg, 1);
    TASK_CHECK(load && load->configs[0]->kind == UD_AMX_CFG_PALETTE1);
    ud_amx_report_free(r);
    return 0;
}

/* ============================ footprint ============================ */

static int
fp_per_row_stride_overlap(void)
{
    static uint8_t b0[64];
    static const uint64_t s0[] = { 0 };
    uint8_t cfg[64];
    /* 3 rows of 16 bytes with an 8-byte stride: rows overlap */
    unsigned rows[8] = {0,0,3,0,0,0,0,0};
    unsigned colsb[8] = {0,0,16,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[1];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *u;
    const struct ud_amx_footprint *fp;
    struct cursor c;

    make_cfg(cfg, 1, rows, colsb);
    c = (struct cursor){ b0, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    mov_imm64(&c, 0, DATA_BASE);
    mov_imm64(&c, 1, 8);             /* rcx stride = 8 */
    tileloadd_sib(&c, 2, 0, 1);

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, c.n, s0, 0 };
    in = (struct ud_amx_input){ blocks, 1, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Itileloadd);
    TASK_CHECK(u && u->nfootprints == 1);
    fp = &u->footprints[0];
    TASK_CHECK(!fp->symbolic);
    TASK_CHECK(fp->nrows == 3);      /* individual rows, no enclosing box */
    TASK_CHECK(fp->rows[0].row == 0 && fp->rows[1].row == 1 &&
               fp->rows[2].row == 2);
    TASK_CHECK(fp->rows[0].lo == DATA_BASE &&
               fp->rows[0].hi == DATA_BASE + 16);
    TASK_CHECK(fp->rows[1].lo == DATA_BASE + 8 &&
               fp->rows[1].hi == DATA_BASE + 24);
    TASK_CHECK(fp->rows[2].lo == DATA_BASE + 16 &&
               fp->rows[2].hi == DATA_BASE + 32);
    TASK_CHECK(fp->config == u->configs[0]);
    ud_amx_report_free(r);
    return 0;
}

static int
fp_symbolic_missing_components(void)
{
    static uint8_t b0[64];
    static const uint64_t s0[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {0,0,2,0,0,0,0,0};
    unsigned colsb[8] = {0,0,16,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[1];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *u;
    struct cursor c;

    make_cfg(cfg, 1, rows, colsb);
    c = (struct cursor){ b0, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    /* rax base known, rcx stride unknown */
    mov_imm64(&c, 0, DATA_BASE);
    tileloadd_sib(&c, 2, 0, 1);

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, c.n, s0, 0 };
    in = (struct ud_amx_input){ blocks, 1, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Itileloadd);
    TASK_CHECK(u && u->nfootprints == 1 && u->footprints[0].symbolic);
    TASK_CHECK(u->mem.base_known && !u->mem.stride_known);
    TASK_CHECK(u->mem.index_reg == UD_R_RCX);
    TASK_CHECK(u->mem.stride_role);
    TASK_CHECK(strstr(u->footprints[0].unknown_what, "stride") != NULL);
    TASK_CHECK(u->footprints[0].nrows == 1 &&
               u->footprints[0].rows[0].row == UD_AMX_ROW_UNKNOWN);
    ud_amx_report_free(r);
    return 0;
}

static int
fp_no_data_rows_for_non_transfers(void)
{
    static uint8_t b0[64];
    static const uint64_t s0[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {1,1,1,1,0,0,0,0};
    unsigned colsb[8] = {16,16,16,16,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[1];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *z, *st, *ld;
    struct cursor c;

    make_cfg(cfg, 1, rows, colsb);
    c = (struct cursor){ b0, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    tilezero(&c, 1);
    put(&c, blob_sttilecfg_rax, sizeof blob_sttilecfg_rax);

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, c.n, s0, 0 };
    in = (struct ud_amx_input){ blocks, 1, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    z = find_use(r, UD_Itilezero);
    st = find_use(r, UD_Isttilecfg);
    ld = find_use(r, UD_Ildtilecfg);
    TASK_CHECK(z && z->ntiles == 1 && z->tiles[0].role == UD_AMX_TILE_WRITE);
    TASK_CHECK(z->nfootprints == 0 && z->mem.present == 0);
    TASK_CHECK(st && st->nfootprints == 0);
    TASK_CHECK(ld && ld->nfootprints == 0);
    ud_amx_report_free(r);
    return 0;
}

static int
fp_tdpbssd_roles_and_dims(void)
{
    /* tmm0 = MxN (4x16), tmm1 = MxK (4x16), tmm2 = KxN (16x16);
       every colsb is a legal palette-1 value {16,32,48,64} */
    static uint8_t good[64], bad[64];
    static const uint64_t s0[] = { 0 };
    uint8_t cfg[64];
    unsigned rows_g[8] = {4,4,16,0,0,0,0,0};
    unsigned col_g[8]  = {16,16,16,0,0,0,0,0};
    unsigned rows_b[8] = {4,4,16,0,0,0,0,0};
    unsigned col_b[8]  = {16,32,16,0,0,0,0,0}; /* K: 32 != rows2 16 */
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[1];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *u;
    struct cursor c;

    /* compatible */
    make_cfg(cfg, 1, rows_g, col_g);
    c = (struct cursor){ good, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    put(&c, blob_tdpbssd_012, sizeof blob_tdpbssd_012);
    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), good, c.n, s0, 0 };
    in = (struct ud_amx_input){ blocks, 1, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Itdpbssd);
    TASK_CHECK(u && u->ntiles == 3);
    TASK_CHECK(u->tiles[0].tile == 0 && u->tiles[0].role == UD_AMX_TILE_RDWR);
    TASK_CHECK(u->tiles[1].tile == 1 && u->tiles[1].role == UD_AMX_TILE_READ);
    TASK_CHECK(u->tiles[2].tile == 2 && u->tiles[2].role == UD_AMX_TILE_READ);
    TASK_CHECK(u->tiles[0].op_index == 0 && u->tiles[1].op_index == 1 &&
               u->tiles[2].op_index == 2);
    TASK_CHECK(u->dims_compatible == 1);
    TASK_CHECK(u->nconfigs == 1 && u->cfg_dims[0] == 1);
    ud_amx_report_free(r);

    /* incompatible */
    make_cfg(cfg, 1, rows_b, col_b);
    c = (struct cursor){ bad, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    put(&c, blob_tdpbssd_012, sizeof blob_tdpbssd_012);
    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), bad, c.n, s0, 0 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    u = find_use(r, UD_Itdpbssd);
    TASK_CHECK(u && u->dims_compatible == 0 && u->cfg_dims[0] == 0);
    ud_amx_report_free(r);
    return 0;
}

static int
fp_t1_and_store_variants(void)
{
    /* tileloaddt1 with disp8 and tilestored both produce per-row ranges;
       the displacement belongs to every row's base, not to the stride */
    static uint8_t b0[80];
    static const uint64_t s0[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {0,0,2,1,0,0,0,0};
    unsigned colsb[8] = {0,0,32,16,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[1];
    struct ud_amx_input in;
    struct ud_amx_report *r;
    const struct ud_amx_use *t1, *st;
    struct cursor c;

    make_cfg(cfg, 1, rows, colsb);
    c = (struct cursor){ b0, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    mov_imm64(&c, 0, DATA_BASE);
    mov_imm64(&c, 1, 64);
    tileloaddt1_sib_disp8(&c, 2, 0, 1, 0x20);   /* [rax+rcx+0x20] */
    tilestored_sib_disp8(&c, 3, 0, 1, 0x10);    /* [rax+rcx+0x10] */

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, c.n, s0, 0 };
    in = (struct ud_amx_input){ blocks, 1, BLK_ADDR(0), NULL, 0,
                                &region, 1 };
    r = ud_amx_analyze(&in);
    TASK_CHECK(r != NULL);
    t1 = find_use(r, UD_Itileloaddt1);
    st = find_use(r, UD_Itilestored);
    TASK_CHECK(t1 && st);
    TASK_CHECK(t1->nfootprints == 1 && st->nfootprints == 1);
    /* t1: tmm2 = 2 rows of 32B, base DATA_BASE+0x20, stride 64 */
    TASK_CHECK(t1->footprints[0].nrows == 2 && t1->footprints[0].tile == 2);
    TASK_CHECK(t1->footprints[0].rows[0].lo == DATA_BASE + 0x20);
    TASK_CHECK(t1->footprints[0].rows[0].hi == DATA_BASE + 0x20 + 32);
    TASK_CHECK(t1->footprints[0].rows[1].lo == DATA_BASE + 0x20 + 64);
    /* store: tmm3 = 1 row of 16B, base DATA_BASE+0x10 */
    TASK_CHECK(st->footprints[0].nrows == 1 && st->footprints[0].tile == 3);
    TASK_CHECK(st->footprints[0].rows[0].lo == DATA_BASE + 0x10);
    TASK_CHECK(st->footprints[0].rows[0].hi == DATA_BASE + 0x10 + 16);
    TASK_CHECK(st->tiles[0].role == UD_AMX_TILE_READ);
    ud_amx_report_free(r);
    return 0;
}

static int
fp_report_independence(void)
{
    static uint8_t b0[64];
    static uint8_t snap[64];
    static const uint64_t s0[] = { 0 };
    uint8_t cfg[64];
    unsigned rows[8] = {0,0,2,0,0,0,0,0};
    unsigned colsb[8] = {0,0,16,0,0,0,0,0};
    struct ud_amx_ro_region region = { CFG_ADDR_A, cfg, 64 };
    struct ud_amx_block blocks[1];
    struct ud_amx_input in;
    struct ud_amx_report *r1, *r2;
    const struct ud_amx_use *u1, *u2;
    struct cursor c;
    size_t n;

    make_cfg(cfg, 1, rows, colsb);
    c = (struct cursor){ b0, 0 };
    mov_imm64(&c, 0, CFG_ADDR_A);
    ldtilecfg_rax(&c);
    mov_imm64(&c, 0, DATA_BASE);
    mov_imm64(&c, 1, 16);
    tileloadd_sib(&c, 2, 0, 1);
    n = c.n;
    memcpy(snap, b0, n);

    blocks[0] = (struct ud_amx_block){ BLK_ADDR(0), b0, n, s0, 0 };
    in = (struct ud_amx_input){ blocks, 1, BLK_ADDR(0), NULL, 0,
                                &region, 1 };

    r1 = ud_amx_analyze(&in);
    TASK_CHECK(r1 != NULL);
    r2 = ud_amx_analyze(&in);
    TASK_CHECK(r2 != NULL);
    /* repeated queries do not mutate the caller's instruction bytes */
    TASK_CHECK(memcmp(snap, b0, n) == 0);
    u1 = find_use(r1, UD_Itileloadd);
    u2 = find_use(r2, UD_Itileloadd);
    TASK_CHECK(u1 && u2 && u2->footprints[0].nrows == 2);
    ud_amx_report_free(r1);
    /* r2 stays valid after r1 is released: reports own their data */
    TASK_CHECK(u2->configs[0]->rows[2] == 2);
    TASK_CHECK(u2->footprints[0].rows[1].hi == DATA_BASE + 32);
    ud_amx_report_free(r2);
    return 0;
}

/* ============================ registry ============================ */

#define CASE(g, n) { g, #n, n }

const struct task_case task_cases[] = {
    CASE("decode", decode_all_mnemonics),
    CASE("decode", decode_tile_class_and_order),
    CASE("decode", decode_stride_role_and_legacy),
    CASE("flow", flow_constant_propagation),
    CASE("flow", flow_entry_register_constants),
    CASE("flow", flow_merge_gpr_unknown),
    CASE("flow", flow_merge_configs_same_content),
    CASE("flow", flow_loop_convergence),
    CASE("flow", flow_unknown_then_reconfigured),
    CASE("flow", flow_unknown_known_coexist),
    CASE("flow", flow_release_store_and_clobber),
    CASE("footprint", fp_per_row_stride_overlap),
    CASE("footprint", fp_symbolic_missing_components),
    CASE("footprint", fp_no_data_rows_for_non_transfers),
    CASE("footprint", fp_tdpbssd_roles_and_dims),
    CASE("footprint", fp_t1_and_store_variants),
    CASE("footprint", fp_report_independence),
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
