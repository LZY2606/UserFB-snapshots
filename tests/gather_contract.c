/*
 * gather_contract.c - AVX2 VEX gather (VSIB) decoding regression.
 *
 * Registered through task_support/check.h as three task_case groups:
 *   width_map - every prepared instruction: operand contents, byte count
 *               and both Intel/AT&T texts;
 *   vsib      - VSIB addressing semantics (vector index 4, high regs,
 *               no-base disp32, signed displacements, scales, 67h);
 *   compat    - register conflicts, truncation and invalid forms stay
 *               invalid while ordinary MOV/SIB/VEX/x87 keep decoding.
 *
 * Only byte decoding is exercised: nothing is executed and no register
 * state or per-lane address is consulted.
 */
#include <stdio.h>
#include <string.h>
#include "udis86.h"
#include "task_support/check.h"
#include "task_support/gather-inputs.h"

/* Byte lengths of the 20 instructions concatenated in gather_inputs[]. */
static const size_t sample_len[20] = {
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,   /* 1..16: the eight mnemonics x L */
    7,6,10,8                            /* 17..20: special addressing */
};

struct gather_spec {
    enum ud_mnemonic_code mn;
    enum ud_type dst;                   /* destination vector register */
    enum ud_type mask;                  /* VEX.vvvv mask register */
    enum ud_type index;                 /* VSIB vector index register */
    enum ud_type base;                  /* general base register / UD_NONE */
    uint16_t dst_size;
    uint16_t mask_size;
    uint16_t elem_size;                 /* single gathered element width */
    uint8_t  scale;
    uint8_t  disp_width;                /* 0, 8 or 32 */
    int32_t  disp;
    uint8_t  mode;
    const char *intel;
    const char *att;
};

#define X(n) UD_R_XMM##n
#define Y(n) UD_R_YMM##n

