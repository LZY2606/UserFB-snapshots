/***************************************************************************************************

  Task test suite for the symbolic provenance ("compaction DAG") component.

  The production graph is checked against an independent, deliberately simple per-lane
  reference simulator. Concrete entry masks are enumerated only here, in the test, and only
  in small fixed sets; the production code never enumerates them.

***************************************************************************************************/

#include <stdint.h>
#include <string.h>

#include <Zydis/Zydis.h>
#include <Zydis/Compact.h>

#include "../task_support/check.h"
#include "../task_support/compact-inputs.h"

/* ============================================================================================== */
/* Byte fixtures (generated with the system assembler; see task_support/compact-inputs.s for the  */
/* reference style).                                                                              */
/* ============================================================================================== */

/* Register/vector/memory forms, widths, merge/zeroing, same-register operands, disp32. */
static const unsigned char bytes_t2[] = {
    0x62,0xf1,0x7e,0x28,0x6f,0xca,                       /* vmovdqu32 ymm1, ymm2          */
    0x62,0xf1,0x7e,0x08,0x6f,0xca,                       /* vmovdqu32 xmm1, xmm2          */
    0x62,0x72,0x7d,0x49,0x8b,0xc7,                       /* vpcompressd zmm7{k1}, zmm8    */
    0x62,0x72,0x7d,0xc9,0x8b,0xc7,                       /* vpcompressd zmm7{k1}{z}, zmm8 */
    0x62,0x52,0x7d,0x2a,0x8b,0xc8,                       /* vpcompressd ymm8{k2}, ymm9    */
    0x62,0x52,0x7d,0xaa,0x8b,0xc8,                       /* vpcompressd ymm8{k2}{z}, ymm9 */
    0x62,0x52,0x7d,0x0b,0x8b,0xd1,                       /* vpcompressd xmm9{k3}, xmm10   */
    0x62,0x52,0x7d,0x8b,0x8b,0xd1,                       /* vpcompressd xmm9{k3}{z},xmm10 */
    0x62,0x52,0x7d,0x4c,0x89,0xd3,                       /* vpexpandd zmm10{k4}, zmm11    */
    0x62,0x52,0x7d,0xcc,0x89,0xd3,                       /* vpexpandd zmm10{k4}{z}, zmm11 */
    0x62,0x52,0x7d,0x2d,0x89,0xdc,                       /* vpexpandd ymm11{k5}, ymm12    */
    0x62,0x52,0x7d,0x0e,0x89,0xe5,                       /* vpexpandd xmm12{k6}, xmm13    */
    0x62,0xf2,0x7d,0x49,0x8b,0xed,                       /* vpcompressd zmm5{k1}, zmm5    */
    0x62,0xf2,0x7d,0xca,0x89,0xf6,                       /* vpexpandd zmm6{k2}{z}, zmm6   */
    0x62,0xf1,0x7e,0x4b,0x6f,0xff,                       /* vmovdqu32 zmm7{k3}, zmm7      */
    0x62,0xf2,0x7d,0x49,0x8b,0x14,0x25,0x00,0x10,0x40,0x00, /* vpcompressd [0x401000]{k1}, zmm2 */
    0x62,0xf2,0x7d,0x4a,0x8b,0x1c,0x25,0x04,0x10,0x40,0x00, /* vpcompressd [0x401004]{k2}, zmm3 */
    0x62,0xf2,0x7d,0x4b,0x89,0x34,0x25,0x00,0x10,0x40,0x00, /* vpexpandd zmm6{k3}, [0x401000]   */
    0x62,0xf2,0x7d,0x4c,0x89,0x3c,0x25,0x04,0x10,0x40,0x00, /* vpexpandd zmm7{k4}, [0x401004]   */
    0x62,0xf2,0x7d,0x2d,0x8b,0x14,0x25,0x00,0x20,0x40,0x00, /* vpcompressd [0x402000]{k5}, ymm2 */
    0x62,0x72,0x7d,0x2e,0x89,0x04,0x25,0x00,0x20,0x40,0x00, /* vpexpandd ymm8{k6}, [0x402000]   */
    0x62,0xf2,0x7d,0x0f,0x8b,0x14,0x25,0x00,0x30,0x40,0x00, /* vpcompressd [0x403000]{k7}, xmm2 */
    0x62,0x72,0x7d,0x09,0x89,0x0c,0x25,0x00,0x30,0x40,0x00, /* vpexpandd xmm9{k1}, [0x403000]   */
    0x62,0xf2,0x7d,0x49,0x8b,0x10,                       /* vpcompressd [rax]{k1}, zmm2   */
    0x62,0xf2,0x7d,0x4b,0x89,0x33,                       /* vpexpandd zmm6{k3}, [rbx]     */
    0x62,0xf1,0x7e,0x49,0x6f,0x08,                       /* vmovdqu32 zmm1{k1}, [rax]     */
    0x90,                                                /* nop                           */
    0x62,0xf1,0x6d,0x48,0xef,0xcb,                       /* vpxord zmm1, zmm2, zmm3       */
    0xc5,0xf9,0x90,0xca                                  /* kmovb k1, k2                  */
};

struct t2_span { unsigned off, len; };
static const struct t2_span t2_spans[] = {
    {  0, 6 }, {  6, 6 }, { 12, 6 }, { 18, 6 }, { 24, 6 }, { 30, 6 },
    { 36, 6 }, { 42, 6 }, { 48, 6 }, { 54, 6 }, { 60, 6 }, { 66, 6 },
    { 72, 6 }, { 78, 6 }, { 84, 6 },
    { 90, 11 }, { 101, 11 }, { 112, 11 }, { 123, 11 },
    { 134, 11 }, { 145, 11 }, { 156, 11 }, { 167, 11 },
    { 178, 6 }, { 184, 6 }, { 190, 6 }, { 196, 1 }, { 197, 6 }, { 203, 4 }
};

/* k0 as an ordinary mask-register operand and deliberately misaligned disp32 addresses. */
static const unsigned char bytes_t3[] = {
    0xc5,0xf8,0x90,0xc1,                                 /* kmovw k0, k1                  */
    0xc5,0xec,0x41,0xc3,                                 /* kandw k0, k2, k3              */
    0xc5,0xdc,0x45,0xc5,                                 /* korw k0, k4, k5               */
    0xc5,0xf8,0x44,0xc6,                                 /* knotw k0, k6                  */
    0x62,0xf2,0x7d,0x49,0x8b,0x14,0x25,0x02,0x10,0x40,0x00, /* vpcompressd [0x401002]{k1},zmm2 */
    0x62,0xf2,0x7d,0x4b,0x89,0x34,0x25,0x06,0x10,0x40,0x00  /* vpexpandd zmm6{k3},[0x401006]    */
};

