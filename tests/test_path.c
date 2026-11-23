/* tests/test_path.c - conformance tests for fd_path_analyze.

   Contains an independent concrete interpreter used to validate edge
   witnesses by replay, plus a small-scale enumeration reference for
   narrow free inputs. */

#include "task_support/check.h"
#include "fd_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_CF = 0, F_PF = 1, F_AF = 2, F_ZF = 3, F_SF = 4, F_OF = 5 };

/* ---------------- independent concrete interpreter ---------------- */

typedef struct {
    uint64_t r[16];
    uint8_t fl[6];
    int err;                     /* set on internal replay problems */
    const FdPathWitness *wit;    /* choices to consume, may be NULL */
    size_t ci;                   /* consumed choice count */
} Tx;

static uint64_t tmask(int w) { return w >= 64 ? ~0ull : ((1ull << w) - 1); }

static int tpar(uint8_t x) {
    x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return x & 1;
}

static uint64_t tread(Tx *x, const FdInstr *d, int i, int *w) {
    if (FD_OP_TYPE(d, i) == FD_OT_IMM) {
        *w = FD_OP_SIZE(d, 0) * 8;
        return (uint64_t)FD_OP_IMM(d, i) & tmask(*w);
    }
    {
        int idx = FD_OP_REG(d, i);
        if (FD_OP_REG_TYPE(d, i) == FD_RT_GPH) {
            *w = 8;
            return (x->r[idx - 4] >> 8) & 0xff;
        }
        *w = FD_OP_SIZE(d, i) * 8;
        return x->r[idx] & tmask(*w);
    }
}

static void twrite(Tx *x, const FdInstr *d, int i, uint64_t v) {
    int idx = FD_OP_REG(d, i);
    int sz = FD_OP_SIZE(d, i);
    if (FD_OP_REG_TYPE(d, i) == FD_RT_GPH) {
        x->r[idx - 4] = (x->r[idx - 4] & ~0xff00ull) | ((v & 0xff) << 8);
        return;
    }
    if (sz == 8) x->r[idx] = v;
    else if (sz == 4) x->r[idx] = (uint32_t)v;
    else x->r[idx] = (x->r[idx] & ~tmask(sz * 8)) | (v & tmask(sz * 8));
}

static void tszp(Tx *x, uint64_t r, int w) {
    r &= tmask(w);
    x->fl[F_ZF] = (r == 0);
    x->fl[F_SF] = (uint8_t)((r >> (w - 1)) & 1);
    x->fl[F_PF] = (uint8_t)!tpar((uint8_t)r);
}

static uint64_t tadd(Tx *x, uint64_t a, uint64_t b, uint64_t cin, int w) {
    uint64_t m = tmask(w), r, sign = 1ull << (w - 1);
    __uint128_t s;
    a &= m; b &= m;
    s = (__uint128_t)a + b + (cin & 1);
    r = (uint64_t)s & m;
    x->fl[F_CF] = (uint8_t)((uint64_t)(s >> w) & 1);
    x->fl[F_OF] = (uint8_t)((~(a ^ b) & (a ^ r) & sign) != 0);
    x->fl[F_AF] = (uint8_t)(((a ^ b ^ r) >> 4) & 1);
    tszp(x, r, w);
    return r;
}

static uint64_t tsub(Tx *x, uint64_t a, uint64_t b, uint64_t bin, int w) {
    uint64_t m = tmask(w), r, sign = 1ull << (w - 1);
    __uint128_t sub;
    a &= m; b &= m;
    sub = (__uint128_t)b + (bin & 1);
    r = (uint64_t)((__uint128_t)a - sub) & m;
    x->fl[F_CF] = (uint8_t)((__uint128_t)a < sub);
    x->fl[F_OF] = (uint8_t)(((a ^ b) & (a ^ r) & sign) != 0);
    x->fl[F_AF] = (uint8_t)(((a ^ b ^ r) >> 4) & 1);
    tszp(x, r, w);
    return r;
}

/* Condition evaluation straight from the architectural definitions. */
static int tcond(const Tx *x, FdInstrType t) {
    int cf = x->fl[F_CF], pf = x->fl[F_PF], zf = x->fl[F_ZF];
    int sf = x->fl[F_SF], of = x->fl[F_OF];
    switch (t) {
    case FDI_JO: case FDI_SETO: case FDI_CMOVO: return of;
    case FDI_JNO: case FDI_SETNO: case FDI_CMOVNO: return !of;
    case FDI_JC: case FDI_SETC: case FDI_CMOVC: return cf;
    case FDI_JNC: case FDI_SETNC: case FDI_CMOVNC: return !cf;
    case FDI_JZ: case FDI_SETZ: case FDI_CMOVZ: return zf;
    case FDI_JNZ: case FDI_SETNZ: case FDI_CMOVNZ: return !zf;
    case FDI_JBE: case FDI_SETBE: case FDI_CMOVBE: return cf || zf;
    case FDI_JA: case FDI_SETA: case FDI_CMOVA: return !cf && !zf;
    case FDI_JS: case FDI_SETS: case FDI_CMOVS: return sf;
    case FDI_JNS: case FDI_SETNS: case FDI_CMOVNS: return !sf;
    case FDI_JP: case FDI_SETP: case FDI_CMOVP: return pf;
    case FDI_JNP: case FDI_SETNP: case FDI_CMOVNP: return !pf;
    case FDI_JL: case FDI_SETL: case FDI_CMOVL: return sf != of;
    case FDI_JGE: case FDI_SETGE: case FDI_CMOVGE: return sf == of;
    case FDI_JLE: case FDI_SETLE: case FDI_CMOVLE: return zf || (sf != of);
    case FDI_JG: case FDI_SETG: case FDI_CMOVG: return !zf && (sf == of);
    default: return -1;
    }
}

static int is_jcc_t(FdInstrType t) {
    switch (t) {
    case FDI_JO: case FDI_JNO: case FDI_JC: case FDI_JNC:
    case FDI_JZ: case FDI_JNZ: case FDI_JBE: case FDI_JA:
    case FDI_JS: case FDI_JNS: case FDI_JP: case FDI_JNP:
    case FDI_JL: case FDI_JGE: case FDI_JLE: case FDI_JG:
        return 1;
    default:
        return 0;
    }
}

/* Execute the instruction at *off; return the next offset, or SIZE_MAX
   for RET.  Jcc directions are evaluated but followed by the caller via
   the return value. */