static const struct gather_spec S[20] = {
  /*  1 */ { UD_Ivgatherdps, X(1), X(2), X(4), UD_R_RAX, 128,128,32,4,8,16,64,
             "vgatherdps xmm1, dword [rax+xmm4*4+0x10], xmm2",
             "vgatherdps %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
  /*  2 */ { UD_Ivgatherdps, Y(1), Y(2), Y(4), UD_R_RAX, 256,256,32,4,8,16,64,
             "vgatherdps ymm1, dword [rax+ymm4*4+0x10], ymm2",
             "vgatherdps %ymm2, 0x10(%rax,%ymm4,4), %ymm1" },
  /*  3 */ { UD_Ivgatherdpd, X(1), X(2), X(4), UD_R_RAX, 128,128,64,4,8,16,64,
             "vgatherdpd xmm1, qword [rax+xmm4*4+0x10], xmm2",
             "vgatherdpd %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
  /*  4 */ { UD_Ivgatherdpd, Y(1), Y(2), X(4), UD_R_RAX, 256,256,64,4,8,16,64,
             "vgatherdpd ymm1, qword [rax+xmm4*4+0x10], ymm2",
             "vgatherdpd %ymm2, 0x10(%rax,%xmm4,4), %ymm1" },
  /*  5 */ { UD_Ivgatherqps, X(1), X(2), X(4), UD_R_RAX, 128,128,32,4,8,16,64,
             "vgatherqps xmm1, dword [rax+xmm4*4+0x10], xmm2",
             "vgatherqps %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
  /*  6 */ { UD_Ivgatherqps, X(1), X(2), Y(4), UD_R_RAX, 128,128,32,4,8,16,64,
             "vgatherqps xmm1, dword [rax+ymm4*4+0x10], xmm2",
             "vgatherqps %xmm2, 0x10(%rax,%ymm4,4), %xmm1" },
  /*  7 */ { UD_Ivgatherqpd, X(1), X(2), X(4), UD_R_RAX, 128,128,64,4,8,16,64,
             "vgatherqpd xmm1, qword [rax+xmm4*4+0x10], xmm2",
             "vgatherqpd %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
  /*  8 */ { UD_Ivgatherqpd, Y(1), Y(2), Y(4), UD_R_RAX, 256,256,64,4,8,16,64,
             "vgatherqpd ymm1, qword [rax+ymm4*4+0x10], ymm2",
             "vgatherqpd %ymm2, 0x10(%rax,%ymm4,4), %ymm1" },
  /*  9 */ { UD_Ivpgatherdd, X(1), X(2), X(4), UD_R_RAX, 128,128,32,4,8,16,64,
             "vpgatherdd xmm1, dword [rax+xmm4*4+0x10], xmm2",
             "vpgatherdd %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
  /* 10 */ { UD_Ivpgatherdd, Y(1), Y(2), Y(4), UD_R_RAX, 256,256,32,4,8,16,64,
             "vpgatherdd ymm1, dword [rax+ymm4*4+0x10], ymm2",
             "vpgatherdd %ymm2, 0x10(%rax,%ymm4,4), %ymm1" },
  /* 11 */ { UD_Ivpgatherdq, X(1), X(2), X(4), UD_R_RAX, 128,128,64,4,8,16,64,
             "vpgatherdq xmm1, qword [rax+xmm4*4+0x10], xmm2",
             "vpgatherdq %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
  /* 12 */ { UD_Ivpgatherdq, Y(1), Y(2), X(4), UD_R_RAX, 256,256,64,4,8,16,64,
             "vpgatherdq ymm1, qword [rax+xmm4*4+0x10], ymm2",
             "vpgatherdq %ymm2, 0x10(%rax,%xmm4,4), %ymm1" },
  /* 13 */ { UD_Ivpgatherqd, X(1), X(2), X(4), UD_R_RAX, 128,128,32,4,8,16,64,
             "vpgatherqd xmm1, dword [rax+xmm4*4+0x10], xmm2",
             "vpgatherqd %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
  /* 14 */ { UD_Ivpgatherqd, X(1), X(2), Y(4), UD_R_RAX, 128,128,32,4,8,16,64,
             "vpgatherqd xmm1, dword [rax+ymm4*4+0x10], xmm2",
             "vpgatherqd %xmm2, 0x10(%rax,%ymm4,4), %xmm1" },
  /* 15 */ { UD_Ivpgatherqq, X(1), X(2), X(4), UD_R_RAX, 128,128,64,4,8,16,64,
             "vpgatherqq xmm1, qword [rax+xmm4*4+0x10], xmm2",
             "vpgatherqq %xmm2, 0x10(%rax,%xmm4,4), %xmm1" },
  /* 16 */ { UD_Ivpgatherqq, Y(1), Y(2), Y(4), UD_R_RAX, 256,256,64,4,8,16,64,
             "vpgatherqq ymm1, qword [rax+ymm4*4+0x10], ymm2",
             "vpgatherqq %ymm2, 0x10(%rax,%ymm4,4), %ymm1" },
  /* 17 */ { UD_Ivgatherdpd, Y(3), Y(5), X(12), UD_R_R13, 256,256,64,8,8,-32,64,
             "vgatherdpd ymm3, qword [r13+xmm12*8-0x20], ymm5",
             "vgatherdpd %ymm5, -0x20(%r13,%xmm12,8), %ymm3" },
  /* 18 */ { UD_Ivgatherqps, X(6), X(8), Y(7), UD_R_RBX, 128,128,32,2,0,0,64,
             "vgatherqps xmm6, dword [rbx+ymm7*2], xmm8",
             "vgatherqps %xmm8, (%rbx,%ymm7,2), %xmm6" },
  /* 19 */ { UD_Ivgatherqpd, Y(9), Y(11), Y(10), UD_NONE, 256,256,64,4,32,
             0x11223344,64,
             "vgatherqpd ymm9, qword [ymm10*4+0x11223344], ymm11",
             "vgatherqpd %ymm11, 0x11223344(%ymm10,4), %ymm9" },
  /* 20 */ { UD_Ivpgatherdd, X(0), X(2), X(4), UD_R_ECX, 128,128,32,2,8,16,64,
             "vpgatherdd xmm0, dword [ecx+xmm4*2+0x10], xmm2",
             "vpgatherdd %xmm2, 0x10(%ecx,%xmm4,2), %xmm0" }
};

static size_t sample_offset(int i)
{
    size_t off = 0;
    int k;
    for (k = 0; k < i; ++k) {
        off += sample_len[k];
    }
    return off;
}

static unsigned int
decode_sample(ud_t *u, int i, int att)
{
    size_t off = sample_offset(i);
    ud_init(u);
    ud_set_mode(u, S[i].mode);
    ud_set_syntax(u, att ? UD_SYN_ATT : UD_SYN_INTEL);
    ud_set_input_buffer(u, gather_inputs + off, sample_len[i]);
    return ud_disassemble(u);
}

static int
disp_matches(const struct ud_operand *m, const struct gather_spec *s)
{
    if (m->offset != s->disp_width) {
        return 0;
    }
    if (s->disp_width == 8) {
        return m->lval.sbyte == s->disp;
    }
    if (s->disp_width == 32) {
        return m->lval.sdword == s->disp;
    }
    return s->disp == 0;
}

/* ---- width_map: full structural and text contract for every sample ---- */

static int width_case(int i)
{
    const struct gather_spec *s = &S[i];
    ud_t u;
    const struct ud_operand *d, *m, *k;
    unsigned int len;

    len = decode_sample(&u, i, 0);
    TASK_CHECK(ud_insn_mnemonic(&u) == s->mn);
    TASK_CHECK(len == sample_len[i]);
    TASK_CHECK(ud_insn_len(&u) == sample_len[i]);

    d = ud_insn_opr(&u, 0);
    m = ud_insn_opr(&u, 1);
    k = ud_insn_opr(&u, 2);
    TASK_CHECK(d && d->type == UD_OP_REG && d->base == s->dst &&
               d->size == s->dst_size);
    TASK_CHECK(m && m->type == UD_OP_MEM && m->size == s->elem_size);
    TASK_CHECK(m->base == s->base);
    TASK_CHECK(m->index == s->index);
    TASK_CHECK(m->scale == s->scale);
    TASK_CHECK(disp_matches(m, s));
    TASK_CHECK(k && k->type == UD_OP_REG && k->base == s->mask &&
               k->size == s->mask_size);
    TASK_CHECK(ud_insn_opr(&u, 3) == NULL);
    TASK_CHECK(strcmp(ud_insn_asm(&u), s->intel) == 0);

    /* AT&T renders the same three operands in reverse order. */
    len = decode_sample(&u, i, 1);
    TASK_CHECK(len == sample_len[i]);
    TASK_CHECK(ud_insn_mnemonic(&u) == s->mn);
    TASK_CHECK(ud_insn_opr(&u, 0)->base == s->dst);
    TASK_CHECK(ud_insn_opr(&u, 1)->index == s->index);
    TASK_CHECK(ud_insn_opr(&u, 2)->base == s->mask);
    TASK_CHECK(strcmp(ud_insn_asm(&u), s->att) == 0);
    return 0;
}

static int w01(void) { return width_case(0); }
static int w02(void) { return width_case(1); }
static int w03(void) { return width_case(2); }
static int w04(void) { return width_case(3); }
static int w05(void) { return width_case(4); }
static int w06(void) { return width_case(5); }
static int w07(void) { return width_case(6); }
static int w08(void) { return width_case(7); }
static int w09(void) { return width_case(8); }
static int w10(void) { return width_case(9); }
static int w11(void) { return width_case(10); }
static int w12(void) { return width_case(11); }
static int w13(void) { return width_case(12); }
static int w14(void) { return width_case(13); }
static int w15(void) { return width_case(14); }
static int w16(void) { return width_case(15); }
static int w17(void) { return width_case(16); }
static int w18(void) { return width_case(17); }
static int w19(void) { return width_case(18); }
static int w20(void) { return width_case(19); }

/* ---- vsib: addressing field semantics ---- */

/* SIB index field 4 names xmm4/ymm4, never the scalar no-index marker. */
static int vsib_index4(void)
{
    ud_t u;
    const struct ud_operand *m;
    decode_sample(&u, 0, 0);
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->type == UD_OP_MEM);
    TASK_CHECK(m->index == UD_R_XMM4);
    TASK_CHECK(m->scale == 4);
    TASK_CHECK(strstr(ud_insn_asm(&u), "xmm4*4") != NULL);
    decode_sample(&u, 1, 1);
    TASK_CHECK(ud_insn_opr(&u, 1)->index == UD_R_YMM4);
    TASK_CHECK(strstr(ud_insn_asm(&u), "%ymm4,4") != NULL);
    return 0;
}

/* VEX.X/B extension bits reach high vector index and high base registers. */
static int vsib_high_regs(void)
{
    ud_t u;
    const struct ud_operand *m;
    decode_sample(&u, 16, 0);               /* sample 17 */
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->base == UD_R_R13);
    TASK_CHECK(m->index == UD_R_XMM12);
    TASK_CHECK(m->scale == 8);
    TASK_CHECK(m->offset == 8 && m->lval.sbyte == -32);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
        "vgatherdpd ymm3, qword [r13+xmm12*8-0x20], ymm5") == 0);
    return 0;
}

/* ModRM.mod==0 with base rbp/r13 means no base plus disp32, never RIP. */
static int vsib_no_base_disp32(void)
{
    ud_t u;
    const struct ud_operand *m;
    decode_sample(&u, 18, 0);               /* sample 19 */
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->base == UD_NONE);
    TASK_CHECK(m->base != UD_R_RIP);
    TASK_CHECK(m->index == UD_R_YMM10);
    TASK_CHECK(m->scale == 4);
    TASK_CHECK(m->offset == 32);
    TASK_CHECK(m->lval.sdword == 0x11223344);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
        "vgatherqpd ymm9, qword [ymm10*4+0x11223344], ymm11") == 0);
    decode_sample(&u, 18, 1);
    TASK_CHECK(ud_insn_opr(&u, 1)->base == UD_NONE);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
        "vgatherqpd %ymm11, 0x11223344(%ymm10,4), %ymm9") == 0);
    return 0;
}