/* Standalone mask-register instructions (verified encodings, independent of the fixture). */
static const unsigned char bytes_kor[]  = { 0xc5,0xe4,0x45,0xd4 }; /* korw k2, k3, k4      */
static const unsigned char bytes_kxor[] = { 0xc5,0xdc,0x47,0xdd }; /* kxorw k3, k4, k5     */
static const unsigned char bytes_kshr[] = { 0xc4,0xe3,0xf9,0x30,0xf9,0x05 }; /* kshiftrw k7,k1,5 */

/* Unmasked (no {k}) compaction forms: results must not depend on any entry mask. */
static const unsigned char bytes_unmasked[] = {
    0x62,0x72,0x7d,0x48,0x8b,0xc7,                         /* vpcompressd zmm7, zmm8        */
    0x62,0x52,0x7d,0x28,0x8b,0xc8,                         /* vpcompressd ymm8, ymm9        */
    0x62,0x52,0x7d,0x48,0x89,0xd3,                         /* vpexpandd zmm10, zmm11        */
    0x62,0x52,0x7d,0x08,0x89,0xe5,                         /* vpexpandd xmm12, xmm13        */
    0x62,0xf2,0x7d,0x48,0x8b,0x14,0x25,0x00,0x10,0x40,0x00, /* vpcompressd [0x401000], zmm2  */
    0x62,0xf2,0x7d,0x48,0x89,0x34,0x25,0x00,0x10,0x40,0x00  /* vpexpandd zmm6, [0x401000]    */
};
struct um_span { unsigned off, len; };
static const struct um_span um_spans[] = {
    { 0, 6 }, { 6, 6 }, { 12, 6 }, { 18, 6 }, { 24, 11 }, { 35, 11 }
};

/* ============================================================================================== */
/* Independent reference simulator                                                                */
/* ============================================================================================== */

#define REF_ZMM 32
#define REF_LANES 16
#define REF_K 8
#define REF_MEM_CELLS 256

enum ref_kind
{
    REF_ZERO,
    REF_LANE,
    REF_MEM
};

struct ref_tag
{
    enum ref_kind kind;
    uint8_t zmm;
    uint8_t lane;
    uint64_t addr;
};

struct ref_mem
{
    uint64_t addr;
    struct ref_tag tag;
    int used;
};

struct ref_state
{
    struct ref_tag vec[REF_ZMM][REF_LANES];
    uint16_t k[REF_K];
    struct ref_mem mem[REF_MEM_CELLS];
    unsigned mem_count;
};

static unsigned ref_popcount(uint16_t v)
{
    unsigned n = 0;
    while (v) { v &= (uint16_t)(v - 1u); ++n; }
    return n;
}

static struct ref_tag ref_lane_tag(uint8_t zmm, uint8_t lane)
{
    struct ref_tag t = { REF_LANE, zmm, lane, 0 };
    return t;
}

static struct ref_tag ref_mem_tag(uint64_t addr)
{
    struct ref_tag t = { REF_MEM, 0, 0, addr };
    return t;
}

static struct ref_tag ref_zero_tag(void)
{
    struct ref_tag t = { REF_ZERO, 0, 0, 0 };
    return t;
}

static void ref_init(struct ref_state* st)
{
    memset(st, 0, sizeof(*st));
    for (int z = 0; z < REF_ZMM; ++z)
        for (int l = 0; l < REF_LANES; ++l)
            st->vec[z][l] = ref_lane_tag((uint8_t)z, (uint8_t)l);
}

static struct ref_tag ref_mem_get(struct ref_state* st, uint64_t addr)
{
    for (unsigned i = 0; i < st->mem_count; ++i)
        if (st->mem[i].used && st->mem[i].addr == addr)
            return st->mem[i].tag;
    /* Lazy entry cell, mirroring the graph's ENTRY_MEM materialization. */
    st->mem[st->mem_count].addr = addr;
    st->mem[st->mem_count].tag = ref_mem_tag(addr);
    st->mem[st->mem_count].used = 1;
    ++st->mem_count;
    return ref_mem_tag(addr);
}

static void ref_mem_set(struct ref_state* st, uint64_t addr, struct ref_tag tag)
{
    for (unsigned i = 0; i < st->mem_count; ++i)
    {
        if (st->mem[i].used && st->mem[i].addr == addr)
        {
            st->mem[i].tag = tag;
            return;
        }
    }
    st->mem[st->mem_count].addr = addr;
    st->mem[st->mem_count].tag = tag;
    st->mem[st->mem_count].used = 1;
    ++st->mem_count;
}

/* ---------------------------------------------------------------------------------------------- */
/* Decoded instruction descriptor (test-side decode, independent of the production walker)       */
/* ---------------------------------------------------------------------------------------------- */

enum desc_kind
{
    D_KMOV, D_KAND, D_KOR, D_KXOR, D_KNOT, D_KSHL, D_KSHR,
    D_MOVE, D_COMPRESS, D_EXPAND, D_OTHER
};

struct desc
{
    enum desc_kind kind;
    uint8_t dst, s1, s2;
    uint8_t km;
    int has_mask;
    int zeroing;
    uint8_t lanes;
    uint8_t imm;
    int is_mem;
    uint64_t addr;
};