static size_t tstep(Tx *x, const uint8_t *code, size_t len, size_t off) {
    FdInstr d;
    FdInstrType t;
    int w = 0, w2 = 0, r;
    uint64_t a, b, res;
    r = fd_decode(code + off, len - off, 64, 0, &d);
    if (r <= 0) { x->err = 1; return SIZE_MAX; }
    t = FD_TYPE(&d);
    switch (t) {
    case FDI_MOV: case FDI_MOVABS:
        twrite(x, &d, 0, tread(x, &d, 1, &w));
        break;
    case FDI_MOVZX:
        twrite(x, &d, 0, tread(x, &d, 1, &w));
        break;
    case FDI_ADD: case FDI_ADC:
        a = tread(x, &d, 0, &w);
        b = tread(x, &d, 1, &w2);
        twrite(x, &d, 0, tadd(x, a, b, t == FDI_ADC ? x->fl[F_CF] : 0, w));
        break;
    case FDI_SUB: case FDI_SBB: case FDI_CMP:
        a = tread(x, &d, 0, &w);
        b = tread(x, &d, 1, &w2);
        res = tsub(x, a, b, t == FDI_SBB ? x->fl[F_CF] : 0, w);
        if (t != FDI_CMP) twrite(x, &d, 0, res);
        break;
    case FDI_INC: case FDI_DEC: {
        uint8_t cf = x->fl[F_CF];
        a = tread(x, &d, 0, &w);
        res = (t == FDI_INC) ? tadd(x, a, 0, 1, w) : tsub(x, a, 0, 1, w);
        x->fl[F_CF] = cf;
        twrite(x, &d, 0, res);
        break;
    }
    case FDI_TEST: case FDI_AND: case FDI_OR: case FDI_XOR:
        a = tread(x, &d, 0, &w);
        b = tread(x, &d, 1, &w2);
        if (t == FDI_OR) res = a | b;
        else if (t == FDI_XOR) res = a ^ b;
        else res = a & b;
        res &= tmask(w);
        x->fl[F_CF] = 0;
        x->fl[F_OF] = 0;
        if (x->wit) {
            if (x->ci >= x->wit->choice_count ||
                x->wit->choices[x->ci].offset != off) {
                x->err = 1;
                return SIZE_MAX;
            }
            x->fl[F_AF] = x->wit->choices[x->ci].af;
            x->ci++;
        } else {
            x->fl[F_AF] = 0;
        }
        tszp(x, res, w);
        if (t != FDI_TEST) twrite(x, &d, 0, res);
        break;
    case FDI_CLC: x->fl[F_CF] = 0; break;
    case FDI_STC: x->fl[F_CF] = 1; break;
    case FDI_CMC: x->fl[F_CF] = !x->fl[F_CF]; break;
    case FDI_LAHF:
        a = (uint64_t)x->fl[F_CF] | 2ull | ((uint64_t)x->fl[F_PF] << 2) |
            ((uint64_t)x->fl[F_AF] << 4) | ((uint64_t)x->fl[F_ZF] << 6) |
            ((uint64_t)x->fl[F_SF] << 7);
        x->r[0] = (x->r[0] & ~0xff00ull) | (a << 8);
        break;
    case FDI_SAHF:
        a = (x->r[0] >> 8) & 0xff;
        x->fl[F_CF] = (uint8_t)(a & 1);
        x->fl[F_PF] = (uint8_t)((a >> 2) & 1);
        x->fl[F_AF] = (uint8_t)((a >> 4) & 1);
        x->fl[F_ZF] = (uint8_t)((a >> 6) & 1);
        x->fl[F_SF] = (uint8_t)((a >> 7) & 1);
        break;
    case FDI_NOP:
        break;
    case FDI_RET:
        return SIZE_MAX;
    case FDI_JMP:
        return off + (size_t)r + FD_OP_IMM(&d, 0);
    default:
        if (is_jcc_t(t)) {
            int c = tcond(x, t);
            if (c < 0) { x->err = 1; return SIZE_MAX; }
            return c ? off + (size_t)r + FD_OP_IMM(&d, 0) : off + (size_t)r;
        }
        if (tcond(x, t) >= 0 && FD_OP_TYPE(&d, 1) == FD_OT_NONE) {
            twrite(x, &d, 0, (uint64_t)tcond(x, t)); /* SETcc */
        } else if (FD_OP_TYPE(&d, 1) == FD_OT_REG) {
            a = tread(x, &d, 0, &w);
            b = tread(x, &d, 1, &w2);
            twrite(x, &d, 0, tcond(x, t) ? b : a); /* CMOVcc */
        } else {
            x->err = 1;
            return SIZE_MAX;
        }
        break;
    }
    return off + (size_t)r;
}

/* ---------------- analyzer helpers ---------------- */

static FdPathInput all_known_zero(void) {
    FdPathInput in;
    int i;
    for (i = 0; i < 16; i++) {
        in.value[i] = 0;
        in.known_mask[i] = ~0ull;
    }
    in.flags_value = 0;
    in.flags_known = FD_PATH_CF | FD_PATH_PF | FD_PATH_AF | FD_PATH_ZF |
                     FD_PATH_SF | FD_PATH_OF;
    return in;
}

static int analyze(const uint8_t *code, size_t len, const FdPathInput *in,
                   size_t limit, FdPathReport *rep) {
    memset(rep, 0, sizeof *rep);
    return fd_path_analyze(code, len, in, limit, rep);
}

static const FdPathEdge *find_edge(const FdPathReport *rep, size_t src,
                                   int taken) {
    size_t i;
    for (i = 0; i < rep->edge_count; i++)
        if (rep->edges[i].source == src && rep->edges[i].taken == taken)
            return &rep->edges[i];
    return NULL;
}

/* Validate one edge's witness by replaying it with the independent
   interpreter: entry values must satisfy the input constraints, the
   replay must arrive at the edge's source and take the recorded
   direction, consuming exactly the witness choices. */