/* Zero displacement is encoded without any disp bytes. */
static int vsib_no_disp(void)
{
    ud_t u;
    const struct ud_operand *m;
    decode_sample(&u, 17, 0);               /* sample 18 */
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->offset == 0);
    TASK_CHECK(m->base == UD_R_RBX);
    TASK_CHECK(m->index == UD_R_YMM7);
    TASK_CHECK(m->scale == 2);
    TASK_CHECK(ud_insn_len(&u) == sample_len[17]);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
        "vgatherqps xmm6, dword [rbx+ymm7*2], xmm8") == 0);
    return 0;
}

/* A crafted negative disp32 keeps its signed value across the VSIB path. */
static int vsib_signed_disp32(void)
{
    static const unsigned char code[] = {
        0xc4,0xe2,0x69,0x92,                /* vgatherdps, L=0 W=0 */
        0x8c,0xa0,                          /* mod=10 rm=4; SIB scale4 idx4 rax */
        0x88,0xa9,0xcb,0xed                 /* disp32 = -0x12345678 */
    };
    ud_t u;
    const struct ud_operand *m;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, code, sizeof code);
    TASK_CHECK(ud_disassemble(&u) == sizeof code);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivgatherdps);
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->offset == 32);
    TASK_CHECK(m->lval.sdword == (int32_t) -0x12345678);
    TASK_CHECK(m->index == UD_R_XMM4);
    TASK_CHECK(m->base == UD_R_RAX);
    return 0;
}

