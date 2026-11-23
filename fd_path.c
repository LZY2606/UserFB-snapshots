/* fd_path.c - Static branch feasibility analysis for acyclic x86-64
   register-only code fragments.

   The fragment is executed symbolically over a shared BDD (reduced ordered
   binary decision diagram) representation.  Every unknown entry register
   bit, every unknown entry flag and every undefined-AF logical instruction
   gets one Boolean variable; register/flag state per program point is a
   vector of BDDs over those variables, and each conditional edge feasibility
   question is a BDD satisfiability check.  Witnesses are concrete satisfying
   assignments, replayed through a concrete interpreter to attach the
   per-instruction undefined-AF choices in path order. */

#include "fd_path.h"

#include <stdlib.h>
#include <string.h>

/* Flag slots, matching the FD_PATH_* bit positions. */
enum { F_CF = 0, F_PF = 1, F_AF = 2, F_ZF = 3, F_SF = 4, F_OF = 5 };

typedef uint32_t B;
enum { BNONE = 0, BONE = 1 };   /* the two constant nodes */
#define BFAIL UINT32_MAX        /* propagation sentinel for budget/oom */
#define VNONE UINT32_MAX        /* "no variable" marker */

/* ------------------------------------------------------------------ */
/* BDD manager: unique table + ITE cache, no garbage collection.      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t *var, *lo, *hi;    /* node arrays; 0=false, 1=true */
    uint32_t nnodes, cap;
    uint32_t *utab;             /* unique table, id+1 or 0 */
    uint32_t ucap;
    uint32_t *cf, *cg, *ch, *cv;/* ITE cache, cf==UINT32_MAX marks empty */
    uint32_t ccap, ccount;
    uint64_t limit;             /* max allocated nodes excluding constants */
    int failed;                 /* 0 ok, 1 budget, 2 allocation */
} Bdd;

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

static void bdd_free(Bdd *m) {
    free(m->var); free(m->lo); free(m->hi); free(m->utab);
    free(m->cf); free(m->cg); free(m->ch); free(m->cv);
    memset(m, 0, sizeof *m);
}

static int bdd_init(Bdd *m, uint64_t limit) {
    memset(m, 0, sizeof *m);
    m->limit = limit;
    m->cap = 256;
    m->nnodes = 2;
    m->var = malloc(m->cap * sizeof(uint32_t));
    m->lo = malloc(m->cap * sizeof(uint32_t));
    m->hi = malloc(m->cap * sizeof(uint32_t));
    m->ucap = 1024;
    m->utab = calloc(m->ucap, sizeof(uint32_t));
    m->ccap = 4096;
    m->cf = malloc(m->ccap * sizeof(uint32_t));
    m->cg = malloc(m->ccap * sizeof(uint32_t));
    m->ch = malloc(m->ccap * sizeof(uint32_t));
    m->cv = malloc(m->ccap * sizeof(uint32_t));
    if (!m->var || !m->lo || !m->hi || !m->utab ||
        !m->cf || !m->cg || !m->ch || !m->cv) {
        bdd_free(m);
        return -1;
    }
    m->var[0] = m->var[1] = UINT32_MAX;
    m->lo[0] = m->hi[0] = m->lo[1] = m->hi[1] = 0;
    memset(m->cf, 0xff, m->ccap * sizeof(uint32_t));
    return 0;
}

static int bdd_grow_nodes(Bdd *m) {
    uint32_t ncap = m->cap * 2;
    uint32_t *nv = realloc(m->var, ncap * sizeof(uint32_t));
    if (!nv) return -1;
    m->var = nv;
    nv = realloc(m->lo, ncap * sizeof(uint32_t));
    if (!nv) return -1;
    m->lo = nv;
    nv = realloc(m->hi, ncap * sizeof(uint32_t));
    if (!nv) return -1;
    m->hi = nv;
    m->cap = ncap;
    return 0;
}

static int bdd_grow_utab(Bdd *m) {
    uint32_t ncap = m->ucap * 2;
    uint32_t *nt = calloc(ncap, sizeof(uint32_t));
    uint32_t i;
    if (!nt) return -1;
    for (i = 0; i < m->ucap; i++) {
        if (m->utab[i]) {
            uint32_t id = m->utab[i] - 1;
            uint64_t key = mix64(m->var[id] ^ ((uint64_t)m->lo[id] << 21) ^
                                 ((uint64_t)m->hi[id] << 42));
            uint32_t p = key & (ncap - 1);
            while (nt[p]) p = (p + 1) & (ncap - 1);
            nt[p] = id + 1;
        }
    }
    free(m->utab);
    m->utab = nt;
    m->ucap = ncap;
    return 0;
}

static B bdd_mk(Bdd *m, uint32_t v, B l, B h) {
    uint64_t key;
    uint32_t p, id;
    if (l == h) return l;
    if (m->failed) return BFAIL;
    /* Budget: allocated nodes excluding the two constants. */
    if ((uint64_t)m->nnodes - 2 >= m->limit) { m->failed = 1; return BFAIL; }
    if (m->nnodes == m->cap && bdd_grow_nodes(m)) { m->failed = 2; return BFAIL; }
    if ((uint64_t)m->nnodes * 4 >= (uint64_t)m->ucap * 3 &&
        bdd_grow_utab(m)) { m->failed = 2; return BFAIL; }
    key = mix64(v ^ ((uint64_t)l << 21) ^ ((uint64_t)h << 42));
    p = key & (m->ucap - 1);
    while (m->utab[p]) {
        id = m->utab[p] - 1;
        if (m->var[id] == v && m->lo[id] == l && m->hi[id] == h) return id;
        p = (p + 1) & (m->ucap - 1);
    }
    id = m->nnodes++;
    m->var[id] = v; m->lo[id] = l; m->hi[id] = h;
    m->utab[p] = id + 1;
    return id;
}

static B bdd_var(Bdd *m, uint32_t v) { return bdd_mk(m, v, BNONE, BONE); }