static int desc_decode(const unsigned char* bytes, unsigned len, uint64_t runtime,
    struct desc* out)
{
    ZydisDecoder dec;
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64,
        ZYDIS_STACK_WIDTH_64)))
        return 0;
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, bytes, len, &ins, ops)))
        return 0;

    memset(out, 0, sizeof(*out));
    out->kind = D_OTHER;
    out->dst = out->s1 = out->s2 = 0xFF;
    out->lanes = (uint8_t)(ins.avx.vector_length / 32u);
    out->zeroing = (ins.avx.mask.mode == ZYDIS_MASK_MODE_ZEROING) ||
                   (ins.avx.mask.mode == ZYDIS_MASK_MODE_CONTROL_ZEROING);
    out->has_mask = (ins.avx.mask.reg != ZYDIS_REGISTER_K0) &&
                    (ins.avx.mask.mode != ZYDIS_MASK_MODE_DISABLED) &&
                    (ins.avx.mask.mode != ZYDIS_MASK_MODE_NONE);
    if (out->has_mask)
        out->km = (uint8_t)ZydisRegisterGetId(ins.avx.mask.reg);

    switch (ins.mnemonic)
    {
    case ZYDIS_MNEMONIC_KMOVW: out->kind = D_KMOV; break;
    case ZYDIS_MNEMONIC_KANDW: out->kind = D_KAND; break;
    case ZYDIS_MNEMONIC_KORW:  out->kind = D_KOR; break;
    case ZYDIS_MNEMONIC_KXORW: out->kind = D_KXOR; break;
    case ZYDIS_MNEMONIC_KNOTW: out->kind = D_KNOT; break;
    case ZYDIS_MNEMONIC_KSHIFTLW: out->kind = D_KSHL; break;
    case ZYDIS_MNEMONIC_KSHIFTRW: out->kind = D_KSHR; break;
    case ZYDIS_MNEMONIC_VMOVDQU32: out->kind = D_MOVE; break;
    case ZYDIS_MNEMONIC_VPCOMPRESSD: out->kind = D_COMPRESS; break;
    case ZYDIS_MNEMONIC_VPEXPANDD: out->kind = D_EXPAND; break;
    default: return 1;
    }

    for (ZyanU8 i = 0; i < ins.operand_count; ++i)
    {
        const ZydisDecodedOperand* op = &ops[i];
        if (op->visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN)
            continue;
        if (op->type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
        {
            out->imm = (uint8_t)op->imm.value.u;
            continue;
        }
        if (op->type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            out->is_mem = 1;
            ZyanU64 addr;
            if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, op, runtime, &addr)))
                return 0;
            out->addr = addr;
            continue;
        }
        if (op->type != ZYDIS_OPERAND_TYPE_REGISTER)
            continue;
        const ZydisRegisterClass cls = ZydisRegisterGetClass(op->reg.value);
        const int is_k_instr = (out->kind >= D_KMOV && out->kind <= D_KSHR);
        if (cls == ZYDIS_REGCLASS_MASK && !is_k_instr)
            continue; /* embedded vector write mask; read via avx.mask */
        if (cls != ZYDIS_REGCLASS_MASK && cls != ZYDIS_REGCLASS_XMM &&
            cls != ZYDIS_REGCLASS_YMM && cls != ZYDIS_REGCLASS_ZMM)
            continue;
        uint8_t id = (uint8_t)ZydisRegisterGetId(op->reg.value);
        if ((op->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0u)
            out->dst = id;
        else if (out->s1 == 0xFF)
            out->s1 = id;
        else
            out->s2 = id;
    }
    return 1;
}