/* A segment override prefix is reported through pfx_seg and both texts. */
static int vsib_segment(void)
{
    static const unsigned char code[] = {
        0x64,                               /* FS */
        0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10
    };
    ud_t u;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, code, sizeof code);
    TASK_CHECK(ud_disassemble(&u) == sizeof code);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivgatherdps);
    TASK_CHECK(u.pfx_seg == UD_R_FS);
    TASK_CHECK(strstr(ud_insn_asm(&u), "[fs:") != NULL);

    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_ATT);
    ud_set_input_buffer(&u, code, sizeof code);
    TASK_CHECK(ud_disassemble(&u) == sizeof code);
    TASK_CHECK(u.pfx_seg == UD_R_FS);
    TASK_CHECK(strstr(ud_insn_asm(&u), "%fs:") != NULL);
    return 0;
}

/* Native 32-bit mode uses 32-bit addressing without any override prefix. */
static int vsib_native32(void)
{
    ud_t u;
    const struct ud_operand *m;
    /* Sample 1 (vgatherdps xmm1,[rax+xmm4*4+16],xmm2) has no 67h. */
    ud_init(&u);
    ud_set_mode(&u, 32);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, gather_inputs, sample_len[0]);
    TASK_CHECK(ud_disassemble(&u) == sample_len[0]);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivgatherdps);
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->base == UD_R_EAX);
    TASK_CHECK(m->index == UD_R_XMM4);
    TASK_CHECK(m->scale == 4 && m->offset == 8 && m->lval.sbyte == 16);
    TASK_CHECK(strcmp(ud_insn_asm(&u),
        "vgatherdps xmm1, dword [eax+xmm4*4+0x10], xmm2") == 0);
    return 0;
}