static int bdd_grow_cache(Bdd *m) {
    uint32_t ncap = m->ccap * 2;
    uint32_t *nf = malloc(ncap * sizeof(uint32_t));
    uint32_t *ng = malloc(ncap * sizeof(uint32_t));
    uint32_t *nh = malloc(ncap * sizeof(uint32_t));
    uint32_t *nv = malloc(ncap * sizeof(uint32_t));
    uint32_t i;
    if (!nf || !ng || !nh || !nv) {
        free(nf); free(ng); free(nh); free(nv);
        return -1;
    }
    memset(nf, 0xff, ncap * sizeof(uint32_t));
    for (i = 0; i < m->ccap; i++) {
        if (m->cf[i] != UINT32_MAX) {
            uint64_t key = mix64(m->cf[i] ^ ((uint64_t)m->cg[i] << 21) ^
                                 ((uint64_t)m->ch[i] << 42));
            uint32_t p = key & (ncap - 1);
            while (nf[p] != UINT32_MAX) p = (p + 1) & (ncap - 1);
            nf[p] = m->cf[i]; ng[p] = m->cg[i];
            nh[p] = m->ch[i]; nv[p] = m->cv[i];
        }
    }
    free(m->cf); free(m->cg); free(m->ch); free(m->cv);
    m->cf = nf; m->cg = ng; m->ch = nh; m->cv = nv;
    m->ccap = ncap;
    return 0;
}

static B bdd_ite(Bdd *m, B f, B g, B h) {
    uint32_t v, p;
    uint64_t key;
    B fl, fh, gl, gh, hl, hh, r0, r1, r;
    if (f == BFAIL || g == BFAIL || h == BFAIL || m->failed) return BFAIL;
    if (f <= BONE) return f ? g : h;
    if (g == h) return g;
    if (g == BONE && h == BNONE) return f;
    key = mix64(f ^ ((uint64_t)g << 21) ^ ((uint64_t)h << 42));
    p = key & (m->ccap - 1);
    while (m->cf[p] != UINT32_MAX) {
        if (m->cf[p] == f && m->cg[p] == g && m->ch[p] == h) return m->cv[p];
        p = (p + 1) & (m->ccap - 1);
    }
    v = m->var[f];
    if (g > BONE && m->var[g] < v) v = m->var[g];
    if (h > BONE && m->var[h] < v) v = m->var[h];
    fl = (m->var[f] == v) ? m->lo[f] : f;
    fh = (m->var[f] == v) ? m->hi[f] : f;
    gl = (g > BONE && m->var[g] == v) ? m->lo[g] : g;
    gh = (g > BONE && m->var[g] == v) ? m->hi[g] : g;
    hl = (h > BONE && m->var[h] == v) ? m->lo[h] : h;
    hh = (h > BONE && m->var[h] == v) ? m->hi[h] : h;
    r0 = bdd_ite(m, fl, gl, hl);
    r1 = bdd_ite(m, fh, gh, hh);
    r = (r0 == BFAIL || r1 == BFAIL) ? BFAIL : bdd_mk(m, v, r0, r1);
    if (r == BFAIL) return BFAIL;
    if (m->ccount * 4 >= m->ccap * 3 && bdd_grow_cache(m)) {
        m->failed = 2;
        return BFAIL;
    }
    p = key & (m->ccap - 1);
    while (m->cf[p] != UINT32_MAX) p = (p + 1) & (m->ccap - 1);
    m->cf[p] = f; m->cg[p] = g; m->ch[p] = h; m->cv[p] = r;
    m->ccount++;
    return r;
}

static B bdd_not(Bdd *m, B f) { return bdd_ite(m, f, BNONE, BONE); }
static B bdd_and(Bdd *m, B f, B g) { return bdd_ite(m, f, g, BNONE); }
static B bdd_or(Bdd *m, B f, B g) { return bdd_ite(m, f, BONE, g); }
static B bdd_xor(Bdd *m, B f, B g) {
    B ng = bdd_not(m, g);
    return bdd_ite(m, f, ng, g);
}

/* Find one satisfying assignment; asn indexed by variable.  f must be
   satisfiable (any non-zero BDD is). */
static void bdd_sat(const Bdd *m, B f, uint8_t *asn) {
    while (f > BONE) {
        uint32_t v = m->var[f];
        if (m->lo[f] != BNONE) { asn[v] = 0; f = m->lo[f]; }
        else { asn[v] = 1; f = m->hi[f]; }
    }
}

/* ------------------------------------------------------------------ */
/* Fragment representation                                            */
/* ------------------------------------------------------------------ */

enum { K_FALL = 0, K_JMP = 1, K_JCC = 2, K_RET = 3 };

typedef struct {
    FdInstr in;
    uint32_t off;
    uint8_t kind;
    int8_t cond;        /* Jcc/SETcc/CMOVcc condition id 0..15, else -1 */
    uint32_t tgt, fall; /* successor instruction indices */
    uint32_t afvar;     /* undefined-AF variable for TEST/AND/OR/XOR */
    B ccbdd;            /* Jcc: condition BDD at this site */
    B e_taken, e_fall;  /* Jcc: per-edge reachability BDDs */
} Insn;

typedef struct {
    B r[16][64];
    B f[6];
    B pc;
} State;

typedef struct {
    Bdd bdd;
    Insn *ins;
    size_t n;
    int32_t *off2idx;
    State *acc;         /* merged input state per instruction */
    uint32_t *topo;
    uint32_t regvar[16][64];
    uint32_t flvar[6];
    uint32_t nvars;
    const FdPathInput *input;
    size_t err;
} Eng;

/* Condition ids follow the architectural tttn order. */
static int jcc_cc(uint16_t t) {
    switch (t) {
    case FDI_JO: return 0;  case FDI_JNO: return 1;
    case FDI_JC: return 2;  case FDI_JNC: return 3;
    case FDI_JZ: return 4;  case FDI_JNZ: return 5;
    case FDI_JBE: return 6; case FDI_JA: return 7;
    case FDI_JS: return 8;  case FDI_JNS: return 9;
    case FDI_JP: return 10; case FDI_JNP: return 11;
    case FDI_JL: return 12; case FDI_JGE: return 13;
    case FDI_JLE: return 14; case FDI_JG: return 15;
    default: return -1;
    }
}

