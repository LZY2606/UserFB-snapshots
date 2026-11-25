/*
 * gather_contract.c
 *
 * Contract regression for AVX2 VSIB gather instructions:
 *   VGATHERDPS / VGATHERDPD / VGATHERQPS / VGATHERQPD
 *   VPGATHERDD / VPGATHERDQ / VPGATHERQD / VPGATHERQQ
 * in their VEX.128 / VEX.256 memory forms, in 32-bit and 64-bit modes.
 *
 * The cases are split into the three task_support groups:
 *   width_map - mnemonic/data/index/destination width matrix
 *   vsib      - ModRM/SIB (VSIB) addressing, conflicts and truncation
 *   compat    - pre-existing MOV/SIB/VEX/x87 decoding and enum stability
 *
 * Only bytes are decoded; nothing is executed and no lane addresses are
 * computed.  Registration goes through task_support/check.h.
 */

#include "../task_support/check.h"

#include "udis86.h"

#include <stdint.h>
#include <string.h>

#include "../task_support/gather-inputs.h"

/* ------------------------------------------------------------------ */
/* Expected decode for every instruction in task_support/gather-inputs */
/* ------------------------------------------------------------------ */

struct mat_exp {
    int mnem;
    unsigned char len;
    int dst;  unsigned short dsz;
    int msk;  unsigned short msz;
    unsigned short memsz;
    int base;
    int idx;  unsigned short isz;
    unsigned char scale;
    unsigned char offw;
    int32_t disp;
    const char *intel;
    const char *att;
};

#define X1 UD_R_XMM1
#define X2 UD_R_XMM2
#define X4 UD_R_XMM4
#define X0 UD_R_XMM0
#define X6 UD_R_XMM6
#define X8 UD_R_XMM8
#define X12 UD_R_XMM12
#define Y1 UD_R_YMM1
#define Y2 UD_R_YMM2
#define Y3 UD_R_YMM3
#define Y4 UD_R_YMM4
#define Y5 UD_R_YMM5
#define Y7 UD_R_YMM7
#define Y9 UD_R_YMM9
#define Y10 UD_R_YMM10
#define Y11 UD_R_YMM11