/* 67h address override selects 32-bit addressing in 64-bit mode. */
static int vsib_addr_override(void)
{
    ud_t u;
    /* Sample 20 carries a leading 67h: in 64-bit mode that means 32-bit
     * addressing, so the base decodes as a 32-bit general register. */
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, gather_inputs + sample_offset(19), sample_len[19]);
    TASK_CHECK(ud_disassemble(&u) == sample_len[19]);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivpgatherdd);
    TASK_CHECK(ud_insn_opr(&u, 1)->base == UD_R_ECX);
    TASK_CHECK(ud_insn_opr(&u, 1)->index == UD_R_XMM4);
    TASK_CHECK(ud_insn_opr(&u, 1)->scale == 2);

    /* The same bytes in native 32-bit mode switch to 16-bit addressing,
     * which has no SIB/VSIB form and therefore stays invalid. */
    ud_init(&u);
    ud_set_mode(&u, 32);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, gather_inputs + sample_offset(19), sample_len[19]);
    ud_disassemble(&u);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
    return 0;
}

/* ---- compat: invalid forms and legacy instruction compatibility ---- */

static int is_invalid(const unsigned char *code, size_t n, unsigned mode)
{
    ud_t u;
    ud_init(&u);
    ud_set_mode(&u, mode);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, code, n);
    ud_disassemble(&u);
    return ud_insn_mnemonic(&u) == UD_Iinvalid;
}

static int compat_conflict_dst_index(void)
{
    /* vgatherdps xmm1 with SIB index field 1 -> index xmm1 == destination */
    static const unsigned char code[] = {0xc4,0xe2,0x69,0x92,0x4c,0x88,0x10};
    TASK_CHECK(is_invalid(code, sizeof code, 64));
    return 0;
}

static int compat_conflict_dst_mask(void)
{
    /* VEX.vvvv set to 1 (xmm1), the same register as ModRM.reg. */
    static const unsigned char code[] = {0xc4,0xe2,0x71,0x92,0x4c,0xa0,0x10};
    TASK_CHECK(is_invalid(code, sizeof code, 64));
    return 0;
}

static int compat_conflict_cross_width(void)
{
    /* vgatherdpd ymm1 (xmm index) with SIB index 1 -> xmm1 aliases ymm1. */
    static const unsigned char code[] = {0xc4,0xe2,0xed,0x92,0x4c,0x88,0x10};
    TASK_CHECK(is_invalid(code, sizeof code, 64));
    return 0;
}

static int compat_conflict_index_mask(void)
{
    /* mask is xmm2; SIB index field 2 makes the VSIB index xmm2 too. */
    static const unsigned char code[] = {0xc4,0xe2,0x69,0x92,0x4c,0x90,0x10};
    TASK_CHECK(is_invalid(code, sizeof code, 64));
    return 0;
}

static int compat_truncation(void)
{
    static const unsigned char code[] = {0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10};
    size_t k;
    for (k = 1; k < sizeof code; ++k) {
        TASK_CHECK(is_invalid(code, k, 64));
    }
    /* The complete byte stream decodes and consumes every byte. */
    TASK_CHECK(!is_invalid(code, sizeof code, 64));
    return 0;
}

static int compat_no_sib(void)
{
    /* ModRM rm=0 (no SIB byte) cannot express a VSIB operand. */
    static const unsigned char code[] = {0xc4,0xe2,0x69,0x92,0x48,0xa0,0x10};
    TASK_CHECK(is_invalid(code, sizeof code, 64));
    return 0;
}

static int compat_register_rm(void)
{
    /* ModRM.mod=3 register form is not a gather memory operand. */
    static const unsigned char code[] = {0xc4,0xe2,0x69,0x92,0xcc,0xa0,0x10};
    TASK_CHECK(is_invalid(code, sizeof code, 64));
    return 0;
}

static int compat_addr16(void)
{
    static const unsigned char code[] = {0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10};
    static const unsigned char with67[] =
        {0x67,0xc4,0xe2,0x69,0x92,0x4c,0xa0,0x10};
    /* 16-bit addressing has no SIB, hence no VSIB form. */
    TASK_CHECK(is_invalid(code, sizeof code, 16));
    TASK_CHECK(is_invalid(with67, sizeof with67, 32));
    return 0;
}