static void ref_apply(struct ref_state* st, const struct desc* d)
{
    switch (d->kind)
    {
    case D_KMOV:
        st->k[d->dst] = st->k[d->s1];
        break;
    case D_KAND:
        st->k[d->dst] = (uint16_t)(st->k[d->s1] & st->k[d->s2]);
        break;
    case D_KOR:
        st->k[d->dst] = (uint16_t)(st->k[d->s1] | st->k[d->s2]);
        break;
    case D_KXOR:
        st->k[d->dst] = (uint16_t)(st->k[d->s1] ^ st->k[d->s2]);
        break;
    case D_KNOT:
        st->k[d->dst] = (uint16_t)(~st->k[d->s1] & 0xFFFFu);
        break;
    case D_KSHL:
        st->k[d->dst] = (uint16_t)((st->k[d->s1] << d->imm) & 0xFFFFu);
        break;
    case D_KSHR:
        st->k[d->dst] = (uint16_t)(st->k[d->s1] >> d->imm);
        break;
    case D_MOVE:
    {
        /* Snapshot first: source and destination may be the same register. */
        struct ref_tag src[REF_LANES];
        struct ref_tag old_dst[REF_LANES];
        memcpy(src, st->vec[d->s1], sizeof(src));
        memcpy(old_dst, st->vec[d->dst], sizeof(old_dst));
        for (uint8_t l = 0; l < d->lanes; ++l)
        {
            if (d->has_mask && !((st->k[d->km] >> l) & 1u))
                st->vec[d->dst][l] = d->zeroing ? ref_zero_tag() : old_dst[l];
            else
                st->vec[d->dst][l] = src[l];
        }
        for (uint8_t l = d->lanes; l < REF_LANES; ++l)
            st->vec[d->dst][l] = ref_zero_tag();
        break;
    }
    case D_COMPRESS:
    {
        const uint16_t m = d->has_mask ? st->k[d->km] : 0xFFFFu;
        struct ref_tag src[REF_LANES], old_dst[REF_LANES];
        memcpy(src, st->vec[d->s1], sizeof(src));
        if (!d->is_mem)
            memcpy(old_dst, st->vec[d->dst], sizeof(old_dst));
        if (d->is_mem)
        {
            for (uint8_t p = 0; p < d->lanes; ++p)
            {
                int found = 0;
                for (uint8_t i = 0; i < d->lanes; ++i)
                {
                    if (((m >> i) & 1u) &&
                        ref_popcount((uint16_t)(m & ((1u << i) - 1u))) == p)
                    {
                        ref_mem_set(st, d->addr + 4u * p, src[i]);
                        found = 1;
                        break;
                    }
                }
                (void)found; /* uncovered cells keep their previous value */
            }
        }
        else
        {
            for (uint8_t p = 0; p < d->lanes; ++p)
            {
                struct ref_tag picked = ref_zero_tag();
                int found = 0;
                for (uint8_t i = 0; i < d->lanes; ++i)
                {
                    if (((m >> i) & 1u) &&
                        ref_popcount((uint16_t)(m & ((1u << i) - 1u))) == p)
                    {
                        picked = src[i];
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    picked = d->zeroing ? ref_zero_tag() : old_dst[p];
                st->vec[d->dst][p] = picked;
            }
            for (uint8_t l = d->lanes; l < REF_LANES; ++l)
                st->vec[d->dst][l] = ref_zero_tag();
        }
        break;
    }
    case D_EXPAND:
    {
        const uint16_t m = d->has_mask ? st->k[d->km] : 0xFFFFu;
        struct ref_tag src[REF_LANES], old_dst[REF_LANES];
        if (!d->is_mem)
        {
            memcpy(src, st->vec[d->s1], sizeof(src));
            memcpy(old_dst, st->vec[d->dst], sizeof(old_dst));
        }
        for (uint8_t p = 0; p < d->lanes; ++p)
        {
            if (d->has_mask && !((m >> p) & 1u))
            {
                if (!d->is_mem)
                    st->vec[d->dst][p] =
                        d->zeroing ? ref_zero_tag() : old_dst[p];
                continue;
            }
            const unsigned idx = ref_popcount((uint16_t)(m & ((1u << p) - 1u)));
            if (d->is_mem)
                st->vec[d->dst][p] = ref_mem_get(st, d->addr + 4u * idx);
            else
                st->vec[d->dst][p] = src[idx];
        }
        if (!d->is_mem)
            for (uint8_t l = d->lanes; l < REF_LANES; ++l)
                st->vec[d->dst][l] = ref_zero_tag();
        break;
    }
    case D_OTHER:
        break;
    }
}

/* ============================================================================================== */
/* Comparison helpers                                                                             */
/* ============================================================================================== */

static int tag_equals(const struct ref_tag* t, const ZydisCompactValue* v)
{
    if (t->kind == REF_ZERO)
        return v->kind == ZYDIS_COMPACT_VALUE_ZERO;
    if (t->kind == REF_LANE)
        return v->kind == ZYDIS_COMPACT_VALUE_ENTRY_LANE &&
               v->zmm == t->zmm && v->lane == t->lane;
    return v->kind == ZYDIS_COMPACT_VALUE_ENTRY_MEM && v->address == t->addr;
}

/* Concrete entry-mask assignments used by every case (small, deliberately fixed sets). */
static const uint16_t mask_sets[][8] = {
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF },
    { 0x7777, 0xAAAA, 0x5555, 0x0F0F, 0xF0F0, 0x00FF, 0xFF00, 0x1248 },
    { 0x018B, 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x8000 },
    { 0xB7E3, 0x3C5A, 0x96CF, 0x07E0, 0xF81F, 0x6DB6, 0xDB6D, 0x1357 },
    { 0x000F, 0x00FF, 0x0F0F, 0x5555, 0xAAAA, 0x3333, 0xCCCC, 0x0001 }
};
#define MASK_SET_COUNT (sizeof(mask_sets) / sizeof(mask_sets[0]))

static int eval_lane(ZydisCompactDag* dag, uint32_t version, uint8_t zmm, uint8_t lane,
    const uint16_t masks[8], ZydisCompactValue* out)
{
    ZyanU32 node;
    if (!ZYAN_SUCCESS(ZydisCompactDagGetLane(dag, version, zmm, lane, &node)))
        return 0;
    return ZYAN_SUCCESS(ZydisCompactDagEvaluate(dag, node, masks, out));
}

static int eval_mem(ZydisCompactDag* dag, uint32_t version, uint64_t addr,
    const uint16_t masks[8], ZydisCompactValue* out)
{
    ZyanU32 node;
    if (!ZYAN_SUCCESS(ZydisCompactDagGetMemory(dag, version, addr, &node)))
        return 0;
    return ZYAN_SUCCESS(ZydisCompactDagEvaluate(dag, node, masks, out));
}

static int compare_vector(ZydisCompactDag* dag, uint32_t version,
    const struct ref_state* ref, const uint16_t masks[8], uint8_t zmm, uint8_t lanes)
{
    for (uint8_t l = 0; l < lanes; ++l)
    {
        ZydisCompactValue v;
        if (!eval_lane(dag, version, zmm, l, masks, &v))
            return 0;
        if (!tag_equals(&ref->vec[zmm][l], &v))
            return 0;
    }
    return 1;
}

/* ============================================================================================== */
/* Group MaskDag                                                                                  */
/* ============================================================================================== */

static int mask_dag_logic(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));

    /* kmovw k1,k2; kandw k3,k1,k2; knotw k4,k3; kshiftlw k5,k4,3; kshiftrw k6,k5,2 */
    const unsigned spans[][2] = { {0,4}, {4,4}, {8,4}, {12,6} };
    /* Reuse the fixture's first four instructions (offsets match compact_spans). */
    uint32_t version = 0;
    for (unsigned i = 0; i < 4u; ++i)
    {
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag,
            compact_bytes + spans[i][0], spans[i][1], 0x1000 + spans[i][0])));
        ++version;
    }
    TASK_CHECK(ZydisCompactDagInstructionCount(dag) == 4u);

    /* The graph must contain the symbolic mask node types. */
    int saw[14] = { 0 };
    for (ZyanU32 n = 0; n < ZydisCompactDagNodeCount(dag); ++n)
    {
        ZydisCompactNodeInfo info;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetNode(dag, n, &info)));
        TASK_CHECK(info.type <= ZYDIS_COMPACT_NODE_MAX_VALUE);
        saw[info.type] = 1;
    }
    TASK_CHECK(saw[ZYDIS_COMPACT_NODE_MASK_AND]);
    TASK_CHECK(saw[ZYDIS_COMPACT_NODE_MASK_NOT]);
    TASK_CHECK(saw[ZYDIS_COMPACT_NODE_MASK_SHLL]);

    struct desc descs[4];
    for (unsigned i = 0; i < 4u; ++i)
        TASK_CHECK(desc_decode(compact_bytes + spans[i][0], spans[i][1],
            0x1000 + spans[i][0], &descs[i]));

    const ZyanU32 nodes_before = ZydisCompactDagNodeCount(dag);
    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned i = 0; i < 4u; ++i)
            ref_apply(&ref, &descs[i]);

        for (uint8_t k = 1; k <= 6; ++k)
        {
            for (uint8_t bit = 0; bit < 16; ++bit)
            {
                ZyanU32 node;
                TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetMaskBit(dag, version, k, bit,
                    &node)));
                ZyanU8 got;
                TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagEvaluateMask(dag, node,
                    mask_sets[m], &got)));
                TASK_CHECK(got == (unsigned)((ref.k[k] >> bit) & 1u));
            }
        }
    }
    /* Evaluation never mutates or grows the graph. */
    TASK_CHECK(ZydisCompactDagNodeCount(dag) == nodes_before);

    ZydisCompactDagDestroy(dag);
    return 0;
}