static const struct mat_exp mat[] = {
 /* 1 */ { UD_Ivgatherdps, 7, X1,128, X2,128, 32, UD_R_RAX, X4,128, 4,8, 16,
    "vgatherdps xmm1, dword [rax+xmm4*4+0x10], xmm2",
    "vgatherdps %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
 /* 2 */ { UD_Ivgatherdps, 7, Y1,256, Y2,256, 32, UD_R_RAX, Y4,256, 4,8, 16,
    "vgatherdps ymm1, dword [rax+ymm4*4+0x10], ymm2",
    "vgatherdps %ymm2, 0x10(%rax,%ymm4,4), %ymm1" },
 /* 3 */ { UD_Ivgatherdpd, 7, X1,128, X2,128, 64, UD_R_RAX, X4,128, 4,8, 16,
    "vgatherdpd xmm1, qword [rax+xmm4*4+0x10], xmm2",
    "vgatherdpd %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
 /* 4: 256-bit DPD keeps an XMM index */
 { UD_Ivgatherdpd, 7, Y1,256, Y2,256, 64, UD_R_RAX, X4,128, 4,8, 16,
    "vgatherdpd ymm1, qword [rax+xmm4*4+0x10], ymm2",
    "vgatherdpd %ymm2, 0x10(%rax,%xmm4,4), %ymm1" },
 /* 5 */ { UD_Ivgatherqps, 7, X1,128, X2,128, 32, UD_R_RAX, X4,128, 4,8, 16,
    "vgatherqps xmm1, dword [rax+xmm4*4+0x10], xmm2",
    "vgatherqps %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
 /* 6: 256-bit QPS has XMM dest/mask but YMM index */
 { UD_Ivgatherqps, 7, X1,128, X2,128, 32, UD_R_RAX, Y4,256, 4,8, 16,
    "vgatherqps xmm1, dword [rax+ymm4*4+0x10], xmm2",
    "vgatherqps %xmm2, 0x10(%rax,%ymm4,4), %xmm1" },
 /* 7 */ { UD_Ivgatherqpd, 7, X1,128, X2,128, 64, UD_R_RAX, X4,128, 4,8, 16,
    "vgatherqpd xmm1, qword [rax+xmm4*4+0x10], xmm2",
    "vgatherqpd %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
 /* 8 */ { UD_Ivgatherqpd, 7, Y1,256, Y2,256, 64, UD_R_RAX, Y4,256, 4,8, 16,
    "vgatherqpd ymm1, qword [rax+ymm4*4+0x10], ymm2",
    "vgatherqpd %ymm2, 0x10(%rax,%ymm4,4), %ymm1" },
 /* 9 */ { UD_Ivpgatherdd, 7, X1,128, X2,128, 32, UD_R_RAX, X4,128, 4,8, 16,
    "vpgatherdd xmm1, dword [rax+xmm4*4+0x10], xmm2",
    "vpgatherdd %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
 /* 10 */{ UD_Ivpgatherdd, 7, Y1,256, Y2,256, 32, UD_R_RAX, Y4,256, 4,8, 16,
    "vpgatherdd ymm1, dword [rax+ymm4*4+0x10], ymm2",
    "vpgatherdd %ymm2, 0x10(%rax,%ymm4,4), %ymm1" },
 /* 11 */{ UD_Ivpgatherdq, 7, X1,128, X2,128, 64, UD_R_RAX, X4,128, 4,8, 16,
    "vpgatherdq xmm1, qword [rax+xmm4*4+0x10], xmm2",
    "vpgatherdq %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
 /* 12: 256-bit DQ keeps an XMM index */
 { UD_Ivpgatherdq, 7, Y1,256, Y2,256, 64, UD_R_RAX, X4,128, 4,8, 16,
    "vpgatherdq ymm1, qword [rax+xmm4*4+0x10], ymm2",
    "vpgatherdq %ymm2, 0x10(%rax,%xmm4,4), %ymm1" },
 /* 13 */{ UD_Ivpgatherqd, 7, X1,128, X2,128, 32, UD_R_RAX, X4,128, 4,8, 16,
    "vpgatherqd xmm1, dword [rax+xmm4*4+0x10], xmm2",
    "vpgatherqd %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
 /* 14: 256-bit QD has XMM dest/mask but YMM index */
 { UD_Ivpgatherqd, 7, X1,128, X2,128, 32, UD_R_RAX, Y4,256, 4,8, 16,
    "vpgatherqd xmm1, dword [rax+ymm4*4+0x10], xmm2",
    "vpgatherqd %xmm2, 0x10(%rax,%ymm4,4), %xmm1" },
 /* 15 */{ UD_Ivpgatherqq, 7, X1,128, X2,128, 64, UD_R_RAX, X4,128, 4,8, 16,
    "vpgatherqq xmm1, qword [rax+xmm4*4+0x10], xmm2",
    "vpgatherqq %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
 /* 16 */{ UD_Ivpgatherqq, 7, Y1,256, Y2,256, 64, UD_R_RAX, Y4,256, 4,8, 16,
    "vpgatherqq ymm1, qword [rax+ymm4*4+0x10], ymm2",
    "vpgatherqq %ymm2, 0x10(%rax,%ymm4,4), %ymm1" },
 /* 17: REX extensions pick r13 and xmm12; negative disp8 */
 { UD_Ivgatherdpd, 7, Y3,256, Y5,256, 64, UD_R_R13, X12,128, 8,8, -32,
    "vgatherdpd ymm3, qword [r13+xmm12*8-0x20], ymm5",
    "vgatherdpd %ymm5, -0x20(%r13,%xmm12,8), %ymm3" },
 /* 18: no displacement, scale 2, high mask/index */
 { UD_Ivgatherqps, 6, X6,128, X8,128, 32, UD_R_RBX, Y7,256, 2,0, 0,
    "vgatherqps xmm6, dword [rbx+ymm7*2], xmm8",
    "vgatherqps %xmm8, (%rbx,%ymm7,2), %xmm6" },
 /* 19: base-less VSIB with disp32; ATT needs the empty base slot */
 { UD_Ivgatherqpd, 10, Y9,256, Y11,256, 64, UD_NONE, Y10,256, 4,32,
   (int32_t)0x11223344,
    "vgatherqpd ymm9, qword [ymm10*4+0x11223344], ymm11",
    "vgatherqpd %ymm11, 0x11223344(,%ymm10,4), %ymm9" },
 /* 20: 67h in 64-bit mode -> 32-bit address size (ecx) */
 { UD_Ivpgatherdd, 8, X0,128, X2,128, 32, UD_R_ECX, X4,128, 2,8, 16,
    "vpgatherdd xmm0, dword [ecx+xmm4*2+0x10], xmm2",
    "vpgatherdd %xmm2, 0x10(%ecx,%xmm4,2), %xmm0" },
};

/* ------------------------------------------------------------------ */
/* width_map                                                           */
/* ------------------------------------------------------------------ */

/* Every prepared instruction: content, byte consumption and text. */
static int
case_material_table(void)
{
    size_t i, off = 0;
    const size_t n = sizeof mat / sizeof mat[0];

    TASK_CHECK(n == 20);
    for (i = 0; i < n; ++i) {
        ud_t u, a;
        unsigned consumed;
        const struct ud_operand *o0, *o1, *o2;

        TASK_CHECK(off + mat[i].len <= sizeof gather_inputs);

        ud_init(&u);
        ud_set_mode(&u, 64);
        ud_set_syntax(&u, UD_SYN_INTEL);
        ud_set_input_buffer(&u, gather_inputs + off,
                            sizeof gather_inputs - off);
        consumed = ud_disassemble(&u);

        TASK_CHECK(consumed == mat[i].len);
        TASK_CHECK(ud_insn_len(&u) == mat[i].len);
        TASK_CHECK(ud_insn_mnemonic(&u) == (enum ud_mnemonic_code)mat[i].mnem);

        o0 = ud_insn_opr(&u, 0);
        o1 = ud_insn_opr(&u, 1);
        o2 = ud_insn_opr(&u, 2);
        TASK_CHECK(o0 && o1 && o2);
        TASK_CHECK(o0->type == UD_OP_REG);
        TASK_CHECK(o1->type == UD_OP_MEM);
        TASK_CHECK(o2->type == UD_OP_REG);
        TASK_CHECK(ud_insn_opr(&u, 3) == NULL);

        /* Intel order: destination, memory, vector mask */
        TASK_CHECK(o0->base == (enum ud_type)mat[i].dst);
        TASK_CHECK(o0->size == mat[i].dsz);
        TASK_CHECK(o2->base == (enum ud_type)mat[i].msk);
        TASK_CHECK(o2->size == mat[i].msz);

        /* Memory size is one gathered element, not the vector size. */
        TASK_CHECK(o1->size == mat[i].memsz);
        TASK_CHECK(o1->base == (enum ud_type)mat[i].base);
        TASK_CHECK(o1->index == (enum ud_type)mat[i].idx);
        TASK_CHECK(o1->scale == mat[i].scale);
        TASK_CHECK(o1->offset == mat[i].offw);

        if (mat[i].offw == 8) {
            TASK_CHECK(o1->lval.sbyte == (int8_t)mat[i].disp);
        } else if (mat[i].offw == 16) {
            TASK_CHECK(o1->lval.sword == (int16_t)mat[i].disp);
        } else if (mat[i].offw == 32) {
            TASK_CHECK(o1->lval.sdword == mat[i].disp);
        }

        TASK_CHECK(strcmp(ud_insn_asm(&u), mat[i].intel) == 0);

        /* AT&T prints the same operands in reversed source order. */
        ud_init(&a);
        ud_set_mode(&a, 64);
        ud_set_syntax(&a, UD_SYN_ATT);
        ud_set_input_buffer(&a, gather_inputs + off,
                            sizeof gather_inputs - off);
        TASK_CHECK(ud_disassemble(&a) == mat[i].len);
        TASK_CHECK(strcmp(ud_insn_asm(&a), mat[i].att) == 0);

        off += mat[i].len;
    }

    /* The table consumes the whole prepared blob. */
    TASK_CHECK(off == sizeof gather_inputs);
    {
        ud_t u;
        ud_init(&u);
        ud_set_mode(&u, 64);
        ud_set_input_buffer(&u, gather_inputs, sizeof gather_inputs);
        /* walk to the end and confirm EOI afterwards */
        size_t at = 0;
        while (at < sizeof gather_inputs) {
            unsigned l;
            ud_set_input_buffer(&u, gather_inputs + at,
                                 sizeof gather_inputs - at);
            l = ud_disassemble(&u);
            TASK_CHECK(l > 0);
            at += l;
        }
        ud_set_input_buffer(&u, gather_inputs + at, 0);
        TASK_CHECK(ud_disassemble(&u) == 0);
    }
    return 0;
}

/* Explicit 8 mnemonic x VEX.L matrix built from one template. */
struct wm_exp {
    int mnem;
    unsigned data;
    int dest_ymm_l1;  /* L=1 makes destination/mask YMM */
    int idx_ymm_l1;   /* L=1 makes the VSIB index YMM */
};

static int
case_explicit_matrix(void)
{
    /* indexed [opcode - 0x90][w] */
    static const struct wm_exp wm[4][2] = {
      /* 90: DD / DQ */
      { { UD_Ivpgatherdd, 32, 1, 1 }, { UD_Ivpgatherdq, 64, 1, 0 } },
      /* 91: QD / QQ */
      { { UD_Ivpgatherqd, 32, 0, 1 }, { UD_Ivpgatherqq, 64, 1, 1 } },
      /* 92: DPS / DPD */
      { { UD_Ivgatherdps,  32, 1, 1 }, { UD_Ivgatherdpd,  64, 1, 0 } },
      /* 93: QPS / QPD */
      { { UD_Ivgatherqps,  32, 0, 1 }, { UD_Ivgatherqpd,  64, 1, 1 } },
    };
    int opi, w, l;

    for (opi = 0; opi < 4; ++opi) {
        for (w = 0; w < 2; ++w) {
            for (l = 0; l < 2; ++l) {
                unsigned char b[7];
                ud_t u;
                const struct ud_operand *o0, *o1, *o2;
                const struct wm_exp *e = &wm[opi][w];
                int wide_dst = l && e->dest_ymm_l1;
                int wide_idx = l && e->idx_ymm_l1;

                b[0] = 0xc4;
                b[1] = 0xe2;
                /* vvvv = 1101 -> mask xmm2, pp = 1 (66) */
                b[2] = (unsigned char)(0x69 | (w ? 0x80 : 0)
                                             | (l ? 0x04 : 0));
                b[3] = (unsigned char)(0x90 + opi);
                b[4] = 0x4c; /* mod=01 reg=1 rm=100 (SIB) */
                b[5] = 0xa0; /* ss=10 (*4), index=4, base=0 */
                b[6] = 0x10;

                ud_init(&u);
                ud_set_mode(&u, 64);
                ud_set_input_buffer(&u, b, sizeof b);
                TASK_CHECK(ud_disassemble(&u) == sizeof b);
                TASK_CHECK(ud_insn_mnemonic(&u) ==
                           (enum ud_mnemonic_code)e->mnem);

                o0 = ud_insn_opr(&u, 0);
                o1 = ud_insn_opr(&u, 1);
                o2 = ud_insn_opr(&u, 2);
                TASK_CHECK(o0 && o1 && o2);

                TASK_CHECK(o0->base ==
                           (wide_dst ? UD_R_YMM1 : UD_R_XMM1));
                TASK_CHECK(o0->size == (wide_dst ? 256 : 128));
                TASK_CHECK(o2->base ==
                           (wide_dst ? UD_R_YMM2 : UD_R_XMM2));
                TASK_CHECK(o2->size == (wide_dst ? 256 : 128));

                TASK_CHECK(o1->type == UD_OP_MEM);
                TASK_CHECK(o1->size == e->data);
                TASK_CHECK(o1->index ==
                           (wide_idx ? UD_R_YMM4 : UD_R_XMM4));
                TASK_CHECK(o1->scale == 4);
            }
        }
    }
    return 0;
}

/* 32-bit disassembly mode: plain GPR base uses E-registers. */
static int
case_mode32(void)
{
    const unsigned char b[] = { 0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10 };
    ud_t u;
    const struct ud_operand *o1;

    ud_init(&u);
    ud_set_mode(&u, 32);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, b, sizeof b);
    TASK_CHECK(ud_disassemble(&u) == sizeof b);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivgatherdps);
    o1 = ud_insn_opr(&u, 1);
    TASK_CHECK(o1 && o1->type == UD_OP_MEM);
    TASK_CHECK(o1->base == UD_R_EAX);
    TASK_CHECK(o1->index == UD_R_XMM4);
    TASK_CHECK(o1->size == 32);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
              "vgatherdps xmm1, dword [eax+xmm4*4+0x10], xmm2") == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* vsib                                                                */
/* ------------------------------------------------------------------ */

/* Explicit base-less disp32 in 32-bit address mode and FS prefix. */
static int
case_vsib_extras(void)
{
    /* 32-bit mode, mod=00 reg=1 rm=100, SIB *4 index=4 base=5 (none) */
    const unsigned char nb[] =
        { 0xc4,0xe2,0x69,0x92,0x0c,0xa5,0x78,0x56,0x34,0x12 };
    ud_t u;
    const struct ud_operand *o1;

    ud_init(&u);
    ud_set_mode(&u, 32);
    ud_set_syntax(&u, UD_SYN_ATT);
    ud_set_input_buffer(&u, nb, sizeof nb);
    TASK_CHECK(ud_disassemble(&u) == sizeof nb);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivgatherdps);
    o1 = ud_insn_opr(&u, 1);
    TASK_CHECK(o1 && o1->base == UD_NONE);
    TASK_CHECK(o1->index == UD_R_XMM4);
    TASK_CHECK(o1->offset == 32);
    TASK_CHECK(o1->lval.udword == 0x12345678u);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
              "vgatherdps %xmm2, 0x12345678(,%xmm4,4), %xmm1") == 0);

    /* FS segment prefix is preserved and printed through old interface. */
    {
        const unsigned char fs[] =
            { 0x64,0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10 };
        ud_t f;
        const struct ud_operand *m;
        ud_init(&f);
        ud_set_mode(&f, 64);
        ud_set_syntax(&f, UD_SYN_INTEL);
        ud_set_input_buffer(&f, fs, sizeof fs);
        TASK_CHECK(ud_disassemble(&f) == sizeof fs);
        TASK_CHECK(f.pfx_seg == UD_R_FS);
        m = ud_insn_opr(&f, 1);
        TASK_CHECK(m && m->type == UD_OP_MEM);
        TASK_CHECK(strcmp(ud_insn_asm(&f),
            "vgatherdps xmm1, dword [fs:rax+xmm4*4+0x10], xmm2") == 0);
    }

    /* mod=10 disp32 with a negative signed value (-32). */
    {
        const unsigned char nd[] =
            { 0xc4,0xe2,0x69,0x92,0x8c,0xa0,0xe0,0xff,0xff,0xff };
        ud_t f;
        const struct ud_operand *m;
        ud_init(&f);
        ud_set_mode(&f, 64);
        ud_set_syntax(&f, UD_SYN_INTEL);
        ud_set_input_buffer(&f, nd, sizeof nd);
        TASK_CHECK(ud_disassemble(&f) == sizeof nd);
        TASK_CHECK(ud_insn_mnemonic(&f) == UD_Ivgatherdps);
        m = ud_insn_opr(&f, 1);
        TASK_CHECK(m && m->offset == 32);
        TASK_CHECK(m->lval.sdword == -32);
        TASK_CHECK(strcmp(ud_insn_asm(&f),
            "vgatherdps xmm1, dword [rax+xmm4*4-0x20], xmm2") == 0);
    }
    return 0;
}

/* VSIB index encoding 4 is xmm4/ymm4; ordinary SIB index 4 means none. */
static int
case_index4_control(void)
{
    const unsigned char vsib[] =
        { 0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10 }; /* SIB index field = 4 */
    const unsigned char sib4[] = { 0x8b,0x04,0x24 }; /* mov eax,[rsp] */
    ud_t g, s;
    const struct ud_operand *mo;

    ud_init(&g);
    ud_set_mode(&g, 64);
    ud_set_input_buffer(&g, vsib, sizeof vsib);
    TASK_CHECK(ud_disassemble(&g) == sizeof vsib);
    mo = ud_insn_opr(&g, 1);
    TASK_CHECK(mo && mo->index == UD_R_XMM4);
    TASK_CHECK(mo->scale == 4);

    ud_init(&s);
    ud_set_mode(&s, 64);
    ud_set_input_buffer(&s, sib4, sizeof sib4);
    TASK_CHECK(ud_disassemble(&s) == 3);
    TASK_CHECK(ud_insn_mnemonic(&s) == UD_Imov);
    mo = ud_insn_opr(&s, 1);
    TASK_CHECK(mo && mo->type == UD_OP_MEM);
    TASK_CHECK(mo->base == UD_R_RSP);
    TASK_CHECK(mo->index == UD_NONE);   /* not r12-ish, just no index */
    TASK_CHECK(mo->scale == UD_NONE);
    return 0;
}

/* Patch helpers for register-alias collision encodings. */
static void set_vvvv(unsigned char *vex2, unsigned n)
{
    *vex2 = (unsigned char)((*vex2 & 0x87) | (((~n) & 0xf) << 3));
}
static void set_modrm_reg(unsigned char *modrm, unsigned n)
{
    *modrm = (unsigned char)((*modrm & 0xc7) | ((n & 7) << 3));
}
static void set_sib_index(unsigned char *sib, unsigned n)
{
    *sib = (unsigned char)((*sib & 0xc7) | ((n & 7) << 3));
}

static int
expect_invalid64(const unsigned char *b, size_t n)
{
    ud_t u;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_input_buffer(&u, b, n);
    (void)ud_disassemble(&u);
    return ud_insn_mnemonic(&u) == UD_Iinvalid;
}

/* Destination, index and mask must be pairwise distinct physical regs. */
static int
case_alias_conflicts(void)
{
    /* template: dest=1 index=4 mask=2 (legal) */
    unsigned char b[7] = { 0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10 };

    /* positive control */
    TASK_CHECK(expect_invalid64(b, sizeof b) == 0);

    /* destination 4 collides with index 4 */
    set_modrm_reg(&b[4], 4);
    TASK_CHECK(expect_invalid64(b, sizeof b));

    /* restore dest=1, make mask 4 collide with index 4 */
    set_modrm_reg(&b[4], 1);
    set_vvvv(&b[2], 4);
    TASK_CHECK(expect_invalid64(b, sizeof b));

    /* mask 2 collides with destination 2 */
    set_vvvv(&b[2], 2);
    set_modrm_reg(&b[4], 2);
    TASK_CHECK(expect_invalid64(b, sizeof b));

    /* index 2 collides with mask 2 */
    set_modrm_reg(&b[4], 1);
    set_sib_index(&b[5], 2);
    TASK_CHECK(expect_invalid64(b, sizeof b));

    /* all three physical register 5 */
    set_modrm_reg(&b[4], 5);
    set_sib_index(&b[5], 5);
    set_vvvv(&b[2], 5);
    TASK_CHECK(expect_invalid64(b, sizeof b));

    /* Cross-name collision: L=1 makes dest/mask ymm4 while the flavour
     * (VGATHERDPS) makes the index ymm4 too; force index xmm4 encoding
     * by using VGATHERDPD L=1 where the index is architecturally XMM. */
    {
        /* c4 e2 dd? build: W=1,L=1,vvvv=4,pp=1 -> b2 = 0xed patched */
        unsigned char x[7] = { 0xc4,0xe2,0xed,0x92,0x64,0xa0,0x10 };
        set_vvvv(&x[2], 4);      /* mask physical 4 */
        set_modrm_reg(&x[4], 4); /* dest physical 4 (ymm4) */
        set_sib_index(&x[5], 4); /* index xmm4 (DPD L=1 stays XMM) */
        TASK_CHECK(expect_invalid64(x, sizeof x));
    }
    return 0;
}

/* Byte-by-byte truncation of every prepared instruction. */
static int
case_truncation(void)
{
    size_t i, off = 0;
    const size_t n = sizeof mat / sizeof mat[0];

    for (i = 0; i < n; ++i) {
        unsigned k;
        for (k = 1; k < mat[i].len; ++k) {
            ud_t u;
            unsigned got;
            ud_init(&u);
            ud_set_mode(&u, 64);
            ud_set_input_buffer(&u, gather_inputs + off, k);
            got = ud_disassemble(&u);
            TASK_CHECK(got == k);
            TASK_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
        }
        off += mat[i].len;
    }
    TASK_CHECK(off == sizeof gather_inputs);
    return 0;
}

/* Missing SIB, register r/m and 16-bit addressing stay invalid. */
static int
case_invalid_forms(void)
{
    /* mod=01 rm=000 (no SIB), disp8 present */
    const unsigned char nosib[] = { 0xc4,0xe2,0x69,0x92,0x40,0x10 };
    /* mod=11 register r/m */
    const unsigned char reg[] = { 0xc4,0xe2,0x69,0x92,0xc8 };
    /* well-formed VSIB bytes, but decoded in 16-bit disassembly mode */
    const unsigned char vsib[] = { 0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10 };
    /* 67h in 32-bit mode selects 16-bit addressing -> invalid VSIB */
    const unsigned char a16[] =
        { 0x67,0xc4,0xe2,0x69,0x90,0x44,0x61,0x10 };
    ud_t u;

    TASK_CHECK(expect_invalid64(nosib, sizeof nosib));
    TASK_CHECK(expect_invalid64(reg, sizeof reg));

    ud_init(&u);
    ud_set_mode(&u, 16);
    ud_set_input_buffer(&u, vsib, sizeof vsib);
    (void)ud_disassemble(&u);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);

    ud_init(&u);
    ud_set_mode(&u, 32);
    ud_set_input_buffer(&u, a16, sizeof a16);
    (void)ud_disassemble(&u);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* compat                                                              */
/* ------------------------------------------------------------------ */

static int
case_mov_scalar_sib(void)
{
    /* 48 8b 44 8b 10 : mov rax,[rbx+rcx*4+0x10] */
    const unsigned char b[] = { 0x48,0x8b,0x44,0x8b,0x10 };
    ud_t u;
    const struct ud_operand *o;

    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, b, sizeof b);
    TASK_CHECK(ud_disassemble(&u) == sizeof b);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Imov);
    o = ud_insn_opr(&u, 1);
    TASK_CHECK(o && o->type == UD_OP_MEM);
    TASK_CHECK(o->base == UD_R_RBX);
    TASK_CHECK(o->index == UD_R_RCX);
    TASK_CHECK(o->scale == 4);
    TASK_CHECK(o->offset == 8);
    TASK_CHECK(o->lval.sbyte == 16);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
                      "mov rax, [rbx+rcx*4+0x10]") == 0);
    return 0;
}