/* Ordinary scalar SIB keeps index field 4 as the explicit no-index marker. */
static int compat_scalar_sib_index4(void)
{
    /* mov rax, [rbx + rsp(no index)*4 + 16]: SIB a3 = scale4 idx4 base3 */
    static const unsigned char code[] = {0x48,0x8b,0x44,0xa3,0x10};
    ud_t u;
    const struct ud_operand *m;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, code, sizeof code);
    TASK_CHECK(ud_disassemble(&u) == sizeof code);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Imov);
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->type == UD_OP_MEM);
    TASK_CHECK(m->base == UD_R_RBX);
    TASK_CHECK(m->index == UD_NONE);
    TASK_CHECK(m->scale == UD_NONE);
    TASK_CHECK(m->offset == 8 && m->lval.sbyte == 16);
    return 0;
}

static int compat_mov(void)
{
    static const unsigned char code[] = {0x48,0x8b,0x44,0x8b,0x10};
    ud_t u;
    const struct ud_operand *m;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, code, sizeof code);
    TASK_CHECK(ud_disassemble(&u) == sizeof code);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Imov);
    m = ud_insn_opr(&u, 1);
    TASK_CHECK(m->base == UD_R_RBX && m->index == UD_R_RCX &&
               m->scale == 4 && m->lval.sbyte == 16);
    return 0;
}

static int compat_vex_arith(void)
{
    static const unsigned char code[] = {0xc5,0xec,0x58,0xcb};
    ud_t u;
    ud_init(&u);
    ud_set_mode(&u, 64);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, code, sizeof code);
    TASK_CHECK(ud_disassemble(&u) == sizeof code);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddps);
    TASK_CHECK(ud_insn_opr(&u, 0)->base == UD_R_YMM1);
    TASK_CHECK(ud_insn_opr(&u, 1)->base == UD_R_YMM2);
    TASK_CHECK(ud_insn_opr(&u, 2)->base == UD_R_YMM3);
    return 0;
}

static int compat_x87(void)
{
    static const unsigned char code[] = {0xd8,0xc1};
    ud_t u;
    ud_init(&u);
    ud_set_mode(&u, 32);
    ud_set_syntax(&u, UD_SYN_INTEL);
    ud_set_input_buffer(&u, code, sizeof code);
    TASK_CHECK(ud_disassemble(&u) == sizeof code);
    TASK_CHECK(ud_insn_mnemonic(&u) == UD_Ifadd);
    TASK_CHECK(ud_insn_opr(&u, 0)->base == UD_R_ST0);
    TASK_CHECK(ud_insn_opr(&u, 1)->base == UD_R_ST1);
    return 0;
}

const struct task_case task_cases[] = {
    /* width_map */
    { "width_map", "w01", w01 }, { "width_map", "w02", w02 },
    { "width_map", "w03", w03 }, { "width_map", "w04", w04 },
    { "width_map", "w05", w05 }, { "width_map", "w06", w06 },
    { "width_map", "w07", w07 }, { "width_map", "w08", w08 },
    { "width_map", "w09", w09 }, { "width_map", "w10", w10 },
    { "width_map", "w11", w11 }, { "width_map", "w12", w12 },
    { "width_map", "w13", w13 }, { "width_map", "w14", w14 },
    { "width_map", "w15", w15 }, { "width_map", "w16", w16 },
    { "width_map", "w17", w17 }, { "width_map", "w18", w18 },
    { "width_map", "w19", w19 }, { "width_map", "w20", w20 },
    /* vsib */
    { "vsib", "index4_vector",    vsib_index4 },
    { "vsib", "high_regs",        vsib_high_regs },
    { "vsib", "no_base_disp32",   vsib_no_base_disp32 },
    { "vsib", "no_disp",          vsib_no_disp },
    { "vsib", "signed_disp32",    vsib_signed_disp32 },
    { "vsib", "native32",         vsib_native32 },
    { "vsib", "addr_override",    vsib_addr_override },
    { "vsib", "segment",          vsib_segment },
    /* compat */
    { "compat", "conflict_dst_index",  compat_conflict_dst_index },
    { "compat", "conflict_dst_mask",   compat_conflict_dst_mask },
    { "compat", "conflict_cross_width",compat_conflict_cross_width },
    { "compat", "conflict_index_mask", compat_conflict_index_mask },
    { "compat", "truncation",          compat_truncation },
    { "compat", "no_sib",              compat_no_sib },
    { "compat", "register_rm",         compat_register_rm },
    { "compat", "addr16",              compat_addr16 },
    { "compat", "scalar_sib_index4",   compat_scalar_sib_index4 },
    { "compat", "mov",                 compat_mov },
    { "compat", "vex_arith",           compat_vex_arith },
    { "compat", "x87",                 compat_x87 }
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