static int mask_dag_k0_ordinary(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));

    /* kmovw k0,k1; kandw k0,k2,k3; korw k0,k4,k5; knotw k0,k6 */
    const unsigned spans[][2] = { {0,4}, {4,4}, {8,4}, {12,4} };
    struct desc descs[4];
    for (unsigned i = 0; i < 4u; ++i)
    {
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t3 + spans[i][0],
            spans[i][1], 0x3000 + spans[i][0])));
        TASK_CHECK(desc_decode(bytes_t3 + spans[i][0], spans[i][1],
            0x3000 + spans[i][0], &descs[i]));
    }

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned i = 0; i < 4u; ++i)
            ref_apply(&ref, &descs[i]);
        for (uint8_t bit = 0; bit < 16; ++bit)
        {
            ZyanU32 node;
            TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetMaskBit(dag, 4u, 0, bit, &node)));
            ZyanU8 got;
            TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagEvaluateMask(dag, node,
                mask_sets[m], &got)));
            TASK_CHECK(got == (unsigned)((ref.k[0] >> bit) & 1u));
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int mask_dag_shift_zeroes(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    /* kshiftlw k5,k4,3 then kshiftrw k7,k1,5 (fixture only has the left shift; build the
       right one from t2-style bytes is unavailable, so exercise left shift thoroughly). */
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, compact_bytes + 12, 6, 0x100C)));

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        const uint16_t expect = (uint16_t)((mask_sets[m][4] << 3) & 0xFFFFu);
        for (uint8_t bit = 0; bit < 16; ++bit)
        {
            ZyanU32 node;
            TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetMaskBit(dag, 1u, 5, bit, &node)));
            ZyanU8 got;
            TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagEvaluateMask(dag, node,
                mask_sets[m], &got)));
            TASK_CHECK(got == (unsigned)((expect >> bit) & 1u));
        }
        /* The low three bits are vacated and deterministically zero. */
        for (uint8_t bit = 0; bit < 3; ++bit)
        {
            ZyanU32 node;
            TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetMaskBit(dag, 1u, 5, bit, &node)));
            ZydisCompactNodeInfo info;
            TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetNode(dag, node, &info)));
            TASK_CHECK(info.type == ZYDIS_COMPACT_NODE_ZERO);
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int mask_dag_or_xor_shr(void)
{
    static const struct { const unsigned char* bytes; unsigned len; } seq[] = {
        { bytes_kor,  sizeof(bytes_kor)  },
        { bytes_kxor, sizeof(bytes_kxor) },
        { bytes_kshr, sizeof(bytes_kshr) }
    };
    struct desc descs[3];
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    uint64_t runtime = 0x4000;
    for (unsigned n = 0; n < 3u; ++n)
    {
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, seq[n].bytes, seq[n].len,
            runtime)));
        TASK_CHECK(desc_decode(seq[n].bytes, seq[n].len, runtime, &descs[n]));
        runtime += seq[n].len;
    }

    int saw_or = 0, saw_xor = 0, saw_shr = 0;
    for (ZyanU32 n = 0; n < ZydisCompactDagNodeCount(dag); ++n)
    {
        ZydisCompactNodeInfo info;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetNode(dag, n, &info)));
        saw_or |= (info.type == ZYDIS_COMPACT_NODE_MASK_OR);
        saw_xor |= (info.type == ZYDIS_COMPACT_NODE_MASK_XOR);
        saw_shr |= (info.type == ZYDIS_COMPACT_NODE_MASK_SHRL);
    }
    TASK_CHECK(saw_or && saw_xor && saw_shr);

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned n = 0; n < 3u; ++n)
            ref_apply(&ref, &descs[n]);

        static const uint8_t watched[] = { 2, 3, 7 };
        for (unsigned w = 0; w < 3u; ++w)
        {
            const uint8_t k = watched[w];
            for (uint8_t bit = 0; bit < 16; ++bit)
            {
                ZyanU32 node;
                TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetMaskBit(dag, 3u, k, bit,
                    &node)));
                ZyanU8 got;
                TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagEvaluateMask(dag, node,
                    mask_sets[m], &got)));
                TASK_CHECK(got == (unsigned)((ref.k[k] >> bit) & 1u));
            }
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

/* ============================================================================================== */
/* Group LaneRanks                                                                                */
/* ============================================================================================== */