static int
case_vex_arithmetic(void)
{
    /* c5 ec 58 cb : vaddps ymm1,ymm2,ymm3 */
    const unsigned char b[] = { 0xc5,0xec,0x58,0xcb };
    ud_t u;

    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, b, sizeof b);
    TASK_CHECK(ud_disassemble(&u) == sizeof b);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddps);
    TASK_CHECK(ud_insn_opr(&u, 0)->base == UD_R_YMM1);
    TASK_CHECK(ud_insn_opr(&u, 1)->base == UD_R_YMM2);
    TASK_CHECK(ud_insn_opr(&u, 2)->base == UD_R_YMM3);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
                      "vaddps ymm1, ymm2, ymm3") == 0);
    return 0;
}

static int
case_x87(void)
{
    const unsigned char b[] = { 0xdb,0xe3 }; /* fninit */
    ud_t u;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, b, sizeof b);
    TASK_CHECK(ud_disassemble(&u) == 2);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ifninit);
    TASK_CHECK(strcmp(ud_insn_asm(&u), "fninit") == 0);
    return 0;
}

/* Reusing a ud_t with a fresh normal input after gather decoding. */
static int
case_reset_input(void)
{
    const unsigned char mov[] = { 0x48,0x8b,0x44,0x8b,0x10 };
    ud_t u;

    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, gather_inputs, sizeof gather_inputs);
    TASK_CHECK(ud_disassemble(&u) == 7);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivgatherdps);

    ud_set_input_buffer(&u, mov, sizeof mov);
    TASK_CHECK(ud_disassemble(&u) == sizeof mov);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Imov);
    TASK_CHECK(strstr(ud_insn_asm(&u), "mov rax, [rbx+rcx*4+0x10]") != NULL);
    return 0;
}