static int setcc_cc(uint16_t t) {
    switch (t) {
    case FDI_SETO: return 0;  case FDI_SETNO: return 1;
    case FDI_SETC: return 2;  case FDI_SETNC: return 3;
    case FDI_SETZ: return 4;  case FDI_SETNZ: return 5;
    case FDI_SETBE: return 6; case FDI_SETA: return 7;
    case FDI_SETS: return 8;  case FDI_SETNS: return 9;
    case FDI_SETP: return 10; case FDI_SETNP: return 11;
    case FDI_SETL: return 12; case FDI_SETGE: return 13;
    case FDI_SETLE: return 14; case FDI_SETG: return 15;
    default: return -1;
    }
}

static int cmovcc_cc(uint16_t t) {
    switch (t) {
    case FDI_CMOVO: return 0;  case FDI_CMOVNO: return 1;
    case FDI_CMOVC: return 2;  case FDI_CMOVNC: return 3;
    case FDI_CMOVZ: return 4;  case FDI_CMOVNZ: return 5;
    case FDI_CMOVBE: return 6; case FDI_CMOVA: return 7;
    case FDI_CMOVS: return 8;  case FDI_CMOVNS: return 9;
    case FDI_CMOVP: return 10; case FDI_CMOVNP: return 11;
    case FDI_CMOVL: return 12; case FDI_CMOVGE: return 13;
    case FDI_CMOVLE: return 14; case FDI_CMOVG: return 15;
    default: return -1;
    }
}

static int is_logic(uint16_t t) {
    return t == FDI_TEST || t == FDI_AND || t == FDI_OR || t == FDI_XOR;
}

/* Check whether the decoded instruction is inside the supported
   register-only subset, and classify its control-flow kind. */