static int check_witness(const uint8_t *code, size_t len,
                         const FdPathInput *in, const FdPathEdge *e) {
    const FdPathWitness *w = &e->witness;
    Tx x;
    size_t off;
    int i;
    if (!e->feasible) {
        for (i = 0; i < 16; i++) TASK_CHECK(w->entry[i] == 0);
        TASK_CHECK(w->entry_flags == 0);
        TASK_CHECK(w->choices == NULL);
        TASK_CHECK(w->choice_count == 0);
        return 0;
    }
    for (i = 0; i < 16; i++)
        TASK_CHECK((w->entry[i] & in->known_mask[i]) ==
                   (in->value[i] & in->known_mask[i]));
    TASK_CHECK((w->entry_flags & in->flags_known) ==
               (in->flags_value & in->flags_known));
    memset(&x, 0, sizeof x);
    memcpy(x.r, w->entry, sizeof x.r);
    for (i = 0; i < 6; i++) x.fl[i] = (w->entry_flags >> i) & 1;
    x.wit = w;
    x.ci = 0;
    off = 0;
    while (off != e->source) {
        TASK_CHECK(off < len);
        off = tstep(&x, code, len, off);
        TASK_CHECK(!x.err);
        TASK_CHECK(off != SIZE_MAX);
    }
    TASK_CHECK(x.ci == w->choice_count);
    {
        FdInstr d;
        int r = fd_decode(code + off, len - off, 64, 0, &d);
        int c;
        size_t next;
        TASK_CHECK(r > 0);
        TASK_CHECK(is_jcc_t(FD_TYPE(&d)));
        c = tcond(&x, FD_TYPE(&d));
        TASK_CHECK(c == e->taken);
        next = c ? off + (size_t)r + FD_OP_IMM(&d, 0) : off + (size_t)r;
        TASK_CHECK(next == e->destination);
    }
    return 0;
}

static int check_all_witnesses(const uint8_t *code, size_t len,
                               const FdPathInput *in,
                               const FdPathReport *rep) {
    size_t i;
    for (i = 0; i < rep->edge_count; i++)
        if (check_witness(code, len, in, &rep->edges[i])) return 1;
    return 0;
}

/* ---------------- alias group ---------------- */