/* Pre-existing public mnemonic enumerator numeric values are unchanged:
 * the gather codes were appended after UD_Ipause. */
static int
case_legacy_enum_values(void)
{
    TASK_CHECK((int)UD_Iaaa == 0);
    TASK_CHECK((int)UD_Imov == 303);
    TASK_CHECK((int)UD_Iinvalid == 903);
    TASK_CHECK((int)UD_I3dnow == 904);
    TASK_CHECK((int)UD_Inone == 905);
    TASK_CHECK((int)UD_Idb == 906);
    TASK_CHECK((int)UD_Ipause == 907);
    TASK_CHECK((int)UD_Ivgatherdps == 908);
    TASK_CHECK((int)UD_Ivpgatherqq == 915);
    TASK_CHECK((int)UD_MAX_MNEMONIC_CODE == 916);
    TASK_CHECK(strcmp(ud_lookup_mnemonic(UD_Ivgatherqpd),
                      "vgatherqpd") == 0);
    TASK_CHECK(strcmp(ud_lookup_mnemonic(UD_Ivpgatherdq),
                      "vpgatherdq") == 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "width_map", "material_table",   case_material_table },
    { "width_map", "explicit_matrix",  case_explicit_matrix },
    { "width_map", "mode32",           case_mode32 },

    { "vsib", "extras",                case_vsib_extras },
    { "vsib", "index4_control",        case_index4_control },
    { "vsib", "alias_conflicts",       case_alias_conflicts },
    { "vsib", "truncation",            case_truncation },
    { "vsib", "invalid_forms",         case_invalid_forms },

    { "compat", "mov_scalar_sib",      case_mov_scalar_sib },
    { "compat", "vex_arithmetic",      case_vex_arithmetic },
    { "compat", "x87",                 case_x87 },
    { "compat", "reset_input",         case_reset_input },
    { "compat", "legacy_enum_values",  case_legacy_enum_values },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