static int lane_ranks_widths(void)
{
    /* Spans 2..11 of t2: compress/expand register forms across 512/256/128. */
    static const unsigned idx[] = { 2,3,4,5,6,7,8,9,10,11 };
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));

    struct desc descs[10];
    uint64_t runtime = 0x5000;
    for (unsigned n = 0; n < 10u; ++n)
    {
        const struct t2_span* sp = &t2_spans[idx[n]];
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + sp->off, sp->len,
            runtime)));
        TASK_CHECK(desc_decode(bytes_t2 + sp->off, sp->len, runtime, &descs[n]));
        runtime += sp->len;
    }

    int saw_rank = 0, saw_eq = 0, saw_select = 0;
    for (ZyanU32 n = 0; n < ZydisCompactDagNodeCount(dag); ++n)
    {
        ZydisCompactNodeInfo info;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetNode(dag, n, &info)));
        saw_rank |= (info.type == ZYDIS_COMPACT_NODE_RANK);
        saw_eq |= (info.type == ZYDIS_COMPACT_NODE_EQ);
        saw_select |= (info.type == ZYDIS_COMPACT_NODE_SELECT);
    }
    TASK_CHECK(saw_rank && saw_eq && saw_select);

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned n = 0; n < 10u; ++n)
        {
            ref_apply(&ref, &descs[n]);
            const uint8_t lanes = descs[n].lanes;
            TASK_CHECK(compare_vector(dag, n + 1u, &ref, mask_sets[m], descs[n].dst,
                lanes));
            /* A narrow (128/256) write zeroes the upper lanes of the enclosing ZMM. */
            for (uint8_t l = lanes; l < REF_LANES; ++l)
            {
                ZydisCompactValue v;
                TASK_CHECK(eval_lane(dag, n + 1u, descs[n].dst, l, mask_sets[m], &v));
                TASK_CHECK(v.kind == ZYDIS_COMPACT_VALUE_ZERO);
            }
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int lane_ranks_narrow_high_zero(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    /* vmovdqu32 ymm1, ymm2 then vmovdqu32 xmm1, xmm2 (t2 spans 0 and 1). */
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2, 6, 0x6000)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + 6, 6, 0x6006)));

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        for (uint8_t l = 4; l < 16; ++l)
        {
            ZydisCompactValue v;
            TASK_CHECK(eval_lane(dag, 2u, 1, l, mask_sets[m], &v));
            TASK_CHECK(v.kind == ZYDIS_COMPACT_VALUE_ZERO);
        }
        /* Low lanes are an unmasked in-order copy of xmm2. */
        for (uint8_t l = 0; l < 4; ++l)
        {
            ZydisCompactValue v;
            TASK_CHECK(eval_lane(dag, 2u, 1, l, mask_sets[m], &v));
            TASK_CHECK(v.kind == ZYDIS_COMPACT_VALUE_ENTRY_LANE && v.zmm == 2 &&
                       v.lane == l);
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int lane_ranks_same_register(void)
{
    /* t2 spans 12..14: compress zmm5{k1},zmm5; expand zmm6{k2}{z},zmm6; mov zmm7{k3},zmm7 */
    static const unsigned idx[] = { 12, 13, 14 };
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    struct desc descs[3];
    uint64_t runtime = 0x7000;
    for (unsigned n = 0; n < 3u; ++n)
    {
        const struct t2_span* sp = &t2_spans[idx[n]];
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + sp->off, sp->len,
            runtime)));
        TASK_CHECK(desc_decode(bytes_t2 + sp->off, sp->len, runtime, &descs[n]));
        runtime += sp->len;
    }
    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned n = 0; n < 3u; ++n)
        {
            ref_apply(&ref, &descs[n]);
            TASK_CHECK(compare_vector(dag, n + 1u, &ref, mask_sets[m], descs[n].dst,
                descs[n].lanes));
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int lane_ranks_unmasked_forms(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, 0x401000u, 32)));

    struct desc descs[6];
    uint64_t runtime = 0xE000;
    for (unsigned n = 0; n < 6u; ++n)
    {
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag,
            bytes_unmasked + um_spans[n].off, um_spans[n].len, runtime)));
        TASK_CHECK(desc_decode(bytes_unmasked + um_spans[n].off, um_spans[n].len,
            runtime, &descs[n]));
        TASK_CHECK(!descs[n].has_mask);
        runtime += um_spans[n].len;
    }

    /* Results must be identical for every entry-mask assignment. */
    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned n = 0; n < 6u; ++n)
        {
            ref_apply(&ref, &descs[n]);
            const uint32_t ver = n + 1u;
            if (descs[n].kind == D_COMPRESS && descs[n].is_mem)
            {
                for (unsigned c = 0; c < 16; ++c)
                {
                    ZydisCompactValue v;
                    TASK_CHECK(eval_mem(dag, ver, 0x401000u + 4u * c, mask_sets[m], &v));
                    const struct ref_tag expect = ref_mem_get(&ref, 0x401000u + 4u * c);
                    TASK_CHECK(tag_equals(&expect, &v));
                }
            }
            else if (descs[n].kind == D_EXPAND && descs[n].is_mem)
            {
                TASK_CHECK(compare_vector(dag, ver, &ref, mask_sets[m], descs[n].dst,
                    descs[n].lanes));
            }
            else
            {
                TASK_CHECK(compare_vector(dag, ver, &ref, mask_sets[m], descs[n].dst,
                    descs[n].lanes));
            }
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

/* ============================================================================================== */
/* Group MemoryVersions                                                                           */
/* ============================================================================================== */

#define REGION_A 0x401000u
#define REGION_B 0x402000u
#define REGION_C 0x403000u

static int memory_versions_partial(void)
{
    /* t2 spans 15..22: compress/expand memory forms over disp32 regions. */
    static const unsigned idx[] = { 15,16,17,18,19,20,21,22 };
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, REGION_A, 64)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, REGION_B, 32)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, REGION_C, 16)));

    struct desc descs[8];
    uint64_t runtime = 0x8000;
    for (unsigned n = 0; n < 8u; ++n)
    {
        const struct t2_span* sp = &t2_spans[idx[n]];
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + sp->off, sp->len,
            runtime)));
        TASK_CHECK(desc_decode(bytes_t2 + sp->off, sp->len, runtime, &descs[n]));
        runtime += sp->len;
    }

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned n = 0; n < 8u; ++n)
        {
            ref_apply(&ref, &descs[n]);
            const uint32_t ver = n + 1u;
            if (descs[n].kind == D_EXPAND)
            {
                TASK_CHECK(compare_vector(dag, ver, &ref, mask_sets[m], descs[n].dst,
                    descs[n].lanes));
            }
            /* Check every cell of every region against the reference at this version. */
            const uint64_t bases[] = { REGION_A, REGION_B, REGION_C };
            const unsigned sizes[] = { 64, 32, 16 };
            for (unsigned r = 0; r < 3u; ++r)
            {
                for (unsigned c = 0; c < sizes[r]; ++c)
                {
                    const uint64_t addr = bases[r] + 4u * c;
                    ZydisCompactValue v;
                    TASK_CHECK(eval_mem(dag, ver, addr, mask_sets[m], &v));
                    const struct ref_tag expect = ref_mem_get(&ref, addr);
                    TASK_CHECK(tag_equals(&expect, &v));
                }
            }
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int memory_versions_overlap_and_history(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, REGION_A, 64)));

    /* Two overlapping compress stores at +0 and +4, then a load from the +4 prefix. */
    const struct t2_span* store0 = &t2_spans[15]; /* [0x401000]{k1}, zmm2 */
    const struct t2_span* store4 = &t2_spans[16]; /* [0x401004]{k2}, zmm3 */
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + store0->off,
        store0->len, 0x9000)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + store4->off,
        store4->len, 0x9100)));

    struct desc d0, d1;
    TASK_CHECK(desc_decode(bytes_t2 + store0->off, store0->len, 0x9000, &d0));
    TASK_CHECK(desc_decode(bytes_t2 + store4->off, store4->len, 0x9100, &d1));

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        ref_apply(&ref, &d0);

        /* Version 1: only the first store is visible. */
        for (unsigned c = 0; c < 16; ++c)
        {
            ZydisCompactValue v;
            TASK_CHECK(eval_mem(dag, 1u, REGION_A + 4u * c, mask_sets[m], &v));
            const struct ref_tag expect = ref_mem_get(&ref, REGION_A + 4u * c);
            TASK_CHECK(tag_equals(&expect, &v));
        }
        ref_apply(&ref, &d1);
        /* Version 2: the overlapping +4 store forwards the newest conditional version. */
        for (unsigned c = 0; c < 17; ++c)
        {
            ZydisCompactValue v;
            TASK_CHECK(eval_mem(dag, 2u, REGION_A + 4u * c, mask_sets[m], &v));
            const struct ref_tag expect = ref_mem_get(&ref, REGION_A + 4u * c);
            TASK_CHECK(tag_equals(&expect, &v));
        }
    }

    /* An untouched cell still reports its entry memory leaf at every version. */
    for (uint32_t ver = 0; ver <= 2u; ++ver)
    {
        ZyanU32 node;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetMemory(dag, ver, REGION_A + 4u * 40,
            &node)));
        ZydisCompactNodeInfo info;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetNode(dag, node, &info)));
        TASK_CHECK(info.type == ZYDIS_COMPACT_NODE_ENTRY_MEM);
        TASK_CHECK(info.address == REGION_A + 4u * 40);
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int memory_versions_errors(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, REGION_A, 64)));

    /* Region validation. */
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, REGION_A + 8, 8))); /* overlap */
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, 0x405001, 8)));    /* align   */
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, 0x405000, 0)));   /* empty   */

    /* Unsupported base-register addressing (t2 spans 23..25). */
    static const unsigned bad_idx[] = { 23, 24, 25 };
    for (unsigned n = 0; n < 3u; ++n)
    {
        ZydisCompactDag* fresh = ZYAN_NULL;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&fresh)));
        const struct t2_span* sp = &t2_spans[bad_idx[n]];
        const ZyanStatus st = ZydisCompactDagDecode(fresh, bytes_t2 + sp->off, sp->len,
            0xA000);
        TASK_CHECK(st == ZYDIS_STATUS_COMPACT_UNSUPPORTED);
        ZyanU32 idx_out;
        ZyanU64 addr_out;
        ZyanUSize len_out;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagLastUnsupported(fresh, &idx_out, &addr_out,
            &len_out)));
        TASK_CHECK(idx_out == 0u && addr_out == 0xA000 && len_out == sp->len);
        /* A poisoned graph rejects further decodes. */
        TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagDecode(fresh, bytes_t2, 6, 0xA100)));
        ZydisCompactDagDestroy(fresh);
    }

    /* Unsupported mnemonics: nop, vpxord, kmovb (t2 spans 26..28). */
    static const unsigned other_idx[] = { 26, 27, 28 };
    for (unsigned n = 0; n < 3u; ++n)
    {
        ZydisCompactDag* fresh = ZYAN_NULL;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&fresh)));
        const struct t2_span* sp = &t2_spans[other_idx[n]];
        TASK_CHECK(ZydisCompactDagDecode(fresh, bytes_t2 + sp->off, sp->len,
            0xB000) == ZYDIS_STATUS_COMPACT_UNSUPPORTED);
        ZydisCompactDagDestroy(fresh);
    }

    /* Misaligned disp32 (t3 spans 4 and 5). */
    {
        ZydisCompactDag* fresh = ZYAN_NULL;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&fresh)));
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(fresh, REGION_A, 64)));
        TASK_CHECK(ZydisCompactDagDecode(fresh, bytes_t3 + 16, 11, 0xC000) ==
            ZYDIS_STATUS_COMPACT_MISALIGNED);
        TASK_CHECK(ZydisCompactDagDecode(fresh, bytes_t3 + 27, 11, 0xC100) ==
            ZYDIS_STATUS_COMPACT_MISALIGNED);
        ZydisCompactDagDestroy(fresh);
    }

    /* Access outside any registered region. */
    {
        ZydisCompactDag* fresh = ZYAN_NULL;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&fresh)));
        const struct t2_span* sp = &t2_spans[15];
        TASK_CHECK(ZydisCompactDagDecode(fresh, bytes_t2 + sp->off, sp->len,
            0xD000) == ZYDIS_STATUS_COMPACT_NO_REGION);
        ZydisCompactDagDestroy(fresh);
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