/* 32-bit writes clear the upper half. */
static int case_alias_w32(void) {
    /* mov eax,1 ; cmp rax,1 ; jz +1 ; ret ; ret */
    static const uint8_t code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,       /* 0 */
        0x48, 0x83, 0xF8, 0x01,             /* 5 */
        0x74, 0x01,                         /* 9 */
        0xC3,                               /* 11 */
        0xC3                                /* 12 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = 0; /* rax fully free */
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    TASK_CHECK(rep.error_offset == SIZE_MAX);
    TASK_CHECK(rep.edge_count == 2);
    e = find_edge(&rep, 9, 1);
    TASK_CHECK(e && e->feasible && e->destination == 12);
    e = find_edge(&rep, 9, 0);
    TASK_CHECK(e && !e->feasible && e->destination == 11);
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* High-byte registers alias bits 8..15; 16-bit compares see them. */
static int case_alias_gph(void) {
    /* mov ah,1 ; cmp ax,0x100 ; jz +1 ; ret ; ret */
    static const uint8_t code[] = {
        0xB4, 0x01,                         /* 0 */
        0x66, 0x3D, 0x00, 0x01,             /* 2 */
        0x74, 0x01,                         /* 6 */
        0xC3,                               /* 8 */
        0xC3                                /* 9 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = ~0xffull; /* al free, rest known 0 */
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    TASK_CHECK(rep.edge_count == 2);
    e = find_edge(&rep, 6, 1);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK((e->witness.entry[0] & 0xff) == 0); /* al==0 on taken path */
    e = find_edge(&rep, 6, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK((e->witness.entry[0] & 0xff) != 0);
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* 16-bit writes preserve the upper bits; MOVZX reads AH and zeroes high. */
static int case_alias_w16_movzx(void) {
    /* mov ax,0 ; test rax,rax ; jz +1 ; ret ; ret */
    static const uint8_t code1[] = {
        0x66, 0xB8, 0x00, 0x00,             /* 0 */
        0x48, 0x85, 0xC0,                   /* 4 */
        0x74, 0x01,                         /* 7 */
        0xC3,                               /* 9 */
        0xC3                                /* 10 */
    };
    /* movzx eax,ah ; cmp rax,0x12 ; jz +1 ; ret ; ret */
    static const uint8_t code2[] = {
        0x0F, 0xB6, 0xC4,                   /* 0 */
        0x48, 0x83, 0xF8, 0x12,             /* 3 */
        0x74, 0x01,                         /* 7 */
        0xC3,                               /* 9 */
        0xC3                                /* 10 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    /* bits 16..31 known 0, low 16 and high 32 free */
    in.known_mask[0] = 0xffff0000ull;
    TASK_CHECK(analyze(code1, sizeof code1, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 7, 1);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK((e->witness.entry[0] >> 32) == 0);
    e = find_edge(&rep, 7, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK((e->witness.entry[0] >> 32) != 0);
    TASK_CHECK(check_all_witnesses(code1, sizeof code1, &in, &rep) == 0);
    fd_path_free(&rep);

    in = all_known_zero();
    in.value[0] = 0x1200; /* ah = 0x12 */
    TASK_CHECK(analyze(code2, sizeof code2, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 7, 1);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 7, 0);
    TASK_CHECK(e && !e->feasible);
    fd_path_free(&rep);

    in.value[0] = 0x1300;
    TASK_CHECK(analyze(code2, sizeof code2, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 7, 1);
    TASK_CHECK(e && !e->feasible);
    e = find_edge(&rep, 7, 0);
    TASK_CHECK(e && e->feasible);
    fd_path_free(&rep);
    return 0;
}

/* SETcc writes exactly one byte; CMOVcc keeps the old value when false
   and the 32-bit form still clears the top half. */
static int case_alias_setcc_cmov(void) {
    /* test rax,rax ; setz cl ; test rcx,0x100 ; jz +1 ; ret ; ret */
    static const uint8_t code1[] = {
        0x48, 0x85, 0xC0,                   /* 0 */
        0x0F, 0x94, 0xC1,                   /* 3 */
        0x48, 0xF7, 0xC1, 0x00, 0x01, 0x00, 0x00, /* 6 */
        0x74, 0x01,                         /* 13 */
        0xC3,                               /* 15 */
        0xC3                                /* 16 */
    };
    /* test rax,rax ; mov ecx,5 ; cmovz rcx,rdx ; cmp rcx,5 ; jz +1 */
    static const uint8_t code2[] = {
        0x48, 0x85, 0xC0,                   /* 0 */
        0xB9, 0x05, 0x00, 0x00, 0x00,       /* 3 */
        0x48, 0x0F, 0x44, 0xCA,             /* 8 */
        0x48, 0x83, 0xF9, 0x05,             /* 12 */
        0x74, 0x01,                         /* 16 */
        0xC3,                               /* 18 */
        0xC3                                /* 19 */
    };
    /* test rax,rax ; cmovnz ecx,edx ; movabs rbx,1<<32 ;
       test rcx,rbx ; jz +1 ; ret ; ret */
    static const uint8_t code3[] = {
        0x48, 0x85, 0xC0,                   /* 0 */
        0x0F, 0x45, 0xCA,                   /* 3 */
        0x48, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, /* 6 */
        0x48, 0x85, 0xD9,                   /* 16 */
        0x74, 0x01,                         /* 19 */
        0xC3,                               /* 21 */
        0xC3                                /* 22 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;

    /* rcx bit 8 known 1; setz cl must leave it alone */
    in.known_mask[0] = 0;      /* rax free */
    in.known_mask[1] = ~0xffull;
    in.value[1] = 0x100;
    TASK_CHECK(analyze(code1, sizeof code1, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 13, 1);
    TASK_CHECK(e && !e->feasible);
    e = find_edge(&rep, 13, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(check_all_witnesses(code1, sizeof code1, &in, &rep) == 0);
    fd_path_free(&rep);

    /* jz taken iff rax!=0 || rdx==5 */
    in = all_known_zero();
    in.known_mask[0] = 0;
    in.known_mask[2] = 0;
    TASK_CHECK(analyze(code2, sizeof code2, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 16, 1);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.entry[0] != 0 || e->witness.entry[2] == 5);
    e = find_edge(&rep, 16, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.entry[0] == 0 && e->witness.entry[2] != 5);
    TASK_CHECK(check_all_witnesses(code2, sizeof code2, &in, &rep) == 0);
    fd_path_free(&rep);

    /* 32-bit cmov clears the top half even when the move is not done */
    in = all_known_zero();
    in.known_mask[0] = 0;
    in.known_mask[1] = 0;
    in.known_mask[2] = 0;
    TASK_CHECK(analyze(code3, sizeof code3, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 19, 1);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 19, 0);
    TASK_CHECK(e && !e->feasible);
    TASK_CHECK(check_all_witnesses(code3, sizeof code3, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* ---------------- carry group ---------------- */

/* add/adc carry chain over four fully free 64-bit GPRs: not enumerable. */
static int case_carry_adc_wide(void) {
    /* add rax,rbx ; adc rcx,rdx ; jc +1 ; ret ; ret */
    static const uint8_t code[] = {
        0x48, 0x01, 0xD8,                   /* 0 */
        0x48, 0x11, 0xD1,                   /* 3 */
        0x72, 0x01,                         /* 6 */
        0xC3,                               /* 8 */
        0xC3                                /* 9 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = in.known_mask[1] = 0;
    in.known_mask[2] = in.known_mask[3] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    TASK_CHECK(rep.edge_count == 2);
    e = find_edge(&rep, 6, 1);
    TASK_CHECK(e && e->feasible);
    {
        /* entry regs: rax=0, rcx=1, rdx=2, rbx=3 */
        const FdPathWitness *w = &e->witness;
        __uint128_t s1 = (__uint128_t)w->entry[0] + w->entry[3];
        __uint128_t s2 = (__uint128_t)w->entry[1] + w->entry[2] +
                         (uint64_t)(s1 >> 64);
        TASK_CHECK((uint64_t)(s2 >> 64) == 1);
    }
    e = find_edge(&rep, 6, 0);
    TASK_CHECK(e && e->feasible);
    {
        const FdPathWitness *w = &e->witness;
        __uint128_t s1 = (__uint128_t)w->entry[0] + w->entry[3];
        __uint128_t s2 = (__uint128_t)w->entry[1] + w->entry[2] +
                         (uint64_t)(s1 >> 64);
        TASK_CHECK((uint64_t)(s2 >> 64) == 0);
    }
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* sub/sbb borrow chain over four fully free 64-bit GPRs. */
static int case_carry_sbb_wide(void) {
    /* sub rax,rbx ; sbb rcx,rdx ; jc +1 ; ret ; ret */
    static const uint8_t code[] = {
        0x48, 0x29, 0xD8,                   /* 0 */
        0x48, 0x19, 0xD1,                   /* 3 */
        0x72, 0x01,                         /* 6 */
        0xC3,                               /* 8 */
        0xC3                                /* 9 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = in.known_mask[1] = 0;
    in.known_mask[2] = in.known_mask[3] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    e = find_edge(&rep, 6, 1);
    TASK_CHECK(e && e->feasible);
    {
        /* entry regs: rax=0, rcx=1, rdx=2, rbx=3 */
        const FdPathWitness *w = &e->witness;
        uint64_t b1 = w->entry[0] < w->entry[3];
        uint64_t b2 = (__uint128_t)w->entry[1] <
                      (__uint128_t)w->entry[2] + b1;
        TASK_CHECK(b2 == 1);
    }
    e = find_edge(&rep, 6, 0);
    TASK_CHECK(e && e->feasible);
    {
        const FdPathWitness *w = &e->witness;
        uint64_t b1 = w->entry[0] < w->entry[3];
        uint64_t b2 = (__uint128_t)w->entry[1] <
                      (__uint128_t)w->entry[2] + b1;
        TASK_CHECK(b2 == 0);
    }
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* Narrow free inputs cross-checked against exhaustive enumeration. */
static int case_carry_enum(void) {
    /* add al,bl ; adc cl,dl ; jc +1 ; ret ; ret */
    static const uint8_t code[] = {
        0x00, 0xD8,                         /* 0 */
        0x10, 0xD1,                         /* 2 */
        0x72, 0x01,                         /* 4 */
        0xC3,                               /* 6 */
        0xC3                                /* 7 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    int dir_seen[2] = {0, 0};
    unsigned a, b, c, d;
    /* Low 4 bits of al/bl/cl/dl free; al and cl start at 0xF0 so the
       carry chain can go either way.  Regs: rax=0, rcx=1, rdx=2, rbx=3. */
    in.known_mask[0] = in.known_mask[1] = ~0x0full;
    in.known_mask[2] = in.known_mask[3] = ~0x0full;
    in.value[0] = 0xf0;
    in.value[1] = 0xf0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    for (a = 0; a < 16; a++)
        for (b = 0; b < 16; b++)
            for (c = 0; c < 16; c++)
                for (d = 0; d < 16; d++) {
                    Tx x;
                    size_t off = 0;
                    memset(&x, 0, sizeof x);
                    x.r[0] = 0xf0 | a; x.r[3] = b;
                    x.r[1] = 0xf0 | c; x.r[2] = d;
                    while (off != 4) {
                        off = tstep(&x, code, sizeof code, off);
                        TASK_CHECK(off != SIZE_MAX && !x.err);
                    }
                    dir_seen[tcond(&x, FDI_JC)] = 1;
                }
    TASK_CHECK(dir_seen[0] && dir_seen[1]);
    e = find_edge(&rep, 4, 1);
    TASK_CHECK(e && e->feasible == dir_seen[1]);
    e = find_edge(&rep, 4, 0);
    TASK_CHECK(e && e->feasible == dir_seen[0]);
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);

    /* Same fragment, only low 3 bits of al/bl free: carry impossible. */
    in = all_known_zero();
    in.known_mask[0] = in.known_mask[3] = ~0x07ull;
    dir_seen[0] = dir_seen[1] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    for (a = 0; a < 8; a++)
        for (b = 0; b < 8; b++) {
            Tx x;
            size_t off = 0;
            memset(&x, 0, sizeof x);
            x.r[0] = a; x.r[3] = b;
            while (off != 4) {
                off = tstep(&x, code, sizeof code, off);
                TASK_CHECK(off != SIZE_MAX && !x.err);
            }
            dir_seen[tcond(&x, FDI_JC)] = 1;
        }
    TASK_CHECK(dir_seen[0] && !dir_seen[1]);
    e = find_edge(&rep, 4, 1);
    TASK_CHECK(e && !e->feasible);
    e = find_edge(&rep, 4, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* ---------------- flags group ---------------- */

/* INC/DEC preserve CF. */
static int case_flags_cf_preserved(void) {
    /* stc ; inc rax ; jc +1 ; ret ; ret */
    static const uint8_t code1[] = {
        0xF9, 0x48, 0xFF, 0xC0, 0x72, 0x01, 0xC3, 0xC3
    };
    /* clc ; dec rax ; jc +1 ; ret ; ret */
    static const uint8_t code2[] = {
        0xF8, 0x48, 0xFF, 0xC8, 0x72, 0x01, 0xC3, 0xC3
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = 0;
    TASK_CHECK(analyze(code1, sizeof code1, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 4, 1);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 4, 0);
    TASK_CHECK(e && !e->feasible);
    TASK_CHECK(check_all_witnesses(code1, sizeof code1, &in, &rep) == 0);
    fd_path_free(&rep);

    TASK_CHECK(analyze(code2, sizeof code2, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 4, 1);
    TASK_CHECK(e && !e->feasible);
    e = find_edge(&rep, 4, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(check_all_witnesses(code2, sizeof code2, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* The free AF of one logical instruction stays correlated across uses. */
static int case_flags_af_correlated(void) {
    /* test rax,rbx ; lahf ; mov bl,ah ; lahf ; cmp ah,bl ; jz +1 */
    static const uint8_t code[] = {
        0x48, 0x85, 0xC3,                   /* 0 */
        0x9F,                               /* 3 */
        0x88, 0xE3,                         /* 4 */
        0x9F,                               /* 6 */
        0x38, 0xDC,                         /* 7 */
        0x74, 0x01,                         /* 9 */
        0xC3,                               /* 11 */
        0xC3                                /* 12 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = in.known_mask[1] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    e = find_edge(&rep, 9, 1);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 9, 0);
    TASK_CHECK(e && !e->feasible); /* both LAHFs see the same AF */
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* A free AF makes both directions reachable; the choice is recorded. */
static int case_flags_af_free(void) {
    /* and rax,rbx ; lahf ; test ah,0x10 ; jnz +1 ; ret ; ret */
    static const uint8_t code[] = {
        0x48, 0x21, 0xD8,                   /* 0 */
        0x9F,                               /* 3 */
        0xF6, 0xC4, 0x10,                   /* 4 */
        0x75, 0x01,                         /* 7 */
        0xC3,                               /* 9 */
        0xC3                                /* 10 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = in.known_mask[1] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    e = find_edge(&rep, 7, 1);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.choice_count == 2);
    TASK_CHECK(e->witness.choices[0].offset == 0 &&
               e->witness.choices[0].af == 1);
    TASK_CHECK(e->witness.choices[1].offset == 4);
    e = find_edge(&rep, 7, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.choice_count == 2);
    TASK_CHECK(e->witness.choices[0].offset == 0 &&
               e->witness.choices[0].af == 0);
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* SAHF restores five flags from AH and preserves OF. */
static int case_flags_sahf(void) {
    /* mov ah,0 ; sahf ; jo +1 ; ret ; ret */
    static const uint8_t code1[] = {
        0xB4, 0x00, 0x9E, 0x70, 0x01, 0xC3, 0xC3
    };
    /* mov ah,1 ; sahf ; jc +1 ; ret ; ret */
    static const uint8_t code2[] = {
        0xB4, 0x01, 0x9E, 0x72, 0x01, 0xC3, 0xC3
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.flags_known = FD_PATH_OF;
    in.flags_value = FD_PATH_OF;
    TASK_CHECK(analyze(code1, sizeof code1, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 3, 1);
    TASK_CHECK(e && e->feasible); /* OF survives SAHF */
    e = find_edge(&rep, 3, 0);
    TASK_CHECK(e && !e->feasible);
    TASK_CHECK(check_all_witnesses(code1, sizeof code1, &in, &rep) == 0);
    fd_path_free(&rep);

    in.flags_value = 0;
    TASK_CHECK(analyze(code1, sizeof code1, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 3, 1);
    TASK_CHECK(e && !e->feasible);
    e = find_edge(&rep, 3, 0);
    TASK_CHECK(e && e->feasible);
    fd_path_free(&rep);

    in = all_known_zero();
    TASK_CHECK(analyze(code2, sizeof code2, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 3, 1);
    TASK_CHECK(e && e->feasible); /* CF from AH bit 0 */
    e = find_edge(&rep, 3, 0);
    TASK_CHECK(e && !e->feasible);
    TASK_CHECK(check_all_witnesses(code2, sizeof code2, &in, &rep) == 0);
    fd_path_free(&rep);

    /* SF/ZF/PF come from AH bits 7/6/2 (RAX bits 15/14/10). */
    {
        /* mov ah,0x80 ; sahf ; js +1 ; ret ; ret */
        static const uint8_t c_sf[] = { 0xB4, 0x80, 0x9E, 0x78, 0x01, 0xC3, 0xC3 };
        /* mov ah,0x40 ; sahf ; jz +1 ; ret ; ret */
        static const uint8_t c_zf[] = { 0xB4, 0x40, 0x9E, 0x74, 0x01, 0xC3, 0xC3 };
        /* mov ah,0x04 ; sahf ; jp +1 ; ret ; ret */
        static const uint8_t c_pf[] = { 0xB4, 0x04, 0x9E, 0x7A, 0x01, 0xC3, 0xC3 };
        TASK_CHECK(analyze(c_sf, sizeof c_sf, &in, 1000000, &rep) ==
                   FD_PATH_OK);
        e = find_edge(&rep, 3, 1);
        TASK_CHECK(e && e->feasible);
        e = find_edge(&rep, 3, 0);
        TASK_CHECK(e && !e->feasible);
        TASK_CHECK(check_all_witnesses(c_sf, sizeof c_sf, &in, &rep) == 0);
        fd_path_free(&rep);

        TASK_CHECK(analyze(c_zf, sizeof c_zf, &in, 1000000, &rep) ==
                   FD_PATH_OK);
        e = find_edge(&rep, 3, 1);
        TASK_CHECK(e && e->feasible);
        e = find_edge(&rep, 3, 0);
        TASK_CHECK(e && !e->feasible);
        TASK_CHECK(check_all_witnesses(c_zf, sizeof c_zf, &in, &rep) == 0);
        fd_path_free(&rep);

        TASK_CHECK(analyze(c_pf, sizeof c_pf, &in, 1000000, &rep) ==
                   FD_PATH_OK);
        e = find_edge(&rep, 3, 1);
        TASK_CHECK(e && e->feasible);
        e = find_edge(&rep, 3, 0);
        TASK_CHECK(e && !e->feasible);
        TASK_CHECK(check_all_witnesses(c_pf, sizeof c_pf, &in, &rep) == 0);
        fd_path_free(&rep);
    }
    return 0;
}

/* LAHF writes SF:ZF:0:AF:0:PF:1:CF into AH. */
static int case_flags_lahf_layout(void) {
    /* stc ; lahf ; test ah,1 ; jz +1 ; ret ; ret */
    static const uint8_t code1[] = {
        0xF9, 0x9F, 0xF6, 0xC4, 0x01, 0x74, 0x01, 0xC3, 0xC3
    };
    /* mov eax,0x80 ; test al,al ; lahf ; test ah,0x80 ; jz +1 ; ret ; ret */
    static const uint8_t code2[] = {
        0xB8, 0x80, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x9F,
        0xF6, 0xC4, 0x80, 0x74, 0x01, 0xC3, 0xC3
    };
    /* xor eax,eax ; lahf ; test ah,0x40 ; jz +1 ; ret ; ret */
    static const uint8_t code3[] = {
        0x31, 0xC0, 0x9F, 0xF6, 0xC4, 0x40, 0x74, 0x01, 0xC3, 0xC3
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    TASK_CHECK(analyze(code1, sizeof code1, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 5, 1);
    TASK_CHECK(e && !e->feasible); /* AH bit 0 = CF = 1 */
    e = find_edge(&rep, 5, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(check_all_witnesses(code1, sizeof code1, &in, &rep) == 0);
    fd_path_free(&rep);

    TASK_CHECK(analyze(code2, sizeof code2, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 11, 1);
    TASK_CHECK(e && !e->feasible); /* AH bit 7 = SF = 1 */
    e = find_edge(&rep, 11, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(check_all_witnesses(code2, sizeof code2, &in, &rep) == 0);
    fd_path_free(&rep);

    TASK_CHECK(analyze(code3, sizeof code3, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 6, 1);
    TASK_CHECK(e && !e->feasible); /* AH bit 6 = ZF = 1 */
    e = find_edge(&rep, 6, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(check_all_witnesses(code3, sizeof code3, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* CLC/STC/CMC and parity. */
static int case_flags_cmc_parity(void) {
    /* stc ; cmc ; jc +1 ; ret ; ret */
    static const uint8_t code1[] = { 0xF9, 0xF5, 0x72, 0x01, 0xC3, 0xC3 };
    /* clc ; cmc ; jc +1 ; ret ; ret */
    static const uint8_t code2[] = { 0xF8, 0xF5, 0x72, 0x01, 0xC3, 0xC3 };
    /* mov al,3 ; test al,al ; jp +1 ; ret ; ret */
    static const uint8_t code3[] = {
        0xB0, 0x03, 0x84, 0xC0, 0x7A, 0x01, 0xC3, 0xC3
    };
    /* mov al,2 ; test al,al ; jp +1 ; ret ; ret */
    static const uint8_t code4[] = {
        0xB0, 0x02, 0x84, 0xC0, 0x7A, 0x01, 0xC3, 0xC3
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    TASK_CHECK(analyze(code1, sizeof code1, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 2, 1);
    TASK_CHECK(e && !e->feasible);
    e = find_edge(&rep, 2, 0);
    TASK_CHECK(e && e->feasible);
    fd_path_free(&rep);

    TASK_CHECK(analyze(code2, sizeof code2, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 2, 1);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 2, 0);
    TASK_CHECK(e && !e->feasible);
    fd_path_free(&rep);

    TASK_CHECK(analyze(code3, sizeof code3, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 4, 1);
    TASK_CHECK(e && e->feasible); /* parity(3) is even */
    e = find_edge(&rep, 4, 0);
    TASK_CHECK(e && !e->feasible);
    TASK_CHECK(check_all_witnesses(code3, sizeof code3, &in, &rep) == 0);
    fd_path_free(&rep);

    TASK_CHECK(analyze(code4, sizeof code4, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 4, 1);
    TASK_CHECK(e && !e->feasible); /* parity(2) is odd */
    e = find_edge(&rep, 4, 0);
    TASK_CHECK(e && e->feasible);
    fd_path_free(&rep);
    return 0;
}

/* ---------------- joins group ---------------- */

/* Diamond: the merge keeps which predecessor wrote what. */
static int case_joins_diamond(void) {
    /* cmp rax,0 ; jz L1 ; mov ecx,1 ; jmp L2 ; L1: mov ecx,2 ;
       L2: cmp rcx,1 ; jz L3 ; ret ; L3: ret */
    static const uint8_t code[] = {
        0x48, 0x83, 0xF8, 0x00,             /* 0 */
        0x74, 0x07,                         /* 4  -> 13 */
        0xB9, 0x01, 0x00, 0x00, 0x00,       /* 6 */
        0xEB, 0x05,                         /* 11 -> 18 */
        0xB9, 0x02, 0x00, 0x00, 0x00,       /* 13 */
        0x48, 0x83, 0xF9, 0x01,             /* 18 */
        0x74, 0x01,                         /* 22 -> 25 */
        0xC3,                               /* 24 */
        0xC3                                /* 25 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    TASK_CHECK(rep.edge_count == 4);
    /* edges sorted by source then taken */
    TASK_CHECK(rep.edges[0].source == 4 && rep.edges[0].taken == 0 &&
               rep.edges[0].destination == 6);
    TASK_CHECK(rep.edges[1].source == 4 && rep.edges[1].taken == 1 &&
               rep.edges[1].destination == 13);
    TASK_CHECK(rep.edges[2].source == 22 && rep.edges[2].taken == 0 &&
               rep.edges[2].destination == 24);
    TASK_CHECK(rep.edges[3].source == 22 && rep.edges[3].taken == 1 &&
               rep.edges[3].destination == 25);
    e = find_edge(&rep, 22, 1);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.entry[0] != 0); /* rcx==1 came from rax!=0 */
    e = find_edge(&rep, 22, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.entry[0] == 0); /* rcx==2 came from rax==0 */
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* The merged relation rules out combinations neither predecessor had. */
static int case_joins_relation(void) {
    /* test rax,rax ; jz L1 ; mov ecx,1 ; jmp L2 ; L1: mov ecx,2 ;
       L2: cmp rcx,2 ; jnz L3 ; test rax,rax ; jnz L4 ; L3: ret ; ret ;
       L4: ret */
    static const uint8_t code[] = {
        0x48, 0x85, 0xC0,                   /* 0 */
        0x74, 0x07,                         /* 3  -> 12 */
        0xB9, 0x01, 0x00, 0x00, 0x00,       /* 5 */
        0xEB, 0x05,                         /* 10 -> 17 */
        0xB9, 0x02, 0x00, 0x00, 0x00,       /* 12 */
        0x48, 0x83, 0xF9, 0x02,             /* 17 */
        0x75, 0x05,                         /* 21 -> 28 */
        0x48, 0x85, 0xC0,                   /* 23 */
        0x75, 0x02,                         /* 26 -> 30 */
        0xC3,                               /* 28 */
        0xC3,                               /* 29 */
        0xC3                                /* 30 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    TASK_CHECK(rep.edge_count == 6);
    e = find_edge(&rep, 3, 0);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 3, 1);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 21, 0);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 21, 1);
    TASK_CHECK(e && e->feasible);
    /* At 26 only reachable with rcx==2, which implies rax==0, so the
       jnz-taken edge is infeasible even though rax is free at entry. */
    e = find_edge(&rep, 26, 1);
    TASK_CHECK(e && !e->feasible);
    e = find_edge(&rep, 26, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.entry[0] == 0);
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* Conditional jumps in unreachable blocks keep two infeasible edges. */
static int case_joins_unreachable(void) {
    /* jmp L ; jz +0 ; ret ; L: ret */
    static const uint8_t code[] = {
        0xEB, 0x03,                         /* 0 -> 5 */
        0x74, 0x00,                         /* 2 -> 4 (unreachable) */
        0xC3,                               /* 4 */
        0xC3                                /* 5 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    TASK_CHECK(rep.edge_count == 2);
    e = find_edge(&rep, 2, 0);
    TASK_CHECK(e && !e->feasible && e->destination == 4);
    e = find_edge(&rep, 2, 1);
    TASK_CHECK(e && !e->feasible && e->destination == 4);
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* ---------------- witness group ---------------- */

/* Replay every feasible edge of a fragment with AF choices. */
static int case_witness_replay_af(void) {
    /* xor rax,rbx ; lahf ; test ah,0x10 ; jnz +1 ; ret ; ret */
    static const uint8_t code[] = {
        0x48, 0x31, 0xD8,                   /* 0 */
        0x9F,                               /* 3 */
        0xF6, 0xC4, 0x10,                   /* 4 */
        0x75, 0x01,                         /* 7 */
        0xC3,                               /* 9 */
        0xC3                                /* 10 */
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = in.known_mask[1] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    e = find_edge(&rep, 7, 1);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.choices[0].offset == 0 &&
               e->witness.choices[0].af == 1);
    e = find_edge(&rep, 7, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.choice_count == 2);
    TASK_CHECK(e->witness.choices[0].offset == 0 &&
               e->witness.choices[0].af == 0);
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* Witnesses for a diamond with known-mask constraints on entry. */
static int case_witness_diamond(void) {
    /* cmp rax,0 ; jz L1 ; mov ecx,1 ; jmp L2 ; L1: mov ecx,2 ;
       L2: cmp rcx,1 ; jz L3 ; ret ; L3: ret */
    static const uint8_t code[] = {
        0x48, 0x83, 0xF8, 0x00,
        0x74, 0x07,
        0xB9, 0x01, 0x00, 0x00, 0x00,
        0xEB, 0x05,
        0xB9, 0x02, 0x00, 0x00, 0x00,
        0x48, 0x83, 0xF9, 0x01,
        0x74, 0x01,
        0xC3,
        0xC3
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    size_t i;
    in.known_mask[0] = 0;             /* rax free */
    in.known_mask[3] = 0xffffffffull; /* rdx: low 32 known, high free */
    in.value[3] = 0x12345678ull;
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    TASK_CHECK(rep.edge_count == 4);
    for (i = 0; i < rep.edge_count; i++) {
        const FdPathEdge *e = &rep.edges[i];
        TASK_CHECK(e->feasible);
        TASK_CHECK((e->witness.entry[3] & 0xffffffffull) == 0x12345678ull);
        TASK_CHECK(e->witness.choice_count == 0);
        TASK_CHECK(e->witness.choices == NULL);
    }
    TASK_CHECK(check_all_witnesses(code, sizeof code, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* Free entry flags: witnesses must pick flag values that reach the edge. */
static int case_witness_entry_flags(void) {
    /* jo +1 ; ret ; ret */
    static const uint8_t code1[] = { 0x70, 0x01, 0xC3, 0xC3 };
    /* jnp +1 ; ret ; ret */
    static const uint8_t code2[] = { 0x7B, 0x01, 0xC3, 0xC3 };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.flags_known = 0; /* all six flags free */
    TASK_CHECK(analyze(code1, sizeof code1, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 0, 1);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.entry_flags & FD_PATH_OF);
    e = find_edge(&rep, 0, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(!(e->witness.entry_flags & FD_PATH_OF));
    TASK_CHECK(check_all_witnesses(code1, sizeof code1, &in, &rep) == 0);
    fd_path_free(&rep);

    TASK_CHECK(analyze(code2, sizeof code2, &in, 1000000, &rep) ==
               FD_PATH_OK);
    e = find_edge(&rep, 0, 1);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(!(e->witness.entry_flags & FD_PATH_PF));
    e = find_edge(&rep, 0, 0);
    TASK_CHECK(e && e->feasible);
    TASK_CHECK(e->witness.entry_flags & FD_PATH_PF);
    TASK_CHECK(check_all_witnesses(code2, sizeof code2, &in, &rep) == 0);
    fd_path_free(&rep);
    return 0;
}

/* ---------------- limits group ---------------- */

/* The node budget is explicit: exhaustion is LIMIT, not infeasibility. */
static int case_limits_budget(void) {
    /* add rax,rbx ; adc rcx,rdx ; jc +1 ; ret ; ret */
    static const uint8_t code[] = {
        0x48, 0x01, 0xD8, 0x48, 0x11, 0xD1, 0x72, 0x01, 0xC3, 0xC3
    };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    const FdPathEdge *e;
    in.known_mask[0] = in.known_mask[1] = 0;
    in.known_mask[2] = in.known_mask[3] = 0;
    TASK_CHECK(analyze(code, sizeof code, &in, 8, &rep) == FD_PATH_LIMIT);
    TASK_CHECK(rep.edges == NULL);
    TASK_CHECK(rep.edge_count == 0);
    TASK_CHECK(rep.error_offset == SIZE_MAX);
    TASK_CHECK(analyze(code, sizeof code, &in, 300, &rep) == FD_PATH_LIMIT);
    TASK_CHECK(rep.edges == NULL && rep.edge_count == 0);
    /* With enough budget the same fragment is fine and both edges are
       feasible: the small-limit result was not a proof of infeasibility. */
    TASK_CHECK(analyze(code, sizeof code, &in, 1000000, &rep) == FD_PATH_OK);
    e = find_edge(&rep, 6, 1);
    TASK_CHECK(e && e->feasible);
    e = find_edge(&rep, 6, 0);
    TASK_CHECK(e && e->feasible);
    fd_path_free(&rep);
    return 0;
}

/* Unsupported constructs, including unreachable ones. */
static int case_limits_unsupported(void) {
    static const uint8_t mem[] = { 0x48, 0x03, 0x03, 0xC3 }; /* add rax,[rbx] */
    static const uint8_t call[] = { 0xE8, 0, 0, 0, 0, 0xC3 };
    static const uint8_t loop[] = { 0xEB, 0xFE };            /* jmp $ */
    static const uint8_t outjmp[] = { 0xEB, 0x7F };          /* jmp +127 */
    static const uint8_t indjmp[] = { 0xFF, 0xE0, 0xC3 };    /* jmp rax */
    static const uint8_t retimm[] = { 0xC2, 0x08, 0x00 };    /* ret 8 */
    static const uint8_t ud[] = { 0x0F, 0x0B, 0xC3 };        /* ud2 */
    static const uint8_t falloff[] = { 0x90 };               /* nop, no ret */
    /* jmp +2 skips an unreachable ud2; still unsupported */
    static const uint8_t unreach[] = { 0xEB, 0x02, 0x0F, 0x0B, 0xC3 };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    TASK_CHECK(analyze(mem, sizeof mem, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(rep.edges == NULL && rep.edge_count == 0);
    TASK_CHECK(analyze(call, sizeof call, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(loop, sizeof loop, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(outjmp, sizeof outjmp, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(indjmp, sizeof indjmp, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(retimm, sizeof retimm, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(ud, sizeof ud, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(falloff, sizeof falloff, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(unreach, sizeof unreach, &in, 1000, &rep) ==
               FD_PATH_UNSUPPORTED);
    TASK_CHECK(rep.error_offset == 2);
    return 0;
}

/* Bad bytes, non-boundary targets and bad arguments. */
static int case_limits_invalid(void) {
    static const uint8_t bad1[] = { 0xC4 };       /* truncated prefix */
    static const uint8_t bad2[] = { 0x48 };       /* lone REX */
    /* jmp +1 lands in the middle of the two-byte nop at 2 */
    static const uint8_t mid[] = { 0xEB, 0x01, 0x66, 0x90, 0xC3 };
    static const uint8_t ok[] = { 0xC3 };
    FdPathInput in = all_known_zero();
    FdPathReport rep;
    TASK_CHECK(analyze(bad1, sizeof bad1, &in, 1000, &rep) == FD_PATH_DECODE);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(bad2, sizeof bad2, &in, 1000, &rep) == FD_PATH_DECODE);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(mid, sizeof mid, &in, 1000, &rep) == FD_PATH_INVALID);
    TASK_CHECK(rep.error_offset == 0);
    TASK_CHECK(analyze(ok, sizeof ok, &in, 0, &rep) == FD_PATH_INVALID);
    TASK_CHECK(analyze(ok, 0, &in, 1000, &rep) == FD_PATH_INVALID);
    TASK_CHECK(analyze(NULL, sizeof ok, &in, 1000, &rep) == FD_PATH_INVALID);
    TASK_CHECK(analyze(ok, sizeof ok, NULL, 1000, &rep) == FD_PATH_INVALID);
    /* a report that was not zeroed/released is rejected */
    memset(&rep, 0, sizeof rep);
    rep.edge_count = 1;
    TASK_CHECK(fd_path_analyze(ok, sizeof ok, &in, 1000, &rep) ==
               FD_PATH_INVALID);
    /* a plain RET fragment analyzes cleanly with zero edges */
    TASK_CHECK(analyze(ok, sizeof ok, &in, 1000, &rep) == FD_PATH_OK);
    TASK_CHECK(rep.edge_count == 0);
    TASK_CHECK(rep.error_offset == SIZE_MAX);
    fd_path_free(&rep);
    return 0;
}

/* ---------------- registration ---------------- */

const struct task_case task_cases[] = {
    { "alias", "w32_clears_high", case_alias_w32 },
    { "alias", "gph_views", case_alias_gph },
    { "alias", "w16_and_movzx", case_alias_w16_movzx },
    { "alias", "setcc_cmov", case_alias_setcc_cmov },
    { "carry", "adc_chain_wide", case_carry_adc_wide },
    { "carry", "sbb_chain_wide", case_carry_sbb_wide },
    { "carry", "enum_reference", case_carry_enum },
    { "flags", "cf_preserved", case_flags_cf_preserved },
    { "flags", "af_correlated", case_flags_af_correlated },
    { "flags", "af_free_choice", case_flags_af_free },
    { "flags", "sahf_roundtrip", case_flags_sahf },
    { "flags", "lahf_layout", case_flags_lahf_layout },
    { "flags", "cmc_parity", case_flags_cmc_parity },
    { "joins", "diamond", case_joins_diamond },
    { "joins", "relation", case_joins_relation },
    { "joins", "unreachable", case_joins_unreachable },
    { "witness", "replay_af", case_witness_replay_af },
    { "witness", "diamond_masks", case_witness_diamond },
    { "witness", "entry_flags", case_witness_entry_flags },
    { "limits", "budget", case_limits_budget },
    { "limits", "unsupported", case_limits_unsupported },
    { "limits", "invalid", case_limits_invalid },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