static int classify(Insn *ins) {
    const FdInstr *in = &ins->in;
    uint16_t t = FD_TYPE(in);
    FdOpType o0 = FD_OP_TYPE(in, 0), o1 = FD_OP_TYPE(in, 1);
    FdOpType o2 = FD_OP_TYPE(in, 2), o3 = FD_OP_TYPE(in, 3);
    int cc;
    if (o0 == FD_OT_MEM || o0 == FD_OT_MEMBCST || o1 == FD_OT_MEM ||
        o1 == FD_OT_MEMBCST || o2 == FD_OT_MEM || o2 == FD_OT_MEMBCST ||
        o3 == FD_OT_MEM || o3 == FD_OT_MEMBCST)
        return 0;
    if ((cc = jcc_cc(t)) >= 0) {
        if (o0 != FD_OT_OFF || o1 != FD_OT_NONE) return 0;
        ins->kind = K_JCC; ins->cond = (int8_t)cc;
        return 1;
    }
    if ((cc = setcc_cc(t)) >= 0) {
        if (o0 != FD_OT_REG || o1 != FD_OT_NONE) return 0;
        ins->cond = (int8_t)cc;
        return 1;
    }
    if ((cc = cmovcc_cc(t)) >= 0) {
        if (o0 != FD_OT_REG || o1 != FD_OT_REG || o2 != FD_OT_NONE) return 0;
        ins->cond = (int8_t)cc;
        return 1;
    }
    switch (t) {
    case FDI_MOV: case FDI_MOVABS:
        return o0 == FD_OT_REG && (o1 == FD_OT_REG || o1 == FD_OT_IMM) &&
               o2 == FD_OT_NONE;
    case FDI_MOVZX:
        return o0 == FD_OT_REG && o1 == FD_OT_REG && o2 == FD_OT_NONE;
    case FDI_ADD: case FDI_ADC: case FDI_SUB: case FDI_SBB: case FDI_CMP:
    case FDI_TEST: case FDI_AND: case FDI_OR: case FDI_XOR:
        return o0 == FD_OT_REG && (o1 == FD_OT_REG || o1 == FD_OT_IMM) &&
               o2 == FD_OT_NONE;
    case FDI_INC: case FDI_DEC:
        return o0 == FD_OT_REG && o1 == FD_OT_NONE;
    case FDI_CLC: case FDI_STC: case FDI_CMC:
    case FDI_LAHF: case FDI_SAHF:
        return o0 == FD_OT_NONE;
    case FDI_NOP:
        return 1; /* memory forms already rejected above */
    case FDI_JMP:
        if (o0 != FD_OT_OFF || o1 != FD_OT_NONE) return 0;
        ins->kind = K_JMP;
        return 1;
    case FDI_RET:
        if (o0 != FD_OT_NONE) return 0;
        ins->kind = K_RET;
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Symbolic execution                                                 */
/* ------------------------------------------------------------------ */

static void sread(const FdInstr *in, int op, const State *st, B out[64],
                  int *pw) {
    int i;
    if (FD_OP_TYPE(in, op) == FD_OT_IMM) {
        uint64_t v = (uint64_t)FD_OP_IMM(in, op);
        int w = FD_OP_SIZE(in, 0) * 8;
        for (i = 0; i < 64; i++)
            out[i] = (i < w && ((v >> i) & 1)) ? BONE : BNONE;
        *pw = w;
    } else {
        int idx = FD_OP_REG(in, op);
        if (FD_OP_REG_TYPE(in, op) == FD_RT_GPH) {
            for (i = 0; i < 8; i++) out[i] = st->r[idx - 4][8 + i];
            *pw = 8;
        } else {
            int w = FD_OP_SIZE(in, op) * 8;
            for (i = 0; i < w; i++) out[i] = st->r[idx][i];
            *pw = w;
        }
    }
}

static void swrite(const FdInstr *in, int op, State *st, const B *bits) {
    int idx = FD_OP_REG(in, op), i;
    int sz = FD_OP_SIZE(in, op);
    if (FD_OP_REG_TYPE(in, op) == FD_RT_GPH) {
        for (i = 0; i < 8; i++) st->r[idx - 4][8 + i] = bits[i];
        return;
    }
    if (sz == 8) {
        for (i = 0; i < 64; i++) st->r[idx][i] = bits[i];
    } else if (sz == 4) {
        for (i = 0; i < 32; i++) st->r[idx][i] = bits[i];
        for (; i < 64; i++) st->r[idx][i] = BNONE;
    } else {
        for (i = 0; i < sz * 8; i++) st->r[idx][i] = bits[i];
    }
}

/* Ripple-carry adder over BDDs: s = a + b + cin at width w, carries in c. */
static void sadder(Bdd *m, const B *a, const B *b, B cin, int w, B *s, B *c) {
    int i;
    c[0] = cin;
    for (i = 0; i < w; i++) {
        B axb = bdd_xor(m, a[i], b[i]);
        s[i] = bdd_xor(m, axb, c[i]);
        c[i + 1] = bdd_or(m, bdd_and(m, a[i], b[i]), bdd_and(m, axb, c[i]));
    }
}

static void sset_szp(Bdd *m, State *st, const B *s, int w) {
    B z = BNONE, p = BNONE;
    int i;
    for (i = 0; i < w; i++) z = bdd_or(m, z, s[i]);
    st->f[F_ZF] = bdd_not(m, z);
    st->f[F_SF] = s[w - 1];
    for (i = 0; i < 8; i++) p = bdd_xor(m, p, s[i]);
    st->f[F_PF] = bdd_not(m, p);
}

static B scond(Bdd *m, const State *st, int cc) {
    B cf = st->f[F_CF], pf = st->f[F_PF], zf = st->f[F_ZF];
    B sf = st->f[F_SF], of = st->f[F_OF];
    switch (cc) {
    case 0: return of;
    case 1: return bdd_not(m, of);
    case 2: return cf;
    case 3: return bdd_not(m, cf);
    case 4: return zf;
    case 5: return bdd_not(m, zf);
    case 6: return bdd_or(m, cf, zf);
    case 7: return bdd_not(m, bdd_or(m, cf, zf));
    case 8: return sf;
    case 9: return bdd_not(m, sf);
    case 10: return pf;
    case 11: return bdd_not(m, pf);
    case 12: return bdd_xor(m, sf, of);
    case 13: return bdd_not(m, bdd_xor(m, sf, of));
    case 14: return bdd_or(m, zf, bdd_xor(m, sf, of));
    case 15: return bdd_not(m, bdd_or(m, zf, bdd_xor(m, sf, of)));
    default: return BNONE;
    }
}

/* Execute one instruction symbolically on its merged input state. */
static void sym_exec(Eng *e, uint32_t idx) {
    Bdd *m = &e->bdd;
    Insn *ins = &e->ins[idx];
    State *st = &e->acc[idx];
    const FdInstr *in = &ins->in;
    uint16_t t = FD_TYPE(in);
    B a[64], b[64], s[64], c[65];
    int w = 0, w2 = 0, i;
    switch (t) {
    case FDI_MOV: case FDI_MOVABS:
        sread(in, 1, st, a, &w);
        swrite(in, 0, st, a);
        break;
    case FDI_MOVZX:
        sread(in, 1, st, a, &w);
        for (i = w; i < 64; i++) a[i] = BNONE;
        swrite(in, 0, st, a);
        break;
    case FDI_ADD: case FDI_ADC: case FDI_SUB: case FDI_SBB: case FDI_CMP: {
        int sub = (t == FDI_SUB || t == FDI_SBB || t == FDI_CMP);
        B c0;
        sread(in, 0, st, a, &w);
        sread(in, 1, st, b, &w2);
        if (sub) {
            for (i = 0; i < w; i++) b[i] = bdd_not(m, b[i]);
            c0 = (t == FDI_SBB) ? bdd_not(m, st->f[F_CF]) : BONE;
        } else {
            c0 = (t == FDI_ADC) ? st->f[F_CF] : BNONE;
        }
        sadder(m, a, b, c0, w, s, c);
        st->f[F_CF] = sub ? bdd_not(m, c[w]) : c[w];
        st->f[F_OF] = bdd_xor(m, c[w], c[w - 1]);
        st->f[F_AF] = sub ? bdd_not(m, c[4]) : c[4];
        sset_szp(m, st, s, w);
        if (t != FDI_CMP) swrite(in, 0, st, s);
        break;
    }
    case FDI_INC: case FDI_DEC: {
        B c0, oldcf = st->f[F_CF];
        sread(in, 0, st, a, &w);
        if (t == FDI_INC) {
            for (i = 0; i < w; i++) b[i] = BNONE;
            c0 = BONE;
        } else {
            for (i = 0; i < w; i++) b[i] = BONE;
            c0 = BNONE;
        }
        sadder(m, a, b, c0, w, s, c);
        st->f[F_OF] = bdd_xor(m, c[w], c[w - 1]);
        st->f[F_AF] = (t == FDI_INC) ? c[4] : bdd_not(m, c[4]);
        sset_szp(m, st, s, w);
        st->f[F_CF] = oldcf; /* INC/DEC preserve CF */
        swrite(in, 0, st, s);
        break;
    }
    case FDI_TEST: case FDI_AND: case FDI_OR: case FDI_XOR:
        sread(in, 0, st, a, &w);
        sread(in, 1, st, b, &w2);
        for (i = 0; i < w; i++) {
            if (t == FDI_OR) s[i] = bdd_or(m, a[i], b[i]);
            else if (t == FDI_XOR) s[i] = bdd_xor(m, a[i], b[i]);
            else s[i] = bdd_and(m, a[i], b[i]);
        }
        st->f[F_CF] = BNONE;
        st->f[F_OF] = BNONE;
        /* Undefined AF: one fresh free variable per logical instruction. */
        st->f[F_AF] = bdd_var(m, ins->afvar);
        sset_szp(m, st, s, w);
        if (t != FDI_TEST) swrite(in, 0, st, s);
        break;
    case FDI_CLC: st->f[F_CF] = BNONE; break;
    case FDI_STC: st->f[F_CF] = BONE; break;
    case FDI_CMC: st->f[F_CF] = bdd_not(m, st->f[F_CF]); break;
    case FDI_LAHF: /* AH := SF:ZF:0:AF:0:PF:1:CF */
        st->r[0][8] = st->f[F_CF];
        st->r[0][9] = BONE;
        st->r[0][10] = st->f[F_PF];
        st->r[0][11] = BNONE;
        st->r[0][12] = st->f[F_AF];
        st->r[0][13] = BNONE;
        st->r[0][14] = st->f[F_ZF];
        st->r[0][15] = st->f[F_SF];
        break;
    case FDI_SAHF: /* five flags from AH, OF preserved */
        st->f[F_CF] = st->r[0][8];
        st->f[F_PF] = st->r[0][10];
        st->f[F_AF] = st->r[0][12];
        st->f[F_ZF] = st->r[0][14];
        st->f[F_SF] = st->r[0][15];
        break;
    case FDI_NOP:
        break;
    default:
        if (ins->cond >= 0 && setcc_cc(t) >= 0) {
            B cc = scond(m, st, ins->cond);
            for (i = 0; i < 64; i++) s[i] = BNONE;
            s[0] = cc;
            swrite(in, 0, st, s);
        } else if (ins->cond >= 0 && cmovcc_cc(t) >= 0) {
            B cc = scond(m, st, ins->cond);
            sread(in, 0, st, a, &w);  /* old destination */
            sread(in, 1, st, b, &w2); /* source, same width */
            for (i = 0; i < w; i++) s[i] = bdd_ite(m, cc, b[i], a[i]);
            swrite(in, 0, st, s); /* 32-bit form still clears high bits */
        } else if (ins->kind == K_JCC) {
            ins->ccbdd = scond(m, st, ins->cond);
        }
        break;
    }
}

/* Merge the state of one executed instruction into a successor under the
   given edge path condition. */
static void propagate(Eng *e, uint32_t to, B pc, const State *s) {
    Bdd *m = &e->bdd;
    State *d = &e->acc[to];
    int i, j;
    if (pc == BNONE || pc == BFAIL) return;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 64; j++)
            d->r[i][j] = bdd_ite(m, pc, s->r[i][j], d->r[i][j]);
    for (i = 0; i < 6; i++)
        d->f[i] = bdd_ite(m, pc, s->f[i], d->f[i]);
    d->pc = bdd_or(m, d->pc, pc);
}

/* ------------------------------------------------------------------ */
/* Concrete interpreter, used to materialize witness AF choices       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t r[16];
    uint8_t fl[6];
} CState;

static uint64_t cmask(int w) { return w >= 64 ? ~0ull : ((1ull << w) - 1); }

static int parity8(uint8_t x) {
    x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return x & 1;
}

static uint64_t cread_op(const CState *cs, const FdInstr *in, int op, int *pw) {
    if (FD_OP_TYPE(in, op) == FD_OT_IMM) {
        *pw = FD_OP_SIZE(in, 0) * 8;
        return (uint64_t)FD_OP_IMM(in, op) & cmask(*pw);
    }
    {
        int idx = FD_OP_REG(in, op);
        if (FD_OP_REG_TYPE(in, op) == FD_RT_GPH) {
            *pw = 8;
            return (cs->r[idx - 4] >> 8) & 0xff;
        }
        *pw = FD_OP_SIZE(in, op) * 8;
        return cs->r[idx] & cmask(*pw);
    }
}

static void cwrite_op(CState *cs, const FdInstr *in, int op, uint64_t v) {
    int idx = FD_OP_REG(in, op);
    int sz = FD_OP_SIZE(in, op);
    if (FD_OP_REG_TYPE(in, op) == FD_RT_GPH) {
        cs->r[idx - 4] = (cs->r[idx - 4] & ~0xff00ull) | ((v & 0xff) << 8);
        return;
    }
    if (sz == 8) cs->r[idx] = v;
    else if (sz == 4) cs->r[idx] = (uint32_t)v;
    else cs->r[idx] = (cs->r[idx] & ~cmask(sz * 8)) | (v & cmask(sz * 8));
}

static void cset_szp(CState *cs, uint64_t r, int w) {
    r &= cmask(w);
    cs->fl[F_ZF] = (r == 0);
    cs->fl[F_SF] = (uint8_t)((r >> (w - 1)) & 1);
    cs->fl[F_PF] = (uint8_t)!parity8((uint8_t)r);
}

static uint64_t cadd(CState *cs, uint64_t a, uint64_t b, uint64_t cin, int w) {
    uint64_t m = cmask(w), r, sign = 1ull << (w - 1);
    __uint128_t s;
    a &= m; b &= m;
    s = (__uint128_t)a + b + (cin & 1);
    r = (uint64_t)s & m;
    cs->fl[F_CF] = (uint8_t)((uint64_t)(s >> w) & 1);
    cs->fl[F_OF] = (uint8_t)((~(a ^ b) & (a ^ r) & sign) != 0);
    cs->fl[F_AF] = (uint8_t)(((a ^ b ^ r) >> 4) & 1);
    cset_szp(cs, r, w);
    return r;
}

static uint64_t csub(CState *cs, uint64_t a, uint64_t b, uint64_t bin, int w) {
    uint64_t m = cmask(w), r, sign = 1ull << (w - 1);
    __uint128_t sub;
    a &= m; b &= m;
    sub = (__uint128_t)b + (bin & 1);
    r = (uint64_t)((__uint128_t)a - sub) & m;
    cs->fl[F_CF] = (uint8_t)((__uint128_t)a < sub);
    cs->fl[F_OF] = (uint8_t)(((a ^ b) & (a ^ r) & sign) != 0);
    cs->fl[F_AF] = (uint8_t)(((a ^ b ^ r) >> 4) & 1);
    cset_szp(cs, r, w);
    return r;
}

static int ccond(const CState *cs, int cc) {
    switch (cc) {
    case 0: return cs->fl[F_OF];
    case 1: return !cs->fl[F_OF];
    case 2: return cs->fl[F_CF];
    case 3: return !cs->fl[F_CF];
    case 4: return cs->fl[F_ZF];
    case 5: return !cs->fl[F_ZF];
    case 6: return cs->fl[F_CF] || cs->fl[F_ZF];
    case 7: return !cs->fl[F_CF] && !cs->fl[F_ZF];
    case 8: return cs->fl[F_SF];
    case 9: return !cs->fl[F_SF];
    case 10: return cs->fl[F_PF];
    case 11: return !cs->fl[F_PF];
    case 12: return cs->fl[F_SF] != cs->fl[F_OF];
    case 13: return cs->fl[F_SF] == cs->fl[F_OF];
    case 14: return cs->fl[F_ZF] || (cs->fl[F_SF] != cs->fl[F_OF]);
    case 15: return !cs->fl[F_ZF] && (cs->fl[F_SF] == cs->fl[F_OF]);
    default: return 0;
    }
}

/* Execute one non-control instruction concretely. */
static void cexec(const Insn *ins, CState *cs, const uint8_t *asn,
                  FdPathChoice *ch, size_t *pnch) {
    const FdInstr *in = &ins->in;
    uint16_t t = FD_TYPE(in);
    int w = 0, w2 = 0;
    uint64_t a, b, r;
    switch (t) {
    case FDI_MOV: case FDI_MOVABS:
        cwrite_op(cs, in, 0, cread_op(cs, in, 1, &w));
        break;
    case FDI_MOVZX:
        cwrite_op(cs, in, 0, cread_op(cs, in, 1, &w));
        break;
    case FDI_ADD: case FDI_ADC:
        a = cread_op(cs, in, 0, &w);
        b = cread_op(cs, in, 1, &w2);
        r = cadd(cs, a, b, t == FDI_ADC ? cs->fl[F_CF] : 0, w);
        cwrite_op(cs, in, 0, r);
        break;
    case FDI_SUB: case FDI_SBB: case FDI_CMP:
        a = cread_op(cs, in, 0, &w);
        b = cread_op(cs, in, 1, &w2);
        r = csub(cs, a, b, t == FDI_SBB ? cs->fl[F_CF] : 0, w);
        if (t != FDI_CMP) cwrite_op(cs, in, 0, r);
        break;
    case FDI_INC: case FDI_DEC: {
        uint8_t cf = cs->fl[F_CF];
        a = cread_op(cs, in, 0, &w);
        r = (t == FDI_INC) ? cadd(cs, a, 0, 1, w) : csub(cs, a, 0, 1, w);
        cs->fl[F_CF] = cf;
        cwrite_op(cs, in, 0, r);
        break;
    }
    case FDI_TEST: case FDI_AND: case FDI_OR: case FDI_XOR:
        a = cread_op(cs, in, 0, &w);
        b = cread_op(cs, in, 1, &w2);
        if (t == FDI_OR) r = a | b;
        else if (t == FDI_XOR) r = a ^ b;
        else r = a & b;
        r &= cmask(w);
        cs->fl[F_CF] = 0;
        cs->fl[F_OF] = 0;
        cs->fl[F_AF] = asn[ins->afvar]; /* the recorded free choice */
        ch[*pnch].offset = ins->off;
        ch[*pnch].af = cs->fl[F_AF];
        (*pnch)++;
        cset_szp(cs, r, w);
        if (t != FDI_TEST) cwrite_op(cs, in, 0, r);
        break;
    case FDI_CLC: cs->fl[F_CF] = 0; break;
    case FDI_STC: cs->fl[F_CF] = 1; break;
    case FDI_CMC: cs->fl[F_CF] = !cs->fl[F_CF]; break;
    case FDI_LAHF:
        a = (uint64_t)cs->fl[F_CF] | 2ull | ((uint64_t)cs->fl[F_PF] << 2) |
            ((uint64_t)cs->fl[F_AF] << 4) | ((uint64_t)cs->fl[F_ZF] << 6) |
            ((uint64_t)cs->fl[F_SF] << 7);
        cs->r[0] = (cs->r[0] & ~0xff00ull) | (a << 8);
        break;
    case FDI_SAHF:
        a = (cs->r[0] >> 8) & 0xff;
        cs->fl[F_CF] = (uint8_t)(a & 1);
        cs->fl[F_PF] = (uint8_t)((a >> 2) & 1);
        cs->fl[F_AF] = (uint8_t)((a >> 4) & 1);
        cs->fl[F_ZF] = (uint8_t)((a >> 6) & 1);
        cs->fl[F_SF] = (uint8_t)((a >> 7) & 1);
        break;
    case FDI_NOP:
        break;
    default:
        if (ins->cond >= 0 && setcc_cc(t) >= 0) {
            cwrite_op(cs, in, 0, (uint64_t)ccond(cs, ins->cond));
        } else if (ins->cond >= 0 && cmovcc_cc(t) >= 0) {
            a = cread_op(cs, in, 0, &w);
            b = cread_op(cs, in, 1, &w2);
            cwrite_op(cs, in, 0, ccond(cs, ins->cond) ? b : a);
        }
        break;
    }
}

/* Replay one feasible edge concretely to collect the undefined-AF choices
   in path order.  Returns 0 on success. */
static int replay_choices(const Eng *e, const uint64_t entry[16],
                          uint8_t eflags, const uint8_t *asn, uint32_t src,
                          int taken, FdPathChoice **out, size_t *outn) {
    CState cs;
    FdPathChoice *ch;
    size_t nch = 0, steps;
    uint32_t cur = 0;
    int i, rc = -1;
    memcpy(cs.r, entry, sizeof cs.r);
    for (i = 0; i < 6; i++) cs.fl[i] = (uint8_t)((eflags >> i) & 1);
    ch = malloc((e->n ? e->n : 1) * sizeof *ch);
    if (!ch) return -1;
    for (steps = 0; steps <= e->n; steps++) {
        const Insn *ins;
        if (cur == src) {
            ins = &e->ins[cur];
            if (ins->kind == K_JCC && ccond(&cs, ins->cond) == taken) rc = 0;
            break;
        }
        ins = &e->ins[cur];
        if (ins->kind == K_RET) break;
        cexec(ins, &cs, asn, ch, &nch);
        if (ins->kind == K_JMP) cur = ins->tgt;
        else if (ins->kind == K_JCC)
            cur = ccond(&cs, ins->cond) ? ins->tgt : ins->fall;
        else cur = ins->fall;
    }
    if (rc) {
        free(ch);
        return -1;
    }
    if (!nch) {
        free(ch);
        ch = NULL;
    }
    *out = ch;
    *outn = nch;
    return 0;
}

/* ------------------------------------------------------------------ */
/* CFG construction                                                   */
/* ------------------------------------------------------------------ */

static int succs(const Eng *e, uint32_t v, uint32_t out[2]) {
    const Insn *p = &e->ins[v];
    switch (p->kind) {
    case K_JCC: out[0] = p->tgt; out[1] = p->fall; return 2;
    case K_JMP: out[0] = p->tgt; return 1;
    case K_FALL: out[0] = p->fall; return 1;
    default: return 0;
    }
}

/* Topological order (reverse postorder); returns -1 on a cycle, setting
   err to the offset of the instruction closing the cycle. */
static int build_topo(Eng *e) {
    size_t n = e->n, no = 0, s;
    uint8_t *color = calloc(n, 1);
    uint32_t *order = malloc(n * sizeof(uint32_t));
    uint32_t *stack = malloc(n * sizeof(uint32_t));
    uint8_t *cix = malloc(n);
    int rc = -1;
    if (!color || !order || !stack || !cix) goto out;
    for (s = 0; s < n; s++) {
        size_t sp;
        if (color[s]) continue;
        sp = 0;
        stack[sp] = (uint32_t)s;
        cix[sp] = 0;
        color[s] = 1;
        sp = 1;
        while (sp) {
            uint32_t v = stack[sp - 1];
            uint32_t succ[2];
            int ns = succs(e, v, succ), pushed = 0;
            while (cix[sp - 1] < ns) {
                uint32_t u = succ[cix[sp - 1]++];
                if (color[u] == 1) {
                    e->err = e->ins[v].off;
                    goto out;
                }
                if (!color[u]) {
                    color[u] = 1;
                    stack[sp] = u;
                    cix[sp] = 0;
                    sp++;
                    pushed = 1;
                    break;
                }
            }
            if (!pushed) {
                color[v] = 2;
                order[no++] = v;
                sp--;
            }
        }
    }
    for (s = 0; s < no; s++) e->topo[s] = order[no - 1 - s];
    rc = 0;
out:
    free(color); free(order); free(stack); free(cix);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Witness construction                                               */
/* ------------------------------------------------------------------ */

static int build_witness(Eng *e, B edge, uint32_t src, int taken,
                         FdPathWitness *w) {
    uint8_t *asn = calloc(e->nvars ? e->nvars : 1, 1);
    int r, b, f;
    if (!asn) return -1;
    bdd_sat(&e->bdd, edge, asn);
    for (r = 0; r < 16; r++) {
        uint64_t v = 0;
        for (b = 0; b < 64; b++) {
            int bit;
            if ((e->input->known_mask[r] >> b) & 1)
                bit = (int)((e->input->value[r] >> b) & 1);
            else
                bit = asn[e->regvar[r][b]];
            v |= (uint64_t)bit << b;
        }
        w->entry[r] = v;
    }
    w->entry_flags = 0;
    for (f = 0; f < 6; f++) {
        int bit;
        if ((e->input->flags_known >> f) & 1)
            bit = (e->input->flags_value >> f) & 1;
        else
            bit = asn[e->flvar[f]];
        w->entry_flags |= (uint8_t)(bit << f);
    }
    r = replay_choices(e, w->entry, w->entry_flags, asn, src, taken,
                       &w->choices, &w->choice_count);
    free(asn);
    return r;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

static void eng_free(Eng *e) {
    bdd_free(&e->bdd);
    free(e->ins);
    free(e->off2idx);
    free(e->acc);
    free(e->topo);
    memset(e, 0, sizeof *e);
}

FdPathStatus fd_path_analyze(const uint8_t *code, size_t len,
                             const FdPathInput *input, size_t node_limit,
                             FdPathReport *report) {
    Eng e;
    FdPathStatus status = FD_PATH_OK;
    size_t off, i, cap, ti, njcc, k;

    if (!report) return FD_PATH_INVALID;
    if (report->edges || report->edge_count) return FD_PATH_INVALID;
    report->error_offset = SIZE_MAX;
    if (!code || !input || !len || !node_limit) return FD_PATH_INVALID;

    memset(&e, 0, sizeof e);
    e.input = input;
    e.err = SIZE_MAX;

    /* Decode the complete linear partition. */
    cap = 16;
    e.ins = malloc(cap * sizeof(Insn));
    if (!e.ins) return FD_PATH_NOMEM;
    off = 0;
    while (off < len) {
        FdInstr fi;
        int r;
        if (e.n == cap) {
            Insn *ni;
            cap *= 2;
            ni = realloc(e.ins, cap * sizeof(Insn));
            if (!ni) { status = FD_PATH_NOMEM; goto fail; }
            e.ins = ni;
        }
        r = fd_decode(code + off, len - off, 64, 0, &fi);
        if (r <= 0) {
            e.err = off;
            status = FD_PATH_DECODE;
            goto fail;
        }
        e.ins[e.n].in = fi;
        e.ins[e.n].off = (uint32_t)off;
        e.ins[e.n].kind = K_FALL;
        e.ins[e.n].cond = -1;
        e.ins[e.n].tgt = e.ins[e.n].fall = UINT32_MAX;
        e.ins[e.n].afvar = VNONE;
        e.ins[e.n].ccbdd = e.ins[e.n].e_taken = e.ins[e.n].e_fall = BNONE;
        e.n++;
        off += (size_t)r;
    }

    /* Structural checks cover unreachable instructions too. */
    for (i = 0; i < e.n; i++) {
        if (!classify(&e.ins[i])) {
            e.err = e.ins[i].off;
            status = FD_PATH_UNSUPPORTED;
            goto fail;
        }
    }

    e.off2idx = malloc(len * sizeof(int32_t));
    if (!e.off2idx) { status = FD_PATH_NOMEM; goto fail; }
    for (i = 0; i < len; i++) e.off2idx[i] = -1;
    for (i = 0; i < e.n; i++) e.off2idx[e.ins[i].off] = (int32_t)i;

    /* CFG edges with target validation. */
    for (i = 0; i < e.n; i++) {
        Insn *p = &e.ins[i];
        if (p->kind == K_JMP || p->kind == K_JCC) {
            int64_t tgt = (int64_t)p->off + FD_SIZE(&p->in) +
                          FD_OP_IMM(&p->in, 0);
            if (tgt < 0 || (uint64_t)tgt >= len) {
                e.err = p->off;
                status = FD_PATH_UNSUPPORTED;
                goto fail;
            }
            if (e.off2idx[tgt] < 0) {
                e.err = p->off;
                status = FD_PATH_INVALID;
                goto fail;
            }
            p->tgt = (uint32_t)e.off2idx[tgt];
        }
        if (p->kind == K_JCC || p->kind == K_FALL) {
            if (i + 1 == e.n) {
                /* Fallthrough would leave [0, len). */
                e.err = p->off;
                status = FD_PATH_UNSUPPORTED;
                goto fail;
            }
            p->fall = (uint32_t)(i + 1);
        }
    }

    e.topo = malloc(e.n * sizeof(uint32_t));
    if (!e.topo) { status = FD_PATH_NOMEM; goto fail; }
    if (build_topo(&e)) {
        status = FD_PATH_UNSUPPORTED; /* cycle */
        goto fail;
    }

    if (bdd_init(&e.bdd, node_limit)) { status = FD_PATH_NOMEM; goto fail; }

    e.acc = calloc(e.n, sizeof(State)); /* all BNONE */
    if (!e.acc) { status = FD_PATH_NOMEM; goto fail; }

    /* Entry state: known bits become constants, free bits become variables.
       Variable order: entry flags, then register bits interleaved by bit
       position (keeps carry chains compact), then per-instruction AF. */
    {
        uint32_t v = 0;
        int r, b, f;
        for (f = 0; f < 6; f++) {
            if ((input->flags_known >> f) & 1)
                e.acc[0].f[f] = ((input->flags_value >> f) & 1) ? BONE : BNONE;
            else {
                e.flvar[f] = v;
                e.acc[0].f[f] = bdd_var(&e.bdd, v);
                v++;
            }
        }
        for (b = 0; b < 64; b++)
            for (r = 0; r < 16; r++) {
                if ((input->known_mask[r] >> b) & 1) {
                    e.acc[0].r[r][b] =
                        ((input->value[r] >> b) & 1) ? BONE : BNONE;
                } else {
                    e.regvar[r][b] = v;
                    e.acc[0].r[r][b] = bdd_var(&e.bdd, v);
                    v++;
                }
            }
        for (i = 0; i < e.n; i++)
            if (is_logic(FD_TYPE(&e.ins[i].in)))
                e.ins[i].afvar = v++;
        e.nvars = v;
        e.acc[0].pc = BONE;
        if (e.bdd.failed) { status = FD_PATH_LIMIT; goto fail; }
    }

    /* Symbolic execution in topological order. */
    for (ti = 0; ti < e.n; ti++) {
        uint32_t idx = e.topo[ti];
        Insn *p = &e.ins[idx];
        State *st = &e.acc[idx];
        sym_exec(&e, idx);
        if (e.bdd.failed) { status = FD_PATH_LIMIT; goto fail; }
        switch (p->kind) {
        case K_FALL:
            propagate(&e, p->fall, st->pc, st);
            break;
        case K_JMP:
            propagate(&e, p->tgt, st->pc, st);
            break;
        case K_JCC: {
            B nt = bdd_not(&e.bdd, p->ccbdd);
            p->e_taken = bdd_and(&e.bdd, st->pc, p->ccbdd);
            p->e_fall = bdd_and(&e.bdd, st->pc, nt);
            propagate(&e, p->tgt, p->e_taken, st);
            propagate(&e, p->fall, p->e_fall, st);
            break;
        }
        default:
            break;
        }
        if (e.bdd.failed) { status = FD_PATH_LIMIT; goto fail; }
    }

    /* Assemble the report: per Jcc, fallthrough edge then taken edge. */
    njcc = 0;
    for (i = 0; i < e.n; i++)
        if (e.ins[i].kind == K_JCC) njcc++;
    if (njcc) {
        report->edges = calloc(njcc * 2, sizeof(FdPathEdge));
        if (!report->edges) { status = FD_PATH_NOMEM; goto fail; }
    }
    report->edge_count = njcc * 2;
    k = 0;
    for (i = 0; i < e.n; i++) {
        Insn *p = &e.ins[i];
        int dir;
        if (p->kind != K_JCC) continue;
        for (dir = 0; dir < 2; dir++) {
            FdPathEdge *ed = &report->edges[k++];
            B eb = dir ? p->e_taken : p->e_fall;
            ed->source = p->off;
            ed->destination =
                e.ins[dir ? p->tgt : p->fall].off;
            ed->taken = (uint8_t)dir;
            ed->feasible = (eb != BNONE);
            if (ed->feasible &&
                build_witness(&e, eb, (uint32_t)i, dir, &ed->witness)) {
                status = FD_PATH_NOMEM;
                goto fail_edges;
            }
        }
    }
    report->error_offset = SIZE_MAX;
    eng_free(&e);
    return FD_PATH_OK;

fail_edges:
    fd_path_free(report);
fail:
    if (status == FD_PATH_LIMIT && e.bdd.failed == 2)
        status = FD_PATH_NOMEM;
    report->edges = NULL;
    report->edge_count = 0;
    report->error_offset = e.err;
    if (status == FD_PATH_LIMIT || status == FD_PATH_NOMEM)
        report->error_offset = SIZE_MAX;
    eng_free(&e);
    return status;
}

void fd_path_free(FdPathReport *report) {
    size_t i;
    if (!report) return;
    if (report->edges) {
        for (i = 0; i < report->edge_count; i++)
            free(report->edges[i].witness.choices);
        free(report->edges);
    }
    report->edges = NULL;
    report->edge_count = 0;
    report->error_offset = 0;
}