/* ============================================================================================== */
/* Group Composition                                                                              */
/* ============================================================================================== */

static int composition_roundtrip_fixture(void)
{
    /* The shipped fixture: full 10-instruction program including RIP-relative memory. */
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, 0x2000, 32)));

    struct desc descs[10];
    for (unsigned n = 0; n < 10u; ++n)
    {
        const struct fixture_span* sp = &compact_spans[n];
        const uint64_t runtime = 0x1000 + sp->offset;
        TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, compact_bytes + sp->offset,
            sp->length, runtime)));
        TASK_CHECK(desc_decode(compact_bytes + sp->offset, sp->length, runtime,
            &descs[n]));
    }
    TASK_CHECK(ZydisCompactDagInstructionCount(dag) == 10u);

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned n = 0; n < 10u; ++n)
            ref_apply(&ref, &descs[n]);

        /* Compression followed by expansion: every destination lane and the touched
           memory region agree with the independent reference on every mask assignment. */
        static const uint8_t watched[] = { 3, 4, 5, 6, 31 };
        for (unsigned w = 0; w < sizeof(watched); ++w)
            TASK_CHECK(compare_vector(dag, 10u, &ref, mask_sets[m], watched[w], 16));
        for (unsigned c = 0; c < 16; ++c)
        {
            ZydisCompactValue v;
            TASK_CHECK(eval_mem(dag, 10u, 0x2000 + 4u * c, mask_sets[m], &v));
            const struct ref_tag expect = ref_mem_get(&ref, 0x2000 + 4u * c);
            TASK_CHECK(tag_equals(&expect, &v));
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int composition_multi_mask_chains(void)
{
    /* Build: K logic producing k3, then compress with k1, expand with k3, move with k2. */
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));

    /* mask_move, mask_and, mask_not from the fixture then t2 compress/expand/move. */
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, compact_bytes + 0, 4, 0x10000)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, compact_bytes + 4, 4, 0x10004)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, compact_bytes + 8, 4, 0x10008)));
    const struct t2_span* cmp = &t2_spans[2];   /* zmm7{k1} <- zmm8 */
    const struct t2_span* exp = &t2_spans[8];   /* zmm10{k4} <- zmm11 */
    const struct t2_span* mov = &t2_spans[14];  /* zmm7{k3} <- zmm7 */
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + cmp->off, cmp->len,
        0x11000)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + exp->off, exp->len,
        0x11100)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + mov->off, mov->len,
        0x11200)));

    struct desc descs[6];
    TASK_CHECK(desc_decode(compact_bytes + 0, 4, 0x10000, &descs[0]));
    TASK_CHECK(desc_decode(compact_bytes + 4, 4, 0x10004, &descs[1]));
    TASK_CHECK(desc_decode(compact_bytes + 8, 4, 0x10008, &descs[2]));
    TASK_CHECK(desc_decode(bytes_t2 + cmp->off, cmp->len, 0x11000, &descs[3]));
    TASK_CHECK(desc_decode(bytes_t2 + exp->off, exp->len, 0x11100, &descs[4]));
    TASK_CHECK(desc_decode(bytes_t2 + mov->off, mov->len, 0x11200, &descs[5]));

    for (unsigned m = 0; m < MASK_SET_COUNT; ++m)
    {
        struct ref_state ref;
        ref_init(&ref);
        memcpy(ref.k, mask_sets[m], sizeof(ref.k));
        for (unsigned n = 0; n < 6u; ++n)
        {
            ref_apply(&ref, &descs[n]);
            if (descs[n].kind == D_MOVE || descs[n].kind == D_COMPRESS ||
                descs[n].kind == D_EXPAND)
            {
                TASK_CHECK(compare_vector(dag, n + 1u, &ref, mask_sets[m],
                    descs[n].dst, descs[n].lanes));
            }
        }
    }
    ZydisCompactDagDestroy(dag);
    return 0;
}

static int composition_versions_and_limits(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));

    /* Entry state queries resolve to identity leaves before any instruction runs. */
    ZyanU32 node;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetLane(dag, 0, 7, 9, &node)));
    ZydisCompactNodeInfo info;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetNode(dag, node, &info)));
    TASK_CHECK(info.type == ZYDIS_COMPACT_NODE_ENTRY_LANE && info.zmm == 7 &&
               info.lane == 9);

    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagAddRegion(dag, 0x7000, 8)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetMemory(dag, 0, 0x7000, &node)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagGetNode(dag, node, &info)));
    TASK_CHECK(info.type == ZYDIS_COMPACT_NODE_ENTRY_MEM && info.address == 0x7000);

    /* Out-of-range and bad-argument queries fail cleanly. */
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagGetLane(dag, 0, 32, 0, &node)));
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagGetLane(dag, 0, 0, 16, &node)));
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagGetMaskBit(dag, 0, 8, 0, &node)));
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagGetMemory(dag, 0, 0x7004 + 0x100, &node)));
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagGetMemory(dag, 0, 0x7001, &node)));
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagEvaluate(dag, ZydisCompactDagNodeCount(dag),
        mask_sets[0], &(ZydisCompactValue){ 0 })));
    TASK_CHECK(ZYAN_STATUS_NOT_FOUND == ZydisCompactDagLastUnsupported(dag, ZYAN_NULL,
        ZYAN_NULL, ZYAN_NULL));

    /* The graph owns its data: destroying it is the only cleanup required. */
    TASK_CHECK(ZydisCompactDagNodeCount(dag) > 2u);
    ZydisCompactDagDestroy(dag);
    ZydisCompactDagDestroy(ZYAN_NULL); /* must be a no-op */
    return 0;
}

static int composition_limit_failure(void)
{
    /* Repeated masked 512-bit compaction must eventually hit the node cap. The failure must
       be explicit, must not grow past the cap, and must poison further decoding. */
    ZydisCompactDag* dag = ZYAN_NULL;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)));
    const struct t2_span* sp = &t2_spans[2]; /* vpcompressd zmm7{k1}, zmm8 */
    const ZyanU32 cap = ZYDIS_COMPACT_NODE_LIMIT;
    ZyanStatus hit = ZYAN_STATUS_SUCCESS;
    ZyanU32 before = 0;
    for (unsigned n = 0; n < ZYDIS_COMPACT_INSTRUCTION_LIMIT; ++n)
    {
        const uint64_t runtime = 0x20000u + (uint64_t)n * 16u;
        const ZyanStatus st = ZydisCompactDagDecode(dag, bytes_t2 + sp->off, sp->len,
            runtime);
        if (!ZYAN_SUCCESS(st))
        {
            hit = st;
            before = n;
            break;
        }
        TASK_CHECK(ZydisCompactDagNodeCount(dag) <= cap);
    }
    TASK_CHECK(hit == ZYDIS_STATUS_COMPACT_LIMIT);
    TASK_CHECK(ZydisCompactDagInstructionCount(dag) == before);
    TASK_CHECK(ZydisCompactDagNodeCount(dag) <= cap);

    /* A truncated graph is poisoned: no further decode is accepted. */
    TASK_CHECK(!ZYAN_SUCCESS(ZydisCompactDagDecode(dag, bytes_t2 + sp->off, sp->len,
        0x30000u)));
    TASK_CHECK(ZydisCompactDagInstructionCount(dag) == before);
    ZydisCompactDagDestroy(dag);
    return 0;
}

/* ============================================================================================== */
/* Registration                                                                                   */
/* ============================================================================================== */

const struct task_case task_cases[] = {
    { "MaskDag", "logic_and_not_shift", mask_dag_logic },
    { "MaskDag", "k0_ordinary_register", mask_dag_k0_ordinary },
    { "MaskDag", "shift_vacated_zeroes", mask_dag_shift_zeroes },
    { "MaskDag", "or_xor_right_shift", mask_dag_or_xor_shr },
    { "LaneRanks", "widths_merge_zeroing", lane_ranks_widths },
    { "LaneRanks", "narrow_write_high_zero", lane_ranks_narrow_high_zero },
    { "LaneRanks", "same_register_read_write", lane_ranks_same_register },
    { "LaneRanks", "unmasked_compaction_forms", lane_ranks_unmasked_forms },
    { "MemoryVersions", "partial_store_forwarding", memory_versions_partial },
    { "MemoryVersions", "overlap_and_version_history", memory_versions_overlap_and_history },
    { "MemoryVersions", "addressing_and_region_errors", memory_versions_errors },
    { "Composition", "fixture_compress_expand_roundtrip", composition_roundtrip_fixture },
    { "Composition", "different_masks_combined", composition_multi_mask_chains },
    { "Composition", "entry_state_queries_and_limits", composition_versions_and_limits },
    { "Composition", "resource_limit_fails_explicitly", composition_limit_failure }
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
